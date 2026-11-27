from datetime import UTC, datetime
from uuid import uuid4

from sqlalchemy import CheckConstraint, DateTime, Index, String, Text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


def utcnow() -> datetime:
    return datetime.now(UTC)


class Base(DeclarativeBase):
    pass


class OperationsMonitorSample(Base):
    __tablename__ = "operations_monitor_samples"
    __table_args__ = (
        Index(
            "ix_operations_monitor_samples_hub_observed",
            "hub_id",
            "observed_at",
        ),
        CheckConstraint(
            "overall_availability IN "
            "('available', 'degraded', 'unavailable', 'unobserved')"
        ),
        CheckConstraint(
            "overall_health IN ('healthy', 'degraded', 'unhealthy', 'unknown')"
        ),
    )

    sample_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, index=True
    )
    overall_availability: Mapped[str] = mapped_column(String(16))
    overall_health: Mapped[str] = mapped_column(String(16))
    components_json: Mapped[str] = mapped_column(Text, default="{}")
    source: Mapped[str] = mapped_column(String(32), default="operations_monitor")


class OperationsMonitorEvent(Base):
    __tablename__ = "operations_monitor_events"
    __table_args__ = (
        Index(
            "ix_operations_monitor_events_hub_occurred",
            "hub_id",
            "occurred_at",
        ),
        CheckConstraint("certainty IN ('confirmed', 'inferred')"),
    )

    event_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    occurred_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, index=True
    )
    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    component: Mapped[str] = mapped_column(String(32))
    event_type: Mapped[str] = mapped_column(String(64))
    previous_state: Mapped[str | None] = mapped_column(String(32), nullable=True)
    new_state: Mapped[str | None] = mapped_column(String(32), nullable=True)
    reason: Mapped[str | None] = mapped_column(String(256), nullable=True)
    source: Mapped[str] = mapped_column(String(32), default="operations_monitor")
    certainty: Mapped[str] = mapped_column(String(16), default="confirmed")


class OperationsMonitorCheckpoint(Base):
    __tablename__ = "operations_monitor_checkpoints"

    hub_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    last_check_at: Mapped[str] = mapped_column(String(40))


class PublicMonitorSample(Base):
    __tablename__ = "operations_public_monitor_samples"
    __table_args__ = (
        Index(
            "ix_operations_public_samples_scope_observed",
            "hub_id",
            "component",
            "observed_at",
        ),
    )

    sample_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    component: Mapped[str] = mapped_column(String(32))
    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, index=True
    )
    overall_availability: Mapped[str] = mapped_column(String(16))
    overall_health: Mapped[str] = mapped_column(String(16))
    components_json: Mapped[str] = mapped_column(Text, default="{}")
    source: Mapped[str] = mapped_column(String(32), default="operations_public_monitor")


class PublicMonitorEvent(Base):
    __tablename__ = "operations_public_monitor_events"
    __table_args__ = (
        Index(
            "ix_operations_public_events_scope_occurred",
            "hub_id",
            "component",
            "occurred_at",
        ),
    )

    event_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    component: Mapped[str] = mapped_column(String(32))
    occurred_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, index=True
    )
    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    event_type: Mapped[str] = mapped_column(String(64))
    previous_state: Mapped[str | None] = mapped_column(String(32), nullable=True)
    new_state: Mapped[str | None] = mapped_column(String(32), nullable=True)
    reason: Mapped[str | None] = mapped_column(String(256), nullable=True)
    source: Mapped[str] = mapped_column(String(32), default="operations_public_monitor")
    certainty: Mapped[str] = mapped_column(String(16), default="confirmed")


class PublicMonitorCheckpoint(Base):
    __tablename__ = "operations_public_monitor_checkpoints"

    hub_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    component: Mapped[str] = mapped_column(String(32), primary_key=True)
    last_check_at: Mapped[str] = mapped_column(String(40))
