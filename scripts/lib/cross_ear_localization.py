"""Script-local coarse localization diagnostics (experimental).

Association and localization are separate: a valid Event does not require a
reliable position. Near-zero two-ray residuals are not treated as calibrated
accuracy. No Hub persistence.
"""

from __future__ import annotations

import math
import random
from collections.abc import Callable, Sequence
from copy import deepcopy
from dataclasses import dataclass, field, replace
from datetime import UTC, datetime, timedelta
from itertools import combinations
from typing import Any, Literal

from lib.cross_ear_bearing_geometry import (
    analyze_pair,
    prepare_observation_geometry,
    smallest_angle_diff_deg,
)

EARTH_RADIUS_M = 6_371_000.0

# Soft timing gates for localization quality only (not association).
LOCALIZATION_GOOD_BEARING_DT_S = 0.5
LOCALIZATION_MAX_BEARING_DT_S = 1.0
# Pairwise intersection consistency heuristics (not calibrated σ).
# Dense (normal deployment ≤500 m) vs sparse (stress-test) networks differ.
LOCALIZATION_USEFUL_SPREAD_DENSE_M = 100.0
LOCALIZATION_USEFUL_SPREAD_SPARSE_M = 250.0
LOCALIZATION_USEFUL_SPREAD_M = LOCALIZATION_USEFUL_SPREAD_SPARSE_M  # backward compatible default
# Intersection-angle gates: acute LOB crossings are highly sensitive to bearing error.
# These are diagnostic heuristics, not calibrated angular uncertainty.
MIN_USEFUL_INTERSECTION_ANGLE_DEG = 20.0
MIN_ACCEPTABLE_INTERSECTION_ANGLE_DEG = 10.0
# Multi-ray outlier: pair locus farther than this from the median of other loci.
OUTLIER_PAIR_DISTANCE_M = 150.0
# Extreme multi-ray disagreement → withhold position (unavailable), not "useful".
UNAVAILABLE_SPREAD_FACTOR = 3.0
# Intra-Ear conflicting samples: large disagreement among near-simultaneous
# reports (not the full episode span — that includes legitimate source motion).
INTRA_EAR_BEARING_CONFLICT_DEG = 15.0
INTRA_EAR_CONFLICT_NEAR_DT_S = 5.0
# Geometric dilution indicator: baseline_m / sin(intersection_angle).
# Not a calibrated uncertainty; sin floor avoids blow-ups at degenerate angles.
GEOMETRIC_DILUTION_MIN_SIN = 1e-6
DEFAULT_BEARING_PERTURBATIONS_DEG = (1.0, 3.0, 5.0)
# Layout leave-one-out: displacement above this ⇒ removed Ear contributed material geometry.
LAYOUT_MATERIAL_DISPLACEMENT_M = 25.0

# Intended normal deployment: neighbouring EARs ≈500 m or less.
DENSE_NETWORK_MAX_BASELINE_M = 500.0
# Evaluation targets (aspirational; not guarantees).
EVALUATION_TARGET_DENSE_M = 100.0
EVALUATION_TARGET_SPARSE_M = 250.0
EVALUATION_TARGET_ERROR_M = EVALUATION_TARGET_SPARSE_M  # backward compatible default
EVALUATION_BANDS_M = (50.0, 100.0, 250.0)

NetworkGeometryClass = Literal["dense", "sparse", "unknown"]
LocalizationStatus = Literal["unavailable", "weak", "useful"]


@dataclass(frozen=True)
class BearingSample:
    bearing_deg: float
    confidence: float | None
    event_time_utc: datetime | None
    sequence_number: int | None
    emission_kind: str | None
    report_index: int


@dataclass
class LocalizationPairResult:
    ear_a_id: str
    ear_b_id: str
    observation_a_id: str
    observation_b_id: str
    bearing_a_deg: float
    bearing_b_deg: float
    bearing_a_time: datetime | None
    bearing_b_time: datetime | None
    bearing_dt_s: float | None
    selection_method: str
    time_aligned: bool
    intersection_status: str
    latitude: float | None
    longitude: float | None
    residual_a_deg: float | None
    residual_b_deg: float | None
    geometric_compatible: bool
    notes: str
    geographic_bearing_a_deg: float | None = None
    geographic_bearing_b_deg: float | None = None
    intersection_angle_deg: float | None = None
    outlier: bool = False
    baseline_m: float | None = None
    # baseline/sin(θ) in metres — geometric sensitivity indicator, not σ.
    geometric_dilution_m: float | None = None


@dataclass
class LocalizationResult:
    status: LocalizationStatus
    quality_notes: list[str]
    latitude: float | None
    longitude: float | None
    evaluation_time_utc: datetime | None
    pairs: list[LocalizationPairResult]
    max_bearing_dt_s: float | None
    pairwise_spread_m: float | None
    calibrated_uncertainty_m: float | None  # always None until a real model exists
    contributing_ears: list[str]
    selection_method: str
    details: dict[str, Any] = field(default_factory=dict)


def intersection_angle_deg(bearing_a_geo: float, bearing_b_geo: float) -> float:
    """Acute angle between two geographic LOBs in degrees (0–90)."""
    diff = smallest_angle_diff_deg(bearing_a_geo, bearing_b_geo)
    return min(diff, 180.0 - diff)


def geometric_dilution_indicator_m(
    baseline_m: float,
    intersection_angle_deg_value: float,
    *,
    min_sin: float = GEOMETRIC_DILUTION_MIN_SIN,
) -> float | None:
    """Geometric sensitivity indicator: baseline / sin(intersection angle).

    Units: metres (same as baseline). Larger ⇒ more sensitive to bearing error.
    This is NOT a calibrated position uncertainty, confidence radius, or σ.
    Limitations: ignores source range along the LOB, multi-ray fusion, timing,
    and measurement-error models; near-zero residuals do not validate it.
    Returns math.inf when the angle is degenerate (sin below min_sin).
    """
    if not math.isfinite(baseline_m) or baseline_m < 0:
        return None
    if not math.isfinite(intersection_angle_deg_value):
        return None
    angle = abs(float(intersection_angle_deg_value))
    if angle <= 0:
        return math.inf
    # Acute form already expected; clamp for safety.
    angle = min(angle, 180.0 - angle) if angle > 90.0 else angle
    sin_a = math.sin(math.radians(angle))
    if sin_a < min_sin:
        return math.inf
    return baseline_m / sin_a


def format_dilution_m(value: float | None) -> str:
    if value is None:
        return "n/a"
    if not math.isfinite(value):
        return "degenerate"
    return f"{value:.0f}m"


@dataclass(frozen=True)
class PairGeometrySensitivity:
    ear_a_id: str
    ear_b_id: str
    baseline_m: float | None
    intersection_angle_deg: float | None
    geometric_dilution_m: float | None
    intersection_status: str
    outlier: bool
    notes: str


@dataclass(frozen=True)
class BearingPerturbationTrial:
    ear_id: str
    perturbation_deg: float
    status: LocalizationStatus
    latitude: float | None
    longitude: float | None
    displacement_from_baseline_m: float | None
    position_withheld: bool
    best_intersection_angle_deg: float | None
    pairwise_spread_m: float | None
    network_geometric_sensitivity_m: float | None


@dataclass
class BearingSensitivityReport:
    baseline_status: LocalizationStatus
    baseline_latitude: float | None
    baseline_longitude: float | None
    baseline_position_withheld: bool
    perturbations_deg: tuple[float, ...]
    trials: list[BearingPerturbationTrial]
    max_displacement_m: float | None
    notes: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class LeaveOneEarOutTrial:
    removed_ear_id: str
    remaining_ears: list[str]
    status: LocalizationStatus
    latitude: float | None
    longitude: float | None
    displacement_from_full_m: float | None
    position_withheld: bool
    best_intersection_angle_deg: float | None
    min_intersection_angle_deg: float | None
    network_geometric_sensitivity_m: float | None
    material_geometry_loss: bool
    notes: str


@dataclass
class LayoutSensitivityReport:
    full_ear_ids: list[str]
    full_status: LocalizationStatus
    full_latitude: float | None
    full_longitude: float | None
    full_position_withheld: bool
    full_best_intersection_angle_deg: float | None
    full_network_geometric_sensitivity_m: float | None
    leave_one_out: list[LeaveOneEarOutTrial]
    redundant_ear_ids: list[str]
    critical_ear_ids: list[str]
    notes: list[str] = field(default_factory=list)


def intra_ear_bearing_span_deg(samples: list[BearingSample]) -> float | None:
    """Max pairwise node-relative bearing disagreement within one Observation."""
    degrees = [sample.bearing_deg for sample in samples]
    if len(degrees) < 2:
        return None
    span = 0.0
    for first, second in combinations(degrees, 2):
        span = max(span, smallest_angle_diff_deg(first, second))
    return span


def intra_ear_near_conflict_deg(
    samples: list[BearingSample],
    *,
    near_dt_s: float = INTRA_EAR_CONFLICT_NEAR_DT_S,
) -> float | None:
    """Max bearing disagreement among samples within near_dt_s of each other.

    Full-episode span mixes in source motion; near-window disagreement is the
    conflict signal for contradictory reports.
    """
    if len(samples) < 2:
        return None
    conflict = 0.0
    found = False
    for first, second in combinations(samples, 2):
        if first.event_time_utc is None or second.event_time_utc is None:
            continue
        dt = abs((second.event_time_utc - first.event_time_utc).total_seconds())
        if dt > near_dt_s:
            continue
        found = True
        conflict = max(
            conflict,
            smallest_angle_diff_deg(first.bearing_deg, second.bearing_deg),
        )
    return conflict if found else None


@dataclass
class GroundTruthEvaluation:
    ground_truth_latitude: float
    ground_truth_longitude: float
    ground_truth_time_utc: datetime | None
    estimated_latitude: float | None
    estimated_longitude: float | None
    error_m: float | None
    within_evaluation_target_250m: bool | None
    within_target_m: bool | None
    target_m: float
    network_class: NetworkGeometryClass
    within_50m: bool | None = None
    within_100m: bool | None = None
    within_250m: bool | None = None
    notes: str = ""


def classify_network_geometry(
    nodes: dict[str, dict[str, Any]],
    ear_ids: list[str] | None = None,
    *,
    dense_max_baseline_m: float = DENSE_NETWORK_MAX_BASELINE_M,
) -> tuple[NetworkGeometryClass, float | None, list[tuple[str, str, float]]]:
    """Classify installation geometry from max pairwise baseline.

    Dense = all considered Ear pairs ≤ dense_max_baseline_m (normal deployment).
    Sparse = any pair exceeds that (stress-test / existing B1-scale sites).
    """
    ids = sorted(ear_ids or list(nodes))
    baselines: list[tuple[str, str, float]] = []
    for first_id, second_id in combinations(ids, 2):
        first = nodes.get(first_id) or {}
        second = nodes.get(second_id) or {}
        inst_a = first.get("installation") if isinstance(first, dict) else None
        inst_b = second.get("installation") if isinstance(second, dict) else None
        if not isinstance(inst_a, dict) or not isinstance(inst_b, dict):
            continue
        lat1 = _finite_number(inst_a.get("latitude"))
        lon1 = _finite_number(inst_a.get("longitude"))
        lat2 = _finite_number(inst_b.get("latitude"))
        lon2 = _finite_number(inst_b.get("longitude"))
        if None in (lat1, lon1, lat2, lon2):
            continue
        distance = haversine_m(lat1, lon1, lat2, lon2)
        baselines.append((first_id, second_id, distance))
    if not baselines:
        return "unknown", None, []
    max_baseline = max(item[2] for item in baselines)
    network_class: NetworkGeometryClass = (
        "dense" if max_baseline <= dense_max_baseline_m else "sparse"
    )
    return network_class, max_baseline, baselines


def evaluation_target_for_network(network_class: NetworkGeometryClass) -> float:
    if network_class == "dense":
        return EVALUATION_TARGET_DENSE_M
    if network_class == "sparse":
        return EVALUATION_TARGET_SPARSE_M
    return EVALUATION_TARGET_SPARSE_M


def useful_spread_for_network(network_class: NetworkGeometryClass) -> float:
    if network_class == "dense":
        return LOCALIZATION_USEFUL_SPREAD_DENSE_M
    return LOCALIZATION_USEFUL_SPREAD_SPARSE_M


FlattenFields = Callable[[dict[str, Any]], dict[str, Any]]
ObservationTime = Callable[[dict[str, Any]], datetime | None]


def _finite_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value):
        return None
    return float(value)


def haversine_m(
    lat1: float,
    lon1: float,
    lat2: float,
    lon2: float,
) -> float:
    rlat1, rlon1, rlat2, rlon2 = map(math.radians, (lat1, lon1, lat2, lon2))
    dlat = rlat2 - rlat1
    dlon = rlon2 - rlon1
    chord = (
        math.sin(dlat / 2) ** 2
        + math.cos(rlat1) * math.cos(rlat2) * math.sin(dlon / 2) ** 2
    )
    return 2 * EARTH_RADIUS_M * math.asin(math.sqrt(min(1.0, chord)))


def extract_bearing_samples(
    reports: list[dict[str, Any]],
    *,
    observation_fields: FlattenFields,
    observation_time: ObservationTime,
) -> list[BearingSample]:
    samples: list[BearingSample] = []
    for index, report in enumerate(reports):
        fields = observation_fields(report)
        bearing = fields.get("bearing")
        if not isinstance(bearing, dict):
            continue
        deg = _finite_number(bearing.get("deg"))
        if deg is None or not 0 <= deg < 360:
            continue
        sequence = fields.get("sequence_number")
        seq = sequence if isinstance(sequence, int) and not isinstance(sequence, bool) else None
        kind = fields.get("emission_kind")
        kind_s = kind if isinstance(kind, str) else None
        samples.append(
            BearingSample(
                bearing_deg=deg,
                confidence=_finite_number(bearing.get("confidence")),
                event_time_utc=observation_time(report),
                sequence_number=seq,
                emission_kind=kind_s,
                report_index=index,
            )
        )
    return samples


def select_time_aware_bearing_pair(
    samples_a: list[BearingSample],
    samples_b: list[BearingSample],
    *,
    max_dt_s: float = LOCALIZATION_MAX_BEARING_DT_S,
) -> tuple[BearingSample, BearingSample, float | None, str, bool] | None:
    """Pick one bearing from each Observation for a pairwise localization attempt.

    Prefer the pair with smallest |Δt| among samples with known timestamps.
    Does not mix monotonic clocks across Ears.
    """
    if not samples_a or not samples_b:
        return None

    timed_pairs: list[tuple[float, BearingSample, BearingSample]] = []
    for first in samples_a:
        if first.event_time_utc is None:
            continue
        for second in samples_b:
            if second.event_time_utc is None:
                continue
            dt = abs((second.event_time_utc - first.event_time_utc).total_seconds())
            timed_pairs.append((dt, first, second))

    if timed_pairs:
        timed_pairs.sort(key=lambda item: (item[0], -(item[1].confidence or -1.0), -(item[2].confidence or -1.0)))
        dt, first, second = timed_pairs[0]
        aligned = dt <= max_dt_s
        method = (
            f"min_|Δt| among timestamped bearings (Δt={dt:.3f}s); "
            + ("within localization timing gate" if aligned else "outside localization timing gate")
        )
        return first, second, dt, method, aligned

    # Timestamps missing: fall back to highest confidence (localization weak).
    def rank(sample: BearingSample) -> float:
        return sample.confidence if sample.confidence is not None else -1.0

    first = max(samples_a, key=rank)
    second = max(samples_b, key=rank)
    return (
        first,
        second,
        None,
        "fallback max bearing.confidence; timestamps unavailable (not cross-Ear sync)",
        False,
    )


def estimate_localization(
    members: list[Any],
    nodes: dict[str, dict[str, Any]],
    *,
    good_dt_s: float = LOCALIZATION_GOOD_BEARING_DT_S,
    max_dt_s: float = LOCALIZATION_MAX_BEARING_DT_S,
    useful_spread_m: float | None = None,
    network_class: NetworkGeometryClass | None = None,
) -> LocalizationResult:
    """Build a coarse localization diagnostic from assembled Observation members."""
    ear_ids = sorted({member.node_id for member in members})
    detected_class, max_baseline, baselines = classify_network_geometry(nodes, ear_ids)
    resolved_class = network_class or detected_class
    spread_limit = (
        useful_spread_m
        if useful_spread_m is not None
        else useful_spread_for_network(resolved_class)
    )
    target_m = evaluation_target_for_network(resolved_class)
    notes: list[str] = [
        "Localization is separate from Event association.",
        "Two non-parallel bearings usually intersect; residual≈0 is not calibrated accuracy.",
        "calibrated_uncertainty_m unavailable (no angular σ model).",
        (
            f"network geometry class={resolved_class}"
            + (
                f"; max baseline={max_baseline:.0f}m"
                if max_baseline is not None
                else ""
            )
            + f"; consistency heuristic ≤{spread_limit:g}m; "
            f"aspirational evaluation target ±{target_m:g}m"
        ),
    ]
    baseline_lookup: dict[tuple[str, str], float] = {}
    if baselines:
        for first_id, second_id, distance in baselines:
            notes.append(f"baseline {first_id}↔{second_id}: {distance:.0f}m")
            baseline_lookup[(first_id, second_id)] = distance
            baseline_lookup[(second_id, first_id)] = distance
    pair_results: list[LocalizationPairResult] = []
    dts: list[float] = []
    intra_ear_conflicts: list[str] = []
    for member in members:
        near = intra_ear_near_conflict_deg(
            getattr(member, "bearing_samples", None) or []
        )
        if near is not None and near >= INTRA_EAR_BEARING_CONFLICT_DEG:
            intra_ear_conflicts.append(
                f"{member.node_id} near-window Δ={near:.1f}° "
                f"within {INTRA_EAR_CONFLICT_NEAR_DT_S:g}s "
                f"(≥{INTRA_EAR_BEARING_CONFLICT_DEG:g}°)"
            )
    if intra_ear_conflicts:
        notes.append(
            "Conflicting bearing samples over time on: "
            + "; ".join(intra_ear_conflicts)
            + " — localization cannot be useful while unresolved."
        )

    for first, second in combinations(members, 2):
        if first.node_id == second.node_id:
            continue
        samples_a = getattr(first, "bearing_samples", None) or []
        samples_b = getattr(second, "bearing_samples", None) or []
        selected = select_time_aware_bearing_pair(
            samples_a, samples_b, max_dt_s=max_dt_s
        )
        if selected is None:
            pair_results.append(
                LocalizationPairResult(
                    ear_a_id=first.node_id,
                    ear_b_id=second.node_id,
                    observation_a_id=first.observation_id,
                    observation_b_id=second.observation_id,
                    bearing_a_deg=float("nan"),
                    bearing_b_deg=float("nan"),
                    bearing_a_time=None,
                    bearing_b_time=None,
                    bearing_dt_s=None,
                    selection_method="no bearings",
                    time_aligned=False,
                    intersection_status="unavailable",
                    latitude=None,
                    longitude=None,
                    residual_a_deg=None,
                    residual_b_deg=None,
                    geometric_compatible=False,
                    notes="bearing missing on one or both Observations",
                )
            )
            continue

        sample_a, sample_b, dt, method, aligned = selected
        if dt is not None:
            dts.append(dt)

        def geometry_input(obs: Any, sample: BearingSample) -> dict[str, Any]:
            payload: dict[str, Any] = {
                "observation_id": obs.observation_id,
                "node_id": obs.node_id,
                "event_time_utc": (
                    sample.event_time_utc.astimezone(UTC).isoformat()
                    if sample.event_time_utc is not None
                    else None
                ),
                "bearing": {
                    "deg": sample.bearing_deg,
                    "reference": getattr(obs, "bearing_reference", None) or "node",
                },
            }
            if sample.confidence is not None:
                payload["bearing"]["confidence"] = sample.confidence
            return payload

        geo_a = prepare_observation_geometry(geometry_input(first, sample_a), nodes)
        geo_b = prepare_observation_geometry(geometry_input(second, sample_b), nodes)
        if not geo_a.eligible or not geo_b.eligible:
            reasons = [
                item.exclusion_reason
                for item in (geo_a, geo_b)
                if not item.eligible and item.exclusion_reason
            ]
            pair_results.append(
                LocalizationPairResult(
                    ear_a_id=first.node_id,
                    ear_b_id=second.node_id,
                    observation_a_id=first.observation_id,
                    observation_b_id=second.observation_id,
                    bearing_a_deg=sample_a.bearing_deg,
                    bearing_b_deg=sample_b.bearing_deg,
                    bearing_a_time=sample_a.event_time_utc,
                    bearing_b_time=sample_b.event_time_utc,
                    bearing_dt_s=dt,
                    selection_method=method,
                    time_aligned=aligned,
                    intersection_status="ineligible",
                    latitude=None,
                    longitude=None,
                    residual_a_deg=None,
                    residual_b_deg=None,
                    geometric_compatible=False,
                    notes="; ".join(reasons) or "geometry ineligible",
                )
            )
            continue

        angle = None
        if (
            geo_a.geographic_bearing_deg is not None
            and geo_b.geographic_bearing_deg is not None
        ):
            angle = intersection_angle_deg(
                geo_a.geographic_bearing_deg,
                geo_b.geographic_bearing_deg,
            )
        pair = analyze_pair(geo_a, geo_b)
        baseline_m = baseline_lookup.get((first.node_id, second.node_id))
        dilution = None
        if baseline_m is not None and angle is not None:
            dilution = geometric_dilution_indicator_m(baseline_m, angle)
        pair_note = pair.geometric_notes
        if angle is not None:
            pair_note = f"{pair_note}; intersection_angle={angle:.1f}°"
        if dilution is not None:
            pair_note = (
                f"{pair_note}; geometric_dilution="
                f"{format_dilution_m(dilution)} (baseline/sinθ; not σ)"
            )
        pair_results.append(
            LocalizationPairResult(
                ear_a_id=first.node_id,
                ear_b_id=second.node_id,
                observation_a_id=first.observation_id,
                observation_b_id=second.observation_id,
                bearing_a_deg=sample_a.bearing_deg,
                bearing_b_deg=sample_b.bearing_deg,
                bearing_a_time=sample_a.event_time_utc,
                bearing_b_time=sample_b.event_time_utc,
                bearing_dt_s=dt,
                selection_method=method,
                time_aligned=aligned,
                intersection_status=pair.intersection_status,
                latitude=pair.intersection_latitude,
                longitude=pair.intersection_longitude,
                residual_a_deg=pair.residual_a_deg,
                residual_b_deg=pair.residual_b_deg,
                geometric_compatible=pair.geometric_compatible,
                notes=pair_note,
                geographic_bearing_a_deg=geo_a.geographic_bearing_deg,
                geographic_bearing_b_deg=geo_b.geographic_bearing_deg,
                intersection_angle_deg=angle,
                baseline_m=baseline_m,
                geometric_dilution_m=dilution,
            )
        )

    max_dt = max(dts) if dts else None
    forward = [
        item
        for item in pair_results
        if item.intersection_status == "forward_intersection"
        and item.latitude is not None
        and item.longitude is not None
    ]
    aligned_forward = [item for item in forward if item.time_aligned]

    # Multi-ray outlier marking (diagnostic; does not invent calibrated σ).
    outlier_ids: list[str] = []
    if len(forward) >= 3:
        for index, item in enumerate(forward):
            others = [
                other
                for j, other in enumerate(forward)
                if j != index and other.latitude is not None and other.longitude is not None
            ]
            if len(others) < 2:
                continue
            other_lats = sorted(other.latitude for other in others)
            other_lons = sorted(other.longitude for other in others)
            mid = len(others) // 2
            if len(others) % 2:
                med_lat = other_lats[mid]
                med_lon = other_lons[mid]
            else:
                med_lat = 0.5 * (other_lats[mid - 1] + other_lats[mid])
                med_lon = 0.5 * (other_lons[mid - 1] + other_lons[mid])
            distance = haversine_m(item.latitude, item.longitude, med_lat, med_lon)
            if distance > OUTLIER_PAIR_DISTANCE_M:
                item.outlier = True
                pair_key = f"{item.ear_a_id}↔{item.ear_b_id}"
                outlier_ids.append(pair_key)
                item.notes = (
                    f"{item.notes}; outlier locus {distance:.0f}m from "
                    f"median of other pairs (>{OUTLIER_PAIR_DISTANCE_M:g}m)"
                )
                notes.append(
                    f"Pair {pair_key} flagged outlier "
                    f"(locus {distance:.0f}m from multi-ray median)."
                )

    inlier_forward = [item for item in forward if not item.outlier]
    consensus_pairs = inlier_forward if inlier_forward else forward
    forward_points = [
        (item.latitude, item.longitude)
        for item in consensus_pairs
        if item.latitude is not None and item.longitude is not None
    ]

    spread = None
    if len(forward_points) >= 2:
        spreads = [
            haversine_m(a[0], a[1], b[0], b[1])
            for a, b in combinations(forward_points, 2)
        ]
        spread = max(spreads) if spreads else None
    all_forward_spread = None
    if len(forward) >= 2:
        all_points = [
            (item.latitude, item.longitude)
            for item in forward
            if item.latitude is not None and item.longitude is not None
        ]
        all_spreads = [
            haversine_m(a[0], a[1], b[0], b[1])
            for a, b in combinations(all_points, 2)
        ]
        all_forward_spread = max(all_spreads) if all_spreads else None

    angles = [
        item.intersection_angle_deg
        for item in consensus_pairs
        if item.intersection_angle_deg is not None
    ]
    min_angle = min(angles) if angles else None
    best_angle = max(angles) if angles else None

    latitude = longitude = None
    evaluation_time = None
    withhold_position = False
    status: LocalizationStatus = "unavailable"

    if not forward:
        notes.append("No forward intersections available.")
        status = "unavailable"
        withhold_position = True
    else:
        # Angle / sensitivity diagnostics (residuals≈0 are not accuracy proof).
        if best_angle is not None:
            notes.append(
                f"LOB intersection angles among consensus pairs: "
                f"min={min_angle:.1f}° best={best_angle:.1f}° "
                f"(acute crossings amplify bearing error; not calibrated σ)."
            )
        if best_angle is not None and best_angle < MIN_ACCEPTABLE_INTERSECTION_ANGLE_DEG:
            notes.append(
                f"Best LOB intersection angle {best_angle:.1f}° < "
                f"{MIN_ACCEPTABLE_INTERSECTION_ANGLE_DEG:g}°; "
                "geometry too ill-conditioned — withhold position."
            )
            status = "unavailable"
            withhold_position = True
        elif (
            all_forward_spread is not None
            and all_forward_spread > UNAVAILABLE_SPREAD_FACTOR * spread_limit
            and not inlier_forward
        ):
            notes.append(
                f"Multi-ray loci disagree by {all_forward_spread:.0f}m "
                f"(>{UNAVAILABLE_SPREAD_FACTOR:g}×{spread_limit:g}m) with no "
                "consistent inlier set — withhold position."
            )
            status = "unavailable"
            withhold_position = True
        elif (
            all_forward_spread is not None
            and outlier_ids
            and not inlier_forward
        ):
            notes.append(
                "All forward pairs marked mutually inconsistent outliers; "
                "withhold position."
            )
            status = "unavailable"
            withhold_position = True
        else:
            # Publish consensus locus unless withheld above.
            if forward_points:
                latitude = sum(point[0] for point in forward_points) / len(
                    forward_points
                )
                longitude = sum(point[1] for point in forward_points) / len(
                    forward_points
                )
            times: list[datetime] = []
            for item in consensus_pairs:
                for stamp in (item.bearing_a_time, item.bearing_b_time):
                    if stamp is not None:
                        times.append(stamp)
            if times:
                epoch = datetime(1970, 1, 1, tzinfo=UTC)
                mean_s = sum(
                    (stamp - epoch).total_seconds() for stamp in times
                ) / len(times)
                evaluation_time = epoch + timedelta(seconds=mean_s)

            if not aligned_forward and max_dt is not None and max_dt > max_dt_s:
                notes.append(
                    f"Forward intersections exist but bearing Δt exceeds "
                    f"{max_dt_s:g}s gate; localization weak for common-position "
                    "assumption."
                )
                status = "weak"
            elif (
                len(consensus_pairs) == 1
                and best_angle is not None
                and best_angle < MIN_USEFUL_INTERSECTION_ANGLE_DEG
            ):
                notes.append(
                    f"Only one forward pair with intersection angle "
                    f"{best_angle:.1f}° < {MIN_USEFUL_INTERSECTION_ANGLE_DEG:g}°; "
                    "single acute crossing is not a defensible estimate — "
                    "withhold position (two-ray residual≈0 is not accuracy)."
                )
                status = "unavailable"
                withhold_position = True
            elif best_angle is not None and best_angle < MIN_USEFUL_INTERSECTION_ANGLE_DEG:
                notes.append(
                    f"Best LOB intersection angle {best_angle:.1f}° < "
                    f"{MIN_USEFUL_INTERSECTION_ANGLE_DEG:g}°; "
                    "geometry sensitive — localization weak."
                )
                status = "weak"
            elif outlier_ids:
                notes.append(
                    "Outlier pair(s) excluded from consensus; contradictory "
                    "measurements retained in diagnostics — localization weak "
                    "(not useful)."
                )
                status = "weak"
            elif intra_ear_conflicts:
                notes.append(
                    "Intra-Ear bearing conflict present; estimate retained as "
                    "weak only (time-aware selection cannot resolve contradiction)."
                )
                status = "weak"
            elif len(consensus_pairs) == 1:
                notes.append(
                    "Only one forward pair; multi-lateration consistency "
                    "unknown (two-ray residual≈0 is not accuracy proof)."
                )
                status = "weak"
            elif spread is not None and spread > spread_limit:
                notes.append(
                    f"Pairwise intersection spread {spread:.0f}m exceeds "
                    f"{spread_limit:g}m consistency heuristic; localization weak."
                )
                status = "weak"
            elif (
                all_forward_spread is not None
                and all_forward_spread > UNAVAILABLE_SPREAD_FACTOR * spread_limit
            ):
                # Inliers exist but raw disagreement is extreme — still weak.
                notes.append(
                    f"Raw multi-ray spread {all_forward_spread:.0f}m is extreme; "
                    "consensus retained as weak only."
                )
                status = "weak"
            elif max_dt is not None and max_dt > good_dt_s:
                notes.append(
                    f"Bearing Δt up to {max_dt:.3f}s > good gate {good_dt_s:g}s; "
                    "localization weak."
                )
                status = "weak"
            elif spread is not None:
                notes.append(
                    f"Multiple forward pairs with pairwise spread "
                    f"{spread:.0f}m ≤ {spread_limit:g}m consistency heuristic, "
                    f"best intersection angle "
                    f"{best_angle:.1f}° ≥ {MIN_USEFUL_INTERSECTION_ANGLE_DEG:g}°, "
                    f"and bearing timing within {good_dt_s:g}s; "
                    "treated as useful coarse area (not calibrated uncertainty)."
                )
                status = "useful"
            else:
                notes.append(
                    "Forward pairs available; treated as weak without spread metric."
                )
                status = "weak"

    if withhold_position or latitude is None:
        latitude = None
        longitude = None
        evaluation_time = None
        status = "unavailable"

    pair_sensitivity = [
        PairGeometrySensitivity(
            ear_a_id=item.ear_a_id,
            ear_b_id=item.ear_b_id,
            baseline_m=item.baseline_m,
            intersection_angle_deg=item.intersection_angle_deg,
            geometric_dilution_m=item.geometric_dilution_m,
            intersection_status=item.intersection_status,
            outlier=item.outlier,
            notes=(
                "geometric dilution = baseline/sin(θ); diagnostic only, "
                "not calibrated uncertainty; residuals are separate"
            ),
        )
        for item in pair_results
    ]
    consensus_dilutions = [
        item.geometric_dilution_m
        for item in consensus_pairs
        if item.geometric_dilution_m is not None
    ]
    finite_dilutions = [value for value in consensus_dilutions if math.isfinite(value)]
    network_sensitivity = None
    if any(not math.isfinite(value) for value in consensus_dilutions):
        network_sensitivity = math.inf
    elif finite_dilutions:
        network_sensitivity = max(finite_dilutions)
    if network_sensitivity is not None:
        notes.append(
            "Network geometric sensitivity indicator (max consensus "
            f"baseline/sinθ)={format_dilution_m(network_sensitivity)}; "
            "not a confidence radius or calibrated σ."
        )

    # contributing_ears = unique EARs in non-outlier consensus pairs that feed the
    # selected position estimate (when a position is published). This is intentionally
    # a subset of association membership. Missing bearings / outlier pairs do not
    # remove Event members.
    ears_with_usable_bearing = sorted(
        {
            member.node_id
            for member in members
            if (
                getattr(member, "bearing_deg", None) is not None
                or (getattr(member, "bearing_samples", None) or [])
            )
        }
    )
    forward_ears = sorted(
        {
            ear_id
            for item in forward
            for ear_id in (item.ear_a_id, item.ear_b_id)
        }
    )
    outlier_ears = sorted(
        {
            ear_id
            for item in forward
            if item.outlier
            for ear_id in (item.ear_a_id, item.ear_b_id)
        }
    )
    consensus_ear_ids = sorted(
        {
            ear_id
            for item in consensus_pairs
            if not item.outlier
            for ear_id in (item.ear_a_id, item.ear_b_id)
        }
    )
    # If no position is published, no EAR contributes to a final estimate.
    contributor_ids = [] if withhold_position or latitude is None else consensus_ear_ids

    return LocalizationResult(
        status=status,
        quality_notes=notes,
        latitude=latitude,
        longitude=longitude,
        evaluation_time_utc=evaluation_time,
        pairs=pair_results,
        max_bearing_dt_s=max_dt,
        pairwise_spread_m=spread if not withhold_position else all_forward_spread,
        calibrated_uncertainty_m=None,
        contributing_ears=contributor_ids,
        selection_method="time-aware min_|Δt| per pair (fallback max confidence)",
        details={
            "forward_pair_count": len(forward),
            "aligned_forward_pair_count": len(aligned_forward),
            "consensus_pair_count": len(consensus_pairs),
            "association_member_ears": ear_ids,
            "ears_with_usable_bearing": ears_with_usable_bearing,
            "forward_intersection_ears": forward_ears,
            "consensus_selected_ears": consensus_ear_ids,
            "outlier_pair_ears": outlier_ears,
            "contributing_ears_definition": (
                "Unique EARs in non-outlier consensus pairs that feed the "
                "published position; empty when position is withheld. "
                "Independent of Event association membership."
            ),
            "outlier_pairs": outlier_ids,
            "intra_ear_bearing_conflicts": intra_ear_conflicts,
            "min_intersection_angle_deg": min_angle,
            "best_intersection_angle_deg": best_angle,
            "all_forward_spread_m": all_forward_spread,
            "position_withheld": withhold_position,
            "network_class": resolved_class,
            "max_baseline_m": max_baseline,
            "evaluation_target_m": target_m,
            "useful_spread_m": spread_limit,
            "pair_geometry_sensitivity": pair_sensitivity,
            "network_geometric_sensitivity_m": network_sensitivity,
            "geometric_sensitivity_definition": (
                "baseline_m / sin(intersection_angle_deg); "
                "geometric sensitivity indicator only — not position accuracy"
            ),
        },
    )


def evaluate_against_ground_truth(
    localization: LocalizationResult,
    *,
    ground_truth_latitude: float,
    ground_truth_longitude: float,
    ground_truth_time_utc: datetime | None = None,
    target_m: float | None = None,
    network_class: NetworkGeometryClass | None = None,
) -> GroundTruthEvaluation:
    """Post-hoc evaluation only — never feeds association or localization math."""
    resolved_class = network_class or localization.details.get("network_class") or "unknown"
    if resolved_class not in ("dense", "sparse", "unknown"):
        resolved_class = "unknown"
    resolved_target = (
        target_m
        if target_m is not None
        else evaluation_target_for_network(resolved_class)  # type: ignore[arg-type]
    )
    if localization.latitude is None or localization.longitude is None:
        return GroundTruthEvaluation(
            ground_truth_latitude=ground_truth_latitude,
            ground_truth_longitude=ground_truth_longitude,
            ground_truth_time_utc=ground_truth_time_utc,
            estimated_latitude=None,
            estimated_longitude=None,
            error_m=None,
            within_evaluation_target_250m=None,
            within_target_m=None,
            target_m=resolved_target,
            network_class=resolved_class,  # type: ignore[arg-type]
            notes="No estimated position to evaluate.",
        )
    error = haversine_m(
        localization.latitude,
        localization.longitude,
        ground_truth_latitude,
        ground_truth_longitude,
    )
    return GroundTruthEvaluation(
        ground_truth_latitude=ground_truth_latitude,
        ground_truth_longitude=ground_truth_longitude,
        ground_truth_time_utc=ground_truth_time_utc,
        estimated_latitude=localization.latitude,
        estimated_longitude=localization.longitude,
        error_m=error,
        within_evaluation_target_250m=error <= EVALUATION_TARGET_SPARSE_M,
        within_target_m=error <= resolved_target,
        target_m=resolved_target,
        network_class=resolved_class,  # type: ignore[arg-type]
        within_50m=error <= 50.0,
        within_100m=error <= 100.0,
        within_250m=error <= 250.0,
        notes=(
            f"Post-hoc GT comparison only; network={resolved_class}; "
            f"error={error:.1f}m; aspirational target ±{resolved_target:g}m "
            f"(within_target={error <= resolved_target}); "
            f"bands ≤50/100/250m = "
            f"{error <= 50.0}/{error <= 100.0}/{error <= 250.0}; "
            "GT was not used for association or position calculation."
        ),
    )


def apply_sensor_imperfections(
    payloads: list[dict[str, Any]],
    *,
    ear_time_offsets_s: dict[str, float] | None = None,
    bearing_noise_deg: float = 0.0,
    ear_bearing_bias_deg: dict[str, float] | None = None,
    drop_bearing_node_ids: set[str] | frozenset[str] | None = None,
    conflicting_ear_bias_deg: dict[str, float] | None = None,
    seed: int = 0,
) -> list[dict[str, Any]]:
    """Deterministic test helper for timing/bearing imperfections.

    - ear_time_offsets_s: per-Ear wall-clock bias on event_time_utc
    - bearing_noise_deg: uniform noise in [-noise, +noise] (seeded)
    - ear_bearing_bias_deg: constant per-Ear bearing bias (incorrect / large error)
    - drop_bearing_node_ids: remove bearing reports for listed Ears
    - conflicting_ear_bias_deg: bias every other bearing sample for an Ear
      (conflicting samples over time)

    Does not claim a relationship between bearing.confidence and angular σ.
    """
    rng = random.Random(seed)
    offsets = ear_time_offsets_s or {}
    biases = ear_bearing_bias_deg or {}
    conflicting = conflicting_ear_bias_deg or {}
    drop_ids = set(drop_bearing_node_ids or ())
    conflict_counts: dict[str, int] = {}
    out: list[dict[str, Any]] = []
    for payload in payloads:
        item = {
            key: (dict(value) if isinstance(value, dict) else value)
            for key, value in payload.items()
        }
        # Deep-copy nested dicts we mutate.
        identity = dict(payload.get("identity") or {})
        context = dict(payload.get("context") or {})
        spatial = dict(payload.get("spatial") or {})
        acoustic = dict(payload.get("acoustic") or {})
        item["identity"] = identity
        item["context"] = context
        item["spatial"] = spatial
        item["acoustic"] = acoustic

        node_id = identity.get("node_id")
        if isinstance(node_id, str) and node_id in offsets:
            raw = context.get("event_time_utc")
            if isinstance(raw, str):
                try:
                    parsed = datetime.fromisoformat(raw)
                except ValueError:
                    parsed = None
                if parsed is not None:
                    if parsed.tzinfo is None:
                        parsed = parsed.replace(tzinfo=UTC)
                    shifted = parsed + timedelta(seconds=float(offsets[node_id]))
                    context["event_time_utc"] = shifted.astimezone(UTC).isoformat()

        if isinstance(node_id, str) and node_id in drop_ids:
            spatial.pop("bearing", None)
            out.append(item)
            continue

        bearing = spatial.get("bearing")
        if isinstance(bearing, dict):
            bearing = dict(bearing)
            deg = _finite_number(bearing.get("deg"))
            if deg is not None:
                delta = 0.0
                if bearing_noise_deg > 0:
                    # Uniform noise in [-noise, +noise], deterministic via seed.
                    delta += (rng.random() * 2.0 - 1.0) * float(bearing_noise_deg)
                if isinstance(node_id, str) and node_id in biases:
                    delta += float(biases[node_id])
                if isinstance(node_id, str) and node_id in conflicting:
                    count = conflict_counts.get(node_id, 0)
                    conflict_counts[node_id] = count + 1
                    if count % 2 == 1:
                        delta += float(conflicting[node_id])
                bearing["deg"] = (deg + delta) % 360.0
                spatial["bearing"] = bearing
        out.append(item)
    return out


def source_position_on_route(
    start: dict[str, float],
    end: dict[str, float],
    *,
    speed_m_s: float,
    at_seconds: float,
) -> tuple[float, float]:
    """Ground-truth source lat/lon at scenario time (evaluation only)."""
    distance = haversine_m(
        start["latitude"], start["longitude"], end["latitude"], end["longitude"]
    )
    if distance <= 0 or speed_m_s <= 0:
        return start["latitude"], start["longitude"]
    duration = distance / speed_m_s
    frac = min(1.0, max(0.0, at_seconds / duration))
    return (
        start["latitude"] + (end["latitude"] - start["latitude"]) * frac,
        start["longitude"] + (end["longitude"] - start["longitude"]) * frac,
    )


def _shift_observation_bearings(member: Any, delta_deg: float) -> Any:
    """Return a shallow-replaced Observation with node-relative bearings shifted."""
    samples = getattr(member, "bearing_samples", None) or []
    shifted = [
        BearingSample(
            bearing_deg=(sample.bearing_deg + delta_deg) % 360.0,
            confidence=sample.confidence,
            event_time_utc=sample.event_time_utc,
            sequence_number=sample.sequence_number,
            emission_kind=sample.emission_kind,
            report_index=sample.report_index,
        )
        for sample in samples
    ]
    kwargs: dict[str, Any] = {"bearing_samples": shifted}
    primary = getattr(member, "bearing_deg", None)
    if isinstance(primary, (int, float)) and not isinstance(primary, bool):
        kwargs["bearing_deg"] = (float(primary) + delta_deg) % 360.0
    try:
        return replace(member, **kwargs)
    except TypeError:
        # Non-dataclass test doubles: mutate a deepcopy.
        clone = deepcopy(member)
        clone.bearing_samples = shifted
        if "bearing_deg" in kwargs:
            clone.bearing_deg = kwargs["bearing_deg"]
        return clone


def analyze_bearing_sensitivity(
    members: list[Any],
    nodes: dict[str, dict[str, Any]],
    *,
    perturbations_deg: Sequence[float] = DEFAULT_BEARING_PERTURBATIONS_DEG,
    baseline: LocalizationResult | None = None,
) -> BearingSensitivityReport:
    """Deterministic ±bearing perturbation diagnostics (evaluation / scripts only).

    Does not feed association or Event promotion. Symmetric ±δ are both measured
    because position displacement need not be symmetric. GT is not used.
    """
    base = baseline or estimate_localization(members, nodes)
    notes = [
        "Bearing sensitivity is a diagnostic displacement study, not calibrated σ.",
        "Baseline estimate is the unperturbed localization; trials are separate.",
        "Association / Event promotion are not re-run here.",
    ]
    ear_ids = sorted({member.node_id for member in members})
    signed = []
    for magnitude in perturbations_deg:
        mag = abs(float(magnitude))
        if mag == 0:
            continue
        signed.extend([-mag, mag])
    # Stable unique order: increasing |δ|, then − then +.
    ordered: list[float] = []
    for magnitude in sorted({abs(float(value)) for value in signed}):
        ordered.extend([-magnitude, magnitude])

    trials: list[BearingPerturbationTrial] = []
    displacements: list[float] = []
    for ear_id in ear_ids:
        for delta in ordered:
            perturbed_members = [
                _shift_observation_bearings(member, delta)
                if member.node_id == ear_id
                else member
                for member in members
            ]
            loc = estimate_localization(perturbed_members, nodes)
            withheld = bool(loc.details.get("position_withheld")) or loc.latitude is None
            displacement = None
            if (
                base.latitude is not None
                and base.longitude is not None
                and loc.latitude is not None
                and loc.longitude is not None
            ):
                displacement = haversine_m(
                    base.latitude, base.longitude, loc.latitude, loc.longitude
                )
                displacements.append(displacement)
            trials.append(
                BearingPerturbationTrial(
                    ear_id=ear_id,
                    perturbation_deg=delta,
                    status=loc.status,
                    latitude=loc.latitude,
                    longitude=loc.longitude,
                    displacement_from_baseline_m=displacement,
                    position_withheld=withheld,
                    best_intersection_angle_deg=loc.details.get(
                        "best_intersection_angle_deg"
                    ),
                    pairwise_spread_m=loc.pairwise_spread_m,
                    network_geometric_sensitivity_m=loc.details.get(
                        "network_geometric_sensitivity_m"
                    ),
                )
            )

    if base.latitude is None:
        notes.append(
            "Baseline position unavailable/withheld; displacement vs baseline "
            "is undefined for trials that also lack a position."
        )
    asymmetric = False
    by_key: dict[tuple[str, float], float] = {}
    for trial in trials:
        if trial.displacement_from_baseline_m is None:
            continue
        key = (trial.ear_id, abs(trial.perturbation_deg))
        other = by_key.get(key)
        if other is None:
            by_key[key] = trial.displacement_from_baseline_m
        elif abs(other - trial.displacement_from_baseline_m) > 1.0:
            asymmetric = True
    if asymmetric:
        notes.append(
            "Observed asymmetric +δ/−δ displacements; do not assume symmetry."
        )

    return BearingSensitivityReport(
        baseline_status=base.status,
        baseline_latitude=base.latitude,
        baseline_longitude=base.longitude,
        baseline_position_withheld=bool(base.details.get("position_withheld"))
        or base.latitude is None,
        perturbations_deg=tuple(sorted({abs(float(v)) for v in perturbations_deg})),
        trials=trials,
        max_displacement_m=max(displacements) if displacements else None,
        notes=notes,
    )


def analyze_leave_one_ear_out(
    members: list[Any],
    nodes: dict[str, dict[str, Any]],
    *,
    baseline: LocalizationResult | None = None,
    material_displacement_m: float = LAYOUT_MATERIAL_DISPLACEMENT_M,
) -> LayoutSensitivityReport:
    """Compare the full multi-EAR solution with leave-one-EAR-out subsets.

    Uses installation geometry and estimated positions only — never ground truth.
    A four-EAR solution is not assumed better than three-EAR.
    """
    base = baseline or estimate_localization(members, nodes)
    ear_ids = sorted({member.node_id for member in members})
    notes = [
        "Leave-one-EAR-out is a layout diagnostic; it does not change Events.",
        ("Material geometry loss: status demotion, withhold, or displacement "
        f"> {material_displacement_m:g}m vs full solution "
        "(or large dilution increase)."),
        "GT is not used.",
    ]
    trials: list[LeaveOneEarOutTrial] = []
    critical: list[str] = []
    redundant: list[str] = []
    full_sens = base.details.get("network_geometric_sensitivity_m")

    for removed in ear_ids:
        subset = [member for member in members if member.node_id != removed]
        remaining = sorted({member.node_id for member in subset})
        if len(remaining) < 2:
            trials.append(
                LeaveOneEarOutTrial(
                    removed_ear_id=removed,
                    remaining_ears=remaining,
                    status="unavailable",
                    latitude=None,
                    longitude=None,
                    displacement_from_full_m=None,
                    position_withheld=True,
                    best_intersection_angle_deg=None,
                    min_intersection_angle_deg=None,
                    network_geometric_sensitivity_m=None,
                    material_geometry_loss=True,
                    notes="fewer than two Ears remain; localization unavailable",
                )
            )
            critical.append(removed)
            continue

        loc = estimate_localization(subset, nodes)
        withheld = bool(loc.details.get("position_withheld")) or loc.latitude is None
        displacement = None
        if (
            base.latitude is not None
            and base.longitude is not None
            and loc.latitude is not None
            and loc.longitude is not None
        ):
            displacement = haversine_m(
                base.latitude, base.longitude, loc.latitude, loc.longitude
            )
        subset_sens = loc.details.get("network_geometric_sensitivity_m")
        status_rank = {"useful": 2, "weak": 1, "unavailable": 0}
        demoted = status_rank.get(loc.status, 0) < status_rank.get(base.status, 0)
        dilution_worse = False
        if (
            isinstance(full_sens, (int, float))
            and isinstance(subset_sens, (int, float))
            and math.isfinite(full_sens)
            and (
                not math.isfinite(subset_sens)
                or subset_sens > full_sens * 1.5 + 50.0
            )
        ):
            dilution_worse = True
        material = bool(
            demoted
            or (withheld and base.latitude is not None)
            or (displacement is not None and displacement > material_displacement_m)
            or dilution_worse
        )
        note_bits = []
        if demoted:
            note_bits.append(f"status {base.status}→{loc.status}")
        if withheld and base.latitude is not None:
            note_bits.append("position withheld after removal")
        if displacement is not None:
            note_bits.append(f"displacement={displacement:.1f}m")
        if dilution_worse:
            note_bits.append(
                f"sensitivity {format_dilution_m(full_sens)}→"
                f"{format_dilution_m(subset_sens)}"
            )
        if not note_bits:
            note_bits.append("similar geometry to full solution")

        trial = LeaveOneEarOutTrial(
            removed_ear_id=removed,
            remaining_ears=remaining,
            status=loc.status,
            latitude=loc.latitude,
            longitude=loc.longitude,
            displacement_from_full_m=displacement,
            position_withheld=withheld,
            best_intersection_angle_deg=loc.details.get("best_intersection_angle_deg"),
            min_intersection_angle_deg=loc.details.get("min_intersection_angle_deg"),
            network_geometric_sensitivity_m=subset_sens,
            material_geometry_loss=material,
            notes="; ".join(note_bits),
        )
        trials.append(trial)
        if material:
            critical.append(removed)
        elif (
            base.status == loc.status
            and not withheld
            and (displacement is None or displacement <= material_displacement_m)
        ):
            redundant.append(removed)

    if critical:
        notes.append(
            "Ears whose removal materially worsens geometry: "
            + ", ".join(critical)
        )
    if redundant:
        notes.append(
            "Ears whose removal left similar status/position (possible redundancy): "
            + ", ".join(redundant)
        )
    if len(ear_ids) >= 4 and redundant:
        notes.append(
            "Additional Ear(s) did not clearly improve the solution under "
            "leave-one-out (four-Ear ≠ always better)."
        )

    return LayoutSensitivityReport(
        full_ear_ids=ear_ids,
        full_status=base.status,
        full_latitude=base.latitude,
        full_longitude=base.longitude,
        full_position_withheld=bool(base.details.get("position_withheld"))
        or base.latitude is None,
        full_best_intersection_angle_deg=base.details.get("best_intersection_angle_deg"),
        full_network_geometric_sensitivity_m=full_sens,
        leave_one_out=trials,
        redundant_ear_ids=redundant,
        critical_ear_ids=critical,
        notes=notes,
    )


def bearing_sensitivity_as_dict(report: BearingSensitivityReport) -> dict[str, Any]:
    return {
        "baseline_status": report.baseline_status,
        "baseline_latitude": report.baseline_latitude,
        "baseline_longitude": report.baseline_longitude,
        "baseline_position_withheld": report.baseline_position_withheld,
        "perturbations_deg": list(report.perturbations_deg),
        "max_displacement_m": report.max_displacement_m,
        "notes": list(report.notes),
        "trials": [
            {
                "ear_id": trial.ear_id,
                "perturbation_deg": trial.perturbation_deg,
                "status": trial.status,
                "latitude": trial.latitude,
                "longitude": trial.longitude,
                "displacement_from_baseline_m": trial.displacement_from_baseline_m,
                "position_withheld": trial.position_withheld,
                "best_intersection_angle_deg": trial.best_intersection_angle_deg,
                "pairwise_spread_m": trial.pairwise_spread_m,
                "network_geometric_sensitivity_m": (
                    trial.network_geometric_sensitivity_m
                    if trial.network_geometric_sensitivity_m is None
                    or math.isfinite(trial.network_geometric_sensitivity_m)
                    else "degenerate"
                ),
            }
            for trial in report.trials
        ],
    }


def layout_sensitivity_as_dict(report: LayoutSensitivityReport) -> dict[str, Any]:
    return {
        "full_ear_ids": list(report.full_ear_ids),
        "full_status": report.full_status,
        "full_latitude": report.full_latitude,
        "full_longitude": report.full_longitude,
        "full_position_withheld": report.full_position_withheld,
        "full_best_intersection_angle_deg": report.full_best_intersection_angle_deg,
        "full_network_geometric_sensitivity_m": (
            report.full_network_geometric_sensitivity_m
            if report.full_network_geometric_sensitivity_m is None
            or math.isfinite(report.full_network_geometric_sensitivity_m)
            else "degenerate"
        ),
        "redundant_ear_ids": list(report.redundant_ear_ids),
        "critical_ear_ids": list(report.critical_ear_ids),
        "notes": list(report.notes),
        "leave_one_out": [
            {
                "removed_ear_id": trial.removed_ear_id,
                "remaining_ears": list(trial.remaining_ears),
                "status": trial.status,
                "latitude": trial.latitude,
                "longitude": trial.longitude,
                "displacement_from_full_m": trial.displacement_from_full_m,
                "position_withheld": trial.position_withheld,
                "best_intersection_angle_deg": trial.best_intersection_angle_deg,
                "min_intersection_angle_deg": trial.min_intersection_angle_deg,
                "network_geometric_sensitivity_m": (
                    trial.network_geometric_sensitivity_m
                    if trial.network_geometric_sensitivity_m is None
                    or math.isfinite(trial.network_geometric_sensitivity_m)
                    else "degenerate"
                ),
                "material_geometry_loss": trial.material_geometry_loss,
                "notes": trial.notes,
            }
            for trial in report.leave_one_out
        ],
    }
