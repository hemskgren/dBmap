from app import api as api_module
from app.db import init_db
from app.main import create_app
from fastapi.testclient import TestClient


def test_nginx_probe_distinguishes_local_health_from_end_to_end_health(
    monkeypatch,
) -> None:
    calls = []

    def fake_https_status(host: str, port: int, path: str, server_name: str):
        calls.append((host, port, path, server_name))
        return "ok", None

    monkeypatch.setattr(api_module.settings, "public_base_path", "/edge")
    monkeypatch.setattr(api_module.probes, "https_status", fake_https_status)

    assert api_module.probes.probe_nginx_local() == ("ok", None)
    assert api_module.probes.probe_nginx() == ("ok", None)

    assert calls == [
        ("nginx", 8443, "/edge/api/v1/nginx/health", "hub.local"),
        ("nginx", 8443, "/edge/health", "hub.local"),
    ]


def test_mqtt_details_include_tls_protocol_and_connack_result(monkeypatch) -> None:
    class AcceptedReason:
        is_failure = False
        value = 0

        def getName(self):
            return "Success"

    class FakeClient:
        def __init__(self, *_args, **_kwargs):
            self.on_connect = None

        def username_pw_set(self, *_args):
            pass

        def tls_set_context(self, _context):
            pass

        def connect(self, *_args, **_kwargs):
            return api_module.probes.mqtt.MQTT_ERR_SUCCESS

        def loop(self, **_kwargs):
            self.on_connect(self, None, None, AcceptedReason(), None)
            return api_module.probes.mqtt.MQTT_ERR_SUCCESS

        def socket(self):
            return None

        def disconnect(self):
            pass

    monkeypatch.setattr(api_module.settings, "mqtt_password", "test-password")
    monkeypatch.setattr(api_module.probes.ssl, "create_default_context", lambda **_kwargs: object())
    monkeypatch.setattr(api_module.probes.mqtt, "Client", FakeClient)

    state, reason, details = api_module.probes.probe_mqtt_broker_diagnostics()

    assert state == "ok"
    assert reason is None
    assert details["protocol"] == "MQTT 3.1.1"
    assert details["transport"] == "TLS"
    assert details["check"] == "CONNECT/CONNACK"
    assert details["connack_code"] == 0
    assert details["connack_name"] == "Success"


def test_operations_details_and_logs_use_direct_api_and_report_delegated_sources(
    monkeypatch,
) -> None:
    monkeypatch.setattr(api_module.settings, "admin_token", "a" * 32)
    monkeypatch.setattr(api_module.settings, "viewer_token", "v" * 32)
    monkeypatch.setattr(api_module, "evaluate", lambda _input: True)
    monkeypatch.setattr(
        api_module.probes,
        "probe_nginx_local",
        lambda: ("ok", None),
    )
    monkeypatch.setattr(
        api_module,
        "probe_nginx",
        lambda: ("offline", "HTTP 503"),
    )
    monkeypatch.setattr(
        api_module.probes,
        "hub_component_details",
        lambda component: (
            {"service": component, "status": "online"},
            None,
        ),
    )
    monkeypatch.setattr(
        api_module.probes,
        "probe_runtime",
        lambda _service: (
            {"docker_state": "running", "restart_count": 1},
            None,
        ),
    )
    def fake_service_logs(service: str):
        if service == "database":
            raise ValueError("service logs are not supported")
        return (
            {"service": service, "lines": ["ready"], "line_limit": 30},
            None,
        )

    monkeypatch.setattr(api_module.probes, "fetch_service_logs", fake_service_logs)
    init_db()

    with TestClient(create_app()) as client:
        details = client.get(
            "/api/v1/operations/services/nginx/details",
            headers={"Authorization": "Bearer " + "v" * 32},
        )
        logs = client.get(
            "/api/v1/operations/services/operations/logs",
            headers={"Authorization": "Bearer " + "v" * 32},
        )
        unauthorized = client.get("/api/v1/operations/services/nginx/details")
        unsupported_logs = client.get(
            "/api/v1/operations/services/database/logs",
            headers={"Authorization": "Bearer " + "v" * 32},
        )

    assert details.status_code == 200
    result = details.json()
    assert result["status"] == "ok"
    assert result["source"] == "Operations direct probe"
    assert result["hub_diagnostics_source"] == "hub-bootstrap"
    assert result["hub_diagnostics"]["status"] == "online"
    assert result["runtime"]["restart_count"] == 1
    assert result["hub_api_via_nginx"]["status"] == "offline"
    assert logs.status_code == 200
    assert logs.json()["line_limit"] == 30
    assert logs.json()["source"] == "hub-bootstrap Docker runtime interface"
    assert unauthorized.status_code == 401
    assert unsupported_logs.status_code == 404


def test_operations_service_details_include_owned_database_diagnostics(monkeypatch) -> None:
    monkeypatch.setattr(api_module, "_self_worker_healthy", lambda: (True, []))
    monkeypatch.setattr(
        api_module.probes,
        "probe_runtime",
        lambda _service: (
            {"docker_state": "running", "restart_count": 0},
            None,
        ),
    )
    init_db()

    result = api_module.service_details("operations")

    assert result["database_source"] == "Operations"
    assert result["database"]["engine"] == "sqlite"
    assert "file_path" not in result["database"]
    assert result["runtime_source"] == "hub-bootstrap Docker runtime interface"


def test_hub_api_details_include_hub_owned_database_diagnostics(monkeypatch) -> None:
    monkeypatch.setattr(api_module, "probe_api", lambda: ("ok", None))
    monkeypatch.setattr(
        api_module.probes,
        "hub_component_details",
        lambda component: (
            {"service": component, "status": "online", "tables": []},
            None,
        ),
    )
    monkeypatch.setattr(
        api_module.probes,
        "probe_runtime",
        lambda _service: ({"docker_state": "running"}, None),
    )

    result = api_module.service_details("hub-api")

    assert result["status"] == "ok"
    assert result["database_source"] == "hub-bootstrap"
    assert result["database"]["service"] == "database"
    assert result["database"]["status"] == "online"
