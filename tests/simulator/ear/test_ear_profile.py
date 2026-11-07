"""Tests for reusable EAR selection profiles."""

from __future__ import annotations

import json
import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import pytest

_ROOT = Path(__file__).parents[3]
_EAR = _ROOT / "simulator" / "ear"
_PROFILES = _ROOT / "shared" / "simulator" / "ear-profiles"
HELICOPTER = _EAR / "scenarios" / "helicopter_east_west.json"

if str(_EAR) not in sys.path:
    sys.path.insert(0, str(_EAR))


def _load(name: str, filename: str):
    spec = spec_from_file_location(name, _EAR / filename)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    # Required so dataclasses can resolve annotations on the module.
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sim_ear_slots = _load("dbmap_sim_ear_slots", "sim_ear_slots.py")
ear_profile = _load("dbmap_ear_profile", "ear_profile.py")
simulate_scenario = _load("dbmap_ear_scenario_simulator", "simulate_scenario.py")
simulate_keepalive = _load("dbmap_ear_keepalive", "simulate_keepalive.py")

INITIAL_PROFILES = ("abc", "ade", "abde", "abcde", "abcdefg")


def test_initial_profiles_load_and_map_slots() -> None:
    expected = {
        "abc": {"ear_a": "a", "ear_b": "b", "ear_c": "c"},
        "ade": {"ear_a": "a", "ear_d": "d", "ear_e": "e"},
        "abde": {"ear_a": "a", "ear_b": "b", "ear_d": "d", "ear_e": "e"},
        "abcde": {
            "ear_a": "a",
            "ear_b": "b",
            "ear_c": "c",
            "ear_d": "d",
            "ear_e": "e",
        },
        "abcdefg": {
            "ear_a": "a",
            "ear_b": "b",
            "ear_c": "c",
            "ear_d": "d",
            "ear_e": "e",
            "ear_f": "f",
            "ear_g": "g",
        },
    }
    for name in INITIAL_PROFILES:
        path = _PROFILES / f"{name}.json"
        profile = ear_profile.load_ear_profile(path)
        assert profile.name == name
        assert profile.ears == expected[name]
        assert list(profile.ears) == list(expected[name])


def test_credential_paths_resolve_through_sim_ear_slots(tmp_path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    sim_dir = tmp_path / "shared" / "simulator"
    sim_dir.mkdir(parents=True)
    for slot in ("a", "b", "c"):
        (sim_dir / f"sim-ear-{slot}.json").write_text("{}")
    profile = ear_profile.load_ear_profile(_PROFILES / "abc.json")
    paths = ear_profile.resolve_profile_credential_paths(profile, require_files=True)
    assert list(paths) == ["ear_a", "ear_b", "ear_c"]
    for alias, slot in profile.ears.items():
        assert paths[alias] == Path(sim_ear_slots.credentials_file_for_ear(slot))


def test_missing_credential_files_raise_clear_error(tmp_path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    (tmp_path / "shared" / "simulator").mkdir(parents=True)
    profile = ear_profile.load_ear_profile(_PROFILES / "abc.json")
    with pytest.raises(ear_profile.EarProfileError, match="credential file\\(s\\) missing"):
        ear_profile.resolve_profile_credential_paths(profile, require_files=True)


def test_invalid_json_and_structure_rejected(tmp_path) -> None:
    bad_json = tmp_path / "bad.json"
    bad_json.write_text("{not json")
    with pytest.raises(ear_profile.EarProfileError, match="could not read"):
        ear_profile.load_ear_profile(bad_json)

    missing_name = tmp_path / "noname.json"
    missing_name.write_text(json.dumps({"ears": {"ear_a": "a"}}))
    with pytest.raises(ear_profile.EarProfileError, match="fields mismatch"):
        ear_profile.load_ear_profile(missing_name)

    extra = tmp_path / "extra.json"
    extra.write_text(
        json.dumps({"name": "x", "ears": {"ear_a": "a"}, "trajectory": {}})
    )
    with pytest.raises(ear_profile.EarProfileError, match="unexpected"):
        ear_profile.load_ear_profile(extra)


def test_duplicate_alias_and_slot_rejected(tmp_path) -> None:
    # JSON object keys are unique; simulate duplicate slot across aliases.
    dup_slot = tmp_path / "dup_slot.json"
    dup_slot.write_text(
        json.dumps({"name": "dup", "ears": {"ear_a": "a", "ear_b": "a"}})
    )
    with pytest.raises(ear_profile.EarProfileError, match="duplicate slot"):
        ear_profile.load_ear_profile(dup_slot)


def test_unsupported_slot_rejected(tmp_path) -> None:
    path = tmp_path / "bad_slot.json"
    path.write_text(json.dumps({"name": "x", "ears": {"ear_a": "z"}}))
    with pytest.raises(ear_profile.EarProfileError, match="unsupported"):
        ear_profile.load_ear_profile(path)


def test_scenario_rejects_mixing_ear_and_profile() -> None:
    args = SimpleNamespace(
        ear=["ear_a=shared/simulator/sim-ear-a.json"],
        ear_profile=str(_PROFILES / "abc.json"),
    )
    with pytest.raises(simulate_scenario.ScenarioError, match="not both"):
        simulate_scenario.resolve_scenario_ear_paths(args)


def test_scenario_legacy_ear_args_still_work() -> None:
    args = SimpleNamespace(
        ear=[
            "ear_a=shared/simulator/sim-ear-a.json",
            "ear_b=shared/simulator/sim-ear-b.json",
        ],
        ear_profile=None,
    )
    paths = simulate_scenario.resolve_scenario_ear_paths(args)
    assert paths == {
        "ear_a": Path("shared/simulator/sim-ear-a.json"),
        "ear_b": Path("shared/simulator/sim-ear-b.json"),
    }


def test_scenario_profile_resolution(tmp_path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    sim_dir = tmp_path / "shared" / "simulator"
    sim_dir.mkdir(parents=True)
    for slot in ("a", "b", "c"):
        (sim_dir / f"sim-ear-{slot}.json").write_text("{}")
    args = SimpleNamespace(ear=[], ear_profile=str(_PROFILES / "abc.json"))
    paths = simulate_scenario.resolve_scenario_ear_paths(args)
    assert list(paths) == ["ear_a", "ear_b", "ear_c"]
    assert paths["ear_a"] == Path("shared/simulator/sim-ear-a.json")


def test_keepalive_profile_uses_slots(tmp_path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    sim_dir = tmp_path / "shared" / "simulator"
    sim_dir.mkdir(parents=True)
    for slot in ("a", "d", "e"):
        (sim_dir / f"sim-ear-{slot}.json").write_text("{}")
    monkeypatch.setattr(
        simulate_keepalive.sys,
        "argv",
        ["simulate_keepalive.py", "once", "--ear-profile", str(_PROFILES / "ade.json")],
    )
    args = SimpleNamespace(
        ear="a",
        ear_profile=str(_PROFILES / "ade.json"),
        credentials_file=None,
    )
    paths = simulate_keepalive.resolve_credentials_files(args)
    assert paths == [
        "shared/simulator/sim-ear-a.json",
        "shared/simulator/sim-ear-d.json",
        "shared/simulator/sim-ear-e.json",
    ]


def test_keepalive_rejects_profile_with_explicit_ear(monkeypatch) -> None:
    monkeypatch.setattr(
        simulate_keepalive.sys,
        "argv",
        [
            "simulate_keepalive.py",
            "once",
            "--ear",
            "b",
            "--ear-profile",
            str(_PROFILES / "abc.json"),
        ],
    )
    args = SimpleNamespace(
        ear="b",
        ear_profile=str(_PROFILES / "abc.json"),
        credentials_file=None,
    )
    with pytest.raises(ValueError, match="cannot be combined with --ear"):
        simulate_keepalive.resolve_credentials_files(args)


def test_helicopter_scenario_unchanged() -> None:
    before = HELICOPTER.read_bytes()
    ear_profile.load_ear_profile(_PROFILES / "abc.json")
    assert HELICOPTER.read_bytes() == before
    scenario = json.loads(before)
    assert set(scenario["ears"]) == {"ear_a", "ear_b", "ear_c"}
    assert "ear_d" not in scenario["ears"]
