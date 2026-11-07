"""Phase 5C: segmented movement normalize + position_at + lifecycle isolation."""

from __future__ import annotations

import inspect
import json
import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

_ROOT = Path(__file__).parents[3]
_EAR = _ROOT / "simulator" / "ear"
_SCENARIOS = _EAR / "scenarios"
_SCRIPTS = _ROOT / "scripts"

sys.path.insert(0, str(_SCRIPTS))

from lib.scenario_ground_truth import ground_truth_from_loaded_scenario
from lib.scenario_movement import (
    CONTINUITY_TOLERANCE_M,
    MovementError,
    distance_m,
    normalize_movement,
    position_at,
    position_on_route,
)

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator_phase5c",
    _EAR / "simulate_scenario.py",
)
assert spec is not None and spec.loader is not None
simulate_scenario = module_from_spec(spec)
spec.loader.exec_module(simulate_scenario)

# Tier 2 segmented fixtures (phase5c_*) are intentionally non-legacy.
_LEGACY_SCENARIOS = sorted(
    path for path in _SCENARIOS.glob("*.json") if not path.name.startswith("phase5c_")
)
_ADE = _SCENARIOS / "helicopter_ade_overlap.json"
_ABDE = _SCENARIOS / "helicopter_abde_overlap.json"
_EW = _SCENARIOS / "helicopter_east_west.json"

# Two short transit legs joined by a hold (Stockholm-ish, continuous).
_HOLD_POS = {"latitude": 59.35225, "longitude": 18.069}
_SEG_START = {"latitude": 59.35225, "longitude": 18.0585}
_SEG_END = {"latitude": 59.35225, "longitude": 18.0795}


def _segmented_raw(
    *,
    hold_duration_s: float = 15.0,
    speed_m_s: float = 30.0,
) -> dict:
    return {
        "segments": [
            {
                "type": "transit",
                "start": dict(_SEG_START),
                "end": dict(_HOLD_POS),
                "speed_m_s": speed_m_s,
            },
            {
                "type": "hold",
                "position": dict(_HOLD_POS),
                "duration_s": hold_duration_s,
            },
            {
                "type": "transit",
                "start": dict(_HOLD_POS),
                "end": dict(_SEG_END),
                "speed_m_s": speed_m_s,
            },
        ]
    }


def _write_scenario(tmp_path: Path, movement: dict, *, at_seconds: list[float] | None = None) -> Path:
    times = at_seconds if at_seconds is not None else [0.0]
    observations = [
        {
            "at_seconds": t,
            "ear": "ear_a",
            "bearing_confidence": 0.5,
            "classification_confidence": 0.5,
            "signal_level_dbfs": -30,
        }
        for t in times
    ]
    payload = {
        "scenario_id": "phase5c-seg",
        "source": {
            "ground_truth_id": "SIM-SEG-001",
            "source_family": "helicopter",
            "movement": movement,
        },
        "ears": {
            "ear_a": {
                "latitude": 59.351176,
                "longitude": 18.048664,
                "orientation_deg": 0,
            }
        },
        "observations": observations,
    }
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


# --- Legacy compatibility -------------------------------------------------


def test_all_legacy_scenarios_still_load() -> None:
    assert _LEGACY_SCENARIOS
    for path in _LEGACY_SCENARIOS:
        scenario = simulate_scenario.load_scenario(path)
        assert scenario["movement"]["form"] == "legacy"
        assert scenario["movement"]["speed_m_s"] is not None
        assert len(scenario["movement"]["segments"]) == 1
        assert scenario["movement"]["segments"][0]["type"] == "transit"


@pytest.mark.parametrize(
    ("path", "speed", "duration"),
    [
        (_ADE, 30.0, 40.0),
        (_ABDE, 30.0, 40.0),
        (_EW, 45.0, None),
    ],
)
def test_legacy_duration_and_speed_expectations(
    path: Path, speed: float, duration: float | None
) -> None:
    scenario = simulate_scenario.load_scenario(path)
    mov = scenario["movement"]
    assert mov["speed_m_s"] == speed
    if duration is not None:
        assert mov["duration_seconds"] == pytest.approx(duration, abs=1e-6)
    else:
        # east-west: distance / 45
        expected = mov["distance_m"] / speed
        assert mov["duration_seconds"] == pytest.approx(expected, abs=1e-6)


def test_legacy_source_positions_and_bearings_unchanged() -> None:
    scenario = simulate_scenario.load_scenario(_EW)
    mov = scenario["movement"]
    # Legacy single-leg: position_at matches spherical position_on_route.
    for item in scenario["observations"]:
        t = float(item["at_seconds"])
        via_helper = simulate_scenario.position_at(mov, t)
        via_legacy = simulate_scenario._position_on_route(
            mov["start"], mov["end"], t / mov["duration_seconds"]
        )
        assert via_helper["latitude"] == pytest.approx(via_legacy["latitude"], abs=1e-12)
        assert via_helper["longitude"] == pytest.approx(
            via_legacy["longitude"], abs=1e-12
        )
        ear = scenario["ears"][item["ear"]]
        expected_bearing = (
            simulate_scenario._bearing_deg(ear, via_helper) - ear["orientation_deg"]
        ) % 360
        if item.get("bearing_available", True):
            assert item["bearing_deg"] == pytest.approx(expected_bearing, abs=1e-9)


# --- Input validation -----------------------------------------------------


def test_mixed_legacy_and_segments_rejected() -> None:
    with pytest.raises(MovementError, match="must not mix"):
        normalize_movement(
            {
                "start": dict(_SEG_START),
                "end": dict(_SEG_END),
                "speed_m_s": 30,
                "segments": _segmented_raw()["segments"],
            }
        )


def test_empty_segments_rejected() -> None:
    with pytest.raises(MovementError, match="non-empty"):
        normalize_movement({"segments": []})


def test_discontinuous_join_rejected() -> None:
    raw = _segmented_raw()
    raw["segments"][1]["position"] = {
        "latitude": 59.35225,
        "longitude": 18.070,  # ~56 m east of hold join
    }
    with pytest.raises(MovementError, match="discontinuous"):
        normalize_movement(raw)


def test_zero_or_negative_transit_speed_rejected() -> None:
    for speed in (0, -1):
        with pytest.raises(MovementError, match="speed_m_s"):
            normalize_movement(
                {
                    "segments": [
                        {
                            "type": "transit",
                            "start": dict(_SEG_START),
                            "end": dict(_SEG_END),
                            "speed_m_s": speed,
                        }
                    ]
                }
            )


def test_zero_or_negative_hold_duration_rejected() -> None:
    for duration in (0, -5):
        with pytest.raises(MovementError, match="duration_s"):
            normalize_movement(
                {
                    "segments": [
                        {
                            "type": "hold",
                            "position": dict(_HOLD_POS),
                            "duration_s": duration,
                        }
                    ]
                }
            )


def test_zero_length_transit_rejected() -> None:
    with pytest.raises(MovementError, match="distance must be greater than zero"):
        normalize_movement(
            {
                "segments": [
                    {
                        "type": "transit",
                        "start": dict(_HOLD_POS),
                        "end": dict(_HOLD_POS),
                        "speed_m_s": 30,
                    }
                ]
            }
        )


def test_unknown_segment_type_rejected() -> None:
    with pytest.raises(MovementError, match="transit' or 'hold"):
        normalize_movement(
            {
                "segments": [
                    {
                        "type": "hover",
                        "position": dict(_HOLD_POS),
                        "duration_s": 10,
                    }
                ]
            }
        )


def test_load_scenario_rejects_mixed_movement(tmp_path: Path) -> None:
    path = _write_scenario(
        tmp_path,
        {
            "start": dict(_SEG_START),
            "end": dict(_SEG_END),
            "speed_m_s": 30,
            "segments": _segmented_raw()["segments"],
        },
    )
    with pytest.raises(simulate_scenario.ScenarioError, match="must not mix"):
        simulate_scenario.load_scenario(path)


# --- Position calculations ------------------------------------------------


def test_positions_at_segment_boundaries_and_hold() -> None:
    mov = normalize_movement(_segmented_raw(hold_duration_s=15.0, speed_m_s=30.0))
    assert mov["form"] == "segments"
    assert mov["speed_m_s"] is None
    d0 = float(mov["segments"][0]["duration_seconds"])
    d1 = float(mov["segments"][1]["duration_seconds"])
    d2 = float(mov["segments"][2]["duration_seconds"])
    assert d1 == pytest.approx(15.0)
    total = d0 + d1 + d2
    assert mov["duration_seconds"] == pytest.approx(total)

    start = position_at(mov, 0.0)
    assert start == pytest.approx(_SEG_START)

    hold_entry = position_at(mov, d0)
    assert hold_entry["latitude"] == pytest.approx(_HOLD_POS["latitude"])
    assert hold_entry["longitude"] == pytest.approx(_HOLD_POS["longitude"])

    mid_hold = position_at(mov, d0 + d1 / 2)
    assert mid_hold == hold_entry

    hold_exit = position_at(mov, d0 + d1)
    assert hold_exit == hold_entry

    end = position_at(mov, total)
    assert end["latitude"] == pytest.approx(_SEG_END["latitude"])
    assert end["longitude"] == pytest.approx(_SEG_END["longitude"])

    # Continuity across every authored join.
    assert distance_m(hold_entry, mid_hold) <= CONTINUITY_TOLERANCE_M
    assert distance_m(hold_exit, position_at(mov, d0 + d1 + 1e-9)) <= 1.0


def test_transit_interpolation_is_spherical() -> None:
    start = {"latitude": 0.0, "longitude": 0.0}
    end = {"latitude": 10.0, "longitude": 20.0}
    mov = normalize_movement(
        {"start": start, "end": end, "speed_m_s": 100.0}
    )
    mid_t = mov["duration_seconds"] / 2.0
    spherical = position_on_route(start, end, 0.5)
    linear = {
        "latitude": (start["latitude"] + end["latitude"]) / 2,
        "longitude": (start["longitude"] + end["longitude"]) / 2,
    }
    got = position_at(mov, mid_t)
    assert got["latitude"] == pytest.approx(spherical["latitude"], abs=1e-12)
    assert got["longitude"] == pytest.approx(spherical["longitude"], abs=1e-12)
    # Distinct from naive linear mid for this long leg.
    assert abs(got["latitude"] - linear["latitude"]) > 1e-4


def test_times_outside_duration_rejected() -> None:
    mov = normalize_movement(_segmented_raw())
    with pytest.raises(MovementError, match="outside movement duration"):
        position_at(mov, -0.01)
    with pytest.raises(MovementError, match="outside movement duration"):
        position_at(mov, mov["duration_seconds"] + 0.01)


def test_position_at_is_deterministic_regardless_of_caller_order() -> None:
    mov = normalize_movement(_segmented_raw())
    times = [0.0, 5.0, 12.3, mov["duration_seconds"] / 2, mov["duration_seconds"]]
    forward = [position_at(mov, t) for t in times]
    reverse = [position_at(mov, t) for t in reversed(times)]
    for expected, got in zip(forward, reversed(reverse)):
        assert expected == got


# --- Ground-truth isolation -----------------------------------------------


def test_ground_truth_agrees_with_simulator_position_at(tmp_path: Path) -> None:
    mov_raw = _segmented_raw()
    norm = normalize_movement(mov_raw)
    times = [
        0.0,
        norm["segments"][0]["duration_seconds"],
        norm["segments"][0]["duration_seconds"]
        + norm["segments"][1]["duration_seconds"] / 2,
        norm["duration_seconds"],
    ]
    path = _write_scenario(tmp_path, mov_raw, at_seconds=times)
    scenario = simulate_scenario.load_scenario(path)
    gt = ground_truth_from_loaded_scenario(scenario)
    assert gt.speed_m_s is None
    for t in times:
        sim_pos = simulate_scenario.position_at(scenario["movement"], t)
        gt_lat, gt_lon = gt.position_at(t)
        assert gt_lat == pytest.approx(sim_pos["latitude"], abs=1e-12)
        assert gt_lon == pytest.approx(sim_pos["longitude"], abs=1e-12)


def test_ground_truth_not_in_production_analysis_signatures() -> None:
    # Temporal epoch evaluator has no GT parameter (production path isolation).
    from lib.localization_epoch_diagnostic import evaluate_event_localization_epochs

    params = inspect.signature(evaluate_event_localization_epochs).parameters
    assert "ground_truth" not in params
    assert "ground_truth_id" not in params


def test_segmented_load_preserves_observation_enrichment(tmp_path: Path) -> None:
    mov_raw = _segmented_raw()
    norm = normalize_movement(mov_raw)
    t_hold = (
        float(norm["segments"][0]["duration_seconds"])
        + float(norm["segments"][1]["duration_seconds"]) / 2
    )
    path = _write_scenario(tmp_path, mov_raw, at_seconds=[0.0, t_hold, norm["duration_seconds"]])
    scenario = simulate_scenario.load_scenario(path)
    assert scenario["movement"]["form"] == "segments"
    assert scenario["movement"]["speed_m_s"] is None
    hold_obs = scenario["observations"][1]
    assert hold_obs["source_distance_m"] > 0
    pos = simulate_scenario.position_at(scenario["movement"], t_hold)
    assert pos["latitude"] == pytest.approx(_HOLD_POS["latitude"])
    assert pos["longitude"] == pytest.approx(_HOLD_POS["longitude"])


# --- Lifecycle behavior ---------------------------------------------------


def test_segment_boundary_does_not_split_observation() -> None:
    """Reports spanning a hold boundary still share one Observation lifecycle."""
    mov = normalize_movement(_segmented_raw(hold_duration_s=15.0))
    d0 = float(mov["segments"][0]["duration_seconds"])
    # Dense reports across transit→hold→transit with gaps << 30 s.
    times = [d0 - 5, d0, d0 + 5, d0 + 10, d0 + 15, d0 + 20]
    events = [
        {"at_seconds": t, "ear": "ear_a", "source_family": "helicopter"}
        for t in times
    ]
    assigned = simulate_scenario.assign_scripted_ear_observation_lifecycles(events)
    assert simulate_scenario.SCRIPTED_LIFECYCLE_GAP_SECONDS == 30.0
    ids = {item["observation_id"] for item in assigned}
    assert len(ids) == 1
    kinds = [item["emission_kind"] for item in assigned]
    assert kinds[0] == "start"
    assert kinds[-1] == "end"
    assert all(kind == "periodic" for kind in kinds[1:-1])


def test_genuine_gap_over_30s_still_splits_observation() -> None:
    gap = simulate_scenario.SCRIPTED_LIFECYCLE_GAP_SECONDS
    events = [
        {"at_seconds": 0, "ear": "ear_a"},
        {"at_seconds": 10, "ear": "ear_a"},
        {"at_seconds": 10 + gap + 1, "ear": "ear_a"},
        {"at_seconds": 10 + gap + 11, "ear": "ear_a"},
    ]
    assigned = simulate_scenario.assign_scripted_ear_observation_lifecycles(events)
    assert assigned[0]["observation_id"] == assigned[1]["observation_id"]
    assert assigned[2]["observation_id"] == assigned[3]["observation_id"]
    assert assigned[0]["observation_id"] != assigned[2]["observation_id"]
    assert [item["emission_kind"] for item in assigned] == [
        "start",
        "end",
        "start",
        "end",
    ]


def test_holds_do_not_invent_reports(tmp_path: Path) -> None:
    """Loading segmented movement only enriches authored times; no hold samples."""
    mov_raw = _segmented_raw(hold_duration_s=20.0)
    # Only endpoints authored — hold interval has no reports.
    norm = normalize_movement(mov_raw)
    path = _write_scenario(
        tmp_path,
        mov_raw,
        at_seconds=[0.0, norm["duration_seconds"]],
    )
    scenario = simulate_scenario.load_scenario(path)
    assert len(scenario["observations"]) == 2
    assert scenario["observations"][0]["at_seconds"] == 0.0
    assert scenario["observations"][1]["at_seconds"] == pytest.approx(
        norm["duration_seconds"]
    )
