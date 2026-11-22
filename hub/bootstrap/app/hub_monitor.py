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
from datetime import UTC, datetime, timedelta
from time import monotonic
from typing import Any
from uuid import uuid4

from sqlalchemy import delete, select, text
from sqlalchemy.orm import Session

from app import (
    container_logs,
    edge_status,
    monitoring_availability,
    monitoring_cycle,
    policy,
    public_monitor,
)
from app.config import settings
from app.db import SessionLocal
from app.models import HubMonitorCheckpoint, HubMonitorEvent, HubMonitorSample
from app.monitoring_availability import (
    COMPONENT_UNAVAILABLE_STATES,
    OK_STATES,
    WINDOW_SPECS,
)
from app.monitoring_math import ensure_aware
from app.mqtt_listener import get_broker_metrics, is_connected

log = logging.getLogger("hub-bootstrap.hub-monitor")

# Reachability path: browser → Nginx → Web/API/DB. Failure ⇒ unhealthy Hub.
REACHABILITY_COMPONENTS = ("api", "database", "nginx", "web")
COMPONENT_IDS = ("api", "database", "nginx", "web", "mqtt", "opa")
# Firmware Builder is intentionally excluded from Health and Availability.
EXCLUDED_FROM_MONITORING = frozenset(
    {"firmware-builder", "firmware_builder", "firmware"}
)
COMPONENT_WINDOW_IDS = ("api", "mqtt", "web")
RECENT_EVENT_LIMIT = 15
CLEANUP_EVERY_CYCLES = 10
# New Hub samples are transition-sparse and hold-attributed. Legacy dense rows
# omit this marker and keep interval attribution for long inter-sample gaps.
SAMPLE_MODE_SPARSE = "sparse"

_thread: threading.Thread | None = None
_stop = threading.Event()
_cycle_lock = threading.Lock()
_last_check_at: datetime | None = None
_last_docker_signatures: dict[str, tuple[Any, ...]] = {}
_cycles_since_cleanup = 0


def _utcnow() -> datetime:
    return datetime.now(UTC)


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


def _record_monitor_boundary(
    db: Session, observed_at: datetime, previous: HubMonitorSample | None,
    gap: monitoring_cycle.ObservationGap | None,
) -> None:
    if previous is None and gap is None and _last_check_at is None:
        _record_event(
            db,
            component="monitor",
            event_type="monitoring_resumed",
            previous_state=None,
            new_state="running",
            reason="monitoring started with no prior samples",
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="confirmed",
        )
    if gap is not None and gap.exceeds_threshold:
        _record_event(
            db,
            component="monitor",
            event_type="monitoring_resumed",
            previous_state="unobserved",
            new_state="running",
            reason=(
                f"monitoring resumed after an unobserved gap of "
                f"{int((gap.end - gap.start).total_seconds())}s starting {gap.start.isoformat()}"
            ),
            occurred_at=observed_at,
            observed_at=observed_at,
            certainty="inferred",
        )


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


def _retention_anchor(db: Session, cutoff: datetime) -> HubMonitorSample | None:
    """Carry forward the last sparse boundary without extending dense history."""
    sample = db.scalar(
        select(HubMonitorSample)
        .where(
            HubMonitorSample.hub_id == settings.hub_id,
            HubMonitorSample.observed_at < cutoff,
        )
        .order_by(HubMonitorSample.observed_at.desc())
        .limit(1)
    )
    if sample is not None and _is_sparse_sample(sample, _parse_sample_payload(sample)):
        return sample
    return None


def cleanup_history(db: Session, now: datetime | None = None) -> dict[str, int]:
    """Delete monitor rows older than retention. Does not touch other tables."""
    now = ensure_aware(now or _utcnow())
    days = settings.hub_monitor_retention_days
    cutoff = monitoring_cycle.retention_cutoff(now, days)
    # SQLite returns naive datetimes into the ORM identity map. Evaluating the
    # DELETE against those objects with an aware cutoff raises TypeError, so skip
    # synchronize_session and let SQL perform the filter.
    anchor = _retention_anchor(db, cutoff)
    expired = delete(HubMonitorSample).where(
        HubMonitorSample.hub_id == settings.hub_id,
        HubMonitorSample.observed_at < cutoff,
    )
    if anchor is not None:
        expired = expired.where(HubMonitorSample.sample_id != anchor.sample_id)
    samples = db.execute(
        expired,
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


def _add_unobserved_gap_sample(
    db: Session, *, previous: HubMonitorSample, gap: monitoring_cycle.ObservationGap
) -> HubMonitorSample:
    """Insert an explicit unobserved marker so hold attribution excludes Hub-down time."""
    sample = HubMonitorSample(
        sample_id=str(uuid4()),
        hub_id=settings.hub_id,
        observed_at=gap.marker_at(previous.observed_at),
        overall_availability="unobserved",
        overall_health="unknown",
        components_json=json.dumps(
            {
                "states": {},
                "last_success": {},
                "reasons": {"monitor": "unobserved monitoring gap"},
                "sample_mode": SAMPLE_MODE_SPARSE,
                "gap": True,
            },
            separators=(",", ":"),
            sort_keys=True,
        ),
        source="local_monitor",
    )
    db.add(sample)
    return sample


def _sample_state(sample: HubMonitorSample) -> tuple:
    states = _parse_sample_payload(sample)["states"]
    return (
        sample.overall_health,
        sample.overall_availability,
        tuple(states.get(name) for name in COMPONENT_IDS),
    )


def run_check(
    db: Session,
    *,
    sample_devices: bool = True,
    include_summary: bool = True,
) -> dict[str, Any] | None:
    """Persist one Hub check and optionally return its historical summary."""
    global _cycles_since_cleanup, _last_check_at
    with _cycle_lock:
        probe = _probe_components(db)
        observed_at: datetime = probe["observed_at"]
        components: dict[str, str] = probe["components"]
        health, availability = assess_states(components)
        previous = _latest_sample(db)
        checkpoint = (
            db.get(HubMonitorCheckpoint, settings.hub_id)
            if _last_check_at is None and previous is not None
            else None
        )
        gap = monitoring_cycle.plan_gap(
            observed_at,
            anchor_at=previous.observed_at if previous is not None else None,
            previous_check_at=_last_check_at,
            persisted_checkpoint=checkpoint.last_check_at if checkpoint else None,
            threshold=timedelta(seconds=settings.hub_monitor_gap_threshold_s),
        )

        def record_history() -> HubMonitorSample | None:
            _record_monitor_boundary(db, observed_at, previous, gap)
            if gap is not None and previous is not None and previous.overall_availability != "unobserved":
                _add_unobserved_gap_sample(db, previous=previous, gap=gap)
            _emit_transitions(
                db, previous, components=components, health=health,
                availability=availability, reasons=probe["reasons"], observed_at=observed_at,
            )
            _detect_docker_restarts(db, probe["docker"], observed_at)

            prior_state = (
                _sample_state(previous)
                if previous is not None and previous.overall_availability != "unobserved"
                else None
            )
            current_state = (health, availability, tuple(components.get(name) for name in COMPONENT_IDS))
            sample = previous
            if monitoring_cycle.needs_sample(prior_state, current_state, gap=gap is not None):
                payload = {
                    "states": components,
                    "last_success": probe["last_success"],
                    "reasons": probe["reasons"],
                    "sample_mode": SAMPLE_MODE_SPARSE,
                }
                sample = HubMonitorSample(
                    sample_id=str(uuid4()), hub_id=settings.hub_id, observed_at=observed_at,
                    overall_availability=availability, overall_health=health,
                    components_json=json.dumps(payload, separators=(",", ":"), sort_keys=True),
                    source="local_monitor",
                )
                db.add(sample)
            # Device sampling stays on the existing Core cycle and cadence.
            from app import device_monitor

            if sample_devices or gap is not None:
                try:
                    device_monitor.sample_devices(
                        db, observed_at=observed_at,
                        hub_gap=(gap.start, gap.end) if gap is not None else None,
                    )
                except Exception:
                    log.exception("device monitor cycle failed")
            return sample

        def save_checkpoint(_sample: HubMonitorSample | None, value: str) -> None:
            db.merge(HubMonitorCheckpoint(hub_id=settings.hub_id, last_check_at=value))

        sample = monitoring_cycle.commit_cycle(
            db, observed_at, record_history=record_history, save_checkpoint=save_checkpoint,
        )
        _last_check_at = observed_at
        # Retention cleanup must not block or roll back a successful sample.
        _cycles_since_cleanup += 1
        if _cycles_since_cleanup >= CLEANUP_EVERY_CYCLES:
            try:
                from app import device_diag, device_monitor

                cleanup_history(db, observed_at)
                device_monitor.cleanup_history(db, observed_at)

                device_diag.cleanup_history(db, observed_at)
                db.commit()
            except Exception:
                log.exception("hub monitor retention cleanup failed")
                try:
                    db.rollback()
                except Exception as rollback_exc:  # noqa: BLE001
                    log.debug("rollback after cleanup failure: %s", rollback_exc)
            _cycles_since_cleanup = 0
        if include_summary:
            return build_monitoring_summary(db, current_probe=probe, current_sample=sample)
        return None


def _parse_sample_payload(sample: HubMonitorSample) -> dict[str, Any]:
    empty = {
        "states": {},
        "last_success": {},
        "reasons": {},
        "docker": {},
        "sample_mode": None,
    }
    try:
        payload = json.loads(sample.components_json)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
        return empty
    if not isinstance(payload, dict):
        return empty
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
        "sample_mode": payload.get("sample_mode")
        if isinstance(payload.get("sample_mode"), str)
        else None,
    }


def _is_sparse_sample(sample: HubMonitorSample, payload: dict[str, Any]) -> bool:
    return (
        sample.overall_availability == "unobserved"
        or payload.get("sample_mode") == SAMPLE_MODE_SPARSE
    )


def _observations(
    samples: list[HubMonitorSample],
    payloads_by_sample_id: dict[str, dict[str, Any]] | None,
    *,
    component_id: str | None = None,
) -> list[monitoring_availability.Observation]:
    observations = []
    for sample in samples:
        payload = payloads_by_sample_id.get(sample.sample_id) if payloads_by_sample_id else None
        if payload is None:
            payload = _parse_sample_payload(sample)
        state = sample.overall_availability
        if component_id is not None:
            states = payload.get("states")
            raw = states.get(component_id) if isinstance(states, dict) else None
            state = monitoring_availability.component_state(str(raw) if raw is not None else None)
        observations.append(monitoring_availability.Observation(
            sample.observed_at, state,
            sparse=_is_sparse_sample(sample, payload),
        ))
    return observations


def compute_availability_window(
    samples: list[HubMonitorSample],
    *,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
    payloads_by_sample_id: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Core availability follows the stored overall state and sample mode."""
    return monitoring_availability.core_window(
        _observations(samples, payloads_by_sample_id), window_start=window_start,
        window_end=window_end, interval_s=interval_s,
    )


def compute_component_availability_window(
    samples: list[HubMonitorSample],
    *,
    component_id: str,
    window_start: datetime,
    window_end: datetime,
    interval_s: float,
    payloads_by_sample_id: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    return monitoring_availability.component_window(
        _observations(samples, payloads_by_sample_id, component_id=component_id),
        component_id=component_id, window_start=window_start, window_end=window_end,
        interval_s=interval_s,
    )


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
        calculated_at = ensure_aware(sample.observed_at)
    else:
        states = {name: "unknown" for name in COMPONENT_IDS}
        last_success = {}
        reasons = {"monitor": "no monitoring samples yet"}
        docker = {}
        health = "unknown"
        availability = "unobserved"
        calculated_at = now

    history_start = monitoring_cycle.retention_cutoff(now, settings.hub_monitor_retention_days)
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
    anchor = _retention_anchor(db, history_start)
    if anchor is not None:
        history.insert(0, anchor)
    # Parse each sample payload once for all windows (P0). Pass the full
    # retention history so hold can carry the latest pre-window sample in.
    payloads_by_sample_id = {
        sample.sample_id: _parse_sample_payload(sample) for sample in history
    }
    overall_observations = _observations(history, payloads_by_sample_id)
    stale_boundary = None
    if overall_observations and overall_observations[-1].sparse:
        checkpoint = db.get(HubMonitorCheckpoint, settings.hub_id)
        gap_at = monitoring_cycle.coverage_end_if_stale(
            now,
            anchor_at=overall_observations[-1].observed_at,
            previous_check_at=_last_check_at,
            persisted_checkpoint=checkpoint.last_check_at if checkpoint else None,
            threshold=timedelta(seconds=settings.hub_monitor_gap_threshold_s),
        )
        if gap_at is not None:
            stale_boundary = monitoring_availability.Observation(gap_at, "unobserved")
            overall_observations.append(stale_boundary)
    windows: dict[str, Any] = {}
    # Report gaps once from the widest window so the UI does not repeat the same
    # unobserved interval under 24h/7d/30d.
    all_gaps: list[dict[str, str]] = []
    for name, delta in WINDOW_SPECS:
        window = monitoring_availability.core_window(
            overall_observations, window_start=now - delta, window_end=now,
            interval_s=settings.hub_monitor_interval_s,
        )
        gaps = window.pop("gaps", [])
        windows[name] = window
        if name == "30d":
            tagged = [
                {
                    **gap,
                    "window": name,
                }
                for gap in gaps
                if isinstance(gap, dict)
            ]
            # Keep Hub-down gaps even when the window also has a long
            # "monitoring had not started" gap at the front of the list.
            not_running_gaps = [
                gap for gap in tagged if gap.get("kind") == "not_running"
            ]
            other_gaps = [
                gap for gap in tagged if gap.get("kind") != "not_running"
            ]
            all_gaps = (not_running_gaps + other_gaps)[:10]

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
            "occurred_at": ensure_aware(last_event.occurred_at).isoformat(),
            "observed_at": ensure_aware(last_event.observed_at).isoformat(),
            "component": last_event.component,
            "event_type": last_event.event_type,
            "previous_state": last_event.previous_state,
            "new_state": last_event.new_state,
            "reason": last_event.reason,
            "certainty": last_event.certainty,
        }

    component_windows: dict[str, dict[str, Any]] = {}
    for component_id in COMPONENT_WINDOW_IDS:
        observations = _observations(history, payloads_by_sample_id, component_id=component_id)
        if stale_boundary is not None:
            observations.append(stale_boundary)
        component_windows[component_id] = {
            name: monitoring_availability.component_window(
                observations, component_id=component_id, window_start=now - delta,
                window_end=now, interval_s=settings.hub_monitor_interval_s,
            )
            for name, delta in WINDOW_SPECS
        }

    insufficient = any(window.get("insufficient_data") for window in windows.values())
    failure_reason = health_failure_reason(states)
    result = {
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
            "calculated_at": ensure_aware(calculated_at).isoformat(),
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
    try:
        public_components = public_monitor.summary(db, now)
    except Exception:
        log.exception("failed to build public monitoring summary")
    else:
        if public_components:
            result["public_components"] = public_components
    return result


def _event_view(event: HubMonitorEvent) -> dict[str, Any]:
    return monitoring_cycle.event_view(
        event_id=event.event_id, occurred_at=event.occurred_at,
        observed_at=event.observed_at, component=event.component,
        event_type=event.event_type, previous_state=event.previous_state,
        new_state=event.new_state, reason=event.reason,
        source=event.source, certainty=event.certainty,
    )


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
    from app import device_monitor

    next_hub = next_device = monotonic()
    hub_check_succeeded = False
    while not _stop.is_set():
        now = monotonic()
        hub_due = now >= next_hub
        device_due = settings.device_monitor_enabled and now >= next_device
        if not hub_due and not device_due:
            deadline = next_hub
            if settings.device_monitor_enabled:
                deadline = min(deadline, next_device)
            _stop.wait(deadline - now)
            continue

        if hub_due:
            next_hub = now + settings.hub_monitor_interval_s
            hub_check_succeeded = False
        if device_due:
            next_device = now + settings.device_monitor_interval_s

        if not hub_due and not hub_check_succeeded:
            continue

        db = SessionLocal()
        try:
            if hub_due:
                run_check(db, sample_devices=device_due, include_summary=False)
                hub_check_succeeded = True
            else:
                # Fast checks observe device connectivity only. A failed Hub
                # cycle pauses them until the next successful full check.
                with _cycle_lock:
                    observed_at = _utcnow()
                    if _last_check_at is None or (
                        observed_at - _last_check_at
                    ).total_seconds() > settings.hub_monitor_gap_threshold_s:
                        # Wall time can advance while the scheduler clock is
                        # paused (for example during host suspend).
                        continue
                    device_monitor.sample_devices(db, observed_at=observed_at)
                    db.commit()
        except Exception:
            log.exception("%s monitor cycle failed", "hub" if hub_due else "device")
            try:
                db.rollback()
            except Exception as rollback_exc:  # noqa: BLE001
                log.debug("rollback after monitor cycle failure: %s", rollback_exc)
        finally:
            db.close()


def start_monitor_thread() -> threading.Thread | None:
    """Start the background monitor once. Safe to call repeatedly."""
    global _thread, _last_check_at
    if not settings.hub_monitor_enabled:
        log.info("hub monitor disabled")
        return None
    if _thread is not None and _thread.is_alive():
        return _thread
    _last_check_at = None
    _stop.clear()
    _thread = threading.Thread(target=_loop, name="hub-monitor", daemon=True)
    _thread.start()
    public_monitor.start(_stop)
    log.info(
        "hub monitor started interval=%ss retention=%sd",
        settings.hub_monitor_interval_s,
        settings.hub_monitor_retention_days,
    )
    return _thread


def stop_monitor_thread() -> None:
    """Signal the monitor thread to stop (tests)."""
    _stop.set()
