"""Deterministic period/phase association for Observations (policy v2).

Primary clock: Hub ``received_time_utc``.
Secondary: ``event_time_utc`` (may disagree — reported, never discarded).

Concepts kept distinct:
- Test period: intentional experiment window (period_id).
- Inter-period pause: wall time between consecutive periods (phase ``pause``).
- Final collection pause: trailing poll window after the last active period
  (phase ``final_collection``) — not an active test period.
- Observation: Ear-reported / Hub-received datum (never equated to a period).

Does not interpret detector emit kinds or acoustic episodes.
"""

from __future__ import annotations

from datetime import UTC, datetime

from lib.ear_test.models import AssociationResult, PauseRecord, PeriodRecord, PhaseKind


def _aware(value: datetime) -> datetime:
    if value.tzinfo is None or value.utcoffset() is None:
        return value.replace(tzinfo=UTC)
    return value.astimezone(UTC)


def _period_window(period: PeriodRecord) -> tuple[datetime | None, datetime | None]:
    """Prefer actual times; fall back to planned only when both planned bounds exist."""
    start = period.actual_start_utc
    end = period.actual_end_utc
    if start is not None:
        return _aware(start), _aware(end) if end is not None else None
    if period.planned_start_utc is not None and period.planned_end_utc is not None:
        return _aware(period.planned_start_utc), _aware(period.planned_end_utc)
    return None, None


def _pause_window(pause: PauseRecord) -> tuple[datetime | None, datetime | None]:
    start = pause.actual_start_utc
    end = pause.actual_end_utc
    if start is not None:
        return _aware(start), _aware(end) if end is not None else None
    if pause.planned_start_utc is not None and pause.planned_end_utc is not None:
        return _aware(pause.planned_start_utc), _aware(pause.planned_end_utc)
    return None, None


def _phase_for_pause(pause: PauseRecord) -> PhaseKind:
    if pause.kind == "final_collection":
        return "final_collection"
    return "pause"


def _locate(
    instant: datetime,
    *,
    job_start: datetime | None,
    job_end: datetime | None,
    periods: list[PeriodRecord],
    pauses: list[PauseRecord] | None = None,
) -> tuple[PhaseKind, str | None]:
    instant = _aware(instant)
    job_start_a = _aware(job_start) if job_start else None
    job_end_a = _aware(job_end) if job_end else None

    if job_start_a is not None and instant < job_start_a:
        return "unassigned", None
    if job_end_a is not None and instant >= job_end_a:
        return "unassigned", None

    ordered = sorted(periods, key=lambda item: item.index)
    windows: list[tuple[PeriodRecord, datetime, datetime | None]] = []
    for period in ordered:
        start, end = _period_window(period)
        if start is None:
            continue
        windows.append((period, start, end))

    for period, start, end in windows:
        if instant < start:
            continue
        if end is None or instant < end:
            return "active", period.period_id

    pause_list = list(pauses or [])
    for pause in pause_list:
        start, end = _pause_window(pause)
        if start is None:
            continue
        if instant < start:
            continue
        if end is None or instant < end:
            return _phase_for_pause(pause), None

    # Backward-compatible gap inference when pause records are absent/incomplete.
    if not pause_list:
        for index, (period, start, end) in enumerate(windows):
            if end is None:
                continue
            next_start = windows[index + 1][1] if index + 1 < len(windows) else None
            if next_start is not None and end <= instant < next_start:
                return "pause", None

    if job_start_a is not None and (job_end_a is None or instant < job_end_a):
        return "job_idle", None

    return "unassigned", None


def associate_observation(
    *,
    received_time_utc: datetime,
    event_time_utc: datetime | None,
    job_start: datetime | None,
    job_end: datetime | None,
    periods: list[PeriodRecord],
    pauses: list[PauseRecord] | None = None,
) -> AssociationResult:
    phase_r, period_r = _locate(
        received_time_utc,
        job_start=job_start,
        job_end=job_end,
        periods=periods,
        pauses=pauses,
    )
    if event_time_utc is None:
        phase_e, period_e = "unassigned", None
        disagree = True
        note = "event_time_utc missing; associated by received_time_utc only"
    else:
        phase_e, period_e = _locate(
            event_time_utc,
            job_start=job_start,
            job_end=job_end,
            periods=periods,
            pauses=pauses,
        )
        disagree = (phase_r, period_r) != (phase_e, period_e)
        if disagree:
            note = (
                "received_time_utc and event_time_utc map to different "
                "periods/phases (latency or interval-start semantics)"
            )
        else:
            note = "received and event clocks agree on period/phase"

    return AssociationResult(
        phase_by_received=phase_r,
        period_id_by_received=period_r,
        phase_by_event=phase_e,
        period_id_by_event=period_e,
        received_event_disagree=disagree,
        association_note=note,
    )
