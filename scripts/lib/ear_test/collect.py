"""Observation collection: Hub HTTP poll + optional read-only SQLite reconcile."""

from __future__ import annotations

import json
import ssl
import subprocess
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from typing import Any, Callable

from lib.ear_test.job import JobController
from lib.ear_test.models import format_utc, parse_utc, utc_now
from lib.ear_test.store import (
    REPO_ROOT,
    append_observation_record,
    build_observation_record,
    persist_controller,
)
from lib.hub_cli import format_hub_http_error

HttpFetcher = Callable[[str, str, str, str], list[dict[str, Any]]]
SqliteReconciler = Callable[[list[str], datetime, datetime | None], list[dict[str, Any]]]


def _normalize_received_time(value: Any) -> str:
    if hasattr(value, "isoformat"):
        text = format_utc(value)  # type: ignore[arg-type]
        return text or ""
    text = str(value)
    if " " in text and "T" not in text[:11]:
        text = text.replace(" ", "T", 1)
    if text.endswith("Z") or "+" in text[10:] or text.endswith("UTC"):
        parsed = parse_utc(text.replace("UTC", "+00:00"))
        return format_utc(parsed) or text
    parsed = parse_utc(text)
    return format_utc(parsed) if parsed else text


def fetch_observations_http(
    base_url: str,
    ca_file: str,
    admin_token: str,
    node_id: str,
    *,
    limit: int = 500,
) -> list[dict[str, Any]]:
    path = f"/api/v1/observations?node_id={urllib.parse.quote(node_id)}&limit={limit}"
    request = urllib.request.Request(
        base_url.rstrip("/") + path,
        headers={"Authorization": f"Bearer {admin_token}"},
        method="GET",
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=60) as response:
            body = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read() if hasattr(exc, "read") else b""
        raise RuntimeError(format_hub_http_error(exc.code, detail, path=path)) from exc
    if not isinstance(body, list):
        raise RuntimeError("hub observations response was not a list")
    return body


def poll_http_once(
    controller: JobController,
    *,
    base_url: str,
    ca_file: str,
    admin_token: str,
    fetcher: HttpFetcher | None = None,
) -> int:
    """Poll each selected Ear; append new observations. Returns newly stored count.

    Only stores Observations whose Hub ``received_time_utc`` falls in
    ``[job.started_at_utc, job.ended_at_utc or now)``. The Hub API returns the
    newest N rows without a since-filter, so the runner must window locally.
    """
    fetch = fetcher or fetch_observations_http
    started = controller.manifest.started_at_utc
    ended = controller.manifest.ended_at_utc
    if started is None:
        return 0
    window_end = ended or utc_now()
    added = 0
    for node_id in controller.manifest.node_ids:
        rows = fetch(base_url, ca_file, admin_token, node_id)
        for item in rows:
            if str(item.get("node_id", "")).upper() != node_id:
                continue
            received = item.get("received_time_utc")
            if received is None:
                continue
            received_text = _normalize_received_time(received)
            received_dt = parse_utc(received_text)
            if received_dt is None:
                continue
            if received_dt < started or received_dt >= window_end:
                continue
            payload = {
                key: value for key, value in item.items() if key != "received_time_utc"
            }
            record = build_observation_record(
                controller=controller,
                payload=payload,
                received_time_utc=received_text,
                source="http",
            )
            if append_observation_record(controller.manifest.job_id, record):
                added += 1
    controller.manifest.collection.http_poll_count += 1
    persist_controller(controller)
    return added


def reconcile_sqlite(
    controller: JobController,
    *,
    reconciler: SqliteReconciler | None = None,
) -> tuple[int, bool, str | None]:
    """Read-only SQLite reconcile. Returns (added, succeeded, error)."""
    health = controller.manifest.collection
    health.sqlite_reconcile_attempted = True
    started = controller.manifest.started_at_utc
    if started is None:
        health.sqlite_reconcile_succeeded = False
        health.sqlite_reconcile_error = "job has no started_at_utc"
        _refresh_completeness(controller)
        persist_controller(controller)
        return 0, False, health.sqlite_reconcile_error

    ended = controller.manifest.ended_at_utc
    try:
        rows = (reconciler or sqlite_reconcile_rows)(
            controller.manifest.node_ids,
            started,
            ended,
        )
    except Exception as exc:  # noqa: BLE001 - surface any compose/db failure
        health.sqlite_reconcile_succeeded = False
        health.sqlite_reconcile_error = str(exc)
        _refresh_completeness(controller)
        persist_controller(controller)
        return 0, False, health.sqlite_reconcile_error

    added = 0
    for row in rows:
        payload = row.get("payload")
        if not isinstance(payload, dict):
            continue
        received = row.get("received_time_utc")
        if not received:
            continue
        record = build_observation_record(
            controller=controller,
            payload=payload,
            received_time_utc=str(received),
            source="sqlite_reconcile",
        )
        if append_observation_record(controller.manifest.job_id, record):
            added += 1

    health.sqlite_reconcile_succeeded = True
    health.sqlite_reconcile_error = None
    _refresh_completeness(controller)
    persist_controller(controller)
    return added, True, None


def _refresh_completeness(controller: JobController) -> None:
    health = controller.manifest.collection
    notes: list[str] = []
    if health.sqlite_reconcile_succeeded:
        health.completeness = "complete"
        notes.append(
            "SQLite reconciliation succeeded; collection treated as complete for "
            "the job time window and selected Ears."
        )
    else:
        health.completeness = "potentially_incomplete"
        if not health.sqlite_reconcile_attempted:
            notes.append("SQLite reconciliation was not attempted.")
        elif health.sqlite_reconcile_error:
            notes.append(
                "SQLite reconciliation failed: "
                f"{health.sqlite_reconcile_error}. "
                "HTTP polling may have missed Observations beyond the Hub API "
                "limit of 500 newest rows per Ear."
            )
        else:
            notes.append("SQLite reconciliation did not succeed.")
        notes.append(
            "Results remain usable but must be treated as potentially incomplete."
        )
    health.completeness_notes = notes


def sqlite_reconcile_rows(
    node_ids: list[str],
    started_at: datetime,
    ended_at: datetime | None,
) -> list[dict[str, Any]]:
    """Query Hub SQLite read-only via docker compose exec/run."""
    nodes_literal = ", ".join(json.dumps(node_id) for node_id in node_ids)
    start_text = format_utc(started_at)
    end_text = format_utc(ended_at) if ended_at else format_utc(utc_now())
    code = f"""
import json, os, sqlite3
from datetime import datetime, timezone

def parse(value):
    text = str(value).strip()
    if " " in text and "T" not in text[:11]:
        text = text.replace(" ", "T", 1)
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    dt = datetime.fromisoformat(text)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)

def fmt(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"

p = "/data/bootstrap.db"
assert os.path.isfile(p), "Hub database not found"
start = parse({json.dumps(start_text)})
end = parse({json.dumps(end_text)})
nodes = {{{nodes_literal}}}
db = sqlite3.connect(f"file:{{p}}?mode=ro", uri=True)
db.row_factory = sqlite3.Row
rows = db.execute(
    "SELECT observation_id, node_id, sequence_number, event_time_utc, "
    "received_time_utc, payload_json FROM observations"
).fetchall()
out = []
for row in rows:
    if row["node_id"] not in nodes:
        continue
    try:
        received = parse(row["received_time_utc"])
    except Exception:
        continue
    if received < start or received >= end:
        continue
    payload = json.loads(row["payload_json"])
    out.append({{
        "observation_id": row["observation_id"],
        "node_id": row["node_id"],
        "sequence_number": row["sequence_number"],
        "event_time_utc": row["event_time_utc"],
        "received_time_utc": fmt(received),
        "payload": payload,
    }})
db.close()
print(json.dumps(out))
"""
    return json.loads(_run_python_in_hub(code))


def _compose_ps_hub() -> bool:
    result = subprocess.run(
        ["docker", "compose", "ps", "-q", "hub-bootstrap"],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    return bool(result.stdout.strip())


def _run_python_in_hub(code: str) -> str:
    if _compose_ps_hub():
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
