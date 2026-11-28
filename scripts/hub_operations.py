#!/usr/bin/env python3
"""Read Operations monitoring and diagnostics through the Hub HTTPS API."""

from __future__ import annotations

import argparse
import json
import math
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from lib.hub_cli import (
    HubCliError,
    add_hub_connection_arguments,
    format_display_datetime,
    format_display_now,
    load_project_env,
    summarize_hub_http_error_body,
    validate_hub_connection,
)

DEFAULT_PAGE_SIZE = 200
MAX_PAGE_SIZE = 500
DEFAULT_WATCH_INTERVAL = 15.0
OPERATIONS_PREFIX = "/api/v1/operations/"


class OperationsApiError(RuntimeError):
    pass


def api_get(base_url: str, ca_file: str, admin_token: str, path: str) -> Any:
    if not path.startswith(OPERATIONS_PREFIX):
        raise ValueError("Operations CLI requests must use the Operations API")
    request = urllib.request.Request(
        f"{base_url}{path}",
        headers={"Authorization": f"Bearer {admin_token}"},
    )
    try:
        context = ssl.create_default_context(cafile=ca_file)
        with urllib.request.urlopen(request, context=context, timeout=15) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        detail = summarize_hub_http_error_body(exc.read(1024))
        if exc.code in (401, 403):
            message = f"authentication/authorization failed (HTTP {exc.code})"
        elif exc.code == 503:
            message = "Operations is disabled or unavailable (HTTP 503)"
        else:
            message = f"Operations API {path} returned HTTP {exc.code}"
        if detail:
            message = f"{message}: {detail}"
        raise OperationsApiError(message) from exc
    except urllib.error.URLError as exc:
        raise OperationsApiError(
            f"connection error reaching Operations API {path}: {exc.reason}"
        ) from exc
    except (OSError, ssl.SSLError) as exc:
        raise OperationsApiError(
            f"connection or TLS error reaching Operations API {path}: {exc}"
        ) from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise OperationsApiError(
            f"Operations API returned invalid JSON for {path}"
        ) from exc


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive integer") from exc
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def bounded_page_size(value: str) -> int:
    parsed = positive_int(value)
    if parsed > MAX_PAGE_SIZE:
        raise argparse.ArgumentTypeError(f"must be at most {MAX_PAGE_SIZE}")
    return parsed


def positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive number") from exc
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive number")
    return parsed


def require_object(payload: Any, label: str) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise OperationsApiError(f"Operations {label} response must be a JSON object")
    return payload


def _require_int(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise OperationsApiError(f"Operations response {label} must be an integer")
    return value


def parse_collection(
    payload: Any, *, item_key: str
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    envelope = require_object(payload, "collection")
    items = envelope.get(item_key)
    pagination = envelope.get("pagination")
    if not isinstance(items, list) or not all(isinstance(item, dict) for item in items):
        raise OperationsApiError(f"Operations response missing {item_key} array")
    if not isinstance(pagination, dict):
        raise OperationsApiError("Operations response missing pagination object")
    order = pagination.get("order")
    if order not in ("newest", "oldest"):
        raise OperationsApiError("Operations pagination.order is invalid")
    snapshot_id = pagination.get("snapshot_id")
    if snapshot_id is not None and (
        not isinstance(snapshot_id, int) or isinstance(snapshot_id, bool)
    ):
        raise OperationsApiError(
            "Operations pagination.snapshot_id must be an integer or null"
        )
    meta = {
        "total_count": _require_int(
            pagination.get("total_count"), "pagination.total_count"
        ),
        "page": _require_int(pagination.get("page"), "pagination.page"),
        "page_size": _require_int(
            pagination.get("page_size"), "pagination.page_size"
        ),
        "total_pages": _require_int(
            pagination.get("total_pages"), "pagination.total_pages"
        ),
        "order": order,
        "snapshot_id": snapshot_id,
    }
    if meta["page"] < 1 or meta["page_size"] < 1 or meta["total_count"] < 0:
        raise OperationsApiError("Operations response has invalid pagination values")
    expected_pages = math.ceil(meta["total_count"] / meta["page_size"])
    if meta["total_pages"] != expected_pages:
        raise OperationsApiError("Operations response has inconsistent pagination totals")
    return items, meta


def collection_complete(items: list[dict[str, Any]], pagination: dict[str, Any]) -> bool:
    total = pagination["total_count"]
    page = pagination["page"]
    page_size = pagination["page_size"]
    expected_items = min(max(total - (page - 1) * page_size, 0), page_size)
    return page == 1 and pagination["total_pages"] <= 1 and len(items) == expected_items


def _query_path(endpoint: str, params: dict[str, str | int | None]) -> str:
    query = urllib.parse.urlencode(
        {key: value for key, value in params.items() if value is not None}
    )
    return f"{endpoint}?{query}" if query else endpoint


def fetch_status(base_url: str, ca_file: str, admin_token: str) -> dict[str, Any]:
    payload = require_object(
        api_get(
            base_url,
            ca_file,
            admin_token,
            f"{OPERATIONS_PREFIX}status",
        ),
        "status",
    )
    if not isinstance(payload.get("service"), dict) or not isinstance(
        payload.get("monitoring"), dict
    ):
        raise OperationsApiError("Operations status response is missing service details")
    coverage = payload["monitoring"].get("coverage")
    if not isinstance(coverage, dict) or not isinstance(
        coverage.get("health"), dict
    ) or not isinstance(coverage.get("availability"), dict):
        raise OperationsApiError("Operations status response is missing monitoring coverage")
    return payload


def fetch_collection(
    base_url: str,
    ca_file: str,
    admin_token: str,
    *,
    item_key: str,
    page: int,
    page_size: int,
    order: str,
    snapshot_id: int | None,
    start: str | None = None,
    end: str | None = None,
) -> dict[str, Any]:
    endpoint = (
        f"{OPERATIONS_PREFIX}{'events' if item_key == 'events' else 'services'}"
    )
    path = _query_path(
        endpoint,
        {
            "page": page,
            "page_size": page_size,
            "order": order,
            "snapshot_id": snapshot_id,
            "start": start,
            "end": end,
        },
    )
    payload = require_object(
        api_get(base_url, ca_file, admin_token, path), "collection"
    )
    items, pagination = parse_collection(payload, item_key=item_key)
    result = dict(payload)
    result["result_complete"] = collection_complete(items, pagination)
    return result


def _fetch_item(
    base_url: str,
    ca_file: str,
    admin_token: str,
    endpoint: str,
    label: str,
) -> dict[str, Any]:
    return require_object(
        api_get(base_url, ca_file, admin_token, f"{OPERATIONS_PREFIX}{endpoint}"),
        label,
    )


def fetch_details(
    base_url: str, ca_file: str, admin_token: str, service: str
) -> dict[str, Any]:
    return _fetch_item(
        base_url,
        ca_file,
        admin_token,
        f"services/{urllib.parse.quote(service, safe='')}/details",
        "service details",
    )


def fetch_logs(
    base_url: str, ca_file: str, admin_token: str, service: str
) -> dict[str, Any]:
    payload = _fetch_item(
        base_url,
        ca_file,
        admin_token,
        f"services/{urllib.parse.quote(service, safe='')}/logs",
        "service logs",
    )
    lines = payload.get("lines")
    if not isinstance(lines, list) or not all(isinstance(line, str) for line in lines):
        raise OperationsApiError("Operations service logs response is missing lines")
    return payload


def fetch_failed_connections(
    base_url: str, ca_file: str, admin_token: str
) -> dict[str, Any]:
    payload = _fetch_item(
        base_url,
        ca_file,
        admin_token,
        "services/mosquitto/connection-failures",
        "MQTT failed-connections",
    )
    lines = payload.get("lines")
    notices = payload.get("notices")
    if not isinstance(lines, list) or not all(
        isinstance(line, str) for line in lines
    ):
        raise OperationsApiError(
            "Operations MQTT failed-connections response is missing lines"
        )
    if not isinstance(notices, list) or not all(
        isinstance(notice, str) for notice in notices
    ):
        raise OperationsApiError(
            "Operations MQTT failed-connections response is missing notices"
        )
    _require_int(payload.get("count"), "count")
    _require_int(payload.get("window_hours"), "window_hours")
    return payload


def _print_json(
    payload: dict[str, Any], *, compact: bool = False, flush: bool = False
) -> None:
    print(
        json.dumps(
            payload,
            indent=None if compact else 2,
            separators=(",", ":") if compact else None,
            ensure_ascii=False,
        ),
        flush=flush,
    )


def _print_worker(name: str, worker: Any) -> None:
    if not isinstance(worker, dict):
        return
    enabled = worker.get("enabled", "unknown")
    running = worker.get("running", "unknown")
    stale = worker.get("stale", "unknown")
    last_check = format_display_datetime(worker.get("last_successful_check"))
    print(
        f"  {name}: enabled={enabled}, running={running}, stale={stale}, "
        f"last success={last_check}"
    )


def _print_windows(label: str, windows: Any) -> None:
    if not isinstance(windows, dict):
        return
    print(f"  {label} coverage")
    for name, window in windows.items():
        if not isinstance(window, dict):
            continue
        percent = window.get("percent")
        if percent is None:
            percent_text = "unknown"
        elif isinstance(percent, (int, float)) and not isinstance(percent, bool):
            if not math.isfinite(percent):
                raise OperationsApiError("Operations status contains invalid coverage")
            percent_text = f"{percent:.2f}%"
        else:
            raise OperationsApiError("Operations status contains invalid coverage")
        flags = [
            key.replace("_", " ")
            for key in ("insufficient_data", "low_coverage")
            if window.get(key) is True
        ]
        if window.get("score_ready") is False:
            flags.append("score not ready")
        flag_text = f" ({', '.join(flags)})" if flags else ""
        print(
            f"    {name}: {percent_text}, "
            f"state={window.get('state_summary', 'unknown')}, "
            f"eligible={window.get('eligible_seconds', 'unknown')}s, "
            f"available={window.get('available_seconds', 'unknown')}s, "
            f"unavailable={window.get('unavailable_seconds', 'unknown')}s, "
            f"unobserved={window.get('unobserved_seconds', 'unknown')}s{flag_text}"
        )


def print_human_status(payload: dict[str, Any], *, observed_at: str | None = None) -> None:
    service = payload.get("service", {})
    monitoring = payload.get("monitoring", {})
    coverage = monitoring.get("coverage", {})
    health = coverage.get("health", {})
    availability = coverage.get("availability", {})
    own_health = payload.get("own_health", {})
    database = payload.get("database", {})
    if observed_at is not None:
        print(f"[{observed_at}]")
    print(
        f"Operations: {service.get('health', 'unknown')} "
        f"(own health: {own_health.get('status', 'unknown')})"
    )
    worker_errors = service.get("worker_errors")
    if isinstance(worker_errors, list):
        for error in worker_errors:
            print(f"  Worker error: {error}")
    print(f"Database: {database.get('status', 'unknown')}")
    print(
        f"Core Hub health: {health.get('state', 'unknown')} "
        f"(score: {health.get('score', 'unknown')})"
    )
    if health.get("reason"):
        print(f"  Reason: {health['reason']}")
    print(
        f"Core Hub availability: {availability.get('current_state', 'unknown')}"
    )
    _print_windows("Core Hub", availability.get("windows"))
    component_windows = availability.get("components")
    if isinstance(component_windows, dict):
        for component, details in component_windows.items():
            if isinstance(details, dict):
                _print_windows(component, details.get("windows"))

    components = health.get("components")
    if isinstance(components, list):
        print("  Components")
        for component in components:
            if isinstance(component, dict):
                last_success = format_display_datetime(
                    component.get("last_success_at")
                )
                reason = component.get("reason")
                diagnostic = f"; last success={last_success}"
                if reason:
                    diagnostic += f"; reason={reason}"
                print(
                    f"    {component.get('name', component.get('id', 'unknown'))}: "
                    f"{component.get('state', 'unknown')}{diagnostic}"
                )

    workers = monitoring.get("core")
    if workers is not None:
        print("Monitoring workers")
        _print_worker("Core", workers)
        public_workers = monitoring.get("public")
        if isinstance(public_workers, dict):
            for name, worker in public_workers.items():
                _print_worker(name, worker)

    public = monitoring.get("public_components")
    if isinstance(public, dict) and public:
        print("Public endpoint monitoring")
        for name, details in public.items():
            if not isinstance(details, dict):
                continue
            checked_at = format_display_datetime(details.get("checked_at"))
            print(
                f"  {name}: {details.get('state', 'unknown')}; "
                f"checked={checked_at}"
            )
            _print_windows(name, details.get("windows"))
            if details.get("reason"):
                print(f"    Reason: {details['reason']}")


def print_human_services(payload: dict[str, Any]) -> None:
    items, pagination = parse_collection(payload, item_key="services")
    complete = collection_complete(items, pagination)
    print(
        f"Services: {len(items)} returned of {pagination['total_count']} "
        f"(page {pagination['page']}/{pagination['total_pages']}, "
        f"complete={str(complete).lower()})"
    )
    for service in items:
        print(
            f"  {service.get('id', 'unknown')}: "
            f"{service.get('state', 'unknown')} — {service.get('name', 'unnamed')}"
        )
        database = service.get("database")
        if isinstance(database, dict):
            print(
                f"    {database.get('name', 'Database')}: "
                f"{database.get('state', 'unknown')}"
            )


def print_human_events(payload: dict[str, Any]) -> None:
    items, pagination = parse_collection(payload, item_key="events")
    complete = collection_complete(items, pagination)
    snapshot = pagination["snapshot_id"]
    print(
        f"Events: {len(items)} returned of {pagination['total_count']} "
        f"(page {pagination['page']}/{pagination['total_pages']}, "
        f"order={pagination['order']}, complete={str(complete).lower()}, "
        f"snapshot={snapshot})"
    )
    if not complete and pagination["total_pages"] > 1:
        print(
            "  Partial history; pass the snapshot ID with --snapshot-id "
            "when requesting another page."
        )
    for event in items:
        occurred_at = format_display_datetime(event.get("occurred_at"))
        observed_at = format_display_datetime(event.get("observed_at"))
        transition = " → ".join(
            str(value)
            for value in (event.get("previous_state"), event.get("new_state"))
            if value is not None
        )
        details = f" {transition}" if transition else ""
        reason = event.get("reason")
        if reason:
            details += f" — {reason}"
        metadata = []
        if event.get("event_id"):
            metadata.append(f"id={event['event_id']}")
        if event.get("source"):
            metadata.append(f"source={event['source']}")
        if event.get("certainty"):
            metadata.append(f"certainty={event['certainty']}")
        if metadata:
            details += f" [{', '.join(metadata)}]"
        print(
            f"  {occurred_at} {event.get('component', 'unknown')}/"
            f"{event.get('event_type', 'unknown')}{details}; "
            f"observed={observed_at}"
        )


def print_human_failed_connections(payload: dict[str, Any]) -> None:
    print(
        f"{payload.get('service', 'MQTT broker')} failed connections: "
        f"{payload.get('count', 0)} in the last "
        f"{payload.get('window_hours', 'unknown')} hours "
        f"(source: {payload.get('source', 'unknown')})"
    )
    for line in payload.get("lines", []):
        print(f"  {line}")
    for notice in payload.get("notices", []):
        print(f"  Note: {notice}")


def _add_output_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--output",
        choices=("text", "json"),
        default="text",
        help="Output format (default: readable text; JSON is suitable for scripts)",
    )


def _add_pagination_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--page", type=positive_int, default=1)
    parser.add_argument(
        "--page-size", type=bounded_page_size, default=DEFAULT_PAGE_SIZE
    )
    parser.add_argument("--order", choices=("newest", "oldest"), default="newest")
    parser.add_argument("--snapshot-id", type=positive_int)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Read Operations monitoring and diagnostics from the Hub API."
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)

    status = commands.add_parser("status", help="Show monitoring health and coverage")
    _add_output_argument(status)
    status.add_argument(
        "--watch",
        action="store_true",
        help="Poll status repeatedly until interrupted",
    )
    status.add_argument(
        "--interval-seconds",
        type=positive_float,
        default=DEFAULT_WATCH_INTERVAL,
        help=f"Watch interval (default: {DEFAULT_WATCH_INTERVAL:g})",
    )

    services = commands.add_parser("services", help="List monitored services")
    _add_output_argument(services)
    _add_pagination_arguments(services)

    details = commands.add_parser("details", help="Show service diagnostics")
    details.add_argument("service")
    _add_output_argument(details)

    logs = commands.add_parser("logs", help="Show the bounded service log tail")
    logs.add_argument("service")
    _add_output_argument(logs)

    events = commands.add_parser("events", help="List monitoring events")
    _add_output_argument(events)
    _add_pagination_arguments(events)
    events.add_argument("--start", help="Inclusive timezone-aware ISO-8601 start")
    events.add_argument("--end", help="Inclusive timezone-aware ISO-8601 end")

    failed = commands.add_parser(
        "failed-connections",
        help="Show recent MQTT connection failures",
    )
    _add_output_argument(failed)
    return parser


def _run_status(args: argparse.Namespace, base_url: str, admin_token: str) -> int:
    while True:
        payload = fetch_status(base_url, args.ca, admin_token)
        if args.output == "json":
            _print_json(payload, compact=args.watch, flush=args.watch)
        else:
            print_human_status(
                payload,
                observed_at=format_display_now() if args.watch else None,
            )
            if args.watch:
                sys.stdout.flush()
        if not args.watch:
            return 0
        time.sleep(args.interval_seconds)


def _run_command(args: argparse.Namespace, base_url: str, admin_token: str) -> int:
    if args.command == "status":
        return _run_status(args, base_url, admin_token)
    if args.command == "services":
        payload = fetch_collection(
            base_url,
            args.ca,
            admin_token,
            item_key="services",
            page=args.page,
            page_size=args.page_size,
            order=args.order,
            snapshot_id=args.snapshot_id,
        )
        if args.output == "json":
            _print_json(payload)
        else:
            print_human_services(payload)
        return 0
    if args.command == "details":
        payload = fetch_details(base_url, args.ca, admin_token, args.service)
        if args.output == "json":
            _print_json(payload)
        else:
            print(f"{payload.get('service', args.service)} details")
            _print_json(payload)
        return 0
    if args.command == "logs":
        payload = fetch_logs(base_url, args.ca, admin_token, args.service)
        if args.output == "json":
            _print_json(payload)
        else:
            print(
                f"{payload.get('service', args.service)} logs "
                f"(limit: {payload.get('line_limit', 'unknown')} lines)"
            )
            for line in payload.get("lines", []):
                print(line)
        return 0
    if args.command == "events":
        payload = fetch_collection(
            base_url,
            args.ca,
            admin_token,
            item_key="events",
            page=args.page,
            page_size=args.page_size,
            order=args.order,
            snapshot_id=args.snapshot_id,
            start=args.start,
            end=args.end,
        )
        if args.output == "json":
            _print_json(payload)
        else:
            print_human_events(payload)
        return 0
    if args.command == "failed-connections":
        payload = fetch_failed_connections(base_url, args.ca, admin_token)
        if args.output == "json":
            _print_json(payload)
        else:
            print_human_failed_connections(payload)
        return 0
    raise OperationsApiError(f"unsupported command: {args.command}")


def main(argv: list[str] | None = None) -> int:
    try:
        load_project_env()
    except HubCliError as exc:
        print(f"Operations configuration failed: {exc}", file=sys.stderr)
        return 1

    parser = build_parser()
    arguments = sys.argv[1:] if argv is None else argv
    if not arguments:
        parser.print_help()
        return 0
    args = parser.parse_args(arguments)
    base_url, admin_token = validate_hub_connection(args, parser)
    try:
        return _run_command(args, base_url, admin_token)
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    except OperationsApiError as exc:
        print(f"Operations CLI failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
