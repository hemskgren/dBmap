#!/usr/bin/env python3
import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class HubApiError(RuntimeError):
    pass


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
        raise HubApiError(f"hub API returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError("hub returned invalid observation JSON") from exc

    if not isinstance(observations, list) or not all(
        isinstance(observation, dict) for observation in observations
    ):
        raise HubApiError("hub returned an invalid observation list")
    return observations


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


def report(observations: list[dict[str, Any]]) -> list[str]:
    groups, ungrouped = group_by_simulated_source(observations)
    lines = [
        f"Observation/source report at {datetime.now(timezone.utc).isoformat()}",
        f"Observations fetched (maximum 500): {len(observations)}",
        "Synthetic-source groups (test correlation hints only; not events or tracks):",
    ]
    if groups:
        for source_id, items in groups.items():
            nodes = sorted({str(item.get("node_id", "unknown-node")) for item in items})
            lines.append(
                f"  {source_id}: {len(items)} observations across {len(nodes)} Ear(s) "
                f"({', '.join(nodes)})"
            )
            for item in reversed(items):
                lines.append(f"    {format_observation(item)}")
    else:
        lines.append("  none")

    if ungrouped:
        lines.append(f"Observations without a simulator source hint: {len(ungrouped)}")
        for item in reversed(ungrouped):
            lines.append(f"  {format_observation(item)}")
    lines.append(
        "No event/track identity or geographic source position is inferred; "
        "bearing is node-relative."
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


def parse_watch_interval(interval_seconds: float) -> float:
    if interval_seconds <= 0:
        raise ValueError("watch interval must be greater than zero")
    return interval_seconds


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Group simulated Ear observations by their test-only source hint; "
            "this does not create event or track identities."
        )
    )
    parser.add_argument("--hub-host", required=True, help="Hub LAN IP or certificate-valid hostname")
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument("--node-id", help="Optionally restrict observations to one Ear")
    parser.add_argument("--watch", action="store_true", help="Print newly received observations")
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval with --watch (default: 15)",
    )
    args = parser.parse_args()
    try:
        parse_watch_interval(args.interval_seconds)
    except ValueError as exc:
        parser.error(str(exc))
    admin_token = os.environ.get("DBMAP_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        parser.error("DBMAP_ADMIN_TOKEN must be loaded from the private .env file")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")

    base_url = f"https://{args.hub_host}:8443"
    try:
        observations = fetch_observations(base_url, args.ca, admin_token, args.node_id)
        for line in report(observations):
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
            print(f"New observations at {datetime.now(timezone.utc).isoformat()}:")
            for observation in reversed(new_observations):
                source_id = simulated_source_id(observation)
                source = f" [simulated source hint: {source_id}]" if source_id else ""
                print(f"  {format_observation(observation)}{source}")
    except KeyboardInterrupt:
        print("Stopped watching.")
        return 0
    except (OSError, ValueError, HubApiError) as exc:
        print(f"Hub event/track report failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
