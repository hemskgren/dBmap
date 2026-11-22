"""Shared sparse observation lifecycle for the Core and public monitor stores."""

from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import TypeVar

from sqlalchemy.orm import Session

from app.monitoring_math import ensure_aware

T = TypeVar("T")


@dataclass(frozen=True)
class ObservationGap:
    start: datetime
    end: datetime
    exceeds_threshold: bool

    def marker_at(self, previous_at: datetime) -> datetime:
        """Place the boundary between the preceding and resumed observations."""
        return min(
            max(self.start, ensure_aware(previous_at) + timedelta(microseconds=1)),
            self.end - timedelta(microseconds=1),
        )


def checkpoint_time(
    value: str | None, *, anchor_at: datetime, observed_at: datetime
) -> datetime | None:
    """A checkpoint must bound real observations in the same timeline."""
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value)
        if parsed.utcoffset() is not None:
            parsed = ensure_aware(parsed)
            if ensure_aware(anchor_at) <= parsed <= observed_at:
                return parsed
    except (ValueError, OverflowError):
        pass
    return None


def plan_gap(
    observed_at: datetime,
    *,
    anchor_at: datetime | None,
    previous_check_at: datetime | None,
    persisted_checkpoint: str | None,
    threshold: timedelta,
) -> ObservationGap | None:
    """Use committed continuity first, falling back to the sparse anchor."""
    restart = False
    if previous_check_at is None and anchor_at is not None:
        saved_at = checkpoint_time(
            persisted_checkpoint, anchor_at=anchor_at, observed_at=observed_at
        )
        restart = saved_at is not None
        previous_check_at = saved_at or ensure_aware(anchor_at)
    if previous_check_at is None:
        return None
    elapsed = observed_at - previous_check_at
    delayed = elapsed > threshold
    if elapsed > timedelta(0) and (restart or delayed):
        return ObservationGap(previous_check_at, observed_at, delayed)
    return None


def coverage_end_if_stale(
    now: datetime,
    *,
    anchor_at: datetime,
    previous_check_at: datetime | None,
    persisted_checkpoint: str | None,
    threshold: timedelta,
) -> datetime | None:
    """End sparse coverage at the last confirmed check while the worker is stale."""
    if previous_check_at is not None and now - previous_check_at <= threshold:
        return None
    anchor_at = ensure_aware(anchor_at)
    confirmed_at = checkpoint_time(
        persisted_checkpoint, anchor_at=anchor_at, observed_at=now
    )
    boundary = max(
        confirmed_at or previous_check_at or anchor_at,
        anchor_at,
    )
    return boundary if boundary < now else None


def needs_sample(previous: object, current: object, *, gap: bool = False) -> bool:
    """Persist a state transition or an explicit boundary, never a stable poll."""
    return gap or previous != current


def commit_cycle(
    db: Session,
    observed_at: datetime,
    *,
    record_history: Callable[[], T],
    save_checkpoint: Callable[[T, str], None],
) -> T:
    """Keep the timestamp and sparse writes in the same transaction."""
    transaction = db.get_transaction()
    observation = record_history()
    if transaction is not None and db.get_transaction() is not transaction:
        raise RuntimeError("Monitoring transaction was rolled back")
    save_checkpoint(observation, observed_at.isoformat())
    db.commit()
    return observation


def retention_cutoff(now: datetime, days: int) -> datetime:
    return ensure_aware(now) - timedelta(days=days)


def event_view(
    *,
    event_id: str,
    occurred_at: datetime,
    observed_at: datetime,
    component: str,
    event_type: str,
    previous_state: str | None,
    new_state: str | None,
    reason: str | None,
    source: str,
    certainty: str = "confirmed",
) -> dict:
    return {
        "event_id": event_id,
        "occurred_at": ensure_aware(occurred_at).isoformat(),
        "observed_at": ensure_aware(observed_at).isoformat(),
        "component": component,
        "event_type": event_type,
        "previous_state": previous_state,
        "new_state": new_state,
        "reason": reason,
        "source": source,
        "certainty": certainty,
    }
