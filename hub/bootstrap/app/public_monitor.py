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

from app.config import settings
from app.db import SessionLocal
from app.models import HubMonitorSample, PublicMonitorTransition
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
    cutoff = now - timedelta(days=settings.hub_monitor_retention_days)
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


def _sample(component: str, observed_at: datetime, state: str) -> HubMonitorSample:
    return HubMonitorSample(
        sample_id=str(uuid4()),
        hub_id=settings.hub_id,
        observed_at=observed_at,
        overall_availability={"ok": "available", "offline": "unavailable"}.get(
            state, "unobserved"
        ),
        overall_health="unknown",
        source=component,
        components_json=json.dumps(
            {"states": {component: state}, "sample_mode": "sparse"}
        ),
    )


def record_check(
    db: Session,
    component: str,
    observed_at: datetime,
    result: Result,
    previous_check_at: datetime | None,
) -> None:
    state, reason = result
    previous = db.scalar(
        _transitions(component)
        .order_by(PublicMonitorTransition.observed_at.desc())
        .limit(1)
    )
    previous_state = None
    if previous is not None:
        previous_state = previous.state
        if previous_state != "unknown" and (
            previous_check_at is None or observed_at - previous_check_at > STALE_AFTER
        ):
            start = max(
                previous_check_at or ensure_aware(previous.observed_at),
                ensure_aware(previous.observed_at) + timedelta(microseconds=1),
            )
            if start < observed_at:
                db.add(
                    PublicMonitorTransition(
                        hub_id=settings.hub_id,
                        component=component,
                        observed_at=start,
                        state="unknown",
                        previous_state=previous_state,
                        is_gap=True,
                    )
                )
                previous_state = "unknown"
    if state == previous_state:
        # A gap marker can be the only change when the resumed probe is unknown.
        return
    db.add(
        PublicMonitorTransition(
            hub_id=settings.hub_id,
            component=component,
            observed_at=observed_at,
            state=state,
            previous_state=previous_state,
            reason=reason,
        )
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
                record_check(db, component, now, result, previous_check_at)
                if start >= next_prune:
                    prune_history(db, component, now)
                db.commit()
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


def summary(db: Session, now: datetime) -> dict:
    from app.hub_monitor import (
        WINDOW_SPECS,
        compute_component_availability_window,
    )

    result = {}
    for component, name in COMPONENTS.items():
        if not enabled(component):
            continue
        with _current_lock:
            checked_at, (state, reason), last_success = _current.get(
                component, (None, ("unknown", "Waiting for first check"), None)
            )
        cutoff = now - timedelta(days=settings.hub_monitor_retention_days)
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
        history = [
            _sample(component, row.observed_at, row.state)
            for row in ([anchor] if anchor is not None else []) + timeline
        ]
        stale = checked_at is None or now - checked_at > STALE_AFTER
        if stale and history:
            # The persisted timeline is extended only as far as current observation.
            last_at = ensure_aware(history[-1].observed_at)
            gap_at = max(checked_at or last_at, last_at + timedelta(microseconds=1))
            if gap_at < now:
                history.append(_sample(component, gap_at, "unknown"))
        if stale and checked_at is not None:
            state, reason = "unknown", "No recent completed check"
        recent = [row for row in reversed(timeline) if not row.is_gap][:5]
        result[component] = {
            "label": name,
            "state": state,
            "reason": reason,
            "checked_at": checked_at.isoformat() if checked_at else None,
            "last_success_at": last_success.isoformat() if last_success else None,
            "interval_seconds": INTERVAL_S,
            "windows": {
                key: compute_component_availability_window(
                    history,
                    component_id=component,
                    window_start=now - delta,
                    window_end=now,
                    interval_s=INTERVAL_S,
                )
                for key, delta in WINDOW_SPECS
            },
            "recent_events": [
                {
                    "event_id": row.transition_id,
                    "occurred_at": ensure_aware(row.observed_at).isoformat(),
                    "observed_at": ensure_aware(row.observed_at).isoformat(),
                    "component": component,
                    "event_type": "state_changed",
                    "previous_state": row.previous_state,
                    "new_state": row.state,
                    "reason": row.reason,
                    "source": "public_monitor",
                    "certainty": "confirmed",
                }
                for row in recent
            ],
        }
    return result
