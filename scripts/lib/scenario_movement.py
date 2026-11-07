"""EAR scenario movement: legacy one-leg and segmented transit/hold models.

Simulator and post-hoc ground-truth helpers share these helpers so position
calculations stay identical. Not used by association or localization inputs.
"""

from __future__ import annotations

import math
from typing import Any

CONTINUITY_TOLERANCE_M = 0.05
_LEGACY_FIELDS = frozenset({"start", "end", "speed_m_s"})
_SEGMENTS_FIELDS = frozenset({"segments"})
_POINT_FIELDS = frozenset({"latitude", "longitude"})
_TRANSIT_FIELDS = frozenset({"type", "start", "end", "speed_m_s"})
_HOLD_FIELDS = frozenset({"type", "position", "duration_s"})
_EARTH_RADIUS_M = 6_371_000.0


class MovementError(ValueError):
    """Invalid authored or normalized movement."""


def _finite_number(value: Any, field: str, *, minimum: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise MovementError(f"{field} must be a number")
    number = float(value)
    if not math.isfinite(number):
        raise MovementError(f"{field} must be finite")
    if minimum is not None and number < minimum:
        raise MovementError(f"{field} must be at least {minimum}")
    return number


def validate_point(value: Any, field: str) -> dict[str, float]:
    if not isinstance(value, dict) or set(value) != _POINT_FIELDS:
        raise MovementError(f"{field} must contain exactly latitude and longitude")
    latitude = _finite_number(value["latitude"], f"{field}.latitude")
    longitude = _finite_number(value["longitude"], f"{field}.longitude")
    if not -90 <= latitude <= 90:
        raise MovementError(f"{field}.latitude must be in [-90, 90]")
    if not -180 <= longitude <= 180:
        raise MovementError(f"{field}.longitude must be in [-180, 180]")
    return {"latitude": latitude, "longitude": longitude}


def distance_m(first: dict[str, float], second: dict[str, float]) -> float:
    lat1 = math.radians(first["latitude"])
    lat2 = math.radians(second["latitude"])
    lat_delta = lat2 - lat1
    lon_delta = math.radians(second["longitude"] - first["longitude"])
    haversine = (
        math.sin(lat_delta / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(lon_delta / 2) ** 2
    )
    return _EARTH_RADIUS_M * 2 * math.asin(math.sqrt(min(1.0, haversine)))


def position_on_route(
    start: dict[str, float],
    end: dict[str, float],
    fraction: float,
) -> dict[str, float]:
    """Spherical linear interpolation along one great-circle transit leg."""
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


def _segment_entry(segment: dict[str, Any]) -> dict[str, float]:
    if segment["type"] == "transit":
        return segment["start"]
    return segment["position"]


def _segment_exit(segment: dict[str, Any]) -> dict[str, float]:
    if segment["type"] == "transit":
        return segment["end"]
    return segment["position"]


def _normalize_transit(raw: dict[str, Any], field: str) -> dict[str, Any]:
    if set(raw) != _TRANSIT_FIELDS:
        raise MovementError(
            f"{field} transit must contain exactly {sorted(_TRANSIT_FIELDS)}"
        )
    if raw["type"] != "transit":
        raise MovementError(f"{field}.type must be 'transit'")
    start = validate_point(raw["start"], f"{field}.start")
    end = validate_point(raw["end"], f"{field}.end")
    speed = _finite_number(raw["speed_m_s"], f"{field}.speed_m_s", minimum=0)
    if speed == 0:
        raise MovementError(f"{field}.speed_m_s must be greater than zero")
    dist = distance_m(start, end)
    if dist <= 0:
        raise MovementError(f"{field} transit distance must be greater than zero")
    return {
        "type": "transit",
        "start": start,
        "end": end,
        "speed_m_s": speed,
        "distance_m": dist,
        "duration_seconds": dist / speed,
    }


def _normalize_hold(raw: dict[str, Any], field: str) -> dict[str, Any]:
    if set(raw) != _HOLD_FIELDS:
        raise MovementError(
            f"{field} hold must contain exactly {sorted(_HOLD_FIELDS)}"
        )
    if raw["type"] != "hold":
        raise MovementError(f"{field}.type must be 'hold'")
    position = validate_point(raw["position"], f"{field}.position")
    duration_s = _finite_number(raw["duration_s"], f"{field}.duration_s", minimum=0)
    if duration_s == 0:
        raise MovementError(f"{field}.duration_s must be greater than zero")
    return {
        "type": "hold",
        "position": position,
        "duration_s": duration_s,
        "distance_m": 0.0,
        "duration_seconds": duration_s,
    }


def _normalize_segment(raw: Any, field: str) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise MovementError(f"{field} must be an object")
    seg_type = raw.get("type")
    if seg_type == "transit":
        return _normalize_transit(raw, field)
    if seg_type == "hold":
        return _normalize_hold(raw, field)
    raise MovementError(
        f"{field}.type must be 'transit' or 'hold' (got {seg_type!r})"
    )


def _assert_continuous(
    previous: dict[str, Any], current: dict[str, Any], field: str
) -> None:
    exit_point = _segment_exit(previous)
    entry_point = _segment_entry(current)
    gap = distance_m(exit_point, entry_point)
    if gap > CONTINUITY_TOLERANCE_M:
        raise MovementError(
            f"{field} is discontinuous with the previous segment "
            f"(gap={gap:.3f}m > {CONTINUITY_TOLERANCE_M:g}m); "
            "coordinates are not auto-repaired"
        )


def normalize_movement(raw: Any, *, field: str = "source.movement") -> dict[str, Any]:
    """Validate authored movement and return the normalized internal model."""
    if not isinstance(raw, dict):
        raise MovementError(f"{field} must be an object")
    keys = set(raw)
    if keys == _LEGACY_FIELDS:
        start = validate_point(raw["start"], f"{field}.start")
        end = validate_point(raw["end"], f"{field}.end")
        speed = _finite_number(raw["speed_m_s"], f"{field}.speed_m_s", minimum=0)
        if speed == 0:
            raise MovementError(f"{field}.speed_m_s must be greater than zero")
        dist = distance_m(start, end)
        if dist <= 0:
            raise MovementError(
                f"{field} start and end positions must be different"
            )
        segment = {
            "type": "transit",
            "start": start,
            "end": end,
            "speed_m_s": speed,
            "distance_m": dist,
            "duration_seconds": dist / speed,
        }
        return {
            "form": "legacy",
            "segments": [segment],
            "start": start,
            "end": end,
            "speed_m_s": speed,
            "distance_m": dist,
            "duration_seconds": segment["duration_seconds"],
        }

    if keys == _SEGMENTS_FIELDS:
        segments_raw = raw["segments"]
        if not isinstance(segments_raw, list) or not segments_raw:
            raise MovementError(f"{field}.segments must be a non-empty list")
        segments: list[dict[str, Any]] = []
        for index, item in enumerate(segments_raw):
            segment = _normalize_segment(item, f"{field}.segments[{index}]")
            if segments:
                _assert_continuous(
                    segments[-1], segment, f"{field}.segments[{index}]"
                )
            segments.append(segment)
        total_distance = sum(float(item["distance_m"]) for item in segments)
        total_duration = sum(float(item["duration_seconds"]) for item in segments)
        return {
            "form": "segments",
            "segments": segments,
            "start": dict(_segment_entry(segments[0])),
            "end": dict(_segment_exit(segments[-1])),
            "speed_m_s": None,
            "distance_m": total_distance,
            "duration_seconds": total_duration,
        }

    if keys & _LEGACY_FIELDS and keys & _SEGMENTS_FIELDS:
        raise MovementError(
            f"{field} must not mix legacy start/end/speed_m_s with segments"
        )
    if "segments" in keys:
        raise MovementError(
            f"{field} segmented form must contain exactly {sorted(_SEGMENTS_FIELDS)}"
        )
    raise MovementError(
        f"{field} must be legacy {sorted(_LEGACY_FIELDS)} or "
        f"segmented {sorted(_SEGMENTS_FIELDS)}"
    )


def position_at(movement: dict[str, Any], t: float) -> dict[str, float]:
    """Source position at scenario time ``t`` seconds (spherical transit / fixed hold).

    Allows ``t`` in ``[0, duration_seconds]`` inclusive. Rejects times outside
    that closed interval. Segment interiors use ``[t_i, t_{i+1})`` except the
    final segment, which includes the end instant.
    """
    if isinstance(t, bool) or not isinstance(t, (int, float)) or not math.isfinite(t):
        raise MovementError("scenario time t must be a finite number")
    duration = float(movement["duration_seconds"])
    if t < 0 or t > duration:
        raise MovementError(
            f"scenario time t={t} is outside movement duration [0, {duration}]"
        )
    segments = movement["segments"]
    if not segments:
        raise MovementError("movement has no segments")

    # Exact end of route.
    if t == duration or (duration > 0 and abs(t - duration) <= 1e-12):
        return dict(_segment_exit(segments[-1]))

    cursor = 0.0
    last_index = len(segments) - 1
    for index, segment in enumerate(segments):
        seg_dur = float(segment["duration_seconds"])
        seg_end = cursor + seg_dur
        is_last = index == last_index
        if is_last:
            in_segment = cursor <= t <= seg_end
        else:
            in_segment = cursor <= t < seg_end
        if not in_segment:
            cursor = seg_end
            continue
        if segment["type"] == "hold":
            return dict(segment["position"])
        # transit
        if seg_dur <= 0:
            return dict(segment["end"])
        fraction = (t - cursor) / seg_dur
        fraction = min(1.0, max(0.0, fraction))
        return position_on_route(segment["start"], segment["end"], fraction)

    # Numerical edge: fall back to final exit.
    return dict(_segment_exit(segments[-1]))
