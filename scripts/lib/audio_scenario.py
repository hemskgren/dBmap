"""Versioned JSON audio scenario schema and fail-stop sequential runner."""

from __future__ import annotations

import json
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from lib.homeassistant_client import (
    HomeAssistantClient,
    HomeAssistantError,
    parse_volume,
    validate_playable_url,
)

SUPPORTED_SCHEMA_VERSION = 1
ALLOWED_ACTIONS = frozenset(
    {"play", "wait", "stop", "set_volume", "pause", "resume"}
)
DEFAULT_SCENARIO_DIR = Path(__file__).resolve().parents[2] / "tests" / "audio"


class AudioScenarioError(HomeAssistantError):
    """Invalid audio scenario document or step failure."""


@dataclass(frozen=True)
class StepResult:
    index: int
    action: str
    started_at_utc: str
    ended_at_utc: str
    duration_s: float
    ok: bool
    dry_run: bool
    detail: str
    ha_accepted: bool | None = None
    requested_volume: float | None = None
    observed_volume: float | None = None


@dataclass
class VolumeRestoreStatus:
    attempted: bool = False
    previous_volume: float | None = None
    requested_restore: float | None = None
    observed_after: float | None = None
    verified: bool = False
    skipped_reason: str | None = None
    error: str | None = None

    @property
    def summary(self) -> str:
        if self.skipped_reason:
            return f"skipped ({self.skipped_reason})"
        if not self.attempted:
            return "not attempted"
        if self.error:
            return f"attempted, failed: {self.error}"
        if self.verified:
            return (
                f"restored and verified "
                f"(requested={self.requested_restore:.2f}, "
                f"observed={self.observed_after:.2f})"
            )
        if self.observed_after is None:
            return (
                f"restore requested={self.requested_restore:.2f} "
                "but readback unavailable (not verified)"
            )
        return (
            f"restore requested={self.requested_restore:.2f}, "
            f"observed={self.observed_after:.2f} (not verified equal)"
        )


@dataclass
class ScenarioRunReport:
    scenario_name: str
    schema_version: int
    entity_id: str | None
    dry_run: bool
    started_at_utc: str
    ended_at_utc: str
    duration_s: float
    steps: list[StepResult] = field(default_factory=list)
    error: str | None = None
    interrupted: bool = False
    play_requests_accepted: int = 0
    volume_restore: VolumeRestoreStatus = field(default_factory=VolumeRestoreStatus)
    cleanup_errors: list[str] = field(default_factory=list)
    estimated_wait_s: float = 0.0
    changes_volume: bool = False

    @property
    def successful_steps(self) -> int:
        return sum(1 for step in self.steps if step.ok)

    @property
    def failed_step(self) -> int | None:
        for step in self.steps:
            if not step.ok:
                return step.index
        return None


def _utc_now() -> datetime:
    return datetime.now(UTC)


def _format_utc(moment: datetime) -> str:
    return moment.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _require_dict(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AudioScenarioError(f"{label} must be a JSON object")
    return value


def _require_str(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise AudioScenarioError(f"{label} must be a non-empty string")
    return value.strip()


def _validate_media_sources(raw: object | None) -> dict[str, dict[str, str]]:
    if raw is None:
        return {}
    sources_obj = _require_dict(raw, "media_sources")
    sources: dict[str, dict[str, str]] = {}
    for name, entry in sources_obj.items():
        if not isinstance(name, str) or not name.strip():
            raise AudioScenarioError("media_sources keys must be non-empty strings")
        entry_obj = _require_dict(entry, f"media_sources.{name}")
        unknown = set(entry_obj) - {"url", "media_content_type"}
        if unknown:
            raise AudioScenarioError(
                f"media_sources.{name} has unknown fields: "
                + ", ".join(sorted(unknown))
            )
        try:
            url = validate_playable_url(
                _require_str(entry_obj.get("url"), f"media_sources.{name}.url")
            )
        except HomeAssistantError as exc:
            if isinstance(exc, AudioScenarioError):
                raise
            raise AudioScenarioError(str(exc)) from exc
        content_type = entry_obj.get("media_content_type", "music")
        if not isinstance(content_type, str) or not content_type.strip():
            raise AudioScenarioError(
                f"media_sources.{name}.media_content_type must be a non-empty string"
            )
        sources[name.strip()] = {
            "url": url,
            "media_content_type": content_type.strip(),
        }
    return sources


def _validate_step(
    step: object,
    index: int,
    sources: dict[str, dict[str, str]],
) -> dict[str, Any]:
    label = f"steps[{index}]"
    step_obj = _require_dict(step, label)
    action = step_obj.get("action")
    if not isinstance(action, str) or action not in ALLOWED_ACTIONS:
        raise AudioScenarioError(
            f"{label}.action must be one of: {', '.join(sorted(ALLOWED_ACTIONS))}"
        )

    allowed_by_action = {
        "play": {"action", "source", "volume", "media_content_type"},
        "wait": {"action", "seconds"},
        "stop": {"action"},
        "pause": {"action"},
        "resume": {"action"},
        "set_volume": {"action", "volume"},
    }
    unknown = set(step_obj) - allowed_by_action[action]
    if unknown:
        raise AudioScenarioError(
            f"{label} has unknown fields for action {action!r}: "
            + ", ".join(sorted(unknown))
        )

    normalized: dict[str, Any] = {"action": action}

    try:
        if action == "play":
            source = _require_str(step_obj.get("source"), f"{label}.source")
            if source not in sources:
                raise AudioScenarioError(
                    f"{label}.source {source!r} is not defined in media_sources"
                )
            normalized["source"] = source
            if "volume" in step_obj:
                normalized["volume"] = parse_volume(
                    step_obj["volume"], label=f"{label}.volume"
                )
            if "media_content_type" in step_obj:
                content_type = step_obj["media_content_type"]
                if not isinstance(content_type, str) or not content_type.strip():
                    raise AudioScenarioError(
                        f"{label}.media_content_type must be a non-empty string"
                    )
                normalized["media_content_type"] = content_type.strip()

        elif action == "wait":
            seconds = step_obj.get("seconds")
            if isinstance(seconds, bool) or not isinstance(seconds, (int, float)):
                raise AudioScenarioError(f"{label}.seconds must be a number")
            if float(seconds) < 0:
                raise AudioScenarioError(f"{label}.seconds must be >= 0")
            normalized["seconds"] = float(seconds)

        elif action == "set_volume":
            normalized["volume"] = parse_volume(
                step_obj.get("volume"), label=f"{label}.volume"
            )
    except HomeAssistantError as exc:
        if isinstance(exc, AudioScenarioError):
            raise
        raise AudioScenarioError(str(exc)) from exc

    return normalized


def validate_scenario_document(document: object) -> dict[str, Any]:
    """Validate a scenario object and return a normalized copy.

    Rejects unknown actions, invalid volumes, unresolved sources, and any
    fields that would imply shell/Python/HA service injection.
    """
    root = _require_dict(document, "scenario")
    unknown_top = set(root) - {
        "schema_version",
        "name",
        "description",
        "media_sources",
        "steps",
    }
    if unknown_top:
        raise AudioScenarioError(
            "scenario has unknown top-level fields: " + ", ".join(sorted(unknown_top))
        )

    version = root.get("schema_version")
    if version != SUPPORTED_SCHEMA_VERSION:
        raise AudioScenarioError(
            f"schema_version must be {SUPPORTED_SCHEMA_VERSION} (got {version!r})"
        )

    name = _require_str(root.get("name"), "name")
    description = root.get("description")
    if description is not None and not isinstance(description, str):
        raise AudioScenarioError("description must be a string when present")

    sources = _validate_media_sources(root.get("media_sources"))
    steps_raw = root.get("steps")
    if not isinstance(steps_raw, list) or not steps_raw:
        raise AudioScenarioError("steps must be a non-empty array")

    steps = [
        _validate_step(step, index, sources) for index, step in enumerate(steps_raw)
    ]
    if any(step["action"] == "play" for step in steps) and not sources:
        raise AudioScenarioError(
            "media_sources must declare at least one source when play steps exist"
        )

    normalized: dict[str, Any] = {
        "schema_version": SUPPORTED_SCHEMA_VERSION,
        "name": name,
        "media_sources": sources,
        "steps": steps,
    }
    if isinstance(description, str):
        normalized["description"] = description
    return normalized


def load_scenario_file(path: Path) -> dict[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise AudioScenarioError(f"could not read scenario file {path}: {exc}") from exc
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise AudioScenarioError(
            f"scenario file {path} is not valid JSON: {exc}"
        ) from exc
    return validate_scenario_document(document)


def list_scenario_files(directory: Path | None = None) -> list[Path]:
    root = directory or DEFAULT_SCENARIO_DIR
    if not root.is_dir():
        return []
    return sorted(path for path in root.glob("*.json") if path.is_file())


def scenario_changes_volume(scenario: dict[str, Any]) -> bool:
    for step in scenario["steps"]:
        if step["action"] == "set_volume":
            return True
        if step["action"] == "play" and "volume" in step:
            return True
    return False


def estimated_wait_seconds(scenario: dict[str, Any]) -> float:
    return sum(
        float(step["seconds"])
        for step in scenario["steps"]
        if step["action"] == "wait"
    )


def format_step_preview(
    index: int,
    step: dict[str, Any],
    *,
    sources: dict[str, dict[str, str]] | None = None,
) -> str:
    action = step["action"]
    if action == "play":
        parts = [f"source={step['source']}"]
        if sources and step["source"] in sources:
            parts.append(f"url={sources[step['source']]['url']}")
        if "volume" in step:
            parts.append(f"volume={step['volume']:.2f}")
        content_type = step.get("media_content_type")
        if content_type is None and sources and step["source"] in sources:
            content_type = sources[step["source"]]["media_content_type"]
        if content_type:
            parts.append(f"media_content_type={content_type}")
        return f"step {index}: play ({', '.join(parts)})"
    if action == "wait":
        return f"step {index}: wait ({step['seconds']:g}s)"
    if action == "set_volume":
        return f"step {index}: set_volume (volume={step['volume']:.2f})"
    return f"step {index}: {action}"


def _try_read_volume(
    client: HomeAssistantClient, entity_id: str | None
) -> tuple[float | None, str | None]:
    try:
        return client.get_volume_level(entity_id=entity_id), None
    except HomeAssistantError as exc:
        return None, str(exc)


def _restore_volume(
    client: HomeAssistantClient,
    entity_id: str | None,
    previous: float,
) -> VolumeRestoreStatus:
    status = VolumeRestoreStatus(
        attempted=True,
        previous_volume=previous,
        requested_restore=previous,
    )
    try:
        client.set_volume_level(previous, entity_id=entity_id)
    except HomeAssistantError as exc:
        status.error = f"volume_set failed: {exc}"
        return status
    observed, read_error = _try_read_volume(client, entity_id)
    status.observed_after = observed
    if read_error:
        status.error = f"restore set accepted but readback failed: {read_error}"
        return status
    if observed is not None and abs(observed - previous) <= 0.001:
        status.verified = True
    return status


def _cleanup_after_interrupt(
    client: HomeAssistantClient,
    entity_id: str | None,
    previous_volume: float | None,
    *,
    restore: bool,
) -> tuple[VolumeRestoreStatus, list[str]]:
    cleanup_errors: list[str] = []
    try:
        client.stop(entity_id=entity_id)
    except HomeAssistantError as exc:
        cleanup_errors.append(f"media_stop during interrupt cleanup failed: {exc}")

    if not restore:
        return (
            VolumeRestoreStatus(skipped_reason="--no-restore"),
            cleanup_errors,
        )
    if previous_volume is None:
        return (
            VolumeRestoreStatus(
                skipped_reason="initial volume unavailable; cannot restore"
            ),
            cleanup_errors,
        )
    restore_status = _restore_volume(client, entity_id, previous_volume)
    if restore_status.error:
        cleanup_errors.append(f"volume restore during interrupt cleanup: {restore_status.error}")
    return restore_status, cleanup_errors


def run_scenario(
    scenario: dict[str, Any],
    client: HomeAssistantClient | None,
    *,
    entity_id: str | None = None,
    dry_run: bool = False,
    restore_volume: bool = True,
    sleep: Callable[[float], None] = time.sleep,
) -> ScenarioRunReport:
    """Execute validated scenario steps sequentially; stop on first failure.

    Dry-run validates resolution and prints the plan without HA calls or waits.
    Live runs save/restore volume by default when the scenario changes volume.
    """
    started = _utc_now()
    changes_volume = scenario_changes_volume(scenario)
    wait_total = estimated_wait_seconds(scenario)
    report = ScenarioRunReport(
        scenario_name=scenario["name"],
        schema_version=int(scenario["schema_version"]),
        entity_id=entity_id,
        dry_run=dry_run,
        started_at_utc=_format_utc(started),
        ended_at_utc=_format_utc(started),
        duration_s=0.0,
        estimated_wait_s=wait_total,
        changes_volume=changes_volume,
    )

    if dry_run:
        sources = scenario["media_sources"]
        for index, step in enumerate(scenario["steps"], start=1):
            step_started = _utc_now()
            detail = format_step_preview(index, step, sources=sources)
            step_ended = _utc_now()
            report.steps.append(
                StepResult(
                    index=index,
                    action=step["action"],
                    started_at_utc=_format_utc(step_started),
                    ended_at_utc=_format_utc(step_ended),
                    duration_s=0.0,
                    ok=True,
                    dry_run=True,
                    detail=detail,
                    requested_volume=step.get("volume"),
                )
            )
        report.volume_restore = VolumeRestoreStatus(
            skipped_reason="dry-run (no device state changes)"
        )
        ended = _utc_now()
        report.ended_at_utc = _format_utc(ended)
        report.duration_s = (ended - started).total_seconds()
        return report

    if client is None:
        raise AudioScenarioError("scenario run requires a Home Assistant client")

    previous_volume, volume_read_error = _try_read_volume(client, entity_id)
    if previous_volume is None:
        if changes_volume and restore_volume:
            raise AudioScenarioError(
                "could not read the media player's current volume before a scenario "
                "that changes volume; refusing to run without a restore baseline. "
                f"Read error: {volume_read_error}. "
                "Re-run with --no-restore only if you accept leaving the player "
                "at a different volume."
            )
        report.volume_restore = VolumeRestoreStatus(
            skipped_reason=(
                "initial volume unavailable"
                + (f" ({volume_read_error})" if volume_read_error else "")
            )
        )
    else:
        report.volume_restore.previous_volume = previous_volume

    if not restore_volume:
        report.volume_restore.skipped_reason = "--no-restore"
        report.volume_restore.previous_volume = previous_volume

    sources = scenario["media_sources"]
    steps: list[dict[str, Any]] = scenario["steps"]
    interrupted = False
    run_error: str | None = None

    for index, step in enumerate(steps, start=1):
        action = step["action"]
        step_started = _utc_now()
        perf_started = time.perf_counter()
        requested_volume: float | None = step.get("volume")
        observed_volume: float | None = None
        ha_accepted: bool | None = None
        try:
            if action == "wait":
                seconds = float(step["seconds"])
                sleep(seconds)
                detail = (
                    f"waited {seconds:g}s "
                    "(wait end does not confirm playback completion)"
                )
            elif action == "set_volume":
                requested = float(step["volume"])
                client.set_volume_level(requested, entity_id=entity_id)
                ha_accepted = True
                observed_volume, read_error = _try_read_volume(client, entity_id)
                if read_error:
                    detail = (
                        f"volume_set accepted requested={requested:.2f}; "
                        f"readback unavailable ({read_error})"
                    )
                else:
                    detail = (
                        f"volume_set accepted requested={requested:.2f} "
                        f"observed={observed_volume:.2f}"
                    )
            elif action == "play":
                source = sources[step["source"]]
                content_type = step.get(
                    "media_content_type", source["media_content_type"]
                )
                play_result = client.play_media(
                    source["url"],
                    entity_id=entity_id,
                    media_content_type=content_type,
                    volume=step.get("volume"),
                )
                ha_accepted = True
                report.play_requests_accepted += 1
                if "volume" in step:
                    observed_volume, _ = _try_read_volume(client, entity_id)
                detail = (
                    f"play_media accepted source={step['source']} "
                    f"url={play_result['media_content_id']} "
                    "(acceptance ≠ audible playback complete)"
                )
            elif action == "pause":
                pause_result = client.pause(entity_id=entity_id)
                ha_accepted = True
                detail = f"{pause_result['service']} accepted"
            elif action == "resume":
                resume_result = client.resume(entity_id=entity_id)
                ha_accepted = True
                detail = f"{resume_result['service']} accepted"
            elif action == "stop":
                stop_result = client.stop(entity_id=entity_id)
                ha_accepted = True
                detail = f"{stop_result['service']} accepted"
            else:
                raise AudioScenarioError(f"unsupported action {action!r}")

            step_ended = _utc_now()
            report.steps.append(
                StepResult(
                    index=index,
                    action=action,
                    started_at_utc=_format_utc(step_started),
                    ended_at_utc=_format_utc(step_ended),
                    duration_s=time.perf_counter() - perf_started,
                    ok=True,
                    dry_run=False,
                    detail=detail,
                    ha_accepted=ha_accepted,
                    requested_volume=requested_volume,
                    observed_volume=observed_volume,
                )
            )
        except KeyboardInterrupt:
            interrupted = True
            step_ended = _utc_now()
            report.steps.append(
                StepResult(
                    index=index,
                    action=action,
                    started_at_utc=_format_utc(step_started),
                    ended_at_utc=_format_utc(step_ended),
                    duration_s=time.perf_counter() - perf_started,
                    ok=False,
                    dry_run=False,
                    detail="interrupted",
                )
            )
            run_error = f"scenario interrupted at step {index} ({action})"
            restore_status, cleanup_errors = _cleanup_after_interrupt(
                client,
                entity_id,
                previous_volume,
                restore=restore_volume and previous_volume is not None,
            )
            if restore_volume and previous_volume is not None:
                report.volume_restore = restore_status
            report.cleanup_errors.extend(cleanup_errors)
            break
        except HomeAssistantError as exc:
            step_ended = _utc_now()
            report.steps.append(
                StepResult(
                    index=index,
                    action=action,
                    started_at_utc=_format_utc(step_started),
                    ended_at_utc=_format_utc(step_ended),
                    duration_s=time.perf_counter() - perf_started,
                    ok=False,
                    dry_run=False,
                    detail=str(exc),
                    ha_accepted=False,
                    requested_volume=requested_volume,
                )
            )
            run_error = f"scenario failed at step {index} ({action}): {exc}"
            break

    # Volume restore after success or non-interrupt failure.
    if not interrupted and restore_volume and previous_volume is not None:
        report.volume_restore = _restore_volume(client, entity_id, previous_volume)
        if report.volume_restore.error:
            report.cleanup_errors.append(
                f"volume restore after run: {report.volume_restore.error}"
            )
    elif not interrupted and not restore_volume:
        report.volume_restore.skipped_reason = "--no-restore"
        report.volume_restore.previous_volume = previous_volume
    elif not interrupted and previous_volume is None and not report.volume_restore.skipped_reason:
        report.volume_restore.skipped_reason = "initial volume unavailable"

    ended = _utc_now()
    report.ended_at_utc = _format_utc(ended)
    report.duration_s = (ended - started).total_seconds()
    report.error = run_error
    report.interrupted = interrupted
    return report


def format_dry_run_plan(
    scenario: dict[str, Any],
    *,
    entity_id: str | None,
) -> list[str]:
    lines = [
        (
            f"Dry-run plan for scenario {scenario['name']!r} "
            f"(schema_version={scenario['schema_version']})"
        ),
        f"  media_player: {entity_id or 'unset'}",
        f"  changes_volume: {scenario_changes_volume(scenario)}",
        f"  estimated_wait_s: {estimated_wait_seconds(scenario):g}",
        "  media_sources:",
    ]
    sources = scenario["media_sources"]
    if not sources:
        lines.append("    (none)")
    else:
        for name, entry in sources.items():
            lines.append(
                f"    {name}: url={entry['url']} "
                f"media_content_type={entry['media_content_type']}"
            )
    lines.append("  steps:")
    for index, step in enumerate(scenario["steps"], start=1):
        lines.append(f"    {format_step_preview(index, step, sources=sources)}")
    lines.append("  note: dry-run makes no Home Assistant service calls")
    return lines


def format_execution_report(report: ScenarioRunReport) -> list[str]:
    failed = (
        f"step {report.failed_step}" if report.failed_step is not None else "none"
    )
    lines = [
        "Scenario execution report",
        f"  name: {report.scenario_name}",
        f"  schema_version: {report.schema_version}",
        f"  media_player: {report.entity_id or 'unset'}",
        f"  mode: {'dry-run' if report.dry_run else 'live'}",
        f"  started_at_utc: {report.started_at_utc}",
        f"  ended_at_utc: {report.ended_at_utc}",
        f"  duration_s: {report.duration_s:.3f}",
        f"  successful_steps: {report.successful_steps}/{len(report.steps)}",
        f"  failed_step: {failed}",
        f"  play_requests_accepted: {report.play_requests_accepted}",
        f"  volume_restore: {report.volume_restore.summary}",
    ]
    if report.error:
        lines.append(f"  error: {report.error}")
    for cleanup in report.cleanup_errors:
        lines.append(f"  cleanup_error: {cleanup}")
    if not report.dry_run:
        lines.append(
            "  note: HA service acceptance does not confirm audible playback"
        )
    return lines
