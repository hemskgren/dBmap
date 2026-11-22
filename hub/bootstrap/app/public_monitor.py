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
from sqlalchemy.orm import Session

from app import monitoring_availability, monitoring_cycle
from app.config import settings
from app.db import SessionLocal
from app.models import PublicMonitorCheckpoint, PublicMonitorEvent, PublicMonitorSample

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


def _store(component: str) -> monitoring_cycle.MonitorStore:
    return monitoring_cycle.MonitorStore(
        PublicMonitorSample, PublicMonitorEvent, PublicMonitorCheckpoint,
        hub_id=settings.hub_id, component=component, source="public_monitor",
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
    availability = monitoring_availability.component_state(state)
    health = {
        "available": "healthy", "unavailable": "unhealthy", "unobserved": "unknown",
    }[availability]
    store = _store(component)
    store.record_check(
        db, observed_at, states={component: state}, reasons={component: reason},
        health=health, availability=availability, previous_check_at=previous_check_at,
        threshold=STALE_AFTER,
    )
    if prune:
        try:
            store.prune(db, observed_at, settings.hub_monitor_retention_days)
            db.commit()
        except Exception:
            db.rollback()
            log.warning("Could not prune the %s history", component)


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


def summary(db: Session, now: datetime) -> dict:
    result = {}
    for component, name in COMPONENTS.items():
        if not enabled(component):
            continue
        with _current_lock:
            checked_at, (state, reason), last_success = _current.get(
                component, (None, ("unknown", "Waiting for first check"), None)
            )
        store = _store(component)
        rows = store.history(db, now, settings.hub_monitor_retention_days)
        payloads = {row.sample_id: monitoring_cycle.sample_payload(row) for row in rows}
        history = monitoring_cycle.observations(rows, payloads, component_id=component)
        stale_boundary = store.stale_boundary(
            db, history, now, previous_check_at=checked_at, threshold=STALE_AFTER,
        )
        if stale_boundary is not None:
            history.append(stale_boundary)
        stale = checked_at is None or now - checked_at > STALE_AFTER
        if stale and checked_at is not None:
            state, reason = "unknown", "No recent completed check"
        result[component] = {
            "label": name,
            "state": state,
            "reason": reason,
            "checked_at": checked_at.isoformat() if checked_at else None,
            "last_success_at": last_success.isoformat() if last_success else None,
            "interval_seconds": INTERVAL_S,
            "windows": monitoring_availability.component_windows(
                history, component_id=component, now=now, interval_s=INTERVAL_S,
            ),
            "recent_events": [
                monitoring_cycle.event_view(event)
                for event in store.recent_events(
                    db, 5, since=now - timedelta(days=settings.hub_monitor_retention_days)
                )
            ],
        }
    return result
