from datetime import datetime, timezone
from importlib.util import module_from_spec, spec_from_file_location
import json
from pathlib import Path

import pytest

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator",
    Path(__file__).parents[3] / "simulator" / "ear" / "simulate_scenario.py",
)
assert spec is not None and spec.loader is not None
simulate_scenario = module_from_spec(spec)
spec.loader.exec_module(simulate_scenario)


SCENARIO = {
    "scenario_id": "test-pass",
    "source_id": "SIM-VEHICLE-TEST",
    "observations": [
        {
            "at_seconds": 0,
            "ear": "ear_a",
            "bearing_deg": 10,
            "bearing_confidence": 0.6,
            "classification_confidence": 0.7,
            "signal_level_dbfs": -30,
        },
        {
            "at_seconds": 2,
            "ear": "ear_b",
            "bearing_deg": 200,
            "bearing_confidence": 0.8,
            "classification_confidence": 0.9,
            "signal_level_dbfs": -38,
        },
        {
            "at_seconds": 5,
            "ear": "ear_a",
            "bearing_deg": 5,
            "bearing_confidence": 0.4,
            "classification_confidence": 0.55,
            "signal_level_dbfs": -32,
        },
    ],
}


def test_load_scenario_rejects_out_of_order_observations(tmp_path) -> None:
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps({**SCENARIO, "observations": list(reversed(SCENARIO["observations"]))}))

    with pytest.raises(simulate_scenario.ScenarioError, match="non-decreasing"):
        simulate_scenario.load_scenario(path)


def test_scenario_validation_rejects_non_finite_or_out_of_range_values(tmp_path) -> None:
    invalid = {
        **SCENARIO,
        "observations": [{**SCENARIO["observations"][0], "bearing_deg": 360}],
    }
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(invalid))

    with pytest.raises(simulate_scenario.ScenarioError, match=r"\[0, 360\)"):
        simulate_scenario.load_scenario(path)

    invalid["observations"] = [
        {**SCENARIO["observations"][0], "signal_level_dbfs": float("nan")}
    ]
    path.write_text(json.dumps(invalid))
    with pytest.raises(simulate_scenario.ScenarioError, match="must be finite"):
        simulate_scenario.load_scenario(path)

    invalid["observations"] = [
        {**SCENARIO["observations"][0], "classification_confidence": 1.1}
    ]
    path.write_text(json.dumps(invalid))
    with pytest.raises(
        simulate_scenario.ScenarioError,
        match="classification_confidence must be at most 1",
    ):
        simulate_scenario.load_scenario(path)

    invalid["observations"] = [
        {**SCENARIO["observations"][0], "bearing_confidence": -0.1}
    ]
    path.write_text(json.dumps(invalid))
    with pytest.raises(
        simulate_scenario.ScenarioError,
        match="bearing_confidence must be at least 0",
    ):
        simulate_scenario.load_scenario(path)


def test_ear_credentials_require_private_file_and_distinct_simulated_nodes(tmp_path) -> None:
    record = {
        "state": "provisioned",
        "node_id": "SIM-EAR-001",
        "mqtt": {
            "host": "hub.example",
            "port": 8883,
            "use_tls": True,
            "username": "sim-ear-001",
            "password": "secret",
        },
        "topics": {"observation": "esp-ear/local/sim-ear-001/observation"},
        "observation": {"sequence_number": 2},
    }
    first = tmp_path / "ear-a.json"
    second = tmp_path / "ear-b.json"
    first.write_text(json.dumps(record))
    second.write_text(json.dumps(record))
    first.chmod(0o600)
    second.chmod(0o600)

    with pytest.raises(simulate_scenario.ScenarioError, match="distinct node"):
        simulate_scenario.load_ear_credentials({"ear_a": first, "ear_b": second})

    second.chmod(0o644)
    with pytest.raises(simulate_scenario.ScenarioError, match="not be accessible"):
        simulate_scenario.load_ear_credentials({"ear_b": second})


def test_scenario_builds_protocol_observations_with_per_ear_sequences_and_timeline() -> None:
    started_at = datetime(2026, 10, 5, 18, 0, tzinfo=timezone.utc)
    credentials = {
        "ear_a": {"node_id": "SIM-EAR-001", "next_sequence_number": 3},
        "ear_b": {"node_id": "SIM-EAR-002", "next_sequence_number": 8},
    }
    published = []
    sleeps = []
    clock = [100.0]

    def sleep(seconds: float) -> None:
        sleeps.append(seconds)
        clock[0] += seconds

    def monotonic() -> float:
        return clock[0]

    def publish(current_credentials, payload, ca_file):
        assert ca_file == "ca.crt"
        published.append(payload)
        return payload["observation_id"]

    ids = simulate_scenario.run_scenario(
        SCENARIO,
        credentials,
        {"ear_a": Path("a.json"), "ear_b": Path("b.json")},
        "ca.crt",
        0.25,
        publish=publish,
        sleep=sleep,
        monotonic=monotonic,
        started_at_utc=started_at,
        started_at_monotonic_us=1_000_000,
        save=lambda *args, **kwargs: None,
    )

    assert ids == [item["observation_id"] for item in published]
    assert len(set(ids)) == 3
    assert [item["node_id"] for item in published] == [
        "SIM-EAR-001",
        "SIM-EAR-002",
        "SIM-EAR-001",
    ]
    assert [item["classification"]["confidence"] for item in published] == [
        0.7,
        0.9,
        0.55,
    ]
    assert [item["bearing"]["confidence"] for item in published] == [0.6, 0.8, 0.4]
    assert [item["sequence_number"] for item in published] == [3, 8, 4]
    assert [item["event_time_utc"] for item in published] == [
        "2026-10-05T18:00:00+00:00",
        "2026-10-05T18:00:02+00:00",
        "2026-10-05T18:00:05+00:00",
    ]
    assert [item["capture_timestamp_monotonic_us"] for item in published] == [
        1_000_000,
        3_000_000,
        6_000_000,
    ]
    assert sleeps == [0.5, 0.75]
    assert [item["classification"]["hints"]["simulated_source_id"] for item in published] == [
        "SIM-VEHICLE-TEST",
        "SIM-VEHICLE-TEST",
        "SIM-VEHICLE-TEST",
    ]
    assert credentials["ear_a"]["next_sequence_number"] == 5
    assert credentials["ear_b"]["next_sequence_number"] == 9


def test_retry_pending_reuses_same_observation_id_and_advances_sequence() -> None:
    pending = simulate_scenario.make_scenario_observation(
        "SIM-EAR-001",
        4,
        SCENARIO["source_id"],
        SCENARIO["observations"][0],
        datetime(2026, 10, 5, 18, 0, tzinfo=timezone.utc),
        1_000_000,
    )
    credentials = {
        "ear_a": {
            "node_id": "SIM-EAR-001",
            "next_sequence_number": 4,
            "pending_observation": pending,
            "pending_scenario_id": SCENARIO["scenario_id"],
        }
    }
    saved = []
    published = []

    ids = simulate_scenario.retry_pending_observations(
        SCENARIO,
        credentials,
        {"ear_a": Path("ear-a.json")},
        "ca.crt",
        publish=lambda current, payload, ca: published.append(payload) or payload["observation_id"],
        save=lambda path, value: saved.append((path, dict(value))),
    )

    assert ids == [pending["observation_id"]]
    assert published[0]["observation_id"] == pending["observation_id"]
    assert credentials["ear_a"]["next_sequence_number"] == 5
    assert "pending_observation" not in credentials["ear_a"]
    assert saved[0][0] == Path("ear-a.json")


def test_retry_pending_rejects_pending_observation_from_different_scenario() -> None:
    credentials = {
        "ear_a": {
            "pending_observation": {"observation_id": "saved"},
            "pending_scenario_id": "other-scenario",
            "next_sequence_number": 2,
        }
    }
    with pytest.raises(simulate_scenario.ScenarioError, match="belongs to another scenario"):
        simulate_scenario.retry_pending_observations(
            SCENARIO,
            credentials,
            {"ear_a": Path("ear-a.json")},
            "ca.crt",
            publish=lambda *args: pytest.fail("must not publish another scenario's observation"),
        )


def test_retry_pending_rejects_observation_for_a_different_ear() -> None:
    pending = simulate_scenario.make_scenario_observation(
        "SIM-EAR-001",
        4,
        SCENARIO["source_id"],
        SCENARIO["observations"][0],
        datetime(2026, 10, 5, 18, 0, tzinfo=timezone.utc),
        1_000_000,
    )
    credentials = {
        "ear_a": {
            "node_id": "SIM-EAR-002",
            "next_sequence_number": 4,
            "pending_observation": pending,
            "pending_scenario_id": SCENARIO["scenario_id"],
        }
    }

    with pytest.raises(simulate_scenario.ScenarioError, match="belongs to another Ear"):
        simulate_scenario.retry_pending_observations(
            SCENARIO,
            credentials,
            {"ear_a": Path("ear-a.json")},
            "ca.crt",
            publish=lambda *args: pytest.fail("must not publish an observation for another Ear"),
        )
