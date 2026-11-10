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

from lib.hub_cli import (
    add_hub_connection_arguments,
    format_display_datetime,
    format_display_now,
    format_hub_http_error,
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
        raise EarAudioCompareError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise EarAudioCompareError(
            f"could not reach the hub API: {exc.reason}"
        ) from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EarAudioCompareError(f"hub returned invalid JSON for {path}") from exc


def is_ear_node(node: dict[str, Any]) -> bool:
    return str(node.get("node_type", "")).lower() == "ear"


def is_online_node(node: dict[str, Any]) -> bool:
    return str(node.get("status", "")).lower() == "online"


def select_ear_nodes(
    nodes: list[dict[str, Any]],
    *,
    online_only: bool = False,
) -> list[dict[str, Any]]:
    ears = [node for node in nodes if is_ear_node(node)]
    if online_only:
        ears = [node for node in ears if is_online_node(node)]
    return sorted(ears, key=lambda item: str(item.get("node_id", "")))


def _reported_payload(node: dict[str, Any]) -> dict[str, Any]:
    reported = node.get("reported")
    if not isinstance(reported, dict):
        return {}
    payload = reported.get("payload")
    return payload if isinstance(payload, dict) else {}


def _audio_block(node: dict[str, Any]) -> dict[str, Any]:
    audio = _reported_payload(node).get("audio")
    return audio if isinstance(audio, dict) else {}


def _microphones(audio: dict[str, Any]) -> list[dict[str, Any]]:
    """Return mic objects sorted by index. Empty dict placeholder if none."""
    mics = audio.get("microphones")
    if not isinstance(mics, list):
        return [{}]
    items = [item for item in mics if isinstance(item, dict)]
    if not items:
        return [{}]
    return sorted(
        items,
        key=lambda item: (
            item.get("index") is None,
            int(item["index"]) if isinstance(item.get("index"), (int, float)) else 10**9,
            str(item.get("index", "")),
        ),
    )


def _mic_id(mic: dict[str, Any]) -> str:
    index = mic.get("index")
    if isinstance(index, bool) or index is None:
        return "—"
    if isinstance(index, (int, float)):
        return str(int(index))
    return "—"


def _format_number(value: object) -> str:
    if isinstance(value, bool) or value is None:
        return "—"
    if isinstance(value, (int, float)):
        if isinstance(value, float):
            return f"{value:.1f}"
        return str(value)
    return "—"


def _format_dbfs(value: object) -> str:
    """Format a level field to one decimal place (int or float)."""
    if isinstance(value, bool) or value is None:
        return "—"
    if isinstance(value, (int, float)):
        return f"{float(value):.1f}"
    return "—"


def _json_number(value: object) -> float | int | None:
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, float):
        return value
    if isinstance(value, int):
        return value
    return None


def _background_block(mic: dict[str, Any]) -> dict[str, Any]:
    background = mic.get("background")
    return background if isinstance(background, dict) else {}


def _transport_status(audio: dict[str, Any], mic: dict[str, Any]) -> str:
    return str(
        audio.get("audio_transport_status")
        or audio.get("subsystem_status")
        or mic.get("status")
        or "—"
    )


def _microphone_status(audio: dict[str, Any], mic: dict[str, Any]) -> str:
    """Prefer per-mic status; fall back to audio-level for older payloads."""
    mic_status = mic.get("microphone_status")
    if mic_status:
        return str(mic_status)
    return str(audio.get("microphone_status") or "—")


def _table_lines(rows: list[tuple[str, ...]]) -> list[str]:
    widths = [max(len(row[index]) for row in rows) for index in range(len(rows[0]))]
    return [
        "  ".join(cell.ljust(widths[index]) for index, cell in enumerate(row))
        for row in rows
    ]


def format_local_timestamp(value: Any) -> str:
    """Human Stockholm time at second precision (for terminal tables, not --json)."""
    if isinstance(value, str) and value.strip().lower() == "never":
        return "never"
    return format_display_datetime(value, empty="never", invalid="never")


def format_now_local_timestamp(now: datetime | None = None) -> str:
    if now is None:
        return format_display_now()
    return format_display_datetime(now)


def ear_audio_list_rows(node: dict[str, Any]) -> list[tuple[str, ...]]:
    """One list-table row per microphone (secondary mics omit Device/Status)."""
    audio = _audio_block(node)
    mics = _microphones(audio)
    rows: list[tuple[str, ...]] = []
    for offset, mic in enumerate(mics):
        first = offset == 0
        # Keep MicId plain so "1" lines up under "0"; nesting is empty Device/Status.
        rows.append(
            (
                str(node.get("node_id", "")) if first else "",
                str(node.get("status", "offline")) if first else "",
                _mic_id(mic),
                _transport_status(audio, mic),
                _microphone_status(audio, mic),
                str(mic.get("classification_reason") or "—"),
                _format_dbfs(mic.get("rms_dbfs")),
                _format_dbfs(mic.get("peak_dbfs")),
                _format_number(mic.get("sample_count")),
                _format_number(mic.get("expected_sample_count")),
                _format_number(mic.get("timeout_reads")),
                _format_number(mic.get("empty_reads")),
                _format_number(mic.get("read_errors")),
                format_local_timestamp(node.get("last_seen_at")) if first else "",
            )
        )
    return rows


def ear_audio_watch_rows(node: dict[str, Any]) -> list[tuple[str, ...]]:
    """Watch rows: first mic full; later mics omit Device/Status (MicId aligned)."""
    audio = _audio_block(node)
    mics = _microphones(audio)
    rows: list[tuple[str, ...]] = []
    for offset, mic in enumerate(mics):
        first = offset == 0
        background = _background_block(mic)
        rows.append(
            (
                str(node.get("node_id", "")) if first else "",
                str(node.get("status", "offline")) if first else "",
                _mic_id(mic),
                _microphone_status(audio, mic),
                _format_number(mic.get("window_ms")),
                _format_dbfs(mic.get("rms_dbfs")),
                _format_dbfs(mic.get("peak_dbfs")),
                _format_dbfs(background.get("rms_min_dbfs")),
                _format_dbfs(background.get("rms_mean_dbfs")),
                _format_dbfs(background.get("rms_max_dbfs")),
                _format_number(background.get("window_ms")),
                _format_number(mic.get("sample_count")),
                _format_number(mic.get("timeout_reads")),
                _format_number(mic.get("empty_reads")),
                _format_number(mic.get("read_errors")),
                format_local_timestamp(node.get("last_seen_at")) if first else "",
            )
        )
    return rows


def ear_audio_json_records(
    nodes: list[dict[str, Any]],
    *,
    online_only: bool = False,
) -> list[dict[str, Any]]:
    """Structured Ear audio snapshot for ``list --json`` (one object per device)."""
    records: list[dict[str, Any]] = []
    for node in select_ear_nodes(nodes, online_only=online_only):
        audio = _audio_block(node)
        mics_out: list[dict[str, Any]] = []
        for mic in _microphones(audio):
            if not mic:
                continue
            background = _background_block(mic)
            entry: dict[str, Any] = {
                "index": _json_number(mic.get("index")),
                "microphone_status": mic.get("microphone_status"),
                "classification_reason": mic.get("classification_reason"),
                "window_ms": _json_number(mic.get("window_ms")),
                "rms_dbfs": _json_number(mic.get("rms_dbfs")),
                "peak_dbfs": _json_number(mic.get("peak_dbfs")),
                "sample_count": _json_number(mic.get("sample_count")),
                "expected_sample_count": _json_number(mic.get("expected_sample_count")),
                "timeout_reads": _json_number(mic.get("timeout_reads")),
                "empty_reads": _json_number(mic.get("empty_reads")),
                "read_errors": _json_number(mic.get("read_errors")),
            }
            if background:
                entry["background"] = {
                    "window_ms": _json_number(background.get("window_ms")),
                    "sample_count": _json_number(background.get("sample_count")),
                    "status": background.get("status"),
                    "rms_min_dbfs": _json_number(background.get("rms_min_dbfs")),
                    "rms_mean_dbfs": _json_number(background.get("rms_mean_dbfs")),
                    "rms_max_dbfs": _json_number(background.get("rms_max_dbfs")),
                }
            mics_out.append(entry)
        records.append(
            {
                "node_id": node.get("node_id"),
                "status": node.get("status"),
                "last_seen_at": node.get("last_seen_at"),
                "audio_transport_status": audio.get("audio_transport_status"),
                "microphone_status": audio.get("microphone_status"),
                "microphones": mics_out,
            }
        )
    return records


def format_ear_audio_list_json(
    nodes: list[dict[str, Any]],
    *,
    online_only: bool = False,
) -> str:
    return json.dumps(
        ear_audio_json_records(nodes, online_only=online_only),
        indent=2,
        sort_keys=False,
    )


def format_ear_audio_watch_legend() -> str:
    """Watch legend printed once at script start (before the first snapshot)."""
    return (
        "RMS/Peak = latest analysis window (Win_ms) per mic, not the interval between "
        "Hub last_seen reports. BgMin/Mean/Max = that mic's rolling background hist "
        "over BgWin_ms (when present).\n"
        "Timeout/Empty/Errors = I2S read counters for that window. "
        "Additional mics omit Device/Status (MicId stays aligned). "
        "Per-device/mic only; dBFS FS, not SPL."
    )


def format_ear_audio_watch(
    nodes: list[dict[str, Any]],
    *,
    online_only: bool = False,
) -> str:
    """Compact multi-Ear snapshot for ``watch`` (all Ears in one block).

    RMS/Peak are each mic's latest completed analysis window (``window_ms``),
    not an average over the Hub last_seen reporting gap. Background min/mean/max
    are that mic's own rolling hist — never combined across devices or mics.
    """
    ears = select_ear_nodes(nodes, online_only=online_only)
    headers = (
        "Device",
        "Status",
        "MicId",
        "Mic",
        "Win_ms",
        "RMS",
        "Peak",
        "BgMin",
        "BgMean",
        "BgMax",
        "BgWin_ms",
        "Samples",
        "Timeout",
        "Empty",
        "Errors",
        "Last seen",
    )
    rows: list[tuple[str, ...]] = [headers]
    for node in ears:
        rows.extend(ear_audio_watch_rows(node))
    if len(rows) == 1:
        return "No online Ear nodes." if online_only else "No Ear nodes registered."
    return "\n".join(_table_lines(rows))


def format_ear_audio_fetch_banner(now: datetime | None = None) -> str:
    return f"Fetched at {format_now_local_timestamp(now)}"


def format_ear_audio_list_legend() -> str:
    """List legend printed above the table (same placement idea as watch)."""
    return (
        "Transport = I2S fill health; Mic = structure-based presence "
        "(noise_suspected for floating DIN; disconnected_suspected for digital silence).\n"
        "MicId = microphone index (aligned in column). Additional mics omit "
        "Device/Status/Last seen. Levels are dBFS, not SPL."
    )


def format_ear_audio_list(
    nodes: list[dict[str, Any]],
    *,
    online_only: bool = False,
) -> str:
    """Format Ear status + mic levels like hub_admin list."""
    ears = select_ear_nodes(nodes, online_only=online_only)
    headers = (
        "Device",
        "Status",
        "MicId",
        "Transport",
        "Mic",
        "Reason",
        "RMS dBFS",
        "Peak dBFS",
        "Samples",
        "Expected",
        "Timeout",
        "Empty",
        "Errors",
        "Last seen",
    )
    rows: list[tuple[str, ...]] = [headers]
    for node in ears:
        rows.extend(ear_audio_list_rows(node))
    if len(rows) == 1:
        return "No online Ear nodes." if online_only else "No Ear nodes registered."
    return "\n".join(
        [
            format_ear_audio_list_legend(),
            format_ear_audio_fetch_banner(),
            "",
            *_table_lines(rows),
        ]
    )


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
    list_command = commands.add_parser(
        "list",
        help="Show Ear status and mic RMS/peak once",
    )
    list_command.add_argument(
        "--online-only",
        action="store_true",
        help="Show only Ear nodes with Hub status online",
    )
    list_command.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON (one object per Ear, all mics)",
    )
    watch_command = commands.add_parser(
        "watch",
        help=(
            "Re-fetch a compact multi-Ear RMS/peak snapshot "
            "(latest analysis window + background hist)"
        ),
    )
    watch_command.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval (default: 15)",
    )
    watch_command.add_argument(
        "--online-only",
        action="store_true",
        help="Show only Ear nodes with Hub status online",
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
    online_only = bool(getattr(args, "online_only", False))
    as_json = bool(getattr(args, "json", False))
    try:
        nodes = fetch_nodes(base_url, args.ca, admin_token)
        if watching:
            print(format_ear_audio_watch_legend(), flush=True)
            print(format_ear_audio_fetch_banner(), flush=True)
            print(flush=True)
            print(
                format_ear_audio_watch(nodes, online_only=online_only),
                flush=True,
            )
            print(
                f"\nWatching Ear mic levels every {args.interval_seconds:g}s"
                f"{' (online only)' if online_only else ''}; "
                "press Ctrl+C to stop.",
                flush=True,
            )
            while True:
                time.sleep(args.interval_seconds)
                nodes = fetch_nodes(base_url, args.ca, admin_token)
                print(f"\n[{format_now_local_timestamp()}]", flush=True)
                print(
                    format_ear_audio_watch(nodes, online_only=online_only),
                    flush=True,
                )
            return 0
        if as_json:
            # Keep Hub timestamps as returned; do not reformat for --json.
            print(format_ear_audio_list_json(nodes, online_only=online_only), flush=True)
        else:
            print(format_ear_audio_list(nodes, online_only=online_only), flush=True)
        return 0
    except KeyboardInterrupt:
        print("\nStopped watching.")
        return 0
    except (OSError, EarAudioCompareError) as exc:
        print(f"Ear audio compare failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
