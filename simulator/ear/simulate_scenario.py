#!/usr/bin/env python3
import argparse
import json
import math
import re
import sys
import time
from collections import defaultdict
from collections.abc import Callable
from datetime import UTC, datetime, timedelta
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from typing import Any
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
    resolve_profile_credential_paths,
)
from independent_detection import (
    DEFAULT_OBSERVATION_INTERVAL_S,
    DEFAULT_PATH_LOSS_EXPONENT,
    DEFAULT_REFERENCE_DISTANCE_M,
    DEFAULT_REFERENCE_LEVEL_DBFS,
    ObservationEmissionPolicy,
    RangeDetectionConfig,
    SignalLevelConfig,
    format_independent_detection_diagnostics,
    generate_independent_detection_events,
)
from lib.hub_cli import (
    load_project_env,
    print_help_without_arguments,
)
from scripted_cadence_profile import (
    ScriptedCadenceProfileError,
    apply_scripted_cadence_profile,
    known_scripted_cadence_profile_ids,
)

_simulator_module = None
_keepalive_module = None

SIMULATION_MODES = ("scripted", "independent_detection")
DEFAULT_INDEPENDENT_MAX_RANGE_M = 4000.0
DEFAULT_INDEPENDENT_TIME_STEP_S = 1.0


def _load_sibling_module(module_name: str, filename: str):
    spec = spec_from_file_location(module_name, Path(__file__).with_name(filename))
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {filename}")
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _observation_helpers():
    """Load MQTT publish helpers only when a command needs them (not for --help)."""
    global _simulator_module
    if _simulator_module is None:
        _simulator_module = _load_sibling_module(
            "dbmap_ear_simulator", "simulate_observation.py"
        )
    return _simulator_module.publish_payload, _simulator_module.save_credentials


def _keepalive_helpers():
    """Load keepalive helpers only when a command needs them (not for --help)."""
    global _keepalive_module
    if _keepalive_module is None:
        _keepalive_module = _load_sibling_module(
            "dbmap_ear_keepalive", "simulate_keepalive.py"
        )
    return _keepalive_module.make_keepalive, _keepalive_module.publish_keepalive


def _observation_body():
    from app.schemas import ObservationBody

    return ObservationBody


class ScenarioError(ValueError):
    pass


def ear_shortname(alias: str) -> str:
    if alias.startswith("ear_"):
        return alias[4:]
    return alias


def run_wall_clock_seconds(scenario: dict[str, Any], time_scale: float) -> float:
    if scenario["observations"]:
        last_at = scenario["observations"][-1]["at_seconds"]
    else:
        last_at = scenario["movement"]["duration_seconds"]
    return last_at * time_scale


def print_scenario_banner(
    scenario: dict[str, Any],
    *,
    count: int | None = None,
    time_scale: float | None = None,
) -> None:
    observation_count = (
        len(scenario["observations"]) if count is None else count
    )
    mode = scenario.get("simulation_mode", "scripted")
    print(
        f"Scenario {scenario['scenario_id']} publishing {observation_count} observations "
        f"(mode={mode}):",
        flush=True,
    )
    movement = scenario["movement"]
    print(
        f"  source_family={scenario['source_family']}  "
        f"speed={movement['speed_m_s']:g} m/s  "
        f"source={scenario['source_id']}",
        flush=True,
    )
    if time_scale is not None:
        wall = run_wall_clock_seconds(scenario, time_scale)
        print(
            f"  movement={movement['distance_m']:.1f}m / "
            f"{movement['duration_seconds']:.1f}s logical  "
            f"wall-clock≈{wall:.1f}s at --time-scale {time_scale:g}",
            flush=True,
        )


def publish_keepalives_for_ears(
    credentials_by_alias: dict[str, dict[str, Any]],
    ca_file: str,
    *,
    label: str,
    publish_keepalive: Callable[[dict[str, Any], str, dict[str, Any]], None]
    | None = None,
    make_keepalive: Callable[[dict[str, Any], int], dict[str, Any]] | None = None,
    uptime_s: int = 0,
) -> None:
    if publish_keepalive is None or make_keepalive is None:
        default_make, default_publish = _keepalive_helpers()
        if make_keepalive is None:
            make_keepalive = default_make
        if publish_keepalive is None:
            publish_keepalive = default_publish
    print(f"  keepalive ({label}):", flush=True)
    for alias in sorted(credentials_by_alias):
        credentials = credentials_by_alias[alias]
        publish_keepalive(
            credentials, ca_file, make_keepalive(credentials, uptime_s)
        )
        print(
            f"    {ear_shortname(alias)}  {credentials['node_id']}",
            flush=True,
        )


_SCENARIO_FIELDS = {"scenario_id", "source", "ears", "observations"}
_SOURCE_FIELDS = {"ground_truth_id", "source_family", "movement"}
_MOVEMENT_FIELDS = {"start", "end", "speed_m_s"}
_POINT_FIELDS = {"latitude", "longitude"}
_EAR_FIELDS = {"latitude", "longitude", "orientation_deg"}
_OBSERVATION_FIELDS = {
    "at_seconds",
    "ear",
    "bearing_confidence",
    "bearing_available",
    "bearing_bias_deg",
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
    x = math.cos(lat1) * math.sin(lat2) - math.sin(lat1) * math.cos(lat2) * math.cos(
        lon_delta
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
    if (
        not isinstance(scenario_id, str)
        or not scenario_id.strip()
        or len(scenario_id) > 64
    ):
        raise ScenarioError(
            "scenario_id must be a non-empty string of at most 64 characters"
        )

    source = scenario["source"]
    if not isinstance(source, dict) or set(source) != _SOURCE_FIELDS:
        raise ScenarioError(f"source must contain exactly {sorted(_SOURCE_FIELDS)}")
    source_id = source["ground_truth_id"]
    source_family = source["source_family"]
    if not isinstance(source_id, str) or not source_id.strip() or len(source_id) > 64:
        raise ScenarioError(
            "source.ground_truth_id must be a non-empty string of at most 64 characters"
        )
    if (
        not isinstance(source_family, str)
        or not source_family.strip()
        or len(source_family) > 64
    ):
        raise ScenarioError(
            "source.source_family must be a non-empty string of at most 64 characters"
        )
    movement = source["movement"]
    if not isinstance(movement, dict) or set(movement) != _MOVEMENT_FIELDS:
        raise ScenarioError(
            f"source.movement must contain exactly {sorted(_MOVEMENT_FIELDS)}"
        )
    start = _validate_point(movement["start"], "source.movement.start")
    end = _validate_point(movement["end"], "source.movement.end")
    speed = _finite_number(
        movement["speed_m_s"], "source.movement.speed_m_s", minimum=0
    )
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
        if not isinstance(alias, str) or not re.fullmatch(
            r"[A-Za-z0-9_-]{1,64}", alias
        ):
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
        if unexpected:
            raise ScenarioError(
                f"observations[{index}] fields mismatch; "
                f"unexpected={sorted(unexpected)}"
            )
        bearing_available = True
        if "bearing_available" in observation:
            raw_available = observation["bearing_available"]
            if not isinstance(raw_available, bool):
                raise ScenarioError(
                    f"observations[{index}].bearing_available must be a boolean"
                )
            bearing_available = raw_available
        required = {
            "at_seconds",
            "ear",
            "classification_confidence",
        }
        if bearing_available:
            required.add("bearing_confidence")
        missing = required - set(observation)
        if missing:
            raise ScenarioError(
                f"observations[{index}] fields mismatch; "
                f"missing={sorted(missing)}"
            )
        if not bearing_available and "bearing_confidence" in observation:
            raise ScenarioError(
                f"observations[{index}]: bearing_confidence must be omitted when "
                "bearing_available is false"
            )
        if not bearing_available and "bearing_bias_deg" in observation:
            raise ScenarioError(
                f"observations[{index}]: bearing_bias_deg requires bearing_available"
            )
        bearing_bias_deg = 0.0
        if "bearing_bias_deg" in observation:
            bearing_bias_deg = _finite_number(
                observation["bearing_bias_deg"],
                f"observations[{index}].bearing_bias_deg",
            )
        at_seconds = _finite_number(
            observation["at_seconds"],
            f"observations[{index}].at_seconds",
            minimum=0,
        )
        if at_seconds < previous_time:
            raise ScenarioError(
                "observations must be ordered by non-decreasing at_seconds"
            )
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
        item = {
            "at_seconds": at_seconds,
            "ear": ear,
            "bearing_available": bearing_available,
        }
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
        item["source_distance_m"] = distance_m
        if bearing_available:
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
            true_bearing = _bearing_deg(ear_position, source_position)
            node_relative = (true_bearing - ear_position["orientation_deg"]) % 360
            # Optional systematic sensor bias on the emitted node-relative bearing only.
            # Ground-truth geometry is unchanged; bias is diagnostic experiment metadata.
            item["bearing_deg"] = (node_relative + bearing_bias_deg) % 360
            if bearing_bias_deg != 0.0:
                item["bearing_bias_deg"] = bearing_bias_deg
        # When bearing_available is false, omit bearing_deg so the MQTT payload
        # reuses the existing spatial.bearing=null emission path (no fabricated deg).
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
            raise ScenarioError(
                "--ear must use the form ALIAS=/path/to/credentials.json"
            )
        if alias in ears:
            raise ScenarioError(f"duplicate --ear alias: {alias}")
        ears[alias] = Path(path)
    return ears


def resolve_scenario_ear_paths(args: argparse.Namespace) -> dict[str, Path]:
    """Resolve Ear credential paths from ``--ear`` or ``--ear-profile`` (not both)."""
    ear_values = list(getattr(args, "ear", None) or [])
    profile_path = getattr(args, "ear_profile", None)
    if profile_path and ear_values:
        raise ScenarioError("use either --ear-profile or --ear, not both")
    if profile_path:
        try:
            profile = load_ear_profile(profile_path)
            paths = resolve_profile_credential_paths(profile, require_files=True)
        except EarProfileError as exc:
            raise ScenarioError(str(exc)) from exc
        print(f"EAR profile: {profile_summary(profile)}", flush=True)
        return paths
    if not ear_values:
        raise ScenarioError(
            "provide --ear ALIAS=CREDENTIALS_FILE and/or --ear-profile PATH"
        )
    return parse_ear_arguments(ear_values)


def validate_ear_aliases(
    scenario: dict[str, Any],
    ears: dict[str, Path],
    *,
    mode: str = "scripted",
) -> None:
    if mode == "independent_detection":
        required = set(scenario["ears"])
    else:
        required = {item["ear"] for item in scenario["observations"]}
    missing = required - ears.keys()
    unused = ears.keys() - required
    if missing or unused:
        raise ScenarioError(
            f"Ear aliases mismatch; missing={sorted(missing)}, unused={sorted(unused)}"
        )


def _saved_observation_sequence_number(saved_observation: dict[str, Any]) -> int | None:
    """Return the last published sequence from a credentials observation blob.

    Current ObservationBody stores ``identity.sequence_number``. Older simulator
    credential files used a flat ``sequence_number`` on the observation object.
    Accept both so ADE (and any Ear provisioned via the nested schema) load.
    """
    identity = saved_observation.get("identity")
    if isinstance(identity, dict) and "sequence_number" in identity:
        value = identity.get("sequence_number")
    else:
        value = saved_observation.get("sequence_number")
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return value


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
                raise PermissionError(
                    f"{path} must not be accessible by group or other users"
                )
            credentials = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ScenarioError(
                f"could not load credentials for {alias}: {exc}"
            ) from exc
        if (
            not isinstance(credentials, dict)
            or credentials.get("state") != "provisioned"
        ):
            raise ScenarioError(
                f"credentials for {alias} are not in the provisioned state"
            )
        node_id = credentials.get("node_id")
        if not isinstance(node_id, str) or not node_id.startswith("SIM-EAR-"):
            raise ScenarioError(
                f"credentials for {alias} must belong to a SIM-EAR-* node"
            )
        if node_id in node_ids:
            raise ScenarioError(
                f"each Ear alias must use a distinct node; repeated node {node_id}"
            )
        node_ids.add(node_id)
        endpoint = credentials.get("mqtt")
        topics = credentials.get("topics")
        if not isinstance(endpoint, dict) or endpoint.get("use_tls") is not True:
            raise ScenarioError(
                f"credentials for {alias} do not contain a TLS MQTT endpoint"
            )
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
            raise ScenarioError(
                f"credentials for {alias} contain an invalid MQTT endpoint"
            )
        if not isinstance(topics, dict) or topics.get("observation") != (
            f"esp-ear/local/{node_id.lower()}/observation"
        ):
            raise ScenarioError(
                f"credentials for {alias} contain an invalid observation topic"
            )
        pending = credentials.get("pending_observation")
        if pending is not None and not allow_pending:
            raise ScenarioError(
                f"{path} has a pending observation; retry or resolve it before running a scenario"
            )
        if pending is not None and credentials.get("pending_scenario_id") is None:
            raise ScenarioError(
                f"{path} has a pending observation from another simulator mode"
            )
        saved_observation = credentials.get("observation")
        if not isinstance(saved_observation, dict):
            raise ScenarioError(f"credentials for {alias} have no saved observation")
        saved_sequence = _saved_observation_sequence_number(saved_observation)
        if saved_sequence is None:
            raise ScenarioError(
                f"credentials for {alias} have an invalid saved sequence number"
            )
        next_sequence = credentials.get(
            "next_sequence_number",
            saved_sequence + 1,
        )
        if (
            isinstance(next_sequence, bool)
            or not isinstance(next_sequence, int)
            or next_sequence < 0
        ):
            raise ScenarioError(
                f"credentials for {alias} have an invalid next sequence number"
            )
        credentials["next_sequence_number"] = next_sequence
        credentials_by_alias[alias] = credentials
    return credentials_by_alias


# Gap (same Ear, consecutive scripted points) that starts a new Observation
# lifecycle. Existing exclusive-window scenarios stay under this threshold;
# overlapping Cross-EAR fixtures use a larger silence for a negative pass.
SCRIPTED_LIFECYCLE_GAP_SECONDS = 30.0


def _scripted_lifecycle_segments(
    prepared: list[dict[str, Any]], indices: list[int]
) -> list[list[int]]:
    if not indices:
        return []
    segments: list[list[int]] = [[indices[0]]]
    for index in indices[1:]:
        previous = segments[-1][-1]
        gap = float(prepared[index]["at_seconds"]) - float(
            prepared[previous]["at_seconds"]
        )
        if gap > SCRIPTED_LIFECYCLE_GAP_SECONDS:
            segments.append([index])
        else:
            segments[-1].append(index)
    return segments


def assign_scripted_ear_observation_lifecycles(
    observations: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Group scripted points by Ear into Observation lifecycles.

    Points that already set ``emission_kind`` (independent_detection) are unchanged.
    Consecutive unassigned points for the same Ear share one ``observation_id``
    until a silence greater than ``SCRIPTED_LIFECYCLE_GAP_SECONDS``, which starts
    a new lifecycle. One point -> COMPLETE; two or more -> START, PERIODIC..., END.

    Lifecycle IDs are minted per Ear segment only — never from simulator
    ``ground_truth_id`` / ``simulated_source_id``.
    """
    prepared = [dict(item) for item in observations]
    by_ear: dict[str, list[int]] = defaultdict(list)
    for index, item in enumerate(prepared):
        if item.get("emission_kind") is not None:
            continue
        ear = item.get("ear")
        if isinstance(ear, str):
            by_ear[ear].append(index)

    for indices in by_ear.values():
        for segment in _scripted_lifecycle_segments(prepared, indices):
            observation_id = str(uuid4())
            count = len(segment)
            episode_start = prepared[segment[0]]["at_seconds"]
            for position, index in enumerate(segment):
                if count == 1:
                    kind = "complete"
                elif position == 0:
                    kind = "start"
                elif position == count - 1:
                    kind = "end"
                else:
                    kind = "periodic"
                prepared[index]["emission_kind"] = kind
                prepared[index]["observation_id"] = observation_id
                prepared[index]["episode_start_at_seconds"] = episode_start
                if kind in ("end", "complete"):
                    prepared[index].setdefault("termination_reason", "natural")
    return prepared


def make_scenario_observation(
    node_id: str,
    sequence_number: int,
    source_id: str,
    event: dict[str, Any],
    started_at_utc: datetime,
    started_at_monotonic_us: int,
) -> dict[str, Any]:
    from l0_measurements import empty_l0_measurements

    at_seconds = event["at_seconds"]
    duration_ms = float(event.get("duration_ms", 850.0))
    # Default COMPLETE for a standalone scripted point. run_scenario assigns
    # START/PERIODIC/END (+ shared observation_id) per Ear when multiple points exist.
    emission_kind = event.get("emission_kind", "complete")
    if emission_kind not in ("start", "periodic", "end", "complete"):
        raise ScenarioError(
            f"unsupported emission_kind {emission_kind!r} in scenario observation"
        )
    episode_start = event.get("episode_start_at_seconds", at_seconds)
    try:
        episode_elapsed_ms = max(0.0, (float(at_seconds) - float(episode_start)) * 1000.0)
    except (TypeError, ValueError):
        episode_elapsed_ms = 0.0
    context: dict[str, Any] = {
        "event_time_utc": (started_at_utc + timedelta(seconds=at_seconds)).isoformat(),
        "capture_timestamp_monotonic_us": started_at_monotonic_us
        + round(at_seconds * 1_000_000),
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": duration_ms,
        "episode_duration_ms": episode_elapsed_ms + duration_ms,
        "emission_kind": emission_kind,
    }
    if emission_kind in ("end", "complete"):
        context["termination_reason"] = event.get("termination_reason", "natural")

    bearing = None
    if "bearing_deg" in event:
        bearing = {
            "deg": event["bearing_deg"],
            "reference": "node",
        }
        if "bearing_confidence" in event and event["bearing_confidence"] is not None:
            bearing["confidence"] = event["bearing_confidence"]

    signal_level = event.get("signal_level_dbfs", -34.0)
    raw_observation_id = event.get("observation_id")
    observation_id = (
        str(raw_observation_id) if raw_observation_id else str(uuid4())
    )
    payload: dict[str, Any] = {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": {
            "observation_id": observation_id,
            "node_id": node_id,
            "hub_id": "",
            "sequence_number": sequence_number,
        },
        "context": context,
        "acoustic": {
            "signal_level_dbfs": signal_level,
            "measurements": empty_l0_measurements(),
            "signature": None,
            "classification": {
                "schema": "l2",
                "version": 1,
                "source_family": event.get("source_family", "vehicle"),
                "confidence": event["classification_confidence"],
                "hints": {"simulated_source_id": source_id},
            },
        },
        "spatial": {
            "bearing": bearing,
            "position": None,
        },
    }
    _observation_body().model_validate(payload)
    return payload


def parse_ear_max_range_arguments(values: list[str]) -> dict[str, float]:
    ranges: dict[str, float] = {}
    for value in values:
        alias, separator, raw = value.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", alias) or not raw:
            raise ScenarioError(
                "--ear-max-range must use the form ALIAS=METERS"
            )
        if alias in ranges:
            raise ScenarioError(f"duplicate --ear-max-range alias: {alias}")
        try:
            parsed = float(raw)
        except ValueError as exc:
            raise ScenarioError(
                f"--ear-max-range {alias} must be a number"
            ) from exc
        meters = _finite_number(parsed, f"--ear-max-range {alias}", minimum=0)
        if meters <= 0:
            raise ScenarioError(f"--ear-max-range {alias} must be greater than zero")
        ranges[alias] = meters
    return ranges


def apply_simulation_mode(
    scenario: dict[str, Any],
    *,
    mode: str,
    time_step_seconds: float,
    max_range_m: float,
    per_ear_max_range_m: dict[str, float] | None = None,
    reference_level_dbfs: float = DEFAULT_REFERENCE_LEVEL_DBFS,
    reference_distance_m: float = DEFAULT_REFERENCE_DISTANCE_M,
    path_loss_exponent: float = DEFAULT_PATH_LOSS_EXPONENT,
    observation_interval_seconds: float = DEFAULT_OBSERVATION_INTERVAL_S,
) -> tuple[dict[str, Any], Any | None]:
    """Return a runnable scenario copy and optional independent-detection diagnostics."""
    if mode not in SIMULATION_MODES:
        raise ScenarioError(
            f"unsupported simulation mode {mode!r}; expected one of {SIMULATION_MODES}"
        )
    prepared = dict(scenario)
    prepared["simulation_mode"] = mode
    if mode == "scripted":
        if not prepared["observations"]:
            raise ScenarioError("scripted mode requires authored observations")
        return prepared, None

    config = RangeDetectionConfig(
        max_range_m=max_range_m,
        time_step_seconds=time_step_seconds,
        per_ear_max_range_m=dict(per_ear_max_range_m or {}),
    )
    signal_level_config = SignalLevelConfig(
        reference_level_dbfs=reference_level_dbfs,
        reference_distance_m=reference_distance_m,
        path_loss_exponent=path_loss_exponent,
    )
    emission_policy = ObservationEmissionPolicy(
        observation_interval_seconds=observation_interval_seconds,
    )
    try:
        events, diagnostics = generate_independent_detection_events(
            movement=prepared["movement"],
            ears=prepared["ears"],
            source_family=prepared["source_family"],
            config=config,
            signal_level_config=signal_level_config,
            emission_policy=emission_policy,
            position_on_route=_position_on_route,
            distance_m=_distance_m,
            bearing_deg=_bearing_deg,
        )
    except ValueError as exc:
        raise ScenarioError(str(exc)) from exc
    prepared["observations"] = events
    prepared["independent_detection"] = diagnostics
    return prepared, diagnostics


def apply_named_scripted_cadence_profile(
    scenario: dict[str, Any],
    profile_id: str | None,
) -> dict[str, Any]:
    """Optionally resample scripted report times via an explicit named profile.

    Independent of ``independent_detection`` / ``--observation-interval-seconds``.
    Default (``profile_id is None``) leaves the scenario unchanged.
    """
    if profile_id is None:
        return scenario
    if scenario.get("simulation_mode", "scripted") != "scripted":
        raise ScenarioError(
            "--scripted-cadence-profile applies only with --mode scripted"
        )
    try:
        return apply_scripted_cadence_profile(
            scenario,
            profile_id,
            position_on_route=_position_on_route,
            distance_m=_distance_m,
            bearing_deg=_bearing_deg,
        )
    except ScriptedCadenceProfileError as exc:
        raise ScenarioError(str(exc)) from exc


def run_scenario(
    scenario: dict[str, Any],
    credentials_by_alias: dict[str, dict[str, Any]],
    credentials_paths: dict[str, Path],
    ca_file: str,
    time_scale: float,
    *,
    publish: Callable[[dict[str, Any], dict[str, Any], str], str] | None = None,
    sleep: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
    started_at_utc: datetime | None = None,
    started_at_monotonic_us: int | None = None,
    save: Callable[..., None] | None = None,
    publish_keepalive: Callable[[dict[str, Any], str, dict[str, Any]], None]
    | None = None,
    make_keepalive: Callable[[dict[str, Any], int], dict[str, Any]] | None = None,
    announce: bool = True,
) -> list[str]:
    if publish is None or save is None:
        default_publish, default_save = _observation_helpers()
        if publish is None:
            publish = default_publish
        if save is None:
            save = default_save
    if not math.isfinite(time_scale) or time_scale <= 0:
        raise ScenarioError("time scale must be finite and greater than zero")
    if announce:
        print_scenario_banner(scenario, time_scale=time_scale)
    publish_keepalives_for_ears(
        credentials_by_alias,
        ca_file,
        label="start",
        publish_keepalive=publish_keepalive,
        make_keepalive=make_keepalive,
        uptime_s=0,
    )
    scenario_start = started_at_utc or datetime.now(UTC)
    monotonic_start = (
        started_at_monotonic_us
        if started_at_monotonic_us is not None
        else time.monotonic_ns() // 1000
    )
    wall_start = monotonic()
    published_ids = []
    timed_events = assign_scripted_ear_observation_lifecycles(scenario["observations"])
    for index, event in enumerate(timed_events, start=1):
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
            payload["identity"]["sequence_number"] + 1,
        )
        credentials.pop("pending_observation", None)
        credentials.pop("pending_scenario_id", None)
        save(credentials_paths[alias], credentials)
        print(
            f"{index:02d}  t={event['at_seconds']:g}s  "
            f"{ear_shortname(alias)}  {observation_id}",
            flush=True,
        )
    elapsed_s = int(max(0.0, monotonic() - wall_start))
    publish_keepalives_for_ears(
        credentials_by_alias,
        ca_file,
        label="end",
        publish_keepalive=publish_keepalive,
        make_keepalive=make_keepalive,
        uptime_s=elapsed_s,
    )
    return published_ids


def print_schedule(scenario: dict[str, Any], *, time_scale: float = 1.0) -> None:
    started_at = datetime.now(UTC)
    started_at_monotonic_us = time.monotonic_ns() // 1000
    sequences: dict[str, int] = {}
    timed_events = assign_scripted_ear_observation_lifecycles(scenario["observations"])
    for event in timed_events:
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
    movement = scenario["movement"]
    wall = run_wall_clock_seconds(scenario, time_scale)
    mode = scenario.get("simulation_mode", "scripted")
    last_at = (
        scenario["observations"][-1]["at_seconds"]
        if scenario["observations"]
        else movement["duration_seconds"]
    )
    print(f"Scenario: {scenario['scenario_id']}")
    print(f"Simulation mode: {mode}")
    cadence_meta = scenario.get("scripted_cadence_profile")
    if cadence_meta:
        print(
            f"Scripted cadence profile: {cadence_meta['profile_id']} "
            f"(nominal {cadence_meta['cadence_seconds']:g}s; "
            f"derived from {cadence_meta['source_scenario_file']}; "
            f"{cadence_meta['authored_report_count']}→"
            f"{cadence_meta['resampled_report_count']} reports)"
        )
    print(
        f"Ground truth source: {scenario['source_id']} "
        f"(test metadata only)"
    )
    print(
        f"source_family={scenario['source_family']}  "
        f"speed={movement['speed_m_s']:g} m/s"
    )
    print(
        f"Movement: {movement['distance_m']:.1f}m over "
        f"{movement['duration_seconds']:.1f}s logical"
    )
    print(
        f"Run wall-clock: ≈{wall:.1f}s at --time-scale {time_scale:g} "
        f"(last observation at t={last_at:g}s; keepalives bookend the run)"
    )
    print("No Hub event, track, or geographic position is generated.")
    diagnostics = scenario.get("independent_detection")
    if diagnostics is not None:
        for line in format_independent_detection_diagnostics(diagnostics):
            print(line)
    if not scenario["observations"]:
        print("No observations generated (no Ear passed detection).")
        return
    # Independent mode can emit hundreds of 1 Hz detections; keep dry-run readable.
    list_all = mode == "scripted" or len(scenario["observations"]) <= 40
    if not list_all:
        print(
            f"Generated {len(scenario['observations'])} observations "
            "(per-step listing omitted; see diagnostics). "
            "Re-run with a smaller --max-range-m to inspect a short timeline."
        )
        return
    for index, event in enumerate(scenario["observations"], start=1):
        synthetic = ""
        if event.get("synthetic_fields"):
            synthetic = "  [synthetic level/confidence placeholders]"
        if "bearing_deg" in event:
            confidence = event.get("bearing_confidence", "not set")
            bias = event.get("bearing_bias_deg")
            bias_text = (
                f"; bias={bias:+g} deg" if isinstance(bias, (int, float)) else ""
            )
            bearing_text = (
                f"bearing={event['bearing_deg']:.1f} deg "
                f"(confidence={confidence}{bias_text})"
            )
        else:
            bearing_text = "bearing=unavailable (spatial.bearing=null)"
        print(
            f"{index:02d}  t={event['at_seconds']:g}s  "
            f"{ear_shortname(event['ear'])}  "
            f"{bearing_text}  "
            f"classification confidence={event['classification_confidence']}  "
            f"ground-truth distance={event['source_distance_m']:.1f}m  "
            f"level={event.get('signal_level_dbfs', 'not set')} dBFS  "
            f"duration={event.get('duration_ms', 850.0):g}ms"
            f"{synthetic}"
        )


def retry_pending_observations(
    scenario: dict[str, Any],
    credentials_by_alias: dict[str, dict[str, Any]],
    credentials_paths: dict[str, Path],
    ca_file: str,
    *,
    publish: Callable[[dict[str, Any], dict[str, Any], str], str] | None = None,
    save: Callable[..., None] | None = None,
    publish_keepalive: Callable[[dict[str, Any], str, dict[str, Any]], None]
    | None = None,
    make_keepalive: Callable[[dict[str, Any], int], dict[str, Any]] | None = None,
    announce: bool = True,
) -> list[str]:
    if publish is None or save is None:
        default_publish, default_save = _observation_helpers()
        if publish is None:
            publish = default_publish
        if save is None:
            save = default_save
    pending_aliases = [
        alias
        for alias, credentials in credentials_by_alias.items()
        if credentials.get("pending_observation") is not None
    ]
    if not pending_aliases:
        raise ScenarioError("no pending scenario observations to retry")
    pending_payloads: list[tuple[str, dict[str, Any], dict[str, Any]]] = []
    for alias in pending_aliases:
        credentials = credentials_by_alias[alias]
        if credentials.get("pending_scenario_id") != scenario["scenario_id"]:
            raise ScenarioError(
                f"pending observation for {alias} belongs to another scenario"
            )
        payload = credentials["pending_observation"]
        _observation_body().model_validate(payload)
        identity = payload.get("identity") or {}
        if identity.get("node_id") != credentials["node_id"]:
            raise ScenarioError(
                f"pending observation for {alias} belongs to another Ear"
            )
        classification = (payload.get("acoustic") or {}).get("classification") or {}
        hints = classification.get("hints") if isinstance(classification, dict) else None
        if (
            not isinstance(hints, dict)
            or hints.get("simulated_source_id") != scenario["source_id"]
        ):
            raise ScenarioError(
                f"pending observation for {alias} has a different simulated source hint"
            )
        pending_payloads.append((alias, credentials, payload))
    pending_credentials = {
        alias: credentials for alias, credentials, _ in pending_payloads
    }
    if announce:
        print_scenario_banner(scenario, count=len(pending_payloads))
    publish_keepalives_for_ears(
        pending_credentials,
        ca_file,
        label="start",
        publish_keepalive=publish_keepalive,
        make_keepalive=make_keepalive,
        uptime_s=0,
    )
    ids = []
    for index, (alias, credentials, payload) in enumerate(pending_payloads, start=1):
        observation_id = publish(credentials, payload, ca_file)
        ids.append(observation_id)
        credentials["next_sequence_number"] = max(
            credentials["next_sequence_number"],
            payload["identity"]["sequence_number"] + 1,
        )
        credentials.pop("pending_observation", None)
        credentials.pop("pending_scenario_id", None)
        save(credentials_paths[alias], credentials)
        event_time = (payload.get("context") or {}).get("event_time_utc", "?")
        print(
            f"{index:02d}  {event_time}  "
            f"{ear_shortname(alias)}  {observation_id}",
            flush=True,
        )
    publish_keepalives_for_ears(
        pending_credentials,
        ca_file,
        label="end",
        publish_keepalive=publish_keepalive,
        make_keepalive=make_keepalive,
        uptime_s=0,
    )
    return ids


def _add_scenario_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--scenario-file",
        required=True,
        help="Scenario definition in JSON",
    )
    parser.add_argument(
        "--ear",
        action="append",
        default=[],
        metavar="ALIAS=CREDENTIALS_FILE",
        help=(
            "Map a scenario Ear alias to a private provisioned simulator credentials "
            "file (repeatable). Cannot combine with --ear-profile."
        ),
    )
    parser.add_argument(
        "--ear-profile",
        default=None,
        metavar="PATH",
        help=(
            "JSON profile mapping scenario Ear aliases to simulator slots "
            "(resolved via sim_ear_slots). Example: "
            "shared/simulator/ear-profiles/abc.json. Cannot combine with --ear."
        ),
    )
    parser.add_argument(
        "--ca", default="shared/trust/ca.crt", help="Path to the local hub CA"
    )
    parser.add_argument(
        "--mode",
        choices=SIMULATION_MODES,
        default="scripted",
        help=(
            "scripted = authored observations (baseline); "
            "independent_detection = shared trajectory + per-Ear range gate "
            "(default: scripted)"
        ),
    )
    parser.add_argument(
        "--time-step-seconds",
        type=float,
        default=DEFAULT_INDEPENDENT_TIME_STEP_S,
        help=(
            "Shared simulation step for independent_detection "
            f"(default: {DEFAULT_INDEPENDENT_TIME_STEP_S:g})"
        ),
    )
    parser.add_argument(
        "--max-range-m",
        type=float,
        default=DEFAULT_INDEPENDENT_MAX_RANGE_M,
        help=(
            "Default detection range for independent_detection "
            f"(default: {DEFAULT_INDEPENDENT_MAX_RANGE_M:g})"
        ),
    )
    parser.add_argument(
        "--ear-max-range",
        action="append",
        default=[],
        metavar="ALIAS=METERS",
        help="Optional per-Ear detection range override for independent_detection",
    )
    parser.add_argument(
        "--reference-level-dbfs",
        type=float,
        default=DEFAULT_REFERENCE_LEVEL_DBFS,
        help=(
            "Stage 2 synthetic signal level at reference distance "
            f"(independent_detection only; default: {DEFAULT_REFERENCE_LEVEL_DBFS:g})"
        ),
    )
    parser.add_argument(
        "--reference-distance-m",
        type=float,
        default=DEFAULT_REFERENCE_DISTANCE_M,
        help=(
            "Stage 2 reference distance for signal attenuation "
            f"(independent_detection only; default: {DEFAULT_REFERENCE_DISTANCE_M:g})"
        ),
    )
    parser.add_argument(
        "--path-loss-exponent",
        type=float,
        default=DEFAULT_PATH_LOSS_EXPONENT,
        help=(
            "Stage 2 path-loss exponent (1.0 ≈ −6 dB per distance doubling; "
            f"independent_detection only; default: {DEFAULT_PATH_LOSS_EXPONENT:g})"
        ),
    )
    parser.add_argument(
        "--observation-interval-seconds",
        type=float,
        default=DEFAULT_OBSERVATION_INTERVAL_S,
        help=(
            "Stage 3 logical reporting interval relative to each Ear detection-episode "
            "start (emit start, then every N seconds, then end; "
            f"independent_detection only; default: {DEFAULT_OBSERVATION_INTERVAL_S:g})"
        ),
    )
    _cadence_choices = known_scripted_cadence_profile_ids()
    parser.add_argument(
        "--scripted-cadence-profile",
        choices=_cadence_choices,
        default=None,
        help=(
            "Optional named scripted report-cadence resampling profile. "
            "Applies only with --mode scripted. Does not change route, speed, "
            "detection windows, or the 30 s Observation lifecycle split rule. "
            f"Known: {', '.join(_cadence_choices) or '(none)'}. "
            "Omit for the immutable authored baseline."
        ),
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Time-ordered Ear scenario over TLS MQTT. "
            "Use dry-run, run, or retry-pending. "
            "Modes: scripted (authored) or independent_detection (range gate)."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)

    dry = commands.add_parser(
        "dry-run",
        help="Validate and print the timeline without loading credentials or publishing",
    )
    _add_scenario_common_args(dry)
    dry.add_argument(
        "--time-scale",
        type=float,
        default=1.0,
        help="Estimate run wall-clock with this scale (default: 1; same as run)",
    )

    run = commands.add_parser(
        "run",
        help="Publish keepalives, then scenario observations over MQTT, then keepalives",
    )
    _add_scenario_common_args(run)
    run.add_argument(
        "--time-scale",
        type=float,
        default=1.0,
        help="Scale wall-clock waits; logical event times remain unchanged (default: 1)",
    )

    retry = commands.add_parser(
        "retry-pending",
        help="Retry a saved in-flight observation from this scenario, then exit",
    )
    _add_scenario_common_args(retry)
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

    try:
        from paho.mqtt import MQTTException
    except ImportError:  # pragma: no cover - help path does not need paho
        MQTTException = ()  # type: ignore[misc, assignment]

    try:
        loaded = load_scenario(Path(args.scenario_file))
        ear_paths = resolve_scenario_ear_paths(args)
        per_ear_ranges = parse_ear_max_range_arguments(args.ear_max_range)
        validate_ear_aliases(loaded, ear_paths, mode=args.mode)
        scenario, diagnostics = apply_simulation_mode(
            loaded,
            mode=args.mode,
            time_step_seconds=args.time_step_seconds,
            max_range_m=args.max_range_m,
            per_ear_max_range_m=per_ear_ranges,
            reference_level_dbfs=args.reference_level_dbfs,
            reference_distance_m=args.reference_distance_m,
            path_loss_exponent=args.path_loss_exponent,
            observation_interval_seconds=args.observation_interval_seconds,
        )
        if getattr(args, "scripted_cadence_profile", None):
            if args.command == "retry-pending":
                raise ScenarioError(
                    "--scripted-cadence-profile is not used with retry-pending"
                )
            scenario = apply_named_scripted_cadence_profile(
                scenario, args.scripted_cadence_profile
            )
        if args.command == "dry-run":
            if not math.isfinite(args.time_scale) or args.time_scale <= 0:
                raise ScenarioError("--time-scale must be finite and greater than zero")
            print_schedule(scenario, time_scale=args.time_scale)
            return 0

        if not Path(args.ca).is_file():
            raise ScenarioError(f"CA file does not exist: {args.ca}")
        if args.command == "run":
            if not math.isfinite(args.time_scale) or args.time_scale <= 0:
                raise ScenarioError("--time-scale must be finite and greater than zero")
            credentials_by_alias = load_ear_credentials(ear_paths, allow_pending=False)
            ids = run_scenario(
                scenario,
                credentials_by_alias,
                ear_paths,
                args.ca,
                args.time_scale,
            )
            if diagnostics is not None:
                for line in format_independent_detection_diagnostics(diagnostics):
                    print(line, flush=True)
            print(
                f"Scenario {scenario['scenario_id']} finished: "
                f"{len(ids)}/{len(scenario['observations'])} MQTT publishes acknowledged.",
                flush=True,
            )
        else:
            if args.mode != "scripted":
                raise ScenarioError(
                    "retry-pending applies to saved payloads; use --mode scripted"
                )
            credentials_by_alias = load_ear_credentials(ear_paths, allow_pending=True)
            ids = retry_pending_observations(
                scenario,
                credentials_by_alias,
                ear_paths,
                args.ca,
            )
            print(
                f"Scenario {scenario['scenario_id']} finished: "
                f"{len(ids)} pending observation(s) retried.",
                flush=True,
            )
        print(
            "Use scripts/hub_event_track.py to inspect the observations; no Hub logic was deployed."
        )
        return 0
    except KeyboardInterrupt:
        print(
            "Scenario interrupted. Any unpublished pending observation is preserved in its credentials file."
        )
        return 130
    except (
        KeyError,
        MQTTException,
        OSError,
        RuntimeError,
        ScenarioError,
        ValueError,
    ) as exc:
        print(f"Scenario simulation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
