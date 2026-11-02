import math
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


def test_signal_equals_reference_at_reference_distance() -> None:
    config = detection.SignalLevelConfig(
        reference_level_dbfs=-30.0,
        reference_distance_m=1000.0,
        path_loss_exponent=1.0,
    )
    assert detection.distance_dependent_signal_level_dbfs(1000.0, config) == pytest.approx(
        -30.0
    )


def test_signal_decreases_as_distance_increases() -> None:
    config = detection.SignalLevelConfig()
    near = detection.distance_dependent_signal_level_dbfs(500.0, config)
    far = detection.distance_dependent_signal_level_dbfs(2000.0, config)
    assert near > far


def test_doubling_distance_attenuates_about_six_db_for_exponent_one() -> None:
    config = detection.SignalLevelConfig(path_loss_exponent=1.0)
    at_1k = detection.distance_dependent_signal_level_dbfs(1000.0, config)
    at_2k = detection.distance_dependent_signal_level_dbfs(2000.0, config)
    # 20 * log10(2) ≈ 6.02 dB for exponent 1.0
    assert at_1k - at_2k == pytest.approx(20.0 * math.log10(2.0))


def test_higher_path_loss_exponent_attenuates_more() -> None:
    mild = detection.SignalLevelConfig(path_loss_exponent=1.0)
    strong = detection.SignalLevelConfig(path_loss_exponent=2.0)
    mild_drop = detection.distance_dependent_signal_level_dbfs(
        1000.0, mild
    ) - detection.distance_dependent_signal_level_dbfs(2000.0, mild)
    strong_drop = detection.distance_dependent_signal_level_dbfs(
        1000.0, strong
    ) - detection.distance_dependent_signal_level_dbfs(2000.0, strong)
    assert strong_drop == pytest.approx(2 * mild_drop)
    assert strong_drop == pytest.approx(40.0 * math.log10(2.0))


def test_very_small_distance_is_safe_and_clamped_to_protocol_max() -> None:
    config = detection.SignalLevelConfig(
        reference_level_dbfs=-30.0,
        reference_distance_m=1000.0,
        path_loss_exponent=1.0,
    )
    level = detection.distance_dependent_signal_level_dbfs(1e-9, config)
    assert math.isfinite(level)
    assert level == 0.0  # explicit protocol clamp (≤ 0 dBFS)


def test_invalid_signal_parameters_are_rejected() -> None:
    with pytest.raises(ValueError, match="finite"):
        detection.SignalLevelConfig(reference_level_dbfs=float("nan")).validate()
    with pytest.raises(ValueError, match="greater than zero"):
        detection.SignalLevelConfig(reference_distance_m=0).validate()
    with pytest.raises(ValueError, match="greater than zero"):
        detection.SignalLevelConfig(path_loss_exponent=0).validate()
    with pytest.raises(ValueError, match="at most 0"):
        detection.SignalLevelConfig(reference_level_dbfs=1.0).validate()


def test_independent_detections_use_calculated_levels_and_differ_by_ear(
    tmp_path,
) -> None:
    scenario = {
        "scenario_id": "level-diff",
        "source": {
            "ground_truth_id": "SIM-SRC",
            "source_family": "helicopter",
            "movement": {
                "start": {"latitude": 0.0, "longitude": 0.0},
                "end": {"latitude": 0.0, "longitude": 0.02},
                "speed_m_s": 20,
            },
        },
        "ears": {
            "near": {"latitude": 0.0, "longitude": 0.005, "orientation_deg": 0},
            "far": {"latitude": 0.05, "longitude": 0.005, "orientation_deg": 0},
        },
        "observations": [
            {
                "at_seconds": 0,
                "ear": "near",
                "bearing_confidence": 0.5,
                "classification_confidence": 0.5,
                "signal_level_dbfs": -20,
            }
        ],
    }
    path = tmp_path / "scenario.json"
    path.write_text(__import__("json").dumps(scenario))
    loaded = simulate_scenario.load_scenario(path)
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=20_000.0,
        observation_interval_seconds=1.0,
    )
    assert diagnostics is not None
    by_ear: dict[str, list[float]] = {"near": [], "far": []}
    for event in prepared["observations"]:
        by_ear[event["ear"]].append(event["signal_level_dbfs"])
    assert by_ear["near"] and by_ear["far"]
    assert max(by_ear["near"]) > max(by_ear["far"])


def test_signal_params_change_levels_not_detection_counts(tmp_path) -> None:
    path = (
        Path(__file__).parents[3]
        / "simulator"
        / "ear"
        / "scenarios"
        / "helicopter_east_west.json"
    )
    loaded = simulate_scenario.load_scenario(path)
    default, default_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=4000.0,
        path_loss_exponent=1.0,
        observation_interval_seconds=1.0,
    )
    steeper, steeper_diag = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="independent_detection",
        time_step_seconds=1.0,
        max_range_m=4000.0,
        path_loss_exponent=2.0,
        observation_interval_seconds=1.0,
    )
    assert default_diag.observations_generated == steeper_diag.observations_generated
    for alias in default_diag.per_ear:
        assert (
            default_diag.per_ear[alias].detections
            == steeper_diag.per_ear[alias].detections
        )
        assert (
            default_diag.per_ear[alias].detection_intervals
            == steeper_diag.per_ear[alias].detection_intervals
        )
    default_levels = [item["signal_level_dbfs"] for item in default["observations"]]
    steeper_levels = [item["signal_level_dbfs"] for item in steeper["observations"]]
    assert default_levels != steeper_levels


def test_scripted_mode_keeps_authored_signal_levels(tmp_path) -> None:
    scenario = {
        "scenario_id": "scripted-levels",
        "source": {
            "ground_truth_id": "SIM-SRC",
            "source_family": "vehicle",
            "movement": {
                "start": {"latitude": 0.0, "longitude": 0.0},
                "end": {"latitude": 0.0, "longitude": 0.01},
                "speed_m_s": 12,
            },
        },
        "ears": {
            "ear_a": {"latitude": 0.001, "longitude": 0.0, "orientation_deg": 0},
        },
        "observations": [
            {
                "at_seconds": 0,
                "ear": "ear_a",
                "bearing_confidence": 0.6,
                "classification_confidence": 0.7,
                "signal_level_dbfs": -22.5,
            }
        ],
    }
    path = tmp_path / "scenario.json"
    path.write_text(__import__("json").dumps(scenario))
    loaded = simulate_scenario.load_scenario(path)
    prepared, diagnostics = simulate_scenario.apply_simulation_mode(
        loaded,
        mode="scripted",
        time_step_seconds=1.0,
        max_range_m=4000.0,
        path_loss_exponent=2.0,
    )
    assert diagnostics is None
    assert prepared["observations"][0]["signal_level_dbfs"] == -22.5
