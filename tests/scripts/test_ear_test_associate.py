from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys

SCRIPT_LIB = Path(__file__).resolve().parents[2] / "scripts"
if str(SCRIPT_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPT_LIB))

from lib.ear_test.associate import associate_observation
from lib.ear_test.models import PauseRecord, PeriodRecord

UTC = timezone.utc
T0 = datetime(2026, 11, 2, 12, 0, 0, tzinfo=UTC)


def _period(
    index: int,
    start: datetime,
    end: datetime | None,
    status: str = "completed",
) -> PeriodRecord:
    return PeriodRecord(
        index=index,
        status=status,  # type: ignore[arg-type]
        actual_start_utc=start,
        actual_end_utc=end,
        planned_start_utc=start,
        planned_end_utc=end or (start + timedelta(seconds=10)),
        pause_after_seconds=5.0,
    )


def _pause(
    *,
    kind: str,
    index: int,
    start: datetime,
    end: datetime | None,
    after: int,
    before: int | None,
    planned_seconds: float = 8.0,
) -> PauseRecord:
    return PauseRecord(
        kind=kind,  # type: ignore[arg-type]
        index=index,
        planned_seconds=planned_seconds,
        status="completed" if end is not None else "active",
        after_period_index=after,
        before_period_index=before,
        planned_start_utc=start,
        planned_end_utc=end or (start + timedelta(seconds=planned_seconds)),
        actual_start_utc=start,
        actual_end_utc=end,
    )


def test_received_inside_active_period() -> None:
    periods = [_period(0, T0, T0 + timedelta(seconds=10))]
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=3),
        event_time_utc=T0 + timedelta(seconds=2),
        job_start=T0 - timedelta(seconds=1),
        job_end=T0 + timedelta(seconds=30),
        periods=periods,
    )
    assert result.phase_by_received == "active"
    assert result.period_id_by_received == "period-000"
    assert result.received_event_disagree is False


def test_pause_between_periods() -> None:
    periods = [
        _period(0, T0, T0 + timedelta(seconds=10)),
        _period(1, T0 + timedelta(seconds=15), T0 + timedelta(seconds=25)),
    ]
    pauses = [
        _pause(
            kind="inter_period",
            index=0,
            start=T0 + timedelta(seconds=10),
            end=T0 + timedelta(seconds=15),
            after=0,
            before=1,
        )
    ]
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=12),
        event_time_utc=T0 + timedelta(seconds=12),
        job_start=T0,
        job_end=T0 + timedelta(seconds=40),
        periods=periods,
        pauses=pauses,
    )
    assert result.phase_by_received == "pause"
    assert result.period_id_by_received is None


def test_pause_between_periods_gap_fallback_without_pause_records() -> None:
    periods = [
        _period(0, T0, T0 + timedelta(seconds=10)),
        _period(1, T0 + timedelta(seconds=15), T0 + timedelta(seconds=25)),
    ]
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=12),
        event_time_utc=T0 + timedelta(seconds=12),
        job_start=T0,
        job_end=T0 + timedelta(seconds=40),
        periods=periods,
        pauses=[],
    )
    assert result.phase_by_received == "pause"


def test_final_collection_phase_distinct_from_pause() -> None:
    periods = [_period(0, T0, T0 + timedelta(seconds=30))]
    pauses = [
        _pause(
            kind="final_collection",
            index=0,
            start=T0 + timedelta(seconds=30),
            end=T0 + timedelta(seconds=38),
            after=0,
            before=None,
        )
    ]
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=34),
        event_time_utc=T0 + timedelta(seconds=28),
        job_start=T0,
        job_end=T0 + timedelta(seconds=38),
        periods=periods,
        pauses=pauses,
    )
    assert result.phase_by_received == "final_collection"
    assert result.period_id_by_received is None
    assert result.phase_by_event == "active"
    assert result.received_event_disagree is True


def test_job_idle_before_first_period() -> None:
    periods = [_period(0, T0 + timedelta(seconds=5), T0 + timedelta(seconds=15))]
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=1),
        event_time_utc=T0 + timedelta(seconds=1),
        job_start=T0,
        job_end=T0 + timedelta(seconds=40),
        periods=periods,
    )
    assert result.phase_by_received == "job_idle"


def test_received_event_disagree_highlighted() -> None:
    periods = [
        _period(0, T0, T0 + timedelta(seconds=10)),
        _period(1, T0 + timedelta(seconds=15), T0 + timedelta(seconds=25)),
    ]
    # Received during period 1; event_time still in period 0 (latency / interval start).
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=16),
        event_time_utc=T0 + timedelta(seconds=9),
        job_start=T0,
        job_end=T0 + timedelta(seconds=40),
        periods=periods,
    )
    assert result.period_id_by_received == "period-001"
    assert result.period_id_by_event == "period-000"
    assert result.received_event_disagree is True
    assert "different" in result.association_note


def test_open_active_period_includes_later_received() -> None:
    periods = [_period(0, T0, None, status="active")]
    result = associate_observation(
        received_time_utc=T0 + timedelta(seconds=60),
        event_time_utc=T0 + timedelta(seconds=59),
        job_start=T0,
        job_end=None,
        periods=periods,
    )
    assert result.phase_by_received == "active"
    assert result.period_id_by_received == "period-000"


def test_outside_job_window_unassigned() -> None:
    periods = [_period(0, T0, T0 + timedelta(seconds=10))]
    result = associate_observation(
        received_time_utc=T0 - timedelta(seconds=30),
        event_time_utc=T0 - timedelta(seconds=30),
        job_start=T0,
        job_end=T0 + timedelta(seconds=20),
        periods=periods,
    )
    assert result.phase_by_received == "unassigned"
