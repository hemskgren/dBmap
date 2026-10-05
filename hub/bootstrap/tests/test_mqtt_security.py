from app import mqtt_security


def test_node_role_is_scoped_to_its_topics(monkeypatch) -> None:
    commands = []

    def command(body):
        commands.append(body)
        if body["command"] == "getRole":
            return {"command": "getRole", "error": "Role not found"}
        if body["command"] == "getClient":
            return {"command": "getClient", "error": "Client not found"}
        return {"command": body["command"]}

    monkeypatch.setattr(mqtt_security, "_command", command)
    mqtt_security.ensure_node_security("output", "OUT-001", "node-password")

    role_command = next(body for body in commands if body["command"] == "createRole")
    assert role_command["rolename"] == "node-out-001"
    acls = role_command["acls"]
    assert {acl["topic"] for acl in acls} == {
        "esp-output/local/out-001/hello",
        "esp-output/local/out-001/keepalive",
        "esp-output/local/out-001/health",
        "esp-output/local/out-001/status",
        "esp-output/local/out-001/ack",
        "esp-output/local/out-001/command",
        "esp-output/local/out-001/config",
    }
    assert all("#" not in acl["topic"] and "+" not in acl["topic"] for acl in acls)

    client_command = next(body for body in commands if body["command"] == "createClient")
    assert client_command["username"] == "out-001"
    assert client_command["password"] == "node-password"
    assert client_command["roles"] == [{"rolename": "node-out-001", "priority": 1}]


def test_ear_role_can_publish_observations_on_its_own_topic(monkeypatch) -> None:
    commands = []

    def command(body):
        commands.append(body)
        if body["command"] == "getRole":
            return {"command": "getRole", "error": "Role not found"}
        if body["command"] == "getClient":
            return {"command": "getClient", "error": "Client not found"}
        return {"command": body["command"]}

    monkeypatch.setattr(mqtt_security, "_command", command)
    mqtt_security.ensure_node_security("ear", "EAR-007", "ear-password")

    role_command = next(body for body in commands if body["command"] == "createRole")
    assert {
        acl["topic"] for acl in role_command["acls"] if acl["acltype"] == "publishClientSend"
    } == {
        "esp-ear/local/ear-007/hello",
        "esp-ear/local/ear-007/keepalive",
        "esp-ear/local/ear-007/health",
        "esp-ear/local/ear-007/status",
        "esp-ear/local/ear-007/ack",
        "esp-ear/local/ear-007/observation",
    }
    assert all(
        acl["topic"] != "esp-ear/local/+/observation"
        for acl in role_command["acls"]
    )


def test_hub_defaults_deny_and_scope_access_to_dbmap_topics(monkeypatch) -> None:
    commands = []

    def command(body):
        commands.append(body)
        if body["command"] == "getRole":
            return {"command": body["command"], "error": "Role not found"}
        if body["command"] == "getClient":
            return {"command": body["command"], "error": "Client not found"}
        if body["command"] == "deleteClient":
            return {"command": body["command"], "error": "Client not found"}
        return {"command": body["command"]}

    monkeypatch.setattr(mqtt_security, "_command", command)
    mqtt_security.ensure_broker_security()

    defaults = next(body for body in commands if body["command"] == "setDefaultACLAccess")
    assert all(acl["allow"] is False for acl in defaults["acls"])

    hub_role = next(body for body in commands if body["command"] == "createRole")
    assert hub_role["rolename"] == "dbmap-hub"
    assert {acl["topic"] for acl in hub_role["acls"]} == {
        "esp-output/local/#",
        "esp-output/local/+/command",
        "esp-ear/local/#",
        "esp-ear/local/+/command",
    }
