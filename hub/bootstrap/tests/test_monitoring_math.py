from datetime import UTC, datetime, timedelta, timezone

import pytest
from app.monitoring_math import attribute_hold, ensure_aware


def test_ensure_aware_treats_naive_values_as_utc_and_converts_offset_values() -> None:
    naive = datetime(2026, 11, 21, 12, tzinfo=UTC).replace(tzinfo=None)
    offset = timezone(timedelta(hours=1))
    assert ensure_aware(naive) == naive.replace(tzinfo=UTC)
    assert ensure_aware(naive.replace(tzinfo=offset)).isoformat() == (
        "2026-11-21T11:00:00+00:00"
    )


@pytest.mark.parametrize(
    ("state", "expected"),
    [
        ("available", (63, 0, 0)),
        ("degraded", (63, 0, 0)),
        ("unavailable", (0, 63, 0)),
        ("unobserved", (0, 0, 63)),
        ("unknown", (0, 0, 63)),
    ],
)
def test_hold_attributes_only_positive_intervals(state, expected) -> None:
    start = datetime(2026, 11, 21, tzinfo=UTC)
    end = start + timedelta(seconds=63)
    assert attribute_hold(state, start, end) == expected
    assert attribute_hold(state, start, start) == (0, 0, 0)
    assert attribute_hold(state, end, start) == (0, 0, 0)
