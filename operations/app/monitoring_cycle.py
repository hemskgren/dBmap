"""Shared sparse observation lifecycle for the Core and public monitor stores."""

import json
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timedelta

from sqlalchemy import delete, select
from sqlalchemy.orm import Session

from app import monitoring_availability
from app.monitoring_math import ensure_aware

SAMPLE_MODE_SPARSE = "sparse"


@dataclass(frozen=True)
class ObservationGap:
    start: datetime
    end: datetime
    exceeds_threshold: bool

    def marker_at(self, previous_at: datetime) -> datetime:
        """Place the boundary between the preceding and resumed observations."""
        return min(
            max(self.start, ensure_aware(previous_at) + timedelta(microseconds=1)),
            self.end - timedelta(microseconds=1),
        )


def checkpoint_time(
    value: str | None, *, anchor_at: datetime, observed_at: datetime
) -> datetime | None:
    """A checkpoint must bound real observations in the same timeline."""
    if not isinstance(value, str):
        return None
    try:
        parsed = datetime.fromisoformat(value)
        if parsed.utcoffset() is not None:
            parsed = ensure_aware(parsed)
            if ensure_aware(anchor_at) <= parsed <= observed_at:
                return parsed
    except (ValueError, OverflowError):
        pass
    return None


def plan_gap(
    observed_at: datetime,
    *,
    anchor_at: datetime | None,
    previous_check_at: datetime | None,
    persisted_checkpoint: str | None,
    threshold: timedelta,
) -> ObservationGap | None:
    """Use committed continuity first, falling back to the sparse anchor."""
    restart = False
    if previous_check_at is None and anchor_at is not None:
        saved_at = checkpoint_time(
            persisted_checkpoint, anchor_at=anchor_at, observed_at=observed_at
        )
        restart = saved_at is not None
        previous_check_at = saved_at or ensure_aware(anchor_at)
    if previous_check_at is None:
        return None
    elapsed = observed_at - previous_check_at
    delayed = elapsed > threshold
    if elapsed > timedelta(0) and (restart or delayed):
        return ObservationGap(previous_check_at, observed_at, delayed)
    return None


def coverage_end_if_stale(
    now: datetime,
    *,
    anchor_at: datetime,
    previous_check_at: datetime | None,
    persisted_checkpoint: str | None,
    threshold: timedelta,
) -> datetime | None:
    """End sparse coverage at the last confirmed check while the worker is stale."""
    if previous_check_at is not None and now - previous_check_at <= threshold:
        return None
    anchor_at = ensure_aware(anchor_at)
    confirmed_at = checkpoint_time(
        persisted_checkpoint, anchor_at=anchor_at, observed_at=now
    )
    boundary = max(
        confirmed_at or previous_check_at or anchor_at,
        anchor_at,
    )
    return boundary if boundary < now else None


def event_view(event) -> dict:
    return {
        "event_id": event.event_id,
        "occurred_at": ensure_aware(event.occurred_at).isoformat(),
        "observed_at": ensure_aware(event.observed_at).isoformat(),
        "component": event.component,
        "event_type": event.event_type,
        "previous_state": event.previous_state,
        "new_state": event.new_state,
        "reason": event.reason,
        "source": event.source,
        "certainty": event.certainty,
    }


def sample_payload(sample, legacy_components: tuple[str, ...] = ()) -> dict:
    try:
        payload = json.loads(sample.components_json)
    except (ValueError, TypeError):
        payload = {}
    if not isinstance(payload, dict):
        payload = {}
    states = payload.get("states")
    if not isinstance(states, dict):
        states = {
            key: payload[key]
            for key in legacy_components
            if isinstance(payload.get(key), str)
        }
    return {
        "states": {str(key): str(value) for key, value in states.items()},
        **{
            key: payload[key] if isinstance(payload.get(key), dict) else {}
            for key in ("last_success", "reasons", "docker")
        },
        "sample_mode": payload.get("sample_mode"),
    }


def is_sparse(sample, payload: dict) -> bool:
    return (
        sample.overall_availability == "unobserved"
        or payload.get("sample_mode") == SAMPLE_MODE_SPARSE
    )


def observations(samples, payloads: dict, *, component_id: str | None = None):
    points = []
    for sample in samples:
        payload = payloads[sample.sample_id]
        state = sample.overall_availability
        if component_id is not None:
            state = monitoring_availability.component_state(
                payload["states"].get(component_id)
            )
        points.append(
            monitoring_availability.Observation(
                sample.observed_at, state, sparse=is_sparse(sample, payload)
            )
        )
    return points


@dataclass(frozen=True)
class MonitorStore:
    sample: type
    event: type
    checkpoint: type
    hub_id: str
    source: str
    component: str | None = None
    legacy_components: tuple[str, ...] = ()
    component_aliases: tuple[tuple[str, str], ...] = ()

    @property
    def scope(self) -> dict[str, str]:
        scope = {"hub_id": self.hub_id}
        if self.component is not None:
            scope["component"] = self.component
        return scope

    def latest_sample(self, db: Session, *, before: datetime | None = None):
        query = select(self.sample).filter_by(**self.scope)
        if before is not None:
            query = query.where(self.sample.observed_at < before)
        return db.scalar(query.order_by(self.sample.observed_at.desc()).limit(1))

    def _retention_anchor(self, db: Session, cutoff: datetime):
        sample = self.latest_sample(db, before=cutoff)
        if sample is not None and is_sparse(sample, sample_payload(sample)):
            return sample
        return None

    def history(self, db: Session, now: datetime, days: int):
        cutoff = now - timedelta(days=days)
        anchor = self._retention_anchor(db, cutoff)
        timeline = db.scalars(
            select(self.sample)
            .filter_by(**self.scope)
            .where(self.sample.observed_at >= cutoff)
            .order_by(self.sample.observed_at)
        )
        return [anchor, *timeline] if anchor is not None else list(timeline)

    def prune(self, db: Session, now: datetime, days: int) -> dict:
        cutoff = now - timedelta(days=days)
        anchor = self._retention_anchor(db, cutoff)
        samples = (
            delete(self.sample)
            .filter_by(**self.scope)
            .where(self.sample.observed_at < cutoff)
        )
        if anchor is not None:
            samples = samples.where(self.sample.sample_id != anchor.sample_id)
        events = (
            delete(self.event)
            .filter_by(**self.scope)
            .where(self.event.occurred_at < cutoff)
        )
        # Let SQL compare timestamps. SQLite ORM rows can have naive datetimes.
        options = {"synchronize_session": False}
        return {
            "samples_deleted": db.execute(samples, execution_options=options).rowcount
            or 0,
            "events_deleted": db.execute(events, execution_options=options).rowcount
            or 0,
            "cutoff": cutoff.isoformat(),
            "retention_days": days,
        }

    def _checkpoint_at(self, db: Session) -> str | None:
        row = db.get(self.checkpoint, self.scope)
        return row.last_check_at if row is not None else None

    def stale_boundary(
        self,
        db: Session,
        points,
        now: datetime,
        *,
        previous_check_at: datetime | None,
        threshold: timedelta,
    ):
        if not points or not points[-1].sparse:
            return None
        end = coverage_end_if_stale(
            now,
            anchor_at=points[-1].observed_at,
            previous_check_at=previous_check_at,
            persisted_checkpoint=self._checkpoint_at(db),
            threshold=threshold,
        )
        if end is not None:
            return monitoring_availability.Observation(end, "unobserved")
        return None

    def recent_events(self, db: Session, limit: int, *, event_types=(), since=None):
        query = select(self.event).filter_by(**self.scope)
        if since is not None:
            query = query.where(self.event.occurred_at >= since)
        if event_types:
            query = query.where(self.event.event_type.in_(event_types))
        # Break ties consistently for multiple events from one observation.
        return list(
            db.scalars(
                query.order_by(
                    self.event.occurred_at.desc(), self.event.event_id.desc()
                ).limit(limit)
            )
        )

    def record_event(
        self,
        db: Session,
        *,
        component: str,
        event_type: str,
        previous_state: str | None,
        new_state: str | None,
        reason: str | None,
        occurred_at: datetime,
        observed_at: datetime,
        certainty: str = "confirmed",
    ) -> None:
        db.add(
            self.event(
                hub_id=self.hub_id,
                component=component,
                event_type=event_type,
                previous_state=previous_state,
                new_state=new_state,
                reason=(reason or "")[:256] or None,
                occurred_at=occurred_at,
                observed_at=observed_at,
                certainty=certainty,
                source=self.source,
            )
        )

    def _add_sample(
        self,
        db: Session,
        at: datetime,
        *,
        health: str,
        availability: str,
        payload: dict,
    ):
        sample = self.sample(
            **self.scope,
            observed_at=at,
            overall_health=health,
            overall_availability=availability,
            source=self.source,
            components_json=json.dumps(
                payload | {"sample_mode": SAMPLE_MODE_SPARSE},
                separators=(",", ":"),
                sort_keys=True,
            ),
        )
        db.add(sample)
        return sample

    def record_check(
        self,
        db: Session,
        observed_at: datetime,
        *,
        states: dict,
        reasons: dict,
        health: str,
        availability: str,
        previous_check_at: datetime | None,
        threshold: timedelta,
        last_success: dict | None = None,
        after_observation: Callable | None = None,
    ):
        previous = self.latest_sample(db)
        transaction = db.get_transaction()
        gap = plan_gap(
            observed_at,
            anchor_at=previous.observed_at if previous is not None else None,
            previous_check_at=previous_check_at,
            persisted_checkpoint=(
                self._checkpoint_at(db)
                if previous_check_at is None and previous is not None
                else None
            ),
            threshold=threshold,
        )
        prior_states = (
            sample_payload(previous, self.legacy_components)["states"]
            if previous
            else {}
        )
        for old_component, current_component in self.component_aliases:
            if current_component not in prior_states and old_component in prior_states:
                prior_states[current_component] = prior_states[old_component]
            prior_states.pop(old_component, None)
        if gap is not None and previous.overall_availability != "unobserved":
            self._add_sample(
                db,
                gap.marker_at(previous.observed_at),
                health="unknown",
                availability="unobserved",
                payload={
                    "states": {},
                    "last_success": {},
                    "gap": True,
                    "reasons": {"monitor": "unobserved monitoring gap"},
                },
            )
        if gap is not None and gap.exceeds_threshold:
            self.record_event(
                db,
                component=self.component or "monitor",
                event_type="monitoring_resumed",
                previous_state="unobserved" if self.component is None else "unknown",
                new_state=states.get(self.component, "running"),
                reason=f"monitoring resumed after an unobserved gap of "
                f"{int((gap.end - gap.start).total_seconds())}s starting {gap.start.isoformat()}",
                occurred_at=observed_at,
                observed_at=observed_at,
                certainty="inferred",
            )

        for component, state in states.items():
            old = prior_states.get(component)
            if state is not None and old != state:
                self.record_event(
                    db,
                    component=component,
                    event_type="state_changed",
                    previous_state=old,
                    new_state=state,
                    reason=reasons.get(component),
                    occurred_at=observed_at,
                    observed_at=observed_at,
                )

        latest = previous
        if (
            previous is None
            or gap is not None
            or prior_states != states
            or previous.overall_health != health
            or previous.overall_availability != availability
        ):
            latest = self._add_sample(
                db,
                observed_at,
                health=health,
                availability=availability,
                payload={
                    "states": states,
                    "last_success": last_success or {},
                    "reasons": reasons,
                },
            )
        if after_observation is not None:
            after_observation(previous, gap)
        if db.get_transaction() is not transaction:
            raise RuntimeError("Monitoring transaction was rolled back")
        db.merge(self.checkpoint(**self.scope, last_check_at=observed_at.isoformat()))
        db.commit()
        return latest
