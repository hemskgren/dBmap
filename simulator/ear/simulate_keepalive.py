#!/usr/bin/env python3
"""Publish a TLS keepalive from an already provisioned simulated Ear."""

import argparse
import json
import ssl
import sys
import time
from pathlib import Path
from uuid import uuid4

import paho.mqtt.client as mqtt


def load_credentials(path: Path) -> dict:
    if path.stat().st_mode & 0o077:
        raise PermissionError(f"{path} must not be accessible by group or other users")
    with path.open(encoding="utf-8") as stream:
        credentials = json.load(stream)
    if credentials.get("state") != "provisioned":
        raise ValueError("credentials file must contain an already provisioned simulator Ear")
    node_id = credentials.get("node_id")
    if not isinstance(node_id, str) or not node_id.startswith("SIM-EAR-"):
        raise ValueError("credentials file does not identify a SIM-EAR-* device")
    endpoint = credentials.get("mqtt")
    topic = credentials.get("topics", {}).get("keepalive")
    expected_topic = f"esp-ear/local/{node_id.lower()}/keepalive"
    if not isinstance(endpoint, dict) or endpoint.get("use_tls") is not True:
        raise ValueError("credentials file has no TLS MQTT endpoint")
    if topic != expected_topic:
        raise ValueError("credentials file does not contain this Ear's expected keepalive topic")
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Publish a TLS keepalive for an existing simulated Ear (without creating a device)."
    )
    parser.add_argument("--credentials-file", default="/tmp/dbmap-ear-simulator-credentials.json")
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument("--watch", action="store_true", help="Keep the simulated Ear online until Ctrl+C")
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=30,
        help="Delay between keepalives in watch mode (default: 30)",
    )
    args = parser.parse_args()
    if args.interval_seconds <= 0:
        parser.error("--interval-seconds must be greater than zero")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")

    try:
        credentials = load_credentials(Path(args.credentials_file))
        started = time.monotonic()
        while True:
            payload = make_keepalive(credentials, int(time.monotonic() - started))
            publish_keepalive(credentials, args.ca, payload)
            print(f"Published keepalive for {credentials['node_id']}.", flush=True)
            if not args.watch:
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
