from __future__ import annotations

import logging
import threading
from datetime import UTC, datetime, timedelta
from time import monotonic
from typing import Any

from sqlalchemy.orm import Session

from app import monitoring_availability, monitoring_cycle, probes
from app.config import settings
from app.db import SessionLocal
from app.models import (
    PublicMonitorCheckpoint,
    PublicMonitorEvent,
    PublicMonitorSample,
)

log = logging.getLogger("operations.public-monitor")

COMPONENTS = {
    "public_api": "Public API",
    "public_mqtt": "Public MQTT",
}
_stop = threading.Event()
_threads: dict[str, threading.Thread] = {}
_current: dict[
    str, tuple[datetime | None, tuple[str, str | None], datetime | None]
] = {}
_current_lock = threading.Lock()


def _utcnow() -> datetime:
    return datetime.now(UTC)


def enabled(component: str) -> bool:
    if component == "public_api":
        return settings.public_api_monitor_enabled
    return settings.public_mqtt_monitor_enabled


def _store(component: str) -> monitoring_cycle.MonitorStore:
    return monitoring_cycle.MonitorStore(
        PublicMonitorSample,
        PublicMonitorEvent,
        PublicMonitorCheckpoint,
        hub_id=settings.hub_id,
        component=component,
        source="operations_public_monitor",
    )


def record_check(
    db: Session,
    component: str,
    observed_at: datetime,
    result: tuple[str, str | None],
    previous_check_at: datetime | None,
) -> None:
    state, reason = result
    availability = monitoring_availability.component_state(state)
    health = {
        "available": "healthy",
        "unavailable": "unhealthy",
        "unobserved": "unknown",
    }[availability]
    _store(component).record_check(
        db,
        observed_at,
        states={component: state},
        reasons={component: reason},
        health=health,
        availability=availability,
        previous_check_at=previous_check_at,
        threshold=timedelta(seconds=max(2 * settings.public_monitor_interval_s, 10)),
    )


def _loop(component: str, probe) -> None:
    previous_check_at: datetime | None = None
    last_success: datetime | None = None
    next_prune = monotonic() + 600
    while not _stop.is_set():
        start = monotonic()
        try:
            result = probe()
        except Exception:
            log.exception("Public monitoring probe failed: %s", component)
            result = ("unknown", "Probe could not be completed")
        if _stop.is_set():
            break
        now = _utcnow()
        db = SessionLocal()
        try:
            record_check(db, component, now, result, previous_check_at)
            if monotonic() >= next_prune:
                _store(component).prune(db, now, settings.retention_days)
                db.commit()
                next_prune = monotonic() + 600
            previous_check_at = now
            if result[0] == "ok":
                last_success = now
            with _current_lock:
                _current[component] = now, result, last_success
        except Exception:
            log.exception("Could not persist public monitoring check: %s", component)
            db.rollback()
        finally:
            db.close()
        elapsed = monotonic() - start
        _stop.wait(max(0.0, settings.public_monitor_interval_s - elapsed))


def start() -> dict[str, threading.Thread]:
    for component, probe in (
        ("public_api", probes.probe_public_api),
        ("public_mqtt", probes.probe_public_mqtt),
    ):
        if not enabled(component):
            continue
        if component in _threads and _threads[component].is_alive():
            continue
        try:
            thread = threading.Thread(
                target=_loop,
                args=(component, probe),
                name=f"operations-{component}",
                daemon=True,
            )
            thread.start()
            _threads[component] = thread
        except RuntimeError:
            log.exception("Could not start public monitor: %s", component)
    return dict(_threads)


def stop() -> None:
    _stop.set()


def worker_status() -> dict[str, Any]:
    result: dict[str, Any] = {}
    now = _utcnow()
    stale_after = timedelta(seconds=max(2 * settings.public_monitor_interval_s, 10))
    for component, label in COMPONENTS.items():
        with _current_lock:
            checked_at, (state, reason), last_success = _current.get(
                component,
                (None, ("unknown", "Waiting for first check"), None),
            )
        thread = _threads.get(component)
        stale = checked_at is None or now - checked_at > stale_after
        result[component] = {
            "label": label,
            "enabled": enabled(component),
            "running": thread is not None and thread.is_alive(),
            "stale": stale,
            "state": state,
            "reason": reason,
            "checked_at": checked_at.isoformat() if checked_at else None,
            "last_success_at": (last_success.isoformat() if last_success else None),
        }
    return result


def summary(db: Session, now: datetime) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for component, label in COMPONENTS.items():
        if not enabled(component):
            continue
        store = _store(component)
        history = store.history(db, now, settings.retention_days)
        payloads = {
            row.sample_id: monitoring_cycle.sample_payload(row) for row in history
        }
        points = monitoring_cycle.observations(
            history,
            payloads,
            component_id=component,
        )
        stale = store.stale_boundary(
            db,
            points,
            now,
            previous_check_at=_current.get(component, (None,))[0],
            threshold=timedelta(
                seconds=max(2 * settings.public_monitor_interval_s, 10)
            ),
        )
        if stale is not None:
            points.append(stale)
        with _current_lock:
            checked_at, (state, reason), last_success = _current.get(
                component,
                (None, ("unknown", "Waiting for first check"), None),
            )
        result[component] = {
            "label": label,
            "state": state,
            "reason": reason,
            "checked_at": checked_at.isoformat() if checked_at else None,
            "last_success_at": (last_success.isoformat() if last_success else None),
            "interval_seconds": settings.public_monitor_interval_s,
            "windows": monitoring_availability.component_windows(
                points,
                component_id=component,
                now=now,
                interval_s=settings.public_monitor_interval_s,
            ),
            "recent_events": [
                monitoring_cycle.event_view(event)
                for event in store.recent_events(db, 5)
            ],
        }
    return result
