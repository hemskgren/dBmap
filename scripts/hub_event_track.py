#!/usr/bin/env python3
import argparse
from itertools import combinations
import json
import math
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


EARTH_RADIUS_M = 6_371_000
LOCAL_SOURCE_FAMILIES = {"vehicle", "animal"}
AIRBORNE_SOURCE_FAMILIES = {"aircraft", "drone"}


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
        detail = exc.read(1024).decode(errors="replace")
        raise HubApiError(
            f"hub API {path} returned HTTP {exc.code}: {detail}"
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
        detail = exc.read(1024).decode(errors="replace")
        raise HubApiError(
            f"hub API {path} returned HTTP {exc.code}: {detail}"
        ) from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid node JSON") from exc

    if not isinstance(nodes, list) or not all(
        isinstance(node, dict)
        and isinstance(node.get("node_id"), str)
        and (node.get("installation") is None or isinstance(node.get("installation"), dict))
        for node in nodes
    ):
        raise HubApiError("hub returned an invalid node list or installation metadata")
    return {node["node_id"]: node for node in nodes}


def simulated_source_id(observation: dict[str, Any]) -> str | None:
    classification = observation.get("classification")
    if not isinstance(classification, dict):
        return None
    hints = classification.get("hints")
    if not isinstance(hints, dict):
        return None
    source_id = hints.get("simulated_source_id")
    return source_id if isinstance(source_id, str) and source_id else None


def group_by_simulated_source(
    observations: list[dict[str, Any]],
) -> tuple[dict[str, list[dict[str, Any]]], list[dict[str, Any]]]:
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    ungrouped = []
    for observation in observations:
        source_id = simulated_source_id(observation)
        if source_id is None:
            ungrouped.append(observation)
        else:
            grouped[source_id].append(observation)
    return dict(sorted(grouped.items())), ungrouped


def observation_time(observation: dict[str, Any]) -> datetime | None:
    value = observation.get("event_time_utc")
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(timezone.utc)


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
    groups, ungrouped = group_by_simulated_source(observations)
    candidates = [
        (source_id, sort_observations(items))
        for source_id, items in groups.items()
    ]
    candidates.extend((None, [item]) for item in sort_observations(ungrouped))
    return candidates


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
            suffix = "m" if key == "height_m" else (" deg" if key == "orientation_deg" else "")
            fields.append(f"{label}={value}{suffix}")
    return f"{node_id}: " + ", ".join(fields)


def source_family_assessment(observations: list[dict[str, Any]]) -> str:
    families = {
        classification.get("source_family")
        for item in observations
        if isinstance((classification := item.get("classification")), dict)
        and isinstance(classification.get("source_family"), str)
    }
    if families and families <= LOCAL_SOURCE_FAMILIES:
        return "local-ground"
    if families and families <= AIRBORNE_SOURCE_FAMILIES:
        return "airborne"
    return "unknown-or-mixed"


def assess_candidate_installations(
    observations: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]],
    local_radius_m: float,
) -> list[str]:
    node_ids = sorted(
        {
            item["node_id"]
            for item in observations
            if isinstance(item.get("node_id"), str)
        }
    )
    if len(node_ids) < 2:
        return []

    category = source_family_assessment(observations)
    lines = [f"    spatial assessment ({category}; local radius {local_radius_m:g}m):"]
    for first_id, second_id in combinations(node_ids, 2):
        first_node = nodes.get(first_id)
        second_node = nodes.get(second_id)
        first_installation = first_node.get("installation") if first_node else None
        second_installation = second_node.get("installation") if second_node else None
        if not isinstance(first_installation, dict) or not isinstance(second_installation, dict):
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
            lines.append(f"      {first_id} ↔ {second_id}: invalid installation coordinates")
            continue
        if category == "local-ground" and distance > local_radius_m:
            assessment = (
                "beyond local radius; one shared local ground source is less plausible"
            )
        elif category == "airborne" and distance > local_radius_m:
            assessment = "beyond local radius; distance alone does not exclude an airborne source"
        elif distance <= local_radius_m:
            assessment = "within local radius; spatially plausible, not proof of identity"
        else:
            assessment = "beyond local radius; source-family assessment is unknown or mixed"
        lines.append(f"      {first_id} ↔ {second_id}: {distance:.0f}m — {assessment}")
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
        duration_ms = observation.get("duration_ms", 0)
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
    observation_id = observation.get("observation_id", "unknown-id")
    node_id = observation.get("node_id", "unknown-node")
    event_time = observation.get("event_time_utc", "unknown-time")
    details = []
    classification = observation.get("classification")
    if isinstance(classification, dict):
        source_family = classification.get("source_family")
        confidence = classification.get("confidence")
        if isinstance(source_family, str):
            classification_detail = f"classification={source_family}"
            if isinstance(confidence, (int, float)) and not isinstance(confidence, bool):
                classification_detail += f" (confidence={confidence:g})"
            details.append(classification_detail)

    bearing = observation.get("bearing")
    if isinstance(bearing, dict) and isinstance(bearing.get("deg"), (int, float)):
        bearing_detail = f"node-relative bearing={bearing['deg']} deg"
        confidence = bearing.get("confidence")
        if isinstance(confidence, (int, float)) and not isinstance(confidence, bool):
            bearing_detail += f" (confidence={confidence:g})"
        details.append(bearing_detail)

    signal = observation.get("signal_level_dbfs")
    if isinstance(signal, (int, float)):
        details.append(f"level={signal} dBFS")
    suffix = f"; {', '.join(details)}" if details else ""
    return f"{event_time}: {observation_id} from {node_id}{suffix}"


def report(
    observations: list[dict[str, Any]],
    event_gap_seconds: float = 15,
    nodes: dict[str, dict[str, Any]] | None = None,
    local_radius_m: float = 500,
) -> list[str]:
    parse_positive_interval(event_gap_seconds)
    parse_positive_interval(local_radius_m)
    nodes = nodes or {}
    lines = [
        f"Observation analysis at {datetime.now(timezone.utc).isoformat()}",
        f"Observations fetched (maximum 500): {len(observations)}",
        "Candidate grouping: shared simulator source hint only; unhinted observations stay separate.",
    ]
    candidates = candidate_groups(observations)
    observed_node_ids = sorted(
        {
            item["node_id"]
            for item in observations
            if isinstance(item.get("node_id"), str)
        }
    )
    if observed_node_ids:
        lines.append("Node installation metadata (mount height is Ear height, not source altitude):")
        for node_id in observed_node_ids:
            lines.append(f"  {format_installation(node_id, nodes.get(node_id))}")
    if candidates:
        for index, (source_id, items) in enumerate(candidates, start=1):
            candidate_node_ids = sorted(
                {str(item.get("node_id", "unknown-node")) for item in items}
            )
            label = f"simulator hint {source_id}" if source_id else "no simulator hint"
            lines.append(
                f"  Candidate C{index}: {label}; {len(items)} observation(s), "
                f"{len(candidate_node_ids)} Ear(s) ({', '.join(candidate_node_ids)})"
            )
            for item in items:
                lines.append(f"    {format_observation(item)}")
            lines.extend(assess_candidate_installations(items, nodes, local_radius_m))
    else:
        lines.append("  none")

    lines.append(
        f"Event-shaped inactivity episodes (not event correlation or persisted Events): "
        f"{event_gap_seconds:g}s of silence after interval end closes an episode. "
        "This is only a temporal boundary; it does not mean observations inside "
        "an episode share one source."
    )
    proposal_number = 0
    for candidate_number, (_, items) in enumerate(candidates, start=1):
        proposals = proposed_event_groups(items, event_gap_seconds)
        for candidate_events, reason in proposals:
            proposal_number += 1
            event_times = [observation_time(item) for item in candidate_events]
            known_times = [item for item in event_times if item is not None]
            start_text = min(known_times).isoformat() if known_times else "unknown"
            end_text = "unknown"
            if known_times:
                last_observation = max(
                    candidate_events,
                    key=lambda item: observation_time(item)
                    or datetime.min.replace(tzinfo=timezone.utc),
                )
                last_time = observation_time(last_observation)
                duration_ms = last_observation.get("duration_ms", 0)
                if (
                    not isinstance(duration_ms, (int, float))
                    or isinstance(duration_ms, bool)
                    or not math.isfinite(duration_ms)
                ):
                    duration_ms = 0
                if last_time is not None:
                    end_text = (
                        last_time + timedelta(milliseconds=max(duration_ms, 0))
                    ).isoformat()
            event_node_ids = sorted(
                {str(item.get("node_id", "unknown-node")) for item in candidate_events}
            )
            observation_ids = [
                str(item.get("observation_id", "unknown-id")) for item in candidate_events
            ]
            lines.append(
                f"  Event proposal E{proposal_number} from Candidate C{candidate_number}: "
                f"{len(candidate_events)} observation(s), Ear(s) {', '.join(event_node_ids)}, "
                f"interval {start_text} to {end_text}; {reason}"
            )
            lines.append(f"    observations: {', '.join(observation_ids)}")
    lines.append(
        "These are exploratory groupings, not persistent Events or Tracks; "
        "the simulator hint is test metadata and bearings are node-relative."
    )
    return lines


def observation_diff(
    previous_ids: set[str],
    observations: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], set[str]]:
    known_ids = {
        observation["observation_id"]
        for observation in observations
        if isinstance(observation.get("observation_id"), str)
    }
    new_observations = [
        observation
        for observation in observations
        if isinstance(observation.get("observation_id"), str)
        and observation["observation_id"] not in previous_ids
    ]
    return new_observations, known_ids


def parse_positive_interval(value: float) -> float:
    if not math.isfinite(value) or value <= 0:
        raise ValueError("must be finite and greater than zero")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Explore candidate groups and print Event proposals from Ear observations; "
            "nothing is created or persisted."
        )
    )
    parser.add_argument("--hub-host", required=True, help="Hub LAN IP or certificate-valid hostname")
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument("--node-id", help="Optionally restrict observations to one Ear")
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Re-fetch observations and reprint the analysis when new records arrive",
    )
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval with --watch (default: 15)",
    )
    parser.add_argument(
        "--event-gap-seconds",
        type=float,
        default=15,
        help=(
            "Silence after an observation interval that closes a time episode; "
            "not a source-correlation threshold (default: 15)"
        ),
    )
    parser.add_argument(
        "--local-source-radius-m",
        type=float,
        default=500,
        help="Distance beyond which a shared local ground source is less plausible (default: 500)",
    )
    args = parser.parse_args()
    for label, value in (
        ("watch interval", args.interval_seconds),
        ("event gap", args.event_gap_seconds),
        ("local source radius", args.local_source_radius_m),
    ):
        try:
            parse_positive_interval(value)
        except ValueError as exc:
            parser.error(f"{label} {exc}")
    admin_token = os.environ.get("DBMAP_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        parser.error("DBMAP_ADMIN_TOKEN must be loaded from the private .env file")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")

    base_url = f"https://{args.hub_host}:8443"
    try:
        observations = fetch_observations(base_url, args.ca, admin_token, args.node_id)
        nodes = fetch_nodes(base_url, args.ca, admin_token)
        for line in report(
            observations,
            args.event_gap_seconds,
            nodes,
            args.local_source_radius_m,
        ):
            print(line)
        known_ids = {
            observation["observation_id"]
            for observation in observations
            if isinstance(observation.get("observation_id"), str)
        }
        if not args.watch:
            return 0

        print(f"Watching for new observations every {args.interval_seconds:g}s; press Ctrl+C to stop.")
        while True:
            time.sleep(args.interval_seconds)
            current = fetch_observations(base_url, args.ca, admin_token, args.node_id)
            new_observations, known_ids = observation_diff(known_ids, current)
            if not new_observations:
                continue
            nodes = fetch_nodes(base_url, args.ca, admin_token)
            print("Reanalyzing the current fetched observation set after new records:")
            for line in report(
                current,
                args.event_gap_seconds,
                nodes,
                args.local_source_radius_m,
            ):
                print(line)
    except KeyboardInterrupt:
        print("Stopped watching.")
        return 0
    except (OSError, ValueError, HubApiError) as exc:
        print(f"Hub event/track report failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
