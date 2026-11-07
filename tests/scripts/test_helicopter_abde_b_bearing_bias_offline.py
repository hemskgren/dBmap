"""Offline ABDE Phase 4A: observe localization under EAR B bearing bias."""

from __future__ import annotations

import contextlib
import io
import sys
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_localization import (
    BEARING_SELECTION_COMMON_EPOCH,
    evaluate_against_ground_truth,
)

_ROOT = Path(__file__).parents[2]
_BASELINE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_P10 = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_b_bearing_bias_p10.json"
_P20 = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_b_bearing_bias_p20.json"
_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
_ALL = set(_ALIAS_TO_NODE.values())
_ADE = {"SIM-EAR-001", "SIM-EAR-004", "SIM-EAR-005"}
_START = datetime(2026, 11, 7, 14, 0, tzinfo=UTC)


def _load(name: str, path: Path):
    spec = spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _run(sim, hub, scenario_path: Path):
    scenario = sim.load_scenario(scenario_path)
    credentials = {
        alias: {"node_id": node_id, "next_sequence_number": 1}
        for alias, node_id in _ALIAS_TO_NODE.items()
    }
    published: list[dict] = []
    with contextlib.redirect_stdout(io.StringIO()):
        sim.run_scenario(
            scenario,
            credentials,
            {alias: Path(f"{alias}.json") for alias in scenario["ears"]},
            "ca.crt",
            1.0,
            publish=lambda _c, payload, _ca: (
                published.append(payload) or payload["identity"]["observation_id"]
            ),
            sleep=lambda _s: None,
            monotonic=lambda: 10**12,
            started_at_utc=_START,
            started_at_monotonic_us=0,
            save=lambda *_a, **_k: None,
            publish_keepalive=lambda *_a, **_k: None,
            make_keepalive=lambda _c, uptime_s: {"uptime_s": uptime_s},
            announce=False,
        )
    nodes = {
        _ALIAS_TO_NODE[alias]: {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
        for alias, ear in scenario["ears"].items()
    }
    members = hub.build_assembled_observations(published, nodes)
    corr = hub.run_cross_ear_correlation(
        published, nodes, allow_long_range_sim_ears=True
    )
    return scenario, published, members, corr


def test_baseline_still_useful_four_contributors() -> None:
    sim = _load("sim_abde_bias_base", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("hub_abde_bias_base", _ROOT / "scripts" / "hub_event_track.py")
    _, published, members, corr = _run(sim, hub, _BASELINE)
    assert len(published) == 20
    assert len(members) == 4
    assert len(corr.events) == 1
    loc = corr.events[0].localization
    assert loc is not None
    assert set(corr.events[0].ear_ids) == _ALL
    assert loc.status == "useful"
    assert set(loc.contributing_ears) == _ALL
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert loc.details["forward_pair_count"] == 6
    assert loc.details["consensus_pair_count"] == 6
    assert loc.details.get("outlier_pairs") == []


def test_plus10_bias_keeps_membership_marks_b_pairs_outliers() -> None:
    sim = _load("sim_abde_bias_p10", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("hub_abde_bias_p10", _ROOT / "scripts" / "hub_event_track.py")
    scenario, published, members, corr = _run(sim, hub, _P10)
    assert len(published) == 20
    assert all(
        payload["spatial"]["bearing"] is not None
        for payload in published
    )
    assert len(members) == 4
    assert all(item.bearing_deg is not None for item in members)
    assert len(corr.events) == 1
    event = corr.events[0]
    assert set(event.ear_ids) == _ALL  # Event membership
    loc = event.localization
    assert loc is not None
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert set(loc.details["ears_with_usable_bearing"]) == _ALL
    assert loc.details.get("common_epoch_excluded_ears") in ([], None)
    assert loc.details["forward_pair_count"] == 6
    assert loc.details["aligned_forward_pair_count"] == 6
    assert loc.details["consensus_pair_count"] == 3
    outliers = set(loc.details.get("outlier_pairs") or [])
    assert outliers == {
        "SIM-EAR-001↔SIM-EAR-002",
        "SIM-EAR-002↔SIM-EAR-004",
        "SIM-EAR-002↔SIM-EAR-005",
    }
    # Localization contributors exclude B even though B remains an Event member.
    assert set(loc.contributing_ears) == _ADE
    assert "SIM-EAR-002" not in loc.contributing_ears
    assert loc.status == "weak"
    assert loc.details.get("network_geometric_sensitivity_m") is not None
    assert loc.latitude is not None and loc.longitude is not None

    epoch = datetime.fromisoformat(loc.details["common_epoch_utc"])
    at_s = (epoch - _START).total_seconds()
    assert at_s == 10.0
    mov = scenario["movement"]
    gt_pos = sim._position_on_route(
        mov["start"], mov["end"], at_s / mov["duration_seconds"]
    )
    gt = evaluate_against_ground_truth(
        loc,
        ground_truth_latitude=gt_pos["latitude"],
        ground_truth_longitude=gt_pos["longitude"],
        ground_truth_time_utc=epoch,
    )
    assert gt.error_m is not None
    assert gt.within_250m is True


def test_plus20_bias_similar_outlier_pattern_different_epoch() -> None:
    sim = _load("sim_abde_bias_p20", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("hub_abde_bias_p20", _ROOT / "scripts" / "hub_event_track.py")
    _, published, members, corr = _run(sim, hub, _P20)
    assert len(published) == 20
    assert len(members) == 4
    event = corr.events[0]
    assert set(event.ear_ids) == _ALL
    loc = event.localization
    assert loc is not None
    assert set(loc.contributing_ears) == _ADE
    assert loc.status == "weak"
    assert loc.details["consensus_pair_count"] == 3
    outliers = set(loc.details.get("outlier_pairs") or [])
    assert "SIM-EAR-002↔SIM-EAR-004" in outliers
    assert all("SIM-EAR-002" in pair for pair in outliers)
    epoch = datetime.fromisoformat(loc.details["common_epoch_utc"])
    assert (epoch - _START).total_seconds() == 30.0
