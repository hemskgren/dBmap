"""Period / job state machine for the EAR test runner."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from uuid import uuid4

from lib.ear_test import ASSOCIATION_POLICY_VERSION, RUNNER_VERSION
from lib.ear_test.models import (
    JobManifest,
    JobMetadata,
    PauseRecord,
    PeriodRecord,
    Progression,
    utc_now,
)


def make_job_id(now: datetime | None = None) -> str:
    stamp = (now or utc_now()).astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"{stamp}-{uuid4().hex[:8]}"


@dataclass
class JobController:
    """Owns period and pause lifecycle. Callers supply wall-clock via ``now`` for tests."""

    manifest: JobManifest
    periods: list[PeriodRecord] = field(default_factory=list)
    pauses: list[PauseRecord] = field(default_factory=list)
    _current_index: int | None = None
    _current_pause_key: tuple[str, int] | None = None

    @classmethod
    def create(
        cls,
        *,
        node_ids: list[str],
        period_seconds: float,
        period_count: int,
        pause_seconds: float,
        progression: Progression,
        hub_base_url: str,
        metadata: JobMetadata | None = None,
        final_collection_pause_seconds: float = 0.0,
        now: datetime | None = None,
    ) -> JobController:
        if period_seconds <= 0:
            raise ValueError("period_seconds must be > 0")
        if period_count < 1:
            raise ValueError("period_count must be >= 1")
        if pause_seconds < 0:
            raise ValueError("pause_seconds must be >= 0")
        if final_collection_pause_seconds < 0:
            raise ValueError("final_collection_pause_seconds must be >= 0")
        if not node_ids:
            raise ValueError("at least one node_id is required")
        normalized = [item.strip().upper() for item in node_ids]
        if any(not item for item in normalized):
            raise ValueError("node_id must be non-empty")
        if len(set(normalized)) != len(normalized):
            raise ValueError("node_ids must be unique")

        now = now or utc_now()
        job_id = make_job_id(now)
        periods = [
            PeriodRecord(
                index=index,
                status="planned",
                pause_after_seconds=pause_seconds if index < period_count - 1 else 0.0,
            )
            for index in range(period_count)
        ]
        pauses: list[PauseRecord] = []
        for index in range(period_count - 1):
            if pause_seconds > 0:
                pauses.append(
                    PauseRecord(
                        kind="inter_period",
                        index=index,
                        planned_seconds=float(pause_seconds),
                        status="planned",
                        after_period_index=index,
                        before_period_index=index + 1,
                    )
                )
        if final_collection_pause_seconds > 0:
            pauses.append(
                PauseRecord(
                    kind="final_collection",
                    index=0,
                    planned_seconds=float(final_collection_pause_seconds),
                    status="planned",
                    after_period_index=period_count - 1,
                    before_period_index=None,
                )
            )
        manifest = JobManifest(
            job_id=job_id,
            runner_version=RUNNER_VERSION,
            association_policy_version=ASSOCIATION_POLICY_VERSION,
            node_ids=normalized,
            period_seconds=float(period_seconds),
            period_count=int(period_count),
            pause_seconds=float(pause_seconds),
            final_collection_pause_seconds=float(final_collection_pause_seconds),
            progression=progression,
            status="running",
            hub_base_url=hub_base_url.rstrip("/"),
            created_at_utc=now,
            started_at_utc=now,
            metadata=metadata or JobMetadata(),
        )
        return cls(manifest=manifest, periods=periods, pauses=pauses)

    @property
    def current_period(self) -> PeriodRecord | None:
        if self._current_index is None:
            return None
        return self.periods[self._current_index]

    @property
    def current_pause(self) -> PauseRecord | None:
        if self._current_pause_key is None:
            return None
        kind, index = self._current_pause_key
        for pause in self.pauses:
            if pause.kind == kind and pause.index == index:
                return pause
        return None

    @property
    def next_planned_period(self) -> PeriodRecord | None:
        for period in self.periods:
            if period.status == "planned":
                return period
        return None

    @property
    def next_planned_pause(self) -> PauseRecord | None:
        for pause in self.pauses:
            if pause.status == "planned":
                return pause
        return None

    def inter_period_pauses(self) -> list[PauseRecord]:
        return [p for p in self.pauses if p.kind == "inter_period"]

    def final_collection_pause(self) -> PauseRecord | None:
        for pause in self.pauses:
            if pause.kind == "final_collection":
                return pause
        return None

    def start_next_period(self, now: datetime | None = None) -> PeriodRecord:
        if self.manifest.status != "running":
            raise RuntimeError(f"job is {self.manifest.status}; cannot start a period")
        if self._current_index is not None:
            current = self.periods[self._current_index]
            if current.status == "active":
                raise RuntimeError("a period is already active; end it first")
        if self.current_pause is not None and self.current_pause.status == "active":
            raise RuntimeError("a pause is already active; end it first")

        period = self.next_planned_period
        if period is None:
            raise RuntimeError("no planned periods remain")

        now = now or utc_now()
        period.status = "active"
        period.actual_start_utc = now
        period.planned_start_utc = now
        period.planned_end_utc = now + timedelta(seconds=self.manifest.period_seconds)
        self._current_index = period.index
        return period

    def complete_current_period(self, now: datetime | None = None) -> PeriodRecord:
        period = self._require_active()
        now = now or utc_now()
        period.status = "completed"
        period.actual_end_utc = now
        self._current_index = None
        return period

    def start_next_pause(self, now: datetime | None = None) -> PauseRecord:
        if self.manifest.status != "running":
            raise RuntimeError(f"job is {self.manifest.status}; cannot start a pause")
        if self._current_index is not None:
            current = self.periods[self._current_index]
            if current.status == "active":
                raise RuntimeError("a period is already active; end it first")
        if self.current_pause is not None and self.current_pause.status == "active":
            raise RuntimeError("a pause is already active; end it first")

        pause = self.next_planned_pause
        if pause is None:
            raise RuntimeError("no planned pauses remain")

        now = now or utc_now()
        pause.status = "active"
        pause.actual_start_utc = now
        pause.planned_start_utc = now
        pause.planned_end_utc = now + timedelta(seconds=pause.planned_seconds)
        self._current_pause_key = (pause.kind, pause.index)
        return pause

    def complete_current_pause(self, now: datetime | None = None) -> PauseRecord:
        pause = self._require_active_pause()
        now = now or utc_now()
        pause.status = "completed"
        pause.actual_end_utc = now
        self._current_pause_key = None
        return pause

    def interrupt(self, now: datetime | None = None) -> None:
        """Mark job (and any active period/pause) interrupted. Preserve collected data externally."""
        now = now or utc_now()
        if self._current_pause_key is not None:
            pause = self.current_pause
            if pause is not None and pause.status == "active":
                pause.status = "interrupted"
                pause.actual_end_utc = now
            self._current_pause_key = None
        if self._current_index is not None:
            period = self.periods[self._current_index]
            if period.status == "active":
                period.status = "interrupted"
                period.actual_end_utc = now
            self._current_index = None
        for period in self.periods:
            if period.status == "planned":
                period.status = "skipped"
        for pause in self.pauses:
            if pause.status == "planned":
                pause.status = "skipped"
        if self.manifest.status == "running":
            self.manifest.status = "interrupted"
        self.manifest.ended_at_utc = now

    def mark_completed(self, now: datetime | None = None) -> None:
        now = now or utc_now()
        if self._current_index is not None:
            raise RuntimeError("cannot complete job while a period is active")
        if self.current_pause is not None and self.current_pause.status == "active":
            raise RuntimeError("cannot complete job while a pause is active")
        remaining = [p for p in self.periods if p.status == "planned"]
        for period in remaining:
            period.status = "skipped"
        for pause in self.pauses:
            if pause.status == "planned":
                pause.status = "skipped"
        self.manifest.status = "completed"
        self.manifest.ended_at_utc = now

    def all_periods_terminal(self) -> bool:
        return all(p.status in {"completed", "skipped", "interrupted"} for p in self.periods)

    def _require_active(self) -> PeriodRecord:
        if self._current_index is None:
            raise RuntimeError("no active period")
        period = self.periods[self._current_index]
        if period.status != "active":
            raise RuntimeError(f"period {period.period_id} is {period.status}")
        return period

    def _require_active_pause(self) -> PauseRecord:
        pause = self.current_pause
        if pause is None or pause.status != "active":
            raise RuntimeError("no active pause")
        return pause
