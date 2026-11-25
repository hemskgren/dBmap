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

_EAR_DIR = Path(__file__).resolve().parent
_SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
for _path in (_EAR_DIR, _SCRIPTS):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))
from assessment import assessment_path_depth, build_assessment
from lib.hub_cli import (
    format_hub_http_error,
    print_help_without_arguments,
)
from publish_safety import (
    HUB_PORT,
    HUB_URL,
    NoRedirect,
    add_ca_argument,
    add_live_mqtt_argument,
    add_mqtt_arguments,
    load_simulator_environment,
    require_identity,
    resolve_mqtt_destination,
    resolve_repo_path,
    validate_api_destination,
    validate_publish,
)
from sim_ear_slots import (
    DEFAULT_EAR,
    EAR_SLOTS,
    SimEarSlotError,
    assert_node_id_matches_slot,
    credentials_file_for_ear,
    slot_help_text,
)

# Re-export for tests / callers that import from this module.
__all_slots__ = EAR_SLOTS


def resolve_credentials_file(args: argparse.Namespace) -> str:
    if getattr(args, "credentials_file", None):
        return args.credentials_file
    return credentials_file_for_ear(getattr(args, "ear", DEFAULT_EAR))


def admin_token_for_destination(api_url: str) -> tuple[str, str]:
    variable = (
        "DBMAP_TEST_HUB_ADMIN_TOKEN"
        if api_url == HUB_URL
        else "DBMAP_ADMIN_TOKEN"
    )
    return variable, os.environ.get(variable, "")


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


def publish_ear_keepalive(
    credentials: dict,
    ca_file: str,
    *,
    uptime_s: int = 0,
    live: bool = False,
    mqtt_host: str | None = None,
    mqtt_port: int | None = None,
) -> None:
    make_keepalive, publish_keepalive = _keepalive_helpers()
    publish_keepalive(
        credentials,
        ca_file,
        make_keepalive(credentials, uptime_s),
        live=live,
        mqtt_host=mqtt_host,
        mqtt_port=mqtt_port,
    )


class UnsupportedSimulatorIdentity(RuntimeError):
    pass


def require_simulator_identity(node_id: str) -> None:
    try:
        require_identity(node_id)
    except ValueError:
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
    *,
    live: bool = False,
    node_id: str | None = None,
) -> dict:
    validate_api_destination(base_url, live=live)
    if method != "POST" or path not in {"/api/v1/nodes", "/api/v1/provision"}:
        raise ValueError("unsupported simulator provisioning request")
    if path == "/api/v1/provision":
        require_identity(node_id)
    elif payload.get("simulated") is not True or payload.get("node_type") != "ear":
        raise ValueError("only simulator Ear registration is permitted")
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
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context), NoRedirect()
    )
    try:
        with opener.open(request, timeout=15) as response:
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
    source_family = "vehicle"
    classification_confidence = 0.72
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
                "source_family": source_family,
                "confidence": classification_confidence,
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
    return refresh_assessment(observation)


def refresh_assessment(observation: dict[str, Any]) -> dict[str, Any]:
    classification = observation["acoustic"]["classification"]
    report_kind = observation["context"].get("emission_kind", "complete")
    observation["acoustic"]["assessment"] = build_assessment(
        classification["source_family"],
        classification["confidence"],
        path_depth=assessment_path_depth(
            classification["source_family"], report_kind
        ),
    )
    return observation


def attach_l0_with_assessment(
    observation: dict[str, Any],
    *,
    scenario: str,
    emission_kind: str | None = None,
) -> dict[str, Any]:
    from l0_measurements import attach_l0_extension

    updated = attach_l0_extension(
        observation,
        scenario=scenario,
        emission_kind=emission_kind,  # type: ignore[arg-type]
    )
    return refresh_assessment(updated)


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
        live=args.live_mqtt,
        node_id=record["node_id"],
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
    mqtt_options = _mqtt_options(args)
    validate_publish(
        result, observation, "observation", live=args.live_mqtt, **mqtt_options
    )
    validate_publish(
        result,
        {"node_id": result["node_id"]},
        "keepalive",
        live=args.live_mqtt,
        **mqtt_options,
    )
    credentials_path = getattr(
        args, "credentials_path", resolve_repo_path(args.credentials_file)
    )
    save_credentials(credentials_path, result)
    return result


def _mqtt_options(args: argparse.Namespace) -> dict[str, str | int]:
    return {
        key: value
        for key, value in (
            ("mqtt_host", getattr(args, "mqtt_host", None)),
            ("mqtt_port", getattr(args, "mqtt_port", None)),
        )
        if value is not None
    }


def load_or_provision(args: argparse.Namespace, admin_token: str) -> dict:
    credentials_path = getattr(
        args, "credentials_path", resolve_repo_path(args.credentials_file)
    )
    mqtt_options = _mqtt_options(args)
    if args.reuse_credentials:
        if credentials_path.stat().st_mode & 0o077:
            raise PermissionError(
                f"{credentials_path} must not be accessible by group or other users"
            )
        with credentials_path.open(encoding="utf-8") as stream:
            record = json.load(stream)
        if record.get("state") == "pending":
            if len(admin_token) < 32:
                raise ValueError(
                    f"{getattr(args, 'admin_token_env', 'DBMAP_ADMIN_TOKEN')} must be "
                    "configured to resume provisioning"
                )
            validate_api_destination(args.api_url, live=args.live_mqtt)
            return finish_provisioning(args, record, admin_token)
        if record.get("state") == "provisioned":
            validate_publish(
                record, record["observation"], "observation",
                live=args.live_mqtt, **mqtt_options,
            )
            pending = record.get("pending_observation")
            if pending is not None:
                validate_publish(
                    record, pending, "observation",
                    live=args.live_mqtt, **mqtt_options,
                )
            validate_publish(
                record, {"node_id": record["node_id"]}, "keepalive",
                live=args.live_mqtt, **mqtt_options,
            )
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

    if len(admin_token) < 32:
        raise ValueError(
            f"{getattr(args, 'admin_token_env', 'DBMAP_ADMIN_TOKEN')} must be "
            "configured to provision a simulator Ear"
        )
    validate_api_destination(args.api_url, live=args.live_mqtt)
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
        live=args.live_mqtt,
    )
    pending = {
        "state": "pending",
        "node_id": created["node_id"],
        "bootstrap_token": created["bootstrap_token"],
    }
    require_simulator_identity(pending["node_id"])
    save_credentials(credentials_path, pending)
    # Slot letters conventionally map to SIM-EAR-001…007. Refuse unexpected ids
    # so we never silently bind e.g. slot d to an unrelated device.
    ear_slot = getattr(args, "ear", None)
    if ear_slot in EAR_SLOTS:
        assert_node_id_matches_slot(ear_slot, pending["node_id"])
    return finish_provisioning(args, pending, admin_token)


def publish_observation(
    credentials: dict,
    ca_file: str,
    *,
    live: bool = False,
    mqtt_host: str | None = None,
    mqtt_port: int | None = None,
) -> str:
    return publish_payload(
        credentials,
        credentials["observation"],
        ca_file,
        live=live,
        mqtt_host=mqtt_host,
        mqtt_port=mqtt_port,
    )


def publish_payload(
    credentials: dict, payload: dict, ca_file: str, *,
    live: bool = False, test_context: bool = False,
    mqtt_host: str | None = None, mqtt_port: int | None = None,
) -> str:
    client_id = validate_publish(
        credentials,
        payload,
        "observation",
        live=live,
        test_context=test_context,
        mqtt_host=mqtt_host,
        mqtt_port=mqtt_port,
    )
    endpoint = credentials["mqtt"]
    host, port = resolve_mqtt_destination(
        endpoint, mqtt_host=mqtt_host, mqtt_port=mqtt_port, live=live
    )
    topic = credentials["topics"]["observation"]
    _observation_body().model_validate(payload)

    mqtt = _mqtt_client()
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
        protocol=mqtt.MQTTv311,
    )
    client.username_pw_set(endpoint["username"], endpoint["password"])
    client.tls_set(ca_certs=ca_file, cert_reqs=ssl.CERT_REQUIRED)
    try:
        client.connect(host, port, keepalive=30)
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
    live: bool = False,
    mqtt_host: str | None = None,
    mqtt_port: int | None = None,
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
        validate_publish(
            credentials,
            pending,
            "observation",
            live=live,
            mqtt_host=mqtt_host,
            mqtt_port=mqtt_port,
        )
        credentials["pending_observation"] = pending
        save_credentials(credentials_path, credentials)

        mqtt_options = {
            key: value
            for key, value in (
                ("mqtt_host", mqtt_host),
                ("mqtt_port", mqtt_port),
            )
            if value is not None
        }
        observation_id = publish_payload(
            credentials, pending, ca_file, live=live, **mqtt_options
        )
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
        choices=EAR_SLOTS,
        default=DEFAULT_EAR,
        help=slot_help_text(),
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
    _add_hub_arguments(parser)
    add_ca_argument(parser)
    add_mqtt_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)

    add_cmd = commands.add_parser(
        "add",
        help="Register a new simulated Ear and publish one observation",
    )
    _add_ear_selection_args(add_cmd)
    _add_publish_options(add_cmd)
    _add_hub_arguments(add_cmd, subcommand=True)
    add_ca_argument(add_cmd, subcommand=True)
    add_mqtt_arguments(add_cmd, subcommand=True)
    add_live_mqtt_argument(add_cmd)

    publish_cmd = commands.add_parser(
        "publish",
        help="Publish using an existing shared/simulator/sim-ear-*.json credentials file",
    )
    _add_ear_selection_args(publish_cmd)
    _add_publish_options(publish_cmd)
    _add_hub_arguments(publish_cmd, subcommand=True)
    add_ca_argument(publish_cmd, subcommand=True)
    add_mqtt_arguments(publish_cmd, subcommand=True)
    add_live_mqtt_argument(publish_cmd)
    return parser


def _add_hub_arguments(
    parser: argparse.ArgumentParser, *, subcommand: bool = False
) -> None:
    default = argparse.SUPPRESS if subcommand else None
    parser.add_argument(
        "--hub-host",
        default=default
        if subcommand
        else os.environ.get("DBMAP_LAN_IP")
        or os.environ.get("DBMAP_LOCAL_MQTT_HOST", "hub.local"),
        help="Configured local Hub host (defaults from project .env)",
    )
    parser.add_argument(
        "--hub-port",
        type=int,
        default=default
        if subcommand
        else int(os.environ.get("DBMAP_HTTPS_PORT", str(HUB_PORT))),
        help="Local Hub HTTPS port (default: DBMAP_HTTPS_PORT or 8443)",
    )
    parser.add_argument(
        "--base-path",
        default=default
        if subcommand
        else os.environ.get("DBMAP_PUBLIC_BASE_PATH", ""),
        help="Configured Hub base path",
    )


def main() -> int:
    try:
        load_simulator_environment()
    except ValueError as exc:
        print(f"Observation configuration failed: {exc}", file=sys.stderr)
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
    args.credentials_path = resolve_repo_path(args.credentials_file)
    base_path = args.base_path.rstrip("/")
    if base_path and (
        not base_path.startswith("/")
        or "//" in base_path
        or any(part in {".", ".."} for part in base_path.split("/"))
    ):
        parser.error("--base-path must be empty or a normalized absolute path")
    args.api_url = f"https://{args.hub_host}:{args.hub_port}{base_path}"
    admin_token_env, admin_token = admin_token_for_destination(args.api_url)
    args.admin_token_env = admin_token_env
    credentials_path = args.credentials_path
    ca_file = resolve_repo_path(args.ca)
    args.ca = str(ca_file)
    if not ca_file.is_file():
        parser.error(f"CA file does not exist: {ca_file}")
    if args.command == "add":
        if len(admin_token) < 32:
            parser.error(
                f"{admin_token_env} must be configured in the project .env file"
            )
        try:
            validate_api_destination(args.api_url, live=args.live_mqtt)
        except ValueError as exc:
            parser.error(str(exc))
    mqtt_options = _mqtt_options(args)
    try:
        credentials = load_or_provision(args, admin_token)
        node_id = credentials["node_id"]
        mqtt_host, mqtt_port = resolve_mqtt_destination(
            credentials["mqtt"],
            live=args.live_mqtt,
            **mqtt_options,
        )
        print(f"MQTT destination: TLS {mqtt_host}:{mqtt_port}", flush=True)
        print(
            f"Publishing {args.count} observation(s) from {node_id} "
            f"(--ear {args.ear}):",
            flush=True,
        )
        publish_ear_keepalive(
            credentials,
            str(ca_file),
            uptime_s=0,
            live=args.live_mqtt,
            **mqtt_options,
        )
        print(f"  keepalive  {args.ear}  {node_id}", flush=True)
        if args.count == 1:
            if args.include_l0:
                pending = credentials.get("pending_observation") or credentials[
                    "observation"
                ]
                pending = attach_l0_with_assessment(
                    pending, scenario=args.l0_scenario
                )
                credentials["pending_observation"] = pending
                credentials["observation"] = pending
                save_credentials(credentials_path, credentials)
                observation_id = publish_payload(
                    credentials,
                    pending,
                    str(ca_file),
                    live=args.live_mqtt,
                    **mqtt_options,
                )
            else:
                observation_id = publish_payload(
                    credentials,
                    credentials["observation"],
                    str(ca_file),
                    live=args.live_mqtt,
                    **mqtt_options,
                )
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
                str(ca_file),
                credentials_path,
                args.count,
                args.interval_seconds,
                args.vehicle_id,
                ear=args.ear,
                include_l0=args.include_l0,
                l0_scenario=args.l0_scenario,
                live=args.live_mqtt,
                **mqtt_options,
            )
    except UnsupportedSimulatorIdentity as exc:
        print(f"Simulator setup failed: {exc}", file=sys.stderr)
        print(
            f"The pending node record is in {args.credentials_file}; "
            "use a new --ear / credentials-file path after redeploying the hub.",
            file=sys.stderr,
        )
        return 1
    except SimEarSlotError as exc:
        print(f"Simulator slot binding failed: {exc}", file=sys.stderr)
        print(
            f"Pending/partial credentials (if any): {args.credentials_file}",
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
