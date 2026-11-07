from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from typing import Any
from uuid import uuid4

from fastapi import HTTPException
from sqlalchemy import delete, func, select
from sqlalchemy.orm import Session

from app.config import settings
from app.models import ScratchObject
from app.schemas import ScratchObjectView


def utcnow() -> datetime:
    return datetime.now(UTC)


def ensure_utc(value: datetime) -> datetime:
    """SQLite often returns naive datetimes; treat them as UTC."""
    if value.tzinfo is None:
        return value.replace(tzinfo=UTC)
    return value.astimezone(UTC)


def serialize_content(content: Any) -> tuple[str, int]:
    try:
        encoded = json.dumps(content, separators=(",", ":"), ensure_ascii=False)
    except (TypeError, ValueError) as exc:
        raise HTTPException(
            status_code=422, detail="content must be JSON-serializable"
        ) from exc
    size = len(encoded.encode("utf-8"))
    if size > settings.analysis_scratch_max_payload_bytes:
        raise HTTPException(
            status_code=413,
            detail=(
                "scratch payload exceeds "
                f"{settings.analysis_scratch_max_payload_bytes} bytes"
            ),
        )
    return encoded, size


def clamp_ttl(ttl_seconds: int | None) -> int:
    requested = (
        settings.analysis_scratch_default_ttl_s
        if ttl_seconds is None
        else ttl_seconds
    )
    if requested < 1:
        raise HTTPException(status_code=422, detail="ttl_seconds must be >= 1")
    if requested > settings.analysis_scratch_max_ttl_s:
        raise HTTPException(
            status_code=422,
            detail=(
                "ttl_seconds must not exceed "
                f"{settings.analysis_scratch_max_ttl_s} seconds"
            ),
        )
    return requested


def purge_expired(db: Session, *, now: datetime | None = None) -> int:
    when = now or utcnow()
    result = db.execute(delete(ScratchObject).where(ScratchObject.expires_at <= when))
    db.commit()
    return int(result.rowcount or 0)


_METADATA_KEYS = (
    "kind",
    "dataset",
    "dataset_title",
    "layer",
    "title",
    "role",
    "description",
)


def extract_metadata(content: Any) -> dict[str, Any]:
    """Pull small discovery fields from scratch content without returning the body."""
    if not isinstance(content, dict):
        return {}
    metadata: dict[str, Any] = {}
    for key in _METADATA_KEYS:
        value = content.get(key)
        if isinstance(value, str) and value.strip():
            metadata[key] = value.strip()
    return metadata


def _to_view(
    row: ScratchObject, *, now: datetime, include_content: bool = True
) -> ScratchObjectView:
    created_at = ensure_utc(row.created_at)
    expires_at = ensure_utc(row.expires_at)
    remaining = max(0, int((expires_at - now).total_seconds()))
    content = json.loads(row.content_json)
    return ScratchObjectView(
        id=row.id,
        namespace=row.namespace,
        content=content if include_content else None,
        content_bytes=row.content_bytes,
        created_at=created_at,
        expires_at=expires_at,
        created_by=row.created_by,
        created_role=row.created_role,
        ttl_remaining_s=remaining,
        metadata=extract_metadata(content),
    )


def create_scratch(
    db: Session,
    *,
    namespace: str,
    content: Any,
    ttl_seconds: int | None,
    created_by: str,
    created_role: str,
) -> ScratchObjectView:
    purge_expired(db)
    active = db.scalar(select(func.count()).select_from(ScratchObject)) or 0
    if active >= settings.analysis_scratch_max_objects:
        raise HTTPException(
            status_code=409,
            detail=(
                "scratch object limit reached "
                f"({settings.analysis_scratch_max_objects})"
            ),
        )
    encoded, size = serialize_content(content)
    ttl = clamp_ttl(ttl_seconds)
    now = utcnow()
    row = ScratchObject(
        id=str(uuid4()),
        namespace=namespace,
        content_json=encoded,
        content_bytes=size,
        created_at=now,
        expires_at=now + timedelta(seconds=ttl),
        created_by=created_by,
        created_role=created_role,
    )
    db.add(row)
    db.commit()
    db.refresh(row)
    return _to_view(row, now=now)


def get_scratch(db: Session, object_id: str) -> ScratchObjectView:
    purge_expired(db)
    row = db.get(ScratchObject, object_id)
    if row is None:
        raise HTTPException(status_code=404, detail="scratch object not found")
    return _to_view(row, now=utcnow())


def list_scratch(
    db: Session,
    *,
    namespace: str | None = None,
    include_content: bool = True,
) -> list[ScratchObjectView]:
    purge_expired(db)
    now = utcnow()
    stmt = select(ScratchObject).order_by(ScratchObject.created_at.desc())
    if namespace is not None:
        stmt = stmt.where(ScratchObject.namespace == namespace)
    rows = db.scalars(stmt).all()
    return [
        _to_view(row, now=now, include_content=include_content) for row in rows
    ]


def update_scratch(
    db: Session,
    object_id: str,
    *,
    content: Any | None,
    ttl_seconds: int | None,
) -> ScratchObjectView:
    purge_expired(db)
    row = db.get(ScratchObject, object_id)
    if row is None:
        raise HTTPException(status_code=404, detail="scratch object not found")
    now = utcnow()
    if content is not None:
        encoded, size = serialize_content(content)
        row.content_json = encoded
        row.content_bytes = size
    if ttl_seconds is not None:
        ttl = clamp_ttl(ttl_seconds)
        row.expires_at = now + timedelta(seconds=ttl)
    db.commit()
    db.refresh(row)
    return _to_view(row, now=now)


def delete_scratch(db: Session, object_id: str) -> int:
    purge_expired(db)
    row = db.get(ScratchObject, object_id)
    if row is None:
        raise HTTPException(status_code=404, detail="scratch object not found")
    db.delete(row)
    db.commit()
    return 1


def clear_scratch(db: Session, *, namespace: str | None = None) -> int:
    purge_expired(db)
    stmt = delete(ScratchObject)
    if namespace is not None:
        stmt = stmt.where(ScratchObject.namespace == namespace)
    result = db.execute(stmt)
    db.commit()
    return int(result.rowcount or 0)
