"""Derived scripted report-cadence profiles (runtime resampling).

Preserves authored scenario JSON as the immutable baseline. Profiles only
resample report timestamps inside each Ear's existing detection window;
route, speed, and windows stay unchanged. Independent of independent_detection.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

# Explicit named profile for the helicopter denser-sampling experiment.
HELICOPTER_EW_10S_PROFILE_ID = "helicopter_ew_10s"
HELICOPTER_EW_SOURCE_SCENARIO_ID = "helicopter_ew_01"
HELICOPTER_EW_10S_CADENCE_SECONDS = 10.0

SCRIPTED_CADENCE_PROFILES: dict[str, dict[str, Any]] = {
    HELICOPTER_EW_10S_PROFILE_ID: {
        "cadence_seconds": HELICOPTER_EW_10S_CADENCE_SECONDS,
        "source_scenario_id": HELICOPTER_EW_SOURCE_SCENARIO_ID,
        "source_scenario_file": "helicopter_east_west.json",
        "description": (
            "Controlled denser scripted sampling at a nominal 10 s interval "
            "inside each authored detection window of helicopter_east_west. "
            "Not a firmware-equivalence claim (firmware PERIODIC default is 5 s). "
            "Baseline authored spacing averages ~18–19 s and is irregular."
        ),
    },
}


class ScriptedCadenceProfileError(ValueError):
    pass


def known_scripted_cadence_profile_ids() -> tuple[str, ...]:
    return tuple(sorted(SCRIPTED_CADENCE_PROFILES))


def detection_windows_by_ear(
    observations: list[dict[str, Any]],
) -> dict[str, tuple[float, float]]:
    """Return inclusive (first, last) authored at_seconds per Ear alias."""
    windows: dict[str, tuple[float, float]] = {}
    for item in observations:
        ear = item["ear"]
        t = float(item["at_seconds"])
        if ear not in windows:
            windows[ear] = (t, t)
        else:
            start, end = windows[ear]
            windows[ear] = (min(start, t), max(end, t))
    return windows


def cadence_timestamps(
    window_start: float,
    window_end: float,
    *,
    cadence_seconds: float,
) -> list[float]:
    """Resample [start, end] at cadence_seconds with explicit START/END bounds.

    Always includes window_start and window_end. Intermediate PERIODIC times are
    start + k * cadence for integer k >= 1 while strictly before window_end.
    Times are stepped in whole milliseconds to keep the schedule reproducible.
    """
    if not (cadence_seconds > 0):
        raise ScriptedCadenceProfileError("cadence_seconds must be greater than zero")
    if window_end < window_start:
        raise ScriptedCadenceProfileError(
            f"detection window end {window_end} precedes start {window_start}"
        )
    start_ms = round(window_start * 1000.0)
    end_ms = round(window_end * 1000.0)
    step_ms = round(cadence_seconds * 1000.0)
    if step_ms < 1:
        raise ScriptedCadenceProfileError("cadence_seconds is too small")
    if end_ms == start_ms:
        return [start_ms / 1000.0]

    times_ms = [start_ms]
    t = start_ms + step_ms
    while t < end_ms:
        times_ms.append(t)
        t += step_ms
    if times_ms[-1] != end_ms:
        times_ms.append(end_ms)
    return [ms / 1000.0 for ms in times_ms]


def _interpolate_scalar(
    samples: list[tuple[float, float]],
    at_seconds: float,
) -> float:
    """Piecewise-linear interpolate; clamp outside the sample span."""
    if not samples:
        raise ScriptedCadenceProfileError("no samples to interpolate")
    if len(samples) == 1:
        return samples[0][1]
    if at_seconds <= samples[0][0]:
        return samples[0][1]
    if at_seconds >= samples[-1][0]:
        return samples[-1][1]
    for index in range(1, len(samples)):
        t0, v0 = samples[index - 1]
        t1, v1 = samples[index]
        if at_seconds <= t1:
            if t1 == t0:
                return v1
            weight = (at_seconds - t0) / (t1 - t0)
            return v0 + weight * (v1 - v0)
    return samples[-1][1]


def _authored_series(
    observations: list[dict[str, Any]],
    ear: str,
    field: str,
    *,
    default: float | None = None,
) -> list[tuple[float, float]]:
    series: list[tuple[float, float]] = []
    for item in observations:
        if item["ear"] != ear:
            continue
        if field not in item:
            if default is None:
                continue
            series.append((float(item["at_seconds"]), float(default)))
            continue
        series.append((float(item["at_seconds"]), float(item[field])))
    series.sort(key=lambda row: row[0])
    return series


def apply_scripted_cadence_profile(
    scenario: dict[str, Any],
    profile_id: str,
    *,
    position_at: Callable[..., dict[str, float]],
    distance_m: Callable[..., float],
    bearing_deg: Callable[..., float],
) -> dict[str, Any]:
    """Return a copy of a loaded scripted scenario with resampled report times.

    Does not modify independent_detection. Does not change movement or ears.
    Lifecycle START/PERIODIC/END is assigned later by
    ``assign_scripted_ear_observation_lifecycles`` (unchanged 30 s split rule).
    """
    if profile_id not in SCRIPTED_CADENCE_PROFILES:
        known = ", ".join(known_scripted_cadence_profile_ids()) or "(none)"
        raise ScriptedCadenceProfileError(
            f"unknown scripted cadence profile {profile_id!r}; known: {known}"
        )
    profile = SCRIPTED_CADENCE_PROFILES[profile_id]
    expected_source = profile["source_scenario_id"]
    source_id = scenario.get("scenario_id")
    if source_id != expected_source:
        raise ScriptedCadenceProfileError(
            f"profile {profile_id!r} requires scenario_id={expected_source!r} "
            f"(got {source_id!r})"
        )
    if scenario.get("simulation_mode", "scripted") not in (None, "scripted"):
        raise ScriptedCadenceProfileError(
            "scripted cadence profiles apply only to scripted mode"
        )

    authored = list(scenario["observations"])
    if not authored:
        raise ScriptedCadenceProfileError("scenario has no authored observations")

    windows = detection_windows_by_ear(authored)
    cadence = float(profile["cadence_seconds"])
    movement = scenario["movement"]
    ears = scenario["ears"]
    duration = float(movement["duration_seconds"])

    rebuilt: list[dict[str, Any]] = []
    for ear, (window_start, window_end) in sorted(
        windows.items(), key=lambda item: (item[1][0], item[0])
    ):
        times = cadence_timestamps(
            window_start, window_end, cadence_seconds=cadence
        )
        bearing_conf = _authored_series(authored, ear, "bearing_confidence")
        class_conf = _authored_series(authored, ear, "classification_confidence")
        signal_levels = _authored_series(authored, ear, "signal_level_dbfs")
        durations = _authored_series(authored, ear, "duration_ms", default=850.0)
        ear_position = ears[ear]
        for at_seconds in times:
            if at_seconds > duration:
                raise ScriptedCadenceProfileError(
                    f"resampled at_seconds={at_seconds} exceeds route duration "
                    f"{duration}"
                )
            source_position = position_at(movement, at_seconds)
            dist = distance_m(ear_position, source_position)
            if dist <= 1e-6:
                raise ScriptedCadenceProfileError(
                    f"resampled point at t={at_seconds}s coincides with {ear}"
                )
            true_bearing = bearing_deg(ear_position, source_position)
            item: dict[str, Any] = {
                "at_seconds": at_seconds,
                "ear": ear,
                "source_family": scenario["source_family"],
                "bearing_confidence": _interpolate_scalar(bearing_conf, at_seconds),
                "classification_confidence": _interpolate_scalar(
                    class_conf, at_seconds
                ),
                "bearing_deg": (true_bearing - ear_position["orientation_deg"]) % 360,
                "source_distance_m": dist,
            }
            if signal_levels:
                item["signal_level_dbfs"] = _interpolate_scalar(
                    signal_levels, at_seconds
                )
            if durations:
                item["duration_ms"] = _interpolate_scalar(durations, at_seconds)
            rebuilt.append(item)

    rebuilt.sort(key=lambda item: (float(item["at_seconds"]), item["ear"]))

    prepared = dict(scenario)
    prepared["observations"] = rebuilt
    prepared["scripted_cadence_profile"] = {
        "profile_id": profile_id,
        "cadence_seconds": cadence,
        "source_scenario_id": expected_source,
        "source_scenario_file": profile["source_scenario_file"],
        "description": profile["description"],
        "detection_windows": {
            ear: {"start_s": start, "end_s": end}
            for ear, (start, end) in sorted(windows.items())
        },
        "authored_report_count": len(authored),
        "resampled_report_count": len(rebuilt),
    }
    return prepared
