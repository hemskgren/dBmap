from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import pytest

spec = spec_from_file_location(
    "dbmap_ear_simulator",
    Path(__file__).with_name("simulate_observation.py"),
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


def test_old_hub_identity_is_rejected_before_consuming_bootstrap_token(monkeypatch) -> None:
    monkeypatch.setattr(
        simulate_observation,
        "api_request",
        lambda *args, **kwargs: pytest.fail("provision endpoint must not be called"),
    )
    args = SimpleNamespace(api_url="https://hub.example:8443", ca="ca.crt")

    with pytest.raises(simulate_observation.UnsupportedSimulatorIdentity):
        simulate_observation.finish_provisioning(
            args,
            {"node_id": "EAR-001", "bootstrap_token": "one-time-token"},
            "admin-token",
        )


def test_observation_batch_uses_unique_ids_and_monotonic_sequences(monkeypatch, tmp_path) -> None:
    observation = simulate_observation.make_observation("SIM-EAR-001")
    credentials = {
        "node_id": "SIM-EAR-001",
        "mqtt": {},
        "topics": {},
        "observation": observation,
        "pending_observation": observation.copy(),
        "next_sequence_number": 1,
    }
    published = []
    sleeps = []

    def record_publish(current_credentials, payload, ca_file):
        published.append(payload.copy())
        return payload["observation_id"]

    monkeypatch.setattr(simulate_observation, "publish_payload", record_publish)
    monkeypatch.setattr(simulate_observation.time, "sleep", sleeps.append)
    ids = simulate_observation.publish_observation_batch(
        credentials,
        "ca.crt",
        tmp_path / "credentials.json",
        count=3,
        interval_seconds=15,
        vehicle_id="SIM-VEHICLE-77",
    )

    assert len(ids) == 3
    assert len(set(ids)) == 3
    assert [item["sequence_number"] for item in published] == [1, 2, 3]
    assert {
        item["classification_hints"]["simulated_source_id"] for item in published
    } == {"SIM-VEHICLE-77"}
    assert sleeps == [15, 15]
    assert credentials["next_sequence_number"] == 4
    assert "pending_observation" not in credentials
