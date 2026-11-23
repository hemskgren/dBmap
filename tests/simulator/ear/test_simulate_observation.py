from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import pytest

spec = spec_from_file_location(
    "dbmap_ear_simulator",
    Path(__file__).parents[3] / "simulator" / "ear" / "simulate_observation.py",
)
assert spec is not None and spec.loader is not None
simulate_observation = module_from_spec(spec)
spec.loader.exec_module(simulate_observation)


def test_simulator_identity_requires_simulated_ear_prefix() -> None:
    simulate_observation.require_simulator_identity("SIM-EAR-001")

    with pytest.raises(
        simulate_observation.UnsupportedSimulatorIdentity,
        match="hub must be rebuilt and redeployed",
    ):
        simulate_observation.require_simulator_identity("EAR-001")


def test_slot_node_id_mismatch_stops_before_finish_provisioning(
    monkeypatch, tmp_path
) -> None:
    credentials_path = tmp_path / "sim-ear-d.json"
    args = SimpleNamespace(
        api_url="https://hub.ear-test.invalid:8443",
        live_mqtt=True,
        ca="ca.crt",
        credentials_file=str(credentials_path),
        ear="d",
        reuse_credentials=False,
    )
    monkeypatch.setattr(
        simulate_observation,
        "api_request",
        lambda *a, **k: {
            "node_id": "SIM-EAR-008",
            "bootstrap_token": "tok",
        },
    )
    finish_calls = []

    def boom(*_args, **_kwargs):
        finish_calls.append(True)
        raise AssertionError("finish_provisioning must not run on slot mismatch")

    monkeypatch.setattr(simulate_observation, "finish_provisioning", boom)

    with pytest.raises(simulate_observation.SimEarSlotError, match="expects SIM-EAR-004"):
        simulate_observation.load_or_provision(args, "admin-token")
    assert finish_calls == []
    assert credentials_path.is_file()
    saved = credentials_path.read_text(encoding="utf-8")
    assert "SIM-EAR-008" in saved


def test_old_hub_identity_is_rejected_before_consuming_bootstrap_token(
    monkeypatch,
) -> None:
    monkeypatch.setattr(
        simulate_observation,
        "api_request",
        lambda *args, **kwargs: pytest.fail("provision endpoint must not be called"),
    )
    args = SimpleNamespace(api_url="https://hub.example", ca="ca.crt")

    with pytest.raises(simulate_observation.UnsupportedSimulatorIdentity):
        simulate_observation.finish_provisioning(
            args,
            {"node_id": "EAR-001", "bootstrap_token": "one-time-token"},
            "admin-token",
        )


def test_observation_batch_uses_unique_ids_and_monotonic_sequences(
    monkeypatch, tmp_path
) -> None:
    observation = simulate_observation.make_observation("SIM-EAR-001")
    credentials = {
        "node_id": "SIM-EAR-001",
        "mqtt": {
            "host": "mqtt.ear-test.invalid", "port": 8883, "use_tls": True,
            "username": "sim-ear-001", "password": "isolated-test-password",
        },
        "topics": {"observation": "esp-ear/local/sim-ear-001/observation"},
        "observation": observation,
        "pending_observation": observation.copy(),
        "next_sequence_number": 1,
    }
    published = []
    sleeps = []

    def record_publish(current_credentials, payload, ca_file, *, live):
        assert live
        published.append(payload.copy())
        return payload["identity"]["observation_id"]

    monkeypatch.setattr(simulate_observation, "publish_payload", record_publish)
    monkeypatch.setattr(simulate_observation.time, "sleep", sleeps.append)
    ids = simulate_observation.publish_observation_batch(
        credentials,
        "ca.crt",
        tmp_path / "credentials.json",
        count=3,
        interval_seconds=15,
        vehicle_id="SIM-VEHICLE-77",
        live=True,
    )

    assert len(ids) == 3
    assert len(set(ids)) == 3
    assert [item["identity"]["sequence_number"] for item in published] == [1, 2, 3]
    assert {
        item["acoustic"]["classification"]["hints"]["simulated_source_id"]
        for item in published
    } == {"SIM-VEHICLE-77"}
    assert sleeps == [15, 15]
    assert credentials["next_sequence_number"] == 4
    assert "pending_observation" not in credentials
