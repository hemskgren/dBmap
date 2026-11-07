#!/usr/bin/env python3
import argparse
import json
import math
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import UTC, datetime, timedelta
from itertools import combinations, pairwise
from typing import Any

from lib.cross_ear_bearing_geometry import (
    analyze_cross_ear_geometry,
    format_cross_ear_geometry_lines,
)
from lib.cross_ear_event_correlation import (
    DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
    DEFAULT_MIN_EARS_FOR_EVENT,
    DEFAULT_MIN_PROMOTE_SCORE,
    AssembledObservation,
    CorrelationResult,
    assemble_observations,
    correlate_cross_ear_events,
    format_join_attempt_lines,
    format_membership_trace_lines,
)
from lib.cross_ear_localization import (
    EVALUATION_TARGET_DENSE_M,
    EVALUATION_TARGET_SPARSE_M,
    GroundTruthEvaluation,
    format_dilution_m,
)
from lib.hub_cli import (
    add_hub_connection_arguments,
    format_hub_http_error,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)

EARTH_RADIUS_M = 6_371_000
DEFAULT_EVENT_GAP_SECONDS = 30.0
DEFAULT_MAX_ASSOCIATION_DISTANCE_M = 500.0
DEFAULT_OBSERVATION_PAGE_SIZE = 200
MAX_OBSERVATION_PAGE_SIZE = 500
WATCH_OBSERVATION_PAGE_SIZE = 500


@dataclass
class AnalysisConfig:
    """Deterministic analysis parameters shared by list and watch (B2.7 Phase A)."""

    event_gap_seconds: float = DEFAULT_EVENT_GAP_SECONDS
    max_association_distance_m: float = DEFAULT_MAX_ASSOCIATION_DISTANCE_M
    allow_long_range_sim_ears: bool = False
    association_temporal_gap_seconds: float = DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS
    min_ears_for_event: int = DEFAULT_MIN_EARS_FOR_EVENT
    min_promote_score: int = DEFAULT_MIN_PROMOTE_SCORE
    include_cross_ear_correlation: bool = True
    include_cross_ear_geometry: bool = True
    cross_ear_geometry_detail: bool = False
    include_site_context: bool = True
    include_observation_details: bool = False


@dataclass
class AnalysisSnapshot:
    """Complete report set + nodes + config for deterministic analysis.

    Distinguishes acoustic ``event_time_utc`` (in report payloads) from Hub
    ``received_time_utc`` (ingestion). Association uses acoustic chronology.
    """

    reports: list[dict[str, Any]]
    nodes: dict[str, dict[str, Any]]
    config: AnalysisConfig
    snapshot_id: int | None = None
    environmental_context: dict[str, Any] | None = None
    filter_status_lines: list[str] = field(default_factory=list)
    fetch_meta: dict[str, Any] = field(default_factory=dict)


@dataclass
class AnalysisResult:
    """Structured output of ``analyze(AnalysisSnapshot)``."""

    snapshot: AnalysisSnapshot
    correlation: CorrelationResult | None
    report_count: int
    unique_observation_count: int
    lines: list[str]


def is_sim_ear(node_id: str) -> bool:
    return node_id.startswith("SIM-EAR-")


class HubApiError(RuntimeError):
    pass


def admin_request(url: str, admin_token: str) -> urllib.request.Request:
    return urllib.request.Request(
        url,
        headers={"Authorization": f"Bearer {admin_token}"},
    )


def parse_positive_int(value: int | str, *, label: str) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{label} must be a positive integer") from exc
    if parsed < 1:
        raise ValueError(f"{label} must be a positive integer")
    return parsed


def format_pagination_summary(
    *,
    total_count: int,
    page: int,
    page_size: int,
    total_pages: int,
    order: str,
) -> str:
    order_label = "newest first" if order == "newest" else "oldest first"
    if total_pages <= 0:
        return f"{total_count} reports | 0 pages | page {page}/0 | {order_label}"
    return (
        f"{total_count} reports | {total_pages} pages | "
        f"page {page}/{total_pages} | {order_label}"
    )


def format_observation_envelope_json(envelope: dict[str, Any]) -> str:
    """Pretty-print the Hub observation list envelope for --json."""
    return json.dumps(envelope, indent=2, sort_keys=False, ensure_ascii=False)


def parse_observation_list_envelope(payload: Any) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Validate Hub observation list envelope; return (reports, pagination)."""
    if not isinstance(payload, dict):
        raise HubApiError("hub observations response must be a JSON object envelope")
    pagination = payload.get("pagination")
    reports = payload.get("reports")
    if not isinstance(pagination, dict):
        raise HubApiError("hub observations response missing pagination object")
    if not isinstance(reports, list) or not all(
        isinstance(observation, dict) for observation in reports
    ):
        raise HubApiError("hub observations response missing reports array")

    def require_int(key: str) -> int:
        value = pagination.get(key)
        if not isinstance(value, int) or isinstance(value, bool):
            raise HubApiError(f"hub pagination.{key} must be an integer")
        return value

    order = pagination.get("order")
    if order not in ("newest", "oldest"):
        raise HubApiError("hub pagination.order must be 'newest' or 'oldest'")
    snapshot_id = pagination.get("snapshot_id")
    if snapshot_id is not None and (
        not isinstance(snapshot_id, int) or isinstance(snapshot_id, bool)
    ):
        raise HubApiError("hub pagination.snapshot_id must be an integer or null")

    meta = {
        "total_count": require_int("total_count"),
        "page": require_int("page"),
        "page_size": require_int("page_size"),
        "total_pages": require_int("total_pages"),
        "order": order,
        "snapshot_id": snapshot_id,
    }
    return reports, meta


def fetch_observations(
    base_url: str,
    ca_file: str,
    admin_token: str,
    node_id: str | None = None,
    *,
    observation_id: str | None = None,
    page: int = 1,
    page_size: int = DEFAULT_OBSERVATION_PAGE_SIZE,
    order: str = "newest",
    snapshot_id: int | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any], dict[str, Any]]:
    """
    Fetch one page of observation reports.

    Returns (reports, pagination_meta, full_envelope).
    """
    if page_size < 1 or page_size > MAX_OBSERVATION_PAGE_SIZE:
        raise HubApiError(
            f"page_size must be between 1 and {MAX_OBSERVATION_PAGE_SIZE}"
        )
    if order not in ("newest", "oldest"):
        raise HubApiError("order must be 'newest' or 'oldest'")
    query: dict[str, str | int] = {
        "page": page,
        "page_size": page_size,
        "order": order,
    }
    if node_id is not None:
        query["node_id"] = node_id
    if observation_id is not None:
        query["observation_id"] = observation_id
    if snapshot_id is not None:
        query["snapshot_id"] = snapshot_id
    path = "/api/v1/observations?" + urllib.parse.urlencode(query)
    request = urllib.request.Request(
        f"{base_url}{path}",
        headers={"Authorization": f"Bearer {admin_token}"},
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            envelope = json.load(response)
    except urllib.error.HTTPError as exc:
        raise HubApiError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid observation JSON") from exc

    reports, meta = parse_observation_list_envelope(envelope)
    return reports, meta, envelope


def fetch_all_observation_reports(
    base_url: str,
    ca_file: str,
    admin_token: str,
    node_id: str | None = None,
    *,
    observation_id: str | None = None,
    page_size: int = MAX_OBSERVATION_PAGE_SIZE,
    order: str = "oldest",
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Fetch every matching report page under a frozen Hub snapshot_id.

    Uses ``order=oldest`` by default so pagination is stable; analysis sorts by
    acoustic event time and does not treat page order as chronology.

    Completeness / immutability contract (Hub):
    - Each MQTT report is one immutable ``observations`` row (new autoincrement
      ``id``). There is no in-place append of reports onto an existing row.
    - Hub freezes the set at ``Observation.id <= snapshot_id`` from page 1.
    - A late report for an already-known ``observation_id`` still creates a
      *new* row with a higher ``id``, so it is excluded from the frozen snapshot
      and only appears after a later rebuild (new snapshot / watch re-fetch).
    - This helper sets ``complete_snapshot`` only when the fetched row count
      matches Hub ``total_count`` for that frozen snapshot.
    """
    if page_size < 1 or page_size > MAX_OBSERVATION_PAGE_SIZE:
        raise HubApiError(
            f"page_size must be between 1 and {MAX_OBSERVATION_PAGE_SIZE}"
        )
    first, meta, _envelope = fetch_observations(
        base_url,
        ca_file,
        admin_token,
        node_id,
        observation_id=observation_id,
        page=1,
        page_size=page_size,
        order=order,
    )
    snapshot_id = meta.get("snapshot_id")
    total_pages = int(meta.get("total_pages") or 1)
    total_count = int(meta.get("total_count") or len(first))
    all_reports = list(first)
    for page in range(2, total_pages + 1):
        if snapshot_id is None:
            raise HubApiError(
                "Hub pagination meta omitted snapshot_id; cannot guarantee a "
                "complete frozen snapshot across pages"
            )
        page_reports, page_meta, _ = fetch_observations(
            base_url,
            ca_file,
            admin_token,
            node_id,
            observation_id=observation_id,
            page=page,
            page_size=page_size,
            order=order,
            snapshot_id=int(snapshot_id),
        )
        all_reports.extend(page_reports)
        # Prefer latest meta for totals while preserving freeze id.
        meta = page_meta

    # Deterministic de-dupe by Hub report identity (node_id, sequence_number).
    deduped: list[dict[str, Any]] = []
    seen_keys: set[str] = set()
    duplicate_delivery_count = 0
    for report_row in all_reports:
        key = observation_report_key(report_row)
        if key is None:
            deduped.append(report_row)
            continue
        if key in seen_keys:
            duplicate_delivery_count += 1
            continue
        seen_keys.add(key)
        deduped.append(report_row)
    all_reports = deduped

    if snapshot_id is not None:
        meta = {**meta, "snapshot_id": snapshot_id}
    fetched_count = len(all_reports)
    complete = fetched_count == total_count and (
        total_pages <= 1 or snapshot_id is not None
    )
    meta = {
        **meta,
        "fetched_report_count": fetched_count,
        "total_count": total_count,
        "complete_snapshot": complete,
        "duplicate_delivery_dropped": duplicate_delivery_count,
        "order": order,
        "snapshot_freeze": (
            "Observation.id <= snapshot_id (Hub); later ingest ids excluded"
        ),
    }
    if not complete:
        meta["completeness_warning"] = (
            f"fetched {fetched_count} reports but Hub total_count={total_count} "
            f"for snapshot_id={snapshot_id!r}"
        )
    return all_reports, meta


def fetch_observation_count(
    base_url: str,
    ca_file: str,
    admin_token: str,
    node_id: str | None = None,
) -> dict[str, int]:
    query: dict[str, str] = {}
    if node_id is not None:
        query["node_id"] = node_id
    path = "/api/v1/observations/count"
    if query:
        path = path + "?" + urllib.parse.urlencode(query)
    request = admin_request(f"{base_url}{path}", admin_token)
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as exc:
        raise HubApiError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid observation count JSON") from exc

    if not isinstance(payload, dict):
        raise HubApiError("hub returned an invalid observation count object")
    total = payload.get("total")
    unique = payload.get("unique_observations")
    if not isinstance(total, int) or isinstance(total, bool):
        raise HubApiError("hub observation count missing integer total")
    if not isinstance(unique, int) or isinstance(unique, bool):
        raise HubApiError("hub observation count missing integer unique_observations")
    return {"total": total, "unique_observations": unique}


def fetch_nodes(
    base_url: str,
    ca_file: str,
    admin_token: str,
) -> dict[str, dict[str, Any]]:
    path = "/api/v1/nodes"
    request = admin_request(f"{base_url}{path}", admin_token)
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            nodes = json.load(response)
    except urllib.error.HTTPError as exc:
        raise HubApiError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid node JSON") from exc

    if not isinstance(nodes, list) or not all(
        isinstance(node, dict)
        and isinstance(node.get("node_id"), str)
        and (
            node.get("installation") is None
            or isinstance(node.get("installation"), dict)
        )
        for node in nodes
    ):
        raise HubApiError("hub returned an invalid node list or installation metadata")
    return {node["node_id"]: node for node in nodes}


def fetch_environmental_context(
    base_url: str,
    ca_file: str,
    admin_token: str,
    *,
    at: datetime | None = None,
    ensure: bool = False,
) -> dict[str, Any] | None:
    query: dict[str, str] = {}
    if at is not None:
        query["at"] = at.astimezone(UTC).isoformat()
    if ensure:
        query["ensure"] = "true"
    path = "/api/v1/hub/environmental-context"
    if query:
        path = path + "?" + urllib.parse.urlencode(query)
    request = admin_request(f"{base_url}{path}", admin_token)
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as exc:
        # Older Hubs without the endpoint should not fail the whole report.
        if exc.code == 404:
            return None
        raise HubApiError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid environmental context JSON") from exc
    if not isinstance(payload, dict):
        raise HubApiError("hub returned an invalid environmental context object")
    return payload


def format_environmental_context_lines(weather: dict[str, Any] | None) -> list[str]:
    if weather is None:
        return []
    availability = weather.get("availability", "unavailable")
    lines = [
        "Regional environmental context (informational; not association input):"
    ]
    region = weather.get("region_id", "?")
    provider = weather.get("provider", "?")
    mode = weather.get("source_mode") or "-"
    cache = "cache" if weather.get("from_cache") else "fetch"
    age = weather.get("age_s")
    age_text = f"{age:.0f}s" if isinstance(age, (int, float)) else "-"
    lines.append(
        f"  region={region}  provider={provider}  mode={mode}  "
        f"availability={availability}  via={cache}  age={age_text}"
    )
    if availability == "unavailable":
        lines.append("  weather unavailable (no fabricated values)")
        return lines
    fetched = weather.get("fetched_at") or weather.get("valid_from") or "?"
    temp = weather.get("temperature_c")
    humidity = weather.get("relative_humidity_pct")
    wind_speed = weather.get("wind_speed_ms")
    wind_dir = weather.get("wind_direction_deg")
    lines.append(f"  snapshot={fetched}")
    lines.append(
        f"  temp={temp if temp is not None else '-'}°C  "
        f"humidity={humidity if humidity is not None else '-'}%  "
        f"wind={wind_speed if wind_speed is not None else '-'} m/s "
        f"@ {wind_dir if wind_dir is not None else '-'}°"
    )
    return lines


def observation_fields(observation: dict[str, Any]) -> dict[str, Any]:
    """Flatten hierarchical v1 Observation fields for display helpers."""
    identity = observation.get("identity")
    context = observation.get("context")
    acoustic = observation.get("acoustic")
    spatial = observation.get("spatial")
    if isinstance(identity, dict) and isinstance(context, dict):
        classification = None
        bearing = None
        signal = None
        if isinstance(acoustic, dict):
            classification = acoustic.get("classification")
            signal = acoustic.get("signal_level_dbfs")
        if isinstance(spatial, dict):
            bearing = spatial.get("bearing")
        return {
            "observation_id": identity.get("observation_id"),
            "node_id": identity.get("node_id"),
            "sequence_number": identity.get("sequence_number"),
            "event_time_utc": context.get("event_time_utc"),
            "duration_ms": context.get("interval_duration_ms"),
            "emission_kind": context.get("emission_kind"),
            "classification": classification,
            "bearing": bearing,
            "signal_level_dbfs": signal,
        }
    return observation


def observation_time(observation: dict[str, Any]) -> datetime | None:
    fields = observation_fields(observation)
    value = fields.get("event_time_utc")
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(UTC)


def format_local_datetime(value: datetime | None) -> str:
    if value is None:
        return "unknown"
    return value.astimezone().strftime("%d/%m/%Y %H:%M:%S %Z")


def sort_observations(observations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(
        observations,
        key=lambda item: (
            observation_time(item) is None,
            observation_time(item) or datetime.max.replace(tzinfo=UTC),
        ),
    )


def candidate_groups(
    observations: list[dict[str, Any]],
) -> list[tuple[str | None, list[dict[str, Any]]]]:
    return [(None, [item]) for item in sort_observations(observations)]


def unique_observation_count(observations: list[dict[str, Any]]) -> int:
    ids: set[str] = set()
    for item in observations:
        observation_id = observation_fields(item).get("observation_id")
        if isinstance(observation_id, str) and observation_id:
            ids.add(observation_id)
    return len(ids)


def filter_observations(
    observations: list[dict[str, Any]],
    *,
    node_ids: set[str] | None = None,
    observation_ids: set[str] | None = None,
) -> list[dict[str, Any]]:
    """Client-side Node ID / Observation ID filters (same semantics as the web UI)."""
    node_filter = {item for item in (node_ids or set()) if item}
    observation_filter = {item for item in (observation_ids or set()) if item}
    if not node_filter and not observation_filter:
        return observations
    filtered: list[dict[str, Any]] = []
    for item in observations:
        fields = observation_fields(item)
        if node_filter:
            node_id = fields.get("node_id")
            if not isinstance(node_id, str) or node_id not in node_filter:
                continue
        if observation_filter:
            observation_id = fields.get("observation_id")
            if (
                not isinstance(observation_id, str)
                or observation_id not in observation_filter
            ):
                continue
        filtered.append(item)
    return filtered


def format_filter_status(
    *,
    node_ids: set[str] | None = None,
    observation_ids: set[str] | None = None,
    fetched_count: int,
    filtered_count: int,
) -> list[str]:
    node_filter = sorted(item for item in (node_ids or set()) if item)
    observation_filter = sorted(item for item in (observation_ids or set()) if item)
    if not node_filter and not observation_filter:
        return []
    lines = [
        f"Filters applied (of {fetched_count} fetched reports → {filtered_count} match):"
    ]
    if node_filter:
        lines.append(f"  Node ID: {', '.join(node_filter)}")
    if observation_filter:
        lines.append(f"  Observation ID: {', '.join(observation_filter)}")
    return lines


def format_observations_json(observations: list[dict[str, Any]]) -> str:
    """Pretty-print filtered observation reports for AI/report feeds (web JSON panel)."""
    return json.dumps(
        sort_observations(observations),
        indent=2,
        ensure_ascii=False,
        default=str,
    )


def installation_distance_m(
    first: dict[str, Any],
    second: dict[str, Any],
) -> float | None:
    try:
        lat1 = math.radians(float(first["latitude"]))
        lon1 = math.radians(float(first["longitude"]))
        lat2 = math.radians(float(second["latitude"]))
        lon2 = math.radians(float(second["longitude"]))
    except (KeyError, TypeError, ValueError):
        return None
    if (
        not all(math.isfinite(value) for value in (lat1, lon1, lat2, lon2))
        or not -math.pi / 2 <= lat1 <= math.pi / 2
        or not -math.pi / 2 <= lat2 <= math.pi / 2
        or not -math.pi <= lon1 <= math.pi
        or not -math.pi <= lon2 <= math.pi
    ):
        return None
    lat_delta = lat2 - lat1
    lon_delta = lon2 - lon1
    haversine = (
        math.sin(lat_delta / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(lon_delta / 2) ** 2
    )
    central_angle = 2 * math.asin(math.sqrt(min(1.0, haversine)))
    return EARTH_RADIUS_M * central_angle


def format_installation(node_id: str, node: dict[str, Any] | None) -> str:
    if node is None:
        return f"{node_id}: node metadata unavailable"
    installation = node.get("installation")
    if installation is None:
        return f"{node_id}: installation not set"
    latitude = installation.get("latitude")
    longitude = installation.get("longitude")
    fields = []
    if isinstance(latitude, (int, float)) and isinstance(longitude, (int, float)):
        fields.append(f"lat/lon={latitude:.6f},{longitude:.6f}")
    else:
        fields.append("lat/lon unavailable")
    for key, label in (
        ("height_m", "mount height"),
        ("mount_type", "mount"),
        ("environment", "environment"),
        ("orientation_deg", "orientation"),
    ):
        value = installation.get(key)
        if value is not None:
            suffix = (
                "m"
                if key == "height_m"
                else (" deg" if key == "orientation_deg" else "")
            )
            fields.append(f"{label}={value}{suffix}")
    return f"{node_id}: " + ", ".join(fields)


def association_range_note(
    first_id: str,
    second_id: str,
    distance_m: float,
    *,
    max_association_distance_m: float,
    allow_long_range_sim_ears: bool,
) -> str:
    limit = f"{max_association_distance_m:g}m"
    if distance_m <= max_association_distance_m:
        return f"≤{limit}"
    both_sim = is_sim_ear(first_id) and is_sim_ear(second_id)
    if both_sim and allow_long_range_sim_ears:
        return f">{limit}, SIM eligible"
    return f">{limit}"


def report_site_distances(
    observations: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]],
    *,
    max_association_distance_m: float = DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
    allow_long_range_sim_ears: bool = False,
) -> list[str]:
    node_ids = sorted(
        {
            nid
            for item in observations
            for nid in [observation_fields(item).get("node_id")]
            if isinstance(nid, str)
        }
    )
    if len(node_ids) < 2:
        return []

    lines = [
        "Ear installation distances (site context only; not source associations):"
    ]
    for first_id, second_id in combinations(node_ids, 2):
        first_node = nodes.get(first_id)
        second_node = nodes.get(second_id)
        first_installation = first_node.get("installation") if first_node else None
        second_installation = second_node.get("installation") if second_node else None
        if not isinstance(first_installation, dict) or not isinstance(
            second_installation, dict
        ):
            missing = [
                node_id
                for node_id, installation in (
                    (first_id, first_installation),
                    (second_id, second_installation),
                )
                if not isinstance(installation, dict)
            ]
            lines.append(
                f"      {first_id} ↔ {second_id}: distance unavailable "
                f"(installation missing for {', '.join(missing)})"
            )
            continue
        distance = installation_distance_m(first_installation, second_installation)
        if distance is None:
            lines.append(
                f"      {first_id} ↔ {second_id}: invalid installation coordinates"
            )
            continue
        note = association_range_note(
            first_id,
            second_id,
            distance,
            max_association_distance_m=max_association_distance_m,
            allow_long_range_sim_ears=allow_long_range_sim_ears,
        )
        lines.append(f"      {first_id} ↔ {second_id}: {distance:.0f}m ({note})")
    return lines


def proposed_event_groups(
    observations: list[dict[str, Any]],
    event_gap_seconds: float,
) -> list[tuple[list[dict[str, Any]], str]]:
    ordered = sort_observations(observations)
    events: list[tuple[list[dict[str, Any]], str]] = []
    current: list[dict[str, Any]] = []
    previous_end: datetime | None = None

    for observation in ordered:
        start = observation_time(observation)
        if start is None:
            if current:
                events.append((current, "timestamp unavailable; kept separate"))
                current = []
            events.append(([observation], "timestamp unavailable; kept separate"))
            previous_end = None
            continue

        if previous_end is not None:
            gap_seconds = (start - previous_end).total_seconds()
            if gap_seconds > event_gap_seconds:
                events.append(
                    (
                        current,
                        f"gap {gap_seconds:.1f}s exceeds {event_gap_seconds:g}s window",
                    )
                )
                current = []
            elif current and gap_seconds < 0:
                reason = f"overlap {abs(gap_seconds):.1f}s within {event_gap_seconds:g}s window"
            else:
                reason = f"gap {max(gap_seconds, 0):.1f}s within {event_gap_seconds:g}s window"
        else:
            reason = "first observation in candidate group"

        current.append(observation)
        duration_ms = observation_fields(observation).get("duration_ms", 0)
        if (
            not isinstance(duration_ms, (int, float))
            or isinstance(duration_ms, bool)
            or not math.isfinite(duration_ms)
        ):
            duration_ms = 0
        previous_end = start + timedelta(milliseconds=max(duration_ms, 0))

    if current:
        events.append((current, reason))
    return events


def format_observation(observation: dict[str, Any]) -> str:
    fields = observation_fields(observation)
    observation_id = fields.get("observation_id", "unknown-id")
    node_id = fields.get("node_id", "unknown-node")
    parsed_event_time = observation_time(observation)
    event_time = (
        format_local_datetime(parsed_event_time)
        if parsed_event_time is not None
        else fields.get("event_time_utc", "unknown-time")
    )
    details = []
    classification = fields.get("classification")
    if isinstance(classification, dict):
        source_family = classification.get("source_family")
        confidence = classification.get("confidence")
        if isinstance(source_family, str):
            classification_detail = f"classification={source_family}"
            if isinstance(confidence, (int, float)) and not isinstance(
                confidence, bool
            ):
                classification_detail += f" (confidence={confidence:g})"
            details.append(classification_detail)

    bearing = fields.get("bearing")
    if isinstance(bearing, dict) and isinstance(bearing.get("deg"), (int, float)):
        bearing_detail = f"node-relative bearing={bearing['deg']} deg"
        confidence = bearing.get("confidence")
        if isinstance(confidence, (int, float)) and not isinstance(confidence, bool):
            bearing_detail += f" (confidence={confidence:g})"
        details.append(bearing_detail)

    signal = fields.get("signal_level_dbfs")
    if isinstance(signal, (int, float)):
        details.append(f"level={signal} dBFS")
    suffix = f"; {', '.join(details)}" if details else ""
    return f"{event_time}: {observation_id} from {node_id}{suffix}"


def _valid_sequence_number(value: Any) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def sequence_gap_count(sequences: list[int]) -> int:
    """Count missing integers between distinct sorted sequence numbers."""
    if len(sequences) < 2:
        return 0
    uniq = sorted(set(sequences))
    missing = 0
    for earlier, later in pairwise(uniq):
        missing += later - earlier - 1
    return missing


def summarize_episode_observation_groups(
    episode: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Group episode reports by observation_id in first-seen order."""
    groups: dict[str, dict[str, Any]] = {}
    order: list[str] = []
    for item in episode:
        fields = observation_fields(item)
        raw_id = fields.get("observation_id")
        observation_id = (
            str(raw_id) if raw_id is not None and raw_id != "" else "unknown-id"
        )
        group = groups.get(observation_id)
        if group is None:
            group = {
                "observation_id": observation_id,
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
            groups[observation_id] = group
            order.append(observation_id)
        group["reports"].append(item)
        kind = fields.get("emission_kind")
        if isinstance(kind, str) and kind.lower() in group["emission_counts"]:
            group["emission_counts"][kind.lower()] += 1
        else:
            group["emission_counts"]["unknown"] += 1
        sequence = _valid_sequence_number(fields.get("sequence_number"))
        if sequence is not None:
            group["sequences"].append(sequence)
    return [groups[observation_id] for observation_id in order]


def format_episode_observation_summaries(
    episode: list[dict[str, Any]],
) -> list[str]:
    """Presentation lines summarizing unique observation_ids inside an episode."""
    groups = summarize_episode_observation_groups(episode)
    unique_label = "observation" if len(groups) == 1 else "observations"
    lines = [f"    observations: {len(groups)} unique {unique_label}"]
    for group in groups:
        counts = group["emission_counts"]
        sequences = group["sequences"]
        lines.append(f"    Observation {group['observation_id']}")
        lines.append(f"      reports: {len(group['reports'])}")
        emission_line = (
            "      emissions: "
            f"START={counts['start']} "
            f"PERIODIC={counts['periodic']} "
            f"END={counts['end']} "
            f"COMPLETE={counts['complete']}"
        )
        if counts["unknown"]:
            emission_line += f" unknown={counts['unknown']}"
        lines.append(emission_line)
        if sequences:
            first_seq = min(sequences)
            last_seq = max(sequences)
            lines.append(f"      sequence: {first_seq}–{last_seq}")
            lines.append(f"      sequence gaps: {sequence_gap_count(sequences)}")
        else:
            lines.append("      sequence: n/a")
            lines.append("      sequence gaps: n/a")
        known_times = [
            stamp
            for item in group["reports"]
            for stamp in [observation_time(item)]
            if stamp is not None
        ]
        if known_times:
            lines.append(
                "      time: "
                f"{format_local_datetime(min(known_times))} to "
                f"{format_local_datetime(max(known_times))}"
            )
    return lines


def build_assembled_observations(
    observations: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]] | None = None,
) -> list[AssembledObservation]:
    return assemble_observations(
        observations,
        nodes,
        observation_fields=observation_fields,
        observation_time=observation_time,
        sequence_gap_count=sequence_gap_count,
        sort_observations=sort_observations,
    )


def run_cross_ear_correlation(
    observations: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]] | None = None,
    *,
    association_temporal_gap_seconds: float = DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
    max_association_distance_m: float = DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
    allow_long_range_sim_ears: bool = False,
    min_ears_for_event: int = DEFAULT_MIN_EARS_FOR_EVENT,
    min_promote_score: int = DEFAULT_MIN_PROMOTE_SCORE,
) -> CorrelationResult:
    assembled = build_assembled_observations(observations, nodes)
    return correlate_cross_ear_events(
        assembled,
        nodes,
        association_temporal_gap_seconds=association_temporal_gap_seconds,
        max_association_distance_m=max_association_distance_m,
        allow_long_range_sim_ears=allow_long_range_sim_ears,
        min_ears_for_event=min_ears_for_event,
        min_promote_score=min_promote_score,
        installation_distance_m=installation_distance_m,
        is_sim_ear=is_sim_ear,
    )


def analyze(snapshot: AnalysisSnapshot) -> AnalysisResult:
    """Shared deterministic analysis entry point for list and watch (B2.7 Phase A)."""
    config = snapshot.config
    correlation = None
    if config.include_cross_ear_correlation:
        correlation = run_cross_ear_correlation(
            snapshot.reports,
            snapshot.nodes,
            association_temporal_gap_seconds=config.association_temporal_gap_seconds,
            max_association_distance_m=config.max_association_distance_m,
            allow_long_range_sim_ears=config.allow_long_range_sim_ears,
            min_ears_for_event=config.min_ears_for_event,
            min_promote_score=config.min_promote_score,
        )
    lines = report(
        snapshot.reports,
        config.event_gap_seconds,
        snapshot.nodes,
        max_association_distance_m=config.max_association_distance_m,
        allow_long_range_sim_ears=config.allow_long_range_sim_ears,
        association_temporal_gap_seconds=config.association_temporal_gap_seconds,
        min_ears_for_event=config.min_ears_for_event,
        min_promote_score=config.min_promote_score,
        include_cross_ear_correlation=config.include_cross_ear_correlation,
        environmental_context=snapshot.environmental_context,
        include_site_context=config.include_site_context,
        include_cross_ear_geometry=config.include_cross_ear_geometry,
        cross_ear_geometry_detail=config.cross_ear_geometry_detail,
        include_observation_details=config.include_observation_details,
        filter_status_lines=snapshot.filter_status_lines or None,
        hub_snapshot_id=snapshot.snapshot_id,
        fetch_meta=snapshot.fetch_meta,
        precomputed_correlation=correlation,
    )
    return AnalysisResult(
        snapshot=snapshot,
        correlation=correlation,
        report_count=len(snapshot.reports),
        unique_observation_count=unique_observation_count(snapshot.reports),
        lines=lines,
    )


def format_assembled_observation_lines(
    observations: list[AssembledObservation],
    correlation: CorrelationResult | None = None,
) -> list[str]:
    membership: dict[str, str] = {}
    membership_ears: dict[str, int] = {}
    if correlation is not None:
        for candidate in correlation.candidates:
            label = (
                f"Event {candidate.event_id}"
                if candidate.status == "Promoted" and candidate.event_id
                else f"Candidate {candidate.candidate_id}"
            )
            ear_count = len(candidate.ear_ids)
            for observation_id in candidate.observation_ids:
                membership[observation_id] = label
                membership_ears[observation_id] = ear_count
    lines = [
        f"Assembled Observations (analysis objects): {len(observations)}",
    ]
    for index, item in enumerate(observations, start=1):
        # Distinct Ears currently associated with this Observation (self alone = 1).
        support = membership_ears.get(item.observation_id, 1)
        assoc = membership.get(item.observation_id, "none")
        interval = "unknown"
        if item.interval_start is not None and item.interval_end is not None:
            interval = (
                f"{format_local_datetime(item.interval_start)} to "
                f"{format_local_datetime(item.interval_end)}"
            )
        lines.append(
            f"  Observation O{index}: {item.observation_id} on {item.node_id}"
        )
        lines.append(
            f"    reports={item.report_count}; lifecycle_complete="
            f"{item.lifecycle_complete}; sequence_gaps={item.sequence_gaps}"
        )
        lines.append(f"    interval: {interval}")
        if item.bearing_samples:
            lines.append(
                f"    bearing reports: {len(item.bearing_samples)} "
                "(confidence is not angular σ)"
            )
            for sample in item.bearing_samples:
                stamp = (
                    format_local_datetime(sample.event_time_utc)
                    if sample.event_time_utc is not None
                    else "unknown-time"
                )
                seq = (
                    str(sample.sequence_number)
                    if sample.sequence_number is not None
                    else "n/a"
                )
                conf = (
                    f"{sample.confidence:g}"
                    if sample.confidence is not None
                    else "n/a"
                )
                lines.append(
                    f"      seq={seq} t={stamp} "
                    f"bearing={sample.bearing_deg:g} deg conf={conf}"
                )
        elif item.bearing_deg is not None:
            bearing_line = f"    bearing: {item.bearing_deg:g} deg (node-relative)"
            if item.bearing_confidence is not None:
                bearing_line += f"; confidence={item.bearing_confidence:g} (not σ)"
            lines.append(bearing_line)
        else:
            lines.append("    bearing: unavailable")
        if item.classification:
            family = item.classification.get("source_family", "unknown")
            conf = item.classification.get("confidence")
            class_line = f"    classification (reporting only): {family}"
            if isinstance(conf, (int, float)):
                class_line += f" (confidence={conf:g})"
            policy = item.classification.get("association_policy")
            if isinstance(policy, str):
                class_line += f"; {policy}"
            lines.append(class_line)
        lines.append(f"    Cross-EAR support: {support}")
        lines.append(f"    Association: {assoc}")
    return lines


def format_localization_lines(
    *,
    title: str,
    localization,
    ground_truth: GroundTruthEvaluation | None = None,
) -> list[str]:
    lines = [title]
    if localization is None:
        lines.append("    localization: unavailable")
        return lines
    lines.append(f"    localization status: {localization.status}")
    if localization.latitude is not None and localization.longitude is not None:
        lines.append(
            f"    estimated position: {localization.latitude:.6f},"
            f"{localization.longitude:.6f}"
        )
    else:
        lines.append("    estimated position: none")
    lines.append(f"    selection: {localization.selection_method}")
    if localization.max_bearing_dt_s is not None:
        lines.append(
            f"    max bearing |Δt|: {localization.max_bearing_dt_s:.3f}s "
            "(event_time_utc; not assumed perfectly synced)"
        )
    else:
        lines.append("    max bearing |Δt|: unknown")
    if localization.pairwise_spread_m is not None:
        lines.append(
            f"    pairwise intersection spread: {localization.pairwise_spread_m:.0f}m"
        )
    best_angle = localization.details.get("best_intersection_angle_deg")
    min_angle = localization.details.get("min_intersection_angle_deg")
    if best_angle is not None and min_angle is not None:
        lines.append(
            f"    LOB intersection angles: min={min_angle:.1f}° "
            f"best={best_angle:.1f}° (acute ⇒ sensitive; not calibrated σ)"
        )
    network_sens = localization.details.get("network_geometric_sensitivity_m")
    if network_sens is not None:
        lines.append(
            f"    geometric sensitivity indicator (max consensus baseline/sinθ): "
            f"{format_dilution_m(network_sens)} "
            "(not a confidence radius; residuals are separate)"
        )
    outliers = localization.details.get("outlier_pairs") or []
    if outliers:
        lines.append(f"    outlier pairs retained as diagnostics: {', '.join(outliers)}")
    if localization.details.get("position_withheld"):
        lines.append(
            "    position withheld: geometry or multi-ray evidence insufficient "
            "for a defensible estimate"
        )
    network_class = localization.details.get("network_class", "unknown")
    aspirational = localization.details.get("evaluation_target_m")
    if aspirational is None:
        aspirational = (
            EVALUATION_TARGET_DENSE_M
            if network_class == "dense"
            else EVALUATION_TARGET_SPARSE_M
        )
    lines.append(f"    network geometry: {network_class}")
    lines.append(
        "    calibrated uncertainty: unavailable "
        f"(aspirational target ±{aspirational:g}m is not a guarantee; "
        f"dense default ±{EVALUATION_TARGET_DENSE_M:g}m, "
        f"sparse default ±{EVALUATION_TARGET_SPARSE_M:g}m)"
    )
    for note in localization.quality_notes[:8]:
        lines.append(f"    quality: {note}")
    for pair in localization.pairs:
        dt = (
            f"{pair.bearing_dt_s:.3f}s"
            if pair.bearing_dt_s is not None
            else "unknown"
        )
        angle = (
            f"{pair.intersection_angle_deg:.1f}°"
            if pair.intersection_angle_deg is not None
            else "n/a"
        )
        baseline = (
            f"{pair.baseline_m:.0f}m"
            if getattr(pair, "baseline_m", None) is not None
            else "n/a"
        )
        dilution = format_dilution_m(getattr(pair, "geometric_dilution_m", None))
        outlier = "; outlier" if pair.outlier else ""
        lines.append(
            f"    pair {pair.ear_a_id}↔{pair.ear_b_id}: "
            f"{pair.intersection_status}; Δt={dt}; "
            f"bearings={pair.bearing_a_deg:g}/{pair.bearing_b_deg:g}; "
            f"angle={angle}; baseline={baseline}; "
            f"dilution={dilution}; aligned={pair.time_aligned}{outlier}"
        )
    if ground_truth is not None:
        lines.append("    ground-truth evaluation (simulation only; post-hoc):")
        lines.append(f"      network class: {ground_truth.network_class}")
        lines.append(
            f"      GT position: {ground_truth.ground_truth_latitude:.6f},"
            f"{ground_truth.ground_truth_longitude:.6f}"
        )
        if ground_truth.error_m is not None:
            within = "yes" if ground_truth.within_target_m else "no"
            lines.append(
                f"      position error: {ground_truth.error_m:.1f}m "
                f"(within ±{ground_truth.target_m:g}m aspirational target: {within})"
            )
            lines.append(
                "      error bands: "
                f"≤50m={ground_truth.within_50m} "
                f"≤100m={ground_truth.within_100m} "
                f"≤250m={ground_truth.within_250m}"
            )
        else:
            lines.append("      position error: n/a")
        lines.append(f"      note: {ground_truth.notes}")
    return lines


def format_correlation_lines(
    correlation: CorrelationResult,
    *,
    ground_truth_by_event: dict[str, GroundTruthEvaluation] | None = None,
) -> list[str]:
    params = correlation.parameters
    lines = [
        "Cross-EAR Event association (experimental, script-local; not Hub Events):",
        (
            "  parameters: "
            f"association_gap={params['association_temporal_gap_seconds']:g}s, "
            f"association_radius={params['max_association_distance_m']:g}m, "
            f"allow_long_range_sim_ears={params['allow_long_range_sim_ears']}, "
            f"min_ears_for_event={params['min_ears_for_event']}, "
            f"min_promote_score={params['min_promote_score']}"
        ),
        "  Ground truth ignored for association and localization math.",
        "  Scenario-authored source_family does not award acoustic association points.",
        "  Localization is reported separately and does not gate Event creation.",
        (
            f"  membership_policy={params.get('membership_policy', 'legacy')}; "
            f"multi_compatible_policy={params.get('multi_compatible_policy', 'legacy')}"
        ),
    ]
    open_candidates = [
        item for item in correlation.candidates if item.status == "EventCandidate"
    ]
    promoted = [
        item for item in correlation.candidates if item.status == "Promoted"
    ]
    lines.append(f"  Event Candidates (open): {len(open_candidates)}")
    if not open_candidates:
        lines.append("    none")
    for candidate in open_candidates:
        lines.append(
            f"    Candidate {candidate.candidate_id}: "
            f"{len(candidate.ear_ids)} Ear(s) ({', '.join(candidate.ear_ids)}); "
            f"association score={candidate.score} {candidate.score_breakdown}; "
            f"uncertainty={candidate.qualitative_uncertainty}"
        )
        lines.append(
            f"      observations: {', '.join(candidate.observation_ids)}"
        )
        for note in candidate.evidence_notes[:6]:
            lines.append(f"      association evidence: {note}")
        lines.extend(
            format_localization_lines(
                title="      Localization (separate from association):",
                localization=candidate.localization,
            )
        )

    lines.append(f"  Events (promoted): {len(correlation.events)}")
    if not correlation.events:
        lines.append("    none")
    for event in correlation.events:
        loc = event.localization
        contributor_count = len(loc.contributing_ears) if loc is not None else 0
        bearing_obs = sum(
            1
            for member in event.members
            if member.bearing_deg is not None or member.bearing_samples
        )
        lines.append(
            f"    Event {event.event_id}: from Candidate {event.candidate_id}; "
            f"{len(event.ear_ids)} Ear(s) ({', '.join(event.ear_ids)}); "
            f"association score={event.score} {event.score_breakdown}; "
            f"uncertainty={event.qualitative_uncertainty}"
        )
        lines.append(f"      observations: {', '.join(event.observation_ids)}")
        lines.append(
            f"      association membership: "
            f"{len(event.observation_ids)} Observation(s), "
            f"{len(event.ear_ids)} unique EAR(s); "
            f"with_bearing={bearing_obs}; "
            f"localization_contributors={contributor_count}"
        )
        gt = None
        if ground_truth_by_event:
            gt = ground_truth_by_event.get(event.event_id)
        lines.extend(
            format_localization_lines(
                title="      Localization (separate from association):",
                localization=event.localization,
                ground_truth=gt,
            )
        )
        if event.localization is None or event.localization.latitude is None:
            lines.append(
                "      note: Event may exist without a reliable position estimate"
            )

    unresolved = getattr(correlation, "unresolved", None) or []
    lines.append(f"  Unresolved multi-compatible associations: {len(unresolved)}")
    if not unresolved:
        lines.append("    none")
    for item in unresolved:
        lines.append(
            f"    Observation {item.observation_id} ({item.node_id}): {item.reason}; "
            f"candidates={item.compatible_candidate_ids}; "
            f"events={item.compatible_event_ids}"
        )

    lines.append(f"  Rejected associations: {len(correlation.rejected)}")
    if not correlation.rejected:
        lines.append("    none")
    for item in correlation.rejected:
        lines.append(
            f"    Observation {item.observation_id} ({item.node_id}) vs "
            f"{item.against}: {item.reason}"
        )
    lines.extend(format_join_attempt_lines(correlation.join_attempts))
    lines.extend(format_membership_trace_lines(correlation))

    lines.append(f"  Expired Candidates: {len(correlation.expired)}")
    if not correlation.expired:
        lines.append("    none (batch analysis does not auto-expire yet)")
    else:
        for candidate in correlation.expired:
            lines.append(f"    Candidate {candidate.candidate_id}: Expired")

    if promoted:
        lines.append(
            f"  Promoted Candidate records retained for diagnostics: {len(promoted)}"
        )
    return lines


def report(
    observations: list[dict[str, Any]],
    event_gap_seconds: float = DEFAULT_EVENT_GAP_SECONDS,
    nodes: dict[str, dict[str, Any]] | None = None,
    *,
    max_association_distance_m: float = DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
    allow_long_range_sim_ears: bool = False,
    association_temporal_gap_seconds: float = DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
    min_ears_for_event: int = DEFAULT_MIN_EARS_FOR_EVENT,
    min_promote_score: int = DEFAULT_MIN_PROMOTE_SCORE,
    include_cross_ear_correlation: bool = True,
    environmental_context: dict[str, Any] | None = None,
    include_site_context: bool = True,
    include_cross_ear_geometry: bool = True,
    cross_ear_geometry_detail: bool = False,
    include_observation_details: bool = False,
    filter_status_lines: list[str] | None = None,
    hub_snapshot_id: int | None = None,
    fetch_meta: dict[str, Any] | None = None,
    precomputed_correlation: CorrelationResult | None = None,
) -> list[str]:
    parse_positive_interval(event_gap_seconds)
    parse_positive_interval(max_association_distance_m)
    parse_positive_interval(association_temporal_gap_seconds)
    nodes = nodes or {}
    fetch_meta = fetch_meta or {}
    complete = bool(fetch_meta.get("complete_snapshot"))
    fetch_note = (
        "complete snapshot"
        if complete
        else "page fetch (prefer complete snapshot via analyze())"
    )
    if hub_snapshot_id is not None:
        fetch_note += f"; hub_snapshot_id={hub_snapshot_id}"
    lines = [
        f"Observation analysis at {format_local_datetime(datetime.now().astimezone())} (local time)",
        f"Observation reports: {len(observations)} ({fetch_note})",
        f"Unique observations: {unique_observation_count(observations)}",
        (
            "Ground-truth simulator IDs are not used for grouping or correlation."
        ),
        (
            "Timestamps: association uses acoustic event_time_utc; "
            "Hub received_time_utc is ingestion only."
        ),
    ]
    if filter_status_lines:
        lines.extend(filter_status_lines)
    if include_observation_details:
        lines.append(
            "Observation reports (detail): each report listed separately."
        )
        candidates = candidate_groups(observations)
        if candidates:
            for index, (_, items) in enumerate(candidates, start=1):
                candidate_node_ids = sorted(
                    {
                        str(observation_fields(item).get("node_id", "unknown-node"))
                        for item in items
                    }
                )
                lines.append(
                    f"  Report R{index}: {len(items)} independent report(s), "
                    f"{len(candidate_node_ids)} Ear(s) ({', '.join(candidate_node_ids)})"
                )
                for item in items:
                    lines.append(f"    {format_observation(item)}")
        else:
            lines.append("  none")

    if include_site_context:
        observed_node_ids = sorted(
            {
                nid
                for item in observations
                for nid in [observation_fields(item).get("node_id")]
                if isinstance(nid, str)
            }
        )
        if observed_node_ids:
            lines.append(
                "Node installation metadata (mount height is Ear height, not source altitude):"
            )
            for node_id in observed_node_ids:
                lines.append(f"  {format_installation(node_id, nodes.get(node_id))}")
        distance_lines = report_site_distances(
            observations,
            nodes,
            max_association_distance_m=max_association_distance_m,
            allow_long_range_sim_ears=allow_long_range_sim_ears,
        )
        lines.extend(distance_lines)
        if allow_long_range_sim_ears and any(
            "SIM eligible" in line for line in distance_lines
        ):
            lines.append(
                "Note: SIM eligible = long-range SIM-EAR pair kept for analysis "
                f"(--allow-long-range-sim-ears); not an association decision. "
                f"Typical range remains {max_association_distance_m:g}m."
            )
        lines.extend(format_environmental_context_lines(environmental_context))
        if include_cross_ear_geometry:
            flat_observations = [observation_fields(item) for item in observations]
            geometry_report = analyze_cross_ear_geometry(
                flat_observations,
                nodes,
                environmental_context=environmental_context,
            )
            lines.extend(
                format_cross_ear_geometry_lines(
                    geometry_report,
                    detail=cross_ear_geometry_detail,
                    format_local_datetime=format_local_datetime,
                )
            )

    correlation: CorrelationResult | None = None
    if include_cross_ear_correlation:
        correlation = precomputed_correlation or run_cross_ear_correlation(
            observations,
            nodes,
            association_temporal_gap_seconds=association_temporal_gap_seconds,
            max_association_distance_m=max_association_distance_m,
            allow_long_range_sim_ears=allow_long_range_sim_ears,
            min_ears_for_event=min_ears_for_event,
            min_promote_score=min_promote_score,
        )
        lines.extend(
            format_assembled_observation_lines(
                correlation.observations, correlation
            )
        )
        lines.extend(format_correlation_lines(correlation))

    lines.append(
        f"Per-Ear activity episodes (not Events): {event_gap_seconds:g}s of silence "
        "after interval end closes an episode. This temporal grouping does not "
        "mean observations inside an episode share one source."
    )
    observations_by_ear: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for item in observations:
        node_id = observation_fields(item).get("node_id")
        if isinstance(node_id, str):
            observations_by_ear[node_id].append(item)
    episode_number = 0
    for node_id, ear_observations in sorted(observations_by_ear.items()):
        for episode, reason in proposed_event_groups(
            ear_observations, event_gap_seconds
        ):
            episode_number += 1
            episode_times = [observation_time(item) for item in episode]
            known_times = [item for item in episode_times if item is not None]
            start_text = (
                format_local_datetime(min(known_times)) if known_times else "unknown"
            )
            end_text = "unknown"
            if known_times:
                last_observation = max(
                    episode,
                    key=lambda item: (
                        observation_time(item)
                        or datetime.min.replace(tzinfo=UTC)
                    ),
                )
                last_time = observation_time(last_observation)
                duration_ms = observation_fields(last_observation).get("duration_ms", 0)
                if (
                    not isinstance(duration_ms, (int, float))
                    or isinstance(duration_ms, bool)
                    or not math.isfinite(duration_ms)
                ):
                    duration_ms = 0
                if last_time is not None:
                    end_text = format_local_datetime(
                        last_time + timedelta(milliseconds=max(duration_ms, 0))
                    )
            lines.append(
                f"  Ear activity episode A{episode_number} for {node_id}: "
                f"{len(episode)} report(s), "
                f"interval {start_text} to {end_text}; {reason}"
            )
            lines.extend(format_episode_observation_summaries(episode))
    lines.append(
        "Track is not implemented. Cross-EAR Event Candidates/Events above are "
        "experimental script-local analysis only (not Hub-persisted Events). "
        "Classification, bearing, time, site distances, and bearing geometry are "
        "analysis inputs; simulator ground truth is excluded. Weather remains "
        "informational only."
    )
    return lines


def observation_report_key(observation: dict[str, Any]) -> str | None:
    fields = observation_fields(observation)
    node_id = fields.get("node_id")
    sequence = fields.get("sequence_number")
    if isinstance(node_id, str) and isinstance(sequence, int):
        return f"{node_id}:{sequence}"
    oid = fields.get("observation_id")
    return oid if isinstance(oid, str) else None


def observation_diff(
    previous_ids: set[str],
    observations: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], set[str]]:
    known_ids = {
        key
        for observation in observations
        for key in [observation_report_key(observation)]
        if isinstance(key, str)
    }
    new_observations = [
        observation
        for observation in observations
        if (key := observation_report_key(observation)) is not None
        and key not in previous_ids
    ]
    return new_observations, known_ids


def parse_positive_interval(value: float) -> float:
    if not math.isfinite(value) or value <= 0:
        raise ValueError("must be finite and greater than zero")
    return value


def main() -> int:
    try:
        load_project_env()
    except ValueError as exc:
        print(f"Hub configuration failed: {exc}", file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(
        description=(
            "Summarize Ear observation reports, per-Ear activity episodes, and "
            "experimental script-local Cross-EAR Event Candidates/Events; "
            "use --details for the per-report listing. "
            "Ground-truth simulator IDs are not used for association."
        )
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)
    list_command = commands.add_parser("list", help="Show the current report once")
    watch_command = commands.add_parser(
        "watch", help="Re-fetch and report new observations"
    )
    for command in (list_command, watch_command):
        command.add_argument(
            "--event-gap-seconds",
            type=float,
            default=DEFAULT_EVENT_GAP_SECONDS,
            help=(
                "Silence after an observation interval that closes a time episode; "
                f"not a source-correlation threshold (default: {DEFAULT_EVENT_GAP_SECONDS:g})"
            ),
        )
        command.add_argument(
            "--max-association-distance-m",
            type=float,
            default=DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
            help=(
                "Cross-EAR association radius for site-distance eligibility and "
                "annotations "
                f"(default: {DEFAULT_MAX_ASSOCIATION_DISTANCE_M:g}; not a Hub product rule)"
            ),
        )
        command.add_argument(
            "--allow-long-range-sim-ears",
            action="store_true",
            help=(
                "Keep SIM-EAR↔SIM-EAR pairs eligible for association analysis even "
                "when site distance exceeds --max-association-distance-m "
                "(distances are still printed; also required for Cross-EAR "
                "correlation beyond the association radius)"
            ),
        )
        command.add_argument(
            "--association-temporal-gap-seconds",
            type=float,
            default=DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
            help=(
                "Maximum gap between Observation intervals still treated as "
                "temporally compatible for Cross-EAR association "
                f"(default: {DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS:g})"
            ),
        )
        command.add_argument(
            "--min-ears-for-event",
            type=int,
            default=DEFAULT_MIN_EARS_FOR_EVENT,
            help=(
                "Minimum distinct Ears required before Event promotion is considered "
                f"(default: {DEFAULT_MIN_EARS_FOR_EVENT}; never sufficient alone)"
            ),
        )
        command.add_argument(
            "--min-promote-score",
            type=int,
            default=DEFAULT_MIN_PROMOTE_SCORE,
            help=(
                "Minimum evidence score required to promote a Candidate to Event "
                f"(default: {DEFAULT_MIN_PROMOTE_SCORE})"
            ),
        )
        command.add_argument(
            "--no-cross-ear-correlation",
            action="store_true",
            help="Omit experimental Cross-EAR Candidate/Event correlation",
        )
        command.add_argument(
            "--details",
            action="store_true",
            help=(
                "Include the per-report listing (one line per MQTT observation report); "
                "default shows episode summaries only"
            ),
        )
        command.add_argument(
            "--cross-ear-geometry-detail",
            action="store_true",
            help=(
                "Print per-observation geographic bearings and pairwise ray "
                "intersection detail (diagnostic only; default is a compact summary)"
            ),
        )
        command.add_argument(
            "--no-cross-ear-geometry",
            action="store_true",
            help="Omit the Cross-Ear Bearing Geometry diagnostic section",
        )
    list_command.add_argument(
        "--node-id",
        action="append",
        default=[],
        metavar="NODE_ID",
        help=(
            "Filter reports by Node ID (repeatable; same as the web Observations filter). "
            "A single value is also sent to the hub API."
        ),
    )
    list_command.add_argument(
        "--observation-id",
        action="append",
        default=[],
        metavar="OBSERVATION_ID",
        help=(
            "Filter reports by Observation ID (repeatable; same as the web Observations filter)"
        ),
    )
    list_command.add_argument(
        "--json",
        action="store_true",
        help=(
            "Print the Hub observation list JSON envelope for the selected page "
            "(pagination + reports). Skips human text analysis. "
            "Pagination summary also goes to stderr."
        ),
    )
    list_command.add_argument(
        "--page",
        type=int,
        default=1,
        metavar="N",
        help="1-based page index (default: 1)",
    )
    list_command.add_argument(
        "--page-size",
        type=int,
        default=DEFAULT_OBSERVATION_PAGE_SIZE,
        metavar="N",
        help=(
            f"Reports per page (default: {DEFAULT_OBSERVATION_PAGE_SIZE}, "
            f"max: {MAX_OBSERVATION_PAGE_SIZE})"
        ),
    )
    list_command.add_argument(
        "--order",
        choices=("newest", "oldest"),
        default="newest",
        help="Page ordering by received_time_utc (default: newest)",
    )
    watch_command.add_argument(
        "--node-id",
        help="Optionally restrict watched observations to one Ear",
    )
    watch_command.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval (default: 15)",
    )
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    base_url, admin_token = validate_hub_connection(args, parser)
    watching = args.command == "watch"
    intervals = [
        ("event gap", args.event_gap_seconds),
        ("max association distance", args.max_association_distance_m),
        (
            "association temporal gap",
            args.association_temporal_gap_seconds,
        ),
    ]
    if watching:
        intervals.append(("watch interval", args.interval_seconds))
    for label, value in intervals:
        try:
            parse_positive_interval(value)
        except ValueError as exc:
            parser.error(f"{label} {exc}")
    if args.min_ears_for_event < 2:
        parser.error("min-ears-for-event must be at least 2")
    if args.min_promote_score < 1:
        parser.error("min-promote-score must be at least 1")
    try:
        list_node_ids: set[str] = set()
        list_observation_ids: set[str] = set()
        watch_node_id: str | None = None
        page = 1
        page_size = MAX_OBSERVATION_PAGE_SIZE
        order = "oldest"
        single_page_json = False
        if watching:
            watch_node_id = args.node_id
            fetch_node_id = watch_node_id
            fetch_observation_id = None
        else:
            list_node_ids = {item for item in args.node_id if item}
            list_observation_ids = {
                item for item in args.observation_id if item
            }
            fetch_node_id = (
                next(iter(list_node_ids)) if len(list_node_ids) == 1 else None
            )
            fetch_observation_id = (
                next(iter(list_observation_ids))
                if len(list_observation_ids) == 1
                else None
            )
            try:
                page = parse_positive_int(args.page, label="page")
                page_size = parse_positive_int(args.page_size, label="page-size")
            except ValueError as exc:
                parser.error(str(exc))
            if page_size > MAX_OBSERVATION_PAGE_SIZE:
                parser.error(
                    f"page-size must be <= {MAX_OBSERVATION_PAGE_SIZE}"
                )
            order = args.order
            single_page_json = bool(getattr(args, "json", False))

        filter_status: list[str] = []
        if single_page_json:
            # JSON dump remains a single-page Hub envelope for API debugging.
            fetched, page_meta, envelope = fetch_observations(
                base_url,
                args.ca,
                admin_token,
                fetch_node_id,
                observation_id=fetch_observation_id,
                page=page,
                page_size=page_size,
                order=order,
            )
            observations = filter_observations(
                fetched,
                node_ids=list_node_ids,
                observation_ids=list_observation_ids,
            )
            if len(observations) == len(fetched):
                print(format_observation_envelope_json(envelope))
            else:
                print(
                    format_observation_envelope_json(
                        {
                            "pagination": {
                                "total_count": page_meta["total_count"],
                                "page": page_meta["page"],
                                "page_size": page_meta["page_size"],
                                "total_pages": page_meta["total_pages"],
                                "order": page_meta["order"],
                                "snapshot_id": page_meta.get("snapshot_id"),
                            },
                            "reports": observations,
                        }
                    )
                )
            return 0

        # B2.7 Phase A: analysis always uses a complete multi-page snapshot.
        fetched, fetch_meta = fetch_all_observation_reports(
            base_url,
            args.ca,
            admin_token,
            fetch_node_id,
            observation_id=fetch_observation_id,
            page_size=MAX_OBSERVATION_PAGE_SIZE,
            order="oldest",
        )
        total_count = int(fetch_meta.get("total_count") or len(fetched))
        print(
            format_pagination_summary(
                total_count=total_count,
                page=1,
                page_size=MAX_OBSERVATION_PAGE_SIZE,
                total_pages=int(fetch_meta.get("total_pages") or 1),
                order="oldest",
            )
            + f"; complete_snapshot_reports={len(fetched)}"
            + (
                f"; hub_snapshot_id={fetch_meta.get('snapshot_id')}"
                if fetch_meta.get("snapshot_id") is not None
                else ""
            ),
            file=sys.stderr,
        )
        if watching:
            observations = fetched
        else:
            observations = filter_observations(
                fetched,
                node_ids=list_node_ids,
                observation_ids=list_observation_ids,
            )
            filter_status = format_filter_status(
                node_ids=list_node_ids,
                observation_ids=list_observation_ids,
                fetched_count=len(fetched),
                filtered_count=len(observations),
            )
        nodes = fetch_nodes(base_url, args.ca, admin_token)
        reference_times = [
            observation_time(item)
            for item in observations
            if observation_time(item) is not None
        ]
        weather_at = max(reference_times) if reference_times else None
        weather = fetch_environmental_context(
            base_url,
            args.ca,
            admin_token,
            at=weather_at,
            ensure=True,
        )
        analysis_config = AnalysisConfig(
            event_gap_seconds=args.event_gap_seconds,
            max_association_distance_m=args.max_association_distance_m,
            allow_long_range_sim_ears=args.allow_long_range_sim_ears,
            association_temporal_gap_seconds=args.association_temporal_gap_seconds,
            min_ears_for_event=args.min_ears_for_event,
            min_promote_score=args.min_promote_score,
            include_cross_ear_correlation=not args.no_cross_ear_correlation,
            include_cross_ear_geometry=not args.no_cross_ear_geometry,
            cross_ear_geometry_detail=args.cross_ear_geometry_detail,
            include_site_context=True,
            include_observation_details=args.details,
        )
        snapshot = AnalysisSnapshot(
            reports=observations,
            nodes=nodes,
            config=analysis_config,
            snapshot_id=(
                int(fetch_meta["snapshot_id"])
                if fetch_meta.get("snapshot_id") is not None
                else None
            ),
            environmental_context=weather,
            filter_status_lines=filter_status,
            fetch_meta=fetch_meta,
        )
        result = analyze(snapshot)
        for line in result.lines:
            print(line)
        known_ids = {
            key
            for observation in observations
            for key in [observation_report_key(observation)]
            if isinstance(key, str)
        }
        if not watching:
            return 0

        count = fetch_observation_count(
            base_url, args.ca, admin_token, watch_node_id
        )
        last_total = count["total"]
        print(
            f"Watching for new observations every {args.interval_seconds:g}s "
            f"(count probe, then complete-snapshot reanalyze on change); "
            f"press Ctrl+C to stop."
        )
        while True:
            time.sleep(args.interval_seconds)
            count = fetch_observation_count(
                base_url, args.ca, admin_token, watch_node_id
            )
            if count["total"] == last_total:
                continue
            last_total = count["total"]
            current, current_meta = fetch_all_observation_reports(
                base_url,
                args.ca,
                admin_token,
                watch_node_id,
                page_size=MAX_OBSERVATION_PAGE_SIZE,
                order="oldest",
            )
            new_observations, known_ids = observation_diff(known_ids, current)
            if not new_observations:
                continue
            print(
                f"Reanalyzing complete snapshot after "
                f"{len(new_observations)} new report key(s) "
                f"({len(current)} reports in snapshot):",
                flush=True,
            )
            nodes = fetch_nodes(base_url, args.ca, admin_token)
            reference_times = [
                observation_time(item)
                for item in current
                if observation_time(item) is not None
            ]
            weather_at = max(reference_times) if reference_times else None
            weather = fetch_environmental_context(
                base_url,
                args.ca,
                admin_token,
                at=weather_at,
                ensure=True,
            )
            watch_config = AnalysisConfig(
                event_gap_seconds=args.event_gap_seconds,
                max_association_distance_m=args.max_association_distance_m,
                allow_long_range_sim_ears=args.allow_long_range_sim_ears,
                association_temporal_gap_seconds=args.association_temporal_gap_seconds,
                min_ears_for_event=args.min_ears_for_event,
                min_promote_score=args.min_promote_score,
                include_cross_ear_correlation=not args.no_cross_ear_correlation,
                include_cross_ear_geometry=not args.no_cross_ear_geometry,
                cross_ear_geometry_detail=args.cross_ear_geometry_detail,
                include_site_context=True,
                include_observation_details=args.details,
            )
            watch_snapshot = AnalysisSnapshot(
                reports=current,
                nodes=nodes,
                config=watch_config,
                snapshot_id=(
                    int(current_meta["snapshot_id"])
                    if current_meta.get("snapshot_id") is not None
                    else None
                ),
                environmental_context=weather,
                fetch_meta=current_meta,
            )
            watch_result = analyze(watch_snapshot)
            for line in watch_result.lines:
                print(line, flush=True)
    except KeyboardInterrupt:
        print("Stopped watching.")
        return 0
    except (OSError, ValueError, HubApiError) as exc:
        print(f"Hub event/track report failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
