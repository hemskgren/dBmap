"""Offline ABDE resilience: Event membership vs localization contributors."""

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
    estimate_localization,
)

_ROOT = Path(__file__).parents[2]
_SCENARIO = (
    _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_one_missing_bearing.json"
)
_BASELINE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
_MISSING_NODE = "SIM-EAR-002"
_USABLE_NODES = {"SIM-EAR-001", "SIM-EAR-004", "SIM-EAR-005"}


def _load(name: str, path: Path):
    spec = spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _publish_scenario(sim, scenario_path: Path) -> tuple[dict, list[dict], dict]:
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
            started_at_utc=datetime(2026, 11, 7, 14, 0, tzinfo=UTC),
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
    return scenario, published, nodes


def test_abde_baseline_still_four_contributors_useful() -> None:
    sim = _load(
        "dbmap_sim_abde_miss_base",
        _ROOT / "simulator" / "ear" / "simulate_scenario.py",
    )
    hub = _load("dbmap_hub_abde_miss_base", _ROOT / "scripts" / "hub_event_track.py")
    _, published, nodes = _publish_scenario(sim, _BASELINE)
    members = hub.build_assembled_observations(published, nodes)
    corr = hub.run_cross_ear_correlation(
        published, nodes, allow_long_range_sim_ears=True
    )
    assert len(members) == 4
    assert len(corr.events) == 1
    event = corr.events[0]
    assert set(event.ear_ids) == set(_ALIAS_TO_NODE.values())
    assert event.localization is not None
    assert event.localization.status == "useful"
    assert set(event.localization.contributing_ears) == set(_ALIAS_TO_NODE.values())
    assert event.localization.details["forward_pair_count"] == 6


def test_missing_bearing_keeps_event_membership_drops_localization_contributor() -> None:
    sim = _load(
        "dbmap_sim_abde_miss_loc",
        _ROOT / "simulator" / "ear" / "simulate_scenario.py",
    )
    hub = _load("dbmap_hub_abde_miss_loc", _ROOT / "scripts" / "hub_event_track.py")
    _, published, nodes = _publish_scenario(sim, _SCENARIO)

    # No fabricated bearing on the unavailable EAR.
    for payload in published:
        if payload["identity"]["node_id"] == _MISSING_NODE:
            assert payload["spatial"]["bearing"] is None

    members = hub.build_assembled_observations(published, nodes)
    assert len(members) == 4
    by_node = {item.node_id: item for item in members}
    assert by_node[_MISSING_NODE].bearing_deg is None
    assert list(by_node[_MISSING_NODE].bearing_samples or []) == []
    for node_id in _USABLE_NODES:
        assert by_node[node_id].bearing_deg is not None

    corr = hub.run_cross_ear_correlation(
        published, nodes, allow_long_range_sim_ears=True
    )
    assert len(corr.events) == 1
    event = corr.events[0]
    # Event membership retains all four EARs.
    assert set(event.ear_ids) == set(_ALIAS_TO_NODE.values())
    assert len(event.observation_ids) == 4

    loc = event.localization
    assert loc is not None
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert loc.details["common_epoch_utc"] is not None
    assert set(loc.details["association_member_ears"]) == set(_ALIAS_TO_NODE.values())
    assert set(loc.details["ears_with_usable_bearing"]) == _USABLE_NODES
    excluded = {
        item["ear_id"] for item in loc.details.get("common_epoch_excluded_ears") or []
    }
    assert excluded == {_MISSING_NODE}
    assert _MISSING_NODE not in loc.contributing_ears
    assert set(loc.contributing_ears) == _USABLE_NODES
    # Three usable EARs → three pair intersections when geometry permits.
    assert loc.details["forward_pair_count"] == 3
    assert loc.details["aligned_forward_pair_count"] == 3
    assert loc.details["consensus_pair_count"] == 3
    assert loc.details.get("outlier_pairs") == []
    # Existing quality rules (do not assume useful a priori; this case is useful).
    assert loc.status == "useful"

    # Direct localization on assembled Observations matches Event localization story.
    direct = estimate_localization(members, nodes)
    assert direct.status == loc.status
    assert set(direct.contributing_ears) == set(loc.contributing_ears)
    assert direct.details["forward_pair_count"] == 3
