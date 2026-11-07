"""ABDE Phase 4A: controlled systematic bearing bias on EAR B only."""

from __future__ import annotations

import json
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

_ROOT = Path(__file__).parents[3]
_BASELINE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_P10 = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_b_bearing_bias_p10.json"
_P20 = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_b_bearing_bias_p20.json"
_ADE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_ade_overlap.json"
_PROFILE = _ROOT / "shared" / "simulator" / "ear-profiles" / "abde.json"

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator_abde_bias",
    _ROOT / "simulator" / "ear" / "simulate_scenario.py",
)
assert spec is not None and spec.loader is not None
simulate_scenario = module_from_spec(spec)
spec.loader.exec_module(simulate_scenario)

EXPECTED_TIMES = (0.0, 10.0, 20.0, 30.0, 40.0)
EXPECTED_EARS = ("ear_a", "ear_b", "ear_d", "ear_e")


def _by_ear(scenario: dict) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {alias: [] for alias in EXPECTED_EARS}
    for item in scenario["observations"]:
        out[item["ear"]].append(item)
    return out


def test_baseline_and_ade_files_untouched_by_bias_variants() -> None:
    ade = json.loads(_ADE.read_text(encoding="utf-8"))
    baseline = json.loads(_BASELINE.read_text(encoding="utf-8"))
    assert ade["scenario_id"] == "helicopter_ade_overlap_01"
    assert baseline["scenario_id"] == "helicopter_abde_overlap_01"
    assert "bearing_bias_deg" not in json.dumps(baseline)
    assert "bearing_bias_deg" not in json.dumps(ade)
    assert json.loads(_PROFILE.read_text(encoding="utf-8"))["name"] == "abde"


def test_baseline_generated_bearings_unchanged_fingerprint() -> None:
    scenario = simulate_scenario.load_scenario(_BASELINE)
    bearings = [
        (item["at_seconds"], item["ear"], round(item["bearing_deg"], 9))
        for item in scenario["observations"]
    ]
    # Stable fingerprint of unbiased ABDE geometry (node-relative).
    assert bearings[0] == (0.0, "ear_a", pytest.approx(77.909560573, abs=1e-6))
    assert bearings[1] == (0.0, "ear_b", pytest.approx(26.221669823, abs=1e-6))
    assert all("bearing_bias_deg" not in item for item in scenario["observations"])
    assert all(item.get("bearing_available", True) for item in scenario["observations"])


@pytest.mark.parametrize(
    ("path", "bias", "scenario_id"),
    [
        (_P10, 10.0, "helicopter_abde_b_bearing_bias_p10_01"),
        (_P20, 20.0, "helicopter_abde_b_bearing_bias_p20_01"),
    ],
)
def test_bias_changes_only_ear_b_bearings(
    path: Path, bias: float, scenario_id: str
) -> None:
    baseline = simulate_scenario.load_scenario(_BASELINE)
    variant = simulate_scenario.load_scenario(path)
    assert variant["scenario_id"] == scenario_id
    assert variant["movement"] == baseline["movement"]
    assert variant["ears"] == baseline["ears"]
    assert [
        (item["at_seconds"], item["ear"]) for item in variant["observations"]
    ] == [(t, ear) for t in EXPECTED_TIMES for ear in EXPECTED_EARS]

    base_by = _by_ear(baseline)
    var_by = _by_ear(variant)
    for alias in EXPECTED_EARS:
        assert len(var_by[alias]) == 5
        for base_item, var_item in zip(base_by[alias], var_by[alias], strict=True):
            assert var_item["at_seconds"] == base_item["at_seconds"]
            assert var_item["classification_confidence"] == base_item[
                "classification_confidence"
            ]
            assert var_item.get("signal_level_dbfs") == base_item.get(
                "signal_level_dbfs"
            )
            assert var_item.get("duration_ms") == base_item.get("duration_ms")
            assert "bearing_deg" in var_item
            if alias == "ear_b":
                assert var_item["bearing_bias_deg"] == bias
                assert var_item["bearing_confidence"] == base_item["bearing_confidence"]
                expected = (base_item["bearing_deg"] + bias) % 360
                assert var_item["bearing_deg"] == pytest.approx(expected, abs=1e-9)
            else:
                assert "bearing_bias_deg" not in var_item
                assert var_item["bearing_deg"] == pytest.approx(
                    base_item["bearing_deg"], abs=1e-12
                )
                assert var_item["bearing_confidence"] == base_item["bearing_confidence"]


def test_bearing_bias_wraparound(tmp_path: Path) -> None:
    raw = json.loads(_BASELINE.read_text(encoding="utf-8"))
    raw["scenario_id"] = "helicopter_abde_bias_wrap_test"
    # Force a large positive bias so node-relative bearing wraps past 360.
    for item in raw["observations"]:
        if item["ear"] == "ear_b" and item["at_seconds"] == 0:
            item["bearing_bias_deg"] = 350.0
    path = tmp_path / "wrap.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    baseline = simulate_scenario.load_scenario(_BASELINE)
    wrapped = simulate_scenario.load_scenario(path)
    base_b0 = next(
        item
        for item in baseline["observations"]
        if item["ear"] == "ear_b" and item["at_seconds"] == 0
    )
    var_b0 = next(
        item
        for item in wrapped["observations"]
        if item["ear"] == "ear_b" and item["at_seconds"] == 0
    )
    expected = (base_b0["bearing_deg"] + 350.0) % 360
    assert 0 <= var_b0["bearing_deg"] < 360
    assert var_b0["bearing_deg"] == pytest.approx(expected, abs=1e-9)
    assert expected == pytest.approx((26.221669823 + 350.0) % 360, abs=1e-6)


def test_bias_rejected_when_bearing_unavailable(tmp_path: Path) -> None:
    raw = json.loads(_P10.read_text(encoding="utf-8"))
    for item in raw["observations"]:
        if item["ear"] == "ear_b":
            item["bearing_available"] = False
            item.pop("bearing_confidence", None)
            # keep bearing_bias_deg → must error
            break
    path = tmp_path / "invalid.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    with pytest.raises(simulate_scenario.ScenarioError, match="bearing_bias_deg"):
        simulate_scenario.load_scenario(path)
