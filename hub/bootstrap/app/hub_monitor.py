"""Local Hub health and availability monitoring.

Availability and Health are independent. Availability is historical and based on
confirmed observation intervals. Health is the current rule-based component state.
A Local Hub cannot observe itself while fully offline; those intervals are
recorded as unobserved gaps after monitoring resumes.
"""

from __future__ import annotations

import json
import logging
import threading
from datetime import datetime, timedelta, timezone
from typing import Any
from uuid import uuid4

from sqlalchemy import delete, select, text
from sqlalchemy.orm import Session

from app import container_logs, edge_status, policy
from app.config import settings
from app.db import SessionLocal
from app.models import HubMonitorEvent, HubMonitorSample
from app.mqtt_listener import get_broker_metrics, is_connected

log = logging.getLogger("hub-bootstrap.hub-monitor")

# Reachability path: browser → Nginx → Web/API/DB. Failure ⇒ unhealthy Hub.
REACHABILITY_COMPONENTS = ("api", "database", "nginx", "web")
# Device communication depends on MQTT; API is already in reachability.
# MQTT failure keeps health at degraded (score 70) but marks core Hub
# availability unavailable so historical % reflects the outage.
CRITICAL_AVAILABILITY_COMPONENTS = ("api", "mqtt")
COMPONENT_IDS = ("api", "database", "nginx", "web", "mqtt", "opa")
# Firmware Builder is intentionally excluded from Health and Availability.
EXCLUDED_FROM_MONITORING = frozenset(
    {"firmware-builder", "firmware_builder", "firmware"}
)
OK_STATES = frozenset({"ok", "online", "healthy", "disabled"})
COMPONENT_UNAVAILABLE_STATES = frozenset(
    {"offline", "unhealthy", "error", "unavailable"}
)
WINDOW_SPECS = (
    ("24h", timedelta(hours=24)),
    ("7d", timedelta(days=7)),
    ("30d", timedelta(days=30)),
)
COMPONENT_WINDOW_IDS = ("api", "mqtt", "web")
MIN_ELIGIBLE_SECONDS = 600
LOW_COVERAGE_FRACTION = 0.5
RECENT_EVENT_LIMIT = 15
CLEANUP_EVERY_CYCLES = 10
# Avoid presenting a rounded 100% when any confirmed unavailable time exists.
PERCENT_QUANTUM = 0.01

_thread: threading.Thread | None = None
_stop = threading.Event()
_cycle_lock = threading.Lock()
_last_docker_signatures: dict[str, tuple[Any, ...]] = {}
_cycles_since_cleanup = 0


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _ensure_aware(value: datetime) -> datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def health_score(state: str) -> int | None:
    if state == "healthy":
        return 100
    if state == "degraded":
        return 70
    if state == "unhealthy":
        return 0
    return None


def _failed_components(components: dict[str, str], names: tuple[str, ...]) -> list[str]:
    return [
        name for name in names if components.get(name) in COMPONENT_UNAVAILABLE_STATES
    ]


def assess_states(components: dict[str, str]) -> tuple[str, str]:
    """Return (overall_health, overall_availability) from component probe states.

    Health and availability are independent:
    - Reachability failure (API/DB/Nginx/Web) ⇒ unhealthy / unavailable.
    - MQTT failure with reachability ok ⇒ degraded health (score 70) and
      **unavailable** core Hub availability (device communication is down).
    - OPA-only failure ⇒ degraded / degraded (degraded still counts as
      available for core Hub availability percentage; devices can still talk).
    """
    reachability_ok = all(
        components.get(name) in OK_STATES for name in REACHABILITY_COMPONENTS
    )
    if not reachability_ok:
        unknown_core = any(
            components.get(name) in {None, "unknown"}
            for name in REACHABILITY_COMPONENTS
        )
        if unknown_core and not _failed_components(components, REACHABILITY_COMPONENTS):
            return "unknown", "unavailable"
        return "unhealthy", "unavailable"

    mqtt_state = components.get("mqtt", "unknown")
    opa_state = components.get("opa", "unknown")
    mqtt_ok = mqtt_state in OK_STATES
    opa_ok = opa_state in OK_STATES
    if mqtt_ok and opa_ok:
        return "healthy", "available"
    if not mqtt_ok:
        # MQTT is required for device communication; do not hide this as a
        # "degraded-but-available" interval in the core Hub availability %.
        return "degraded", "unavailable"
    return "degraded", "degraded"


def health_failure_reason(components: dict[str, str]) -> str | None:
    """Plain-language reason for the current overall health state."""
    reachability_failed = _failed_components(components, REACHABILITY_COMPONENTS)
    if reachability_failed:
        return (
            "Reachability path failed: "
            + ", ".join(reachability_failed)
            + ". The web UI may still load from cache while API or dependencies are down."
        )
    mqtt_state = components.get("mqtt", "unknown")
    if mqtt_state not in OK_STATES:
        return (
            "MQTT broker is "
            f"{mqtt_state}; devices cannot communicate with the Hub until it recovers. "
            "A working web session does not mean MQTT is available."
        )
    opa_state = components.get("opa", "unknown")
    if opa_state not in OK_STATES:
        return f"Policy engine is {opa_state}; authorization decisions may fail."
    return None


def _probe_components(db: Session) -> dict[str, Any]:
    components: dict[str, str] = {}
    last_success: dict[str, str | None] = {}
    reasons: dict[str, str] = {}
    now = _utcnow()

    try:
        db.execute(text("SELECT 1"))
        components["api"] = "ok"
        components["database"] = "ok"
        last_success["api"] = now.isoformat()
        last_success["database"] = now.isoformat()
    except Exception as exc:  # noqa: BLE001 - monitoring must not crash the Hub
        log.warning("hub monitor database probe failed: %s", exc)
        try:
            db.rollback()
        except Exception as rollback_exc:  # noqa: BLE001
            log.debug("rollback after database probe failure: %s", rollback_exc)
        components["api"] = "ok"
        components["database"] = "unhealthy"
        last_success["api"] = now.isoformat()
        last_success["database"] = None
        reasons["database"] = "database query failed"

    try:
        edge = edge_status.statuses()
    except Exception as exc:  # noqa: BLE001
        log.warning("hub monitor edge probes failed: %s", exc)
        edge = {"nginx": "unknown", "web": "unknown", "firmware-builder": "unknown"}
        reasons["nginx"] = "edge probe failed"
        reasons["web"] = "edge probe failed"

    nginx_state = edge.get("nginx", "unknown")
    web_state = edge.get("web", "unknown")
    components["nginx"] = "ok" if nginx_state == "online" else nginx_state
    components["web"] = "ok" if web_state == "online" else web_state
    if components["nginx"] == "ok":
        last_success["nginx"] = now.isoformat()
    else:
        last_success["nginx"] = None
        reasons.setdefault("nginx", f"nginx probe returned {nginx_state}")
    if components["web"] == "ok":
        last_success["web"] = now.isoformat()
    else:
        last_success["web"] = None
        reasons.setdefault("web", f"web probe returned {web_state}")

    if not settings.mqtt_enabled:
        components["mqtt"] = "disabled"
        last_success["mqtt"] = now.isoformat()
    elif is_connected():
        components["mqtt"] = "ok"
        last_success["mqtt"] = now.isoformat()
        metrics = get_broker_metrics()
        if metrics.get("updated_at") is None:
            reasons["mqtt"] = "connected but broker metrics not yet received"
    else:
        components["mqtt"] = "offline"
        last_success["mqtt"] = None
        reasons["mqtt"] = "hub MQTT session is not connected"

    try:
        opa = policy.health_summary()
        opa_status = str(opa.get("status") or "unknown")
        components["opa"] = "ok" if opa_status == "healthy" else opa_status
        if components["opa"] == "ok":
            last_success["opa"] = now.isoformat()
        else:
            last_success["opa"] = None
            reasons["opa"] = str(opa.get("message") or f"OPA status {opa_status}")
    except Exception as exc:  # noqa: BLE001
        log.warning("hub monitor OPA probe failed: %s", exc)
        components["opa"] = "unavailable"
        last_success["opa"] = None
        reasons["opa"] = "OPA health probe failed"

    docker: dict[str, Any] = {}
    try:
        docker = container_logs.inspect_monitored_runtimes()
    except Exception as exc:  # noqa: BLE001
        log.warning("hub monitor docker inspect failed: %s", exc)
        docker = {"error": str(exc)}
    # Defense in depth: never persist Firmware Builder in HA samples/events.
    if isinstance(docker, dict):
        docker = {
            key: value
            for key, value in docker.items()
            if key not in EXCLUDED_FROM_MONITORING
        }

    return {
        "components": components,
        "last_success": last_success,
        "reasons": reasons,
        "docker": docker,
        "observed_at": now,
    }


def _record_event(
    db: Session,
    *,
    component: str,
    event_type: str,
    previous_state: str | None,
    new_state: str | None,
    reason: str | None,
    occurred_at: datetime,
    observed_at: datetime,
    certainty: str = "confirmed",
) -> None:
    db.add(
        HubMonitorEvent(
            event_id=str(uuid4()),
            hub_id=settings.hub_id,
            occurred_at=occurred_at,
            observed_at=observed_at,
            component=component,
            event_type=event_type,
            previous_state=previous_state,
            new_state=new_state,
            reason=(reason or "")[:256] or None,
            source="local_monitor",
            certainty=certainty,
        )
    )


def _latest_sample(db: Session) -> HubMonitorSample | None:
    return db.scalar(
        select(HubMonitorSample)
        .where(HubMonitorSample.hub_id == settings.hub_id)
        .order_by(HubMonitorSample.observed_at.desc())
        .limit(1)
    )


def _maybe_record_gap(db: Session, now: datetime) -> tuple[datetime, datetime] | None:
    """Record a monitoring_resumed event when needed.

    Returns (gap_start, gap_end) when an unobserved wall-clock gap was detected
    so device monitoring can exclude that interval from availability.
    """
    previous = _latest_sample(db)
    if previous is None:
        _record_event(
            db,
            component="monitor",
            event_type="monitoring_resumed",
            previous_state=None,
            new_state="running",
            reason="monitoring started with no prior samples",
            occurred_at=now,
            observed_at=now,
            certainty="confirmed",
        )
        return None
    previous_at = _ensure_aware(previous.observed_at)
    gap = now - previous_at
    threshold = timedelta(seconds=2 * settings.hub_monitor_interval_s)
    if gap <= threshold:
        return None
    _record_event(
        db,
        component="monitor",
        event_type="monitoring_resumed",
        previous_state="unobserved",
        new_state="running",
        reason=(
            f"monitoring resumed after an unobserved gap of "
            f"{int(gap.total_seconds())}s starting {previous_at.isoformat()}"
        ),
        # Stamp at detection time so the event appears in recent-event lists.
        occurred_at=now,
        observed_at=now,
        certainty="inferred",
    )
    return previous_at, now


def _detect_docker_restarts(
    db: Session, docker: dict[str, Any], observed_at: datetime
) -> None:
    for component, info in docker.items():
        if component in EXCLUDED_FROM_MONITORING:
            continue
        if not isinstance(info, dict) or "error" in info:
            continue
        signature = (info.get("started_at"), info.get("restart_count"))
        previous = _last_docker_signatures.get(component)
        _last_docker_signatures[component] = signature
        if previous is None or previous == signature:
            continue
        if previous[0] == signature[0] and previous[1] == signature[1]:
            continue
        reason_parts = []
        if previous[0] != signature[0] and signature[0]:
            reason_parts.append(f"started_at changed to {signature[0]}")
        if previous[1] != signature[1] and signature[1] is not None:
            reason_parts.append(f"restart_count {previous[1]} -> {signature[1]}")
        mapped = {
            "hub-api": "api",
            "mosquitto": "mqtt",
        }.get(component, component)
        if mapped in EXCLUDED_FROM_MONITORING:
            continue
        # previous_state/new_state describe the runtime transition, not a
        # confirmed availability duration. Restart counts belong in reason.
        _record_event(
            db,
            component=mapped,
            event_type="service_restart_detected",
            previous_state="running",
            new_state="restart_detected",
            reason="; ".join(reason_parts) or "container runtime signature changed",
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="inferred",
        )


def _emit_transitions(
    db: Session,
    previous: HubMonitorSample | None,
    *,
    components: dict[str, str],
    health: str,
    availability: str,
    reasons: dict[str, str],
    observed_at: datetime,
) -> None:
    previous_components: dict[str, str] = {}
    previous_health = None
    previous_availability = None
    if previous is not None:
        previous_components = _parse_sample_payload(previous)["states"]
        previous_health = previous.overall_health
        previous_availability = previous.overall_availability

    for name in COMPONENT_IDS:
        old = previous_components.get(name)
        new = components.get(name)
        if old == new or new is None:
            continue
        _record_event(
            db,
            component=name,
            event_type="state_changed",
            previous_state=old,
            new_state=new,
            reason=reasons.get(name),
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="confirmed",
        )

    if previous_health != health:
        _record_event(
            db,
            component="hub",
            event_type="health_changed",
            previous_state=previous_health,
            new_state=health,
            reason="overall hub health changed",
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="confirmed",
        )
    if previous_availability != availability:
        _record_event(
            db,
            component="hub",
            event_type="availability_changed",
            previous_state=previous_availability,
            new_state=availability,
            reason="overall hub availability state changed",
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="confirmed",
        )


def cleanup_history(db: Session, now: datetime | None = None) -> dict[str, int]:
    """Delete monitor rows older than retention. Does not touch other tables."""
    now = _ensure_aware(now or _utcnow())
    days = settings.hub_monitor_retention_days
    cutoff = now - timedelta(days=days)
    # SQLite returns naive datetimes into the ORM identity map. Evaluating the
    # DELETE against those objects with an aware cutoff raises TypeError, so skip
    # synchronize_session and let SQL perform the filter.
    samples = db.execute(
        delete(HubMonitorSample).where(
            HubMonitorSample.hub_id == settings.hub_id,
            HubMonitorSample.observed_at < cutoff,
        ),
        execution_options={"synchronize_session": False},
    )
    events = db.execute(
        delete(HubMonitorEvent).where(
            HubMonitorEvent.hub_id == settings.hub_id,
            HubMonitorEvent.occurred_at < cutoff,
        ),
        execution_options={"synchronize_session": False},
    )
    return {
        "samples_deleted": samples.rowcount or 0,
        "events_deleted": events.rowcount or 0,
        "cutoff": cutoff.isoformat(),
        "retention_days": days,
    }


def run_check(db: Session, *, record_startup_gap: bool = True) -> dict[str, Any]:
    """Run one monitoring cycle and persist sample/events. Safe against probe failures."""
    global _cycles_since_cleanup
    with _cycle_lock:
        probe = _probe_components(db)
        observed_at: datetime = probe["observed_at"]
        components: dict[str, str] = probe["components"]
        health, availability = assess_states(components)
        hub_gap: tuple[datetime, datetime] | None = None
        if record_startup_gap:
            hub_gap = _maybe_record_gap(db, observed_at)
        previous = _latest_sample(db)
        _emit_transitions(
            db,
            previous,
            components=components,
            health=health,
            availability=availability,
            reasons=probe["reasons"],
            observed_at=observed_at,
        )
        _detect_docker_restarts(db, probe["docker"], observed_at)
        payload = {
            "states": components,
            "last_success": probe["last_success"],
            "reasons": probe["reasons"],
            "docker": probe["docker"],
        }
        sample = HubMonitorSample(
            sample_id=str(uuid4()),
            hub_id=settings.hub_id,
            observed_at=observed_at,
            overall_availability=availability,
            overall_health=health,
            components_json=json.dumps(payload, separators=(",", ":"), sort_keys=True),
            source="local_monitor",
        )
        db.add(sample)
        # Device sampling shares the Hub cycle; failures must not lose the Hub sample.
        from app import device_monitor

        try:
            device_monitor.sample_devices(db, observed_at=observed_at, hub_gap=hub_gap)
        except Exception:
            log.exception("device monitor cycle failed")
        db.commit()
        # Retention cleanup must not block or roll back a successful sample.
        _cycles_since_cleanup += 1
        if _cycles_since_cleanup >= CLEANUP_EVERY_CYCLES:
            try:
                cleanup_history(db, observed_at)
                device_monitor.cleanup_history(db, observed_at)
                from app import device_diag

                device_diag.cleanup_history(db, observed_at)
                db.commit()
            except Exception:
                log.exception("hub monitor retention cleanup failed")
                try:
                    db.rollback()
                except Exception as rollback_exc:  # noqa: BLE001
                    log.debug("rollback after cleanup failure: %s", rollback_exc)
            _cycles_since_cleanup = 0
        return build_monitoring_summary(db, current_probe=probe, current_sample=sample)


def _parse_sample_payload(sample: HubMonitorSample) -> dict[str, Any]:
    try:
        payload = json.loads(sample.components_json)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
        return {"states": {}, "last_success": {}, "reasons": {}, "docker": {}}
    if not isinstance(payload, dict):
        return {"states": {}, "last_success": {}, "reasons": {}, "docker": {}}
    states = payload.get("states")
    if not isinstance(states, dict):
        # Back-compat if a test stored a flat map.
        states = {
            key: value
            for key, value in payload.items()
            if key in COMPONENT_IDS and isinstance(value, str)
        }
    return {
        "states": {str(k): str(v) for k, v in states.items()},
        "last_success": payload.get("last_success")
        if isinstance(payload.get("last_success"), dict)
        else {},
        "reasons": payload.get("reasons")
        if isinstance(payload.get("reasons"), dict)
        else {},
        "docker": payload.get("docker")
        if isinstance(payload.get("docker"), dict)
        else {},
    }


def _attribute_interval(
    state: str, start: datetime, end: datetime, interval: timedelta
) -> tuple[float, float, float]:
    """Return (available_seconds, unavailable_seconds, unobserved_seconds).

    `degraded` counts as available for the core Hub percentage (e.g. OPA-only
    failure). MQTT failure is stored as `unavailable`, not `degraded`.
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
        "sample_count": sample_count,
        "insufficient_data": insufficient,
        "low_coverage": low_coverage,
        "score_ready": score_ready,
        "reason": reason,
        "basis": basis,
        "gaps": gaps,
    }


def compute_availability_window(
    samples: list[HubMonitorSample],
    *,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
) -> dict[str, Any]:
    """Core Hub availability from overall sample states.

    MQTT outages are recorded as overall_availability=unavailable and therefore
    reduce this percentage. OPA-only degraded intervals still count as available.
    """
    interval = timedelta(seconds=interval_s)
    window_seconds = max((window_end - window_start).total_seconds(), 0.0)
    basis = (
        "Core Hub availability % = confirmed available-or-OPA-degraded time / "
        "eligible observation time × 100. MQTT or reachability failure is "
        "unavailable. Unobserved gaps are excluded. A percentage requires at "
        "least 10 minutes of eligible observation. Multi-day windows are not "
        "score_ready until eligible time covers at least half the window."
    )
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
            avail, unavail, unobs = _attribute_interval(
                sample.overall_availability, start, end, interval
            )
            available += avail
            unavailable += unavail
            unobserved += unobs
            if unobs > 0:
                confirmed_end = start + timedelta(seconds=avail + unavail)
                if confirmed_end < end:
                    gaps.append(
                        {
                            "start": confirmed_end.isoformat(),
                            "end": end.isoformat(),
                            "certainty": "inferred",
                        }
                    )

    return _window_result(
        available=available,
        unavailable=unavailable,
        unobserved=unobserved,
        window_seconds=window_seconds,
        sample_count=len(ordered),
        gaps=gaps,
        basis=basis,
    )


def _component_sample_state(sample: HubMonitorSample, component_id: str) -> str:
    states = _parse_sample_payload(sample)["states"]
    raw = states.get(component_id)
    if raw is None or raw == "unknown":
        return "unobserved"
    if raw in OK_STATES:
        return "available"
    if raw in COMPONENT_UNAVAILABLE_STATES:
        return "unavailable"
    return "unobserved"


def compute_component_availability_window(
    samples: list[HubMonitorSample],
    *,
    component_id: str,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
) -> dict[str, Any]:
    """Independent component availability from per-sample component states."""
    interval = timedelta(seconds=interval_s)
    window_seconds = max((window_end - window_start).total_seconds(), 0.0)
    basis = (
        f"{component_id} availability % = confirmed ok time / eligible observation "
        "time × 100. Unknown component states are unobserved and excluded. "
        "Independent of overall Hub availability."
    )
    if window_seconds <= 0:
        return _window_result(
            available=0,
            unavailable=0,
            unobserved=0,
            window_seconds=0,
            sample_count=0,
            gaps=[],
            basis=basis,
        ) | {
            "component": component_id,
            "insufficient_data": True,
            "reason": "invalid_window",
            "percent": None,
        }

    ordered = sorted(samples, key=lambda item: _ensure_aware(item.observed_at))
    available = 0.0
    unavailable = 0.0
    unobserved = 0.0
    gaps: list[dict[str, str]] = []
    if not ordered:
        unobserved = window_seconds
    else:
        first_at = _ensure_aware(ordered[0].observed_at)
        if first_at > window_start:
            unobserved += (first_at - window_start).total_seconds()
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
            state = _component_sample_state(sample, component_id)
            avail, unavail, unobs = _attribute_interval(state, start, end, interval)
            available += avail
            unavailable += unavail
            unobserved += unobs
            if unobs > 0:
                confirmed_end = start + timedelta(seconds=avail + unavail)
                if confirmed_end < end:
                    gaps.append(
                        {
                            "start": confirmed_end.isoformat(),
                            "end": end.isoformat(),
                            "certainty": "inferred",
                        }
                    )

    result = _window_result(
        available=available,
        unavailable=unavailable,
        unobserved=unobserved,
        window_seconds=window_seconds,
        sample_count=len(ordered),
        gaps=gaps,
        basis=basis,
    )
    result["component"] = component_id
    return result


def _component_views(
    states: dict[str, str],
    last_success: dict[str, Any],
    reasons: dict[str, Any],
    docker: dict[str, Any],
) -> list[dict[str, Any]]:
    labels = {
        "api": "Hub API",
        "database": "Database",
        "nginx": "Nginx",
        "web": "Web",
        "mqtt": "MQTT broker",
        "opa": "Policy engine",
    }
    docker_key = {
        "api": "hub-api",
        "mqtt": "mosquitto",
        "database": None,
        "nginx": "nginx",
        "web": "web",
        "opa": "opa",
    }
    views = []
    for component_id in COMPONENT_IDS:
        runtime = None
        key = docker_key.get(component_id)
        if key and isinstance(docker.get(key), dict):
            runtime = {
                "docker_state": docker[key].get("docker_state"),
                "health_status": docker[key].get("health_status"),
                "started_at": docker[key].get("started_at"),
                "restart_count": docker[key].get("restart_count"),
                "error": docker[key].get("error"),
            }
        views.append(
            {
                "id": component_id,
                "name": labels[component_id],
                "state": states.get(component_id, "unknown"),
                "last_success_at": last_success.get(component_id),
                "reason": reasons.get(component_id),
                "docker": runtime,
            }
        )
    return views


def build_monitoring_summary(
    db: Session,
    *,
    current_probe: dict[str, Any] | None = None,
    current_sample: HubMonitorSample | None = None,
) -> dict[str, Any]:
    now = _utcnow()
    sample = current_sample or _latest_sample(db)
    if current_probe is not None:
        states = current_probe["components"]
        last_success = current_probe["last_success"]
        reasons = current_probe["reasons"]
        docker = current_probe["docker"]
        health, availability = assess_states(states)
        calculated_at = current_probe["observed_at"]
    elif sample is not None:
        payload = _parse_sample_payload(sample)
        states = payload["states"]
        last_success = payload["last_success"]
        reasons = payload["reasons"]
        docker = payload["docker"]
        health = sample.overall_health
        availability = sample.overall_availability
        calculated_at = _ensure_aware(sample.observed_at)
    else:
        states = {name: "unknown" for name in COMPONENT_IDS}
        last_success = {}
        reasons = {"monitor": "no monitoring samples yet"}
        docker = {}
        health = "unknown"
        availability = "unobserved"
        calculated_at = now

    history_start = now - timedelta(days=settings.hub_monitor_retention_days)
    history = list(
        db.scalars(
            select(HubMonitorSample)
            .where(
                HubMonitorSample.hub_id == settings.hub_id,
                HubMonitorSample.observed_at >= history_start,
            )
            .order_by(HubMonitorSample.observed_at.asc())
        )
    )
    windows: dict[str, Any] = {}
    # Report gaps once from the widest window so the UI does not repeat the same
    # unobserved interval under 24h/7d/30d.
    all_gaps: list[dict[str, str]] = []
    for name, delta in WINDOW_SPECS:
        window = compute_availability_window(
            [
                item
                for item in history
                if _ensure_aware(item.observed_at) >= now - delta
            ],
            window_start=now - delta,
            window_end=now,
            interval_s=settings.hub_monitor_interval_s,
        )
        gaps = window.pop("gaps", [])
        windows[name] = window
        if name == "30d":
            all_gaps = [
                {
                    **gap,
                    "window": name,
                }
                for gap in gaps
                if isinstance(gap, dict)
            ]

    # Avoid loading the event list on every Hub status refresh; callers that need
    # the timeline use list_recent_events() / GET /api/v1/hub/monitoring/events.
    last_event = db.scalar(
        select(HubMonitorEvent)
        .where(
            HubMonitorEvent.hub_id == settings.hub_id,
            HubMonitorEvent.event_type.in_(
                (
                    "state_changed",
                    "health_changed",
                    "availability_changed",
                    "monitoring_resumed",
                    "service_restart_detected",
                )
            ),
        )
        .order_by(HubMonitorEvent.occurred_at.desc())
        .limit(1)
    )
    last_change = None
    if last_event is not None:
        last_change = {
            "occurred_at": _ensure_aware(last_event.occurred_at).isoformat(),
            "observed_at": _ensure_aware(last_event.observed_at).isoformat(),
            "component": last_event.component,
            "event_type": last_event.event_type,
            "previous_state": last_event.previous_state,
            "new_state": last_event.new_state,
            "reason": last_event.reason,
            "certainty": last_event.certainty,
        }

    component_windows: dict[str, dict[str, Any]] = {}
    for component_id in COMPONENT_WINDOW_IDS:
        component_windows[component_id] = {}
        for name, delta in WINDOW_SPECS:
            component_windows[component_id][name] = (
                compute_component_availability_window(
                    [
                        item
                        for item in history
                        if _ensure_aware(item.observed_at) >= now - delta
                    ],
                    component_id=component_id,
                    window_start=now - delta,
                    window_end=now,
                    interval_s=settings.hub_monitor_interval_s,
                )
            )

    insufficient = any(window.get("insufficient_data") for window in windows.values())
    failure_reason = health_failure_reason(states)
    return {
        "health": {
            "state": health,
            "score": health_score(health),
            "score_definition": {
                "healthy": 100,
                "degraded": 70,
                "unhealthy": 0,
                "unknown": None,
            },
            "reason": failure_reason,
            "components": _component_views(states, last_success, reasons, docker),
            "calculated_at": _ensure_aware(calculated_at).isoformat(),
            "notes": (
                "Health is the current rule-based state. Reachability path is API, "
                "database, Nginx, and Web (failure ⇒ unhealthy). MQTT failure with "
                "reachability ok ⇒ degraded (score 70); a working web session does "
                "not mean devices can communicate. Firmware Builder is excluded. "
                "Docker inspect fields are diagnostic only and do not invent outage "
                "durations."
            ),
        },
        "availability": {
            "current_state": availability,
            "label": "Core Hub availability",
            "definition": (
                "Whether core Hub functions for device communication were confirmed "
                "available. MQTT or reachability failure counts as unavailable. "
                "OPA-only degradation still counts as available. Web UI reachability "
                "alone is not sufficient."
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
            "insufficient_data": insufficient,
            "gaps": all_gaps[:10],
            "retention_days": settings.hub_monitor_retention_days,
            "interval_seconds": settings.hub_monitor_interval_s,
            "notes": (
                "Core Hub availability is historical and independent of the current "
                "health score. Unobserved intervals after a Hub outage or monitoring "
                "gap are excluded from the percentage denominator. Component "
                "availability windows are calculated independently from the same "
                "samples. Confirmed unavailable duration is reported per metric; "
                "MQTT unavailable duration is not total Hub downtime unless "
                "reachability also failed."
            ),
        },
        "last_checks": {
            component_id: last_success.get(component_id)
            for component_id in COMPONENT_IDS
        },
        "last_state_change": last_change,
        "recent_events": [],
        "events_endpoint": "/api/v1/hub/monitoring/events",
        "sample_count": len(history),
    }


def _event_view(event: HubMonitorEvent) -> dict[str, Any]:
    return {
        "event_id": event.event_id,
        "occurred_at": _ensure_aware(event.occurred_at).isoformat(),
        "observed_at": _ensure_aware(event.observed_at).isoformat(),
        "component": event.component,
        "event_type": event.event_type,
        "previous_state": event.previous_state,
        "new_state": event.new_state,
        "reason": event.reason,
        "source": event.source,
        "certainty": event.certainty,
    }


def list_recent_events(
    db: Session, *, limit: int = RECENT_EVENT_LIMIT
) -> dict[str, Any]:
    """Return the newest monitoring events for on-demand UI expansion."""
    bounded = max(1, min(int(limit), 50))
    # Fetch a little extra then drop excluded components (e.g. historical noise).
    events = [
        event
        for event in db.scalars(
            select(HubMonitorEvent)
            .where(HubMonitorEvent.hub_id == settings.hub_id)
            .order_by(HubMonitorEvent.occurred_at.desc())
            .limit(bounded * 2)
        )
        if event.component not in EXCLUDED_FROM_MONITORING
    ][:bounded]
    return {
        "events": [_event_view(event) for event in events],
        "limit": bounded,
        "count": len(events),
    }


def monitoring_status(db: Session) -> dict[str, Any]:
    """Read-only monitoring payload for the Hub status API."""
    try:
        return build_monitoring_summary(db)
    except Exception:
        log.exception("failed to build monitoring summary")
        return {
            "health": {
                "state": "unknown",
                "score": None,
                "components": [],
                "calculated_at": _utcnow().isoformat(),
                "notes": "monitoring summary unavailable",
            },
            "availability": {
                "current_state": "unobserved",
                "windows": {},
                "insufficient_data": True,
                "gaps": [],
                "notes": "monitoring summary unavailable",
            },
            "last_checks": {},
            "last_state_change": None,
            "recent_events": [],
            "events_endpoint": "/api/v1/hub/monitoring/events",
            "sample_count": 0,
            "error": "monitoring summary unavailable",
        }


def _loop() -> None:
    while not _stop.is_set():
        db = SessionLocal()
        try:
            run_check(db)
        except Exception:
            log.exception("hub monitor cycle failed")
            try:
                db.rollback()
            except Exception as rollback_exc:  # noqa: BLE001
                log.debug("rollback after monitor cycle failure: %s", rollback_exc)
        finally:
            db.close()
        _stop.wait(settings.hub_monitor_interval_s)


def start_monitor_thread() -> threading.Thread | None:
    """Start the background monitor once. Safe to call repeatedly."""
    global _thread
    if not settings.hub_monitor_enabled:
        log.info("hub monitor disabled")
        return None
    if _thread is not None and _thread.is_alive():
        return _thread
    _stop.clear()
    _thread = threading.Thread(target=_loop, name="hub-monitor", daemon=True)
    _thread.start()
    log.info(
        "hub monitor started interval=%ss retention=%sd",
        settings.hub_monitor_interval_s,
        settings.hub_monitor_retention_days,
    )
    return _thread


def stop_monitor_thread() -> None:
    """Signal the monitor thread to stop (tests)."""
    _stop.set()
