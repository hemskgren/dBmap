from datetime import datetime

from sqlalchemy import DateTime, Index, Integer, String, Text
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class ScratchObject(Base):
    """Ephemeral script-uploaded data. Not used for persistent analysis records."""

    __tablename__ = "scratch_objects"
    __table_args__ = (
        Index("ix_scratch_objects_expires_at", "expires_at"),
        Index("ix_scratch_objects_namespace", "namespace"),
    )

    id: Mapped[str] = mapped_column(String(36), primary_key=True)
    namespace: Mapped[str] = mapped_column(String(64), nullable=False)
    content_json: Mapped[str] = mapped_column(Text, nullable=False)
    content_bytes: Mapped[int] = mapped_column(Integer, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    created_by: Mapped[str] = mapped_column(String(64), nullable=False)
    created_role: Mapped[str] = mapped_column(String(16), nullable=False)
