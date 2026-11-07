#!/usr/bin/env python3
"""Upload or clear the Helicopter map-demo scratch layers via the Analysis API."""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from lib.hub_cli import (
    add_hub_connection_arguments,
    format_hub_http_error,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)

NAMESPACE = "map-demo"
DATASET = "helicopter"
DATASET_TITLE = "Helicopter"
DEFAULT_TTL_S = 7200


class DemoError(RuntimeError):
    pass


def request_json(
    base_url: str,
    ca_file: str,
    token: str,
    method: str,
    path: str,
    payload: dict[str, Any] | None = None,
) -> Any:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Authorization": f"Bearer {token}"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=data,
        headers=headers,
        method=method,
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=30) as response:
            if response.status == 204 or response.length == 0:
                return None
            return json.load(response)
    except urllib.error.HTTPError as exc:
        raise DemoError(
            format_hub_http_error(exc.code, exc.read(2048), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise DemoError(f"could not reach Analysis/Hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise DemoError(f"invalid JSON for {path}") from exc


def _route_points(start: datetime) -> list[dict[str, Any]]:
    # Simulated ground-truth track near the default Hub weather region (Stockholm).
    # Distinct from analysis localization estimates below.
    waypoints = [
        (59.34080, 18.06050),
        (59.34210, 18.06620),
        (59.34340, 18.07240),
        (59.34420, 18.07890),
        (59.34480, 18.08560),
        (59.34410, 18.09210),
        (59.34270, 18.09780),
        (59.34090, 18.10240),
        (59.33880, 18.10590),
        (59.33650, 18.10720),
        (59.33420, 18.10600),
        (59.33240, 18.10210),
    ]
    points: list[dict[str, Any]] = []
    for index, (lat, lon) in enumerate(waypoints):
        points.append(
            {
                "lat": lat,
                "lon": lon,
                "time_utc": (start + timedelta(seconds=index * 25)).isoformat(),
                "label": f"GT-{index + 1:02d}",
            }
        )
    return points


def helicopter_route_content(*, start: datetime) -> dict[str, Any]:
    return {
        "kind": "map-layer",
        "dataset": DATASET,
        "dataset_title": DATASET_TITLE,
        "layer": "route",
        "title": "Helicopter ground-truth route",
        "role": "ground-truth",
        "description": (
            "Simulated reference flight path (ground truth). "
            "Not an input to acoustic analysis."
        ),
        "points": _route_points(start),
    }


def helicopter_analysis_content(*, start: datetime) -> dict[str, Any]:
    # Observation / event / localization coordinates are offset from the GT route.
    return {
        "kind": "map-layer",
        "dataset": DATASET,
        "dataset_title": DATASET_TITLE,
        "layer": "analysis",
        "title": "Helicopter acoustic analysis (simulated)",
        "role": "analysis",
        "description": (
            "Simulated Observation markers, Event summary, and estimated "
            "Localization points for map integration demos."
        ),
        "observations": [
            {
                "id": "obs-sim-01",
                "lat": 59.34155,
                "lon": 18.06980,
                "time_utc": (start + timedelta(seconds=40)).isoformat(),
                "label": "Ear-side detection A",
                "node_id": "SIM-EAR-001",
                "signal_level_dbfs": -28.4,
            },
            {
                "id": "obs-sim-02",
                "lat": 59.34305,
                "lon": 18.08120,
                "time_utc": (start + timedelta(seconds=95)).isoformat(),
                "label": "Ear-side detection B",
                "node_id": "SIM-EAR-002",
                "signal_level_dbfs": -31.1,
            },
            {
                "id": "obs-sim-03",
                "lat": 59.33940,
                "lon": 18.10050,
                "time_utc": (start + timedelta(seconds=185)).isoformat(),
                "label": "Ear-side detection C",
                "node_id": "SIM-EAR-003",
                "signal_level_dbfs": -26.7,
            },
        ],
        "events": [
            {
                "id": "event-heli-01",
                "label": "Simulated helicopter pass",
                "time_utc": (start + timedelta(seconds=120)).isoformat(),
                "lat": 59.34250,
                "lon": 18.08600,
                "member_count": 3,
                "status": "complete",
            }
        ],
        "localizations": [
            {
                "id": "loc-sim-01",
                "lat": 59.34295,
                "lon": 18.07510,
                "time_utc": (start + timedelta(seconds=70)).isoformat(),
                "label": "Estimated position 1",
                "confidence": 0.62,
            },
            {
                "id": "loc-sim-02",
                "lat": 59.34355,
                "lon": 18.08840,
                "time_utc": (start + timedelta(seconds=140)).isoformat(),
                "label": "Estimated position 2",
                "confidence": 0.71,
            },
            {
                "id": "loc-sim-03",
                "lat": 59.33720,
                "lon": 18.10380,
                "time_utc": (start + timedelta(seconds=220)).isoformat(),
                "label": "Estimated position 3",
                "confidence": 0.58,
            },
        ],
    }


def cmd_upload(base_url: str, ca_file: str, token: str, ttl_seconds: int) -> int:
    start = datetime.now(UTC).replace(microsecond=0)
    created: list[dict[str, Any]] = []
    for layer_name, content in (
        ("helicopter_route", helicopter_route_content(start=start)),
        ("helicopter_analysis", helicopter_analysis_content(start=start)),
    ):
        body = request_json(
            base_url,
            ca_file,
            token,
            "POST",
            "/api/v1/analysis/scratch",
            {
                "namespace": NAMESPACE,
                "content": content,
                "ttl_seconds": ttl_seconds,
            },
        )
        if not isinstance(body, dict) or "id" not in body:
            raise DemoError(f"unexpected create response for {layer_name}")
        created.append(
            {
                "layer": layer_name,
                "id": body["id"],
                "title": content["title"],
                "expires_at": body.get("expires_at"),
            }
        )
        print(
            f"created {layer_name}: id={body['id']} "
            f"ttl_remaining_s={body.get('ttl_remaining_s')} "
            f"title={content['title']!r}"
        )

    if len(created) != 2:
        raise DemoError("expected exactly two scratch objects")
    print(json.dumps({"namespace": NAMESPACE, "dataset": DATASET, "objects": created}, indent=2))
    print(
        "Select dataset 'Helicopter' on the Hub map. "
        f"Clear with: uv run python scripts/{Path(__file__).name} clear"
    )
    return 0


def cmd_clear(base_url: str, ca_file: str, token: str) -> int:
    result = request_json(
        base_url,
        ca_file,
        token,
        "DELETE",
        f"/api/v1/analysis/scratch?{urllib.parse.urlencode({'namespace': NAMESPACE})}",
    )
    deleted = result.get("deleted") if isinstance(result, dict) else None
    print(f"cleared namespace {NAMESPACE!r}: deleted={deleted}")
    return 0


def cmd_list(base_url: str, ca_file: str, token: str) -> int:
    result = request_json(
        base_url,
        ca_file,
        token,
        "GET",
        "/api/v1/analysis/scratch?"
        + urllib.parse.urlencode(
            {"namespace": NAMESPACE, "include_content": "false"}
        ),
    )
    print(json.dumps(result, indent=2, default=str))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Create or clear the Helicopter map-demo Analysis scratch layers "
            f"(namespace {NAMESPACE})."
        )
    )
    add_hub_connection_arguments(parser)
    parser.add_argument(
        "--ttl-seconds",
        type=int,
        default=DEFAULT_TTL_S,
        help=f"Scratch TTL in seconds (default {DEFAULT_TTL_S}, max 7200)",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("upload", help="Create the two Helicopter scratch objects")
    sub.add_parser("clear", help=f"Delete all scratch objects in namespace {NAMESPACE}")
    sub.add_parser("list", help="List map-demo scratch metadata (no content bodies)")
    return parser


def main(argv: list[str] | None = None) -> int:
    load_project_env()
    parser = build_parser()
    if argv is None and print_help_without_arguments(parser):
        return 0
    args = parser.parse_args(argv)
    base_url, token = validate_hub_connection(args, parser)
    try:
        if args.command == "upload":
            return cmd_upload(base_url, args.ca, token, args.ttl_seconds)
        if args.command == "clear":
            return cmd_clear(base_url, args.ca, token)
        if args.command == "list":
            return cmd_list(base_url, args.ca, token)
        parser.error(f"unknown command {args.command}")
    except DemoError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
