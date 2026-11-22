"""Pure timestamp and interval operations shared by the monitors."""

from datetime import UTC, datetime


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
