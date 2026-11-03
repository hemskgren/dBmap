"""Hub Observation v1 + optional L0 extension validation and round-trip."""

from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

import pytest
from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-obs-l0-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from fastapi.testclient import TestClient
from pydantic import ValidationError

from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import Base, Node
from app.schemas import ObservationBody
from app.service import ingest_observation, list_observations


def setup_module() -> None:
    Base.metadata.drop_all(bind=engine)
    init_db()


def _legacy(node_id: str = "EAR-001") -> dict:
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": str(uuid4()),
        "node_id": node_id,
        "sequence_number": 1,
        "event_time_utc": datetime.now(timezone.utc).isoformat(),
        "capture_timestamp_monotonic_us": 123456,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "classification": None,
        "bearing": None,
        "signal_level_dbfs": -34.0,
        "duration_ms": 5100.0,
    }


def _measurements(**overrides) -> dict:
    base = {
        "schema": "l0",
        "version": 1,
        "band_plan_version": 1,
        "crest_threshold_version": 1,
        "crest_threshold_db": 12.0,
        "mic_index": 0,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "window_count": 5,
        "clipped_window_count": 0,
        "n_crest": 5,
        "rms": {"mean_dbfs": -42.5, "min_dbfs": -50.0, "max_dbfs": -35.0},
        # Consistent with mean linear E={4e-5,3e-5,2e-5,1e-5} (Ear finalize formulas).
        "bands": {
            "mean_dbfs": [-43.9794, -45.228787, -46.9897, -50.0],
            "fraction": [0.4, 0.3, 0.2, 0.1],
        },
        "hl": {"mean_db": -3.679768},
        "t1": {"rms_stdev_db": 2.0, "rms_range_db": 15.0},
        "t2": {"rms_slope_db_per_s": 0.25},
        "t3": {
            "crest_max_db": 14.0,
            "high_crest_count": 1,
            "high_crest_rate": 0.2,
        },
        "t6": {"hl_slope_db_per_s": -0.1},
    }
    base.update(overrides)
    return base


def _extended(node_id: str = "EAR-001", kind: str = "periodic") -> dict:
    payload = _legacy(node_id)
    payload["emission_kind"] = kind
    payload["measurements"] = _measurements()
    return payload


def test_legacy_v1_still_validates() -> None:
    ObservationBody.model_validate(_legacy())


def test_extended_v1_validates() -> None:
    body = ObservationBody.model_validate(_extended())
    assert body.emission_kind == "periodic"
    assert body.measurements is not None
    assert body.measurements.measurement_schema == "l0"
    assert body.protocol_version == 1


def test_reject_only_emission_kind() -> None:
    payload = _legacy()
    payload["emission_kind"] = "start"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_only_measurements() -> None:
    payload = _legacy()
    payload["measurements"] = _measurements()
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_invalid_emission_kind() -> None:
    payload = _extended()
    payload["emission_kind"] = "middle"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


# --- Newly added: termination_reason ---


def test_end_without_termination_reason_accepted() -> None:
    payload = _extended(kind="end")
    body = ObservationBody.model_validate(payload)
    assert body.termination_reason is None


def test_end_with_termination_reasons_accepted() -> None:
    for reason in ("natural", "invalid_hold", "ota_reboot"):
        payload = _extended(kind="end")
        payload["termination_reason"] = reason
        body = ObservationBody.model_validate(payload)
        assert body.termination_reason == reason


def test_reject_termination_reason_on_non_end() -> None:
    for kind in ("start", "periodic"):
        payload = _extended(kind=kind)
        payload["termination_reason"] = "natural"
        with pytest.raises(ValidationError):
            ObservationBody.model_validate(payload)


def test_reject_unknown_termination_reason() -> None:
    payload = _extended(kind="end")
    payload["termination_reason"] = "power_loss"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_termination_reason_without_extension_pair() -> None:
    payload = _legacy()
    payload["termination_reason"] = "natural"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_unknown_nested_measurement_field() -> None:
    payload = _extended()
    payload["measurements"]["extra_field"] = 1
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_malformed_bands_length() -> None:
    payload = _extended()
    payload["measurements"]["bands"]["mean_dbfs"] = [-40.0]
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_null_metrics_preserved() -> None:
    payload = _extended(kind="start")
    payload["measurements"] = _measurements(
        window_count=0,
        clipped_window_count=0,
        n_crest=0,
        rms={"mean_dbfs": None, "min_dbfs": None, "max_dbfs": None},
        bands={
            "mean_dbfs": [None, None, None, None],
            "fraction": [None, None, None, None],
        },
        hl={"mean_db": None},
        t1={"rms_stdev_db": None, "rms_range_db": None},
        t2={"rms_slope_db_per_s": None},
        t3={
            "crest_max_db": None,
            "high_crest_count": None,
            "high_crest_rate": None,
        },
        t6={"hl_slope_db_per_s": None},
    )
    body = ObservationBody.model_validate(payload)
    assert body.measurements is not None
    assert body.measurements.hl.mean_db is None
    assert body.measurements.t2.rms_slope_db_per_s is None


def test_ingest_and_api_round_trip_extended() -> None:
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

    with SessionLocal() as db:
        node = db.get(Node, node_id)
        assert node is not None
        node.provisioning_state = "active"
        node.lifecycle_state = "active"
        db.commit()

    payload = _extended(node_id=node_id, kind="end")
    body = ObservationBody.model_validate(payload)
    with SessionLocal() as db:
        assert ingest_observation(db, node_id, body) is True
        assert ingest_observation(db, node_id, body) is False
        rows = list_observations(db, node_id, limit=5)
        assert len(rows) == 1
        assert rows[0].emission_kind == "end"
        assert rows[0].measurements is not None
        assert rows[0].measurements.hl.mean_db == pytest.approx(-3.679768)
        dumped = rows[0].model_dump(mode="json", by_alias=True)
        assert dumped["measurements"]["schema"] == "l0"

    listed = client.get(
        f"/api/v1/observations?node_id={node_id}&limit=10", headers=headers
    )
    assert listed.status_code == 200
    items = listed.json()
    assert len(items) >= 1
    assert items[0]["protocol_version"] == 1
    assert items[0]["emission_kind"] == "end"
    # FastAPI serializes response models; alias should appear as schema
    assert items[0]["measurements"]["schema"] == "l0"
    assert items[0]["measurements"]["hl"]["mean_db"] == pytest.approx(-3.679768)


def test_simulator_extended_and_legacy_payloads() -> None:
    """E2E-style: simulator fixtures accepted; sequence preserved; dedup holds."""
    import sys
    from pathlib import Path

    root = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(root / "simulator" / "ear"))
    from l0_measurements import SCENARIO_IDS
    from simulate_observation import make_observation

    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear", "hardware_revision": "esp32-s3-n16r8"},
        headers=headers,
    )
    node_id = created.json()["node_id"]
    with SessionLocal() as db:
        node = db.get(Node, node_id)
        assert node is not None
        node.provisioning_state = "active"
        node.lifecycle_state = "active"
        db.commit()

        legacy = make_observation(node_id, sequence_number=1)
        legacy["node_id"] = node_id
        body = ObservationBody.model_validate(legacy)
        assert ingest_observation(db, node_id, body) is True

        for index, scenario in enumerate(SCENARIO_IDS, start=2):
            payload = make_observation(
                node_id,
                sequence_number=index,
                include_l0=True,
                l0_scenario=scenario,
            )
            payload["node_id"] = node_id
            body = ObservationBody.model_validate(payload)
            assert ingest_observation(db, node_id, body) is True
            # Dedup: same observation_id again
            assert ingest_observation(db, node_id, body) is False

        rows = list_observations(db, node_id, limit=50)
        by_seq = {r.sequence_number: r for r in rows}
        assert 1 in by_seq and by_seq[1].measurements is None
        assert by_seq[2].emission_kind is not None
        assert by_seq[2].measurements is not None
        # API round-trip preserves extension for one extended row
        listed = client.get(
            f"/api/v1/observations?node_id={node_id}&limit=50", headers=headers
        )
        assert listed.status_code == 200
        extended = [i for i in listed.json() if i.get("emission_kind")]
        assert len(extended) == len(SCENARIO_IDS)
        assert all(i["protocol_version"] == 1 for i in listed.json())
        assert any(
            i["measurements"]["t2"]["rms_slope_db_per_s"] is None for i in extended
        )


def test_legacy_ingest_still_works() -> None:
    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}
    created = client.post(
        "/api/v1/nodes",
        json={"node_type": "ear", "hardware_revision": "esp32-s3-n16r8"},
        headers=headers,
    )
    node_id = created.json()["node_id"]
    with SessionLocal() as db:
        node = db.get(Node, node_id)
        assert node is not None
        node.provisioning_state = "active"
        node.lifecycle_state = "active"
        db.commit()
        body = ObservationBody.model_validate(_legacy(node_id))
        assert ingest_observation(db, node_id, body) is True
        rows = list_observations(db, node_id, limit=1)
        assert rows[0].emission_kind is None
        assert rows[0].measurements is None
