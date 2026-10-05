#!/usr/bin/env python3
import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from uuid import uuid4

import paho.mqtt.client as mqtt

from app.schemas import ObservationBody


class UnsupportedSimulatorIdentity(RuntimeError):
    pass


def require_simulator_identity(node_id: str) -> None:
    if not node_id.startswith("SIM-EAR-"):
        raise UnsupportedSimulatorIdentity(
            f"hub assigned {node_id!r} instead of a SIM-EAR-* ID. "
            "The hub must be rebuilt and redeployed before simulator provisioning; "
            "create a new credentials file after deployment because node IDs cannot be changed."
        )


def api_request(
    base_url: str,
    ca_file: str,
    method: str,
    path: str,
    payload: dict,
    admin_token: str,
) -> dict:
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": f"Bearer {admin_token}",
            "Content-Type": "application/json",
        },
        method=method,
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read(1024).decode(errors="replace")
        raise RuntimeError(f"hub API returned HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"could not reach the hub API: {exc.reason}") from exc


def save_credentials(path: Path, credentials: dict, *, create: bool = False) -> None:
    if create:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(credentials, stream)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        return

    temporary_path = path.with_name(f".{path.name}.{uuid4().hex}.tmp")
    fd = os.open(temporary_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        json.dump(credentials, stream)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary_path, path)


def make_observation(
    node_id: str,
    sequence_number: int = 1,
    vehicle_id: str = "SIM-VEHICLE-001",
) -> dict[str, Any]:
    now = datetime.now(timezone.utc)
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": str(uuid4()),
        "node_id": node_id,
        "sequence_number": sequence_number,
        "event_time_utc": now.isoformat(),
        "capture_timestamp_monotonic_us": time.monotonic_ns() // 1000,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "classification": {
            "source_family": "vehicle",
            "confidence": 0.72,
            "hints": {"simulated_source_id": vehicle_id},
        },
        "bearing": {
            "deg": 180.0,
            "reference": "node",
            "confidence": 0.61,
        },
        "signal_level_dbfs": -34.0,
        "duration_ms": 850.0,
    }


def finish_provisioning(args: argparse.Namespace, record: dict, admin_token: str) -> dict:
    require_simulator_identity(record["node_id"])
    provisioned = api_request(
        args.api_url,
        args.ca,
        "POST",
        "/api/v1/provision",
        {
            "bootstrap_token": record["bootstrap_token"],
            "hardware_revision": "SIM-EAR-V1",
            "firmware_version": "0.1.0-simulator",
        },
        admin_token,
    )
    mqtt_endpoint = provisioned.get("mqtt")
    expected_topic = f"esp-ear/local/{provisioned['node_id'].lower()}/observation"
    if not isinstance(mqtt_endpoint, dict) or mqtt_endpoint.get("use_tls") is not True:
        raise RuntimeError("hub did not provision a TLS MQTT endpoint; refusing plaintext")
    if provisioned.get("topics", {}).get("observation") != expected_topic:
        raise RuntimeError("hub returned an observation topic outside the simulated Ear's own topic")
    if provisioned["node_id"] != record["node_id"]:
        raise RuntimeError("hub changed the node ID while provisioning the simulated Ear")
    observation = make_observation(provisioned["node_id"])
    ObservationBody.model_validate(observation)
    result = {
        "state": "provisioned",
        "node_id": provisioned["node_id"],
        "mqtt": mqtt_endpoint,
        "topics": provisioned["topics"],
        "observation": observation,
        "next_sequence_number": 1,
        "pending_observation": observation,
    }
    save_credentials(Path(args.credentials_file), result)
    return result


def load_or_provision(args: argparse.Namespace, admin_token: str) -> dict:
    credentials_path = Path(args.credentials_file)
    if args.reuse_credentials:
        if credentials_path.stat().st_mode & 0o077:
            raise PermissionError(f"{credentials_path} must not be accessible by group or other users")
        with credentials_path.open(encoding="utf-8") as stream:
            record = json.load(stream)
        if record.get("state") == "pending":
            return finish_provisioning(args, record, admin_token)
        if record.get("state") == "provisioned":
            record.setdefault(
                "next_sequence_number",
                record["observation"]["sequence_number"] + 1,
            )
            return record
        raise ValueError("credentials file is not resumable; it has no saved bootstrap token")
    if credentials_path.exists():
        raise FileExistsError(
            f"{credentials_path} already exists; choose another path or pass --reuse-credentials"
        )

    save_credentials(credentials_path, {"state": "creating"}, create=True)
    created = api_request(
        args.api_url,
        args.ca,
        "POST",
        "/api/v1/nodes",
        {
            "node_type": "ear",
            "hardware_revision": "SIM-EAR-V1",
            "simulated": True,
        },
        admin_token,
    )
    pending = {
        "state": "pending",
        "node_id": created["node_id"],
        "bootstrap_token": created["bootstrap_token"],
    }
    save_credentials(credentials_path, pending)
    require_simulator_identity(pending["node_id"])
    return finish_provisioning(args, pending, admin_token)


def publish_observation(credentials: dict, ca_file: str) -> str:
    return publish_payload(credentials, credentials["observation"], ca_file)


def publish_payload(credentials: dict, payload: dict, ca_file: str) -> str:
    endpoint = credentials["mqtt"]
    topic = credentials["topics"]["observation"]
    ObservationBody.model_validate(payload)

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
        info = client.publish(topic, json.dumps(payload), qos=1)
        info.wait_for_publish(timeout=15)
        if not info.is_published():
            raise RuntimeError("timed out publishing the observation")
    finally:
        if client.is_connected():
            client.disconnect()
        client.loop_stop()
    return payload["observation_id"]


def publish_observation_batch(
    credentials: dict,
    ca_file: str,
    credentials_path: Path,
    count: int,
    interval_seconds: float,
    vehicle_id: str,
) -> list[str]:
    published_ids = []
    for index in range(count):
        pending = credentials.get("pending_observation")
        if pending is None:
            if index > 0:
                time.sleep(interval_seconds)
            sequence = credentials["next_sequence_number"]
            pending = make_observation(credentials["node_id"], sequence, vehicle_id)
        pending["classification"]["hints"]["simulated_source_id"] = vehicle_id
        credentials["pending_observation"] = pending
        save_credentials(credentials_path, credentials)

        observation_id = publish_payload(credentials, pending, ca_file)
        published_ids.append(observation_id)
        credentials["next_sequence_number"] = max(
            credentials["next_sequence_number"],
            pending["sequence_number"] + 1,
        )
        credentials.pop("pending_observation", None)
        save_credentials(credentials_path, credentials)
    return published_ids


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create a simulated Ear and publish one protocol-validated TLS observation."
    )
    parser.add_argument("--hub-host", required=True, help="Hub LAN IP or certificate-valid hostname")
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument(
        "--credentials-file",
        default="/tmp/dbmap-ear-simulator-credentials.json",
        help="Private file used to preserve simulator MQTT credentials",
    )
    parser.add_argument(
        "--reuse-credentials",
        action="store_true",
        help="Republish using an existing credentials file instead of creating another Ear",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=1,
        help="Number of observations to publish; 1 republishes the saved observation",
    )
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=15,
        help="Delay between observations when --count is greater than 1 (default: 15)",
    )
    parser.add_argument(
        "--vehicle-id",
        default="SIM-VEHICLE-001",
        help="Stable synthetic source ID shared by a simulated vehicle's observations",
    )
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be at least 1")
    if args.interval_seconds < 0:
        parser.error("--interval-seconds cannot be negative")
    if not args.vehicle_id.strip():
        parser.error("--vehicle-id cannot be empty")

    admin_token = os.environ.get("DBMAP_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        parser.error("DBMAP_ADMIN_TOKEN must be loaded from the private .env file")
    if not Path(args.ca).is_file():
        parser.error(f"CA file does not exist: {args.ca}")

    args.api_url = f"https://{args.hub_host}:8443"
    credentials_path = Path(args.credentials_file)
    try:
        credentials = load_or_provision(args, admin_token)
        if args.count == 1:
            observation_id = publish_observation(credentials, args.ca)
            credentials["next_sequence_number"] = max(
                credentials.get("next_sequence_number", 1),
                credentials["observation"]["sequence_number"] + 1,
            )
            credentials.pop("pending_observation", None)
            save_credentials(credentials_path, credentials)
            published_ids = [observation_id]
        else:
            published_ids = publish_observation_batch(
                credentials,
                args.ca,
                credentials_path,
                args.count,
                args.interval_seconds,
                args.vehicle_id,
            )
    except UnsupportedSimulatorIdentity as exc:
        print(f"Simulator setup failed: {exc}", file=sys.stderr)
        print(
            f"The pending node record is in {args.credentials_file}; "
            "use a new credentials-file path after redeploying the hub.",
            file=sys.stderr,
        )
        return 1
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f"Observation simulation failed: {exc}", file=sys.stderr)
        if not args.reuse_credentials:
            print(
                f"MQTT credentials file (if created): {args.credentials_file}",
                file=sys.stderr,
            )
        print(
            "Retry with --reuse-credentials to resume provisioning or republish the same observation.",
            file=sys.stderr,
        )
        return 1

    for observation_id in published_ids:
        print(f"Published observation {observation_id} from {credentials['node_id']}.")
    print(f"Credentials are stored in {args.credentials_file} (mode 0600); keep it private.")
    print(
        f'Verify with: curl --cacert {args.ca} -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" '
        f"'{args.api_url}/api/v1/observations?node_id={credentials['node_id']}'"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
