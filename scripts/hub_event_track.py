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
from datetime import datetime, timedelta, timezone
from itertools import combinations
from typing import Any

from lib.cross_ear_bearing_geometry import (
    analyze_cross_ear_geometry,
    format_cross_ear_geometry_lines,
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


def is_sim_ear(node_id: str) -> bool:
    return node_id.startswith("SIM-EAR-")


class HubApiError(RuntimeError):
    pass


def admin_request(url: str, admin_token: str) -> urllib.request.Request:
    return urllib.request.Request(
        url,
        headers={"Authorization": f"Bearer {admin_token}"},
    )


def fetch_observations(
    base_url: str,
    ca_file: str,
    admin_token: str,
    node_id: str | None = None,
) -> list[dict[str, Any]]:
    query: dict[str, str | int] = {"limit": 500}
    if node_id is not None:
        query["node_id"] = node_id
    path = "/api/v1/observations?" + urllib.parse.urlencode(query)
    request = urllib.request.Request(
        f"{base_url}{path}",
        headers={"Authorization": f"Bearer {admin_token}"},
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            observations = json.load(response)
    except urllib.error.HTTPError as exc:
        raise HubApiError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid observation JSON") from exc

    if not isinstance(observations, list) or not all(
        isinstance(observation, dict) for observation in observations
    ):
        raise HubApiError("hub returned an invalid observation list")
    return observations


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
        query["at"] = at.astimezone(timezone.utc).isoformat()
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
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(timezone.utc)


def format_local_datetime(value: datetime | None) -> str:
    if value is None:
        return "unknown"
    return value.astimezone().strftime("%d/%m/%Y %H:%M:%S %Z")


def sort_observations(observations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(
        observations,
        key=lambda item: (
            observation_time(item) is None,
            observation_time(item) or datetime.max.replace(tzinfo=timezone.utc),
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
    for earlier, later in zip(uniq, uniq[1:]):
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


def report(
    observations: list[dict[str, Any]],
    event_gap_seconds: float = DEFAULT_EVENT_GAP_SECONDS,
    nodes: dict[str, dict[str, Any]] | None = None,
    *,
    max_association_distance_m: float = DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
    allow_long_range_sim_ears: bool = False,
    environmental_context: dict[str, Any] | None = None,
    include_site_context: bool = True,
    include_cross_ear_geometry: bool = True,
    cross_ear_geometry_detail: bool = False,
    include_observation_details: bool = False,
    filter_status_lines: list[str] | None = None,
) -> list[str]:
    parse_positive_interval(event_gap_seconds)
    parse_positive_interval(max_association_distance_m)
    nodes = nodes or {}
    lines = [
        f"Observation analysis at {format_local_datetime(datetime.now().astimezone())} (local time)",
        f"Observation reports: {len(observations)} (fetched maximum 500)",
        f"Unique observations: {unique_observation_count(observations)}",
        (
            "Ground-truth simulator IDs are not used for grouping or correlation."
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
            geometry_report = analyze_cross_ear_geometry(
                observations,
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
                        or datetime.min.replace(tzinfo=timezone.utc)
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
        "No Event or Track association is attempted yet. Classification, bearing, "
        "time, site distances, and cross-Ear bearing geometry are analysis inputs; "
        "simulator ground truth is excluded. Weather remains informational only."
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
            "Summarize Ear observation reports and per-Ear activity episodes; "
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
                "Typical cross-Ear association range used only to annotate site "
                f"distances (default: {DEFAULT_MAX_ASSOCIATION_DISTANCE_M:g})"
            ),
        )
        command.add_argument(
            "--allow-long-range-sim-ears",
            action="store_true",
            help=(
                "Keep SIM-EAR↔SIM-EAR pairs eligible for association analysis even "
                "when site distance exceeds --max-association-distance-m "
                "(distances are still printed)"
            ),
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
            "Print filtered observation reports as pretty-printed JSON "
            "(all matching MQTT payloads; for AI/report feeds). "
            "Skips the human text analysis."
        ),
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
    ]
    if watching:
        intervals.append(("watch interval", args.interval_seconds))
    for label, value in intervals:
        try:
            parse_positive_interval(value)
        except ValueError as exc:
            parser.error(f"{label} {exc}")
    try:
        list_node_ids: set[str] = set()
        list_observation_ids: set[str] = set()
        watch_node_id: str | None = None
        if watching:
            watch_node_id = args.node_id
            fetch_node_id = watch_node_id
        else:
            list_node_ids = {item for item in args.node_id if item}
            list_observation_ids = {
                item for item in args.observation_id if item
            }
            fetch_node_id = (
                next(iter(list_node_ids)) if len(list_node_ids) == 1 else None
            )

        fetched = fetch_observations(base_url, args.ca, admin_token, fetch_node_id)
        filter_status: list[str] = []
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
            if getattr(args, "json", False):
                print(format_observations_json(observations))
                return 0
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
        for line in report(
            observations,
            args.event_gap_seconds,
            nodes,
            max_association_distance_m=args.max_association_distance_m,
            allow_long_range_sim_ears=args.allow_long_range_sim_ears,
            environmental_context=weather,
            include_cross_ear_geometry=not args.no_cross_ear_geometry,
            cross_ear_geometry_detail=args.cross_ear_geometry_detail,
            include_observation_details=args.details,
            filter_status_lines=filter_status,
        ):
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
            f"(count probe, then full fetch on change); press Ctrl+C to stop."
        )
        while True:
            time.sleep(args.interval_seconds)
            count = fetch_observation_count(
                base_url, args.ca, admin_token, watch_node_id
            )
            if count["total"] == last_total:
                continue
            last_total = count["total"]
            current = fetch_observations(
                base_url, args.ca, admin_token, watch_node_id
            )
            new_observations, known_ids = observation_diff(known_ids, current)
            if not new_observations:
                continue
            print(
                f"Reanalyzing after {len(new_observations)} new observation(s) "
                f"(site/weather context omitted in watch updates; use list for full context):",
                flush=True,
            )
            for line in report(
                current,
                args.event_gap_seconds,
                include_site_context=False,
                include_observation_details=args.details,
            ):
                print(line, flush=True)
    except KeyboardInterrupt:
        print("Stopped watching.")
        return 0
    except (OSError, ValueError, HubApiError) as exc:
        print(f"Hub event/track report failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
