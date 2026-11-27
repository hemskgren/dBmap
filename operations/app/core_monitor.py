from __future__ import annotations

import logging
import threading
from datetime import UTC, datetime, timedelta
from time import monotonic
from typing import Any
from uuid import uuid4

from sqlalchemy.orm import Session

from app import monitoring_cycle, probes
from app.config import settings
from app.db import SessionLocal
from app.models import (
    OperationsMonitorCheckpoint,
    OperationsMonitorEvent,
    OperationsMonitorSample,
    OperationsRuntimeCheckpoint,
)
from app.monitoring_availability import (
    COMPONENT_UNAVAILABLE_STATES,
    OK_STATES,
    WINDOW_SPECS,
)
from app.monitoring_cycle import MonitorStore
from app.monitoring_math import ensure_aware

log = logging.getLogger("operations.core-monitor")

COMPONENT_IDS = (
    "api",
    "database",
    "hub_api_via_nginx",
    "nginx_local",
    "web",
    "mqtt",
    "opa",
)
COMPONENT_WINDOW_IDS = (
    "api",
    "hub_api_via_nginx",
    "nginx_local",
    "mqtt",
    "web",
)
REACHABILITY_COMPONENTS = ("api", "database", "hub_api_via_nginx", "web")

_thread: threading.Thread | None = None
_stop = threading.Event()
_cycle_lock = threading.Lock()
_last_check_at: datetime | None = None
_last_successful_cycle_at: datetime | None = None
_cycles_since_cleanup = 0


def _utcnow() -> datetime:
    return datetime.now(UTC)


def health_score(state: str) -> int | None:
    return {"healthy": 100, "degraded": 70, "unhealthy": 0}.get(state)


def assess_states(components: dict[str, str]) -> tuple[str, str]:
    reachability_ok = all(
        components.get(name) in OK_STATES for name in REACHABILITY_COMPONENTS
    )
    if not reachability_ok:
        unknown_core = any(
            components.get(name) in {None, "unknown"}
            for name in REACHABILITY_COMPONENTS
        )
        if unknown_core and not any(
            components.get(name) in COMPONENT_UNAVAILABLE_STATES
            for name in REACHABILITY_COMPONENTS
        ):
            return "unknown", "unavailable"
        return "unhealthy", "unavailable"

    mqtt_state = components.get("mqtt", "unknown")
    opa_state = components.get("opa", "unknown")
    if mqtt_state in OK_STATES and opa_state in OK_STATES:
        return "healthy", "available"
    if mqtt_state not in OK_STATES:
        return "degraded", "unavailable"
    return "degraded", "degraded"


def health_failure_reason(components: dict[str, str]) -> str | None:
    failed = [
        name
        for name in REACHABILITY_COMPONENTS
        if components.get(name) in COMPONENT_UNAVAILABLE_STATES
    ]
    if failed:
        return "Reachability path failed: " + ", ".join(failed)
    mqtt_state = components.get("mqtt", "unknown")
    if mqtt_state not in OK_STATES:
        return f"MQTT broker is {mqtt_state}"
    opa_state = components.get("opa", "unknown")
    if opa_state not in OK_STATES:
        return f"Policy engine is {opa_state}"
    return None


def _store() -> MonitorStore:
    return MonitorStore(
        OperationsMonitorSample,
        OperationsMonitorEvent,
        OperationsMonitorCheckpoint,
        hub_id=settings.hub_id,
        source="operations_monitor",
        legacy_components=(*COMPONENT_IDS, "nginx"),
        component_aliases=(("nginx", "hub_api_via_nginx"),),
    )


def _sample_payload(sample: OperationsMonitorSample) -> dict[str, Any]:
    payload = monitoring_cycle.sample_payload(
        sample,
        (*COMPONENT_IDS, "nginx"),
    )
    states = payload["states"]
    if "hub_api_via_nginx" not in states and "nginx" in states:
        states["hub_api_via_nginx"] = states["nginx"]
    states.pop("nginx", None)
    return payload


def _probe_components() -> dict[str, Any]:
    observed_at = _utcnow()
    states: dict[str, str] = {}
    reasons: dict[str, str] = {}
    last_success: dict[str, str] = {}

    api_state, api_reason = probes.probe_api()
    states["api"] = api_state
    if api_state == "ok":
        last_success["api"] = observed_at.isoformat()
    elif api_reason:
        reasons["api"] = api_reason

    database_state, database_reason = probes.probe_database()
    states["database"] = database_state
    if database_state == "ok":
        last_success["database"] = observed_at.isoformat()
    elif database_reason:
        reasons["database"] = database_reason

    nginx_state, nginx_reason = probes.probe_nginx()
    states["hub_api_via_nginx"] = nginx_state
    if nginx_state == "ok":
        last_success["hub_api_via_nginx"] = observed_at.isoformat()
    elif nginx_reason:
        reasons["hub_api_via_nginx"] = nginx_reason

    nginx_local_state, nginx_local_reason = probes.probe_nginx_local()
    states["nginx_local"] = nginx_local_state
    if nginx_local_state == "ok":
        last_success["nginx_local"] = observed_at.isoformat()
    elif nginx_local_reason:
        reasons["nginx_local"] = nginx_local_reason

    web_state, web_reason = probes.probe_web()
    states["web"] = web_state
    if web_state == "ok":
        last_success["web"] = observed_at.isoformat()
    elif web_reason:
        reasons["web"] = web_reason

    mqtt_state, mqtt_reason = probes.probe_mqtt_broker()
    states["mqtt"] = mqtt_state
    if mqtt_state == "ok":
        last_success["mqtt"] = observed_at.isoformat()
    elif mqtt_reason:
        reasons["mqtt"] = mqtt_reason

    opa_state, opa_reason = probes.probe_opa()
    states["opa"] = opa_state
    if opa_state == "ok":
        last_success["opa"] = observed_at.isoformat()
    elif opa_reason:
        reasons["opa"] = opa_reason

    return {
        "components": states,
        "last_success": last_success,
        "reasons": reasons,
        "observed_at": observed_at,
    }


def run_check(db: Session, *, include_summary: bool = False) -> dict[str, Any] | None:
    global _last_check_at, _last_successful_cycle_at
    probe = _probe_components()
    observed_at = probe["observed_at"]
    states = probe["components"]
    health, availability = assess_states(states)
    store = _store()

    def after_observation(previous, _gap):
        if previous is None:
            if health != "unknown":
                store.record_event(
                    db,
                    component="hub",
                    event_type="health_changed",
                    previous_state=None,
                    new_state=health,
                    reason="initial hub health",
                    occurred_at=observed_at,
                    observed_at=observed_at,
                )
            if availability != "unobserved":
                store.record_event(
                    db,
                    component="hub",
                    event_type="availability_changed",
                    previous_state=None,
                    new_state=availability,
                    reason="initial hub availability",
                    occurred_at=observed_at,
                    observed_at=observed_at,
                )
            return
        if previous.overall_health != health:
            store.record_event(
                db,
                component="hub",
                event_type="health_changed",
                previous_state=previous.overall_health,
                new_state=health,
                reason="overall hub health changed",
                occurred_at=observed_at,
                observed_at=observed_at,
            )
        if previous.overall_availability != availability:
            store.record_event(
                db,
                component="hub",
                event_type="availability_changed",
                previous_state=previous.overall_availability,
                new_state=availability,
                reason="overall hub availability changed",
                occurred_at=observed_at,
                observed_at=observed_at,
            )

    store.record_check(
        db,
        observed_at,
        states=states,
        reasons=probe["reasons"],
        health=health,
        availability=availability,
        last_success=probe["last_success"],
        previous_check_at=_last_check_at,
        threshold=timedelta(seconds=settings.gap_threshold_s),
        after_observation=after_observation,
    )
    _last_check_at = observed_at
    _last_successful_cycle_at = observed_at
    _detect_runtime_restarts(
        db,
        probes.inspect_monitored_runtimes(),
        observed_at,
    )
    db.commit()
    if include_summary:
        return build_status(db)
    return None


def cleanup_history(db: Session, now: datetime | None = None) -> dict:
    return _store().prune(
        db,
        ensure_aware(now or _utcnow()),
        settings.retention_days,
    )


def _detect_runtime_restarts(
    db: Session,
    runtimes: dict[str, dict],
    observed_at: datetime,
) -> None:
    component_names = {"hub-api": "api", "mqtt": "mqtt"}
    for service, info in runtimes.items():
        if not isinstance(info, dict) or info.get("error"):
            continue
        started_at = info.get("started_at")
        restart_count = info.get("restart_count")
        if not isinstance(started_at, str):
            started_at = None
        if not isinstance(restart_count, int):
            restart_count = None
        if started_at is None and restart_count is None:
            continue

        checkpoint = db.get(
            OperationsRuntimeCheckpoint,
            {"hub_id": settings.hub_id, "service": service},
        )
        if checkpoint is None:
            db.add(
                OperationsRuntimeCheckpoint(
                    hub_id=settings.hub_id,
                    service=service,
                    container_id=info.get("container_id"),
                    started_at=started_at,
                    restart_count=restart_count,
                )
            )
            continue

        previous_started_at = checkpoint.started_at
        previous_restart_count = checkpoint.restart_count
        if (
            previous_started_at != started_at
            or previous_restart_count != restart_count
        ):
            reason_parts = []
            if previous_started_at != started_at and started_at:
                reason_parts.append(f"started_at changed to {started_at}")
            if previous_restart_count != restart_count and restart_count is not None:
                reason_parts.append(
                    f"restart_count {previous_restart_count} -> {restart_count}"
                )
            db.add(
                OperationsMonitorEvent(
                    event_id=str(uuid4()),
                    hub_id=settings.hub_id,
                    occurred_at=observed_at,
                    observed_at=observed_at,
                    component=component_names.get(service, service),
                    event_type="service_restart_detected",
                    previous_state="running",
                    new_state="restart_detected",
                    reason="; ".join(reason_parts)
                    or "container runtime signature changed",
                    source="operations_monitor",
                    certainty="inferred",
                )
            )
        checkpoint.container_id = info.get("container_id")
        checkpoint.started_at = started_at
        checkpoint.restart_count = restart_count


def build_status(db: Session) -> dict[str, Any]:
    now = _utcnow()
    store = _store()
    history = store.history(db, now, settings.retention_days)
    from app import monitoring_availability

    payloads = {
        sample.sample_id: _sample_payload(sample)
        for sample in history
    }
    overall = monitoring_cycle.observations(history, payloads)
    stale = store.stale_boundary(
        db,
        overall,
        now,
        previous_check_at=_last_check_at,
        threshold=timedelta(seconds=settings.gap_threshold_s),
    )
    if stale is not None:
        overall.append(stale)

    windows: dict[str, Any] = {}
    all_gaps: list[dict[str, str]] = []
    for name, delta in WINDOW_SPECS:
        window = monitoring_availability.core_window(
            overall,
            window_start=now - delta,
            window_end=now,
            interval_s=settings.core_monitor_interval_s,
        )
        gaps = window.pop("gaps", [])
        windows[name] = window
        if name == "30d":
            all_gaps = [gap | {"window": "30d"} for gap in gaps][:10]

    component_windows: dict[str, Any] = {}
    for component_id in COMPONENT_WINDOW_IDS:
        points = monitoring_cycle.observations(
            history,
            payloads,
            component_id=component_id,
        )
        component_windows[component_id] = monitoring_availability.component_windows(
            points,
            component_id=component_id,
            now=now,
            interval_s=settings.core_monitor_interval_s,
        )

    sample = store.latest_sample(db)
    if sample is not None:
        payload = _sample_payload(sample)
        states = payload["states"]
        health = sample.overall_health
        availability = sample.overall_availability
        calculated_at = ensure_aware(sample.observed_at).isoformat()
        last_success = payload["last_success"]
        reasons = payload["reasons"]
    else:
        states = {name: "unknown" for name in COMPONENT_IDS}
        health = "unknown"
        availability = "unobserved"
        calculated_at = now.isoformat()
        last_success = {}
        reasons = {"monitor": "no monitoring samples yet"}

    components = [
        {
            "id": component_id,
            "name": {
                "api": "Hub API",
                "database": "Database",
                "hub_api_via_nginx": "Hub API via Nginx",
                "nginx_local": "Nginx",
                "web": "Web",
                "mqtt": "MQTT broker",
                "opa": "Policy engine",
            }[component_id],
            "state": states.get(component_id, "unknown"),
            "last_success_at": last_success.get(component_id),
            "reason": reasons.get(component_id),
        }
        for component_id in COMPONENT_IDS
    ]

    events = [
        monitoring_cycle.event_view(event)
        for event in store.recent_events(
            db,
            15,
            event_types=(
                "state_changed",
                "health_changed",
                "availability_changed",
                "monitoring_resumed",
                "service_restart_detected",
            ),
        )
    ]

    return {
        "health": {
            "state": health,
            "score": health_score(health),
            "reason": health_failure_reason(states),
            "components": components,
            "calculated_at": calculated_at,
        },
        "availability": {
            "current_state": availability,
            "label": "Core Hub availability",
            "definition": (
                "Whether core Hub functions for device communication were "
                "confirmed available. MQTT or reachability failure is "
                "unavailable; unobserved periods are excluded."
            ),
            "windows": windows,
            "components": {
                "api": {
                    "label": "API availability",
                    "windows": component_windows["api"],
                },
                "mqtt": {
                    "label": "MQTT availability",
                    "windows": component_windows["mqtt"],
                },
                "web": {
                    "label": "Web availability",
                    "windows": component_windows["web"],
                },
            },
            "gaps": all_gaps,
            "retention_days": settings.retention_days,
            "interval_seconds": settings.core_monitor_interval_s,
        },
        "last_checks": last_success,
        "recent_events": events,
        "sample_count": len(history),
    }


def worker_status() -> dict[str, Any]:
    stale = (
        _last_successful_cycle_at is None
        or _utcnow() - _last_successful_cycle_at
        > timedelta(
            seconds=max(settings.gap_threshold_s, settings.core_monitor_interval_s * 2)
        )
    )
    return {
        "enabled": settings.core_monitor_enabled,
        "running": _thread is not None and _thread.is_alive(),
        "last_successful_check": (
            _last_successful_cycle_at.isoformat()
            if _last_successful_cycle_at is not None
            else None
        ),
        "stale": stale,
    }


def _loop() -> None:
    global _cycles_since_cleanup
    next_check = monotonic()
    while not _stop.is_set():
        now = monotonic()
        if now < next_check:
            _stop.wait(max(0.0, next_check - now))
            continue
        db = SessionLocal()
        try:
            with _cycle_lock:
                run_check(db)
                _cycles_since_cleanup += 1
                if _cycles_since_cleanup >= 10:
                    cleanup_history(db)
                    db.commit()
                    _cycles_since_cleanup = 0
        except Exception:
            log.exception("Core monitoring cycle failed")
            try:
                db.rollback()
            except Exception:
                log.exception("Failed to roll back core monitoring cycle")
        finally:
            db.close()
        next_check = monotonic() + settings.core_monitor_interval_s


def start() -> threading.Thread | None:
    global _thread
    if not settings.core_monitor_enabled:
        log.info("Core monitor disabled")
        return None
    if _thread is not None and _thread.is_alive():
        return _thread
    _stop.clear()
    _thread = threading.Thread(
        target=_loop,
        name="operations-core-monitor",
        daemon=True,
    )
    _thread.start()
    log.info(
        "Core monitor started interval=%ss retention=%sd",
        settings.core_monitor_interval_s,
        settings.retention_days,
    )
    return _thread


def stop() -> None:
    _stop.set()
