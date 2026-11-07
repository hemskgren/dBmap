"""Diagnostic-only cross-Ear bearing geometry.

Conventions (must match docs/protocol.md and simulator synthesis):
- Node-relative bearing.deg is clockwise degrees in [0, 360) from the Ear
  forward/reference axis (not geographic north).
- Installation orientation_deg is clockwise degrees from true north to that
  same node reference axis.
- Geographic bearing = (orientation_deg + node_relative_deg) % 360, clockwise
  from true north (0=N, 90=E, 180=S, 270=W).

Geometry method: local east-north tangent plane at a reference latitude near the
Ears, with WGS84 spherical radius matching hub_event_track. Suitable for
regional (~tens of km) installations; not a global geodesic solver. Ray
intersection is a candidate locus only — not a confirmed source position.

Bearing uncertainty: only bearing.confidence in [0, 1] exists. There is no
calibrated angular σ; confidence is not mapped to degrees.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from itertools import combinations
from typing import Any

EARTH_RADIUS_M = 6_371_000.0

# Provisional diagnostic parameters only — not validated localization accuracy.
DIAGNOSTIC_PARALLEL_SIN_EPS = 1e-3  # |sin Δbearing| below this → nearly parallel
DIAGNOSTIC_RESIDUAL_PLAUSIBLE_DEG = 15.0
DIAGNOSTIC_LOW_BEARING_CONFIDENCE = 0.4
DIAGNOSTIC_TEMPORAL_CLOSE_S = 30.0


def normalize_deg(value: float) -> float:
    return value % 360.0


def smallest_angle_diff_deg(first: float, second: float) -> float:
    return abs((normalize_deg(first) - normalize_deg(second) + 180.0) % 360.0 - 180.0)


def geographic_bearing_deg(orientation_deg: float, node_relative_deg: float) -> float:
    return normalize_deg(orientation_deg + node_relative_deg)


def bearing_unit_en(geographic_bearing_deg_value: float) -> tuple[float, float]:
    """Unit vector (east, north) for a geographic bearing clockwise from north."""
    radians = math.radians(normalize_deg(geographic_bearing_deg_value))
    return math.sin(radians), math.cos(radians)


def latlon_to_local_en(
    latitude: float,
    longitude: float,
    *,
    origin_lat: float,
    origin_lon: float,
) -> tuple[float, float]:
    lat0 = math.radians(origin_lat)
    dlat = math.radians(latitude - origin_lat)
    dlon = math.radians(longitude - origin_lon)
    east = EARTH_RADIUS_M * math.cos(lat0) * dlon
    north = EARTH_RADIUS_M * dlat
    return east, north


def local_en_to_latlon(
    east_m: float,
    north_m: float,
    *,
    origin_lat: float,
    origin_lon: float,
) -> tuple[float, float]:
    lat0 = math.radians(origin_lat)
    latitude = origin_lat + math.degrees(north_m / EARTH_RADIUS_M)
    longitude = origin_lon + math.degrees(east_m / (EARTH_RADIUS_M * math.cos(lat0)))
    return latitude, longitude


def forward_azimuth_deg(
    from_lat: float,
    from_lon: float,
    to_lat: float,
    to_lon: float,
) -> float:
    """Initial great-circle bearing clockwise from true north."""
    lat1 = math.radians(from_lat)
    lat2 = math.radians(to_lat)
    lon_delta = math.radians(to_lon - from_lon)
    y = math.sin(lon_delta) * math.cos(lat2)
    x = (
        math.cos(lat1) * math.sin(lat2)
        - math.sin(lat1) * math.cos(lat2) * math.cos(lon_delta)
    )
    return normalize_deg(math.degrees(math.atan2(y, x)))


@dataclass(frozen=True)
class RayIntersection:
    status: str
    east_m: float | None = None
    north_m: float | None = None
    t1_m: float | None = None
    t2_m: float | None = None


def intersect_rays_en(
    origin1: tuple[float, float],
    bearing1_deg: float,
    origin2: tuple[float, float],
    bearing2_deg: float,
    *,
    parallel_sin_eps: float = DIAGNOSTIC_PARALLEL_SIN_EPS,
) -> RayIntersection:
    """Intersect two planar LOBs in local east-north meters.

    Parameters t1_m / t2_m are signed distances along each ray (forward > 0).
    """
    d1e, d1n = bearing_unit_en(bearing1_deg)
    d2e, d2n = bearing_unit_en(bearing2_deg)
    cross = d1e * d2n - d1n * d2e
    if abs(cross) < parallel_sin_eps:
        return RayIntersection(status="nearly_parallel")

    o1e, o1n = origin1
    o2e, o2n = origin2
    diff_e = o2e - o1e
    diff_n = o2n - o1n
    t1 = (diff_e * d2n - diff_n * d2e) / cross
    t2 = (diff_e * d1n - diff_n * d1e) / cross
    if not math.isfinite(t1) or not math.isfinite(t2):
        return RayIntersection(status="numerically_unstable")

    east = o1e + t1 * d1e
    north = o1n + t1 * d1n
    if not math.isfinite(east) or not math.isfinite(north):
        return RayIntersection(status="numerically_unstable")

    if t1 > 0 and t2 > 0:
        status = "forward_intersection"
    elif t1 <= 0 and t2 <= 0:
        status = "behind_both"
    else:
        status = "behind_one"
    return RayIntersection(
        status=status,
        east_m=east,
        north_m=north,
        t1_m=t1,
        t2_m=t2,
    )


def _finite_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value):
        return None
    return float(value)


def _observation_time(observation: dict[str, Any]) -> datetime | None:
    value = observation.get("event_time_utc")
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(UTC)


@dataclass(frozen=True)
class ObservationGeometry:
    observation_id: str
    node_id: str
    event_time_utc: datetime | None
    latitude: float | None
    longitude: float | None
    node_relative_bearing_deg: float | None
    orientation_deg: float | None
    geographic_bearing_deg: float | None
    bearing_confidence: float | None
    eligible: bool
    exclusion_reason: str | None
    low_bearing_confidence: bool = False
    source_family: str | None = None
    classification_confidence: float | None = None


def prepare_observation_geometry(
    observation: dict[str, Any],
    nodes: dict[str, dict[str, Any]],
    *,
    low_confidence_threshold: float = DIAGNOSTIC_LOW_BEARING_CONFIDENCE,
) -> ObservationGeometry:
    observation_id = str(observation.get("observation_id", "unknown-id"))
    node_id_raw = observation.get("node_id")
    node_id = str(node_id_raw) if isinstance(node_id_raw, str) else "unknown-node"
    event_time = _observation_time(observation)

    source_family = None
    classification_confidence = None
    classification = observation.get("classification")
    if isinstance(classification, dict):
        family = classification.get("source_family")
        if isinstance(family, str):
            source_family = family
        classification_confidence = _finite_number(classification.get("confidence"))

    bearing = observation.get("bearing")
    node_bearing = None
    bearing_confidence = None
    if isinstance(bearing, dict):
        node_bearing = _finite_number(bearing.get("deg"))
        bearing_confidence = _finite_number(bearing.get("confidence"))
        reference = bearing.get("reference", "node")
        if reference is not None and reference != "node":
            return ObservationGeometry(
                observation_id=observation_id,
                node_id=node_id,
                event_time_utc=event_time,
                latitude=None,
                longitude=None,
                node_relative_bearing_deg=node_bearing,
                orientation_deg=None,
                geographic_bearing_deg=None,
                bearing_confidence=bearing_confidence,
                eligible=False,
                exclusion_reason=f"unsupported bearing.reference={reference!r}",
                source_family=source_family,
                classification_confidence=classification_confidence,
            )

    if node_bearing is None or not 0 <= node_bearing < 360:
        return ObservationGeometry(
            observation_id=observation_id,
            node_id=node_id,
            event_time_utc=event_time,
            latitude=None,
            longitude=None,
            node_relative_bearing_deg=node_bearing,
            orientation_deg=None,
            geographic_bearing_deg=None,
            bearing_confidence=bearing_confidence,
            eligible=False,
            exclusion_reason="missing or invalid node-relative bearing",
            source_family=source_family,
            classification_confidence=classification_confidence,
        )

    node = nodes.get(node_id) if isinstance(node_id_raw, str) else None
    installation = node.get("installation") if isinstance(node, dict) else None
    if not isinstance(installation, dict):
        return ObservationGeometry(
            observation_id=observation_id,
            node_id=node_id,
            event_time_utc=event_time,
            latitude=None,
            longitude=None,
            node_relative_bearing_deg=node_bearing,
            orientation_deg=None,
            geographic_bearing_deg=None,
            bearing_confidence=bearing_confidence,
            eligible=False,
            exclusion_reason="installation metadata missing",
            source_family=source_family,
            classification_confidence=classification_confidence,
        )

    latitude = _finite_number(installation.get("latitude"))
    longitude = _finite_number(installation.get("longitude"))
    if (
        latitude is None
        or longitude is None
        or not -90 <= latitude <= 90
        or not -180 <= longitude <= 180
    ):
        return ObservationGeometry(
            observation_id=observation_id,
            node_id=node_id,
            event_time_utc=event_time,
            latitude=latitude,
            longitude=longitude,
            node_relative_bearing_deg=node_bearing,
            orientation_deg=None,
            geographic_bearing_deg=None,
            bearing_confidence=bearing_confidence,
            eligible=False,
            exclusion_reason="invalid installation coordinates",
            source_family=source_family,
            classification_confidence=classification_confidence,
        )

    orientation = _finite_number(installation.get("orientation_deg"))
    if orientation is None:
        return ObservationGeometry(
            observation_id=observation_id,
            node_id=node_id,
            event_time_utc=event_time,
            latitude=latitude,
            longitude=longitude,
            node_relative_bearing_deg=node_bearing,
            orientation_deg=None,
            geographic_bearing_deg=None,
            bearing_confidence=bearing_confidence,
            eligible=False,
            exclusion_reason="orientation_deg missing (no default assumed)",
            source_family=source_family,
            classification_confidence=classification_confidence,
        )
    if not 0 <= orientation < 360:
        return ObservationGeometry(
            observation_id=observation_id,
            node_id=node_id,
            event_time_utc=event_time,
            latitude=latitude,
            longitude=longitude,
            node_relative_bearing_deg=node_bearing,
            orientation_deg=orientation,
            geographic_bearing_deg=None,
            bearing_confidence=bearing_confidence,
            eligible=False,
            exclusion_reason="orientation_deg out of [0, 360)",
            source_family=source_family,
            classification_confidence=classification_confidence,
        )

    geo = geographic_bearing_deg(orientation, node_bearing)
    low_conf = (
        bearing_confidence is not None
        and bearing_confidence < low_confidence_threshold
    )
    return ObservationGeometry(
        observation_id=observation_id,
        node_id=node_id,
        event_time_utc=event_time,
        latitude=latitude,
        longitude=longitude,
        node_relative_bearing_deg=node_bearing,
        orientation_deg=orientation,
        geographic_bearing_deg=geo,
        bearing_confidence=bearing_confidence,
        eligible=True,
        exclusion_reason=None,
        low_bearing_confidence=low_conf,
        source_family=source_family,
        classification_confidence=classification_confidence,
    )


@dataclass(frozen=True)
class PairGeometry:
    observation_a_id: str
    observation_b_id: str
    ear_a_id: str
    ear_b_id: str
    time_delta_s: float | None
    geographic_bearing_a_deg: float
    geographic_bearing_b_deg: float
    intersection_status: str
    intersection_latitude: float | None
    intersection_longitude: float | None
    distance_a_m: float | None
    distance_b_m: float | None
    residual_a_deg: float | None
    residual_b_deg: float | None
    geometric_compatible: bool
    temporal_compatible: bool
    geometric_notes: str
    temporal_notes: str
    both_compatible: bool


def assess_temporal_compatibility(
    first: ObservationGeometry,
    second: ObservationGeometry,
    *,
    distance_a_m: float | None,
    distance_b_m: float | None,
    temporal_close_s: float = DIAGNOSTIC_TEMPORAL_CLOSE_S,
) -> tuple[bool, float | None, str]:
    """Report temporal compatibility separately from geometry.

    No fixed source speed is assumed: the Hub analysis model does not define a
    justified speed range. Ear-to-intersection ranges are reported for context
    only. Classification family match is noted but never treated as proof of a
    common source. The provisional quasi-static Δt window only flags whether
    concurrent-ray triangulation is a weaker or stronger diagnostic assumption.
    """
    if first.event_time_utc is None or second.event_time_utc is None:
        return False, None, "timestamp missing; temporal check skipped"

    delta = abs((second.event_time_utc - first.event_time_utc).total_seconds())
    notes: list[str] = [f"Δt={delta:.1f}s"]

    family_a = first.source_family
    family_b = second.source_family
    if family_a is None or family_b is None:
        notes.append("classification incomplete")
    elif family_a == family_b:
        notes.append(f"source_family match={family_a}")
    else:
        notes.append(f"source_family mismatch ({family_a} vs {family_b})")

    if first.low_bearing_confidence or second.low_bearing_confidence:
        notes.append("low bearing confidence present")

    if distance_a_m is not None and distance_b_m is not None:
        notes.append(
            f"ear→locus ranges {distance_a_m:.0f}m / {distance_b_m:.0f}m "
            "(not source speed)"
        )

    close = delta <= temporal_close_s
    if close:
        notes.append(
            f"within provisional quasi-static window ({temporal_close_s:g}s)"
        )
    else:
        notes.append(
            f"outside provisional quasi-static window ({temporal_close_s:g}s); "
            "concurrent-ray assumption degraded"
        )

    family_ok = family_a is None or family_b is None or family_a == family_b
    compatible = close and family_ok
    return compatible, delta, "; ".join(notes)


def analyze_pair(
    first: ObservationGeometry,
    second: ObservationGeometry,
    *,
    residual_plausible_deg: float = DIAGNOSTIC_RESIDUAL_PLAUSIBLE_DEG,
    parallel_sin_eps: float = DIAGNOSTIC_PARALLEL_SIN_EPS,
    temporal_close_s: float = DIAGNOSTIC_TEMPORAL_CLOSE_S,
) -> PairGeometry:
    assert first.eligible and second.eligible
    assert first.latitude is not None and first.longitude is not None
    assert second.latitude is not None and second.longitude is not None
    assert first.geographic_bearing_deg is not None
    assert second.geographic_bearing_deg is not None

    origin_lat = (first.latitude + second.latitude) / 2.0
    origin_lon = (first.longitude + second.longitude) / 2.0
    o1 = latlon_to_local_en(
        first.latitude,
        first.longitude,
        origin_lat=origin_lat,
        origin_lon=origin_lon,
    )
    o2 = latlon_to_local_en(
        second.latitude,
        second.longitude,
        origin_lat=origin_lat,
        origin_lon=origin_lon,
    )
    intersection = intersect_rays_en(
        o1,
        first.geographic_bearing_deg,
        o2,
        second.geographic_bearing_deg,
        parallel_sin_eps=parallel_sin_eps,
    )

    lat = lon = None
    residual_a = residual_b = None
    distance_a = intersection.t1_m
    distance_b = intersection.t2_m
    geometric_notes = intersection.status.replace("_", " ")

    if (
        intersection.east_m is not None
        and intersection.north_m is not None
        and intersection.status
        in {"forward_intersection", "behind_one", "behind_both"}
    ):
        lat, lon = local_en_to_latlon(
            intersection.east_m,
            intersection.north_m,
            origin_lat=origin_lat,
            origin_lon=origin_lon,
        )
        azimuth_a = forward_azimuth_deg(
            first.latitude, first.longitude, lat, lon
        )
        azimuth_b = forward_azimuth_deg(
            second.latitude, second.longitude, lat, lon
        )
        residual_a = smallest_angle_diff_deg(
            first.geographic_bearing_deg, azimuth_a
        )
        residual_b = smallest_angle_diff_deg(
            second.geographic_bearing_deg, azimuth_b
        )
        # For behind intersections, residual to the intersection point along the
        # reverse direction can still be small; status already records geometry.
        geometric_notes = (
            f"{geometric_notes}; "
            f"residuals={residual_a:.2f}°/{residual_b:.2f}° "
            f"(provisional plausible ≤{residual_plausible_deg:g}°)"
        )

    geometric_compatible = False
    if intersection.status == "forward_intersection":
        if (
            residual_a is not None
            and residual_b is not None
            and residual_a <= residual_plausible_deg
            and residual_b <= residual_plausible_deg
        ):
            geometric_compatible = True
            geometric_notes = f"forward intersection plausible; {geometric_notes}"
        else:
            geometric_notes = (
                f"forward intersection but residual outside provisional "
                f"{residual_plausible_deg:g}°; {geometric_notes}"
            )
        if first.low_bearing_confidence or second.low_bearing_confidence:
            geometric_notes += "; treat cautiously (low bearing confidence)"
            # Keep geometric_compatible True if residuals OK, but note caution.
    elif intersection.status == "nearly_parallel":
        geometric_notes = "nearly parallel rays; no useful intersection"
    elif intersection.status == "numerically_unstable":
        geometric_notes = "numerically unstable intersection"
    else:
        geometric_notes = f"{intersection.status.replace('_', ' ')}; not a useful forward locus"

    temporal_compatible, time_delta_s, temporal_notes = assess_temporal_compatibility(
        first,
        second,
        distance_a_m=distance_a if intersection.status == "forward_intersection" else None,
        distance_b_m=distance_b if intersection.status == "forward_intersection" else None,
        temporal_close_s=temporal_close_s,
    )

    return PairGeometry(
        observation_a_id=first.observation_id,
        observation_b_id=second.observation_id,
        ear_a_id=first.node_id,
        ear_b_id=second.node_id,
        time_delta_s=time_delta_s,
        geographic_bearing_a_deg=first.geographic_bearing_deg,
        geographic_bearing_b_deg=second.geographic_bearing_deg,
        intersection_status=intersection.status,
        intersection_latitude=lat,
        intersection_longitude=lon,
        distance_a_m=distance_a,
        distance_b_m=distance_b,
        residual_a_deg=residual_a,
        residual_b_deg=residual_b,
        geometric_compatible=geometric_compatible,
        temporal_compatible=temporal_compatible,
        geometric_notes=geometric_notes,
        temporal_notes=temporal_notes,
        both_compatible=geometric_compatible and temporal_compatible,
    )


@dataclass(frozen=True)
class CrossEarGeometryReport:
    observations: list[ObservationGeometry]
    pairs: list[PairGeometry]
    eligible_count: int
    pair_count: int
    forward_intersection_count: int
    parallel_or_invalid_count: int
    geometrically_plausible_count: int
    temporally_plausible_count: int
    both_compatible_count: int


def analyze_cross_ear_geometry(
    observations: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]],
    *,
    low_confidence_threshold: float = DIAGNOSTIC_LOW_BEARING_CONFIDENCE,
    residual_plausible_deg: float = DIAGNOSTIC_RESIDUAL_PLAUSIBLE_DEG,
    parallel_sin_eps: float = DIAGNOSTIC_PARALLEL_SIN_EPS,
    temporal_close_s: float = DIAGNOSTIC_TEMPORAL_CLOSE_S,
    # Weather must never affect results; accept and ignore if callers pass it.
    environmental_context: dict[str, Any] | None = None,
) -> CrossEarGeometryReport:
    del environmental_context  # informational only; never used
    prepared = [
        prepare_observation_geometry(
            item,
            nodes,
            low_confidence_threshold=low_confidence_threshold,
        )
        for item in observations
    ]
    eligible = [item for item in prepared if item.eligible]
    pairs: list[PairGeometry] = []
    for first, second in combinations(eligible, 2):
        if first.node_id == second.node_id:
            continue
        pairs.append(
            analyze_pair(
                first,
                second,
                residual_plausible_deg=residual_plausible_deg,
                parallel_sin_eps=parallel_sin_eps,
                temporal_close_s=temporal_close_s,
            )
        )

    forward = sum(1 for pair in pairs if pair.intersection_status == "forward_intersection")
    parallel_or_invalid = sum(
        1
        for pair in pairs
        if pair.intersection_status
        in {"nearly_parallel", "numerically_unstable"}
    )
    return CrossEarGeometryReport(
        observations=prepared,
        pairs=pairs,
        eligible_count=len(eligible),
        pair_count=len(pairs),
        forward_intersection_count=forward,
        parallel_or_invalid_count=parallel_or_invalid,
        geometrically_plausible_count=sum(
            1 for pair in pairs if pair.geometric_compatible
        ),
        temporally_plausible_count=sum(
            1 for pair in pairs if pair.temporal_compatible
        ),
        both_compatible_count=sum(1 for pair in pairs if pair.both_compatible),
    )


def _format_time(value: datetime | None) -> str:
    if value is None:
        return "unknown"
    return value.astimezone().strftime("%d/%m/%Y %H:%M:%S %Z")


def format_cross_ear_geometry_lines(
    report: CrossEarGeometryReport,
    *,
    detail: bool = False,
    format_local_datetime: Callable[[datetime | None], str] | None = None,
) -> list[str]:
    format_time = format_local_datetime or _format_time
    lines = [
        "Cross-Ear Bearing Geometry (diagnostic only — not Events or Tracks):",
        (
            "  Convention: geographic_bearing = "
            "(orientation_deg + node_relative_bearing) % 360, "
            "clockwise from true north. "
            "No default orientation is assumed."
        ),
        (
            "  Geometry: local east-north tangent plane; "
            "intersection is a candidate locus only. "
            f"Provisional residual ≤{DIAGNOSTIC_RESIDUAL_PLAUSIBLE_DEG:g}°, "
            f"quasi-static Δt ≤{DIAGNOSTIC_TEMPORAL_CLOSE_S:g}s, "
            f"low bearing confidence <{DIAGNOSTIC_LOW_BEARING_CONFIDENCE:g}."
        ),
        (
            "  Bearing uncertainty: only confidence [0,1] is available; "
            "no calibrated angular uncertainty (σ)."
        ),
        (
            f"  Summary: eligible={report.eligible_count}/"
            f"{len(report.observations)} obs; "
            f"cross-Ear pairs={report.pair_count}; "
            f"forward intersections={report.forward_intersection_count}; "
            f"parallel/invalid={report.parallel_or_invalid_count}; "
            f"geometrically plausible={report.geometrically_plausible_count}; "
            f"temporally plausible={report.temporally_plausible_count}; "
            f"both={report.both_compatible_count}"
        ),
    ]
    if not detail:
        lines.append(
            "  (compact; pass --cross-ear-geometry-detail for per-observation "
            "and pairwise detail)"
        )
        return lines

    lines.append("  Per-observation geographic bearings:")
    if not report.observations:
        lines.append("    none")
    for item in report.observations:
        if item.eligible:
            conf = (
                f"{item.bearing_confidence:g}"
                if item.bearing_confidence is not None
                else "not set"
            )
            caution = " [low confidence]" if item.low_bearing_confidence else ""
            lines.append(
                f"    {item.observation_id} {item.node_id} "
                f"@ {format_time(item.event_time_utc)}: "
                f"lat/lon={item.latitude:.6f},{item.longitude:.6f}; "
                f"node-relative={item.node_relative_bearing_deg:.2f}°; "
                f"orientation={item.orientation_deg:.2f}°; "
                f"geographic={item.geographic_bearing_deg:.2f}°; "
                f"bearing confidence={conf}; angular σ=unavailable"
                f"{caution}"
            )
        else:
            lines.append(
                f"    {item.observation_id} {item.node_id} "
                f"@ {format_time(item.event_time_utc)}: "
                f"ineligible — {item.exclusion_reason}"
            )

    lines.append("  Pairwise analysis:")
    if not report.pairs:
        lines.append("    none (need ≥2 eligible observations from different Ears)")
        return lines

    # Detail stays readable: emphasize temporally close pairs and dual-compatible
    # hits; summarize the long-Δt combinatorial remainder.
    highlighted = [
        pair
        for pair in report.pairs
        if pair.both_compatible
        or (
            pair.time_delta_s is not None
            and pair.time_delta_s <= DIAGNOSTIC_TEMPORAL_CLOSE_S
        )
    ]
    omitted = len(report.pairs) - len(highlighted)
    if omitted:
        lines.append(
            f"    Showing {len(highlighted)} pair(s) with Δt≤"
            f"{DIAGNOSTIC_TEMPORAL_CLOSE_S:g}s or dual-compatible; "
            f"omitted {omitted} long-Δt pair(s) (see summary counts)"
        )
    if not highlighted:
        lines.append(
            "    none within provisional quasi-static window "
            f"({DIAGNOSTIC_TEMPORAL_CLOSE_S:g}s); see summary for full pair counts"
        )
        return lines

    for pair in highlighted:
        delta = (
            f"{pair.time_delta_s:.1f}s"
            if pair.time_delta_s is not None
            else "unknown"
        )
        if (
            pair.intersection_latitude is not None
            and pair.intersection_longitude is not None
        ):
            locus = (
                f"candidate lat/lon={pair.intersection_latitude:.6f},"
                f"{pair.intersection_longitude:.6f}"
            )
        else:
            locus = "no candidate coordinates"
        dist_a = (
            f"{pair.distance_a_m:.0f}m" if pair.distance_a_m is not None else "-"
        )
        dist_b = (
            f"{pair.distance_b_m:.0f}m" if pair.distance_b_m is not None else "-"
        )
        res_a = (
            f"{pair.residual_a_deg:.2f}°"
            if pair.residual_a_deg is not None
            else "-"
        )
        res_b = (
            f"{pair.residual_b_deg:.2f}°"
            if pair.residual_b_deg is not None
            else "-"
        )
        lines.append(
            f"    {pair.observation_a_id}↔{pair.observation_b_id} "
            f"({pair.ear_a_id}↔{pair.ear_b_id}) Δt={delta}: "
            f"geo bearings={pair.geographic_bearing_a_deg:.1f}°/"
            f"{pair.geographic_bearing_b_deg:.1f}°; "
            f"status={pair.intersection_status}; {locus}; "
            f"range={dist_a}/{dist_b}; residual={res_a}/{res_b}"
        )
        lines.append(
            f"      geometric: "
            f"{'plausible' if pair.geometric_compatible else 'not plausible'} "
            f"({pair.geometric_notes})"
        )
        lines.append(
            f"      temporal: "
            f"{'plausible' if pair.temporal_compatible else 'not plausible'} "
            f"({pair.temporal_notes})"
        )
        lines.append(
            f"      both dimensions: "
            f"{'compatible' if pair.both_compatible else 'not compatible'} "
            "(diagnostic only; not an Event/Track)"
        )
    return lines
