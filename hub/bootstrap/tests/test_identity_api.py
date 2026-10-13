from pathlib import Path
from uuid import uuid4

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-bootstrap-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app.main import create_app
from fastapi.testclient import TestClient


def test_admin_can_manage_users_and_github_identities_only() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    viewer_headers = {"Authorization": f"Bearer {settings.viewer_token}"}

    created = client.post(
        "/api/v1/users",
        json={"display_name": "  Local User  "},
        headers=admin_headers,
    )
    assert created.status_code == 200, created.text
    user = created.json()
    assert user["display_name"] == "Local User"
    assert user["status"] == "active"
    assert user["external_identities"] == []

    viewer_list = client.get("/api/v1/users", headers=viewer_headers)
    assert viewer_list.status_code == 200
    assert any(item["user_id"] == user["user_id"] for item in viewer_list.json())
    viewer_detail = client.get(f"/api/v1/users/{user['user_id']}", headers=viewer_headers)
    assert viewer_detail.status_code == 200
    assert viewer_detail.json()["user_id"] == user["user_id"]
    assert client.get("/api/v1/users").status_code == 401
    assert client.post(
        "/api/v1/users", json={"display_name": "Blocked"}, headers=viewer_headers
    ).status_code == 403
    assert client.put(
        f"/api/v1/users/{user['user_id']}/status",
        json={"status": "disabled"},
        headers=viewer_headers,
    ).status_code == 403
    assert client.post(
        "/api/v1/users", json={"display_name": "   "}, headers=admin_headers
    ).status_code == 422

    link_path = f"/api/v1/users/{user['user_id']}/identities"
    provider_subject = str(uuid4().int % (10**32))
    link_body = {"provider": "github", "provider_subject": provider_subject}
    linked = client.post(link_path, json=link_body, headers=admin_headers)
    assert linked.status_code == 200, linked.text
    identity = linked.json()["external_identities"][0]
    assert identity["provider"] == "github"
    assert identity["provider_subject"] == provider_subject
    assert "email" not in identity
    assert "access_token" not in identity

    # Retrying the same explicit link is safe and does not create another row.
    retried = client.post(link_path, json=link_body, headers=admin_headers)
    assert retried.status_code == 200
    assert len(retried.json()["external_identities"]) == 1

    other = client.post(
        "/api/v1/users", json={"display_name": "Other"}, headers=admin_headers
    ).json()
    duplicate = client.post(
        f"/api/v1/users/{other['user_id']}/identities",
        json=link_body,
        headers=admin_headers,
    )
    assert duplicate.status_code == 409

    identity_path = f"/api/v1/users/{user['user_id']}/identities/{identity['identity_id']}/status"
    disabled_identity = client.put(
        identity_path, json={"status": "disabled"}, headers=admin_headers
    )
    assert disabled_identity.status_code == 200
    assert disabled_identity.json()["external_identities"][0]["status"] == "disabled"

    disabled_user = client.put(
        f"/api/v1/users/{user['user_id']}/status",
        json={"status": "disabled"},
        headers=admin_headers,
    )
    assert disabled_user.status_code == 200
    assert disabled_user.json()["status"] == "disabled"
    assert disabled_user.json()["external_identities"][0]["identity_id"] == identity["identity_id"]


def test_identity_management_returns_not_found_for_unknown_user() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}

    response = client.get("/api/v1/users/unknown-user", headers=admin_headers)

    assert response.status_code == 404


def test_admin_can_manage_groups_and_viewer_can_read_group_directory() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": "Bearer test-admin"}
    viewer_headers = {"Authorization": "Bearer test-viewer"}
    owner = client.post(
        "/api/v1/users", json={"display_name": "Group owner"}, headers=admin_headers
    ).json()
    member = client.post(
        "/api/v1/users", json={"display_name": "Group member"}, headers=admin_headers
    ).json()

    created = client.post(
        "/api/v1/groups",
        json={"name": "  Operations  ", "owner_user_id": owner["user_id"]},
        headers=admin_headers,
    )
    assert created.status_code == 200, created.text
    group = created.json()
    assert group["name"] == "Operations"
    assert group["status"] == "active"
    assert [(item["user_id"], item["role"]) for item in group["members"]] == [
        (owner["user_id"], "owner")
    ]
    assert any(
        item["group_id"] == group["group_id"]
        for item in client.get("/api/v1/groups", headers=viewer_headers).json()
    )
    assert client.post(
        "/api/v1/groups",
        json={"name": "Missing owner"},
        headers=admin_headers,
    ).status_code == 422

    added = client.put(
        f"/api/v1/groups/{group['group_id']}/members/{member['user_id']}",
        json={"role": "member"},
        headers=admin_headers,
    )
    assert added.status_code == 200, added.text
    assert len(added.json()["members"]) == 2

    changed = client.put(
        f"/api/v1/groups/{group['group_id']}/members/{member['user_id']}",
        json={"role": "viewer"},
        headers=admin_headers,
    )
    assert next(item for item in changed.json()["members"] if item["user_id"] == member["user_id"])["role"] == "viewer"

    removed = client.delete(
        f"/api/v1/groups/{group['group_id']}/members/{member['user_id']}",
        headers=admin_headers,
    )
    assert removed.status_code == 200
    assert [item["user_id"] for item in removed.json()["members"]] == [owner["user_id"]]
    cannot_remove_last_owner = client.delete(
        f"/api/v1/groups/{group['group_id']}/members/{owner['user_id']}",
        headers=admin_headers,
    )
    assert cannot_remove_last_owner.status_code == 409

    disabled = client.put(
        f"/api/v1/groups/{group['group_id']}/status",
        json={"status": "disabled"},
        headers=admin_headers,
    )
    assert disabled.status_code == 200
    assert disabled.json()["status"] == "disabled"


def test_admin_can_assign_reassign_and_clear_device_ownership_without_changing_device_id() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    viewer_headers = {"Authorization": f"Bearer {settings.viewer_token}"}
    node = client.post("/api/v1/nodes", json={"node_type": "ear"}, headers=admin_headers)
    assert node.status_code == 200, node.text
    node_id = node.json()["node_id"]
    first_user = client.post(
        "/api/v1/users", json={"display_name": "First owner"}, headers=admin_headers
    ).json()
    second_user = client.post(
        "/api/v1/users", json={"display_name": "Second owner"}, headers=admin_headers
    ).json()

    empty = client.get(f"/api/v1/nodes/{node_id}/ownership", headers=admin_headers)
    assert empty.status_code == 200
    assert empty.json() == {
        "node_id": node_id,
        "current_owner_type": None,
        "current_owner_id": None,
        "current_owner_display_name": None,
        "assignments": [],
    }
    assert client.get(
        f"/api/v1/nodes/{node_id}/ownership", headers=viewer_headers
    ).status_code == 403

    assigned = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": "user", "owner_id": first_user["user_id"]},
        headers=admin_headers,
    )
    assert assigned.status_code == 200, assigned.text
    assert assigned.json()["current_owner_id"] == first_user["user_id"]

    reassigned = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": "user", "owner_id": second_user["user_id"]},
        headers=admin_headers,
    )
    assert reassigned.status_code == 200, reassigned.text
    history = reassigned.json()
    assert history["current_owner_id"] == second_user["user_id"]
    assert [item["owner_id"] for item in history["assignments"]] == [
        first_user["user_id"],
        second_user["user_id"],
    ]
    assert history["assignments"][0]["valid_to"] is not None
    assert history["assignments"][1]["valid_to"] is None

    unassigned = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": None, "owner_id": None},
        headers=admin_headers,
    )
    assert unassigned.status_code == 200
    assert unassigned.json()["current_owner_id"] is None
    assert len(unassigned.json()["assignments"]) == 2
    assert unassigned.json()["assignments"][1]["valid_to"] is not None
    assert client.get(f"/api/v1/nodes/{node_id}", headers=admin_headers).json()["node_id"] == node_id

    group = client.post(
        "/api/v1/groups",
        json={"name": "Device group", "owner_user_id": first_user["user_id"]},
        headers=admin_headers,
    )
    assert group.status_code == 200, group.text
    assigned_group = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": "group", "owner_id": group.json()["group_id"]},
        headers=admin_headers,
    )
    assert assigned_group.status_code == 200, assigned_group.text
    assert assigned_group.json()["current_owner_type"] == "group"
    device = client.get("/api/v1/devices", headers=viewer_headers)
    summary = next(item for item in device.json() if item["node_id"] == node_id)
    assert summary["owner_type"] == "group"
    assert summary["owner_id"] == group.json()["group_id"]
    assert summary["owner_display_name"] == "Device group"


def test_device_ownership_requires_existing_active_user() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    node = client.post("/api/v1/nodes", json={"node_type": "ear"}, headers=admin_headers)
    node_id = node.json()["node_id"]
    unknown = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": "user", "owner_id": str(uuid4())},
        headers=admin_headers,
    )
    assert unknown.status_code == 404

    user = client.post(
        "/api/v1/users", json={"display_name": "Disabled"}, headers=admin_headers
    ).json()
    client.put(
        f"/api/v1/users/{user['user_id']}/status",
        json={"status": "disabled"},
        headers=admin_headers,
    )
    disabled = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": "user", "owner_id": user["user_id"]},
        headers=admin_headers,
    )
    assert disabled.status_code == 409


def test_node_creation_assigns_owner_atomically_and_is_idempotent() -> None:
    client = TestClient(create_app())
    admin_headers = {"Authorization": "Bearer test-admin"}
    before = client.get("/api/v1/nodes", headers=admin_headers)
    assert before.status_code == 200
    user = client.post(
        "/api/v1/users",
        json={"display_name": "Deployment owner"},
        headers=admin_headers,
    ).json()
    idempotency_key = str(uuid4())
    headers = {**admin_headers, "Idempotency-Key": idempotency_key}
    payload = {
        "node_type": "output",
        "hardware_revision": "unknown",
        "owner_type": "user",
        "owner_id": user["user_id"],
    }

    created = client.post("/api/v1/nodes", json=payload, headers=headers)
    assert created.status_code == 200, created.text
    node_id = created.json()["node_id"]
    detail = client.get(f"/api/v1/nodes/{node_id}", headers=admin_headers)
    assert detail.status_code == 200
    assert detail.json()["owner_type"] == "user"
    assert detail.json()["owner_id"] == user["user_id"]

    retry = client.post("/api/v1/nodes", json=payload, headers=headers)
    assert retry.status_code == 409
    assert node_id in retry.json()["detail"]
    assert "bootstrap_token" not in retry.json()
    after_retry = client.get("/api/v1/nodes", headers=admin_headers)
    assert len(after_retry.json()) == len(before.json()) + 1

    invalid_owner = client.post(
        "/api/v1/nodes",
        json={
            **payload,
            "owner_id": str(uuid4()),
        },
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert invalid_owner.status_code == 404
    after_invalid = client.get("/api/v1/nodes", headers=admin_headers)
    assert len(after_invalid.json()) == len(after_retry.json())

    disabled_user = client.post(
        "/api/v1/users",
        json={"display_name": "Disabled deployment owner"},
        headers=admin_headers,
    ).json()
    client.put(
        f"/api/v1/users/{disabled_user['user_id']}/status",
        json={"status": "disabled"},
        headers=admin_headers,
    )
    inactive_owner = client.post(
        "/api/v1/nodes",
        json={**payload, "owner_id": disabled_user["user_id"]},
        headers={**admin_headers, "Idempotency-Key": str(uuid4())},
    )
    assert inactive_owner.status_code == 409
    after_inactive = client.get("/api/v1/nodes", headers=admin_headers)
    assert len(after_inactive.json()) == len(after_retry.json())

    viewer_create = client.post(
        "/api/v1/nodes",
        json=payload,
        headers={"Authorization": "Bearer test-viewer", "Idempotency-Key": str(uuid4())},
    )
    assert viewer_create.status_code == 403


def test_node_policy_input_uses_database_owner_not_client_input(monkeypatch) -> None:
    from app import policy

    client = TestClient(create_app())
    admin_headers = {"Authorization": f"Bearer {settings.admin_token}"}
    node = client.post("/api/v1/nodes", json={"node_type": "ear"}, headers=admin_headers)
    user = client.post(
        "/api/v1/users", json={"display_name": "Assigned"}, headers=admin_headers
    ).json()
    node_id = node.json()["node_id"]
    assigned = client.put(
        f"/api/v1/nodes/{node_id}/ownership",
        json={"owner_type": "user", "owner_id": user["user_id"]},
        headers=admin_headers,
    )
    assert assigned.status_code == 200

    inputs = []
    monkeypatch.setattr(policy, "evaluate", lambda data: inputs.append(data) or True)
    response = client.get(f"/api/v1/nodes/{node_id}", headers=admin_headers)

    assert response.status_code == 200
    assert inputs[-1]["resource"]["owner_id"] == user["user_id"]
