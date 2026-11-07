#!/usr/bin/env python3
"""Reusable host-side EAR test runner (Hub read-only; descriptive artifacts only)."""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections.abc import Callable

from lib.ear_test.collect import poll_http_once, reconcile_sqlite
from lib.ear_test.job import JobController
from lib.ear_test.models import JobMetadata, PauseRecord, PeriodRecord
from lib.ear_test.outputs import write_job_outputs
from lib.ear_test.store import (
    JobStoreError,
    clear_active_lock,
    create_job_directory,
    job_dir,
    list_job_ids,
    load_controller,
    load_observations,
    persist_controller,
    read_active_job_id,
    reassociate_observations,
)
from lib.hub_cli import (
    add_hub_connection_arguments,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)

DEFAULT_PAUSE_SECONDS = 8.0
DEFAULT_FINAL_COLLECTION_PAUSE_SECONDS = 8.0


def finalize_job(controller: JobController, *, ca_file: str | None = None) -> None:
    """Reconcile (best-effort), reassociate, write shared outputs, clear ACTIVE."""
    reassociate_observations(controller)
    reconcile_sqlite(controller)
    reassociate_observations(controller)
    persist_controller(controller)
    summary = write_job_outputs(controller)
    try:
        clear_active_lock(controller.manifest.job_id)
    except JobStoreError:
        pass
    out = job_dir(controller.manifest.job_id)
    print(
        f"Finalized job {controller.manifest.job_id} "
        f"status={controller.manifest.status} "
        f"completeness={controller.manifest.collection.completeness} "
        f"observations={summary['observation_count_total']}"
    )
    print(f"Report: {out / 'report.html'}")
    print(f"Analysis: {out / 'analysis.md'}")


def cmd_list(_args: argparse.Namespace) -> int:
    active = read_active_job_id()
    ids = list_job_ids()
    if not ids:
        print("No EAR test jobs found under shared/results/ear-tests/")
        if active:
            print(f"ACTIVE lock points at missing/incomplete job: {active}")
        return 0
    for job_id in ids:
        controller = load_controller(job_id)
        mark = " [ACTIVE]" if active == job_id else ""
        print(
            f"{job_id}  status={controller.manifest.status}  "
            f"ears={','.join(controller.manifest.node_ids)}  "
            f"obs={len(load_observations(job_id))}  "
            f"completeness={controller.manifest.collection.completeness}{mark}"
        )
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    active = read_active_job_id()
    job_id = args.job_id or active
    if not job_id:
        print("No ACTIVE job and no --job-id given.")
        return 1
    controller = load_controller(job_id)
    print(json.dumps(controller.manifest.to_dict(), indent=2))
    print("periods:")
    for period in controller.periods:
        print(f"  {period.period_id} status={period.status}")
    print("pauses:")
    for pause in controller.pauses:
        print(f"  {pause.pause_id} kind={pause.kind} status={pause.status}")
    if active and active != job_id:
        print(f"Note: ACTIVE lock is {active}")
    elif active == job_id:
        print("ACTIVE lock: yes")
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    controller = load_controller(args.job_id)
    reassociate_observations(controller)
    summary = write_job_outputs(controller)
    out = job_dir(controller.manifest.job_id)
    print(f"Wrote {out / 'summary.json'}")
    print(f"Wrote {out / 'report.html'}")
    print(f"Wrote {out / 'analysis.md'}")
    print(
        f"observations={summary['observation_count_total']} "
        f"completeness={summary['collection']['completeness']}"
    )
    return 0


def cmd_metadata(args: argparse.Namespace) -> int:
    controller = load_controller(args.job_id)
    meta = controller.manifest.metadata
    if args.test_name is not None:
        meta.test_name = args.test_name
    if args.description is not None:
        meta.description = args.description
    if args.environment is not None:
        meta.environment = args.environment
    if args.operator_notes is not None:
        meta.operator_notes = args.operator_notes
    if args.firmware_version:
        for item in args.firmware_version:
            node_id, sep, version = item.partition("=")
            if not sep or not node_id or not version:
                raise SystemExit("--firmware-version expects NODE=VERSION")
            meta.firmware_version_by_ear[node_id.strip().upper()] = version.strip()
    persist_controller(controller)
    write_job_outputs(controller)
    print(json.dumps(meta.to_dict(), indent=2))
    return 0


def cmd_recover(args: argparse.Namespace) -> int:
    active = read_active_job_id()
    if not active:
        print("No ACTIVE lock found.")
        return 0
    print(f"ACTIVE lock: {active}")
    if not (job_dir(active) / "manifest.json").is_file():
        print("Job directory/manifest missing. Use --clear-stale-lock to remove ACTIVE only.")
        if args.clear_stale_lock:
            clear_active_lock(active)
            print("Cleared stale ACTIVE lock.")
            return 0
        return 1

    controller = load_controller(active)
    if args.finalize_interrupted:
        if controller.manifest.status == "running":
            controller.interrupt()
        persist_controller(controller)
        finalize_job(controller)
        return 0
    if args.clear_stale_lock:
        print(
            "Refusing --clear-stale-lock while job directory exists. "
            "Use --finalize-interrupted to preserve artifacts, or remove the lock "
            "only after manual inspection."
        )
        return 1

    print(
        "Safe recovery options:\n"
        f"  {sys.argv[0]} recover --finalize-interrupted\n"
        "    → mark interrupted (if still running), reconcile, write report, clear ACTIVE\n"
        "Job artifacts are never deleted by recover."
    )
    return 0


def _wait_manual(prompt: str) -> str:
    print(prompt, flush=True)
    try:
        return input().strip().lower()
    except EOFError:
        return "q"


def _period_label(period: PeriodRecord, period_count: int) -> str:
    return f"P{period.index + 1}/{period_count} ({period.period_id})"


def _sleep_with_poll(
    controller: JobController,
    *,
    seconds: float,
    base_url: str,
    ca_file: str,
    admin_token: str,
    poll_interval: float,
    countdown: bool = False,
    sleep_fn: Callable[[float], None] | None = None,
    monotonic_fn: Callable[[], float] | None = None,
    poll_fn: Callable[..., None] | None = None,
) -> None:
    """Poll Hub while waiting. ``sleep_fn`` / ``monotonic_fn`` are injectable for tests."""
    sleep_fn = sleep_fn or time.sleep
    monotonic_fn = monotonic_fn or time.monotonic
    poll_fn = poll_fn or poll_http_once
    deadline = monotonic_fn() + seconds
    last_announced: int | None = None
    while monotonic_fn() < deadline:
        poll_fn(
            controller,
            base_url=base_url,
            ca_file=ca_file,
            admin_token=admin_token,
        )
        remaining = deadline - monotonic_fn()
        if remaining <= 0:
            break
        if countdown:
            whole = int(remaining)
            if whole != last_announced and whole >= 0:
                print(f"  … {whole}s remaining", flush=True)
                last_announced = whole
        sleep_fn(min(poll_interval, remaining))


def _run_inter_period_pause(
    controller: JobController,
    *,
    completed: PeriodRecord,
    next_period_index: int,
    base_url: str,
    ca_file: str,
    admin_token: str,
    poll_interval: float,
    sleep_fn: Callable[[float], None] | None = None,
    monotonic_fn: Callable[[], float] | None = None,
    poll_fn: Callable[..., None] | None = None,
) -> PauseRecord:
    pause = controller.start_next_pause()
    persist_controller(controller)
    print(
        f"{_period_label(completed, controller.manifest.period_count)} COMPLETE",
        flush=True,
    )
    print(f"INTER-PERIOD PAUSE — {pause.planned_seconds:g} seconds", flush=True)
    print("Stop the previous activity.", flush=True)
    print("Prepare for the next period.", flush=True)
    _sleep_with_poll(
        controller,
        seconds=pause.planned_seconds,
        base_url=base_url,
        ca_file=ca_file,
        admin_token=admin_token,
        poll_interval=poll_interval,
        countdown=True,
        sleep_fn=sleep_fn,
        monotonic_fn=monotonic_fn,
        poll_fn=poll_fn,
    )
    controller.complete_current_pause()
    persist_controller(controller)
    print(
        f"P{next_period_index + 1}/{controller.manifest.period_count} STARTING "
        f"(period-{next_period_index:03d})",
        flush=True,
    )
    return pause


def _run_final_collection_pause(
    controller: JobController,
    *,
    last_period: PeriodRecord,
    base_url: str,
    ca_file: str,
    admin_token: str,
    poll_interval: float,
    sleep_fn: Callable[[float], None] | None = None,
    monotonic_fn: Callable[[], float] | None = None,
    poll_fn: Callable[..., None] | None = None,
) -> PauseRecord | None:
    planned = controller.manifest.final_collection_pause_seconds
    if planned <= 0:
        print(
            f"{_period_label(last_period, controller.manifest.period_count)} COMPLETE",
            flush=True,
        )
        return None
    pause = controller.start_next_pause()
    if pause.kind != "final_collection":
        raise RuntimeError(
            f"expected final_collection pause, got {pause.kind}/{pause.pause_id}"
        )
    persist_controller(controller)
    print(
        f"{_period_label(last_period, controller.manifest.period_count)} COMPLETE",
        flush=True,
    )
    print(
        f"FINAL COLLECTION PAUSE — {pause.planned_seconds:g} seconds",
        flush=True,
    )
    print(
        "Collecting trailing Observations before finalization "
        "(not an active test period).",
        flush=True,
    )
    _sleep_with_poll(
        controller,
        seconds=pause.planned_seconds,
        base_url=base_url,
        ca_file=ca_file,
        admin_token=admin_token,
        poll_interval=poll_interval,
        countdown=True,
        sleep_fn=sleep_fn,
        monotonic_fn=monotonic_fn,
        poll_fn=poll_fn,
    )
    controller.complete_current_pause()
    persist_controller(controller)
    print("Final collection pause complete.", flush=True)
    return pause


def run_job_sequence(
    controller: JobController,
    *,
    progression: str,
    base_url: str,
    ca_file: str,
    admin_token: str,
    poll_interval: float,
    wait_manual_fn: Callable[[str], str] | None = None,
    sleep_fn: Callable[[float], None] | None = None,
    monotonic_fn: Callable[[], float] | None = None,
    poll_fn: Callable[..., None] | None = None,
) -> None:
    """Execute periods → inter-period pauses → final collection pause.

    Injectable wait/sleep/poll hooks keep unit tests off real wall-clock waits.
    """
    wait_manual_fn = wait_manual_fn or _wait_manual
    last_completed: PeriodRecord | None = None
    for index in range(controller.manifest.period_count):
        if progression == "manual":
            answer = wait_manual_fn(
                f"[period {index + 1}/{controller.manifest.period_count}] "
                "Press Enter to start next period (q to stop): "
            )
            if answer in {"q", "quit", "stop"}:
                controller.interrupt()
                return
        if index > 0:
            # Inter-period pause already announced "Pn STARTING"; period begins now.
            pass
        else:
            print(
                f"P1/{controller.manifest.period_count} STARTING "
                f"(period-000)",
                flush=True,
            )
        period = controller.start_next_period()
        persist_controller(controller)
        print(
            f"Period {period.period_id} active "
            f"({controller.manifest.period_seconds:g}s)",
            flush=True,
        )
        _sleep_with_poll(
            controller,
            seconds=controller.manifest.period_seconds,
            base_url=base_url,
            ca_file=ca_file,
            admin_token=admin_token,
            poll_interval=poll_interval,
            countdown=False,
            sleep_fn=sleep_fn,
            monotonic_fn=monotonic_fn,
            poll_fn=poll_fn,
        )
        controller.complete_current_period()
        persist_controller(controller)
        reconcile_sqlite(controller)
        reassociate_observations(controller)
        persist_controller(controller)
        last_completed = period

        is_last = index >= controller.manifest.period_count - 1
        if not is_last and controller.manifest.pause_seconds > 0:
            _run_inter_period_pause(
                controller,
                completed=period,
                next_period_index=index + 1,
                base_url=base_url,
                ca_file=ca_file,
                admin_token=admin_token,
                poll_interval=poll_interval,
                sleep_fn=sleep_fn,
                monotonic_fn=monotonic_fn,
                poll_fn=poll_fn,
            )
        elif not is_last:
            print(
                f"{_period_label(period, controller.manifest.period_count)} COMPLETE",
                flush=True,
            )
            print(
                f"P{index + 2}/{controller.manifest.period_count} STARTING "
                f"(period-{index + 1:03d})",
                flush=True,
            )

    if controller.manifest.status != "running":
        return

    if last_completed is not None:
        _run_final_collection_pause(
            controller,
            last_period=last_completed,
            base_url=base_url,
            ca_file=ca_file,
            admin_token=admin_token,
            poll_interval=poll_interval,
            sleep_fn=sleep_fn,
            monotonic_fn=monotonic_fn,
            poll_fn=poll_fn,
        )

    if controller.manifest.status == "running":
        controller.mark_completed()
        persist_controller(controller)
        print("TEST COMPLETE", flush=True)


def cmd_run(args: argparse.Namespace) -> int:
    active = read_active_job_id()
    if active:
        print(
            f"ACTIVE lock present for job {active}. "
            f"Run `{sys.argv[0]} recover` before starting a new job.",
            file=sys.stderr,
        )
        return 1

    metadata = JobMetadata(
        test_name=args.test_name,
        description=args.description,
        environment=args.environment,
        operator_notes=args.operator_notes,
    )
    if args.firmware_version:
        for item in args.firmware_version:
            node_id, sep, version = item.partition("=")
            if not sep:
                raise SystemExit("--firmware-version expects NODE=VERSION")
            metadata.firmware_version_by_ear[node_id.strip().upper()] = version.strip()

    base_url, admin_token = validate_hub_connection(args, args._parser)
    controller = JobController.create(
        node_ids=args.node_id,
        period_seconds=args.period_seconds,
        period_count=args.periods,
        pause_seconds=args.pause_seconds,
        final_collection_pause_seconds=args.final_collection_pause_seconds,
        progression=args.progression,
        hub_base_url=base_url,
        metadata=metadata,
    )
    create_job_directory(controller)
    print(f"Started job {controller.manifest.job_id}")
    print(f"Artifacts: {job_dir(controller.manifest.job_id)}")
    print(
        f"Timing: period={controller.manifest.period_seconds:g}s × "
        f"{controller.manifest.period_count}, "
        f"inter-period pause={controller.manifest.pause_seconds:g}s, "
        f"final collection={controller.manifest.final_collection_pause_seconds:g}s",
        flush=True,
    )

    poll_interval = max(0.5, float(args.poll_interval_seconds))
    try:
        run_job_sequence(
            controller,
            progression=args.progression,
            base_url=base_url,
            ca_file=args.ca,
            admin_token=admin_token,
            poll_interval=poll_interval,
        )
    except KeyboardInterrupt:
        print("\nInterrupted — preserving collected observations.", flush=True)
        controller.interrupt()
        persist_controller(controller)
    finally:
        finalize_job(controller, ca_file=args.ca)
    return 0 if controller.manifest.status == "completed" else 2


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Reusable EAR test runner: timed periods, Hub observation collection "
            "(HTTP + read-only SQLite reconcile), descriptive reports. "
            "Does not modify firmware, protocol, or Hub ingestion."
        )
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="Start a new EAR test job")
    run.add_argument(
        "--node-id",
        action="append",
        required=True,
        help="EAR node id (repeatable)",
    )
    run.add_argument("--period-seconds", type=float, required=True)
    run.add_argument("--periods", type=int, required=True)
    run.add_argument(
        "--pause-seconds",
        type=float,
        default=DEFAULT_PAUSE_SECONDS,
        help=(
            "Inter-period pause between consecutive active periods "
            f"(default: {DEFAULT_PAUSE_SECONDS:g}). Not added after the last period."
        ),
    )
    run.add_argument(
        "--final-collection-pause-seconds",
        type=float,
        default=DEFAULT_FINAL_COLLECTION_PAUSE_SECONDS,
        help=(
            "Trailing collection window after the last active period "
            f"(default: {DEFAULT_FINAL_COLLECTION_PAUSE_SECONDS:g}). "
            "Not an active test period. Use 0 to skip."
        ),
    )
    run.add_argument(
        "--progression",
        choices=("automatic", "manual"),
        default="automatic",
        help=(
            "automatic: timed periods/pauses without prompts. "
            "manual: Enter before each active period; pauses still auto-timed."
        ),
    )
    run.add_argument("--poll-interval-seconds", type=float, default=1.0)
    run.add_argument("--test-name")
    run.add_argument("--description")
    run.add_argument("--environment")
    run.add_argument("--operator-notes")
    run.add_argument(
        "--firmware-version",
        action="append",
        default=[],
        help="Optional NODE=VERSION (repeatable); not inferred",
    )
    run.set_defaults(func=cmd_run)

    list_cmd = commands.add_parser("list", help="List jobs under shared/results/ear-tests")
    list_cmd.set_defaults(func=cmd_list)

    status = commands.add_parser("status", help="Show job or ACTIVE status")
    status.add_argument("--job-id")
    status.set_defaults(func=cmd_status)

    report = commands.add_parser("report", help="Regenerate summary.json and report.html")
    report.add_argument("job_id")
    report.set_defaults(func=cmd_report)

    metadata = commands.add_parser("metadata", help="Edit job metadata after the run")
    metadata.add_argument("job_id")
    metadata.add_argument("--test-name")
    metadata.add_argument("--description")
    metadata.add_argument("--environment")
    metadata.add_argument("--operator-notes")
    metadata.add_argument(
        "--firmware-version",
        action="append",
        default=[],
        help="NODE=VERSION (repeatable)",
    )
    metadata.set_defaults(func=cmd_metadata)

    recover = commands.add_parser(
        "recover",
        help="Inspect ACTIVE lock; finalize interrupted job without deleting artifacts",
    )
    recover.add_argument(
        "--finalize-interrupted",
        action="store_true",
        help="Mark interrupted if needed, reconcile, write report, clear ACTIVE",
    )
    recover.add_argument(
        "--clear-stale-lock",
        action="store_true",
        help="Clear ACTIVE only when the job directory/manifest is missing",
    )
    recover.set_defaults(func=cmd_recover)
    return parser


def main() -> int:
    load_project_env()
    parser = build_parser()
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    args._parser = parser
    try:
        return int(args.func(args))
    except (JobStoreError, OSError, ValueError, RuntimeError) as exc:
        print(f"ear_test_runner failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
