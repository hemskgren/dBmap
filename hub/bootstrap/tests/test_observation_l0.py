"""Hub Observation v1 hierarchical validation and round-trip."""

from __future__ import annotations

from datetime import UTC, datetime
from pathlib import Path
from uuid import uuid4

import pytest
from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-obs-l0-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app.db import SessionLocal, engine, init_db
from app.main import create_app
from app.models import Base, Node
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
        "bands": {"mean_dbfs": [None, None, None, None], "fraction": [None, None, None, None]},
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


def _flat_legacy(node_id: str = "EAR-001") -> dict:
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": str(uuid4()),
        "node_id": node_id,
        "sequence_number": 1,
        "event_time_utc": datetime.now(UTC).isoformat(),
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


def _hierarchical(
    node_id: str = "EAR-001",
    *,
    kind: str = "periodic",
    termination_reason: str | None = None,
    measurements: dict | None = None,
    observation_id: str | None = None,
    sequence_number: int = 1,
) -> dict:
    if kind in ("end", "complete") and termination_reason is None:
        termination_reason = "natural"
    context: dict = {
        "event_time_utc": datetime.now(UTC).isoformat(),
        "capture_timestamp_monotonic_us": 123456,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": 5100.0,
        "episode_duration_ms": 5100.0,
        "emission_kind": kind,
    }
    if termination_reason is not None:
        context["termination_reason"] = termination_reason
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": {
            "observation_id": observation_id or str(uuid4()),
            "node_id": node_id,
            "hub_id": "",
            "sequence_number": sequence_number,
        },
        "context": context,
        "acoustic": {
            "signal_level_dbfs": -34.0,
            "measurements": measurements if measurements is not None else _empty_l0(),
            "signature": None,
            "classification": None,
        },
        "spatial": {"bearing": None, "position": None},
    }


def _extended(node_id: str = "EAR-001", kind: str = "periodic") -> dict:
    return _hierarchical(node_id, kind=kind, measurements=_measurements())


def test_flat_payload_rejected() -> None:
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(_flat_legacy())


def test_hierarchical_with_l0_validates() -> None:
    body = ObservationBody.model_validate(_extended())
    assert body.context.emission_kind == "periodic"
    assert body.acoustic.measurements is not None
    assert body.acoustic.measurements.measurement_schema == "l0"
    assert body.protocol_version == 1
    assert body.acoustic.signature is None


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


def _populated_l1() -> dict:
    return {
        "schema": "l1",
        "version": 1,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "fft_size": 1024,
        "freq_min_hz": 80.0,
        "freq_max_hz": 7600.0,
        "window_count": 5,
        "invalid_window_count": 1,
        "centroid_hz": {"mean": 1840.2, "stdev": 210.5},
        "bandwidth_hz": {"mean": 960.0, "stdev": 120.0},
        "dominant_hz": {"mean": 1720.0, "stdev": 85.0},
        "flatness": {"mean": 0.18, "stdev": 0.04},
    }


def test_l1_signature_null_populated_and_empty() -> None:
    null_body = ObservationBody.model_validate(_extended())
    assert null_body.acoustic.signature is None

    populated = _extended()
    populated["acoustic"]["signature"] = _populated_l1()
    body = ObservationBody.model_validate(populated)
    assert body.acoustic.signature is not None
    assert body.acoustic.signature.signature_schema == "l1"
    assert body.acoustic.signature.centroid_hz.mean == pytest.approx(1840.2)
    assert body.acoustic.signature.flatness.stdev == pytest.approx(0.04)

    empty = _extended()
    empty["acoustic"]["signature"] = _empty_l1()
    empty_body = ObservationBody.model_validate(empty)
    assert empty_body.acoustic.signature is not None
    assert empty_body.acoustic.signature.window_count == 0
    assert empty_body.acoustic.signature.centroid_hz.mean is None


def test_l1_signature_rejects_unknown_fields() -> None:
    payload = _extended()
    bad = _populated_l1()
    bad["rolloff_hz"] = {"mean": 3000.0, "stdev": 1.0}
    payload["acoustic"]["signature"] = bad
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_flat_only_emission_kind() -> None:
    payload = _flat_legacy()
    payload["emission_kind"] = "start"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_flat_only_measurements() -> None:
    payload = _flat_legacy()
    payload["measurements"] = _measurements()
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_invalid_emission_kind() -> None:
    payload = _extended()
    payload["context"]["emission_kind"] = "middle"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_end_without_termination_reason_rejected() -> None:
    payload = _extended(kind="end")
    payload["context"].pop("termination_reason", None)
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_end_with_termination_reasons_accepted() -> None:
    for reason in ("natural", "invalid_hold", "ota_reboot", "report_rotation"):
        payload = _extended(kind="end")
        payload["context"]["termination_reason"] = reason
        body = ObservationBody.model_validate(payload)
        assert body.context.termination_reason == reason


def test_complete_with_termination_reason_accepted() -> None:
    payload = _hierarchical(kind="complete", measurements=_measurements())
    body = ObservationBody.model_validate(payload)
    assert body.context.emission_kind == "complete"
    assert body.context.termination_reason == "natural"


def test_reject_termination_reason_on_non_end() -> None:
    for kind in ("start", "periodic"):
        payload = _extended(kind=kind)
        payload["context"]["termination_reason"] = "natural"
        with pytest.raises(ValidationError):
            ObservationBody.model_validate(payload)


def test_reject_unknown_termination_reason() -> None:
    payload = _extended(kind="end")
    payload["context"]["termination_reason"] = "power_loss"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_flat_termination_reason_without_hierarchy() -> None:
    payload = _flat_legacy()
    payload["termination_reason"] = "natural"
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_unknown_nested_measurement_field() -> None:
    payload = _extended()
    payload["acoustic"]["measurements"]["extra_field"] = 1
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_reject_malformed_bands_length() -> None:
    payload = _extended()
    payload["acoustic"]["measurements"]["bands"]["mean_dbfs"] = [-40.0]
    with pytest.raises(ValidationError):
        ObservationBody.model_validate(payload)


def test_null_metrics_preserved() -> None:
    payload = _extended(kind="start")
    payload["acoustic"]["measurements"] = _measurements(
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
    assert body.acoustic.measurements is not None
    assert body.acoustic.measurements.hl.mean_db is None
    assert body.acoustic.measurements.t2.rms_slope_db_per_s is None


def test_interleaved_same_observation_id_different_sequences() -> None:
    shared_id = str(uuid4())
    first = _hierarchical(
        sequence_number=10,
        observation_id=shared_id,
        kind="start",
        measurements=_empty_l0(),
    )
    second = _hierarchical(
        sequence_number=11,
        observation_id=shared_id,
        kind="periodic",
        measurements=_measurements(),
    )
    body_a = ObservationBody.model_validate(first)
    body_b = ObservationBody.model_validate(second)
    assert body_a.identity.observation_id == body_b.identity.observation_id
    assert body_a.identity.sequence_number != body_b.identity.sequence_number


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
        assert rows[0].context.emission_kind == "end"
        assert rows[0].acoustic.measurements is not None
        assert rows[0].acoustic.measurements.hl.mean_db == pytest.approx(-3.679768)
        dumped = rows[0].model_dump(mode="json", by_alias=True)
        assert dumped["acoustic"]["measurements"]["schema"] == "l0"

    listed = client.get(
        f"/api/v1/observations?node_id={node_id}&limit=10", headers=headers
    )
    assert listed.status_code == 200
    items = listed.json()["reports"]
    assert len(items) >= 1
    assert items[0]["protocol_version"] == 1
    assert items[0]["context"]["emission_kind"] == "end"
    assert items[0]["acoustic"]["measurements"]["schema"] == "l0"
    assert items[0]["acoustic"]["measurements"]["hl"]["mean_db"] == pytest.approx(
        -3.679768
    )


def test_simulator_hierarchical_payloads() -> None:
    """E2E-style: simulator fixtures accepted; sequence preserved; dedup by sequence."""
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

        baseline = make_observation(node_id, sequence_number=1)
        baseline["identity"]["node_id"] = node_id
        body = ObservationBody.model_validate(baseline)
        assert ingest_observation(db, node_id, body) is True

        for index, scenario in enumerate(SCENARIO_IDS, start=2):
            payload = make_observation(
                node_id,
                sequence_number=index,
                include_l0=True,
                l0_scenario=scenario,
            )
            payload["identity"]["node_id"] = node_id
            body = ObservationBody.model_validate(payload)
            assert ingest_observation(db, node_id, body) is True
            assert ingest_observation(db, node_id, body) is False

        rows = list_observations(db, node_id, limit=50)
        by_seq = {r.identity.sequence_number: r for r in rows}
        assert by_seq[1].acoustic.measurements.window_count == 0
        assert by_seq[2].context.emission_kind is not None
        assert by_seq[2].acoustic.measurements is not None
        listed = client.get(
            f"/api/v1/observations?node_id={node_id}&limit=50", headers=headers
        )
        assert listed.status_code == 200
        listed_reports = listed.json()["reports"]
        extended = [
            i
            for i in listed_reports
            if i.get("context", {}).get("emission_kind") == "periodic"
            or i.get("context", {}).get("emission_kind") == "start"
        ]
        assert len(extended) >= len(SCENARIO_IDS)
        assert all(i["protocol_version"] == 1 for i in listed_reports)
        assert any(
            i["acoustic"]["measurements"]["t2"]["rms_slope_db_per_s"] is None
            for i in extended
        )


def test_scenario_ear_lifecycle_observations_ingest() -> None:
    """Helicopter scripted points form per-Ear START/PERIODIC/END; Hub accepts them."""
    import sys
    from collections import defaultdict
    from datetime import datetime
    from importlib.util import module_from_spec, spec_from_file_location
    from pathlib import Path

    root = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(root / "simulator" / "ear"))
    spec = spec_from_file_location(
        "dbmap_ear_scenario_simulator_hub_test",
        root / "simulator" / "ear" / "simulate_scenario.py",
    )
    assert spec is not None and spec.loader is not None
    simulate_scenario = module_from_spec(spec)
    spec.loader.exec_module(simulate_scenario)

    scenario = simulate_scenario.load_scenario(
        root / "simulator" / "ear" / "scenarios" / "helicopter_east_west.json"
    )
    assert len(scenario["observations"]) == 18
    timed_events = simulate_scenario.assign_scripted_ear_observation_lifecycles(
        scenario["observations"]
    )

    app = create_app()
    client = TestClient(app)
    headers = {"Authorization": "Bearer test-admin"}
    created = {
        alias: client.post(
            "/api/v1/nodes",
            json={"node_type": "ear", "hardware_revision": "esp32-s3-n16r8"},
            headers=headers,
        ).json()["node_id"]
        for alias in ("ear_a", "ear_b", "ear_c")
    }
    with SessionLocal() as db:
        for node_id in created.values():
            node = db.get(Node, node_id)
            assert node is not None
            node.provisioning_state = "active"
            node.lifecycle_state = "active"
        db.commit()

        sequences = defaultdict(int)
        for event in timed_events:
            alias = event["ear"]
            node_id = created[alias]
            sequences[node_id] += 1
            payload = simulate_scenario.make_scenario_observation(
                node_id,
                sequences[node_id],
                scenario["source_id"],
                event,
                datetime(2026, 11, 3, 18, 0, tzinfo=UTC),
                0,
            )
            payload["identity"]["node_id"] = node_id
            body = ObservationBody.model_validate(payload)
            assert ingest_observation(db, node_id, body) is True
            assert ingest_observation(db, node_id, body) is False

        for alias, node_id in created.items():
            rows = sorted(
                list_observations(db, node_id, limit=50),
                key=lambda row: row.identity.sequence_number,
            )
            assert len(rows) == 6
            assert [row.context.emission_kind for row in rows] == [
                "start",
                "periodic",
                "periodic",
                "periodic",
                "periodic",
                "end",
            ]
            assert len({str(row.identity.observation_id) for row in rows}) == 1
            assert rows[-1].context.termination_reason == "natural"
            assert all(row.context.termination_reason is None for row in rows[:-1])
