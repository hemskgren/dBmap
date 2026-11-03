"""Summarize recent Mosquitto connection failures from the host-mounted broker log."""

from __future__ import annotations

import re
from datetime import datetime, timedelta, timezone
from pathlib import Path

from app.config import settings

FAILURE_WINDOW_HOURS = 3
MAX_LINES = 200

# Mosquitto log_timestamp_format %d/%b/%Y %H:%M:%S (container local time; image uses UTC).
_LINE_RE = re.compile(
    r"^(?P<stamp>\d{2}/[A-Za-z]{3}/\d{4} \d{2}:\d{2}:\d{2}): (?P<body>.*)$"
)

# Auth, TLS/CA, protocol, and other disconnect failures — not clean client closes.
_FAILURE_RE = re.compile(
    r"(?i)("
    r"not authorised|"
    r"bad username or password|"
    r"OpenSSL Error|"
    r"tlsv1 alert|"
    r"unknown ca|"
    r"certificate verify|"
    r"certificate|"
    r"Protocol error|"
    r"protocol error|"
    r"Socket error|"
    r"Client connection from .+ failed|"
    r"disconnected: (?!connection closed by client)"
    r")"
)

_FAIL2BAN_NOTICE = (
    "Host fail2ban (if enabled for MQTT) drops banned clients before Mosquitto, "
    "so those attempts never appear in this list."
)


def _parse_stamp(raw: str) -> datetime | None:
    try:
        # Treat broker timestamps as UTC (eclipse-mosquitto default container TZ).
        return datetime.strptime(raw, "%d/%b/%Y %H:%M:%S").replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def is_failure_line(body: str) -> bool:
    return _FAILURE_RE.search(body) is not None


def filter_failure_lines(
    lines: list[str],
    *,
    now: datetime | None = None,
    window_hours: int = FAILURE_WINDOW_HOURS,
) -> list[str]:
    """Keep failure lines whose timestamp falls inside the window (when parseable)."""
    cutoff = (now or datetime.now(timezone.utc)) - timedelta(hours=window_hours)
    matched: list[str] = []
    for line in lines:
        text = line.rstrip("\n")
        if not text:
            continue
        parsed = _LINE_RE.match(text)
        if parsed is None:
            if is_failure_line(text):
                matched.append(text)
            continue
        body = parsed.group("body")
        if not is_failure_line(body):
            continue
        stamp = _parse_stamp(parsed.group("stamp"))
        if stamp is not None and stamp < cutoff:
            continue
        matched.append(text)
    return matched[-MAX_LINES:]


def read_recent_failures(
    *,
    window_hours: int = FAILURE_WINDOW_HOURS,
    log_path: str | None = None,
) -> dict:
    path = Path(log_path or settings.mosquitto_log_path)
    notices = [_FAIL2BAN_NOTICE]
    if not path.is_file():
        return {
            "service": "MQTT broker",
            "compose_service": "mosquitto",
            "window_hours": window_hours,
            "source": "unavailable",
            "lines": [],
            "count": 0,
            "notices": notices
            + [
                (
                    f"Mosquitto log file is not readable at {path}. "
                    "Ensure the Hub mounts shared/mosquitto/logs and Mosquitto is writing it."
                )
            ],
        }

    try:
        # Bound read size so a huge log cannot blow memory; keep the trailing chunk.
        data = path.read_bytes()
    except OSError as exc:
        return {
            "service": "MQTT broker",
            "compose_service": "mosquitto",
            "window_hours": window_hours,
            "source": "unavailable",
            "lines": [],
            "count": 0,
            "notices": notices + [f"Could not read Mosquitto log: {exc}"],
        }

    max_bytes = 2_000_000
    if len(data) > max_bytes:
        data = data[-max_bytes:]
        # Drop a possible partial first line after the slice.
        newline = data.find(b"\n")
        if newline != -1:
            data = data[newline + 1 :]
        notices.append(
            "Log file is large; only the trailing portion was scanned for this window."
        )

    text = data.decode("utf-8", errors="replace")
    lines = filter_failure_lines(text.splitlines(), window_hours=window_hours)
    return {
        "service": "MQTT broker",
        "compose_service": "mosquitto",
        "window_hours": window_hours,
        "source": "file",
        "path": str(path),
        "lines": lines,
        "count": len(lines),
        "notices": notices,
    }
