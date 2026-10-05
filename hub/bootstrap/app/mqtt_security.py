import json
import ssl
import threading
import uuid
from typing import Any

import paho.mqtt.client as mqtt

from app.config import settings
from app.topics import topic_root

CONTROL_TOPIC = "$CONTROL/dynamic-security/v1"
RESPONSE_TOPIC = "$CONTROL/dynamic-security/v1/response"


class MqttSecurityError(RuntimeError):
    pass


def _command(command: dict[str, Any]) -> dict[str, Any]:
    connected = threading.Event()
    subscribed = threading.Event()
    response_ready = threading.Event()
    state: dict[str, Any] = {"response": None, "correlation": uuid.uuid4().hex}

    def on_connect(client, userdata, flags, reason_code, properties) -> None:
        if reason_code.is_failure:
            state["error"] = f"broker connect rejected: {reason_code}"
            connected.set()
            return
        connected.set()

    def on_subscribe(client, userdata, mid, reason_code_list, properties) -> None:
        if any(code.is_failure for code in reason_code_list):
            state["error"] = f"broker denied dynamic-security response subscription: {reason_code_list}"
        subscribed.set()

    def on_message(client, userdata, message) -> None:
        try:
            payload = json.loads(message.payload)
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        for result in payload.get("responses", []):
            if result.get("correlationData") == state["correlation"]:
                state["response"] = result
                response_ready.set()
                return

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"dbmap-control-{uuid.uuid4().hex[:12]}",
        protocol=mqtt.MQTTv5,
    )
    client.username_pw_set(settings.mqtt_admin_username, settings.mqtt_admin_password)
    client.tls_set(ca_certs=settings.mqtt_ca_file, cert_reqs=ssl.CERT_REQUIRED)
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message

    try:
        client.connect(settings.mqtt_host, settings.mqtt_port, keepalive=15)
        client.loop_start()
        if not connected.wait(8):
            raise MqttSecurityError("timed out connecting to the MQTT security control API")
        if "error" in state:
            raise MqttSecurityError(state["error"])
        result, _ = client.subscribe(RESPONSE_TOPIC, qos=1)
        if result != mqtt.MQTT_ERR_SUCCESS or not subscribed.wait(8):
            raise MqttSecurityError("failed to subscribe to the MQTT security control response")
        if "error" in state:
            raise MqttSecurityError(state["error"])

        command["correlationData"] = state["correlation"]
        info = client.publish(CONTROL_TOPIC, json.dumps({"commands": [command]}), qos=1)
        info.wait_for_publish(timeout=8)
        if not info.is_published():
            raise MqttSecurityError("timed out publishing an MQTT security control request")
        if not response_ready.wait(8):
            raise MqttSecurityError(f"timed out waiting for MQTT security command {command['command']}")
        response = state["response"]
        if response.get("error"):
            return response
        return response
    except MqttSecurityError:
        raise
    except (OSError, ValueError, mqtt.MQTTException) as exc:
        raise MqttSecurityError("MQTT security control request failed") from exc
    finally:
        if client.is_connected():
            client.disconnect()
        client.loop_stop()


def _ensure_role(rolename: str, acls: list[dict[str, Any]]) -> None:
    result = _command({"command": "getRole", "rolename": rolename})
    if result.get("error") == "Role not found":
        _command({"command": "createRole", "rolename": rolename, "acls": acls})
    elif result.get("error"):
        raise MqttSecurityError(f"could not inspect broker role {rolename}")
    else:
        result = _command({"command": "modifyRole", "rolename": rolename, "acls": acls})
        if result.get("error"):
            raise MqttSecurityError(f"could not update broker role {rolename}")


def _ensure_client(username: str, password: str, rolename: str) -> None:
    result = _command({"command": "getClient", "username": username})
    if result.get("error") == "Client not found":
        result = _command({
            "command": "createClient",
            "username": username,
            "password": password,
            "roles": [{"rolename": rolename, "priority": 1}],
        })
    elif result.get("error"):
        raise MqttSecurityError(f"could not inspect broker client {username}")
    else:
        result = _command({
            "command": "modifyClient",
            "username": username,
            "password": password,
            "roles": [{"rolename": rolename, "priority": 1}],
        })
    if result.get("error"):
        raise MqttSecurityError(f"could not configure broker client {username}")


def ensure_broker_security() -> None:
    _command({
        "command": "setDefaultACLAccess",
        "acls": [
            {"acltype": "publishClientSend", "allow": False},
            {"acltype": "publishClientReceive", "allow": False},
            {"acltype": "subscribe", "allow": False},
            {"acltype": "unsubscribe", "allow": False},
        ],
    })

    hub_acls: list[dict[str, Any]] = []
    for kind in ("esp-output", "esp-ear"):
        root = f"{kind}/local/#"
        hub_acls.extend([
            {"acltype": "subscribePattern", "topic": root, "allow": True},
            {"acltype": "publishClientReceive", "topic": root, "allow": True},
            {"acltype": "publishClientSend", "topic": f"{kind}/local/+/command", "allow": True},
        ])
    _ensure_role("dbmap-hub", hub_acls)
    _ensure_client(settings.mqtt_username, settings.mqtt_password, "dbmap-hub")

    democlient = _command({"command": "deleteClient", "username": "democlient"})
    if democlient.get("error") not in (None, "Client not found"):
        raise MqttSecurityError("could not remove the default Mosquitto demo client")
    default_user = _command({"command": "deleteClient", "username": "user"})
    if default_user.get("error") not in (None, "Client not found"):
        raise MqttSecurityError("could not remove the default Mosquitto user")


def ensure_node_security(node_type: str, node_id: str, password: str) -> None:
    root = topic_root(node_type, node_id)
    role = f"node-{node_id.lower()}"
    acls = [
        {"acltype": "publishClientSend", "topic": f"{root}/{name}", "allow": True}
        for name in ("hello", "keepalive", "health", "status", "ack")
    ]
    acls.extend([
        {"acltype": "publishClientReceive", "topic": f"{root}/{name}", "allow": True}
        for name in ("command", "config")
    ])
    acls.extend([
        {"acltype": acltype, "topic": f"{root}/{name}", "allow": True}
        for name in ("command", "config")
        for acltype in ("subscribeLiteral", "unsubscribeLiteral")
    ])
    if node_type == "ear":
        acls.append({
            "acltype": "publishClientSend",
            "topic": f"{root}/observation",
            "allow": True,
        })
    _ensure_role(role, acls)
    _ensure_client(node_id.lower(), password, role)
