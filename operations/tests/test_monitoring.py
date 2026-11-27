import json
from datetime import UTC, datetime, timedelta

from app import api as api_module
from app import core_monitor, monitoring_availability, monitoring_cycle
from app.config import settings
from app.db import SessionLocal, init_db
from app.models import (
    OperationsMonitorCheckpoint,
    OperationsMonitorEvent,
    OperationsMonitorSample,
    OperationsRuntimeCheckpoint,
)
from app.monitoring_availability import (
    Observation,
    calculate_window,
)


def test_assess_states_distinguishes_reachability_and_mqtt() -> None:
    unavailable = {
        "api": "ok",
        "database": "ok",
        "hub_api_via_nginx": "ok",
        "web": "ok",
        "mqtt": "offline",
        "opa": "ok",
    }
    assert core_monitor.assess_states(unavailable) == (
        "degraded",
        "unavailable",
    )

    reachability_down = dict(
        unavailable, mqtt="ok", hub_api_via_nginx="offline"
    )
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
    probe = core_monitor._probe_components()
    states = probe["components"]
    assert states["api"] == "offline"
    assert states["hub_api_via_nginx"] == "ok"
    assert states["nginx_local"] == "ok"
    assert states["web"] == "ok"
    assert states["mqtt"] == "ok"


def test_hub_restart_keeps_nginx_local_state_and_availability(monkeypatch) -> None:
    init_db()
    start = datetime.now(UTC) - timedelta(seconds=60)
    recovered = start + timedelta(seconds=60)
    check_times = [start, recovered]
    current_time = recovered
    results = {
        "api": ("ok", None),
        "database": ("ok", None),
        "nginx_end_to_end": ("ok", None),
        "nginx_local": ("ok", None),
        "web": ("ok", None),
        "mqtt": ("ok", None),
        "opa": ("ok", None),
    }

    def next_check_time() -> datetime:
        return check_times.pop(0) if check_times else current_time

    monkeypatch.setattr(core_monitor, "_utcnow", next_check_time)
    monkeypatch.setattr(core_monitor, "_last_check_at", None)
    monkeypatch.setattr(core_monitor.settings, "gap_threshold_s", 120)
    monkeypatch.setattr(core_monitor.settings, "core_monitor_interval_s", 60)
    monkeypatch.setattr(api_module, "_database_healthy", lambda _db: (True, None))
    monkeypatch.setattr(api_module, "_self_worker_healthy", lambda: (True, []))

    probes = {
        "probe_api": "api",
        "probe_database": "database",
        "probe_nginx": "nginx_end_to_end",
        "probe_nginx_local": "nginx_local",
        "probe_web": "web",
        "probe_mqtt_broker": "mqtt",
        "probe_opa": "opa",
    }
    for probe_name, result_name in probes.items():
        monkeypatch.setattr(
            core_monitor.probes,
            probe_name,
            lambda name=result_name: results[name],
        )
    monkeypatch.setattr(core_monitor.probes, "inspect_monitored_runtimes", dict)

    with SessionLocal() as db:
        db.query(OperationsMonitorEvent).delete()
        db.query(OperationsMonitorSample).delete()
        db.query(OperationsMonitorCheckpoint).delete()
        db.query(OperationsRuntimeCheckpoint).delete()
        db.commit()

        core_monitor.run_check(db)
        results.update(
            {
                "api": ("offline", "Temporary DNS resolution failure"),
                "database": ("unknown", "Hub diagnostic is unavailable"),
                "nginx_end_to_end": ("offline", "HTTP 502"),
            }
        )
        core_monitor.run_check(db)

        latest = core_monitor._store().latest_sample(db)
        assert latest is not None
        states = json.loads(latest.components_json)["states"]
        assert states["api"] == "offline"
        assert states["database"] == "unknown"
        assert states["hub_api_via_nginx"] == "offline"
        assert states["nginx_local"] == "ok"
        assert "nginx" not in states
        assert latest.overall_availability == "unavailable"

        events = core_monitor._store().recent_events(db, 100)
        assert any(
            event.component == "hub_api_via_nginx"
            and event.new_state == "offline"
            and event.reason == "HTTP 502"
            for event in events
        )
        assert not any(
            event.component == "nginx" and event.new_state == "offline"
            for event in events
        )
        assert not any(
            event.component == "nginx_local" and event.new_state == "offline"
            for event in events
        )

        services = {
            service["id"]: service for service in api_module._service_inventory(db)
        }
        assert services["nginx"]["state"] == "ok"
        assert services["hub-api"]["state"] == "offline"
        assert services["hub-api"]["api_via_nginx_state"] == "offline"
        assert services["hub-api"]["database"]["state"] == "unknown"

        history = core_monitor._store().history(db, recovered, settings.retention_days)
        payloads = {
            sample.sample_id: monitoring_cycle.sample_payload(
                sample, core_monitor.COMPONENT_IDS
            )
            for sample in history
        }
        nginx_points = monitoring_cycle.observations(
            history,
            payloads,
            component_id="nginx_local",
        )
        nginx_window = monitoring_availability.component_window(
            nginx_points,
            component_id="nginx_local",
            window_start=start,
            window_end=recovered,
            interval_s=60,
        )
        assert nginx_window["available_seconds"] == 60
        assert nginx_window["unavailable_seconds"] == 0
        assert nginx_window["unobserved_seconds"] == 0


def test_local_nginx_state_does_not_change_core_availability() -> None:
    states = {
        "api": "ok",
        "database": "ok",
        "hub_api_via_nginx": "ok",
        "nginx_local": "offline",
        "web": "ok",
        "mqtt": "ok",
        "opa": "ok",
    }
    assert core_monitor.assess_states(states) == ("healthy", "available")


def test_runtime_restart_events_use_persisted_signatures() -> None:
    init_db()
    initial = {
        "hub-api": {
            "container_id": "abc123abc123",
            "started_at": "2026-12-01T10:00:00Z",
            "restart_count": 1,
        }
    }
    restarted = {
        "hub-api": {
            "container_id": "abc123abc123",
            "started_at": "2026-12-01T10:01:00Z",
            "restart_count": 2,
        }
    }
    with SessionLocal() as db:
        db.query(OperationsMonitorEvent).delete()
        db.query(OperationsRuntimeCheckpoint).delete()
        core_monitor._detect_runtime_restarts(
            db, initial, datetime(2026, 12, 1, 10, 0, tzinfo=UTC)
        )
        db.commit()
    with SessionLocal() as db:
        core_monitor._detect_runtime_restarts(
            db, restarted, datetime(2026, 12, 1, 10, 1, tzinfo=UTC)
        )
        db.commit()
        events = db.query(OperationsMonitorEvent).all()
        assert len(events) == 1
        event = events[0]
        assert event is not None
        assert event.occurred_at.replace(tzinfo=UTC).isoformat() == (
            "2026-12-01T10:01:00+00:00"
        )
        assert event.component == "api"
        assert event.new_state == "restart_detected"
        assert event.reason == (
            "started_at changed to 2026-12-01T10:01:00Z; restart_count 1 -> 2"
        )
        assert event.source == "operations_monitor"
        assert event.certainty == "inferred"
    with SessionLocal() as db:
        core_monitor._detect_runtime_restarts(
            db, restarted, datetime(2026, 12, 1, 10, 2, tzinfo=UTC)
        )
        db.commit()
        assert db.query(OperationsMonitorEvent).count() == 1


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
    monkeypatch.setattr(core_monitor.probes, "inspect_monitored_runtimes", dict)
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
