"""Self-contained HTML report for an EAR test job (descriptive only)."""

from __future__ import annotations

import html
import json
from typing import Any

from lib.ear_test.job import JobController
from lib.ear_test.store import load_observations
from lib.ear_test.summary import build_summary


def render_report_html(
    controller: JobController,
    summary: dict[str, Any] | None = None,
) -> str:
    """Render report.html. Pass the same ``summary`` written to summary.json."""
    summary = summary if summary is not None else build_summary(controller)
    observations = load_observations(controller.manifest.job_id)
    manifest = controller.manifest.to_dict()
    periods = [period.to_dict() for period in controller.periods]
    pauses = [pause.to_dict() for pause in controller.pauses]
    disagree_rows = [
        row
        for row in observations
        if (row.get("association") or {}).get("received_event_disagree")
    ]
    firmware_json = json.dumps(
        (manifest.get("metadata") or {}).get("firmware_version_by_ear") or {},
        indent=2,
    )
    summary_json = json.dumps(summary, indent=2)
    completeness = summary["collection"]["completeness"]
    completeness_class = "ok" if completeness == "complete" else "warn"
    completeness_notes = " ".join(
        html.escape(note) for note in summary["collection"].get("completeness_notes") or []
    )

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <title>EAR test {html.escape(controller.manifest.job_id)}</title>
  <style>
    :root {{
      --ink: #172033;
      --muted: #5b6b82;
      --line: #d8e0eb;
      --bg: #f7f9fc;
      --card: #fff;
      --warn: #9a4f00;
      --ok: #1f6b3a;
    }}
    body {{
      margin: 0; padding: 1.25rem 1.5rem 3rem;
      font: 15px/1.45 system-ui, sans-serif;
      color: var(--ink); background: var(--bg);
    }}
    h1, h2, h3 {{ margin: 0 0 0.5rem; }}
    h1 {{ font-size: 1.35rem; }}
    h2 {{ font-size: 1.1rem; margin-top: 1.5rem; }}
    p, li {{ color: var(--muted); }}
    .banner {{
      padding: 0.75rem 1rem; border: 1px solid var(--line);
      background: var(--card); border-radius: 8px; margin-bottom: 1rem;
    }}
    .warn {{ border-color: #e0b56a; color: var(--warn); }}
    .ok {{ border-color: #9dceb0; color: var(--ok); }}
    .grid {{ display: grid; gap: 0.75rem; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); }}
    .stat {{ background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 0.7rem 0.85rem; }}
    .stat strong {{ display: block; font-size: 1.15rem; }}
    .stat span {{ color: var(--muted); font-size: 0.85rem; }}
    table {{ width: 100%; border-collapse: collapse; background: var(--card); }}
    th, td {{ border: 1px solid var(--line); padding: 0.4rem 0.5rem; text-align: left; vertical-align: top; font-size: 0.88rem; }}
    th {{ background: #eef2f7; }}
    tr.disagree {{ background: #fff6e8; }}
    details {{ margin: 0.35rem 0; }}
    pre {{
      margin: 0; padding: 0.65rem; background: #0f172a; color: #e2e8f0;
      overflow: auto; border-radius: 6px; font-size: 0.78rem;
    }}
    .filters {{ display: flex; flex-wrap: wrap; gap: 0.6rem; margin: 0.6rem 0 0.8rem; }}
    label {{ font-size: 0.9rem; color: var(--muted); }}
    select {{ margin-left: 0.25rem; }}
  </style>
</head>
<body>
  <h1>EAR test job {html.escape(controller.manifest.job_id)}</h1>
  <p>Descriptive report only. Observations are Hub-received data; periods are intentional experiment windows. No START/PERIODIC/END or acoustic-episode conclusions.</p>

  <div class="banner {completeness_class}">
    Collection: <strong>{html.escape(str(completeness))}</strong>.
    SQLite reconcile attempted={summary['collection']['sqlite_reconcile_attempted']},
    succeeded={summary['collection']['sqlite_reconcile_succeeded']}.
    {completeness_notes}
  </div>

  <div class="grid">
    <div class="stat"><strong>{html.escape(str(manifest.get('status')))}</strong><span>Job status</span></div>
    <div class="stat"><strong>{summary['observation_count_total']}</strong><span>Observations</span></div>
    <div class="stat"><strong>{manifest.get('period_count')}</strong><span>Configured periods</span></div>
    <div class="stat"><strong>{summary['received_event_disagree_count']}</strong><span>Received≠event phase</span></div>
  </div>

  <h2>Job configuration</h2>
  <table>
    <tr><th>Field</th><th>Value</th></tr>
    <tr><td>Ears</td><td>{html.escape(", ".join(manifest.get("node_ids") or []))}</td></tr>
    <tr><td>Period / inter-period pause / final collection / progression</td><td>{manifest.get("period_seconds")}s / {manifest.get("pause_seconds")}s / {manifest.get("final_collection_pause_seconds", 0)}s / {html.escape(str(manifest.get("progression")))}</td></tr>
    <tr><td>Started / ended</td><td>{html.escape(str(manifest.get("started_at_utc")))} → {html.escape(str(manifest.get("ended_at_utc")))}</td></tr>
    <tr><td>test_name</td><td>{html.escape(str((manifest.get("metadata") or {}).get("test_name")))}</td></tr>
    <tr><td>description</td><td>{html.escape(str((manifest.get("metadata") or {}).get("description")))}</td></tr>
    <tr><td>environment</td><td>{html.escape(str((manifest.get("metadata") or {}).get("environment")))}</td></tr>
    <tr><td>operator_notes</td><td>{html.escape(str((manifest.get("metadata") or {}).get("operator_notes")))}</td></tr>
    <tr><td>firmware_version_by_ear</td><td><pre>{html.escape(firmware_json)}</pre></td></tr>
  </table>

  <h2>Active periods</h2>
  <table>
    <tr><th>Period</th><th>Status</th><th>Planned start→end</th><th>Actual start→end</th><th>Obs (by received)</th><th>Duration deviation (s)</th></tr>
    {"".join(_period_row(item, summary) for item in periods)}
  </table>

  <h2>Pauses (not active periods)</h2>
  <p>Inter-period pauses separate consecutive active periods. The final collection pause follows the last active period so trailing Hub Observations can be polled; it is not an experiment window.</p>
  <table>
    <tr><th>Pause</th><th>Kind</th><th>Status</th><th>Planned (s)</th><th>Actual start→end</th><th>Obs (by received)</th></tr>
    {"".join(_pause_row(item, summary) for item in pauses) or "<tr><td colspan='6'>No pauses configured</td></tr>"}
  </table>

  <h2>Per-EAR summary</h2>
  <table>
    <tr><th>EAR</th><th>Count</th><th>duration_ms median</th><th>signal_level_dbfs median</th><th>inter-received median (s)</th></tr>
    {"".join(_ear_row(node_id, data) for node_id, data in (summary.get("ears") or {}).items())}
  </table>

  <h2>Observations</h2>
  <div class="filters">
    <label>EAR<select id="filter-ear"><option value="">All</option></select></label>
    <label>Period (received)<select id="filter-period"><option value="">All</option></select></label>
    <label><input id="filter-disagree" type="checkbox"/> Only received≠event</label>
  </div>
  <table id="obs-table">
    <thead>
      <tr>
        <th>node</th><th>seq</th><th>event_time</th><th>received_time</th>
        <th>duration_ms</th><th>level</th>
        <th>phase(recv)</th><th>period(recv)</th><th>period(event)</th><th>payload</th>
      </tr>
    </thead>
    <tbody>
      {"".join(_obs_row(row) for row in observations)}
    </tbody>
  </table>

  <h2>Received vs event disagreement ({len(disagree_rows)})</h2>
  <p>Useful for latency and interval-start semantics. Not an error by itself.</p>
  <table>
    <tr><th>node</th><th>seq</th><th>received phase/period</th><th>event phase/period</th><th>note</th></tr>
    {"".join(_disagree_row(row) for row in disagree_rows) or "<tr><td colspan='5'>None</td></tr>"}
  </table>

  <h2>Raw summary JSON</h2>
  <pre>{html.escape(summary_json)}</pre>

  <script>
    const earSelect = document.getElementById('filter-ear');
    const periodSelect = document.getElementById('filter-period');
    const disagreeOnly = document.getElementById('filter-disagree');
    const ears = new Set();
    const periods = new Set();
    document.querySelectorAll('#obs-table tbody tr').forEach((row) => {{
      ears.add(row.dataset.ear || '');
      periods.add(row.dataset.period || '');
    }});
    [...ears].filter(Boolean).sort().forEach((value) => {{
      const opt = document.createElement('option'); opt.value = value; opt.textContent = value; earSelect.append(opt);
    }});
    [...periods].filter(Boolean).sort().forEach((value) => {{
      const opt = document.createElement('option'); opt.value = value; opt.textContent = value; periodSelect.append(opt);
    }});
    function applyFilters() {{
      const ear = earSelect.value;
      const period = periodSelect.value;
      const onlyDisagree = disagreeOnly.checked;
      document.querySelectorAll('#obs-table tbody tr').forEach((row) => {{
        const okEar = !ear || row.dataset.ear === ear;
        const okPeriod = !period || row.dataset.period === period;
        const okDisagree = !onlyDisagree || row.dataset.disagree === '1';
        row.hidden = !(okEar && okPeriod && okDisagree);
      }});
    }}
    earSelect.addEventListener('change', applyFilters);
    periodSelect.addEventListener('change', applyFilters);
    disagreeOnly.addEventListener('change', applyFilters);
  </script>
</body>
</html>
"""


def _period_row(period: dict[str, Any], summary: dict[str, Any]) -> str:
    timing = next(
        (item for item in summary.get("periods") or [] if item["period_id"] == period["period_id"]),
        {},
    )
    planned = (
        f"{html.escape(str(period.get('planned_start_utc')))} → "
        f"{html.escape(str(period.get('planned_end_utc')))}"
    )
    actual = (
        f"{html.escape(str(period.get('actual_start_utc')))} → "
        f"{html.escape(str(period.get('actual_end_utc')))}"
    )
    return (
        "<tr>"
        f"<td>{html.escape(period['period_id'])}</td>"
        f"<td>{html.escape(period['status'])}</td>"
        f"<td>{planned}</td>"
        f"<td>{actual}</td>"
        f"<td>{timing.get('observation_count_by_received', 0)}</td>"
        f"<td>{html.escape(str(timing.get('duration_deviation_s')))}</td>"
        "</tr>"
    )


def _pause_row(pause: dict[str, Any], summary: dict[str, Any]) -> str:
    timing = next(
        (
            item
            for item in summary.get("pauses") or []
            if item.get("pause_id") == pause.get("pause_id")
        ),
        {},
    )
    actual = (
        f"{html.escape(str(pause.get('actual_start_utc')))} → "
        f"{html.escape(str(pause.get('actual_end_utc')))}"
    )
    kind = pause.get("kind") or ""
    kind_label = (
        "final collection"
        if kind == "final_collection"
        else "inter-period"
        if kind == "inter_period"
        else kind
    )
    return (
        "<tr>"
        f"<td>{html.escape(str(pause.get('pause_id')))}</td>"
        f"<td>{html.escape(kind_label)}</td>"
        f"<td>{html.escape(str(pause.get('status')))}</td>"
        f"<td>{html.escape(str(pause.get('planned_seconds')))}</td>"
        f"<td>{actual}</td>"
        f"<td>{timing.get('observation_count_by_received', 0)}</td>"
        "</tr>"
    )


def _ear_row(node_id: str, data: dict[str, Any]) -> str:
    dur = data.get("duration_ms") or {}
    lvl = data.get("signal_level_dbfs") or {}
    gap = data.get("inter_received_seconds") or {}
    return (
        "<tr>"
        f"<td>{html.escape(node_id)}</td>"
        f"<td>{data.get('observation_count')}</td>"
        f"<td>{html.escape(str(dur.get('median')))}</td>"
        f"<td>{html.escape(str(lvl.get('median')))}</td>"
        f"<td>{html.escape(str(gap.get('median')))}</td>"
        "</tr>"
    )


def _obs_row(row: dict[str, Any]) -> str:
    payload = row.get("payload") or {}
    assoc = row.get("association") or {}
    disagree = "1" if assoc.get("received_event_disagree") else "0"
    cls = "disagree" if disagree == "1" else ""
    return (
        f"<tr class='{cls}' data-ear='{html.escape(str(row.get('node_id') or ''))}' "
        f"data-period='{html.escape(str(assoc.get('period_id_by_received') or ''))}' "
        f"data-disagree='{disagree}'>"
        f"<td>{html.escape(str(row.get('node_id')))}</td>"
        f"<td>{html.escape(str(row.get('sequence_number')))}</td>"
        f"<td>{html.escape(str(row.get('event_time_utc')))}</td>"
        f"<td>{html.escape(str(row.get('received_time_utc')))}</td>"
        f"<td>{html.escape(str(payload.get('duration_ms')))}</td>"
        f"<td>{html.escape(str(payload.get('signal_level_dbfs')))}</td>"
        f"<td>{html.escape(str(assoc.get('phase_by_received')))}</td>"
        f"<td>{html.escape(str(assoc.get('period_id_by_received')))}</td>"
        f"<td>{html.escape(str(assoc.get('period_id_by_event')))}</td>"
        "<td><details><summary>JSON</summary>"
        f"<pre>{html.escape(json.dumps(payload, indent=2))}</pre></details></td>"
        "</tr>"
    )


def _disagree_row(row: dict[str, Any]) -> str:
    assoc = row.get("association") or {}
    return (
        "<tr>"
        f"<td>{html.escape(str(row.get('node_id')))}</td>"
        f"<td>{html.escape(str(row.get('sequence_number')))}</td>"
        f"<td>{html.escape(str(assoc.get('phase_by_received')))} / {html.escape(str(assoc.get('period_id_by_received')))}</td>"
        f"<td>{html.escape(str(assoc.get('phase_by_event')))} / {html.escape(str(assoc.get('period_id_by_event')))}</td>"
        f"<td>{html.escape(str(assoc.get('association_note')))}</td>"
        "</tr>"
    )
