#!/usr/bin/env python3
import argparse
import json
import os
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


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
        detail = exc.read(1024).decode(errors="replace")
        raise HubAdminError(f"hub API returned HTTP {exc.code}: {detail}") from exc
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
        raise ValueError("latitude and longitude are required for a node without installation data")
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
    parser.add_argument("--hub-host", required=True, help="Hub LAN IP or certificate-valid hostname")
    parser.add_argument("--hub-port", type=int, default=8443, help="HTTPS listener port (default: 8443)")
    parser.add_argument(
        "--base-path",
        default=os.environ.get("DBMAP_PUBLIC_BASE_PATH", ""),
        help="Optional public path prefix, for example /dbmap",
    )
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="List registered nodes")

    show = commands.add_parser("show", help="Show one node")
    show.add_argument("node_id")

    desired = commands.add_parser("set-desired", help="Update selected desired-state fields")
    desired.add_argument("node_id")
    desired.add_argument("--firmware-version")
    desired.add_argument("--config-version", type=int)
    desired.add_argument("--calibration-version", type=int)
    desired.add_argument("--classifier-version")
    desired.add_argument("--payload-file", help="JSON object to replace the desired payload")

    installation = commands.add_parser(
        "set-installation",
        help="Update selected installation fields without clearing other fields",
    )
    installation.add_argument("node_id")
    installation.add_argument("--latitude", type=float)
    installation.add_argument("--longitude", type=float)
    installation.add_argument("--floor", type=int)
    installation.add_argument("--height-m", dest="height_m", type=float)
    installation.add_argument("--height-accuracy-m", dest="height_accuracy_m", type=float)
    installation.add_argument("--mount-type")
    installation.add_argument("--environment")
    installation.add_argument("--orientation-deg", dest="orientation_deg", type=float)

    for name, state, help_text in (
        ("deactivate", "deactivated", "Deactivate a node but retain its history and MQTT credentials"),
        ("reactivate", "active", "Reactivate a previously deactivated node"),
    ):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("node_id")
        command.set_defaults(lifecycle_state=state)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    admin_token = os.environ.get("DBMAP_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        parser.error("DBMAP_ADMIN_TOKEN must be loaded from the private .env file")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")
    args.base_path = args.base_path.rstrip("/")
    if args.base_path and (
        not args.base_path.startswith("/")
        or "//" in args.base_path
        or any(part in {".", ".."} for part in args.base_path.split("/"))
    ):
        parser.error("--base-path must be empty or a normalized absolute path")

    base_url = f"https://{args.hub_host}:{args.hub_port}{args.base_path}"
    try:
        if args.command == "list":
            nodes = request_json(base_url, args.ca, admin_token, "GET", "/api/v1/nodes")
            if not isinstance(nodes, list) or not all(
                isinstance(node, dict) and "node_id" in node for node in nodes
            ):
                raise HubAdminError("hub returned an invalid node list")
            for node in nodes:
                print(
                    f"{node['node_id']}\t{node['node_type']}\t"
                    f"{node.get('lifecycle_state', 'active')}\t"
                    f"{node.get('availability', 'unknown')}"
                )
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
