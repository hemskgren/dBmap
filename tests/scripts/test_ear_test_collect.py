import sys
from datetime import UTC, datetime
from pathlib import Path

SCRIPT_LIB = Path(__file__).resolve().parents[2] / "scripts"
if str(SCRIPT_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPT_LIB))

from lib.ear_test import store
from lib.ear_test.collect import poll_http_once, reconcile_sqlite
from lib.ear_test.job import JobController

T0 = datetime(2026, 11, 2, 12, 0, 0, tzinfo=UTC)


def test_http_and_sqlite_merge_and_completeness(tmp_path, monkeypatch) -> None:
    root = tmp_path / "ear-tests"
    root.mkdir()
    monkeypatch.setattr(store, "RESULTS_ROOT", root)

    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=1,
        pause_seconds=0,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(job)
    job.start_next_period(now=T0)
    store.persist_controller(job)

    def fake_http(base_url, ca, token, node_id):
        return [
            {
                "protocol_version": 1,
                "message_type": "observation",
                "observation_id": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
                "node_id": "EAR-001",
                "sequence_number": 1,
                "event_time_utc": "2026-11-02T12:00:01Z",
                "duration_ms": 5100,
                "signal_level_dbfs": -50.0,
                "received_time_utc": "2026-11-02T12:00:03Z",
                "capture_timestamp_monotonic_us": 1,
                "timing_quality": "synchronized",
                "timestamp_uncertainty_ms": 100,
                "classification": None,
                "bearing": None,
                "clock_offset_ms": None,
            },
            # Outside job window — must be ignored
            {
                "protocol_version": 1,
                "message_type": "observation",
                "observation_id": "cccccccc-cccc-cccc-cccc-cccccccccccc",
                "node_id": "EAR-001",
                "sequence_number": 0,
                "event_time_utc": "2026-11-02T11:00:00Z",
                "duration_ms": 5100,
                "signal_level_dbfs": -50.0,
                "received_time_utc": "2026-11-02T11:00:01Z",
                "capture_timestamp_monotonic_us": 0,
                "timing_quality": "synchronized",
                "timestamp_uncertainty_ms": 100,
                "classification": None,
                "bearing": None,
                "clock_offset_ms": None,
            },
        ]

    added = poll_http_once(
        job,
        base_url="https://hub.example:8443",
        ca_file="/dev/null",
        admin_token="x" * 32,
        fetcher=fake_http,
    )
    assert added == 1

    def fake_sqlite(node_ids, started, ended):
        return [
            {
                "payload": {
                    "protocol_version": 1,
                    "message_type": "observation",
                    "observation_id": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
                    "node_id": "EAR-001",
                    "sequence_number": 2,
                    "event_time_utc": "2026-11-02T12:00:06Z",
                    "duration_ms": 5100,
                    "signal_level_dbfs": -49.0,
                    "capture_timestamp_monotonic_us": 2,
                    "timing_quality": "synchronized",
                    "timestamp_uncertainty_ms": 100,
                    "classification": None,
                    "bearing": None,
                    "clock_offset_ms": None,
                },
                "received_time_utc": "2026-11-02T12:00:08Z",
            },
            # Duplicate of HTTP row — must not overwrite
            {
                "payload": {
                    "protocol_version": 1,
                    "message_type": "observation",
                    "observation_id": "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
                    "node_id": "EAR-001",
                    "sequence_number": 1,
                    "event_time_utc": "2026-11-02T12:00:01Z",
                    "duration_ms": 9999,
                    "signal_level_dbfs": 0.0,
                    "capture_timestamp_monotonic_us": 1,
                    "timing_quality": "synchronized",
                    "timestamp_uncertainty_ms": 100,
                    "classification": None,
                    "bearing": None,
                    "clock_offset_ms": None,
                },
                "received_time_utc": "2026-11-02T12:00:03Z",
            },
        ]

    added_sql, ok, err = reconcile_sqlite(job, reconciler=fake_sqlite)
    assert ok is True
    assert err is None
    assert added_sql == 1
    assert job.manifest.collection.completeness == "complete"
    rows = store.load_observations(job.manifest.job_id)
    assert len(rows) == 2
    first = next(r for r in rows if r["observation_id"].startswith("aaaaaaaa"))
    assert first["payload"]["duration_ms"] == 5100


def test_sqlite_failure_marks_potentially_incomplete(tmp_path, monkeypatch) -> None:
    root = tmp_path / "ear-tests"
    root.mkdir()
    monkeypatch.setattr(store, "RESULTS_ROOT", root)
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=5,
        period_count=1,
        pause_seconds=0,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(job)

    def boom(node_ids, started, ended):
        raise RuntimeError("docker unavailable")

    added, ok, err = reconcile_sqlite(job, reconciler=boom)
    assert added == 0
    assert ok is False
    assert "docker unavailable" in (err or "")
    assert job.manifest.collection.completeness == "potentially_incomplete"
    assert job.manifest.collection.sqlite_reconcile_attempted is True
