"""CLI tests for hub_map_analysis (mocked Hub/Analysis HTTP)."""

from __future__ import annotations

import importlib.util
import sys
from datetime import UTC, datetime
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import pytest

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))

SPEC = importlib.util.spec_from_file_location(
    "hub_map_analysis", SCRIPTS / "hub_map_analysis.py"
)
assert SPEC is not None and SPEC.loader is not None
hma = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hma)

from lib.map_analysis_layer import IncompleteSnapshotError


def _args(**overrides):
    base = {
        "node_id": [],
        "since": None,
        "until": None,
        "event_gap_seconds": 30.0,
        "max_association_distance_m": 500.0,
        "allow_long_range_sim_ears": False,
        "association_temporal_gap_seconds": 15.0,
        "min_ears_for_event": 2,
        "min_promote_score": 3,
        "dataset": "live-run",
        "dataset_title": None,
        "ttl_seconds": 3600,
        "command": "analyze",
        "detailed": False,
    }
    base.update(overrides)
    return SimpleNamespace(**base)


def test_build_analysis_config_defaults_disable_long_range_sim() -> None:
    config = hma.build_analysis_config(_args())
    assert config.allow_long_range_sim_ears is False


def test_build_analysis_config_opt_in_long_range_sim() -> None:
    config = hma.build_analysis_config(_args(allow_long_range_sim_ears=True))
    assert config.allow_long_range_sim_ears is True


def test_parser_allow_long_range_sim_ears_defaults_false() -> None:
    parser = hma.build_parser()
    args = parser.parse_args(["analyze"])
    assert args.allow_long_range_sim_ears is False
    assert args.detailed is False
    publish_args = parser.parse_args(["publish", "--dataset", "x"])
    assert publish_args.allow_long_range_sim_ears is False
    assert not hasattr(publish_args, "detailed")


def test_parser_detailed_selects_diagnostic_presentation() -> None:
    parser = hma.build_parser()
    args = parser.parse_args(["analyze", "--detailed"])
    assert args.detailed is True
    assert hma.build_analysis_config(args).presentation == "list"
    assert hma.build_analysis_config(_args()).presentation == "brief"


def test_filter_reports_by_event_time_after_fetch() -> None:
    reports = [
        {
            "identity": {"observation_id": "a", "node_id": "EAR-1", "sequence_number": 1},
            "context": {
                "event_time_utc": "2026-11-07T12:00:00+00:00",
                "interval_duration_ms": 1000,
                "emission_kind": "start",
            },
        },
        {
            "identity": {"observation_id": "b", "node_id": "EAR-1", "sequence_number": 2},
            "context": {
                "event_time_utc": "2026-11-07T13:00:00+00:00",
                "interval_duration_ms": 1000,
                "emission_kind": "start",
            },
        },
    ]
    since = datetime(2026, 11, 7, 12, 30, tzinfo=UTC)
    filtered = hma.filter_reports_by_event_time(reports, since=since, until=None)
    assert len(filtered) == 1
    assert filtered[0]["identity"]["observation_id"] == "b"


def test_fetch_and_analyze_rejects_invalid_time_before_api_request() -> None:
    args = _args(since="2026-11-07T10:00:00")
    with (
        patch.object(
            hma,
            "fetch_all_observation_reports",
            side_effect=AssertionError("API must not be called"),
        ),
        pytest.raises(ValueError, match="include a timezone"),
    ):
        hma.fetch_and_analyze("https://hub.test", "ca.crt", "token", args)


def _analysis_reports() -> list[dict]:
    return [
        {
            "observation_id": f"obs-{index}",
            "node_id": f"SIM-EAR-{index:03d}",
            "event_time_utc": f"2026-11-07T10:00:0{index}Z",
            "duration_ms": 10000,
            "classification": {
                "source_family": "vehicle",
                "confidence": 0.7,
            },
            "bearing": {
                "deg": bearing,
                "reference": "node",
                "confidence": 0.8,
            },
        }
        for index, bearing in enumerate((150.0, 210.0, 0.0), start=1)
    ]


def _analysis_nodes() -> dict:
    return {
        "SIM-EAR-001": {
            "installation": {"latitude": 59.3400, "longitude": 18.0500}
        },
        "SIM-EAR-002": {
            "installation": {"latitude": 59.3400, "longitude": 18.0600}
        },
        "SIM-EAR-003": {
            "installation": {"latitude": 59.3480, "longitude": 18.0550}
        },
    }


def test_compact_and_detailed_analysis_have_identical_results(capsys) -> None:
    reports = _analysis_reports()
    fetch_meta = {
        "complete_snapshot": True,
        "fetched_report_count": len(reports),
        "total_count": len(reports),
        "snapshot_id": 12,
    }
    compact_args = _args(
        allow_long_range_sim_ears=True,
        max_association_distance_m=100.0,
        min_ears_for_event=3,
    )
    detailed_args = _args(
        detailed=True,
        allow_long_range_sim_ears=True,
        max_association_distance_m=100.0,
        min_ears_for_event=3,
    )
    with (
        patch.object(
            hma,
            "fetch_all_observation_reports",
            return_value=(reports, fetch_meta),
        ),
        patch.object(hma, "fetch_nodes", return_value=_analysis_nodes()),
        patch.object(
            hma,
            "fetch_environmental_context",
            return_value={
                "availability": "available",
                "region_id": "test",
                "provider": "fixture",
                "temperature_c": 12,
                "wind_speed_ms": 3,
            },
        ),
    ):
        compact_result, _, _ = hma.fetch_and_analyze(
            "https://hub.test", "ca.crt", "token", compact_args
        )
        detailed_result, _, _ = hma.fetch_and_analyze(
            "https://hub.test", "ca.crt", "token", detailed_args
        )

    assert compact_result.snapshot.reports == detailed_result.snapshot.reports
    assert compact_result.correlation == detailed_result.correlation
    assert compact_result.unique_observation_count == detailed_result.unique_observation_count

    with patch.object(
        hma,
        "fetch_and_analyze",
        return_value=(compact_result, fetch_meta, []),
    ):
        assert hma.cmd_analyze("https://hub.test", "ca.crt", "token", compact_args) == 0
    compact_output = capsys.readouterr().out
    assert "Per-Ear activity episodes (not Events)" in compact_output
    assert "Site context:" in compact_output
    assert "Weather context (informational" in compact_output
    assert "Warning: long-range SIM-EAR pairs remain eligible" in compact_output
    assert "Assembled Observations (analysis objects)" not in compact_output
    assert "Join attempt diagnostics" not in compact_output
    assert "Membership stage trace" not in compact_output

    with patch.object(
        hma,
        "fetch_and_analyze",
        return_value=(detailed_result, fetch_meta, ["reports_fetched=3"]),
    ):
        assert hma.cmd_analyze("https://hub.test", "ca.crt", "token", detailed_args) == 0
    detailed_output = capsys.readouterr().out
    assert "Assembled Observations (analysis objects)" in detailed_output
    assert "Ear activity episode A1" in detailed_output


def test_fetch_and_analyze_refuses_incomplete_snapshot() -> None:
    with (
        patch.object(
            hma,
            "fetch_all_observation_reports",
            return_value=(
                [],
                {
                    "complete_snapshot": False,
                    "fetched_report_count": 1,
                    "total_count": 2,
                    "snapshot_id": 5,
                },
            ),
        ),
        pytest.raises(IncompleteSnapshotError, match="incomplete"),
    ):
        hma.fetch_and_analyze(
            "https://hub.test", "ca.crt", "token", _args()
        )


def test_replace_dataset_analysis_layer_deletes_only_matching_dataset() -> None:
    calls: list[tuple[str, str, dict | None]] = []

    def fake_request(base_url, ca_file, token, method, path, payload=None):
        calls.append((method, path, payload))
        if method == "GET":
            # First list: prior objects; second list: after create
            get_calls = [c for c in calls if c[0] == "GET"]
            if len(get_calls) == 1:
                return {
                    "objects": [
                        {
                            "id": "old-target",
                            "metadata": {"dataset": "target", "kind": "map-layer"},
                        },
                        {
                            "id": "keep-heli",
                            "metadata": {"dataset": "helicopter", "kind": "map-layer"},
                        },
                        {
                            "id": "old-target-2",
                            "metadata": {"dataset": "target", "layer": "analysis"},
                        },
                    ],
                    "count": 3,
                }
            return {
                "objects": [
                    {
                        "id": "keep-heli",
                        "metadata": {"dataset": "helicopter", "kind": "map-layer"},
                    },
                    {
                        "id": "new-id",
                        "metadata": {"dataset": "target", "layer": "analysis"},
                    },
                ],
                "count": 2,
            }
        if method == "DELETE":
            return {"deleted": 1}
        if method == "POST":
            assert payload is not None
            assert payload["namespace"] == "map-demo"
            assert payload["content"]["dataset"] == "target"
            assert payload["content"]["layer"] == "analysis"
            assert payload["content"]["role"] == "analysis"
            assert "points" not in payload["content"]
            return {
                "id": "new-id",
                "ttl_remaining_s": 3600,
                "expires_at": "2026-11-07T14:00:00+00:00",
            }
        raise AssertionError(f"unexpected {method} {path}")

    content = {
        "kind": "map-layer",
        "dataset": "target",
        "dataset_title": "Target",
        "layer": "analysis",
        "role": "analysis",
        "observations": [],
        "events": [],
        "localizations": [],
        "provenance": {"run_id": "r1"},
    }
    with patch.object(hma, "request_json", side_effect=fake_request):
        result = hma.replace_dataset_analysis_layer(
            "https://hub.test",
            "ca.crt",
            "token",
            content=content,
            ttl_seconds=3600,
        )

    delete_paths = [path for method, path, _ in calls if method == "DELETE"]
    assert delete_paths == [
        "/api/v1/analysis/scratch/old-target",
        "/api/v1/analysis/scratch/old-target-2",
    ]
    assert "keep-heli" not in "".join(delete_paths)
    assert result["created"]["id"] == "new-id"
    assert result["deleted_prior_ids"] == ["old-target", "old-target-2"]
    # Namespace-wide clear must never be used.
    assert not any(
        method == "DELETE" and path.startswith("/api/v1/analysis/scratch?")
        for method, path, _ in calls
    )


def test_replace_dataset_rejects_duplicate_after_publish() -> None:
    def fake_request(base_url, ca_file, token, method, path, payload=None):
        if method == "GET":
            return {
                "objects": [
                    {"id": "new-id", "metadata": {"dataset": "target"}},
                    {"id": "dup", "metadata": {"dataset": "target"}},
                ],
                "count": 2,
            }
        if method == "POST":
            return {"id": "new-id", "ttl_remaining_s": 10}
        if method == "DELETE":
            return {"deleted": 1}
        raise AssertionError(method)

    with (
        patch.object(hma, "request_json", side_effect=fake_request),
        pytest.raises(hma.MapAnalysisError, match="unexpected scratch objects"),
    ):
        hma.replace_dataset_analysis_layer(
            "https://hub.test",
            "ca.crt",
            "token",
            content={
                "dataset": "target",
                "layer": "analysis",
                "role": "analysis",
            },
            ttl_seconds=60,
        )


def test_cmd_publish_wires_serializer_and_replace() -> None:
    from hub_event_track import AnalysisConfig, AnalysisSnapshot
    from lib.cross_ear_event_correlation import CorrelationResult

    snapshot = AnalysisSnapshot(
        reports=[],
        nodes={},
        config=AnalysisConfig(allow_long_range_sim_ears=False),
        snapshot_id=11,
        fetch_meta={
            "complete_snapshot": True,
            "fetched_report_count": 0,
            "total_count": 0,
        },
    )
    fake_result = SimpleNamespace(
        snapshot=snapshot,
        correlation=CorrelationResult(
            observations=[],
            candidates=[],
            events=[],
            rejected=[],
            expired=[],
            parameters={},
        ),
        report_count=0,
        unique_observation_count=0,
        lines=["ok"],
    )

    with (
        patch.object(
            hma,
            "fetch_and_analyze",
            return_value=(fake_result, snapshot.fetch_meta, ["reports_fetched=0"]),
        ),
        patch.object(
            hma,
            "replace_dataset_analysis_layer",
            return_value={
                "deleted_prior_ids": ["old"],
                "created": {"id": "new", "ttl_remaining_s": 100},
                "namespace": "map-demo",
                "dataset": "live-run",
            },
        ) as replace,
    ):
        code = hma.cmd_publish(
            "https://hub.test", "ca.crt", "token", _args(dataset="live-run")
        )
    assert code == 0
    content = replace.call_args.kwargs["content"]
    assert content["layer"] == "analysis"
    assert content["role"] == "analysis"
    assert content["dataset"] == "live-run"
    assert "points" not in content
    assert content["provenance"]["analysis_config"]["allow_long_range_sim_ears"] is False
    assert content["provenance"]["snapshot_id"] == 11
