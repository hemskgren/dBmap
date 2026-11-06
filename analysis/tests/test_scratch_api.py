from datetime import datetime, timedelta, timezone
from unittest.mock import Mock

import pytest
from app import policy
from app.config import settings
from app.db import SessionLocal
from app.main import create_app
from app.models import ScratchObject
from app.policy import PolicyUnavailable
from fastapi.testclient import TestClient


def _admin() -> dict[str, str]:
    return {"Authorization": f"Bearer {settings.admin_token}"}


def _viewer() -> dict[str, str]:
    return {"Authorization": f"Bearer {settings.viewer_token}"}


def test_health() -> None:
    client = TestClient(create_app())
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json() == {"status": "ok", "service": "analysis"}


def test_admin_scratch_crud_roundtrip_and_viewer_read_only() -> None:
    client = TestClient(create_app())

    created = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={
            "namespace": "hub-event-track",
            "content": {"points": [1, 2, 3], "label": "trial"},
            "ttl_seconds": 600,
        },
    )
    assert created.status_code == 200, created.text
    body = created.json()
    object_id = body["id"]
    assert body["namespace"] == "hub-event-track"
    assert body["content"] == {"points": [1, 2, 3], "label": "trial"}
    assert body["created_by"] == "local-admin"
    assert body["created_role"] == "admin"
    assert 0 < body["ttl_remaining_s"] <= 600

    listed = client.get(
        "/api/v1/analysis/scratch?namespace=hub-event-track", headers=_viewer()
    )
    assert listed.status_code == 200
    assert listed.json()["count"] == 1
    assert listed.json()["objects"][0]["id"] == object_id

    fetched = client.get(f"/api/v1/analysis/scratch/{object_id}", headers=_viewer())
    assert fetched.status_code == 200
    assert fetched.json()["content"]["label"] == "trial"

    assert (
        client.post(
            "/api/v1/analysis/scratch",
            headers=_viewer(),
            json={"namespace": "x", "content": {}},
        ).status_code
        == 403
    )
    assert (
        client.put(
            f"/api/v1/analysis/scratch/{object_id}",
            headers=_viewer(),
            json={"content": {"no": "write"}},
        ).status_code
        == 403
    )
    assert (
        client.delete(
            f"/api/v1/analysis/scratch/{object_id}", headers=_viewer()
        ).status_code
        == 403
    )

    updated = client.put(
        f"/api/v1/analysis/scratch/{object_id}",
        headers=_admin(),
        json={"content": {"points": [9]}, "ttl_seconds": 120},
    )
    assert updated.status_code == 200
    assert updated.json()["content"] == {"points": [9]}
    assert updated.json()["ttl_remaining_s"] <= 120

    deleted = client.delete(f"/api/v1/analysis/scratch/{object_id}", headers=_admin())
    assert deleted.status_code == 200
    assert deleted.json() == {"deleted": 1}
    assert (
        client.get(f"/api/v1/analysis/scratch/{object_id}", headers=_admin()).status_code
        == 404
    )


def test_ttl_cannot_exceed_configured_maximum() -> None:
    client = TestClient(create_app())
    too_long = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={
            "namespace": "ttl-test",
            "content": {"ok": True},
            "ttl_seconds": settings.analysis_scratch_max_ttl_s + 1,
        },
    )
    assert too_long.status_code == 422
    assert "must not exceed" in too_long.json()["detail"]


def test_expired_objects_are_purged_on_access() -> None:
    client = TestClient(create_app())
    created = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={"namespace": "expire", "content": {"n": 1}, "ttl_seconds": 60},
    )
    assert created.status_code == 200
    object_id = created.json()["id"]

    db = SessionLocal()
    try:
        row = db.get(ScratchObject, object_id)
        assert row is not None
        row.expires_at = datetime.now(timezone.utc) - timedelta(seconds=1)
        db.commit()
    finally:
        db.close()

    assert (
        client.get(f"/api/v1/analysis/scratch/{object_id}", headers=_admin()).status_code
        == 404
    )
    listed = client.get("/api/v1/analysis/scratch", headers=_admin())
    assert listed.status_code == 200
    assert listed.json()["count"] == 0


def test_payload_and_object_count_limits(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(settings, "analysis_scratch_max_payload_bytes", 32)
    monkeypatch.setattr(settings, "analysis_scratch_max_objects", 2)
    client = TestClient(create_app())

    oversized = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={"namespace": "limits", "content": {"pad": "x" * 64}},
    )
    assert oversized.status_code == 413

    for index in range(2):
        ok = client.post(
            "/api/v1/analysis/scratch",
            headers=_admin(),
            json={"namespace": "limits", "content": {"i": index}},
        )
        assert ok.status_code == 200, ok.text

    blocked = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={"namespace": "limits", "content": {"i": 2}},
    )
    assert blocked.status_code == 409

    cleared = client.delete(
        "/api/v1/analysis/scratch?namespace=limits", headers=_admin()
    )
    assert cleared.status_code == 200
    assert cleared.json()["deleted"] == 2


def test_namespace_is_not_an_authorization_boundary() -> None:
    client = TestClient(create_app())
    first = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={"namespace": "script-a", "content": {"a": 1}},
    )
    second = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={"namespace": "script-b", "content": {"b": 2}},
    )
    assert first.status_code == 200 and second.status_code == 200

    assert (
        client.get(
            "/api/v1/analysis/scratch?namespace=script-a", headers=_viewer()
        ).json()["count"]
        == 1
    )
    assert (
        client.delete(
            f"/api/v1/analysis/scratch/{second.json()['id']}", headers=_admin()
        ).status_code
        == 200
    )


def test_list_can_omit_content_and_expose_map_metadata() -> None:
    client = TestClient(create_app())
    created = client.post(
        "/api/v1/analysis/scratch",
        headers=_admin(),
        json={
            "namespace": "map-demo",
            "content": {
                "kind": "map-layer",
                "dataset": "helicopter",
                "dataset_title": "Helicopter",
                "layer": "route",
                "title": "Helicopter ground-truth route",
                "role": "ground-truth",
                "points": [{"lat": 59.34, "lon": 18.07, "time_utc": "2026-01-01T00:00:00Z"}]
                * 20,
            },
        },
    )
    assert created.status_code == 200, created.text
    listed = client.get(
        "/api/v1/analysis/scratch?namespace=map-demo&include_content=false",
        headers=_viewer(),
    )
    assert listed.status_code == 200
    body = listed.json()
    assert body["count"] == 1
    item = body["objects"][0]
    assert item["content"] is None
    assert item["metadata"]["dataset"] == "helicopter"
    assert item["metadata"]["dataset_title"] == "Helicopter"
    assert item["metadata"]["layer"] == "route"
    assert item["content_bytes"] > 0


def test_unauthenticated_and_policy_unavailable(monkeypatch: pytest.MonkeyPatch) -> None:
    client = TestClient(create_app())
    assert client.get("/api/v1/analysis/scratch").status_code == 401

    monkeypatch.setattr(
        policy,
        "evaluate",
        Mock(
            side_effect=PolicyUnavailable(
                "authorization policy service is unavailable"
            )
        ),
    )
    response = client.get("/api/v1/analysis/scratch", headers=_admin())
    assert response.status_code == 503
    assert response.json()["detail"] == "authorization policy service is unavailable"
