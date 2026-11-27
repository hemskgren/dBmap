import json

import pytest
from app import container_runtime
from app.config import settings


def _listed_container_path(path: str) -> bool:
    return path.startswith("/v1.47/containers/json?")


def test_inspect_runtime_filters_compose_project_and_returns_selected_metadata(
    monkeypatch,
) -> None:
    monkeypatch.setattr(settings, "compose_project", "dbmap-test")
    container_runtime._api_version = None
    calls = []

    def fake_request(method: str, path: str) -> tuple[int, bytes]:
        calls.append((method, path))
        if path == "/version":
            return 200, b'{"ApiVersion":"1.47"}'
        if _listed_container_path(path):
            return 200, json.dumps(
                [{"Id": "abc123abc123", "State": "running", "Created": 2}]
            ).encode()
        if path == "/v1.47/containers/abc123abc123/json":
            return 200, json.dumps(
                {
                    "State": {
                        "Status": "running",
                        "Health": {"Status": "healthy"},
                        "StartedAt": "2026-12-01T10:00:00Z",
                    },
                    "RestartCount": 3,
                    "Config": {"Env": ["secret=not-returned"]},
                }
            ).encode()
        raise AssertionError(path)

    monkeypatch.setattr(container_runtime, "_request", fake_request)

    result = container_runtime.inspect_runtime("web")

    assert result == {
        "service": "Web",
        "compose_service": "dbmap-web",
        "container_id": "abc123abc123",
        "running": True,
        "docker_state": "running",
        "health_status": "healthy",
        "started_at": "2026-12-01T10:00:00Z",
        "restart_count": 3,
    }
    listed_path = calls[1][1]
    assert "com.docker.compose.service%3Ddbmap-web" in listed_path
    assert "com.docker.compose.project%3Ddbmap-test" in listed_path
    assert "Config" not in result


def test_logs_are_scoped_to_allowlist_and_limited_to_thirty_lines(monkeypatch) -> None:
    monkeypatch.setattr(settings, "compose_project", "dbmap")
    container_runtime._api_version = None
    calls = []
    body = "".join(f"line-{index}\n" for index in range(35)).encode()

    def fake_request(method: str, path: str) -> tuple[int, bytes]:
        calls.append(path)
        if path == "/version":
            return 200, b'{"ApiVersion":"1.47"}'
        if _listed_container_path(path):
            return 200, b'[{"Id":"abc123abc123","State":"running","Created":2}]'
        if path == "/v1.47/containers/abc123abc123/logs?stdout=1&stderr=1&tail=30":
            return 200, body
        raise AssertionError(path)

    monkeypatch.setattr(container_runtime, "_request", fake_request)
    result = container_runtime.read_tail("nginx")

    assert result["lines"] == [f"line-{index}" for index in range(5, 35)]
    assert result["line_limit"] == 30
    assert calls[-1].endswith("tail=30")
    with pytest.raises(container_runtime.UnknownService):
        container_runtime.read_tail("firmware-builder")


def test_docker_request_caps_response_body(monkeypatch) -> None:
    class FakeResponse:
        status = 200

        def read(self, limit: int) -> bytes:
            assert limit == container_runtime.MAX_RESPONSE_BYTES
            return b"bounded"

    class FakeConnection:
        def __init__(self, *_args, **_kwargs) -> None:
            pass

        def request(self, *_args) -> None:
            pass

        def getresponse(self) -> FakeResponse:
            return FakeResponse()

        def close(self) -> None:
            pass

    monkeypatch.setattr(container_runtime.os.path, "exists", lambda _path: True)
    monkeypatch.setattr(container_runtime, "_UnixHTTPConnection", FakeConnection)

    assert container_runtime._request("GET", "/version") == (200, b"bounded")
