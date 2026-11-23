"""Run only inside the disposable integration stack created by run.sh."""

import json
import time
from pathlib import Path
from threading import Event

from app import mqtt_listener, regional_environment
from app.config import settings
from app.db import SessionLocal, init_db
from app.models import Node, Observation
from app.topics import topics_for
from publish_safety import BROKER_HOST, BROKER_PORT
from simulate_keepalive import make_keepalive, publish_keepalive
from simulate_observation import make_observation, publish_payload
from sqlalchemy import select

CA = "/test-ca.crt"


def wait_for(check):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for the isolated MQTT round trip")


def main() -> None:
    if settings.database_url != "sqlite:////data/bootstrap.db":
        raise RuntimeError("integration test must run with its disposable database")
    passwords = json.loads(Path("/test-credentials.json").read_text())
    settings.mqtt_host = BROKER_HOST
    settings.mqtt_port = BROKER_PORT
    settings.mqtt_ca_file = CA
    settings.mqtt_username = "test-hub"
    settings.mqtt_password = passwords["test-hub"]
    # The test network has no external weather service.
    regional_environment.schedule_weather_ensure = lambda *_a, **_kw: None
    init_db()
    identities = ("TST-EAR-001", "SIM-EAR-001")
    with SessionLocal() as db:
        for node_id in identities:
            db.add(Node(
                node_id=node_id, node_type="ear", hardware_revision="ear-test-fixture",
                lifecycle_state="active", provisioning_state="provisioned",
            ))
        db.commit()

    subscribed = Event()
    on_connect = mqtt_listener._on_connect

    def connect_and_wait_for_subscriptions(client, userdata, flags, reason_code, properties):
        on_connect(client, userdata, flags, reason_code, properties)
        if reason_code.is_failure:
            return
        _, marker_id = client.subscribe("esp-ear/local/tst-ear-001/observation", qos=1)

        def on_subscribe(client, userdata, message_id, reason_codes, properties):
            if message_id == marker_id and not any(code.is_failure for code in reason_codes):
                subscribed.set()

        client.on_subscribe = on_subscribe

    mqtt_listener._on_connect = connect_and_wait_for_subscriptions
    mqtt_listener.start_mqtt_thread()
    wait_for(subscribed.is_set)
    for node_id in identities:
        test_context = node_id.startswith("TST-")
        username = node_id.lower()
        credentials = {
            "node_id": node_id,
            "mqtt": {
                "host": BROKER_HOST, "port": BROKER_PORT, "use_tls": True,
                "username": username, "password": passwords[username],
            },
            "topics": topics_for("ear", node_id),
        }
        publish_keepalive(
            credentials, CA, make_keepalive(credentials, 1),
            live=True, test_context=test_context,
        )
        payload = make_observation(node_id)
        obs_id = publish_payload(credentials, payload, CA, live=True, test_context=test_context)

        def ingested(node_id=node_id, obs_id=obs_id):
            with SessionLocal() as db:
                return db.scalar(select(Observation).where(
                    Observation.node_id == node_id,
                    Observation.sequence_number == 1,
                    Observation.observation_id == obs_id,
                )) is not None

        wait_for(ingested)
        print(f"{node_id}: TLS MQTT publish and Hub ingestion passed", flush=True)


if __name__ == "__main__":
    main()
