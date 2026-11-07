from datetime import UTC, datetime
from pathlib import Path

from app import mqtt_connection_failures
from app.config import settings
from app.main import create_app
from fastapi.testclient import TestClient


def test_filter_keeps_auth_tls_and_protocol_failures_inside_window() -> None:
    now = datetime(2026, 11, 1, 12, 0, 0, tzinfo=UTC)
    lines = [
        "01/Nov/2026 08:00:00: Client x [1.2.3.4:1] disconnected: connection closed by client.",
        "01/Nov/2026 10:00:00: Client y [1.2.3.4:2] disconnected: not authorised.",
        "01/Nov/2026 10:01:00: OpenSSL Error while trying to get the error[0]: error:0A000418:SSL routines::tlsv1 alert unknown ca",
        "01/Nov/2026 10:01:00: Client 1.2.3.4 [1.2.3.4:3] disconnected: Protocol error.",
        "01/Nov/2026 06:00:00: Client old [9.9.9.9:9] disconnected: not authorised.",
        "01/Nov/2026 10:02:00: Protocol error from 5.6.7.8:9: First packet not CONNECT (0A).",
        "01/Nov/2026 10:03:00: New client connected from 1.2.3.4:4 as ok (p4, c1, k60, u'ear-001').",
    ]
    matched = mqtt_connection_failures.filter_failure_lines(
        lines, now=now, window_hours=3
    )
    assert matched == [
        "01/Nov/2026 10:00:00: Client y [1.2.3.4:2] disconnected: not authorised.",
        "01/Nov/2026 10:01:00: OpenSSL Error while trying to get the error[0]: error:0A000418:SSL routines::tlsv1 alert unknown ca",
        "01/Nov/2026 10:01:00: Client 1.2.3.4 [1.2.3.4:3] disconnected: Protocol error.",
        "01/Nov/2026 10:02:00: Protocol error from 5.6.7.8:9: First packet not CONNECT (0A).",
    ]


def test_read_recent_failures_from_file(tmp_path: Path, monkeypatch) -> None:
    log_path = tmp_path / "mosquitto.log"
    stamp = datetime.now(UTC).strftime("%d/%b/%Y %H:%M:%S")
    log_path.write_text(
        f"{stamp}: Client z [203.0.113.10:1] disconnected: not authorised.\n"
        f"{stamp}: Client z [203.0.113.10:1] disconnected: connection closed by client.\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(settings, "mosquitto_log_path", str(log_path))
    result = mqtt_connection_failures.read_recent_failures()
    assert result["source"] == "file"
    assert result["count"] == 1
    assert "not authorised" in result["lines"][0]
    assert any("fail2ban" in notice.lower() for notice in result["notices"])


def test_connection_failures_endpoint_is_mosquitto_only(monkeypatch) -> None:
    monkeypatch.setattr(
        mqtt_connection_failures,
        "read_recent_failures",
        lambda: {
            "service": "MQTT broker",
            "compose_service": "mosquitto",
            "window_hours": 3,
            "source": "file",
            "lines": ["auth fail"],
            "count": 1,
            "notices": ["fail2ban note"],
        },
    )
    client = TestClient(create_app())
    ok = client.get(
        "/api/v1/hub/components/mosquitto/connection-failures",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    other = client.get(
        "/api/v1/hub/components/nginx/connection-failures",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert ok.status_code == 200, ok.text
    assert ok.json()["lines"] == ["auth fail"]
    assert other.status_code == 404
