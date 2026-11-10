"""Hub Web Observation Statistics: lifecycle aggregation and retained-history contracts."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEB_ROOT = ROOT / "web"
HUB_INDEX = WEB_ROOT / "index.html"
JS_HELPER = WEB_ROOT / "observation-lifecycle.js"
PY_HELPER = WEB_ROOT / "observation_lifecycle.py"


def _load_web_lifecycle():
    spec = importlib.util.spec_from_file_location(
        "dbmap_web_observation_lifecycle",
        PY_HELPER,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _load_hub_event_track():
    scripts = str(ROOT / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    lib = str(ROOT / "scripts" / "lib")
    if lib not in sys.path:
        sys.path.insert(0, lib)
    spec = importlib.util.spec_from_file_location(
        "dbmap_hub_event_track_for_web_obs",
        ROOT / "scripts" / "hub_event_track.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _hierarchical_report(
    *,
    observation_id: str,
    node_id: str,
    sequence_number: int | None,
    event_time_utc: str,
    emission_kind: str | None,
    interval_duration_ms: float = 1000.0,
    episode_duration_ms: float | None = None,
) -> dict:
    identity = {
        "observation_id": observation_id,
        "node_id": node_id,
        "hub_id": "local-01",
    }
    if sequence_number is not None:
        identity["sequence_number"] = sequence_number
    context = {
        "event_time_utc": event_time_utc,
        "capture_timestamp_monotonic_us": 0,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": interval_duration_ms,
        "episode_duration_ms": (
            interval_duration_ms if episode_duration_ms is None else episode_duration_ms
        ),
    }
    if emission_kind is not None:
        context["emission_kind"] = emission_kind
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": identity,
        "context": context,
        "acoustic": {
            "signal_level_dbfs": -40.0,
            "measurements": None,
            "signature": None,
            "classification": None,
        },
        "spatial": {"bearing": None, "position": None},
    }


def _multi_device_fixture() -> list[dict]:
    completed_a = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    open_a = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    completed_b = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    periodic_only = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    return [
        _hierarchical_report(
            observation_id=completed_a,
            node_id="EAR-001",
            sequence_number=1,
            event_time_utc="2026-11-10T10:00:00Z",
            emission_kind="start",
            interval_duration_ms=2020.0,
            episode_duration_ms=2020.0,
        ),
        _hierarchical_report(
            observation_id=completed_a,
            node_id="EAR-001",
            sequence_number=2,
            event_time_utc="2026-11-10T10:00:01Z",
            emission_kind="periodic",
            interval_duration_ms=1000.0,
            episode_duration_ms=3020.0,
        ),
        _hierarchical_report(
            observation_id=completed_a,
            node_id="EAR-001",
            sequence_number=3,
            event_time_utc="2026-11-10T10:00:03Z",
            emission_kind="end",
            interval_duration_ms=3060.0,
            episode_duration_ms=5080.0,
        ),
        _hierarchical_report(
            observation_id=open_a,
            node_id="EAR-001",
            sequence_number=4,
            event_time_utc="2026-11-10T10:05:00Z",
            emission_kind="start",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
        _hierarchical_report(
            observation_id=open_a,
            node_id="EAR-001",
            sequence_number=5,
            event_time_utc="2026-11-10T10:05:05Z",
            emission_kind="periodic",
            interval_duration_ms=1000.0,
            episode_duration_ms=6000.0,
        ),
        _hierarchical_report(
            observation_id=completed_b,
            node_id="EAR-002",
            sequence_number=10,
            event_time_utc="2026-11-10T10:00:00Z",
            emission_kind="start",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
        _hierarchical_report(
            observation_id=completed_b,
            node_id="EAR-002",
            sequence_number=11,
            event_time_utc="2026-11-10T10:00:02Z",
            emission_kind="end",
            interval_duration_ms=1500.0,
            episode_duration_ms=2500.0,
        ),
        _hierarchical_report(
            observation_id=periodic_only,
            node_id="EAR-002",
            sequence_number=12,
            event_time_utc="2026-11-10T10:10:00Z",
            emission_kind="periodic",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
    ]


def test_web_lifecycle_matches_cli_summary_semantics() -> None:
    web = _load_web_lifecycle()
    cli = _load_hub_event_track()
    observations = _multi_device_fixture()

    web_summary = web.summarize_observation_lifecycles(observations)
    cli_summary = cli.summarize_observation_lifecycles(observations)

    assert web_summary.combined.unique_observations == cli_summary.combined.unique_observations == 3
    assert web_summary.combined.completed_observations == cli_summary.combined.completed_observations == 2
    assert web_summary.combined.open_observations == cli_summary.combined.open_observations == 1
    assert web_summary.combined.shortest_completed_ms == cli_summary.combined.shortest_completed_ms == 2500.0
    assert web_summary.combined.longest_completed_ms == cli_summary.combined.longest_completed_ms == 5080.0
    assert web_summary.combined.total_completed_ms == cli_summary.combined.total_completed_ms == 7580.0
    assert web_summary.report_count == cli_summary.report_count == 8

    web_by_device = {item.node_id: item for item in web_summary.devices}
    cli_by_device = {item.node_id: item for item in cli_summary.devices}
    assert set(web_by_device) == set(cli_by_device) == {"EAR-001", "EAR-002"}
    assert web_by_device["EAR-001"].unique_observations == 2
    assert web_by_device["EAR-001"].completed_observations == 1
    assert web_by_device["EAR-001"].open_observations == 1
    assert web_by_device["EAR-002"].unique_observations == 1
    assert any("without START/COMPLETE" in note for note in web_summary.quality_notes)


def test_periodic_never_increments_unique_and_complete_kind_counts() -> None:
    web = _load_web_lifecycle()
    periodic_only = [
        _hierarchical_report(
            observation_id="dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            node_id="EAR-002",
            sequence_number=1,
            event_time_utc="2026-11-10T10:10:00Z",
            emission_kind="periodic",
        )
    ]
    summary = web.summarize_observation_lifecycles(periodic_only)
    assert summary.combined.unique_observations == 0
    assert summary.combined.completed_observations == 0
    assert any("without START/COMPLETE" in note for note in summary.quality_notes)

    complete_only = [
        _hierarchical_report(
            observation_id="ffffffff-ffff-4fff-8fff-ffffffffffff",
            node_id="EAR-004",
            sequence_number=1,
            event_time_utc="2026-11-10T12:00:00Z",
            emission_kind="complete",
            episode_duration_ms=4000.0,
        )
    ]
    completed = web.summarize_observation_lifecycles(complete_only)
    assert completed.combined.unique_observations == 1
    assert completed.combined.completed_observations == 1
    assert completed.combined.open_observations == 0
    assert completed.combined.total_completed_ms == 4000.0


def test_missing_episode_duration_excluded_from_duration_stats() -> None:
    web = _load_web_lifecycle()
    oid = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-003",
            sequence_number=1,
            event_time_utc="2026-11-10T11:00:00Z",
            emission_kind="start",
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-003",
            sequence_number=2,
            event_time_utc="2026-11-10T11:00:10Z",
            emission_kind="end",
            episode_duration_ms=-1.0,
        ),
    ]
    # Force unusable END duration.
    observations[1]["context"]["episode_duration_ms"] = None
    summary = web.summarize_observation_lifecycles(observations)
    assert summary.combined.unique_observations == 1
    assert summary.combined.completed_observations == 1
    assert summary.combined.completed_with_duration == 0
    assert summary.combined.total_completed_ms == 0.0
    assert web.duration_cell(summary.combined, "total") == "n/a"
    assert any("episode_duration_ms" in note for note in summary.quality_notes)


def test_raw_report_counts_differ_from_unique_lifecycles() -> None:
    web = _load_web_lifecycle()
    observations = _multi_device_fixture()
    report_counts = web.count_reports_by_node(observations)
    summary = web.summarize_observation_lifecycles(observations)

    assert report_counts["EAR-001"] == 5
    assert report_counts["EAR-002"] == 3
    by_device = {item.node_id: item for item in summary.devices}
    assert by_device["EAR-001"].unique_observations == 2
    assert by_device["EAR-002"].unique_observations == 1
    assert report_counts["EAR-001"] != by_device["EAR-001"].unique_observations
    assert sum(report_counts.values()) == summary.report_count == 8


def test_snapshot_completeness_and_dedupe() -> None:
    web = _load_web_lifecycle()
    reports = _multi_device_fixture()
    # Duplicate delivery of one report key.
    reports_with_dup = [*reports, reports[0]]

    complete_reports, complete_meta = web.finalize_observation_snapshot(
        reports=reports,
        total_count=8,
        total_pages=1,
        snapshot_id=42,
        order="oldest",
    )
    assert len(complete_reports) == 8
    assert complete_meta["complete_snapshot"] is True
    assert complete_meta["snapshot_id"] == 42
    assert complete_meta["duplicate_delivery_dropped"] == 0

    partial_reports, partial_meta = web.finalize_observation_snapshot(
        reports=reports[:3],
        total_count=8,
        total_pages=2,
        snapshot_id=42,
        order="oldest",
    )
    assert len(partial_reports) == 3
    assert partial_meta["complete_snapshot"] is False
    assert "completeness_warning" in partial_meta

    deduped, dedupe_meta = web.finalize_observation_snapshot(
        reports=reports_with_dup,
        total_count=8,
        total_pages=2,
        snapshot_id=99,
        order="oldest",
    )
    assert len(deduped) == 8
    assert dedupe_meta["duplicate_delivery_dropped"] == 1
    assert dedupe_meta["complete_snapshot"] is True

    # Multi-page without snapshot_id cannot be complete.
    _, no_snap = web.finalize_observation_snapshot(
        reports=reports,
        total_count=8,
        total_pages=2,
        snapshot_id=None,
        order="oldest",
    )
    assert no_snap["complete_snapshot"] is False


def test_duration_formatting_matches_cli() -> None:
    web = _load_web_lifecycle()
    cli = _load_hub_event_track()
    for value in (2500.0, 5080.0, 400.0, 120000.0):
        assert web.format_observation_duration_ms(value) == cli.format_duration_ms(value)


def test_nginx_allows_observation_lifecycle_js_when_base_path_empty() -> None:
    """Empty DBMAP_PUBLIC_BASE_PATH: only_base would 404 /observation-lifecycle.js."""
    template = (ROOT / "nginx" / "default.conf.template").read_text(encoding="utf-8")
    assert "observation-lifecycle\\.js" in template
    assert "display-time\\.js" in template
    assert "display-config\\.js" in template
    asset_route = template.index("observation-lifecycle\\.js")
    only_base = template.index("(?<only_base>")
    assert asset_route < only_base


def test_observation_view_wires_statistics_separately_from_recent_100() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    js = JS_HELPER.read_text(encoding="utf-8")

    assert "observation-lifecycle.js" in html
    assert 'id="observation-retained-stats"' in html
    assert "<details id=\"observation-retained-stats\"" in html
    assert 'id="refresh-observation-stats"' in html
    assert "Observation Statistics" in html
    assert "Complete retained Observation history" in html
    assert "Loads only when you expand this panel" in html
    assert "recent-100" in html or "recent 100" in html
    assert "in recent 100" in html
    assert "page_size=100&order=newest" in html
    assert "fetchAllObservationReportsSnapshot" in html
    assert "refreshObservationStatistics" in html
    assert "observationStatsState" in html
    assert "requireSnapshot: true" in html
    assert "order: 'oldest'" in html
    assert "summarizeObservationLifecycles" in html
    assert "Incomplete snapshot" in html
    assert "maximum 100);" not in html
    assert "Showing ${observations.length} most recent" not in html

    # Statistics load only on expand / Refresh — not on Observations tab open or timeline refresh.
    assert "refreshObservationStatistics({ force: true })" in html
    assert "Retained-history fetch is on-demand only" in html
    observations_tab = html.split("if (activeTab === 'observations')", 1)[1].split(
        "function renderUsers", 1
    )[0]
    assert "refreshObservationStatistics" not in observations_tab
    timeline_refresh = html.split("async function refreshObservations", 1)[1].split(
        "async function formatCounts", 1
    )[0]
    assert "fetchAllObservationReportsSnapshot" not in timeline_refresh
    assert "page_size=100" in timeline_refresh

    assert "function summarizeObservationLifecycles" in js
    assert "function finalizeObservationSnapshot" in js
    assert "function countReportsByNode" in js
    assert PY_HELPER.is_file()
