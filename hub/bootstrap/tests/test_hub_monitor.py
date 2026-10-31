import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
from uuid import uuid4

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.hub_monitor_enabled = False
settings.hub_id = "local-test"
settings.hub_monitor_interval_s = 60
settings.hub_monitor_retention_days = 90

from app import container_logs, edge_status, hub_monitor, policy
from app.db import SessionLocal, init_db
from app.main import create_app
from app.models import HubMonitorEvent, HubMonitorSample
from fastapi.testclient import TestClient
from sqlalchemy import delete, select


def _reset_db() -> None:
    init_db()
    db = SessionLocal()
    try:
        db.execute(
            delete(HubMonitorEvent).where(HubMonitorEvent.hub_id == settings.hub_id)
        )
        db.execute(
            delete(HubMonitorSample).where(HubMonitorSample.hub_id == settings.hub_id)
        )
        db.commit()
    finally:
        db.close()
    hub_monitor._last_docker_signatures.clear()
    hub_monitor._cycles_since_cleanup = 0


def _patch_healthy(monkeypatch) -> None:
    monkeypatch.setattr(
        edge_status,
        "statuses",
        lambda: {"nginx": "online", "web": "online", "firmware-builder": "unavailable"},
    )
    monkeypatch.setattr(
        policy,
        "health_summary",
        lambda: {"status": "healthy", "message": None},
    )
    monkeypatch.setattr(
        container_logs,
        "inspect_monitored_runtimes",
        lambda: {
            "nginx": {
                "docker_state": "running",
                "health_status": "healthy",
                "started_at": "2026-10-01T00:00:00Z",
                "restart_count": 0,
            },
            "web": {
                "docker_state": "running",
                "health_status": "healthy",
                "started_at": "2026-10-01T00:00:00Z",
                "restart_count": 0,
            },
            "hub-api": {
                "docker_state": "running",
                "health_status": "healthy",
                "started_at": "2026-10-01T00:00:00Z",
                "restart_count": 0,
            },
            "opa": {
                "docker_state": "running",
                "health_status": "healthy",
                "started_at": "2026-10-01T00:00:00Z",
                "restart_count": 0,
            },
            "mosquitto": {
                "docker_state": "running",
                "health_status": "healthy",
                "started_at": "2026-10-01T00:00:00Z",
                "restart_count": 0,
            },
        },
    )


def test_assess_states_core_and_auxiliaries() -> None:
    assert hub_monitor.assess_states(
        {
            "api": "ok",
            "database": "ok",
            "nginx": "ok",
            "web": "ok",
            "mqtt": "ok",
            "opa": "ok",
        }
    ) == ("healthy", "available")
    # MQTT down: health degraded (not 100), core availability unavailable.
    assert hub_monitor.assess_states(
        {
            "api": "ok",
            "database": "ok",
            "nginx": "ok",
            "web": "ok",
            "mqtt": "offline",
            "opa": "ok",
        }
    ) == ("degraded", "unavailable")
    assert hub_monitor.health_score("degraded") == 70
    # OPA-only failure remains degraded/degraded (still counts as available).
    assert hub_monitor.assess_states(
        {
            "api": "ok",
            "database": "ok",
            "nginx": "ok",
            "web": "ok",
            "mqtt": "ok",
            "opa": "unavailable",
        }
    ) == ("degraded", "degraded")
    assert hub_monitor.assess_states(
        {
            "api": "ok",
            "database": "ok",
            "nginx": "offline",
            "web": "ok",
            "mqtt": "ok",
            "opa": "ok",
        }
    ) == ("unhealthy", "unavailable")
    # API down while web might still answer elsewhere ⇒ unhealthy.
    assert hub_monitor.assess_states(
        {
            "api": "unhealthy",
            "database": "ok",
            "nginx": "ok",
            "web": "ok",
            "mqtt": "ok",
            "opa": "ok",
        }
    ) == ("unhealthy", "unavailable")


def test_all_services_healthy_records_sample(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        summary = hub_monitor.run_check(db)
        assert summary["health"]["state"] == "healthy"
        assert summary["health"]["score"] == 100
        assert summary["availability"]["current_state"] == "available"
        assert summary["sample_count"] == 1
        assert summary["recent_events"] == []
        events = hub_monitor.list_recent_events(db, limit=15)
        assert any(
            event["event_type"] == "monitoring_resumed" for event in events["events"]
        )
    finally:
        db.close()


def test_mqtt_offline_degrades_while_api_remains_usable(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    settings.mqtt_enabled = True
    monkeypatch.setattr(hub_monitor, "is_connected", lambda: False)
    db = SessionLocal()
    try:
        summary = hub_monitor.run_check(db)
        assert summary["health"]["state"] == "degraded"
        assert summary["health"]["score"] == 70
        assert summary["availability"]["current_state"] == "unavailable"
        assert summary["health"]["reason"]
        assert "MQTT" in summary["health"]["reason"]
        components = {
            item["id"]: item["state"] for item in summary["health"]["components"]
        }
        assert components["mqtt"] == "offline"
        assert components["api"] == "ok"
        assert "firmware-builder" not in components
    finally:
        db.close()
        settings.mqtt_enabled = False


def test_web_failure_marks_unhealthy(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    monkeypatch.setattr(
        edge_status,
        "statuses",
        lambda: {
            "nginx": "online",
            "web": "offline",
            "firmware-builder": "unavailable",
        },
    )
    db = SessionLocal()
    try:
        summary = hub_monitor.run_check(db)
        assert summary["health"]["state"] == "unhealthy"
        assert summary["availability"]["current_state"] == "unavailable"
    finally:
        db.close()


def test_opa_failure_degrades(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    monkeypatch.setattr(
        policy,
        "health_summary",
        lambda: {"status": "unhealthy", "message": "plugin failed"},
    )
    db = SessionLocal()
    try:
        summary = hub_monitor.run_check(db)
        assert summary["health"]["state"] == "degraded"
    finally:
        db.close()


def test_stable_healthy_cycle_does_not_spam_state_changes(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
        before = hub_monitor.list_recent_events(db, limit=50)["count"]
        assert before > 0
        hub_monitor.run_check(db)
        after = hub_monitor.list_recent_events(db, limit=50)["count"]
        assert after == before
    finally:
        db.close()


def test_recovery_emits_state_change_events(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    monkeypatch.setattr(
        edge_status,
        "statuses",
        lambda: {
            "nginx": "online",
            "web": "offline",
            "firmware-builder": "unavailable",
        },
    )
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
        monkeypatch.setattr(
            edge_status,
            "statuses",
            lambda: {
                "nginx": "online",
                "web": "online",
                "firmware-builder": "unavailable",
            },
        )
        summary = hub_monitor.run_check(db)
        event_types = {
            event["event_type"]
            for event in hub_monitor.list_recent_events(db, limit=15)["events"]
        }
        assert "state_changed" in event_types
        assert "health_changed" in event_types
        assert summary["health"]["state"] == "healthy"
    finally:
        db.close()


def test_monitoring_gap_is_unobserved(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
        sample = db.scalar(
            select(HubMonitorSample)
            .where(HubMonitorSample.hub_id == settings.hub_id)
            .order_by(HubMonitorSample.observed_at.desc())
            .limit(1)
        )
        assert sample is not None
        sample.observed_at = datetime.now(timezone.utc) - timedelta(hours=2)
        db.commit()
        hub_monitor.run_check(db, record_startup_gap=True)
        resumed = [
            event
            for event in hub_monitor.list_recent_events(db, limit=15)["events"]
            if event["event_type"] == "monitoring_resumed"
            and event["certainty"] == "inferred"
        ]
        assert resumed
        window = hub_monitor.compute_availability_window(
            list(db.scalars(select(HubMonitorSample))),
            window_start=datetime.now(timezone.utc) - timedelta(hours=3),
            window_end=datetime.now(timezone.utc),
            interval_s=60,
        )
        assert window["unobserved_seconds"] > 0
    finally:
        db.close()


def test_insufficient_history_returns_null_percent() -> None:
    now = datetime.now(timezone.utc)
    window = hub_monitor.compute_availability_window(
        [],
        window_start=now - timedelta(hours=24),
        window_end=now,
        interval_s=60,
    )
    assert window["percent"] is None
    assert window["insufficient_data"] is True
    assert window["reason"] == "insufficient_observation"


def test_availability_excludes_unobserved_from_denominator() -> None:
    now = datetime.now(timezone.utc)
    samples = [
        HubMonitorSample(
            sample_id=str(uuid4()),
            hub_id="local-test",
            observed_at=now - timedelta(minutes=10),
            overall_availability="available",
            overall_health="healthy",
            components_json="{}",
        ),
        HubMonitorSample(
            sample_id=str(uuid4()),
            hub_id="local-test",
            observed_at=now - timedelta(minutes=9),
            overall_availability="available",
            overall_health="healthy",
            components_json="{}",
        ),
    ]
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=now - timedelta(minutes=10),
        window_end=now - timedelta(minutes=8),
        interval_s=60,
    )
    assert window["insufficient_data"] is False or window["eligible_seconds"] >= 60
    assert window["unobserved_seconds"] >= 0
    if window["eligible_seconds"] > 0 and not window["insufficient_data"]:
        assert window["percent"] == 100.0


def test_retention_cleanup_only_touches_monitor_tables(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        old = HubMonitorSample(
            sample_id=str(uuid4()),
            hub_id="local-test",
            observed_at=datetime.now(timezone.utc) - timedelta(days=120),
            overall_availability="available",
            overall_health="healthy",
            components_json="{}",
        )
        old_event = HubMonitorEvent(
            event_id=str(uuid4()),
            hub_id="local-test",
            occurred_at=datetime.now(timezone.utc) - timedelta(days=120),
            observed_at=datetime.now(timezone.utc) - timedelta(days=120),
            component="hub",
            event_type="health_changed",
            previous_state="degraded",
            new_state="healthy",
            reason="old",
            certainty="confirmed",
        )
        db.add_all([old, old_event])
        db.commit()
        # Load a naive SQLite datetime into the identity map, then clean up with an
        # aware cutoff — this is the production failure mode on SQLite.
        assert db.scalar(select(HubMonitorSample).limit(1)) is not None
        result = hub_monitor.cleanup_history(db)
        db.commit()
        assert result["samples_deleted"] == 1
        assert result["events_deleted"] == 1
        assert (
            db.scalar(
                select(HubMonitorSample).where(
                    HubMonitorSample.hub_id == settings.hub_id
                )
            )
            is None
        )
        assert (
            db.scalar(
                select(HubMonitorEvent).where(HubMonitorEvent.hub_id == settings.hub_id)
            )
            is None
        )
    finally:
        db.close()


def test_cleanup_failure_does_not_block_later_samples(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor._cycles_since_cleanup = hub_monitor.CLEANUP_EVERY_CYCLES - 1

        def boom(*_args, **_kwargs):
            raise TypeError("can't compare offset-naive and offset-aware datetimes")

        monkeypatch.setattr(hub_monitor, "cleanup_history", boom)
        summary = hub_monitor.run_check(db)
        assert summary["health"]["state"] == "healthy"
        assert hub_monitor._cycles_since_cleanup == 0
        assert (
            db.scalar(
                select(HubMonitorSample).where(
                    HubMonitorSample.hub_id == settings.hub_id
                )
            )
            is not None
        )
    finally:
        db.close()
        hub_monitor._cycles_since_cleanup = 0


def test_docker_restart_detection_emits_inferred_event(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    hub_monitor._last_docker_signatures.clear()
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
        monkeypatch.setattr(
            container_logs,
            "inspect_monitored_runtimes",
            lambda: {
                "nginx": {
                    "docker_state": "running",
                    "health_status": "healthy",
                    "started_at": "2026-10-27T12:00:00Z",
                    "restart_count": 2,
                },
                "web": {
                    "docker_state": "running",
                    "health_status": "healthy",
                    "started_at": "2026-10-01T00:00:00Z",
                    "restart_count": 0,
                },
                "hub-api": {
                    "docker_state": "running",
                    "health_status": "healthy",
                    "started_at": "2026-10-01T00:00:00Z",
                    "restart_count": 0,
                },
                "opa": {
                    "docker_state": "running",
                    "health_status": "healthy",
                    "started_at": "2026-10-01T00:00:00Z",
                    "restart_count": 0,
                },
                "mosquitto": {
                    "docker_state": "running",
                    "health_status": "healthy",
                    "started_at": "2026-10-01T00:00:00Z",
                    "restart_count": 0,
                },
            },
        )
        hub_monitor.run_check(db)
        restarts = [
            event
            for event in hub_monitor.list_recent_events(db, limit=15)["events"]
            if event["event_type"] == "service_restart_detected"
        ]
        assert restarts
        assert restarts[0]["certainty"] == "inferred"
        assert restarts[0]["component"] == "nginx"
    finally:
        db.close()
        hub_monitor._last_docker_signatures.clear()


def test_hub_status_includes_monitoring_payload(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
    finally:
        db.close()
    client = TestClient(create_app())
    response = client.get(
        "/api/v1/hub/status",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert "monitoring" in body
    assert body["monitoring"]["health"]["state"] in {
        "healthy",
        "degraded",
        "unhealthy",
        "unknown",
    }
    assert "24h" in body["monitoring"]["availability"]["windows"]
    assert "7d" in body["monitoring"]["availability"]["windows"]
    assert "30d" in body["monitoring"]["availability"]["windows"]
    assert body["monitoring"]["recent_events"] == []
    events = client.get(
        "/api/v1/hub/monitoring/events?limit=15",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert events.status_code == 200, events.text
    assert isinstance(events.json()["events"], list)


def test_monitor_failure_does_not_break_status(monkeypatch) -> None:
    _reset_db()
    monkeypatch.setattr(
        hub_monitor,
        "build_monitoring_summary",
        lambda db, **kwargs: (_ for _ in ()).throw(RuntimeError("boom")),
    )
    client = TestClient(create_app())
    response = client.get(
        "/api/v1/hub/status",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert response.status_code == 200, response.text
    assert response.json()["monitoring"]["health"]["state"] == "unknown"


def test_dense_history_produces_availability_percent(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        now = datetime.now(timezone.utc)
        for offset in range(20, 0, -1):
            db.add(
                HubMonitorSample(
                    sample_id=str(uuid4()),
                    hub_id="local-test",
                    observed_at=now - timedelta(minutes=offset),
                    overall_availability="available",
                    overall_health="healthy",
                    components_json=json.dumps({"states": {"api": "ok"}}),
                )
            )
        db.commit()
        summary = hub_monitor.build_monitoring_summary(db)
        window_24h = summary["availability"]["windows"]["24h"]
        window_7d = summary["availability"]["windows"]["7d"]
        assert window_24h["insufficient_data"] is False
        assert window_24h["percent"] == 100.0
        assert window_24h["low_coverage"] is True
        assert window_24h["score_ready"] is True
        assert window_7d["percent"] == 100.0
        assert window_7d["low_coverage"] is True
        assert window_7d["score_ready"] is False
    finally:
        db.close()


def _sample(
    *,
    observed_at: datetime,
    availability: str,
    health: str = "healthy",
    states: dict[str, str] | None = None,
) -> HubMonitorSample:
    return HubMonitorSample(
        sample_id=str(uuid4()),
        hub_id=settings.hub_id,
        observed_at=observed_at,
        overall_availability=availability,
        overall_health=health,
        components_json=json.dumps(
            {"states": states or {"api": "ok", "mqtt": "ok", "web": "ok"}},
            separators=(",", ":"),
        ),
    )


def test_mqtt_outage_reduces_core_and_mqtt_availability() -> None:
    """Confirmed MQTT downtime must appear in historical metrics (not round to 100%)."""
    now = datetime.now(timezone.utc)
    samples: list[HubMonitorSample] = []
    # Dense 20-minute window; minutes 8–9 are MQTT offline (120s unavailable).
    for minute in range(20, 0, -1):
        mqtt_offline = 8 <= minute <= 9
        samples.append(
            _sample(
                observed_at=now - timedelta(minutes=minute),
                availability="unavailable" if mqtt_offline else "available",
                health="degraded" if mqtt_offline else "healthy",
                states={
                    "api": "ok",
                    "mqtt": "offline" if mqtt_offline else "ok",
                    "web": "ok",
                    "nginx": "ok",
                    "database": "ok",
                    "opa": "ok",
                },
            )
        )
    window_start = now - timedelta(minutes=20)
    core = hub_monitor.compute_availability_window(
        samples,
        window_start=window_start,
        window_end=now,
        interval_s=60,
    )
    assert core["unavailable_seconds"] >= 60
    assert core["percent"] is not None
    assert core["percent"] < 100.0
    mqtt = hub_monitor.compute_component_availability_window(
        samples,
        component_id="mqtt",
        window_start=window_start,
        window_end=now,
        interval_s=60,
    )
    assert mqtt["unavailable_seconds"] >= 60
    assert mqtt["percent"] is not None
    assert mqtt["percent"] < 100.0
    api = hub_monitor.compute_component_availability_window(
        samples,
        component_id="api",
        window_start=window_start,
        window_end=now,
        interval_s=60,
    )
    assert api["unavailable_seconds"] == 0
    assert api["percent"] == 100.0


def test_short_outage_does_not_round_to_100() -> None:
    now = datetime.now(timezone.utc)
    # Long eligible window with a short outage: without the 100% guard,
    # 120s / ~15h would round to 100.00.
    samples: list[HubMonitorSample] = []
    for minute in range(0, 15 * 60, 5):
        observed = now - timedelta(hours=15) + timedelta(minutes=minute)
        outage = (
            timedelta(hours=14)
            <= (observed - (now - timedelta(hours=15)))
            < timedelta(hours=14, minutes=2)
        )
        samples.append(
            _sample(
                observed_at=observed,
                availability="unavailable" if outage else "available",
                health="degraded" if outage else "healthy",
                states={
                    "api": "ok",
                    "mqtt": "offline" if outage else "ok",
                    "web": "ok",
                },
            )
        )
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=now - timedelta(hours=15),
        window_end=now,
        interval_s=60,
    )
    assert window["unavailable_seconds"] >= 60
    assert window["percent"] is not None
    assert window["percent"] < 100.0
    # Guard specifically: must not present bare 100 after rounding.
    assert window["percent"] != 100.0


def test_unknown_component_intervals_are_not_counted_available() -> None:
    now = datetime.now(timezone.utc)
    samples = [
        _sample(
            observed_at=now - timedelta(minutes=10),
            availability="available",
            states={"api": "ok", "mqtt": "unknown", "web": "ok"},
        ),
        _sample(
            observed_at=now - timedelta(minutes=8),
            availability="available",
            states={"api": "ok", "mqtt": "ok", "web": "ok"},
        ),
    ]
    mqtt = hub_monitor.compute_component_availability_window(
        samples,
        component_id="mqtt",
        window_start=now - timedelta(minutes=10),
        window_end=now - timedelta(minutes=8),
        interval_s=60,
    )
    # Unknown mqtt state is unobserved, never treated as available uptime.
    assert mqtt["available_seconds"] == 0
    assert mqtt["unavailable_seconds"] == 0
    assert mqtt["unobserved_seconds"] >= 120
    assert mqtt["percent"] is None


def test_mqtt_recovery_restores_current_state_keeps_history(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    settings.mqtt_enabled = True
    db = SessionLocal()
    try:
        monkeypatch.setattr(hub_monitor, "is_connected", lambda: False)
        down = hub_monitor.run_check(db)
        assert down["health"]["score"] == 70
        assert down["availability"]["current_state"] == "unavailable"
        monkeypatch.setattr(hub_monitor, "is_connected", lambda: True)
        up = hub_monitor.run_check(db)
        assert up["health"]["state"] == "healthy"
        assert up["health"]["score"] == 100
        assert up["availability"]["current_state"] == "available"
        events = hub_monitor.list_recent_events(db, limit=20)["events"]
        mqtt_events = [
            event
            for event in events
            if event["component"] == "mqtt" and event["event_type"] == "state_changed"
        ]
        assert any(event["new_state"] == "offline" for event in mqtt_events)
        assert any(event["new_state"] == "ok" for event in mqtt_events)
    finally:
        db.close()
        settings.mqtt_enabled = False


def test_firmware_builder_restart_does_not_create_monitor_events(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
        before = hub_monitor.list_recent_events(db, limit=50)["count"]
        hub_monitor._detect_docker_restarts(
            db,
            {
                "firmware-builder": {
                    "started_at": "2026-10-28T00:00:00Z",
                    "restart_count": 3,
                },
                "nginx": {
                    "started_at": "2026-10-01T00:00:00Z",
                    "restart_count": 0,
                },
            },
            datetime.now(timezone.utc),
        )
        # Seed previous signatures then change firmware-builder only.
        hub_monitor._last_docker_signatures["firmware-builder"] = (
            "2026-10-28T00:00:00Z",
            3,
        )
        hub_monitor._detect_docker_restarts(
            db,
            {
                "firmware-builder": {
                    "started_at": "2026-10-28T01:00:00Z",
                    "restart_count": 4,
                }
            },
            datetime.now(timezone.utc),
        )
        db.commit()
        after = hub_monitor.list_recent_events(db, limit=50)
        assert after["count"] == before
        assert all(
            event["component"] != "firmware-builder" for event in after["events"]
        )
        summary = hub_monitor.build_monitoring_summary(db)
        assert all(
            component["id"] != "firmware-builder"
            for component in summary["health"]["components"]
        )
    finally:
        db.close()


def test_service_restart_event_does_not_use_restart_count_as_state(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor._last_docker_signatures["mosquitto"] = (
            "2026-10-28T00:00:00Z",
            0,
        )
        hub_monitor._detect_docker_restarts(
            db,
            {
                "mosquitto": {
                    "started_at": "2026-10-28T01:00:00Z",
                    "restart_count": 1,
                }
            },
            datetime.now(timezone.utc),
        )
        db.commit()
        events = hub_monitor.list_recent_events(db, limit=10)["events"]
        restart = next(
            event
            for event in events
            if event["event_type"] == "service_restart_detected"
        )
        assert restart["previous_state"] == "running"
        assert restart["new_state"] == "restart_detected"
        assert "restart_count" in (restart["reason"] or "")
    finally:
        db.close()


def test_overlapping_samples_do_not_double_count_downtime() -> None:
    now = datetime.now(timezone.utc)
    # Adjacent unavailable samples covering the same wall-clock span once.
    samples = [
        _sample(
            observed_at=now - timedelta(minutes=4),
            availability="unavailable",
            health="degraded",
            states={"api": "ok", "mqtt": "offline", "web": "ok"},
        ),
        _sample(
            observed_at=now - timedelta(minutes=3),
            availability="unavailable",
            health="degraded",
            states={"api": "ok", "mqtt": "offline", "web": "ok"},
        ),
        _sample(
            observed_at=now - timedelta(minutes=2),
            availability="available",
            states={"api": "ok", "mqtt": "ok", "web": "ok"},
        ),
    ]
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=now - timedelta(minutes=4),
        window_end=now - timedelta(minutes=2),
        interval_s=60,
    )
    # Two minutes of wall clock from first→third sample; not doubled.
    assert window["unavailable_seconds"] == 120
    assert window["available_seconds"] == 0


def test_api_unhealthy_while_web_ok_is_unhealthy() -> None:
    assert hub_monitor.assess_states(
        {
            "api": "unhealthy",
            "database": "ok",
            "nginx": "ok",
            "web": "ok",
            "mqtt": "ok",
            "opa": "ok",
        }
    ) == ("unhealthy", "unavailable")
    reason = hub_monitor.health_failure_reason(
        {
            "api": "unhealthy",
            "database": "ok",
            "nginx": "ok",
            "web": "ok",
            "mqtt": "ok",
            "opa": "ok",
        }
    )
    assert reason is not None
    assert "api" in reason


def test_summary_exposes_independent_component_windows(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        now = datetime.now(timezone.utc)
        for offset in range(30, 0, -1):
            mqtt_state = "offline" if 10 <= offset <= 12 else "ok"
            availability = "unavailable" if mqtt_state == "offline" else "available"
            health = "degraded" if mqtt_state == "offline" else "healthy"
            db.add(
                _sample(
                    observed_at=now - timedelta(minutes=offset),
                    availability=availability,
                    health=health,
                    states={
                        "api": "ok",
                        "mqtt": mqtt_state,
                        "web": "ok",
                        "nginx": "ok",
                        "database": "ok",
                        "opa": "ok",
                    },
                )
            )
        db.commit()
        summary = hub_monitor.build_monitoring_summary(db)
        assert summary["availability"]["label"] == "Core Hub availability"
        mqtt_24h = summary["availability"]["components"]["mqtt"]["windows"]["24h"]
        api_24h = summary["availability"]["components"]["api"]["windows"]["24h"]
        core_24h = summary["availability"]["windows"]["24h"]
        assert mqtt_24h["unavailable_seconds"] > 0
        assert api_24h["unavailable_seconds"] == 0
        assert core_24h["unavailable_seconds"] > 0
        assert core_24h["percent"] < 100.0
    finally:
        db.close()
