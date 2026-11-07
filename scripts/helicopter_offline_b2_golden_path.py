#!/usr/bin/env python3
"""Run the offline helicopter_east_west → B2 golden path and print a report.

No MQTT, Hub API, Analysis container, or map integration.

Example (from repository root, Hub bootstrap venv):

  hub/bootstrap/.venv/bin/python scripts/helicopter_offline_b2_golden_path.py
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

from lib.helicopter_offline_golden_path import (
    GoldenPathParams,
    analyze_reports,
    event_membership_signature,
    format_golden_path_report,
    localization_signature,
    measurement_fingerprint,
    run_golden_path,
)


def main() -> int:
    params = GoldenPathParams()
    first = run_golden_path(params)
    # Determinism: re-analyze the same collected snapshot.
    second_analysis = analyze_reports(
        first.reports,
        first.nodes,
        params,
    )
    # Regeneration: new UUIDs, equivalent measurement content.
    regenerated = run_golden_path(params)

    for line in format_golden_path_report(first):
        print(line)

    same_snapshot = (
        event_membership_signature(first.analysis_result)
        == event_membership_signature(second_analysis)
        and localization_signature(first.analysis_result)
        == localization_signature(second_analysis)
    )
    print(
        "determinism same-snapshot reanalyze: "
        f"{'PASS' if same_snapshot else 'FAIL'}"
    )
    content_equal = measurement_fingerprint(
        first.reports
    ) == measurement_fingerprint(regenerated.reports)
    print(
        "determinism regenerated measurement content (excl. observation_id): "
        f"{'PASS' if content_equal else 'FAIL'}"
    )
    membership_shape = [
        (tuple(sorted(ears)), score)
        for _event_id, ears, _obs, score in event_membership_signature(
            first.analysis_result
        )
    ]
    regenerated_shape = [
        (tuple(sorted(ears)), score)
        for _event_id, ears, _obs, score in event_membership_signature(
            regenerated.analysis_result
        )
    ]
    print(
        "determinism regenerated membership shape (ears+score): "
        f"{'PASS' if membership_shape == regenerated_shape else 'FAIL'}"
    )
    return 0 if same_snapshot and content_equal else 1


if __name__ == "__main__":
    raise SystemExit(main())
