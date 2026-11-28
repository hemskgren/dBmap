from pathlib import Path

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app import edge_status, service
from app.main import create_app
from fastapi.testclient import TestClient


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
        lambda: {
            "nginx": "online",
            "web": "offline",
            "firmware-builder": "unavailable",
        },
    )
    client = TestClient(create_app())
    response = client.get(
        "/api/v1/hub/status",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert response.status_code == 200, response.text
    components = {item["name"]: item for item in response.json()["components"]}
    assert components["Nginx"] == {
        "id": "nginx",
        "name": "Nginx",
        "status": "online",
    }
    assert components["Web"]["status"] == "offline"
    assert components["Firmware builder"]["status"] == "unavailable"
    assert components["Hub API"]["id"] == "hub-api"
    assert components["Policy engine"]["id"] == "opa"
    assert components["MQTT broker"]["id"] == "mosquitto"


def test_hub_status_uses_observation_aggregates_not_capped_list(monkeypatch) -> None:
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
    monkeypatch.setattr(
        edge_status,
        "statuses",
        lambda: {
            "nginx": "online",
            "web": "online",
            "firmware-builder": "unavailable",
        },
    )

    response = TestClient(create_app()).get(
        "/api/v1/hub/status",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert response.status_code == 200, response.text
    summary = response.json()["summary"]
    assert "observation_count" in summary
    assert "latest_observation_at" in summary
    assert "observation_limit" not in summary
    assert calls == {"list": 0, "count": 1, "latest": 1}


def test_hub_component_details_remain_available_to_operations(monkeypatch) -> None:
    monkeypatch.setattr(
        edge_status,
        "detail",
        lambda component: (
            {"service": "Nginx", "status": "online"}
            if component == "nginx"
            else None
        ),
    )
    client = TestClient(create_app())

    nginx = client.get(
        "/api/v1/hub/components/nginx/details",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    database = client.get(
        "/api/v1/hub/components/database/details",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert nginx.status_code == 200
    assert nginx.json() == {"service": "Nginx", "status": "online"}
    assert database.status_code == 200
    assert database.json()["status"] == "online"
    assert database.json()["engine"] == "sqlite"


def test_hub_mqtt_connection_failures_endpoint_is_retired() -> None:
    client = TestClient(create_app())
    response = client.get(
        "/api/v1/hub/components/mosquitto/connection-failures",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert response.status_code == 404


def test_hub_component_logs_endpoint_is_retired() -> None:
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
    operations = client.get(
        "/api/v1/hub/components/operations/logs",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )

    assert admin.status_code == 404
    assert viewer.status_code == 404
    assert missing.status_code == 404
    assert operations.status_code == 404
