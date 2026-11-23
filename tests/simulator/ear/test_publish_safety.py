import json
import socket
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

sys.path.insert(0, str(Path(__file__).parents[3] / "simulator" / "ear"))
import simulate_keepalive
import simulate_observation
from publish_safety import BROKER_HOST, HUB_URL, NoRedirect

PUBLISHERS = {
    "observation": simulate_observation.publish_payload,
    "keepalive": simulate_keepalive.publish_keepalive,
}


def credentials(node_id):
    slug = node_id.lower()
    return {
        "state": "provisioned",
        "node_id": node_id,
        "mqtt": {
            "host": BROKER_HOST, "port": 8883, "use_tls": True,
            "username": slug, "password": "fake-test-password", "client_id": slug,
        },
        "topics": {
            kind: f"esp-ear/local/{slug}/{kind}" for kind in PUBLISHERS
        },
        "observation": simulate_observation.make_observation(node_id),
    }


@pytest.fixture
def transport(monkeypatch):
    client = Mock()
    client.return_value.is_connected.return_value = True
    client.return_value.publish.return_value.is_published.return_value = True
    http = Mock(side_effect=AssertionError("unexpected HTTP transport"))
    network = Mock(side_effect=AssertionError("unexpected network socket"))
    monkeypatch.setattr(socket, "socket", network)
    monkeypatch.setattr("paho.mqtt.client.Client", client)
    monkeypatch.setattr("urllib.request.build_opener", http)
    yield client, http
    network.assert_not_called()


def publish(kind, record, **options):
    if kind == "observation":
        return PUBLISHERS[kind](record, record["observation"], "test-ca.crt", **options)
    payload = {"node_id": options.pop("payload_id", record["node_id"])}
    return PUBLISHERS[kind](record, "test-ca.crt", payload, **options)


@pytest.mark.parametrize("kind", PUBLISHERS)
@pytest.mark.parametrize("identity,test_context,live,accepted", [
    ("EAR-001", False, True, False),
    ("ear-001", False, True, False),
    ("Ear-001", True, True, False),
    ("SIM-EAR-001", False, True, True),
    ("SIM-EAR-001", True, True, False),
    ("sim-ear-001", False, True, False),
    ("TST-EAR-001", True, True, True),
    ("TST-EAR-001", False, True, False),
    ("Tst-Ear-001", True, True, False),
    ("SIM-EAR-001", False, False, False),
    ("TST-EAR-001", True, False, False),
])
def test_publish_identity(kind, identity, test_context, live, accepted, transport):
    client, http = transport
    record = credentials(identity)
    if accepted:
        publish(kind, record, test_context=test_context, live=live)
        assert client.call_args.kwargs["client_id"].rsplit("-", 1)[0] == identity.lower()
        client.return_value.connect.assert_called_once()
    else:
        with pytest.raises(ValueError):
            publish(kind, record, test_context=test_context, live=live)
        client.assert_not_called()
    http.assert_not_called()


@pytest.mark.parametrize("kind", PUBLISHERS)
@pytest.mark.parametrize("field,value", [
    ("host", "hub.example"),
    ("host", "MQTT.EAR-TEST.INVALID"),
    ("host", "mqtt.ear-test.invalid."),
    ("port", 1883),
    ("use_tls", False),
    ("client_id", "ear-001"),
    ("username", "SIM-EAR-001"),
    ("username", "sim-ear-002"),
    ("topic", "esp-ear/local/ear-001/report"),
    ("payload", "sim-ear-001"),
    ("payload", "SIM-EAR-002"),
])
def test_publish_mismatches(kind, field, value, transport):
    record = credentials("SIM-EAR-001")
    args = {"live": True}
    if field == "topic":
        record["topics"][kind] = value
    elif field == "payload":
        if kind == "observation":
            record["observation"]["identity"]["node_id"] = value
        else:
            args["payload_id"] = value
    else:
        record["mqtt"][field] = value
    with pytest.raises(ValueError):
        publish(kind, record, **args)
    for call in transport:
        call.assert_not_called()


@pytest.mark.parametrize("base,live", [
    ("https://prod.example:8443", True),
    ("https://HUB.EAR-TEST.INVALID:8443", True),
    (HUB_URL + "/redirect", True),
    (HUB_URL, False),
])
def test_provisioning_destination(base, live, transport):
    with pytest.raises(ValueError):
        simulate_observation.api_request(
            base, "test-ca.crt", "POST", "/api/v1/nodes", {}, "fake-token", live=live
        )
    for call in transport:
        call.assert_not_called()


def test_http_redirect_is_rejected():
    with pytest.raises(simulate_observation.urllib.error.HTTPError):
        NoRedirect().redirect_request(
            SimpleNamespace(full_url=HUB_URL), None, 307, "", {}, "https://prod.example"
        )


@pytest.mark.parametrize("identity,state", [
    ("EAR-001", "pending"), ("EAR-001", "provisioned"), ("ear-001", "provisioned"),
    ("TST-EAR-001", "provisioned"), ("SIM-EAR-001", "provisioned"),
])
def test_reused_credentials_fail_before_transport(identity, state, tmp_path, transport):
    record = credentials(identity)
    record.update(state=state, bootstrap_token="fake-test-token")
    if identity == "SIM-EAR-001":
        record["pending_observation"] = simulate_observation.make_observation("EAR-001")
    path = tmp_path / "creds.json"
    path.write_text(json.dumps(record))
    path.chmod(0o600)
    args = SimpleNamespace(reuse_credentials=True, credentials_file=path, live_mqtt=True)
    with pytest.raises((ValueError, simulate_observation.UnsupportedSimulatorIdentity)):
        simulate_observation.load_or_provision(args, "fake-token")
    for call in transport:
        call.assert_not_called()


@pytest.mark.parametrize("path,body,identity", [
    ("/api/v1/nodes", {"node_type": "ear", "simulated": False}, None),
    ("/api/v1/provision", {"bootstrap_token": "fake-token"}, "EAR-001"),
    ("/api/v1/provision", {"bootstrap_token": "fake-token"}, "sim-ear-001"),
])
def test_http_identity_rejected_before_transport(path, body, identity, transport):
    with pytest.raises(ValueError):
        simulate_observation.api_request(
            HUB_URL, "test-ca.crt", "POST", path, body, "fake-token",
            live=True, node_id=identity,
        )
    for call in transport:
        call.assert_not_called()


@pytest.mark.parametrize("module,command", [
    (simulate_observation, "add"),
    (simulate_observation, "publish"),
    (simulate_keepalive, "once"),
    (simulate_keepalive, "watch"),
])
def test_cli_without_opt_in_stops_before_credentials(module, command, monkeypatch, transport):
    monkeypatch.setattr(sys, "argv", [module.__file__, command])
    with pytest.raises(SystemExit, match="2"):
        module.main()
    for call in transport:
        call.assert_not_called()
