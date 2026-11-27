from datetime import UTC, datetime, timedelta

import pytest
from app import api as api_module
from app.api import MAX_PAGE_SIZE, list_events
from app.db import SessionLocal, init_db
from app.main import create_app
from app.models import OperationsMonitorEvent
from fastapi import HTTPException
from fastapi.testclient import TestClient


def test_operations_page_size_has_existing_project_bounds() -> None:
    assert MAX_PAGE_SIZE == 500


def test_event_pagination_is_bounded_ordered_and_snapshot_based() -> None:
    init_db()
    with SessionLocal() as db:
        db.query(OperationsMonitorEvent).delete()
        db.commit()
        now = datetime.now(UTC)
        for index in range(3):
            db.add(
                OperationsMonitorEvent(
                    hub_id="local-01",
                    occurred_at=now - timedelta(minutes=index),
                    observed_at=now - timedelta(minutes=index),
                    component="api",
                    event_type="state_changed",
                    previous_state="ok",
                    new_state="offline",
                    source="operations_monitor",
                    certainty="confirmed",
                )
            )
        db.commit()

        first = list_events(
            page=1,
            page_size=1,
            order="newest",
            snapshot_id=None,
            start=None,
            end=None,
            db=db,
        )
        assert first["pagination"]["total_count"] == 3
        assert first["pagination"]["total_pages"] == 3
        assert first["pagination"]["page_size"] == 1
        snapshot = first["pagination"]["snapshot_id"]
        assert snapshot is not None

        second = list_events(
            page=2,
            page_size=1,
            order="newest",
            snapshot_id=snapshot,
            start=None,
            end=None,
            db=db,
        )
        assert second["pagination"]["snapshot_id"] == snapshot
        assert len(second["events"]) == 1
        assert second["events"][0]["event_id"] != first["events"][0]["event_id"]


def test_event_pagination_rejects_invalid_parameters() -> None:
    init_db()
    with SessionLocal() as db:
        with pytest.raises(HTTPException) as exc:
            list_events(
                page=1,
                page_size=501,
                order="newest",
                snapshot_id=None,
                start=None,
                end=None,
                db=db,
            )
        assert exc.value.status_code == 422


def test_operations_health_and_status_authentication(monkeypatch) -> None:
    monkeypatch.setattr(api_module.settings, "admin_token", "a" * 32)
    monkeypatch.setattr(api_module.settings, "viewer_token", "v" * 32)
    monkeypatch.setattr(api_module.settings, "core_monitor_enabled", False)
    monkeypatch.setattr(api_module, "evaluate", lambda _input: True)
    init_db()
    with TestClient(create_app()) as client:
        health = client.get("/health")
        assert health.status_code == 200
        assert health.json()["service"] == "operations"

        unauthorized = client.get("/api/v1/operations/status")
        assert unauthorized.status_code == 401

        viewer = client.get(
            "/api/v1/operations/status",
            headers={"Authorization": "Bearer " + "v" * 32},
        )
        assert viewer.status_code == 200
        assert viewer.json()["service"]["service_id"] == "operations"
