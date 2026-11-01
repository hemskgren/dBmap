import sys
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


def test_prune_rejects_non_positive_hours(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_observations_db,
        "_run_python_in_hub",
        lambda *args, **kwargs: pytest.fail("must not touch docker"),
    )
    with pytest.raises(ValueError, match="greater than zero"):
        hub_observations_db.prune_observations(older_than_hours=0)
