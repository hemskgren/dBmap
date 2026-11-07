import json
import sys
from datetime import UTC, datetime
from pathlib import Path

SCRIPT_LIB = Path(__file__).resolve().parents[2] / "scripts"
if str(SCRIPT_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPT_LIB))

from lib.ear_test import store
from lib.ear_test.job import JobController
from lib.ear_test.models import JobMetadata
from lib.ear_test.outputs import write_job_outputs

T0 = datetime(2026, 11, 2, 12, 0, 0, tzinfo=UTC)


def test_outputs_share_one_summary_snapshot(tmp_path, monkeypatch) -> None:
    root = tmp_path / "ear-tests"
    root.mkdir()
    monkeypatch.setattr(store, "RESULTS_ROOT", root)

    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=1,
        pause_seconds=0,
        progression="manual",
        hub_base_url="https://hub.example:8443",
        metadata=JobMetadata(
            test_name="unit-analysis",
            description="fixture",
            environment="lab",
            firmware_version_by_ear={"EAR-001": "0.5.20"},
        ),
        now=T0,
    )
    store.create_job_directory(job)
    job.start_next_period(now=T0)
    job.complete_current_period(now=T0.replace(second=10))
    job.mark_completed(now=T0.replace(second=10))
    store.persist_controller(job)

    record = store.build_observation_record(
        controller=job,
        payload={
            "protocol_version": 1,
            "message_type": "observation",
            "observation_id": "dddddddd-dddd-dddd-dddd-dddddddddddd",
            "node_id": "EAR-001",
            "sequence_number": 7,
            "event_time_utc": "2026-11-02T12:00:02Z",
            "duration_ms": 5100.0,
            "signal_level_dbfs": -48.5,
            "capture_timestamp_monotonic_us": 1,
            "timing_quality": "synchronized",
            "timestamp_uncertainty_ms": 100,
            "classification": None,
            "bearing": None,
            "clock_offset_ms": None,
        },
        received_time_utc="2026-11-02T12:00:05Z",
        source="http",
    )
    store.append_observation_record(job.manifest.job_id, record)

    summary = write_job_outputs(job)
    job_path = store.job_dir(job.manifest.job_id)
    saved = json.loads((job_path / "summary.json").read_text(encoding="utf-8"))
    analysis = (job_path / "analysis.md").read_text(encoding="utf-8")
    html = (job_path / "report.html").read_text(encoding="utf-8")

    assert saved["generated_at_utc"] == summary["generated_at_utc"]
    assert saved["observation_count_total"] == 1
    assert summary["generated_at_utc"] in analysis
    assert "unit-analysis" in analysis
    assert "0.5.20" in analysis
    assert "does **not** label Observations as START / PERIODIC / END" in analysis
    assert "Measured facts" in analysis
    assert "Calculated statistics" in analysis
    assert "Open questions" in analysis
    assert str(summary["observation_count_total"]) in html
    assert (job_path / "report.html").is_file()
    assert (job_path / "analysis.md").is_file()
