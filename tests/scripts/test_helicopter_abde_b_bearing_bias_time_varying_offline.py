"""Offline ABDE Phase 4B: production prefers unbiased epoch under time-varying B bias."""

from __future__ import annotations

import contextlib
import io
import sys
from datetime import UTC, datetime, timedelta
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_localization import (
    BEARING_SELECTION_COMMON_EPOCH,
    estimate_localization,
    evaluate_against_ground_truth,
)
from lib.localization_epoch_diagnostic import members_restricted_to_epoch

_ROOT = Path(__file__).parents[2]
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
_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
_ALL = set(_ALIAS_TO_NODE.values())
_ADE = {"SIM-EAR-001", "SIM-EAR-004", "SIM-EAR-005"}
_START = datetime(2026, 11, 7, 14, 0, tzinfo=UTC)
_B_OUTLIERS = {
    "SIM-EAR-001↔SIM-EAR-002",
    "SIM-EAR-002↔SIM-EAR-004",
    "SIM-EAR-002↔SIM-EAR-005",
}


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
    return scenario, published, members, nodes, corr


def test_four_way_comparison_shapes() -> None:
    sim = _load("sim_tv_cmp", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("hub_tv_cmp", _ROOT / "scripts" / "hub_event_track.py")

    base = _run(sim, hub, _BASELINE)
    p10 = _run(sim, hub, _P10)
    p20 = _run(sim, hub, _P20)
    tv = _run(sim, hub, _TV)

    for label, pack in (("base", base), ("p10", p10), ("p20", p20), ("tv", tv)):
        _, published, members, _nodes, corr = pack
        assert len(published) == 20, label
        assert len(members) == 4, label
        assert len(corr.events) == 1, label
        assert set(corr.events[0].ear_ids) == _ALL, label
        assert all(item.bearing_deg is not None for item in members), label

    base_loc = base[4].events[0].localization
    p10_loc = p10[4].events[0].localization
    p20_loc = p20[4].events[0].localization
    tv_loc = tv[4].events[0].localization
    assert base_loc is not None and tv_loc is not None
    assert p10_loc is not None and p20_loc is not None

    assert base_loc.status == "useful"
    assert set(base_loc.contributing_ears) == _ALL
    assert (datetime.fromisoformat(base_loc.details["common_epoch_utc"]) - _START).total_seconds() == 0

    assert p10_loc.status == "weak"
    assert set(p10_loc.contributing_ears) == _ADE
    assert set(p10_loc.details.get("outlier_pairs") or []) == _B_OUTLIERS
    assert (datetime.fromisoformat(p10_loc.details["common_epoch_utc"]) - _START).total_seconds() == 10

    assert p20_loc.status == "weak"
    assert set(p20_loc.contributing_ears) == _ADE
    assert (datetime.fromisoformat(p20_loc.details["common_epoch_utc"]) - _START).total_seconds() == 30

    # Time-varying: production selects unbiased t=0 → matches baseline outcome.
    assert tv_loc.status == "useful"
    assert set(tv_loc.contributing_ears) == _ALL
    assert tv_loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert tv_loc.details["consensus_pair_count"] == 6
    assert tv_loc.details.get("outlier_pairs") == []
    assert (datetime.fromisoformat(tv_loc.details["common_epoch_utc"]) - _START).total_seconds() == 0
    assert tv_loc.latitude == pytest.approx(base_loc.latitude)
    assert tv_loc.longitude == pytest.approx(base_loc.longitude)


def test_forced_epochs_show_bias_effect_when_selected() -> None:
    sim = _load("sim_tv_forced", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("hub_tv_forced", _ROOT / "scripts" / "hub_event_track.py")
    scenario, _published, members, nodes, corr = _run(sim, hub, _TV)
    loc = corr.events[0].localization
    assert loc is not None

    expected = {
        0: ("useful", _ALL, 6, set()),
        10: ("weak", _ADE, 3, _B_OUTLIERS),
        20: ("weak", _ADE, 3, _B_OUTLIERS),
        30: ("weak", _ADE, 3, _B_OUTLIERS),
        40: ("weak", _ADE, 3, _B_OUTLIERS),
    }
    for t, (status, contrib, consensus, outliers) in expected.items():
        restricted = members_restricted_to_epoch(
            members, _START + timedelta(seconds=t)
        )
        forced = estimate_localization(restricted, nodes)
        assert forced.status == status
        assert set(forced.contributing_ears) == contrib
        assert forced.details["consensus_pair_count"] == consensus
        assert set(forced.details.get("outlier_pairs") or []) == outliers

    epoch = datetime.fromisoformat(loc.details["common_epoch_utc"])
    at_s = (epoch - _START).total_seconds()
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
