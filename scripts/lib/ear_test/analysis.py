"""AI-friendly Markdown analysis report for an EAR test job.

Generated from the same persisted job data and the same ``summary`` dict used
for ``summary.json`` / ``report.html``. Descriptive only — no emit-kind or
acoustic-episode interpretation.
"""

from __future__ import annotations

import math
from typing import Any

from lib.ear_test.job import JobController
from lib.ear_test.models import format_utc
from lib.ear_test.store import load_observations
from lib.ear_test.summary import build_summary


def render_analysis_md(
    controller: JobController,
    summary: dict[str, Any] | None = None,
) -> str:
    """Render analysis.md. Pass the same ``summary`` written to summary.json."""
    summary = summary if summary is not None else build_summary(controller)
    observations = load_observations(controller.manifest.job_id)
    manifest = controller.manifest
    meta = manifest.metadata
    collection = summary.get("collection") or {}

    lines: list[str] = []
    lines.append(f"# EAR test analysis — `{manifest.job_id}`")
    lines.append("")
    lines.append(
        "Self-contained descriptive summary for AI-assisted analysis. "
        "Companion files from the same job directory: `summary.json` (machine stats), "
        "`observations.jsonl` (full payloads), `report.html` (human UI). "
        "All three report formats are derived from the same persisted artifacts."
    )
    lines.append("")
    lines.append("## How to read this document")
    lines.append("")
    lines.append(
        "- **Measured facts** — recorded configuration, Hub reception timestamps, "
        "stored Observation fields, collection health flags."
    )
    lines.append(
        "- **Calculated statistics** — counts, min/max/mean/median, interval gaps "
        "derived from those facts."
    )
    lines.append(
        "- **Open questions** — items that need further investigation; not conclusions."
    )
    lines.append(
        "- This runner does **not** label Observations as START / PERIODIC / END "
        "and does **not** infer continuous acoustic events."
    )
    lines.append("")

    lines.append("## 1. Test identity and configuration (fact)")
    lines.append("")
    lines.append(f"- **job_id:** `{manifest.job_id}`")
    lines.append(f"- **status:** `{manifest.status}`")
    lines.append(f"- **test_name:** {_md(meta.test_name)}")
    lines.append(f"- **description:** {_md(meta.description)}")
    lines.append(f"- **environment:** {_md(meta.environment)}")
    lines.append(f"- **operator_notes:** {_md(meta.operator_notes)}")
    lines.append(f"- **Ears:** {', '.join(f'`{n}`' for n in manifest.node_ids)}")
    lines.append(
        f"- **period_seconds / periods / pause_seconds / final_collection_pause_seconds:** "
        f"{manifest.period_seconds:g} / {manifest.period_count} / "
        f"{manifest.pause_seconds:g} / {manifest.final_collection_pause_seconds:g}"
    )
    lines.append(f"- **progression:** `{manifest.progression}`")
    lines.append(f"- **association_policy_version:** {manifest.association_policy_version}")
    lines.append(f"- **runner_version:** `{manifest.runner_version}`")
    lines.append(f"- **hub_base_url:** `{manifest.hub_base_url}`")
    lines.append(f"- **started_at_utc:** `{format_utc(manifest.started_at_utc)}`")
    lines.append(f"- **ended_at_utc:** `{format_utc(manifest.ended_at_utc)}`")
    lines.append(f"- **summary.generated_at_utc:** `{summary.get('generated_at_utc')}`")
    lines.append("")

    lines.append("## 2. Participating Ears and firmware (fact)")
    lines.append("")
    if meta.firmware_version_by_ear:
        for node_id in manifest.node_ids:
            version = meta.firmware_version_by_ear.get(node_id)
            lines.append(
                f"- `{node_id}`: firmware `{version}`"
                if version
                else f"- `{node_id}`: firmware not provided"
            )
        for node_id, version in sorted(meta.firmware_version_by_ear.items()):
            if node_id not in manifest.node_ids:
                lines.append(f"- `{node_id}` (metadata only): firmware `{version}`")
    else:
        lines.append("- Firmware versions were not supplied for this job.")
    lines.append("")

    lines.append("## 3. Test periods and timing (fact + calculated)")
    lines.append("")
    lines.append(
        "| period_id | status | actual_start_utc | actual_end_utc | "
        "planned_s | actual_s | deviation_s | obs_by_received |"
    )
    lines.append("|---|---|---|---|---:|---:|---:|---:|")
    timing_by_id = {item["period_id"]: item for item in summary.get("periods") or []}
    for period in controller.periods:
        timing = timing_by_id.get(period.period_id) or {}
        lines.append(
            "| {pid} | {status} | {start} | {end} | {planned} | {actual} | {dev} | {obs} |".format(
                pid=period.period_id,
                status=period.status,
                start=format_utc(period.actual_start_utc) or "—",
                end=format_utc(period.actual_end_utc) or "—",
                planned=_num(timing.get("planned_duration_s")),
                actual=_num(timing.get("actual_duration_s")),
                dev=_num(timing.get("duration_deviation_s")),
                obs=timing.get("observation_count_by_received", 0),
            )
        )
    lines.append("")
    lines.append(
        "Period windows are intentional experiment intervals. They are not detector episodes."
    )
    lines.append("")
    lines.append("### Pauses (not active periods)")
    lines.append("")
    lines.append(
        "| pause_id | kind | status | planned_s | actual_start_utc | actual_end_utc | "
        "obs_by_received |"
    )
    lines.append("|---|---|---|---:|---|---|---:|")
    for pause in summary.get("pauses") or []:
        lines.append(
            "| {pid} | {kind} | {status} | {planned} | {start} | {end} | {obs} |".format(
                pid=pause.get("pause_id"),
                kind=pause.get("kind"),
                status=pause.get("status"),
                planned=_num(pause.get("planned_duration_s")),
                start=pause.get("actual_start_utc") or "—",
                end=pause.get("actual_end_utc") or "—",
                obs=pause.get("observation_count_by_received", 0),
            )
        )
    if not (summary.get("pauses") or []):
        lines.append("| — | — | — | — | — | — | 0 |")
    lines.append("")
    lines.append(
        "`inter_period` sits between consecutive active periods. "
        "`final_collection` is a trailing poll window after the last active period "
        "(not an experiment window). An Observation received then may still have "
        "`event_time_utc` from an earlier interval."
    )
    lines.append("")

    lines.append("## 4. Observation counts (calculated from persisted jsonl)")
    lines.append("")
    lines.append(f"- **Total Observations:** {summary.get('observation_count_total', 0)}")
    lines.append("- **Per EAR:**")
    for node_id, count in (summary.get("observation_count_by_ear") or {}).items():
        lines.append(f"  - `{node_id}`: {count}")
    lines.append("- **By phase (`received_time_utc` association):**")
    for phase, count in sorted((summary.get("phase_by_received_counts") or {}).items()):
        lines.append(f"  - `{phase}`: {count}")
    lines.append("- **By period id (`received_time_utc` association):**")
    for period_id, count in sorted((summary.get("period_by_received_counts") or {}).items()):
        lines.append(f"  - `{period_id}`: {count}")
    lines.append(
        f"- **received vs event period/phase disagreements:** "
        f"{summary.get('received_event_disagree_count', 0)}"
    )
    lines.append("")

    lines.append("## 5. Timing patterns (calculated)")
    lines.append("")
    ears = summary.get("ears") or {}
    if not ears:
        lines.append("- No Observations stored for interval statistics.")
    else:
        for node_id, ear in sorted(ears.items()):
            gap = ear.get("inter_received_seconds") or {}
            lines.append(f"### `{node_id}`")
            lines.append("")
            lines.append(
                f"- Inter-`received_time` interval seconds: "
                f"n={gap.get('n')}, min={_num(gap.get('min'))}, "
                f"max={_num(gap.get('max'))}, mean={_num(gap.get('mean'))}, "
                f"median={_num(gap.get('median'))}"
            )
            lines.append("")
    lines.append(
        "Intervals use Hub `received_time_utc` order. They are not the same as "
        "`duration_ms` (Ear-declared interval length on each Observation)."
    )
    lines.append("")

    lines.append("## 6. Signal level and duration_ms (calculated from payload fields)")
    lines.append("")
    if not ears:
        lines.append("- No Observation payloads available.")
    else:
        lines.append(
            "| EAR | n | duration_ms median | duration_ms min/max | "
            "level_dbfs median | level_dbfs min/max |"
        )
        lines.append("|---|---:|---:|---|---:|---|")
        for node_id, ear in sorted(ears.items()):
            dur = ear.get("duration_ms") or {}
            lvl = ear.get("signal_level_dbfs") or {}
            lines.append(
                "| `{ear}` | {n} | {dmed} | {dmin} / {dmax} | {lmed} | {lmin} / {lmax} |".format(
                    ear=node_id,
                    n=ear.get("observation_count", 0),
                    dmed=_num(dur.get("median")),
                    dmin=_num(dur.get("min")),
                    dmax=_num(dur.get("max")),
                    lmed=_num(lvl.get("median")),
                    lmin=_num(lvl.get("min")),
                    lmax=_num(lvl.get("max")),
                )
            )
        lines.append("")
        lines.append(
            "`duration_ms` and `signal_level_dbfs` are copied from Observation payloads. "
            "No emit-kind inference is applied."
        )
    lines.append("")

    lines.append("## 7. Collection limitations and anomalies (fact)")
    lines.append("")
    lines.append(f"- **completeness:** `{collection.get('completeness')}`")
    lines.append(
        f"- **sqlite_reconcile_attempted:** `{collection.get('sqlite_reconcile_attempted')}`"
    )
    lines.append(
        f"- **sqlite_reconcile_succeeded:** `{collection.get('sqlite_reconcile_succeeded')}`"
    )
    if collection.get("sqlite_reconcile_error"):
        lines.append(f"- **sqlite_reconcile_error:** `{collection['sqlite_reconcile_error']}`")
    lines.append(f"- **http_poll_count:** {collection.get('http_poll_count')}")
    for note in collection.get("completeness_notes") or []:
        lines.append(f"- {note}")
    for note in summary.get("notes") or []:
        lines.append(f"- {note}")
    lines.append("")

    lines.append("## 8. What was actually observed (fact)")
    lines.append("")
    if not observations:
        lines.append("- No Observations were stored for this job window.")
    else:
        lines.append(
            f"- Stored **{len(observations)}** Observation(s) for selected Ear(s) "
            f"with Hub `received_time_utc` inside the job window."
        )
        lines.append(
            "- Chronological sample (up to 20 rows by `received_time_utc`):"
        )
        lines.append("")
        lines.append(
            "| received_time_utc | node_id | seq | event_time_utc | "
            "duration_ms | signal_level_dbfs | phase_recv | period_recv | disagree |"
        )
        lines.append("|---|---|---:|---|---:|---:|---|---|---|")
        ordered = sorted(
            observations,
            key=lambda row: str(row.get("received_time_utc") or ""),
        )
        for row in ordered[:20]:
            payload = row.get("payload") or {}
            assoc = row.get("association") or {}
            lines.append(
                "| {recv} | `{node}` | {seq} | {event} | {dur} | {lvl} | {phase} | {period} | {dis} |".format(
                    recv=row.get("received_time_utc") or "—",
                    node=row.get("node_id") or "—",
                    seq=row.get("sequence_number") if row.get("sequence_number") is not None else "—",
                    event=row.get("event_time_utc") or "—",
                    dur=_num(payload.get("duration_ms")),
                    lvl=_num(payload.get("signal_level_dbfs")),
                    phase=assoc.get("phase_by_received") or "—",
                    period=assoc.get("period_id_by_received") or "—",
                    dis="yes" if assoc.get("received_event_disagree") else "no",
                )
            )
        if len(ordered) > 20:
            lines.append("")
            lines.append(
                f"- … {len(ordered) - 20} additional row(s) omitted here; "
                "see `observations.jsonl`."
            )
        disagree_rows = [
            row
            for row in observations
            if (row.get("association") or {}).get("received_event_disagree")
        ]
        lines.append("")
        lines.append(
            f"- Rows where `received_time_utc` and `event_time_utc` map to different "
            f"periods/phases: **{len(disagree_rows)}** "
            "(useful for latency / interval-start analysis; not an error by itself)."
        )
    lines.append("")

    lines.append("## 9. Open questions / further investigation")
    lines.append("")
    questions = _open_questions(controller, summary, observations)
    for item in questions:
        lines.append(f"- {item}")
    lines.append("")

    lines.append("## Companion files")
    lines.append("")
    lines.append("| File | Role |")
    lines.append("|---|---|")
    lines.append("| `report.html` | Human visual inspection |")
    lines.append("| `analysis.md` | This AI-oriented narrative |")
    lines.append("| `summary.json` | Same statistics in machine-readable form |")
    lines.append(
        "| `observations.jsonl` | Original payloads + reception/association metadata |"
    )
    lines.append("| `manifest.json` / `periods.json` | Job config and period timeline |")
    lines.append("")
    return "\n".join(lines) + "\n"


def _open_questions(
    controller: JobController,
    summary: dict[str, Any],
    observations: list[dict[str, Any]],
) -> list[str]:
    questions: list[str] = []
    collection = summary.get("collection") or {}
    if collection.get("completeness") != "complete":
        questions.append(
            "Collection is marked potentially incomplete — confirm whether missing "
            "Observations matter for the intended analysis."
        )
    if summary.get("received_event_disagree_count"):
        questions.append(
            "Some Observations disagree between received-time and event-time period "
            "association — investigate latency vs Ear interval-start semantics if relevant."
        )
    pause_count = (summary.get("phase_by_received_counts") or {}).get("pause", 0)
    if pause_count:
        questions.append(
            f"{pause_count} Observation(s) associated to inter-period pause phases — "
            "expected for ongoing Ear emissions spanning pauses, or unexpected "
            "relative to the test plan?"
        )
    final_count = (summary.get("phase_by_received_counts") or {}).get(
        "final_collection", 0
    )
    if final_count:
        questions.append(
            f"{final_count} Observation(s) associated to final_collection — trailing "
            "Hub receives after the last active period; event_time_utc may still "
            "refer to an earlier interval."
        )
    idle_count = (summary.get("phase_by_received_counts") or {}).get("job_idle", 0)
    if idle_count:
        questions.append(
            f"{idle_count} Observation(s) associated to job_idle — check job start/end "
            "boundaries and period actual times."
        )
    if controller.manifest.status == "interrupted":
        questions.append(
            "Job ended interrupted — later periods may be skipped; do not treat the "
            "run as a complete configured schedule."
        )
    if not observations:
        questions.append(
            "No Observations in the job window — was the Ear quiet, offline, or was "
            "the window too short?"
        )
    if not questions:
        questions.append(
            "No automatic anomalies flagged beyond the measured counts above. "
            "Further acoustic or detector interpretation is out of scope for this report."
        )
    return questions


def _md(value: Any) -> str:
    if value is None or value == "":
        return "—"
    text = str(value).replace("\n", " ").strip()
    return text if text else "—"


def _num(value: Any) -> str:
    if value is None:
        return "—"
    if isinstance(value, float):
        if math.isnan(value):
            return "—"
        if abs(value - round(value)) < 1e-9:
            return str(round(value))
        return f"{value:.3f}".rstrip("0").rstrip(".")
    return str(value)
