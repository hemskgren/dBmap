"""Offline ABDE common-epoch localization smoke (no MQTT / Hub)."""

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
_SCENARIO = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}


def _load(name: str, path: Path):
    spec = spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_abde_offline_common_epoch_four_contributors_useful() -> None:
    sim = _load("dbmap_sim_abde_loc", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("dbmap_hub_abde_loc", _ROOT / "scripts" / "hub_event_track.py")
    scenario = sim.load_scenario(_SCENARIO)
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
    members = hub.build_assembled_observations(published, nodes)
    assert len(members) == 4
    loc = estimate_localization(members, nodes)
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert loc.details["common_epoch_utc"] is not None
    assert loc.status == "useful"
    assert set(loc.contributing_ears) == set(_ALIAS_TO_NODE.values())
    assert loc.details["forward_pair_count"] == 6
    assert len(loc.details.get("outlier_pairs") or []) == 0
    assert set(loc.details["association_member_ears"]) == set(_ALIAS_TO_NODE.values())
