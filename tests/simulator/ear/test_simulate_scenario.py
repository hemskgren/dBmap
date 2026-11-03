import json
from datetime import datetime, timezone
from importlib.util import module_from_spec, spec_from_file_location
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
    "source": {
        "ground_truth_id": "SIM-VEHICLE-TEST",
        "source_family": "vehicle",
        "movement": {
            "start": {"latitude": 0, "longitude": 0},
            "end": {"latitude": 0, "longitude": 0.01},
            "speed_m_s": 12,
        },
    },
    "ears": {
        "ear_a": {"latitude": 0.001, "longitude": 0, "orientation_deg": 90},
        "ear_b": {"latitude": 0.001, "longitude": 0.004, "orientation_deg": 270},
    },
    "observations": [
        {
            "at_seconds": 0,
            "ear": "ear_a",
            "bearing_confidence": 0.6,
            "classification_confidence": 0.7,
            "signal_level_dbfs": -30,
        },
        {
            "at_seconds": 2,
            "ear": "ear_b",
            "bearing_confidence": 0.8,
            "classification_confidence": 0.9,
            "signal_level_dbfs": -38,
        },
        {
            "at_seconds": 5,
            "ear": "ear_a",
            "bearing_confidence": 0.4,
            "classification_confidence": 0.55,
            "signal_level_dbfs": -32,
        },
    ],
}


def load_test_scenario(tmp_path):
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(SCENARIO))
    return simulate_scenario.load_scenario(path)


def test_load_scenario_rejects_out_of_order_observations(tmp_path) -> None:
    path = tmp_path / "scenario.json"
    path.write_text(
        json.dumps(
            {**SCENARIO, "observations": list(reversed(SCENARIO["observations"]))}
        )
    )

    with pytest.raises(simulate_scenario.ScenarioError, match="non-decreasing"):
        simulate_scenario.load_scenario(path)


def test_scenario_validation_rejects_non_finite_or_out_of_range_values(
    tmp_path,
) -> None:
    invalid = {
        **SCENARIO,
        "ears": {
            **SCENARIO["ears"],
            "ear_a": {**SCENARIO["ears"]["ear_a"], "orientation_deg": 360},
        },
    }
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(invalid))

    with pytest.raises(
        simulate_scenario.ScenarioError,
        match=r"orientation_deg must be in \[0, 360\)",
    ):
        simulate_scenario.load_scenario(path)

    invalid = {
        **SCENARIO,
        "observations": [
            {**SCENARIO["observations"][0], "signal_level_dbfs": float("nan")}
        ],
    }
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


def test_ear_credentials_require_private_file_and_distinct_simulated_nodes(
    tmp_path,
) -> None:
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


def test_scenario_builds_protocol_observations_with_per_ear_sequences_and_timeline(
    tmp_path,
) -> None:
    scenario = load_test_scenario(tmp_path)
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
        return payload["identity"]["observation_id"]

    ids = simulate_scenario.run_scenario(
        scenario,
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
        publish_keepalive=lambda *args, **kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )

    assert ids == [item["identity"]["observation_id"] for item in published]
    # ear_a has two points (shared id); ear_b has one (COMPLETE).
    assert len(set(ids)) == 2
    assert ids[0] == ids[2]
    assert ids[0] != ids[1]
    assert [item["identity"]["node_id"] for item in published] == [
        "SIM-EAR-001",
        "SIM-EAR-002",
        "SIM-EAR-001",
    ]
    assert [
        item["acoustic"]["classification"]["confidence"] for item in published
    ] == [
        0.7,
        0.9,
        0.55,
    ]
    assert [
        item["acoustic"]["classification"]["source_family"] for item in published
    ] == [
        "vehicle",
        "vehicle",
        "vehicle",
    ]
    assert [item["spatial"]["bearing"]["confidence"] for item in published] == [
        0.6,
        0.8,
        0.4,
    ]
    assert (
        published[0]["spatial"]["bearing"]["deg"]
        != published[2]["spatial"]["bearing"]["deg"]
    )
    assert all(0 <= item["spatial"]["bearing"]["deg"] < 360 for item in published)
    assert published[0]["spatial"]["bearing"]["deg"] == pytest.approx(90, abs=0.01)
    assert scenario["observations"][0]["source_distance_m"] == pytest.approx(
        111.2, abs=1
    )
    assert scenario["movement"]["speed_m_s"] == 12
    assert [item["identity"]["sequence_number"] for item in published] == [3, 8, 4]
    assert [item["context"]["event_time_utc"] for item in published] == [
        "2026-10-05T18:00:00+00:00",
        "2026-10-05T18:00:02+00:00",
        "2026-10-05T18:00:05+00:00",
    ]
    assert [item["context"]["capture_timestamp_monotonic_us"] for item in published] == [
        1_000_000,
        3_000_000,
        6_000_000,
    ]
    assert sleeps == [0.5, 0.75]
    assert [
        item["acoustic"]["classification"]["hints"]["simulated_source_id"]
        for item in published
    ] == [
        "SIM-VEHICLE-TEST",
        "SIM-VEHICLE-TEST",
        "SIM-VEHICLE-TEST",
    ]
    assert credentials["ear_a"]["next_sequence_number"] == 5
    assert credentials["ear_b"]["next_sequence_number"] == 9
    assert [item["context"]["emission_kind"] for item in published] == [
        "start",
        "complete",
        "end",
    ]
    assert "termination_reason" not in published[0]["context"]
    assert published[1]["context"]["termination_reason"] == "natural"
    assert published[2]["context"]["termination_reason"] == "natural"


def test_retry_pending_reuses_same_observation_id_and_advances_sequence(
    tmp_path,
) -> None:
    scenario = load_test_scenario(tmp_path)
    pending = simulate_scenario.make_scenario_observation(
        "SIM-EAR-001",
        4,
        scenario["source_id"],
        scenario["observations"][0],
        datetime(2026, 10, 5, 18, 0, tzinfo=timezone.utc),
        1_000_000,
    )
    credentials = {
        "ear_a": {
            "node_id": "SIM-EAR-001",
            "next_sequence_number": 4,
            "pending_observation": pending,
            "pending_scenario_id": scenario["scenario_id"],
        }
    }
    saved = []
    published = []

    ids = simulate_scenario.retry_pending_observations(
        scenario,
        credentials,
        {"ear_a": Path("ear-a.json")},
        "ca.crt",
        publish=lambda current, payload, ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        save=lambda path, value: saved.append((path, dict(value))),
        publish_keepalive=lambda *args, **kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )

    assert ids == [pending["identity"]["observation_id"]]
    assert (
        published[0]["identity"]["observation_id"]
        == pending["identity"]["observation_id"]
    )
    assert credentials["ear_a"]["next_sequence_number"] == 5
    assert "pending_observation" not in credentials["ear_a"]
    assert saved[0][0] == Path("ear-a.json")


def test_retry_pending_rejects_pending_observation_from_different_scenario(
    tmp_path,
) -> None:
    scenario = load_test_scenario(tmp_path)
    credentials = {
        "ear_a": {
            "pending_observation": {"observation_id": "saved"},
            "pending_scenario_id": "other-scenario",
            "next_sequence_number": 2,
        }
    }
    with pytest.raises(
        simulate_scenario.ScenarioError, match="belongs to another scenario"
    ):
        simulate_scenario.retry_pending_observations(
            scenario,
            credentials,
            {"ear_a": Path("ear-a.json")},
            "ca.crt",
            publish=lambda *args: pytest.fail(
                "must not publish another scenario's observation"
            ),
            publish_keepalive=lambda *args, **kwargs: None,
            make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
            announce=False,
        )


def test_retry_pending_rejects_observation_for_a_different_ear(tmp_path) -> None:
    scenario = load_test_scenario(tmp_path)
    pending = simulate_scenario.make_scenario_observation(
        "SIM-EAR-001",
        4,
        scenario["source_id"],
        scenario["observations"][0],
        datetime(2026, 10, 5, 18, 0, tzinfo=timezone.utc),
        1_000_000,
    )
    credentials = {
        "ear_a": {
            "node_id": "SIM-EAR-002",
            "next_sequence_number": 4,
            "pending_observation": pending,
            "pending_scenario_id": scenario["scenario_id"],
        }
    }

    with pytest.raises(simulate_scenario.ScenarioError, match="belongs to another Ear"):
        simulate_scenario.retry_pending_observations(
            scenario,
            credentials,
            {"ear_a": Path("ear-a.json")},
            "ca.crt",
            publish=lambda *args: pytest.fail(
                "must not publish an observation for another Ear"
            ),
            publish_keepalive=lambda *args, **kwargs: None,
            make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
            announce=False,
        )


def test_scripted_mode_preserves_authored_observations(tmp_path) -> None:
    loaded = load_test_scenario(tmp_path)
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="scripted",
        time_step_seconds=1.0,
        max_range_m=4000.0,
    )
    assert diagnostics is None
    assert prepared["simulation_mode"] == "scripted"
    assert [item["at_seconds"] for item in prepared["observations"]] == [0, 2, 5]
    assert [item["ear"] for item in prepared["observations"]] == [
        "ear_a",
        "ear_b",
        "ear_a",
    ]


def test_independent_detection_evaluates_all_ears_each_step(tmp_path) -> None:
    loaded = load_test_scenario(tmp_path)
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=500.0,
        observation_interval_seconds=1.0,
    )
    assert diagnostics is not None
    assert diagnostics.ear_count == 2
    assert diagnostics.total_evaluations == diagnostics.simulation_steps * 2
    assert diagnostics.simulation_steps >= 2
    # Every step evaluates both ears; detections only when in range.
    assert all(
        stats.evaluations == diagnostics.simulation_steps
        for stats in diagnostics.per_ear.values()
    )
    assert prepared["simulation_mode"] == "independent_detection"
    assert all("bearing_confidence" not in event for event in prepared["observations"])
    assert prepared["observations"]
    # Stage 2: levels vary with distance (not a fixed -40 placeholder).
    levels = [event["signal_level_dbfs"] for event in prepared["observations"]]
    assert all(level <= 0 for level in levels)
    distances = [event["source_distance_m"] for event in prepared["observations"]]
    if len(set(round(d, 3) for d in distances)) > 1:
        assert len(set(round(level, 6) for level in levels)) > 1


def test_independent_detection_is_deterministic(tmp_path) -> None:
    loaded = load_test_scenario(tmp_path)
    first, first_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=800.0,
        observation_interval_seconds=1.0,
    )
    second, second_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=800.0,
        observation_interval_seconds=1.0,
    )
    assert [
        (e["at_seconds"], e["ear"], e["bearing_deg"], e["source_distance_m"])
        for e in first["observations"]
    ] == [
        (e["at_seconds"], e["ear"], e["bearing_deg"], e["source_distance_m"])
        for e in second["observations"]
    ]
    assert first_diag.observations_generated == second_diag.observations_generated


def test_independent_detection_omits_bearing_confidence_in_payload(tmp_path) -> None:
    loaded = load_test_scenario(tmp_path)
    prepared, _ = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=2000.0,
        observation_interval_seconds=1.0,
    )
    assert prepared["observations"]
    payload = simulate_scenario.make_scenario_observation(
        "SIM-EAR-001",
        1,
        loaded["source_id"],
        prepared["observations"][0],
        datetime(2026, 10, 5, 18, 0, tzinfo=timezone.utc),
        1_000_000,
    )
    assert "confidence" not in payload["spatial"]["bearing"]
    assert payload["acoustic"]["signal_level_dbfs"] <= 0
    assert payload["acoustic"]["classification"]["confidence"] == 0.5
    # Distance-dependent: nearer observation is louder (or equal if clamped).
    nearer = min(prepared["observations"], key=lambda item: item["source_distance_m"])
    farther = max(prepared["observations"], key=lambda item: item["source_distance_m"])
    assert nearer["signal_level_dbfs"] >= farther["signal_level_dbfs"]


def test_scripted_detection_defaults_to_complete_lifecycle() -> None:
    payload = simulate_scenario.make_scenario_observation(
        "SIM-EAR-001",
        7,
        "SIM-SRC",
        {
            "at_seconds": 12,
            "ear": "ear_a",
            "bearing_deg": 90.0,
            "bearing_confidence": 0.5,
            "classification_confidence": 0.6,
            "signal_level_dbfs": -34.0,
            "duration_ms": 700,
        },
        datetime(2026, 11, 3, 18, 0, tzinfo=timezone.utc),
        1_000_000,
    )
    assert payload["protocol_version"] == 1
    assert payload["message_type"] == "observation"
    assert set(payload) >= {"identity", "context", "acoustic", "spatial"}
    assert payload["context"]["emission_kind"] == "complete"
    assert payload["context"]["termination_reason"] == "natural"
    assert payload["identity"]["sequence_number"] == 7
    # Hub protocol v1 schema accepts the payload.
    simulate_scenario._observation_body().model_validate(payload)


def test_assign_scripted_ear_lifecycles_groups_per_ear() -> None:
    events = [
        {"at_seconds": 1, "ear": "ear_b"},
        {"at_seconds": 2, "ear": "ear_b"},
        {"at_seconds": 3, "ear": "ear_a"},
        {"at_seconds": 4, "ear": "ear_b"},
    ]
    assigned = simulate_scenario.assign_scripted_ear_observation_lifecycles(events)
    assert [item["emission_kind"] for item in assigned] == [
        "start",
        "periodic",
        "complete",
        "end",
    ]
    assert assigned[0]["observation_id"] == assigned[1]["observation_id"] == assigned[3]["observation_id"]
    assert assigned[2]["observation_id"] != assigned[0]["observation_id"]
    assert assigned[2]["termination_reason"] == "natural"
    assert assigned[3]["termination_reason"] == "natural"
    assert "termination_reason" not in assigned[0]
    # Original list unchanged.
    assert "emission_kind" not in events[0]


def test_helicopter_scripted_run_emits_full_lifecycle_per_ear() -> None:
    path = (
        Path(__file__).parents[3]
        / "simulator"
        / "ear"
        / "scenarios"
        / "helicopter_east_west.json"
    )
    scenario = simulate_scenario.load_scenario(path)
    assert len(scenario["observations"]) == 18
    authored = [
        (item["at_seconds"], item["ear"], item["signal_level_dbfs"])
        for item in scenario["observations"]
    ]
    trajectory = (
        scenario["movement"]["start"],
        scenario["movement"]["end"],
        scenario["movement"]["speed_m_s"],
    )

    credentials = {
        "ear_a": {"node_id": "SIM-EAR-001", "next_sequence_number": 1},
        "ear_b": {"node_id": "SIM-EAR-002", "next_sequence_number": 10},
        "ear_c": {"node_id": "SIM-EAR-003", "next_sequence_number": 100},
    }
    published: list[dict] = []

    ids = simulate_scenario.run_scenario(
        scenario,
        credentials,
        {
            "ear_a": Path("a.json"),
            "ear_b": Path("b.json"),
            "ear_c": Path("c.json"),
        },
        "ca.crt",
        1.0,
        publish=lambda _credentials, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _seconds: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 3, 18, 0, tzinfo=timezone.utc),
        started_at_monotonic_us=0,
        save=lambda *_args, **_kwargs: None,
        publish_keepalive=lambda *_args, **_kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )

    assert len(published) == 18
    assert len(ids) == 18
    assert len(set(ids)) == 3
    assert [
        (item["at_seconds"], item["ear"], item["signal_level_dbfs"])
        for item in scenario["observations"]
    ] == authored
    assert (
        scenario["movement"]["start"],
        scenario["movement"]["end"],
        scenario["movement"]["speed_m_s"],
    ) == trajectory

    ObservationBody = simulate_scenario._observation_body()
    by_node: dict[str, list[dict]] = {
        "SIM-EAR-001": [],
        "SIM-EAR-002": [],
        "SIM-EAR-003": [],
    }
    for payload in published:
        ObservationBody.model_validate(payload)
        by_node[payload["identity"]["node_id"]].append(payload)

    for node_id, reports in by_node.items():
        assert len(reports) == 6
        kinds = [item["context"]["emission_kind"] for item in reports]
        assert kinds == ["start", "periodic", "periodic", "periodic", "periodic", "end"]
        assert len({item["identity"]["observation_id"] for item in reports}) == 1
        assert "termination_reason" not in reports[0]["context"]
        assert all("termination_reason" not in item["context"] for item in reports[1:5])
        assert reports[-1]["context"]["termination_reason"] == "natural"
        sequences = [item["identity"]["sequence_number"] for item in reports]
        assert sequences == sorted(sequences)
        assert len(sequences) == len(set(sequences))

    assert credentials["ear_a"]["next_sequence_number"] == 7
    assert credentials["ear_b"]["next_sequence_number"] == 16
    assert credentials["ear_c"]["next_sequence_number"] == 106


def test_helicopter_independent_range_sweep_counts() -> None:
    path = (
        Path(__file__).parents[3]
        / "simulator"
        / "ear"
        / "scenarios"
        / "helicopter_east_west.json"
    )
    loaded = simulate_scenario.load_scenario(path)
    rows = []
    for max_range in (1000, 2000, 3000, 4000, 5000):
        prepared, diagnostics = simulate_scenario.apply_simulation_mode(
            loaded,
            mode="independent_detection",
            time_step_seconds=1.0,
            max_range_m=float(max_range),
            observation_interval_seconds=1.0,
        )
        assert diagnostics.total_evaluations == diagnostics.simulation_steps * 3
        counts = {
            alias: stats.detections for alias, stats in diagnostics.per_ear.items()
        }
        rows.append((max_range, counts, diagnostics.observations_generated))
        # Ear B is closest; at 1 km only B (and maybe none of A).
        if max_range == 1000:
            assert counts["ear_a"] == 0
            assert counts["ear_b"] > 0
        if max_range >= 4000:
            assert counts["ear_a"] > 0
            assert counts["ear_b"] > 0
            assert counts["ear_c"] > 0
        # Detection windows come from the model, not authored exclusive blocks.
        authored_times = {
            (item["at_seconds"], item["ear"]) for item in loaded["observations"]
        }
        generated = {
            (item["at_seconds"], item["ear"]) for item in prepared["observations"]
        }
        assert generated != authored_times
    # Larger range must not reduce total observations.
    totals = [total for _, _, total in rows]
    assert totals == sorted(totals)
