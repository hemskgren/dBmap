#!/usr/bin/env python3
import argparse
import json
import math
import re
import sys
import time
from datetime import datetime, timedelta, timezone
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from typing import Any, Callable
from uuid import uuid4

from app.schemas import ObservationBody
from paho.mqtt import MQTTException

_simulator_spec = spec_from_file_location(
    "dbmap_ear_simulator",
    Path(__file__).with_name("simulate_observation.py"),
)
if _simulator_spec is None or _simulator_spec.loader is None:
    raise RuntimeError("could not load shared Ear simulator helpers")
_simulator_module = module_from_spec(_simulator_spec)
_simulator_spec.loader.exec_module(_simulator_module)
publish_payload = _simulator_module.publish_payload
save_credentials = _simulator_module.save_credentials


class ScenarioError(ValueError):
    pass


_SCENARIO_FIELDS = {"scenario_id", "source_id", "observations"}
_OBSERVATION_FIELDS = {
    "at_seconds",
    "ear",
    "bearing_deg",
    "signal_level_dbfs",
    "duration_ms",
}


def _finite_number(value: Any, field: str, *, minimum: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ScenarioError(f"{field} must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise ScenarioError(f"{field} must be finite")
    if minimum is not None and number < minimum:
        raise ScenarioError(f"{field} must be at least {minimum}")
    return number


def load_scenario(path: Path) -> dict[str, Any]:
    try:
        scenario = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ScenarioError(f"could not read scenario JSON: {exc}") from exc
    if not isinstance(scenario, dict):
        raise ScenarioError("scenario must be a JSON object")
    unexpected = set(scenario) - _SCENARIO_FIELDS
    missing = _SCENARIO_FIELDS - set(scenario)
    if unexpected or missing:
        raise ScenarioError(
            f"scenario fields mismatch; missing={sorted(missing)}, unexpected={sorted(unexpected)}"
        )
    for field in ("scenario_id", "source_id"):
        value = scenario[field]
        if not isinstance(value, str) or not value.strip() or len(value) > 64:
            raise ScenarioError(f"{field} must be a non-empty string of at most 64 characters")

    observations = scenario["observations"]
    if not isinstance(observations, list) or not observations:
        raise ScenarioError("observations must be a non-empty list")
    previous_time = -1.0
    normalized = []
    for index, observation in enumerate(observations):
        if not isinstance(observation, dict):
            raise ScenarioError(f"observations[{index}] must be an object")
        unexpected = set(observation) - _OBSERVATION_FIELDS
        missing = {"at_seconds", "ear"} - set(observation)
        if unexpected or missing:
            raise ScenarioError(
                f"observations[{index}] fields mismatch; "
                f"missing={sorted(missing)}, unexpected={sorted(unexpected)}"
            )
        at_seconds = _finite_number(
            observation["at_seconds"],
            f"observations[{index}].at_seconds",
            minimum=0,
        )
        if at_seconds < previous_time:
            raise ScenarioError("observations must be ordered by non-decreasing at_seconds")
        previous_time = at_seconds
        ear = observation["ear"]
        if not isinstance(ear, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", ear):
            raise ScenarioError(f"observations[{index}].ear must be a valid Ear alias")
        item = {"at_seconds": at_seconds, "ear": ear}
        if "bearing_deg" in observation:
            bearing = _finite_number(
                observation["bearing_deg"],
                f"observations[{index}].bearing_deg",
            )
            if not 0 <= bearing < 360:
                raise ScenarioError(f"observations[{index}].bearing_deg must be in [0, 360)")
            item["bearing_deg"] = bearing
        if "signal_level_dbfs" in observation:
            signal_level = _finite_number(
                observation["signal_level_dbfs"],
                f"observations[{index}].signal_level_dbfs",
            )
            if signal_level > 0:
                raise ScenarioError(
                    f"observations[{index}].signal_level_dbfs must be at most 0"
                )
            item["signal_level_dbfs"] = signal_level
        if "duration_ms" in observation:
            item["duration_ms"] = _finite_number(
                observation["duration_ms"],
                f"observations[{index}].duration_ms",
                minimum=0,
            )
        normalized.append(item)
    return {
        "scenario_id": scenario["scenario_id"],
        "source_id": scenario["source_id"],
        "observations": normalized,
    }


def parse_ear_arguments(values: list[str]) -> dict[str, Path]:
    ears: dict[str, Path] = {}
    for value in values:
        alias, separator, path = value.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", alias) or not path:
            raise ScenarioError("--ear must use the form ALIAS=/path/to/credentials.json")
        if alias in ears:
            raise ScenarioError(f"duplicate --ear alias: {alias}")
        ears[alias] = Path(path)
    return ears


def validate_ear_aliases(scenario: dict[str, Any], ears: dict[str, Path]) -> None:
    required = {item["ear"] for item in scenario["observations"]}
    missing = required - ears.keys()
    unused = ears.keys() - required
    if missing or unused:
        raise ScenarioError(
            f"Ear aliases mismatch; missing={sorted(missing)}, unused={sorted(unused)}"
        )


def load_ear_credentials(
    paths: dict[str, Path],
    *,
    allow_pending: bool = False,
) -> dict[str, dict[str, Any]]:
    credentials_by_alias = {}
    node_ids = set()
    for alias, path in paths.items():
        try:
            if path.stat().st_mode & 0o077:
                raise PermissionError(f"{path} must not be accessible by group or other users")
            credentials = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ScenarioError(f"could not load credentials for {alias}: {exc}") from exc
        if not isinstance(credentials, dict) or credentials.get("state") != "provisioned":
            raise ScenarioError(f"credentials for {alias} are not in the provisioned state")
        node_id = credentials.get("node_id")
        if not isinstance(node_id, str) or not node_id.startswith("SIM-EAR-"):
            raise ScenarioError(f"credentials for {alias} must belong to a SIM-EAR-* node")
        if node_id in node_ids:
            raise ScenarioError(f"each Ear alias must use a distinct node; repeated node {node_id}")
        node_ids.add(node_id)
        endpoint = credentials.get("mqtt")
        topics = credentials.get("topics")
        if not isinstance(endpoint, dict) or endpoint.get("use_tls") is not True:
            raise ScenarioError(f"credentials for {alias} do not contain a TLS MQTT endpoint")
        host = endpoint.get("host")
        port = endpoint.get("port")
        username = endpoint.get("username")
        password = endpoint.get("password")
        if (
            not isinstance(host, str)
            or not host
            or isinstance(port, bool)
            or not isinstance(port, int)
            or not 1 <= port <= 65535
            or not isinstance(username, str)
            or not username
            or not isinstance(password, str)
            or not password
        ):
            raise ScenarioError(f"credentials for {alias} contain an invalid MQTT endpoint")
        if not isinstance(topics, dict) or topics.get("observation") != (
            f"esp-ear/local/{node_id.lower()}/observation"
        ):
            raise ScenarioError(f"credentials for {alias} contain an invalid observation topic")
        pending = credentials.get("pending_observation")
        if pending is not None and not allow_pending:
            raise ScenarioError(
                f"{path} has a pending observation; retry or resolve it before running a scenario"
            )
        if pending is not None and credentials.get("pending_scenario_id") is None:
            raise ScenarioError(f"{path} has a pending observation from another simulator mode")
        saved_observation = credentials.get("observation")
        if not isinstance(saved_observation, dict):
            raise ScenarioError(f"credentials for {alias} have no saved observation")
        saved_sequence = saved_observation.get("sequence_number")
        if (
            isinstance(saved_sequence, bool)
            or not isinstance(saved_sequence, int)
            or saved_sequence < 0
        ):
            raise ScenarioError(f"credentials for {alias} have an invalid saved sequence number")
        next_sequence = credentials.get(
            "next_sequence_number",
            saved_sequence + 1,
        )
        if isinstance(next_sequence, bool) or not isinstance(next_sequence, int) or next_sequence < 0:
            raise ScenarioError(f"credentials for {alias} have an invalid next sequence number")
        credentials["next_sequence_number"] = next_sequence
        credentials_by_alias[alias] = credentials
    return credentials_by_alias


def make_scenario_observation(
    node_id: str,
    sequence_number: int,
    source_id: str,
    event: dict[str, Any],
    started_at_utc: datetime,
    started_at_monotonic_us: int,
) -> dict[str, Any]:
    at_seconds = event["at_seconds"]
    payload: dict[str, Any] = {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": str(uuid4()),
        "node_id": node_id,
        "sequence_number": sequence_number,
        "event_time_utc": (started_at_utc + timedelta(seconds=at_seconds)).isoformat(),
        "capture_timestamp_monotonic_us": started_at_monotonic_us + round(at_seconds * 1_000_000),
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "classification": {
            "source_family": "vehicle",
            "confidence": 0.72,
            "hints": {"simulated_source_id": source_id},
        },
        "duration_ms": event.get("duration_ms", 850.0),
    }
    if "bearing_deg" in event:
        payload["bearing"] = {
            "deg": event["bearing_deg"],
            "reference": "node",
            "confidence": 0.61,
        }
    if "signal_level_dbfs" in event:
        payload["signal_level_dbfs"] = event["signal_level_dbfs"]
    ObservationBody.model_validate(payload)
    return payload


def run_scenario(
    scenario: dict[str, Any],
    credentials_by_alias: dict[str, dict[str, Any]],
    credentials_paths: dict[str, Path],
    ca_file: str,
    time_scale: float,
    *,
    publish: Callable[[dict[str, Any], dict[str, Any], str], str] = publish_payload,
    sleep: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
    started_at_utc: datetime | None = None,
    started_at_monotonic_us: int | None = None,
    save: Callable[..., None] = save_credentials,
) -> list[str]:
    if not math.isfinite(time_scale) or time_scale <= 0:
        raise ScenarioError("time scale must be finite and greater than zero")
    scenario_start = started_at_utc or datetime.now(timezone.utc)
    monotonic_start = (
        started_at_monotonic_us
        if started_at_monotonic_us is not None
        else time.monotonic_ns() // 1000
    )
    wall_start = monotonic()
    published_ids = []
    for event in scenario["observations"]:
        target_elapsed = event["at_seconds"] * time_scale
        delay = target_elapsed - (monotonic() - wall_start)
        if delay > 0:
            sleep(delay)

        alias = event["ear"]
        credentials = credentials_by_alias[alias]
        payload = make_scenario_observation(
            credentials["node_id"],
            credentials["next_sequence_number"],
            scenario["source_id"],
            event,
            scenario_start,
            monotonic_start,
        )
        credentials["pending_observation"] = payload
        credentials["pending_scenario_id"] = scenario["scenario_id"]
        save(credentials_paths[alias], credentials)

        observation_id = publish(credentials, payload, ca_file)
        published_ids.append(observation_id)
        credentials["next_sequence_number"] = max(
            credentials["next_sequence_number"],
            payload["sequence_number"] + 1,
        )
        credentials.pop("pending_observation", None)
        credentials.pop("pending_scenario_id", None)
        save(credentials_paths[alias], credentials)
    return published_ids


def print_schedule(scenario: dict[str, Any]) -> None:
    started_at = datetime.now(timezone.utc)
    started_at_monotonic_us = time.monotonic_ns() // 1000
    sequences: dict[str, int] = {}
    for event in scenario["observations"]:
        alias = event["ear"]
        sequence = sequences.get(alias, 1)
        make_scenario_observation(
            f"SIM-EAR-DRYRUN-{alias}",
            sequence,
            scenario["source_id"],
            event,
            started_at,
            started_at_monotonic_us,
        )
        sequences[alias] = sequence + 1
    print(f"Scenario: {scenario['scenario_id']}")
    print(f"Simulated source hint: {scenario['source_id']}")
    print("No Hub event, track, or geographic position is generated.")
    for index, event in enumerate(scenario["observations"], start=1):
        print(
            f"{index:02d}  t={event['at_seconds']:g}s  Ear={event['ear']}  "
            f"bearing={event.get('bearing_deg', 'not set')}  "
            f"level={event.get('signal_level_dbfs', 'not set')} dBFS  "
            f"duration={event.get('duration_ms', 850.0):g}ms"
        )


def retry_pending_observations(
    scenario: dict[str, Any],
    credentials_by_alias: dict[str, dict[str, Any]],
    credentials_paths: dict[str, Path],
    ca_file: str,
    *,
    publish: Callable[[dict[str, Any], dict[str, Any], str], str] = publish_payload,
    save: Callable[..., None] = save_credentials,
) -> list[str]:
    pending_aliases = [
        alias for alias, credentials in credentials_by_alias.items()
        if credentials.get("pending_observation") is not None
    ]
    if not pending_aliases:
        raise ScenarioError("no pending scenario observations to retry")
    ids = []
    for alias in pending_aliases:
        credentials = credentials_by_alias[alias]
        if credentials.get("pending_scenario_id") != scenario["scenario_id"]:
            raise ScenarioError(
                f"pending observation for {alias} belongs to another scenario"
            )
        payload = credentials["pending_observation"]
        ObservationBody.model_validate(payload)
        if payload["node_id"] != credentials["node_id"]:
            raise ScenarioError(f"pending observation for {alias} belongs to another Ear")
        hints = (payload.get("classification") or {}).get("hints")
        if not isinstance(hints, dict) or hints.get("simulated_source_id") != scenario["source_id"]:
            raise ScenarioError(
                f"pending observation for {alias} has a different simulated source hint"
            )
        observation_id = publish(credentials, payload, ca_file)
        ids.append(observation_id)
        credentials["next_sequence_number"] = max(
            credentials["next_sequence_number"],
            payload["sequence_number"] + 1,
        )
        credentials.pop("pending_observation", None)
        credentials.pop("pending_scenario_id", None)
        save(credentials_paths[alias], credentials)
    return ids


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Publish a time-ordered scenario of Ear observations over TLS MQTT."
    )
    parser.add_argument("--scenario-file", required=True, help="Scenario definition in JSON")
    parser.add_argument(
        "--ear",
        action="append",
        default=[],
        metavar="ALIAS=CREDENTIALS_FILE",
        help="Map a scenario Ear alias to a private provisioned simulator credentials file",
    )
    parser.add_argument(
        "--time-scale",
        type=float,
        default=1.0,
        help="Scale wall-clock waits; logical event times remain unchanged (default: 1)",
    )
    parser.add_argument("--ca", default="mqtt/certs/ca.crt", help="Path to the local hub CA")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and print the timeline without loading credentials or publishing",
    )
    parser.add_argument(
        "--retry-pending",
        action="store_true",
        help="Retry a saved in-flight observation from this scenario, then exit",
    )
    args = parser.parse_args()

    try:
        if args.dry_run and args.retry_pending:
            raise ScenarioError("--dry-run and --retry-pending cannot be combined")
        scenario = load_scenario(Path(args.scenario_file))
        if not math.isfinite(args.time_scale) or args.time_scale <= 0:
            raise ScenarioError("--time-scale must be finite and greater than zero")
        ear_paths = parse_ear_arguments(args.ear)
        validate_ear_aliases(scenario, ear_paths)
        if args.dry_run:
            print_schedule(scenario)
            return 0

        if not Path(args.ca).is_file():
            raise ScenarioError(f"CA file does not exist: {args.ca}")
        credentials_by_alias = load_ear_credentials(
            ear_paths,
            allow_pending=args.retry_pending,
        )
        if args.retry_pending:
            ids = retry_pending_observations(
                scenario,
                credentials_by_alias,
                ear_paths,
                args.ca,
            )
            print(f"Retried {len(ids)} pending observation(s):")
            for observation_id in ids:
                print(f"  {observation_id}")
            return 0
        ids = run_scenario(
            scenario,
            credentials_by_alias,
            ear_paths,
            args.ca,
            args.time_scale,
        )
    except KeyboardInterrupt:
        print("Scenario interrupted. Any unpublished pending observation is preserved in its credentials file.")
        return 130
    except (KeyError, MQTTException, OSError, RuntimeError, ScenarioError, ValueError) as exc:
        print(f"Scenario simulation failed: {exc}", file=sys.stderr)
        return 1

    print(f"Scenario {scenario['scenario_id']} published {len(ids)} observations:")
    for observation_id in ids:
        print(f"  {observation_id}")
    print("Use scripts/hub_event_track.py to inspect the observations; no Hub logic was deployed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
