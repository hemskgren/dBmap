import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.hub_monitor_enabled = False
settings.device_monitor_enabled = True
settings.device_diag_enabled = True
settings.hub_id = "local-test"
settings.device_diag_max_events_per_device = 30
settings.device_diag_rssi_degraded_dbm = -80
settings.device_diag_aggregate_window_s = 3600

from app import device_diag, service
from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import Base, DeviceDiagEvent, DeviceDiagState, Node, ReportedState
from fastapi.testclient import TestClient
from sqlalchemy import func, select


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _reset_db() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def _add_node(db, node_id: str = "OUTAAAAA") -> None:
    db.add(
        Node(
            node_id=node_id,
            node_type="output",
            hardware_revision="test",
            provisioning_state="active",
            lifecycle_state="active",
        )
    )
    db.add(ReportedState(node_id=node_id, firmware_version="1.0.0"))
    db.commit()


def test_assess_health_rules() -> None:
    assert (
        device_diag.assess_health(
            online=False,
            has_diag_evidence=True,
            diag={"wifi_disconnects": 1},
            wifi_rssi_dbm=-40,
        )[0]
        == "unknown"
    )
    assert (
        device_diag.assess_health(
            online=True, has_diag_evidence=False, diag=None, wifi_rssi_dbm=-40
        )[0]
        == "unknown"
    )
    assert (
        device_diag.assess_health(
            online=True,
            has_diag_evidence=True,
            diag={"last_reset_reason": "PANIC"},
            wifi_rssi_dbm=-40,
        )[0]
        == "unhealthy"
    )
    assert (
        device_diag.assess_health(
            online=True,
            has_diag_evidence=True,
            diag={"wifi_disconnects": 2},
            wifi_rssi_dbm=-40,
        )[0]
        == "degraded"
    )
    assert (
        device_diag.assess_health(
            online=True, has_diag_evidence=True, diag={}, wifi_rssi_dbm=-90
        )[0]
        == "degraded"
    )
    assert (
        device_diag.assess_health(
            online=True, has_diag_evidence=True, diag={}, wifi_rssi_dbm=-40
        )[0]
        == "healthy"
    )


def test_hello_without_diag_does_not_reinflame_counters() -> None:
    """Regression: hello omits diag; next health must not re-add full totals."""
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        health = {
            "node_id": "OUTAAAAA",
            "diag": {"mqtt_reconnects": 12, "mqtt_disconnects": 12},
        }
        device_diag.ingest_reported_diag(
            db,
            node_id="OUTAAAAA",
            previous_payload_json=None,
            new_payload=health,
            online=True,
        )
        db.commit()
        hello = {"node_id": "OUTAAAAA", "uptime_s": 30}
        # Simulate apply_reported storing hello (no diag) as latest payload.
        device_diag.ingest_reported_diag(
            db,
            node_id="OUTAAAAA",
            previous_payload_json=json.dumps(health),
            new_payload=hello,
            online=True,
        )
        db.commit()
        device_diag.ingest_reported_diag(
            db,
            node_id="OUTAAAAA",
            previous_payload_json=json.dumps(hello),
            new_payload=health,
            online=True,
        )
        db.commit()
        events = list(
            db.scalars(
                select(DeviceDiagEvent).where(DeviceDiagEvent.node_id == "OUTAAAAA")
            )
        )
        reconnect = next(
            event for event in events if event.event_type == "mqtt_reconnect_burst"
        )
        # Only the first +12 should count; hello must not cause another +12.
        assert int(reconnect.event_count) == 12
    finally:
        db.close()


def test_ingest_deltas_and_fifo_cap(monkeypatch) -> None:
    _reset_db()
    monkeypatch.setattr(settings, "device_diag_max_events_per_device", 5)
    monkeypatch.setattr(settings, "device_diag_aggregate_window_s", 1.0)
    db = SessionLocal()
    try:
        _add_node(db)
        previous = None
        for index in range(8):
            payload = {
                "node_id": "OUTAAAAA",
                "diag": {
                    "wifi_disconnects": index + 1,
                    "mqtt_reconnects": 0,
                },
            }
            device_diag.ingest_reported_diag(
                db,
                node_id="OUTAAAAA",
                previous_payload_json=previous,
                new_payload=payload,
                online=True,
            )
            previous = json.dumps(payload)
            # Force new rows instead of aggregation by aging last_at artificially.
            latest = db.scalar(
                select(DeviceDiagEvent)
                .where(DeviceDiagEvent.node_id == "OUTAAAAA")
                .order_by(DeviceDiagEvent.occurred_at.desc())
                .limit(1)
            )
            if latest is not None:
                aged = _utcnow() - timedelta(hours=2)
                latest.last_at = aged
                latest.occurred_at = aged
            db.commit()

        count = db.scalar(
            select(func.count())
            .select_from(DeviceDiagEvent)
            .where(DeviceDiagEvent.node_id == "OUTAAAAA")
        )
        assert count == 5
        state = db.get(DeviceDiagState, "OUTAAAAA")
        assert state is not None
        assert state.has_diag_evidence == 1
        assert state.last_health in {"degraded", "healthy", "unhealthy", "unknown"}
    finally:
        db.close()


def test_noisy_device_does_not_starve_other() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db, "OUTAAAAA")
        _add_node(db, "OUTBBBBB")
        prev_a = None
        for index in range(40):
            payload = {"diag": {"wifi_disconnects": index + 1}}
            device_diag.ingest_reported_diag(
                db,
                node_id="OUTAAAAA",
                previous_payload_json=prev_a,
                new_payload=payload,
                online=True,
            )
            prev_a = json.dumps(payload)
            latest = db.scalar(
                select(DeviceDiagEvent)
                .where(DeviceDiagEvent.node_id == "OUTAAAAA")
                .order_by(DeviceDiagEvent.occurred_at.desc())
                .limit(1)
            )
            if latest is not None:
                aged = _utcnow() - timedelta(hours=2, minutes=index)
                latest.last_at = aged
                latest.occurred_at = aged
            db.commit()

        device_diag.ingest_reported_diag(
            db,
            node_id="OUTBBBBB",
            previous_payload_json=None,
            new_payload={"diag": {"mqtt_reconnects": 1}},
            online=True,
        )
        db.commit()

        count_a = db.scalar(
            select(func.count())
            .select_from(DeviceDiagEvent)
            .where(DeviceDiagEvent.node_id == "OUTAAAAA")
        )
        count_b = db.scalar(
            select(func.count())
            .select_from(DeviceDiagEvent)
            .where(DeviceDiagEvent.node_id == "OUTBBBBB")
        )
        assert count_a == settings.device_diag_max_events_per_device
        assert count_b == 1
    finally:
        db.close()


def test_malformed_diag_ignored() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        result = device_diag.ingest_reported_diag(
            db,
            node_id="OUTAAAAA",
            previous_payload_json=None,
            new_payload={"diag": "not-an-object", "wifi_rssi_dbm": -50},
            online=True,
        )
        db.commit()
        assert result["health"] == "unknown"
        assert db.scalar(select(func.count()).select_from(DeviceDiagEvent)) == 0
    finally:
        db.close()


def test_apply_reported_ingests_diag_and_node_detail() -> None:
    _reset_db()
    client = TestClient(create_app())
    created = client.post(
        "/api/v1/nodes",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={"node_type": "output", "hardware_revision": "rev-a"},
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]

    db = SessionLocal()
    try:
        service.apply_reported(
            db,
            node_id,
            {
                "firmware_version": "1.0.0",
                "wifi_rssi_dbm": -55,
                "diag": {"last_reset_reason": "POWERON"},
            },
        )
        service.apply_reported(
            db,
            node_id,
            {
                "firmware_version": "1.0.0",
                "wifi_rssi_dbm": -55,
                "diag": {"wifi_disconnects": 2, "mqtt_reconnects": 1},
            },
        )
    finally:
        db.close()

    detail = client.get(
        f"/api/v1/nodes/{node_id}",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    assert detail.status_code == 200
    monitoring = detail.json()["monitoring"]
    assert monitoring["phase"] == "D2"
    assert monitoring["health"]["state"] == "degraded"
    assert monitoring["health"]["score"] == 70
    assert monitoring["health"]["has_diag_evidence"] is True

    events = client.get(
        f"/api/v1/nodes/{node_id}/monitoring/events?limit=15",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    assert events.status_code == 200
    sources = {item.get("source") for item in events.json()["events"]}
    assert "device_reported" in sources or "hub_assessed" in sources
