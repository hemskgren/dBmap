"""Offline helicopter_east_west → B2 golden-path tests (Phase 1).

No MQTT, Hub API, Analysis container, or map. Ground truth is evaluation-only.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.helicopter_offline_golden_path import (
    ALIAS_TO_NODE,
    GOLDEN_ALLOW_LONG_RANGE_SIM_EARS,
    GOLDEN_MAX_RANGE_M,
    GOLDEN_MIN_EARS_FOR_EVENT,
    GOLDEN_MODE,
    GOLDEN_OBSERVATION_INTERVAL_SECONDS,
    GOLDEN_TIME_STEP_SECONDS,
    GoldenPathParams,
    analyze_reports,
    event_membership_signature,
    localization_signature,
    measurement_fingerprint,
    prepare_independent_scenario,
    run_golden_path,
)
from lib.scenario_ground_truth import ground_truth_from_loaded_scenario


def test_independent_detection_generates_three_ear_overlap() -> None:
    scenario, diagnostics = prepare_independent_scenario()
    assert scenario["simulation_mode"] == GOLDEN_MODE
    assert diagnostics is not None
    assert diagnostics.simultaneous_three_intervals
    participating = {
        alias
        for alias, stats in diagnostics.per_ear.items()
        if stats.observations_emitted > 0
    }
    assert participating == {"ear_a", "ear_b", "ear_c"}
    # Each Ear episode shares one observation_id across start/periodic/end.
    by_ear: dict[str, set[str]] = {alias: set() for alias in scenario["ears"]}
    for event in scenario["observations"]:
        assert "observation_id" in event
        by_ear[event["ear"]].add(event["observation_id"])
    assert all(len(ids) >= 1 for ids in by_ear.values())


def test_golden_path_promotes_three_ear_event_and_keeps_gt_out_of_analysis() -> None:
    bundle = run_golden_path()
    result = bundle.analysis_result
    correlation = result.correlation
    assert correlation is not None
    assert result.report_count == len(bundle.reports)
    assert result.unique_observation_count == 3
    assert len(correlation.events) == 1
    event = correlation.events[0]
    assert sorted(event.ear_ids) == sorted(ALIAS_TO_NODE.values())
    assert event.score >= 4
    assert correlation.parameters["allow_long_range_sim_ears"] is True
    assert correlation.parameters["min_ears_for_event"] == GOLDEN_MIN_EARS_FOR_EVENT
    assert (
        correlation.parameters["ground_truth_policy"]
        == "ignored_for_association_and_localization"
    )
    # GT trajectory exists separately and is not part of the snapshot reports.
    assert bundle.ground_truth.ground_truth_id == "SIM-HELI-001"
    for report in bundle.reports:
        spatial = report.get("spatial") or {}
        assert spatial.get("position") is None
        # Association must ignore the simulator hint; it may still be present.
        hints = (
            ((report.get("acoustic") or {}).get("classification") or {}).get("hints")
            or {}
        )
        assert hints.get("simulated_source_id") == "SIM-HELI-001"


def test_ground_truth_helper_is_derived_from_scenario_movement() -> None:
    scenario, _ = prepare_independent_scenario()
    trajectory = ground_truth_from_loaded_scenario(scenario)
    start = trajectory.position_at(0.0)
    end = trajectory.position_at(trajectory.duration_seconds)
    assert start == (
        scenario["movement"]["start"]["latitude"],
        scenario["movement"]["start"]["longitude"],
    )
    assert end == (
        scenario["movement"]["end"]["latitude"],
        scenario["movement"]["end"]["longitude"],
    )
    mid = trajectory.position_at(trajectory.duration_seconds / 2.0)
    assert start[0] == mid[0]  # constant latitude east-west route
    assert start[1] > mid[1] > end[1]


def test_same_snapshot_analysis_is_deterministic() -> None:
    bundle = run_golden_path()
    again = analyze_reports(bundle.reports, bundle.nodes, bundle.params)
    assert event_membership_signature(bundle.analysis_result) == (
        event_membership_signature(again)
    )
    assert localization_signature(bundle.analysis_result) == localization_signature(
        again
    )


def test_regenerated_run_has_equivalent_measurements_and_membership_shape() -> None:
    first = run_golden_path()
    second = run_golden_path()
    assert measurement_fingerprint(first.reports) == measurement_fingerprint(
        second.reports
    )
    # Opaque observation_ids (and therefore event_ids) may differ across runs.
    first_obs = {
        report["identity"]["observation_id"] for report in first.reports
    }
    second_obs = {
        report["identity"]["observation_id"] for report in second.reports
    }
    assert first_obs != second_obs
    first_shape = [
        (tuple(sorted(ears)), score)
        for _event_id, ears, _obs, score in event_membership_signature(
            first.analysis_result
        )
    ]
    second_shape = [
        (tuple(sorted(ears)), score)
        for _event_id, ears, _obs, score in event_membership_signature(
            second.analysis_result
        )
    ]
    assert first_shape == second_shape
    first_loc = [
        row[1:] for row in localization_signature(first.analysis_result)
    ]
    second_loc = [
        row[1:] for row in localization_signature(second.analysis_result)
    ]
    assert first_loc == second_loc


def test_ground_truth_evaluation_reports_error_when_position_available() -> None:
    bundle = run_golden_path()
    assert bundle.localization_evaluations
    row = bundle.localization_evaluations[0]
    event = bundle.analysis_result.correlation.events[0]
    localization = event.localization
    assert localization is not None
    # Membership includes all three EARs; contributors may be a subset.
    assert sorted(row.membership_ears) == sorted(ALIAS_TO_NODE.values())
    assert set(row.contributing_ears).issubset(set(row.membership_ears))
    if localization.latitude is None or localization.details.get("position_withheld"):
        assert row.error_m is None
        assert row.position_withheld is True or localization.latitude is None
    else:
        assert row.error_m is not None
        assert row.error_m >= 0.0
        assert row.estimated_latitude == localization.latitude
        assert row.ground_truth_latitude is not None
        assert row.evaluation_time_utc == localization.evaluation_time_utc


def test_documented_parameters_match_constants() -> None:
    params = GoldenPathParams()
    assert params.mode == "independent_detection"
    assert params.max_range_m == GOLDEN_MAX_RANGE_M == 4000.0
    assert params.time_step_seconds == GOLDEN_TIME_STEP_SECONDS == 1.0
    assert (
        params.observation_interval_seconds
        == GOLDEN_OBSERVATION_INTERVAL_SECONDS
        == 5.0
    )
    assert params.allow_long_range_sim_ears is GOLDEN_ALLOW_LONG_RANGE_SIM_EARS is True
    assert params.min_ears_for_event == GOLDEN_MIN_EARS_FOR_EVENT == 3
