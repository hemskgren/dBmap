"""Descriptive statistics for EAR test jobs (no detector interpretation)."""

from __future__ import annotations

from collections import Counter, defaultdict
from statistics import mean, median
from typing import Any

from lib.ear_test.job import JobController
from lib.ear_test.models import format_utc, parse_utc, utc_now
from lib.ear_test.store import load_observations


def build_summary(controller: JobController) -> dict[str, Any]:
    rows = load_observations(controller.manifest.job_id)
    by_ear: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        node_id = str(row.get("node_id") or "unknown")
        by_ear[node_id].append(row)

    disagree = sum(
        1
        for row in rows
        if (row.get("association") or {}).get("received_event_disagree")
    )
    phase_counts = Counter(
        (row.get("association") or {}).get("phase_by_received") or "unknown"
        for row in rows
    )
    period_counts = Counter(
        (row.get("association") or {}).get("period_id_by_received") or "none"
        for row in rows
    )

    ear_summaries = {}
    for node_id, items in sorted(by_ear.items()):
        durations = [
            float(item["payload"]["duration_ms"])
            for item in items
            if isinstance(item.get("payload"), dict)
            and isinstance(item["payload"].get("duration_ms"), (int, float))
        ]
        levels = [
            float(item["payload"]["signal_level_dbfs"])
            for item in items
            if isinstance(item.get("payload"), dict)
            and isinstance(item["payload"].get("signal_level_dbfs"), (int, float))
        ]
        received_times = sorted(
            t
            for t in (parse_utc(item.get("received_time_utc")) for item in items)
            if t is not None
        )
        gaps = [
            (received_times[i] - received_times[i - 1]).total_seconds()
            for i in range(1, len(received_times))
        ]
        ear_summaries[node_id] = {
            "observation_count": len(items),
            "duration_ms": _stats(durations),
            "signal_level_dbfs": _stats(levels),
            "inter_received_seconds": _stats(gaps),
        }

    period_timing = []
    for period in controller.periods:
        planned = None
        actual = None
        deviation_s = None
        if period.planned_start_utc and period.planned_end_utc:
            planned = (
                period.planned_end_utc - period.planned_start_utc
            ).total_seconds()
        if period.actual_start_utc and period.actual_end_utc:
            actual = (period.actual_end_utc - period.actual_start_utc).total_seconds()
        if planned is not None and actual is not None:
            deviation_s = actual - planned
        period_timing.append(
            {
                "period_id": period.period_id,
                "status": period.status,
                "planned_duration_s": planned,
                "actual_duration_s": actual,
                "duration_deviation_s": deviation_s,
                "observation_count_by_received": period_counts.get(period.period_id, 0),
                "planned_start_utc": format_utc(period.planned_start_utc),
                "planned_end_utc": format_utc(period.planned_end_utc),
                "actual_start_utc": format_utc(period.actual_start_utc),
                "actual_end_utc": format_utc(period.actual_end_utc),
            }
        )

    pause_timing = []
    for pause in controller.pauses:
        planned = pause.planned_seconds
        actual = None
        if pause.actual_start_utc and pause.actual_end_utc:
            actual = (pause.actual_end_utc - pause.actual_start_utc).total_seconds()
        pause_timing.append(
            {
                "pause_id": pause.pause_id,
                "kind": pause.kind,
                "status": pause.status,
                "planned_duration_s": planned,
                "actual_duration_s": actual,
                "planned_start_utc": format_utc(pause.planned_start_utc),
                "planned_end_utc": format_utc(pause.planned_end_utc),
                "actual_start_utc": format_utc(pause.actual_start_utc),
                "actual_end_utc": format_utc(pause.actual_end_utc),
                "after_period_index": pause.after_period_index,
                "before_period_index": pause.before_period_index,
                "observation_count_by_received": phase_counts.get(
                    "final_collection"
                    if pause.kind == "final_collection"
                    else "pause",
                    0,
                )
                if pause.kind == "final_collection"
                else None,
            }
        )
    # Inter-period observation counts: phase is shared ("pause"); report per-window
    # by re-scanning associations against pause bounds when available.
    for item in pause_timing:
        if item["kind"] != "inter_period":
            continue
        start = parse_utc(item.get("actual_start_utc"))
        end = parse_utc(item.get("actual_end_utc"))
        if start is None:
            item["observation_count_by_received"] = 0
            continue
        count = 0
        for row in rows:
            assoc = row.get("association") or {}
            if assoc.get("phase_by_received") != "pause":
                continue
            received = parse_utc(row.get("received_time_utc"))
            if received is None:
                continue
            if received < start:
                continue
            if end is not None and received >= end:
                continue
            count += 1
        item["observation_count_by_received"] = count

    return {
        "job_id": controller.manifest.job_id,
        "status": controller.manifest.status,
        "generated_at_utc": format_utc(utc_now()),
        "collection": controller.manifest.collection.to_dict(),
        "metadata": controller.manifest.metadata.to_dict(),
        "observation_count_total": len(rows),
        "observation_count_by_ear": {
            node_id: len(items) for node_id, items in sorted(by_ear.items())
        },
        "phase_by_received_counts": dict(phase_counts),
        "period_by_received_counts": dict(period_counts),
        "received_event_disagree_count": disagree,
        "ears": ear_summaries,
        "periods": period_timing,
        "pauses": pause_timing,
        "configured_pause_seconds": controller.manifest.pause_seconds,
        "configured_final_collection_pause_seconds": (
            controller.manifest.final_collection_pause_seconds
        ),
        "notes": [
            "Counts describe Observations received by the Hub during the job.",
            "No START/PERIODIC/END or acoustic-episode conclusions are made.",
            "final_collection is a trailing poll window after the last active period; "
            "it is not an active test period. event_time_utc may still refer to an "
            "earlier interval.",
            *controller.manifest.collection.completeness_notes,
        ],
    }


def _stats(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {"n": 0, "min": None, "max": None, "mean": None, "median": None}
    return {
        "n": len(values),
        "min": min(values),
        "max": max(values),
        "mean": mean(values),
        "median": median(values),
    }
