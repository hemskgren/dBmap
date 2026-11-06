from __future__ import annotations

import hmac
import re
from dataclasses import dataclass

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request
from sqlalchemy.orm import Session

from app import policy, service
from app.config import settings
from app.db import get_db
from app.policy import PolicyUnavailable
from app.schemas import (
    NAMESPACE_PATTERN,
    ScratchCreateBody,
    ScratchDeleteResult,
    ScratchListView,
    ScratchObjectView,
    ScratchUpdateBody,
)

router = APIRouter()

_NAMESPACE_RE = re.compile(NAMESPACE_PATTERN)


@dataclass(frozen=True)
class Principal:
    subject_id: str
    role: str


def timing_safe_eq(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


@router.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok", "service": "analysis"}


def _authenticate(authorization: str | None) -> Principal:
    if authorization is not None:
        scheme, separator, token = authorization.partition(" ")
        if separator and scheme.lower() == "bearer":
            if settings.admin_token and timing_safe_eq(token, settings.admin_token):
                return Principal(subject_id="local-admin", role="admin")
            if settings.viewer_token and timing_safe_eq(token, settings.viewer_token):
                return Principal(subject_id="local-viewer", role="viewer")
    raise HTTPException(status_code=401, detail="valid local access token required")


def require_policy(action: str, resource_type: str = "analysis_scratch"):
    def dependency(
        request: Request,
        authorization: str | None = Header(default=None),
    ) -> Principal:
        principal = _authenticate(authorization)
        resource: dict[str, str] = {"type": resource_type}
        resource_id = request.path_params.get("object_id")
        if resource_id is not None:
            resource["id"] = resource_id
        decision_input = {
            "subject": {
                "id": principal.subject_id,
                "role": principal.role,
                "authenticated": True,
            },
            "action": action,
            "resource": resource,
            "context": {},
        }
        try:
            allowed = policy.evaluate(decision_input)
        except PolicyUnavailable as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc
        if not allowed:
            raise HTTPException(status_code=403, detail="access denied by policy")
        return principal

    return dependency


def _optional_namespace(namespace: str | None) -> str | None:
    if namespace is None:
        return None
    if not _NAMESPACE_RE.fullmatch(namespace):
        raise HTTPException(status_code=422, detail="invalid namespace")
    return namespace


@router.get("/api/v1/analysis/scratch", response_model=ScratchListView)
def list_scratch_objects(
    namespace: str | None = Query(default=None),
    include_content: bool = Query(
        default=True,
        description=(
            "When false, omit content bodies and return metadata only "
            "(for map dataset discovery)."
        ),
    ),
    _: Principal = Depends(require_policy("analysis.scratch.read")),
    db: Session = Depends(get_db),
) -> ScratchListView:
    objects = service.list_scratch(
        db,
        namespace=_optional_namespace(namespace),
        include_content=include_content,
    )
    return ScratchListView(objects=objects, count=len(objects))


@router.post("/api/v1/analysis/scratch", response_model=ScratchObjectView)
def create_scratch_object(
    body: ScratchCreateBody,
    principal: Principal = Depends(require_policy("analysis.scratch.write")),
    db: Session = Depends(get_db),
) -> ScratchObjectView:
    return service.create_scratch(
        db,
        namespace=body.namespace,
        content=body.content,
        ttl_seconds=body.ttl_seconds,
        created_by=principal.subject_id,
        created_role=principal.role,
    )


@router.get("/api/v1/analysis/scratch/{object_id}", response_model=ScratchObjectView)
def get_scratch_object(
    object_id: str,
    _: Principal = Depends(require_policy("analysis.scratch.read")),
    db: Session = Depends(get_db),
) -> ScratchObjectView:
    return service.get_scratch(db, object_id)


@router.put("/api/v1/analysis/scratch/{object_id}", response_model=ScratchObjectView)
def update_scratch_object(
    object_id: str,
    body: ScratchUpdateBody,
    _: Principal = Depends(require_policy("analysis.scratch.write")),
    db: Session = Depends(get_db),
) -> ScratchObjectView:
    return service.update_scratch(
        db,
        object_id,
        content=body.content,
        ttl_seconds=body.ttl_seconds,
    )


@router.delete(
    "/api/v1/analysis/scratch/{object_id}", response_model=ScratchDeleteResult
)
def delete_scratch_object(
    object_id: str,
    _: Principal = Depends(require_policy("analysis.scratch.delete")),
    db: Session = Depends(get_db),
) -> ScratchDeleteResult:
    return ScratchDeleteResult(deleted=service.delete_scratch(db, object_id))


@router.delete("/api/v1/analysis/scratch", response_model=ScratchDeleteResult)
def clear_scratch_objects(
    namespace: str | None = Query(default=None),
    _: Principal = Depends(require_policy("analysis.scratch.delete")),
    db: Session = Depends(get_db),
) -> ScratchDeleteResult:
    return ScratchDeleteResult(
        deleted=service.clear_scratch(db, namespace=_optional_namespace(namespace))
    )
