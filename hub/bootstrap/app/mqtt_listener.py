import json
import logging
import ssl
import threading
import time

import paho.mqtt.client as mqtt
from sqlalchemy.orm import Session

from app.config import settings
from app.db import SessionLocal
from app.schemas import ObservationBody
from app.service import apply_reported, ingest_observation
from app.topics import parse_node_id_from_topic, parse_observation_topic

log = logging.getLogger("hub-bootstrap.mqtt")


def _on_connect(client: mqtt.Client, userdata, flags, reason_code, properties=None) -> None:
    log.info("mqtt connected rc=%s", reason_code)
    client.subscribe("esp-output/local/+/hello", qos=1)
    client.subscribe("esp-output/local/+/keepalive", qos=0)
    client.subscribe("esp-output/local/+/health", qos=0)
    client.subscribe("esp-output/local/+/status", qos=0)
    client.subscribe("esp-ear/local/+/hello", qos=1)
    client.subscribe("esp-ear/local/+/keepalive", qos=0)
    client.subscribe("esp-ear/local/+/health", qos=0)
    client.subscribe("esp-ear/local/+/observation", qos=1)


def _on_message(client: mqtt.Client, userdata, msg: mqtt.MQTTMessage) -> None:
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
        client.on_message = _on_message
        try:
            client.connect(settings.mqtt_host, settings.mqtt_port, keepalive=30)
            backoff = 1
            client.loop_forever()
        except Exception:
            log.exception("mqtt loop ended")
        time.sleep(backoff)
        backoff = min(backoff * 2, 120)
