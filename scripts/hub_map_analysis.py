#!/usr/bin/env python3
"""Fetch Hub Observations, run analyze(), optionally publish analysis-only map layers.

Uses the public Hub and Analysis HTTPS APIs only (no SQLite). Scratch uploads are
ephemeral Analysis overlays — not durable Event history.

Commands:
  analyze  — complete snapshot → AnalysisSnapshot → analyze() → print summary
  publish  — same, then replace the prior scratch object for --dataset
"""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import UTC, datetime
from itertools import combinations
from typing import Any

from hub_event_track import (
    DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
    DEFAULT_EVENT_GAP_SECONDS,
    DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
    DEFAULT_MIN_EARS_FOR_EVENT,
    DEFAULT_MIN_PROMOTE_SCORE,
    MAX_OBSERVATION_PAGE_SIZE,
    AnalysisConfig,
    AnalysisSnapshot,
    HubApiError,
    analyze,
    fetch_all_observation_reports,
    fetch_environmental_context,
    fetch_nodes,
    filter_observations,
    filter_reports_by_event_time,
    format_brief_report,
    format_filter_status,
    installation_distance_m,
    is_sim_ear,
    observation_fields,
    observation_time,
    parse_positive_interval,
    parse_utc_time_range,
    report_time_span,
)
from lib.hub_cli import (
    add_hub_connection_arguments,
    format_hub_http_error,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)
from lib.map_analysis_layer import (
    MAP_DEMO_NAMESPACE,
    IncompleteSnapshotError,
    counts_from_result,
    format_run_counts,
    require_complete_snapshot,
    scratch_object_ids_for_dataset,
    serialize_analysis_map_layer,
)

DEFAULT_TTL_S = 3600
MAX_TTL_S = 7200


class MapAnalysisError(RuntimeError):
    pass


def request_json(
    base_url: str,
    ca_file: str,
    token: str,
    method: str,
    path: str,
    payload: dict[str, Any] | None = None,
) -> Any:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Authorization": f"Bearer {token}"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=data,
        headers=headers,
        method=method,
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=60) as response:
            if response.status == 204 or response.length == 0:
                return None
            return json.load(response)
    except urllib.error.HTTPError as exc:
        raise MapAnalysisError(
            format_hub_http_error(exc.code, exc.read(2048), path=path)
        ) from exc
    except urllib.error.URLError as exc:
        raise MapAnalysisError(f"could not reach Hub/Analysis API: {exc.reason}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise MapAnalysisError(f"invalid JSON for {path}") from exc


def build_analysis_config(args: argparse.Namespace) -> AnalysisConfig:
    return AnalysisConfig(
        event_gap_seconds=args.event_gap_seconds,
        max_association_distance_m=args.max_association_distance_m,
        allow_long_range_sim_ears=bool(args.allow_long_range_sim_ears),
        association_temporal_gap_seconds=args.association_temporal_gap_seconds,
        min_ears_for_event=args.min_ears_for_event,
        min_promote_score=args.min_promote_score,
        include_cross_ear_correlation=True,
        include_cross_ear_geometry=True,
        cross_ear_geometry_detail=False,
        include_site_context=True,
        include_observation_details=False,
        presentation=(
            "list"
            if getattr(args, "detailed", False)
            or getattr(args, "command", "analyze") == "publish"
            else "brief"
        ),
    )


def fetch_and_analyze(
    base_url: str,
    ca_file: str,
    token: str,
    args: argparse.Namespace,
) -> tuple[Any, dict[str, Any], list[str]]:
    """Complete Hub snapshot → optional client filters → analyze().

    Returns (AnalysisResult, fetch_meta, summary_lines).
    """
    node_ids = {item for item in (args.node_id or []) if item}
    fetch_node_id = next(iter(node_ids)) if len(node_ids) == 1 else None
    since, until = parse_utc_time_range(
        getattr(args, "since", None),
        getattr(args, "until", None),
    )

    fetched, fetch_meta = fetch_all_observation_reports(
        base_url,
        ca_file,
        token,
        fetch_node_id,
        page_size=MAX_OBSERVATION_PAGE_SIZE,
        order="oldest",
    )
    require_complete_snapshot(fetch_meta)

    node_filtered_reports = filter_observations(fetched, node_ids=node_ids)
    missing_event_time_count = (
        sum(observation_time(item) is None for item in node_filtered_reports)
        if since is not None or until is not None
        else None
    )
    reports = filter_reports_by_event_time(
        node_filtered_reports,
        since=since,
        until=until,
    )
    time_start, time_end, missing_retained_time_count = report_time_span(reports)

    nodes = fetch_nodes(base_url, ca_file, token)
    reference_times = [
        observation_time(item)
        for item in reports
        if observation_time(item) is not None
    ]
    weather_at = max(reference_times) if reference_times else None
    weather = fetch_environmental_context(
        base_url,
        ca_file,
        token,
        at=weather_at,
        ensure=True,
    )

    config = build_analysis_config(args)
    filter_status_lines = format_filter_status(
        node_ids=node_ids,
        fetched_count=len(fetched),
        filtered_count=len(reports),
        since=since,
        until=until,
        retained_time_span=(
            (time_start, time_end) if since is not None or until is not None else None
        ),
        missing_event_time_count=missing_event_time_count,
    )
    snapshot = AnalysisSnapshot(
        reports=reports,
        nodes=nodes,
        config=config,
        snapshot_id=(
            int(fetch_meta["snapshot_id"])
            if fetch_meta.get("snapshot_id") is not None
            else None
        ),
        environmental_context=weather,
        filter_status_lines=filter_status_lines,
        fetch_meta={
            **fetch_meta,
            "reports_after_node_filter": len(node_filtered_reports),
            "reports_after_filter": len(reports),
            "client_node_ids": sorted(node_ids),
            "client_since_utc": since.isoformat() if since else None,
            "client_until_utc": until.isoformat() if until else None,
            "time_filter_applied": since is not None or until is not None,
            "reports_excluded_missing_event_time": missing_event_time_count or 0,
            "reports_missing_event_time_after_filter": missing_retained_time_count,
        },
    )
    result = analyze(snapshot)
    counts = counts_from_result(
        result,
        reports_fetched=int(fetch_meta.get("fetched_report_count") or len(fetched)),
        reports_after_filter=len(reports),
        observations_after_filter=(
            len(result.correlation.observations)
            if result.correlation is not None
            else 0
        ),
    )
    summary = [
        f"hub_snapshot_id={snapshot.snapshot_id}",
        f"complete_snapshot={fetch_meta.get('complete_snapshot')}",
        f"allow_long_range_sim_ears={config.allow_long_range_sim_ears}",
        *format_run_counts(counts),
    ]
    return result, fetch_meta, summary


def _utc_iso(value: datetime | None, *, empty: str = "unknown") -> str:
    if value is None:
        return empty
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def _compact_map_summary_lines(
    result: Any,
    args: argparse.Namespace,
) -> list[str]:
    snapshot = result.snapshot
    fetch_meta = snapshot.fetch_meta
    reports = snapshot.reports
    reports_fetched = int(fetch_meta.get("fetched_report_count") or len(reports))
    counts = counts_from_result(
        result,
        reports_fetched=reports_fetched,
        reports_after_filter=len(reports),
    )
    start, end, missing_time_count = report_time_span(reports)
    node_ids = sorted(
        {
            fields["node_id"]
            for report in reports
            for fields in [observation_fields(report)]
            if isinstance(fields.get("node_id"), str)
        }
    )
    selected_nodes = sorted(item for item in (args.node_id or []) if item)
    filter_parts = []
    if selected_nodes:
        filter_parts.append(f"Node ID: {', '.join(selected_nodes)}")
    since = fetch_meta.get("client_since_utc")
    until = fetch_meta.get("client_until_utc")
    if since is not None or until is not None:
        filter_parts.append(
            "event_time_utc (inclusive): "
            f"{_utc_iso(datetime.fromisoformat(since)) if since else 'unbounded'} to "
            f"{_utc_iso(datetime.fromisoformat(until)) if until else 'unbounded'}"
        )
    total_count = fetch_meta.get("total_count")
    snapshot_label = (
        f"Hub snapshot {snapshot.snapshot_id}; complete; "
        f"retained total={total_count if total_count is not None else 'unknown'}"
    )
    lines = [
        "Hub Map Analysis — script-local analysis; not Hub-persisted Events.",
        snapshot_label,
        f"Applied filters: {'; '.join(filter_parts) if filter_parts else 'none'}",
        f"Reports: {reports_fetched} fetched; {len(reports)} matched",
        (
            f"Unique observations: {result.unique_observation_count} "
            "(identity: node_id + observation_id); "
            f"assembled: {counts.observations_assembled}"
        ),
        (
            "Matched report event_time_utc span: "
            f"{_utc_iso(start)} to {_utc_iso(end)}; "
            f"matched reports without valid event_time_utc: {missing_time_count}"
        ),
        (
            "Localization outcomes: "
            f"{counts.localizations_published} published; "
            f"{counts.localizations_withheld} withheld"
        ),
    ]
    if fetch_meta.get("time_filter_applied"):
        lines.extend(
            [
                (
                    "Reports excluded for missing/invalid event_time_utc: "
                    f"{fetch_meta.get('reports_excluded_missing_event_time', 0)}"
                ),
                (
                    "Lifecycle caveat: filtering is report-level; observation "
                    "lifecycles may be partial, affecting association and promotion."
                ),
            ]
        )

    installed: dict[str, dict[str, Any]] = {}
    for node_id in node_ids:
        node = snapshot.nodes.get(node_id)
        installation = node.get("installation") if isinstance(node, dict) else None
        if not isinstance(installation, dict):
            continue
        if installation_distance_m(installation, installation) is not None:
            installed[node_id] = installation
    within_radius = 0
    beyond_radius = 0
    unknown_distance = 0
    long_range_sim_pairs = 0
    for first_id, second_id in combinations(node_ids, 2):
        first = installed.get(first_id)
        second = installed.get(second_id)
        distance = (
            installation_distance_m(first, second)
            if first is not None and second is not None
            else None
        )
        if distance is None:
            unknown_distance += 1
        elif distance <= args.max_association_distance_m:
            within_radius += 1
        else:
            beyond_radius += 1
            if (
                args.allow_long_range_sim_ears
                and is_sim_ear(first_id)
                and is_sim_ear(second_id)
            ):
                long_range_sim_pairs += 1
    lines.append(
        f"Site context: {len(installed)}/{len(node_ids)} observed Ears have "
        "valid installation coordinates; site-distance pairs "
        f"within/outside/unknown {args.max_association_distance_m:g}m="
        f"{within_radius}/{beyond_radius}/{unknown_distance} "
        "(context/eligibility only, not association evidence)."
    )
    if args.allow_long_range_sim_ears and long_range_sim_pairs:
        lines.append(
            "Warning: long-range SIM-EAR pairs remain eligible beyond the "
            f"{args.max_association_distance_m:g}m radius "
            f"({long_range_sim_pairs} pair(s)); this is not an association decision."
        )

    weather = snapshot.environmental_context
    if weather is None:
        lines.append("Weather context: unavailable.")
    else:
        availability = weather.get("availability", "unavailable")
        weather_summary = (
            f"availability={availability}; region={weather.get('region_id', '?')}; "
            f"provider={weather.get('provider', '?')}"
        )
        if availability != "unavailable":
            weather_summary += (
                f"; temperature={weather.get('temperature_c', '?')}°C; "
                f"wind={weather.get('wind_speed_ms', '?')}m/s"
            )
        lines.append(
            "Weather context (informational, not association input): "
            f"{weather_summary}."
        )
    return lines


def list_scratch_metadata(
    base_url: str,
    ca_file: str,
    token: str,
    *,
    namespace: str = MAP_DEMO_NAMESPACE,
) -> list[dict[str, Any]]:
    path = (
        "/api/v1/analysis/scratch?"
        + urllib.parse.urlencode(
            {"namespace": namespace, "include_content": "false"}
        )
    )
    body = request_json(base_url, ca_file, token, "GET", path)
    if not isinstance(body, dict):
        raise MapAnalysisError("scratch list response must be an object")
    objects = body.get("objects")
    if not isinstance(objects, list):
        raise MapAnalysisError("scratch list response missing objects array")
    return [item for item in objects if isinstance(item, dict)]


def delete_scratch_object(
    base_url: str,
    ca_file: str,
    token: str,
    object_id: str,
) -> None:
    request_json(
        base_url,
        ca_file,
        token,
        "DELETE",
        f"/api/v1/analysis/scratch/{urllib.parse.quote(object_id, safe='')}",
    )


def replace_dataset_analysis_layer(
    base_url: str,
    ca_file: str,
    token: str,
    *,
    content: dict[str, Any],
    ttl_seconds: int,
    namespace: str = MAP_DEMO_NAMESPACE,
) -> dict[str, Any]:
    """Delete prior objects for content['dataset'], then POST the new analysis layer."""
    dataset = content.get("dataset")
    if not isinstance(dataset, str) or not dataset:
        raise MapAnalysisError("content.dataset is required for publish")

    existing = list_scratch_metadata(
        base_url, ca_file, token, namespace=namespace
    )
    prior_ids = scratch_object_ids_for_dataset(existing, dataset)
    for object_id in prior_ids:
        delete_scratch_object(base_url, ca_file, token, object_id)

    created = request_json(
        base_url,
        ca_file,
        token,
        "POST",
        "/api/v1/analysis/scratch",
        {
            "namespace": namespace,
            "content": content,
            "ttl_seconds": ttl_seconds,
        },
    )
    if not isinstance(created, dict) or "id" not in created:
        raise MapAnalysisError("unexpected scratch create response")

    # Verify no duplicate dataset layers remain after replace.
    after = list_scratch_metadata(base_url, ca_file, token, namespace=namespace)
    remaining = scratch_object_ids_for_dataset(after, dataset)
    if remaining != [created["id"]]:
        raise MapAnalysisError(
            f"dataset {dataset!r} has unexpected scratch objects after replace: "
            f"{remaining!r} (created={created['id']!r})"
        )
    return {
        "deleted_prior_ids": prior_ids,
        "created": created,
        "namespace": namespace,
        "dataset": dataset,
    }


def cmd_analyze(base_url: str, ca_file: str, token: str, args: argparse.Namespace) -> int:
    result, _fetch_meta, summary = fetch_and_analyze(base_url, ca_file, token, args)
    if getattr(args, "detailed", False):
        print("Hub → analyze() summary")
        for line in summary:
            print(f"  {line}")
        print()
        lines = result.lines
    else:
        lines = format_brief_report(
            result.snapshot,
            result.correlation,
            include_per_ear_episodes=True,
            compact_summary_lines=_compact_map_summary_lines(result, args),
        )
    for line in lines:
        print(line)
    return 0


def cmd_publish(base_url: str, ca_file: str, token: str, args: argparse.Namespace) -> int:
    result, _fetch_meta, summary = fetch_and_analyze(base_url, ca_file, token, args)
    print("Hub → analyze() summary")
    for line in summary:
        print(f"  {line}")

    run_id = str(uuid.uuid4())
    run_timestamp = datetime.now(UTC)
    dataset_title = args.dataset_title or args.dataset
    content = serialize_analysis_map_layer(
        result,
        dataset=args.dataset,
        dataset_title=dataset_title,
        run_id=run_id,
        run_timestamp=run_timestamp,
    )
    publish_result = replace_dataset_analysis_layer(
        base_url,
        ca_file,
        token,
        content=content,
        ttl_seconds=args.ttl_seconds,
    )
    created = publish_result["created"]
    print()
    print(
        f"published analysis-only scratch layer "
        f"namespace={publish_result['namespace']!r} "
        f"dataset={publish_result['dataset']!r} "
        f"id={created['id']} "
        f"deleted_prior={len(publish_result['deleted_prior_ids'])} "
        f"ttl_remaining_s={created.get('ttl_remaining_s')} "
        f"run_id={run_id}"
    )
    print(
        "Temporary Analysis scratch data — not durable Event history. "
        f"Select dataset {dataset_title!r} on the Hub map."
    )
    print(
        json.dumps(
            {
                "namespace": publish_result["namespace"],
                "dataset": publish_result["dataset"],
                "run_id": run_id,
                "scratch_id": created["id"],
                "deleted_prior_ids": publish_result["deleted_prior_ids"],
                "provenance": content["provenance"],
                "localizations": len(content["localizations"]),
                "events": len(content["events"]),
                "observations": len(content["observations"]),
            },
            indent=2,
            default=str,
        )
    )
    return 0


def add_shared_analysis_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--node-id",
        action="append",
        default=[],
        metavar="NODE_ID",
        help="Optional client-side Node ID filter (repeatable; applied after complete fetch)",
    )
    parser.add_argument(
        "--since",
        metavar="ISO_DATETIME",
        help=(
            "Optional client-side lower bound on report event_time_utc "
            "(timezone required; applied after complete fetch)"
        ),
    )
    parser.add_argument(
        "--until",
        metavar="ISO_DATETIME",
        help=(
            "Optional client-side upper bound on report event_time_utc "
            "(timezone required; applied after complete fetch)"
        ),
    )
    parser.add_argument(
        "--event-gap-seconds",
        type=float,
        default=DEFAULT_EVENT_GAP_SECONDS,
        help=f"Per-Ear episode gap (default: {DEFAULT_EVENT_GAP_SECONDS:g})",
    )
    parser.add_argument(
        "--max-association-distance-m",
        type=float,
        default=DEFAULT_MAX_ASSOCIATION_DISTANCE_M,
        help=(
            "Cross-EAR association radius "
            f"(default: {DEFAULT_MAX_ASSOCIATION_DISTANCE_M:g})"
        ),
    )
    parser.add_argument(
        "--allow-long-range-sim-ears",
        action="store_true",
        default=False,
        help=(
            "Opt-in: keep SIM-EAR pairs eligible beyond association radius. "
            "Default false for real EAR data."
        ),
    )
    parser.add_argument(
        "--association-temporal-gap-seconds",
        type=float,
        default=DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS,
        help=(
            "Max Observation interval gap for association "
            f"(default: {DEFAULT_ASSOCIATION_TEMPORAL_GAP_SECONDS:g})"
        ),
    )
    parser.add_argument(
        "--min-ears-for-event",
        type=int,
        default=DEFAULT_MIN_EARS_FOR_EVENT,
        help=f"Minimum Ears for Event promotion (default: {DEFAULT_MIN_EARS_FOR_EVENT})",
    )
    parser.add_argument(
        "--min-promote-score",
        type=int,
        default=DEFAULT_MIN_PROMOTE_SCORE,
        help=f"Minimum score to promote an Event (default: {DEFAULT_MIN_PROMOTE_SCORE})",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run Cross-EAR analyze() on a complete Hub Observation snapshot and "
            "optionally publish an analysis-only map overlay to Analysis scratch "
            f"(namespace {MAP_DEMO_NAMESPACE}). Scratch data is temporary."
        )
    )
    add_hub_connection_arguments(parser)
    sub = parser.add_subparsers(dest="command", required=True)

    analyze_parser = sub.add_parser(
        "analyze",
        help=(
            "Fetch complete snapshot and analyze (compact output by default; "
            "no scratch write)"
        ),
    )
    add_shared_analysis_arguments(analyze_parser)
    analyze_parser.add_argument(
        "--detailed",
        action="store_true",
        help="Print the full diagnostic analysis report",
    )

    publish_parser = sub.add_parser(
        "publish",
        help=(
            "Analyze then replace the prior analysis-only scratch object for "
            "--dataset (never clears unrelated map-demo objects)"
        ),
    )
    add_shared_analysis_arguments(publish_parser)
    publish_parser.add_argument(
        "--dataset",
        required=True,
        help="Dataset id for map selector grouping (replaced on re-run)",
    )
    publish_parser.add_argument(
        "--dataset-title",
        help="Display title in the map dataset selector (defaults to --dataset)",
    )
    publish_parser.add_argument(
        "--ttl-seconds",
        type=int,
        default=DEFAULT_TTL_S,
        help=f"Scratch TTL seconds (default {DEFAULT_TTL_S}, max {MAX_TTL_S})",
    )
    return parser


def _validate_shared_args(
    parser: argparse.ArgumentParser, args: argparse.Namespace
) -> None:
    for label, value in (
        ("event gap", args.event_gap_seconds),
        ("max association distance", args.max_association_distance_m),
        ("association temporal gap", args.association_temporal_gap_seconds),
    ):
        try:
            parse_positive_interval(value)
        except ValueError as exc:
            parser.error(f"{label} {exc}")
    if args.min_ears_for_event < 2:
        parser.error("min-ears-for-event must be at least 2")
    if args.min_promote_score < 1:
        parser.error("min-promote-score must be at least 1")
    if args.command == "publish":
        if args.ttl_seconds < 1 or args.ttl_seconds > MAX_TTL_S:
            parser.error(f"ttl-seconds must be between 1 and {MAX_TTL_S}")
        if not args.dataset.strip():
            parser.error("--dataset must be non-empty")


def main(argv: list[str] | None = None) -> int:
    try:
        load_project_env()
    except ValueError as exc:
        print(f"Hub configuration failed: {exc}", file=sys.stderr)
        return 1
    parser = build_parser()
    if argv is None and print_help_without_arguments(parser):
        return 0
    args = parser.parse_args(argv)
    _validate_shared_args(parser, args)
    base_url, token = validate_hub_connection(args, parser)
    try:
        if args.command == "analyze":
            return cmd_analyze(base_url, args.ca, token, args)
        if args.command == "publish":
            return cmd_publish(base_url, args.ca, token, args)
        parser.error(f"unknown command {args.command}")
    except IncompleteSnapshotError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except (MapAnalysisError, HubApiError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
