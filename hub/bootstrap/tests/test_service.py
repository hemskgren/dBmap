import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock
from uuid import uuid4

import pytest
from app.config import settings
from fastapi.testclient import TestClient
from sqlalchemy import inspect, text

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.mqtt_host = "127.0.0.1"

from app import (
    mqtt_listener,
    service,
)
from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import (
    Base,
    Node,
    NodeCredential,
    ReportedState,
)
from app.mqtt_security import MqttSecurityError
from app.schemas import (
    ObservationBearing,
    ObservationBody,
    ObservationClassification,
)
from app.service import ingest_observation, list_observations
from app.topics import parse_observation_topic


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
        "classification": {
            "source_family": "vehicle",
            "confidence": 0.72,
            "hints": {"simulated_source_id": "SIM-VEHICLE-001"},
        },
        "bearing": {"deg": 180.0, "reference": "node", "confidence": 0.61},
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


def test_viewer_can_read_device_summaries_but_not_admin_api() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    viewer_headers = {"Authorization": f"Bearer {settings.viewer_token}"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear", "hardware_revision": "EAR-DEV-V1"},
        headers=admin_headers,
    )
    assert created.status_code == 200, created.text

    assert client.get("/api/v1/session").status_code == 401
    viewer_session = client.get("/api/v1/session", headers=viewer_headers)
    assert viewer_session.status_code == 200
    assert viewer_session.json() == {"role": "viewer"}
    admin_session = client.get("/api/v1/session", headers=admin_headers)
    assert admin_session.status_code == 200
    assert admin_session.json() == {"role": "admin"}

    devices = client.get("/api/v1/devices", headers=viewer_headers)
    assert devices.status_code == 200, devices.text
    device = next(item for item in devices.json() if item["node_id"] == created.json()["node_id"])
    assert device["status"] == "offline"
    assert device["hardware_revision"] == "EAR-DEV-V1"
    assert "desired" not in device
    assert "reported" not in device
    assert client.get("/api/v1/nodes", headers=viewer_headers).status_code == 403
    assert client.post(
        "/api/v1/nodes",
        json={"node_type": "ear"},
        headers=viewer_headers,
    ).status_code == 403


def test_device_summary_status_uses_recent_keepalive() -> None:
    client = TestClient(create_app())
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear"},
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]

    db = SessionLocal()
    try:
        service.apply_reported(db, node_id, {})
    finally:
        db.close()

    response = client.get(
        "/api/v1/devices",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    device = next(item for item in response.json() if item["node_id"] == node_id)
    assert device["status"] == "online"

    db = SessionLocal()
    try:
        reported = db.get(ReportedState, node_id)
        assert reported is not None
        reported.last_seen_at = datetime.now(timezone.utc) - timedelta(seconds=91)
        db.commit()
    finally:
        db.close()

    response = client.get(
        "/api/v1/devices",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    device = next(item for item in response.json() if item["node_id"] == node_id)
    assert device["status"] == "offline"


def test_init_db_drops_legacy_plaintext_password_column() -> None:
    with engine.begin() as connection:
        connection.execute(
            text("ALTER TABLE node_credentials ADD COLUMN mqtt_password_plain_once VARCHAR")
        )

    init_db()

    columns = {column["name"] for column in inspect(engine).get_columns("node_credentials")}
    assert "mqtt_password_plain_once" not in columns


def test_init_db_adds_lifecycle_state_to_existing_nodes() -> None:
    with engine.begin() as connection:
        connection.execute(text("ALTER TABLE nodes DROP COLUMN lifecycle_state"))
        connection.execute(
            text(
                "INSERT INTO nodes (node_id, node_type, hardware_revision, "
                "provisioning_state, availability, created_at, updated_at) "
                "VALUES ('LEGACY-EAR-001', 'ear', 'unknown', 'pending', 'SERVICE', "
                "'2026-01-01T00:00:00+00:00', '2026-01-01T00:00:00+00:00')"
            )
        )

    init_db()

    with engine.connect() as connection:
        lifecycle_state = connection.execute(
            text("SELECT lifecycle_state FROM nodes WHERE node_id = 'LEGACY-EAR-001'")
        ).scalar_one()
    assert lifecycle_state == "active"


def test_deactivated_node_blocks_provisioning_and_observations_until_reactivated() -> None:
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear", "hardware_revision": "EAR-DEV-V1"},
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]
    bootstrap_token = created.json()["bootstrap_token"]

    headers = {"Authorization": f"Bearer {settings.admin_token}"}
    deactivated = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        json={"lifecycle_state": "deactivated"},
        headers=headers,
    )
    assert deactivated.status_code == 200, deactivated.text
    assert deactivated.json()["lifecycle_state"] == "deactivated"
    assert client.post(
        "/api/v1/provision",
        json={"bootstrap_token": bootstrap_token, "hardware_revision": "EAR-DEV-V1"},
    ).status_code == 400

    reactivated = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        json={"lifecycle_state": "active"},
        headers=headers,
    )
    assert reactivated.status_code == 200, reactivated.text
    assert reactivated.json()["lifecycle_state"] == "active"
    provisioned = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": bootstrap_token, "hardware_revision": "EAR-DEV-V1"},
    )
    assert provisioned.status_code == 200, provisioned.text

    deactivated_again = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        json={"lifecycle_state": "deactivated"},
        headers=headers,
    )
    assert deactivated_again.status_code == 200
    body = ObservationBody.model_validate(valid_observation_payload(node_id))
    db = SessionLocal()
    try:
        with pytest.raises(ValueError, match="node is deactivated"):
            ingest_observation(db, node_id, body)
    finally:
        db.close()

    reactivated_again = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        json={"lifecycle_state": "active"},
        headers=headers,
    )
    assert reactivated_again.status_code == 200
    db = SessionLocal()
    try:
        assert ingest_observation(db, node_id, body) is True
    finally:
        db.close()


def test_node_deactivation_reports_broker_acl_failure_without_changing_state(monkeypatch) -> None:
    client = TestClient(create_app())
    headers = {"Authorization": f"Bearer {settings.admin_token}"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "output", "hardware_revision": "OUTPUT-DEV-V1"},
        headers=headers,
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]

    monkeypatch.setattr(settings, "mqtt_enabled", True)
    monkeypatch.setattr(
        service,
        "set_node_security_active",
        Mock(side_effect=MqttSecurityError("test failure")),
    )
    response = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        json={"lifecycle_state": "deactivated"},
        headers=headers,
    )

    assert response.status_code == 503
    assert "node remains active" in response.json()["detail"]
    fetched = client.get(f"/api/v1/nodes/{node_id}", headers=headers)
    assert fetched.status_code == 200
    assert fetched.json()["lifecycle_state"] == "active"


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
        classification=ObservationClassification(
            source_family="vehicle",
            confidence=0.72,
            hints={"simulated_source_id": "SIM-VEHICLE-001"},
        ),
        bearing=ObservationBearing(
            deg=180,
            reference="node",
            confidence=0.61,
        ),
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
        assert observations[0].classification is not None
        assert observations[0].classification.source_family == "vehicle"
        assert observations[0].classification.confidence == 0.72
        assert observations[0].classification.hints == {"simulated_source_id": "SIM-VEHICLE-001"}
        assert observations[0].bearing is not None
        assert observations[0].bearing.deg == 180
        assert observations[0].bearing.reference == "node"
        assert observations[0].bearing.confidence == 0.61

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
    assert all(item["node_id"] == "EAR-007" for item in response.json())

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
        "bearing": {"deg": 180, "reference": "node", "confidence": 0.5},
    }
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload["event_time_utc"] = "2026-10-04T20:00:00+02:00"
    payload["bearing"]["deg"] = 360
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload["bearing"]["deg"] = 359.9
    body = ObservationBody.model_validate(payload)
    assert body.event_time_utc.isoformat() == "2026-10-04T18:00:00+00:00"


def test_observation_rejects_out_of_range_confidences_and_legacy_shape() -> None:
    payload = valid_observation_payload()
    payload["classification"]["confidence"] = 1.1
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload = valid_observation_payload()
    payload["bearing"]["confidence"] = -0.1
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload = valid_observation_payload()
    payload["confidence"] = 0.72
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload = valid_observation_payload()
    payload["bearing_deg"] = payload.pop("bearing")["deg"]
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)


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
    assert listed[0].classification is not None
    assert listed[0].classification.confidence == 0.72
    assert listed[0].bearing is not None
    assert listed[0].bearing.deg == 180


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
