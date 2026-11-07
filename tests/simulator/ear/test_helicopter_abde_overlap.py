"""Focused checks for the ABDE four-EAR simultaneous helicopter scenario."""

from __future__ import annotations

import json
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

_ROOT = Path(__file__).parents[3]
_SCENARIO = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_abde_overlap.json"
_ADE = _ROOT / "simulator" / "ear" / "scenarios" / "helicopter_ade_overlap.json"
_PROFILE = _ROOT / "shared" / "simulator" / "ear-profiles" / "abde.json"

spec = spec_from_file_location(
    "dbmap_ear_scenario_simulator_abde",
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


def test_abde_profile_aliases_match_slots() -> None:
    profile = json.loads(_PROFILE.read_text(encoding="utf-8"))
    assert profile["name"] == "abde"
    assert profile["ears"] == {
        "ear_a": "a",
        "ear_b": "b",
        "ear_d": "d",
        "ear_e": "e",
    }


def test_helicopter_ade_overlap_untouched() -> None:
    ade = json.loads(_ADE.read_text(encoding="utf-8"))
    assert ade["scenario_id"] == "helicopter_ade_overlap_01"
    assert set(ade["ears"]) == {"ear_a", "ear_d", "ear_e"}
    assert "ear_b" not in ade["ears"]


def test_helicopter_abde_overlap_load_geometry_and_schedule() -> None:
    scenario = simulate_scenario.load_scenario(_SCENARIO)
    assert scenario["scenario_id"] == "helicopter_abde_overlap_01"
    assert scenario["source_id"] == "SIM-HELI-ABDE-001"
    assert scenario["source_family"] == "helicopter"

    movement = scenario["movement"]
    assert movement["start"] == {
        "latitude": 59.35224966,
        "longitude": 18.05850000,
    }
    assert movement["speed_m_s"] == 30
    assert movement["duration_seconds"] == pytest.approx(40.0, abs=1e-6)
    assert movement["duration_seconds"] >= 40.0

    ears = scenario["ears"]
    assert set(ears) == set(EXPECTED_EARS)
    assert ears["ear_a"]["orientation_deg"] == 0
    assert ears["ear_b"]["orientation_deg"] == 270
    assert ears["ear_d"]["orientation_deg"] == 180
    assert ears["ear_e"]["orientation_deg"] == 0
    # Authoritative installs (ADE-verified A/D/E + documented Hub B).
    assert ears["ear_a"]["latitude"] == pytest.approx(59.351176)
    assert ears["ear_b"]["latitude"] == pytest.approx(59.335118371024905)
    assert ears["ear_d"]["latitude"] == pytest.approx(59.36694541493451)
    assert ears["ear_e"]["latitude"] == pytest.approx(59.32006416685073)

    observations = scenario["observations"]
    assert len(observations) == 20
    assert [(item["at_seconds"], item["ear"]) for item in observations] == [
        (t, ear) for t in EXPECTED_TIMES for ear in EXPECTED_EARS
    ]
    assert max(item["at_seconds"] for item in observations) <= movement["duration_seconds"]


def test_helicopter_abde_overlap_one_lifecycle_per_ear_synced() -> None:
    scenario = simulate_scenario.load_scenario(_SCENARIO)
    credentials = {
        alias: {"node_id": node_id, "next_sequence_number": 1}
        for alias, node_id in EXPECTED_NODE_IDS.items()
    }
    published: list[dict] = []
    ids = simulate_scenario.run_scenario(
        scenario,
        credentials,
        {alias: Path(f"{alias}.json") for alias in EXPECTED_EARS},
        "ca.crt",
        1.0,
        publish=lambda _credentials, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _seconds: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 7, 14, 0, tzinfo=UTC),
        started_at_monotonic_us=0,
        save=lambda *_args, **_kwargs: None,
        publish_keepalive=lambda *_args, **_kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )

    assert len(published) == 20
    assert len(ids) == 20
    assert len(set(ids)) == 4

    by_node: dict[str, list[dict]] = {node: [] for node in EXPECTED_NODE_IDS.values()}
    for payload in published:
        by_node[payload["identity"]["node_id"]].append(payload)

    for node_id in EXPECTED_NODE_IDS.values():
        reports = by_node[node_id]
        assert len(reports) == 5
        kinds = [item["context"]["emission_kind"] for item in reports]
        assert kinds == ["start", "periodic", "periodic", "periodic", "end"]
        assert len({item["identity"]["observation_id"] for item in reports}) == 1
        times = [
            (
                datetime.fromisoformat(item["context"]["event_time_utc"])
                - datetime(2026, 11, 7, 14, 0, tzinfo=UTC)
            ).total_seconds()
            for item in reports
        ]
        assert times == list(EXPECTED_TIMES)

    by_time: dict[float, set[str]] = {}
    for payload in published:
        elapsed = (
            datetime.fromisoformat(payload["context"]["event_time_utc"])
            - datetime(2026, 11, 7, 14, 0, tzinfo=UTC)
        ).total_seconds()
        by_time.setdefault(elapsed, set()).add(payload["identity"]["node_id"])
    assert set(by_time) == set(EXPECTED_TIMES)
    for nodes in by_time.values():
        assert nodes == set(EXPECTED_NODE_IDS.values())


def test_helicopter_abde_overlap_matches_abde_profile_aliases() -> None:
    profile = json.loads(_PROFILE.read_text(encoding="utf-8"))
    scenario = json.loads(_SCENARIO.read_text(encoding="utf-8"))
    assert set(profile["ears"]) == set(scenario["ears"])
