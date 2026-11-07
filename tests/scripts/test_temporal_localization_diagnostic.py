"""Phase 5A: Event-scoped temporal localization diagnostics."""

from __future__ import annotations

import contextlib
import io
import sys
from copy import deepcopy
from dataclasses import replace
from datetime import UTC, datetime, timedelta
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_event_correlation import AssembledObservation, CorrelatedEvent
from lib.cross_ear_localization import (
    BEARING_SELECTION_COMMON_EPOCH,
    BEARING_SELECTION_PAIRWISE_FALLBACK,
    BearingSample,
    estimate_localization,
    estimate_localization_at_epoch,
    list_common_epoch_candidates,
)
from lib.localization_epoch_diagnostic import (
    evaluate_event_localization_epochs,
    format_event_epoch_localization_report,
)

_ROOT = Path(__file__).parents[2]
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


def _run_tv():
    sim = _load("sim_tv_5a", _ROOT / "simulator" / "ear" / "simulate_scenario.py")
    hub = _load("hub_tv_5a", _ROOT / "scripts" / "hub_event_track.py")
    scenario = sim.load_scenario(_TV)
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
    return members, nodes, corr


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


def _event_from_members(
    members: list[AssembledObservation],
    localization,
) -> CorrelatedEvent:
    return CorrelatedEvent(
        event_id="evt-test",
        candidate_id="cand-test",
        observation_ids=[m.observation_id for m in members],
        ear_ids=sorted({m.node_id for m in members}),
        members=members,
        score=10,
        score_breakdown={"test": 10},
        evidence_notes=[],
        qualitative_uncertainty="low",
        localization=localization,
        provisional_position=None,
        created_at=None,
        promoted_at=None,
    )


def _snapshot_loc(loc):
    return deepcopy(loc)


def test_abde_tv_discovers_five_epochs_and_expected_rows() -> None:
    _members, nodes, corr = _run_tv()
    event = corr.events[0]
    before = _snapshot_loc(event.localization)

    report = evaluate_event_localization_epochs(event, nodes)

    assert event.localization == before
    assert len(report.discovered_epochs) == 5
    assert len(report.epochs) == 5
    offsets = [
        (row.epoch_utc - _START).total_seconds() for row in report.epochs
    ]
    assert offsets == [0.0, 10.0, 20.0, 30.0, 40.0]

    t0 = report.epochs[0]
    assert t0.status == "useful"
    assert set(t0.contributing_ears) == _ALL
    assert t0.forward_pair_count == 6
    assert t0.aligned_forward_pair_count == 6
    assert t0.consensus_pair_count == 6
    assert t0.outlier_pairs == ()
    assert t0.unavailable is False

    for row in report.epochs[1:]:
        assert row.status == "weak"
        assert set(row.contributing_ears) == _ADE
        assert row.forward_pair_count == 6
        assert row.aligned_forward_pair_count == 6
        assert row.consensus_pair_count == 3
        assert set(row.outlier_pairs) == _B_OUTLIERS

    t40 = report.epochs[-1]
    assert (t40.epoch_utc - _START).total_seconds() == 40.0
    assert t40.status == "weak"
    assert set(t40.outlier_pairs) == _B_OUTLIERS

    prod = report.production_localization
    assert prod is not None
    assert prod.status == "useful"
    assert report.production_common_epoch_utc == _START
    assert "Ground truth is omitted" in " ".join(report.notes)


def test_production_localization_unchanged_by_diagnostic() -> None:
    _members, nodes, corr = _run_tv()
    event = corr.events[0]
    before = _snapshot_loc(event.localization)
    before_id = id(event.localization)

    report = evaluate_event_localization_epochs(event, nodes)
    assert id(event.localization) == before_id
    assert event.localization == before
    assert report.production_localization is event.localization


def test_estimate_localization_auto_ranking_unchanged_on_tv() -> None:
    members, nodes, corr = _run_tv()
    prod = corr.events[0].localization
    assert prod is not None
    again = estimate_localization(members, nodes)
    assert again.status == prod.status == "useful"
    assert again.details["bearing_selection_mode"] == BEARING_SELECTION_COMMON_EPOCH
    assert again.details["common_epoch_utc"] == prod.details["common_epoch_utc"]
    assert again.latitude == pytest.approx(prod.latitude)
    assert again.longitude == pytest.approx(prod.longitude)
    assert set(again.contributing_ears) == set(prod.contributing_ears) == _ALL


def test_at_epoch_matches_production_winner_at_selected_epoch() -> None:
    members, nodes, corr = _run_tv()
    prod = corr.events[0].localization
    assert prod is not None
    epoch = datetime.fromisoformat(prod.details["common_epoch_utc"])
    forced = estimate_localization_at_epoch(members, nodes, epoch)
    assert forced.status == prod.status
    assert forced.latitude == pytest.approx(prod.latitude)
    assert forced.longitude == pytest.approx(prod.longitude)
    assert set(forced.contributing_ears) == set(prod.contributing_ears)
    assert forced.details["forward_pair_count"] == prod.details["forward_pair_count"]
    assert forced.details["consensus_pair_count"] == prod.details["consensus_pair_count"]
    assert forced.details.get("outlier_pairs") == prod.details.get("outlier_pairs")


def test_pairwise_fallback_unchanged() -> None:
    t0 = _START
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [_sample(45.0, t0, confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(200.0, t0 + timedelta(seconds=5), confidence=0.9, index=1)],
        ),
    ]
    nodes = _triangle_nodes()
    assert list_common_epoch_candidates(members) == []
    auto = estimate_localization(members, nodes)
    pairwise = estimate_localization(
        members, nodes, bearing_selection="pairwise_only"
    )
    assert auto.details["bearing_selection_mode"] == BEARING_SELECTION_PAIRWISE_FALLBACK
    assert pairwise.details["bearing_selection_mode"] == BEARING_SELECTION_PAIRWISE_FALLBACK
    assert auto.status == pairwise.status
    assert auto.latitude == pairwise.latitude
    assert auto.longitude == pairwise.longitude


def test_missing_bearing_from_one_ear() -> None:
    members, nodes, _corr = _run_tv()
    # Drop B samples at t=0 only by removing the Observation's t=0 sample.
    epoch = _START
    trimmed: list[AssembledObservation] = []
    for member in members:
        if member.node_id == "SIM-EAR-002":
            samples = [
                sample
                for sample in member.bearing_samples
                if sample.event_time_utc is None
                or abs((sample.event_time_utc - epoch).total_seconds()) > 0.0
            ]
            trimmed.append(
                replace(
                    member,
                    bearing_samples=samples,
                    bearing_deg=samples[0].bearing_deg if samples else None,
                    bearing_confidence=(
                        samples[0].confidence if samples else None
                    ),
                )
            )
        else:
            trimmed.append(member)

    loc = estimate_localization_at_epoch(trimmed, nodes, epoch)
    excluded = {
        item["ear_id"]: item["reason"]
        for item in loc.details.get("common_epoch_excluded_ears") or []
    }
    assert "SIM-EAR-002" in excluded
    assert set(loc.details["common_epoch_participating_ears"]) == _ADE
    assert loc.status in {"useful", "weak"}
    assert len(loc.contributing_ears) >= 2


def test_only_two_usable_ears() -> None:
    t0 = _START
    # Bearings aimed roughly into the triangle interior.
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [_sample(90.0, t0, confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(220.0, t0, confidence=0.9, index=1)],
        ),
    ]
    nodes = _triangle_nodes()
    event = _event_from_members(
        members, estimate_localization(members, nodes)
    )
    report = evaluate_event_localization_epochs(event, nodes)
    assert len(report.discovered_epochs) == 1
    row = report.epochs[0]
    assert row.evaluated is True
    assert set(row.participating_ear_ids) == {"SIM-EAR-001", "SIM-EAR-004"}
    assert row.forward_pair_count in {0, 1}
    assert row.localization is not None


def test_no_forward_intersections_unavailable() -> None:
    t0 = _START
    # After orientation, both LOBs point geographic east (parallel / no forward cross).
    # EAR-001 orientation 90° + node bearing 0° → 90°
    # EAR-004 orientation 180° + node bearing 270° → 90°
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [_sample(0.0, t0, confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(270.0, t0, confidence=0.9, index=1)],
        ),
    ]
    nodes = _triangle_nodes()
    loc = estimate_localization_at_epoch(members, nodes, t0)
    assert loc.details.get("forward_pair_count") == 0
    assert loc.status == "unavailable"
    assert loc.latitude is None
    assert loc.details.get("position_withheld") is True
    assert loc.contributing_ears == []


def test_timestamp_jitter_within_one_second_gate() -> None:
    t0 = _START
    members = [
        _member(
            "SIM-EAR-001",
            "obs-a",
            [_sample(90.0, t0, confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-004",
            "obs-d",
            [_sample(220.0, t0 + timedelta(milliseconds=400), confidence=0.9, index=1)],
        ),
        _member(
            "SIM-EAR-005",
            "obs-e",
            [_sample(310.0, t0 + timedelta(milliseconds=800), confidence=0.9, index=1)],
        ),
    ]
    nodes = _triangle_nodes()
    candidates = list_common_epoch_candidates(members)
    assert len(candidates) >= 1
    # Evaluating at A's stamp should still include D and E (within 1 s).
    loc = estimate_localization_at_epoch(members, nodes, t0)
    assert set(loc.details["common_epoch_participating_ears"]) == {
        "SIM-EAR-001",
        "SIM-EAR-004",
        "SIM-EAR-005",
    }


def test_duplicate_and_out_of_order_input_deterministic() -> None:
    t0 = _START
    t1 = t0 + timedelta(seconds=10)
    samples_a = [
        _sample(90.0, t0, confidence=0.8, index=1),
        _sample(91.0, t0, confidence=0.7, index=2),  # duplicate time; lower conf
        _sample(95.0, t1, confidence=0.9, index=3),
    ]
    samples_d = [
        _sample(220.0, t1, confidence=0.9, index=2),
        _sample(210.0, t0, confidence=0.85, index=1),
    ]
    samples_e = [
        _sample(300.0, t1, confidence=0.8, index=2),
        _sample(310.0, t0, confidence=0.8, index=1),
    ]
    order_a = [
        _member("SIM-EAR-005", "obs-e", samples_e),
        _member("SIM-EAR-001", "obs-a", samples_a),
        _member("SIM-EAR-004", "obs-d", samples_d),
    ]
    order_b = list(reversed(order_a))
    nodes = _triangle_nodes()

    first = evaluate_event_localization_epochs(
        _event_from_members(order_a, estimate_localization(order_a, nodes)),
        nodes,
    )
    second = evaluate_event_localization_epochs(
        _event_from_members(order_b, estimate_localization(order_b, nodes)),
        nodes,
    )
    assert first.discovered_epochs == second.discovered_epochs
    assert [row.epoch_utc for row in first.epochs] == [
        row.epoch_utc for row in second.epochs
    ]
    for left, right in zip(first.epochs, second.epochs, strict=True):
        assert left.status == right.status
        assert left.participating_ear_ids == right.participating_ear_ids
        assert left.selected_bearings == right.selected_bearings
        assert left.latitude == right.latitude
        assert left.longitude == right.longitude
        assert left.contributing_ears == right.contributing_ears


def test_ground_truth_absent_still_works() -> None:
    _members, nodes, corr = _run_tv()
    event = corr.events[0]
    report = evaluate_event_localization_epochs(event, nodes)
    assert report.epochs
    assert all(
        "ground_truth" not in (row.localization.details or {})
        for row in report.epochs
        if row.localization is not None
    )
    text = "\n".join(format_event_epoch_localization_report(report))
    assert "ground-truth" not in text.lower() or "omitted" in " ".join(report.notes).lower()


def test_cli_formatter_includes_concise_rows() -> None:
    _members, nodes, corr = _run_tv()
    report = evaluate_event_localization_epochs(corr.events[0], nodes)
    lines = format_event_epoch_localization_report(report)
    blob = "\n".join(lines)
    assert "Temporal localization diagnostic" in blob
    assert "production common_epoch_utc=" in blob
    assert "epoch[0]" in blob
    assert "fwd/al/con=6/6/6" in blob
    assert "epoch[4]" in blob
    assert "geometric sensitivity is not position accuracy" in blob
