"""Presentation-layer display datetime helpers (Europe/Stockholm, YYYY-MM-DD)."""

from __future__ import annotations

from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from zoneinfo import ZoneInfo

import pytest

SCRIPT_DIRECTORY = Path(__file__).parents[2] / "scripts"
_hub_cli_spec = spec_from_file_location(
    "dbmap_hub_cli_display_time",
    SCRIPT_DIRECTORY / "lib" / "hub_cli.py",
)
assert _hub_cli_spec is not None and _hub_cli_spec.loader is not None
hub_cli = module_from_spec(_hub_cli_spec)
_hub_cli_spec.loader.exec_module(hub_cli)


@pytest.fixture(autouse=True)
def _clear_display_timezone_env(monkeypatch):
    monkeypatch.delenv("DBMAP_DISPLAY_TIMEZONE", raising=False)


def test_format_display_datetime_uses_yyyy_mm_dd_order() -> None:
    stamp = datetime(2026, 11, 10, 8, 0, 1, tzinfo=UTC)
    text = hub_cli.format_display_datetime(stamp)
    assert text.startswith("2026-11-10 ")
    assert text.split(" ", 1)[0].count("-") == 2
    assert "/" not in text.split(" ", 1)[0]


def test_format_display_datetime_stockholm_winter_cet() -> None:
    stamp = datetime(2026, 1, 15, 12, 0, 0, tzinfo=UTC)
    text = hub_cli.format_display_datetime(stamp)
    assert text == "2026-01-15 13:00:00 CET"
    assert ZoneInfo("Europe/Stockholm").utcoffset(stamp).total_seconds() == 3600


def test_format_display_datetime_stockholm_summer_cest() -> None:
    stamp = datetime(2026, 7, 15, 12, 0, 0, tzinfo=UTC)
    text = hub_cli.format_display_datetime(stamp)
    assert text == "2026-07-15 14:00:00 CEST"
    assert ZoneInfo("Europe/Stockholm").utcoffset(stamp).total_seconds() == 7200


def test_format_display_datetime_missing_and_invalid_timezone_fallback(
    monkeypatch,
) -> None:
    stamp = datetime(2026, 1, 15, 12, 0, 0, tzinfo=UTC)
    monkeypatch.setenv("DBMAP_DISPLAY_TIMEZONE", "")
    assert hub_cli.format_display_datetime(stamp).endswith("CET")
    monkeypatch.setenv("DBMAP_DISPLAY_TIMEZONE", "Not/ARealZone")
    assert hub_cli.format_display_datetime(stamp) == "2026-01-15 13:00:00 CET"
    assert (
        hub_cli.format_display_datetime(stamp, timezone_name="Also/Invalid")
        == "2026-01-15 13:00:00 CET"
    )


def test_format_display_datetime_empty_and_invalid_inputs() -> None:
    assert hub_cli.format_display_datetime(None) == "unknown"
    assert hub_cli.format_display_datetime("") == "unknown"
    assert hub_cli.format_display_datetime(None, empty="never") == "never"
    assert hub_cli.format_display_datetime("not-a-time", invalid="bad") == "bad"


def test_machine_readable_iso_strings_are_not_rewritten_by_helper() -> None:
    """Helpers only format for display; callers keep raw ISO for JSON/API."""
    raw = "2026-11-10T08:00:01.456789+00:00"
    assert raw.endswith("+00:00")
    assert "T" in raw
    rendered = hub_cli.format_display_datetime(raw)
    assert rendered.startswith("2026-11-10 ")
    assert "T08:00:01" not in rendered
    assert raw != rendered


def test_resolve_display_timezone_defaults(monkeypatch) -> None:
    assert hub_cli.resolve_display_timezone(None).key == "Europe/Stockholm"
    monkeypatch.setenv("DBMAP_DISPLAY_TIMEZONE", "UTC")
    assert hub_cli.resolve_display_timezone(None).key == "UTC"
    assert hub_cli.resolve_display_timezone("").key == "Europe/Stockholm"
