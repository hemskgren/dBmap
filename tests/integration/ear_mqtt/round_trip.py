"""Run only inside the disposable integration stack created by run.sh."""

import json
import os
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
from queue import Queue
from threading import Barrier, Event, Thread
from uuid import uuid4

import paho.mqtt.client as mqtt
from app import mqtt_listener, regional_environment
from app.config import settings
from app.db import SessionLocal, init_db
from app.models import Node, Observation, ReportedState
from app.topics import parse_node_id_from_topic, topics_for
from lease_state import TEST_NODE, LeaseBusy, TestLease
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


def node_status(node_id):
    with SessionLocal() as db:
        node = db.get(Node, node_id)
        state = db.get(ReportedState, node_id)
        reports = db.scalars(select(Observation.sequence_number).where(
            Observation.node_id == node_id
        )).all()
        return node.lifecycle_state, None if state is None else state.uptime_s, sorted(reports)


def credentials(node_id, password):
    return {
        "node_id": node_id,
        "mqtt": {
            "host": BROKER_HOST, "port": BROKER_PORT, "use_tls": True,
            "username": node_id.lower(), "password": password,
        },
        "topics": topics_for("ear", node_id),
    }


def publish_pair(node_id, password, seq):
    record = credentials(node_id, password)
    options = {"live": True, "test_context": node_id == TEST_NODE}
    publish_keepalive(record, CA, make_keepalive(record, seq), **options)
    payload = make_observation(node_id, sequence_number=seq)
    obs_id = publish_payload(record, payload, CA, **options)

    def ingested():
        with SessionLocal() as db:
            return db.scalar(select(Observation).where(
                Observation.node_id == node_id,
                Observation.sequence_number == seq,
                Observation.observation_id == obs_id,
            )) is not None

    wait_for(ingested)
    wait_for(lambda: node_status(node_id)[1] == seq)


@contextmanager
def probe(password, *, accepted):
    """Use MQTT 5 reason codes to verify the broker's own admission decisions."""
    status = Queue()
    acks = Queue()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"tst-ear-001-probe-{uuid4().hex[:12]}",
                         protocol=mqtt.MQTTv5, reconnect_on_failure=False)
    client.username_pw_set(TEST_NODE.lower(), password)
    client.tls_set(ca_certs=CA)
    client.on_connect = lambda _c, _d, _f, reason, _p: status.put(not reason.is_failure)
    client.on_publish = lambda _c, _d, mid, reason, _p: acks.put((mid, reason.is_failure))
    client.connect(BROKER_HOST, BROKER_PORT)
    client.loop_start()
    try:
        assert status.get(timeout=5) is accepted, "unexpected broker authentication result"
        yield client, acks
    finally:
        client.disconnect()
        client.loop_stop()


def denied_on_open_connection(connection, seq):
    client, acks = connection
    assert client.is_connected(), "the test requires an already authenticated connection"
    topics = topics_for("ear", TEST_NODE)
    for kind, body in (
        ("observation", make_observation(TEST_NODE, sequence_number=seq)),
        ("keepalive", {"node_id": TEST_NODE, "uptime_s": seq}),
    ):
        info = client.publish(topics[kind], json.dumps(body), qos=1)
        assert acks.get(timeout=5) == (info.mid, True), (
            f"broker did not reject {kind} with a failure PUBACK"
        )


def denied_at_hub(seq, *, lifecycle_state="deactivated"):
    # Bypass MQTT transport locally so this also fails if only the broker enforces.
    before = node_status(TEST_NODE)
    assert before[0] == lifecycle_state
    for kind, body in (
        ("observation", make_observation(TEST_NODE, sequence_number=seq)),
        ("keepalive", {"node_id": TEST_NODE, "uptime_s": seq}),
    ):
        message = mqtt.MQTTMessage(topic=topics_for("ear", TEST_NODE)[kind].encode())
        message.payload = json.dumps(body).encode()
        mqtt_listener._on_message(None, None, message)
    assert node_status(TEST_NODE) == before


def exercise_lease(lease, reaping):
    denied_at_hub(100)
    with probe(uuid4().hex, accepted=False):
        pass
    print("TST-EAR-001 initially disabled: broker and Hub deny access", flush=True)

    barrier = Barrier(2)

    def compete():
        barrier.wait(timeout=3)
        try:
            return lease.acquire(10)
        except LeaseBusy:
            return None

    with ThreadPoolExecutor(max_workers=2) as pool:
        attempts = list(pool.map(lambda _: compete(), range(2)))
    sessions = [session for session in attempts if session is not None]
    try:
        assert len(sessions) == 1, "concurrent activation was not exclusive"
        print("Concurrent TST-EAR-001 activation rejected", flush=True)
        _, first = sessions[0]
        assert node_status(TEST_NODE)[0] == "active"
        publish_pair(TEST_NODE, first, 1)
        with probe(first, accepted=True) as connected:
            lease.release(first)
            lease.release(first)
            denied_on_open_connection(connected, 101)
            denied_at_hub(101)
            print("TST-EAR-001 enabled → disabled: cleanup revokes both message types", flush=True)
            with probe(first, accepted=False):
                pass

            reaping.clear()
            deadline, second = lease.acquire(6)
            try:
                # An old connection and credential stay revoked under a new lease.
                assert first != second
                denied_on_open_connection(connected, 102)
                with probe(first, accepted=False):
                    pass
                publish_pair(TEST_NODE, second, 2)
                with probe(second, accepted=True) as expiring:
                    wait_for(lambda: time.monotonic_ns() >= deadline)
                    # Simulate an abandoned controller. The stale lease still exists
                    # and the Hub row still says active while both boundaries deny.
                    assert lease.state.is_file()
                    denied_on_open_connection(expiring, 103)
                    with probe(second, accepted=False):
                        pass
                    denied_at_hub(103, lifecycle_state="active")
                    reaping.set()
                    wait_for(lambda: node_status(TEST_NODE)[0] == "deactivated")
                    assert not lease.state.exists()
                    denied_at_hub(103)
                print("TST-EAR-001 automatically disabled at TTL; expired credentials rejected",
                      flush=True)
            finally:
                lease.release(second)
    finally:
        for _, token in sessions:
            lease.release(token)
    assert node_status(TEST_NODE) == ("deactivated", 2, [1, 2])


def main() -> None:
    canary = Path(os.environ["CHECKOUT_CANARY"])
    assert canary.name == str(canary)
    for forbidden in (canary, Path(".env"), Path("shared"), Path("docs"), Path(".git")):
        assert not forbidden.exists(), "private checkout file is visible in the container"
    assert "tst-ear-001" not in json.loads(Path("/test-credentials.json").read_text())
    print("Checkout secret canary and private directories are inaccessible", flush=True)
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
                lifecycle_state="deactivated" if node_id == TEST_NODE else "active",
                provisioning_state="provisioned",
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

    lease = TestLease(Path("/lease"))
    on_message = mqtt_listener._on_message

    def enforce_test_lifecycle(client, userdata, message):
        if parse_node_id_from_topic(message.topic) == TEST_NODE:
            with lease.admit() as active:
                if active:
                    on_message(client, userdata, message)
            return
        on_message(client, userdata, message)

    stop = Event()
    reaping = Event()
    reaping.set()

    def expire_test_node():
        while not stop.wait(0.05):
            if reaping.is_set():
                lease.reap_expired()

    watchdog = Thread(target=expire_test_node, daemon=True)
    mqtt_listener._on_connect = connect_and_wait_for_subscriptions
    mqtt_listener._on_message = enforce_test_lifecycle
    watchdog.start()
    try:
        mqtt_listener.start_mqtt_thread()
        wait_for(subscribed.is_set)
        exercise_lease(lease, reaping)
        publish_pair("SIM-EAR-001", passwords["sim-ear-001"], 1)
        print("SIM-EAR-001: TLS MQTT publish and Hub ingestion passed", flush=True)
    finally:
        stop.set()
        watchdog.join()


if __name__ == "__main__":
    main()
