"""Hub Observation v1 — optional track_id, assessment, evidence, debug, report_rotation."""

from __future__ import annotations

import json
from datetime import UTC, datetime
from pathlib import Path
from uuid import uuid4

import pytest
from app.config import settings

settings.database_url = "sqlite:///" + str(
    Path("/tmp/dbmap-obs-track-assessment-test.db")
)
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
from sqlalchemy import select


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


def _valid_assessment(**overrides) -> dict:
    base = {
        "schema": "ear_hyp_v1",
        "version": 1,
        "primary": {"family": "helicopter", "tier": "possible"},
        "alternates": [{"family": "airplane", "tier": "possible"}],
        "clutter": {"family": "hvac_or_fan", "tier": "possible"},
    }
    base.update(overrides)
    return base


def _valid_evidence(**overrides) -> dict:
    base = {
        "schema": "ear_ev_v1",
        "version": 1,
        "contrast_to_background_db": 8.5,
        "duration_ms_so_far": 45000,
        "path_votes": {
            "impulse": 0,
            "sustained": 1,
            "modulated": 0,
            "long_tone": 0,
        },
        "crest_max_db": 9.2,
        "flatness_mean": 0.21,
        "dominant_hz_mean": 180.0,
        "spectral_flux_mean": 0.4,
    }
    base.update(overrides)
    return base


def _valid_debug(**overrides) -> dict:
    base = {
        "schema": "ear_dbg_v1",
        "version": 1,
        "background_rms_dbfs": -60.6,
        "gate": "adaptive_contrast",
        "lifecycle_decision": "stay",
        "window_snapshots": [
            {
                "seq": 100,
                "rms": -51.2,
                "crest": 8.1,
                "flatness": 0.22,
                "flux": 0.35,
                "dom": 175.0,
            }
        ],
    }
    base.update(overrides)
    return base


def _hierarchical(
    node_id: str = "EAR-TRACK-001",
    *,
    sequence_number: int = 1,
    kind: str = "periodic",
    termination_reason: str | None = None,
    track_id: str | None = None,
    include_track_id_key: bool = False,
    assessment: dict | None = None,
    include_assessment_key: bool = False,
    evidence: dict | None = None,
    include_evidence_key: bool = False,
    debug: dict | None = None,
    include_debug_key: bool = False,
    observation_id: str | None = None,
) -> dict:
    context = {
        "event_time_utc": datetime.now(UTC).isoformat(),
        "capture_timestamp_monotonic_us": 123456,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": 5000.0,
        "episode_duration_ms": 5000.0,
        "emission_kind": kind,
    }
    if kind in ("end", "complete"):
        context["termination_reason"] = (
            termination_reason if termination_reason is not None else "natural"
        )
    elif termination_reason is not None:
        context["termination_reason"] = termination_reason

    acoustic: dict = {
        "signal_level_dbfs": -34.0,
        "measurements": _empty_l0(),
        "signature": None,
        "classification": None,
    }
    if include_assessment_key:
        acoustic["assessment"] = assessment
    if include_evidence_key:
        acoustic["evidence"] = evidence
    if include_debug_key:
        acoustic["debug"] = debug

    identity: dict = {
        "observation_id": observation_id or str(uuid4()),
        "node_id": node_id,
        "hub_id": "",
        "sequence_number": sequence_number,
    }
    if include_track_id_key:
        identity["track_id"] = track_id

    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": identity,
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


def test_legacy_payload_without_new_fields_still_validates() -> None:
    payload = _hierarchical()
    body = ObservationBody.model_validate(payload)
    assert body.identity.track_id is None
    assert body.acoustic.assessment is None
    assert body.acoustic.evidence is None
    assert body.acoustic.debug is None


def test_track_id_optional_null_and_uuid_accepted() -> None:
    track = str(uuid4())
    payload = _hierarchical(include_track_id_key=True, track_id=track)
    body = ObservationBody.model_validate(payload)
    assert str(body.identity.track_id) == track

    payload_null = _hierarchical(include_track_id_key=True, track_id=None)
    body_null = ObservationBody.model_validate(payload_null)
    assert body_null.identity.track_id is None


def test_assessment_ear_hyp_v1_accepted() -> None:
    payload = _hierarchical(
        include_assessment_key=True, assessment=_valid_assessment()
    )
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.assessment is not None
    assert body.acoustic.assessment.assessment_schema == "ear_hyp_v1"
    assert body.acoustic.assessment.version == 1
    assert body.acoustic.assessment.primary.family == "helicopter"
    assert body.acoustic.assessment.primary.tier == "possible"
    assert len(body.acoustic.assessment.alternates) == 1
    assert body.acoustic.assessment.clutter is not None
    assert body.acoustic.assessment.clutter.family == "hvac_or_fan"


def test_compact_evidence_ear_ev_v1_accepted() -> None:
    payload = _hierarchical(include_evidence_key=True, evidence=_valid_evidence())
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.evidence is not None
    assert body.acoustic.evidence.evidence_schema == "ear_ev_v1"
    assert body.acoustic.evidence.path_votes is not None
    assert body.acoustic.evidence.path_votes.sustained == 1
    assert body.acoustic.evidence.duration_ms_so_far == 45000


def test_debug_ear_dbg_v1_accepted() -> None:
    payload = _hierarchical(include_debug_key=True, debug=_valid_debug())
    body = ObservationBody.model_validate(payload)
    assert body.acoustic.debug is not None
    assert body.acoustic.debug.debug_schema == "ear_dbg_v1"
    assert body.acoustic.debug.gate == "adaptive_contrast"
    assert len(body.acoustic.debug.window_snapshots) == 1
    assert body.acoustic.debug.window_snapshots[0].seq == 100


def test_invalid_track_id_rejected() -> None:
    payload = _hierarchical(include_track_id_key=True, track_id="not-a-uuid")
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_invalid_schema_identifiers_rejected() -> None:
    bad_assessment = _valid_assessment()
    bad_assessment["schema"] = "l2"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_assessment_key=True, assessment=bad_assessment)
        )

    bad_evidence = _valid_evidence()
    bad_evidence["schema"] = "ear_hyp_v1"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_evidence_key=True, evidence=bad_evidence)
        )

    bad_debug = _valid_debug()
    bad_debug["schema"] = "ear_dbg_v2"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_debug_key=True, debug=bad_debug)
        )


def test_unknown_fields_in_new_nested_models_rejected() -> None:
    bad_assessment = _valid_assessment()
    bad_assessment["confidence"] = 0.9
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_assessment_key=True, assessment=bad_assessment)
        )

    bad_primary = _valid_assessment()
    bad_primary["primary"] = {
        "family": "drone",
        "tier": "possible",
        "score": 0.5,
    }
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_assessment_key=True, assessment=bad_primary)
        )

    bad_evidence = _valid_evidence()
    bad_evidence["extra"] = True
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_evidence_key=True, evidence=bad_evidence)
        )

    bad_debug = _valid_debug()
    bad_debug["window_snapshots"][0]["mystery"] = 1
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_debug_key=True, debug=bad_debug)
        )

    payload = _hierarchical(
        include_assessment_key=True, assessment=_valid_assessment()
    )
    payload["acoustic"]["assessment_extra"] = True
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_report_rotation_and_existing_termination_reasons_accepted() -> None:
    for reason in ("natural", "invalid_hold", "ota_reboot", "report_rotation"):
        payload = _hierarchical(kind="end", termination_reason=reason)
        body = ObservationBody.model_validate(payload)
        assert body.context.termination_reason == reason


def test_report_rotation_rejected_on_non_end() -> None:
    payload = _hierarchical(kind="periodic", termination_reason="report_rotation")
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_invalid_hypothesis_tier_rejected() -> None:
    bad = _valid_assessment()
    bad["primary"] = {"family": "drone", "tier": "certain"}
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(
            _hierarchical(include_assessment_key=True, assessment=bad)
        )


def test_new_fields_survive_persistence_and_api_retrieval() -> None:
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear"},
        headers=headers,
    )
    assert created.status_code == 200
    node_id = created.json()["node_id"]
    _activate_ear(node_id)

    track = str(uuid4())
    observation_id = str(uuid4())
    payload = _hierarchical(
        node_id=node_id,
        kind="end",
        termination_reason="report_rotation",
        sequence_number=42,
        observation_id=observation_id,
        include_track_id_key=True,
        track_id=track,
        include_assessment_key=True,
        assessment=_valid_assessment(),
        include_evidence_key=True,
        evidence=_valid_evidence(),
        include_debug_key=True,
        debug=_valid_debug(),
    )
    body = ObservationBody.model_validate(payload)
    with SessionLocal() as db:
        assert ingest_observation(db, node_id, body) is True

    listed = client.get(
        f"/api/v1/observations?node_id={node_id}&observation_id={observation_id}",
        headers=headers,
    )
    assert listed.status_code == 200
    reports = listed.json()["reports"]
    assert len(reports) == 1
    report = reports[0]
    assert report["identity"]["track_id"] == track
    assert report["context"]["termination_reason"] == "report_rotation"
    assert report["acoustic"]["assessment"]["schema"] == "ear_hyp_v1"
    assert report["acoustic"]["assessment"]["primary"]["family"] == "helicopter"
    assert report["acoustic"]["evidence"]["schema"] == "ear_ev_v1"
    assert report["acoustic"]["evidence"]["path_votes"]["sustained"] == 1
    assert report["acoustic"]["debug"]["schema"] == "ear_dbg_v1"
    assert report["acoustic"]["debug"]["window_snapshots"][0]["seq"] == 100

    with SessionLocal() as db:
        row = db.scalars(
            select(Observation).where(
                Observation.node_id == node_id,
                Observation.sequence_number == 42,
            )
        ).first()
        assert row is not None
        stored = json.loads(row.payload_json)
        assert stored["identity"]["track_id"] == track
        assert stored["acoustic"]["assessment"]["schema"] == "ear_hyp_v1"


def test_deduplication_unchanged_with_track_id() -> None:
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear"},
        headers=headers,
    )
    node_id = created.json()["node_id"]
    _activate_ear(node_id)

    observation_id = str(uuid4())
    track = str(uuid4())
    payload = _hierarchical(
        node_id=node_id,
        sequence_number=7,
        observation_id=observation_id,
        include_track_id_key=True,
        track_id=track,
    )
    body = ObservationBody.model_validate(payload)
    with SessionLocal() as db:
        assert ingest_observation(db, node_id, body) is True
        assert ingest_observation(db, node_id, body) is False
        rows = list_observations(db, node_id, limit=10)
        assert len(rows) == 1
        assert str(rows[0].identity.observation_id) == observation_id
        assert str(rows[0].identity.track_id) == track


def test_mqtt_ingest_accepts_extended_payload() -> None:
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear"},
        headers=headers,
    )
    node_id = created.json()["node_id"]
    _activate_ear(node_id)
    track = str(uuid4())
    payload = _hierarchical(
        node_id=node_id,
        sequence_number=99,
        include_track_id_key=True,
        track_id=track,
        include_assessment_key=True,
        assessment=_valid_assessment(alternates=[], clutter=None),
    )
    raw = json.dumps(payload).encode("utf-8")
    _ingest_observation(node_id, raw)
    with SessionLocal() as db:
        rows = list_observations(db, node_id, limit=5)
        assert any(r.identity.sequence_number == 99 for r in rows)
        match = next(r for r in rows if r.identity.sequence_number == 99)
        assert str(match.identity.track_id) == track
        assert match.acoustic.assessment is not None
        assert match.acoustic.assessment.alternates == []
        assert match.acoustic.assessment.clutter is None


def test_old_stored_payload_without_new_keys_still_lists() -> None:
    """Backward compatibility: payload_json minted before Hub knew track_id/assessment."""
    client = TestClient(create_app())
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear"},
        headers=headers,
    )
    node_id = created.json()["node_id"]
    _activate_ear(node_id)

    observation_id = str(uuid4())
    legacy = _hierarchical(
        node_id=node_id,
        sequence_number=3,
        observation_id=observation_id,
    )
    # Simulate older Hub dump that never included the new keys.
    with SessionLocal() as db:
        db.add(
            Observation(
                observation_id=observation_id,
                node_id=node_id,
                sequence_number=3,
                event_time_utc=datetime.now(UTC),
                payload_json=json.dumps(legacy),
            )
        )
        db.commit()

    listed = client.get(
        f"/api/v1/observations?node_id={node_id}&observation_id={observation_id}",
        headers=headers,
    )
    assert listed.status_code == 200
    report = listed.json()["reports"][0]
    assert report["identity"]["observation_id"] == observation_id
    assert report["identity"].get("track_id") is None
    assert report["acoustic"].get("assessment") is None
    assert report["acoustic"].get("evidence") is None
    assert report["acoustic"].get("debug") is None
