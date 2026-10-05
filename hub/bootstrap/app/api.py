from fastapi import APIRouter, Depends, Header, HTTPException, Query
from sqlalchemy.orm import Session

from app.config import settings
from app.db import get_db
from app.ids import timing_safe_eq
from app.schemas import (
    CreateNodeRequest,
    CreateNodeResponse,
    DesiredStateBody,
    InstallationMetadataBody,
    NodeView,
    ObservationView,
    ProvisionRequest,
    ProvisionResponse,
)
from app import service
from app.mqtt_security import MqttSecurityError

router = APIRouter()


def require_admin(authorization: str | None = Header(default=None)) -> None:
    expected = f"Bearer {settings.admin_token}"
    if authorization is None or not timing_safe_eq(authorization, expected):
        raise HTTPException(status_code=401, detail="admin token required")


@router.get("/health")
def health() -> dict:
    return {"status": "ok", "service": "hub-bootstrap"}


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
