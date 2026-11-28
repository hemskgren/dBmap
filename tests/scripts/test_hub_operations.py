import io
import json
import sys
import urllib.error
import urllib.parse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

spec = spec_from_file_location(
    "dbmap_hub_operations",
    Path(__file__).parents[2] / "scripts" / "hub_operations.py",
)
assert spec is not None and spec.loader is not None
hub_operations = module_from_spec(spec)
spec.loader.exec_module(hub_operations)


STATUS = {
    "service": {"health": "healthy"},
    "database": {"status": "ok"},
    "own_health": {"status": "healthy"},
    "monitoring": {
        "core": {"enabled": True, "running": True, "stale": False},
        "public": {},
        "coverage": {
            "health": {
                "state": "degraded",
                "score": 70,
                "reason": "Policy engine is offline",
                "components": [
                    {"id": "mqtt", "name": "MQTT broker", "state": "ok"}
                ],
            },
            "availability": {
                "current_state": "degraded",
                "windows": {
                    "24h": {
                        "percent": 98.5,
                        "state_summary": "degraded",
                        "eligible_seconds": 2000,
                        "available_seconds": 1950,
                        "unavailable_seconds": 50,
                        "unobserved_seconds": 100,
                        "low_coverage": True,
                        "insufficient_data": False,
                    }
                },
                "components": {},
            },
        },
        "public_components": {},
    },
}


def _collection(item_key, item, *, page=1, page_size=1, total_count=2, pages=2):
    return {
        item_key: [item],
        "pagination": {
            "total_count": total_count,
            "page": page,
            "page_size": page_size,
            "total_pages": pages,
            "order": "newest",
            "snapshot_id": 9,
        },
    }


@pytest.mark.parametrize(
    ("arguments", "payload", "expected_path", "expected_output"),
    [
        (
            ["status"],
            STATUS,
            "/api/v1/operations/status",
            "Core Hub availability: degraded",
        ),
        (
            ["services"],
            _collection(
                "services",
                {"id": "nginx", "name": "Nginx", "state": "ok"},
            ),
            "/api/v1/operations/services?",
            "complete=false",
        ),
        (
            ["details", "mqtt"],
            {"service": "MQTT broker", "status": "ok", "source": "Operations"},
            "/api/v1/operations/services/mqtt/details",
            "MQTT broker details",
        ),
        (
            ["logs", "nginx"],
            {
                "service": "Nginx",
                "compose_service": "nginx",
                "line_limit": 30,
                "lines": ["nginx started"],
                "source": "Operations Docker runtime",
            },
            "/api/v1/operations/services/nginx/logs",
            "nginx started",
        ),
        (
            [
                "events",
                "--page-size",
                "1",
                "--snapshot-id",
                "9",
                "--start",
                "2026-11-27T00:00:00Z",
                "--end",
                "2026-11-28T00:00:00Z",
            ],
            _collection(
                "events",
                {
                    "event_id": "event-1",
                    "occurred_at": "2026-11-28T00:00:00+00:00",
                    "component": "mqtt",
                    "event_type": "state_changed",
                    "previous_state": "ok",
                    "new_state": "offline",
                    "reason": "broker unavailable",
                    "observed_at": "2026-11-28T00:00:01+00:00",
                    "source": "operations_monitor",
                    "certainty": "confirmed",
                },
            ),
            "/api/v1/operations/events?",
            "Partial history",
        ),
        (
            ["failed-connections"],
            {
                "service": "MQTT broker",
                "window_hours": 3,
                "source": "file",
                "count": 1,
                "lines": ["client authentication failed"],
                "notices": [],
            },
            "/api/v1/operations/services/mosquitto/connection-failures",
            "last 3 hours",
        ),
    ],
)
def test_commands_use_operations_api_and_render_human_output(
    monkeypatch, capsys, arguments, payload, expected_path, expected_output
) -> None:
    calls = []
    monkeypatch.setattr(hub_operations, "load_project_env", lambda: None)
    monkeypatch.setattr(
        hub_operations,
        "validate_hub_connection",
        lambda _args, _parser: ("https://hub.test", "admin-token"),
    )

    def api_get(base_url, ca_file, admin_token, path):
        calls.append((base_url, ca_file, admin_token, path))
        return payload

    monkeypatch.setattr(hub_operations, "api_get", api_get)

    assert hub_operations.main(arguments) == 0

    output = capsys.readouterr().out
    assert expected_output in output
    assert len(calls) == 1
    assert calls[0][0] == "https://hub.test"
    assert calls[0][2] == "admin-token"
    assert calls[0][3].startswith(expected_path)
    assert calls[0][3].startswith("/api/v1/operations/")
    if arguments[0] == "events":
        query = urllib.parse.parse_qs(calls[0][3].split("?", 1)[1])
        assert query["page_size"] == ["1"]
        assert query["snapshot_id"] == ["9"]
        assert query["start"] == ["2026-11-27T00:00:00Z"]
        assert query["end"] == ["2026-11-28T00:00:00Z"]
        assert "source=operations_monitor" in output


def test_events_json_marks_partial_result_explicitly(monkeypatch, capsys) -> None:
    monkeypatch.setattr(hub_operations, "load_project_env", lambda: None)
    monkeypatch.setattr(
        hub_operations,
        "validate_hub_connection",
        lambda _args, _parser: ("https://hub.test", "admin-token"),
    )
    monkeypatch.setattr(
        hub_operations,
        "api_get",
        lambda *_args: _collection(
            "events",
            {"event_id": "event-1"},
            page=1,
            page_size=1,
            total_count=2,
            pages=2,
        ),
    )

    assert hub_operations.main(["events", "--output", "json"]) == 0

    payload = json.loads(capsys.readouterr().out)
    assert payload["result_complete"] is False
    assert payload["pagination"]["snapshot_id"] == 9


def test_status_watch_polls_until_interrupted(monkeypatch, capsys) -> None:
    calls = []
    monkeypatch.setattr(hub_operations, "load_project_env", lambda: None)
    monkeypatch.setattr(
        hub_operations,
        "validate_hub_connection",
        lambda _args, _parser: ("https://hub.test", "admin-token"),
    )
    monkeypatch.setattr(
        hub_operations,
        "api_get",
        lambda *_args: calls.append(True) or STATUS,
    )

    def stop_watch(_interval):
        raise KeyboardInterrupt

    monkeypatch.setattr(hub_operations.time, "sleep", stop_watch)

    assert hub_operations.main(["status", "--watch"]) == 0

    output = capsys.readouterr().out
    assert len(calls) == 1
    assert "Core Hub health: degraded" in output
    assert "Stopped." in output


@pytest.mark.parametrize(
    ("failure", "message"),
    [
        (
            urllib.error.HTTPError(
                "https://hub.test/api/v1/operations/status",
                503,
                "Unavailable",
                {},
                io.BytesIO(b'{"detail":"Operations is not enabled"}'),
            ),
            "disabled or unavailable",
        ),
        (
            urllib.error.HTTPError(
                "https://hub.test/api/v1/operations/status",
                403,
                "Forbidden",
                {},
                io.BytesIO(b'{"detail":"access denied"}'),
            ),
            "authentication/authorization failed",
        ),
        (
            urllib.error.URLError("connection refused"),
            "connection error reaching Operations API",
        ),
    ],
)
def test_api_errors_distinguish_disabled_auth_and_connection(
    monkeypatch, failure, message
) -> None:
    monkeypatch.setattr(
        hub_operations.ssl, "create_default_context", lambda **_kwargs: object()
    )

    def fail_request(*_args, **_kwargs):
        raise failure

    monkeypatch.setattr(hub_operations.urllib.request, "urlopen", fail_request)

    with pytest.raises(hub_operations.OperationsApiError, match=message):
        hub_operations.api_get(
            "https://hub.test",
            "ca.crt",
            "token",
            "/api/v1/operations/status",
        )
