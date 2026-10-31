"""Device-reported diagnostics ingest and health assessment (D2).

Availability remains Hub-observed (D0). Health is rule-based from optional
`diag` on health/keepalive plus RSSI on the reported payload. Diagnostic
events are capped at DBMAP_DEVICE_DIAG_MAX_EVENTS_PER_DEVICE (default 30)
per device with FIFO eviction. Bursts of the same type within the aggregate
window are merged into one retained row.
"""

from __future__ import annotations

import json
import logging
from datetime import datetime, timedelta, timezone
from typing import Any
from uuid import uuid4

from sqlalchemy import delete, func, select
from sqlalchemy.orm import Session

from app.config import settings
from app.models import DeviceDiagEvent, DeviceDiagState, DeviceMonitorEvent

log = logging.getLogger("hub-bootstrap.device-diag")

UNEXPECTED_RESETS = frozenset({"PANIC", "TASK_WDT", "INT_WDT", "WDT", "BROWNOUT"})
COUNTER_FIELDS = (
    ("wifi_disconnects", "wifi_disconnect_burst"),
    ("mqtt_disconnects", "mqtt_disconnect_burst"),
    ("mqtt_reconnects", "mqtt_reconnect_burst"),
    ("mqtt_connect_attempts", "mqtt_connect_attempt_burst"),
    ("mqtt_connect_failures", "mqtt_connect_failure_burst"),
    ("desired_failures", "desired_failure_burst"),
)


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


def _parse_diag(value: Any) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    return value


def _counter(diag: dict[str, Any] | None, key: str) -> int:
    if not diag:
        return 0
    raw = diag.get(key)
    try:
        value = int(raw)
    except (TypeError, ValueError):
        return 0
    return max(0, value)


def _load_previous_diag(payload_json: str | None) -> dict[str, Any] | None:
    """Load diag from a full reported payload or a bare diag snapshot."""
    if not payload_json:
        return None
    try:
        payload = json.loads(payload_json)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
        return None
    if not isinstance(payload, dict):
        return None
    nested = _parse_diag(payload.get("diag"))
    if nested is not None:
        return nested
    # device_diag_state.last_diag_json stores the bare diag object.
    if any(
        key in payload
        for key in (
            "wifi_disconnects",
            "mqtt_disconnects",
            "mqtt_reconnects",
            "mqtt_connect_attempts",
            "mqtt_connect_failures",
            "desired_failures",
            "last_reset_reason",
        )
    ):
        return payload
    return None


def _get_or_create_state(db: Session, node_id: str) -> DeviceDiagState:
    state = db.get(DeviceDiagState, node_id)
    if state is None:
        state = DeviceDiagState(
            node_id=node_id,
            hub_id=settings.hub_id,
            last_diag_json="{}",
            pending_ota_json="{}",
            has_diag_evidence=0,
            last_health="unknown",
            last_health_reason="no device diagnostics observed yet",
        )
        db.add(state)
        db.flush()
    return state


def _load_pending_ota(state: DeviceDiagState) -> dict[str, Any] | None:
    raw = state.pending_ota_json or "{}"
    try:
        payload = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
        return None
    if not isinstance(payload, dict) or not payload:
        return None
    return payload


def _set_pending_ota(state: DeviceDiagState, pending: dict[str, Any] | None) -> None:
    if not pending:
        state.pending_ota_json = "{}"
        return
    state.pending_ota_json = json.dumps(pending, separators=(",", ":"), sort_keys=True)


def ingest_ota_report(
    db: Session,
    *,
    node_id: str,
    payload: dict[str, Any],
    previous_firmware_version: str | None,
) -> dict[str, Any] | None:
    """Correlate pre-reboot OTA status with post-reboot hello/boot confirmation.

    Distinguishes image install (installed_pending_reboot) from confirmed boot
    into the new image (boot_confirmed). Software reset reasons in diag remain
    separate; reboot_reason=ota is required to attribute the transition to OTA.
    """
    node_id = node_id.upper()
    now = _utcnow()
    state = _get_or_create_state(db, node_id)
    message_type = payload.get("message_type")
    event = str(payload.get("event") or "")
    ota_phase = str(payload.get("ota_phase") or "")
    reboot_reason = str(payload.get("reboot_reason") or "")
    previous = str(
        payload.get("previous_firmware_version") or previous_firmware_version or ""
    )
    target = str(payload.get("target_firmware_version") or "")
    running = str(payload.get("firmware_version") or "")
    firmware_id = str(payload.get("firmware_id") or "")

    if message_type == "ota_update" and event == "install_succeeded":
        if not previous or not target or previous == target:
            return {"handled": False, "reason": "invalid_install_report"}
        pending = {
            "previous_firmware_version": previous,
            "target_firmware_version": target,
            "firmware_id": firmware_id,
            "phase": "installed_pending_reboot",
            "reported_at": now.isoformat(),
        }
        _set_pending_ota(state, pending)
        _record_or_aggregate(
            db,
            node_id=node_id,
            event_type="ota_install_reported",
            count=1,
            reason=(f"OTA image installed pending reboot: {previous} -> {target}"),
            detail={
                "previous_firmware_version": previous,
                "target_firmware_version": target,
                "firmware_id": firmware_id,
                "ota_phase": ota_phase or "installed_pending_reboot",
                "reboot_reason": reboot_reason or "ota",
            },
            now=now,
        )
        return {
            "handled": True,
            "phase": "installed_pending_reboot",
            "pending": pending,
        }

    if message_type == "ota_update" and event == "boot_confirmed":
        if not target or running != target:
            return {"handled": False, "reason": "boot_not_on_target"}
        pending = _load_pending_ota(state) or {
            "previous_firmware_version": previous,
            "target_firmware_version": target,
            "firmware_id": firmware_id,
        }
        _record_or_aggregate(
            db,
            node_id=node_id,
            event_type="ota_boot_confirmed",
            count=1,
            reason=(
                f"OTA boot confirmed: "
                f"{pending.get('previous_firmware_version') or previous} -> {running}"
            ),
            detail={
                "previous_firmware_version": pending.get("previous_firmware_version")
                or previous,
                "target_firmware_version": target,
                "running_firmware_version": running,
                "firmware_id": firmware_id or pending.get("firmware_id") or "",
                "ota_phase": "boot_confirmed",
                "reboot_reason": reboot_reason or "ota",
            },
            now=now,
        )
        _set_pending_ota(state, None)
        return {"handled": True, "phase": "boot_confirmed"}

    if message_type == "ota_update" and event == "boot_mismatch":
        pending = _load_pending_ota(state)
        _record_or_aggregate(
            db,
            node_id=node_id,
            event_type="ota_boot_mismatch",
            count=1,
            reason=(
                f"OTA boot mismatch: expected {target or (pending or {}).get('target_firmware_version')}, "
                f"running {running}"
            ),
            detail={
                "previous_firmware_version": previous
                or (pending or {}).get("previous_firmware_version"),
                "target_firmware_version": target
                or (pending or {}).get("target_firmware_version"),
                "running_firmware_version": running,
                "ota_phase": "boot_mismatch",
            },
            now=now,
        )
        _set_pending_ota(state, None)
        return {"handled": True, "phase": "boot_mismatch"}

    # Correlate normal hello/health when a pending install awaits confirmation.
    pending = _load_pending_ota(state)
    if not pending:
        return None
    target = str(pending.get("target_firmware_version") or "")
    previous = str(pending.get("previous_firmware_version") or "")
    if not target or not running:
        return None
    if running != target:
        return {"handled": False, "phase": "pending", "waiting_for": target}
    if previous and running == previous:
        return {"handled": False, "phase": "pending", "waiting_for": target}

    _record_or_aggregate(
        db,
        node_id=node_id,
        event_type="ota_boot_confirmed",
        count=1,
        reason=f"OTA boot confirmed via hello/health: {previous} -> {running}",
        detail={
            "previous_firmware_version": previous,
            "target_firmware_version": target,
            "running_firmware_version": running,
            "firmware_id": pending.get("firmware_id") or "",
            "ota_phase": "boot_confirmed",
            "correlated_from": "hello_or_health",
        },
        now=now,
    )
    _set_pending_ota(state, None)
    return {
        "handled": True,
        "phase": "boot_confirmed",
        "correlated_from": "hello_or_health",
    }


def _prune_fifo(db: Session, node_id: str) -> None:
    max_events = settings.device_diag_max_events_per_device
    db.flush()
    overflow = db.scalars(
        select(DeviceDiagEvent.event_id)
        .where(
            DeviceDiagEvent.hub_id == settings.hub_id,
            DeviceDiagEvent.node_id == node_id,
        )
        .order_by(DeviceDiagEvent.occurred_at.desc())
        .offset(max_events)
    ).all()
    if overflow:
        db.execute(
            delete(DeviceDiagEvent).where(DeviceDiagEvent.event_id.in_(overflow)),
            execution_options={"synchronize_session": False},
        )


def _record_or_aggregate(
    db: Session,
    *,
    node_id: str,
    event_type: str,
    count: int,
    reason: str,
    detail: dict[str, Any] | None,
    now: datetime,
) -> None:
    if count <= 0:
        return
    window = timedelta(seconds=settings.device_diag_aggregate_window_s)
    latest = db.scalar(
        select(DeviceDiagEvent)
        .where(
            DeviceDiagEvent.hub_id == settings.hub_id,
            DeviceDiagEvent.node_id == node_id,
            DeviceDiagEvent.event_type == event_type,
        )
        .order_by(DeviceDiagEvent.occurred_at.desc())
        .limit(1)
    )
    if latest is not None and now - _ensure_aware(latest.last_at) <= window:
        latest.event_count = int(latest.event_count) + count
        latest.last_at = now
        latest.observed_at = now
        latest.reason = reason[:256]
        if detail:
            try:
                existing = json.loads(latest.detail_json or "{}")
            except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
                existing = {}
            if not isinstance(existing, dict):
                existing = {}
            existing.update(detail)
            latest.detail_json = json.dumps(
                existing, separators=(",", ":"), sort_keys=True
            )
        return

    # Make room before insert so the hard ceiling is never exceeded.
    current = db.scalar(
        select(func.count())
        .select_from(DeviceDiagEvent)
        .where(
            DeviceDiagEvent.hub_id == settings.hub_id,
            DeviceDiagEvent.node_id == node_id,
        )
    )
    while int(current or 0) >= settings.device_diag_max_events_per_device:
        oldest_id = db.scalar(
            select(DeviceDiagEvent.event_id)
            .where(
                DeviceDiagEvent.hub_id == settings.hub_id,
                DeviceDiagEvent.node_id == node_id,
            )
            .order_by(DeviceDiagEvent.occurred_at.asc())
            .limit(1)
        )
        if oldest_id is None:
            break
        db.execute(
            delete(DeviceDiagEvent).where(DeviceDiagEvent.event_id == oldest_id),
            execution_options={"synchronize_session": False},
        )
        current = int(current or 0) - 1

    db.add(
        DeviceDiagEvent(
            event_id=str(uuid4()),
            hub_id=settings.hub_id,
            node_id=node_id,
            occurred_at=now,
            observed_at=now,
            first_at=now,
            last_at=now,
            event_type=event_type,
            event_count=count,
            reason=reason[:256],
            detail_json=json.dumps(detail or {}, separators=(",", ":"), sort_keys=True),
            source="device_reported",
            certainty="confirmed",
        )
    )
    _prune_fifo(db, node_id)


def assess_health(
    *,
    online: bool,
    has_diag_evidence: bool,
    diag: dict[str, Any] | None,
    wifi_rssi_dbm: int | None,
) -> tuple[str, str]:
    """Return (state, reason) for instantaneous device health."""
    if not online:
        return "unknown", "no recent heartbeat within the online window"
    if not has_diag_evidence and not diag:
        return (
            "unknown",
            "no device diagnostics observed yet (older firmware or no diag)",
        )

    reset = ""
    if diag and isinstance(diag.get("last_reset_reason"), str):
        reset = diag["last_reset_reason"].strip().upper()
    if reset in UNEXPECTED_RESETS:
        return "unhealthy", f"unexpected reset reason {reset}"

    wifi_d = _counter(diag, "wifi_disconnects")
    mqtt_d = _counter(diag, "mqtt_disconnects")
    mqtt_r = _counter(diag, "mqtt_reconnects")
    mqtt_fail = _counter(diag, "mqtt_connect_failures")
    desired_f = _counter(diag, "desired_failures")
    # connect_attempts alone are not instability (includes the first boot connect).
    if wifi_d or mqtt_d or mqtt_r or mqtt_fail or desired_f:
        parts = []
        if wifi_d:
            parts.append(f"wifi_disconnects={wifi_d}")
        if mqtt_d:
            parts.append(f"mqtt_disconnects={mqtt_d}")
        if mqtt_r:
            parts.append(f"mqtt_reconnects={mqtt_r}")
        if mqtt_fail:
            parts.append(f"mqtt_connect_failures={mqtt_fail}")
        if desired_f:
            parts.append(f"desired_failures={desired_f}")
        return "degraded", "connectivity instability: " + ", ".join(parts)

    threshold = settings.device_diag_rssi_degraded_dbm
    if wifi_rssi_dbm is not None and wifi_rssi_dbm < threshold:
        return (
            "degraded",
            f"wifi_rssi_dbm {wifi_rssi_dbm} below threshold {threshold}",
        )

    if reset and reset not in UNEXPECTED_RESETS:
        return "healthy", f"online with reset {reset}; no elevated diag counters"
    return "healthy", "online with no elevated diag counters"


def ingest_reported_diag(
    db: Session,
    *,
    node_id: str,
    previous_payload_json: str | None,
    new_payload: dict[str, Any],
    online: bool = True,
) -> dict[str, Any]:
    """Parse diag deltas, retain events, update health. Safe on malformed input."""
    if not settings.device_diag_enabled:
        return {"enabled": False}

    node_id = node_id.upper()
    now = _utcnow()
    current = _parse_diag(new_payload.get("diag"))
    state = _get_or_create_state(db, node_id)
    # Baseline must be the last retained diag snapshot, not previous_payload_json.
    # Hello omits diag and would otherwise make every health look like a full
    # counter jump (inflating mqtt_reconnect_burst into thousands).
    baseline = _load_previous_diag(state.last_diag_json)
    if baseline is None:
        baseline = _load_previous_diag(previous_payload_json)

    try:
        rssi_raw = new_payload.get("wifi_rssi_dbm")
        wifi_rssi = int(rssi_raw) if rssi_raw is not None else None
    except (TypeError, ValueError):
        wifi_rssi = None

    if current is not None:
        state.has_diag_evidence = 1

        for field, event_type in COUNTER_FIELDS:
            delta = _counter(current, field) - _counter(baseline, field)
            # Counters are boot-lifetime on device; a drop means reboot (or saturate reset).
            if delta > 0:
                _record_or_aggregate(
                    db,
                    node_id=node_id,
                    event_type=event_type,
                    count=delta,
                    reason=f"{field} increased by {delta}",
                    detail={
                        "field": field,
                        "delta": delta,
                        "total": _counter(current, field),
                    },
                    now=now,
                )

        reset = current.get("last_reset_reason")
        previous_reset = baseline.get("last_reset_reason") if baseline else None
        if isinstance(reset, str) and reset.strip():
            reset_norm = reset.strip().upper()
            prev_norm = (
                previous_reset.strip().upper()
                if isinstance(previous_reset, str)
                else None
            )
            if reset_norm != prev_norm:
                event_type = (
                    "unexpected_reset"
                    if reset_norm in UNEXPECTED_RESETS
                    else "reset_reported"
                )
                _record_or_aggregate(
                    db,
                    node_id=node_id,
                    event_type=event_type,
                    count=1,
                    reason=f"device reported reset reason {reset_norm}",
                    detail={"last_reset_reason": reset_norm},
                    now=now,
                )

        state.last_diag_json = json.dumps(
            current, separators=(",", ":"), sort_keys=True
        )

    health_state, health_reason = assess_health(
        online=online,
        has_diag_evidence=bool(state.has_diag_evidence),
        diag=current
        if current is not None
        else (
            json.loads(state.last_diag_json)
            if state.last_diag_json and state.last_diag_json != "{}"
            else None
        ),
        wifi_rssi_dbm=wifi_rssi,
    )
    # When offline, prefer unknown even if last diag looked healthy.
    if not online:
        health_state, health_reason = assess_health(
            online=False,
            has_diag_evidence=bool(state.has_diag_evidence),
            diag=current,
            wifi_rssi_dbm=wifi_rssi,
        )

    previous_health = state.last_health
    if previous_health != health_state:
        db.add(
            DeviceMonitorEvent(
                event_id=str(uuid4()),
                hub_id=settings.hub_id,
                node_id=node_id,
                occurred_at=now,
                observed_at=now,
                event_type="health_changed",
                previous_state=previous_health,
                new_state=health_state,
                reason=health_reason[:256],
                source="hub_assessed",
                certainty="confirmed",
            )
        )
    state.last_health = health_state
    state.last_health_reason = health_reason[:256]
    state.hub_id = settings.hub_id
    state.updated_at = now

    return {
        "enabled": True,
        "health": health_state,
        "reason": health_reason,
        "has_diag_evidence": bool(state.has_diag_evidence),
    }


def cleanup_history(db: Session, now: datetime | None = None) -> dict[str, int]:
    now = _ensure_aware(now or _utcnow())
    days = settings.device_monitor_retention_days
    cutoff = now - timedelta(days=days)
    result = db.execute(
        delete(DeviceDiagEvent).where(
            DeviceDiagEvent.hub_id == settings.hub_id,
            DeviceDiagEvent.occurred_at < cutoff,
        ),
        execution_options={"synchronize_session": False},
    )
    return {
        "diag_events_deleted": result.rowcount or 0,
        "cutoff": cutoff.isoformat(),
        "retention_days": days,
    }


def _diag_event_view(event: DeviceDiagEvent) -> dict[str, Any]:
    try:
        detail = json.loads(event.detail_json or "{}")
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
        detail = {}
    return {
        "event_id": event.event_id,
        "node_id": event.node_id,
        "occurred_at": _ensure_aware(event.occurred_at).isoformat(),
        "observed_at": _ensure_aware(event.observed_at).isoformat(),
        "first_at": _ensure_aware(event.first_at).isoformat(),
        "last_at": _ensure_aware(event.last_at).isoformat(),
        "event_type": event.event_type,
        "count": int(event.event_count),
        "reason": event.reason,
        "detail": detail if isinstance(detail, dict) else {},
        "source": event.source,
        "certainty": event.certainty,
        "kind": "diag",
    }


def list_diag_events(
    db: Session, node_id: str, *, limit: int = 15
) -> list[dict[str, Any]]:
    node_id = node_id.upper()
    limit = max(1, min(int(limit), 50))
    events = list(
        db.scalars(
            select(DeviceDiagEvent)
            .where(
                DeviceDiagEvent.hub_id == settings.hub_id,
                DeviceDiagEvent.node_id == node_id,
            )
            .order_by(DeviceDiagEvent.occurred_at.desc())
            .limit(limit)
        )
    )
    return [_diag_event_view(event) for event in events]


def health_summary_for_node(
    db: Session,
    node_id: str,
    *,
    online: bool,
    reported_payload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    now = _utcnow()
    state = db.get(DeviceDiagState, node_id.upper())
    diag = None
    if reported_payload is not None:
        diag = _parse_diag(reported_payload.get("diag"))
    elif state and state.last_diag_json:
        try:
            parsed = json.loads(state.last_diag_json)
            diag = parsed if isinstance(parsed, dict) else None
        except (UnicodeDecodeError, json.JSONDecodeError, TypeError):
            diag = None

    wifi_rssi = None
    if reported_payload is not None:
        try:
            if reported_payload.get("wifi_rssi_dbm") is not None:
                wifi_rssi = int(reported_payload["wifi_rssi_dbm"])
        except (TypeError, ValueError):
            wifi_rssi = None

    has_evidence = bool(state and state.has_diag_evidence) or diag is not None
    health_state, reason = assess_health(
        online=online,
        has_diag_evidence=has_evidence,
        diag=diag,
        wifi_rssi_dbm=wifi_rssi,
    )
    # Prefer stored reason when state matches (includes last ingest nuance).
    if state and state.last_health == health_state and state.last_health_reason:
        reason = state.last_health_reason

    diag_count = db.scalar(
        select(func.count())
        .select_from(DeviceDiagEvent)
        .where(
            DeviceDiagEvent.hub_id == settings.hub_id,
            DeviceDiagEvent.node_id == node_id.upper(),
        )
    )
    return {
        "state": health_state,
        "score": health_score(health_state),
        "score_definition": {
            "healthy": 100,
            "degraded": 70,
            "unhealthy": 0,
            "unknown": None,
        },
        "reason": reason,
        "has_diag_evidence": has_evidence,
        "last_diag": diag,
        "diag_event_count": int(diag_count or 0),
        "diag_event_cap": settings.device_diag_max_events_per_device,
        "rssi_degraded_dbm": settings.device_diag_rssi_degraded_dbm,
        "calculated_at": now.isoformat(),
    }
