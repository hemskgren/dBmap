import json
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from uuid import uuid4

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import inspect, text

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.mqtt_enabled = False
settings.mqtt_host = "127.0.0.1"

from app.db import SessionLocal, engine, init_db  # noqa: E402
from app.models import Node, NodeCredential  # noqa: E402
from app.models import Base  # noqa: E402
from app.main import create_app  # noqa: E402
from app import mqtt_listener  # noqa: E402
from app.schemas import ObservationBody  # noqa: E402
from app.service import ingest_observation, list_observations  # noqa: E402
from app.topics import parse_observation_topic  # noqa: E402


def setup_module() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def valid_observation_payload(node_id: str = "SIM-EAR-001") -> dict:
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": str(uuid4()),
        "node_id": node_id,
        "sequence_number": 1,
        "event_time_utc": datetime.now(timezone.utc).isoformat(),
        "capture_timestamp_monotonic_us": 123456,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "classification_hints": {"source_family": "vehicle"},
        "confidence": 0.72,
        "bearing_deg": 180.0,
        "signal_level_dbfs": -34.0,
        "duration_ms": 850.0,
    }


def test_create_provision_and_desired_state() -> None:
    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}

    created = client.post("/api/v1/nodes", json={"node_type": "output"}, headers=headers)
    assert created.status_code == 200
    body = created.json()
    assert body["node_id"].startswith("OUT-")
    token = body["bootstrap_token"]

    provisioned = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": token, "hardware_revision": "OUTPUT-DEV-V1", "firmware_version": "0.1.0"},
    )
    assert provisioned.status_code == 200
    prov = provisioned.json()
    assert prov["mqtt"]["username"] == prov["node_id"].lower()
    assert prov["mqtt"]["password"]
    assert prov["mqtt"]["use_tls"] is True
    assert "esp-output/local/" in prov["topics"]["command"]
    assert prov["topics"]["observation"] == f"esp-output/local/{prov['node_id'].lower()}/observation"
    assert not hasattr(NodeCredential, "mqtt_password_plain_once")

    node = client.get(f"/api/v1/nodes/{prov['node_id']}", headers=headers)
    assert node.status_code == 200
    view = node.json()
    assert view["provisioning_state"] == "provisioned"

    desired = client.put(
        f"/api/v1/nodes/{prov['node_id']}/desired",
        json={"config_version": 18, "firmware_version": "0.1.0"},
        headers=headers,
    )
    assert desired.status_code == 200
    assert desired.json()["pending_configuration_change"] is True


def test_init_db_drops_legacy_plaintext_password_column() -> None:
    with engine.begin() as connection:
        connection.execute(
            text("ALTER TABLE node_credentials ADD COLUMN mqtt_password_plain_once VARCHAR")
        )

    init_db()

    columns = {column["name"] for column in inspect(engine).get_columns("node_credentials")}
    assert "mqtt_password_plain_once" not in columns


def test_installation_requires_geographic_position_and_is_returned_by_node_api() -> None:
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post("/api/v1/nodes", json={"node_type": "output"}, headers=headers)
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]

    missing_longitude = client.put(
        f"/api/v1/nodes/{node_id}/installation",
        json={"latitude": 59.91},
        headers=headers,
    )
    assert missing_longitude.status_code == 422

    invalid_position = client.put(
        f"/api/v1/nodes/{node_id}/installation",
        json={"latitude": 91, "longitude": 10},
        headers=headers,
    )
    assert invalid_position.status_code == 422

    installation = {
        "latitude": 59.91,
        "longitude": 10.75,
        "floor": 7,
        "height_m": 21,
        "height_accuracy_m": 2,
        "mount_type": "balcony",
        "environment": "urban",
        "orientation_deg": 180,
    }
    updated = client.put(
        f"/api/v1/nodes/{node_id}/installation",
        json=installation,
        headers=headers,
    )
    assert updated.status_code == 200
    assert updated.json()["installation"] == installation

    fetched = client.get(f"/api/v1/nodes/{node_id}", headers=headers)
    assert fetched.status_code == 200
    assert fetched.json()["installation"] == installation

    unauthorized = client.put(
        f"/api/v1/nodes/{node_id}/installation",
        json=installation,
    )
    assert unauthorized.status_code == 401


def test_observations_are_validated_deduplicated_and_listed() -> None:
    observation_id = uuid4()
    body = ObservationBody(
        protocol_version=1,
        message_type="observation",
        observation_id=observation_id,
        node_id="EAR-007",
        sequence_number=1,
        event_time_utc=datetime.now(timezone.utc),
        capture_timestamp_monotonic_us=123456,
        timing_quality="estimated",
        timestamp_uncertainty_ms=10,
        classification_hints={"source_family": "vehicle"},
        confidence=0.72,
        bearing_deg=180,
        signal_level_dbfs=-34,
        duration_ms=850,
    )
    db = SessionLocal()
    try:
        db.add(
            Node(
                node_id="EAR-007",
                node_type="ear",
                hardware_revision="EAR-SIM-V1",
                provisioning_state="active",
                availability="OPERATIONAL",
            )
        )
        db.commit()
        assert ingest_observation(db, "EAR-007", body) is True
        assert ingest_observation(db, "EAR-007", body) is False
        observations = list_observations(db, "ear-007", limit=10)
        assert len(observations) == 1
        assert str(observations[0].observation_id) == str(observation_id)
        assert observations[0].node_id == "EAR-007"
        assert observations[0].classification_hints == {"source_family": "vehicle"}

        with pytest.raises(ValueError, match="does not match topic"):
            ingest_observation(db, "EAR-008", body)
        with pytest.raises(ValueError, match="registered Ear"):
            ingest_observation(db, "OUT-003", body.model_copy(update={"node_id": "OUT-003"}))
    finally:
        db.close()

    client = TestClient(create_app())
    response = client.get(
        "/api/v1/observations?node_id=EAR-007&limit=10",
        headers={"Authorization": "Bearer test-admin"},
    )
    assert response.status_code == 200
    assert len(response.json()) == 1
    assert response.json()[0]["node_id"] == "EAR-007"

    unauthorized = client.get("/api/v1/observations")
    assert unauthorized.status_code == 401


def test_observation_requires_timezone_and_valid_bearing() -> None:
    payload = {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": str(uuid4()),
        "node_id": "EAR-008",
        "sequence_number": 1,
        "event_time_utc": "2026-10-04T20:00:00",
        "capture_timestamp_monotonic_us": 123456,
        "timing_quality": "estimated",
        "timestamp_uncertainty_ms": 10,
        "duration_ms": 100,
        "bearing_deg": 180,
    }
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload["event_time_utc"] = "2026-10-04T20:00:00+02:00"
    payload["bearing_deg"] = 360
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload["bearing_deg"] = 359.9
    body = ObservationBody.model_validate(payload)
    assert body.event_time_utc.isoformat() == "2026-10-04T18:00:00+00:00"


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("protocol_version", 2),
        ("message_type", "heartbeat"),
        ("protocol_version", None),
        ("message_type", None),
    ],
)
def test_observation_rejects_missing_or_unsupported_protocol_envelope(field, value) -> None:
    payload = valid_observation_payload()
    if value is None:
        payload.pop(field)
    else:
        payload[field] = value

    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)


@pytest.mark.parametrize(
    ("field", "value"),
    [("protocol_version", 2), ("message_type", "heartbeat")],
)
def test_mqtt_listener_rejects_unsupported_observation_protocol(
    monkeypatch,
    field: str,
    value: object,
) -> None:
    payload = valid_observation_payload()
    payload[field] = value
    monkeypatch.setattr(
        mqtt_listener,
        "SessionLocal",
        lambda: pytest.fail("invalid protocol must be rejected before database access"),
    )

    mqtt_listener._ingest_observation("SIM-EAR-001", json.dumps(payload).encode())


def test_simulator_observation_round_trips_through_hub_protocol() -> None:
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    invalid_simulated_output = client.post(
        "/api/v1/nodes",
        json={"node_type": "output", "simulated": True},
        headers=headers,
    )
    assert invalid_simulated_output.status_code == 422

    created = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "ear",
            "hardware_revision": "SIM-EAR-V1",
            "simulated": True,
        },
        headers=headers,
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]
    assert node_id == "SIM-EAR-001"

    provisioned = client.post(
        "/api/v1/provision",
        json={
            "bootstrap_token": created.json()["bootstrap_token"],
            "hardware_revision": "SIM-EAR-V1",
            "firmware_version": "0.1.0-simulator",
        },
    )
    assert provisioned.status_code == 200, provisioned.text
    assert provisioned.json()["node_id"] == node_id
    assert provisioned.json()["topics"]["observation"] == (
        f"esp-ear/local/{node_id.lower()}/observation"
    )

    payload = valid_observation_payload(node_id)
    body = ObservationBody.model_validate(payload)
    db = SessionLocal()
    try:
        assert ingest_observation(db, node_id, body) is True
        assert ingest_observation(db, node_id, body) is False
        listed = list_observations(db, node_id, limit=10)
    finally:
        db.close()

    assert len(listed) == 1
    assert listed[0].protocol_version == 1
    assert listed[0].message_type == "observation"
    assert listed[0].node_id == "SIM-EAR-001"


def test_observation_topic_parser_requires_exact_ear_topic_shape() -> None:
    assert parse_observation_topic("esp-ear/local/ear-007/observation") == "EAR-007"
    assert parse_observation_topic("esp-ear/local/EAR-007/observation") is None
    assert parse_observation_topic("esp-output/local/out-007/observation") is None
    assert parse_observation_topic("esp-ear/local/ear-007/observation/extra") is None


def test_mqtt_listener_routes_and_rejects_retained_observations(monkeypatch) -> None:
    received = []
    monkeypatch.setattr(
        mqtt_listener,
        "_ingest_observation",
        lambda node_id, payload: received.append((node_id, payload)),
    )
    payload = b'{"observation_id":"00000000-0000-0000-0000-000000000001"}'

    mqtt_listener._on_message(
        None,
        None,
        SimpleNamespace(
            topic="esp-ear/local/ear-007/observation",
            payload=payload,
            retain=False,
        ),
    )
    mqtt_listener._on_message(
        None,
        None,
        SimpleNamespace(
            topic="esp-ear/local/ear-007/observation",
            payload=payload,
            retain=True,
        ),
    )

    assert received == [("EAR-007", payload)]
