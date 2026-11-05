"""Hub Observation v1 — optional acoustic.dynamics (L2 contract version 1)."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

import pytest
from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-obs-l2-dynamics-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import Base, Node, Observation
from app.mqtt_listener import _ingest_observation
from app.schemas import ObservationBody
from app.service import ingest_observation, list_observations
from fastapi.testclient import TestClient
from pydantic import ValidationError


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


def _empty_l1() -> dict:
    null_stats = {"mean": None, "stdev": None}
    return {
        "schema": "l1",
        "version": 1,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "fft_size": 1024,
        "freq_min_hz": 80.0,
        "freq_max_hz": 7600.0,
        "window_count": 0,
        "invalid_window_count": 0,
        "centroid_hz": dict(null_stats),
        "bandwidth_hz": dict(null_stats),
        "dominant_hz": dict(null_stats),
        "flatness": dict(null_stats),
    }


def _cleared_feature() -> dict:
    return {"valid_count": 0, "mean": None, "stdev": None}


def _valid_dynamics(**overrides) -> dict:
    base = {
        "schema": "l2",
        "version": 1,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "fft_size": 1024,
        "freq_min_hz": 80.0,
        "freq_max_hz": 7600.0,
        "frame_hop_samples": 1024,
        "window_count": 5,
        "invalid_window_count": 0,
        "history_overflow": False,
        "spectral_flux": {"valid_count": 5, "mean": 0.042, "stdev": 0.011},
        "envelope_cv": {"valid_count": 5, "mean": 0.18, "stdev": 0.05},
        "dominant_delta_hz": {"valid_count": 5, "mean": 15.625, "stdev": 0.0},
    }
    base.update(overrides)
    return base


def _overflow_dynamics() -> dict:
    return _valid_dynamics(
        window_count=0,
        invalid_window_count=0,
        history_overflow=True,
        spectral_flux=_cleared_feature(),
        envelope_cv=_cleared_feature(),
        dominant_delta_hz=_cleared_feature(),
    )


def _hierarchical(
    node_id: str = "EAR-001",
    *,
    sequence_number: int = 1,
    kind: str = "periodic",
    dynamics: dict | None = None,
    include_dynamics_key: bool = True,
    classification: dict | None = None,
) -> dict:
    context = {
        "event_time_utc": datetime.now(timezone.utc).isoformat(),
        "capture_timestamp_monotonic_us": 123456,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": 5000.0,
        "episode_duration_ms": 5000.0,
        "emission_kind": kind,
    }
    if kind in ("end", "complete"):
        context["termination_reason"] = "natural"
    acoustic: dict = {
        "signal_level_dbfs": -34.0,
        "measurements": _empty_l0(),
        "signature": _empty_l1(),
        "classification": classification,
    }
    if include_dynamics_key:
        acoustic["dynamics"] = dynamics
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": {
            "observation_id": str(uuid4()),
            "node_id": node_id,
            "hub_id": "",
            "sequence_number": sequence_number,
        },
        "context": context,
        "acoustic": acoustic,
        "spatial": {"bearing": None, "position": None},
    }


def _activate_ear(node_id: str) -> None:
    with SessionLocal() as db:
        node = db.get(Node, node_id)
        assert node is not None
        node.provisioning_state = "active"
        node.lifecycle_state = "active"
        db.commit()


def test_payload_without_dynamics_key_accepted() -> None:
    payload = _hierarchical(include_dynamics_key=False)
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.dynamics is None
    assert body.acoustic.signature is not None
    assert body.acoustic.signature.signature_schema == "l1"


def test_explicit_dynamics_null_accepted() -> None:
    payload = _hierarchical(dynamics=None)
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.dynamics is None


def test_valid_l2_version_1_accepted() -> None:
    payload = _hierarchical(dynamics=_valid_dynamics())
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.dynamics is not None
    assert body.acoustic.dynamics.dynamics_schema == "l2"
    assert body.acoustic.dynamics.version == 1
    assert body.acoustic.dynamics.spectral_flux.mean == pytest.approx(0.042)
    assert body.acoustic.dynamics.dominant_delta_hz.valid_count == 5


def test_schema_other_than_l2_rejected() -> None:
    bad = _valid_dynamics()
    bad["schema"] = "l1"
    payload = _hierarchical(dynamics=bad)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_version_0_rejected() -> None:
    bad = _valid_dynamics(version=0)
    payload = _hierarchical(dynamics=bad)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_version_2_rejected() -> None:
    bad = _valid_dynamics(version=2)
    payload = _hierarchical(dynamics=bad)
    with pytest.raises(ValidationError) as exc_info:
        ObservationBody.model_validate(payload)
    assert "version" in str(exc_info.value).lower() or "1" in str(exc_info.value)


def test_missing_required_fields_rejected() -> None:
    for drop_key in (
        "history_overflow",
        "frame_hop_samples",
        "spectral_flux",
        "envelope_cv",
        "dominant_delta_hz",
    ):
        bad = _valid_dynamics()
        del bad[drop_key]
        payload = _hierarchical(dynamics=bad)
        with pytest.raises(ValidationError):
            ObservationBody.model_validate(payload)


def test_incorrect_types_and_ranges_rejected() -> None:
    cases = [
        _valid_dynamics(window_count=-1),
        _valid_dynamics(sample_rate_hz=0),
        _valid_dynamics(
            spectral_flux={"valid_count": 1, "mean": -0.1, "stdev": None}
        ),
        _valid_dynamics(
            envelope_cv={"valid_count": 2, "mean": 0.1, "stdev": float("nan")}
        ),
        _valid_dynamics(
            dominant_delta_hz={"valid_count": 0, "mean": 1.0, "stdev": None}
        ),
        _valid_dynamics(
            spectral_flux={"valid_count": 2, "mean": 0.1, "stdev": None}
        ),
    ]
    for bad in cases:
        payload = _hierarchical(dynamics=bad)
        with pytest.raises(ValidationError):
            ObservationBody.model_validate(payload)


def test_overflow_cleared_metrics_accepted() -> None:
    payload = _hierarchical(dynamics=_overflow_dynamics())
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.dynamics is not None
    assert body.acoustic.dynamics.history_overflow is True
    assert body.acoustic.dynamics.window_count == 0
    assert body.acoustic.dynamics.invalid_window_count == 0
    assert body.acoustic.dynamics.spectral_flux.valid_count == 0
    assert body.acoustic.dynamics.spectral_flux.mean is None


def test_overflow_nonzero_counts_rejected() -> None:
    bad = _overflow_dynamics()
    bad["window_count"] = 3
    payload = _hierarchical(dynamics=bad)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)

    bad2 = _overflow_dynamics()
    bad2["invalid_window_count"] = 1
    payload2 = _hierarchical(dynamics=bad2)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload2)


def test_overflow_populated_features_rejected() -> None:
    bad = _overflow_dynamics()
    bad["spectral_flux"] = {"valid_count": 1, "mean": 0.01, "stdev": None}
    payload = _hierarchical(dynamics=bad)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_classification_and_dynamics_together() -> None:
    classification = {
        "schema": "l2",
        "version": 1,
        "source_family": "vehicle",
        "confidence": 0.72,
        "hints": {"simulated_source_id": "SIM-VEHICLE-001"},
    }
    payload = _hierarchical(
        dynamics=_valid_dynamics(),
        classification=classification,
    )
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.dynamics is not None
    assert body.acoustic.dynamics.version == 1
    assert body.acoustic.classification is not None
    assert body.acoustic.classification.source_family == "vehicle"
    assert body.acoustic.classification.classification_schema == "l2"


def test_unknown_fields_rejected() -> None:
    bad = _valid_dynamics()
    bad["tonality"] = 0.5
    payload = _hierarchical(dynamics=bad)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)

    payload2 = _hierarchical(dynamics=_valid_dynamics())
    payload2["acoustic"]["extra_acoustic"] = True
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload2)


def test_empty_interval_not_confused_with_overflow() -> None:
    empty = _valid_dynamics(
        window_count=0,
        invalid_window_count=0,
        history_overflow=False,
        spectral_flux=_cleared_feature(),
        envelope_cv=_cleared_feature(),
        dominant_delta_hz=_cleared_feature(),
    )
    body = ObservationBody.model_validate(_hierarchical(dynamics=empty))
    assert body.acoustic.dynamics is not None
    assert body.acoustic.dynamics.history_overflow is False

    overflow = ObservationBody.model_validate(
        _hierarchical(dynamics=_overflow_dynamics())
    )
    assert overflow.acoustic.dynamics is not None
    assert overflow.acoustic.dynamics.history_overflow is True


def test_mqtt_ingest_and_persistence_round_trip() -> None:
    from sqlalchemy import select

    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear", "hardware_revision": "esp32-s3-n16r8"},
        headers=headers,
    )
    assert created.status_code == 200
    node_id = created.json()["node_id"]
    _activate_ear(node_id)

    payload = _hierarchical(
        node_id=node_id,
        sequence_number=42,
        dynamics=_valid_dynamics(),
    )
    _ingest_observation(node_id, json.dumps(payload).encode())

    with SessionLocal() as db:
        stored = db.scalars(
            select(Observation).where(Observation.node_id == node_id)
        ).all()
        assert len(stored) == 1
        stored_payload = json.loads(stored[0].payload_json)
        assert stored_payload["acoustic"]["dynamics"]["schema"] == "l2"
        assert stored_payload["acoustic"]["dynamics"]["version"] == 1
        assert stored_payload["acoustic"]["dynamics"]["spectral_flux"][
            "mean"
        ] == pytest.approx(0.042)

        listed = list_observations(db, node_id, limit=5)
        assert len(listed) == 1
        assert listed[0].acoustic.dynamics is not None
        assert listed[0].acoustic.dynamics.version == 1
        assert listed[0].identity.sequence_number == 42


def test_api_rehydration_response_round_trip() -> None:
    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear", "hardware_revision": "esp32-s3-n16r8"},
        headers=headers,
    )
    assert created.status_code == 200
    node_id = created.json()["node_id"]
    _activate_ear(node_id)

    payload = _hierarchical(
        node_id=node_id,
        sequence_number=7,
        dynamics=_overflow_dynamics(),
        classification={
            "schema": "l2",
            "version": 1,
            "source_family": "unknown",
            "confidence": 0.1,
            "hints": {},
        },
    )
    body = ObservationBody.model_validate(payload)
    with SessionLocal() as db:
        assert ingest_observation(db, node_id, body) is True

    listed = client.get(
        f"/api/v1/observations?node_id={node_id}&limit=10", headers=headers
    )
    assert listed.status_code == 200
    items = listed.json()["reports"]
    assert len(items) >= 1
    dynamics = items[0]["acoustic"]["dynamics"]
    assert dynamics["schema"] == "l2"
    assert dynamics["version"] == 1
    assert dynamics["history_overflow"] is True
    assert dynamics["window_count"] == 0
    assert dynamics["spectral_flux"]["mean"] is None
    assert items[0]["acoustic"]["classification"]["source_family"] == "unknown"
