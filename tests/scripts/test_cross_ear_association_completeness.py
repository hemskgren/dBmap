"""B2.5 Cross-EAR association completeness audit tests.

Documents actual Candidate membership behavior. Does not relax association rules.
"""

from __future__ import annotations

import sys
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_event_correlation import (
    DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
    build_membership_stage_trace,
    evaluate_observation_pair,
)
from lib.cross_ear_localization import estimate_localization

spec = spec_from_file_location(
    "dbmap_hub_event_track_b25",
    Path(__file__).parents[2] / "scripts" / "hub_event_track.py",
)
assert spec is not None and spec.loader is not None
hub_event_track = module_from_spec(spec)
spec.loader.exec_module(hub_event_track)


def _node(lat: float, lon: float, orientation: float = 0.0) -> dict:
    return {
        "installation": {
            "latitude": lat,
            "longitude": lon,
            "orientation_deg": orientation,
        }
    }


def _obs(
    observation_id: str,
    node_id: str,
    event_time_utc: str,
    *,
    duration_ms: float = 5000,
    bearing_deg: float | None = None,
    bearing_confidence: float | None = 0.8,
) -> dict:
    bearing = None
    if bearing_deg is not None:
        bearing = {
            "deg": bearing_deg,
            "reference": "node",
            "confidence": bearing_confidence,
        }
    return {
        "observation_id": observation_id,
        "node_id": node_id,
        "event_time_utc": event_time_utc,
        "duration_ms": duration_ms,
        "classification": {
            "source_family": "vehicle",
            "confidence": 0.7,
            "association_policy": (
                "simulator_or_unverified_classification; "
                "ignored for association score"
            ),
        },
        "bearing": bearing,
    }


TRIANGLE = {
    "SIM-EAR-001": _node(59.344534, 18.056, 0.0),
    "SIM-EAR-002": _node(59.3408, 18.060228, 0.0),
    "SIM-EAR-003": _node(59.3408, 18.051772, 0.0),
}


def _pair(a, b, nodes=TRIANGLE, gap=DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS):
    assembled = hub_event_track.build_assembled_observations([a, b], nodes)
    by_id = {item.observation_id: item for item in assembled}
    return evaluate_observation_pair(
        by_id[a["observation_id"]],
        by_id[b["observation_id"]],
        nodes,
        association_temporal_gap_seconds=gap,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        installation_distance_m=hub_event_track.installation_distance_m,
        is_sim_ear=hub_event_track.is_sim_ear,
    )


def _publish_four_ear():
    sim_spec = spec_from_file_location(
        "dbmap_sim_b25",
        Path(__file__).parents[2] / "simulator" / "ear" / "simulate_scenario.py",
    )
    assert sim_spec is not None and sim_spec.loader is not None
    simulate_scenario = module_from_spec(sim_spec)
    sim_spec.loader.exec_module(simulate_scenario)
    path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_four_ear.json"
    )
    scenario = simulate_scenario.load_scenario(path)
    alias = {
        "ear_a": "SIM-EAR-001",
        "ear_b": "SIM-EAR-002",
        "ear_c": "SIM-EAR-003",
        "ear_d": "SIM-EAR-004",
    }
    credentials = {
        a: {"node_id": alias[a], "next_sequence_number": 1} for a in scenario["ears"]
    }
    published: list[dict] = []
    simulate_scenario.run_scenario(
        scenario,
        credentials,
        {a: Path(f"{a}.json") for a in scenario["ears"]},
        "ca.crt",
        1.0,
        publish=lambda _c, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _s: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 5, 18, 0, tzinfo=UTC),
        started_at_monotonic_us=0,
        save=lambda *_a, **_k: None,
        publish_keepalive=lambda *_a, **_k: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    nodes = {
        alias[a]: {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
        for a, ear in scenario["ears"].items()
    }
    return scenario, published, nodes


def test_abc_chain_ab_bc_pass_ac_fail_prevents_c_join() -> None:
    """Must be compatible with every Candidate member — no transitive join."""
    nodes = {
        "SIM-EAR-001": _node(0.0, 0.0, 0.0),
        "SIM-EAR-002": _node(0.0, 0.005, 0.0),
        "SIM-EAR-003": _node(0.0, 0.01, 0.0),
    }
    # A–C use outward rays (behind_both). B has no bearing → abstains with both.
    a = _obs("oa", "SIM-EAR-001", "2026-11-05T18:00:10Z", bearing_deg=300.0)
    b = _obs("ob", "SIM-EAR-002", "2026-11-05T18:00:11Z", bearing_deg=None)
    c = _obs("oc", "SIM-EAR-003", "2026-11-05T18:00:12Z", bearing_deg=60.0)

    assert _pair(a, b, nodes).accepted
    assert _pair(b, c, nodes).accepted
    ac = _pair(a, c, nodes)
    assert ac.rejected
    assert ac.bearing == "incompatible"

    result = hub_event_track.run_cross_ear_correlation(
        [a, b, c],
        nodes,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    for candidate in result.candidates:
        member_ids = {m.observation_id for m in candidate.members}
        if "oa" in member_ids and "ob" in member_ids:
            assert "oc" not in member_ids
    rejected_to_ab = [
        attempt
        for attempt in result.join_attempts
        if attempt.new_observation_id == "oc" and attempt.decision == "rejected"
    ]
    assert rejected_to_ab
    assert any(
        row["member_observation_id"] == "oa" and row["rejected"]
        for attempt in rejected_to_ab
        for row in attempt.pairwise
    )


def test_candidate_membership_independent_of_input_order() -> None:
    reports = [
        _obs("oa", "SIM-EAR-001", "2026-11-05T18:00:10Z"),
        _obs("ob", "SIM-EAR-002", "2026-11-05T18:00:11Z"),
        _obs("oc", "SIM-EAR-003", "2026-11-05T18:00:12Z"),
    ]
    forward = hub_event_track.run_cross_ear_correlation(
        reports,
        TRIANGLE,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    reverse = hub_event_track.run_cross_ear_correlation(
        list(reversed(reports)),
        TRIANGLE,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(forward.events) == len(reverse.events) == 1
    assert set(forward.events[0].ear_ids) == set(reverse.events[0].ear_ids)
    assert set(forward.events[0].observation_ids) == set(
        reverse.events[0].observation_ids
    )


def test_four_ear_membership_grows_after_promotion() -> None:
    """B2.7 Phase A: compatible fourth EAR joins after promote (freeze removed)."""
    _scenario, published, nodes = _publish_four_ear()
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.observations) == 4
    assert len(result.events) == 1
    event = result.events[0]
    assert set(event.ear_ids) == {
        "SIM-EAR-001",
        "SIM-EAR-002",
        "SIM-EAR-003",
        "SIM-EAR-004",
    }

    assembled = {item.node_id: item for item in result.observations}
    for first_id, second_id in (
        ("SIM-EAR-001", "SIM-EAR-002"),
        ("SIM-EAR-001", "SIM-EAR-003"),
        ("SIM-EAR-001", "SIM-EAR-004"),
        ("SIM-EAR-002", "SIM-EAR-003"),
        ("SIM-EAR-002", "SIM-EAR-004"),
        ("SIM-EAR-003", "SIM-EAR-004"),
    ):
        evidence = evaluate_observation_pair(
            assembled[first_id],
            assembled[second_id],
            nodes,
            association_temporal_gap_seconds=DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
            max_association_distance_m=500,
            allow_long_range_sim_ears=False,
            installation_distance_m=hub_event_track.installation_distance_m,
            is_sim_ear=hub_event_track.is_sim_ear,
        )
        assert evidence.accepted, (first_id, second_id, evidence.reason)

    frozen = [
        attempt
        for attempt in result.join_attempts
        if attempt.decision == "accepted_but_event_frozen"
    ]
    assert not frozen

    accepted_fourth = [
        attempt
        for attempt in result.join_attempts
        if attempt.new_node_id == "SIM-EAR-004" and attempt.decision == "accepted"
    ]
    assert accepted_fourth
    assert "post-promotion" in accepted_fourth[0].reason

    trace = build_membership_stage_trace(result)
    by_ear = {row["node_id"]: row for row in trace}
    assert by_ear["SIM-EAR-004"]["in_event"] is True
    assert by_ear["SIM-EAR-004"]["compatible_but_event_frozen"] is False
    for ear in ("SIM-EAR-001", "SIM-EAR-002", "SIM-EAR-003", "SIM-EAR-004"):
        assert by_ear[ear]["in_event"] is True


def test_missing_bearing_abstains_contradictory_bearing_rejects() -> None:
    with_bearing = _obs("oa", "SIM-EAR-001", "2026-11-05T18:00:10Z", bearing_deg=90.0)
    missing = _obs("ob", "SIM-EAR-002", "2026-11-05T18:00:11Z", bearing_deg=None)
    missing_pair = _pair(with_bearing, missing)
    assert missing_pair.accepted
    assert missing_pair.bearing == "abstain"

    nodes = {
        "SIM-EAR-001": _node(0.0, 0.0, 0.0),
        "SIM-EAR-002": _node(0.0, 0.01, 0.0),
    }
    left = _obs("ol", "SIM-EAR-001", "2026-11-05T18:00:10Z", bearing_deg=300.0)
    right = _obs("or", "SIM-EAR-002", "2026-11-05T18:00:10Z", bearing_deg=60.0)
    bad = _pair(left, right, nodes)
    assert bad.rejected
    assert bad.bearing == "incompatible"


def test_temporal_boundaries_below_at_above_threshold() -> None:
    gap = DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS
    first = _obs(
        "oa",
        "SIM-EAR-001",
        "2026-11-05T18:00:00Z",
        duration_ms=10_000,
        bearing_deg=None,
    )

    def second_at(start_iso: str) -> dict:
        return _obs(
            "ob",
            "SIM-EAR-002",
            start_iso,
            duration_ms=5_000,
            bearing_deg=None,
        )

    below = _pair(first, second_at("2026-11-05T18:00:39Z"), gap=gap)  # gap=29s
    assert below.accepted
    assert below.temporal == "compatible"
    assert below.details.get("overlapped") is False

    at = _pair(first, second_at("2026-11-05T18:00:40Z"), gap=gap)  # gap=30s
    assert at.accepted
    assert at.temporal == "compatible"

    above = _pair(first, second_at("2026-11-05T18:00:41Z"), gap=gap)  # gap=31s
    assert above.rejected
    assert above.temporal == "incompatible"


def test_pairwise_overlap_without_common_three_way_overlap() -> None:
    """Join requires pairwise member compatibility; common ∩ of all is not required."""
    a = _obs(
        "oa",
        "SIM-EAR-001",
        "2026-11-05T18:00:00Z",
        duration_ms=10_000,
        bearing_deg=None,
    )
    b = _obs(
        "ob",
        "SIM-EAR-002",
        "2026-11-05T18:00:08Z",
        duration_ms=10_000,
        bearing_deg=None,
    )
    c = _obs(
        "oc",
        "SIM-EAR-003",
        "2026-11-05T18:00:16Z",
        duration_ms=10_000,
        bearing_deg=None,
    )
    assert _pair(a, b).accepted and _pair(a, b).details.get("overlapped") is True
    assert _pair(b, c).accepted and _pair(b, c).details.get("overlapped") is True
    ac = _pair(a, c)
    assert ac.accepted
    assert ac.details.get("overlapped") is False

    result = hub_event_track.run_cross_ear_correlation(
        [a, b, c],
        TRIANGLE,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    assert set(result.events[0].observation_ids) == {"oa", "ob", "oc"}


def test_one_observation_compatible_with_two_candidates_joins_first_only() -> None:
    nodes = {
        "SIM-EAR-001": _node(59.3440, 18.0500, 0.0),
        "SIM-EAR-002": _node(59.3440, 18.0540, 0.0),
        "SIM-EAR-003": _node(59.3400, 18.0500, 0.0),
        "SIM-EAR-004": _node(59.3400, 18.0540, 0.0),
        "SIM-EAR-005": _node(59.3420, 18.0520, 0.0),
    }
    reports = [
        _obs("o1", "SIM-EAR-001", "2026-11-05T18:00:00Z"),
        _obs("o2", "SIM-EAR-002", "2026-11-05T18:00:01Z"),
        _obs("o3", "SIM-EAR-003", "2026-11-05T18:00:02Z"),
        _obs("o4", "SIM-EAR-004", "2026-11-05T18:00:03Z"),
        _obs("o5", "SIM-EAR-005", "2026-11-05T18:00:04Z"),
    ]
    result = hub_event_track.run_cross_ear_correlation(
        reports,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    memberships = [
        candidate.candidate_id
        for candidate in result.candidates
        if "o5" in candidate.observation_ids
    ]
    assert len(memberships) <= 1


def test_two_distinct_sources_partial_overlap_remain_separate_when_incompatible() -> None:
    nodes = {
        "SIM-EAR-001": _node(0.0, 0.0, 0.0),
        "SIM-EAR-002": _node(0.0, 0.005, 0.0),
        "SIM-EAR-003": _node(0.0, 0.015, 0.0),
        "SIM-EAR-004": _node(0.0, 0.02, 0.0),
    }
    west_a = _obs("w1", "SIM-EAR-001", "2026-11-05T18:00:10Z", bearing_deg=None)
    west_b = _obs("w2", "SIM-EAR-002", "2026-11-05T18:00:10Z", bearing_deg=300.0)
    east_c = _obs("e3", "SIM-EAR-003", "2026-11-05T18:00:10Z", bearing_deg=60.0)
    east_d = _obs("e4", "SIM-EAR-004", "2026-11-05T18:00:10Z", bearing_deg=None)
    cross = _pair(west_b, east_c, nodes)
    assert cross.rejected
    assert cross.bearing == "incompatible"

    result = hub_event_track.run_cross_ear_correlation(
        [west_a, west_b, east_c, east_d],
        nodes,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    for event in result.events:
        assert len(event.ear_ids) < 4


def test_association_membership_independent_of_localization_quality() -> None:
    _scenario, published, nodes = _publish_four_ear()
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    event = result.events[0]
    assert event.localization is not None
    # B2.7: all four associated EARs are Event members; localization contributors
    # are a (possibly equal) subset and never define membership.
    assert len(event.ear_ids) == 4
    assert set(event.localization.contributing_ears).issubset(set(event.ear_ids))


def test_ground_truth_never_influences_association() -> None:
    reports = [
        _obs("oa", "SIM-EAR-001", "2026-11-05T18:00:10Z"),
        _obs("ob", "SIM-EAR-002", "2026-11-05T18:00:11Z"),
        _obs("oc", "SIM-EAR-003", "2026-11-05T18:00:12Z"),
    ]
    for item in reports:
        item["classification"]["hints"] = {
            "simulated_source_id": "SIM-DIFFERENT-" + item["observation_id"]
        }
    result = hub_event_track.run_cross_ear_correlation(
        reports,
        TRIANGLE,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    assert result.parameters.get("ground_truth_policy") == (
        "ignored_for_association_and_localization"
    )


def test_b23_withhold_and_b24_sensitivity_still_importable() -> None:
    from lib.cross_ear_localization import (
        analyze_bearing_sensitivity,
        geometric_dilution_indicator_m,
    )

    assert geometric_dilution_indicator_m(100.0, 30.0) is not None
    assert callable(analyze_bearing_sensitivity)
    assert callable(estimate_localization)
