"""Observation lifecycle summary helpers for Hub Web (Python twin of JS).

Keep in sync with ``web/observation-lifecycle.js``. Semantics match
``scripts/hub_event_track.py`` summary: unique START-backed lifecycles over a
frozen multi-page Hub snapshot.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import UTC, datetime
from typing import Any


def observation_fields(observation: dict[str, Any]) -> dict[str, Any]:
    identity = observation.get("identity")
    context = observation.get("context")
    acoustic = observation.get("acoustic")
    spatial = observation.get("spatial")
    if isinstance(identity, dict) and isinstance(context, dict):
        classification = None
        bearing = None
        signal = None
        if isinstance(acoustic, dict):
            classification = acoustic.get("classification")
            signal = acoustic.get("signal_level_dbfs")
        if isinstance(spatial, dict):
            bearing = spatial.get("bearing")
        return {
            "observation_id": identity.get("observation_id"),
            "node_id": identity.get("node_id"),
            "sequence_number": identity.get("sequence_number"),
            "event_time_utc": context.get("event_time_utc"),
            "duration_ms": context.get("interval_duration_ms"),
            "episode_duration_ms": context.get("episode_duration_ms"),
            "emission_kind": context.get("emission_kind"),
            "classification": classification,
            "bearing": bearing,
            "signal_level_dbfs": signal,
        }
    return observation


def observation_time(observation: dict[str, Any]) -> datetime | None:
    fields = observation_fields(observation)
    value = fields.get("event_time_utc")
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(UTC)


def observation_report_key(observation: dict[str, Any]) -> str | None:
    fields = observation_fields(observation)
    node_id = fields.get("node_id")
    sequence = fields.get("sequence_number")
    if isinstance(node_id, str) and isinstance(sequence, int) and not isinstance(
        sequence, bool
    ):
        return f"{node_id}:{sequence}"
    oid = fields.get("observation_id")
    return oid if isinstance(oid, str) else None


def emission_kind_token(value: Any) -> str:
    if not isinstance(value, str):
        return "unknown"
    token = value.strip().lower()
    if token in {"start", "periodic", "end", "complete"}:
        return token
    return "unknown"


def finite_nonnegative_ms(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if not math.isfinite(value) or value < 0:
        return None
    return float(value)


def valid_sequence_number(value: Any) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def format_observation_duration_ms(duration_ms: float) -> str:
    if duration_ms < 1000:
        return f"{duration_ms:.0f} ms"
    seconds = duration_ms / 1000.0
    if seconds < 60:
        return f"{duration_ms:.0f} ms ({seconds:.1f}s)"
    minutes = seconds / 60.0
    return f"{duration_ms:.0f} ms ({minutes:.1f} min)"


def count_reports_by_node(reports: list[dict[str, Any]]) -> dict[str, int]:
    """Raw MQTT report counts per node_id (recent-window filter semantics)."""
    counts: dict[str, int] = {}
    for item in reports:
        node_id = observation_fields(item).get("node_id")
        if isinstance(node_id, str) and node_id:
            counts[node_id] = counts.get(node_id, 0) + 1
    return counts


def dedupe_observation_reports(
    reports: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], int]:
    deduped: list[dict[str, Any]] = []
    seen: set[str] = set()
    dropped = 0
    for row in reports:
        key = observation_report_key(row)
        if key is None:
            deduped.append(row)
            continue
        if key in seen:
            dropped += 1
            continue
        seen.add(key)
        deduped.append(row)
    return deduped, dropped


def finalize_observation_snapshot(
    *,
    reports: list[dict[str, Any]],
    total_count: int,
    total_pages: int,
    snapshot_id: int | None,
    order: str = "oldest",
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Apply delivery de-dupe and Hub snapshot completeness checks."""
    deduped, dropped = dedupe_observation_reports(reports)
    fetched_count = len(deduped)
    complete = fetched_count == total_count and (
        total_pages <= 1 or snapshot_id is not None
    )
    meta: dict[str, Any] = {
        "fetched_report_count": fetched_count,
        "total_count": total_count,
        "total_pages": total_pages,
        "complete_snapshot": complete,
        "duplicate_delivery_dropped": dropped,
        "order": order,
        "snapshot_id": snapshot_id,
        "snapshot_freeze": "Observation.id <= snapshot_id (Hub); later ingest ids excluded",
    }
    if not complete:
        meta["completeness_warning"] = (
            f"fetched {fetched_count} reports but Hub total_count={total_count} "
            f"for snapshot_id={snapshot_id!r}"
        )
    return deduped, meta


@dataclass
class DeviceObservationStats:
    node_id: str
    unique_observations: int = 0
    completed_observations: int = 0
    open_observations: int = 0
    shortest_completed_ms: float | None = None
    longest_completed_ms: float | None = None
    total_completed_ms: float = 0.0
    completed_with_duration: int = 0
    quality_notes: list[str] = field(default_factory=list)


@dataclass
class ObservationLifecycleSummary:
    devices: list[DeviceObservationStats]
    combined: DeviceObservationStats
    report_count: int
    oldest_report_event_time: datetime | None = None
    fetched_at: datetime | None = None
    quality_notes: list[str] = field(default_factory=list)


def summarize_observation_lifecycles(
    observations: list[dict[str, Any]],
    *,
    fetched_at: datetime | None = None,
) -> ObservationLifecycleSummary:
    """Aggregate per-device and combined observation lifecycle statistics."""
    groups: dict[tuple[str, str], dict[str, Any]] = {}
    group_order: list[tuple[str, str]] = []
    quality_notes: list[str] = []
    skipped_missing_identity = 0
    oldest_report_event_time: datetime | None = None

    for item in observations:
        fields = observation_fields(item)
        event_stamp = observation_time(item)
        if event_stamp is not None and (
            oldest_report_event_time is None or event_stamp < oldest_report_event_time
        ):
            oldest_report_event_time = event_stamp
        node_id = fields.get("node_id")
        observation_id = fields.get("observation_id")
        if not isinstance(node_id, str) or not node_id:
            skipped_missing_identity += 1
            continue
        if not isinstance(observation_id, str) or not observation_id:
            skipped_missing_identity += 1
            continue
        key = (node_id, observation_id)
        group = groups.get(key)
        if group is None:
            group = {
                "node_id": node_id,
                "observation_id": observation_id,
                "counts": {
                    "start": 0,
                    "periodic": 0,
                    "end": 0,
                    "complete": 0,
                    "unknown": 0,
                },
                "terminal_candidates": [],
            }
            groups[key] = group
            group_order.append(key)
        kind = emission_kind_token(fields.get("emission_kind"))
        group["counts"][kind] += 1
        if kind in {"end", "complete"}:
            group["terminal_candidates"].append(
                {
                    "kind": kind,
                    "sequence": valid_sequence_number(fields.get("sequence_number")),
                    "episode_duration_ms": finite_nonnegative_ms(
                        fields.get("episode_duration_ms")
                    ),
                    "event_time": event_stamp,
                }
            )

    if skipped_missing_identity:
        quality_notes.append(
            f"skipped {skipped_missing_identity} report(s) missing node_id "
            "or observation_id"
        )

    per_device: dict[str, DeviceObservationStats] = {}
    combined = DeviceObservationStats(node_id="ALL")
    no_start_count = 0
    duplicate_start_count = 0
    duplicate_end_count = 0
    missing_episode_ms_count = 0

    def ensure_device(node_id: str) -> DeviceObservationStats:
        stats = per_device.get(node_id)
        if stats is None:
            stats = DeviceObservationStats(node_id=node_id)
            per_device[node_id] = stats
        return stats

    def pick_terminal_episode_ms(
        candidates: list[dict[str, Any]],
    ) -> float | None:
        if not candidates:
            return None
        ranked = sorted(
            candidates,
            key=lambda row: (
                row["sequence"] is None,
                row["sequence"] if row["sequence"] is not None else -1,
                row["event_time"] is None,
                row["event_time"] or datetime.min.replace(tzinfo=UTC),
            ),
        )
        chosen = ranked[-1]
        return chosen["episode_duration_ms"]

    for key in group_order:
        group = groups[key]
        counts = group["counts"]
        device = ensure_device(group["node_id"])
        has_start = counts["start"] > 0 or counts["complete"] > 0
        has_end = counts["end"] > 0 or counts["complete"] > 0
        if counts["start"] > 1:
            duplicate_start_count += 1
        if counts["end"] > 1 or (
            counts["end"] > 0 and counts["complete"] > 0
        ) or counts["complete"] > 1:
            duplicate_end_count += 1
        if not has_start:
            no_start_count += 1
            continue

        device.unique_observations += 1
        combined.unique_observations += 1

        if has_start and has_end:
            device.completed_observations += 1
            combined.completed_observations += 1
            duration_ms = pick_terminal_episode_ms(group["terminal_candidates"])
            if duration_ms is None:
                missing_episode_ms_count += 1
                continue
            device.completed_with_duration += 1
            combined.completed_with_duration += 1
            device.total_completed_ms += duration_ms
            combined.total_completed_ms += duration_ms
            if (
                device.shortest_completed_ms is None
                or duration_ms < device.shortest_completed_ms
            ):
                device.shortest_completed_ms = duration_ms
            if (
                device.longest_completed_ms is None
                or duration_ms > device.longest_completed_ms
            ):
                device.longest_completed_ms = duration_ms
            if (
                combined.shortest_completed_ms is None
                or duration_ms < combined.shortest_completed_ms
            ):
                combined.shortest_completed_ms = duration_ms
            if (
                combined.longest_completed_ms is None
                or duration_ms > combined.longest_completed_ms
            ):
                combined.longest_completed_ms = duration_ms
        else:
            device.open_observations += 1
            combined.open_observations += 1

    if no_start_count:
        quality_notes.append(
            f"{no_start_count} observation_id group(s) had PERIODIC/END/unknown "
            "reports without START/COMPLETE (excluded from unique count)"
        )
    if duplicate_start_count:
        quality_notes.append(
            f"{duplicate_start_count} observation_id group(s) had duplicate START "
            "reports (counted once)"
        )
    if duplicate_end_count:
        quality_notes.append(
            f"{duplicate_end_count} observation_id group(s) had duplicate "
            "END/COMPLETE reports (duration taken from latest sequence)"
        )
    if missing_episode_ms_count:
        quality_notes.append(
            f"{missing_episode_ms_count} completed observation(s) lacked usable "
            "END/COMPLETE episode_duration_ms (excluded from duration stats)"
        )

    devices = [per_device[node_id] for node_id in sorted(per_device)]
    return ObservationLifecycleSummary(
        devices=devices,
        combined=combined,
        report_count=len(observations),
        oldest_report_event_time=oldest_report_event_time,
        fetched_at=fetched_at,
        quality_notes=quality_notes,
    )


def duration_cell(stats: DeviceObservationStats, which: str) -> str:
    if which == "shortest":
        value = stats.shortest_completed_ms
    elif which == "longest":
        value = stats.longest_completed_ms
    else:
        value = stats.total_completed_ms if stats.completed_with_duration else None
    if value is None:
        return "n/a"
    return format_observation_duration_ms(value)
