import json
import logging
import ssl
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from sqlalchemy.orm import Session

from app.config import settings
from app.db import SessionLocal
from app.schemas import ObservationBody
from app.service import apply_reported, ingest_observation
from app.topics import parse_node_id_from_topic, parse_observation_topic

log = logging.getLogger("hub-bootstrap.mqtt")
_connection_lock = threading.Lock()
_connected = False
_broker_metric_topics = {
    "$SYS/broker/clients/connected": "connected_clients",
    "$SYS/broker/clients/total": "total_client_sessions",
    "$SYS/broker/bytes/received": "bytes_received",
    "$SYS/broker/bytes/sent": "bytes_sent",
    "$SYS/broker/uptime": "uptime",
    "$SYS/broker/version": "version",
}
_numeric_broker_metrics = {
    "connected_clients",
    "total_client_sessions",
    "bytes_received",
    "bytes_sent",
}
_broker_metrics: dict[str, str | int] = {}
_broker_metrics_updated_at: str | None = None


def is_connected() -> bool:
    with _connection_lock:
        return _connected


def get_broker_metrics() -> dict[str, str | int | None]:
    with _connection_lock:
        return {
            **_broker_metrics,
            "updated_at": _broker_metrics_updated_at,
        }


def _set_connected(connected: bool) -> None:
    global _connected
    with _connection_lock:
        _connected = connected


def _on_connect(client: mqtt.Client, userdata, flags, reason_code, properties=None) -> None:
    global _broker_metrics_updated_at
    log.info("mqtt connected rc=%s", reason_code)
    _set_connected(not reason_code.is_failure)
    if reason_code.is_failure:
        return
    with _connection_lock:
        _broker_metrics.clear()
        _broker_metrics_updated_at = None
    client.subscribe("esp-output/local/+/hello", qos=1)
    client.subscribe("esp-output/local/+/keepalive", qos=0)
    client.subscribe("esp-output/local/+/health", qos=0)
    client.subscribe("esp-output/local/+/status", qos=0)
    client.subscribe("esp-ear/local/+/hello", qos=1)
    client.subscribe("esp-ear/local/+/keepalive", qos=0)
    client.subscribe("esp-ear/local/+/health", qos=0)
    client.subscribe("esp-ear/local/+/observation", qos=1)
    for topic in _broker_metric_topics:
        client.subscribe(topic, qos=0)


def _on_disconnect(
    client: mqtt.Client,
    userdata,
    disconnect_flags,
    reason_code,
    properties=None,
) -> None:
    global _broker_metrics_updated_at
    _set_connected(False)
    with _connection_lock:
        _broker_metrics.clear()
        _broker_metrics_updated_at = None
    log.warning("mqtt disconnected rc=%s", reason_code)


def _on_message(client: mqtt.Client, userdata, msg: mqtt.MQTTMessage) -> None:
    global _broker_metrics_updated_at
    metric_name = _broker_metric_topics.get(msg.topic)
    if metric_name is not None:
        try:
            value = msg.payload.decode().strip()
            metric: str | int = int(value) if metric_name in _numeric_broker_metrics else value
        except (UnicodeDecodeError, ValueError):
            log.warning("invalid Mosquitto metric on %s", msg.topic)
            return
        with _connection_lock:
            _broker_metrics[metric_name] = metric
            _broker_metrics_updated_at = datetime.now(timezone.utc).isoformat()
        return

    if msg.topic.endswith("/observation"):
        observation_node_id = parse_observation_topic(msg.topic)
        if observation_node_id is None:
            log.warning("rejected observation on invalid topic %s", msg.topic)
        elif msg.retain:
            log.warning("rejected retained observation from %s", observation_node_id)
        else:
            _ingest_observation(observation_node_id, msg.payload)
        return

    node_id = parse_node_id_from_topic(msg.topic)
    if node_id is None:
        return
    try:
        payload = json.loads(msg.payload.decode())
    except (UnicodeDecodeError, json.JSONDecodeError):
        log.warning("invalid json on %s", msg.topic)
        return
    if not isinstance(payload, dict):
        return
    payload.setdefault("node_id", node_id)
    db: Session = SessionLocal()
    try:
        apply_reported(db, node_id, payload)
    except Exception:
        log.exception("failed to apply reported state for %s", node_id)
    finally:
        db.close()


def _ingest_observation(node_id: str, message: bytes) -> None:
    try:
        body = ObservationBody.model_validate_json(message)
    except (UnicodeDecodeError, ValueError) as exc:
        log.warning("invalid observation from %s: %s", node_id, exc)
        return

    db: Session = SessionLocal()
    try:
        inserted = ingest_observation(db, node_id, body)
        if not inserted:
            log.info("duplicate observation %s from %s ignored", body.observation_id, node_id)
    except ValueError as exc:
        log.warning("rejected observation from %s: %s", node_id, exc)
    except Exception:
        log.exception("failed to persist observation from %s", node_id)
    finally:
        db.close()


def start_mqtt_thread() -> threading.Thread:
    thread = threading.Thread(target=_run, name="mqtt-listener", daemon=True)
    thread.start()
    return thread


def _run() -> None:
    backoff = 1
    while True:
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="hub-bootstrap",
            protocol=mqtt.MQTTv311,
        )
        client.username_pw_set(settings.mqtt_username, settings.mqtt_password)
        client.tls_set(ca_certs=settings.mqtt_ca_file, cert_reqs=ssl.CERT_REQUIRED)
        client.on_connect = _on_connect
        client.on_disconnect = _on_disconnect
        client.on_message = _on_message
        try:
            client.connect(settings.mqtt_host, settings.mqtt_port, keepalive=30)
            backoff = 1
            client.loop_forever()
        except Exception:
            log.exception("mqtt loop ended")
        time.sleep(backoff)
        backoff = min(backoff * 2, 120)
