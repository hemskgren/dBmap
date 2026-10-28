from datetime import datetime, timezone
from uuid import uuid4

from sqlalchemy import (
    CheckConstraint,
    DateTime,
    Float,
    ForeignKey,
    Index,
    Integer,
    String,
    Text,
    UniqueConstraint,
    text,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"
    __table_args__ = (CheckConstraint("status IN ('active', 'disabled')"),)

    user_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    display_name: Mapped[str] = mapped_column(String(128))
    status: Mapped[str] = mapped_column(String(16), default="active")
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )

    external_identities: Mapped[list["ExternalIdentity"]] = relationship(
        back_populates="user", passive_deletes="all"
    )


class Group(Base):
    __tablename__ = "groups"
    __table_args__ = (CheckConstraint("status IN ('active', 'disabled')"),)

    group_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    name: Mapped[str] = mapped_column(String(128))
    created_by_user_id: Mapped[str] = mapped_column(
        String(36), ForeignKey("users.user_id", ondelete="RESTRICT"), index=True
    )
    status: Mapped[str] = mapped_column(String(16), default="active")
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )


class GroupMembership(Base):
    __tablename__ = "group_memberships"
    __table_args__ = (
        CheckConstraint("role IN ('owner', 'member', 'viewer')"),
        CheckConstraint("valid_to IS NULL OR valid_to >= valid_from"),
        Index(
            "uq_group_membership_current",
            "group_id",
            "user_id",
            unique=True,
            sqlite_where=text("valid_to IS NULL"),
            postgresql_where=text("valid_to IS NULL"),
        ),
    )

    membership_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    group_id: Mapped[str] = mapped_column(
        String(36), ForeignKey("groups.group_id", ondelete="RESTRICT"), index=True
    )
    user_id: Mapped[str] = mapped_column(
        String(36), ForeignKey("users.user_id", ondelete="RESTRICT"), index=True
    )
    role: Mapped[str] = mapped_column(String(16))
    valid_from: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    valid_to: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class ExternalIdentity(Base):
    __tablename__ = "external_identities"
    __table_args__ = (
        UniqueConstraint(
            "provider", "provider_subject", name="uq_external_provider_subject"
        ),
        CheckConstraint("status IN ('active', 'disabled')"),
    )

    identity_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    user_id: Mapped[str] = mapped_column(
        String(36), ForeignKey("users.user_id", ondelete="RESTRICT"), index=True
    )
    provider: Mapped[str] = mapped_column(String(32))
    provider_subject: Mapped[str] = mapped_column(String(255))
    status: Mapped[str] = mapped_column(String(16), default="active")
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    last_authenticated_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )

    user: Mapped[User] = relationship(back_populates="external_identities")


class Node(Base):
    __tablename__ = "nodes"

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    node_type: Mapped[str] = mapped_column(String(16))  # ear | output
    hardware_revision: Mapped[str] = mapped_column(String(64), default="unknown")
    # Unique when set (partial index created in db._migrate_node_chip_identity).
    chip_mac: Mapped[str | None] = mapped_column(String(17), nullable=True, index=True)
    chip_variant: Mapped[str | None] = mapped_column(String(128), nullable=True)
    provisioning_state: Mapped[str] = mapped_column(String(32), default="pending")
    lifecycle_state: Mapped[str] = mapped_column(String(16), default="active")
    recovery_opened_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    availability: Mapped[str] = mapped_column(String(32), default="SERVICE")
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )


class DeviceOwnership(Base):
    __tablename__ = "device_ownership"
    __table_args__ = (
        CheckConstraint("owner_type IN ('user', 'group')"),
        CheckConstraint(
            "(owner_type = 'user' AND owner_user_id IS NOT NULL AND owner_group_id IS NULL) OR "
            "(owner_type = 'group' AND owner_group_id IS NOT NULL AND owner_user_id IS NULL)"
        ),
        CheckConstraint("valid_to IS NULL OR valid_to >= valid_from"),
        Index(
            "uq_device_ownership_current",
            "node_id",
            unique=True,
            sqlite_where=text("valid_to IS NULL"),
            postgresql_where=text("valid_to IS NULL"),
        ),
    )

    assignment_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    node_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("nodes.node_id", ondelete="RESTRICT"), index=True
    )
    owner_type: Mapped[str] = mapped_column(String(8), default="user")
    owner_user_id: Mapped[str | None] = mapped_column(
        String(36),
        ForeignKey("users.user_id", ondelete="RESTRICT"),
        nullable=True,
        index=True,
    )
    owner_group_id: Mapped[str | None] = mapped_column(
        String(36),
        ForeignKey("groups.group_id", ondelete="RESTRICT"),
        nullable=True,
        index=True,
    )
    valid_from: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    valid_to: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class RegistrationRequest(Base):
    __tablename__ = "registration_requests"

    idempotency_key: Mapped[str] = mapped_column(String(36), primary_key=True)
    node_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("nodes.node_id", ondelete="RESTRICT"), unique=True
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )


class FirmwareCatalogEntry(Base):
    __tablename__ = "firmware_catalog"
    __table_args__ = (CheckConstraint("status IN ('candidate', 'registered')"),)

    firmware_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    status: Mapped[str] = mapped_column(String(16), default="candidate")
    metadata_json: Mapped[str] = mapped_column(Text)
    artifact_sha256: Mapped[str] = mapped_column(String(64))
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    registered_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class DeploymentAttempt(Base):
    __tablename__ = "deployment_attempts"
    __table_args__ = (
        CheckConstraint("status IN ('in_progress', 'succeeded', 'failed', 'unknown')"),
        CheckConstraint(
            "current_stage IN "
            "('connecting', 'erasing', 'writing', 'verifying', 'completed', 'failed', 'unknown')"
        ),
        CheckConstraint("progress_percent >= 0 AND progress_percent <= 100"),
        Index("ix_deployment_attempts_node_started", "node_id", "started_at"),
        Index(
            "uq_deployment_attempts_node_active",
            "node_id",
            unique=True,
            sqlite_where=text("status = 'in_progress'"),
            postgresql_where=text("status = 'in_progress'"),
        ),
    )

    attempt_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    node_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("nodes.node_id", ondelete="RESTRICT")
    )
    firmware_id: Mapped[str] = mapped_column(String(64))
    firmware_version: Mapped[str] = mapped_column(String(64))
    artifact_sha256: Mapped[str] = mapped_column(String(64))
    status: Mapped[str] = mapped_column(String(16), default="in_progress")
    current_stage: Mapped[str] = mapped_column(String(16), default="connecting")
    progress_percent: Mapped[int] = mapped_column(Integer, default=0)
    failure_code: Mapped[str | None] = mapped_column(String(32), nullable=True)
    initiated_by: Mapped[str] = mapped_column(String(64))
    flasher_version: Mapped[str | None] = mapped_column(String(64), nullable=True)
    started_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    completed_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class DeploymentAttemptEvent(Base):
    __tablename__ = "deployment_attempt_events"
    __table_args__ = (
        CheckConstraint("status IN ('in_progress', 'succeeded', 'failed', 'unknown')"),
        CheckConstraint(
            "stage IN "
            "('connecting', 'erasing', 'writing', 'verifying', 'completed', 'failed', 'unknown')"
        ),
        CheckConstraint("progress_percent >= 0 AND progress_percent <= 100"),
        Index(
            "ix_deployment_attempt_events_attempt_created", "attempt_id", "created_at"
        ),
    )

    event_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    attempt_id: Mapped[str] = mapped_column(
        String(36), ForeignKey("deployment_attempts.attempt_id", ondelete="CASCADE")
    )
    status: Mapped[str] = mapped_column(String(16))
    stage: Mapped[str] = mapped_column(String(16))
    progress_percent: Mapped[int] = mapped_column(Integer)
    failure_code: Mapped[str | None] = mapped_column(String(32), nullable=True)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )


class InstallationMetadata(Base):
    __tablename__ = "installation_metadata"

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    latitude: Mapped[float] = mapped_column(Float)
    longitude: Mapped[float] = mapped_column(Float)
    floor: Mapped[int | None] = mapped_column(Integer, nullable=True)
    height_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    height_accuracy_m: Mapped[float | None] = mapped_column(Float, nullable=True)
    mount_type: Mapped[str | None] = mapped_column(String(64), nullable=True)
    environment: Mapped[str | None] = mapped_column(String(64), nullable=True)
    orientation_deg: Mapped[float | None] = mapped_column(Float, nullable=True)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )


class BootstrapToken(Base):
    __tablename__ = "bootstrap_tokens"
    __table_args__ = (
        Index(
            "uq_bootstrap_tokens_one_unconsumed_per_node",
            "node_id",
            unique=True,
            sqlite_where=text("consumed_at IS NULL"),
            postgresql_where=text("consumed_at IS NULL"),
        ),
    )

    token_hash: Mapped[str] = mapped_column(String(64), primary_key=True)
    node_id: Mapped[str] = mapped_column(String(64), index=True)
    consumed_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )


class NodeCredential(Base):
    __tablename__ = "node_credentials"
    __table_args__ = (
        UniqueConstraint("mqtt_username", name="uq_mqtt_username"),
        Index(
            "uq_node_credentials_device_api_token_hash",
            "device_api_token_hash",
            unique=True,
            sqlite_where=text("device_api_token_hash IS NOT NULL"),
            postgresql_where=text("device_api_token_hash IS NOT NULL"),
        ),
    )

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    mqtt_username: Mapped[str] = mapped_column(String(128))
    issued_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    device_api_token_hash: Mapped[str | None] = mapped_column(String(64), nullable=True)
    device_api_issued_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    device_api_revoked_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )


class DesiredState(Base):
    __tablename__ = "desired_state"

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    firmware_id: Mapped[str] = mapped_column(String(64), default="")
    firmware_version: Mapped[str] = mapped_column(String(32), default="")
    config_version: Mapped[int] = mapped_column(Integer, default=0)
    calibration_version: Mapped[int] = mapped_column(Integer, default=0)
    classifier_version: Mapped[str] = mapped_column(String(32), default="")
    payload_json: Mapped[str] = mapped_column(Text, default="{}")
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )


class ReportedState(Base):
    __tablename__ = "reported_state"

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    firmware_version: Mapped[str] = mapped_column(String(32), default="")
    hardware_revision: Mapped[str] = mapped_column(String(64), default="")
    config_version: Mapped[int] = mapped_column(Integer, default=0)
    calibration_version: Mapped[int] = mapped_column(Integer, default=0)
    classifier_version: Mapped[str] = mapped_column(String(32), default="")
    uptime_s: Mapped[int] = mapped_column(Integer, default=0)
    pending_configuration_change: Mapped[int] = mapped_column(Integer, default=0)
    payload_json: Mapped[str] = mapped_column(Text, default="{}")
    last_seen_at: Mapped[datetime | None] = mapped_column(
        DateTime(timezone=True), nullable=True
    )
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )


class Observation(Base):
    __tablename__ = "observations"

    observation_id: Mapped[str] = mapped_column(String(36), primary_key=True)
    node_id: Mapped[str] = mapped_column(String(64), index=True)
    sequence_number: Mapped[int] = mapped_column(Integer)
    event_time_utc: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), index=True
    )
    received_time_utc: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    payload_json: Mapped[str] = mapped_column(Text)


class HubMonitorSample(Base):
    __tablename__ = "hub_monitor_samples"
    __table_args__ = (
        Index("ix_hub_monitor_samples_hub_observed", "hub_id", "observed_at"),
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
    source: Mapped[str] = mapped_column(String(32), default="local_monitor")


class HubMonitorEvent(Base):
    __tablename__ = "hub_monitor_events"
    __table_args__ = (
        Index("ix_hub_monitor_events_hub_occurred", "hub_id", "occurred_at"),
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
    source: Mapped[str] = mapped_column(String(32), default="local_monitor")
    certainty: Mapped[str] = mapped_column(String(16), default="confirmed")


class DeviceMonitorSample(Base):
    """Sparse Hub-observed connectivity samples (transition + gap markers)."""

    __tablename__ = "device_monitor_samples"
    __table_args__ = (
        Index("ix_device_monitor_samples_node_observed", "node_id", "observed_at"),
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
    node_id: Mapped[str] = mapped_column(String(64), index=True)
    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, index=True
    )
    overall_availability: Mapped[str] = mapped_column(String(16))
    overall_health: Mapped[str] = mapped_column(String(16), default="unknown")
    source: Mapped[str] = mapped_column(String(32), default="hub_observed")


class DeviceMonitorEvent(Base):
    __tablename__ = "device_monitor_events"
    __table_args__ = (
        Index("ix_device_monitor_events_node_occurred", "node_id", "occurred_at"),
        CheckConstraint("certainty IN ('confirmed', 'inferred')"),
    )

    event_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    node_id: Mapped[str] = mapped_column(String(64), index=True)
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
    source: Mapped[str] = mapped_column(String(32), default="hub_observed")
    certainty: Mapped[str] = mapped_column(String(16), default="confirmed")


class DeviceDiagEvent(Base):
    """Retained device-reported diagnostic events. Hard cap of 30 rows per node."""

    __tablename__ = "device_diag_events"
    __table_args__ = (
        Index("ix_device_diag_events_node_occurred", "node_id", "occurred_at"),
        CheckConstraint("certainty IN ('confirmed', 'inferred')"),
        CheckConstraint("event_count >= 1"),
    )

    event_id: Mapped[str] = mapped_column(
        String(36), primary_key=True, default=lambda: str(uuid4())
    )
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    node_id: Mapped[str] = mapped_column(String(64), index=True)
    occurred_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, index=True
    )
    observed_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    first_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    last_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow
    )
    event_type: Mapped[str] = mapped_column(String(64))
    event_count: Mapped[int] = mapped_column(Integer, default=1)
    reason: Mapped[str | None] = mapped_column(String(256), nullable=True)
    detail_json: Mapped[str] = mapped_column(Text, default="{}")
    source: Mapped[str] = mapped_column(String(32), default="device_reported")
    certainty: Mapped[str] = mapped_column(String(16), default="confirmed")


class DeviceDiagState(Base):
    """Latest diag snapshot and Hub-assessed health per device."""

    __tablename__ = "device_diag_state"
    __table_args__ = (
        CheckConstraint(
            "last_health IN ('healthy', 'degraded', 'unhealthy', 'unknown')"
        ),
    )

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    hub_id: Mapped[str] = mapped_column(String(64), index=True)
    last_diag_json: Mapped[str] = mapped_column(Text, default="{}")
    has_diag_evidence: Mapped[int] = mapped_column(Integer, default=0)
    last_health: Mapped[str] = mapped_column(String(16), default="unknown")
    last_health_reason: Mapped[str | None] = mapped_column(String(256), nullable=True)
    # Pending OTA transition awaiting post-reboot hello/boot_confirmed.
    pending_ota_json: Mapped[str] = mapped_column(Text, default="{}")
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=utcnow, onupdate=utcnow
    )
