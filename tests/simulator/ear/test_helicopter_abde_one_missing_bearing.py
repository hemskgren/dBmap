"""ABDE resilience: complete Observation without usable bearing on one EAR."""

from __future__ import annotations

import json
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

_ROOT = Path(__file__).parents[3]
_SCENARIO = (
    _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_one_missing_bearing.json"
)
_BASELINE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_ADE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_ade_overlap.json"
_PROFILE = _ROOT / "shared" / "simulator" / "ear-profiles" / "abde.json"

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator_abde_missing",
    _ROOT / "simulator" / "ear" / "simulate_scenario.py",
)
assert spec is not None and spec.loader is not None
simulate_scenario = module_from_spec(spec)
spec.loader.exec_module(simulate_scenario)

EXPECTED_TIMES = (0.0, 10.0, 20.0, 30.0, 40.0)
EXPECTED_EARS = ("ear_a", "ear_b", "ear_d", "ear_e")
EXPECTED_NODE_IDS = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
MISSING_BEARING_EAR = "ear_b"


def test_baselines_untouched() -> None:
    ade = json.loads(_ADE.read_text(encoding="utf-8"))
    abde = json.loads(_BASELINE.read_text(encoding="utf-8"))
    assert ade["scenario_id"] == "helicopter_ade_overlap_01"
    assert abde["scenario_id"] == "helicopter_abde_overlap_01"
    assert "bearing_available" not in json.dumps(abde)


def test_missing_bearing_scenario_preserves_abde_geometry_and_schedule() -> None:
    scenario = simulate_scenario.load_scenario(_SCENARIO)
    baseline = simulate_scenario.load_scenario(_BASELINE)
    assert scenario["scenario_id"] == "helicopter_abde_one_missing_bearing_01"
    assert scenario["source_family"] == "helicopter"
    assert scenario["movement"] == baseline["movement"]
    assert scenario["ears"] == baseline["ears"]
    assert json.loads(_PROFILE.read_text(encoding="utf-8"))["name"] == "abde"

    observations = scenario["observations"]
    assert len(observations) == 20
    assert [(item["at_seconds"], item["ear"]) for item in observations] == [
        (t, ear) for t in EXPECTED_TIMES for ear in EXPECTED_EARS
    ]
    for item in observations:
        if item["ear"] == MISSING_BEARING_EAR:
            assert item["bearing_available"] is False
            assert "bearing_deg" not in item
            assert "bearing_confidence" not in item
        else:
            assert item.get("bearing_available", True) is True
            assert "bearing_deg" in item
            assert 0 <= item["bearing_deg"] < 360


def test_missing_bearing_emits_null_spatial_bearing_not_fabricated_deg() -> None:
    scenario = simulate_scenario.load_scenario(_SCENARIO)
    credentials = {
        alias: {"node_id": node_id, "next_sequence_number": 1}
        for alias, node_id in EXPECTED_NODE_IDS.items()
    }
    published: list[dict] = []
    simulate_scenario.run_scenario(
        scenario,
        credentials,
        {alias: Path(f"{alias}.json") for alias in EXPECTED_EARS},
        "ca.crt",
        1.0,
        publish=lambda _c, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _s: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 7, 15, 0, tzinfo=UTC),
        started_at_monotonic_us=0,
        save=lambda *_a, **_k: None,
        publish_keepalive=lambda *_a, **_k: None,
        make_keepalive=lambda _c, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    assert len(published) == 20
    by_node: dict[str, list[dict]] = {node: [] for node in EXPECTED_NODE_IDS.values()}
    for payload in published:
        by_node[payload["identity"]["node_id"]].append(payload)
    for node_id, reports in by_node.items():
        assert len(reports) == 5
        for report in reports:
            bearing = report["spatial"]["bearing"]
            if node_id == EXPECTED_NODE_IDS[MISSING_BEARING_EAR]:
                assert bearing is None
            else:
                assert isinstance(bearing, dict)
                assert "deg" in bearing
                assert 0 <= bearing["deg"] < 360


def test_bearing_available_false_rejects_confidence(tmp_path: Path) -> None:
    raw = json.loads(_SCENARIO.read_text(encoding="utf-8"))
    for item in raw["observations"]:
        if item["ear"] == "ear_b":
            item["bearing_confidence"] = 0.4
            break
    path = tmp_path / "invalid_missing_bearing.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    with pytest.raises(simulate_scenario.ScenarioError, match="bearing_confidence"):
        simulate_scenario.load_scenario(path)
