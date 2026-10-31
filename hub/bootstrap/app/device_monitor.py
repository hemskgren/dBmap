"""Hub-observed device availability (D0) and monitoring summaries.

Availability is derived from reported_state.last_seen_at relative to
DBMAP_DEVICE_ONLINE_WINDOW_S. Health is assessed from device-reported diag
(D2) when present. Samples are transition-sparse; Hub monitoring gaps are
recorded as unobserved so they are excluded from the availability denominator.
"""

from __future__ import annotations

import json
import logging
from datetime import datetime, timedelta, timezone
from typing import Any
from uuid import uuid4

from sqlalchemy import delete, func, select
from sqlalchemy.orm import Session

from app import device_diag
from app.config import settings
from app.models import (
    DeviceMonitorEvent,
    DeviceMonitorSample,
    Node,
    ReportedState,
)

log = logging.getLogger("hub-bootstrap.device-monitor")

WINDOW_SPECS = (
    ("24h", timedelta(hours=24)),
    ("7d", timedelta(days=7)),
    ("30d", timedelta(days=30)),
)
MIN_ELIGIBLE_SECONDS = 600
LOW_COVERAGE_FRACTION = 0.5
RECENT_EVENT_LIMIT = 15


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _ensure_aware(value: datetime) -> datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def online_window() -> timedelta:
    return timedelta(seconds=settings.device_online_window_s)


def is_online(last_seen_at: datetime | None, *, now: datetime | None = None) -> bool:
    if last_seen_at is None:
        return False
    now = _ensure_aware(now or _utcnow())
    return now - _ensure_aware(last_seen_at) <= online_window()


def connectivity_state(
    last_seen_at: datetime | None, *, now: datetime | None = None
) -> str | None:
    """Return available/unavailable, or None when the device has never been seen."""
    if last_seen_at is None:
        return None
    return "available" if is_online(last_seen_at, now=now) else "unavailable"


def _record_event(
    db: Session,
    *,
    node_id: str,
    event_type: str,
    previous_state: str | None,
    new_state: str | None,
    reason: str | None,
    occurred_at: datetime,
    observed_at: datetime,
    certainty: str = "confirmed",
) -> None:
    db.add(
        DeviceMonitorEvent(
            event_id=str(uuid4()),
            hub_id=settings.hub_id,
            node_id=node_id,
            occurred_at=occurred_at,
            observed_at=observed_at,
            event_type=event_type,
            previous_state=previous_state,
            new_state=new_state,
            reason=(reason or "")[:256] or None,
            source="hub_observed",
            certainty=certainty,
        )
    )


def _latest_sample(db: Session, node_id: str) -> DeviceMonitorSample | None:
    return db.scalar(
        select(DeviceMonitorSample)
        .where(
            DeviceMonitorSample.hub_id == settings.hub_id,
            DeviceMonitorSample.node_id == node_id,
        )
        .order_by(DeviceMonitorSample.observed_at.desc())
        .limit(1)
    )


def _add_sample(
    db: Session,
    *,
    node_id: str,
    observed_at: datetime,
    availability: str,
) -> DeviceMonitorSample:
    sample = DeviceMonitorSample(
        sample_id=str(uuid4()),
        hub_id=settings.hub_id,
        node_id=node_id,
        observed_at=observed_at,
        overall_availability=availability,
        overall_health="unknown",
        source="hub_observed",
    )
    db.add(sample)
    return sample


def _prune_node_history(db: Session, node_id: str) -> None:
    # SessionLocal uses autoflush=False; flush so caps see rows added this cycle.
    db.flush()
    max_samples = settings.device_monitor_max_samples_per_device
    overflow_samples = db.scalars(
        select(DeviceMonitorSample.sample_id)
        .where(
            DeviceMonitorSample.hub_id == settings.hub_id,
            DeviceMonitorSample.node_id == node_id,
        )
        .order_by(DeviceMonitorSample.observed_at.desc())
        .offset(max_samples)
    ).all()
    if overflow_samples:
        db.execute(
            delete(DeviceMonitorSample).where(
                DeviceMonitorSample.sample_id.in_(overflow_samples)
            ),
            execution_options={"synchronize_session": False},
        )

    max_events = settings.device_monitor_max_events_per_device
    overflow_events = db.scalars(
        select(DeviceMonitorEvent.event_id)
        .where(
            DeviceMonitorEvent.hub_id == settings.hub_id,
            DeviceMonitorEvent.node_id == node_id,
        )
        .order_by(DeviceMonitorEvent.occurred_at.desc())
        .offset(max_events)
    ).all()
    if overflow_events:
        db.execute(
            delete(DeviceMonitorEvent).where(
                DeviceMonitorEvent.event_id.in_(overflow_events)
            ),
            execution_options={"synchronize_session": False},
        )


def _active_reported(db: Session) -> list[tuple[Node, ReportedState]]:
    rows = db.execute(
        select(Node, ReportedState)
        .join(ReportedState, ReportedState.node_id == Node.node_id)
        .where(Node.lifecycle_state == "active")
        .order_by(Node.node_id)
    ).all()
    return [(node, reported) for node, reported in rows]


def _emit_transition(
    db: Session,
    *,
    node_id: str,
    previous: str | None,
    new: str,
    observed_at: datetime,
) -> None:
    if previous is None:
        _record_event(
            db,
            node_id=node_id,
            event_type="monitoring_started",
            previous_state=None,
            new_state=new,
            reason="first Hub-observed connectivity sample",
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="confirmed",
        )
        return
    if previous == new:
        return
    if previous == "available" and new == "unavailable":
        event_type = "heartbeat_overdue"
        reason = (
            f"no keepalive within {int(settings.device_online_window_s)}s online window"
        )
    elif previous == "unavailable" and new == "available":
        event_type = "heartbeat_resumed"
        reason = "keepalive received after overdue period"
    elif previous == "unobserved":
        event_type = "monitoring_resumed"
        reason = "Hub monitoring resumed; current connectivity sampled"
    else:
        event_type = "availability_changed"
        reason = "Hub-observed connectivity state changed"
    _record_event(
        db,
        node_id=node_id,
        event_type=event_type,
        previous_state=previous,
        new_state=new,
        reason=reason,
        occurred_at=observed_at,
        observed_at=observed_at,
        certainty="confirmed" if previous != "unobserved" else "inferred",
    )


def sample_devices(
    db: Session,
    *,
    observed_at: datetime | None = None,
    hub_gap: tuple[datetime, datetime] | None = None,
) -> dict[str, Any]:
    """Sample active devices once. Safe against individual device failures."""
    if not settings.device_monitor_enabled:
        return {"enabled": False, "sampled": 0, "transitions": 0}

    now = _ensure_aware(observed_at or _utcnow())
    sampled = 0
    transitions = 0
    touched: set[str] = set()

    try:
        devices = _active_reported(db)
    except Exception:
        log.exception("device monitor failed to list active nodes")
        try:
            db.rollback()
        except Exception as rollback_exc:  # noqa: BLE001
            log.debug("rollback after device list failure: %s", rollback_exc)
        return {"enabled": True, "sampled": 0, "transitions": 0, "error": "list_failed"}

    for node, reported in devices:
        state = connectivity_state(reported.last_seen_at, now=now)
        if state is None:
            continue
        previous = _latest_sample(db, node.node_id)
        previous_state = previous.overall_availability if previous is not None else None

        if hub_gap is not None and previous is not None:
            gap_start, gap_end = (_ensure_aware(hub_gap[0]), _ensure_aware(hub_gap[1]))
            if gap_end > gap_start and previous_state != "unobserved":
                # Stamp slightly after the last confirmed sample when needed
                # so ordering keeps previous state until the gap.
                unobserved_at = max(
                    gap_start,
                    _ensure_aware(previous.observed_at) + timedelta(microseconds=1),
                )
                if unobserved_at < now:
                    _add_sample(
                        db,
                        node_id=node.node_id,
                        observed_at=unobserved_at,
                        availability="unobserved",
                    )
                    _record_event(
                        db,
                        node_id=node.node_id,
                        event_type="monitoring_resumed",
                        previous_state=previous_state,
                        new_state="unobserved",
                        reason=(
                            "Hub monitoring resumed after an unobserved gap; "
                            "device connectivity during the gap is unknown"
                        ),
                        occurred_at=now,
                        observed_at=now,
                        certainty="inferred",
                    )
                    previous_state = "unobserved"
                    transitions += 1

        if previous_state == state and hub_gap is None:
            continue

        _add_sample(
            db,
            node_id=node.node_id,
            observed_at=now,
            availability=state,
        )
        _emit_transition(
            db,
            node_id=node.node_id,
            previous=previous_state,
            new=state,
            observed_at=now,
        )
        touched.add(node.node_id)
        sampled += 1
        if previous_state != state:
            transitions += 1

    for node_id in touched:
        _prune_node_history(db, node_id)

    return {"enabled": True, "sampled": sampled, "transitions": transitions}


def cleanup_history(db: Session, now: datetime | None = None) -> dict[str, int]:
    """Delete device monitor rows older than retention. Does not touch other tables."""
    now = _ensure_aware(now or _utcnow())
    days = settings.device_monitor_retention_days
    cutoff = now - timedelta(days=days)
    samples = db.execute(
        delete(DeviceMonitorSample).where(
            DeviceMonitorSample.hub_id == settings.hub_id,
            DeviceMonitorSample.observed_at < cutoff,
        ),
        execution_options={"synchronize_session": False},
    )
    events = db.execute(
        delete(DeviceMonitorEvent).where(
            DeviceMonitorEvent.hub_id == settings.hub_id,
            DeviceMonitorEvent.occurred_at < cutoff,
        ),
        execution_options={"synchronize_session": False},
    )
    return {
        "samples_deleted": samples.rowcount or 0,
        "events_deleted": events.rowcount or 0,
        "cutoff": cutoff.isoformat(),
        "retention_days": days,
    }


def _attribute_hold(
    state: str, start: datetime, end: datetime
) -> tuple[float, float, float]:
    """Attribute the full interval to the sample state (transition-sparse model)."""
    total = max((end - start).total_seconds(), 0.0)
    if total <= 0:
        return 0.0, 0.0, 0.0
    if state in {"available", "degraded"}:
        return total, 0.0, 0.0
    if state == "unavailable":
        return 0.0, total, 0.0
    return 0.0, 0.0, total


def compute_availability_window(
    samples: list[DeviceMonitorSample],
    *,
    window_start: datetime,
    window_end: datetime,
) -> dict[str, Any]:
    window_seconds = max((window_end - window_start).total_seconds(), 0.0)
    if window_seconds <= 0:
        return {
            "percent": None,
            "state_summary": "unknown",
            "eligible_seconds": 0,
            "available_seconds": 0,
            "unavailable_seconds": 0,
            "unobserved_seconds": 0,
            "sample_count": 0,
            "insufficient_data": True,
            "low_coverage": True,
            "score_ready": False,
            "reason": "invalid_window",
            "basis": (
                "Hub-observed device availability from last_seen transitions; "
                "unobserved Hub gaps excluded"
            ),
            "gaps": [],
        }

    ordered = sorted(samples, key=lambda item: _ensure_aware(item.observed_at))
    available = 0.0
    unavailable = 0.0
    unobserved = 0.0
    gaps: list[dict[str, str]] = []

    if not ordered:
        unobserved = window_seconds
        gaps.append(
            {
                "start": window_start.isoformat(),
                "end": window_end.isoformat(),
                "certainty": "inferred",
            }
        )
    else:
        first_at = _ensure_aware(ordered[0].observed_at)
        if first_at > window_start:
            leading = (first_at - window_start).total_seconds()
            unobserved += leading
            gaps.append(
                {
                    "start": window_start.isoformat(),
                    "end": first_at.isoformat(),
                    "certainty": "inferred",
                }
            )
        for index, sample in enumerate(ordered):
            start = max(_ensure_aware(sample.observed_at), window_start)
            if index + 1 < len(ordered):
                end = min(_ensure_aware(ordered[index + 1].observed_at), window_end)
            else:
                end = window_end
            if end <= start:
                continue
            avail, unavail, unobs = _attribute_hold(
                sample.overall_availability, start, end
            )
            available += avail
            unavailable += unavail
            unobserved += unobs
            if unobs > 0:
                gaps.append(
                    {
                        "start": start.isoformat(),
                        "end": end.isoformat(),
                        "certainty": "inferred",
                    }
                )

    eligible = available + unavailable
    insufficient = eligible < MIN_ELIGIBLE_SECONDS
    low_coverage = eligible < (window_seconds * LOW_COVERAGE_FRACTION)
    short_window = window_seconds <= 24 * 3600 + 1
    score_ready = (not insufficient) and (short_window or not low_coverage)
    percent = (
        round(100.0 * available / eligible, 2)
        if eligible > 0 and not insufficient
        else None
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
        "sample_count": len(ordered),
        "insufficient_data": insufficient,
        "low_coverage": low_coverage,
        "score_ready": score_ready,
        "reason": reason,
        "basis": (
            "Availability % = confirmed available time / eligible observation "
            "time × 100. Unobserved Hub gaps are excluded. Requires at least "
            "10 minutes of eligible observation. Multi-day windows are not "
            "score_ready until eligible time covers at least half the window."
        ),
        "gaps": gaps,
    }


def build_monitoring_summary(db: Session, node_id: str) -> dict[str, Any]:
    """Build device monitoring summary for node detail. Does not mutate state."""
    node_id = node_id.upper()
    now = _utcnow()
    node = db.get(Node, node_id)
    if node is None:
        raise KeyError(node_id)

    reported = db.get(ReportedState, node_id)
    last_seen = reported.last_seen_at if reported else None
    if last_seen is not None:
        last_seen = _ensure_aware(last_seen)

    excluded = node.lifecycle_state != "active"
    current = connectivity_state(last_seen, now=now)
    if excluded:
        current_state = "excluded"
    elif current is None:
        current_state = "unknown"
    else:
        current_state = current

    history_start = now - timedelta(days=settings.device_monitor_retention_days)
    samples = list(
        db.scalars(
            select(DeviceMonitorSample)
            .where(
                DeviceMonitorSample.hub_id == settings.hub_id,
                DeviceMonitorSample.node_id == node_id,
                DeviceMonitorSample.observed_at >= history_start,
            )
            .order_by(DeviceMonitorSample.observed_at.asc())
        )
    )

    windows: dict[str, Any] = {}
    all_gaps: list[dict[str, str]] = []
    for label, delta in WINDOW_SPECS:
        window = compute_availability_window(
            samples,
            window_start=now - delta,
            window_end=now,
        )
        windows[label] = window
        for gap in window.get("gaps") or []:
            all_gaps.append({"window": label, **gap})

    last_change = db.scalar(
        select(DeviceMonitorEvent)
        .where(
            DeviceMonitorEvent.hub_id == settings.hub_id,
            DeviceMonitorEvent.node_id == node_id,
            DeviceMonitorEvent.event_type.in_(
                (
                    "heartbeat_overdue",
                    "heartbeat_resumed",
                    "availability_changed",
                    "monitoring_started",
                    "monitoring_resumed",
                )
            ),
        )
        .order_by(DeviceMonitorEvent.occurred_at.desc())
        .limit(1)
    )

    sample_count = db.scalar(
        select(func.count())
        .select_from(DeviceMonitorSample)
        .where(
            DeviceMonitorSample.hub_id == settings.hub_id,
            DeviceMonitorSample.node_id == node_id,
        )
    )

    reported_payload: dict[str, Any] | None = None
    if reported and reported.payload_json:
        try:
            parsed = json.loads(reported.payload_json)
            if isinstance(parsed, dict):
                reported_payload = parsed
        except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
            reported_payload = None

    online = current == "available"
    health = device_diag.health_summary_for_node(
        db,
        node_id,
        online=online and not excluded,
        reported_payload=reported_payload,
    )

    notes = [
        "Availability is Hub-observed from last_seen relative to the online window.",
        "Health is Hub-assessed from optional device-reported diag (D2).",
        "nodes.availability (SERVICE/OPERATIONAL/…) is a separate operational mode field.",
    ]
    if excluded:
        notes.append("Deactivated devices are excluded from further sampling.")
    if not health.get("has_diag_evidence"):
        notes.append(
            "No device diagnostics observed yet; health stays unknown until "
            "firmware publishes diag on health/keepalive."
        )

    return {
        "enabled": settings.device_monitor_enabled,
        "phase": "D2",
        "eligibility": "excluded" if excluded else "active",
        "health": health,
        "availability": {
            "current_state": current_state,
            "online_window_seconds": settings.device_online_window_s,
            "last_seen_at": last_seen.isoformat() if last_seen else None,
            "windows": windows,
            "gaps": all_gaps,
            "insufficient_data": any(
                window.get("insufficient_data") for window in windows.values()
            ),
        },
        "last_state_change": _event_view(last_change) if last_change else None,
        "events_endpoint": f"/api/v1/nodes/{node_id}/monitoring/events",
        "sample_count": int(sample_count or 0),
        "config": {
            "online_window_seconds": settings.device_online_window_s,
            "retention_days": settings.device_monitor_retention_days,
            "max_samples_per_device": settings.device_monitor_max_samples_per_device,
            "max_events_per_device": settings.device_monitor_max_events_per_device,
            "diag_max_events_per_device": settings.device_diag_max_events_per_device,
            "diag_rssi_degraded_dbm": settings.device_diag_rssi_degraded_dbm,
            "hub_monitor_interval_seconds": settings.hub_monitor_interval_s,
        },
        "notes": notes,
    }


def _event_view(event: DeviceMonitorEvent) -> dict[str, Any]:
    return {
        "event_id": event.event_id,
        "node_id": event.node_id,
        "occurred_at": _ensure_aware(event.occurred_at).isoformat(),
        "observed_at": _ensure_aware(event.observed_at).isoformat(),
        "event_type": event.event_type,
        "previous_state": event.previous_state,
        "new_state": event.new_state,
        "reason": event.reason,
        "source": event.source,
        "certainty": event.certainty,
        "kind": "monitor",
    }


def list_recent_events(
    db: Session, node_id: str, *, limit: int = RECENT_EVENT_LIMIT
) -> dict[str, Any]:
    """Newest Hub-observed monitor events plus device-reported diag events."""
    node_id = node_id.upper()
    limit = max(1, min(int(limit), 50))
    monitor_events = [
        _event_view(event)
        for event in db.scalars(
            select(DeviceMonitorEvent)
            .where(
                DeviceMonitorEvent.hub_id == settings.hub_id,
                DeviceMonitorEvent.node_id == node_id,
            )
            .order_by(DeviceMonitorEvent.occurred_at.desc())
            .limit(limit)
        )
    ]
    diag_events = device_diag.list_diag_events(db, node_id, limit=limit)
    merged = sorted(
        [*monitor_events, *diag_events],
        key=lambda item: item.get("occurred_at") or "",
        reverse=True,
    )[:limit]
    return {
        "node_id": node_id,
        "events": merged,
        "limit": limit,
        "count": len(merged),
    }
