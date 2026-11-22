"""Availability windows shared by Core Hub and public observations."""

from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import Any

from app.monitoring_math import attribute_hold, ensure_aware

OK_STATES = frozenset({"ok", "online", "healthy", "disabled"})
COMPONENT_UNAVAILABLE_STATES = frozenset(
    {"offline", "unhealthy", "error", "unavailable"}
)
WINDOW_SPECS = (
    ("24h", timedelta(hours=24)),
    ("7d", timedelta(days=7)),
    ("30d", timedelta(days=30)),
)
MIN_ELIGIBLE_SECONDS = 600
LOW_COVERAGE_FRACTION = 0.5
PERCENT_QUANTUM = 0.01


@dataclass(frozen=True)
class Observation:
    observed_at: datetime
    state: str
    sparse: bool = True


def component_state(raw: str | None) -> str:
    if raw in OK_STATES:
        return "available"
    if raw in COMPONENT_UNAVAILABLE_STATES:
        return "unavailable"
    return "unobserved"


def _attribute_interval(
    state: str, start: datetime, end: datetime, interval: timedelta
) -> tuple[float, float, float]:
    """Return (available_seconds, unavailable_seconds, unobserved_seconds).

    `degraded` counts as available for the core Hub percentage (e.g. OPA-only
    failure). MQTT failure is stored as `unavailable`, not `degraded`.
    Legacy dense samples use this for long gaps so Hub-down time stays
    unobserved rather than held as the last confirmed state.
    """
    total = max((end - start).total_seconds(), 0.0)
    if total <= 0:
        return 0.0, 0.0, 0.0
    max_confirmed = max(interval.total_seconds() * 2, interval.total_seconds())
    if total > max_confirmed:
        confirmed = interval.total_seconds()
        unobserved = total - confirmed
    else:
        confirmed = total
        unobserved = 0.0
    if state in {"available", "degraded"}:
        return confirmed, 0.0, unobserved
    if state == "unavailable":
        return 0.0, confirmed, unobserved
    return 0.0, 0.0, confirmed + unobserved


def _format_percent(
    available: float, eligible: float, unavailable: float
) -> float | None:
    if eligible <= 0:
        return None
    raw = 100.0 * available / eligible
    percent = round(raw, 2)
    # Never present a bare 100.00 when any confirmed unavailable time exists.
    if unavailable > 0 and percent >= 100.0:
        percent = 100.0 - PERCENT_QUANTUM
    return percent


def _window_result(
    *,
    available: float,
    unavailable: float,
    unobserved: float,
    not_running: float = 0,
    window_seconds: float,
    sample_count: int,
    gaps: list[dict[str, str]],
    basis: str,
) -> dict[str, Any]:
    eligible = available + unavailable
    insufficient = eligible < MIN_ELIGIBLE_SECONDS
    low_coverage = eligible < (window_seconds * LOW_COVERAGE_FRACTION)
    short_window = window_seconds <= 24 * 3600 + 1
    score_ready = (not insufficient) and (short_window or not low_coverage)
    percent = (
        None if insufficient else _format_percent(available, eligible, unavailable)
    )
    if insufficient:
        state_summary = "unknown"
        reason = "insufficient_observation"
    elif unavailable <= 0 and available > 0:
        state_summary = "available"
        reason = "low_coverage" if low_coverage else None
    elif available > unavailable:
        state_summary = "degraded" if unavailable > 0 else "available"
        reason = "low_coverage" if low_coverage else None
    else:
        state_summary = "unavailable"
        reason = "low_coverage" if low_coverage else None
    return {
        "percent": percent,
        "state_summary": state_summary,
        "eligible_seconds": int(eligible),
        "available_seconds": int(available),
        "unavailable_seconds": int(unavailable),
        "unobserved_seconds": int(unobserved),
        "not_running_seconds": int(not_running),
        "sample_count": sample_count,
        "insufficient_data": insufficient,
        "low_coverage": low_coverage,
        "score_ready": score_ready,
        "reason": reason,
        "basis": basis,
        "gaps": gaps,
    }


def calculate_window(
    observations: list[Observation],
    *,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
    basis: str,
    core: bool = False,
) -> dict[str, Any]:
    window_seconds = (window_end - window_start).total_seconds()
    if window_seconds <= 0:
        return _window_result(
            available=0,
            unavailable=0,
            unobserved=0,
            window_seconds=0,
            sample_count=0,
            gaps=[],
            basis=basis,
        ) | {"insufficient_data": True, "reason": "invalid_window", "percent": None}

    inside = []
    carry = None
    for point in sorted(observations, key=lambda item: ensure_aware(item.observed_at)):
        at = ensure_aware(point.observed_at)
        if at < window_start:
            carry = point
        elif at <= window_end:
            inside.append(point)
    ordered = [carry, *inside] if carry is not None else inside

    gaps = []

    def add_gap(start: datetime, end: datetime, kind: str) -> None:
        gap = {
            "start": start.isoformat(),
            "end": end.isoformat(),
            "certainty": "inferred",
        }
        if core:
            gap["kind"] = kind
        gaps.append(gap)

    first_at = ensure_aware(ordered[0].observed_at) if ordered else window_end
    unobserved = max(0.0, (first_at - window_start).total_seconds())
    if unobserved and (core or ordered):
        add_gap(window_start, first_at, "not_started")

    available = unavailable = not_running = 0.0
    interval = timedelta(seconds=interval_s)
    for index, point in enumerate(ordered):
        start = max(ensure_aware(point.observed_at), window_start)
        end = (
            ensure_aware(ordered[index + 1].observed_at)
            if index + 1 < len(ordered)
            else window_end
        )
        if end <= start:
            continue
        if point.sparse:
            avail, unavail, unobs = attribute_hold(point.state, start, end)
        else:
            avail, unavail, unobs = _attribute_interval(
                point.state, start, end, interval
            )
        available += avail
        unavailable += unavail
        unobserved += unobs
        if unobs > 0:
            confirmed_end = start + timedelta(seconds=avail + unavail)
            if confirmed_end < end:
                add_gap(confirmed_end, end, "not_running")
                if core:
                    not_running += (end - confirmed_end).total_seconds()

    return _window_result(
        available=available,
        unavailable=unavailable,
        unobserved=unobserved,
        not_running=not_running,
        window_seconds=window_seconds,
        sample_count=len(inside),
        gaps=gaps,
        basis=basis,
    )


def component_window(
    observations: list[Observation],
    *,
    component_id: str,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
) -> dict[str, Any]:
    basis = (
        f"{component_id} availability % = confirmed ok time / eligible observation "
        "time × 100. Unknown component states are unobserved and excluded. "
        "Independent of overall Hub availability."
    )
    return calculate_window(
        observations,
        window_start=window_start,
        window_end=window_end,
        interval_s=interval_s,
        basis=basis,
    ) | {"component": component_id}


def core_window(
    observations: list[Observation],
    *,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
) -> dict[str, Any]:
    basis = (
        "Core Hub availability % = confirmed available-or-OPA-degraded time / "
        "eligible observation time × 100. MQTT or reachability failure is "
        "unavailable. Unobserved gaps are excluded. Sparse-era samples hold "
        "state; legacy dense samples use interval confirmation for long gaps. "
        "A percentage requires at least 10 minutes of eligible observation. "
        "Multi-day windows are not score_ready until eligible time covers at "
        "least half the window."
    )
    return calculate_window(
        observations,
        window_start=window_start,
        window_end=window_end,
        interval_s=interval_s,
        basis=basis,
        core=True,
    )
