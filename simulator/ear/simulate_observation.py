#!/usr/bin/env python3
import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from uuid import uuid4

_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
from lib.hub_cli import (
    add_hub_connection_arguments,
    format_hub_http_error,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)

DEFAULT_EAR = "a"


def credentials_file_for_ear(ear: str) -> str:
    return f"shared/simulator/sim-ear-{ear}.json"


def resolve_credentials_file(args: argparse.Namespace) -> str:
    if getattr(args, "credentials_file", None):
        return args.credentials_file
    return credentials_file_for_ear(getattr(args, "ear", DEFAULT_EAR))


def _observation_body():
    from app.schemas import ObservationBody

    return ObservationBody


def _mqtt_client():
    import paho.mqtt.client as mqtt

    return mqtt


def _keepalive_helpers():
    from importlib.util import module_from_spec, spec_from_file_location

    spec = spec_from_file_location(
        "dbmap_ear_keepalive",
        Path(__file__).with_name("simulate_keepalive.py"),
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load keepalive helpers")
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.make_keepalive, module.publish_keepalive


def publish_ear_keepalive(credentials: dict, ca_file: str, *, uptime_s: int = 0) -> None:
    make_keepalive, publish_keepalive = _keepalive_helpers()
    publish_keepalive(credentials, ca_file, make_keepalive(credentials, uptime_s))


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
        raise RuntimeError(
            format_hub_http_error(exc.code, exc.read(1024), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"could not reach the hub API: {exc.reason}") from exc


def save_credentials(path: Path, credentials: dict, *, create: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
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
    *,
    include_l0: bool = False,
    l0_scenario: str = "quiet_ambient",
    emission_kind: str | None = None,
) -> dict[str, Any]:
    """Build a hierarchical protocol-v1 Observation."""
    from l0_measurements import attach_l0_extension, empty_l0_measurements

    now = datetime.now(UTC)
    observation: dict[str, Any] = {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": {
            "observation_id": str(uuid4()),
            "node_id": node_id,
            "hub_id": "",
            "sequence_number": sequence_number,
        },
        "context": {
            "event_time_utc": now.isoformat(),
            "capture_timestamp_monotonic_us": time.monotonic_ns() // 1000,
            "timing_quality": "estimated",
            "clock_offset_ms": None,
            "timestamp_uncertainty_ms": 10.0,
            "interval_duration_ms": 850.0,
            "episode_duration_ms": 850.0,
            "emission_kind": "periodic",
        },
        "acoustic": {
            "signal_level_dbfs": -34.0,
            "measurements": empty_l0_measurements(),
            "signature": None,
            "classification": {
                "schema": "l2",
                "version": 1,
                "source_family": "vehicle",
                "confidence": 0.72,
                "hints": {"simulated_source_id": vehicle_id},
            },
        },
        "spatial": {
            "bearing": {
                "deg": 180.0,
                "reference": "node",
                "confidence": 0.61,
            },
            "position": None,
        },
    }
    if include_l0:
        observation = attach_l0_extension(
            observation,
            scenario=l0_scenario,
            emission_kind=emission_kind,  # type: ignore[arg-type]
        )
    return observation


def finish_provisioning(
    args: argparse.Namespace, record: dict, admin_token: str
) -> dict:
    require_simulator_identity(record["node_id"])
    provisioned = api_request(
        args.api_url,
        args.ca,
        "POST",
        "/api/v1/provision",
        {
            "bootstrap_token": record["bootstrap_token"],
            "hardware_revision": "SIM-EAR-V1",
            "firmware_version": "0.3.0-simulator",
        },
        admin_token,
    )
    mqtt_endpoint = provisioned.get("mqtt")
    expected_topic = f"esp-ear/local/{provisioned['node_id'].lower()}/observation"
    if not isinstance(mqtt_endpoint, dict) or mqtt_endpoint.get("use_tls") is not True:
        raise RuntimeError(
            "hub did not provision a TLS MQTT endpoint; refusing plaintext"
        )
    if provisioned.get("topics", {}).get("observation") != expected_topic:
        raise RuntimeError(
            "hub returned an observation topic outside the simulated Ear's own topic"
        )
    if provisioned["node_id"] != record["node_id"]:
        raise RuntimeError(
            "hub changed the node ID while provisioning the simulated Ear"
        )
    include_l0 = bool(getattr(args, "include_l0", False))
    l0_scenario = getattr(args, "l0_scenario", "quiet_ambient")
    observation = make_observation(
        provisioned["node_id"],
        include_l0=include_l0,
        l0_scenario=l0_scenario,
    )
    _observation_body().model_validate(observation)
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
            raise PermissionError(
                f"{credentials_path} must not be accessible by group or other users"
            )
        with credentials_path.open(encoding="utf-8") as stream:
            record = json.load(stream)
        if record.get("state") == "pending":
            return finish_provisioning(args, record, admin_token)
        if record.get("state") == "provisioned":
            record.setdefault(
                "next_sequence_number",
                record["observation"]["identity"]["sequence_number"] + 1,
            )
            return record
        raise ValueError(
            "credentials file is not resumable; it has no saved bootstrap token"
        )
    if credentials_path.exists():
        raise FileExistsError(
            f"{credentials_path} already exists; use publish (or choose another --ear / --credentials-file)"
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
    _observation_body().model_validate(payload)

    mqtt = _mqtt_client()
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
    return payload["identity"]["observation_id"]


def publish_observation_batch(
    credentials: dict,
    ca_file: str,
    credentials_path: Path,
    count: int,
    interval_seconds: float,
    vehicle_id: str,
    *,
    ear: str = DEFAULT_EAR,
    include_l0: bool = False,
    l0_scenario: str = "quiet_ambient",
) -> list[str]:
    published_ids = []
    for index in range(count):
        pending = credentials.get("pending_observation")
        if pending is None:
            if index > 0:
                time.sleep(interval_seconds)
            sequence = credentials["next_sequence_number"]
            pending = make_observation(
                credentials["node_id"],
                sequence,
                vehicle_id,
                include_l0=include_l0,
                l0_scenario=l0_scenario,
            )
        pending["acoustic"]["classification"]["hints"]["simulated_source_id"] = (
            vehicle_id
        )
        credentials["pending_observation"] = pending
        save_credentials(credentials_path, credentials)

        observation_id = publish_payload(credentials, pending, ca_file)
        published_ids.append(observation_id)
        credentials["next_sequence_number"] = max(
            credentials["next_sequence_number"],
            pending["identity"]["sequence_number"] + 1,
        )
        credentials.pop("pending_observation", None)
        save_credentials(credentials_path, credentials)
        at_seconds = index * interval_seconds
        print(
            f"{index + 1:02d}  t={at_seconds:g}s  {ear}  {observation_id}",
            flush=True,
        )
    return published_ids


def _add_ear_selection_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--ear",
        choices=("a", "b", "c"),
        default=DEFAULT_EAR,
        help="Sim Ear slot → shared/simulator/sim-ear-{a,b,c}.json (default: a)",
    )
    parser.add_argument(
        "--credentials-file",
        default=None,
        help="Override credentials path (default: from --ear)",
    )


def _add_publish_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--count",
        type=int,
        default=1,
        help="Number of observations to publish; 1 uses/republishes the saved observation",
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
    parser.add_argument(
        "--include-l0",
        action="store_true",
        help=(
            "Attach deterministic L0 scene fixtures and matching emission_kind "
            "(default baseline uses empty L0 measurements with periodic emission)."
        ),
    )
    parser.add_argument(
        "--l0-scenario",
        default="quiet_ambient",
        choices=(
            "quiet_ambient",
            "stable_tonal",
            "varying_level",
            "high_crest_transient",
            "clipped_windows",
            "insufficient_data_nulls",
        ),
        help="Deterministic L0 fixture when --include-l0 is set (default: quiet_ambient)",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Simulated Ear observations. "
            "Use add for a new SIM-EAR-* device, publish to reuse credentials."
        )
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)

    add_cmd = commands.add_parser(
        "add",
        help="Register a new simulated Ear and publish one observation",
    )
    _add_ear_selection_args(add_cmd)
    _add_publish_options(add_cmd)

    publish_cmd = commands.add_parser(
        "publish",
        help="Publish using an existing shared/simulator/sim-ear-*.json credentials file",
    )
    _add_ear_selection_args(publish_cmd)
    _add_publish_options(publish_cmd)
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
    if args.count < 1:
        parser.error("--count must be at least 1")
    if args.interval_seconds < 0:
        parser.error("--interval-seconds cannot be negative")
    if not args.vehicle_id.strip():
        parser.error("--vehicle-id cannot be empty")

    args.reuse_credentials = args.command == "publish"
    args.credentials_file = resolve_credentials_file(args)
    args.api_url, admin_token = validate_hub_connection(args, parser)
    credentials_path = Path(args.credentials_file)
    try:
        credentials = load_or_provision(args, admin_token)
        node_id = credentials["node_id"]
        print(
            f"Publishing {args.count} observation(s) from {node_id} (--ear {args.ear}):",
            flush=True,
        )
        publish_ear_keepalive(credentials, args.ca, uptime_s=0)
        print(f"  keepalive  {args.ear}  {node_id}", flush=True)
        if args.count == 1:
            if args.include_l0:
                from l0_measurements import attach_l0_extension

                pending = credentials.get("pending_observation") or credentials[
                    "observation"
                ]
                pending = attach_l0_extension(pending, scenario=args.l0_scenario)
                credentials["pending_observation"] = pending
                credentials["observation"] = pending
                save_credentials(credentials_path, credentials)
                observation_id = publish_payload(credentials, pending, args.ca)
            else:
                observation_id = publish_observation(credentials, args.ca)
            credentials["next_sequence_number"] = max(
                credentials.get("next_sequence_number", 1),
                credentials["observation"]["identity"]["sequence_number"] + 1,
            )
            credentials.pop("pending_observation", None)
            save_credentials(credentials_path, credentials)
            print(
                f"01  t=0s  {args.ear}  {observation_id}",
                flush=True,
            )
        else:
            publish_observation_batch(
                credentials,
                args.ca,
                credentials_path,
                args.count,
                args.interval_seconds,
                args.vehicle_id,
                ear=args.ear,
                include_l0=args.include_l0,
                l0_scenario=args.l0_scenario,
            )
    except UnsupportedSimulatorIdentity as exc:
        print(f"Simulator setup failed: {exc}", file=sys.stderr)
        print(
            f"The pending node record is in {args.credentials_file}; "
            "use a new --ear / credentials-file path after redeploying the hub.",
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
            "Use publish to resume provisioning or republish with the same credentials.",
            file=sys.stderr,
        )
        return 1

    print(
        f"Credentials: {args.credentials_file} (mode 0600); "
        f"reuse with: ./scripts/sim-ear-observation publish --ear {args.ear}"
    )
    print(f"Inspect node: uv run python scripts/hub_admin.py show {node_id}")
    print(
        f"List observations: uv run python scripts/hub_event_track.py list --node-id {node_id}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
