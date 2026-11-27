from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Literal, TypeVar

ListOrder = Literal["newest", "oldest"]

DEFAULT_PAGE_SIZE = 200
MAX_PAGE_SIZE = 500

T = TypeVar("T")


@dataclass(frozen=True)
class PageResult:
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
    requested: int | None,
    current_max: int | None,
) -> int | None:
    if requested is None:
        return current_max
    if current_max is None:
        return None
    return min(requested, current_max)
