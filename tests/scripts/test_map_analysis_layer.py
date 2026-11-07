"""Unit tests for AnalysisResult → map-layer serializer (no network)."""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_event_correlation import (
    AssembledObservation,
    CorrelatedEvent,
    CorrelationResult,
)
from lib.cross_ear_localization import LocalizationResult
from lib.map_analysis_layer import (
    AnalysisRunCounts,
    IncompleteSnapshotError,
    analysis_config_dict,
    counts_from_result,
    localization_evidence_fields,
    localization_is_published,
    require_complete_snapshot,
    scratch_object_ids_for_dataset,
    serialize_analysis_map_layer,
)


@dataclass
class _Config:
    event_gap_seconds: float = 30.0
    max_association_distance_m: float = 500.0
    allow_long_range_sim_ears: bool = False
    association_temporal_gap_seconds: float = 15.0
    min_ears_for_event: int = 2
    min_promote_score: int = 3
    include_cross_ear_correlation: bool = True


@dataclass
class _Snapshot:
    reports: list[dict[str, Any]]
    nodes: dict[str, dict[str, Any]]
    config: _Config
    snapshot_id: int | None = 42
    environmental_context: dict[str, Any] | None = None
    filter_status_lines: list[str] = field(default_factory=list)
    fetch_meta: dict[str, Any] = field(default_factory=dict)


@dataclass
class _Result:
    snapshot: _Snapshot
    correlation: CorrelationResult | None
    report_count: int
    unique_observation_count: int
    lines: list[str] = field(default_factory=list)


def _assembled(
    *,
    observation_id: str,
    node_id: str,
    lat: float | None,
    lon: float | None,
    signal: float | None = -30.0,
) -> AssembledObservation:
    return AssembledObservation(
        observation_id=observation_id,
        node_id=node_id,
        reports=[],
        first_time=datetime(2026, 11, 7, 12, 0, tzinfo=UTC),
        last_time=datetime(2026, 11, 7, 12, 1, tzinfo=UTC),
        interval_start=datetime(2026, 11, 7, 12, 0, tzinfo=UTC),
        interval_end=datetime(2026, 11, 7, 12, 1, tzinfo=UTC),
        report_count=1,
        emission_counts={"start": 1},
        lifecycle_complete=True,
        sequence_gaps=0,
        sequences=[1],
        bearing_deg=10.0,
        bearing_confidence=0.5,
        bearing_reference="magnetic",
        bearing_samples=[],
        classification=None,
        signal_level_dbfs=signal,
        measurements=None,
        signature=None,
        timing_quality="good",
        timestamp_uncertainty_ms=None,
        capture_timestamp_monotonic_us=None,
        installation_latitude=lat,
        installation_longitude=lon,
        installation_orientation_deg=0.0,
    )


def _localization(
    *,
    lat: float | None,
    lon: float | None,
    status: str = "useful",
    withheld: bool = False,
    contributing: list[str] | None = None,
    calibrated_uncertainty_m: float | None = None,
    details: dict[str, Any] | None = None,
) -> LocalizationResult:
    payload: dict[str, Any] = {"position_withheld": withheld}
    if details:
        payload.update(details)
    return LocalizationResult(
        status=status,  # type: ignore[arg-type]
        quality_notes=[],
        latitude=lat,
        longitude=lon,
        evaluation_time_utc=datetime(2026, 11, 7, 12, 0, 30, tzinfo=UTC),
        pairs=[],
        max_bearing_dt_s=1.0,
        pairwise_spread_m=25.0,
        calibrated_uncertainty_m=calibrated_uncertainty_m,
        contributing_ears=list(contributing or []),
        selection_method="median_of_forward_pairs",
        details=payload,
    )


def _event(
    *,
    event_id: str,
    members: list[AssembledObservation],
    localization: LocalizationResult | None,
) -> CorrelatedEvent:
    return CorrelatedEvent(
        event_id=event_id,
        candidate_id=f"cand-{event_id}",
        observation_ids=[m.observation_id for m in members],
        ear_ids=[m.node_id for m in members],
        members=members,
        score=5,
        score_breakdown={"temporal": 2, "bearing": 1},
        evidence_notes=[],
        qualitative_uncertainty="lower",
        localization=localization,
        provisional_position=None,
        created_at=datetime(2026, 11, 7, 12, 0, tzinfo=UTC),
        promoted_at=datetime(2026, 11, 7, 12, 0, 30, tzinfo=UTC),
    )


def _result(
    observations: list[AssembledObservation],
    events: list[CorrelatedEvent],
    *,
    allow_long_range_sim_ears: bool = False,
) -> _Result:
    correlation = CorrelationResult(
        observations=observations,
        candidates=[],
        events=events,
        rejected=[],
        expired=[],
        parameters={"allow_long_range_sim_ears": allow_long_range_sim_ears},
    )
    config = _Config(allow_long_range_sim_ears=allow_long_range_sim_ears)
    snapshot = _Snapshot(
        reports=[{}] * 3,
        nodes={},
        config=config,
        snapshot_id=99,
        fetch_meta={
            "complete_snapshot": True,
            "fetched_report_count": 3,
            "total_count": 3,
            "snapshot_id": 99,
        },
    )
    return _Result(
        snapshot=snapshot,
        correlation=correlation,
        report_count=3,
        unique_observation_count=len(observations),
    )


def test_observation_markers_use_installation_coordinates_only() -> None:
    obs_a = _assembled(
        observation_id="obs-a", node_id="EAR-001", lat=59.33, lon=18.06
    )
    obs_b = _assembled(
        observation_id="obs-b", node_id="EAR-002", lat=None, lon=18.07
    )
    result = _result([obs_a, obs_b], [])
    content = serialize_analysis_map_layer(
        result,
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    assert len(content["observations"]) == 1
    marker = content["observations"][0]
    assert marker["lat"] == 59.33
    assert marker["lon"] == 18.06
    assert marker["coordinate_source"] == "ear_installation"
    assert marker["node_id"] == "EAR-001"
    assert marker["report_count"] == 1
    assert marker["interval_start_utc"] == "2026-11-07T12:00:00Z"
    assert marker["interval_end_utc"] == "2026-11-07T12:01:00Z"


def test_localization_markers_only_from_published_non_withheld() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
    ]
    published = _localization(
        lat=59.15,
        lon=18.15,
        contributing=["EAR-001", "EAR-002"],
    )
    withheld = _localization(lat=59.9, lon=18.9, withheld=True, contributing=[])
    missing = _localization(lat=None, lon=None, status="unavailable")
    events = [
        _event(event_id="e-pub", members=members, localization=published),
        _event(event_id="e-withheld", members=members, localization=withheld),
        _event(event_id="e-missing", members=members, localization=missing),
    ]
    content = serialize_analysis_map_layer(
        _result(members, events),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    assert len(content["localizations"]) == 1
    loc = content["localizations"][0]
    assert loc["lat"] == 59.15
    assert loc["lon"] == 18.15
    assert loc["event_id"] == "e-pub"
    assert loc["contributing_ears"] == ["EAR-001", "EAR-002"]
    assert "confidence" not in loc

    by_id = {item["id"]: item for item in content["events"]}
    assert "lat" in by_id["e-pub"]
    assert by_id["e-pub"]["lat"] == 59.15
    assert "lat" not in by_id["e-withheld"]
    assert "lon" not in by_id["e-withheld"]
    assert by_id["e-withheld"]["position_withheld"] is True
    assert "lat" not in by_id["e-missing"]


def test_no_route_or_ground_truth_layer_emitted() -> None:
    content = serialize_analysis_map_layer(
        _result([], []),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    assert content["layer"] == "analysis"
    assert content["role"] == "analysis"
    assert content["kind"] == "map-layer"
    assert "points" not in content
    assert content.get("role") != "ground-truth"


def test_no_fabricated_confidence_when_uncertainty_uncalibrated() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
    ]
    loc = _localization(
        lat=59.15, lon=18.15, calibrated_uncertainty_m=None, contributing=["EAR-001"]
    )
    content = serialize_analysis_map_layer(
        _result(members, [_event(event_id="e1", members=members, localization=loc)]),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    assert "confidence" not in content["localizations"][0]
    assert "calibrated_uncertainty_m" not in content["localizations"][0]


def test_calibrated_uncertainty_emitted_only_when_finite() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
    ]
    loc = _localization(
        lat=59.15,
        lon=18.15,
        calibrated_uncertainty_m=42.5,
        contributing=["EAR-001"],
    )
    content = serialize_analysis_map_layer(
        _result(members, [_event(event_id="e1", members=members, localization=loc)]),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    assert content["localizations"][0]["calibrated_uncertainty_m"] == 42.5
    assert "confidence" not in content["localizations"][0]


def test_event_membership_independent_of_localization_contributors() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
        _assembled(observation_id="o3", node_id="EAR-003", lat=59.3, lon=18.3),
    ]
    loc = _localization(
        lat=59.2, lon=18.2, contributing=["EAR-001", "EAR-002"]  # not EAR-003
    )
    content = serialize_analysis_map_layer(
        _result(members, [_event(event_id="e1", members=members, localization=loc)]),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    event = content["events"][0]
    assert event["membership_ear_ids"] == ["EAR-001", "EAR-002", "EAR-003"]
    assert event["membership_ear_count"] == 3
    assert event["membership_observation_count"] == 3
    assert event["localization_contributing_ears"] == ["EAR-001", "EAR-002"]
    assert event["localization_contributing_ear_count"] == 2
    assert event["membership_ear_count"] > event["localization_contributing_ear_count"]
    loc_marker = content["localizations"][0]
    assert loc_marker["membership_ear_ids"] == ["EAR-001", "EAR-002", "EAR-003"]
    assert loc_marker["membership_ear_count"] == 3
    assert loc_marker["contributing_ears"] == ["EAR-001", "EAR-002"]
    assert loc_marker["contributing_ear_count"] == 2
    assert loc_marker["label"] == "Estimated position"
    assert loc_marker["not_ground_truth"] is True


def test_useful_localization_includes_optional_diagnostics() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
        _assembled(observation_id="o3", node_id="EAR-003", lat=59.3, lon=18.3),
    ]
    loc = _localization(
        lat=59.2,
        lon=18.2,
        status="useful",
        contributing=["EAR-001", "EAR-002", "EAR-003"],
        details={
            "bearing_selection_mode": "common_epoch",
            "common_epoch_utc": "2026-11-07T12:00:30+00:00",
            "forward_pair_count": 3,
            "aligned_forward_pair_count": 3,
            "consensus_pair_count": 3,
            "consensus_selected_ears": ["EAR-001", "EAR-002", "EAR-003"],
            "outlier_pairs": [],
            "network_geometric_sensitivity_m": 120.5,
            "common_epoch_excluded_ears": [],
        },
    )
    content = serialize_analysis_map_layer(
        _result(members, [_event(event_id="e-useful", members=members, localization=loc)]),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    marker = content["localizations"][0]
    assert marker["status"] == "useful"
    assert marker["lat"] == 59.2
    assert marker["lon"] == 18.2
    assert marker["bearing_selection_mode"] == "common_epoch"
    assert marker["common_epoch_utc"] == "2026-11-07T12:00:30+00:00"
    assert marker["forward_pair_count"] == 3
    assert marker["aligned_forward_pair_count"] == 3
    assert marker["consensus_pair_count"] == 3
    assert marker["consensus_selected_ears"] == ["EAR-001", "EAR-002", "EAR-003"]
    assert marker["outlier_pairs"] == []
    assert marker["network_geometric_sensitivity_m"] == 120.5
    assert marker["common_epoch_excluded_ears"] == []
    assert marker["contributing_ear_count"] == 3


def test_weak_localization_includes_diagnostics_and_exclusions() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
        _assembled(observation_id="o3", node_id="EAR-003", lat=59.3, lon=18.3),
    ]
    loc = _localization(
        lat=59.18,
        lon=18.18,
        status="weak",
        contributing=["EAR-001", "EAR-002"],
        details={
            "bearing_selection_mode": "common_epoch",
            "common_epoch_utc": "2026-11-07T12:00:20+00:00",
            "forward_pair_count": 1,
            "aligned_forward_pair_count": 1,
            "consensus_pair_count": 1,
            "consensus_selected_ears": ["EAR-001", "EAR-002"],
            "outlier_pairs": ["EAR-001|EAR-003"],
            "network_geometric_sensitivity_m": 480.0,
            "common_epoch_excluded_ears": [
                {"ear_id": "EAR-003", "reason": "no_usable_bearing_at_epoch"}
            ],
        },
    )
    content = serialize_analysis_map_layer(
        _result(members, [_event(event_id="e-weak", members=members, localization=loc)]),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    marker = content["localizations"][0]
    assert marker["status"] == "weak"
    assert marker["lat"] == 59.18
    assert marker["lon"] == 18.18
    assert marker["contributing_ear_count"] == 2
    assert marker["membership_ear_count"] == 3
    assert marker["outlier_pairs"] == ["EAR-001|EAR-003"]
    assert marker["common_epoch_excluded_ears"] == [
        {"ear_id": "EAR-003", "reason": "no_usable_bearing_at_epoch"}
    ]


def test_missing_optional_diagnostic_fields_are_omitted() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
    ]
    loc = _localization(
        lat=59.15,
        lon=18.15,
        status="weak",
        contributing=["EAR-001", "EAR-002"],
        details={},  # older / partial payload: no diagnostics
    )
    content = serialize_analysis_map_layer(
        _result(members, [_event(event_id="e-partial", members=members, localization=loc)]),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    marker = content["localizations"][0]
    for key in (
        "bearing_selection_mode",
        "common_epoch_utc",
        "forward_pair_count",
        "aligned_forward_pair_count",
        "consensus_pair_count",
        "consensus_selected_ears",
        "outlier_pairs",
        "network_geometric_sensitivity_m",
        "common_epoch_excluded_ears",
    ):
        assert key not in marker
    assert marker["contributing_ear_count"] == 2
    assert marker["status"] == "weak"
    assert localization_evidence_fields(loc) == {"contributing_ear_count": 2}


def test_localization_without_published_position_omits_localization_marker() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
        _assembled(observation_id="o2", node_id="EAR-002", lat=59.2, lon=18.2),
    ]
    unavailable = _localization(
        lat=None,
        lon=None,
        status="unavailable",
        contributing=[],
        details={
            "bearing_selection_mode": "pairwise_fallback",
            "forward_pair_count": 0,
            "consensus_pair_count": 0,
        },
    )
    content = serialize_analysis_map_layer(
        _result(
            members,
            [_event(event_id="e-none", members=members, localization=unavailable)],
        ),
        dataset="live-1",
        dataset_title="Live",
        run_id="run-1",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    assert content["localizations"] == []
    event = content["events"][0]
    assert "lat" not in event
    assert event["position_withheld"] is True
    assert event["localization_status"] == "unavailable"
    assert event["localization_contributing_ear_count"] == 0
    assert event["bearing_selection_mode"] == "pairwise_fallback"
    assert event["forward_pair_count"] == 0


def test_provenance_identifies_snapshot_and_config() -> None:
    result = _result([], [])
    content = serialize_analysis_map_layer(
        result,
        dataset="live-1",
        dataset_title="Live Analysis",
        run_id="run-abc",
        run_timestamp=datetime(2026, 11, 7, 13, 0, tzinfo=UTC),
    )
    provenance = content["provenance"]
    assert provenance["run_id"] == "run-abc"
    assert provenance["snapshot_id"] == 99
    assert provenance["complete_snapshot"] is True
    assert provenance["fetched_report_count"] == 3
    assert provenance["analysis_config"]["allow_long_range_sim_ears"] is False
    assert provenance["scratch_ephemeral"] is True
    assert provenance["not_durable_event_history"] is True
    assert provenance["run_timestamp_utc"] == "2026-11-07T13:00:00Z"


def test_require_complete_snapshot_blocks_incomplete() -> None:
    with pytest.raises(IncompleteSnapshotError, match="incomplete"):
        require_complete_snapshot(
            {
                "complete_snapshot": False,
                "fetched_report_count": 10,
                "total_count": 12,
                "snapshot_id": 7,
            }
        )
    require_complete_snapshot({"complete_snapshot": True})


def test_scratch_object_ids_for_dataset_scopes_deletion() -> None:
    objects = [
        {"id": "a", "metadata": {"dataset": "keep-me", "kind": "map-layer"}},
        {"id": "b", "metadata": {"dataset": "target", "kind": "map-layer"}},
        {"id": "c", "metadata": {"dataset": "target", "layer": "analysis"}},
        {"id": "d", "metadata": {"dataset": "other"}},
        {"id": "e", "metadata": {}},
        {"id": "f"},
    ]
    assert scratch_object_ids_for_dataset(objects, "target") == ["b", "c"]
    assert scratch_object_ids_for_dataset(objects, "missing") == []


def test_real_data_config_defaults_disable_long_range_sim() -> None:
    assert analysis_config_dict(_Config())["allow_long_range_sim_ears"] is False
    assert localization_is_published(None) is False


def test_counts_from_result_separate_published_and_withheld() -> None:
    members = [
        _assembled(observation_id="o1", node_id="EAR-001", lat=59.1, lon=18.1),
    ]
    events = [
        _event(
            event_id="e1",
            members=members,
            localization=_localization(lat=59.1, lon=18.1, contributing=["EAR-001"]),
        ),
        _event(
            event_id="e2",
            members=members,
            localization=_localization(lat=None, lon=None, status="unavailable"),
        ),
    ]
    counts = counts_from_result(
        _result(members, events),
        reports_fetched=10,
        reports_after_filter=8,
        observations_after_filter=1,
    )
    assert counts == AnalysisRunCounts(
        reports_fetched=10,
        reports_after_filter=8,
        observations_assembled=1,
        observations_after_filter=1,
        events=2,
        localizations_published=1,
        localizations_withheld=1,
    )
