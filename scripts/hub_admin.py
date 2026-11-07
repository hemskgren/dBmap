#!/usr/bin/env python3
import argparse
import json
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from lib.hub_cli import (
    add_hub_connection_arguments,
    format_hub_http_error,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)


class HubAdminError(RuntimeError):
    pass


def request_json(
    base_url: str,
    ca_file: str,
    admin_token: str,
    method: str,
    path: str,
    payload: dict[str, Any] | None = None,
) -> Any:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Authorization": f"Bearer {admin_token}"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=data,
        headers=headers,
        method=method,
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        raise HubAdminError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise HubAdminError(f"could not reach the hub API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubAdminError(f"hub returned invalid JSON for {path}") from exc


def get_node(
    base_url: str,
    ca_file: str,
    admin_token: str,
    node_id: str,
) -> dict[str, Any]:
    result = request_json(
        base_url,
        ca_file,
        admin_token,
        "GET",
        f"/api/v1/nodes/{urllib.parse.quote(node_id, safe='')}",
    )
    if not isinstance(result, dict):
        raise HubAdminError("hub returned an invalid node")
    return result


MISSING_VALUE = "—"


def _format_location(installation: object) -> str:
    if not isinstance(installation, dict):
        return "Not set"
    latitude = installation.get("latitude")
    longitude = installation.get("longitude")
    if latitude is None or longitude is None:
        return "Not set"
    return f"{latitude}, {longitude}"


def _display_value(value: object) -> str:
    """Render a Hub field for the terminal; never invent substitutes for missing data."""
    if value is None:
        return MISSING_VALUE
    if isinstance(value, str):
        text = value.strip()
        return text if text else MISSING_VALUE
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def _sorted_devices(devices: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(devices, key=lambda item: str(item.get("node_id", "")))


def format_device_list(devices: list[dict[str, Any]]) -> str:
    """Format device summaries like the web Devices table."""
    headers = (
        "Device",
        "Type",
        "Status",
        "Provisioning",
        "Lifecycle",
        "Assigned owner",
        "Location",
    )
    rows: list[tuple[str, ...]] = [headers]
    for device in _sorted_devices(devices):
        rows.append(
            (
                str(device.get("node_id", "")),
                str(device.get("node_type", "")).upper(),
                str(device.get("status", "offline")),
                str(device.get("provisioning_state", "")),
                str(device.get("lifecycle_state", "active")),
                str(device.get("owner_display_name") or "Unassigned"),
                _format_location(device.get("installation")),
            )
        )
    widths = [max(len(row[index]) for row in rows) for index in range(len(headers))]
    lines = [
        "  ".join(cell.ljust(widths[index]) for index, cell in enumerate(row))
        for row in rows
    ]
    return "\n".join(lines)


def _format_labeled_block(title: str, fields: list[tuple[str, object]]) -> list[str]:
    label_width = max((len(label) for label, _ in fields), default=0)
    lines = [title]
    for label, value in fields:
        lines.append(f"  {label.ljust(label_width)}  {_display_value(value)}")
    return lines


def format_device_list_detail(
    devices: list[dict[str, Any]],
    nodes_by_id: dict[str, dict[str, Any]],
) -> str:
    """Summary table plus per-device reported state and Hub installation."""
    lines = [format_device_list(devices), ""]
    for device in _sorted_devices(devices):
        node_id = str(device.get("node_id", ""))
        node = nodes_by_id.get(node_id) or {}
        reported = node.get("reported")
        if not isinstance(reported, dict):
            reported = {}
        installation = node.get("installation")
        if installation is None:
            installation = device.get("installation")
        if not isinstance(installation, dict):
            installation = {}

        lines.append(f"=== {node_id} ===")
        lines.extend(
            _format_labeled_block(
                "Reported state (device)",
                [
                    ("Firmware version", reported.get("firmware_version")),
                    ("Hardware revision", reported.get("hardware_revision")),
                    ("Configuration version", reported.get("config_version")),
                    ("Calibration version", reported.get("calibration_version")),
                    ("Classifier version", reported.get("classifier_version")),
                    ("Uptime (s)", reported.get("uptime_s")),
                ],
            )
        )
        lines.append("")
        lines.extend(
            _format_labeled_block(
                "Installation (Hub-managed)",
                [
                    ("Latitude", installation.get("latitude")),
                    ("Longitude", installation.get("longitude")),
                    ("Orientation (deg)", installation.get("orientation_deg")),
                    ("Height (m)", installation.get("height_m")),
                ],
            )
        )
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def set_desired(args: argparse.Namespace, base_url: str, admin_token: str) -> Any:
    current = get_node(base_url, args.ca, admin_token, args.node_id)
    desired = current.get("desired")
    if not isinstance(desired, dict):
        raise HubAdminError("hub returned an invalid desired state")
    fields = (
        "firmware_version",
        "config_version",
        "calibration_version",
        "classifier_version",
    )
    updates = {
        field: getattr(args, field)
        for field in fields
        if getattr(args, field) is not None
    }
    if args.payload_file is not None:
        payload = json.loads(Path(args.payload_file).read_text(encoding="utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("--payload-file must contain a JSON object")
        updates["payload"] = payload
    if not updates:
        raise ValueError("set-desired requires at least one field to change")

    body = {field: desired.get(field) for field in fields}
    body["payload"] = desired.get("payload", {})
    body.update(updates)
    node_id = urllib.parse.quote(args.node_id, safe="")
    return request_json(
        base_url,
        args.ca,
        admin_token,
        "PUT",
        f"/api/v1/nodes/{node_id}/desired",
        body,
    )


def set_installation(args: argparse.Namespace, base_url: str, admin_token: str) -> Any:
    current = get_node(base_url, args.ca, admin_token, args.node_id)
    installation = current.get("installation")
    if installation is None:
        installation = {}
    elif not isinstance(installation, dict):
        raise HubAdminError("hub returned invalid installation metadata")
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
    body = {field: installation.get(field) for field in fields}
    updates = {
        field: getattr(args, field)
        for field in fields
        if getattr(args, field) is not None
    }
    if not updates:
        raise ValueError("set-installation requires at least one field to change")
    body.update(updates)
    if body["latitude"] is None or body["longitude"] is None:
        raise ValueError(
            "latitude and longitude are required for a node without installation data"
        )
    node_id = urllib.parse.quote(args.node_id, safe="")
    return request_json(
        base_url,
        args.ca,
        admin_token,
        "PUT",
        f"/api/v1/nodes/{node_id}/installation",
        body,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Inspect and update registered nodes through the authenticated Hub API."
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)
    list_cmd = commands.add_parser("list", help="List registered nodes")
    list_cmd.add_argument(
        "--detail",
        action="store_true",
        help=(
            "After the summary table, print each node's reported state and "
            "Hub-managed installation fields"
        ),
    )

    show = commands.add_parser("show", help="Show one node")
    show.add_argument("node_id")

    desired = commands.add_parser(
        "set-desired", help="Update selected desired-state fields"
    )
    desired.add_argument("node_id")
    desired.add_argument("--firmware-version")
    desired.add_argument("--config-version", type=int)
    desired.add_argument("--calibration-version", type=int)
    desired.add_argument("--classifier-version")
    desired.add_argument(
        "--payload-file", help="JSON object to replace the desired payload"
    )

    installation = commands.add_parser(
        "set-installation",
        help="Update selected installation fields without clearing other fields",
    )
    installation.add_argument("node_id")
    installation.add_argument("--latitude", type=float)
    installation.add_argument("--longitude", type=float)
    installation.add_argument("--floor", type=int)
    installation.add_argument("--height-m", dest="height_m", type=float)
    installation.add_argument(
        "--height-accuracy-m", dest="height_accuracy_m", type=float
    )
    installation.add_argument("--mount-type")
    installation.add_argument("--environment")
    installation.add_argument("--orientation-deg", dest="orientation_deg", type=float)

    for name, state, help_text in (
        (
            "deactivate",
            "deactivated",
            "Deactivate a node but retain its history and MQTT credentials",
        ),
        ("reactivate", "active", "Reactivate a previously deactivated node"),
    ):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("node_id")
        command.set_defaults(lifecycle_state=state)
    return parser


def main() -> int:
    try:
        load_project_env()
    except ValueError as exc:
        print(f"Hub configuration failed: {exc}", file=sys.stderr)
        return 1
    parser = build_parser()
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    base_url, admin_token = validate_hub_connection(args, parser)
    try:
        if args.command == "list":
            devices = request_json(
                base_url, args.ca, admin_token, "GET", "/api/v1/devices"
            )
            if not isinstance(devices, list) or not all(
                isinstance(device, dict) and "node_id" in device for device in devices
            ):
                raise HubAdminError("hub returned an invalid device list")
            if args.detail:
                nodes_by_id: dict[str, dict[str, Any]] = {}
                for device in _sorted_devices(devices):
                    node_id = str(device.get("node_id", ""))
                    nodes_by_id[node_id] = get_node(
                        base_url, args.ca, admin_token, node_id
                    )
                print(format_device_list_detail(devices, nodes_by_id), end="")
            else:
                print(format_device_list(devices))
            return 0
        if args.command == "show":
            result = get_node(base_url, args.ca, admin_token, args.node_id)
        elif args.command == "set-desired":
            result = set_desired(args, base_url, admin_token)
        elif args.command == "set-installation":
            result = set_installation(args, base_url, admin_token)
        else:
            node_id = urllib.parse.quote(args.node_id, safe="")
            result = request_json(
                base_url,
                args.ca,
                admin_token,
                "PUT",
                f"/api/v1/nodes/{node_id}/lifecycle",
                {"lifecycle_state": args.lifecycle_state},
            )
    except (KeyError, OSError, ValueError, HubAdminError) as exc:
        print(f"Hub admin operation failed: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
