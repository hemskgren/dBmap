#!/usr/bin/env python3
"""Local Hub SQLite observation cleanup (Compose volume). Does not use the HTTP API."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from _hub_cli import print_help_without_arguments

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OLDER_THAN_HOURS = 4.0

DEVICE_WIPE_TABLES = (
    "observations",
    "deployment_attempt_events",
    "deployment_attempts",
    "device_ownership",
    "registration_requests",
    "device_monitor_samples",
    "device_monitor_events",
    "device_diag_events",
    "device_diag_state",
    "installation_metadata",
    "bootstrap_tokens",
    "node_credentials",
    "desired_state",
    "reported_state",
    "nodes",
)


def _compose(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["docker", "compose", *args],
        cwd=REPO_ROOT,
        check=check,
        text=True,
        capture_output=True,
    )


def _hub_bootstrap_running() -> bool:
    result = _compose("ps", "-q", "hub-bootstrap", check=False)
    return bool(result.stdout.strip())


def _run_python_in_hub(code: str, *, require_stopped: bool = False) -> str:
    if require_stopped and _hub_bootstrap_running():
        raise RuntimeError(
            "hub-bootstrap is still running; stop it before wipe-device-data"
        )
    if _hub_bootstrap_running():
        command = [
            "docker",
            "compose",
            "exec",
            "-T",
            "hub-bootstrap",
            "python",
            "-c",
            code,
        ]
    else:
        command = [
            "docker",
            "compose",
            "run",
            "--rm",
            "--no-deps",
            "--entrypoint",
            "python",
            "hub-bootstrap",
            "-c",
            code,
        ]
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(detail or f"docker compose failed ({result.returncode})")
    return (result.stdout or "").strip()


def clear_all_observations() -> dict[str, int]:
    code = r"""
import json, os, sqlite3
p = "/data/bootstrap.db"
assert os.path.isfile(p), "Hub database not found"
db = sqlite3.connect(p)
before = db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
db.execute("BEGIN IMMEDIATE")
db.execute("DELETE FROM observations")
db.commit()
after = db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
db.close()
print(json.dumps({"deleted": before - after, "remaining": after}))
"""
    return json.loads(_run_python_in_hub(code))


def prune_observations(*, older_than_hours: float) -> dict[str, int | float]:
    if older_than_hours <= 0 or older_than_hours != older_than_hours:
        raise ValueError("--older-than-hours must be finite and greater than zero")
    code = f"""
import json, os, sqlite3
from datetime import datetime, timedelta, timezone
p = "/data/bootstrap.db"
assert os.path.isfile(p), "Hub database not found"
hours = {older_than_hours!r}
cutoff = datetime.now(timezone.utc) - timedelta(hours=hours)
cutoff_text = cutoff.isoformat()
db = sqlite3.connect(p)
before = db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
db.execute("BEGIN IMMEDIATE")
deleted = db.execute(
    "DELETE FROM observations WHERE received_time_utc < ?",
    (cutoff_text,),
).rowcount
db.commit()
after = db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
db.close()
print(json.dumps({{
    "deleted": deleted if deleted is not None else before - after,
    "remaining": after,
    "older_than_hours": hours,
    "cutoff_utc": cutoff_text,
}}))
"""
    return json.loads(_run_python_in_hub(code))


def wipe_device_data() -> dict[str, int]:
    tables_literal = ", ".join(f'"{name}"' for name in DEVICE_WIPE_TABLES)
    deletes = "; ".join(f"DELETE FROM {name}" for name in DEVICE_WIPE_TABLES)
    code = f"""
import json, os, sqlite3
p = "/data/bootstrap.db"
assert os.path.isfile(p), "Hub database not found"
tables = [{tables_literal}]
db = sqlite3.connect(p)
db.execute("PRAGMA foreign_keys=ON")
db.executescript("BEGIN IMMEDIATE; {deletes}; COMMIT;")
counts = {{table: db.execute(f"SELECT COUNT(*) FROM {{table}}").fetchone()[0] for table in tables}}
db.close()
print(json.dumps(counts))
"""
    _compose("stop", "hub-bootstrap")
    try:
        counts = json.loads(_run_python_in_hub(code, require_stopped=True))
    finally:
        _compose("up", "-d", "hub-bootstrap")
    return counts


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Maintain Hub observation rows in the Compose SQLite volume. "
            "clear/prune keep nodes; wipe-device-data removes all device tables."
        )
    )
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser(
        "clear",
        help="Delete every observation (nodes/installations stay)",
    )
    prune = commands.add_parser(
        "prune",
        help=(
            f"Delete observations older than N hours by received_time_utc "
            f"(default: {DEFAULT_OLDER_THAN_HOURS:g})"
        ),
    )
    prune.add_argument(
        "--older-than-hours",
        type=float,
        default=DEFAULT_OLDER_THAN_HOURS,
        help=f"Age cutoff in hours (default: {DEFAULT_OLDER_THAN_HOURS:g})",
    )
    wipe = commands.add_parser(
        "wipe-device-data",
        help=(
            "Destructive: delete all nodes, credentials, installations, "
            "observations, and related device tables (users/firmware/TLS kept)"
        ),
    )
    wipe.add_argument(
        "--yes",
        action="store_true",
        help="Required confirmation for wipe-device-data",
    )
    return parser


def main() -> int:
    parser = build_parser()
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    try:
        if args.command == "clear":
            result = clear_all_observations()
            print(
                f"Cleared observations: deleted={result['deleted']} "
                f"remaining={result['remaining']}"
            )
        elif args.command == "prune":
            result = prune_observations(older_than_hours=args.older_than_hours)
            print(
                f"Pruned observations older than {result['older_than_hours']:g}h "
                f"(cutoff {result['cutoff_utc']}): "
                f"deleted={result['deleted']} remaining={result['remaining']}"
            )
        else:
            if not args.yes:
                parser.error("wipe-device-data requires --yes")
            counts = wipe_device_data()
            print("Device data wiped (row counts after delete):")
            for table, count in counts.items():
                print(f"  {table}: {count}")
            print(
                "If you used the Ear simulator, also remove "
                "shared/simulator/sim-ear-*.json and run add again."
            )
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"Hub observation DB maintenance failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
