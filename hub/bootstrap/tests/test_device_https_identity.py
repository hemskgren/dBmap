import hashlib

import pytest
from app.config import settings

settings.database_url = "sqlite:////tmp/dbmap-bootstrap-test.db"
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app import api, firmware_catalog
from app.db import SessionLocal
from app.ids import token_hash
from app.main import create_app
from app.models import DesiredState, NodeCredential
from fastapi.testclient import TestClient
from test_firmware_catalog import make_firmware_pair, upload_pair


def _admin() -> dict[str, str]:
    return {"Authorization": f"Bearer {settings.admin_token}"}


def _provision(
    client: TestClient, firmware_id: str, firmware_version: str
) -> tuple[str, str, str]:
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


def test_device_reads_only_its_desired_firmware_and_repeat_provision_is_rejected() -> (
    None
):
    client = TestClient(create_app())
    node_a, bootstrap_a, token_a = _provision(client, "fw-a", "1.0.0")
    node_b, _bootstrap_b, token_b = _provision(client, "fw-b", "2.0.0")

    desired_a = client.get(
        "/api/v1/device/desired-firmware",
        headers={"Authorization": f"Bearer {token_a}"},
    )
    assert desired_a.status_code == 200, desired_a.text
    assert desired_a.json() == {
        "node_id": node_a,
        "firmware_id": "fw-a",
        "firmware_version": "1.0.0",
        "app_sha256": "",
        "app_size_bytes": 0,
    }

    desired_b = client.get(
        "/api/v1/device/desired-firmware",
        headers={"Authorization": f"Bearer {token_b}"},
    )
    assert desired_b.status_code == 200, desired_b.text
    assert desired_b.json()["node_id"] == node_b
    assert desired_b.json()["firmware_id"] == "fw-b"
    assert desired_b.json()["firmware_version"] == "2.0.0"
    assert desired_b.json()["app_sha256"] == ""
    assert desired_b.json()["app_size_bytes"] == 0

    assert client.get("/api/v1/device/desired-firmware").status_code == 401
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {settings.admin_token}"},
        ).status_code
        == 401
    )
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {settings.viewer_token}"},
        ).status_code
        == 401
    )

    repeat = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": bootstrap_a, "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert repeat.status_code == 400
    assert (
        client.post(
            f"/api/v1/nodes/{node_a}/bootstrap-token",
            headers=_admin(),
        ).status_code
        == 409
    )


def test_rotate_revoke_and_recovery_replace_device_credentials() -> None:
    client = TestClient(create_app())
    node_id, _bootstrap, token = _provision(client, "fw-a", "1.0.0")

    assert (
        client.post(
            f"/api/v1/nodes/{node_id}/device-api-token",
            headers={"Authorization": f"Bearer {settings.viewer_token}"},
        ).status_code
        == 403
    )
    rotated = client.post(f"/api/v1/nodes/{node_id}/device-api-token", headers=_admin())
    assert rotated.status_code == 200, rotated.text
    assert rotated.headers["cache-control"] == "no-store"
    new_token = rotated.json()["device_api_token"]
    assert new_token != token
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {token}"},
        ).status_code
        == 401
    )
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {new_token}"},
        ).status_code
        == 200
    )

    assert (
        client.post(f"/api/v1/nodes/{node_id}/device-api-token/revoke").status_code
        == 401
    )
    revoked = client.post(
        f"/api/v1/nodes/{node_id}/device-api-token/revoke", headers=_admin()
    )
    assert revoked.status_code == 204, revoked.text
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {new_token}"},
        ).status_code
        == 401
    )
    assert (
        client.post(
            f"/api/v1/nodes/{node_id}/device-api-token/revoke", headers=_admin()
        ).status_code
        == 409
    )

    restored = client.post(
        f"/api/v1/nodes/{node_id}/device-api-token", headers=_admin()
    )
    assert restored.status_code == 200, restored.text
    restored_token = restored.json()["device_api_token"]
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {restored_token}"},
        ).status_code
        == 200
    )

    ordinary = client.post(
        "/api/v1/provision",
        json={
            "bootstrap_token": "not-a-recovery-token",
            "hardware_revision": "OUTPUT-DEV-V1",
        },
    )
    assert ordinary.status_code == 400

    recovery = client.post(
        f"/api/v1/nodes/{node_id}/recovery-provision", headers=_admin()
    )
    assert recovery.status_code == 200, recovery.text
    assert recovery.json()["provisioning_state"] == "provisioned"
    recovery_token = recovery.json()["bootstrap_token"]
    reprovisioned = client.post(
        "/api/v1/provision",
        json={
            "bootstrap_token": recovery_token,
            "hardware_revision": "OUTPUT-DEV-V1",
            "firmware_version": "1.0.0",
        },
    )
    assert reprovisioned.status_code == 200, reprovisioned.text
    recovered_api = reprovisioned.json()["device_api"]["token"]
    assert recovered_api != restored_token
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {restored_token}"},
        ).status_code
        == 401
    )
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {recovered_api}"},
        ).status_code
        == 200
    )
    again = client.post(
        "/api/v1/provision",
        json={"bootstrap_token": recovery_token, "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert again.status_code == 400
    assert (
        client.post(
            f"/api/v1/nodes/{node_id}/recovery-provision",
            headers={"Authorization": f"Bearer {settings.viewer_token}"},
        ).status_code
        == 403
    )


def test_deactivated_device_cannot_read_desired_firmware() -> None:
    client = TestClient(create_app())
    node_id, _bootstrap, token = _provision(client, "fw-a", "1.0.0")
    deactivated = client.put(
        f"/api/v1/nodes/{node_id}/lifecycle",
        headers=_admin(),
        json={"lifecycle_state": "deactivated"},
    )
    assert deactivated.status_code == 200, deactivated.text
    assert (
        client.get(
            "/api/v1/device/desired-firmware",
            headers={"Authorization": f"Bearer {token}"},
        ).status_code
        == 403
    )
    assert (
        client.get(
            "/api/v1/device/ota-image", headers={"Authorization": f"Bearer {token}"}
        ).status_code
        == 403
    )
    assert (
        client.post(
            f"/api/v1/nodes/{node_id}/recovery-provision", headers=_admin()
        ).status_code
        == 409
    )


def test_device_ota_image_is_desired_scoped_with_sha_and_size_guard(
    tmp_path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    catalog_dir = tmp_path / "firmware-catalog"
    monkeypatch.setattr(api, "FIRMWARE_CATALOG_DIR", catalog_dir)
    monkeypatch.setattr(settings, "firmware_catalog_dir", str(catalog_dir))
    client = TestClient(create_app())

    firmware_id, bundle, metadata = make_firmware_pair()
    uploaded = upload_pair(client, firmware_id, bundle, metadata)
    assert uploaded.status_code == 201, uploaded.text
    app_digest = hashlib.sha256(b"application").hexdigest()

    node_a, _bootstrap_a, token_a = _provision(client, firmware_id, "1.2.3")
    _node_b, _bootstrap_b, token_b = _provision(client, "missing-fw", "9.9.9")

    desired = client.get(
        "/api/v1/device/desired-firmware",
        headers={"Authorization": f"Bearer {token_a}"},
    )
    assert desired.status_code == 200, desired.text
    assert desired.json() == {
        "node_id": node_a,
        "firmware_id": firmware_id,
        "firmware_version": "1.2.3",
        "app_sha256": app_digest,
        "app_size_bytes": len(b"application"),
    }
    desired_b = client.get(
        "/api/v1/device/desired-firmware",
        headers={"Authorization": f"Bearer {token_b}"},
    )
    assert desired_b.status_code == 200, desired_b.text
    assert desired_b.json()["app_sha256"] == ""
    assert desired_b.json()["app_size_bytes"] == 0

    image = client.get(
        "/api/v1/device/ota-image", headers={"Authorization": f"Bearer {token_a}"}
    )
    assert image.status_code == 200, image.text
    assert image.headers["cache-control"] == "no-store"
    assert image.headers["x-content-sha256"] == app_digest
    assert image.content == b"application"

    # Device B cannot pull A's Desired image; missing catalog Desired yields 404.
    assert (
        client.get(
            "/api/v1/device/ota-image", headers={"Authorization": f"Bearer {token_b}"}
        ).status_code
        == 404
    )
    assert client.get("/api/v1/device/ota-image").status_code == 401
    assert (
        client.get(
            "/api/v1/device/ota-image",
            headers={"Authorization": f"Bearer {settings.admin_token}"},
        ).status_code
        == 401
    )

    revoked = client.post(
        f"/api/v1/nodes/{node_a}/device-api-token/revoke", headers=_admin()
    )
    assert revoked.status_code == 204, revoked.text
    assert (
        client.get(
            "/api/v1/device/ota-image", headers={"Authorization": f"Bearer {token_a}"}
        ).status_code
        == 401
    )

    rotated = client.post(f"/api/v1/nodes/{node_a}/device-api-token", headers=_admin())
    assert rotated.status_code == 200, rotated.text
    fresh_token = rotated.json()["device_api_token"]

    monkeypatch.setattr(firmware_catalog, "ESP_OUTPUT_OTA_SLOT_BYTES", 4)
    oversized = client.get(
        "/api/v1/device/ota-image", headers={"Authorization": f"Bearer {fresh_token}"}
    )
    assert oversized.status_code == 409
    assert "OTA slot" in oversized.json()["detail"]

    with SessionLocal() as db:
        row = db.get(DesiredState, node_a)
        assert row is not None
        row.firmware_id = ""
        row.firmware_version = ""
        db.commit()
    assert (
        client.get(
            "/api/v1/device/ota-image",
            headers={"Authorization": f"Bearer {fresh_token}"},
        ).status_code
        == 404
    )


def test_desired_firmware_withholds_image_when_version_mismatches_catalog(
    tmp_path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    catalog_dir = tmp_path / "firmware-catalog"
    monkeypatch.setattr(api, "FIRMWARE_CATALOG_DIR", catalog_dir)
    monkeypatch.setattr(settings, "firmware_catalog_dir", str(catalog_dir))
    client = TestClient(create_app())

    firmware_id, bundle, metadata = make_firmware_pair()
    uploaded = upload_pair(client, firmware_id, bundle, metadata)
    assert uploaded.status_code == 201, uploaded.text

    node_id, _bootstrap, token = _provision(client, firmware_id, "1.2.3")
    with SessionLocal() as db:
        row = db.get(DesiredState, node_id)
        assert row is not None
        row.firmware_version = "9.9.9"
        db.commit()

    desired = client.get(
        "/api/v1/device/desired-firmware", headers={"Authorization": f"Bearer {token}"}
    )
    assert desired.status_code == 200, desired.text
    assert desired.json() == {
        "node_id": node_id,
        "firmware_id": firmware_id,
        "firmware_version": "9.9.9",
        "app_sha256": "",
        "app_size_bytes": 0,
    }
    ota = client.get(
        "/api/v1/device/ota-image", headers={"Authorization": f"Bearer {token}"}
    )
    assert ota.status_code == 409, ota.text
    assert "does not match" in ota.json()["detail"]

    rejected = client.put(
        f"/api/v1/nodes/{node_id}/desired",
        headers=_admin(),
        json={
            "firmware_version": "9.9.9",
            "config_version": 0,
            "calibration_version": 0,
            "classifier_version": "",
            "payload": {},
        },
    )
    assert rejected.status_code == 422, rejected.text
    assert "firmware-selection" in rejected.json()["detail"]

    restored = client.put(
        f"/api/v1/nodes/{node_id}/desired",
        headers=_admin(),
        json={
            "firmware_version": "1.2.3",
            "config_version": 0,
            "calibration_version": 0,
            "classifier_version": "",
            "payload": {},
        },
    )
    assert restored.status_code == 200, restored.text
    assert restored.json()["desired"]["firmware_version"] == "1.2.3"
    assert restored.json()["desired"]["firmware_id"] == firmware_id
