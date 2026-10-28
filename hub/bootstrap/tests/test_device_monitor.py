from datetime import datetime, timedelta, timezone
from pathlib import Path
from uuid import uuid4

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.hub_monitor_enabled = False
settings.device_monitor_enabled = True
settings.hub_id = "local-test"
settings.hub_monitor_interval_s = 60
settings.device_online_window_s = 90
settings.device_monitor_retention_days = 90
settings.device_monitor_max_samples_per_device = 500
settings.device_monitor_max_events_per_device = 500

from app import device_monitor, hub_monitor, service
from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import (
    Base,
    DeviceMonitorEvent,
    DeviceMonitorSample,
    Node,
    ReportedState,
)
from fastapi.testclient import TestClient
from sqlalchemy import func, select


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _reset_db() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()
    hub_monitor._cycles_since_cleanup = 0


def _add_active_node(
    db, node_id: str, *, last_seen: datetime | None, lifecycle: str = "active"
) -> None:
    db.add(
        Node(
            node_id=node_id,
            node_type="output",
            hardware_revision="test",
            provisioning_state="provisioned",
            lifecycle_state=lifecycle,
        )
    )
    db.add(
        ReportedState(
            node_id=node_id,
            firmware_version="1.0.0",
            last_seen_at=last_seen,
        )
    )
    db.commit()


def test_sample_records_overdue_and_resume() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        now = _utcnow()
        _add_active_node(db, "OUTAAAAA", last_seen=now)
        result = device_monitor.sample_devices(db, observed_at=now)
        db.commit()
        assert result["sampled"] == 1

        reported = db.get(ReportedState, "OUTAAAAA")
        assert reported is not None
        reported.last_seen_at = now - timedelta(seconds=settings.device_online_window_s + 5)
        db.commit()

        later = now + timedelta(seconds=120)
        result = device_monitor.sample_devices(db, observed_at=later)
        db.commit()
        assert result["transitions"] >= 1

        events = [
            event.event_type
            for event in db.scalars(
                select(DeviceMonitorEvent).where(
                    DeviceMonitorEvent.node_id == "OUTAAAAA"
                )
            )
        ]
        assert "monitoring_started" in events
        assert "heartbeat_overdue" in events

        reported.last_seen_at = later
        db.commit()
        result = device_monitor.sample_devices(
            db, observed_at=later + timedelta(seconds=30)
        )
        db.commit()
        events = [
            event.event_type
            for event in db.scalars(
                select(DeviceMonitorEvent).where(
                    DeviceMonitorEvent.node_id == "OUTAAAAA"
                )
            )
        ]
        assert "heartbeat_resumed" in events

        samples = list(
            db.scalars(
                select(DeviceMonitorSample)
                .where(DeviceMonitorSample.node_id == "OUTAAAAA")
                .order_by(DeviceMonitorSample.observed_at.asc())
            )
        )
        assert [sample.overall_availability for sample in samples] == [
            "available",
            "unavailable",
            "available",
        ]
    finally:
        db.close()


def test_deactivated_devices_are_not_sampled() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        now = _utcnow()
        _add_active_node(db, "OUTBBBBB", last_seen=now, lifecycle="deactivated")
        result = device_monitor.sample_devices(db, observed_at=now)
        db.commit()
        assert result["sampled"] == 0
        assert db.scalar(select(func.count()).select_from(DeviceMonitorSample)) == 0

        summary = device_monitor.build_monitoring_summary(db, "OUTBBBBB")
        assert summary["eligibility"] == "excluded"
        assert summary["availability"]["current_state"] == "excluded"
        assert summary["health"]["state"] == "unknown"
    finally:
        db.close()


def test_never_seen_devices_are_skipped() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_active_node(db, "OUTCCCCC", last_seen=None)
        result = device_monitor.sample_devices(db, observed_at=_utcnow())
        db.commit()
        assert result["sampled"] == 0
    finally:
        db.close()


def test_hub_gap_marks_unobserved_and_excludes_from_percent() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        t0 = _utcnow() - timedelta(hours=2)
        _add_active_node(db, "OUTDDDDD", last_seen=t0)
        device_monitor.sample_devices(db, observed_at=t0)
        db.commit()

        gap_start = t0 + timedelta(minutes=5)
        gap_end = t0 + timedelta(hours=1)
        device_monitor.sample_devices(
            db, observed_at=gap_end, hub_gap=(gap_start, gap_end)
        )
        db.commit()

        samples = list(
            db.scalars(
                select(DeviceMonitorSample)
                .where(DeviceMonitorSample.node_id == "OUTDDDDD")
                .order_by(DeviceMonitorSample.observed_at.asc())
            )
        )
        assert any(sample.overall_availability == "unobserved" for sample in samples)
        assert samples[-1].overall_availability == "unavailable"

        window = device_monitor.compute_availability_window(
            samples,
            window_start=t0,
            window_end=gap_end + timedelta(minutes=1),
        )
        assert window["unobserved_seconds"] > 0
        assert window["available_seconds"] + window["unavailable_seconds"] == window[
            "eligible_seconds"
        ]
    finally:
        db.close()


def test_multi_device_prune_is_isolated(monkeypatch) -> None:
    _reset_db()
    monkeypatch.setattr(settings, "device_monitor_max_samples_per_device", 3)
    monkeypatch.setattr(settings, "device_monitor_max_events_per_device", 3)
    db = SessionLocal()
    try:
        now = _utcnow()
        _add_active_node(db, "OUTAAAAA", last_seen=now)
        _add_active_node(db, "OUTBBBBB", last_seen=now)

        for index in range(6):
            stamp = now + timedelta(minutes=index)
            for node_id, online in (("OUTAAAAA", index % 2 == 0), ("OUTBBBBB", True)):
                reported = db.get(ReportedState, node_id)
                assert reported is not None
                reported.last_seen_at = (
                    stamp if online else stamp - timedelta(seconds=200)
                )
            db.commit()
            device_monitor.sample_devices(db, observed_at=stamp)
            db.commit()

        count_a = db.scalar(
            select(func.count())
            .select_from(DeviceMonitorSample)
            .where(DeviceMonitorSample.node_id == "OUTAAAAA")
        )
        count_b = db.scalar(
            select(func.count())
            .select_from(DeviceMonitorSample)
            .where(DeviceMonitorSample.node_id == "OUTBBBBB")
        )
        assert count_a == 3
        assert count_b <= 3
        assert count_b >= 1
    finally:
        db.close()


def test_score_ready_false_for_short_multi_day_coverage() -> None:
    samples = [
        DeviceMonitorSample(
            sample_id=str(uuid4()),
            hub_id=settings.hub_id,
            node_id="OUTAAAAA",
            observed_at=_utcnow() - timedelta(hours=2),
            overall_availability="available",
            overall_health="unknown",
        )
    ]
    window = device_monitor.compute_availability_window(
        samples,
        window_start=_utcnow() - timedelta(days=7),
        window_end=_utcnow(),
    )
    assert window["percent"] is not None
    assert window["score_ready"] is False
    assert window["low_coverage"] is True


def test_node_detail_includes_monitoring_and_events_endpoint() -> None:
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
        service.apply_reported(db, node_id, {})
        device_monitor.sample_devices(db, observed_at=_utcnow())
        db.commit()
    finally:
        db.close()

    detail = client.get(
        f"/api/v1/nodes/{node_id}",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    assert detail.status_code == 200
    body = detail.json()
    assert body["monitoring"] is not None
    assert body["monitoring"]["phase"] == "D2"
    assert body["monitoring"]["health"]["state"] in {
        "unknown",
        "healthy",
        "degraded",
        "unhealthy",
    }
    assert "24h" in body["monitoring"]["availability"]["windows"]
    assert isinstance(body["availability"], str)

    events = client.get(
        f"/api/v1/nodes/{node_id}/monitoring/events?limit=15",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    assert events.status_code == 200
    payload = events.json()
    assert payload["node_id"] == node_id
    assert payload["count"] >= 1


def test_online_window_setting_drives_status(monkeypatch) -> None:
    _reset_db()
    monkeypatch.setattr(settings, "device_online_window_s", 30.0)
    client = TestClient(create_app())
    created = client.post(
        "/api/v1/nodes",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={"node_type": "output", "hardware_revision": "rev-a"},
    )
    node_id = created.json()["node_id"]
    db = SessionLocal()
    try:
        service.apply_reported(db, node_id, {})
        reported = db.get(ReportedState, node_id)
        assert reported is not None
        reported.last_seen_at = _utcnow() - timedelta(seconds=31)
        db.commit()
    finally:
        db.close()

    devices = client.get(
        "/api/v1/devices",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    device = next(item for item in devices.json() if item["node_id"] == node_id)
    assert device["status"] == "offline"
