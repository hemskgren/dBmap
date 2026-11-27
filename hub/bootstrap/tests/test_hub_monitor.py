import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from uuid import uuid4

import pytest
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
from app.models import (
    Base,
    HubMonitorCheckpoint,
    HubMonitorEvent,
    HubMonitorSample,
    Node,
    ReportedState,
)
from app.monitoring_math import ensure_aware
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, delete, select
from sqlalchemy.orm import Session


def _reset_db() -> None:
    init_db()
    db = SessionLocal()
    try:
        db.execute(
            delete(HubMonitorCheckpoint).where(
                HubMonitorCheckpoint.hub_id == settings.hub_id
            )
        )
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
    hub_monitor._last_check_at = None


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


@pytest.fixture
def monitored_device(monkeypatch, tmp_path):
    _patch_healthy(monkeypatch)
    monkeypatch.setattr(hub_monitor, "_last_check_at", None)
    monkeypatch.setattr(hub_monitor, "_last_docker_signatures", {})
    monkeypatch.setattr(hub_monitor, "_cycles_since_cleanup", 0)
    monkeypatch.setattr(settings, "device_monitor_enabled", True)
    monkeypatch.setattr(settings, "hub_monitor_interval_s", 60)
    monkeypatch.setattr(settings, "hub_monitor_gap_threshold_s", 120)
    monkeypatch.setattr(settings, "device_monitor_interval_s", 30)
    monkeypatch.setattr(settings, "device_online_window_s", 90)
    now = datetime.now(UTC)
    monkeypatch.setattr(hub_monitor, "_utcnow", lambda: now)
    node_id = f"OUT-{uuid4().hex[:12].upper()}"
    engine = create_engine(f"sqlite:///{tmp_path / 'monitoring.db'}")
    Base.metadata.create_all(engine)
    with Session(engine, autoflush=False) as db:
        db.add(
            Node(
                node_id=node_id,
                node_type="output",
                hardware_revision="test",
                provisioning_state="provisioned",
                lifecycle_state="active",
            )
        )
        db.add(ReportedState(node_id=node_id, last_seen_at=now))
        db.commit()

        def advance(seconds, *, keepalive=True):
            nonlocal now
            now += timedelta(seconds=seconds)
            if keepalive:
                db.get(ReportedState, node_id).last_seen_at = now
                db.commit()
            return now

        yield db, node_id, advance
    engine.dispose()


@pytest.mark.parametrize("downtime", [15, 180])
def test_restart_uses_durable_checkpoint_after_an_hour_of_sparse_checks(
    monkeypatch, monitored_device, downtime
) -> None:
    db, _, advance = monitored_device
    first_at = hub_monitor._utcnow()
    hub_monitor.run_check(db, include_summary=False)
    initial_event_ids = set(db.scalars(select(HubMonitorEvent.event_id)))
    for _ in range(60):
        last_check_at = advance(60)
        hub_monitor.run_check(db, include_summary=False)

    assert db.scalars(select(HubMonitorSample.observed_at)).all() == [
        first_at.replace(tzinfo=None)
    ]
    assert set(db.scalars(select(HubMonitorEvent.event_id))) == initial_event_ids
    assert db.scalars(select(HubMonitorCheckpoint.last_check_at)).all() == [
        last_check_at.isoformat()
    ]

    restarted_at = advance(downtime)
    bind = db.get_bind()
    db.close()
    # Lose the process's memory and reopen the database without a shutdown write.
    bind.dispose()
    monkeypatch.setattr(hub_monitor, "_last_check_at", None)
    with Session(bind, autoflush=False) as restarted:
        before_resume = hub_monitor.build_monitoring_summary(restarted)["availability"]
        for name in ("24h", "7d", "30d"):
            assert before_resume["windows"][name]["eligible_seconds"] == 3600
            assert before_resume["windows"][name]["not_running_seconds"] == downtime
            for component in ("api", "mqtt", "web"):
                window = before_resume["components"][component]["windows"][name]
                assert window["eligible_seconds"] == 3600
        assert len(restarted.scalars(select(HubMonitorSample)).all()) == 1

        summary = hub_monitor.run_check(restarted)
        window = summary["availability"]["windows"]["24h"]
        assert window["eligible_seconds"] == 3600
        assert window["not_running_seconds"] == downtime
        assert window["percent"] == 100.0
        assert restarted.scalar(
            select(HubMonitorCheckpoint.last_check_at)
        ) == restarted_at.isoformat()

        samples = restarted.scalars(
            select(HubMonitorSample).order_by(HubMonitorSample.observed_at)
        ).all()
        assert [s.overall_availability for s in samples] == [
            "available",
            "unobserved",
            "available",
        ]
        assert ensure_aware(samples[1].observed_at) == last_check_at
        assert ensure_aware(samples[2].observed_at) == restarted_at

        advance(60)
        summary = hub_monitor.run_check(restarted)
        assert summary["sample_count"] == 3
        assert summary["availability"]["windows"]["24h"]["eligible_seconds"] == 3660
        events = hub_monitor.list_recent_events(restarted)["events"]
        assert sum(e["certainty"] == "inferred" for e in events) == int(downtime > 120)
        if downtime <= 120:
            assert {event["event_id"] for event in events} == initial_event_ids


@pytest.mark.parametrize("checkpoint", [None, "invalid", "naive", "stale", "future"])
def test_restart_falls_back_if_checkpoint_missing_or_invalid(
    monkeypatch, monitored_device, checkpoint
) -> None:
    db, _, advance = monitored_device
    first_at = hub_monitor._utcnow()
    hub_monitor.run_check(db, include_summary=False)
    now = advance(180)
    values = {
        "invalid": "not a timestamp",
        "naive": now.replace(tzinfo=None).isoformat(),
        "stale": (first_at - timedelta(seconds=1)).isoformat(),
        "future": (now + timedelta(seconds=1)).isoformat(),
    }
    saved = db.get(HubMonitorCheckpoint, settings.hub_id)
    if checkpoint is None:
        db.delete(saved)
    else:
        saved.last_check_at = values[checkpoint]
    db.commit()

    monkeypatch.setattr(hub_monitor, "_last_check_at", None)
    summary = hub_monitor.run_check(db)
    window = summary["availability"]["windows"]["24h"]
    assert window["eligible_seconds"] == 0
    assert window["not_running_seconds"] == 179
    assert db.scalar(select(HubMonitorCheckpoint.last_check_at)) == now.isoformat()


@pytest.mark.parametrize("failure", ["probe", "before_commit"])
def test_incomplete_cycle_does_not_commit_checkpoint_or_history(
    monkeypatch, monitored_device, failure
) -> None:
    db, _, advance = monitored_device
    first_at = hub_monitor._utcnow()
    hub_monitor.run_check(db, include_summary=False)
    initial_event_ids = set(db.scalars(select(HubMonitorEvent.event_id)))
    attempted_at = advance(60)
    monkeypatch.setattr(settings, "mqtt_enabled", True)
    monkeypatch.setattr(hub_monitor, "is_connected", lambda: False)

    def fail_commit():
        # All three have reached the transaction, but none may survive its rollback.
        db.flush()
        assert db.scalar(
            select(HubMonitorCheckpoint.last_check_at)
        ) == attempted_at.isoformat()
        assert len(db.scalars(select(HubMonitorSample)).all()) == 2
        assert set(db.scalars(select(HubMonitorEvent.event_id))) > initial_event_ids
        raise RuntimeError("test commit failure")

    def fail_probe(_session):
        raise RuntimeError("test probe failure")

    with monkeypatch.context() as patch:
        if failure == "probe":
            patch.setattr(hub_monitor, "_probe_components", fail_probe)
            error = "test probe failure"
        elif failure == "before_commit":
            patch.setattr(db, "commit", fail_commit)
            error = "test commit failure"
        with pytest.raises(RuntimeError, match=error):
            hub_monitor.run_check(db, include_summary=False)

    bind = db.get_bind()
    db.close()
    bind.dispose()
    assert hub_monitor._last_check_at == first_at
    with Session(bind) as reopened:
        assert reopened.scalar(
            select(HubMonitorCheckpoint.last_check_at)
        ) == first_at.isoformat()
        assert reopened.scalars(select(HubMonitorSample.overall_availability)).all() == [
            "available"
        ]
        assert set(reopened.scalars(select(HubMonitorEvent.event_id))) == initial_event_ids

        with monkeypatch.context() as patch:
            patch.setattr(hub_monitor, "_utcnow", lambda: first_at + timedelta(minutes=5))
            window = hub_monitor.build_monitoring_summary(reopened)["availability"]["windows"]["24h"]
            assert window["eligible_seconds"] == 0
            assert window["not_running_seconds"] == 300

        hub_monitor.run_check(reopened, include_summary=False)
        assert reopened.scalar(
            select(HubMonitorCheckpoint.last_check_at)
        ) == attempted_at.isoformat()
        assert reopened.scalars(
            select(HubMonitorSample.overall_availability).order_by(HubMonitorSample.observed_at)
        ).all() == ["available", "unavailable"]


def test_failed_commit_does_not_advance_monitor_continuity(
    monkeypatch, monitored_device
) -> None:
    db, _, advance = monitored_device
    hub_monitor.run_check(db)
    advance(63)

    def fail_commit():
        raise RuntimeError("test commit failure")

    with monkeypatch.context() as patch:
        patch.setattr(db, "commit", fail_commit)
        with pytest.raises(RuntimeError, match="test commit failure"):
            hub_monitor.run_check(db)
    db.rollback()

    advance(63)
    summary = hub_monitor.run_check(db)
    assert summary["sample_count"] == 3
    # The gap marker is one microsecond after the initial sample.
    assert summary["availability"]["windows"]["24h"]["not_running_seconds"] == 125


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
        sample.observed_at = datetime.now(UTC) - timedelta(hours=2)
        db.execute(delete(HubMonitorCheckpoint))
        db.commit()
        monkeypatch.setattr(hub_monitor, "_last_check_at", None)
        hub_monitor.run_check(db)
        resumed = [
            event
            for event in hub_monitor.list_recent_events(db, limit=15)["events"]
            if event["event_type"] == "monitoring_resumed"
            and event["certainty"] == "inferred"
        ]
        assert resumed
        window = hub_monitor.compute_availability_window(
            list(db.scalars(select(HubMonitorSample))),
            window_start=datetime.now(UTC) - timedelta(hours=3),
            window_end=datetime.now(UTC),
            interval_s=60,
        )
        assert window["unobserved_seconds"] > 0
        assert window["not_running_seconds"] > 0
        assert any(gap.get("kind") == "not_running" for gap in window["gaps"])
    finally:
        db.close()


def test_insufficient_history_returns_null_percent() -> None:
    now = datetime.now(UTC)
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
    now = datetime.now(UTC)
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
            observed_at=datetime.now(UTC) - timedelta(days=120),
            overall_availability="available",
            overall_health="healthy",
            components_json="{}",
        )
        old_event = HubMonitorEvent(
            event_id=str(uuid4()),
            hub_id="local-test",
            occurred_at=datetime.now(UTC) - timedelta(days=120),
            observed_at=datetime.now(UTC) - timedelta(days=120),
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


def test_stable_sparse_anchor_survives_retention(monkeypatch, monitored_device) -> None:
    db, _, advance = monitored_device
    monkeypatch.setattr(settings, "hub_monitor_retention_days", 30)
    hub_monitor.run_check(db, include_summary=False)
    anchor = db.scalar(select(HubMonitorSample))
    older = _sample(
        observed_at=ensure_aware(anchor.observed_at) - timedelta(minutes=1),
        availability="unavailable",
        health="degraded",
        states={"mqtt": "offline"},
        sparse=True,
    )
    db.add(older)
    db.commit()

    now = advance(31 * 86400)
    # Model the immediately preceding committed check in a month of stable polling.
    monkeypatch.setattr(hub_monitor, "_last_check_at", now - timedelta(minutes=1))
    before = hub_monitor.run_check(db)
    removed = hub_monitor.cleanup_history(db, now)
    db.commit()
    assert removed["samples_deleted"] == 1
    assert removed["events_deleted"] > 0
    assert db.scalars(select(HubMonitorSample.sample_id)).all() == [anchor.sample_id]
    assert db.scalars(select(HubMonitorEvent)).all() == []

    after = hub_monitor.build_monitoring_summary(db)
    for name, delta in hub_monitor.WINDOW_SPECS:
        window = after["availability"]["windows"][name]
        assert window == before["availability"]["windows"][name]
        assert window["percent"] == 100
        assert window["eligible_seconds"] == delta.total_seconds()
    assert db.scalar(select(HubMonitorCheckpoint.last_check_at)) == now.isoformat()


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
        now = datetime.now(UTC)
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
    sparse: bool = False,
) -> HubMonitorSample:
    payload: dict = {"states": states or {"api": "ok", "mqtt": "ok", "web": "ok"}}
    if sparse:
        payload["sample_mode"] = hub_monitor.SAMPLE_MODE_SPARSE
    return HubMonitorSample(
        sample_id=str(uuid4()),
        hub_id=settings.hub_id,
        observed_at=observed_at,
        overall_availability=availability,
        overall_health=health,
        components_json=json.dumps(payload, separators=(",", ":")),
    )


def test_mqtt_outage_reduces_core_and_mqtt_availability() -> None:
    """Confirmed MQTT downtime must appear in historical metrics (not round to 100%)."""
    now = datetime.now(UTC)
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
    now = datetime.now(UTC)
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
    now = datetime.now(UTC)
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
            datetime.now(UTC),
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
            datetime.now(UTC),
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
            datetime.now(UTC),
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
    now = datetime.now(UTC)
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
        now = datetime.now(UTC)
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


def test_new_samples_omit_docker_but_legacy_docker_payloads_still_parse(
    monkeypatch,
) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        now = datetime.now(UTC)
        db.add(
            HubMonitorSample(
                sample_id=str(uuid4()),
                hub_id=settings.hub_id,
                observed_at=now - timedelta(minutes=2),
                overall_availability="available",
                overall_health="healthy",
                components_json=json.dumps(
                    {
                        "states": {"api": "ok", "mqtt": "ok"},
                        "docker": {"nginx": {"docker_state": "running"}},
                    }
                ),
            )
        )
        db.commit()
        legacy = hub_monitor._parse_sample_payload(
            db.scalars(select(HubMonitorSample)).first()
        )
        assert legacy["docker"]["nginx"]["docker_state"] == "running"

        summary = hub_monitor.run_check(db)
        assert summary["health"]["state"] == "healthy"
        sample = hub_monitor._store().latest_sample(db)
        assert sample is not None
        stored = json.loads(sample.components_json)
        assert "docker" not in stored
        assert "states" in stored
        assert "last_success" in stored
        assert "reasons" in stored
        assert stored.get("sample_mode") == hub_monitor.SAMPLE_MODE_SPARSE
    finally:
        db.close()


def test_build_monitoring_summary_parses_each_sample_json_once(monkeypatch) -> None:
    """P0: component windows must not re-parse JSON per component × window."""
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        now = datetime.now(UTC)
        sample_count = 40
        for offset in range(sample_count, 0, -1):
            db.add(
                _sample(
                    observed_at=now - timedelta(minutes=offset),
                    availability="available",
                    states={
                        "api": "ok",
                        "mqtt": "ok",
                        "web": "ok",
                        "nginx": "ok",
                        "database": "ok",
                        "opa": "ok",
                    },
                )
            )
        db.commit()

        parse_calls = {"n": 0}
        original = hub_monitor._parse_sample_payload

        def counting_parse(sample: HubMonitorSample):
            parse_calls["n"] += 1
            return original(sample)

        monkeypatch.setattr(hub_monitor, "_parse_sample_payload", counting_parse)
        summary = hub_monitor.build_monitoring_summary(db)
        assert summary["sample_count"] == sample_count
        # One parse per history sample (plus at most one for latest-sample current view).
        assert parse_calls["n"] <= sample_count + 1
        # Without the cache, 3 component IDs × 3 windows would re-parse each sample.
        assert parse_calls["n"] < sample_count * len(hub_monitor.COMPONENT_WINDOW_IDS)
    finally:
        db.close()


def test_sparse_hold_covers_window_from_pre_window_sample() -> None:
    """Stable sparse state covers a window even when no sample falls inside it."""
    now = datetime.now(UTC)
    window_start = now - timedelta(hours=24)
    window_end = now
    samples = [
        _sample(
            observed_at=window_start - timedelta(hours=2),
            availability="available",
            sparse=True,
        )
    ]
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=window_start,
        window_end=window_end,
        interval_s=60,
    )
    assert window["available_seconds"] == int(timedelta(hours=24).total_seconds())
    assert window["unavailable_seconds"] == 0
    assert window["unobserved_seconds"] == 0
    assert window["sample_count"] == 0
    assert window["percent"] == 100.0


def test_sparse_unobserved_gap_excluded_and_not_held_as_available() -> None:
    now = datetime.now(UTC)
    gap_start = now - timedelta(hours=3)
    gap_end = now - timedelta(hours=1)
    samples = [
        _sample(
            observed_at=now - timedelta(hours=4),
            availability="available",
            sparse=True,
        ),
        _sample(
            observed_at=gap_start + timedelta(seconds=1),
            availability="unobserved",
            sparse=True,
        ),
        _sample(
            observed_at=gap_end,
            availability="available",
            sparse=True,
        ),
    ]
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=now - timedelta(hours=5),
        window_end=now,
        interval_s=60,
    )
    assert window["unobserved_seconds"] >= int(timedelta(hours=2).total_seconds()) - 2
    assert window["not_running_seconds"] >= int(timedelta(hours=2).total_seconds()) - 2
    eligible = window["available_seconds"] + window["unavailable_seconds"]
    assert eligible == window["eligible_seconds"]
    assert window["unobserved_seconds"] + eligible == int(
        timedelta(hours=5).total_seconds()
    )


def test_legacy_dense_long_gap_stays_unobserved_without_rewrite() -> None:
    """Historical dense rows without sample_mode keep interval gap semantics."""
    now = datetime.now(UTC)
    samples = [
        _sample(
            observed_at=now - timedelta(hours=3),
            availability="available",
            sparse=False,
        ),
        _sample(
            observed_at=now - timedelta(minutes=1),
            availability="available",
            sparse=False,
        ),
    ]
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=now - timedelta(hours=3),
        window_end=now,
        interval_s=60,
    )
    assert window["available_seconds"] == 120  # one interval confirmed per sample span end
    assert window["unobserved_seconds"] > 0
    assert window["not_running_seconds"] > 0
    assert any(gap.get("kind") == "not_running" for gap in window["gaps"])


def test_mixed_dense_then_sparse_window_preserves_both_attribution_modes() -> None:
    """A long window spanning legacy dense + sparse samples must not blur modes.

    Near the model boundary: dense MQTT outage keeps interval unavailable
    seconds; later sparse stable available holds for hours without rewriting
    the dense history.
    """
    now = datetime.now(UTC)
    window_start = now - timedelta(days=30)
    samples: list[HubMonitorSample] = []
    # Legacy dense minute ticks for one hour ending 10 days ago.
    dense_end = now - timedelta(days=10)
    for minute in range(60, 0, -1):
        mqtt_down = 20 <= minute <= 29  # 10 minutes unavailable
        samples.append(
            _sample(
                observed_at=dense_end - timedelta(minutes=minute),
                availability="unavailable" if mqtt_down else "available",
                health="degraded" if mqtt_down else "healthy",
                states={
                    "api": "ok",
                    "mqtt": "offline" if mqtt_down else "ok",
                    "web": "ok",
                },
                sparse=False,
            )
        )
    # Model boundary: first sparse sample immediately after the dense block,
    # then a long stable sparse hold with no further samples until now.
    boundary = dense_end
    samples.append(
        _sample(
            observed_at=boundary,
            availability="available",
            sparse=True,
        )
    )
    window = hub_monitor.compute_availability_window(
        samples,
        window_start=window_start,
        window_end=now,
        interval_s=60,
    )
    # Dense block: ~10 minutes unavailable under interval attribution.
    assert window["unavailable_seconds"] >= 9 * 60
    assert window["unavailable_seconds"] <= 11 * 60
    # Sparse hold from boundary to now (~10 days) must count as available,
    # not collapse to a single interval or become unobserved.
    assert window["available_seconds"] >= int(timedelta(days=9).total_seconds())
    # Leading time before the dense block remains unobserved (not unavailable).
    assert window["unobserved_seconds"] >= int(timedelta(days=19).total_seconds())
    assert window["percent"] is not None
    assert window["percent"] < 100.0


def test_run_check_is_transition_sparse_and_records_unobserved_on_gap(
    monkeypatch,
) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    db = SessionLocal()
    try:
        hub_monitor.run_check(db)
        hub_monitor.run_check(db)
        samples = list(
            db.scalars(
                select(HubMonitorSample).order_by(HubMonitorSample.observed_at.asc())
            )
        )
        assert len(samples) == 1

        only = samples[0]
        only.observed_at = datetime.now(UTC) - timedelta(hours=2)
        db.execute(delete(HubMonitorCheckpoint))
        db.commit()
        monkeypatch.setattr(hub_monitor, "_last_check_at", None)
        hub_monitor.run_check(db)
        samples = list(
            db.scalars(
                select(HubMonitorSample).order_by(HubMonitorSample.observed_at.asc())
            )
        )
        assert len(samples) == 3
        assert samples[1].overall_availability == "unobserved"
        assert json.loads(samples[2].components_json)["sample_mode"] == "sparse"
        resumed = [
            event
            for event in hub_monitor.list_recent_events(db, limit=15)["events"]
            if event["event_type"] == "monitoring_resumed"
            and event["certainty"] == "inferred"
        ]
        assert resumed
        window = hub_monitor.compute_availability_window(
            samples,
            window_start=datetime.now(UTC) - timedelta(hours=3),
            window_end=datetime.now(UTC),
            interval_s=60,
        )
        assert window["unobserved_seconds"] > 0
        assert window["not_running_seconds"] > 0
    finally:
        db.close()


def test_hub_status_uses_observation_aggregates_not_capped_list(monkeypatch) -> None:
    _reset_db()
    _patch_healthy(monkeypatch)
    from app import service

    calls = {"list": 0, "count": 0, "latest": 0}
    original_list = service.list_observations
    original_count = service.count_observations
    original_latest = service.latest_observation_received_at

    def counting_list(*args, **kwargs):
        calls["list"] += 1
        return original_list(*args, **kwargs)

    def counting_count(*args, **kwargs):
        calls["count"] += 1
        return original_count(*args, **kwargs)

    def counting_latest(*args, **kwargs):
        calls["latest"] += 1
        return original_latest(*args, **kwargs)

    monkeypatch.setattr(service, "list_observations", counting_list)
    monkeypatch.setattr(service, "count_observations", counting_count)
    monkeypatch.setattr(service, "latest_observation_received_at", counting_latest)

    client = TestClient(create_app())
    response = client.get(
        "/api/v1/hub/status",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert response.status_code == 200, response.text
    summary = response.json()["summary"]
    assert "observation_count" in summary
    assert "latest_observation_at" in summary
    assert "observation_limit" not in summary
    assert calls["list"] == 0
    assert calls["count"] == 1
    assert calls["latest"] == 1
