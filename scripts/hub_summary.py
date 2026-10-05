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
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class HubApiError(RuntimeError):
    pass


def api_get(base_url: str, ca_file: str, admin_token: str, path: str) -> Any:
    request = urllib.request.Request(
        f"{base_url}{path}",
        headers={"Authorization": f"Bearer {admin_token}"},
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read(1024).decode(errors="replace")
        raise HubApiError(f"hub API returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise HubApiError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubApiError(f"hub returned invalid JSON for {path}") from exc


def fetch_snapshot(base_url: str, ca_file: str, admin_token: str) -> dict[str, Any]:
    nodes = api_get(base_url, ca_file, admin_token, "/api/v1/nodes")
    observations = api_get(
        base_url,
        ca_file,
        admin_token,
        "/api/v1/observations?" + urllib.parse.urlencode({"limit": 500}),
    )
    if not isinstance(nodes, list) or not all(isinstance(node, dict) for node in nodes):
        raise HubApiError("hub returned an invalid node list")
    if not isinstance(observations, list) or not all(
        isinstance(observation, dict) for observation in observations
    ):
        raise HubApiError("hub returned an invalid observation list")
    return {"nodes": nodes, "observations": observations}


def summarize(snapshot: dict[str, Any]) -> dict[str, Any]:
    nodes = snapshot["nodes"]
    observations = snapshot["observations"]
    node_types = Counter(node.get("node_type", "unknown") for node in nodes)
    lifecycle_states = Counter(node.get("lifecycle_state", "active") for node in nodes)
    node_states = Counter(node.get("availability", "unknown") for node in nodes)
    observations_by_node = Counter(
        observation.get("node_id", "unknown") for observation in observations
    )
    observations_by_source = Counter(
        _simulated_source_id(observation) or "unlabelled"
        for observation in observations
    )
    latest_received = max(
        (observation.get("received_time_utc", "") for observation in observations),
        default=None,
    )
    return {
        "node_count": len(nodes),
        "nodes_by_type": dict(sorted(node_types.items())),
        "nodes_by_lifecycle_state": dict(sorted(lifecycle_states.items())),
        "nodes_by_availability": dict(sorted(node_states.items())),
        "observation_count": len(observations),
        "observations_by_node": dict(sorted(observations_by_node.items())),
        "observations_by_simulated_source": dict(sorted(observations_by_source.items())),
        "latest_received_time_utc": latest_received,
        "observation_limit": 500,
    }


def node_signature(node: dict[str, Any]) -> tuple[Any, ...]:
    return (
        node.get("node_type"),
        node.get("hardware_revision"),
        node.get("provisioning_state"),
        node.get("lifecycle_state", "active"),
        node.get("availability"),
        node.get("pending_configuration_change"),
        node.get("desired"),
        node.get("reported", {}).get("firmware_version"),
        node.get("reported", {}).get("config_version"),
        node.get("reported", {}).get("calibration_version"),
        node.get("reported", {}).get("classifier_version"),
    )


def _simulated_source_id(observation: dict[str, Any]) -> str | None:
    classification = observation.get("classification")
    if not isinstance(classification, dict):
        return None
    hints = classification.get("hints")
    if not isinstance(hints, dict):
        return None
    source_id = hints.get("simulated_source_id")
    return source_id if isinstance(source_id, str) else None


def snapshot_diff(previous: dict[str, Any], current: dict[str, Any]) -> list[str]:
    changes = []
    old_nodes = {node["node_id"]: node for node in previous["nodes"]}
    new_nodes = {node["node_id"]: node for node in current["nodes"]}
    for node_id in sorted(new_nodes.keys() - old_nodes.keys()):
        changes.append(f"New node: {node_id} ({new_nodes[node_id].get('node_type', 'unknown')})")
    for node_id in sorted(old_nodes.keys() - new_nodes.keys()):
        changes.append(f"Node missing from current response: {node_id}")
    for node_id in sorted(old_nodes.keys() & new_nodes.keys()):
        if node_signature(old_nodes[node_id]) != node_signature(new_nodes[node_id]):
            changes.append(f"Node state changed: {node_id}")

    old_observation_ids = {
        observation["observation_id"]
        for observation in previous["observations"]
        if "observation_id" in observation
    }
    new_observations = [
        observation
        for observation in current["observations"]
        if observation.get("observation_id") not in old_observation_ids
    ]
    if new_observations:
        by_node = Counter(observation.get("node_id", "unknown") for observation in new_observations)
        summary = ", ".join(f"{node_id}: +{count}" for node_id, count in sorted(by_node.items()))
        changes.append(f"New observations: {len(new_observations)} ({summary})")
        for observation in new_observations[:5]:
            source_id = _simulated_source_id(observation)
            source = f", source={source_id}" if source_id else ""
            changes.append(
                f"  {observation.get('observation_id')} from "
                f"{observation.get('node_id', 'unknown')}{source}"
            )
        if len(new_observations) > 5:
            changes.append(f"  ... and {len(new_observations) - 5} more")
    return changes


def parse_watch_interval(interval_seconds: float) -> float:
    if interval_seconds <= 0:
        raise ValueError("watch interval must be greater than zero")
    return interval_seconds


def print_summary(snapshot: dict[str, Any]) -> None:
    summary = summarize(snapshot)
    print(f"Hub summary at {datetime.now(timezone.utc).isoformat()}")
    print(f"Nodes: {summary['node_count']} ({summary['nodes_by_type'] or 'none'})")
    print(f"Lifecycle: {summary['nodes_by_lifecycle_state'] or 'none'}")
    print(f"Availability: {summary['nodes_by_availability'] or 'none'}")
    print(
        f"Observations in latest {summary['observation_limit']}: "
        f"{summary['observation_count']}"
    )
    print(f"Observations by node: {summary['observations_by_node'] or 'none'}")
    print(f"Simulated sources: {summary['observations_by_simulated_source'] or 'none'}")
    print(f"Latest received: {summary['latest_received_time_utc'] or 'none'}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize node and observation state from the authenticated hub API."
    )
    parser.add_argument("--hub-host", required=True, help="Hub LAN IP or certificate-valid hostname")
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument("--watch", action="store_true", help="Poll repeatedly and report differences")
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval when --watch is set (default: 15)",
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
        previous = fetch_snapshot(base_url, args.ca, admin_token)
        print_summary(previous)
        if not args.watch:
            return 0
        print("Watching for changes; press Ctrl+C to stop.")
        while True:
            time.sleep(args.interval_seconds)
            current = fetch_snapshot(base_url, args.ca, admin_token)
            changes = snapshot_diff(previous, current)
            if changes:
                print(f"\nChanges at {datetime.now(timezone.utc).isoformat()}")
                for change in changes:
                    print(change)
            else:
                print(f"No changes at {datetime.now(timezone.utc).isoformat()}")
            previous = current
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    except (OSError, HubApiError) as exc:
        print(f"Hub summary failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
