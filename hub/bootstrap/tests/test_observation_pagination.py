"""Pagination for GET /api/v1/observations (JSON envelope)."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from pathlib import Path
from uuid import uuid4

import pytest
from app.config import settings
from fastapi.testclient import TestClient
from sqlalchemy import text

settings.database_url = "sqlite:///" + str(
    Path("/tmp/dbmap-bootstrap-obs-pagination-test.db")
)
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.mqtt_host = "127.0.0.1"

from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import Base, Node
from app.schemas import ObservationBody
from app.service import (
    DEFAULT_OBSERVATION_PAGE_SIZE,
    MAX_OBSERVATION_PAGE_SIZE,
    ingest_observation,
    list_observations_page,
)


def setup_module() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def _empty_l0() -> dict:
    return {
        "schema": "l0",
        "version": 1,
        "band_plan_version": 1,
        "crest_threshold_version": 1,
        "crest_threshold_db": 12.0,
        "mic_index": 0,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "window_count": 0,
        "clipped_window_count": 0,
        "n_crest": 0,
        "rms": {"mean_dbfs": None, "min_dbfs": None, "max_dbfs": None},
        "bands": {
            "mean_dbfs": [None, None, None, None],
            "fraction": [None, None, None, None],
        },
        "hl": {"mean_db": None},
        "t1": {"rms_stdev_db": None, "rms_range_db": None},
        "t2": {"rms_slope_db_per_s": None},
        "t3": {
            "crest_max_db": None,
            "high_crest_count": None,
            "high_crest_rate": None,
        },
        "t6": {"hl_slope_db_per_s": None},
    }


def _payload(
    node_id: str,
    *,
    observation_id: str,
    sequence_number: int,
    event_time: datetime,
    emission_kind: str = "periodic",
) -> dict:
    ctx = {
        "event_time_utc": event_time.isoformat(),
        "capture_timestamp_monotonic_us": sequence_number,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": 1000.0,
        "episode_duration_ms": float(sequence_number * 1000),
        "emission_kind": emission_kind,
    }
    if emission_kind in ("end", "complete"):
        ctx["termination_reason"] = "natural"
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": {
            "observation_id": observation_id,
            "node_id": node_id,
            "hub_id": "local-01",
            "sequence_number": sequence_number,
        },
        "context": ctx,
        "acoustic": {
            "signal_level_dbfs": -50.0,
            "measurements": _empty_l0(),
            "signature": None,
            "classification": None,
        },
        "spatial": {"bearing": None, "position": None},
    }


def _ensure_node(db, node_id: str = "EAR-PAG") -> None:
    if db.get(Node, node_id) is None:
        db.add(
            Node(
                node_id=node_id,
                node_type="ear",
                hardware_revision="EAR-SIM-V1",
                provisioning_state="active",
                availability="OPERATIONAL",
            )
        )
        db.commit()


def _seed_reports(
    n: int,
    *,
    node_id: str = "EAR-PAG",
    observation_id: str | None = None,
    same_received_time: bool = False,
) -> str:
    oid = observation_id or str(uuid4())
    db = SessionLocal()
    try:
        _ensure_node(db, node_id)
        base = datetime(2026, 11, 5, 12, 0, 0, tzinfo=timezone.utc)
        shared_received = base + timedelta(hours=1)
        for i in range(n):
            event_time = base + timedelta(seconds=i)
            if i == 0:
                kind = "start"
            elif i == n - 1 and n > 1:
                kind = "end"
            else:
                kind = "periodic"
            body = ObservationBody.model_validate(
                _payload(
                    node_id,
                    observation_id=oid,
                    sequence_number=1000 + i,
                    event_time=event_time,
                    emission_kind=kind,
                )
            )
            assert ingest_observation(db, node_id, body) is True
            if same_received_time:
                db.execute(
                    text(
                        "UPDATE observations SET received_time_utc = :ts "
                        "WHERE node_id = :node AND sequence_number = :seq"
                    ),
                    {
                        "ts": shared_received.replace(tzinfo=None).isoformat(sep=" "),
                        "node": node_id,
                        "seq": 1000 + i,
                    },
                )
                db.commit()
        return oid
    finally:
        db.close()


@pytest.fixture(autouse=True)
def _clean_observations():
    db = SessionLocal()
    try:
        db.execute(text("DELETE FROM observations"))
        db.execute(text("DELETE FROM nodes WHERE node_id = 'EAR-PAG'"))
        db.commit()
    finally:
        db.close()
    yield


def test_default_page_size_is_200() -> None:
    assert DEFAULT_OBSERVATION_PAGE_SIZE == 200
    oid = _seed_reports(5)
    db = SessionLocal()
    try:
        result = list_observations_page(db, "EAR-PAG", observation_id=oid)
        assert result.page_size == 200
        assert result.page == 1
        assert result.total == 5
        assert result.total_pages == 1
        assert len(result.items) == 5
    finally:
        db.close()


def test_max_page_size_500_and_reject_above() -> None:
    assert MAX_OBSERVATION_PAGE_SIZE == 500
    oid = _seed_reports(3)
    db = SessionLocal()
    try:
        ok = list_observations_page(
            db, "EAR-PAG", observation_id=oid, page_size=500
        )
        assert ok.page_size == 500
        with pytest.raises(ValueError, match="page_size"):
            list_observations_page(
                db, "EAR-PAG", observation_id=oid, page_size=501
            )
    finally:
        db.close()


def test_total_and_page_counts_first_middle_last() -> None:
    oid = _seed_reports(7)
    db = SessionLocal()
    try:
        first = list_observations_page(
            db, "EAR-PAG", observation_id=oid, page=1, page_size=3, order="newest"
        )
        assert first.total == 7
        assert first.total_pages == 3
        assert len(first.items) == 3
        middle = list_observations_page(
            db,
            "EAR-PAG",
            observation_id=oid,
            page=2,
            page_size=3,
            order="newest",
            snapshot_id=first.snapshot_id,
        )
        assert len(middle.items) == 3
        last = list_observations_page(
            db,
            "EAR-PAG",
            observation_id=oid,
            page=3,
            page_size=3,
            order="newest",
            snapshot_id=first.snapshot_id,
        )
        assert len(last.items) == 1
        ids = [
            item.identity.sequence_number
            for page in (first, middle, last)
            for item in page.items
        ]
        assert len(ids) == len(set(ids)) == 7
    finally:
        db.close()


def test_oldest_first_ordering() -> None:
    oid = _seed_reports(4)
    db = SessionLocal()
    try:
        page = list_observations_page(
            db, "EAR-PAG", observation_id=oid, page_size=10, order="oldest"
        )
        seqs = [item.identity.sequence_number for item in page.items]
        assert seqs == sorted(seqs)
        assert page.order == "oldest"
    finally:
        db.close()


def test_deterministic_tiebreak_for_equal_timestamps() -> None:
    oid = _seed_reports(5, same_received_time=True)
    db = SessionLocal()
    try:
        newest = list_observations_page(
            db, "EAR-PAG", observation_id=oid, page_size=5, order="newest"
        )
        oldest = list_observations_page(
            db, "EAR-PAG", observation_id=oid, page_size=5, order="oldest"
        )
        newest_seqs = [i.identity.sequence_number for i in newest.items]
        oldest_seqs = [i.identity.sequence_number for i in oldest.items]
        assert newest_seqs == list(reversed(oldest_seqs))
        assert len(set(newest_seqs)) == 5
    finally:
        db.close()


def test_empty_result() -> None:
    db = SessionLocal()
    try:
        result = list_observations_page(db, "EAR-PAG", observation_id=str(uuid4()))
        assert result.total == 0
        assert result.total_pages == 0
        assert result.items == []
        assert result.snapshot_id is None
    finally:
        db.close()


def test_http_envelope_and_legacy_limit_alias() -> None:
    oid = _seed_reports(3)
    client = TestClient(create_app())
    response = client.get(
        f"/api/v1/observations?observation_id={oid}&page_size=2&page=1",
        headers={"Authorization": "Bearer test-admin"},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["pagination"]["total_count"] == 3
    assert body["pagination"]["page"] == 1
    assert body["pagination"]["page_size"] == 2
    assert body["pagination"]["total_pages"] == 2
    assert body["pagination"]["order"] == "newest"
    assert body["pagination"]["snapshot_id"] is not None
    assert len(body["reports"]) == 2
    assert "identity" in body["reports"][0]
    assert "acoustic" in body["reports"][0]

    legacy = client.get(
        f"/api/v1/observations?observation_id={oid}&limit=2",
        headers={"Authorization": "Bearer test-admin"},
    )
    assert legacy.status_code == 200
    assert legacy.json()["pagination"]["page_size"] == 2
    assert len(legacy.json()["reports"]) == 2


def test_http_invalid_page_size() -> None:
    client = TestClient(create_app())
    response = client.get(
        "/api/v1/observations?page_size=501",
        headers={"Authorization": "Bearer test-admin"},
    )
    assert response.status_code == 422


def test_snapshot_prevents_duplicates_across_pages_when_inserts_arrive() -> None:
    oid = _seed_reports(4)
    db = SessionLocal()
    try:
        first = list_observations_page(
            db, "EAR-PAG", observation_id=oid, page=1, page_size=2, order="newest"
        )
        snapshot = first.snapshot_id
        assert snapshot is not None
        body = ObservationBody.model_validate(
            _payload(
                "EAR-PAG",
                observation_id=oid,
                sequence_number=9999,
                event_time=datetime.now(timezone.utc),
            )
        )
        assert ingest_observation(db, "EAR-PAG", body) is True
        second = list_observations_page(
            db,
            "EAR-PAG",
            observation_id=oid,
            page=2,
            page_size=2,
            order="newest",
            snapshot_id=snapshot,
        )
        first_seqs = {i.identity.sequence_number for i in first.items}
        second_seqs = {i.identity.sequence_number for i in second.items}
        assert first_seqs.isdisjoint(second_seqs)
        assert 9999 not in first_seqs | second_seqs
        assert first.total == 4
        assert second.total == 4
    finally:
        db.close()


def test_out_of_range_page_returns_empty_reports_with_metadata() -> None:
    oid = _seed_reports(3)
    client = TestClient(create_app())
    response = client.get(
        f"/api/v1/observations?observation_id={oid}&page=9&page_size=2",
        headers={"Authorization": "Bearer test-admin"},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["reports"] == []
    assert body["pagination"]["total_count"] == 3
    assert body["pagination"]["total_pages"] == 2
    assert body["pagination"]["page"] == 9
