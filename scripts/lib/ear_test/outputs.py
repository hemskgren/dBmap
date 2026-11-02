"""Write all derived job reports from one shared summary snapshot."""

from __future__ import annotations

from typing import Any

from lib.ear_test.analysis import render_analysis_md
from lib.ear_test.job import JobController
from lib.ear_test.report import render_report_html
from lib.ear_test.store import write_analysis_md, write_report_html, write_summary
from lib.ear_test.summary import build_summary


def write_job_outputs(
    controller: JobController,
    summary: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build one summary and write summary.json, report.html, and analysis.md."""
    summary = summary if summary is not None else build_summary(controller)
    job_id = controller.manifest.job_id
    write_summary(job_id, summary)
    write_report_html(job_id, render_report_html(controller, summary=summary))
    write_analysis_md(job_id, render_analysis_md(controller, summary=summary))
    return summary
