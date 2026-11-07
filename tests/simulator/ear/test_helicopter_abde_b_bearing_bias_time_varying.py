"""ABDE Phase 4B: time-varying bearing bias on EAR B via per-observation field."""

from __future__ import annotations

import json
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

_ROOT = Path(__file__).parents[3]
_BASELINE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_P10 = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_b_bearing_bias_p10.json"
_P20 = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_b_bearing_bias_p20.json"
_TV = (
    _ROOT
    / "simulator"
    / "ear"
    / "scenarios"
    / "helicopter_abde_b_bearing_bias_time_varying.json"
)
_PROFILE = _ROOT / "shared" / "simulator" / "ear-profiles" / "abde.json"

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator_abde_bias_tv",
    _ROOT / "simulator" / "ear" / "simulate_scenario.py",
)
assert spec is not None and spec.loader is not None
simulate_scenario = module_from_spec(spec)
spec.loader.exec_module(simulate_scenario)

EXPECTED_TIMES = (0.0, 10.0, 20.0, 30.0, 40.0)
EXPECTED_EARS = ("ear_a", "ear_b", "ear_d", "ear_e")
B_BIAS_SCHEDULE = {
    0.0: 0.0,
    10.0: 5.0,
    20.0: 10.0,
    30.0: 20.0,
    40.0: 5.0,
}


def test_prior_scenarios_unchanged() -> None:
    baseline = json.loads(_BASELINE.read_text(encoding="utf-8"))
    p10 = json.loads(_P10.read_text(encoding="utf-8"))
    p20 = json.loads(_P20.read_text(encoding="utf-8"))
    assert baseline["scenario_id"] == "helicopter_abde_overlap_01"
    assert "bearing_bias_deg" not in json.dumps(baseline)
    assert p10["scenario_id"] == "helicopter_abde_b_bearing_bias_p10_01"
    assert p20["scenario_id"] == "helicopter_abde_b_bearing_bias_p20_01"
    assert all(
        item.get("bearing_bias_deg") == 10
        for item in p10["observations"]
        if item["ear"] == "ear_b"
    )
    assert all(
        item.get("bearing_bias_deg") == 20
        for item in p20["observations"]
        if item["ear"] == "ear_b"
    )
    assert json.loads(_PROFILE.read_text(encoding="utf-8"))["name"] == "abde"


def test_authored_schedule_and_loaded_bearings() -> None:
    authored = json.loads(_TV.read_text(encoding="utf-8"))
    assert authored["scenario_id"] == "helicopter_abde_b_bearing_bias_time_varying_01"
    authored_b = {
        float(item["at_seconds"]): float(item["bearing_bias_deg"])
        for item in authored["observations"]
        if item["ear"] == "ear_b"
    }
    assert authored_b == B_BIAS_SCHEDULE

    baseline = simulate_scenario.load_scenario(_BASELINE)
    variant = simulate_scenario.load_scenario(_TV)
    assert variant["movement"] == baseline["movement"]
    assert variant["ears"] == baseline["ears"]
    assert [
        (item["at_seconds"], item["ear"]) for item in variant["observations"]
    ] == [(t, ear) for t in EXPECTED_TIMES for ear in EXPECTED_EARS]

    base_by_time_ear = {
        (item["at_seconds"], item["ear"]): item for item in baseline["observations"]
    }
    for item in variant["observations"]:
        key = (item["at_seconds"], item["ear"])
        base = base_by_time_ear[key]
        assert item["at_seconds"] == base["at_seconds"]
        assert item.get("duration_ms") == base.get("duration_ms")
        assert item["classification_confidence"] == base["classification_confidence"]
        assert item.get("signal_level_dbfs") == base.get("signal_level_dbfs")
        assert "bearing_deg" in item
        if item["ear"] == "ear_b":
            bias = B_BIAS_SCHEDULE[item["at_seconds"]]
            expected = (base["bearing_deg"] + bias) % 360
            assert item["bearing_deg"] == pytest.approx(expected, abs=1e-9)
            if bias == 0.0:
                assert "bearing_bias_deg" not in item
            else:
                assert item["bearing_bias_deg"] == bias
            assert item["bearing_confidence"] == base["bearing_confidence"]
        else:
            assert "bearing_bias_deg" not in item
            assert item["bearing_deg"] == pytest.approx(base["bearing_deg"], abs=1e-12)


def test_time_varying_wraparound_still_uses_per_observation_bias(tmp_path: Path) -> None:
    raw = json.loads(_TV.read_text(encoding="utf-8"))
    for item in raw["observations"]:
        if item["ear"] == "ear_b" and item["at_seconds"] == 30:
            item["bearing_bias_deg"] = 350.0
    path = tmp_path / "wrap_tv.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    baseline = simulate_scenario.load_scenario(_BASELINE)
    wrapped = simulate_scenario.load_scenario(path)
    base_b30 = next(
        item
        for item in baseline["observations"]
        if item["ear"] == "ear_b" and item["at_seconds"] == 30
    )
    var_b30 = next(
        item
        for item in wrapped["observations"]
        if item["ear"] == "ear_b" and item["at_seconds"] == 30
    )
    expected = (base_b30["bearing_deg"] + 350.0) % 360
    assert 0 <= var_b30["bearing_deg"] < 360
    assert var_b30["bearing_deg"] == pytest.approx(expected, abs=1e-9)
