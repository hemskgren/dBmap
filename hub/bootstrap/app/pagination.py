"""Shared Hub list pagination: params, totals, and integer snapshot freeze."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Generic, Literal, TypeVar

from sqlalchemy import ColumnElement, column, func, select
from sqlalchemy.orm import Session

ListOrder = Literal["newest", "oldest"]

DEFAULT_PAGE_SIZE = 200
MAX_PAGE_SIZE = 500

# SQLite assigns a monotonic rowid to ordinary tables (including UUID PKs).
# Used as snapshot_id for Users/Groups where the primary key is not monotonic.
SQLITE_ROWID = column("rowid")

T = TypeVar("T")


@dataclass(frozen=True)
class PageResult(Generic[T]):
    """One page of items plus pagination metadata."""

    items: list[T]
    total: int
    page: int
    page_size: int
    total_pages: int
    order: ListOrder
    snapshot_id: int | None


def validate_page_params(page: int, page_size: int, order: str) -> ListOrder:
    if page < 1:
        raise ValueError("page must be >= 1")
    if page_size < 1 or page_size > MAX_PAGE_SIZE:
        raise ValueError(f"page_size must be between 1 and {MAX_PAGE_SIZE}")
    if order not in ("newest", "oldest"):
        raise ValueError("order must be 'newest' or 'oldest'")
    return order  # type: ignore[return-value]


def compute_total_pages(total: int, page_size: int) -> int:
    return math.ceil(total / page_size) if total else 0


def resolve_snapshot_id(
    requested: int | None, current_max: int | None
) -> int | None:
    """
    Freeze the result set at an inclusive maximum integer boundary.

    When ``requested`` is omitted, use ``current_max`` from the live query.
    A requested id past the live max is clamped to the live max.
    """
    if requested is None:
        return current_max
    if current_max is None:
        return None
    if requested > current_max:
        return current_max
    return requested


def page_offset(page: int, page_size: int) -> int:
    return (page - 1) * page_size


def max_sqlite_rowid(
    db: Session,
    model: type,
    filters: list[ColumnElement[bool]] | None = None,
) -> int | None:
    statement = select(func.max(SQLITE_ROWID)).select_from(model)
    for clause in filters or ():
        statement = statement.where(clause)
    value = db.scalar(statement)
    return int(value) if value is not None else None


def count_rows(
    db: Session,
    model: type,
    filters: list[ColumnElement[bool]] | None = None,
) -> int:
    statement = select(func.count()).select_from(model)
    for clause in filters or ():
        statement = statement.where(clause)
    return int(db.scalar(statement) or 0)
