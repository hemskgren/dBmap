#!/usr/bin/env python3
"""List Ear nodes with Hub status and latest reported mic levels (dBFS).

Same connection style as hub_admin.py (.env + CA). Ear-only; Output nodes are
skipped. Levels are digital full-scale, not calibrated SPL. Room differences
do not mean defective mics — expand with CSV/stats later if needed.
"""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from typing import Any

from _hub_cli import (
    add_hub_connection_arguments,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)


class EarAudioCompareError(RuntimeError):
    pass


def request_json(
    base_url: str,
    ca_file: str,
    admin_token: str,
    method: str,
    path: str,
) -> Any:
    request = urllib.request.Request(
        f"{base_url}{path}",
        headers={"Authorization": f"Bearer {admin_token}"},
        method=method,
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read(1024).decode(errors="replace")
        raise EarAudioCompareError(f"hub API returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise EarAudioCompareError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EarAudioCompareError(f"hub returned invalid JSON for {path}") from exc


def is_ear_node(node: dict[str, Any]) -> bool:
    return str(node.get("node_type", "")).lower() == "ear"


def _reported_payload(node: dict[str, Any]) -> dict[str, Any]:
    reported = node.get("reported")
    if not isinstance(reported, dict):
        return {}
    payload = reported.get("payload")
    return payload if isinstance(payload, dict) else {}


def _audio_block(node: dict[str, Any]) -> dict[str, Any]:
    audio = _reported_payload(node).get("audio")
    return audio if isinstance(audio, dict) else {}


def _primary_microphone(audio: dict[str, Any]) -> dict[str, Any]:
    mics = audio.get("microphones")
    if not isinstance(mics, list):
        return {}
    for item in mics:
        if isinstance(item, dict) and item.get("index", 0) == 0:
            return item
    for item in mics:
        if isinstance(item, dict):
            return item
    return {}


def _format_number(value: object) -> str:
    if isinstance(value, bool) or value is None:
        return "—"
    if isinstance(value, (int, float)):
        if isinstance(value, float):
            return f"{value:.1f}"
        return str(value)
    return "—"


def ear_audio_row(node: dict[str, Any]) -> tuple[str, ...]:
    audio = _audio_block(node)
    mic = _primary_microphone(audio)
    return (
        str(node.get("node_id", "")),
        str(node.get("status", "offline")),
        str(audio.get("subsystem_status") or "—"),
        str(mic.get("status") or "—"),
        _format_number(mic.get("rms_dbfs")),
        _format_number(mic.get("peak_dbfs")),
        _format_number(mic.get("sample_count")),
        str(node.get("last_seen_at") or "never"),
    )


def format_ear_audio_list(nodes: list[dict[str, Any]]) -> str:
    """Format Ear status + mic levels like hub_admin list."""
    ears = [node for node in nodes if is_ear_node(node)]
    headers = (
        "Device",
        "Status",
        "Audio",
        "Mic",
        "RMS dBFS",
        "Peak dBFS",
        "Samples",
        "Last seen",
    )
    rows: list[tuple[str, ...]] = [headers]
    for node in sorted(ears, key=lambda item: str(item.get("node_id", ""))):
        rows.append(ear_audio_row(node))
    if len(rows) == 1:
        return "No Ear nodes registered."
    widths = [max(len(row[index]) for row in rows) for index in range(len(headers))]
    lines = [
        "  ".join(cell.ljust(widths[index]) for index, cell in enumerate(row))
        for row in rows
    ]
    lines.append("")
    lines.append(
        "Levels are dBFS (digital full-scale), not SPL. "
        "Room/placement differences are expected."
    )
    return "\n".join(lines)


def parse_watch_interval(interval_seconds: float) -> float:
    if interval_seconds <= 0:
        raise ValueError("watch interval must be greater than zero")
    return interval_seconds


def fetch_nodes(base_url: str, ca_file: str, admin_token: str) -> list[dict[str, Any]]:
    nodes = request_json(base_url, ca_file, admin_token, "GET", "/api/v1/nodes")
    if not isinstance(nodes, list) or not all(isinstance(node, dict) for node in nodes):
        raise EarAudioCompareError("hub returned an invalid node list")
    return nodes


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Compare Ear device status and latest mic levels from Hub reported state. "
            "Type EAR only."
        )
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser(
        "list",
        help="Show Ear status and mic RMS/peak once",
    )
    watch_command = commands.add_parser(
        "watch",
        help="Re-fetch and reprint Ear status and mic levels",
    )
    watch_command.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval (default: 15)",
    )
    return parser


def main() -> int:
    try:
        load_project_env()
    except ValueError as exc:
        print(f"Hub configuration failed: {exc}", file=sys.stderr)
        return 1
    parser = build_parser()
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    base_url, admin_token = validate_hub_connection(args, parser)
    watching = args.command == "watch"
    if watching:
        try:
            parse_watch_interval(args.interval_seconds)
        except ValueError as exc:
            parser.error(str(exc))
    try:
        nodes = fetch_nodes(base_url, args.ca, admin_token)
        print(format_ear_audio_list(nodes), flush=True)
        if not watching:
            return 0
        print(
            f"\nWatching Ear mic levels every {args.interval_seconds:g}s; "
            "press Ctrl+C to stop.",
            flush=True,
        )
        while True:
            time.sleep(args.interval_seconds)
            nodes = fetch_nodes(base_url, args.ca, admin_token)
            timestamp = datetime.now().astimezone().strftime("%d/%m/%Y %H:%M:%S %Z")
            print(f"\n[{timestamp}]", flush=True)
            print(format_ear_audio_list(nodes), flush=True)
    except KeyboardInterrupt:
        print("\nStopped watching.")
        return 0
    except (OSError, EarAudioCompareError) as exc:
        print(f"Ear audio compare failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
