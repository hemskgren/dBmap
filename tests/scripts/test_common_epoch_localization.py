"""Production common-epoch bearing selection tests."""

from __future__ import annotations

import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_event_correlation import AssembledObservation
from lib.cross_ear_localization import (
    BEARING_SELECTION_COMMON_EPOCH,
    BEARING_SELECTION_PAIRWISE_FALLBACK,
    COMMON_EPOCH_RANKING_RULE,
    BearingSample,
    estimate_localization,
    list_common_epoch_candidates,
)
from lib.localization_epoch_diagnostic import (
    ADE_STARTED_AT_UTC,
    load_ade_overlap_members_offline,
)


def _sample(
    bearing_deg: float,
    at: datetime,
    *,
    confidence: float,
    index: int,
) -> BearingSample:
    return BearingSample(
        bearing_deg=bearing_deg,
        confidence=confidence,
        event_time_utc=at,
        sequence_number=index,
        emission_kind="periodic",
        report_index=index,
    )


def _member(
    node_id: str,
    observation_id: str,
    samples: list[BearingSample],
) -> AssembledObservation:
    times = [sample.event_time_utc for sample in samples if sample.event_time_utc]
    best = max(samples, key=lambda item: item.confidence or -1.0)
    return AssembledObservation(
        observation_id=observation_id,
        node_id=node_id,
        reports=[],
        first_time=min(times) if times else None,
        last_time=max(times) if times else None,
        interval_start=min(times) if times else None,
        interval_end=max(times) if times else None,
        report_count=len(samples),
        emission_counts={"start": 1, "periodic": max(0, len(samples) - 2), "end": 1},
        lifecycle_complete=True,
        sequence_gaps=0,
        sequences=list(range(1, len(samples) + 1)),
        bearing_deg=best.bearing_deg,
        bearing_confidence=best.confidence,
        bearing_reference="node",
        bearing_samples=samples,
        classification=None,
        signal_level_dbfs=None,
        measurements=None,
        signature=None,
        timing_quality="estimated",
        timestamp_uncertainty_ms=10.0,
        capture_timestamp_monotonic_us=None,
    )


def _triangle_nodes() -> dict[str, dict]:
    return {
        "SIM-EAR-001": {
            "installation": {
                "latitude": 59.35,
                "longitude": 18.05,
                "orientation_deg": 90.0,
            }
        },
        "SIM-EAR-004": {
            "installation": {
                "latitude": 59.36,
                "longitude": 18.07,
                "orientation_deg": 180.0,
            }
        },
        "SIM-EAR-005": {
            "installation": {
                "latitude": 59.34,
                "longitude": 18.07,
                "orientation_deg": 0.0,
            }
        },
    }


def test_ade_regression_common_epoch_three_contributors_useful() -> None:
    members, nodes, _scenario, _published = load_ade_overlap_members_offline()
    first = estimate_localization(members, nodes)
    second = estimate_localization(list(reversed(members)), nodes)

    assert first.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert first.details["common_epoch_utc"] is not None
    assert first.details["common_epoch_ranking_rule"] == COMMON_EPOCH_RANKING_RULE
    assert set(first.details["common_epoch_participating_ears"]) == {
        "SIM-EAR-001",
        "SIM-EAR-004",
        "SIM-EAR-005",
    }
    assert first.status == "useful"
    assert set(first.contributing_ears) == {
        "SIM-EAR-001",
        "SIM-EAR-004",
        "SIM-EAR-005",
    }
    assert first.details["forward_pair_count"] == 3
    assert first.details.get("outlier_pairs") in ([], None) or len(
        first.details.get("outlier_pairs") or []
    ) == 0
    # All pairs share the selected epoch (Δt=0).
    epoch = datetime.fromisoformat(first.details["common_epoch_utc"])
    for pair in first.pairs:
        assert pair.time_aligned
        assert pair.bearing_dt_s == pytest.approx(0.0)
        assert pair.bearing_a_time == epoch
        assert pair.bearing_b_time == epoch
        assert pair.intersection_status == "forward_intersection"
        assert pair.outlier is False

    # Deterministic vs report/member order.
    assert first.details["common_epoch_utc"] == second.details["common_epoch_utc"]
    assert first.status == second.status
    assert first.latitude == pytest.approx(second.latitude)
    assert first.longitude == pytest.approx(second.longitude)
    assert first.contributing_ears == second.contributing_ears


def test_missing_ear_at_epoch_keeps_event_member_set_separate() -> None:
    t0 = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)
    t1 = t0 + timedelta(seconds=10)
    nodes = _triangle_nodes()
    # Only A+D share an epoch (t1). E reports alone at t0 → excluded from loc only.
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [_sample(85.0, t1, confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(25.0, t1, confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-005",
            "obs-e",
            [_sample(330.0, t0, confidence=0.9, index=1)],
        ),
    ]
    association_ids = sorted(item.node_id for item in members)
    loc = estimate_localization(members, nodes)
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert loc.details["association_member_ears"] == association_ids
    assert "SIM-EAR-005" in loc.details["association_member_ears"]
    assert loc.details["common_epoch_utc"] == t1.isoformat()
    excluded_ids = {
        item["ear_id"] for item in loc.details.get("common_epoch_excluded_ears") or []
    }
    assert "SIM-EAR-005" in excluded_ids
    assert "SIM-EAR-005" not in loc.contributing_ears
    assert set(loc.details["common_epoch_participating_ears"]) == {
        "SIM-EAR-001",
        "SIM-EAR-004",
    }


def test_no_common_epoch_uses_pairwise_fallback() -> None:
    t0 = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)
    nodes = _triangle_nodes()
    members = [
        _member("SIM-EAR-001", "obs-a", [_sample(80.0, t0, confidence=0.8, index=1)]),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(20.0, t0 + timedelta(seconds=30), confidence=0.8, index=1)],
        ),
        _member(
            "SIM-EAR-005",
            "obs-e",
            [_sample(330.0, t0 + timedelta(seconds=60), confidence=0.8, index=1)],
        ),
    ]
    assert list_common_epoch_candidates(members) == []
    loc = estimate_localization(members, nodes)
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_PAIRWISE_FALLBACK
    assert loc.details["common_epoch_utc"] is None
    pairwise = estimate_localization(members, nodes, bearing_selection="pairwise_only")
    assert pairwise.details["bearing_selection_mode"] == BEARING_SELECTION_PAIRWISE_FALLBACK
    assert loc.status == pairwise.status


def test_unequal_timestamps_respect_one_second_gate() -> None:
    t0 = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)
    nodes = _triangle_nodes()
    # Δt = 2 s > 1 s gate → not a common epoch together.
    members = [
        _member("SIM-EAR-001", "obs-a", [_sample(80.0, t0, confidence=0.8, index=1)]),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(20.0, t0 + timedelta(seconds=2), confidence=0.8, index=1)],
        ),
    ]
    assert list_common_epoch_candidates(members) == []
    loc = estimate_localization(members, nodes)
    assert loc.details["bearing_selection_mode"] == BEARING_SELECTION_PAIRWISE_FALLBACK
    # Within gate (0.5 s) does form a common epoch.
    near = [
        _member("SIM-EAR-001", "obs-a", [_sample(80.0, t0, confidence=0.8, index=1)]),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(20.0, t0 + timedelta(seconds=0.5), confidence=0.8, index=1)],
        ),
    ]
    assert len(list_common_epoch_candidates(near)) >= 1
    near_loc = estimate_localization(near, nodes)
    assert near_loc.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH


def test_multiple_epochs_deterministic_ranking_independent_of_order() -> None:
    t0 = ADE_STARTED_AT_UTC
    times = [t0 + timedelta(seconds=s) for s in (0, 10, 20, 30, 40)]
    nodes = _triangle_nodes()

    def build(order: list[str]) -> list[AssembledObservation]:
        by_ear = {
            "SIM-EAR-001": _member(
                "SIM-EAR-001",
                "obs-a",
                [
                    _sample(80.0 + i, stamp, confidence=0.5 + 0.05 * i, index=i + 1)
                    for i, stamp in enumerate(times)
                ],
            ),
            "SIM-EAR-004": _member(
                "SIM-EAR-004",
                "obs-d",
                [
                    _sample(20.0 + i, stamp, confidence=0.4 + 0.08 * i, index=i + 1)
                    for i, stamp in enumerate(times)
                ],
            ),
            "SIM-EAR-005": _member(
                "SIM-EAR-005",
                "obs-e",
                [
                    _sample(330.0 + i, stamp, confidence=0.3 + 0.04 * i, index=i + 1)
                    for i, stamp in enumerate(times)
                ],
            ),
        }
        return [by_ear[ear_id] for ear_id in order]

    order_a = ["SIM-EAR-001", "SIM-EAR-004", "SIM-EAR-005"]
    order_b = ["SIM-EAR-005", "SIM-EAR-001", "SIM-EAR-004"]
    a = estimate_localization(build(order_a), nodes)
    b = estimate_localization(build(order_b), nodes)
    assert a.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert a.details["common_epoch_utc"] == b.details["common_epoch_utc"]
    assert a.latitude == pytest.approx(b.latitude)
    assert a.longitude == pytest.approx(b.longitude)


def test_pairwise_only_matches_legacy_mixed_epoch_on_ade() -> None:
    """Force pairwise_only to reproduce the pre-common-epoch ADE symptom."""
    members, nodes, _scenario, _published = load_ade_overlap_members_offline()
    legacy = estimate_localization(members, nodes, bearing_selection="pairwise_only")
    assert legacy.details["bearing_selection_mode"] == BEARING_SELECTION_PAIRWISE_FALLBACK
    pair_epochs = {
        pair.bearing_a_time for pair in legacy.pairs if pair.bearing_a_time is not None
    }
    assert len(pair_epochs) >= 2
    production = estimate_localization(members, nodes)
    assert production.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert len(production.contributing_ears) == 3
    assert len(legacy.contributing_ears) == 2
