import hashlib
import io
import json
import zipfile
from uuid import uuid4

import pytest
from app.config import settings

settings.database_url = "sqlite:////tmp/dbmap-bootstrap-test.db"
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app import api
from app.main import create_app
from fastapi.testclient import TestClient


@pytest.fixture
def catalog_client(tmp_path, monkeypatch):
    monkeypatch.setattr(api, "FIRMWARE_CATALOG_DIR", tmp_path / "firmware-catalog")
    return TestClient(create_app()), tmp_path / "firmware-catalog"


def make_firmware_pair(platform: str = "esp32") -> tuple[str, bytes, bytes]:
    firmware_id = f"esp-output-{uuid4().hex[:12]}"
    components = {
        "bootloader.bin": ("0x1000", b"bootloader"),
        "partition-table.bin": ("0x8000", b"partition-table"),
        "esp-output.bin": ("0x10000", b"application"),
    }
    component_manifest = [
        {
            "name": name,
            "offset": offset,
            "size_bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        }
        for name, (offset, content) in components.items()
    ]
    manifest = {
        "target": "esp-output-esp32",
        "node_type": "output",
        "chip": platform,
        "hardware_revision": "OUTPUT-DEV-V1",
        "firmware_version": "1.2.3",
        "esp_idf_version": "v5.4.2",
        "configuration_version": 0,
        "nvs_preserved": True,
        "flash_components": component_manifest,
    }
    bundle_buffer = io.BytesIO()
    with zipfile.ZipFile(bundle_buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, (_, content) in components.items():
            archive.writestr(name, content)
        archive.writestr("manifest.json", json.dumps(manifest))
    bundle = bundle_buffer.getvalue()
    metadata = {
        "firmware_id": firmware_id,
        "build_date_utc": "2026-10-13T08:00:00Z",
        "target": "esp-output-esp32",
        "node_type": "output",
        "platform": platform,
        "hardware_revision": "OUTPUT-DEV-V1",
        "firmware_version": "1.2.3",
        "esp_idf_version": "v5.4.2",
        "configuration_version": 0,
        "artifact_sha256": hashlib.sha256(bundle).hexdigest(),
    }
    return firmware_id, bundle, json.dumps(metadata).encode()


def upload_pair(client, firmware_id: str, bundle: bytes, metadata: bytes, token: str = "test-admin"):
    return client.post(
        "/api/v1/firmware/catalog",
        headers={"Authorization": f"Bearer {token}"},
        files={
            "firmware": (f"{firmware_id}.zip", bundle, "application/zip"),
            "metadata": (f"{firmware_id}.json", metadata, "application/json"),
        },
    )


def test_catalog_candidate_promote_viewer_access_and_delete(catalog_client):
    client, catalog_dir = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()
    admin = {"Authorization": f"Bearer {settings.admin_token}"}
    viewer = {"Authorization": f"Bearer {settings.viewer_token}"}

    uploaded = upload_pair(client, firmware_id, bundle, metadata)
    assert uploaded.status_code == 201, uploaded.text
    candidate = uploaded.json()
    assert candidate["firmware_id"] == firmware_id
    assert candidate["status"] == "candidate"
    assert candidate["node_type"] == "output"
    assert (catalog_dir / f"{firmware_id}.zip").read_bytes() == bundle

    viewer_entries = client.get("/api/v1/firmware/catalog", headers=viewer)
    assert viewer_entries.status_code == 200
    assert any(
        item["firmware_id"] == firmware_id and item["status"] == "candidate"
        for item in viewer_entries.json()
    )
    assert (
        client.get(f"/api/v1/firmware/catalog/{firmware_id}", headers=viewer).json()["status"]
        == "candidate"
    )
    candidate_download = client.get(
        f"/api/v1/firmware/catalog/{firmware_id}/artifact",
        headers=viewer,
    )
    assert candidate_download.status_code == 200
    assert candidate_download.content == bundle
    assert client.get("/api/v1/firmware/catalog", headers=admin).json()[0]["status"] == "candidate"
    assert upload_pair(client, f"blocked-{uuid4().hex}", bundle, metadata, token="test-viewer").status_code == 403
    assert client.post(
        f"/api/v1/firmware/catalog/{firmware_id}/promote",
        headers=viewer,
    ).status_code == 403

    promoted = client.post(
        f"/api/v1/firmware/catalog/{firmware_id}/promote",
        headers=admin,
    )
    assert promoted.status_code == 200
    assert promoted.json()["status"] == "registered"
    assert promoted.json()["registered_at"]
    viewer_entries = client.get("/api/v1/firmware/catalog", headers=viewer)
    assert any(
        item["firmware_id"] == firmware_id and item["status"] == "registered"
        for item in viewer_entries.json()
    )
    download = client.get(
        f"/api/v1/firmware/catalog/{firmware_id}/artifact",
        headers=viewer,
    )
    assert download.status_code == 200
    assert download.content == bundle

    assert client.delete(
        f"/api/v1/firmware/catalog/{firmware_id}",
        headers=viewer,
    ).status_code == 403
    assert client.delete(
        f"/api/v1/firmware/catalog/{firmware_id}",
        headers=admin,
    ).status_code == 204
    assert not (catalog_dir / f"{firmware_id}.zip").exists()
    assert all(
        item["firmware_id"] != firmware_id
        for item in client.get("/api/v1/firmware/catalog", headers=viewer).json()
    )


def test_flash_manifest_and_images_require_registered_firmware_and_admin(catalog_client):
    client, catalog_dir = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()
    admin = {"Authorization": "Bearer test-admin"}
    viewer = {"Authorization": "Bearer test-viewer"}
    base_path = f"/api/v1/firmware/catalog/{firmware_id}"

    assert upload_pair(client, firmware_id, bundle, metadata).status_code == 201
    assert client.get(f"{base_path}/flash-manifest", headers=admin).status_code == 404
    assert client.get(f"{base_path}/flash-manifest", headers=viewer).status_code == 403
    assert client.post(f"{base_path}/promote", headers=admin).status_code == 200

    manifest_response = client.get(f"{base_path}/flash-manifest", headers=admin)
    assert manifest_response.status_code == 200, manifest_response.text
    assert manifest_response.json() == {
        "name": "dBmap output",
        "version": "1.2.3",
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32",
                "parts": [
                    {"path": "bootloader.bin", "offset": 0x1000},
                    {"path": "partition-table.bin", "offset": 0x8000},
                    {"path": "esp-output.bin", "offset": 0x10000},
                ],
            }
        ],
    }
    assert client.get(f"{base_path}/flash-manifest", headers=viewer).status_code == 403

    for component_name, expected_content in (
        ("bootloader.bin", b"bootloader"),
        ("partition-table.bin", b"partition-table"),
        ("esp-output.bin", b"application"),
    ):
        response = client.get(
            f"{base_path}/flash-images/{component_name}",
            headers=admin,
        )
        assert response.status_code == 200, response.text
        assert response.content == expected_content
        assert response.headers["cache-control"] == "no-store"
        assert response.headers["x-content-sha256"] == hashlib.sha256(expected_content).hexdigest()
        assert (
            client.get(f"{base_path}/flash-images/{component_name}", headers=viewer).status_code
            == 403
        )
    assert client.get(f"{base_path}/flash-images/not-a-component.bin", headers=admin).status_code == 404

    (catalog_dir / f"{firmware_id}.zip").write_bytes(b"tampered artifact")
    assert client.get(f"{base_path}/flash-manifest", headers=admin).status_code == 409
    assert client.get(f"{base_path}/flash-images/bootloader.bin", headers=admin).status_code == 409


def test_flash_manifest_rejects_unsupported_platform(catalog_client):
    client, _ = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair(platform="esp32x")
    admin = {"Authorization": "Bearer test-admin"}
    base_path = f"/api/v1/firmware/catalog/{firmware_id}"

    assert upload_pair(client, firmware_id, bundle, metadata).status_code == 201
    assert client.post(f"{base_path}/promote", headers=admin).status_code == 200
    response = client.get(f"{base_path}/flash-manifest", headers=admin)

    assert response.status_code == 422
    assert "does not support" in response.json()["detail"]


def test_admin_can_record_verified_hardware_revision_for_compatibility(catalog_client):
    client, _ = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()
    admin = {"Authorization": "Bearer test-admin"}
    viewer = {"Authorization": "Bearer test-viewer"}
    assert upload_pair(client, firmware_id, bundle, metadata).status_code == 201
    assert client.post(
        f"/api/v1/firmware/catalog/{firmware_id}/promote",
        headers=admin,
    ).status_code == 200
    created = client.post(
        "/api/v1/nodes",
        headers=admin,
        json={"node_type": "output", "hardware_revision": "unknown"},
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]
    compatible_path = f"/api/v1/nodes/{node_id}/compatible-firmware"
    update_path = f"/api/v1/nodes/{node_id}/hardware-revision"

    assert client.get(compatible_path, headers=admin).json() == []
    assert client.put(
        update_path,
        headers=viewer,
        json={"hardware_revision": "OUTPUT-DEV-V1"},
    ).status_code == 403
    assert client.put(
        update_path,
        headers=admin,
        json={"hardware_revision": "   "},
    ).status_code == 422

    updated = client.put(
        update_path,
        headers=admin,
        json={"hardware_revision": " OUTPUT-DEV-V1 "},
    )
    assert updated.status_code == 200, updated.text
    assert updated.json()["hardware_revision"] == "OUTPUT-DEV-V1"
    compatible = client.get(compatible_path, headers=admin)
    assert compatible.status_code == 200
    assert firmware_id in {entry["firmware_id"] for entry in compatible.json()}
    assert client.put(
        "/api/v1/nodes/OUT-NOT-REGISTERED/hardware-revision",
        headers=admin,
        json={"hardware_revision": "OUTPUT-DEV-V1"},
    ).status_code == 404


def test_firmware_compatibility_is_server_checked_and_admin_only(catalog_client):
    client, _ = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()
    uploaded = upload_pair(client, firmware_id, bundle, metadata)
    assert uploaded.status_code == 201, uploaded.text

    candidate_node_response = client.post(
        "/api/v1/nodes",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={"node_type": "output", "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert candidate_node_response.status_code == 200, candidate_node_response.text
    node_id = candidate_node_response.json()["node_id"]

    compatible_before_promotion = client.get(
        f"/api/v1/nodes/{node_id}/compatible-firmware",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert compatible_before_promotion.status_code == 200
    assert all(item["firmware_id"] != firmware_id for item in compatible_before_promotion.json())
    candidate_selection = client.post(
        f"/api/v1/nodes/{node_id}/firmware-selection",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={
            "firmware_id": firmware_id,
            "acknowledge_unverified_platform": True,
        },
    )
    assert candidate_selection.status_code == 404

    promoted = client.post(
        f"/api/v1/firmware/catalog/{firmware_id}/promote",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert promoted.status_code == 200

    compatible = client.get(
        f"/api/v1/nodes/{node_id}/compatible-firmware",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert compatible.status_code == 200
    match = next(item for item in compatible.json() if item["firmware_id"] == firmware_id)
    assert match["compatibility"] == {
        "node_type": "matched",
        "hardware_revision": "matched",
        "hardware_reference": "registered_device_record",
        "platform": "unknown",
    }
    assert match["requires_platform_review"] is True

    selection_path = f"/api/v1/nodes/{node_id}/firmware-selection"
    blocked_unacknowledged = client.post(
        selection_path,
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={"firmware_id": firmware_id},
    )
    assert blocked_unacknowledged.status_code == 409
    assert "chip platform" in blocked_unacknowledged.json()["detail"]

    selection = client.post(
        selection_path,
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={
            "firmware_id": firmware_id,
            "acknowledge_unverified_platform": True,
        },
    )
    assert selection.status_code == 200
    assert selection.json()["device_id"] == node_id
    assert selection.json()["acknowledged_unverified_platform"] is True
    assert selection.json()["selection_persisted"] is False
    assert selection.json()["flashing_started"] is False

    viewer_selection = client.get(
        f"/api/v1/nodes/{node_id}/compatible-firmware",
        headers={"Authorization": f"Bearer {settings.viewer_token}"},
    )
    assert viewer_selection.status_code == 403

    mismatch_response = client.post(
        "/api/v1/nodes",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={"node_type": "output", "hardware_revision": "OUTPUT-DEV-V2"},
    )
    assert mismatch_response.status_code == 200, mismatch_response.text
    mismatch_node_id = mismatch_response.json()["node_id"]
    mismatch_list = client.get(
        f"/api/v1/nodes/{mismatch_node_id}/compatible-firmware",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
    )
    assert all(item["firmware_id"] != firmware_id for item in mismatch_list.json())
    mismatch_selection = client.post(
        f"/api/v1/nodes/{mismatch_node_id}/firmware-selection",
        headers={"Authorization": f"Bearer {settings.admin_token}"},
        json={
            "firmware_id": firmware_id,
            "acknowledge_unverified_platform": True,
        },
    )
    assert mismatch_selection.status_code == 422
    assert "hardware_revision" in mismatch_selection.json()["detail"]


def test_deployment_attempt_history_retry_and_admin_access(catalog_client):
    client, _ = catalog_client
    admin = {"Authorization": f"Bearer {settings.admin_token}"}
    viewer = {"Authorization": f"Bearer {settings.viewer_token}"}
    firmware_id, bundle, metadata = make_firmware_pair()
    assert upload_pair(client, firmware_id, bundle, metadata).status_code == 201
    assert (
        client.post(
            f"/api/v1/firmware/catalog/{firmware_id}/promote",
            headers=admin,
        ).status_code
        == 200
    )
    node_response = client.post(
        "/api/v1/nodes",
        headers=admin,
        json={"node_type": "output", "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert node_response.status_code == 200, node_response.text
    node_id = node_response.json()["node_id"]
    attempts_path = f"/api/v1/nodes/{node_id}/deployment-attempts"
    request_body = {
        "firmware_id": firmware_id,
        "acknowledge_unverified_platform": True,
        "flasher_version": "web-serial-test",
    }

    assert client.post(attempts_path, headers=viewer, json=request_body).status_code == 403
    unacknowledged = client.post(
        attempts_path,
        headers=admin,
        json={"firmware_id": firmware_id},
    )
    assert unacknowledged.status_code == 409
    started = client.post(attempts_path, headers=admin, json=request_body)
    assert started.status_code == 201, started.text
    first_attempt = started.json()
    first_attempt_id = first_attempt["attempt_id"]
    assert first_attempt["status"] == "in_progress"
    assert first_attempt["stage"] == "connecting"
    assert first_attempt["progress_percent"] == 0
    assert first_attempt["firmware_version"] == "1.2.3"
    assert first_attempt["artifact_sha256"]
    assert first_attempt["initiated_by"] == "local-admin"
    assert first_attempt["started_at"] and first_attempt["completed_at"] is None
    assert len(first_attempt["events"]) == 1
    assert client.post(attempts_path, headers=admin, json=request_body).status_code == 409

    attempt_path = f"{attempts_path}/{first_attempt_id}"
    progress = client.patch(
        attempt_path,
        headers=admin,
        json={"status": "in_progress", "stage": "writing", "progress_percent": 45},
    )
    assert progress.status_code == 200, progress.text
    assert progress.json()["progress_percent"] == 45
    assert len(progress.json()["events"]) == 2

    regression = client.patch(
        attempt_path,
        headers=admin,
        json={"status": "in_progress", "stage": "erasing", "progress_percent": 30},
    )
    assert regression.status_code == 409
    assert (
        client.patch(
            attempt_path,
            headers=viewer,
            json={"status": "in_progress", "stage": "verifying", "progress_percent": 80},
        ).status_code
        == 403
    )

    failed = client.patch(
        attempt_path,
        headers=admin,
        json={"status": "failed", "stage": "failed", "failure_code": "flash_failed"},
    )
    assert failed.status_code == 200, failed.text
    assert failed.json()["status"] == "failed"
    assert failed.json()["failure_code"] == "flash_failed"
    assert failed.json()["completed_at"]
    assert (
        client.patch(
            attempt_path,
            headers=admin,
            json={"status": "succeeded", "stage": "completed", "progress_percent": 100},
        ).status_code
        == 409
    )
    assert (
        client.patch(
            attempt_path,
            headers=admin,
            json={
                "status": "in_progress",
                "stage": "writing",
                "progress_percent": 30,
                "error_detail": "must not be persisted",
            },
        ).status_code
        == 422
    )

    retried = client.post(attempts_path, headers=admin, json=request_body)
    assert retried.status_code == 201, retried.text
    assert retried.json()["attempt_id"] != first_attempt_id
    completed = client.patch(
        f"{attempts_path}/{retried.json()['attempt_id']}",
        headers=admin,
        json={"status": "succeeded", "stage": "completed", "progress_percent": 100},
    )
    assert completed.status_code == 200
    assert completed.json()["status"] == "succeeded"
    assert completed.json()["progress_percent"] == 100

    unknown = client.post(attempts_path, headers=admin, json=request_body)
    assert unknown.status_code == 201
    unknown_outcome = client.patch(
        f"{attempts_path}/{unknown.json()['attempt_id']}",
        headers=admin,
        json={
            "status": "unknown",
            "stage": "unknown",
            "failure_code": "browser_interrupted",
        },
    )
    assert unknown_outcome.status_code == 200
    assert unknown_outcome.json()["status"] == "unknown"
    assert unknown_outcome.json()["failure_code"] == "browser_interrupted"

    history = client.get(attempts_path, headers=admin)
    assert history.status_code == 200
    assert {item["attempt_id"] for item in history.json()} == {
        first_attempt_id,
        retried.json()["attempt_id"],
        unknown.json()["attempt_id"],
    }
    assert (
        client.get(f"{attempts_path}/{first_attempt_id}", headers=viewer).status_code
        == 403
    )


def test_deployment_attempt_rejects_modified_artifact(catalog_client):
    client, catalog_dir = catalog_client
    admin = {"Authorization": f"Bearer {settings.admin_token}"}
    firmware_id, bundle, metadata = make_firmware_pair()
    assert upload_pair(client, firmware_id, bundle, metadata).status_code == 201
    assert (
        client.post(
            f"/api/v1/firmware/catalog/{firmware_id}/promote",
            headers=admin,
        ).status_code
        == 200
    )
    node_response = client.post(
        "/api/v1/nodes",
        headers=admin,
        json={"node_type": "output", "hardware_revision": "OUTPUT-DEV-V1"},
    )
    assert node_response.status_code == 200
    artifact = catalog_dir / f"{firmware_id}.zip"
    artifact.write_bytes(b"modified after catalog registration")
    attempts_path = f"/api/v1/nodes/{node_response.json()['node_id']}/deployment-attempts"
    body = {"firmware_id": firmware_id, "acknowledge_unverified_platform": True}

    attempt_response = client.post(attempts_path, headers=admin, json=body)
    download_response = client.get(
        f"/api/v1/firmware/catalog/{firmware_id}/artifact",
        headers=admin,
    )

    assert attempt_response.status_code == 409
    assert "integrity check" in attempt_response.json()["detail"]
    assert download_response.status_code == 409


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (lambda metadata: metadata.update(artifact_sha256="0" * 64), "artifact_sha256"),
        (lambda metadata: metadata.update(platform="esp32s3"), "platform"),
        (lambda metadata: metadata.update(build_date_utc="not-a-date"), "build_date_utc"),
        (lambda metadata: metadata.update(node_type="ear"), "node_type"),
    ],
)
def test_upload_rejects_invalid_or_mismatched_metadata(catalog_client, mutate, message):
    client, catalog_dir = catalog_client
    firmware_id, bundle, metadata_bytes = make_firmware_pair()
    metadata = json.loads(metadata_bytes)
    mutate(metadata)

    response = upload_pair(client, firmware_id, bundle, json.dumps(metadata).encode())

    assert response.status_code == 422
    assert message in response.json()["detail"]
    assert not (catalog_dir / f"{firmware_id}.zip").exists()


def test_upload_rejects_duplicate_firmware_id(catalog_client):
    client, _ = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()

    assert upload_pair(client, firmware_id, bundle, metadata).status_code == 201
    duplicate = upload_pair(client, firmware_id, bundle, metadata)

    assert duplicate.status_code == 409


def test_upload_does_not_replace_existing_unindexed_artifact(catalog_client):
    client, catalog_dir = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()
    catalog_dir.mkdir()
    existing_artifact = catalog_dir / f"{firmware_id}.zip"
    existing_artifact.write_bytes(b"existing artifact")

    response = upload_pair(client, firmware_id, bundle, metadata)

    assert response.status_code == 409
    assert existing_artifact.read_bytes() == b"existing artifact"


def test_upload_requires_filenames_to_match_metadata_id(catalog_client):
    client, catalog_dir = catalog_client
    firmware_id, bundle, metadata = make_firmware_pair()

    response = client.post(
        "/api/v1/firmware/catalog",
        headers={"Authorization": "Bearer test-admin"},
        files={
            "firmware": ("different-id.zip", bundle, "application/zip"),
            "metadata": (f"{firmware_id}.json", metadata, "application/json"),
        },
    )

    assert response.status_code == 422
    assert "filenames must match" in response.json()["detail"]
    assert not (catalog_dir / f"{firmware_id}.zip").exists()
