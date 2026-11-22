from datetime import UTC, datetime, timedelta
from io import BytesIO
from unittest.mock import Mock

import pytest
from app import db as database
from app import hub_monitor, public_monitor
from app.config import settings
from app.models import (
    Base,
    HubMonitorCheckpoint,
    HubMonitorEvent,
    HubMonitorSample,
    PublicMonitorCheckpoint,
    PublicMonitorEvent,
    PublicMonitorSample,
)
from sqlalchemy import create_engine, select, text
from sqlalchemy.orm import Session

START = datetime(2026, 11, 22, tzinfo=UTC)


@pytest.fixture
def db(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path / 'public-monitor.db'}")
    Base.metadata.create_all(engine)
    with Session(engine) as session:
        yield session
    engine.dispose()


@pytest.fixture(autouse=True)
def clean_settings(monkeypatch):
    monkeypatch.setattr(public_monitor, "_current", {})
    monkeypatch.setattr(settings, "hub_monitor_enabled", True)
    monkeypatch.setattr(settings, "public_api_monitor_enabled", False)
    monkeypatch.setattr(settings, "public_mqtt_monitor_enabled", False)


def test_workers_start_only_for_enabled_components(monkeypatch):
    factory = Mock()
    monkeypatch.setattr(public_monitor.threading, "Thread", factory)
    stop = public_monitor.threading.Event()
    public_monitor.start(stop)
    factory.assert_not_called()
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    public_monitor.start(stop)
    factory.assert_called_once()
    assert factory.call_args.kwargs["args"] == (
        "public_api",
        public_monitor.probe_api,
        stop,
    )
    assert factory.call_args.kwargs["daemon"]


def test_worker_uses_60_second_schedule_and_keeps_runtime_status(monkeypatch, db):
    monkeypatch.setattr(public_monitor, "SessionLocal", lambda: db)
    elapsed = 0
    calls = []

    def probe():
        nonlocal elapsed
        calls.append(elapsed)
        elapsed += 2
        return "ok", None

    class Stop:
        def is_set(self):
            return len(calls) == 3 and elapsed >= 180

        def wait(self, seconds):
            nonlocal elapsed
            assert seconds == 58
            elapsed += seconds

    monkeypatch.setattr(public_monitor, "monotonic", lambda: elapsed)
    monkeypatch.setattr(
        public_monitor, "_utcnow", lambda: START + timedelta(seconds=elapsed)
    )
    public_monitor._loop("public_api", probe, Stop())
    assert calls == [0, 60, 120]
    assert len(db.scalars(select(PublicMonitorSample)).all()) == 1
    assert db.scalar(select(HubMonitorCheckpoint.last_check_at)) is None
    assert public_monitor._current["public_api"][0] == START + timedelta(seconds=122)


@pytest.mark.parametrize(
    ("status", "body", "expected"),
    [
        (200, b'{"status":"ok","service":"hub-bootstrap"}', "ok"),
        (200, b'{"status":"ok","service":"other"}', "offline"),
        (200, b"<html>proxy error</html>", "offline"),
        (302, b"", "offline"),
    ],
)
def test_public_api_checks_configured_tls_url_and_health_payload(
    monkeypatch, status, body, expected
):
    monkeypatch.setattr(settings, "public_hub_url", "https://public.example:8443/base")
    context = object()
    connection = Mock()
    connection.getresponse.return_value = BytesIO(body)
    connection.getresponse.return_value.status = status
    factory = Mock(return_value=connection)
    tls = Mock(return_value=context)
    monkeypatch.setattr(public_monitor.ssl, "create_default_context", tls)
    monkeypatch.setattr(public_monitor.http.client, "HTTPSConnection", factory)

    assert public_monitor.probe_api()[0] == expected
    tls.assert_called_once_with(cafile=settings.opa_ca_file)
    factory.assert_called_once_with(
        "public.example", 8443, timeout=settings.edge_probe_timeout_s, context=context
    )
    connection.request.assert_called_once_with("GET", "/base/health")
    connection.close.assert_called_once_with()


@pytest.mark.parametrize("rejected", [False, True])
def test_public_mqtt_requires_connack_and_never_uses_production_session(
    monkeypatch, rejected
):
    monkeypatch.setattr(settings, "public_mqtt_host", "public.example")
    monkeypatch.setattr(settings, "public_mqtt_port", 18883)
    monkeypatch.setattr(settings, "mqtt_username", "user")
    monkeypatch.setattr(settings, "mqtt_password", "test-only")
    context = object()
    tls = Mock(return_value=context)
    monkeypatch.setattr(public_monitor.ssl, "create_default_context", tls)
    client = Mock()
    client.connect.return_value = public_monitor.mqtt.MQTT_ERR_SUCCESS

    def receive(*, timeout):
        assert timeout > 0
        client.on_connect(
            client, None, None, Mock(is_failure=rejected, value=0x87), None
        )
        return public_monitor.mqtt.MQTT_ERR_SUCCESS

    client.loop.side_effect = receive
    factory = Mock(return_value=client)
    monkeypatch.setattr(public_monitor.mqtt, "Client", factory)
    result = public_monitor.probe_mqtt()
    assert result[0] == ("offline" if rejected else "ok")
    if rejected:
        assert "DynSec" in result[1]
    tls.assert_called_once_with(cafile=settings.mqtt_ca_file)
    client.tls_set_context.assert_called_once_with(context)
    client.username_pw_set.assert_called_once_with("user", "test-only")
    client.connect.assert_called_once_with("public.example", 18883, keepalive=30)
    assert factory.call_args.kwargs["client_id"].startswith("dbmap-probe-")
    assert factory.call_args.kwargs["clean_session"]
    assert factory.call_args.kwargs["reconnect_on_failure"] is False
    assert factory.call_args.kwargs["protocol"] == public_monitor.mqtt.MQTTv311
    client.subscribe.assert_not_called()
    client.publish.assert_not_called()
    client.will_set.assert_not_called()
    client.disconnect.assert_called_once()
    client.socket.return_value.close.assert_called_once()


def test_public_mqtt_missing_config_never_opens_a_connection(monkeypatch):
    monkeypatch.setattr(settings, "public_mqtt_host", "hub.local")
    factory = Mock()
    monkeypatch.setattr(public_monitor.mqtt, "Client", factory)
    assert public_monitor.probe_mqtt()[0] == "unknown"
    factory.assert_not_called()


def test_public_windows_and_events_do_not_change_existing_hub_history(monkeypatch, db):
    until = START + timedelta(minutes=20)
    monkeypatch.setattr(hub_monitor, "_utcnow", lambda: until)
    db.add(
        HubMonitorSample(
            hub_id=settings.hub_id,
            observed_at=START,
            overall_availability="available",
            overall_health="healthy",
            components_json='{"sample_mode":"sparse","states":{"mqtt":"ok"}}',
        )
    )
    db.commit()
    baseline = hub_monitor.build_monitoring_summary(db)
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    monkeypatch.setattr(settings, "public_mqtt_monitor_enabled", True)

    for component in public_monitor.PUBLIC_IDS:
        previous = None
        for cycle in range(20):
            at = START + timedelta(minutes=cycle)
            outage = component == "public_mqtt" and 5 <= cycle < 15
            result = ("offline", "TLS handshake failed") if outage else ("ok", None)
            public_monitor.record_check(db, component, at, result, previous)
            previous = at
        public_monitor._current[component] = at, result, at

    checked = hub_monitor.build_monitoring_summary(db)
    public = checked.pop("public_components")
    assert checked == baseline
    assert hub_monitor.list_recent_events(db)["events"] == []
    assert public["public_api"]["windows"]["24h"]["percent"] == 100
    assert public["public_mqtt"]["windows"]["24h"]["percent"] == 50
    assert public["public_mqtt"]["windows"]["24h"]["eligible_seconds"] == 1200
    assert public["public_mqtt"]["windows"]["7d"]["score_ready"] is False
    assert public["public_mqtt"]["windows"]["30d"]["score_ready"] is False
    assert [e["new_state"] for e in public["public_mqtt"]["recent_events"]] == [
        "ok",
        "offline",
        "ok",
    ]
    recovery = public["public_mqtt"]["recent_events"][0]
    assert recovery["event_type"] == "state_changed"
    assert recovery["previous_state"] == "offline"
    assert len(public["public_api"]["recent_events"]) == 1
    assert len(db.scalars(select(HubMonitorSample)).all()) == 1
    assert len(db.scalars(select(PublicMonitorSample)).all()) == 4


@pytest.mark.parametrize("restart", [False, True])
def test_public_missed_check_excludes_gap_until_observations_resume(
    monkeypatch, db, restart
):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    ok = ("ok", None)
    last = None
    for minute in range(11):
        at = START + timedelta(minutes=minute)
        public_monitor.record_check(db, "public_api", at, ok, last)
        last = at
    now = START + timedelta(minutes=20)
    if not restart:
        public_monitor._current["public_api"] = last, ok, last
    stale = public_monitor.summary(db, now)["public_api"]
    assert stale["state"] == "unknown"
    assert stale["windows"]["24h"]["eligible_seconds"] == 600

    public_monitor.record_check(db, "public_api", now, ok, None if restart else last)
    public_monitor._current["public_api"] = now, ok, now
    resumed = public_monitor.summary(db, now + timedelta(minutes=1))["public_api"]
    assert resumed["windows"]["24h"]["eligible_seconds"] == 660
    assert resumed["state"] == "ok"
    assert resumed["recent_events"][0]["event_type"] == "monitoring_resumed"


@pytest.mark.parametrize("downtime", [15, 180])
def test_public_restart_preserves_hour_of_stable_observation(monkeypatch, db, downtime):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    monkeypatch.setattr(settings, "public_mqtt_monitor_enabled", True)
    ok = ("ok", None)
    last = None
    for minute in range(61):
        at = START + timedelta(minutes=minute)
        public_monitor.record_check(db, "public_api", at, ok, last)
        last = at
    public_monitor.record_check(
        db, "public_mqtt", START, ("offline", "TLS failed"), None
    )
    assert len(db.scalars(select(PublicMonitorSample)).all()) == 2

    engine = db.get_bind()
    db.close()
    engine.dispose()
    now = last + timedelta(seconds=downtime)
    with Session(engine) as restarted:
        before_resume = public_monitor.summary(restarted, now)["public_api"]
        for name in ("24h", "7d", "30d"):
            assert before_resume["windows"][name]["eligible_seconds"] == 3600
        assert len(restarted.scalars(select(PublicMonitorSample)).all()) == 2

        public_monitor.record_check(restarted, "public_api", now, ok, None)
        public_monitor._current["public_api"] = now, ok, now
        summary = public_monitor.summary(restarted, now + timedelta(seconds=60))
        api = summary["public_api"]
        assert api["windows"]["24h"]["eligible_seconds"] == 3660
        assert api["windows"]["24h"]["percent"] == 100.0
        assert [e["event_type"] for e in api["recent_events"]] == (
            ["state_changed"]
            if downtime <= 120
            else ["monitoring_resumed", "state_changed"]
        )
        rows = restarted.scalars(
            select(PublicMonitorSample)
            .where(PublicMonitorSample.component == "public_api")
            .order_by(PublicMonitorSample.observed_at)
        ).all()
        assert [r.overall_availability for r in rows] == ["available", "unobserved", "available"]
        assert rows[1].observed_at == last.replace(tzinfo=None)
        assert restarted.get(
            PublicMonitorCheckpoint, (settings.hub_id, "public_api")
        ).last_check_at == now.isoformat()
        mqtt = restarted.scalar(
            select(PublicMonitorCheckpoint).where(
                PublicMonitorCheckpoint.component == "public_mqtt"
            )
        )
        assert mqtt.last_check_at == START.isoformat()
        assert summary["public_mqtt"]["windows"]["24h"]["unavailable_seconds"] == 0
        assert restarted.scalars(select(HubMonitorCheckpoint)).all() == []


def test_public_failed_commit_does_not_advance_checkpoint_or_transition(
    monkeypatch, db
):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    public_monitor._current["public_api"] = START, ("ok", None), START
    public_monitor.record_check(db, "public_api", START, ("ok", None), None)
    attempt = START + timedelta(seconds=60)

    def fail():
        db.flush()
        points = db.scalars(
            select(PublicMonitorSample).order_by(
                PublicMonitorSample.observed_at
            )
        ).all()
        assert len(points) == 2
        assert db.get(PublicMonitorCheckpoint, (settings.hub_id, "public_api")).last_check_at == attempt.isoformat()
        assert len(db.scalars(select(PublicMonitorEvent)).all()) == 2
        raise RuntimeError("commit failed")

    with monkeypatch.context() as patch:
        patch.setattr(db, "commit", fail)
        with pytest.raises(RuntimeError, match="commit failed"):
            public_monitor.record_check(
                db, "public_api", attempt, ("offline", "TLS failed"), START
            )
    db.rollback()
    rows = db.scalars(select(PublicMonitorSample)).all()
    assert len(rows) == 1 and rows[0].overall_availability == "available"
    assert db.get(PublicMonitorCheckpoint, (settings.hub_id, "public_api")).last_check_at == START.isoformat()
    assert len(db.scalars(select(PublicMonitorEvent)).all()) == 1
    summary = public_monitor.summary(db, START + timedelta(minutes=5))["public_api"]
    assert summary["windows"]["24h"]["eligible_seconds"] == 0
    assert summary["state"] == "unknown"



@pytest.mark.parametrize("legacy_value", [None, "invalid", "2026-11-21T00:00:00+00:00"])
def test_public_legacy_checkpoint_falls_back_without_fabricating_history(
    monkeypatch, db, legacy_value
):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    public_monitor.record_check(db, "public_api", START, ("ok", None), None)
    row = db.scalar(select(PublicMonitorCheckpoint))
    if legacy_value is None:
        db.delete(row)
    else:
        row.last_check_at = legacy_value
    db.commit()
    now = START + timedelta(hours=1)
    public_monitor.record_check(db, "public_api", now, ("ok", None), None)
    public_monitor._current["public_api"] = now, ("ok", None), now
    window = public_monitor.summary(db, now + timedelta(minutes=1))["public_api"]
    assert window["windows"]["24h"]["eligible_seconds"] == 60
    assert window["recent_events"][0]["event_type"] == "monitoring_resumed"


def test_upgrade_creates_parallel_tables_without_using_legacy_public_history(monkeypatch):
    engine = create_engine("sqlite://")
    monkeypatch.setattr(database, "engine", engine)
    with engine.begin() as connection:
        connection.execute(text(
            "CREATE TABLE public_monitor_transitions (transition_id VARCHAR PRIMARY KEY, state VARCHAR)"
        ))
        connection.execute(text("INSERT INTO public_monitor_transitions VALUES ('old', 'ok')"))
    database.init_db()
    database.init_db()

    for core, public, extra in (
        (HubMonitorSample, PublicMonitorSample, {"component"}),
        (HubMonitorEvent, PublicMonitorEvent, set()),
        (HubMonitorCheckpoint, PublicMonitorCheckpoint, {"component"}),
    ):
        assert set(public.__table__.columns.keys()) == set(core.__table__.columns.keys()) | extra
        for name, column in core.__table__.columns.items():
            other = public.__table__.columns[name]
            assert (str(other.type), other.nullable, other.primary_key) == (
                str(column.type), column.nullable, column.primary_key,
            )
    with Session(engine) as session:
        assert session.execute(text("SELECT * FROM public_monitor_transitions")).all() == [
            ("old", "ok"),
        ]
        for model in (PublicMonitorSample, PublicMonitorEvent, PublicMonitorCheckpoint):
            assert session.scalars(select(model)).all() == []
    engine.dispose()


def test_public_latest_anchor_survives_cleanup_without_stable_state_writes(
    monkeypatch, db
):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    monkeypatch.setattr(settings, "hub_monitor_retention_days", 30)
    now = START + timedelta(days=31)
    ok = ("ok", None)
    public_monitor.record_check(db, "public_api", START, ok, None)
    older = START - timedelta(minutes=1)
    db.add(
        PublicMonitorSample(
            hub_id=settings.hub_id,
            component="public_api",
            observed_at=older,
            overall_availability="unavailable",
            overall_health="unhealthy",
            components_json='{"sample_mode":"sparse","states":{"public_api":"offline"}}',
        )
    )
    db.commit()
    public_monitor.record_check(
        db, "public_api", now, ok, now - timedelta(seconds=60), prune=True
    )
    assert len(db.scalars(select(PublicMonitorSample)).all()) == 1
    assert db.scalars(select(HubMonitorSample)).all() == []
    assert db.scalars(select(PublicMonitorEvent)).all() == []
    public_monitor._current["public_api"] = now, ok, now
    assert (
        public_monitor.summary(db, now)["public_api"]["windows"]["30d"]["percent"]
        == 100
    )
