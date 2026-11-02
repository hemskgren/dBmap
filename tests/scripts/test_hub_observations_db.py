import sys
from datetime import datetime, timedelta, timezone
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

spec = spec_from_file_location(
    "dbmap_hub_observations_db",
    Path(__file__).parents[2] / "scripts" / "hub_observations_db.py",
)
assert spec is not None and spec.loader is not None
hub_observations_db = module_from_spec(spec)
spec.loader.exec_module(hub_observations_db)


def test_prune_defaults_to_four_hours() -> None:
    parser = hub_observations_db.build_parser()
    args = parser.parse_args(["prune"])
    assert args.older_than_hours == 4.0


def test_wipe_requires_yes() -> None:
    parser = hub_observations_db.build_parser()
    args = parser.parse_args(["wipe-device-data"])
    assert args.yes is False


def test_prune_rejects_negative_hours(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_observations_db,
        "_run_python_in_hub",
        lambda *args, **kwargs: pytest.fail("must not touch docker"),
    )
    with pytest.raises(ValueError, match="at least zero"):
        hub_observations_db.prune_observations(older_than_hours=-1)


def test_prune_zero_hours_clears_all(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_observations_db,
        "clear_all_observations",
        lambda: {"deleted": 12, "remaining": 0},
    )
    result = hub_observations_db.prune_observations(older_than_hours=0)
    assert result == {
        "deleted": 12,
        "remaining": 0,
        "older_than_hours": 0.0,
        "cutoff_utc": "all",
    }


def test_parse_stored_utc_accepts_sqlalchemy_space_format() -> None:
    parsed = hub_observations_db.parse_stored_utc("2026-11-01 18:30:00.123456")
    assert parsed == datetime(2026, 11, 1, 18, 30, 0, 123456, tzinfo=timezone.utc)


def test_prune_does_not_delete_fresh_rows_with_space_separated_timestamps() -> None:
    now = datetime(2026, 11, 1, 18, 32, tzinfo=timezone.utc)
    cutoff = now - timedelta(hours=1)
    rows = [
        ("fresh", now.strftime("%Y-%m-%d %H:%M:%S.%f")),
        ("old", (now - timedelta(hours=2)).strftime("%Y-%m-%d %H:%M:%S.%f")),
        ("iso-fresh", now.isoformat()),
    ]
    # Lexicographic SQL would incorrectly mark "fresh" as older than an ISO cutoff.
    assert rows[0][1] < cutoff.isoformat()

    doomed = hub_observations_db.prune_observation_ids(rows, cutoff=cutoff)
    assert doomed == ["old"]
