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

    assert client.get("/api/v1/users", headers=viewer_headers).status_code == 403
    assert client.get("/api/v1/users").status_code == 401
    assert client.post(
        "/api/v1/users", json={"display_name": "Blocked"}, headers=viewer_headers
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
