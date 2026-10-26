from app.config import settings
from app.db import SessionLocal
from app.ids import token_hash
from app.main import create_app
from app.models import DesiredState, NodeCredential
from fastapi.testclient import TestClient

settings.database_url = "sqlite:////tmp/dbmap-bootstrap-test.db"
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False


def _admin() -> dict[str, str]:
    return {"Authorization": f"Bearer {settings.admin_token}"}


def _provision(client: TestClient, firmware_id: str, firmware_version: str) -> tuple[str, str, str]:
    created = client.post(
        "/api/v1/nodes",
        headers=_admin(),
        json={"node_type": "output", "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]
    with SessionLocal() as db:
        desired = db.get(DesiredState, node_id)
        assert desired is not None
        desired.firmware_id = firmware_id
        desired.firmware_version = firmware_version
        db.commit()
    provisioned = client.post(
        "/api/v1/provision",
        json={
            "bootstrap_token": created.json()["bootstrap_token"],
            "hardware_revision": "OUTPUT-DEV-V1",
            "firmware_version": "0.0.1",
        },
    )
    assert provisioned.status_code == 200, provisioned.text
    body = provisioned.json()
    assert body["device_api"]["token"]
    assert provisioned.headers["cache-control"] == "no-store"
    with SessionLocal() as db:
        cred = db.get(NodeCredential, node_id)
        assert cred is not None
        assert cred.device_api_token_hash == token_hash(body["device_api"]["token"])
        assert body["device_api"]["token"] not in (cred.device_api_token_hash or "")
        assert cred.mqtt_username == node_id.lower()
    return node_id, created.json()["bootstrap_token"], body["device_api"]["token"]


def test_device_reads_only_its_desired_firmware_and_repeat_provision_is_rejected() -> None:
    client = TestClient(create_app())
    node_a, bootstrap_a, token_a = _provision(client, "fw-a", "1.0.0")
    node_b, _bootstrap_b, token_b = _provision(client, "fw-b", "2.0.0")

    desired_a = client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {token_a}"})
    assert desired_a.status_code == 200, desired_a.text
    assert desired_a.json() == {"node_id": node_a, "firmware_id": "fw-a", "firmware_version": "1.0.0"}

    desired_b = client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {token_b}"})
    assert desired_b.status_code == 200, desired_b.text
    assert desired_b.json()["node_id"] == node_b
    assert desired_b.json()["firmware_id"] == "fw-b"
    assert desired_b.json()["firmware_version"] == "2.0.0"

    assert client.get("/api/v1/device/desired-firmware").status_code == 401
    assert client.get(
        "/api/v1/device/desired-firmware",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    ).status_code == 401
    assert client.get(
        "/api/v1/device/desired-firmware",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    ).status_code == 401

    repeat = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": bootstrap_a, "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert repeat.status_code == 400
    assert client.post(
        f"/api/v1/nodes/{node_a}/bootstrap-token",
        headers=_admin(),
    ).status_code == 409


def test_rotate_revoke_and_recovery_replace_device_credentials() -> None:
    client = TestClient(create_app())
    node_id, _bootstrap, token = _provision(client, "fw-a", "1.0.0")

    assert client.post(f"/api/v1/nodes/{node_id}/device-api-token", headers={"Authorization": f"Bearer {settings.viewer_token}"}).status_code == 403
    rotated = client.post(f"/api/v1/nodes/{node_id}/device-api-token", headers=_admin())
    assert rotated.status_code == 200, rotated.text
    assert rotated.headers["cache-control"] == "no-store"
    new_token = rotated.json()["device_api_token"]
    assert new_token != token
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {token}"}).status_code == 401
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {new_token}"}).status_code == 200

    assert client.post(f"/api/v1/nodes/{node_id}/device-api-token/revoke").status_code == 401
    revoked = client.post(f"/api/v1/nodes/{node_id}/device-api-token/revoke", headers=_admin())
    assert revoked.status_code == 204, revoked.text
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {new_token}"}).status_code == 401
    assert client.post(f"/api/v1/nodes/{node_id}/device-api-token/revoke", headers=_admin()).status_code == 409

    restored = client.post(f"/api/v1/nodes/{node_id}/device-api-token", headers=_admin())
    assert restored.status_code == 200, restored.text
    restored_token = restored.json()["device_api_token"]
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {restored_token}"}).status_code == 200

    ordinary = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": "not-a-recovery-token", "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert ordinary.status_code == 400

    recovery = client.post(f"/api/v1/nodes/{node_id}/recovery-provision", headers=_admin())
    assert recovery.status_code == 200, recovery.text
    assert recovery.json()["provisioning_state"] == "provisioned"
    recovery_token = recovery.json()["bootstrap_token"]
    reprovisioned = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": recovery_token, "hardware_revision": "OUTPUT-DEV-V1", "firmware_version": "1.0.0"},
    )
    assert reprovisioned.status_code == 200, reprovisioned.text
    recovered_api = reprovisioned.json()["device_api"]["token"]
    assert recovered_api != restored_token
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {restored_token}"}).status_code == 401
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {recovered_api}"}).status_code == 200
    again = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": recovery_token, "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert again.status_code == 400
    assert client.post(f"/api/v1/nodes/{node_id}/recovery-provision", headers={"Authorization": f"Bearer {settings.viewer_token}"}).status_code == 403


def test_deactivated_device_cannot_read_desired_firmware() -> None:
    client = TestClient(create_app())
    node_id, _bootstrap, token = _provision(client, "fw-a", "1.0.0")
    deactivated = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        headers=_admin(),
        json={"lifecycle_state": "deactivated"},
    )
    assert deactivated.status_code == 200, deactivated.text
    assert client.get("/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {token}"}).status_code == 403
    assert client.post(f"/api/v1/nodes/{node_id}/recovery-provision", headers=_admin()).status_code == 409
