"""Offline pairwise vs common-epoch localization diagnostic tests."""

from __future__ import annotations

import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_event_correlation import AssembledObservation
from lib.cross_ear_localization import BearingSample, estimate_localization
from lib.localization_epoch_diagnostic import (
    ADE_STARTED_AT_UTC,
    common_epochs,
    compare_pairwise_vs_common_epoch,
    ears_present_at_epoch,
    explain_mixed_epoch_root_cause,
    format_comparison_report,
    load_ade_overlap_members_offline,
    members_restricted_to_epoch,
    run_ade_overlap_epoch_comparison,
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
    # Equilateral-ish ~1 km; orientations face the interior.
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


def test_ade_five_shared_timestamps_mixed_epoch_vs_common() -> None:
    report = run_ade_overlap_epoch_comparison()
    assert len(report.candidate_epochs) == 5
    assert report.selected_common_epoch is not None
    assert report.common_epoch is not None

    # Current mode: independent pair selection; ADE authored confidences make
    # AD/AE prefer the A-peak epoch and DE prefer the D-peak epoch (±10 s).
    pair_times = {
        (pair.ear_a_id, pair.ear_b_id): pair.bearing_a_time
        for pair in report.current_pairs
        if pair.bearing_a_time is not None
    }
    assert len(pair_times) == 3
    distinct_epochs = {stamp for stamp in pair_times.values()}
    assert len(distinct_epochs) >= 2

    # Common-epoch mode forces one timestamp for every contributing pair.
    assert report.common_epoch_pairs
    common_times = {
        pair.bearing_a_time for pair in report.common_epoch_pairs if pair.bearing_a_time
    }
    common_times |= {
        pair.bearing_b_time for pair in report.common_epoch_pairs if pair.bearing_b_time
    }
    assert common_times == {report.selected_common_epoch}
    assert all(
        pair.time_aligned and (pair.bearing_dt_s or 0) == 0.0
        for pair in report.common_epoch_pairs
        if pair.intersection_status != "unavailable"
    )

    text = format_comparison_report(report)
    assert "Pairwise-only" in text
    assert "Common-epoch" in text
    cause = explain_mixed_epoch_root_cause(report)
    assert "independently" in cause
    assert "no shared localization epoch" in cause.lower() or "There is no shared" in cause


def test_epoch_missing_one_ear_still_forms_remaining_pairs() -> None:
    t0 = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)
    t1 = t0 + timedelta(seconds=10)
    nodes = _triangle_nodes()
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [
                _sample(80.0, t0, confidence=0.5, index=1),
                _sample(85.0, t1, confidence=0.9, index=2),
            ],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [
                _sample(20.0, t0, confidence=0.5, index=1),
                _sample(25.0, t1, confidence=0.9, index=2),
            ],
        ),
        _member(
            "SIM-EAR-005",
            "obs-e",
            [
                # E only present at t0 — no report at t1.
                _sample(330.0, t0, confidence=0.5, index=1),
            ],
        ),
    ]

    assert ears_present_at_epoch(members, t0) == [
        "SIM-EAR-001",
        "SIM-EAR-004",
        "SIM-EAR-005",
    ]
    assert ears_present_at_epoch(members, t1) == ["SIM-EAR-001", "SIM-EAR-004"]
    assert set(common_epochs(members)) == {t0, t1}

    restricted = members_restricted_to_epoch(members, t1)
    assert {item.node_id for item in restricted} == {"SIM-EAR-001", "SIM-EAR-004"}
    loc = estimate_localization(restricted, nodes)
    assert loc.status in {"useful", "weak", "unavailable"}
    assert "SIM-EAR-005" not in loc.contributing_ears

    report = compare_pairwise_vs_common_epoch(members, nodes)
    assert t0 in report.candidate_epochs
    assert t1 in report.candidate_epochs
    eval_t1 = next(item for item in report.epoch_evaluations if item.epoch_utc == t1)
    assert eval_t1.ear_ids_present == ["SIM-EAR-001", "SIM-EAR-004"]
    assert "SIM-EAR-005" not in eval_t1.ear_ids_present


def test_no_common_usable_epoch_when_samples_never_overlap() -> None:
    t0 = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)
    t_far = t0 + timedelta(seconds=30)
    nodes = _triangle_nodes()
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [_sample(80.0, t0, confidence=0.8, index=1)],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(20.0, t_far, confidence=0.8, index=1)],
        ),
        _member(
            "SIM-EAR-005",
            "obs-e",
            [_sample(330.0, t_far + timedelta(seconds=30), confidence=0.8, index=1)],
        ),
    ]
    assert common_epochs(members, min_ears=2) == []
    report = compare_pairwise_vs_common_epoch(members, nodes)
    assert report.candidate_epochs == []
    assert report.selected_common_epoch is None
    assert report.common_epoch is None
    assert report.current is not None  # pairwise still runs (likely misaligned)


def test_ade_offline_assembly_has_five_synced_samples_per_ear() -> None:
    members, nodes, scenario, published = load_ade_overlap_members_offline()
    assert scenario["scenario_id"] == "helicopter_ade_overlap_01"
    assert len(published) == 15
    assert len(members) == 3
    assert set(nodes) == {"SIM-EAR-001", "SIM-EAR-004", "SIM-EAR-005"}
    expected = {
        ADE_STARTED_AT_UTC + timedelta(seconds=offset)
        for offset in (0, 10, 20, 30, 40)
    }
    for member in members:
        stamps = {
            sample.event_time_utc.astimezone(UTC)
            for sample in member.bearing_samples
            if sample.event_time_utc is not None
        }
        assert stamps == expected
