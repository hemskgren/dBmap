import json
import threading
from datetime import datetime, timedelta, timezone
from io import BytesIO
from pathlib import Path
from uuid import uuid4

import pytest
from app.config import settings

settings.database_url = "sqlite:///" + str(
    Path("/tmp/dbmap-bootstrap-weather-test.db")
)
settings.admin_token = "test-admin-token-weather-0123456789ab"
settings.viewer_token = "test-viewer-token-weather-0123456789ab"
settings.mqtt_enabled = False
settings.mqtt_host = "127.0.0.1"
settings.weather_enabled = True
settings.weather_region_id = "local-01"
settings.weather_region_latitude = 59.345879
settings.weather_region_longitude = 18.060589
settings.weather_cache_ttl_s = 900.0
settings.weather_live_max_age_hours = 6.0
settings.weather_forecast_base_url = "https://forecast.test/v1/forecast"
settings.weather_archive_base_url = "https://archive.test/v1/archive"
settings.hub_monitor_enabled = False

from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import Base, Node, RegionalEnvironmentalContext
from app import policy, regional_environment
from app.schemas import (
    ObservationBearing,
    ObservationBody,
    ObservationClassification,
)
from app.service import ingest_observation
from fastapi.testclient import TestClient
from sqlalchemy import delete


def setup_module() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


@pytest.fixture(autouse=True)
def _reset_weather_state(monkeypatch):
    regional_environment.stop_weather_worker()
    with regional_environment._inflight_lock:
        regional_environment._inflight.clear()
    while not regional_environment._queue.empty():
        try:
            regional_environment._queue.get_nowait()
            regional_environment._queue.task_done()
        except Exception:
            break
    db = SessionLocal()
    try:
        db.execute(delete(RegionalEnvironmentalContext))
        db.commit()
    finally:
        db.close()
    yield
    regional_environment.stop_weather_worker()


def _hourly_payload(hour: datetime, *, temperature: float = 12.5) -> dict:
    stamp = hour.strftime("%Y-%m-%dT%H:00")
    return {
        "hourly": {
            "time": [stamp],
            "temperature_2m": [temperature],
            "relative_humidity_2m": [55],
            "wind_speed_10m": [3.2],
            "wind_direction_10m": [180],
            "wind_gusts_10m": [5.1],
            "surface_pressure": [1012.0],
            "precipitation": [0.0],
        }
    }


class _FakeResponse:
    def __init__(self, payload: dict):
        self._payload = payload

    def read(self):
        return json.dumps(self._payload).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def _install_urlopen(monkeypatch, handler):
    monkeypatch.setattr(regional_environment, "_urlopen", handler)


def _make_ear(db, node_id: str) -> None:
    if db.get(Node, node_id) is not None:
        return
    db.add(
        Node(
            node_id=node_id,
            node_type="ear",
            hardware_revision="SIM-EAR-V1",
            provisioning_state="active",
            lifecycle_state="active",
            availability="OPERATIONAL",
        )
    )
    db.commit()


def _observation(node_id: str, event_time: datetime) -> ObservationBody:
    return ObservationBody(
        protocol_version=1,
        message_type="observation",
        observation_id=uuid4(),
        node_id=node_id,
        sequence_number=1,
        event_time_utc=event_time,
        capture_timestamp_monotonic_us=1,
        timing_quality="estimated",
        clock_offset_ms=None,
        timestamp_uncertainty_ms=10.0,
        classification=ObservationClassification(
            source_family="helicopter",
            confidence=0.7,
            hints={},
        ),
        bearing=ObservationBearing(deg=90.0, reference="node", confidence=0.5),
        duration_ms=800.0,
        signal_level_dbfs=-30.0,
    )


def test_first_observation_triggers_regional_retrieval(monkeypatch) -> None:
    calls = []

    def fake_urlopen(request, timeout=None):
        calls.append(request.full_url)
        hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
        return _FakeResponse(_hourly_payload(hour))

    _install_urlopen(monkeypatch, fake_urlopen)
    event_time = datetime.now(timezone.utc)
    db = SessionLocal()
    try:
        _make_ear(db, "SIM-EAR-001")
        assert ingest_observation(db, "SIM-EAR-001", _observation("SIM-EAR-001", event_time))
        row, from_cache = regional_environment.ensure_context(db, event_time)
        assert row is not None
        assert from_cache is False
        assert row.temperature_c == 12.5
        assert len(calls) == 1
        assert "forecast.test" in calls[0]
    finally:
        db.close()


def test_multiple_ears_share_same_regional_context(monkeypatch) -> None:
    hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)

    def fake_urlopen(request, timeout=None):
        return _FakeResponse(_hourly_payload(hour, temperature=9.0))

    _install_urlopen(monkeypatch, fake_urlopen)
    db = SessionLocal()
    try:
        _make_ear(db, "SIM-EAR-001")
        _make_ear(db, "SIM-EAR-002")
        _make_ear(db, "SIM-EAR-003")
        t1 = hour + timedelta(minutes=10)
        t2 = hour + timedelta(minutes=20)
        t3 = hour + timedelta(minutes=30)
        ingest_observation(db, "SIM-EAR-001", _observation("SIM-EAR-001", t1))
        ingest_observation(db, "SIM-EAR-002", _observation("SIM-EAR-002", t2))
        ingest_observation(db, "SIM-EAR-003", _observation("SIM-EAR-003", t3))
        first, _ = regional_environment.ensure_context(db, t1)
        second = regional_environment.find_covering_context(db, t2)
        third = regional_environment.find_covering_context(db, t3)
        assert first is not None and second is not None and third is not None
        assert first.context_id == second.context_id == third.context_id
    finally:
        db.close()


def test_subsequent_observations_reuse_cache(monkeypatch) -> None:
    calls = []
    hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)

    def fake_urlopen(request, timeout=None):
        calls.append(1)
        return _FakeResponse(_hourly_payload(hour))

    _install_urlopen(monkeypatch, fake_urlopen)
    db = SessionLocal()
    try:
        first, cached = regional_environment.ensure_context(db, hour + timedelta(minutes=5))
        second, cached_again = regional_environment.ensure_context(
            db, hour + timedelta(minutes=15)
        )
        assert first is not None and second is not None
        assert cached is False
        assert cached_again is True
        assert first.context_id == second.context_id
        assert len(calls) == 1
    finally:
        db.close()


def test_cache_expiration_triggers_refresh(monkeypatch) -> None:
    calls = []
    hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)

    def fake_urlopen(request, timeout=None):
        calls.append(request.full_url)
        return _FakeResponse(_hourly_payload(hour, temperature=len(calls)))

    _install_urlopen(monkeypatch, fake_urlopen)
    db = SessionLocal()
    try:
        first, _ = regional_environment.ensure_context(db, hour + timedelta(minutes=1))
        assert first is not None
        first.fetched_at = datetime.now(timezone.utc) - timedelta(seconds=901)
        db.commit()
        second, from_cache = regional_environment.ensure_context(
            db, hour + timedelta(minutes=2)
        )
        assert second is not None
        assert from_cache is False
        assert len(calls) == 2
        assert second.context_id != first.context_id
    finally:
        db.close()


def test_concurrent_requests_do_not_duplicate_retrievals(monkeypatch) -> None:
    calls = []
    started = threading.Event()
    release = threading.Event()
    hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)

    def fake_urlopen(request, timeout=None):
        calls.append(1)
        started.set()
        release.wait(timeout=2)
        return _FakeResponse(_hourly_payload(hour))

    _install_urlopen(monkeypatch, fake_urlopen)
    results: list[str | None] = []

    def worker():
        db = SessionLocal()
        try:
            row, _ = regional_environment.ensure_context(db, hour + timedelta(minutes=3))
            results.append(None if row is None else row.context_id)
        finally:
            db.close()

    first = threading.Thread(target=worker)
    second = threading.Thread(target=worker)
    first.start()
    assert started.wait(timeout=2)
    second.start()
    release.set()
    first.join(timeout=3)
    second.join(timeout=3)
    assert len(calls) == 1
    assert len([item for item in results if item]) >= 1


def test_provider_failure_does_not_block_observation_ingestion(monkeypatch) -> None:
    def fake_urlopen(request, timeout=None):
        raise TimeoutError("provider down")

    _install_urlopen(monkeypatch, fake_urlopen)
    event_time = datetime.now(timezone.utc)
    db = SessionLocal()
    try:
        _make_ear(db, "SIM-EAR-010")
        assert ingest_observation(
            db, "SIM-EAR-010", _observation("SIM-EAR-010", event_time)
        )
        regional_environment.schedule_weather_ensure(event_time)
        row, _ = regional_environment.ensure_context(db, event_time)
        assert row is None
        listed = regional_environment.get_context_for_analysis(db, event_time)
        assert listed["availability"] == "unavailable"
    finally:
        db.close()


def test_historical_replay_uses_archive_endpoint(monkeypatch) -> None:
    calls = []
    historical = datetime.now(timezone.utc) - timedelta(days=3)
    hour = historical.replace(minute=0, second=0, microsecond=0)

    def fake_urlopen(request, timeout=None):
        calls.append(request.full_url)
        return _FakeResponse(_hourly_payload(hour, temperature=-2.0))

    _install_urlopen(monkeypatch, fake_urlopen)
    db = SessionLocal()
    try:
        row, from_cache = regional_environment.ensure_context(db, historical)
        assert row is not None
        assert from_cache is False
        assert row.source_mode == "historical"
        assert len(calls) == 1
        assert "archive.test" in calls[0]
        assert "forecast.test" not in calls[0]
    finally:
        db.close()


def test_missing_historical_weather_does_not_use_current(monkeypatch) -> None:
    calls = []

    def fake_urlopen(request, timeout=None):
        calls.append(request.full_url)
        raise TimeoutError("archive unavailable")

    _install_urlopen(monkeypatch, fake_urlopen)
    historical = datetime.now(timezone.utc) - timedelta(days=10)
    db = SessionLocal()
    try:
        row, _ = regional_environment.ensure_context(db, historical)
        assert row is None
        assert all("archive.test" in url for url in calls)
        assert all("forecast.test" not in url for url in calls)
        payload = regional_environment.get_context_for_analysis(db, historical)
        assert payload["availability"] == "unavailable"
        assert payload["temperature_c"] is None
    finally:
        db.close()


def test_weather_context_does_not_alter_detection_fields(monkeypatch) -> None:
    hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)

    def fake_urlopen(request, timeout=None):
        return _FakeResponse(_hourly_payload(hour))

    _install_urlopen(monkeypatch, fake_urlopen)
    event_time = hour + timedelta(minutes=7)
    db = SessionLocal()
    try:
        _make_ear(db, "SIM-EAR-020")
        body = _observation("SIM-EAR-020", event_time)
        ingest_observation(db, "SIM-EAR-020", body)
        regional_environment.ensure_context(db, event_time)
        from app.service import list_observations

        views = list_observations(db, "SIM-EAR-020", limit=10)
        assert len(views) == 1
        assert views[0].classification is not None
        assert views[0].classification.confidence == 0.7
        assert views[0].bearing is not None
        assert views[0].bearing.confidence == 0.5
        assert not hasattr(views[0], "regional_environmental_context")
    finally:
        db.close()


def test_environmental_context_api_returns_snapshot(monkeypatch) -> None:
    hour = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
    admin = "test-admin-token-weather-0123456789ab"
    monkeypatch.setattr(settings, "admin_token", admin)
    monkeypatch.setattr(settings, "viewer_token", "test-viewer-token-weather-0123456789ab")

    def fake_urlopen(request, timeout=None):
        return _FakeResponse(_hourly_payload(hour, temperature=4.5))

    _install_urlopen(monkeypatch, fake_urlopen)
    monkeypatch.setattr(policy, "evaluate", lambda *args, **kwargs: True)
    app = create_app()
    client = TestClient(app)
    response = client.get(
        "/api/v1/hub/environmental-context",
        params={"at": hour.strftime("%Y-%m-%dT%H:%M:%SZ"), "ensure": "true"},
        headers={"Authorization": f"Bearer {admin}"},
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["availability"] == "ok"
    assert body["temperature_c"] == 4.5
    assert body["provider"] == "open-meteo"
    assert body["region_id"] == "local-01"
