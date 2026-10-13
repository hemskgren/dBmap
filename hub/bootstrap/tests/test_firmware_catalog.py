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


def make_firmware_pair() -> tuple[str, bytes, bytes]:
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
        "chip": "esp32",
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
        "platform": "esp32",
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
