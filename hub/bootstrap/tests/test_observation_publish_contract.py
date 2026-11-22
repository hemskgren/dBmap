"""Firmware-to-Hub Observation publish contract.

Reads the seven JSON documents produced by the host exporter and feeds the
original bytes through the production MQTT listener.
"""

from __future__ import annotations

import copy
import json
import os
from datetime import UTC, datetime
from pathlib import Path

import pytest
from app.config import settings

_DB_PATH = Path("/tmp/dbmap-obs-publish-contract.db")
settings.database_url = "sqlite:///" + str(_DB_PATH)
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

import app.regional_environment as regional
import paho.mqtt.client as mqtt
from app.db import SessionLocal, engine, init_db
from app.models import Base, Node, Observation
from app.mqtt_listener import _on_message
from app.service import list_observations_page
from app.topics import parse_observation_topic
from sqlalchemy import func, select

regional.schedule_weather_ensure = lambda *_args, **_kwargs: None

_CORPUS = os.environ.get("DBMAP_OBS_CONTRACT_DIR", "")


def setup_module() -> None:
    if _DB_PATH.exists():
        _DB_PATH.unlink()
    Base.metadata.drop_all(bind=engine)
    init_db()


@pytest.fixture(autouse=True)
def _suppress_weather(monkeypatch: pytest.MonkeyPatch) -> None:
    import app.regional_environment as regional

    monkeypatch.setattr(regional, "schedule_weather_ensure", lambda *_a, **_k: None)


def _corpus() -> Path:
    if not _CORPUS:
        pytest.fail("DBMAP_OBS_CONTRACT_DIR is not set")
    path = Path(_CORPUS)
    if not path.is_dir():
        pytest.fail(f"contract directory missing: {path}")
    return path


def _load_corpus() -> tuple[str, list[tuple[bytes, dict]]]:
    root = _corpus()
    topic_path = root / "topic.txt"
    if not topic_path.is_file():
        pytest.fail("topic.txt missing from contract export")
    topic = topic_path.read_text(encoding="utf-8").strip()
    paths = sorted(root.glob("report-*.json"))
    if len(paths) != 7:
        pytest.fail(f"expected 7 reports, found {len(paths)}")
    loaded: list[tuple[bytes, dict]] = []
    for path in paths:
        raw = path.read_bytes()
        loaded.append((raw, json.loads(raw)))
    return topic, loaded


def _message(topic: str, payload: bytes, *, retain: bool = False) -> mqtt.MQTTMessage:
    message = mqtt.MQTTMessage(topic=topic.encode())
    message.payload = payload
    message.retain = retain
    return message


def _publish(topic: str, payload: bytes, *, retain: bool = False) -> None:
    _on_message(None, None, _message(topic, payload, retain=retain))


def _count() -> int:
    with SessionLocal() as db:
        return int(db.scalar(select(func.count()).select_from(Observation)) or 0)


def _stored_by_sequence() -> dict[int, dict]:
    with SessionLocal() as db:
        rows = db.scalars(select(Observation).order_by(Observation.sequence_number)).all()
        return {int(row.sequence_number): json.loads(row.payload_json) for row in rows}


def _path_ids(doc: dict) -> list[str]:
    assessment = doc["acoustic"].get("assessment")
    if not assessment:
        return []
    return [node["id"] for node in assessment["path"]]


def _confidences(doc: dict) -> list[float]:
    assessment = doc["acoustic"].get("assessment")
    if not assessment:
        return []
    return [float(node["confidence"]) for node in assessment["path"]]


def _by_kind(docs: list[dict], observation_id: str) -> dict[str, dict]:
    return {
        doc["context"]["emission_kind"]: doc
        for doc in docs
        if doc["identity"]["observation_id"] == observation_id
    }


def assert_wire_contract(docs: list[dict]) -> None:
    """Fail when the seven production documents no longer match the contract."""
    assert len(docs) == 7
    sequences = [doc["identity"]["sequence_number"] for doc in docs]
    assert sequences == list(range(1, 8))
    assert docs[0]["protocol_version"] == 1
    assert all(doc["message_type"] == "observation" for doc in docs)

    normal = [doc for doc in docs if "origin" not in doc["context"]]
    critical = [doc for doc in docs if doc["context"].get("origin") == "critical_path"]
    assert len(normal) == 4
    assert len(critical) == 3
    assert [doc["context"]["emission_kind"] for doc in critical] == [
        "start",
        "periodic",
        "end",
    ]

    normal_starts = [doc for doc in normal if doc["context"]["emission_kind"] == "start"]
    normal_periodics = [
        doc for doc in normal if doc["context"]["emission_kind"] == "periodic"
    ]
    assert len(normal_starts) == 2
    assert len(normal_periodics) == 2
    assert _path_ids(normal_starts[0]) == ["AIRCRAFT"]
    assert _path_ids(normal_starts[1]) == ["ALARM"]
    as001_id = normal_starts[0]["identity"]["observation_id"]
    as002_id = normal_starts[1]["identity"]["observation_id"]
    crit_id = critical[0]["identity"]["observation_id"]
    assert len({as001_id, as002_id, crit_id}) == 3

    as001 = _by_kind(docs, as001_id)
    as002 = _by_kind(docs, as002_id)
    crit = _by_kind(docs, crit_id)
    assert set(as001) == {"start", "periodic"}
    assert set(as002) == {"start", "periodic"}
    assert _path_ids(as001["periodic"]) == ["AIRCRAFT", "Helicopter"]
    assert _path_ids(as002["periodic"]) == ["ALARM", "SE_PUBLIC_ALERT"]
    assert "ALARM" not in _path_ids(as001["start"])
    assert "ALARM" not in _path_ids(as001["periodic"])
    assert "AIRCRAFT" not in _path_ids(as002["start"])
    assert "Helicopter" not in _path_ids(as002["periodic"])

    for doc in normal:
        assessment = doc["acoustic"]["assessment"]
        assert assessment["schema"] == "ear_hyp_v2"
        assert assessment["version"] == 1
        confidences = _confidences(doc)
        assert confidences
        assert all(0.0 < value <= 1.0 for value in confidences)
    aircraft = _confidences(as001["periodic"])
    assert aircraft[0] + 1e-6 >= aircraft[1]

    for doc in critical:
        assert "assessment" not in doc["acoustic"]
        assert doc["context"]["origin"] == "critical_path"
        measurements = doc["acoustic"]["measurements"]
        assert measurements["schema"] == "l0"
        assert measurements["window_count"] == 0
        assert measurements["clipped_window_count"] == 0
        assert measurements["n_crest"] == 0
        assert measurements["sample_rate_hz"] > 0
        assert measurements["analysis_window_ms"] > 0
        assert measurements["rms"]["mean_dbfs"] is None
        assert measurements["rms"]["min_dbfs"] is None
        assert measurements["rms"]["max_dbfs"] is None
        assert doc["acoustic"]["signature"] is None
        assert doc["acoustic"]["dynamics"] is None
    assert crit["start"]["context"]["interval_duration_ms"] == 0
    assert crit["start"]["context"]["episode_duration_ms"] == 0
    assert "termination_reason" not in crit["start"]["context"]
    assert "termination_reason" not in crit["periodic"]["context"]
    assert crit["end"]["context"]["termination_reason"] == "natural"
    assert crit["end"]["identity"]["observation_id"] == crit_id
    assert crit["start"]["identity"]["sequence_number"] < crit["periodic"][
        "identity"
    ]["sequence_number"] < crit["end"]["identity"]["sequence_number"]

    scene_docs = [as001["periodic"], as002["periodic"]]
    left = scene_docs[0]["acoustic"]
    right = scene_docs[1]["acoustic"]
    assert left["measurements"] == right["measurements"]
    assert left["signature"] == right["signature"]
    assert left["dynamics"] == right["dynamics"]
    assert left["measurements"]["window_count"] > 0
    assert left["measurements"]["rms"]["mean_dbfs"] not in (None, 0)
    assert left["signature"]["schema"] == "l1"
    assert left["signature"]["window_count"] > 0
    assert left["signature"]["centroid_hz"]["mean"] not in (None, 0)
    assert left["dynamics"]["schema"] == "l2"
    assert left["dynamics"]["version"] == 1
    assert left["dynamics"]["window_count"] > 0
    assert left["dynamics"]["spectral_flux"]["mean"] not in (None, 0)
    assert left["dynamics"]["envelope_cv"]["mean"] not in (None, 0)
    assert left["dynamics"]["dominant_delta_hz"]["mean"] not in (None, 0)


def assert_stored_contract(wire: list[dict], stored: dict[int, dict], topic_node: str) -> None:
    assert set(stored) == set(range(1, 8))
    for doc in wire:
        seq = doc["identity"]["sequence_number"]
        row = stored[seq]
        assert row["identity"]["node_id"] == topic_node
        assert row["identity"]["observation_id"] == doc["identity"]["observation_id"]
        assert row["identity"]["sequence_number"] == seq
        assert row["identity"]["track_id"] is None
        assert row["context"]["emission_kind"] == doc["context"]["emission_kind"]
        assert row["context"]["origin"] == doc["context"].get("origin")
        wire_time = datetime.fromisoformat(doc["context"]["event_time_utc"])
        stored_time = datetime.fromisoformat(row["context"]["event_time_utc"])
        if stored_time.tzinfo is None:
            stored_time = stored_time.replace(tzinfo=UTC)
        assert stored_time.astimezone(UTC) == wire_time.astimezone(UTC)
        assert row["acoustic"]["evidence"] is None
        assert row["acoustic"]["debug"] is None
        assert row["acoustic"]["classification"] is None
        if "assessment" in doc["acoustic"]:
            assert row["acoustic"]["assessment"]["schema"] == "ear_hyp_v2"
            assert row["acoustic"]["assessment"]["version"] == 1
            assert _path_ids(row) == _path_ids(doc)
            for got, expect in zip(_confidences(row), _confidences(doc), strict=True):
                assert got == pytest.approx(expect)
        else:
            assert row["acoustic"]["assessment"] is None
        assert row["acoustic"]["measurements"]["window_count"] == doc["acoustic"][
            "measurements"
        ]["window_count"]
        assert row["context"]["termination_reason"] == doc["context"].get(
            "termination_reason"
        )


def _seed_active_ear(node_id: str) -> None:
    with SessionLocal() as db:
        existing = db.get(Node, node_id)
        if existing is None:
            db.add(
                Node(
                    node_id=node_id,
                    node_type="ear",
                    hardware_revision="EAR-SIM-V1",
                    provisioning_state="active",
                    lifecycle_state="active",
                    availability="OPERATIONAL",
                )
            )
        else:
            existing.node_type = "ear"
            existing.lifecycle_state = "active"
        db.commit()


def _set_lifecycle(node_id: str, state: str) -> None:
    with SessionLocal() as db:
        node = db.get(Node, node_id)
        assert node is not None
        node.lifecycle_state = state
        db.commit()


def _dumps(doc: dict) -> bytes:
    return json.dumps(doc, separators=(",", ":")).encode()


@pytest.fixture(scope="module", autouse=True)
def _ingest_exported_reports() -> None:
    topic, loaded = _load_corpus()
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    _seed_active_ear(node_id)
    if _count() == 0:
        for raw, _doc in loaded:
            _publish(topic, raw)


def test_seven_production_reports_round_trip() -> None:
    topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    assert_wire_contract(docs)
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    assert _count() == 7

    stored = _stored_by_sequence()
    assert_stored_contract(docs, stored, node_id)
    with SessionLocal() as db:
        page = list_observations_page(db, node_id, page=1, page_size=50, order="oldest")
    assert page.total == 7
    assert len(page.items) == 7
    listed_ids = {str(item.identity.observation_id) for item in page.items}
    assert listed_ids == {doc["identity"]["observation_id"] for doc in docs}


def test_assessment_swap_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    starts = [doc for doc in docs if doc["context"]["emission_kind"] == "start" and "origin" not in doc["context"]]
    starts[0]["acoustic"]["assessment"], starts[1]["acoustic"]["assessment"] = (
        starts[1]["acoustic"]["assessment"],
        starts[0]["acoustic"]["assessment"],
    )
    with pytest.raises(AssertionError, match="AIRCRAFT"):
        assert_wire_contract(docs)


def test_lost_assessment_depth_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    periodic = next(
        doc
        for doc in docs
        if doc["context"]["emission_kind"] == "periodic" and "origin" not in doc["context"]
        and _path_ids(doc)[0] == "AIRCRAFT"
    )
    periodic["acoustic"]["assessment"]["path"] = periodic["acoustic"]["assessment"]["path"][:1]
    with pytest.raises(AssertionError, match="Helicopter"):
        assert_wire_contract(docs)


def test_reused_observation_id_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    first = docs[0]["identity"]["observation_id"]
    for doc in docs[1:]:
        if "origin" not in doc["context"] and doc["context"]["emission_kind"] == "start":
            doc["identity"]["observation_id"] = first
            break
    with pytest.raises(AssertionError):
        assert_wire_contract(docs)


def test_divergent_scene_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    periodics = [
        doc
        for doc in docs
        if doc["context"]["emission_kind"] == "periodic" and "origin" not in doc["context"]
    ]
    periodics[0]["acoustic"]["measurements"]["rms"]["mean_dbfs"] = -99.0
    with pytest.raises(AssertionError):
        assert_wire_contract(docs)


def test_empty_scene_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    for doc in docs:
        if doc["context"]["emission_kind"] == "periodic" and "origin" not in doc["context"]:
            doc["acoustic"]["measurements"]["window_count"] = 0
            doc["acoustic"]["measurements"]["rms"]["mean_dbfs"] = None
    with pytest.raises(AssertionError):
        assert_wire_contract(docs)


def test_missing_critical_origin_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    for doc in docs:
        doc["context"].pop("origin", None)
    with pytest.raises(AssertionError):
        assert_wire_contract(docs)


def test_nonempty_critical_l0_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    critical = next(doc for doc in docs if doc["context"].get("origin") == "critical_path")
    critical["acoustic"]["measurements"]["window_count"] = 4
    critical["acoustic"]["measurements"]["rms"]["mean_dbfs"] = -31.5
    with pytest.raises(AssertionError):
        assert_wire_contract(docs)


def test_wrong_critical_end_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    end = next(
        doc
        for doc in docs
        if doc["context"].get("origin") == "critical_path"
        and doc["context"]["emission_kind"] == "end"
    )
    end["context"]["termination_reason"] = "ota_reboot"
    end["identity"]["observation_id"] = docs[0]["identity"]["observation_id"]
    with pytest.raises(AssertionError):
        assert_wire_contract(docs)


def _fresh_sequence_copy(doc: dict, sequence: int) -> dict:
    copied = copy.deepcopy(doc)
    copied["identity"]["sequence_number"] = sequence
    copied["identity"]["observation_id"] = "dddddddd-eeee-4fff-8aaa-000000000099"
    return copied


def test_rejected_payloads_do_not_create_rows() -> None:
    topic, loaded = _load_corpus()
    before = _count()
    assert before == 7
    base = copy.deepcopy(loaded[0][1])
    node_id = base["identity"]["node_id"]

    mismatch = _fresh_sequence_copy(base, 101)
    mismatch["identity"]["node_id"] = "EAR-OTHER"
    _publish(topic, _dumps(mismatch))

    _publish(topic.upper(), _dumps(_fresh_sequence_copy(base, 102)))
    slug = topic.split("/")[2]
    _publish(
        f"esp-ear/local/{slug.upper()}/observation",
        _dumps(_fresh_sequence_copy(base, 103)),
    )
    _publish(
        f"esp-output/local/{slug}/observation",
        _dumps(_fresh_sequence_copy(base, 104)),
    )
    missing = _fresh_sequence_copy(base, 105)
    missing["identity"]["node_id"] = "EAR-MISSING"
    _publish("esp-ear/local/ear-missing/observation", _dumps(missing))

    _set_lifecycle(node_id, "deactivated")
    try:
        inactive = _fresh_sequence_copy(base, 106)
        inactive["identity"]["node_id"] = node_id
        _publish(topic, _dumps(inactive))
    finally:
        _set_lifecycle(node_id, "active")

    unknown = _fresh_sequence_copy(base, 107)
    unknown["identity"]["node_id"] = node_id
    unknown["acoustic"]["measurements"]["not_a_field"] = 1
    _publish(topic, _dumps(unknown))

    bad_origin = _fresh_sequence_copy(base, 108)
    bad_origin["identity"]["node_id"] = node_id
    bad_origin["context"]["origin"] = "ce_path"
    _publish(topic, _dumps(bad_origin))

    next_version = _fresh_sequence_copy(base, 109)
    next_version["identity"]["node_id"] = node_id
    next_version["protocol_version"] = 2
    _publish(topic, _dumps(next_version))

    retained = _fresh_sequence_copy(base, 110)
    retained["identity"]["node_id"] = node_id
    _publish(topic, _dumps(retained), retain=True)

    assert _count() == before


def test_same_sequence_replay_does_not_overwrite() -> None:
    topic, loaded = _load_corpus()
    raw, doc = loaded[0]
    before = _stored_by_sequence()[doc["identity"]["sequence_number"]]
    _publish(topic, raw)
    replay = copy.deepcopy(doc)
    replay["acoustic"]["signal_level_dbfs"] = -1.0
    _publish(topic, _dumps(replay))
    conflict = copy.deepcopy(doc)
    conflict["identity"]["observation_id"] = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    _publish(topic, _dumps(conflict))
    after = _stored_by_sequence()[doc["identity"]["sequence_number"]]
    assert after == before
    assert _count() == 7
    assert after["acoustic"].get("assessment") == before["acoustic"].get("assessment")
    assert after["identity"]["node_id"] == before["identity"]["node_id"]


def test_stored_assessment_loss_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    stored = _stored_by_sequence()
    damaged = copy.deepcopy(stored)
    damaged[1]["acoustic"]["assessment"] = None
    damaged[1]["context"]["origin"] = "critical_path"
    with pytest.raises(AssertionError):
        assert_stored_contract(docs, damaged, stored[1]["identity"]["node_id"])
