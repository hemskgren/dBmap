import json
from datetime import UTC, datetime, timedelta
from uuid import uuid4

from app import core_monitor
from app.config import settings
from app.db import SessionLocal, init_db
from app.models import (
    OperationsMonitorCheckpoint,
    OperationsMonitorEvent,
    OperationsMonitorSample,
)
from app.monitoring_availability import (
    Observation,
    calculate_window,
)


def test_assess_states_distinguishes_reachability_and_mqtt() -> None:
    unavailable = {
        "api": "ok",
        "database": "ok",
        "nginx": "ok",
        "web": "ok",
        "mqtt": "offline",
        "opa": "ok",
    }
    assert core_monitor.assess_states(unavailable) == (
        "degraded",
        "unavailable",
    )

    reachability_down = dict(unavailable, mqtt="ok", nginx="offline")
    assert core_monitor.assess_states(reachability_down) == (
        "unhealthy",
        "unavailable",
    )


def test_availability_excludes_unobserved_time() -> None:
    now = datetime(2026, 11, 26, 20, 0, tzinfo=UTC)
    observations = [
        Observation(now - timedelta(hours=2), "unobserved"),
        Observation(now - timedelta(minutes=30), "available"),
    ]
    window = calculate_window(
        observations,
        window_start=now - timedelta(hours=1),
        window_end=now,
        interval_s=60,
        basis="core",
        core=True,
    )
    assert window["unobserved_seconds"] >= 1800
    assert window["available_seconds"] >= 1800


def test_core_probe_uses_independent_probe_results(monkeypatch) -> None:
    monkeypatch.setattr(settings, "admin_token", "admin-token")
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_api",
        lambda: ("offline", "connection refused"),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_database",
        lambda: ("unknown", "Hub API unavailable"),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_nginx",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_nginx_local",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_web",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_mqtt_broker",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_opa",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "fetch_hub_monitoring_events",
        lambda: ([], None),
    )

    probe = core_monitor._probe_components()
    states = probe["components"]
    assert states["api"] == "offline"
    assert states["nginx"] == "ok"
    assert states["nginx_local"] == "ok"
    assert states["web"] == "ok"
    assert states["mqtt"] == "ok"


def test_local_nginx_state_does_not_change_core_availability() -> None:
    states = {
        "api": "ok",
        "database": "ok",
        "nginx": "ok",
        "nginx_local": "offline",
        "web": "ok",
        "mqtt": "ok",
        "opa": "ok",
    }
    assert core_monitor.assess_states(states) == ("healthy", "available")


def test_hub_restart_events_are_mirrored_once_with_original_identity(monkeypatch) -> None:
    init_db()
    event_id = str(uuid4())
    occurred_at = "2026-12-01T10:00:00+00:00"
    observed_at = "2026-12-01T10:00:05+00:00"
    monkeypatch.setattr(
        core_monitor.probes,
        "fetch_hub_monitoring_events",
        lambda: (
            [
                {
                    "event_id": event_id,
                    "occurred_at": occurred_at,
                    "observed_at": observed_at,
                    "component": "hub-api",
                    "event_type": "service_restart_detected",
                    "previous_state": "running",
                    "new_state": "restart_detected",
                    "reason": "container restart count changed",
                    "source": "hub_monitor",
                    "certainty": "inferred",
                }
            ],
            None,
        ),
    )
    with SessionLocal() as db:
        assert core_monitor.sync_restart_events(db) == 1
        db.commit()
        assert core_monitor.sync_restart_events(db) == 0
        event = db.get(OperationsMonitorEvent, event_id)
        assert event is not None
        assert event.occurred_at.replace(tzinfo=UTC).isoformat() == occurred_at
        assert event.new_state == "restart_detected"
        assert event.source == "hub_monitor"
        assert event.certainty == "inferred"
        db.delete(event)
        db.commit()


def test_public_check_persists_sample_and_restart_state(monkeypatch) -> None:
    from app import public_monitor

    monkeypatch.setattr(settings, "public_monitor_interval_s", 60)
    monkeypatch.setattr(settings, "retention_days", 30)
    init_db()
    now = datetime.now(UTC)
    with SessionLocal() as db:
        public_monitor.record_check(
            db,
            "public_api",
            now,
            ("ok", None),
            None,
        )
        store = public_monitor._store("public_api")
        sample = store.latest_sample(db)
        checkpoint = db.get(
            __import__(
                "app.models", fromlist=["PublicMonitorCheckpoint"]
            ).PublicMonitorCheckpoint,
            {"hub_id": settings.hub_id, "component": "public_api"},
        )
        assert sample is not None
        assert sample.overall_availability == "available"
        assert checkpoint is not None


def test_retention_prunes_core_history(monkeypatch) -> None:
    monkeypatch.setattr(settings, "retention_days", 30)
    init_db()
    now = datetime.now(UTC)
    with SessionLocal() as db:
        db.add(
            OperationsMonitorSample(
                hub_id=settings.hub_id,
                observed_at=now - timedelta(days=31),
                overall_availability="available",
                overall_health="healthy",
                components_json='{"states":{"api":"ok"}}',
                source="operations_monitor",
            )
        )
        db.commit()
        result = core_monitor.cleanup_history(db, now)
        db.commit()
        assert result["samples_deleted"] >= 1


def test_core_worker_loop_executes_an_actual_check(monkeypatch) -> None:
    import app.core_monitor as core_monitor_module

    monkeypatch.setattr(settings, "core_monitor_enabled", True)
    monkeypatch.setattr(settings, "core_monitor_interval_s", 60)
    init_db()
    calls = {"count": 0}

    def run_one(db):
        calls["count"] += 1
        core_monitor_module._stop.set()

    monkeypatch.setattr(core_monitor_module, "run_check", run_one)
    core_monitor_module._stop.clear()
    with SessionLocal() as db:
        monkeypatch.setattr(
            core_monitor_module,
            "SessionLocal",
            lambda: db,
        )
        core_monitor_module._loop()
    assert calls["count"] == 1


def test_public_worker_loop_executes_an_actual_poll(monkeypatch) -> None:
    from app import public_monitor

    monkeypatch.setattr(settings, "public_monitor_interval_s", 60)
    monkeypatch.setattr(settings, "retention_days", 30)
    init_db()
    public_monitor._stop.clear()

    def probe():
        return "ok", None

    public_monitor._stop.wait = lambda _timeout: public_monitor._stop.set()

    public_monitor._loop("public_api", probe)


def test_core_check_persists_sample_event_and_checkpoint(monkeypatch) -> None:
    monkeypatch.setattr(settings, "core_monitor_enabled", True)
    monkeypatch.setattr(settings, "core_monitor_interval_s", 60)
    monkeypatch.setattr(settings, "gap_threshold_s", 120)
    monkeypatch.setattr(settings, "retention_days", 30)
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_api",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_database",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_nginx",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_nginx_local",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_web",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_mqtt_broker",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "probe_opa",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        core_monitor.probes,
        "fetch_hub_monitoring_events",
        lambda: ([], None),
    )
    init_db()
    with SessionLocal() as db:
        db.query(OperationsMonitorEvent).delete()
        db.query(OperationsMonitorSample).delete()
        db.query(OperationsMonitorCheckpoint).delete()
        db.commit()
        core_monitor.run_check(db)
        store = core_monitor._store()
        sample = store.latest_sample(db)
        events = store.recent_events(db, 20)
        checkpoint = db.get(
            __import__(
                "app.models", fromlist=["OperationsMonitorCheckpoint"]
            ).OperationsMonitorCheckpoint,
            {"hub_id": settings.hub_id},
        )
        assert sample is not None
        assert sample.overall_availability == "available"
        assert sample.overall_health == "healthy"
        assert json.loads(sample.components_json)["states"]["nginx_local"] == "ok"
        assert any(
            event.event_type
            in {
                "health_changed",
                "availability_changed",
            }
            for event in events
        )
        assert checkpoint is not None
