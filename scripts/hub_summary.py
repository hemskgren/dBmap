#!/usr/bin/env python3
import argparse
import json
import math
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from datetime import datetime
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
    broker = api_get(
        base_url,
        ca_file,
        admin_token,
        "/api/v1/hub/components/mosquitto/details",
    )
    if not isinstance(nodes, list) or not all(isinstance(node, dict) for node in nodes):
        raise HubApiError("hub returned an invalid node list")
    if not isinstance(observations, list) or not all(
        isinstance(observation, dict) for observation in observations
    ):
        raise HubApiError("hub returned an invalid observation list")
    if not isinstance(broker, dict):
        raise HubApiError("hub returned invalid Mosquitto details")
    return {"nodes": nodes, "observations": observations, "broker": broker}


def fetch_status(base_url: str, ca_file: str, admin_token: str) -> dict[str, Any]:
    status = api_get(base_url, ca_file, admin_token, "/api/v1/hub/status")
    if not isinstance(status, dict):
        raise HubApiError("hub returned an invalid status response")
    components = status.get("components")
    if not isinstance(components, list) or not all(
        isinstance(component, dict) and "name" in component and "status" in component
        for component in components
    ):
        raise HubApiError("hub returned an invalid component status list")
    return status


def summarize(snapshot: dict[str, Any]) -> dict[str, Any]:
    nodes = snapshot["nodes"]
    observations = snapshot["observations"]
    node_types = Counter(node.get("node_type", "unknown") for node in nodes)
    lifecycle_states = Counter(node.get("lifecycle_state", "active") for node in nodes)
    node_states = Counter(node.get("availability", "unknown") for node in nodes)
    installation_completed_count = sum(
        isinstance(node.get("installation"), dict)
        and _valid_installation(node["installation"])
        for node in nodes
    )
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
    broker = snapshot.get("broker", {})
    return {
        "node_count": len(nodes),
        "nodes_by_type": dict(sorted(node_types.items())),
        "nodes_by_lifecycle_state": dict(sorted(lifecycle_states.items())),
        "nodes_by_availability": dict(sorted(node_states.items())),
        "installation_completed_count": installation_completed_count,
        "installation_incomplete_count": len(nodes) - installation_completed_count,
        "observation_count": len(observations),
        "observations_by_node": dict(sorted(observations_by_node.items())),
        "observations_by_simulated_source": dict(sorted(observations_by_source.items())),
        "latest_received_time_utc": latest_received,
        "observation_limit": 500,
        "broker": broker,
    }


def node_signature(node: dict[str, Any]) -> tuple[Any, ...]:
    return (
        node.get("node_type"),
        node.get("hardware_revision"),
        node.get("provisioning_state"),
        node.get("lifecycle_state", "active"),
        node.get("availability"),
        node.get("pending_configuration_change"),
        _installation_signature(node.get("installation")),
        node.get("desired"),
        node.get("reported", {}).get("firmware_version"),
        node.get("reported", {}).get("config_version"),
        node.get("reported", {}).get("calibration_version"),
        node.get("reported", {}).get("classifier_version"),
    )


def _valid_installation(installation: dict[str, Any]) -> bool:
    try:
        latitude = float(installation.get("latitude"))
        longitude = float(installation.get("longitude"))
    except (TypeError, ValueError):
        return False
    return (
        math.isfinite(latitude)
        and math.isfinite(longitude)
        and abs(latitude) <= 90
        and abs(longitude) <= 180
    )


def _installation_signature(installation: Any) -> tuple[Any, ...] | None:
    if not isinstance(installation, dict):
        return None
    fields = (
        "latitude",
        "longitude",
        "floor",
        "height_m",
        "height_accuracy_m",
        "mount_type",
        "environment",
        "orientation_deg",
    )
    return tuple(installation.get(field) for field in fields)


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

    old_broker = previous.get("broker", {})
    new_broker = current.get("broker", {})
    if any(
        old_broker.get(field) != new_broker.get(field)
        for field in ("connected", "connected_clients", "total_client_sessions")
    ):
        changes.append(
            "Mosquitto clients changed: "
            f"connected={old_broker.get('connected_clients', 'unknown')} -> "
            f"{new_broker.get('connected_clients', 'unknown')}, "
            f"sessions={old_broker.get('total_client_sessions', 'unknown')} -> "
            f"{new_broker.get('total_client_sessions', 'unknown')}"
        )

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


def component_states(status: dict[str, Any]) -> dict[str, str]:
    return {
        str(component["name"]): str(component["status"])
        for component in status["components"]
    }


def print_human_status(status: dict[str, Any]) -> None:
    print("Hub status")
    print(f"  Mode:   {status.get('mode') or 'Unknown'}")
    print(f"  Hub ID: {status.get('hub_id') or 'Not set'}")
    print("\nServices")
    for name, state in component_states(status).items():
        print(f"  • {name}: {state.replace('_', ' ').capitalize()}")

    summary = status.get("summary")
    if not isinstance(summary, dict):
        summary = {}
    print("\nDevices")
    print(f"  Registered: {summary.get('device_count', 'Not reported')}")
    print(f"  Online:     {summary.get('online_device_count', 'Not reported')}")
    for label, field in (
        ("By type", "devices_by_type"),
        ("Lifecycle", "devices_by_lifecycle"),
        ("Availability", "devices_by_availability"),
    ):
        counts = summary.get(field)
        print(f"  {label}:")
        if isinstance(counts, dict) and counts:
            for name, count in sorted(counts.items()):
                print(f"    • {name}: {count}")
        else:
            print("    • None")
    print("  Installation:")
    print(f"    • Completed: {summary.get('installation_completed_count', 'Not reported')}")
    print(
        "    • Missing, incomplete, or invalid: "
        f"{summary.get('installation_incomplete_count', 'Not reported')}"
    )


def status_changes(previous: dict[str, Any], current: dict[str, Any]) -> list[str]:
    old_states = component_states(previous)
    new_states = component_states(current)
    return [
        f"{name}: {old_states.get(name, 'not reported')} → {state}"
        for name, state in new_states.items()
        if old_states.get(name) != state
    ] + [
        f"{name}: {state} → not reported"
        for name, state in old_states.items()
        if name not in new_states
    ]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Show Hub service status from the authenticated Hub API."
    )
    parser.add_argument("--hub-host", required=True, help="Hub LAN IP or certificate-valid hostname")
    parser.add_argument("--hub-port", type=int, default=8443, help="HTTPS listener port (default: 8443)")
    parser.add_argument(
        "--base-path",
        default=os.environ.get("DBMAP_PUBLIC_BASE_PATH", ""),
        help="Optional public path prefix, for example /dbmap",
    )
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument(
        "--output",
        choices=("text", "json"),
        default="text",
        help="Output format (default: readable text; JSON is suitable for scripts)",
    )
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Poll repeatedly; JSON output is newline-delimited, one status object per poll",
    )
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Polling interval when --watch is set (default: 15)",
    )
    args = parser.parse_args()
    args.base_path = args.base_path.rstrip("/")
    if args.base_path and (
        not args.base_path.startswith("/")
        or "//" in args.base_path
        or any(part in {".", ".."} for part in args.base_path.split("/"))
    ):
        parser.error("--base-path must be empty or a normalized absolute path")
    try:
        parse_watch_interval(args.interval_seconds)
    except ValueError as exc:
        parser.error(str(exc))
    admin_token = os.environ.get("DBMAP_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        parser.error("DBMAP_ADMIN_TOKEN must be loaded from the private .env file")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")

    base_url = f"https://{args.hub_host}:{args.hub_port}{args.base_path}"
    try:
        previous = fetch_status(base_url, args.ca, admin_token)
        if args.output == "json":
            print(
                json.dumps(previous, indent=None if args.watch else 2, sort_keys=True),
                flush=args.watch,
            )
        else:
            print_human_status(previous)
        if not args.watch:
            return 0
        if args.output == "text":
            print("\nWatching service status; press Ctrl+C to stop.")
        while True:
            time.sleep(args.interval_seconds)
            current = fetch_status(base_url, args.ca, admin_token)
            if args.output == "json":
                print(json.dumps(current, separators=(",", ":"), sort_keys=True), flush=True)
            else:
                changes = status_changes(previous, current)
                timestamp = datetime.now().astimezone().strftime("%d/%m/%Y %H:%M:%S %Z")
                if changes:
                    print(f"\n[{timestamp}] Service status changes")
                    for change in changes:
                        print(f"  • {change}")
                else:
                    print(f"[{timestamp}] No service status changes.")
            previous = current
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    except (OSError, HubApiError) as exc:
        print(f"Hub summary failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
