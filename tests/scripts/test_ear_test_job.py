from datetime import datetime, timezone
import sys
from pathlib import Path

SCRIPT_LIB = Path(__file__).resolve().parents[2] / "scripts"
if str(SCRIPT_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPT_LIB))

import pytest
from lib.ear_test.job import JobController
from lib.ear_test.models import JobMetadata

UTC = timezone.utc
T0 = datetime(2026, 11, 2, 12, 0, 0, tzinfo=UTC)


def test_create_and_run_two_periods() -> None:
    job = JobController.create(
        node_ids=["ear-001"],
        period_seconds=10,
        period_count=2,
        pause_seconds=5,
        progression="manual",
        hub_base_url="https://hub.example:8443",
        metadata=JobMetadata(test_name="unit"),
        now=T0,
    )
    assert job.manifest.node_ids == ["EAR-001"]
    assert job.manifest.status == "running"
    assert len(job.periods) == 2
    assert all(p.status == "planned" for p in job.periods)

    first = job.start_next_period(now=T0)
    assert first.status == "active"
    assert first.period_id == "period-000"
    job.complete_current_period(now=T0.replace(second=10))
    assert first.status == "completed"

    second = job.start_next_period(now=T0.replace(second=15))
    assert second.index == 1
    job.complete_current_period(now=T0.replace(second=25))
    job.mark_completed(now=T0.replace(second=25))
    assert job.manifest.status == "completed"
    assert job.all_periods_terminal()


def test_default_pause_records_skip_after_last_period() -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=30,
        period_count=4,
        pause_seconds=8,
        final_collection_pause_seconds=8,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    assert job.manifest.pause_seconds == 8
    assert job.manifest.final_collection_pause_seconds == 8
    inter = job.inter_period_pauses()
    assert len(inter) == 3
    assert all(p.planned_seconds == 8 for p in inter)
    assert inter[-1].after_period_index == 2
    assert inter[-1].before_period_index == 3
    assert job.periods[-1].pause_after_seconds == 0.0
    final = job.final_collection_pause()
    assert final is not None
    assert final.kind == "final_collection"
    assert final.planned_seconds == 8
    assert final.after_period_index == 3
    assert final.before_period_index is None


def test_custom_pause_durations_respected() -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=30,
        period_count=2,
        pause_seconds=12,
        final_collection_pause_seconds=15,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    assert job.inter_period_pauses()[0].planned_seconds == 12
    assert job.final_collection_pause() is not None
    assert job.final_collection_pause().planned_seconds == 15


def test_zero_final_collection_omits_pause_record() -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=2,
        pause_seconds=8,
        final_collection_pause_seconds=0,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    assert len(job.inter_period_pauses()) == 1
    assert job.final_collection_pause() is None


def test_zero_inter_period_omits_inter_records_keeps_final() -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=3,
        pause_seconds=0,
        final_collection_pause_seconds=8,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    assert job.inter_period_pauses() == []
    assert job.final_collection_pause() is not None


def test_pause_lifecycle_and_interrupt_during_final_collection() -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=1,
        pause_seconds=0,
        final_collection_pause_seconds=8,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    job.start_next_period(now=T0)
    job.complete_current_period(now=T0.replace(second=10))
    pause = job.start_next_pause(now=T0.replace(second=10))
    assert pause.kind == "final_collection"
    assert pause.status == "active"
    job.interrupt(now=T0.replace(second=14))
    assert job.manifest.status == "interrupted"
    assert pause.status == "interrupted"
    assert pause.actual_end_utc == T0.replace(second=14)


def test_interrupt_marks_active_and_skips_remaining() -> None:
    job = JobController.create(
        node_ids=["EAR-001", "EAR-002"],
        period_seconds=10,
        period_count=3,
        pause_seconds=5,
        final_collection_pause_seconds=8,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    job.start_next_period(now=T0)
    job.interrupt(now=T0.replace(second=4))
    assert job.manifest.status == "interrupted"
    assert job.periods[0].status == "interrupted"
    assert job.periods[0].actual_end_utc is not None
    assert job.periods[1].status == "skipped"
    assert job.periods[2].status == "skipped"
    assert all(p.status == "skipped" for p in job.pauses)


def test_cannot_start_two_active_periods() -> None:
    job = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=2,
        pause_seconds=0,
        progression="manual",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    job.start_next_period(now=T0)
    with pytest.raises(RuntimeError, match="already active"):
        job.start_next_period(now=T0.replace(second=1))


def test_rejects_invalid_config() -> None:
    with pytest.raises(ValueError):
        JobController.create(
            node_ids=[],
            period_seconds=10,
            period_count=1,
            pause_seconds=0,
            progression="manual",
            hub_base_url="https://hub.example:8443",
            now=T0,
        )
    with pytest.raises(ValueError, match="unique"):
        JobController.create(
            node_ids=["EAR-001", "EAR-001"],
            period_seconds=10,
            period_count=1,
            pause_seconds=0,
            progression="manual",
            hub_base_url="https://hub.example:8443",
            now=T0,
        )
    with pytest.raises(ValueError, match="final_collection"):
        JobController.create(
            node_ids=["EAR-001"],
            period_seconds=10,
            period_count=1,
            pause_seconds=0,
            final_collection_pause_seconds=-1,
            progression="manual",
            hub_base_url="https://hub.example:8443",
            now=T0,
        )
