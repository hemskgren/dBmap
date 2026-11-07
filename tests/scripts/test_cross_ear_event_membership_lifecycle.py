"""B2.7 Phase A – Event membership lifecycle and deterministic analysis tests."""

from __future__ import annotations

import copy
import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_event_correlation import (
    build_membership_stage_trace,
    evaluate_observation_pair,
)
from lib.cross_ear_localization import estimate_localization

spec = spec_from_file_location(
    "dbmap_hub_event_track_b27",
    Path(__file__).parents[2] / "scripts" / "hub_event_track.py",
)
assert spec is not None and spec.loader is not None
hub = module_from_spec(spec)
spec.loader.exec_module(hub)


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
    bearing_deg: float | None = 90.0,
    bearing_confidence: float | None = 0.8,
    sequence_number: int = 1,
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
        "sequence_number": sequence_number,
        "event_time_utc": event_time_utc,
        "received_time_utc": event_time_utc,
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


# Equilateral-ish triangle; SIM-EAR-004 sits near the centroid latitude on the
# northing of the B–C baseline (same lon as A). Bearings below point at the
# triangle centroid (orientation 0 ⇒ node-relative == geographic).
TRIANGLE = {
    "SIM-EAR-001": _node(59.344534, 18.056, 0.0),
    "SIM-EAR-002": _node(59.3408, 18.060228, 0.0),
    "SIM-EAR-003": _node(59.3408, 18.051772, 0.0),
    "SIM-EAR-004": _node(59.3420, 18.056, 0.0),
}

# Geometrically consistent node-relative bearings toward the ABC centroid.
CENTROID_BEARINGS = {
    "SIM-EAR-001": 180.0,
    "SIM-EAR-002": 300.0,
    "SIM-EAR-003": 60.0,
    # Due north of the centroid → 0°. A previous synthetic 90° was association-
    # incompatible with SIM-EAR-003 (behind_one) under B2.5 bearing rules.
    "SIM-EAR-004": 0.0,
}


def _three_then_fourth_payloads(*, fourth_bearing: float | None) -> list[dict]:
    return [
        _obs(
            "o1",
            "SIM-EAR-001",
            "2026-11-05T18:00:00Z",
            bearing_deg=CENTROID_BEARINGS["SIM-EAR-001"],
        ),
        _obs(
            "o2",
            "SIM-EAR-002",
            "2026-11-05T18:00:01Z",
            bearing_deg=CENTROID_BEARINGS["SIM-EAR-002"],
        ),
        _obs(
            "o3",
            "SIM-EAR-003",
            "2026-11-05T18:00:02Z",
            bearing_deg=CENTROID_BEARINGS["SIM-EAR-003"],
        ),
        _obs(
            "o4",
            "SIM-EAR-004",
            "2026-11-05T18:00:04Z",
            bearing_deg=fourth_bearing,
        ),
    ]


def test_a_valid_compatible_bearing_joins_after_promotion() -> None:
    """Test A: fourth EAR with a valid, geometrically compatible bearing."""
    payloads = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )
    # Document that all pairs including the fourth are association-compatible.
    assembled = {
        item.observation_id: item
        for item in hub.build_assembled_observations(payloads, TRIANGLE)
    }
    for left, right in (("o1", "o4"), ("o2", "o4"), ("o3", "o4")):
        evidence = evaluate_observation_pair(
            assembled[left],
            assembled[right],
            TRIANGLE,
            association_temporal_gap_seconds=30.0,
            max_association_distance_m=5000,
            allow_long_range_sim_ears=True,
            installation_distance_m=hub.installation_distance_m,
            is_sim_ear=hub.is_sim_ear,
        )
        assert evidence.accepted, (left, right, evidence.reason)
        assert evidence.bearing == "compatible"

    promoted_only = hub.run_cross_ear_correlation(
        payloads[:3],
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(promoted_only.events) == 1
    assert set(promoted_only.events[0].ear_ids) == {
        "SIM-EAR-001",
        "SIM-EAR-002",
        "SIM-EAR-003",
    }
    event_id_before = promoted_only.events[0].event_id

    result = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(result.events) == 1
    event = result.events[0]
    assert event.event_id == event_id_before
    assert set(event.ear_ids) == {
        "SIM-EAR-001",
        "SIM-EAR-002",
        "SIM-EAR-003",
        "SIM-EAR-004",
    }
    assert set(event.observation_ids) == {"o1", "o2", "o3", "o4"}
    accepted = [
        attempt
        for attempt in result.join_attempts
        if attempt.new_node_id == "SIM-EAR-004" and attempt.decision == "accepted"
    ]
    assert accepted
    assert "post-promotion" in accepted[0].reason
    assert event.localization is not None
    # Event membership includes SIM-EAR-004; common-epoch localization may omit
    # it when its report falls outside the selected epoch's 1 s gate while still
    # remaining an association member with a usable bearing.
    assert "SIM-EAR-004" in event.ear_ids
    assert "SIM-EAR-004" in event.localization.details["association_member_ears"]
    assert "SIM-EAR-004" in event.localization.details["ears_with_usable_bearing"]
    excluded = {
        item.get("ear_id")
        for item in (event.localization.details.get("common_epoch_excluded_ears") or [])
    }
    if event.localization.details.get("bearing_selection_mode") == "common_epoch":
        assert (
            "SIM-EAR-004" in event.localization.contributing_ears
            or "SIM-EAR-004" in excluded
        )
    else:
        assert "SIM-EAR-004" in event.localization.contributing_ears


def test_a_incompatible_synthetic_bearing_is_rejected_by_b25_rule() -> None:
    """The original 90° fourth-EAR bearing fails B2.5 behind_one vs SIM-EAR-003."""
    payloads = _three_then_fourth_payloads(fourth_bearing=90.0)
    assembled = {
        item.observation_id: item
        for item in hub.build_assembled_observations(payloads, TRIANGLE)
    }
    evidence = evaluate_observation_pair(
        assembled["o3"],
        assembled["o4"],
        TRIANGLE,
        association_temporal_gap_seconds=30.0,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        installation_distance_m=hub.installation_distance_m,
        is_sim_ear=hub.is_sim_ear,
    )
    assert evidence.rejected
    assert evidence.bearing == "incompatible"
    assert "behind one" in evidence.bearing_notes

    result = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(result.events) == 1
    assert "SIM-EAR-004" not in result.events[0].ear_ids
    assert any(
        attempt.new_node_id == "SIM-EAR-004" and attempt.decision == "rejected"
        for attempt in result.join_attempts
    )


def test_b_abstaining_bearing_joins_and_stays_non_contributor() -> None:
    """Test B: missing bearing abstains for association score but may still join."""
    payloads = _three_then_fourth_payloads(fourth_bearing=None)
    assembled = {
        item.observation_id: item
        for item in hub.build_assembled_observations(payloads, TRIANGLE)
    }
    for left in ("o1", "o2", "o3"):
        evidence = evaluate_observation_pair(
            assembled[left],
            assembled["o4"],
            TRIANGLE,
            association_temporal_gap_seconds=30.0,
            max_association_distance_m=5000,
            allow_long_range_sim_ears=True,
            installation_distance_m=hub.installation_distance_m,
            is_sim_ear=hub.is_sim_ear,
        )
        assert evidence.accepted, (left, evidence.reason)
        assert evidence.bearing == "abstain"

    result = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(result.events) == 1
    event = result.events[0]
    assert "SIM-EAR-004" in event.ear_ids
    assert "o4" in event.observation_ids
    loc = event.localization
    assert loc is not None
    assert "SIM-EAR-004" not in loc.contributing_ears
    assert "SIM-EAR-004" not in (loc.details.get("ears_with_usable_bearing") or [])
    assert set(loc.contributing_ears).issubset(set(event.ear_ids))
    assert len(loc.contributing_ears) < len(event.ear_ids)


def test_observation_without_usable_bearing_remains_event_member() -> None:
    payloads = [
        _obs(
            "o1",
            "SIM-EAR-001",
            "2026-11-05T18:00:00Z",
            bearing_deg=CENTROID_BEARINGS["SIM-EAR-001"],
        ),
        _obs(
            "o2",
            "SIM-EAR-002",
            "2026-11-05T18:00:01Z",
            bearing_deg=CENTROID_BEARINGS["SIM-EAR-002"],
        ),
        _obs("o3", "SIM-EAR-003", "2026-11-05T18:00:02Z", bearing_deg=None),
    ]
    result = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(result.events) == 1
    assert "SIM-EAR-003" in result.events[0].ear_ids
    loc = result.events[0].localization
    assert loc is not None
    assert "SIM-EAR-003" not in loc.contributing_ears


def test_localization_exclusion_does_not_reduce_event_membership() -> None:
    payloads = _three_then_fourth_payloads(fourth_bearing=None)
    result = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    event = result.events[0]
    membership_before = set(event.ear_ids)
    assert "SIM-EAR-004" in membership_before

    victim = next(m for m in event.members if m.node_id == "SIM-EAR-002")
    victim.bearing_deg = None
    victim.bearing_confidence = None
    victim.bearing_samples = []
    loc = estimate_localization(list(event.members), TRIANGLE)

    assert set(event.ear_ids) == membership_before
    assert "SIM-EAR-002" in event.ear_ids
    assert "SIM-EAR-002" not in loc.contributing_ears
    details = loc.details
    assert "SIM-EAR-002" in details["association_member_ears"]
    assert "SIM-EAR-002" not in details["ears_with_usable_bearing"]
    assert set(loc.contributing_ears).issubset(membership_before)


def test_contributing_ears_semantics_layers() -> None:
    payloads = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )
    result = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    event = result.events[0]
    loc = event.localization
    assert loc is not None
    details = loc.details
    assert set(details["association_member_ears"]) == set(event.ear_ids)
    assert set(details["ears_with_usable_bearing"]) == set(event.ear_ids)
    assert set(loc.contributing_ears).issubset(set(details["consensus_selected_ears"]))
    assert set(details["consensus_selected_ears"]).issubset(
        set(details["forward_intersection_ears"])
    )
    assert set(details["forward_intersection_ears"]).issubset(
        set(details["ears_with_usable_bearing"])
    )
    if loc.latitude is not None:
        assert loc.contributing_ears
        assert set(loc.contributing_ears) == set(details["consensus_selected_ears"])
    else:
        assert loc.contributing_ears == []


def test_multi_compatible_observation_remains_unresolved() -> None:
    nodes = {
        "W1": _node(59.3400, 18.0500, 0.0),
        "W2": _node(59.3410, 18.0505, 0.0),
        "W3": _node(59.3405, 18.0510, 0.0),
        "E1": _node(59.3400, 18.0650, 0.0),
        "E2": _node(59.3410, 18.0655, 0.0),
        "E3": _node(59.3405, 18.0660, 0.0),
        "BRIDGE": _node(59.3405, 18.0575, 0.0),
    }
    payloads = [
        _obs("w1", "W1", "2026-11-05T18:00:00Z", bearing_deg=None),
        _obs("w2", "W2", "2026-11-05T18:00:00Z", bearing_deg=None),
        _obs("w3", "W3", "2026-11-05T18:00:00Z", bearing_deg=None),
        _obs("e1", "E1", "2026-11-05T18:00:00Z", bearing_deg=None),
        _obs("e2", "E2", "2026-11-05T18:00:00Z", bearing_deg=None),
        _obs("e3", "E3", "2026-11-05T18:00:00Z", bearing_deg=None),
        _obs("br", "BRIDGE", "2026-11-05T18:00:05Z", bearing_deg=None),
    ]
    result = hub.run_cross_ear_correlation(
        payloads,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
        min_ears_for_event=3,
        min_promote_score=4,
    )
    assert len(result.events) >= 2
    unresolved_ids = {item.observation_id for item in result.unresolved}
    assert "br" in unresolved_ids
    for event in result.events:
        assert "br" not in event.observation_ids


def test_report_arrival_order_does_not_change_final_membership() -> None:
    payloads = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )
    forward = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    reverse = hub.run_cross_ear_correlation(
        list(reversed(payloads)),
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(forward.events) == len(reverse.events) == 1
    assert set(forward.events[0].ear_ids) == set(reverse.events[0].ear_ids)
    assert set(forward.events[0].observation_ids) == set(
        reverse.events[0].observation_ids
    )


def test_duplicate_reports_do_not_change_final_result() -> None:
    payloads = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )
    once = hub.run_cross_ear_correlation(
        payloads,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    duped = payloads + [copy.deepcopy(payloads[0])]
    twice = hub.run_cross_ear_correlation(
        duped,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert set(once.events[0].observation_ids) == set(twice.events[0].observation_ids)
    assert set(once.events[0].ear_ids) == set(twice.events[0].ear_ids)


def test_late_reports_included_in_complete_snapshot_replay() -> None:
    early = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )[:3]
    late = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )
    early_result = hub.run_cross_ear_correlation(
        early,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    full_result = hub.run_cross_ear_correlation(
        late,
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(early_result.events[0].ear_ids) == 3
    assert len(full_result.events[0].ear_ids) == 4
    assert full_result.events[0].event_id == early_result.events[0].event_id


def test_analyze_list_and_watch_share_logic_and_equivalent_results() -> None:
    payloads = _three_then_fourth_payloads(
        fourth_bearing=CENTROID_BEARINGS["SIM-EAR-004"]
    )
    config = hub.AnalysisConfig(
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        include_site_context=False,
        include_cross_ear_geometry=False,
    )
    list_result = hub.analyze(
        hub.AnalysisSnapshot(
            reports=payloads,
            nodes=TRIANGLE,
            config=config,
            fetch_meta={"complete_snapshot": True},
        )
    )
    watch_result = hub.analyze(
        hub.AnalysisSnapshot(
            reports=list(reversed(payloads)),
            nodes=TRIANGLE,
            config=config,
            fetch_meta={"complete_snapshot": True},
        )
    )
    assert list_result.correlation is not None
    assert watch_result.correlation is not None
    assert set(list_result.correlation.events[0].ear_ids) == set(
        watch_result.correlation.events[0].ear_ids
    )
    assert set(list_result.correlation.events[0].observation_ids) == set(
        watch_result.correlation.events[0].observation_ids
    )


def test_fetch_all_pages_beyond_500_with_snapshot_freeze() -> None:
    """More than one full Hub page (500); third page must not be truncated away."""
    total = 1200
    page_size = 500
    snapshot_id = 4242
    pages: dict[int, tuple] = {}
    for page in range(1, 4):
        start = (page - 1) * page_size + 1
        end = min(page * page_size, total)
        rows = [
            {
                "observation_id": f"o{i}",
                "node_id": "N",
                "sequence_number": i,
            }
            for i in range(start, end + 1)
        ]
        pages[page] = (
            rows,
            {
                "total_count": total,
                "page": page,
                "page_size": page_size,
                "total_pages": 3,
                "order": "oldest",
                "snapshot_id": snapshot_id,
            },
            {},
        )

    seen_snapshot_args: list[int | None] = []

    def fake_fetch(*_a, page=1, page_size=500, snapshot_id=None, **_k):
        seen_snapshot_args.append(snapshot_id)
        assert page_size == 500
        if page > 1:
            assert snapshot_id == 4242
        return pages[page]

    with patch.object(hub, "fetch_observations", side_effect=fake_fetch):
        reports, meta = hub.fetch_all_observation_reports(
            "https://hub.example",
            "ca.crt",
            "token",
            page_size=500,
            order="oldest",
        )

    assert len(reports) == 1200
    assert meta["complete_snapshot"] is True
    assert meta["fetched_report_count"] == 1200
    assert meta["total_count"] == 1200
    assert meta["snapshot_id"] == 4242
    assert reports[0]["sequence_number"] == 1
    assert reports[499]["sequence_number"] == 500
    assert reports[500]["sequence_number"] == 501
    assert reports[-1]["sequence_number"] == 1200
    # Page 1 may omit snapshot_id argument (None); later pages must freeze.
    assert seen_snapshot_args[0] is None
    assert seen_snapshot_args[1:] == [4242, 4242]


def test_fetch_all_drops_duplicate_delivery() -> None:
    page1 = (
        [
            {"observation_id": "o1", "node_id": "N", "sequence_number": 1},
            {"observation_id": "o2", "node_id": "N", "sequence_number": 2},
        ],
        {
            "total_count": 3,
            "page": 1,
            "page_size": 2,
            "total_pages": 2,
            "order": "oldest",
            "snapshot_id": 7,
        },
        {},
    )
    page2 = (
        [
            {"observation_id": "o2-dup", "node_id": "N", "sequence_number": 2},
            {"observation_id": "o3", "node_id": "N", "sequence_number": 3},
        ],
        {
            "total_count": 3,
            "page": 2,
            "page_size": 2,
            "total_pages": 2,
            "order": "oldest",
            "snapshot_id": 7,
        },
        {},
    )

    def fake_fetch(*_a, page=1, snapshot_id=None, **_k):
        return page1 if page == 1 else page2

    with patch.object(hub, "fetch_observations", side_effect=fake_fetch):
        reports, meta = hub.fetch_all_observation_reports(
            "https://hub.example",
            "ca.crt",
            "token",
            page_size=2,
            order="oldest",
        )
    assert [r["sequence_number"] for r in reports] == [1, 2, 3]
    assert meta["duplicate_delivery_dropped"] == 1
    assert meta["complete_snapshot"] is True
    assert meta["fetched_report_count"] == 3


def test_fetch_all_marks_incomplete_when_count_mismatches() -> None:
    """Script completeness is verified against Hub total_count, not assumed."""
    page1 = (
        [{"observation_id": "o1", "node_id": "N", "sequence_number": 1}],
        {
            "total_count": 3,
            "page": 1,
            "page_size": 1,
            "total_pages": 2,
            "order": "oldest",
            "snapshot_id": 9,
        },
        {},
    )
    page2 = (
        [{"observation_id": "o2", "node_id": "N", "sequence_number": 2}],
        {
            "total_count": 3,
            "page": 2,
            "page_size": 1,
            "total_pages": 2,
            "order": "oldest",
            "snapshot_id": 9,
        },
        {},
    )

    def fake_fetch(*_a, page=1, snapshot_id=None, **_k):
        return page1 if page == 1 else page2

    with patch.object(hub, "fetch_observations", side_effect=fake_fetch):
        reports, meta = hub.fetch_all_observation_reports(
            "https://hub.example",
            "ca.crt",
            "token",
            page_size=1,
            order="oldest",
        )
    assert len(reports) == 2
    assert meta["complete_snapshot"] is False
    assert "completeness_warning" in meta


def test_membership_trace_separates_association_and_localization() -> None:
    result = hub.run_cross_ear_correlation(
        _three_then_fourth_payloads(fourth_bearing=None),
        TRIANGLE,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    trace = build_membership_stage_trace(result)
    by_ear = {row["node_id"]: row for row in trace}
    assert by_ear["SIM-EAR-004"]["in_event"] is True
    assert by_ear["SIM-EAR-004"]["associated_observation"] is True
    assert by_ear["SIM-EAR-004"]["localization_contributor"] is False
