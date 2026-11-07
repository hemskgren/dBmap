#!/usr/bin/env python3
"""Publish a TLS keepalive from an already provisioned simulated Ear."""

import argparse
import json
import ssl
import sys
import time
from pathlib import Path
from uuid import uuid4

_EAR_DIR = Path(__file__).resolve().parent
_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
for _path in (_EAR_DIR, _SCRIPTS):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))
from ear_profile import (
    EarProfileError,
    load_ear_profile,
    profile_summary,
    resolve_profile_keepalive_paths,
)
from lib.hub_cli import (
    load_project_env,
    print_help_without_arguments,
)
from sim_ear_slots import (
    DEFAULT_EAR,
    EAR_SLOTS,
    credentials_file_for_ear,
    slot_help_text,
)


def _cli_has_ear_flag(argv: list[str] | None = None) -> bool:
    """True when the user passed ``--ear`` / ``--ear=`` (not merely the default)."""
    tokens = sys.argv[1:] if argv is None else argv
    return any(token == "--ear" or token.startswith("--ear=") for token in tokens)


def resolve_credentials_files(args: argparse.Namespace) -> list[str]:
    profile_path = getattr(args, "ear_profile", None)
    if profile_path:
        if args.credentials_file:
            raise ValueError("--ear-profile cannot be combined with --credentials-file")
        if _cli_has_ear_flag():
            raise ValueError("--ear-profile cannot be combined with --ear")
        try:
            profile = load_ear_profile(profile_path)
            paths = resolve_profile_keepalive_paths(profile, require_files=True)
        except EarProfileError as exc:
            raise ValueError(str(exc)) from exc
        print(f"EAR profile: {profile_summary(profile)}", flush=True)
        return paths
    if args.credentials_file:
        if args.ear == "all":
            raise ValueError("--credentials-file cannot be combined with --ear all")
        return [args.credentials_file]
    if args.ear == "all":
        paths = [
            credentials_file_for_ear(slot)
            for slot in EAR_SLOTS
            if Path(credentials_file_for_ear(slot)).is_file()
        ]
        if not paths:
            letters = ",".join(EAR_SLOTS)
            raise ValueError(
                f"no shared/simulator/sim-ear-{{{letters}}}.json files found for --ear all"
            )
        return paths
    return [credentials_file_for_ear(args.ear)]


def load_credentials(path: Path) -> dict:
    if path.stat().st_mode & 0o077:
        raise PermissionError(f"{path} must not be accessible by group or other users")
    with path.open(encoding="utf-8") as stream:
        credentials = json.load(stream)
    if credentials.get("state") != "provisioned":
        raise ValueError(
            "credentials file must contain an already provisioned simulator Ear"
        )
    node_id = credentials.get("node_id")
    if not isinstance(node_id, str) or not node_id.startswith("SIM-EAR-"):
        raise ValueError("credentials file does not identify a SIM-EAR-* device")
    endpoint = credentials.get("mqtt")
    topic = credentials.get("topics", {}).get("keepalive")
    expected_topic = f"esp-ear/local/{node_id.lower()}/keepalive"
    if not isinstance(endpoint, dict) or endpoint.get("use_tls") is not True:
        raise ValueError("credentials file has no TLS MQTT endpoint")
    if topic != expected_topic:
        raise ValueError(
            "credentials file does not contain this Ear's expected keepalive topic"
        )
    return credentials


def make_keepalive(credentials: dict, uptime_s: int) -> dict:
    return {
        "node_id": credentials["node_id"],
        "firmware_version": credentials.get("firmware_version", "0.1.0-simulator"),
        "hardware_revision": credentials.get("hardware_revision", "SIM-EAR-V1"),
        "config_version": 0,
        "calibration_version": 0,
        "classifier_version": "",
        "uptime_s": uptime_s,
        "pending_configuration_change": False,
    }


def publish_keepalive(credentials: dict, ca_file: str, payload: dict) -> None:
    import paho.mqtt.client as mqtt

    endpoint = credentials["mqtt"]
    topic = credentials["topics"]["keepalive"]
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"dbmap-ear-sim-{uuid4().hex[:12]}",
        protocol=mqtt.MQTTv311,
    )
    client.username_pw_set(endpoint["username"], endpoint["password"])
    client.tls_set(ca_certs=ca_file, cert_reqs=ssl.CERT_REQUIRED)
    try:
        client.connect(endpoint["host"], endpoint["port"], keepalive=30)
        client.loop_start()
        deadline = time.monotonic() + 10
        while not client.is_connected() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not client.is_connected():
            raise RuntimeError("timed out connecting to the TLS MQTT broker")
        info = client.publish(topic, json.dumps(payload), qos=0, retain=False)
        info.wait_for_publish(timeout=10)
        if not info.is_published():
            raise RuntimeError("timed out publishing the keepalive")
    finally:
        if client.is_connected():
            client.disconnect()
        client.loop_stop()


def _add_ear_selection_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--ear",
        choices=(*EAR_SLOTS, "all"),
        default=DEFAULT_EAR,
        help=slot_help_text(include_all=True),
    )
    parser.add_argument(
        "--ear-profile",
        default=None,
        metavar="PATH",
        help=(
            "JSON EAR profile; keepalive uses the profile's unique slots "
            "(via sim_ear_slots). Cannot combine with --ear or --credentials-file."
        ),
    )
    parser.add_argument(
        "--credentials-file",
        default=None,
        help="Override credentials path (single Ear; cannot combine with --ear all)",
    )
    parser.add_argument(
        "--ca", default="shared/trust/ca.crt", help="Path to the local hub CA"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Publish TLS keepalives for provisioned simulated Ears. "
            "once = one round then exit; watch = continuous until Ctrl+C."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)
    once = commands.add_parser(
        "once",
        help="Publish one keepalive per selected Ear, then exit",
    )
    _add_ear_selection_args(once)
    watch = commands.add_parser(
        "watch",
        help="Publish keepalives continuously until Ctrl+C (default every 30s)",
    )
    _add_ear_selection_args(watch)
    watch.add_argument(
        "--interval-seconds",
        type=float,
        default=30,
        help="Delay between rounds (default: 30)",
    )
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
    if args.command == "watch" and args.interval_seconds <= 0:
        parser.error("--interval-seconds must be greater than zero")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")

    try:
        credential_paths = resolve_credentials_files(args)
        loaded = [
            (path, load_credentials(Path(path))) for path in credential_paths
        ]
        started = time.monotonic()
        while True:
            for path, credentials in loaded:
                payload = make_keepalive(
                    credentials, int(time.monotonic() - started)
                )
                publish_keepalive(credentials, args.ca, payload)
                print(
                    f"Published keepalive for {credentials['node_id']} ({path}).",
                    flush=True,
                )
            if args.command != "watch":
                break
            time.sleep(args.interval_seconds)
    except KeyboardInterrupt:
        print("Stopped simulated keepalive.")
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f"Keepalive simulation failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
