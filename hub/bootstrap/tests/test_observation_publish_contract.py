"""Firmware-to-Hub Observation publish contract.

Reads the seven JSON documents produced by the host exporter and feeds the
original bytes through the production MQTT listener.

``mqtt_listener._ingest_observation`` logs ``body.observation_id`` when a
replay is ignored. ``ObservationBody`` stores that id on ``identity``, so the
log line raises ``AttributeError``. The listener catches ``Exception`` and
does not write a row. That pre-existing defect stays out of this PR.
"""

from __future__ import annotations

import copy
import hashlib
import json
import os
import sqlite3
from datetime import UTC, datetime, timedelta
from pathlib import Path

import paho.mqtt.client as mqtt
import pytest
from app import mqtt_listener
from app import regional_environment as regional
from app.config import settings
from app.models import Base, Node, Observation
from app.schemas import ObservationBody, ObservationView
from app.service import ingest_observation, list_observations_page
from app.topics import parse_observation_topic
from sqlalchemy import create_engine, func, select
from sqlalchemy.orm import sessionmaker

_CORPUS = os.environ.get("DBMAP_OBS_CONTRACT_DIR", "")
_NULL4 = [None, None, None, None]
_KINDS = {
    1: "start",
    2: "start",
    3: "periodic",
    4: "periodic",
    5: "start",
    6: "periodic",
    7: "end",
}
_PATHS = {
    1: ["AIRCRAFT"],
    2: ["ALARM"],
    3: ["AIRCRAFT", "Helicopter"],
    4: ["ALARM", "SE_PUBLIC_ALERT"],
}
# Callback durations for the exporter's fixed Critical timeline:
# start 12_000_000 us, periodic 17_000_000 us, end 18_000_000 us.
_CRITICAL_DURATIONS = {
    5: (0, 0),
    6: (5000, 5000),
    7: (1000, 6000),
}


class _ContractDb:
    factory: sessionmaker | None = None
    path: Path | None = None


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


def _session():
    assert _ContractDb.factory is not None
    return _ContractDb.factory()


def _message(topic: str, payload: bytes, *, retain: bool = False) -> mqtt.MQTTMessage:
    message = mqtt.MQTTMessage(topic=topic.encode())
    message.payload = payload
    message.retain = retain
    return message


def _publish(topic: str, payload: bytes, *, retain: bool = False) -> None:
    mqtt_listener._on_message(None, None, _message(topic, payload, retain=retain))


def _count() -> int:
    with _session() as db:
        return int(db.scalar(select(func.count()).select_from(Observation)) or 0)


def _stored_by_sequence() -> dict[int, dict]:
    with _session() as db:
        rows = db.scalars(select(Observation).order_by(Observation.sequence_number)).all()
        return {int(row.sequence_number): json.loads(row.payload_json) for row in rows}


def _seed_active_ear(node_id: str) -> None:
    with _session() as db:
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
    with _session() as db:
        node = db.get(Node, node_id)
        assert node is not None
        node.lifecycle_state = state
        db.commit()


def _dumps(doc: dict) -> bytes:
    return json.dumps(doc, separators=(",", ":")).encode()


def _by_sequence(docs: list[dict]) -> dict[int, dict]:
    found: dict[int, dict] = {}
    for doc in docs:
        seq = int(doc["identity"]["sequence_number"])
        assert seq not in found
        found[seq] = doc
    assert set(found) == set(_KINDS)
    return found


def _at(found: dict[int, dict], seq: int) -> dict:
    doc = found[seq]
    assert doc["context"]["emission_kind"] == _KINDS[seq]
    return doc


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


def _require_canonical_utc(value: str) -> datetime:
    parsed = datetime.fromisoformat(value)
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise AssertionError(f"naive event_time_utc: {value}")
    if parsed.utcoffset() != timedelta(0):
        raise AssertionError(f"non-UTC event_time_utc: {value}")
    return parsed.astimezone(UTC)


def _assert_critical_empty(acoustic: dict) -> None:
    measurements = acoustic["measurements"]
    assert measurements["schema"] == "l0"
    assert measurements["window_count"] == 0, "window_count must be zero"
    assert measurements["clipped_window_count"] == 0
    assert measurements["n_crest"] == 0
    assert measurements["bands"]["mean_dbfs"] == _NULL4, "mean_dbfs must be null"
    assert measurements["bands"]["fraction"] == _NULL4
    assert acoustic["signal_level_dbfs"] is None, "signal_level_dbfs must be null"
    assert acoustic["signature"] is None, "signature must be null"
    assert acoustic["dynamics"] is None
    assert measurements["sample_rate_hz"] > 0
    assert measurements["analysis_window_ms"] > 0
    assert measurements["rms"]["mean_dbfs"] is None
    assert measurements["rms"]["min_dbfs"] is None
    assert measurements["rms"]["max_dbfs"] is None
    assert measurements["hl"]["mean_db"] is None
    assert measurements["t1"]["rms_stdev_db"] is None
    assert measurements["t1"]["rms_range_db"] is None
    assert measurements["t2"]["rms_slope_db_per_s"] is None
    assert measurements["t3"]["crest_max_db"] is None
    assert measurements["t3"]["high_crest_count"] is None
    assert measurements["t3"]["high_crest_rate"] is None
    assert measurements["t6"]["hl_slope_db_per_s"] is None


def _assert_same_measurement_interval(left: dict, right: dict) -> None:
    assert left["context"]["event_time_utc"] == right["context"]["event_time_utc"]
    assert (
        left["context"]["capture_timestamp_monotonic_us"]
        == right["context"]["capture_timestamp_monotonic_us"]
    )
    assert left["context"]["interval_duration_ms"] == right["context"]["interval_duration_ms"]
    assert left["acoustic"]["measurements"] == right["acoustic"]["measurements"], (
        "measurements must be the shared interval"
    )
    assert left["acoustic"]["signature"] == right["acoustic"]["signature"]
    assert left["acoustic"]["dynamics"] == right["acoustic"]["dynamics"]
    assert left["acoustic"]["measurements"]["window_count"] > 0, "window_count must be populated"
    assert left["acoustic"]["measurements"]["rms"]["mean_dbfs"] not in (None, 0)
    _assert_populated_scene(left["acoustic"])
    _assert_populated_scene(right["acoustic"])


def _near(expect: float):
    # Firmware statistics are float32. Compare the JSON number, not the text.
    return pytest.approx(expect, rel=1e-5, abs=1e-5)


def _assert_populated_scene(acoustic: dict) -> None:
    """Require the shared scene fixture on one report, independent of its pair."""
    measurements = acoustic["measurements"]
    assert isinstance(measurements, dict)
    assert measurements["schema"] == "l0"
    assert measurements["version"] == 1
    assert measurements["window_count"] == 4, "L0 window_count must match the scene fixture"
    assert measurements["rms"]["mean_dbfs"] == _near(-31.5), (
        "L0 rms mean must match the scene fixture"
    )
    assert measurements["bands"]["mean_dbfs"][0] == _near(-33.0)
    signature = acoustic.get("signature")
    assert isinstance(signature, dict), "L1 signature must be present"
    assert signature["schema"] == "l1"
    assert signature["version"] == 1
    assert signature["window_count"] == 4, "L1 window_count must match the scene fixture"
    assert signature["centroid_hz"]["mean"] == _near(812.5), (
        "L1 centroid mean must match the scene fixture"
    )
    assert signature["flatness"]["mean"] == _near(0.25)
    dynamics = acoustic.get("dynamics")
    assert isinstance(dynamics, dict), "L2 dynamics must be present"
    assert dynamics["schema"] == "l2"
    assert dynamics["version"] == 1
    assert dynamics["window_count"] == 4, "L2 window_count must match the scene fixture"
    assert dynamics["spectral_flux"]["mean"] == _near(0.375), (
        "L2 spectral flux mean must match the scene fixture"
    )
    assert dynamics["dominant_delta_hz"]["mean"] == _near(15.625)


def _assert_semantic(got: object, expect: object, label: str) -> None:
    if expect is None or got is None:
        assert got is None and expect is None, f"{label} must match the wire"
        return
    if isinstance(expect, dict):
        assert isinstance(got, dict), f"{label} must match the wire"
        assert set(got) == set(expect), f"{label} keys must match the wire"
        for key, value in expect.items():
            _assert_semantic(got[key], value, f"{label}.{key}")
        return
    if isinstance(expect, list):
        assert isinstance(got, list) and len(got) == len(expect), f"{label} must match the wire"
        for index, value in enumerate(expect):
            _assert_semantic(got[index], value, f"{label}[{index}]")
        return
    if isinstance(expect, bool):
        assert got == expect, f"{label} must match the wire"
        return
    if isinstance(expect, (int, float)):
        assert isinstance(got, (int, float)) and not isinstance(got, bool), (
            f"{label} must match the wire"
        )
        assert got == _near(float(expect)), f"{label} must match the wire"
        return
    assert got == expect, f"{label} must match the wire"


def _assert_critical_timeline(found: dict[int, dict]) -> None:
    start = _at(found, 5)
    periodic = _at(found, 6)
    end = _at(found, 7)
    captures = [
        start["context"]["capture_timestamp_monotonic_us"],
        periodic["context"]["capture_timestamp_monotonic_us"],
        end["context"]["capture_timestamp_monotonic_us"],
    ]
    assert captures[0] < captures[1] < captures[2], (
        "critical capture timestamps must increase"
    )
    assert (
        start["identity"]["observation_id"]
        == periodic["identity"]["observation_id"]
        == end["identity"]["observation_id"]
    ), "observation_id must be shared by the critical lifecycle"
    for seq, (interval_ms, episode_ms) in _CRITICAL_DURATIONS.items():
        doc = _at(found, seq)
        assert doc["context"]["interval_duration_ms"] == interval_ms
        assert doc["context"]["episode_duration_ms"] == episode_ms
    assert end["context"]["termination_reason"] == "natural", (
        "termination_reason must be natural"
    )


def assert_wire_contract(docs: list[dict]) -> None:
    """Fail when the seven production documents no longer match the contract."""
    found = _by_sequence(docs)
    assert all(doc["protocol_version"] == 1 for doc in docs)
    assert all(doc["message_type"] == "observation" for doc in docs)
    for seq in (1, 2, 3, 4):
        doc = _at(found, seq)
        assert "origin" not in doc["context"]
        assert _path_ids(doc) == _PATHS[seq]
        assessment = doc["acoustic"]["assessment"]
        assert assessment["schema"] == "ear_hyp_v2"
        assert assessment["version"] == 1
        confidences = _confidences(doc)
        assert confidences
        assert all(0.0 < value <= 1.0 for value in confidences)
    aircraft = _confidences(_at(found, 3))
    assert aircraft[0] + 1e-6 >= aircraft[1]
    assert "ALARM" not in _path_ids(_at(found, 1))
    assert "ALARM" not in _path_ids(_at(found, 3))
    assert "AIRCRAFT" not in _path_ids(_at(found, 2))
    assert "Helicopter" not in _path_ids(_at(found, 4))
    as001 = _at(found, 1)["identity"]["observation_id"]
    as002 = _at(found, 2)["identity"]["observation_id"]
    critical = _at(found, 5)["identity"]["observation_id"]
    assert _at(found, 3)["identity"]["observation_id"] == as001
    assert _at(found, 4)["identity"]["observation_id"] == as002, (
        "observation_id must stay with its lifecycle"
    )
    assert len({as001, as002, critical}) == 3
    for seq in (5, 6, 7):
        doc = _at(found, seq)
        assert doc["context"].get("origin") == "critical_path"
        assert "assessment" not in doc["acoustic"]
        _assert_critical_empty(doc["acoustic"])
    assert "termination_reason" not in _at(found, 5)["context"]
    assert "termination_reason" not in _at(found, 6)["context"]
    _assert_critical_timeline(found)
    _assert_same_measurement_interval(_at(found, 3), _at(found, 4))
    assert _at(found, 3)["acoustic"]["signature"]["schema"] == "l1"
    assert _at(found, 3)["acoustic"]["dynamics"]["schema"] == "l2"


def assert_stored_contract(
    wire: list[dict], stored: dict[int, dict], topic_node: str
) -> None:
    found = _by_sequence(wire)
    assert set(stored) >= set(_KINDS)
    for seq, kind in _KINDS.items():
        doc = _at(found, seq)
        row = stored[seq]
        assert row["identity"]["node_id"] == topic_node
        assert row["identity"]["observation_id"] == doc["identity"]["observation_id"]
        assert row["identity"]["sequence_number"] == seq
        assert row["identity"]["track_id"] is None
        assert row["context"]["emission_kind"] == kind
        stored_time = _require_canonical_utc(row["context"]["event_time_utc"])
        wire_time = _require_canonical_utc(doc["context"]["event_time_utc"])
        assert stored_time == wire_time
        assert row["context"]["capture_timestamp_monotonic_us"] == (
            doc["context"]["capture_timestamp_monotonic_us"]
        )
        assert row["context"]["interval_duration_ms"] == doc["context"]["interval_duration_ms"], (
            "interval_duration_ms must match the observed duration"
        )
        assert row["context"]["episode_duration_ms"] == doc["context"]["episode_duration_ms"]
        assert row["context"]["termination_reason"] == doc["context"].get("termination_reason")
        assert row["acoustic"]["evidence"] is None
        assert row["acoustic"]["debug"] is None
        assert row["acoustic"]["classification"] is None
        assert row["acoustic"]["measurements"] == doc["acoustic"]["measurements"]
        assert row["acoustic"]["signature"] == doc["acoustic"]["signature"], (
            "signature must match the wire"
        )
        assert row["acoustic"]["dynamics"] == doc["acoustic"]["dynamics"]
        if seq in _PATHS:
            assert row["context"]["origin"] is None
            assert row["acoustic"]["assessment"] is not None, "assessment must be stored"
            assert row["acoustic"]["assessment"]["schema"] == "ear_hyp_v2"
            assert row["acoustic"]["assessment"]["version"] == 1
            assert _path_ids(row) == _PATHS[seq]
            for got, expect in zip(_confidences(row), _confidences(doc), strict=True):
                assert got == pytest.approx(expect)
        else:
            assert row["context"]["origin"] == "critical_path"
            assert row["acoustic"]["assessment"] is None
            _assert_critical_empty(row["acoustic"])
    _assert_same_measurement_interval(stored[3], stored[4])
    _assert_critical_timeline(stored)


def _view_payload(item: ObservationView) -> dict:
    payload = item.model_dump(by_alias=True, mode="json")
    payload.pop("received_time_utc", None)
    return payload


def assert_readback_contract(
    wire: list[dict], items: list[ObservationView], topic_node: str
) -> None:
    found = _by_sequence(wire)
    listed = {int(item.identity.sequence_number): item for item in items}
    assert set(_KINDS) <= set(listed)
    dumped = {seq: _view_payload(listed[seq]) for seq in _KINDS}
    for seq, kind in _KINDS.items():
        item = listed[seq]
        doc = _at(found, seq)
        event = item.context.event_time_utc
        assert event.tzinfo is not None and event.utcoffset() == timedelta(0)
        assert event == _require_canonical_utc(doc["context"]["event_time_utc"])
        assert item.identity.node_id == topic_node
        assert str(item.identity.observation_id) == doc["identity"]["observation_id"]
        assert item.context.emission_kind == kind
        if seq in _PATHS:
            assert item.context.origin is None, "list readback lost normal origin"
            assert item.acoustic.assessment is not None, "list readback lost assessment"
            assert [node.id for node in item.acoustic.assessment.path] == _PATHS[seq]
        else:
            assert item.context.origin == "critical_path", "list readback lost critical origin"
            assert item.acoustic.assessment is None
            assert item.acoustic.signature is None
            assert item.acoustic.dynamics is None
        assert dumped[seq]["context"]["termination_reason"] == doc["context"].get(
            "termination_reason"
        )
        listed_acoustic = dumped[seq]["acoustic"]
        wire_acoustic = doc["acoustic"]
        _assert_semantic(
            listed_acoustic.get("assessment"),
            wire_acoustic.get("assessment"),
            "assessment",
        )
        _assert_semantic(listed_acoustic["measurements"], wire_acoustic["measurements"], "L0")
        _assert_semantic(listed_acoustic.get("signature"), wire_acoustic.get("signature"), "L1")
        _assert_semantic(listed_acoustic.get("dynamics"), wire_acoustic.get("dynamics"), "L2")
    _assert_same_measurement_interval(dumped[3], dumped[4])
    _assert_critical_timeline(dumped)
    for seq in (5, 6, 7):
        _assert_critical_empty(dumped[seq]["acoustic"])


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _configured_sqlite_path() -> Path | None:
    url = settings.database_url
    if not url.startswith("sqlite:///"):
        return None
    raw = url.removeprefix("sqlite:///")
    if raw in ("", ":memory:"):
        return None
    path = Path(raw)
    if not path.is_file():
        return None
    return path


@pytest.fixture(autouse=True)
def _suppress_weather(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(regional, "schedule_weather_ensure", lambda *_a, **_k: None)


@pytest.fixture(scope="module", autouse=True)
def _contract_database(tmp_path_factory: pytest.TempPathFactory):
    """Own the contract SQLite file and the listener session factory."""
    db_path = tmp_path_factory.mktemp("obs-publish-contract") / "contract.db"
    engine = create_engine(
        "sqlite:///" + db_path.as_posix(),
        connect_args={"check_same_thread": False},
    )
    Base.metadata.create_all(bind=engine)
    factory = sessionmaker(bind=engine, autoflush=False, autocommit=False)
    previous_session = mqtt_listener.SessionLocal
    previous_weather = regional.schedule_weather_ensure
    mqtt_listener.SessionLocal = factory
    regional.schedule_weather_ensure = lambda *_args, **_kwargs: None
    _ContractDb.factory = factory
    _ContractDb.path = db_path
    try:
        topic, loaded = _load_corpus()
        node_id = parse_observation_topic(topic)
        assert node_id is not None
        _seed_active_ear(node_id)
        for raw, _doc in loaded:
            _publish(topic, raw)
        assert _count() == 7
        yield
    finally:
        mqtt_listener.SessionLocal = previous_session
        regional.schedule_weather_ensure = previous_weather
        _ContractDb.factory = None
        _ContractDb.path = None
        engine.dispose()


def test_seven_production_reports_round_trip() -> None:
    topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    assert_wire_contract(docs)
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    assert _count() == 7
    stored = _stored_by_sequence()
    assert_stored_contract(docs, stored, node_id)
    with _session() as db:
        page = list_observations_page(db, node_id, page=1, page_size=50, order="oldest")
    assert page.total == 7
    assert len(page.items) == 7
    assert_readback_contract(docs, list(page.items), node_id)


def test_unrelated_sqlite_database_remains_untouched(tmp_path: Path) -> None:
    sentinel = tmp_path / "unrelated.db"
    with sqlite3.connect(sentinel) as connection:
        connection.execute("CREATE TABLE sentinel (note TEXT)")
        connection.execute("INSERT INTO sentinel (note) VALUES ('keep')")
    before = sentinel.read_bytes()
    configured = _configured_sqlite_path()
    configured_before = _sha256(configured) if configured is not None else None
    topic, loaded = _load_corpus()
    extra = copy.deepcopy(loaded[0][1])
    extra["identity"]["sequence_number"] = 301
    extra["identity"]["observation_id"] = "dddddddd-eeee-4fff-8aaa-000000000301"
    _publish(topic, _dumps(extra))
    try:
        assert sentinel.read_bytes() == before
        with sqlite3.connect(sentinel) as connection:
            note = connection.execute("SELECT note FROM sentinel").fetchone()
        assert note == ("keep",)
        if configured is not None and configured_before is not None:
            assert _sha256(configured) == configured_before
        assert _ContractDb.path is not None
        assert _ContractDb.path.resolve() != sentinel.resolve()
        assert _count() == 8
    finally:
        with _session() as db:
            row = db.scalars(
                select(Observation).where(Observation.sequence_number == 301)
            ).first()
            if row is not None:
                db.delete(row)
                db.commit()


def test_offset_event_time_is_stored_as_utc() -> None:
    topic, loaded = _load_corpus()
    original = copy.deepcopy(loaded[4][1])
    instant = _require_canonical_utc(original["context"]["event_time_utc"])
    offset_clock = instant.astimezone(UTC).replace(tzinfo=None) + timedelta(hours=1)
    offset_input = offset_clock.isoformat() + "+01:00"
    assert datetime.fromisoformat(offset_input).astimezone(UTC) == instant
    published = copy.deepcopy(original)
    published["identity"]["sequence_number"] = 302
    published["identity"]["observation_id"] = "dddddddd-eeee-4fff-8aaa-000000000302"
    published["context"]["event_time_utc"] = offset_input
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    _publish(topic, _dumps(published))
    try:
        stored = _stored_by_sequence()[302]
        stored_time = _require_canonical_utc(stored["context"]["event_time_utc"])
        assert stored_time == instant
        assert "+01:00" not in stored["context"]["event_time_utc"]
        with _session() as db:
            page = list_observations_page(db, node_id, page=1, page_size=50, order="oldest")
        listed = next(item for item in page.items if item.identity.sequence_number == 302)
        assert listed.context.event_time_utc.utcoffset() == timedelta(0)
        assert listed.context.event_time_utc == instant
    finally:
        with _session() as db:
            row = db.scalars(
                select(Observation).where(Observation.sequence_number == 302)
            ).first()
            if row is not None:
                db.delete(row)
                db.commit()


def test_assessment_swap_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    found = _by_sequence(docs)
    first = _at(found, 1)
    second = _at(found, 2)
    first["acoustic"]["assessment"], second["acoustic"]["assessment"] = (
        second["acoustic"]["assessment"],
        first["acoustic"]["assessment"],
    )
    with pytest.raises(AssertionError, match="AIRCRAFT"):
        assert_wire_contract(docs)


def test_lost_assessment_depth_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    periodic = _at(_by_sequence(docs), 3)
    periodic["acoustic"]["assessment"]["path"] = periodic["acoustic"]["assessment"]["path"][:1]
    with pytest.raises(AssertionError, match="Helicopter"):
        assert_wire_contract(docs)


def test_reused_observation_id_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    found = _by_sequence(docs)
    _at(found, 2)["identity"]["observation_id"] = _at(found, 1)["identity"]["observation_id"]
    with pytest.raises(AssertionError, match="observation_id must stay with its lifecycle"):
        assert_wire_contract(docs)


def test_divergent_scene_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    _at(_by_sequence(docs), 3)["acoustic"]["measurements"]["rms"]["mean_dbfs"] = -99.0
    with pytest.raises(AssertionError, match="measurements must be the shared interval"):
        assert_wire_contract(docs)


def test_empty_scene_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    for seq in (3, 4):
        acoustic = _at(_by_sequence(docs), seq)["acoustic"]
        acoustic["measurements"]["window_count"] = 0
        acoustic["measurements"]["rms"]["mean_dbfs"] = None
    with pytest.raises(AssertionError, match="window_count must be populated"):
        assert_wire_contract(docs)


def _zero_l1(signature: dict) -> None:
    signature["window_count"] = 0
    signature["invalid_window_count"] = 0
    for name in ("centroid_hz", "bandwidth_hz", "dominant_hz", "flatness"):
        signature[name]["mean"] = None
        signature[name]["stdev"] = None


def _zero_l2(dynamics: dict) -> None:
    dynamics["window_count"] = 0
    dynamics["invalid_window_count"] = 0
    for name in ("spectral_flux", "envelope_cv", "dominant_delta_hz"):
        dynamics[name]["valid_count"] = 0
        dynamics[name]["mean"] = None
        dynamics[name]["stdev"] = None


def test_zero_periodic_l1_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    for seq in (3, 4):
        _zero_l1(_at(_by_sequence(docs), seq)["acoustic"]["signature"])
    with pytest.raises(AssertionError, match="L1 window_count must match the scene fixture"):
        assert_wire_contract(docs)


def test_zero_periodic_l2_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    for seq in (3, 4):
        _zero_l2(_at(_by_sequence(docs), seq)["acoustic"]["dynamics"])
    with pytest.raises(AssertionError, match="L2 window_count must match the scene fixture"):
        assert_wire_contract(docs)


def test_missing_critical_origin_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    for seq in (5, 6, 7):
        _at(_by_sequence(docs), seq)["context"].pop("origin", None)
    with pytest.raises(AssertionError, match="critical_path"):
        assert_wire_contract(docs)


def test_nonempty_critical_l0_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    measurements = _at(_by_sequence(docs), 5)["acoustic"]["measurements"]
    measurements["window_count"] = 4
    measurements["rms"]["mean_dbfs"] = -31.5
    with pytest.raises(AssertionError, match="window_count must be zero"):
        assert_wire_contract(docs)


def test_populated_critical_band_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    acoustic = _at(_by_sequence(docs), 5)["acoustic"]
    acoustic["measurements"]["bands"]["mean_dbfs"] = [-33.0, None, None, None]
    acoustic["signal_level_dbfs"] = -12.0
    with pytest.raises(AssertionError, match="mean_dbfs must be null"):
        assert_wire_contract(docs)


def test_reordered_critical_captures_fail_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    found = _by_sequence(docs)
    start = _at(found, 5)["context"]
    periodic = _at(found, 6)["context"]
    start["capture_timestamp_monotonic_us"], periodic["capture_timestamp_monotonic_us"] = (
        periodic["capture_timestamp_monotonic_us"],
        start["capture_timestamp_monotonic_us"],
    )
    with pytest.raises(AssertionError, match="critical capture timestamps must increase"):
        assert_wire_contract(docs)


def test_wrong_critical_end_uuid_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    end = _at(_by_sequence(docs), 7)
    assert end["context"]["termination_reason"] == "natural"
    end["identity"]["observation_id"] = "ffffffff-ffff-4fff-8fff-ffffffffffff"
    with pytest.raises(AssertionError, match="observation_id must be shared"):
        assert_wire_contract(docs)


def test_wrong_critical_end_reason_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [copy.deepcopy(doc) for _raw, doc in loaded]
    end = _at(_by_sequence(docs), 7)
    end["context"]["termination_reason"] = "ota_reboot"
    with pytest.raises(AssertionError, match="natural"):
        assert_wire_contract(docs)


def test_naive_stored_timestamp_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    stored = copy.deepcopy(_stored_by_sequence())
    stored[1]["context"]["event_time_utc"] = "2026-01-01T00:00:00"
    node_id = stored[1]["identity"]["node_id"]
    with pytest.raises(AssertionError, match="naive event_time_utc"):
        assert_stored_contract(docs, stored, node_id)


def test_non_utc_stored_timestamp_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    stored = copy.deepcopy(_stored_by_sequence())
    stored[1]["context"]["event_time_utc"] = "2026-01-01T01:00:00+01:00"
    node_id = stored[1]["identity"]["node_id"]
    with pytest.raises(AssertionError, match="non-UTC event_time_utc"):
        assert_stored_contract(docs, stored, node_id)


def test_stored_critical_duration_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    stored = copy.deepcopy(_stored_by_sequence())
    stored[5]["context"]["interval_duration_ms"] = 1
    node_id = stored[1]["identity"]["node_id"]
    with pytest.raises(AssertionError, match="interval_duration_ms must match"):
        assert_stored_contract(docs, stored, node_id)


def test_stored_critical_signature_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    stored = copy.deepcopy(_stored_by_sequence())
    stored[5]["acoustic"]["signature"] = {"schema": "l1"}
    node_id = stored[1]["identity"]["node_id"]
    with pytest.raises(AssertionError, match="signature must match the wire"):
        assert_stored_contract(docs, stored, node_id)


def test_readback_missing_assessment_fails_contract() -> None:
    topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    with _session() as db:
        page = list_observations_page(db, node_id, page=1, page_size=50, order="oldest")
    items = list(page.items)
    target = next(item for item in items if item.identity.sequence_number == 1)
    damaged = _view_payload(target)
    damaged["acoustic"]["assessment"] = None
    damaged["received_time_utc"] = target.received_time_utc
    replacement = ObservationView.model_validate(damaged)
    items = [replacement if item.identity.sequence_number == 1 else item for item in items]
    with pytest.raises(AssertionError, match="list readback lost assessment"):
        assert_readback_contract(docs, items, node_id)


def test_readback_missing_origin_fails_contract() -> None:
    topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    with _session() as db:
        page = list_observations_page(db, node_id, page=1, page_size=50, order="oldest")
    items = list(page.items)
    critical = next(item for item in items if item.identity.sequence_number == 5)
    damaged = _view_payload(critical)
    damaged["context"]["origin"] = None
    damaged["received_time_utc"] = critical.received_time_utc
    replacement = ObservationView.model_validate(damaged)
    items = [
        replacement if item.identity.sequence_number == 5 else item for item in items
    ]
    with pytest.raises(AssertionError, match="list readback lost critical origin"):
        assert_readback_contract(docs, items, node_id)


def _listed_items() -> tuple[str, list[dict], list[ObservationView]]:
    topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    node_id = parse_observation_topic(topic)
    assert node_id is not None
    with _session() as db:
        page = list_observations_page(db, node_id, page=1, page_size=50, order="oldest")
    return node_id, docs, list(page.items)


def _replace_listed(items: list[ObservationView], seq: int, mutate) -> list[ObservationView]:
    target = next(item for item in items if item.identity.sequence_number == seq)
    damaged = _view_payload(target)
    mutate(damaged)
    damaged["received_time_utc"] = target.received_time_utc
    replacement = ObservationView.model_validate(damaged)
    return [replacement if item.identity.sequence_number == seq else item for item in items]


def test_readback_removed_l1_l2_fails_contract() -> None:
    node_id, docs, items = _listed_items()

    def clear_scene(payload: dict) -> None:
        payload["acoustic"]["signature"] = None
        payload["acoustic"]["dynamics"] = None

    for seq in (3, 4):
        items = _replace_listed(items, seq, clear_scene)
    with pytest.raises(AssertionError, match="L1 must match the wire"):
        assert_readback_contract(docs, items, node_id)


def test_readback_confidence_change_fails_contract() -> None:
    node_id, docs, items = _listed_items()

    def change_confidence(payload: dict) -> None:
        payload["acoustic"]["assessment"]["path"][0]["confidence"] = 0.1

    items = _replace_listed(items, 3, change_confidence)
    with pytest.raises(AssertionError, match="confidence must match the wire"):
        assert_readback_contract(docs, items, node_id)


def test_readback_l1_statistic_change_fails_contract() -> None:
    node_id, docs, items = _listed_items()

    def change_centroid(payload: dict) -> None:
        payload["acoustic"]["signature"]["centroid_hz"]["mean"] = 1.0

    items = _replace_listed(items, 3, change_centroid)
    with pytest.raises(AssertionError, match=r"L1\.centroid_hz\.mean must match the wire"):
        assert_readback_contract(docs, items, node_id)


def test_contract_database_is_under_pytest_basetemp(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    assert _ContractDb.path is not None
    base = tmp_path_factory.getbasetemp().resolve()
    assert _ContractDb.path.resolve().is_relative_to(base)


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
    node_id = parse_observation_topic(topic)
    assert node_id is not None

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


def test_same_sequence_replay_and_conflict() -> None:
    topic, loaded = _load_corpus()
    raw, doc = loaded[0]
    seq = doc["identity"]["sequence_number"]
    before = _stored_by_sequence()[seq]
    _publish(topic, raw)
    replay = copy.deepcopy(doc)
    replay["acoustic"]["signal_level_dbfs"] = -1.0
    _publish(topic, _dumps(replay))
    conflict = copy.deepcopy(doc)
    conflict["identity"]["observation_id"] = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    _publish(topic, _dumps(conflict))
    after = _stored_by_sequence()[seq]
    assert after == before
    assert _count() == 7
    assert after["acoustic"].get("assessment") == before["acoustic"].get("assessment")
    assert after["identity"]["node_id"] == before["identity"]["node_id"]

    node_id = before["identity"]["node_id"]
    body = ObservationBody.model_validate(doc)
    conflict_body = ObservationBody.model_validate(conflict)
    with _session() as db:
        assert ingest_observation(db, node_id, body) is False
        with pytest.raises(ValueError, match="different observation_id"):
            ingest_observation(db, node_id, conflict_body)
        db.rollback()
    assert _stored_by_sequence()[seq] == before


def test_stored_assessment_loss_fails_contract() -> None:
    _topic, loaded = _load_corpus()
    docs = [doc for _raw, doc in loaded]
    stored = copy.deepcopy(_stored_by_sequence())
    stored[1]["acoustic"]["assessment"] = None
    with pytest.raises(AssertionError, match="assessment must be stored"):
        assert_stored_contract(docs, stored, stored[1]["identity"]["node_id"])
