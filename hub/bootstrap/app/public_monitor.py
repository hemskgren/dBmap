"""Optional public-path checks with timelines separate from Core Hub monitoring."""

import http.client
import json
import logging
import socket
import ssl
import threading
from collections.abc import Callable
from datetime import UTC, datetime, timedelta
from time import monotonic
from urllib.parse import urlsplit
from uuid import uuid4

import paho.mqtt.client as mqtt
from sqlalchemy import delete, select
from sqlalchemy.orm import Session

from app import monitoring_availability, monitoring_cycle
from app.config import settings
from app.db import SessionLocal
from app.models import PublicMonitorTransition
from app.monitoring_math import ensure_aware

log = logging.getLogger("hub-bootstrap.public-monitor")

INTERVAL_S = 60
STALE_AFTER = timedelta(seconds=2 * INTERVAL_S)
PRUNE_INTERVAL_S = 10 * INTERVAL_S
COMPONENTS = {"public_api": "Public API", "public_mqtt": "Public MQTT"}
PUBLIC_IDS = tuple(COMPONENTS)
Result = tuple[str, str | None]
_current: dict[str, tuple[datetime, Result, datetime | None]] = {}
_current_lock = threading.Lock()


def _utcnow() -> datetime:
    return datetime.now(UTC)


def enabled(component: str) -> bool:
    return settings.hub_monitor_enabled and getattr(
        settings, f"{component}_monitor_enabled"
    )


def _transitions(component: str):
    return select(PublicMonitorTransition).where(
        PublicMonitorTransition.hub_id == settings.hub_id,
        PublicMonitorTransition.component == component,
    )


def prune_history(db: Session, component: str, now: datetime) -> None:
    """Remove aged transitions, preserving the state at the retention boundary."""
    cutoff = monitoring_cycle.retention_cutoff(now, settings.hub_monitor_retention_days)
    aged = _transitions(component).where(PublicMonitorTransition.observed_at < cutoff)
    anchor = db.scalar(
        aged.order_by(PublicMonitorTransition.observed_at.desc()).limit(1)
    )
    if anchor is None:
        return
    expired = db.scalars(
        aged.with_only_columns(PublicMonitorTransition.transition_id)
        .where(PublicMonitorTransition.transition_id != anchor.transition_id)
        .limit(500)
    ).all()
    if expired:
        db.execute(
            delete(PublicMonitorTransition).where(
                PublicMonitorTransition.transition_id.in_(expired)
            )
        )


def _network_failure(exc: Exception) -> Result:
    if isinstance(exc, socket.gaierror):
        return "offline", "DNS lookup failed"
    if isinstance(exc, ssl.SSLCertVerificationError):
        return "offline", "TLS certificate verification failed"
    if isinstance(exc, ssl.SSLError):
        return "offline", "TLS handshake failed"
    if isinstance(exc, TimeoutError):
        return "offline", "Probe timed out"
    return "offline", "Connection failed"


def probe_api() -> Result:
    url = urlsplit(settings.public_hub_url)
    if (
        url.scheme != "https"
        or not url.hostname
        or url.username is not None
        or url.password is not None
    ):
        return "unknown", "Configure a public Hub HTTPS URL without user information"
    try:
        port = url.port or 443
        context = ssl.create_default_context(cafile=settings.opa_ca_file)
    except (OSError, ValueError):
        return "unknown", "Public Hub URL or local CA trust is invalid"
    connection = http.client.HTTPSConnection(
        url.hostname, port, timeout=settings.edge_probe_timeout_s, context=context
    )
    try:
        connection.request("GET", url.path.rstrip("/") + "/health")
        response = connection.getresponse()
        if response.status != 200:
            return "offline", f"Hub health endpoint returned HTTP {response.status}"
        payload = json.loads(response.read(1024))
        if (
            isinstance(payload, dict)
            and payload.get("status") == "ok"
            and payload.get("service") == "hub-bootstrap"
        ):
            return "ok", None
        return "offline", "Endpoint did not return Hub health JSON"
    except (ValueError, UnicodeError, http.client.HTTPException):
        return "offline", "Endpoint did not return Hub health JSON"
    except OSError as exc:
        return _network_failure(exc)
    finally:
        connection.close()


def probe_mqtt() -> Result:
    host = settings.public_mqtt_host
    if (
        not host
        or host.lower().rstrip(".").endswith(".local")
        or not 1 <= settings.public_mqtt_port <= 65535
        or not settings.public_mqtt_use_tls
    ):
        return "unknown", "Configure a public MQTT endpoint with TLS"
    if not settings.mqtt_username or not settings.mqtt_password:
        return "unknown", "The existing Hub MQTT login is not configured"
    try:
        context = ssl.create_default_context(cafile=settings.mqtt_ca_file)
    except (OSError, ValueError):
        return "unknown", "Local MQTT CA trust is invalid"
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"dbmap-probe-{uuid4().hex[:11]}",
        clean_session=True,
        protocol=mqtt.MQTTv311,
        reconnect_on_failure=False,
    )
    client.username_pw_set(settings.mqtt_username, settings.mqtt_password)
    client.tls_set_context(context)
    client.connect_timeout = settings.edge_probe_timeout_s
    result: Result | None = None

    def on_connect(_client, _userdata, _flags, reason_code, _properties):
        nonlocal result
        if not reason_code.is_failure:
            result = "ok", None
        elif reason_code.value in (0x85, 0x86, 0x87):
            result = (
                "offline",
                "MQTT CONNECT rejected (check credentials and DynSec client-ID policy)",
            )
        else:
            result = "offline", f"MQTT CONNECT rejected: {reason_code.getName()}"

    client.on_connect = on_connect
    try:
        if (
            client.connect(host, settings.public_mqtt_port, keepalive=30)
            != mqtt.MQTT_ERR_SUCCESS
        ):
            return "offline", "MQTT CONNECT could not be sent"
        deadline = monotonic() + settings.edge_probe_timeout_s
        while result is None:
            remaining = deadline - monotonic()
            if remaining <= 0:
                return "offline", "MQTT CONNACK timed out"
            if (
                client.loop(timeout=remaining) != mqtt.MQTT_ERR_SUCCESS
                and result is None
            ):
                return "offline", "Connection closed before successful MQTT CONNACK"
        return result
    except OSError as exc:
        return _network_failure(exc)
    finally:
        sock = client.socket()
        try:
            client.disconnect()
        finally:
            if sock is not None:
                sock.close()


def record_check(
    db: Session,
    component: str,
    observed_at: datetime,
    result: Result,
    previous_check_at: datetime | None,
    *,
    prune: bool = False,
) -> None:
    state, reason = result
    previous = db.scalar(
        _transitions(component)
        .order_by(PublicMonitorTransition.observed_at.desc())
        .limit(1)
    )
    gap = monitoring_cycle.plan_gap(
        observed_at,
        anchor_at=previous.observed_at if previous is not None else None,
        previous_check_at=previous_check_at,
        persisted_checkpoint=previous.last_check_at if previous is not None else None,
        threshold=STALE_AFTER,
    )

    def record_history() -> PublicMonitorTransition:
        latest = previous
        if gap is not None and previous is not None and previous.state != "unknown":
            marker_at = gap.marker_at(previous.observed_at)
            if marker_at > ensure_aware(previous.observed_at):
                latest = PublicMonitorTransition(
                    hub_id=settings.hub_id,
                    component=component,
                    observed_at=marker_at,
                    state="unknown",
                    previous_state=previous.state,
                    is_gap=True,
                )
                db.add(latest)
        prior_state = latest.state if latest is not None else None
        if monitoring_cycle.needs_sample(prior_state, state):
            latest = PublicMonitorTransition(
                hub_id=settings.hub_id,
                component=component,
                observed_at=observed_at,
                state=state,
                previous_state=prior_state,
                reason=reason,
            )
            db.add(latest)
        if prune:
            prune_history(db, component, observed_at)
        return latest

    def save_checkpoint(latest: PublicMonitorTransition, value: str) -> None:
        # The latest boundary carries metadata only; its observed_at is immutable.
        latest.last_check_at = value

    monitoring_cycle.commit_cycle(
        db,
        observed_at,
        record_history=record_history,
        save_checkpoint=save_checkpoint,
    )


def _loop(component: str, probe: Callable[[], Result], stop: threading.Event) -> None:
    previous_check_at = last_success = None
    next_prune = monotonic() + PRUNE_INTERVAL_S
    while not stop.is_set():
        start = monotonic()
        try:
            result = probe()
        except Exception:  # noqa: BLE001 - isolate optional work from the monitor
            # Do not put endpoint details or exception data into logs or events.
            result = "unknown", "Probe could not be completed"
        if stop.is_set():
            break
        now = _utcnow()
        try:
            with SessionLocal() as db:
                record_check(
                    db,
                    component,
                    now,
                    result,
                    previous_check_at,
                    prune=start >= next_prune,
                )
        except Exception:  # noqa: BLE001 - retry on the next scheduled cycle
            log.warning("Could not persist the %s check", component)
        else:
            if start >= next_prune:
                next_prune = start + PRUNE_INTERVAL_S
            previous_check_at = now
            if result[0] == "ok":
                last_success = now
            with _current_lock:
                _current[component] = now, result, last_success
        stop.wait(max(0.0, INTERVAL_S - (monotonic() - start)))


def start(stop: threading.Event) -> None:
    for component, probe in (("public_api", probe_api), ("public_mqtt", probe_mqtt)):
        if enabled(component):
            with _current_lock:
                _current.pop(component, None)
            try:
                threading.Thread(
                    target=_loop,
                    args=(component, probe, stop),
                    name=component,
                    daemon=True,
                ).start()
            except RuntimeError:
                log.warning("Could not start the %s worker", component)


def _recent_events(
    rows: list[PublicMonitorTransition], anchor: PublicMonitorTransition | None
) -> list[dict]:
    events = []
    for index in range(len(rows) - 1, -1, -1):
        row = rows[index]
        if row is anchor or row.is_gap:
            continue
        previous = rows[index - 1] if index else None
        resumed = previous is not None and previous.is_gap
        if resumed and previous.previous_state == row.state:
            # A short restart still has two history boundaries, just as Core
            # does, but an unchanged result doesn't need a notification.
            elapsed = ensure_aware(row.observed_at) - ensure_aware(previous.observed_at)
            if elapsed <= STALE_AFTER:
                continue
        events.append(
            monitoring_cycle.event_view(
                event_id=row.transition_id,
                occurred_at=row.observed_at,
                observed_at=row.observed_at,
                component=row.component,
                event_type="monitoring_resumed" if resumed else "state_changed",
                previous_state=row.previous_state,
                new_state=row.state,
                reason=row.reason,
                source="public_monitor",
            )
        )
        if len(events) == 5:
            break
    return events


def summary(db: Session, now: datetime) -> dict:
    result = {}
    for component, name in COMPONENTS.items():
        if not enabled(component):
            continue
        with _current_lock:
            checked_at, (state, reason), last_success = _current.get(
                component, (None, ("unknown", "Waiting for first check"), None)
            )
        cutoff = monitoring_cycle.retention_cutoff(
            now, settings.hub_monitor_retention_days
        )
        anchor = db.scalar(
            _transitions(component)
            .where(PublicMonitorTransition.observed_at < cutoff)
            .order_by(PublicMonitorTransition.observed_at.desc())
            .limit(1)
        )
        timeline = list(
            db.scalars(
                _transitions(component)
                .where(PublicMonitorTransition.observed_at >= cutoff)
                .order_by(PublicMonitorTransition.observed_at)
            )
        )
        rows = ([anchor] if anchor is not None else []) + timeline
        history = [
            monitoring_availability.Observation(
                row.observed_at, monitoring_availability.component_state(row.state)
            )
            for row in rows
        ]
        stale = checked_at is None or now - checked_at > STALE_AFTER
        if stale and rows:
            latest = rows[-1]
            gap_at = monitoring_cycle.coverage_end_if_stale(
                now,
                anchor_at=latest.observed_at,
                previous_check_at=checked_at,
                persisted_checkpoint=latest.last_check_at,
                threshold=STALE_AFTER,
            )
            if gap_at is not None:
                history.append(
                    monitoring_availability.Observation(gap_at, "unobserved")
                )
        if stale and checked_at is not None:
            state, reason = "unknown", "No recent completed check"
        result[component] = {
            "label": name,
            "state": state,
            "reason": reason,
            "checked_at": checked_at.isoformat() if checked_at else None,
            "last_success_at": last_success.isoformat() if last_success else None,
            "interval_seconds": INTERVAL_S,
            "windows": {
                key: monitoring_availability.component_window(
                    history,
                    component_id=component,
                    window_start=now - delta,
                    window_end=now,
                    interval_s=INTERVAL_S,
                )
                for key, delta in monitoring_availability.WINDOW_SPECS
            },
            "recent_events": _recent_events(rows, anchor),
        }
    return result
