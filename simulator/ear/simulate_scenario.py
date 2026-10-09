#!/usr/bin/env python3
import argparse
import json
import math
import re
import sys
import time
from collections.abc import Callable
from datetime import datetime, timedelta, timezone
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from typing import Any
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


_SCENARIO_FIELDS = {"scenario_id", "source", "ears", "observations"}
_SOURCE_FIELDS = {"ground_truth_id", "source_family", "movement"}
_MOVEMENT_FIELDS = {"start", "end", "speed_m_s"}
_POINT_FIELDS = {"latitude", "longitude"}
_EAR_FIELDS = {"latitude", "longitude", "orientation_deg"}
_OBSERVATION_FIELDS = {
    "at_seconds",
    "ear",
    "bearing_confidence",
    "classification_confidence",
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


def _validate_point(value: Any, field: str) -> dict[str, float]:
    if not isinstance(value, dict) or set(value) != _POINT_FIELDS:
        raise ScenarioError(f"{field} must contain exactly latitude and longitude")
    latitude = _finite_number(value["latitude"], f"{field}.latitude")
    longitude = _finite_number(value["longitude"], f"{field}.longitude")
    if not -90 <= latitude <= 90:
        raise ScenarioError(f"{field}.latitude must be in [-90, 90]")
    if not -180 <= longitude <= 180:
        raise ScenarioError(f"{field}.longitude must be in [-180, 180]")
    return {"latitude": latitude, "longitude": longitude}


def _distance_m(first: dict[str, float], second: dict[str, float]) -> float:
    lat1 = math.radians(first["latitude"])
    lat2 = math.radians(second["latitude"])
    lat_delta = lat2 - lat1
    lon_delta = math.radians(second["longitude"] - first["longitude"])
    haversine = (
        math.sin(lat_delta / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(lon_delta / 2) ** 2
    )
    return 6_371_000 * 2 * math.asin(math.sqrt(min(1.0, haversine)))


def _position_on_route(
    start: dict[str, float],
    end: dict[str, float],
    fraction: float,
) -> dict[str, float]:
    start_lat = math.radians(start["latitude"])
    start_lon = math.radians(start["longitude"])
    end_lat = math.radians(end["latitude"])
    end_lon = math.radians(end["longitude"])
    start_vector = (
        math.cos(start_lat) * math.cos(start_lon),
        math.cos(start_lat) * math.sin(start_lon),
        math.sin(start_lat),
    )
    end_vector = (
        math.cos(end_lat) * math.cos(end_lon),
        math.cos(end_lat) * math.sin(end_lon),
        math.sin(end_lat),
    )
    dot = max(-1.0, min(1.0, sum(a * b for a, b in zip(start_vector, end_vector))))
    angle = math.acos(dot)
    if angle < 1e-12:
        return dict(start)
    sine = math.sin(angle)
    first_weight = math.sin((1 - fraction) * angle) / sine
    second_weight = math.sin(fraction * angle) / sine
    x, y, z = (
        first_weight * start_vector[index] + second_weight * end_vector[index]
        for index in range(3)
    )
    return {
        "latitude": math.degrees(math.atan2(z, math.hypot(x, y))),
        "longitude": math.degrees(math.atan2(y, x)),
    }


def _bearing_deg(first: dict[str, float], second: dict[str, float]) -> float:
    lat1 = math.radians(first["latitude"])
    lat2 = math.radians(second["latitude"])
    lon_delta = math.radians(second["longitude"] - first["longitude"])
    y = math.sin(lon_delta) * math.cos(lat2)
    x = (
        math.cos(lat1) * math.sin(lat2)
        - math.sin(lat1) * math.cos(lat2) * math.cos(lon_delta)
    )
    return math.degrees(math.atan2(y, x)) % 360


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
    scenario_id = scenario["scenario_id"]
    if not isinstance(scenario_id, str) or not scenario_id.strip() or len(scenario_id) > 64:
        raise ScenarioError("scenario_id must be a non-empty string of at most 64 characters")

    source = scenario["source"]
    if not isinstance(source, dict) or set(source) != _SOURCE_FIELDS:
        raise ScenarioError(
            f"source must contain exactly {sorted(_SOURCE_FIELDS)}"
        )
    source_id = source["ground_truth_id"]
    source_family = source["source_family"]
    if not isinstance(source_id, str) or not source_id.strip() or len(source_id) > 64:
        raise ScenarioError("source.ground_truth_id must be a non-empty string of at most 64 characters")
    if not isinstance(source_family, str) or not source_family.strip() or len(source_family) > 64:
        raise ScenarioError("source.source_family must be a non-empty string of at most 64 characters")
    movement = source["movement"]
    if not isinstance(movement, dict) or set(movement) != _MOVEMENT_FIELDS:
        raise ScenarioError(
            f"source.movement must contain exactly {sorted(_MOVEMENT_FIELDS)}"
        )
    start = _validate_point(movement["start"], "source.movement.start")
    end = _validate_point(movement["end"], "source.movement.end")
    speed = _finite_number(movement["speed_m_s"], "source.movement.speed_m_s", minimum=0)
    if speed == 0:
        raise ScenarioError("source.movement.speed_m_s must be greater than zero")
    route_distance_m = _distance_m(start, end)
    if route_distance_m <= 0:
        raise ScenarioError("source movement start and end positions must be different")
    route_duration_seconds = route_distance_m / speed

    ears_raw = scenario["ears"]
    if not isinstance(ears_raw, dict) or not ears_raw:
        raise ScenarioError("ears must be a non-empty object keyed by scenario alias")
    ears: dict[str, dict[str, float]] = {}
    for alias, ear_raw in ears_raw.items():
        if not isinstance(alias, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", alias):
            raise ScenarioError(f"invalid Ear alias in ears: {alias!r}")
        if not isinstance(ear_raw, dict) or set(ear_raw) != _EAR_FIELDS:
            raise ScenarioError(
                f"ears.{alias} must contain exactly {sorted(_EAR_FIELDS)}"
            )
        point = _validate_point(
            {"latitude": ear_raw["latitude"], "longitude": ear_raw["longitude"]},
            f"ears.{alias}",
        )
        orientation = _finite_number(
            ear_raw["orientation_deg"],
            f"ears.{alias}.orientation_deg",
        )
        if not 0 <= orientation < 360:
            raise ScenarioError(f"ears.{alias}.orientation_deg must be in [0, 360)")
        ears[alias] = {**point, "orientation_deg": orientation}

    observations = scenario["observations"]
    if not isinstance(observations, list) or not observations:
        raise ScenarioError("observations must be a non-empty list")
    previous_time = -1.0
    normalized = []
    for index, observation in enumerate(observations):
        if not isinstance(observation, dict):
            raise ScenarioError(f"observations[{index}] must be an object")
        unexpected = set(observation) - _OBSERVATION_FIELDS
        missing = {"at_seconds", "ear", "bearing_confidence", "classification_confidence"} - set(
            observation
        )
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
        if ear not in ears:
            raise ScenarioError(f"observations[{index}].ear is not declared in ears")
        if at_seconds > route_duration_seconds:
            raise ScenarioError(
                f"observations[{index}].at_seconds exceeds source movement duration "
                f"({route_duration_seconds:.3f}s)"
            )
        item = {"at_seconds": at_seconds, "ear": ear}
        item["source_family"] = source_family
        classification_confidence = _finite_number(
            observation["classification_confidence"],
            f"observations[{index}].classification_confidence",
            minimum=0,
        )
        if classification_confidence > 1:
            raise ScenarioError(
                f"observations[{index}].classification_confidence must be at most 1"
            )
        item["classification_confidence"] = classification_confidence
        bearing_confidence = _finite_number(
            observation["bearing_confidence"],
            f"observations[{index}].bearing_confidence",
            minimum=0,
        )
        if bearing_confidence > 1:
            raise ScenarioError(
                f"observations[{index}].bearing_confidence must be at most 1"
            )
        item["bearing_confidence"] = bearing_confidence
        source_position = _position_on_route(
            start,
            end,
            at_seconds / route_duration_seconds,
        )
        ear_position = ears[ear]
        distance_m = _distance_m(ear_position, source_position)
        if distance_m <= 1e-6:
            raise ScenarioError(
                f"observations[{index}] source position coincides with Ear position; "
                "bearing is undefined"
            )
        true_bearing = _bearing_deg(ear_position, source_position)
        item["bearing_deg"] = (true_bearing - ear_position["orientation_deg"]) % 360
        item["source_distance_m"] = distance_m
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
        "scenario_id": scenario_id,
        "source_id": source_id,
        "source_family": source_family,
        "movement": {
            "start": start,
            "end": end,
            "speed_m_s": speed,
            "distance_m": route_distance_m,
            "duration_seconds": route_duration_seconds,
        },
        "ears": ears,
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
            "source_family": event.get("source_family", "vehicle"),
            "confidence": event["classification_confidence"],
            "hints": {"simulated_source_id": source_id},
        },
        "duration_ms": event.get("duration_ms", 850.0),
    }
    if "bearing_deg" in event:
        payload["bearing"] = {
            "deg": event["bearing_deg"],
            "reference": "node",
            "confidence": event["bearing_confidence"],
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
    print(
        f"Ground truth source: {scenario['source_id']} "
        f"({scenario['source_family']}; test metadata only)"
    )
    movement = scenario["movement"]
    print(
        f"Movement: {movement['distance_m']:.1f}m at {movement['speed_m_s']:g}m/s "
        f"({movement['duration_seconds']:.1f}s)"
    )
    print("No Hub event, track, or geographic position is generated.")
    for index, event in enumerate(scenario["observations"], start=1):
        print(
            f"{index:02d}  t={event['at_seconds']:g}s  Ear={event['ear']}  "
            f"bearing={event['bearing_deg']:.1f} deg "
            f"(confidence={event.get('bearing_confidence', 'not set')})  "
            f"classification confidence={event['classification_confidence']}  "
            f"ground-truth distance={event['source_distance_m']:.1f}m  "
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
