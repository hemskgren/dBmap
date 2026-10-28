from pathlib import Path
from uuid import uuid4

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-chip-identity-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app.db import engine, init_db
from app.main import create_app
from app.models import Base
from app.schemas import normalize_chip_mac
from fastapi.testclient import TestClient


def _reset_db() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def test_normalize_chip_mac_accepts_common_formats() -> None:
    assert normalize_chip_mac("aa:bb:cc:dd:ee:ff") == "AA:BB:CC:DD:EE:FF"
    assert normalize_chip_mac("AABBCCDDEEFF") == "AA:BB:CC:DD:EE:FF"
    assert normalize_chip_mac("aa-bb-cc-dd-ee-ff") == "AA:BB:CC:DD:EE:FF"
    assert normalize_chip_mac(None) is None


def test_create_node_stores_chip_variant_and_rejects_duplicate_mac() -> None:
    _reset_db()
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    user = client.post(
        "/api/v1/users",
        json={"display_name": "Chip owner"},
        headers=admin_headers,
    ).json()

    created = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "output",
            "hardware_revision": "unknown",
            "chip_mac": "24:0a:c4:12:34:56",
            "chip_variant": "ESP32-D0WD-V3 (revision 3)",
            "owner_type": "user",
            "owner_id": user["user_id"],
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]

    detail = client.get(f"/api/v1/nodes/{node_id}", headers=admin_headers)
    assert detail.status_code == 200
    assert detail.json()["chip_mac"] == "24:0A:C4:12:34:56"
    assert detail.json()["chip_variant"] == "ESP32-D0WD-V3 (revision 3)"

    by_mac = client.get(
        "/api/v1/nodes/by-chip-mac/24%3A0A%3AC4%3A12%3A34%3A56",
        headers=admin_headers,
    )
    assert by_mac.status_code == 200
    assert by_mac.json()["node_id"] == node_id

    duplicate = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "output",
            "chip_mac": "24:0a:c4:12:34:56",
            "chip_variant": "ESP32-D0WD-V3 (revision 3)",
            "owner_type": "user",
            "owner_id": user["user_id"],
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert duplicate.status_code == 409
    assert node_id in duplicate.json()["detail"]

    replacement = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "output",
            "chip_mac": "24:0a:c4:12:34:56",
            "chip_variant": "ESP32-D0WD-V3 (revision 3)",
            "owner_type": "user",
            "owner_id": user["user_id"],
            "replace_existing_chip_mac": True,
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert replacement.status_code == 200, replacement.text
    assert replacement.json()["node_id"] != node_id

    old = client.get(f"/api/v1/nodes/{node_id}", headers=admin_headers)
    assert old.json()["chip_mac"] is None
    new = client.get(
        f"/api/v1/nodes/{replacement.json()['node_id']}", headers=admin_headers
    )
    assert new.json()["chip_mac"] == "24:0A:C4:12:34:56"
    assert new.json()["chip_variant"] == "ESP32-D0WD-V3 (revision 3)"


def test_create_node_skips_orphan_ownership_ids_after_nodes_wiped() -> None:
    _reset_db()
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    group = client.post(
        "/api/v1/groups",
        json={
            "name": "Orphan owners",
            "owner_user_id": client.post(
                "/api/v1/users",
                json={"display_name": "Group owner"},
                headers=admin_headers,
            ).json()["user_id"],
        },
        headers=admin_headers,
    ).json()
    first = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "output",
            "owner_type": "group",
            "owner_id": group["group_id"],
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert first.status_code == 200, first.text
    assert first.json()["node_id"] == "OUT-001"

    from app.db import SessionLocal
    from app.models import Node
    from sqlalchemy import delete

    with SessionLocal() as db:
        db.execute(delete(Node).where(Node.node_id == "OUT-001"))
        db.commit()

    created = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "output",
            "owner_type": "group",
            "owner_id": group["group_id"],
            "chip_mac": "AA:BB:CC:DD:EE:01",
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert created.status_code == 200, created.text
    assert created.json()["node_id"] == "OUT-002"


def test_devices_summary_includes_chip_fields() -> None:
    _reset_db()
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    created = client.post(
        "/api/v1/nodes",
        json={
            "node_type": "output",
            "chip_mac": "11:22:33:44:55:66",
            "chip_variant": "ESP32-D0WD (revision 1)",
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert created.status_code == 200, created.text
    devices = client.get("/api/v1/devices", headers=admin_headers)
    assert devices.status_code == 200
    match = next(
        item for item in devices.json() if item["node_id"] == created.json()["node_id"]
    )
    assert match["chip_mac"] == "11:22:33:44:55:66"
    assert match["chip_variant"] == "ESP32-D0WD (revision 1)"
