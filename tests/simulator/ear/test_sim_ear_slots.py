"""Tests for shared sim Ear slots a–g."""

from __future__ import annotations

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from types import SimpleNamespace

import pytest

_EAR = Path(__file__).parents[3] / "simulator" / "ear"


def _load(name: str, filename: str):
    spec = spec_from_file_location(name, _EAR / filename)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sim_ear_slots = _load("dbmap_sim_ear_slots", "sim_ear_slots.py")
simulate_observation = _load("dbmap_ear_simulator", "simulate_observation.py")
simulate_keepalive = _load("dbmap_ear_keepalive", "simulate_keepalive.py")

HELICOPTER = (
    Path(__file__).parents[3]
    / "simulator"
    / "ear"
    / "scenarios"
    / "helicopter_east_west.json"
)


def test_shared_slots_are_a_through_g() -> None:
    assert sim_ear_slots.EAR_SLOTS == ("a", "b", "c", "d", "e", "f", "g")
    assert simulate_observation.EAR_SLOTS == sim_ear_slots.EAR_SLOTS
    assert simulate_keepalive.EAR_SLOTS == sim_ear_slots.EAR_SLOTS


def test_credentials_paths_for_new_slots() -> None:
    assert sim_ear_slots.credentials_file_for_ear("d") == "shared/simulator/sim-ear-d.json"
    assert sim_ear_slots.credentials_file_for_ear("e") == "shared/simulator/sim-ear-e.json"
    assert sim_ear_slots.credentials_file_for_ear("f") == "shared/simulator/sim-ear-f.json"
    assert sim_ear_slots.credentials_file_for_ear("g") == "shared/simulator/sim-ear-g.json"
    # Existing A–C paths unchanged.
    assert sim_ear_slots.credentials_file_for_ear("a") == "shared/simulator/sim-ear-a.json"
    assert sim_ear_slots.credentials_file_for_ear("b") == "shared/simulator/sim-ear-b.json"
    assert sim_ear_slots.credentials_file_for_ear("c") == "shared/simulator/sim-ear-c.json"


def test_observation_and_keepalive_resolve_d_g_consistently() -> None:
    for slot in ("d", "e", "f", "g"):
        obs = simulate_observation.resolve_credentials_file(
            SimpleNamespace(credentials_file=None, ear=slot)
        )
        keep = simulate_keepalive.resolve_credentials_files(
            SimpleNamespace(credentials_file=None, ear=slot)
        )
        assert obs == f"shared/simulator/sim-ear-{slot}.json"
        assert keep == [obs]


def test_unsupported_slot_rejected() -> None:
    with pytest.raises(sim_ear_slots.SimEarSlotError, match="unsupported"):
        sim_ear_slots.validate_ear_slot("h")
    with pytest.raises(sim_ear_slots.SimEarSlotError, match="unsupported"):
        sim_ear_slots.credentials_file_for_ear("z")


def test_expected_node_id_mapping() -> None:
    assert sim_ear_slots.expected_node_id_for_slot("a") == "SIM-EAR-001"
    assert sim_ear_slots.expected_node_id_for_slot("d") == "SIM-EAR-004"
    assert sim_ear_slots.expected_node_id_for_slot("g") == "SIM-EAR-007"
    sim_ear_slots.assert_node_id_matches_slot("d", "SIM-EAR-004")
    with pytest.raises(sim_ear_slots.SimEarSlotError, match="expects SIM-EAR-004"):
        sim_ear_slots.assert_node_id_matches_slot("d", "SIM-EAR-008")


def test_dg_installation_proposals() -> None:
    d = sim_ear_slots.installation_proposal_for_slot("d")
    assert d["node_id"] == "SIM-EAR-004"
    assert d["latitude"] == 59.3655000
    assert d["longitude"] == 18.0900000
    assert d["height_m"] == 95.0
    assert d["orientation_deg"] == 180.0
    e = sim_ear_slots.installation_proposal_for_slot("e")
    assert (e["latitude"], e["longitude"]) == (59.3180000, 18.1000000)
    f = sim_ear_slots.installation_proposal_for_slot("f")
    assert (f["latitude"], f["longitude"]) == (59.3550000, 17.9950000)
    g = sim_ear_slots.installation_proposal_for_slot("g")
    assert (g["latitude"], g["longitude"]) == (59.3120000, 17.9800000)


def test_observation_parser_accepts_a_through_g() -> None:
    parser = simulate_observation.build_parser()
    for slot in sim_ear_slots.EAR_SLOTS:
        args = parser.parse_args(["add", "--ear", slot])
        assert args.ear == slot
    with pytest.raises(SystemExit):
        parser.parse_args(["add", "--ear", "h"])


def test_keepalive_parser_accepts_slots_and_all() -> None:
    parser = simulate_keepalive.build_parser()
    for slot in sim_ear_slots.EAR_SLOTS:
        args = parser.parse_args(["once", "--ear", slot])
        assert args.ear == slot
    args = parser.parse_args(["once", "--ear", "all"])
    assert args.ear == "all"
    with pytest.raises(SystemExit):
        parser.parse_args(["once", "--ear", "h"])


def test_keepalive_all_scans_a_through_g(tmp_path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    sim_dir = tmp_path / "shared" / "simulator"
    sim_dir.mkdir(parents=True)
    (sim_dir / "sim-ear-a.json").write_text("{}")
    (sim_dir / "sim-ear-d.json").write_text("{}")
    (sim_dir / "sim-ear-g.json").write_text("{}")
    paths = simulate_keepalive.resolve_credentials_files(
        SimpleNamespace(credentials_file=None, ear="all")
    )
    assert paths == [
        "shared/simulator/sim-ear-a.json",
        "shared/simulator/sim-ear-d.json",
        "shared/simulator/sim-ear-g.json",
    ]


def test_helicopter_east_west_json_untouched_by_slot_module() -> None:
    before = HELICOPTER.read_bytes()
    # Touching proposals / validators must not rewrite scenario data.
    _ = sim_ear_slots.INSTALLATION_PROPOSALS["d"]
    sim_ear_slots.validate_ear_slot("g")
    assert HELICOPTER.read_bytes() == before
    # Baseline A–C scenario coords remain as authored (A still differs from Hub doc).
    import json

    scenario = json.loads(before.decode())
    assert scenario["ears"]["ear_a"]["latitude"] == pytest.approx(59.35116522644004)
    assert scenario["ears"]["ear_a"]["longitude"] == pytest.approx(18.048707350995315)
    assert "ear_d" not in scenario["ears"]
