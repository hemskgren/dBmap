import json
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

SCRIPT_LIB = Path(__file__).resolve().parents[2] / "scripts"
if str(SCRIPT_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPT_LIB))

from lib.ear_test.job import JobController
from lib.ear_test import store


UTC = timezone.utc
T0 = datetime(2026, 11, 2, 12, 0, 0, tzinfo=UTC)


@pytest.fixture()
def isolated_results(tmp_path, monkeypatch):
    root = tmp_path / "ear-tests"
    root.mkdir()
    monkeypatch.setattr(store, "RESULTS_ROOT", root)
    return root


def test_dedupe_preserves_first_payload(isolated_results) -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=1,
        pause_seconds=0,
        progression="manual",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(job)
    job.start_next_period(now=T0)
    store.persist_controller(job)

    payload_a = {
        "protocol_version": 1,
        "message_type": "observation",
        "observation_id": "11111111-1111-1111-1111-111111111111",
        "node_id": "EAR-001",
        "sequence_number": 1,
        "event_time_utc": "2026-11-02T12:00:01Z",
        "duration_ms": 2000,
        "signal_level_dbfs": -40.0,
    }
    payload_b = dict(payload_a)
    payload_b["signal_level_dbfs"] = -10.0

    first = store.build_observation_record(
        controller=job,
        payload=payload_a,
        received_time_utc="2026-11-02T12:00:03Z",
        source="http",
    )
    second = store.build_observation_record(
        controller=job,
        payload=payload_b,
        received_time_utc="2026-11-02T12:00:04Z",
        source="sqlite_reconcile",
    )
    assert store.append_observation_record(job.manifest.job_id, first) is True
    assert store.append_observation_record(job.manifest.job_id, second) is False
    rows = store.load_observations(job.manifest.job_id)
    assert len(rows) == 1
    assert rows[0]["payload"]["signal_level_dbfs"] == -40.0
    assert rows[0]["source"] == "http"


def test_active_lock_blocks_second_job(isolated_results) -> None:
    first = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=5,
        period_count=1,
        pause_seconds=0,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(first)
    second = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=5,
        period_count=1,
        pause_seconds=0,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0 + timedelta(seconds=1),
    )
    with pytest.raises(store.JobStoreError, match="ACTIVE lock"):
        store.create_job_directory(second)
