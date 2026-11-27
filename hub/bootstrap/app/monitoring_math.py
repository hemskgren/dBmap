"""Pure timestamp and interval operations shared by the monitors."""

from dataclasses import dataclass
from datetime import UTC, datetime, timedelta


def ensure_aware(value: datetime) -> datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=UTC)
    return value.astimezone(UTC)


def attribute_hold(
    state: str, start: datetime, end: datetime
) -> tuple[float, float, float]:
    """Attribute the full interval to the sample state (transition-sparse model)."""
    total = max((end - start).total_seconds(), 0.0)
    if total <= 0:
        return 0.0, 0.0, 0.0
    if state in {"available", "degraded"}:
        return total, 0.0, 0.0
    if state == "unavailable":
        return 0.0, total, 0.0
    return 0.0, 0.0, total


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
