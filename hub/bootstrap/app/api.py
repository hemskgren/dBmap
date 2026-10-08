from fastapi import APIRouter, Depends, Header, HTTPException, Query
from sqlalchemy.orm import Session

from app import service
from app.config import settings
from app.db import get_db
from app.ids import timing_safe_eq
from app.mqtt_security import MqttSecurityError
from app.schemas import (
    CreateNodeRequest,
    CreateNodeResponse,
    DesiredStateBody,
    DeviceSummaryView,
    InstallationMetadataBody,
    NodeLifecycleBody,
    NodeView,
    ObservationView,
    ProvisionRequest,
    ProvisionResponse,
)

router = APIRouter()


def require_admin(authorization: str | None = Header(default=None)) -> None:
    role = _user_role(authorization)
    if role != "admin":
        raise HTTPException(status_code=403, detail="admin access required")


@router.get("/health")
def health() -> dict:
    return {"status": "ok", "service": "hub-bootstrap"}


def _user_role(authorization: str | None) -> str:
    if authorization is not None:
        scheme, separator, token = authorization.partition(" ")
        if separator and scheme.lower() == "bearer":
            if settings.admin_token and timing_safe_eq(token, settings.admin_token):
                return "admin"
            if settings.viewer_token and timing_safe_eq(token, settings.viewer_token):
                return "viewer"
    raise HTTPException(status_code=401, detail="valid local access token required")


def require_viewer(authorization: str | None = Header(default=None)) -> None:
    _user_role(authorization)


@router.get("/api/v1/session", dependencies=[Depends(require_viewer)])
def get_session(authorization: str | None = Header(default=None)) -> dict[str, str]:
    return {"role": _user_role(authorization)}


@router.get(
    "/api/v1/devices",
    response_model=list[DeviceSummaryView],
    dependencies=[Depends(require_viewer)],
)
def list_devices(db: Session = Depends(get_db)) -> list[DeviceSummaryView]:
    nodes = service.list_nodes(db)
    return [
        DeviceSummaryView(
            node_id=node.node_id,
            node_type=node.node_type,
            hardware_revision=node.hardware_revision,
            provisioning_state=node.provisioning_state,
            lifecycle_state=node.lifecycle_state,
            status=node.status,
            last_seen_at=node.last_seen_at,
            installation=node.installation,
        )
        for node in nodes
    ]


@router.post("/api/v1/nodes", response_model=CreateNodeResponse, dependencies=[Depends(require_admin)])
def create_node(body: CreateNodeRequest, db: Session = Depends(get_db)) -> CreateNodeResponse:
    return service.create_node(db, body)


@router.get("/api/v1/nodes", response_model=list[NodeView], dependencies=[Depends(require_admin)])
def list_nodes(db: Session = Depends(get_db)) -> list[NodeView]:
    return service.list_nodes(db)


@router.get("/api/v1/nodes/{node_id}", response_model=NodeView, dependencies=[Depends(require_admin)])
def get_node(node_id: str, db: Session = Depends(get_db)) -> NodeView:
    try:
        return service.get_node(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.put("/api/v1/nodes/{node_id}/desired", response_model=NodeView, dependencies=[Depends(require_admin)])
def put_desired(node_id: str, body: DesiredStateBody, db: Session = Depends(get_db)) -> NodeView:
    try:
        return service.set_desired(db, node_id, body)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.put(
    "/api/v1/nodes/{node_id}/lifecycle",
    response_model=NodeView,
    dependencies=[Depends(require_admin)],
)
def put_node_lifecycle(
    node_id: str,
    body: NodeLifecycleBody,
    db: Session = Depends(get_db),
) -> NodeView:
    try:
        return service.set_node_lifecycle(db, node_id, body)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None
    except MqttSecurityError as exc:
        if body.lifecycle_state == "active":
            detail = (
                "MQTT ACL restore failed; the node is active in Hub state but broker "
                "access remains suspended. Retry reactivation."
            )
        else:
            detail = "MQTT ACL suspension failed; the node remains active in Hub state."
        raise HTTPException(status_code=503, detail=detail) from exc


@router.put(
    "/api/v1/nodes/{node_id}/installation",
    response_model=NodeView,
    dependencies=[Depends(require_admin)],
)
def put_installation(
    node_id: str,
    body: InstallationMetadataBody,
    db: Session = Depends(get_db),
) -> NodeView:
    try:
        return service.set_installation(db, node_id, body)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.get(
    "/api/v1/observations",
    response_model=list[ObservationView],
    dependencies=[Depends(require_admin)],
)
def list_observations(
    node_id: str | None = None,
    limit: int = Query(default=100, ge=1, le=500),
    db: Session = Depends(get_db),
) -> list[ObservationView]:
    return service.list_observations(db, node_id, limit)


@router.post("/api/v1/provision", response_model=ProvisionResponse)
def provision(body: ProvisionRequest, db: Session = Depends(get_db)) -> ProvisionResponse:
    try:
        return service.provision(db, body)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except MqttSecurityError as exc:
        raise HTTPException(status_code=503, detail="MQTT credential provisioning failed") from exc
