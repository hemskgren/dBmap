"""Pagination for GET /api/v1/users and GET /api/v1/groups (JSON envelope)."""

from __future__ import annotations

from pathlib import Path

import pytest
from app.config import settings
from fastapi.testclient import TestClient

settings.database_url = "sqlite:///" + str(
    Path("/tmp/dbmap-bootstrap-directory-pagination-test.db")
)
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.mqtt_host = "127.0.0.1"

from app.db import SessionLocal, engine, init_db
from app.group_service import list_groups_page
from app.identity_service import create_user, list_users_page
from app.main import create_app
from app.models import Base
from app.pagination import DEFAULT_PAGE_SIZE, MAX_PAGE_SIZE


def setup_module() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def _admin_client() -> TestClient:
    return TestClient(create_app())


def _headers() -> dict[str, str]:
    return {"Authorization": "Bearer test-admin"}


def _seed_users(count: int) -> list[str]:
    with SessionLocal() as db:
        return [
            create_user(db, f"User {index:03d}").user_id for index in range(count)
        ]


def _seed_groups(owner_user_id: str, count: int) -> list[str]:
    from app.group_service import create_group

    with SessionLocal() as db:
        return [
            create_group(db, f"Group {index:03d}", owner_user_id).group_id
            for index in range(count)
        ]


def test_users_default_page_size_is_200() -> None:
    assert DEFAULT_PAGE_SIZE == 200
    setup_module()
    _seed_users(3)
    with SessionLocal() as db:
        result = list_users_page(db)
        assert result.page_size == 200
        assert result.total == 3
        assert result.total_pages == 1
        assert result.order == "newest"
        assert result.snapshot_id is not None


def test_users_max_page_size_and_reject_above() -> None:
    setup_module()
    _seed_users(2)
    with SessionLocal() as db:
        ok = list_users_page(db, page_size=MAX_PAGE_SIZE)
        assert ok.page_size == MAX_PAGE_SIZE
        with pytest.raises(ValueError, match="page_size"):
            list_users_page(db, page_size=MAX_PAGE_SIZE + 1)


def test_users_first_middle_last_pages() -> None:
    setup_module()
    ids = _seed_users(8)
    with SessionLocal() as db:
        first = list_users_page(db, page=1, page_size=3, order="newest")
        middle = list_users_page(
            db,
            page=2,
            page_size=3,
            order="newest",
            snapshot_id=first.snapshot_id,
        )
        last = list_users_page(
            db,
            page=3,
            page_size=3,
            order="newest",
            snapshot_id=first.snapshot_id,
        )
    assert first.total == 8
    assert first.total_pages == 3
    assert len(first.items) == 3
    assert len(middle.items) == 3
    assert len(last.items) == 2
    seen = [item.user_id for item in first.items + middle.items + last.items]
    assert len(seen) == len(set(seen)) == 8
    assert set(seen) == set(ids)


def test_users_newest_and_oldest_ordering() -> None:
    setup_module()
    _seed_users(5)
    with SessionLocal() as db:
        newest = list_users_page(db, page_size=5, order="newest")
        oldest = list_users_page(db, page_size=5, order="oldest")
    newest_ids = [item.user_id for item in newest.items]
    oldest_ids = [item.user_id for item in oldest.items]
    assert newest_ids == list(reversed(oldest_ids))


def test_users_empty_result() -> None:
    setup_module()
    with SessionLocal() as db:
        result = list_users_page(db)
    assert result.total == 0
    assert result.total_pages == 0
    assert result.items == []
    assert result.snapshot_id is None


def test_users_http_envelope_and_invalid_page_size() -> None:
    setup_module()
    _seed_users(4)
    client = _admin_client()
    response = client.get(
        "/api/v1/users?page_size=2&page=1&order=oldest",
        headers=_headers(),
    )
    assert response.status_code == 200
    body = response.json()
    assert set(body.keys()) == {"pagination", "items"}
    assert body["pagination"]["page_size"] == 2
    assert body["pagination"]["page"] == 1
    assert body["pagination"]["order"] == "oldest"
    assert body["pagination"]["total_count"] == 4
    assert body["pagination"]["total_pages"] == 2
    assert body["pagination"]["snapshot_id"] is not None
    assert len(body["items"]) == 2

    bad = client.get("/api/v1/users?page_size=501", headers=_headers())
    assert bad.status_code == 422


def test_users_snapshot_prevents_duplicates_when_inserts_arrive() -> None:
    setup_module()
    _seed_users(5)
    with SessionLocal() as db:
        first = list_users_page(db, page=1, page_size=2, order="newest")
        snapshot = first.snapshot_id
        create_user(db, "Concurrent User A")
        create_user(db, "Concurrent User B")
        second = list_users_page(
            db,
            page=2,
            page_size=2,
            order="newest",
            snapshot_id=snapshot,
        )
        third = list_users_page(
            db,
            page=3,
            page_size=2,
            order="newest",
            snapshot_id=snapshot,
        )
    ids = [item.user_id for item in first.items + second.items + third.items]
    assert first.total == 5
    assert len(ids) == len(set(ids)) == 5
    assert all(not item.display_name.startswith("Concurrent") for item in first.items)
    assert all(
        not item.display_name.startswith("Concurrent")
        for item in second.items + third.items
    )


def test_groups_http_envelope_pages_and_snapshot() -> None:
    setup_module()
    owner_id = _seed_users(1)[0]
    group_ids = _seed_groups(owner_id, 7)
    client = _admin_client()
    first = client.get(
        "/api/v1/groups?page=1&page_size=3&order=newest",
        headers=_headers(),
    )
    assert first.status_code == 200
    body = first.json()
    assert set(body.keys()) == {"pagination", "items"}
    snapshot = body["pagination"]["snapshot_id"]
    assert body["pagination"]["total_count"] == 7
    assert body["pagination"]["total_pages"] == 3
    assert len(body["items"]) == 3

    # Concurrent inserts after page 1 must not appear in later snapshot pages.
    _seed_groups(owner_id, 3)
    collected = list(body["items"])
    for page in (2, 3):
        response = client.get(
            f"/api/v1/groups?page={page}&page_size=3&order=newest"
            f"&snapshot_id={snapshot}",
            headers=_headers(),
        )
        assert response.status_code == 200
        page_body = response.json()
        assert page_body["pagination"]["total_count"] == 7
        assert page_body["pagination"]["snapshot_id"] == snapshot
        collected.extend(page_body["items"])

    seen = [item["group_id"] for item in collected]
    assert len(seen) == len(set(seen)) == 7
    assert set(seen) == set(group_ids)


def test_groups_newest_oldest_and_empty() -> None:
    setup_module()
    owner_id = _seed_users(1)[0]
    _seed_groups(owner_id, 4)
    with SessionLocal() as db:
        newest = list_groups_page(db, page_size=10, order="newest")
        oldest = list_groups_page(db, page_size=10, order="oldest")
    assert [item.group_id for item in newest.items] == list(
        reversed([item.group_id for item in oldest.items])
    )

    setup_module()
    with SessionLocal() as db:
        empty = list_groups_page(db)
    assert empty.total == 0
    assert empty.items == []
    assert empty.snapshot_id is None


def test_groups_invalid_page_parameters() -> None:
    setup_module()
    client = _admin_client()
    assert (
        client.get("/api/v1/groups?page=0", headers=_headers()).status_code == 422
    )
    assert (
        client.get("/api/v1/groups?page_size=501", headers=_headers()).status_code
        == 422
    )
    with SessionLocal() as db, pytest.raises(ValueError, match="page"):
        list_groups_page(db, page=0)
