"""Filesystem persistence for EAR test jobs under shared/results/ear-tests/."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from lib.ear_test.associate import associate_observation
from lib.ear_test.job import JobController
from lib.ear_test.models import (
    JobManifest,
    PauseRecord,
    PeriodRecord,
    format_utc,
    parse_utc,
    utc_now,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
RESULTS_ROOT = REPO_ROOT / "shared" / "results" / "ear-tests"
ACTIVE_LOCK_NAME = "ACTIVE"


class JobStoreError(RuntimeError):
    pass


def results_root() -> Path:
    RESULTS_ROOT.mkdir(parents=True, exist_ok=True)
    return RESULTS_ROOT


def job_dir(job_id: str) -> Path:
    return results_root() / job_id


def active_lock_path() -> Path:
    return results_root() / ACTIVE_LOCK_NAME


def read_active_job_id() -> str | None:
    path = active_lock_path()
    if not path.is_file():
        return None
    text = path.read_text(encoding="utf-8").strip()
    return text or None


def write_active_lock(job_id: str) -> None:
    path = active_lock_path()
    existing = read_active_job_id()
    if existing and existing != job_id:
        raise JobStoreError(
            f"ACTIVE lock held by job {existing}; recover or clear before starting {job_id}"
        )
    path.write_text(job_id + "\n", encoding="utf-8")


def clear_active_lock(job_id: str | None = None) -> None:
    path = active_lock_path()
    if not path.is_file():
        return
    current = read_active_job_id()
    if job_id is not None and current is not None and current != job_id:
        raise JobStoreError(
            f"ACTIVE lock is for {current}, not {job_id}; refusing to clear"
        )
    path.unlink()


def create_job_directory(controller: JobController) -> Path:
    path = job_dir(controller.manifest.job_id)
    if path.exists():
        raise JobStoreError(f"job directory already exists: {path}")
    path.mkdir(parents=True, exist_ok=False)
    write_active_lock(controller.manifest.job_id)
    persist_controller(controller)
    return path


def persist_controller(controller: JobController) -> None:
    path = job_dir(controller.manifest.job_id)
    path.mkdir(parents=True, exist_ok=True)
    _write_json(path / "manifest.json", controller.manifest.to_dict())
    _write_json(
        path / "periods.json",
        {
            "periods": [period.to_dict() for period in controller.periods],
            "pauses": [pause.to_dict() for pause in controller.pauses],
        },
    )


def load_controller(job_id: str) -> JobController:
    path = job_dir(job_id)
    manifest = JobManifest.from_dict(_read_json(path / "manifest.json"))
    periods_raw = _read_json(path / "periods.json")
    periods = [PeriodRecord.from_dict(item) for item in periods_raw.get("periods") or []]
    pauses = [PauseRecord.from_dict(item) for item in periods_raw.get("pauses") or []]
    controller = JobController(manifest=manifest, periods=periods, pauses=pauses)
    for period in periods:
        if period.status == "active":
            controller._current_index = period.index
            break
    for pause in pauses:
        if pause.status == "active":
            controller._current_pause_key = (pause.kind, pause.index)
            break
    return controller


def list_job_ids() -> list[str]:
    root = results_root()
    ids = []
    for child in sorted(root.iterdir()):
        if child.name == ACTIVE_LOCK_NAME:
            continue
        if child.is_dir() and (child / "manifest.json").is_file():
            ids.append(child.name)
    return ids


def append_observation_record(job_id: str, record: dict[str, Any]) -> bool:
    """Append one observation if observation_id is new. Returns True if appended.

    Never overwrites an existing payload for the same observation_id.
    """
    observation_id = str(record.get("observation_id") or "")
    if not observation_id:
        raise JobStoreError("observation record missing observation_id")
    path = job_dir(job_id) / "observations.jsonl"
    existing = load_observation_ids(job_id)
    if observation_id in existing:
        return False
    line = json.dumps(record, separators=(",", ":"), ensure_ascii=False)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(line + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    return True


def load_observation_ids(job_id: str) -> set[str]:
    path = job_dir(job_id) / "observations.jsonl"
    if not path.is_file():
        return set()
    ids: set[str] = set()
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            oid = row.get("observation_id")
            if oid:
                ids.add(str(oid))
    return ids


def load_observations(job_id: str) -> list[dict[str, Any]]:
    path = job_dir(job_id) / "observations.jsonl"
    if not path.is_file():
        return []
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def reassociate_observations(controller: JobController) -> int:
    """Rewrite association fields on all stored observations (payload untouched)."""
    job_id = controller.manifest.job_id
    rows = load_observations(job_id)
    if not rows:
        return 0
    path = job_dir(job_id) / "observations.jsonl"
    updated: list[dict[str, Any]] = []
    for row in rows:
        payload = row.get("payload") or {}
        received = parse_utc(row.get("received_time_utc"))
        event = parse_utc(
            payload.get("event_time_utc")
            if isinstance(payload.get("event_time_utc"), str)
            else row.get("event_time_utc")
        )
        if received is None:
            updated.append(row)
            continue
        association = associate_observation(
            received_time_utc=received,
            event_time_utc=event,
            job_start=controller.manifest.started_at_utc,
            job_end=controller.manifest.ended_at_utc,
            periods=controller.periods,
            pauses=controller.pauses,
        )
        row = dict(row)
        row["association"] = {
            "phase_by_received": association.phase_by_received,
            "period_id_by_received": association.period_id_by_received,
            "phase_by_event": association.phase_by_event,
            "period_id_by_event": association.period_id_by_event,
            "received_event_disagree": association.received_event_disagree,
            "association_note": association.association_note,
        }
        updated.append(row)
    tmp = path.with_suffix(".jsonl.tmp")
    with tmp.open("w", encoding="utf-8") as handle:
        for row in updated:
            handle.write(json.dumps(row, separators=(",", ":"), ensure_ascii=False) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    tmp.replace(path)
    return len(updated)


def build_observation_record(
    *,
    controller: JobController,
    payload: dict[str, Any],
    received_time_utc: str,
    source: str,
) -> dict[str, Any]:
    observation_id = str(payload.get("observation_id") or "")
    event_raw = payload.get("event_time_utc")
    event_dt = parse_utc(event_raw if isinstance(event_raw, str) else None)
    received_dt = parse_utc(received_time_utc)
    if received_dt is None:
        raise JobStoreError(f"invalid received_time_utc: {received_time_utc}")
    association = associate_observation(
        received_time_utc=received_dt,
        event_time_utc=event_dt,
        job_start=controller.manifest.started_at_utc,
        job_end=controller.manifest.ended_at_utc,
        periods=controller.periods,
        pauses=controller.pauses,
    )
    return {
        "observation_id": observation_id,
        "node_id": payload.get("node_id"),
        "sequence_number": payload.get("sequence_number"),
        "event_time_utc": event_raw,
        "received_time_utc": format_utc(received_dt),
        "collected_at_utc": format_utc(utc_now()),
        "source": source,
        "payload": payload,
        "association": {
            "phase_by_received": association.phase_by_received,
            "period_id_by_received": association.period_id_by_received,
            "phase_by_event": association.phase_by_event,
            "period_id_by_event": association.period_id_by_event,
            "received_event_disagree": association.received_event_disagree,
            "association_note": association.association_note,
        },
    }


def write_summary(job_id: str, summary: dict[str, Any]) -> None:
    _write_json(job_dir(job_id) / "summary.json", summary)


def write_report_html(job_id: str, html: str) -> None:
    path = job_dir(job_id) / "report.html"
    path.write_text(html, encoding="utf-8")


def write_analysis_md(job_id: str, markdown: str) -> None:
    path = job_dir(job_id) / "analysis.md"
    path.write_text(markdown, encoding="utf-8")


def _write_json(path: Path, data: dict[str, Any]) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp.replace(path)


def _read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise JobStoreError(f"missing file: {path}")
    return json.loads(path.read_text(encoding="utf-8"))
