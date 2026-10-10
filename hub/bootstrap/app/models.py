from datetime import datetime, timezone
from uuid import uuid4

from sqlalchemy import (
    CheckConstraint,
    DateTime,
    Float,
    ForeignKey,
    Integer,
    String,
    Text,
    UniqueConstraint,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"
    __table_args__ = (CheckConstraint("status IN ('active', 'disabled')"),)

    user_id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid4()))
    display_name: Mapped[str] = mapped_column(String(128))
    status: Mapped[str] = mapped_column(String(16), default="active")
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)

    external_identities: Mapped[list["ExternalIdentity"]] = relationship(
        back_populates="user", passive_deletes="all"
    )


class ExternalIdentity(Base):
    __tablename__ = "external_identities"
    __table_args__ = (
        UniqueConstraint("provider", "provider_subject", name="uq_external_provider_subject"),
        CheckConstraint("status IN ('active', 'disabled')"),
    )

    identity_id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid4()))
    user_id: Mapped[str] = mapped_column(
        String(36), ForeignKey("users.user_id", ondelete="RESTRICT"), index=True
    )
    provider: Mapped[str] = mapped_column(String(32))
    provider_subject: Mapped[str] = mapped_column(String(255))
    status: Mapped[str] = mapped_column(String(16), default="active")
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    last_authenticated_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)

    user: Mapped[User] = relationship(back_populates="external_identities")


class Node(Base):
    __tablename__ = "nodes"

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    node_type: Mapped[str] = mapped_column(String(16))  # ear | output
    hardware_revision: Mapped[str] = mapped_column(String(64), default="unknown")
    provisioning_state: Mapped[str] = mapped_column(String(32), default="pending")
    lifecycle_state: Mapped[str] = mapped_column(String(16), default="active")
    availability: Mapped[str] = mapped_column(String(32), default="SERVICE")
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)


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
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)


class BootstrapToken(Base):
    __tablename__ = "bootstrap_tokens"

    token_hash: Mapped[str] = mapped_column(String(64), primary_key=True)
    node_id: Mapped[str] = mapped_column(String(64), index=True)
    consumed_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class NodeCredential(Base):
    __tablename__ = "node_credentials"
    __table_args__ = (UniqueConstraint("mqtt_username", name="uq_mqtt_username"),)

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    mqtt_username: Mapped[str] = mapped_column(String(128))
    issued_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class DesiredState(Base):
    __tablename__ = "desired_state"

    node_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    firmware_version: Mapped[str] = mapped_column(String(32), default="")
    config_version: Mapped[int] = mapped_column(Integer, default=0)
    calibration_version: Mapped[int] = mapped_column(Integer, default=0)
    classifier_version: Mapped[str] = mapped_column(String(32), default="")
    payload_json: Mapped[str] = mapped_column(Text, default="{}")
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)


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
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, onupdate=utcnow)


class Observation(Base):
    __tablename__ = "observations"

    observation_id: Mapped[str] = mapped_column(String(36), primary_key=True)
    node_id: Mapped[str] = mapped_column(String(64), index=True)
    sequence_number: Mapped[int] = mapped_column(Integer)
    event_time_utc: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    received_time_utc: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    payload_json: Mapped[str] = mapped_column(Text)
