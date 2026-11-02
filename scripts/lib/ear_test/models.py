"""Data models for EAR test jobs. Descriptive only — no detector interpretation."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import Any, Literal

JobStatus = Literal["running", "completed", "interrupted"]
PeriodStatus = Literal["planned", "active", "completed", "skipped", "interrupted"]
PauseStatus = Literal["planned", "active", "completed", "skipped", "interrupted"]
PauseKind = Literal["inter_period", "final_collection"]
Progression = Literal["automatic", "manual"]
CollectionCompleteness = Literal["complete", "potentially_incomplete"]
# ``final_collection`` = trailing collection window after the last active period (not an
# active test period, and not the same as an inter-period ``pause``).
PhaseKind = Literal["active", "pause", "final_collection", "job_idle", "unassigned"]


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def format_utc(value: datetime | None) -> str | None:
    if value is None:
        return None
    if value.tzinfo is None or value.utcoffset() is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def parse_utc(value: str | None) -> datetime | None:
    if value is None or value == "":
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    parsed = datetime.fromisoformat(text)
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


@dataclass
class JobMetadata:
    """Optional operator-supplied context. Never auto-inferred."""

    test_name: str | None = None
    description: str | None = None
    environment: str | None = None
    operator_notes: str | None = None
    firmware_version_by_ear: dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "test_name": self.test_name,
            "description": self.description,
            "environment": self.environment,
            "operator_notes": self.operator_notes,
            "firmware_version_by_ear": dict(self.firmware_version_by_ear),
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> JobMetadata:
        data = data or {}
        firmware = data.get("firmware_version_by_ear") or {}
        if not isinstance(firmware, dict):
            firmware = {}
        return cls(
            test_name=data.get("test_name"),
            description=data.get("description"),
            environment=data.get("environment"),
            operator_notes=data.get("operator_notes"),
            firmware_version_by_ear={
                str(key): str(value) for key, value in firmware.items()
            },
        )


@dataclass
class PeriodRecord:
    index: int
    status: PeriodStatus
    planned_start_utc: datetime | None = None
    planned_end_utc: datetime | None = None
    actual_start_utc: datetime | None = None
    actual_end_utc: datetime | None = None
    pause_after_seconds: float | None = None

    @property
    def period_id(self) -> str:
        return f"period-{self.index:03d}"

    def to_dict(self) -> dict[str, Any]:
        return {
            "index": self.index,
            "period_id": self.period_id,
            "status": self.status,
            "planned_start_utc": format_utc(self.planned_start_utc),
            "planned_end_utc": format_utc(self.planned_end_utc),
            "actual_start_utc": format_utc(self.actual_start_utc),
            "actual_end_utc": format_utc(self.actual_end_utc),
            "pause_after_seconds": self.pause_after_seconds,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> PeriodRecord:
        return cls(
            index=int(data["index"]),
            status=data["status"],
            planned_start_utc=parse_utc(data.get("planned_start_utc")),
            planned_end_utc=parse_utc(data.get("planned_end_utc")),
            actual_start_utc=parse_utc(data.get("actual_start_utc")),
            actual_end_utc=parse_utc(data.get("actual_end_utc")),
            pause_after_seconds=data.get("pause_after_seconds"),
        )


@dataclass
class PauseRecord:
    """Wall-clock wait that is not an active test period.

    ``inter_period`` sits between consecutive active periods.
    ``final_collection`` follows the last active period so delayed Observations
    can still be polled; it is never treated as an active experiment window.
    """

    kind: PauseKind
    index: int
    planned_seconds: float
    status: PauseStatus = "planned"
    after_period_index: int = 0
    before_period_index: int | None = None
    planned_start_utc: datetime | None = None
    planned_end_utc: datetime | None = None
    actual_start_utc: datetime | None = None
    actual_end_utc: datetime | None = None

    @property
    def pause_id(self) -> str:
        if self.kind == "final_collection":
            return "final-collection"
        return f"inter-period-{self.index:03d}"

    def to_dict(self) -> dict[str, Any]:
        return {
            "pause_id": self.pause_id,
            "kind": self.kind,
            "index": self.index,
            "planned_seconds": self.planned_seconds,
            "status": self.status,
            "after_period_index": self.after_period_index,
            "before_period_index": self.before_period_index,
            "planned_start_utc": format_utc(self.planned_start_utc),
            "planned_end_utc": format_utc(self.planned_end_utc),
            "actual_start_utc": format_utc(self.actual_start_utc),
            "actual_end_utc": format_utc(self.actual_end_utc),
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> PauseRecord:
        return cls(
            kind=data["kind"],
            index=int(data["index"]),
            planned_seconds=float(data["planned_seconds"]),
            status=data.get("status") or "planned",
            after_period_index=int(data.get("after_period_index") or 0),
            before_period_index=(
                None
                if data.get("before_period_index") is None
                else int(data["before_period_index"])
            ),
            planned_start_utc=parse_utc(data.get("planned_start_utc")),
            planned_end_utc=parse_utc(data.get("planned_end_utc")),
            actual_start_utc=parse_utc(data.get("actual_start_utc")),
            actual_end_utc=parse_utc(data.get("actual_end_utc")),
        )


@dataclass
class CollectionHealth:
    sqlite_reconcile_attempted: bool = False
    sqlite_reconcile_succeeded: bool = False
    sqlite_reconcile_error: str | None = None
    http_poll_count: int = 0
    completeness: CollectionCompleteness = "potentially_incomplete"
    completeness_notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> CollectionHealth:
        data = data or {}
        return cls(
            sqlite_reconcile_attempted=bool(data.get("sqlite_reconcile_attempted", False)),
            sqlite_reconcile_succeeded=bool(data.get("sqlite_reconcile_succeeded", False)),
            sqlite_reconcile_error=data.get("sqlite_reconcile_error"),
            http_poll_count=int(data.get("http_poll_count") or 0),
            completeness=data.get("completeness") or "potentially_incomplete",
            completeness_notes=list(data.get("completeness_notes") or []),
        )


@dataclass
class JobManifest:
    job_id: str
    runner_version: str
    association_policy_version: int
    node_ids: list[str]
    period_seconds: float
    period_count: int
    pause_seconds: float
    progression: Progression
    status: JobStatus
    hub_base_url: str
    created_at_utc: datetime
    started_at_utc: datetime | None = None
    ended_at_utc: datetime | None = None
    metadata: JobMetadata = field(default_factory=JobMetadata)
    collection: CollectionHealth = field(default_factory=CollectionHealth)
    # Trailing poll window after the last active period (not an active period).
    # Absent / 0 on older manifests → no final collection pause.
    final_collection_pause_seconds: float = 0.0

    def to_dict(self) -> dict[str, Any]:
        return {
            "job_id": self.job_id,
            "runner_version": self.runner_version,
            "association_policy_version": self.association_policy_version,
            "node_ids": list(self.node_ids),
            "period_seconds": self.period_seconds,
            "period_count": self.period_count,
            "pause_seconds": self.pause_seconds,
            "final_collection_pause_seconds": self.final_collection_pause_seconds,
            "progression": self.progression,
            "status": self.status,
            "hub_base_url": self.hub_base_url,
            "created_at_utc": format_utc(self.created_at_utc),
            "started_at_utc": format_utc(self.started_at_utc),
            "ended_at_utc": format_utc(self.ended_at_utc),
            "metadata": self.metadata.to_dict(),
            "collection": self.collection.to_dict(),
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> JobManifest:
        created = parse_utc(data["created_at_utc"])
        if created is None:
            raise ValueError("manifest missing created_at_utc")
        return cls(
            job_id=str(data["job_id"]),
            runner_version=str(data.get("runner_version") or ""),
            association_policy_version=int(data.get("association_policy_version") or 1),
            node_ids=[str(item) for item in data.get("node_ids") or []],
            period_seconds=float(data["period_seconds"]),
            period_count=int(data["period_count"]),
            pause_seconds=float(data["pause_seconds"]),
            final_collection_pause_seconds=float(
                data.get("final_collection_pause_seconds") or 0.0
            ),
            progression=data["progression"],
            status=data["status"],
            hub_base_url=str(data.get("hub_base_url") or ""),
            created_at_utc=created,
            started_at_utc=parse_utc(data.get("started_at_utc")),
            ended_at_utc=parse_utc(data.get("ended_at_utc")),
            metadata=JobMetadata.from_dict(data.get("metadata")),
            collection=CollectionHealth.from_dict(data.get("collection")),
        )


@dataclass(frozen=True)
class AssociationResult:
    phase_by_received: PhaseKind
    period_id_by_received: str | None
    phase_by_event: PhaseKind
    period_id_by_event: str | None
    received_event_disagree: bool
    association_note: str
