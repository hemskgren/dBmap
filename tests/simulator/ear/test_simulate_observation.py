import json
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
        simulate_observation.load_or_provision(args, "x" * 32)
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


def test_provisioning_token_matches_destination(monkeypatch) -> None:
    monkeypatch.setenv("DBMAP_ADMIN_TOKEN", "local-admin-token")
    monkeypatch.setenv("DBMAP_TEST_HUB_ADMIN_TOKEN", "isolated-test-token")

    test_variable, test_token = simulate_observation.admin_token_for_destination(
        "https://hub.ear-test.invalid:8443"
    )
    local_variable, local_token = simulate_observation.admin_token_for_destination(
        "https://hub.local:8443"
    )

    assert (test_variable, test_token) == (
        "DBMAP_TEST_HUB_ADMIN_TOKEN",
        "isolated-test-token",
    )
    assert (local_variable, local_token) == ("DBMAP_ADMIN_TOKEN", "local-admin-token")


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


def test_republishing_saved_ear_does_not_require_hub_admin_token(
    monkeypatch, tmp_path, capsys
) -> None:
    observation = simulate_observation.make_observation("SIM-EAR-001")
    record = {
        "state": "provisioned",
        "node_id": "SIM-EAR-001",
        "mqtt": {
            "host": "local-hub.example",
            "port": 8883,
            "use_tls": True,
            "username": "sim-ear-001",
            "password": "do-not-print-this",
        },
        "topics": {
            "observation": "esp-ear/local/sim-ear-001/observation",
            "keepalive": "esp-ear/local/sim-ear-001/keepalive",
        },
        "observation": observation,
    }
    credentials_path = tmp_path / "sim-ear-a.json"
    credentials_path.write_text(json.dumps(record), encoding="utf-8")
    credentials_path.chmod(0o600)
    ca_file = tmp_path / "ca.crt"
    ca_file.touch()
    monkeypatch.setenv("DBMAP_LOCAL_MQTT_HOST", "local-hub.example")
    monkeypatch.setenv("DBMAP_LOCAL_MQTT_PORT", "8883")
    monkeypatch.delenv("DBMAP_ADMIN_TOKEN", raising=False)
    monkeypatch.delenv("DBMAP_TEST_HUB_ADMIN_TOKEN", raising=False)
    monkeypatch.setattr(simulate_observation, "load_simulator_environment", lambda: None)
    monkeypatch.setattr(
        simulate_observation.sys,
        "argv",
        [
            simulate_observation.__file__,
            "publish",
            "--credentials-file",
            str(credentials_path),
            "--ca",
            str(ca_file),
        ],
    )
    monkeypatch.setattr(
        simulate_observation,
        "publish_ear_keepalive",
        lambda *_args, **_kwargs: None,
    )
    monkeypatch.setattr(
        simulate_observation,
        "publish_payload",
        lambda *_args, **_kwargs: "observation-id",
    )
    monkeypatch.setattr(
        simulate_observation,
        "api_request",
        lambda *_args, **_kwargs: pytest.fail("republish must not call the Hub API"),
    )

    assert simulate_observation.main() == 0
    output = capsys.readouterr().out
    assert "MQTT destination: TLS local-hub.example:8883" in output
    assert "do-not-print-this" not in output
