import json
from datetime import UTC, datetime, timedelta
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
    policy,
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
from app.policy import PolicyUnavailable
from app.schemas import InstallationMetadataBody, ObservationBody
from app.service import ingest_observation, list_observations
from app.topics import parse_observation_topic
from pydantic import ValidationError


def setup_module() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def _empty_l0_measurements() -> dict:
    return {
        "schema": "l0",
        "version": 1,
        "band_plan_version": 1,
        "crest_threshold_version": 1,
        "crest_threshold_db": 12.0,
        "mic_index": 0,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "window_count": 0,
        "clipped_window_count": 0,
        "n_crest": 0,
        "rms": {"mean_dbfs": None, "min_dbfs": None, "max_dbfs": None},
        "bands": {
            "mean_dbfs": [None, None, None, None],
            "fraction": [None, None, None, None],
        },
        "hl": {"mean_db": None},
        "t1": {"rms_stdev_db": None, "rms_range_db": None},
        "t2": {"rms_slope_db_per_s": None},
        "t3": {
            "crest_max_db": None,
            "high_crest_count": None,
            "high_crest_rate": None,
        },
        "t6": {"hl_slope_db_per_s": None},
    }


def valid_observation_payload(node_id: str = "SIM-EAR-001") -> dict:
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": {
            "observation_id": str(uuid4()),
            "node_id": node_id,
            "hub_id": "",
            "sequence_number": 1,
        },
        "context": {
            "event_time_utc": datetime.now(UTC).isoformat(),
            "capture_timestamp_monotonic_us": 123456,
            "timing_quality": "estimated",
            "clock_offset_ms": None,
            "timestamp_uncertainty_ms": 10.0,
            "interval_duration_ms": 850.0,
            "episode_duration_ms": 850.0,
            "emission_kind": "periodic",
        },
        "acoustic": {
            "signal_level_dbfs": -34.0,
            "measurements": _empty_l0_measurements(),
            "signature": None,
            "classification": {
                "schema": "l2",
                "version": 1,
                "source_family": "vehicle",
                "confidence": 0.72,
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"},
            },
        },
        "spatial": {
            "bearing": {"deg": 180.0, "reference": "node", "confidence": 0.61},
            "position": None,
        },
    }


def test_create_provision_and_desired_state() -> None:
    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}

    created = client.post(
        "/api/v1/nodes", json={"node_type": "output"}, headers=headers
    )
    assert created.status_code == 200
    body = created.json()
    assert body["node_id"].startswith("OUT-")
    token = body["bootstrap_token"]

    provisioned = client.post(
        "/api/v1/provision",
        json={
            "bootstrap_token": token,
            "hardware_revision": "OUTPUT-DEV-V1",
            "firmware_version": "0.1.0",
        },
    )
    assert provisioned.status_code == 200
    prov = provisioned.json()
    assert prov["mqtt"]["username"] == prov["node_id"].lower()
    assert prov["mqtt"]["password"]
    assert prov["mqtt"]["use_tls"] is True
    assert "esp-output/local/" in prov["topics"]["command"]
    assert (
        prov["topics"]["observation"]
        == f"esp-output/local/{prov['node_id'].lower()}/observation"
    )
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
    device = next(
        item for item in devices.json() if item["node_id"] == created.json()["node_id"]
    )
    assert device["status"] == "offline"
    assert device["hardware_revision"] == "EAR-DEV-V1"
    assert device["owner_type"] is None
    assert device["owner_id"] is None
    assert device["owner_display_name"] is None
    assert "desired" not in device
    assert "reported" not in device
    assert client.get("/api/v1/nodes", headers=viewer_headers).status_code == 403
    node_detail = client.get(
        f"/api/v1/nodes/{created.json()['node_id']}", headers=viewer_headers
    )
    assert node_detail.status_code == 200
    assert "reported" in node_detail.json()
    assert (
        client.put(
            f"/api/v1/nodes/{created.json()['node_id']}/installation",
            json={"latitude": 59.9, "longitude": 10.7},
            headers=viewer_headers,
        ).status_code
        == 403
    )
    assert (
        client.post(
            "/api/v1/nodes",
            json={"node_type": "ear"},
            headers=viewer_headers,
        ).status_code
        == 403
    )


def test_api_fails_closed_when_policy_service_is_unavailable(monkeypatch) -> None:
    client = TestClient(create_app())
    monkeypatch.setattr(
        policy,
        "evaluate",
        Mock(
            side_effect=PolicyUnavailable("authorization policy service is unavailable")
        ),
    )

    response = client.get(
        "/api/v1/session",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert response.status_code == 503
    assert response.json()["detail"] == "authorization policy service is unavailable"


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
        reported.last_seen_at = datetime.now(UTC) - timedelta(
            seconds=settings.device_online_window_s + 1
        )
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
            text(
                "ALTER TABLE node_credentials ADD COLUMN mqtt_password_plain_once VARCHAR"
            )
        )

    init_db()

    columns = {
        column["name"] for column in inspect(engine).get_columns("node_credentials")
    }
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


def test_deactivated_node_blocks_provisioning_and_observations_until_reactivated() -> (
    None
):
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
    assert (
        client.post(
            "/api/v1/provision",
            json={
                "bootstrap_token": bootstrap_token,
                "hardware_revision": "EAR-DEV-V1",
            },
        ).status_code
        == 400
    )

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


def test_node_deactivation_reports_broker_acl_failure_without_changing_state(
    monkeypatch,
) -> None:
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


def test_installation_requires_geographic_position_and_is_returned_by_node_api() -> (
    None
):
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes", json={"node_type": "output"}, headers=headers
    )
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


def test_installation_coordinate_precision_roundtrip_and_range() -> None:
    """API/DB preserve IEEE float coords; no six-decimal cap (web must match)."""
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes", json={"node_type": "ear"}, headers=headers
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]

    cases = [
        # integers
        {"latitude": 60, "longitude": 10},
        # six decimal places
        {"latitude": 59.912345, "longitude": 10.752345},
        # seven+ decimal places (IEEE float, same as JSON/JS Number)
        {"latitude": 59.1234567, "longitude": 10.7654321},
        {"latitude": -33.8688197, "longitude": 151.2092955},
        # geographic boundaries
        {"latitude": -90, "longitude": -180},
        {"latitude": 90, "longitude": 180},
        {"latitude": 0, "longitude": 0},
    ]
    for coords in cases:
        updated = client.put(
            f"/api/v1/nodes/{node_id}/installation",
            json=coords,
            headers=headers,
        )
        assert updated.status_code == 200, (coords, updated.text)
        body = updated.json()["installation"]
        assert body["latitude"] == coords["latitude"]
        assert body["longitude"] == coords["longitude"]

        fetched = client.get(f"/api/v1/nodes/{node_id}", headers=headers)
        assert fetched.status_code == 200
        installation = fetched.json()["installation"]
        assert installation["latitude"] == coords["latitude"]
        assert installation["longitude"] == coords["longitude"]

    out_of_range = [
        {"latitude": 90.0000001, "longitude": 0},
        {"latitude": -90.1, "longitude": 0},
        {"latitude": 0, "longitude": 180.0000001},
        {"latitude": 0, "longitude": -180.1},
    ]
    for coords in out_of_range:
        rejected = client.put(
            f"/api/v1/nodes/{node_id}/installation",
            json=coords,
            headers=headers,
        )
        assert rejected.status_code == 422, coords

    # Non-finite values are rejected by the schema (allow_inf_nan=False).
    # Assert via model_validate: JSON cannot carry NaN/Inf in a standard body.
    for bad in (
        {"latitude": float("nan"), "longitude": 0.0},
        {"latitude": float("inf"), "longitude": 0.0},
        {"latitude": 0.0, "longitude": float("-inf")},
    ):
        with pytest.raises(ValidationError):
            InstallationMetadataBody.model_validate(bad)


def test_observations_are_validated_deduplicated_and_listed() -> None:
    observation_id = uuid4()
    payload = valid_observation_payload("EAR-007")
    payload["identity"]["observation_id"] = str(observation_id)
    body = ObservationBody.model_validate(payload)
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
        assert str(observations[0].identity.observation_id) == str(observation_id)
        assert observations[0].identity.node_id == "EAR-007"
        assert observations[0].acoustic.classification is not None
        assert observations[0].acoustic.classification.source_family == "vehicle"
        assert observations[0].acoustic.classification.confidence == 0.72
        assert observations[0].acoustic.classification.hints == {
            "simulated_source_id": "SIM-VEHICLE-001"
        }
        assert observations[0].spatial.bearing is not None
        assert observations[0].spatial.bearing.deg == 180
        assert observations[0].spatial.bearing.reference == "node"
        assert observations[0].spatial.bearing.confidence == 0.61

        with pytest.raises(ValueError, match="does not match topic"):
            ingest_observation(db, "EAR-008", body)
        with pytest.raises(ValueError, match="registered Ear"):
            ingest_observation(
                db,
                "OUT-003",
                body.model_copy(
                    update={
                        "identity": body.identity.model_copy(update={"node_id": "OUT-003"})
                    }
                ),
            )

    finally:
        db.close()

    client = TestClient(create_app())
    response = client.get(
        "/api/v1/observations?node_id=EAR-007&limit=10",
        headers={"Authorization": "Bearer test-admin"},
    )
    assert response.status_code == 200
    envelope = response.json()
    assert set(envelope.keys()) == {"pagination", "reports"}
    assert len(envelope["reports"]) == 1
    assert envelope["pagination"]["total_count"] == 1
    assert all(
        item["identity"]["node_id"] == "EAR-007" for item in envelope["reports"]
    )

    unauthorized = client.get("/api/v1/observations")
    assert unauthorized.status_code == 401


def test_observation_requires_timezone_and_valid_bearing() -> None:
    payload = valid_observation_payload("EAR-008")
    payload["context"]["event_time_utc"] = "2026-10-04T20:00:00"
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload["context"]["event_time_utc"] = "2026-10-04T20:00:00+02:00"
    payload["spatial"]["bearing"]["deg"] = 360
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload["spatial"]["bearing"]["deg"] = 359.9
    body = ObservationBody.model_validate(payload)
    assert body.context.event_time_utc.isoformat() == "2026-10-04T18:00:00+00:00"


def test_observation_count_endpoint_returns_table_total() -> None:
    node_id = "EAR-COUNT-1"
    db = SessionLocal()
    try:
        db.add(
            Node(
                node_id=node_id,
                node_type="ear",
                hardware_revision="EAR-SIM-V1",
                provisioning_state="active",
                availability="OPERATIONAL",
            )
        )
        db.commit()
        before = service.count_observations(db)
        before_unique = service.count_unique_observations(db)
        body = ObservationBody.model_validate(valid_observation_payload(node_id))
        assert ingest_observation(db, node_id, body) is True
        assert service.count_observations(db) == before + 1
        assert service.count_observations(db, node_id) == 1
        assert service.count_unique_observations(db) == before_unique + 1
        assert service.count_unique_observations(db, node_id) == 1
    finally:
        db.close()

    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    response = client.get("/api/v1/observations/count", headers=headers)
    assert response.status_code == 200
    body = response.json()
    assert body["total"] >= 1
    assert body["unique_observations"] >= 1
    assert body["unique_observations"] <= body["total"]
    filtered = client.get(
        f"/api/v1/observations/count?node_id={node_id}", headers=headers
    )
    assert filtered.status_code == 200
    assert filtered.json()["total"] == 1
    assert filtered.json()["unique_observations"] == 1
    unauthorized = client.get("/api/v1/observations/count")
    assert unauthorized.status_code == 401


def test_observation_accepts_null_classification_and_bearing() -> None:
    """Physical Ear Phase 1 publishes unclassified, bearing-less Observations."""
    payload = valid_observation_payload("EAR-PHASE1")
    payload["acoustic"]["classification"] = None
    payload["spatial"]["bearing"] = None
    payload["context"]["interval_duration_ms"] = 1000.0
    payload["context"]["episode_duration_ms"] = 1000.0
    payload["acoustic"]["signal_level_dbfs"] = -42.5
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.classification is None
    assert body.spatial.bearing is None
    assert body.acoustic.signal_level_dbfs == -42.5
    assert body.context.interval_duration_ms == 1000.0


def test_observation_rejects_out_of_range_confidences_and_legacy_shape() -> None:
    payload = valid_observation_payload()
    payload["acoustic"]["classification"]["confidence"] = 1.1
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload = valid_observation_payload()
    payload["spatial"]["bearing"]["confidence"] = -0.1
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload = valid_observation_payload()
    payload["confidence"] = 0.72
    with pytest.raises(ValueError):
        ObservationBody.model_validate(payload)

    payload = valid_observation_payload()
    payload["bearing_deg"] = payload["spatial"]["bearing"]["deg"]
    payload["spatial"]["bearing"] = None
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
def test_observation_rejects_missing_or_unsupported_protocol_envelope(
    field, value
) -> None:
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
    assert listed[0].identity.node_id == "SIM-EAR-001"
    assert listed[0].acoustic.classification is not None
    assert listed[0].acoustic.classification.confidence == 0.72
    assert listed[0].spatial.bearing is not None
    assert listed[0].spatial.bearing.deg == 180


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


def test_output_node_commands(monkeypatch) -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    viewer_headers = {"Authorization": f"Bearer {settings.viewer_token}"}

    created = client.post(
        "/api/v1/nodes", json={"node_type": "output"}, headers=admin_headers
    )
    assert created.status_code == 200
    node_id = created.json()["node_id"]
    token = created.json()["bootstrap_token"]

    blocked_pending = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "RELAY_1_ON", "duration_s": 5, "action_type": "output-test"},
        headers=admin_headers,
    )
    assert blocked_pending.status_code == 409
    assert "not completed Hub provisioning" in blocked_pending.json()["detail"]

    prov_res = client.post(
        "/api/v1/provision",
        json={
            "bootstrap_token": token,
            "hardware_revision": "OUTPUT-DEV-V1",
            "firmware_version": "0.1.0",
        },
    )
    assert prov_res.status_code == 200

    published_messages = []

    def fake_publish_json(topic, payload, qos=1):
        published_messages.append((topic, payload, qos))

    monkeypatch.setattr(mqtt_listener, "publish_json", fake_publish_json)

    expires_at = (datetime.now(UTC) + timedelta(seconds=30)).isoformat()
    test_cmd = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={
            "protocol_version": 1,
            "message_type": "output-test",
            "action": "RELAY_1_ON",
            "duration_s": 5,
            "expires_at_utc": expires_at,
        },
        headers=admin_headers,
    )
    assert test_cmd.status_code == 200, test_cmd.text
    cmd_data = test_cmd.json()
    assert cmd_data["node_id"] == node_id
    assert cmd_data["action"] == "RELAY_1_ON"
    assert cmd_data["duration_s"] == 5
    assert cmd_data["protocol_version"] == 1
    assert cmd_data["message_type"] == "output-test"
    assert cmd_data["action_type"] == "output-test"
    assert cmd_data["published"] is True
    assert len(published_messages) == 1
    topic, payload, qos = published_messages[0]
    assert topic == f"esp-output/local/{node_id.lower()}/command"
    assert payload["protocol_version"] == 1
    assert payload["message_type"] == "output-test"
    assert payload["action"] == "RELAY_1_ON"
    assert payload["duration_s"] == 5
    assert "expires_at_utc" in payload
    assert qos == 1

    alias_cmd = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "RELAY_1_ON", "duration_s": 5, "action_type": "output-test"},
        headers=admin_headers,
    )
    assert alias_cmd.status_code == 200, alias_cmd.text
    assert published_messages[-1][1]["message_type"] == "output-test"
    assert published_messages[-1][1]["protocol_version"] == 1

    off_cmd = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "RELAY_1_OFF"},
        headers=admin_headers,
    )
    assert off_cmd.status_code == 200, off_cmd.text
    assert len(published_messages) == 3
    topic, payload, qos = published_messages[2]
    assert payload["action"] == "RELAY_1_OFF"
    assert "duration_s" not in payload

    viewer_cmd = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "RELAY_1_OFF"},
        headers=viewer_headers,
    )
    assert viewer_cmd.status_code == 403

    assert (
        client.post(
            f"/api/v1/nodes/{node_id}/commands", json={"action": "RELAY_1_OFF"}
        ).status_code
        == 401
    )

    bad_payload = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "RELAY_1_ON"},
        headers=admin_headers,
    )
    assert bad_payload.status_code == 422

    audio_cmd = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "AUDIO_PLAY_HTTPS", "url": "https://example.com/alert.mp3"},
        headers=admin_headers,
    )
    assert audio_cmd.status_code == 200, audio_cmd.text
    assert audio_cmd.json()["action"] == "AUDIO_PLAY_HTTPS"
    assert audio_cmd.json()["url"] == "https://example.com/alert.mp3"
    assert published_messages[-1][1]["action"] == "AUDIO_PLAY_HTTPS"
    assert published_messages[-1][1]["url"] == "https://example.com/alert.mp3"

    bad_audio = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "AUDIO_PLAY_HTTPS", "url": "http://example.com/alert.mp3"},
        headers=admin_headers,
    )
    assert bad_audio.status_code == 422

    client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        json={"lifecycle_state": "deactivated"},
        headers=admin_headers,
    )
    deact_cmd = client.post(
        f"/api/v1/nodes/{node_id}/commands",
        json={"action": "RELAY_1_OFF"},
        headers=admin_headers,
    )
    assert deact_cmd.status_code == 409
    assert "deactivated" in deact_cmd.json()["detail"]


def test_publish_json_success(monkeypatch) -> None:
    mock_client = Mock()
    mock_info = Mock()
    mock_info.rc = 0
    mock_info.is_published.return_value = True
    mock_client.publish.return_value = mock_info

    monkeypatch.setattr(mqtt_listener, "_client", mock_client)
    monkeypatch.setattr(mqtt_listener, "_connected", True)

    mqtt_listener.publish_json(
        "esp-output/local/out-1/command", {"action": "RELAY_1_ON"}
    )
    mock_client.publish.assert_called_once_with(
        "esp-output/local/out-1/command",
        '{"action":"RELAY_1_ON"}',
        qos=1,
        retain=False,
    )
    mock_info.wait_for_publish.assert_called_once_with(timeout=5)


def test_publish_json_timeout(monkeypatch) -> None:
    mock_client = Mock()
    mock_info = Mock()
    mock_info.rc = 0
    mock_info.is_published.return_value = False
    mock_client.publish.return_value = mock_info

    monkeypatch.setattr(mqtt_listener, "_client", mock_client)
    monkeypatch.setattr(mqtt_listener, "_connected", True)

    with pytest.raises(
        mqtt_listener.MqttPublishError, match="timed out publishing MQTT command"
    ):
        mqtt_listener.publish_json(
            "esp-output/local/out-1/command", {"action": "RELAY_1_ON"}
        )
