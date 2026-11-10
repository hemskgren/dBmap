"""Shared local configuration and connection defaults for Hub command-line tools."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from datetime import UTC, datetime
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

DEFAULT_DISPLAY_TIMEZONE = "Europe/Stockholm"


class HubCliError(ValueError):
    pass


def resolve_display_timezone(name: str | None = None) -> ZoneInfo:
    """Return the display timezone; invalid/missing names fall back to Stockholm."""
    raw = (
        name
        if name is not None
        else os.environ.get("DBMAP_DISPLAY_TIMEZONE", DEFAULT_DISPLAY_TIMEZONE)
    )
    if not isinstance(raw, str) or not raw.strip():
        raw = DEFAULT_DISPLAY_TIMEZONE
    candidate = raw.strip()
    try:
        return ZoneInfo(candidate)
    except ZoneInfoNotFoundError:
        if candidate == DEFAULT_DISPLAY_TIMEZONE:
            raise
        return ZoneInfo(DEFAULT_DISPLAY_TIMEZONE)


def _coerce_display_datetime(value: datetime | str | None) -> datetime | None:
    if value is None:
        return None
    if isinstance(value, datetime):
        parsed = value
    elif isinstance(value, str):
        text = value.strip()
        if not text:
            return None
        try:
            parsed = datetime.fromisoformat(text)
        except ValueError:
            return None
    else:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        parsed = parsed.replace(tzinfo=UTC)
    return parsed


def format_display_datetime(
    value: datetime | str | None,
    *,
    empty: str = "unknown",
    invalid: str | None = None,
    timezone_name: str | None = None,
) -> str:
    """Format a timestamp for human CLI/web-aligned display.

    Always ``YYYY-MM-DD HH:MM:SS <tzname>`` in ``Europe/Stockholm`` by default
    (CET/CEST chosen automatically for the displayed instant). Machine-readable
    API/MQTT/DB values are not modified by this helper.
    """
    if value is None or (isinstance(value, str) and not value.strip()):
        return empty
    parsed = _coerce_display_datetime(value)
    if parsed is None:
        if invalid is not None:
            return invalid
        return str(value)
    zone = resolve_display_timezone(timezone_name)
    local = parsed.astimezone(zone)
    return local.strftime("%Y-%m-%d %H:%M:%S %Z")


def format_display_now(*, timezone_name: str | None = None) -> str:
    return format_display_datetime(
        datetime.now(tz=UTC),
        timezone_name=timezone_name,
    )


def summarize_hub_http_error_body(body: bytes | str, *, max_chars: int = 160) -> str:
    """Turn an HTTP error body into a single short CLI-safe phrase.

    Nginx HTML maintenance pages stay friendly in the browser; CLI tools should
    not dump that markup. Prefer JSON ``detail`` when the Hub API answered.
    """
    if isinstance(body, bytes):
        text = body.decode("utf-8", errors="replace").strip()
    else:
        text = str(body).strip()
    if not text:
        return ""

    lowered = text.lstrip().lower()
    if lowered.startswith(("<!doctype html", "<html")):
        return "Hub temporarily unavailable"

    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        payload = None
    if isinstance(payload, dict):
        detail = payload.get("detail")
        if isinstance(detail, str) and detail.strip():
            text = detail.strip()
        elif isinstance(detail, list):
            parts: list[str] = []
            for item in detail:
                if isinstance(item, dict):
                    msg = item.get("msg")
                    if isinstance(msg, str) and msg.strip():
                        parts.append(msg.strip())
                elif isinstance(item, str) and item.strip():
                    parts.append(item.strip())
            text = "; ".join(parts) if parts else json.dumps(detail, separators=(",", ":"))
        elif detail is not None:
            text = str(detail)

    one_line = " ".join(text.split())
    if len(one_line) > max_chars:
        return one_line[: max_chars - 1] + "…"
    return one_line


def format_hub_http_error(
    code: int,
    body: bytes | str = b"",
    *,
    path: str | None = None,
) -> str:
    """Single-line Hub API HTTP failure message for CLI stderr."""
    where = f"hub API {path}" if path else "hub API"
    summary = summarize_hub_http_error_body(body)
    if summary:
        return f"{where} returned HTTP {code}: {summary}"
    return f"{where} returned HTTP {code}"


def print_help_without_arguments(parser: argparse.ArgumentParser) -> bool:
    if len(sys.argv) == 1:
        parser.print_help()
        return True
    return False


def load_project_env(project_root: Path | None = None) -> None:
    # scripts/lib/hub_cli.py → parents[2] is the project root (was parents[1] at scripts/_hub_cli.py).
    env_file = (project_root or Path(__file__).resolve().parents[2]) / ".env"
    try:
        lines = env_file.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return
    except OSError as exc:
        raise HubCliError(
            f"could not read project environment file {env_file}: {exc}"
        ) from exc

    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        key, separator, value = stripped.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        os.environ.setdefault(key, value)


def add_hub_connection_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--hub-host",
        default=os.environ.get("DBMAP_LAN_IP"),
        help="Hub LAN IP or certificate-valid hostname (defaults to project .env)",
    )
    parser.add_argument(
        "--hub-port",
        type=int,
        default=int(os.environ.get("DBMAP_HTTPS_PORT", "8443")),
        help="HTTPS listener port (default: 8443 or DBMAP_HTTPS_PORT)",
    )
    parser.add_argument(
        "--base-path",
        default=os.environ.get("DBMAP_PUBLIC_BASE_PATH", ""),
        help="Optional public path prefix (defaults to project .env)",
    )
    parser.add_argument(
        "--ca",
        default=os.environ.get("DBMAP_CA_FILE", "shared/trust/ca.crt"),
        help="Path to the local Hub CA (default: shared/trust/ca.crt)",
    )


def validate_hub_connection(
    args: argparse.Namespace, parser: argparse.ArgumentParser
) -> tuple[str, str]:
    if not args.hub_host:
        parser.error(
            "Hub address not configured; set DBMAP_LAN_IP in the project .env file"
        )
    base_path = args.base_path.rstrip("/")
    if base_path and (
        not base_path.startswith("/")
        or "//" in base_path
        or any(part in {".", ".."} for part in base_path.split("/"))
    ):
        parser.error("--base-path must be empty or a normalized absolute path")

    admin_token = os.environ.get("DBMAP_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        parser.error("DBMAP_ADMIN_TOKEN must be configured in the project .env file")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")
    return f"https://{args.hub_host}:{args.hub_port}{base_path}", admin_token
