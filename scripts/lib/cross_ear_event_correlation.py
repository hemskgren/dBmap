"""Script-local Cross-EAR Event Candidate / Event correlation (experimental).

Not a Hub Event store. Association never uses simulator ground-truth hints
(``simulated_source_id``, scenario IDs, or source trajectories).

Association and localization are separate concerns: scenario-authored
``source_family`` does not award acoustic association points; bearing geometry
quality feeds localization diagnostics, not association score inflation.
"""

from __future__ import annotations

import hashlib
import math
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta
from typing import Any, Literal

from lib.cross_ear_bearing_geometry import (
    analyze_pair,
    prepare_observation_geometry,
)
from lib.cross_ear_localization import (
    BearingSample,
    LocalizationResult,
    estimate_localization,
    extract_bearing_samples,
)

DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS = 30.0
DEFAULT_MAX_ASSOCIATION_DISTANCE_M = 500.0
DEFAULT_MIN_EARS_FOR_EVENT = 3
# ear_diversity(2) + temporal_overlap(2) == 4; optional bearing_presence +1.
DEFAULT_MIN_PROMOTE_SCORE = 4

EvidenceStatus = Literal["compatible", "incompatible", "abstain"]
CandidateStatus = Literal[
    "EventCandidate",
    "Promoted",
    "Rejected",
    "Expired",
]
# Script-local join / membership diagnostic labels (B2.5 audit / B2.7 Phase A).
JoinDecision = Literal[
    "accepted",
    "rejected",
    "not_eligible",
    "ear_already_represented",
    "unresolved_multi_compatible",
    # Retained for diagnostic compatibility; no longer produced after B2.7 Phase A.
    "accepted_but_event_frozen",
]
MembershipStage = Literal[
    "available",
    "assembled",
    "pair_accepted",
    "pair_rejected",
    "candidate_member",
    "event_member",
    "associated_but_not_in_event",
    "associated_but_not_in_consensus",
    "outlier_excluded",
    "unresolved_multi_compatible",
]


def _finite_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value):
        return None
    return float(value)


def _classification_for_reporting(classification: Any) -> dict[str, Any] | None:
    """Retain classification for reporting/evaluation; mark simulator GT when hinted."""
    if not isinstance(classification, dict):
        return None
    cleaned: dict[str, Any] = {}
    family = classification.get("source_family")
    if isinstance(family, str) and family.strip():
        cleaned["source_family"] = family
    confidence = _finite_number(classification.get("confidence"))
    if confidence is not None:
        cleaned["confidence"] = confidence
    schema = classification.get("schema")
    if isinstance(schema, str):
        cleaned["schema"] = schema
    version = classification.get("version")
    if isinstance(version, int) and not isinstance(version, bool):
        cleaned["version"] = version
    hints = classification.get("hints")
    simulated = None
    if isinstance(hints, dict):
        raw = hints.get("simulated_source_id")
        if isinstance(raw, str) and raw.strip():
            simulated = raw
            cleaned["simulated_source_id"] = raw
    # Scripted SIM payloads copy scenario family into classification; treat as GT.
    if simulated is not None or cleaned.get("source_family") is not None:
        cleaned["association_policy"] = (
            "simulator_or_unverified_classification; ignored for association score"
        )
    return cleaned or None


@dataclass
class AssembledObservation:
    """One Observation lifecycle assembled from MQTT/Hub reports."""

    observation_id: str
    node_id: str
    reports: list[dict[str, Any]]
    first_time: datetime | None
    last_time: datetime | None
    interval_start: datetime | None
    interval_end: datetime | None
    report_count: int
    emission_counts: dict[str, int]
    lifecycle_complete: bool
    sequence_gaps: int
    sequences: list[int]
    bearing_deg: float | None
    bearing_confidence: float | None
    bearing_reference: str | None
    bearing_samples: list[BearingSample]
    classification: dict[str, Any] | None
    signal_level_dbfs: float | None
    measurements: dict[str, Any] | None
    signature: dict[str, Any] | None
    timing_quality: str | None
    timestamp_uncertainty_ms: float | None
    capture_timestamp_monotonic_us: int | None
    installation_latitude: float | None = None
    installation_longitude: float | None = None
    installation_orientation_deg: float | None = None

    @property
    def key(self) -> tuple[str, str]:
        return (self.node_id, self.observation_id)


@dataclass(frozen=True)
class AssociationEvidence:
    accepted: bool
    rejected: bool
    reason: str
    temporal: EvidenceStatus
    temporal_notes: str
    installation: EvidenceStatus
    installation_notes: str
    installation_distance_m: float | None
    bearing: EvidenceStatus
    bearing_notes: str
    acoustic: EvidenceStatus
    acoustic_notes: str
    score_delta: int
    pair_geometry: Any | None = None
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class EventCandidate:
    candidate_id: str
    status: CandidateStatus
    observation_ids: list[str]
    ear_ids: list[str]
    members: list[AssembledObservation]
    evidence_notes: list[str]
    score: int
    score_breakdown: dict[str, int]
    created_at: datetime | None
    updated_at: datetime | None
    qualitative_uncertainty: str
    rejected_joins: list[dict[str, Any]] = field(default_factory=list)
    localization: LocalizationResult | None = None
    # Backward-compatible summary mirror of localization (optional coarse locus).
    provisional_position: dict[str, Any] | None = None
    event_id: str | None = None


@dataclass
class CorrelatedEvent:
    event_id: str
    candidate_id: str
    observation_ids: list[str]
    ear_ids: list[str]
    members: list[AssembledObservation]
    score: int
    score_breakdown: dict[str, int]
    evidence_notes: list[str]
    qualitative_uncertainty: str
    localization: LocalizationResult | None
    provisional_position: dict[str, Any] | None
    created_at: datetime | None
    promoted_at: datetime | None


@dataclass
class RejectedAssociation:
    observation_id: str
    node_id: str
    against: str
    reason: str
    evidence: AssociationEvidence


@dataclass
class UnresolvedAssociation:
    """Observation compatible with multiple Candidates/Events (B2.7).

    Left unassigned — never first-match or arrival-order forced.
    """

    observation_id: str
    node_id: str
    compatible_candidate_ids: list[str]
    compatible_event_ids: list[str]
    reason: str
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class JoinAttemptDiagnostic:
    """One Candidate join evaluation (script-local B2.5 diagnostic)."""

    candidate_id: str
    candidate_status: str
    new_observation_id: str
    new_node_id: str
    existing_member_ids: list[str]
    existing_ear_ids: list[str]
    pairwise: list[dict[str, Any]]
    decision: JoinDecision
    reason: str


@dataclass
class CorrelationResult:
    observations: list[AssembledObservation]
    candidates: list[EventCandidate]
    events: list[CorrelatedEvent]
    rejected: list[RejectedAssociation]
    expired: list[EventCandidate]
    parameters: dict[str, Any]
    join_attempts: list[JoinAttemptDiagnostic] = field(default_factory=list)
    unresolved: list[UnresolvedAssociation] = field(default_factory=list)


FlattenFields = Callable[[dict[str, Any]], dict[str, Any]]
ObservationTime = Callable[[dict[str, Any]], datetime | None]
SequenceGapCount = Callable[[list[int]], int]
InstallationDistance = Callable[[dict[str, Any], dict[str, Any]], float | None]
IsSimEar = Callable[[str], bool]


def _report_end_time(
    report: dict[str, Any],
    *,
    observation_fields: FlattenFields,
    observation_time: ObservationTime,
) -> datetime | None:
    start = observation_time(report)
    if start is None:
        return None
    duration_ms = observation_fields(report).get("duration_ms", 0)
    if (
        not isinstance(duration_ms, (int, float))
        or isinstance(duration_ms, bool)
        or not math.isfinite(duration_ms)
    ):
        duration_ms = 0
    return start + timedelta(milliseconds=max(float(duration_ms), 0.0))


def _pick_bearing(
    reports: list[dict[str, Any]],
    *,
    observation_fields: FlattenFields,
) -> tuple[float | None, float | None, str | None]:
    best: tuple[float, float | None, str | None] | None = None
    for report in reports:
        fields = observation_fields(report)
        bearing = fields.get("bearing")
        if not isinstance(bearing, dict):
            continue
        deg = _finite_number(bearing.get("deg"))
        if deg is None or not 0 <= deg < 360:
            continue
        confidence = _finite_number(bearing.get("confidence"))
        reference = bearing.get("reference")
        ref = reference if isinstance(reference, str) else None
        rank = confidence if confidence is not None else -1.0
        if best is None or rank >= (best[1] if best[1] is not None else -1.0):
            best = (deg, confidence, ref)
    if best is None:
        return None, None, None
    return best


def _pick_classification(
    reports: list[dict[str, Any]],
    *,
    observation_fields: FlattenFields,
) -> dict[str, Any] | None:
    chosen: dict[str, Any] | None = None
    best_confidence = -1.0
    for report in reports:
        fields = observation_fields(report)
        cleaned = _classification_for_reporting(fields.get("classification"))
        if cleaned is None:
            continue
        confidence = cleaned.get("confidence")
        rank = float(confidence) if isinstance(confidence, (int, float)) else -1.0
        if chosen is None or rank >= best_confidence:
            chosen = cleaned
            best_confidence = rank
    return chosen


def _hierarchical_acoustic(report: dict[str, Any]) -> tuple[Any, Any]:
    acoustic = report.get("acoustic")
    if isinstance(acoustic, dict):
        return acoustic.get("measurements"), acoustic.get("signature")
    return None, None


def assemble_observations(
    reports: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]] | None,
    *,
    observation_fields: FlattenFields,
    observation_time: ObservationTime,
    sequence_gap_count: SequenceGapCount,
    sort_observations: Callable[[list[dict[str, Any]]], list[dict[str, Any]]],
) -> list[AssembledObservation]:
    """Group reports by (node_id, observation_id) into analysis Observations."""
    nodes = nodes or {}
    groups: dict[tuple[str, str], dict[str, Any]] = {}
    order: list[tuple[str, str]] = []
    for report in sort_observations(reports):
        fields = observation_fields(report)
        raw_node = fields.get("node_id")
        raw_oid = fields.get("observation_id")
        if not isinstance(raw_node, str) or not raw_node:
            continue
        if not isinstance(raw_oid, str) or not raw_oid:
            continue
        key = (raw_node, raw_oid)
        group = groups.get(key)
        if group is None:
            group = {
                "reports": [],
                "emission_counts": {
                    "start": 0,
                    "periodic": 0,
                    "end": 0,
                    "complete": 0,
                    "unknown": 0,
                },
                "sequences": [],
            }
            groups[key] = group
            order.append(key)
        group["reports"].append(report)
        kind = fields.get("emission_kind")
        if isinstance(kind, str) and kind.lower() in group["emission_counts"]:
            group["emission_counts"][kind.lower()] += 1
        else:
            group["emission_counts"]["unknown"] += 1
        sequence = fields.get("sequence_number")
        if isinstance(sequence, int) and not isinstance(sequence, bool):
            group["sequences"].append(sequence)

    assembled: list[AssembledObservation] = []
    for node_id, observation_id in order:
        group = groups[(node_id, observation_id)]
        report_list: list[dict[str, Any]] = group["reports"]
        times = [observation_time(item) for item in report_list]
        known_times = [item for item in times if item is not None]
        first_time = min(known_times) if known_times else None
        last_time = max(known_times) if known_times else None
        ends = [
            end
            for item in report_list
            for end in [
                _report_end_time(
                    item,
                    observation_fields=observation_fields,
                    observation_time=observation_time,
                )
            ]
            if end is not None
        ]
        interval_start = first_time
        interval_end = max(ends) if ends else last_time
        counts = group["emission_counts"]
        lifecycle_complete = counts["complete"] > 0 or (
            counts["start"] > 0 and counts["end"] > 0
        )
        bearing_deg, bearing_confidence, bearing_reference = _pick_bearing(
            report_list, observation_fields=observation_fields
        )
        bearing_samples = extract_bearing_samples(
            report_list,
            observation_fields=observation_fields,
            observation_time=observation_time,
        )
        classification = _pick_classification(
            report_list, observation_fields=observation_fields
        )
        signal = None
        timing_quality = None
        uncertainty = None
        mono = None
        measurements = None
        signature = None
        for report in reversed(report_list):
            fields = observation_fields(report)
            if signal is None:
                signal = _finite_number(fields.get("signal_level_dbfs"))
            context = report.get("context")
            if isinstance(context, dict):
                if timing_quality is None and isinstance(
                    context.get("timing_quality"), str
                ):
                    timing_quality = context["timing_quality"]
                if uncertainty is None:
                    uncertainty = _finite_number(
                        context.get("timestamp_uncertainty_ms")
                    )
                if mono is None:
                    raw_mono = context.get("capture_timestamp_monotonic_us")
                    if isinstance(raw_mono, int) and not isinstance(raw_mono, bool):
                        mono = raw_mono
            meas, sig = _hierarchical_acoustic(report)
            if measurements is None and isinstance(meas, dict):
                measurements = meas
            if signature is None and isinstance(sig, dict):
                signature = sig

        installation = None
        node = nodes.get(node_id)
        if isinstance(node, dict):
            raw_installation = node.get("installation")
            if isinstance(raw_installation, dict):
                installation = raw_installation

        assembled.append(
            AssembledObservation(
                observation_id=observation_id,
                node_id=node_id,
                reports=report_list,
                first_time=first_time,
                last_time=last_time,
                interval_start=interval_start,
                interval_end=interval_end,
                report_count=len(report_list),
                emission_counts=dict(counts),
                lifecycle_complete=lifecycle_complete,
                sequence_gaps=sequence_gap_count(group["sequences"]),
                sequences=list(group["sequences"]),
                bearing_deg=bearing_deg,
                bearing_confidence=bearing_confidence,
                bearing_reference=bearing_reference,
                bearing_samples=bearing_samples,
                classification=classification,
                signal_level_dbfs=signal,
                measurements=measurements,
                signature=signature,
                timing_quality=timing_quality,
                timestamp_uncertainty_ms=uncertainty,
                capture_timestamp_monotonic_us=mono,
                installation_latitude=(
                    _finite_number(installation.get("latitude"))
                    if installation
                    else None
                ),
                installation_longitude=(
                    _finite_number(installation.get("longitude"))
                    if installation
                    else None
                ),
                installation_orientation_deg=(
                    _finite_number(installation.get("orientation_deg"))
                    if installation
                    else None
                ),
            )
        )
    return assembled


def _stable_id(prefix: str, observation_ids: list[str]) -> str:
    digest = hashlib.sha1(
        "|".join(sorted(observation_ids)).encode("utf-8")
    ).hexdigest()[:12]
    return f"{prefix}-{digest}"


def _interval_relationship(
    first: AssembledObservation,
    second: AssembledObservation,
    *,
    max_gap_seconds: float,
) -> tuple[EvidenceStatus, str, bool]:
    """Temporal compatibility from Observation intervals (not TDOA)."""
    if (
        first.interval_start is None
        or first.interval_end is None
        or second.interval_start is None
        or second.interval_end is None
    ):
        return (
            "abstain",
            "observation interval incomplete; temporal check abstains",
            False,
        )

    start = max(first.interval_start, second.interval_start)
    end = min(first.interval_end, second.interval_end)
    overlap_s = (end - start).total_seconds()
    if overlap_s >= 0:
        return (
            "compatible",
            f"intervals overlap by {overlap_s:.1f}s",
            True,
        )

    gap_s = -overlap_s
    if gap_s <= max_gap_seconds:
        return (
            "compatible",
            f"interval gap {gap_s:.1f}s ≤ {max_gap_seconds:g}s association allowance",
            False,
        )
    return (
        "incompatible",
        f"interval gap {gap_s:.1f}s exceeds {max_gap_seconds:g}s association allowance",
        False,
    )


def _installation_relationship(
    first: AssembledObservation,
    second: AssembledObservation,
    *,
    max_association_distance_m: float,
    allow_long_range_sim_ears: bool,
    installation_distance_m: InstallationDistance,
    is_sim_ear: IsSimEar,
) -> tuple[EvidenceStatus, str, float | None]:
    if (
        first.installation_latitude is None
        or first.installation_longitude is None
        or second.installation_latitude is None
        or second.installation_longitude is None
    ):
        return (
            "abstain",
            "installation coordinates missing; distance check abstains",
            None,
        )
    distance = installation_distance_m(
        {
            "latitude": first.installation_latitude,
            "longitude": first.installation_longitude,
        },
        {
            "latitude": second.installation_latitude,
            "longitude": second.installation_longitude,
        },
    )
    if distance is None:
        return "abstain", "installation distance unavailable", None
    if distance <= max_association_distance_m:
        return (
            "compatible",
            f"site distance {distance:.0f}m ≤ {max_association_distance_m:g}m",
            distance,
        )
    both_sim = is_sim_ear(first.node_id) and is_sim_ear(second.node_id)
    if both_sim and allow_long_range_sim_ears:
        return (
            "compatible",
            (
                f"site distance {distance:.0f}m > {max_association_distance_m:g}m "
                "but SIM long-range analysis enabled"
            ),
            distance,
        )
    return (
        "incompatible",
        (
            f"site distance {distance:.0f}m exceeds "
            f"{max_association_distance_m:g}m association radius"
        ),
        distance,
    )


def _acoustic_relationship(
    first: AssembledObservation,
    second: AssembledObservation,
) -> tuple[EvidenceStatus, str]:
    """Acoustic association channel.

    Scenario-authored ``source_family`` / confidence are not independent acoustic
    evidence. Always abstain for association scoring until a verified non-GT
    classifier is available. Values may still appear on AssembledObservation for
    evaluation/reporting.
    """
    del first, second
    return (
        "abstain",
        ("acoustic association abstains "
        "(scenario/unverified source_family not used for association score)"),
    )


def _geometry_input(observation: AssembledObservation) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "observation_id": observation.observation_id,
        "node_id": observation.node_id,
        "event_time_utc": (
            observation.first_time.astimezone(UTC).isoformat()
            if observation.first_time is not None
            else None
        ),
        "classification": observation.classification,
    }
    if observation.bearing_deg is not None:
        bearing: dict[str, Any] = {
            "deg": observation.bearing_deg,
            "reference": observation.bearing_reference or "node",
        }
        if observation.bearing_confidence is not None:
            bearing["confidence"] = observation.bearing_confidence
        payload["bearing"] = bearing
    return payload


def _bearing_relationship(
    first: AssembledObservation,
    second: AssembledObservation,
    nodes: dict[str, dict[str, Any]],
) -> tuple[EvidenceStatus, str, Any | None, int]:
    """Association-only bearing gate.

    Presence of non-contradictory bearings is weak independent sensor evidence
    (+1). Forward-intersection quality is NOT used to inflate association score;
    that belongs to localization diagnostics.
    """
    if not first.bearing_samples or not second.bearing_samples:
        return (
            "abstain",
            "bearing missing on one or both Observations; association bearing abstains",
            None,
            0,
        )
    geo_a = prepare_observation_geometry(_geometry_input(first), nodes)
    geo_b = prepare_observation_geometry(_geometry_input(second), nodes)
    if not geo_a.eligible or not geo_b.eligible:
        # Bearings present but installation/orientation incomplete — abstain.
        return (
            "compatible",
            ("bearings present; geometry ineligible for contradiction check "
            "(localization will report detail)"),
            None,
            1,
        )
    pair = analyze_pair(geo_a, geo_b)
    if pair.intersection_status in {"behind_both", "behind_one"}:
        return (
            "incompatible",
            f"bearing geometry contradictory for association ({pair.geometric_notes})",
            pair,
            0,
        )
    return (
        "compatible",
        ("bearings present and not contradictory "
        "(intersection quality assessed in localization, not association score)"),
        pair,
        1,
    )


def evaluate_observation_pair(
    first: AssembledObservation,
    second: AssembledObservation,
    nodes: dict[str, dict[str, Any]],
    *,
    association_temporal_gap_seconds: float,
    max_association_distance_m: float,
    allow_long_range_sim_ears: bool,
    installation_distance_m: InstallationDistance,
    is_sim_ear: IsSimEar,
) -> AssociationEvidence:
    if first.node_id == second.node_id:
        return AssociationEvidence(
            accepted=False,
            rejected=True,
            reason="same Ear cannot create Cross-EAR support",
            temporal="abstain",
            temporal_notes="not evaluated",
            installation="abstain",
            installation_notes="not evaluated",
            installation_distance_m=None,
            bearing="abstain",
            bearing_notes="not evaluated",
            acoustic="abstain",
            acoustic_notes="not evaluated",
            score_delta=0,
        )
    if first.observation_id == second.observation_id:
        return AssociationEvidence(
            accepted=False,
            rejected=True,
            reason="duplicate Observation identity",
            temporal="abstain",
            temporal_notes="not evaluated",
            installation="abstain",
            installation_notes="not evaluated",
            installation_distance_m=None,
            bearing="abstain",
            bearing_notes="not evaluated",
            acoustic="abstain",
            acoustic_notes="not evaluated",
            score_delta=0,
        )

    temporal, temporal_notes, overlapped = _interval_relationship(
        first,
        second,
        max_gap_seconds=association_temporal_gap_seconds,
    )
    installation, installation_notes, distance = _installation_relationship(
        first,
        second,
        max_association_distance_m=max_association_distance_m,
        allow_long_range_sim_ears=allow_long_range_sim_ears,
        installation_distance_m=installation_distance_m,
        is_sim_ear=is_sim_ear,
    )
    bearing, bearing_notes, pair_geometry, bearing_score = _bearing_relationship(
        first, second, nodes
    )
    acoustic, acoustic_notes = _acoustic_relationship(first, second)

    hard = [
        name
        for name, status in (
            ("temporal", temporal),
            ("installation", installation),
            ("bearing", bearing),
            ("acoustic", acoustic),
        )
        if status == "incompatible"
    ]
    if hard:
        return AssociationEvidence(
            accepted=False,
            rejected=True,
            reason="incompatible: " + ", ".join(hard),
            temporal=temporal,
            temporal_notes=temporal_notes,
            installation=installation,
            installation_notes=installation_notes,
            installation_distance_m=distance,
            bearing=bearing,
            bearing_notes=bearing_notes,
            acoustic=acoustic,
            acoustic_notes=acoustic_notes,
            score_delta=0,
            pair_geometry=pair_geometry,
        )

    # Require temporal compatibility to associate; other dims may abstain.
    if temporal != "compatible":
        return AssociationEvidence(
            accepted=False,
            rejected=False,
            reason="temporal evidence abstains; no association",
            temporal=temporal,
            temporal_notes=temporal_notes,
            installation=installation,
            installation_notes=installation_notes,
            installation_distance_m=distance,
            bearing=bearing,
            bearing_notes=bearing_notes,
            acoustic=acoustic,
            acoustic_notes=acoustic_notes,
            score_delta=0,
            pair_geometry=pair_geometry,
        )

    # Association score for this pair (candidate score uses aggregate breakdown).
    score = 1  # distinct-Ear association
    if overlapped:
        score += 2
    else:
        score += 1
    score += bearing_score  # 0 or 1; not localization quality
    # acoustic never contributes under current policy

    notes = [
        temporal_notes,
        installation_notes,
        bearing_notes,
        acoustic_notes,
    ]
    return AssociationEvidence(
        accepted=True,
        rejected=False,
        reason="; ".join(notes),
        temporal=temporal,
        temporal_notes=temporal_notes,
        installation=installation,
        installation_notes=installation_notes,
        installation_distance_m=distance,
        bearing=bearing,
        bearing_notes=bearing_notes,
        acoustic=acoustic,
        acoustic_notes=acoustic_notes,
        score_delta=score,
        pair_geometry=pair_geometry,
        details={"overlapped": overlapped},
    )


def _score_candidate(members: list[AssembledObservation], pair_scores: list[AssociationEvidence]) -> tuple[int, dict[str, int], str]:
    ear_count = len({item.node_id for item in members})
    breakdown = {
        "ear_diversity": max(0, ear_count - 1),
        "temporal": 0,
        "bearing": 0,
        "acoustic": 0,  # retained for transparency; always 0 under GT-safe policy
    }
    for evidence in pair_scores:
        if evidence.details.get("overlapped"):
            breakdown["temporal"] = max(breakdown["temporal"], 2)
        elif evidence.temporal == "compatible":
            breakdown["temporal"] = max(breakdown["temporal"], 1)
        if evidence.bearing == "compatible":
            breakdown["bearing"] = max(breakdown["bearing"], 1)
    score = sum(breakdown.values())
    uncertainty = "moderate"
    if breakdown["temporal"] < 2 and breakdown["bearing"] == 0:
        uncertainty = "high"
    elif breakdown["temporal"] >= 2 and ear_count >= 3:
        uncertainty = "lower"
    return score, breakdown, uncertainty


def _localization_summary(localization: LocalizationResult | None) -> dict[str, Any] | None:
    if localization is None or localization.latitude is None or localization.longitude is None:
        return None
    return {
        "label": (
            f"localization status={localization.status} "
            "(not calibrated; separate from association)"
        ),
        "latitude": localization.latitude,
        "longitude": localization.longitude,
        "status": localization.status,
        "max_bearing_dt_s": localization.max_bearing_dt_s,
        "pairwise_spread_m": localization.pairwise_spread_m,
        "calibrated_uncertainty_m": localization.calibrated_uncertainty_m,
        "selection_method": localization.selection_method,
    }


def _pairwise_diag(member: AssembledObservation, evidence: AssociationEvidence) -> dict[str, Any]:
    return {
        "member_observation_id": member.observation_id,
        "member_node_id": member.node_id,
        "accepted": evidence.accepted,
        "rejected": evidence.rejected,
        "temporal": evidence.temporal,
        "temporal_notes": evidence.temporal_notes,
        "bearing": evidence.bearing,
        "bearing_notes": evidence.bearing_notes,
        "installation": evidence.installation,
        "installation_notes": evidence.installation_notes,
        "installation_distance_m": evidence.installation_distance_m,
        "acoustic": evidence.acoustic,
        "reason": evidence.reason,
    }


def build_membership_stage_trace(
    result: CorrelationResult,
) -> list[dict[str, Any]]:
    """Stage-by-stage Observation membership from assembly through localization.

    Distinguishes availability / association / Candidate / Event / consensus.
    """
    event_obs = {
        obs_id
        for event in result.events
        for obs_id in event.observation_ids
    }
    candidate_obs = {
        obs_id
        for candidate in result.candidates
        for obs_id in candidate.observation_ids
    }
    frozen_compatible = {
        attempt.new_observation_id
        for attempt in result.join_attempts
        if attempt.decision == "accepted_but_event_frozen"
    }
    unresolved_ids = {item.observation_id for item in result.unresolved}
    primary_loc = (
        result.events[0].localization
        if result.events and result.events[0].localization is not None
        else None
    )
    contributing = set(primary_loc.contributing_ears) if primary_loc else set()
    outlier_pairs = [
        f"{pair.ear_a_id}↔{pair.ear_b_id}"
        for pair in (primary_loc.pairs if primary_loc else [])
        if pair.outlier
    ]

    rows: list[dict[str, Any]] = []
    for obs in result.observations:
        stages: list[str] = ["available", "assembled"]
        if obs.observation_id in candidate_obs:
            stages.append("candidate_member")
        if obs.observation_id in event_obs:
            stages.append("event_member")
        elif obs.observation_id in frozen_compatible:
            stages.append("associated_but_not_in_event")
        if obs.observation_id in unresolved_ids:
            stages.append("unresolved_multi_compatible")

        consensus_role = "n/a"
        if obs.observation_id in event_obs and primary_loc is not None:
            forward_pairs = [
                pair
                for pair in primary_loc.pairs
                if obs.node_id in (pair.ear_a_id, pair.ear_b_id)
                and pair.intersection_status == "forward_intersection"
            ]
            if not forward_pairs:
                stages.append("associated_but_not_in_consensus")
                consensus_role = "no_forward_pair"
            elif all(pair.outlier for pair in forward_pairs):
                stages.append("outlier_excluded")
                consensus_role = "outlier_excluded"
            else:
                consensus_role = "in_consensus"

        rows.append(
            {
                "observation_id": obs.observation_id,
                "node_id": obs.node_id,
                "interval_start": (
                    obs.interval_start.isoformat() if obs.interval_start else None
                ),
                "interval_end": (
                    obs.interval_end.isoformat() if obs.interval_end else None
                ),
                "in_candidate": obs.observation_id in candidate_obs,
                "in_event": obs.observation_id in event_obs,
                "compatible_but_event_frozen": obs.observation_id in frozen_compatible,
                "unresolved_multi_compatible": obs.observation_id in unresolved_ids,
                "in_localization_contributing_ears": obs.node_id in contributing,
                "outlier_pairs_involving_ear": [
                    pair for pair in outlier_pairs if obs.node_id in pair
                ],
                "consensus_role": consensus_role,
                "stages": stages,
                "associated_observation": obs.observation_id in event_obs,
                "localization_contributor": (
                    obs.node_id in contributing and obs.observation_id in event_obs
                ),
            }
        )
    return rows


def format_join_attempt_lines(
    attempts: list[JoinAttemptDiagnostic],
    *,
    limit: int = 40,
) -> list[str]:
    lines = [f"  Candidate join attempts (diagnostic): {len(attempts)}"]
    if not attempts:
        lines.append("    none")
        return lines
    for attempt in attempts[:limit]:
        lines.append(
            f"    {attempt.decision}: obs={attempt.new_observation_id} "
            f"({attempt.new_node_id}) → {attempt.candidate_id} "
            f"[{attempt.candidate_status}] members={attempt.existing_ear_ids}"
        )
        lines.append(f"      reason: {attempt.reason}")
        for pair in attempt.pairwise:
            lines.append(
                f"      vs {pair['member_node_id']}: "
                f"accepted={pair['accepted']} rejected={pair['rejected']} "
                f"temporal={pair['temporal']} bearing={pair['bearing']} "
                f"installation={pair['installation']} "
                f"dist_m={pair['installation_distance_m']}"
            )
    if len(attempts) > limit:
        lines.append(f"    … {len(attempts) - limit} more join attempts")
    return lines


def format_membership_trace_lines(result: CorrelationResult) -> list[str]:
    rows = build_membership_stage_trace(result)
    lines = ["  Membership stage trace (assembly → Event → localization):"]
    for row in rows:
        lines.append(
            f"    {row['node_id']} obs={row['observation_id']}: "
            f"candidate={row['in_candidate']} event={row['in_event']} "
            f"unresolved={row.get('unresolved_multi_compatible', False)} "
            f"loc_contributor={row.get('localization_contributor', False)} "
            f"consensus_role={row.get('consensus_role', 'n/a')}"
        )
        lines.append(
            f"      interval={row['interval_start']}..{row['interval_end']}"
        )
    return lines


def correlate_cross_ear_events(
    assembled: list[AssembledObservation],
    nodes: dict[str, dict[str, Any]] | None,
    *,
    association_temporal_gap_seconds: float = DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
    max_association_distance_m: float = DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
    allow_long_range_sim_ears: bool = False,
    min_ears_for_event: int = DEFAULT_MIN_EARS_FOR_EVENT,
    min_promote_score: int = DEFAULT_MIN_PROMOTE_SCORE,
    installation_distance_m: InstallationDistance,
    is_sim_ear: IsSimEar,
) -> CorrelationResult:
    """Build Event Candidates / Events from assembled Observations."""
    nodes = nodes or {}
    parameters = {
        "association_temporal_gap_seconds": association_temporal_gap_seconds,
        "max_association_distance_m": max_association_distance_m,
        "allow_long_range_sim_ears": allow_long_range_sim_ears,
        "min_ears_for_event": min_ears_for_event,
        "min_promote_score": min_promote_score,
        "ground_truth_policy": "ignored_for_association_and_localization",
        "acoustic_association_policy": "abstain_scenario_or_unverified_classification",
        "localization_policy": "separate_from_association",
    }

    ordered = sorted(
        assembled,
        key=lambda item: (
            item.interval_start is None,
            item.interval_start or datetime.max.replace(tzinfo=UTC),
            item.node_id,
            item.observation_id,
        ),
    )

    unassigned: list[AssembledObservation] = []
    candidates: list[EventCandidate] = []
    events: list[CorrelatedEvent] = []
    rejected: list[RejectedAssociation] = []
    unresolved: list[UnresolvedAssociation] = []
    join_attempts: list[JoinAttemptDiagnostic] = []
    membership: dict[str, str] = {}
    pair_evidence_by_candidate: dict[str, list[AssociationEvidence]] = {}

    def sync_event_from_candidate(candidate: EventCandidate) -> None:
        """Keep CorrelatedEvent membership/localization in sync after post-promote growth."""
        if candidate.status != "Promoted" or not candidate.event_id:
            return
        for event in events:
            if event.event_id != candidate.event_id:
                continue
            event.observation_ids = list(candidate.observation_ids)
            event.ear_ids = list(candidate.ear_ids)
            event.members = list(candidate.members)
            event.score = candidate.score
            event.score_breakdown = dict(candidate.score_breakdown)
            event.evidence_notes = list(candidate.evidence_notes)
            event.qualitative_uncertainty = candidate.qualitative_uncertainty
            event.localization = candidate.localization
            event.provisional_position = candidate.provisional_position
            return

    def maybe_promote(candidate: EventCandidate) -> None:
        ear_count = len(candidate.ear_ids)
        if candidate.status != "EventCandidate":
            # Already promoted: still refresh the mirrored Event after growth.
            sync_event_from_candidate(candidate)
            return
        if ear_count < min_ears_for_event:
            return
        if candidate.score < min_promote_score:
            candidate.evidence_notes.append(
                f"promotion withheld: score {candidate.score} < {min_promote_score}"
            )
            return
        breakdown = candidate.score_breakdown
        # Require overlapping intervals (temporal=2). Gap-only + ear count is not enough.
        if breakdown.get("temporal", 0) < 2:
            candidate.evidence_notes.append(
                "promotion withheld: require overlapping Observation intervals "
                "(gap-only temporal compatibility is insufficient)"
            )
            return
        event_id = _stable_id("event", candidate.observation_ids)
        candidate.status = "Promoted"
        candidate.event_id = event_id
        promoted_at = candidate.updated_at
        events.append(
            CorrelatedEvent(
                event_id=event_id,
                candidate_id=candidate.candidate_id,
                observation_ids=list(candidate.observation_ids),
                ear_ids=list(candidate.ear_ids),
                members=list(candidate.members),
                score=candidate.score,
                score_breakdown=dict(candidate.score_breakdown),
                evidence_notes=list(candidate.evidence_notes),
                qualitative_uncertainty=candidate.qualitative_uncertainty,
                localization=candidate.localization,
                provisional_position=candidate.provisional_position,
                created_at=candidate.created_at,
                promoted_at=promoted_at,
            )
        )

    def refresh_candidate(candidate: EventCandidate) -> None:
        evidences = pair_evidence_by_candidate.get(candidate.candidate_id, [])
        score, breakdown, uncertainty = _score_candidate(candidate.members, evidences)
        candidate.score = score
        candidate.score_breakdown = breakdown
        candidate.qualitative_uncertainty = uncertainty
        localization = estimate_localization(candidate.members, nodes)
        candidate.localization = localization
        candidate.provisional_position = _localization_summary(localization)
        candidate.ear_ids = sorted({item.node_id for item in candidate.members})
        candidate.observation_ids = [item.observation_id for item in candidate.members]
        maybe_promote(candidate)

    def evaluate_join(
        candidate: EventCandidate, obs: AssembledObservation
    ) -> tuple[
        str,
        list[AssociationEvidence],
        list[dict[str, Any]],
        str,
        AssociationEvidence | None,
    ]:
        """Return (decision, evidences, pairwise, reason, blocking_evidence)."""
        against_label = (
            candidate.event_id
            if candidate.status == "Promoted" and candidate.event_id
            else candidate.candidate_id
        )
        pairwise_rows: list[dict[str, Any]] = []
        if obs.node_id in candidate.ear_ids:
            member_rejects: list[AssociationEvidence] = []
            member_accepts: list[AssociationEvidence] = []
            sample_evidence: AssociationEvidence | None = None
            for member in candidate.members:
                evidence = evaluate_observation_pair(
                    member,
                    obs,
                    nodes,
                    association_temporal_gap_seconds=association_temporal_gap_seconds,
                    max_association_distance_m=max_association_distance_m,
                    allow_long_range_sim_ears=allow_long_range_sim_ears,
                    installation_distance_m=installation_distance_m,
                    is_sim_ear=is_sim_ear,
                )
                if sample_evidence is None:
                    sample_evidence = evidence
                pairwise_rows.append(_pairwise_diag(member, evidence))
                if evidence.rejected:
                    member_rejects.append(evidence)
                elif evidence.accepted:
                    member_accepts.append(evidence)
            reason = (
                "Observation from an Ear already represented in the Candidate "
                f"({obs.node_id}); Cross-EAR join refused"
            )
            blocking = sample_evidence
            if member_rejects:
                reason = member_rejects[0].reason
                blocking = member_rejects[0]
            elif not member_accepts and sample_evidence is not None:
                reason = sample_evidence.reason
            return "ear_already_represented", [], pairwise_rows, reason, blocking

        member_evidences: list[AssociationEvidence] = []
        for member in candidate.members:
            evidence = evaluate_observation_pair(
                member,
                obs,
                nodes,
                association_temporal_gap_seconds=association_temporal_gap_seconds,
                max_association_distance_m=max_association_distance_m,
                allow_long_range_sim_ears=allow_long_range_sim_ears,
                installation_distance_m=installation_distance_m,
                is_sim_ear=is_sim_ear,
            )
            pairwise_rows.append(_pairwise_diag(member, evidence))
            if evidence.rejected or not evidence.accepted:
                return (
                    "rejected",
                    [],
                    pairwise_rows,
                    evidence.reason or f"not compatible with {against_label}",
                    evidence,
                )
            member_evidences.append(evidence)
        return (
            "compatible",
            member_evidences,
            pairwise_rows,
            f"compatible with all members of {against_label}",
            None,
        )

    for obs in ordered:
        if obs.observation_id in membership:
            continue

        compatible: list[
            tuple[EventCandidate, list[AssociationEvidence], list[dict[str, Any]]]
        ] = []
        for candidate in candidates:
            if candidate.status not in {"EventCandidate", "Promoted"}:
                continue
            against_label = (
                candidate.event_id
                if candidate.status == "Promoted" and candidate.event_id
                else candidate.candidate_id
            )
            decision, evidences, pairwise_rows, reason, blocking = evaluate_join(
                candidate, obs
            )
            if decision in {"ear_already_represented", "rejected"}:
                assert blocking is not None
                rejected.append(
                    RejectedAssociation(
                        observation_id=obs.observation_id,
                        node_id=obs.node_id,
                        against=against_label,
                        reason=reason,
                        evidence=blocking,
                    )
                )
                candidate.rejected_joins.append(
                    {
                        "observation_id": obs.observation_id,
                        "node_id": obs.node_id,
                        "reason": reason,
                        "decision": decision,
                        "pairwise": pairwise_rows,
                    }
                )
                join_attempts.append(
                    JoinAttemptDiagnostic(
                        candidate_id=candidate.candidate_id,
                        candidate_status=candidate.status,
                        new_observation_id=obs.observation_id,
                        new_node_id=obs.node_id,
                        existing_member_ids=[
                            m.observation_id for m in candidate.members
                        ],
                        existing_ear_ids=list(candidate.ear_ids),
                        pairwise=pairwise_rows,
                        decision=decision,  # type: ignore[arg-type]
                        reason=reason,
                    )
                )
                continue
            compatible.append((candidate, evidences, pairwise_rows))

        if len(compatible) > 1:
            cand_ids = [item[0].candidate_id for item in compatible]
            event_ids = sorted(
                {
                    item[0].event_id
                    for item in compatible
                    if item[0].status == "Promoted" and item[0].event_id
                }
            )
            reason = (
                "compatible with multiple Candidates/Events; left unresolved "
                "(no first-match assignment)"
            )
            unresolved.append(
                UnresolvedAssociation(
                    observation_id=obs.observation_id,
                    node_id=obs.node_id,
                    compatible_candidate_ids=cand_ids,
                    compatible_event_ids=event_ids,
                    reason=reason,
                    details={"pairwise_by_candidate": {
                        item[0].candidate_id: item[2] for item in compatible
                    }},
                )
            )
            for candidate, _evidences, pairwise_rows in compatible:
                join_attempts.append(
                    JoinAttemptDiagnostic(
                        candidate_id=candidate.candidate_id,
                        candidate_status=candidate.status,
                        new_observation_id=obs.observation_id,
                        new_node_id=obs.node_id,
                        existing_member_ids=[
                            m.observation_id for m in candidate.members
                        ],
                        existing_ear_ids=list(candidate.ear_ids),
                        pairwise=pairwise_rows,
                        decision="unresolved_multi_compatible",
                        reason=reason,
                    )
                )
            continue

        if len(compatible) == 1:
            candidate, member_evidences, pairwise_rows = compatible[0]
            prior_members = list(candidate.members)
            prior_ears = list(candidate.ear_ids)
            candidate.members.append(obs)
            candidate.updated_at = obs.interval_start or candidate.updated_at
            pair_evidence_by_candidate.setdefault(candidate.candidate_id, []).extend(
                member_evidences
            )
            membership[obs.observation_id] = candidate.candidate_id
            note = f"joined {obs.observation_id} from {obs.node_id}"
            if candidate.status == "Promoted":
                note += " (post-promotion membership growth)"
            candidate.evidence_notes.append(note)
            join_attempts.append(
                JoinAttemptDiagnostic(
                    candidate_id=candidate.candidate_id,
                    candidate_status=candidate.status,
                    new_observation_id=obs.observation_id,
                    new_node_id=obs.node_id,
                    existing_member_ids=[m.observation_id for m in prior_members],
                    existing_ear_ids=prior_ears,
                    pairwise=pairwise_rows,
                    decision="accepted",
                    reason=note,
                )
            )
            refresh_candidate(candidate)
            continue

        paired = False
        for index, other in enumerate(list(unassigned)):
            evidence = evaluate_observation_pair(
                other,
                obs,
                nodes,
                association_temporal_gap_seconds=association_temporal_gap_seconds,
                max_association_distance_m=max_association_distance_m,
                allow_long_range_sim_ears=allow_long_range_sim_ears,
                installation_distance_m=installation_distance_m,
                is_sim_ear=is_sim_ear,
            )
            if evidence.rejected:
                rejected.append(
                    RejectedAssociation(
                        observation_id=obs.observation_id,
                        node_id=obs.node_id,
                        against=other.observation_id,
                        reason=evidence.reason,
                        evidence=evidence,
                    )
                )
                continue
            if not evidence.accepted:
                continue
            observation_ids = [other.observation_id, obs.observation_id]
            candidate_id = _stable_id("cand", observation_ids)
            # Avoid duplicate Candidate for the same Observation pair.
            if any(
                set(item.observation_ids) == set(observation_ids)
                for item in candidates
            ):
                paired = True
                membership[obs.observation_id] = candidate_id
                membership[other.observation_id] = candidate_id
                unassigned.pop(index)
                break
            created = other.interval_start or obs.interval_start
            candidate = EventCandidate(
                candidate_id=candidate_id,
                status="EventCandidate",
                observation_ids=observation_ids,
                ear_ids=sorted({other.node_id, obs.node_id}),
                members=[other, obs],
                evidence_notes=[
                    (f"created from {other.observation_id} ({other.node_id}) + "
                    f"{obs.observation_id} ({obs.node_id})"),
                    evidence.reason,
                ],
                score=0,
                score_breakdown={},
                created_at=created,
                updated_at=obs.interval_start or created,
                qualitative_uncertainty="moderate",
            )
            pair_evidence_by_candidate[candidate_id] = [evidence]
            membership[other.observation_id] = candidate_id
            membership[obs.observation_id] = candidate_id
            unassigned.pop(index)
            candidates.append(candidate)
            refresh_candidate(candidate)
            paired = True
            break

        if not paired and obs.observation_id not in membership:
            unassigned.append(obs)

    parameters["membership_policy"] = "grow_after_promotion"
    parameters["multi_compatible_policy"] = "leave_unresolved"
    return CorrelationResult(
        observations=ordered,
        candidates=candidates,
        events=events,
        rejected=rejected,
        expired=[],  # batch analysis has no wall-clock expiry yet
        parameters=parameters,
        join_attempts=join_attempts,
        unresolved=unresolved,
    )
