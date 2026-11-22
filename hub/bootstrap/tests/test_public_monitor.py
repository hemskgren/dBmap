from datetime import UTC, datetime, timedelta
from io import BytesIO
from unittest.mock import Mock

import pytest
from app import hub_monitor, public_monitor
from app.config import settings
from app.models import Base, HubMonitorSample, PublicMonitorTransition
from sqlalchemy import create_engine, select
from sqlalchemy.orm import Session

START = datetime(2026, 11, 22, tzinfo=UTC)


@pytest.fixture
def db():
    engine = create_engine("sqlite://")
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
    assert len(db.scalars(select(PublicMonitorTransition)).all()) == 1
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
        client.on_connect(client, None, None, Mock(is_failure=rejected, value=0x87), None)
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
            db.commit()
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
    assert len(public["public_api"]["recent_events"]) == 1
    assert len(db.scalars(select(HubMonitorSample)).all()) == 1
    assert len(db.scalars(select(PublicMonitorTransition)).all()) == 4


@pytest.mark.parametrize("restart", [False, True])
def test_public_missed_check_excludes_gap_until_observations_resume(
    monkeypatch, db, restart
):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    ok = ("ok", None)
    public_monitor.record_check(db, "public_api", START, ok, None)
    db.commit()
    last = START + timedelta(minutes=10)
    now = START + timedelta(minutes=20)
    if not restart:
        public_monitor._current["public_api"] = last, ok, last
    stale = public_monitor.summary(db, now)["public_api"]
    assert stale["state"] == "unknown"
    assert stale["windows"]["24h"]["eligible_seconds"] == (0 if restart else 600)

    public_monitor.record_check(db, "public_api", now, ok, None if restart else last)
    db.commit()
    public_monitor._current["public_api"] = now, ok, now
    resumed = public_monitor.summary(db, now + timedelta(minutes=1))["public_api"]
    assert resumed["windows"]["24h"]["eligible_seconds"] == (60 if restart else 660)
    assert resumed["state"] == "ok"


def test_public_latest_anchor_survives_cleanup_without_stable_state_writes(
    monkeypatch, db
):
    monkeypatch.setattr(settings, "public_api_monitor_enabled", True)
    monkeypatch.setattr(settings, "hub_monitor_retention_days", 30)
    now = START + timedelta(days=31)
    ok = ("ok", None)
    public_monitor.record_check(db, "public_api", START, ok, None)
    db.commit()
    older = START - timedelta(minutes=1)
    db.add(
        PublicMonitorTransition(
            hub_id=settings.hub_id,
            component="public_api",
            observed_at=older,
            state="offline",
        )
    )
    db.commit()
    public_monitor.prune_history(db, "public_api", now)
    db.commit()
    public_monitor.record_check(db, "public_api", now, ok, now - timedelta(seconds=60))
    db.commit()
    assert len(db.scalars(select(PublicMonitorTransition)).all()) == 1
    assert db.scalars(select(HubMonitorSample)).all() == []
    public_monitor._current["public_api"] = now, ok, now
    assert (
        public_monitor.summary(db, now)["public_api"]["windows"]["30d"]["percent"]
        == 100
    )
