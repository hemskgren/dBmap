import json
from pathlib import Path

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app import container_logs, edge_status
from app.main import create_app
from fastapi.testclient import TestClient


def _frame(text: str, stream: int = 1) -> bytes:
    payload = text.encode()
    return bytes([stream, 0, 0, 0]) + len(payload).to_bytes(4, "big") + payload


def test_decode_docker_logs_reads_multiplexed_frames_and_plain_text() -> None:
    assert container_logs.decode_docker_logs(_frame("one\n") + _frame("two\n", stream=2)) == ["one", "two"]
    assert container_logs.decode_docker_logs(b"plain line\n") == ["plain line"]
    assert container_logs.decode_docker_logs(b"") == []


def test_decode_docker_logs_keeps_the_last_ten_lines() -> None:
    lines = "".join(f"line-{index}\n" for index in range(12))
    assert container_logs.decode_docker_logs(lines.encode()) == [f"line-{index}" for index in range(2, 12)]


def test_read_tail_requests_only_the_named_compose_service(monkeypatch) -> None:
    monkeypatch.setattr(settings, "compose_project", "dbmap")
    container_logs._api_version = None
    calls: list[str] = []

    def fake_request(method: str, path: str) -> tuple[int, bytes]:
        calls.append(path)
        if path == "/version":
            return 200, b'{"ApiVersion":"1.47"}'
        if path.startswith("/v1.47/containers/json?"):
            body = [{"Id": "abc123abc123", "State": "running", "Created": 2}]
            return 200, json.dumps(body).encode()
        if path.startswith("/v1.47/containers/abc123abc123/logs?"):
            return 200, _frame("nginx: ready\n")
        raise AssertionError(path)

    monkeypatch.setattr(container_logs, "_request", fake_request)
    result = container_logs.read_tail("nginx")

    assert result["service"] == "Nginx"
    assert result["compose_service"] == "nginx"
    assert result["lines"] == ["nginx: ready"]
    assert result["line_limit"] == 10
    listed = calls[1]
    assert "com.docker.compose.service%3Dnginx" in listed
    assert "com.docker.compose.project%3Ddbmap" in listed
    assert calls[2].endswith("tail=10")


def test_read_tail_rejects_components_without_a_container() -> None:
    try:
        container_logs.read_tail("database")
    except container_logs.UnknownComponent:
        return
    raise AssertionError("database logs should be rejected before contacting Docker")


def test_probe_reads_the_health_body_before_closing() -> None:
    response = b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n"
    chunks = [response[:16], response[16:]]

    def read(size: int) -> bytes:
        if not chunks:
            return b""
        block = chunks.pop(0)
        return block[:size]

    assert edge_status.read_http_response(read) == response
    assert chunks == []


def test_nginx_detail_uses_the_public_base_path(monkeypatch) -> None:
    monkeypatch.setattr(settings, "public_base_path", "/dbmap")
    monkeypatch.setattr(settings, "nginx_probe_port", 8443)
    monkeypatch.setattr(settings, "nginx_probe_name", "hub.local")
    monkeypatch.setattr(edge_status, "https_status", lambda *args: 200)

    detail = edge_status.detail("nginx")

    assert detail is not None
    assert detail["status"] == "online"
    assert detail["check"] == "GET /dbmap/health as hub.local"


def test_firmware_builder_status_uses_its_health_endpoint(monkeypatch) -> None:
    calls = []

    def fake_status(host: str, port: int, path: str, server_hostname: str) -> int:
        calls.append((host, port, path, server_hostname))
        return 200

    monkeypatch.setattr(settings, "firmware_probe_host", "firmware-builder")
    monkeypatch.setattr(settings, "firmware_probe_port", 8443)
    monkeypatch.setattr(edge_status, "https_status", fake_status)
    assert edge_status.statuses()["firmware-builder"] == "online"
    assert ("firmware-builder", 8443, "/health", "firmware-builder") in calls

    monkeypatch.setattr(edge_status, "https_status", lambda *args: None)
    assert edge_status.statuses()["firmware-builder"] == "unavailable"


def test_hub_status_lists_web_nginx_and_firmware_builder(monkeypatch) -> None:
    monkeypatch.setattr(
        edge_status,
        "statuses",
        lambda: {"nginx": "online", "web": "offline", "firmware-builder": "unavailable"},
    )
    client = TestClient(create_app())
    response = client.get("/api/v1/hub/status", headers={"Authorization": f"Bearer {settings.admin_token}"})

    assert response.status_code == 200, response.text
    components = {item["name"]: item for item in response.json()["components"]}
    assert components["Nginx"] == {"id": "nginx", "name": "Nginx", "status": "online", "logs": True}
    assert components["Web"]["status"] == "offline"
    assert components["Web"]["logs"] is True
    assert components["Firmware builder"]["status"] == "unavailable"
    assert components["Database"]["logs"] is False
    assert components["Hub API"]["id"] == "hub-api"
    assert components["Policy engine"]["id"] == "opa"
    assert components["MQTT broker"]["id"] == "mosquitto"


def test_admin_can_read_component_logs_and_viewer_cannot(monkeypatch) -> None:
    def fake_tail(component: str) -> dict:
        if component != "web":
            raise container_logs.UnknownComponent(component)
        return {
            "service": "Web",
            "compose_service": "dbmap-web",
            "lines": ["GET /"],
            "line_limit": 10,
        }

    monkeypatch.setattr(container_logs, "read_tail", fake_tail)
    client = TestClient(create_app())
    admin = client.get(
        "/api/v1/hub/components/web/logs",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    viewer = client.get(
        "/api/v1/hub/components/web/logs",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    missing = client.get(
        "/api/v1/hub/components/database/logs",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert admin.status_code == 200, admin.text
    assert admin.json()["lines"] == ["GET /"]
    assert viewer.status_code == 403
    assert missing.status_code == 404
