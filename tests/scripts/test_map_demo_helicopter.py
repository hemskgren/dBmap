"""Helicopter map-demo script: two Analysis scratch objects, shared namespace."""

from __future__ import annotations

import importlib.util
import sys
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "map_demo_helicopter.py"
sys.path.insert(0, str(SCRIPT_PATH.parent))
SPEC = importlib.util.spec_from_file_location("map_demo_helicopter", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
demo = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(demo)


def test_demo_payloads_are_two_related_map_layers() -> None:
    start = datetime(2026, 1, 1, 12, 0, tzinfo=timezone.utc)
    route = demo.helicopter_route_content(start=start)
    analysis = demo.helicopter_analysis_content(start=start)

    assert route["kind"] == analysis["kind"] == "map-layer"
    assert route["dataset"] == analysis["dataset"] == "helicopter"
    assert route["dataset_title"] == analysis["dataset_title"] == "Helicopter"
    assert route["layer"] == "route"
    assert analysis["layer"] == "analysis"
    assert route["role"] == "ground-truth"
    assert analysis["role"] == "analysis"
    assert len(route["points"]) >= 8

    gt = {(p["lat"], p["lon"]) for p in route["points"]}
    locs = {(p["lat"], p["lon"]) for p in analysis["localizations"]}
    assert gt.isdisjoint(locs)
    assert analysis["observations"]
    assert analysis["events"]
    assert analysis["localizations"]


def test_upload_creates_exactly_two_scratch_objects_via_api() -> None:
    calls: list[tuple[str, str, dict | None]] = []

    def fake_request(base_url, ca_file, token, method, path, payload=None):
        calls.append((method, path, payload))
        assert method == "POST"
        assert path == "/api/v1/analysis/scratch"
        assert payload is not None
        assert payload["namespace"] == "map-demo"
        assert payload["content"]["dataset"] == "helicopter"
        return {
            "id": f"id-{len(calls)}",
            "ttl_remaining_s": payload["ttl_seconds"],
            "expires_at": "2026-01-01T14:00:00+00:00",
        }

    with patch.object(demo, "request_json", side_effect=fake_request):
        assert demo.cmd_upload("https://hub.test", "ca.crt", "token", 3600) == 0

    assert len(calls) == 2
    layers = {call[2]["content"]["layer"] for call in calls}
    assert layers == {"route", "analysis"}


def test_clear_deletes_map_demo_namespace() -> None:
    with patch.object(
        demo,
        "request_json",
        return_value={"deleted": 2},
    ) as mocked:
        assert demo.cmd_clear("https://hub.test", "ca.crt", "token") == 0
        mocked.assert_called_once()
        _args, kwargs = mocked.call_args
        assert mocked.call_args.args[3] == "DELETE"
        assert "namespace=map-demo" in mocked.call_args.args[4]


def test_list_uses_include_content_false() -> None:
    with patch.object(
        demo,
        "request_json",
        return_value={"objects": [], "count": 0},
    ) as mocked:
        assert demo.cmd_list("https://hub.test", "ca.crt", "token") == 0
        path = mocked.call_args.args[4]
        assert "include_content=false" in path
        assert "namespace=map-demo" in path
