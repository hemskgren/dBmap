"""Shared-trajectory multi-Ear detection (Stages 1–3).

Stage 1: deterministic range-gate detection (eligibility only).
Stage 2: deterministic distance-dependent synthetic signal_level_dbfs.
Stage 3: deterministic observation emission (start / periodic / end).

Detection evaluation continues at every shared time step. Emission policy only
selects which positive detections become published observations.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any, Literal
from uuid import uuid4

SYNTHETIC_CLASSIFICATION_CONFIDENCE = 0.5
SYNTHETIC_DURATION_MS = 850.0
# Bearing confidence omitted from published payloads (schema allows null/absent).

# Floor for log10 distance ratio; detections already skip ≤1e-6 m (undefined bearing).
MIN_DISTANCE_FOR_SIGNAL_LEVEL_M = 1.0

DEFAULT_REFERENCE_LEVEL_DBFS = -30.0
DEFAULT_REFERENCE_DISTANCE_M = 1000.0
DEFAULT_PATH_LOSS_EXPONENT = 1.0
DEFAULT_OBSERVATION_INTERVAL_S = 5.0

EmissionKind = Literal["start", "periodic", "end"]


@dataclass(frozen=True)
class RangeDetectionConfig:
    """Deterministic range-gate detection model (Stage 1)."""

    max_range_m: float
    time_step_seconds: float = 1.0
    per_ear_max_range_m: dict[str, float] = field(default_factory=dict)

    def range_for(self, ear_alias: str) -> float:
        return self.per_ear_max_range_m.get(ear_alias, self.max_range_m)


@dataclass(frozen=True)
class SignalLevelConfig:
    """Deterministic distance attenuation for synthetic signal_level_dbfs (Stage 2)."""

    reference_level_dbfs: float = DEFAULT_REFERENCE_LEVEL_DBFS
    reference_distance_m: float = DEFAULT_REFERENCE_DISTANCE_M
    path_loss_exponent: float = DEFAULT_PATH_LOSS_EXPONENT

    def validate(self) -> None:
        if (
            not math.isfinite(self.reference_level_dbfs)
            or not math.isfinite(self.reference_distance_m)
            or not math.isfinite(self.path_loss_exponent)
        ):
            raise ValueError("signal-level parameters must be finite")
        if self.reference_distance_m <= 0:
            raise ValueError("reference_distance_m must be greater than zero")
        if self.path_loss_exponent <= 0:
            raise ValueError("path_loss_exponent must be greater than zero")
        if self.reference_level_dbfs > 0:
            raise ValueError("reference_level_dbfs must be at most 0 (protocol)")


@dataclass(frozen=True)
class ObservationEmissionPolicy:
    """Stage 3: which positive detections become published observations.

    Periodic reports are scheduled relative to each Ear's detection-episode
    start: start + k * observation_interval_seconds, for times strictly before
    episode end. Episode end always emits once (unless it coincides with a
    periodic/start timestamp already chosen).
    """

    observation_interval_seconds: float = DEFAULT_OBSERVATION_INTERVAL_S
    emit_on_start: bool = True
    emit_on_end: bool = True

    def validate(self) -> None:
        if (
            not math.isfinite(self.observation_interval_seconds)
            or self.observation_interval_seconds <= 0
        ):
            raise ValueError(
                "observation_interval_seconds must be finite and greater than zero"
            )


def distance_dependent_signal_level_dbfs(
    distance_m: float,
    config: SignalLevelConfig,
) -> float:
    """Return synthetic dBFS from distance; clamp to ≤0 for protocol compliance."""
    if not math.isfinite(distance_m) or distance_m < 0:
        raise ValueError("distance_m must be finite and non-negative")
    config.validate()
    safe_distance = max(float(distance_m), MIN_DISTANCE_FOR_SIGNAL_LEVEL_M)
    level = config.reference_level_dbfs - (
        20.0
        * config.path_loss_exponent
        * math.log10(safe_distance / config.reference_distance_m)
    )
    if not math.isfinite(level):
        raise ValueError("signal level calculation produced a non-finite value")
    if level > 0.0:
        return 0.0
    return level


def emission_schedule_for_episode(
    start_t: float,
    end_t: float,
    policy: ObservationEmissionPolicy,
) -> list[tuple[float, EmissionKind]]:
    """Return unique (timestamp, kind) emissions for one detection episode."""
    policy.validate()
    if end_t + 1e-12 < start_t:
        raise ValueError("episode end must be >= start")

    # Single evaluation step: start and end coincide → one start observation.
    if abs(end_t - start_t) <= 1e-12:
        return [(start_t, "start")] if policy.emit_on_start else []

    by_time: dict[float, EmissionKind] = {}
    if policy.emit_on_start:
        by_time[start_t] = "start"

    interval = float(policy.observation_interval_seconds)
    k = 1
    while True:
        periodic_t = start_t + k * interval
        if periodic_t > end_t + 1e-12:
            break
        if abs(periodic_t - end_t) <= 1e-12:
            # Coincides with end — single end emission (not a separate periodic).
            break
        by_time[periodic_t] = "periodic"
        k += 1

    if policy.emit_on_end:
        by_time[end_t] = "end"

    return sorted(by_time.items(), key=lambda item: item[0])


@dataclass
class EarPassStats:
    evaluations: int = 0
    detections: int = 0
    detection_episodes: int = 0
    observations_emitted: int = 0
    emissions_start: int = 0
    emissions_periodic: int = 0
    emissions_end: int = 0
    min_distance_m: float | None = None
    max_distance_m: float | None = None
    closest_approach_t_s: float | None = None
    closest_approach_distance_m: float | None = None
    closest_approach_signal_level_dbfs: float | None = None
    min_signal_level_dbfs: float | None = None
    max_signal_level_dbfs: float | None = None
    detection_intervals: list[tuple[float, float]] = field(default_factory=list)


@dataclass
class IndependentDetectionDiagnostics:
    mode: str
    time_step_seconds: float
    default_max_range_m: float
    per_ear_max_range_m: dict[str, float]
    signal_level: SignalLevelConfig
    emission_policy: ObservationEmissionPolicy
    simulation_steps: int
    ear_count: int
    total_evaluations: int
    total_positive_detection_evaluations: int
    observations_generated: int
    emissions_start: int
    emissions_periodic: int
    emissions_end: int
    per_ear: dict[str, EarPassStats]
    simultaneous_two_or_more_intervals: list[tuple[float, float]]
    simultaneous_three_intervals: list[tuple[float, float]]
    notes: list[str]


def _merge_intervals(active_times: list[float], step: float) -> list[tuple[float, float]]:
    if not active_times:
        return []
    ordered = sorted(active_times)
    intervals: list[tuple[float, float]] = []
    start = ordered[0]
    prev = ordered[0]
    for current in ordered[1:]:
        if current - prev > step + 1e-9:
            intervals.append((start, prev))
            start = current
        prev = current
    intervals.append((start, prev))
    return intervals


def generate_independent_detection_events(
    *,
    movement: dict[str, Any],
    ears: dict[str, dict[str, float]],
    source_family: str,
    config: RangeDetectionConfig,
    signal_level_config: SignalLevelConfig | None = None,
    emission_policy: ObservationEmissionPolicy | None = None,
    position_at: Callable[[dict[str, Any], float], dict[str, float]],
    distance_m: Callable[[dict[str, float], dict[str, float]], float],
    bearing_deg: Callable[[dict[str, float], dict[str, float]], float],
) -> tuple[list[dict[str, Any]], IndependentDetectionDiagnostics]:
    """Evaluate every Ear every step; emit observations per emission policy."""
    if not math.isfinite(config.time_step_seconds) or config.time_step_seconds <= 0:
        raise ValueError("time_step_seconds must be finite and greater than zero")
    if not math.isfinite(config.max_range_m) or config.max_range_m <= 0:
        raise ValueError("max_range_m must be finite and greater than zero")
    for alias, value in config.per_ear_max_range_m.items():
        if alias not in ears:
            raise ValueError(f"per-ear range override for unknown Ear alias: {alias}")
        if not math.isfinite(value) or value <= 0:
            raise ValueError(
                f"per-ear max range for {alias} must be finite and greater than zero"
            )

    level_config = signal_level_config or SignalLevelConfig()
    level_config.validate()
    policy = emission_policy or ObservationEmissionPolicy()
    policy.validate()

    duration = float(movement["duration_seconds"])
    step = float(config.time_step_seconds)

    times: list[float] = []
    t = 0.0
    while t < duration - 1e-12:
        # Keep samples inside [0, duration]; round() can push the end past duration.
        times.append(min(duration, round(t, 9)))
        t += step
    if not times or times[-1] < duration - 1e-9:
        times.append(duration)

    stats = {alias: EarPassStats() for alias in ears}
    detection_times: dict[str, list[float]] = {alias: [] for alias in ears}
    # (ear, t) -> measurement fields needed to build an observation
    measurements: dict[tuple[str, float], dict[str, Any]] = {}
    multi_two_times: list[float] = []
    multi_three_times: list[float] = []

    for sample_t in times:
        sample_t = min(max(0.0, float(sample_t)), duration)
        source_position = position_at(movement, sample_t)
        detecting_aliases: list[str] = []
        for alias, ear in ears.items():
            stats[alias].evaluations += 1
            dist = distance_m(ear, source_position)
            if stats[alias].min_distance_m is None or dist < stats[alias].min_distance_m:
                stats[alias].min_distance_m = dist
                stats[alias].closest_approach_distance_m = dist
                stats[alias].closest_approach_t_s = sample_t
                stats[alias].closest_approach_signal_level_dbfs = (
                    distance_dependent_signal_level_dbfs(dist, level_config)
                )
            if stats[alias].max_distance_m is None or dist > stats[alias].max_distance_m:
                stats[alias].max_distance_m = dist

            limit = config.range_for(alias)
            if dist > limit or dist <= 1e-6:
                continue

            level = distance_dependent_signal_level_dbfs(dist, level_config)
            detecting_aliases.append(alias)
            stats[alias].detections += 1
            detection_times[alias].append(sample_t)
            if (
                stats[alias].min_signal_level_dbfs is None
                or level < stats[alias].min_signal_level_dbfs
            ):
                stats[alias].min_signal_level_dbfs = level
            if (
                stats[alias].max_signal_level_dbfs is None
                or level > stats[alias].max_signal_level_dbfs
            ):
                stats[alias].max_signal_level_dbfs = level

            geo = bearing_deg(ear, source_position)
            node_bearing = (geo - ear["orientation_deg"]) % 360
            measurements[(alias, sample_t)] = {
                "at_seconds": sample_t,
                "ear": alias,
                "source_family": source_family,
                "bearing_deg": node_bearing,
                "geographic_bearing_deg": geo,
                "source_distance_m": dist,
                "detection_range_m": limit,
                "classification_confidence": SYNTHETIC_CLASSIFICATION_CONFIDENCE,
                "signal_level_dbfs": level,
                "duration_ms": SYNTHETIC_DURATION_MS,
                "synthetic_fields": (
                    "signal_level_dbfs",
                    "classification_confidence",
                    "duration_ms",
                ),
            }

        if len(detecting_aliases) >= 2:
            multi_two_times.append(sample_t)
        if len(detecting_aliases) >= 3:
            multi_three_times.append(sample_t)

    events: list[dict[str, Any]] = []
    total_start = total_periodic = total_end = 0
    for alias, ear_stats in stats.items():
        intervals = _merge_intervals(detection_times[alias], step)
        ear_stats.detection_intervals = intervals
        ear_stats.detection_episodes = len(intervals)
        for episode_start, episode_end in intervals:
            # One opaque Observation lifecycle ID per detection episode
            # (START/PERIODIC/END share it). Never derived from ground_truth_id.
            observation_id = str(uuid4())
            for emit_t, kind in emission_schedule_for_episode(
                episode_start, episode_end, policy
            ):
                key = (alias, emit_t)
                if key not in measurements:
                    matches = [
                        sample
                        for sample in detection_times[alias]
                        if abs(sample - emit_t) <= 1e-9
                    ]
                    if not matches:
                        raise ValueError(
                            f"emission time {emit_t} for {alias} has no detection sample"
                        )
                    key = (alias, matches[0])
                    emit_t = matches[0]
                event = dict(measurements[key])
                event["at_seconds"] = emit_t
                event["emission_kind"] = kind
                event["observation_id"] = observation_id
                event["episode_start_at_seconds"] = episode_start
                if kind == "end":
                    event["termination_reason"] = "natural"
                events.append(event)
                ear_stats.observations_emitted += 1
                if kind == "start":
                    ear_stats.emissions_start += 1
                    total_start += 1
                elif kind == "periodic":
                    ear_stats.emissions_periodic += 1
                    total_periodic += 1
                else:
                    ear_stats.emissions_end += 1
                    total_end += 1

    events.sort(key=lambda item: (item["at_seconds"], item["ear"]))

    ear_count = len(ears)
    total_evaluations = sum(item.evaluations for item in stats.values())
    total_detections = sum(item.detections for item in stats.values())
    diagnostics = IndependentDetectionDiagnostics(
        mode="independent_detection",
        time_step_seconds=step,
        default_max_range_m=config.max_range_m,
        per_ear_max_range_m=dict(config.per_ear_max_range_m),
        signal_level=level_config,
        emission_policy=policy,
        simulation_steps=len(times),
        ear_count=ear_count,
        total_evaluations=total_evaluations,
        total_positive_detection_evaluations=total_detections,
        observations_generated=len(events),
        emissions_start=total_start,
        emissions_periodic=total_periodic,
        emissions_end=total_end,
        per_ear=stats,
        simultaneous_two_or_more_intervals=_merge_intervals(multi_two_times, step),
        simultaneous_three_intervals=_merge_intervals(multi_three_times, step),
        notes=[
            ("Detection model: deterministic range gate (Stage 1); "
            "eligibility is not based on signal level or emission policy."),
            ("Signal level model (Stage 2): "
            "reference_level - 20*exponent*log10(distance/reference_distance); "
            "synthetic/uncalibrated, not microphone measurements."),
            (f"signal params: reference_level_dbfs={level_config.reference_level_dbfs:g}, "
            f"reference_distance_m={level_config.reference_distance_m:g}, "
            f"path_loss_exponent={level_config.path_loss_exponent:g}, "
            f"min_distance_for_log={MIN_DISTANCE_FOR_SIGNAL_LEVEL_M:g}m; "
            "levels clamped to ≤0 dBFS for protocol."),
            ("Emission policy (Stage 3): episode-relative fixed interval; "
            f"observation_interval={policy.observation_interval_seconds:g}s; "
            f"emit_on_start={policy.emit_on_start}; emit_on_end={policy.emit_on_end}. "
            "Detection evaluations ≠ emitted observations."),
            (f"Synthetic classification_confidence="
            f"{SYNTHETIC_CLASSIFICATION_CONFIDENCE} (not calibrated)."),
            "Bearing confidence omitted (unavailable / not calibrated).",
            "All configured Ears are evaluated at every shared simulation step.",
        ],
    )
    return events, diagnostics


def format_independent_detection_diagnostics(
    diagnostics: IndependentDetectionDiagnostics,
) -> list[str]:
    signal = diagnostics.signal_level
    policy = diagnostics.emission_policy
    lines = [
        ("Independent detection diagnostics "
        "(Stage 1 range gate + Stage 2 levels + Stage 3 emission):"),
        (f"  mode={diagnostics.mode}  time_step={diagnostics.time_step_seconds:g}s  "
        f"default_max_range={diagnostics.default_max_range_m:g}m"),
        (
            f"  signal model (synthetic): "
            f"reference_level={signal.reference_level_dbfs:g} dBFS  "
            f"reference_distance={signal.reference_distance_m:g}m  "
            f"path_loss_exponent={signal.path_loss_exponent:g}"
        ),
        (
            f"  emission policy: observation_interval="
            f"{policy.observation_interval_seconds:g}s  "
            f"emit_on_start={policy.emit_on_start}  "
            f"emit_on_end={policy.emit_on_end}  "
            f"(schedule relative to each Ear detection-episode start)"
        ),
    ]
    if diagnostics.per_ear_max_range_m:
        overrides = ", ".join(
            f"{alias}={value:g}m"
            for alias, value in sorted(diagnostics.per_ear_max_range_m.items())
        )
        lines.append(f"  per-Ear range overrides: {overrides}")
    lines.append(
        f"  steps={diagnostics.simulation_steps}  ears={diagnostics.ear_count}  "
        f"evaluations={diagnostics.total_evaluations}  "
        f"positive_detections={diagnostics.total_positive_detection_evaluations}  "
        f"emitted_observations={diagnostics.observations_generated}  "
        f"(start={diagnostics.emissions_start}, "
        f"periodic={diagnostics.emissions_periodic}, "
        f"end={diagnostics.emissions_end})"
    )
    for alias, stats in sorted(diagnostics.per_ear.items()):
        limit = diagnostics.per_ear_max_range_m.get(
            alias, diagnostics.default_max_range_m
        )
        cpa = (
            f"t={stats.closest_approach_t_s:g}s@{stats.closest_approach_distance_m:.0f}m"
            if stats.closest_approach_t_s is not None
            and stats.closest_approach_distance_m is not None
            else "n/a"
        )
        cpa_level = (
            f"{stats.closest_approach_signal_level_dbfs:.2f} dBFS"
            if stats.closest_approach_signal_level_dbfs is not None
            else "n/a"
        )
        dmin = (
            f"{stats.min_distance_m:.0f}m"
            if stats.min_distance_m is not None
            else "-"
        )
        dmax = (
            f"{stats.max_distance_m:.0f}m"
            if stats.max_distance_m is not None
            else "-"
        )
        smin = (
            f"{stats.min_signal_level_dbfs:.2f}"
            if stats.min_signal_level_dbfs is not None
            else "-"
        )
        smax = (
            f"{stats.max_signal_level_dbfs:.2f}"
            if stats.max_signal_level_dbfs is not None
            else "-"
        )
        intervals = (
            ", ".join(f"[{start:g},{end:g}]s" for start, end in stats.detection_intervals)
            or "none"
        )
        lines.append(
            f"  {alias}: detections={stats.detections}/{stats.evaluations}  "
            f"episodes={stats.detection_episodes}  "
            f"emitted={stats.observations_emitted} "
            f"(start={stats.emissions_start}, periodic={stats.emissions_periodic}, "
            f"end={stats.emissions_end})  "
            f"range≤{limit:g}m  dist[{dmin},{dmax}]  CPA {cpa} "
            f"(level {cpa_level})  "
            f"signal[{smin},{smax}] dBFS  intervals={intervals}"
        )
    two = (
        ", ".join(
            f"[{start:g},{end:g}]s"
            for start, end in diagnostics.simultaneous_two_or_more_intervals
        )
        or "none"
    )
    three = (
        ", ".join(
            f"[{start:g},{end:g}]s"
            for start, end in diagnostics.simultaneous_three_intervals
        )
        or "none"
    )
    lines.append(f"  simultaneous ≥2 Ears: {two}")
    lines.append(f"  simultaneous 3 Ears: {three}")
    for note in diagnostics.notes:
        lines.append(f"  note: {note}")
    return lines
