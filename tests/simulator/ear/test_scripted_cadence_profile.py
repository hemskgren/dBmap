"""Focused tests for the helicopter_ew_10s scripted cadence profile."""

from __future__ import annotations

from importlib.util import module_from_spec, spec_from_file_location
from itertools import pairwise
from pathlib import Path

import pytest

_ROOT = Path(__file__).parents[3]
_EAR = _ROOT / "simulator" / "ear"

spec_sim = spec_from_file_location(
    "dbmap_ear_scenario_simulator",
    _EAR / "simulate_scenario.py",
)
assert spec_sim is not None and spec_sim.loader is not None
simulate_scenario = module_from_spec(spec_sim)
spec_sim.loader.exec_module(simulate_scenario)

spec_cadence = spec_from_file_location(
    "dbmap_scripted_cadence_profile",
    _EAR / "scripted_cadence_profile.py",
)
assert spec_cadence is not None and spec_cadence.loader is not None
scripted_cadence_profile = module_from_spec(spec_cadence)
spec_cadence.loader.exec_module(scripted_cadence_profile)

HELICOPTER_PATH = _EAR / "scenarios" / "helicopter_east_west.json"
PROFILE_ID = scripted_cadence_profile.HELICOPTER_EW_10S_PROFILE_ID


def _load_baseline():
    return simulate_scenario.load_scenario(HELICOPTER_PATH)


def _apply_10s(scenario):
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        scenario,
        mode="scripted",
        time_step_seconds=1.0,
        max_range_m=4000.0,
    )
    assert diagnostics is None
    return simulate_scenario.apply_named_scripted_cadence_profile(
        prepared, PROFILE_ID
    )


def test_baseline_helicopter_remains_unchanged_without_profile() -> None:
    scenario = _load_baseline()
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        scenario,
        mode="scripted",
        time_step_seconds=1.0,
        max_range_m=4000.0,
    )
    assert diagnostics is None
    unchanged = simulate_scenario.apply_named_scripted_cadence_profile(prepared, None)
    assert unchanged is prepared
    assert [item["at_seconds"] for item in prepared["observations"]] == [
        12,
        40,
        60,
        72,
        90,
        105,
        112,
        120,
        150,
        164,
        185,
        200,
        205,
        210,
        230,
        250,
        280,
        300,
    ]
    assert "scripted_cadence_profile" not in prepared
    timed = simulate_scenario.assign_scripted_ear_observation_lifecycles(
        prepared["observations"]
    )
    by_ear: dict[str, list] = {"ear_a": [], "ear_b": [], "ear_c": []}
    for item in timed:
        by_ear[item["ear"]].append(item)
    for reports in by_ear.values():
        assert len(reports) == 6
        assert len({item["observation_id"] for item in reports}) == 1
        kinds = [item["emission_kind"] for item in reports]
        assert kinds == ["start", "periodic", "periodic", "periodic", "periodic", "end"]


def test_cadence_timestamps_include_boundaries_and_nominal_step() -> None:
    times = scripted_cadence_profile.cadence_timestamps(
        12.0, 105.0, cadence_seconds=10.0
    )
    assert times[0] == 12.0
    assert times[-1] == 105.0
    assert 105.0 not in times[:-1]
    for left, right in pairwise(times[:-1]):
        assert abs((right - left) - 10.0) < 1e-9
    assert times[-1] - times[-2] < 10.0
    assert times == [12.0, 22.0, 32.0, 42.0, 52.0, 62.0, 72.0, 82.0, 92.0, 102.0, 105.0]


def test_helicopter_ew_10s_preserves_route_windows_and_one_observation_per_ear() -> None:
    baseline = _load_baseline()
    windows = scripted_cadence_profile.detection_windows_by_ear(baseline["observations"])
    assert windows == {
        "ear_a": (205.0, 300.0),
        "ear_b": (12.0, 105.0),
        "ear_c": (112.0, 200.0),
    }

    profiled = _apply_10s(baseline)
    meta = profiled["scripted_cadence_profile"]
    assert meta["profile_id"] == PROFILE_ID
    assert meta["cadence_seconds"] == 10.0
    assert meta["source_scenario_id"] == "helicopter_ew_01"
    assert meta["source_scenario_file"] == "helicopter_east_west.json"
    assert meta["authored_report_count"] == 18

    assert profiled["movement"] == baseline["movement"]
    assert profiled["ears"] == baseline["ears"]
    assert profiled["source_id"] == baseline["source_id"]
    assert profiled["scenario_id"] == baseline["scenario_id"]

    profiled_windows = scripted_cadence_profile.detection_windows_by_ear(
        profiled["observations"]
    )
    assert profiled_windows == windows

    timed = simulate_scenario.assign_scripted_ear_observation_lifecycles(
        profiled["observations"]
    )
    by_ear: dict[str, list] = {"ear_a": [], "ear_b": [], "ear_c": []}
    for item in timed:
        by_ear[item["ear"]].append(item)

    expected_counts = {"ear_a": 11, "ear_b": 11, "ear_c": 10}
    assert {ear: len(items) for ear, items in by_ear.items()} == expected_counts
    assert meta["resampled_report_count"] == sum(expected_counts.values())

    for ear, reports in by_ear.items():
        assert len({item["observation_id"] for item in reports}) == 1
        kinds = [item["emission_kind"] for item in reports]
        assert kinds[0] == "start"
        assert kinds[-1] == "end"
        assert all(kind == "periodic" for kind in kinds[1:-1])
        assert reports[0]["at_seconds"] == windows[ear][0]
        assert reports[-1]["at_seconds"] == windows[ear][1]
        # Dense 10 s sampling must not trigger the 30 s lifecycle split.
        for left, right in pairwise(reports):
            gap = right["at_seconds"] - left["at_seconds"]
            assert gap <= simulate_scenario.SCRIPTED_LIFECYCLE_GAP_SECONDS
            if right is not reports[-1]:
                assert abs(gap - 10.0) < 1e-9
            else:
                assert 0 < gap <= 10.0 + 1e-9


def test_helicopter_ew_10s_rejects_wrong_scenario_and_independent_mode(
    tmp_path,
) -> None:
    baseline = _load_baseline()
    prepared, _ = simulate_scenario.apply_simulation_mode(
        baseline,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=1000.0,
        observation_interval_seconds=5.0,
    )
    with pytest.raises(simulate_scenario.ScenarioError, match="only with --mode scripted"):
        simulate_scenario.apply_named_scripted_cadence_profile(prepared, PROFILE_ID)

    other = dict(baseline)
    other["scenario_id"] = "not_helicopter"
    with pytest.raises(simulate_scenario.ScenarioError, match="requires scenario_id"):
        simulate_scenario.apply_named_scripted_cadence_profile(other, PROFILE_ID)


def test_helicopter_json_file_not_modified_by_profile_module() -> None:
    before = HELICOPTER_PATH.read_bytes()
    _apply_10s(_load_baseline())
    assert HELICOPTER_PATH.read_bytes() == before
