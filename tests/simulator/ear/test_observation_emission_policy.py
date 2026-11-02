import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[3] / "simulator" / "ear"))
sys.path.insert(0, str(Path(__file__).parents[3] / "scripts"))

from importlib.util import module_from_spec, spec_from_file_location

import independent_detection as detection

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator",
    Path(__file__).parents[3] / "simulator" / "ear" / "simulate_scenario.py",
)
assert spec is not None and spec.loader is not None
simulate_scenario = module_from_spec(spec)
spec.loader.exec_module(simulate_scenario)


def test_emission_schedule_start_periodic_end() -> None:
    policy = detection.ObservationEmissionPolicy(observation_interval_seconds=5.0)
    schedule = detection.emission_schedule_for_episode(10.0, 22.0, policy)
    assert schedule == [
        (10.0, "start"),
        (15.0, "periodic"),
        (20.0, "periodic"),
        (22.0, "end"),
    ]


def test_emission_schedule_no_duplicate_when_end_on_periodic() -> None:
    policy = detection.ObservationEmissionPolicy(observation_interval_seconds=5.0)
    schedule = detection.emission_schedule_for_episode(10.0, 20.0, policy)
    assert schedule == [
        (10.0, "start"),
        (15.0, "periodic"),
        (20.0, "end"),
    ]
    assert len(schedule) == len({t for t, _ in schedule})


def test_short_single_step_episode_emits_start_only() -> None:
    policy = detection.ObservationEmissionPolicy(observation_interval_seconds=5.0)
    assert detection.emission_schedule_for_episode(7.0, 7.0, policy) == [
        (7.0, "start")
    ]


def test_invalid_observation_interval_rejected() -> None:
    with pytest.raises(ValueError, match="greater than zero"):
        detection.ObservationEmissionPolicy(observation_interval_seconds=0).validate()
    with pytest.raises(ValueError, match="finite"):
        detection.ObservationEmissionPolicy(
            observation_interval_seconds=float("nan")
        ).validate()


def _two_ear_scenario() -> dict:
    return {
        "scenario_id": "emission-test",
        "source": {
            "ground_truth_id": "SIM-SRC",
            "source_family": "helicopter",
            "movement": {
                "start": {"latitude": 0.0, "longitude": 0.0},
                "end": {"latitude": 0.0, "longitude": 0.01},
                "speed_m_s": 10,
            },
        },
        "ears": {
            "ear_a": {"latitude": 0.0, "longitude": 0.002, "orientation_deg": 0},
            "ear_b": {"latitude": 0.0, "longitude": 0.008, "orientation_deg": 90},
        },
        "observations": [
            {
                "at_seconds": 0,
                "ear": "ear_a",
                "bearing_confidence": 0.5,
                "classification_confidence": 0.5,
                "signal_level_dbfs": -30,
            }
        ],
    }


def test_interval_changes_emissions_not_detections(tmp_path) -> None:
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(_two_ear_scenario()))
    loaded = simulate_scenario.load_scenario(path)
    every, every_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=50_000.0,
        observation_interval_seconds=1.0,
    )
    sparse, sparse_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=50_000.0,
        observation_interval_seconds=5.0,
    )
    assert (
        every_diag.total_positive_detection_evaluations
        == sparse_diag.total_positive_detection_evaluations
    )
    for alias in every_diag.per_ear:
        assert (
            every_diag.per_ear[alias].detection_intervals
            == sparse_diag.per_ear[alias].detection_intervals
        )
        assert (
            every_diag.per_ear[alias].detections
            == sparse_diag.per_ear[alias].detections
        )
    assert sparse_diag.observations_generated < every_diag.observations_generated
    assert len(sparse["observations"]) == sparse_diag.observations_generated


def test_ears_have_independent_emission_schedules(tmp_path) -> None:
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(_two_ear_scenario()))
    loaded = simulate_scenario.load_scenario(path)
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=50_000.0,
        observation_interval_seconds=5.0,
    )
    assert diagnostics.per_ear["ear_a"].detection_episodes >= 1
    assert diagnostics.per_ear["ear_b"].detection_episodes >= 1
    starts = [
        item
        for item in prepared["observations"]
        if item.get("emission_kind") == "start"
    ]
    assert {item["ear"] for item in starts} == {"ear_a", "ear_b"}


def test_emitted_signal_levels_match_timestamp_distance(tmp_path) -> None:
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(_two_ear_scenario()))
    loaded = simulate_scenario.load_scenario(path)
    prepared, _ = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=50_000.0,
        observation_interval_seconds=5.0,
    )
    assert prepared["observations"]
    for event in prepared["observations"]:
        expected = detection.distance_dependent_signal_level_dbfs(
            event["source_distance_m"],
            detection.SignalLevelConfig(),
        )
        assert event["signal_level_dbfs"] == pytest.approx(expected)


def test_new_episode_emits_fresh_start(tmp_path) -> None:
    # Construct geometry where one ear has two separate detection windows.
    scenario = {
        "scenario_id": "two-episodes",
        "source": {
            "ground_truth_id": "SIM-SRC",
            "source_family": "helicopter",
            "movement": {
                "start": {"latitude": 0.0, "longitude": -0.02},
                "end": {"latitude": 0.0, "longitude": 0.02},
                "speed_m_s": 5,
            },
        },
        "ears": {
            "ear_a": {"latitude": 0.0, "longitude": 0.0, "orientation_deg": 0},
        },
        "observations": [
            {
                "at_seconds": 0,
                "ear": "ear_a",
                "bearing_confidence": 0.5,
                "classification_confidence": 0.5,
                "signal_level_dbfs": -30,
            }
        ],
    }
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(scenario))
    loaded = simulate_scenario.load_scenario(path)
    # Small range → two passes (approach then leave then approach other side)
    # may be one continuous window; use tiny range around the ear.
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=400.0,
        observation_interval_seconds=5.0,
    )
    starts = [
        item["at_seconds"]
        for item in prepared["observations"]
        if item.get("emission_kind") == "start"
    ]
    assert diagnostics.per_ear["ear_a"].detection_episodes == len(starts)
    assert len(starts) == len(set(starts))


def test_helicopter_emission_interval_preserves_stage1_detections() -> None:
    path = (
        Path(__file__).parents[3]
        / "simulator"
        / "ear"
        / "scenarios"
        / "helicopter_east_west.json"
    )
    loaded = simulate_scenario.load_scenario(path)
    baseline, baseline_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=4000.0,
        observation_interval_seconds=1.0,
    )
    sampled, sampled_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=4000.0,
        observation_interval_seconds=5.0,
    )
    assert (
        baseline_diag.total_positive_detection_evaluations
        == sampled_diag.total_positive_detection_evaluations
        == 497
    )
    for alias in baseline_diag.per_ear:
        assert (
            baseline_diag.per_ear[alias].detection_intervals
            == sampled_diag.per_ear[alias].detection_intervals
        )
    assert sampled_diag.observations_generated < baseline_diag.observations_generated
    assert len(baseline["observations"]) == 497
