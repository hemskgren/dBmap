from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request
from sqlalchemy import inspect, text
from sqlalchemy.orm import Session

from app import policy, service
from app.config import settings
from app.db import get_db
from app.ids import timing_safe_eq
from app.mqtt_listener import get_broker_metrics, is_connected
from app.mqtt_security import MqttSecurityError
from app.policy import PolicyUnavailable
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


@dataclass(frozen=True)
class Principal:
    subject_id: str
    role: str


@router.get("/health")
def health() -> dict:
    return {"status": "ok", "service": "hub-bootstrap"}


def _authenticate(authorization: str | None) -> Principal:
    if authorization is not None:
        scheme, separator, token = authorization.partition(" ")
        if separator and scheme.lower() == "bearer":
            if settings.admin_token and timing_safe_eq(token, settings.admin_token):
                return Principal(subject_id="local-admin", role="admin")
            if settings.viewer_token and timing_safe_eq(token, settings.viewer_token):
                return Principal(subject_id="local-viewer", role="viewer")
    raise HTTPException(status_code=401, detail="valid local access token required")


def require_policy(action: str, resource_type: str):
    def dependency(
        request: Request,
        authorization: str | None = Header(default=None),
    ) -> Principal:
        principal = _authenticate(authorization)
        resource = {"type": resource_type}
        resource_id = request.path_params.get("node_id")
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


@router.get("/api/v1/session")
def get_session(principal: Principal = Depends(require_policy("session.read", "session"))) -> dict[str, str]:
    return {"role": principal.role}


@router.get(
    "/api/v1/devices",
    response_model=list[DeviceSummaryView],
    dependencies=[Depends(require_policy("devices.list_summary", "device_summaries"))],
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


@router.get(
    "/api/v1/hub/status",
    dependencies=[Depends(require_policy("hub.status.read", "hub"))],
)
def get_hub_status(db: Session = Depends(get_db)) -> dict:
    db.execute(text("SELECT 1"))
    nodes = service.list_nodes(db)
    observations = service.list_observations(db, None, 500)
    if not settings.mqtt_enabled:
        mqtt_status = "disabled"
    else:
        mqtt_status = "online" if is_connected() else "offline"
    installation_completed_count = sum(node.installation is not None for node in nodes)
    return {
        "mode": "Local Hub",
        "hub_id": settings.site_id,
        "components": [
            {"name": "Hub API", "status": "online"},
            {"name": "Database", "status": "online"},
            {"name": "Policy engine", "status": "online"},
            {"name": "MQTT broker", "status": mqtt_status},
        ],
        "summary": {
            "device_count": len(nodes),
            "devices_by_type": dict(sorted(Counter(node.node_type for node in nodes).items())),
            "devices_by_lifecycle": dict(
                sorted(Counter(node.lifecycle_state for node in nodes).items())
            ),
            "devices_by_availability": dict(
                sorted(Counter(node.availability for node in nodes).items())
            ),
            "online_device_count": sum(node.status == "online" for node in nodes),
            "installation_completed_count": installation_completed_count,
            "installation_incomplete_count": len(nodes) - installation_completed_count,
            "observation_count": len(observations),
            "observation_limit": 500,
            "latest_observation_at": max(
                (item.received_time_utc.isoformat() for item in observations), default=None
            ),
        },
    }


@router.get(
    "/api/v1/hub/components/{component}/details",
    dependencies=[Depends(require_policy("hub.status.read", "hub"))],
)
def get_hub_component_details(component: str, db: Session = Depends(get_db)) -> dict:
    if component == "hub-api":
        return {
            "service": "Hub API",
            "status": "online",
            "api_version": "v1",
            "listener": "HTTPS on port 8443 inside the Compose network",
            "browser_route": "HTTPS through Nginx",
        }
    if component == "opa":
        return policy.health_summary()
    if component == "mosquitto":
        return {
            "service": "Mosquitto broker",
            "status": "disabled" if not settings.mqtt_enabled else "online" if is_connected() else "offline",
            "connected": is_connected(),
            **get_broker_metrics(),
        }
    if component != "database":
        raise HTTPException(status_code=404, detail="unknown Hub component")

    bind = db.get_bind()
    details: dict = {
        "service": "Database",
        "status": "online",
        "engine": bind.dialect.name,
        "driver": bind.dialect.driver,
        "tables": [],
    }
    if bind.dialect.name == "sqlite":
        details["version"] = db.execute(text("SELECT sqlite_version()")).scalar_one()
        page_count = db.execute(text("PRAGMA page_count")).scalar_one()
        page_size = db.execute(text("PRAGMA page_size")).scalar_one()
        free_pages = db.execute(text("PRAGMA freelist_count")).scalar_one()
        details["allocated_size_bytes"] = page_count * page_size
        details["free_size_bytes"] = free_pages * page_size
        database_path = bind.url.database
        if database_path and database_path != ":memory:":
            try:
                details["file_size_bytes"] = Path(database_path).stat().st_size
            except OSError:
                details["file_size_bytes"] = None

    inspector = inspect(bind)
    for table_name in sorted(inspector.get_table_names()):
        quoted_name = bind.dialect.identifier_preparer.quote(table_name)
        row_count = db.execute(text(f"SELECT COUNT(*) FROM {quoted_name}")).scalar_one()
        details["tables"].append({"name": table_name, "rows": row_count})
    return details


@router.post("/api/v1/nodes", response_model=CreateNodeResponse, dependencies=[Depends(require_policy("nodes.create", "nodes"))])
def create_node(body: CreateNodeRequest, db: Session = Depends(get_db)) -> CreateNodeResponse:
    return service.create_node(db, body)


@router.get("/api/v1/nodes", response_model=list[NodeView], dependencies=[Depends(require_policy("nodes.list", "nodes"))])
def list_nodes(db: Session = Depends(get_db)) -> list[NodeView]:
    return service.list_nodes(db)


@router.get("/api/v1/nodes/{node_id}", response_model=NodeView, dependencies=[Depends(require_policy("nodes.read_detail", "nodes"))])
def get_node(node_id: str, db: Session = Depends(get_db)) -> NodeView:
    try:
        return service.get_node(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.put("/api/v1/nodes/{node_id}/desired", response_model=NodeView, dependencies=[Depends(require_policy("nodes.update_desired", "nodes"))])
def put_desired(node_id: str, body: DesiredStateBody, db: Session = Depends(get_db)) -> NodeView:
    try:
        return service.set_desired(db, node_id, body)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.put(
    "/api/v1/nodes/{node_id}/lifecycle",
    response_model=NodeView,
    dependencies=[Depends(require_policy("nodes.update_lifecycle", "nodes"))],
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
    dependencies=[Depends(require_policy("nodes.update_installation", "nodes"))],
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
    dependencies=[Depends(require_policy("observations.read", "observations"))],
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
