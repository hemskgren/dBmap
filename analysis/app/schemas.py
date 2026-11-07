from datetime import datetime
from typing import Any

from pydantic import BaseModel, Field, model_validator

NAMESPACE_PATTERN = r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"


class ScratchCreateBody(BaseModel):
    namespace: str = Field(min_length=1, max_length=64, pattern=NAMESPACE_PATTERN)
    content: Any
    ttl_seconds: int | None = Field(default=None, ge=1)


class ScratchUpdateBody(BaseModel):
    content: Any | None = None
    ttl_seconds: int | None = Field(default=None, ge=1)

    @model_validator(mode="after")
    def require_change(self) -> "ScratchUpdateBody":
        if self.content is None and self.ttl_seconds is None:
            raise ValueError("provide content and/or ttl_seconds")
        return self


class ScratchObjectView(BaseModel):
    id: str
    namespace: str
    content: Any | None = None
    content_bytes: int
    created_at: datetime
    expires_at: datetime
    created_by: str
    created_role: str
    ttl_remaining_s: int
    # Discovery fields derived from content (map-layer kind, dataset, title, …).
    metadata: dict[str, Any] = Field(default_factory=dict)


class ScratchListView(BaseModel):
    objects: list[ScratchObjectView]
    count: int


class ScratchDeleteResult(BaseModel):
    deleted: int
