import json
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from uuid import UUID

from fastapi import (
    APIRouter,
    Depends,
    File,
    Header,
    HTTPException,
    Query,
    Request,
    UploadFile,
)
from fastapi.responses import FileResponse, Response
from sqlalchemy import inspect, text
from sqlalchemy.orm import Session

from app import (
    container_logs,
    deployment_service,
    edge_status,
    firmware_catalog,
    group_service,
    identity_service,
    ownership_service,
    policy,
    service,
)
from app.config import settings
from app.db import get_db
from app.ids import timing_safe_eq
from app.models import Node
from app.mqtt_listener import get_broker_metrics, is_connected
from app.mqtt_security import MqttSecurityError
from app.policy import PolicyUnavailable
from app.schemas import (
    BootstrapTokenResponse,
    CreateGroupBody,
    CreateNodeRequest,
    CreateNodeResponse,
    CreateUserBody,
    DeploymentAttemptCreateBody,
    DeploymentAttemptUpdateBody,
    DesiredStateBody,
    DeviceApiTokenResponse,
    DeviceDesiredFirmware,
    DeviceOwnerBody,
    DeviceOwnershipView,
    DeviceSummaryView,
    FirmwareSelectionBody,
    GroupMemberBody,
    GroupStatusBody,
    GroupView,
    HardwareRevisionBody,
    InstallationMetadataBody,
    LinkExternalIdentityBody,
    NodeLifecycleBody,
    NodeView,
    ObservationView,
    OutputCommandBody,
    OutputCommandView,
    ProvisionRequest,
    ProvisionResponse,
    UserStatusBody,
    UserView,
)

router = APIRouter()
FIRMWARE_CATALOG_DIR = Path(settings.firmware_catalog_dir)
MAX_BUNDLE_BYTES = firmware_catalog.MAX_BUNDLE_BYTES
MAX_METADATA_BYTES = firmware_catalog.MAX_METADATA_BYTES


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
        db: Session = Depends(get_db),
    ) -> Principal:
        principal = _authenticate(authorization)
        resource = {"type": resource_type}
        resource_id = request.path_params.get("node_id") or request.path_params.get("user_id")
        if resource_id is None:
            resource_id = request.path_params.get("identity_id")
        if resource_id is not None:
            resource["id"] = resource_id
            if resource_type == "nodes":
                owner = ownership_service.get_current_owner(db, resource_id)
                resource["owner_id"] = owner[1] if owner else None
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
    "/api/v1/users",
    response_model=list[UserView],
    dependencies=[Depends(require_policy("users.read", "users"))],
)
def list_users(db: Session = Depends(get_db)) -> list[UserView]:
    return identity_service.list_users(db)


@router.post(
    "/api/v1/users",
    response_model=UserView,
    dependencies=[Depends(require_policy("users.create", "users"))],
)
def create_user(body: CreateUserBody, db: Session = Depends(get_db)) -> UserView:
    return identity_service.create_user(db, body.display_name)


@router.get(
    "/api/v1/users/{user_id}",
    response_model=UserView,
    dependencies=[Depends(require_policy("users.read", "users"))],
)
def get_user(user_id: str, db: Session = Depends(get_db)) -> UserView:
    try:
        return identity_service.get_user(db, user_id)
    except identity_service.UserNotFound:
        raise HTTPException(status_code=404, detail="unknown user") from None


@router.put(
    "/api/v1/users/{user_id}/status",
    response_model=UserView,
    dependencies=[Depends(require_policy("users.update_status", "users"))],
)
def put_user_status(user_id: str, body: UserStatusBody, db: Session = Depends(get_db)) -> UserView:
    try:
        return identity_service.set_user_status(db, user_id, body.status)
    except identity_service.UserNotFound:
        raise HTTPException(status_code=404, detail="unknown user") from None


@router.post(
    "/api/v1/users/{user_id}/identities",
    response_model=UserView,
    dependencies=[Depends(require_policy("users.identities.link", "users"))],
)
def link_user_identity(
    user_id: str,
    body: LinkExternalIdentityBody,
    db: Session = Depends(get_db),
) -> UserView:
    try:
        return identity_service.link_github_identity(db, user_id, body.provider_subject)
    except identity_service.UserNotFound:
        raise HTTPException(status_code=404, detail="unknown user") from None
    except identity_service.IdentityConflict:
        raise HTTPException(status_code=409, detail="identity is already linked or disabled") from None


@router.put(
    "/api/v1/users/{user_id}/identities/{identity_id}/status",
    response_model=UserView,
    dependencies=[Depends(require_policy("users.identities.update_status", "users"))],
)
def put_identity_status(
    user_id: str,
    identity_id: str,
    body: UserStatusBody,
    db: Session = Depends(get_db),
) -> UserView:
    try:
        return identity_service.set_identity_status(db, user_id, identity_id, body.status)
    except identity_service.UserNotFound:
        raise HTTPException(status_code=404, detail="unknown user identity") from None


@router.get(
    "/api/v1/groups",
    response_model=list[GroupView],
    dependencies=[Depends(require_policy("groups.read", "groups"))],
)
def list_groups(db: Session = Depends(get_db)) -> list[GroupView]:
    return group_service.list_groups(db)


@router.post(
    "/api/v1/groups",
    response_model=GroupView,
    dependencies=[Depends(require_policy("groups.create", "groups"))],
)
def create_group(body: CreateGroupBody, db: Session = Depends(get_db)) -> GroupView:
    if body.owner_user_id is None:
        raise HTTPException(status_code=422, detail="owner_user_id is required")
    try:
        return group_service.create_group(
            db,
            body.name,
            body.owner_user_id,
            settings.group_creation_limit_per_user,
        )
    except group_service.InvalidGroupMember:
        raise HTTPException(status_code=404, detail="unknown or disabled owner user") from None
    except group_service.GroupCreationLimit:
        raise HTTPException(status_code=409, detail="group creation limit reached") from None


@router.put(
    "/api/v1/groups/{group_id}/members/{user_id}",
    response_model=GroupView,
    dependencies=[Depends(require_policy("groups.update", "groups"))],
)
def put_group_member(
    group_id: str,
    user_id: str,
    body: GroupMemberBody,
    db: Session = Depends(get_db),
) -> GroupView:
    try:
        return group_service.set_member_role(db, group_id, user_id, body.role)
    except group_service.GroupNotFound:
        raise HTTPException(status_code=404, detail="unknown group") from None
    except group_service.InvalidGroupMember:
        raise HTTPException(status_code=404, detail="unknown or disabled user") from None
    except group_service.LastGroupOwner:
        raise HTTPException(status_code=409, detail="a group must have at least one owner") from None


@router.delete(
    "/api/v1/groups/{group_id}/members/{user_id}",
    response_model=GroupView,
    dependencies=[Depends(require_policy("groups.update", "groups"))],
)
def delete_group_member(group_id: str, user_id: str, db: Session = Depends(get_db)) -> GroupView:
    try:
        return group_service.remove_member(db, group_id, user_id)
    except group_service.GroupNotFound:
        raise HTTPException(status_code=404, detail="unknown group") from None
    except group_service.LastGroupOwner:
        raise HTTPException(status_code=409, detail="a group must have at least one owner") from None


@router.put(
    "/api/v1/groups/{group_id}/status",
    response_model=GroupView,
    dependencies=[Depends(require_policy("groups.update", "groups"))],
)
def put_group_status(
    group_id: str,
    body: GroupStatusBody,
    db: Session = Depends(get_db),
) -> GroupView:
    try:
        return group_service.set_group_status(db, group_id, body.status)
    except group_service.GroupNotFound:
        raise HTTPException(status_code=404, detail="unknown group") from None


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
            owner_type=node.owner_type,
            owner_id=node.owner_id,
            owner_display_name=node.owner_display_name,
        )
        for node in nodes
    ]


@router.get("/api/v1/firmware/catalog")
def list_firmware_catalog(
    principal: Principal = Depends(require_policy("firmware.catalog.read", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> list[dict]:
    return firmware_catalog.list_entries(
        db,
        FIRMWARE_CATALOG_DIR,
        include_candidates=True,
    )


@router.get("/api/v1/firmware/catalog/{firmware_id}")
def get_firmware_metadata(
    firmware_id: str,
    principal: Principal = Depends(require_policy("firmware.catalog.read", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> dict:
    try:
        entry = firmware_catalog.get_entry(
            db,
            FIRMWARE_CATALOG_DIR,
            firmware_id,
            include_candidate=True,
        )
        return firmware_catalog.public_entry(entry, FIRMWARE_CATALOG_DIR)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="registered firmware not found") from None


@router.get("/api/v1/firmware/catalog/{firmware_id}/artifact")
def download_firmware_artifact(
    firmware_id: str,
    principal: Principal = Depends(require_policy("firmware.catalog.read", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> FileResponse:
    try:
        entry = firmware_catalog.get_entry(
            db,
            FIRMWARE_CATALOG_DIR,
            firmware_id,
            include_candidate=True,
        )
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="registered firmware not found") from None
    try:
        firmware_catalog.verify_artifact_integrity(entry, FIRMWARE_CATALOG_DIR)
    except firmware_catalog.FirmwareArtifactIntegrityError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    return FileResponse(
        FIRMWARE_CATALOG_DIR / f"{entry.firmware_id}.zip",
        media_type="application/zip",
        filename=f"{entry.firmware_id}.zip",
        headers={"Cache-Control": "no-store"},
    )


@router.get("/api/v1/firmware/catalog/{firmware_id}/flash-manifest")
def get_firmware_flash_manifest(
    firmware_id: str,
    allow_candidate: bool = Query(default=False),
    _: Principal = Depends(require_policy("firmware.deploy.flash.read", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> dict:
    try:
        entry = firmware_catalog.get_entry(
            db,
            FIRMWARE_CATALOG_DIR,
            firmware_id,
            include_candidate=allow_candidate,
        )
        return firmware_catalog.get_flash_manifest(entry, FIRMWARE_CATALOG_DIR)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="registered firmware not found") from None
    except firmware_catalog.FirmwareArtifactIntegrityError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    except firmware_catalog.FirmwarePlatformUnsupported as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from None


@router.get("/api/v1/firmware/catalog/{firmware_id}/flash-images/{component_name}")
def download_firmware_flash_image(
    firmware_id: str,
    component_name: str,
    allow_candidate: bool = Query(default=False),
    _: Principal = Depends(require_policy("firmware.deploy.flash.read", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> Response:
    try:
        entry = firmware_catalog.get_entry(
            db,
            FIRMWARE_CATALOG_DIR,
            firmware_id,
            include_candidate=allow_candidate,
        )
        image, digest = firmware_catalog.get_flash_image(
            entry,
            FIRMWARE_CATALOG_DIR,
            component_name,
        )
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="registered firmware image not found") from None
    except firmware_catalog.FirmwareArtifactIntegrityError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    return Response(
        content=image,
        media_type="application/octet-stream",
        headers={
            "Cache-Control": "no-store",
            "X-Content-SHA256": digest,
        },
    )


@router.post("/api/v1/firmware/catalog", status_code=201)
async def upload_firmware_candidate(
    firmware: UploadFile = File(),
    metadata: UploadFile = File(),
    _: Principal = Depends(require_policy("firmware.catalog.upload", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> dict:
    bundle = await firmware.read(MAX_BUNDLE_BYTES + 1)
    metadata_bytes = await metadata.read(MAX_METADATA_BYTES + 1)
    if len(bundle) > MAX_BUNDLE_BYTES or len(metadata_bytes) > MAX_METADATA_BYTES:
        raise HTTPException(status_code=413, detail="Firmware ZIP or metadata JSON exceeds the upload size limit.")
    try:
        parsed_metadata = json.loads(metadata_bytes)
        firmware_id = parsed_metadata.get("firmware_id") if isinstance(parsed_metadata, dict) else None
        if not isinstance(firmware_id, str):
            raise firmware_catalog.InvalidFirmwareBundle("Metadata must include a string firmware_id.")
        if firmware.filename != f"{firmware_id}.zip" or metadata.filename != f"{firmware_id}.json":
            raise firmware_catalog.InvalidFirmwareBundle(
                "Firmware and metadata filenames must match metadata firmware_id."
            )
        entry = firmware_catalog.create_candidate(
            db,
            FIRMWARE_CATALOG_DIR,
            firmware_id,
            bundle,
            metadata_bytes,
        )
        return firmware_catalog.public_entry(entry, FIRMWARE_CATALOG_DIR)
    except firmware_catalog.FirmwareAlreadyExists as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    except firmware_catalog.InvalidFirmwareBundle as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from None
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise HTTPException(status_code=422, detail="Metadata sidecar must contain valid UTF-8 JSON.") from None


@router.post("/api/v1/firmware/catalog/{firmware_id}/promote")
def promote_firmware_candidate(
    firmware_id: str,
    _: Principal = Depends(require_policy("firmware.catalog.promote", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> dict:
    try:
        return firmware_catalog.promote_entry(db, FIRMWARE_CATALOG_DIR, firmware_id)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="firmware candidate not found") from None


@router.post("/api/v1/firmware/catalog/{firmware_id}/unregister")
def return_firmware_to_candidate(
    firmware_id: str,
    _: Principal = Depends(require_policy("firmware.catalog.unpromote", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> dict:
    try:
        return firmware_catalog.return_to_candidate(db, FIRMWARE_CATALOG_DIR, firmware_id)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="firmware entry not found") from None
    except firmware_catalog.FirmwareDeploymentInProgress as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None


@router.delete("/api/v1/firmware/catalog/{firmware_id}", status_code=204)
def delete_firmware_entry(
    firmware_id: str,
    _: Principal = Depends(require_policy("firmware.catalog.delete", "firmware_catalog")),
    db: Session = Depends(get_db),
) -> None:
    try:
        firmware_catalog.delete_entry(db, FIRMWARE_CATALOG_DIR, firmware_id)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="firmware entry not found") from None


@router.get("/api/v1/nodes/{node_id}/compatible-firmware")
def list_compatible_firmware(
    node_id: str,
    include_candidates: bool = Query(default=False),
    _: Principal = Depends(require_policy("firmware.deploy.select", "nodes")),
    db: Session = Depends(get_db),
) -> list[dict]:
    node = db.get(Node, node_id)
    if node is None:
        raise HTTPException(status_code=404, detail="unknown device") from None
    if node.lifecycle_state != "active":
        raise HTTPException(status_code=409, detail="firmware cannot be selected for an inactive device")
    return firmware_catalog.list_compatible_entries(
        db,
        FIRMWARE_CATALOG_DIR,
        node.node_type,
        node.hardware_revision,
        include_candidates=include_candidates,
    )


@router.post("/api/v1/nodes/{node_id}/firmware-selection")
def validate_firmware_selection(
    node_id: str,
    body: FirmwareSelectionBody,
    _: Principal = Depends(require_policy("firmware.deploy.select", "nodes")),
    db: Session = Depends(get_db),
) -> dict:
    node = db.get(Node, node_id)
    if node is None:
        raise HTTPException(status_code=404, detail="unknown device") from None
    if node.lifecycle_state != "active":
        raise HTTPException(status_code=409, detail="firmware cannot be selected for an inactive device")
    try:
        selection = firmware_catalog.validate_selection(
            db,
            FIRMWARE_CATALOG_DIR,
            node_id,
            body.firmware_id,
            node.node_type,
            node.hardware_revision,
            body.acknowledge_unverified_platform,
            allow_candidate=body.allow_candidate,
        )
        entry = firmware_catalog.get_entry(
            db,
            FIRMWARE_CATALOG_DIR,
            body.firmware_id,
            include_candidate=body.allow_candidate,
        )
        metadata = json.loads(entry.metadata_json)
        service.set_desired_firmware(
            db,
            node_id,
            entry.firmware_id,
            str(metadata.get("firmware_version") or ""),
        )
        db.commit()
        return selection
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="registered firmware not found") from None
    except firmware_catalog.FirmwareNotCompatible as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from None
    except firmware_catalog.FirmwarePlatformUnverified as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None


@router.post("/api/v1/nodes/{node_id}/deployment-attempts", status_code=201)
def start_deployment_attempt(
    node_id: str,
    body: DeploymentAttemptCreateBody,
    principal: Principal = Depends(require_policy("firmware.deploy.attempt.start", "nodes")),
    db: Session = Depends(get_db),
) -> dict:
    node = db.get(Node, node_id)
    if node is None:
        raise HTTPException(status_code=404, detail="unknown device") from None
    if node.lifecycle_state != "active":
        raise HTTPException(status_code=409, detail="firmware cannot be deployed to an inactive device")
    try:
        firmware_catalog.validate_selection(
            db,
            FIRMWARE_CATALOG_DIR,
            node_id,
            body.firmware_id,
            node.node_type,
            node.hardware_revision,
            body.acknowledge_unverified_platform,
            allow_candidate=body.allow_candidate,
        )
        entry = firmware_catalog.get_entry(
            db,
            FIRMWARE_CATALOG_DIR,
            body.firmware_id,
            include_candidate=body.allow_candidate,
        )
        firmware_catalog.verify_artifact_integrity(entry, FIRMWARE_CATALOG_DIR)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=404, detail="registered firmware not found") from None
    except firmware_catalog.FirmwareNotCompatible as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from None
    except (
        firmware_catalog.FirmwareArtifactIntegrityError,
        firmware_catalog.FirmwarePlatformUnverified,
    ) as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None

    metadata = json.loads(entry.metadata_json)
    try:
        service.set_desired_firmware(
            db,
            node_id,
            entry.firmware_id,
            str(metadata.get("firmware_version") or ""),
        )
        attempt = deployment_service.create_attempt(
            db=db,
            node_id=node_id,
            firmware_id=entry.firmware_id,
            firmware_version=metadata["firmware_version"],
            artifact_sha256=entry.artifact_sha256,
            initiated_by=principal.subject_id,
            flasher_version=body.flasher_version,
        )
    except deployment_service.AttemptAlreadyInProgress:
        raise HTTPException(status_code=409, detail="a deployment attempt is already in progress") from None
    return deployment_service.public_attempt(db, attempt)


@router.get("/api/v1/nodes/{node_id}/deployment-attempts")
def list_deployment_attempts(
    node_id: str,
    limit: int = Query(default=50, ge=1, le=200),
    _: Principal = Depends(require_policy("firmware.deploy.attempt.read", "nodes")),
    db: Session = Depends(get_db),
) -> list[dict]:
    if db.get(Node, node_id) is None:
        raise HTTPException(status_code=404, detail="unknown device") from None
    attempts = deployment_service.list_attempts(db, node_id, limit)
    return [deployment_service.public_attempt(db, attempt) for attempt in attempts]


@router.get("/api/v1/nodes/{node_id}/deployment-attempts/{attempt_id}")
def get_deployment_attempt(
    node_id: str,
    attempt_id: str,
    _: Principal = Depends(require_policy("firmware.deploy.attempt.read", "nodes")),
    db: Session = Depends(get_db),
) -> dict:
    if db.get(Node, node_id) is None:
        raise HTTPException(status_code=404, detail="unknown device") from None
    try:
        attempt = deployment_service.get_attempt(db, node_id, attempt_id)
    except deployment_service.AttemptNotFound:
        raise HTTPException(status_code=404, detail="deployment attempt not found") from None
    return deployment_service.public_attempt(db, attempt)


@router.patch("/api/v1/nodes/{node_id}/deployment-attempts/{attempt_id}")
def update_deployment_attempt(
    node_id: str,
    attempt_id: str,
    body: DeploymentAttemptUpdateBody,
    _: Principal = Depends(require_policy("firmware.deploy.attempt.update", "nodes")),
    db: Session = Depends(get_db),
) -> dict:
    if db.get(Node, node_id) is None:
        raise HTTPException(status_code=404, detail="unknown device") from None
    try:
        attempt = deployment_service.update_attempt(
            db,
            node_id,
            attempt_id,
            body.status,
            body.stage,
            body.progress_percent,
            body.failure_code,
        )
    except deployment_service.AttemptNotFound:
        raise HTTPException(status_code=404, detail="deployment attempt not found") from None
    except deployment_service.AttemptAlreadyFinished:
        raise HTTPException(status_code=409, detail="deployment attempt is already complete") from None
    except deployment_service.AttemptProgressRegressed:
        raise HTTPException(status_code=409, detail="deployment progress cannot move backwards") from None
    return deployment_service.public_attempt(db, attempt)


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
        "hub_id": settings.hub_id,
        "addresses": {
            "local_hub_url": settings.local_hub_url(),
            "external_hub_url": settings.public_hub_url.strip().rstrip("/") or None,
            "local_mqtt": {"host": settings.local_mqtt_host, "port": settings.local_mqtt_port, "use_tls": True},
            "external_mqtt": (
                {"host": settings.public_mqtt_host, "port": settings.public_mqtt_port, "use_tls": settings.public_mqtt_use_tls}
                if settings.public_hub_url.strip() else None
            ),
        },
        "components": _hub_components(mqtt_status, edge_status.statuses()),
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
    probed = edge_status.detail(component)
    if probed is not None:
        return probed
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


def _hub_components(mqtt_status: str, edge: dict[str, str]) -> list[dict]:
    return [
        {"id": "nginx", "name": "Nginx", "status": edge["nginx"], "logs": True},
        {"id": "web", "name": "Web", "status": edge["web"], "logs": True},
        {"id": "hub-api", "name": "Hub API", "status": "online", "logs": True},
        {"id": "database", "name": "Database", "status": "online", "logs": False},
        {"id": "opa", "name": "Policy engine", "status": "online", "logs": True},
        {"id": "mosquitto", "name": "MQTT broker", "status": mqtt_status, "logs": True},
        {"id": "firmware-builder", "name": "Firmware builder", "status": edge["firmware-builder"], "logs": True},
    ]


@router.get(
    "/api/v1/hub/components/{component}/logs",
    dependencies=[Depends(require_policy("hub.logs.read", "hub"))],
)
def get_hub_component_logs(component: str) -> dict:
    try:
        return container_logs.read_tail(component)
    except container_logs.UnknownComponent:
        raise HTTPException(status_code=404, detail="unknown Hub component") from None
    except container_logs.ContainerNotFound as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from None
    except container_logs.ContainerLogsUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from None


@router.post("/api/v1/nodes", response_model=CreateNodeResponse, dependencies=[Depends(require_policy("nodes.create", "nodes"))])
def create_node(
    body: CreateNodeRequest,
    idempotency_key: UUID | None = Header(default=None, alias="Idempotency-Key"),
    db: Session = Depends(get_db),
) -> CreateNodeResponse:
    try:
        if body.firmware_id:
            entry = firmware_catalog.get_entry(
                db,
                FIRMWARE_CATALOG_DIR,
                body.firmware_id,
                include_candidate=False,
            )
            metadata = firmware_catalog.public_entry(entry, FIRMWARE_CATALOG_DIR)
            if metadata["status"] != "registered":
                raise HTTPException(status_code=422, detail="Select an approved firmware release.")
            if metadata["node_type"] != body.node_type:
                raise HTTPException(
                    status_code=422,
                    detail="Firmware is not supported for the selected device type.",
                )
            if (
                metadata["hardware_revision"] != "generic"
                and metadata["hardware_revision"] != body.hardware_revision
            ):
                raise HTTPException(
                    status_code=422,
                    detail="Firmware is not supported for the selected hardware revision.",
                )
            body.desired_firmware_version = metadata["firmware_version"]
        return service.create_node(db, body, str(idempotency_key) if idempotency_key else None)
    except firmware_catalog.FirmwareNotFound:
        raise HTTPException(status_code=422, detail="Selected firmware is no longer available or approved.") from None
    except ownership_service.OwnershipTargetNotFound:
        db.rollback()
        raise HTTPException(status_code=404, detail="unknown or disabled owner") from None
    except ownership_service.InactiveOwner:
        db.rollback()
        raise HTTPException(status_code=409, detail="disabled owners cannot own devices") from None
    except service.DuplicateRegistration as exc:
        raise HTTPException(
            status_code=409,
            detail=f"registration already completed for this attempt as {exc.node_id}; bootstrap token cannot be recovered",
        ) from None


@router.get("/api/v1/nodes", response_model=list[NodeView], dependencies=[Depends(require_policy("nodes.list", "nodes"))])
def list_nodes(db: Session = Depends(get_db)) -> list[NodeView]:
    return service.list_nodes(db)


@router.get("/api/v1/nodes/{node_id}", response_model=NodeView, dependencies=[Depends(require_policy("nodes.read_detail", "nodes"))])
def get_node(node_id: str, db: Session = Depends(get_db)) -> NodeView:
    try:
        return service.get_node(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.post(
    "/api/v1/nodes/{node_id}/bootstrap-token",
    response_model=BootstrapTokenResponse,
    dependencies=[Depends(require_policy("nodes.bootstrap_token.rotate", "nodes"))],
)
def rotate_node_bootstrap_token(
    node_id: str,
    response: Response,
    db: Session = Depends(get_db),
) -> BootstrapTokenResponse:
    try:
        token = service.rotate_pending_bootstrap_token(db, node_id)
        node = service.get_node(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None
    except service.BootstrapTokenRotationUnavailable as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    response.headers["Cache-Control"] = "no-store"
    return BootstrapTokenResponse(
        node_id=node.node_id,
        bootstrap_token=token,
        provisioning_state=node.provisioning_state,
    )


@router.get(
    "/api/v1/nodes/{node_id}/ownership",
    response_model=DeviceOwnershipView,
    dependencies=[Depends(require_policy("nodes.read_ownership", "nodes"))],
)
def get_node_ownership(node_id: str, db: Session = Depends(get_db)) -> DeviceOwnershipView:
    try:
        return ownership_service.get_ownership(db, node_id)
    except ownership_service.OwnershipTargetNotFound:
        raise HTTPException(status_code=404, detail="unknown node") from None


@router.put(
    "/api/v1/nodes/{node_id}/ownership",
    response_model=DeviceOwnershipView,
    dependencies=[Depends(require_policy("nodes.update_ownership", "nodes"))],
)
def put_node_ownership(
    node_id: str,
    body: DeviceOwnerBody,
    db: Session = Depends(get_db),
) -> DeviceOwnershipView:
    try:
        return ownership_service.set_owner(db, node_id, body.owner_type, body.owner_id)
    except ownership_service.OwnershipTargetNotFound:
        raise HTTPException(status_code=404, detail="unknown node or owner") from None
    except ownership_service.InactiveOwner:
        raise HTTPException(status_code=409, detail="disabled owners cannot own devices") from None


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
    "/api/v1/nodes/{node_id}/hardware-revision",
    response_model=NodeView,
    dependencies=[Depends(require_policy("nodes.update_hardware", "nodes"))],
)
def put_node_hardware_revision(
    node_id: str,
    body: HardwareRevisionBody,
    db: Session = Depends(get_db),
) -> NodeView:
    try:
        return service.set_hardware_revision(db, node_id, body.hardware_revision)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None


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


@router.post(
    "/api/v1/nodes/{node_id}/commands",
    response_model=OutputCommandView,
    dependencies=[Depends(require_policy("nodes.send_command", "nodes"))],
)
def post_node_command(
    node_id: str,
    body: OutputCommandBody,
    db: Session = Depends(get_db),
) -> OutputCommandView:
    try:
        return service.send_output_command(db, node_id, body)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None
    except service.OutputCommandUnavailable as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None


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


def _device_api_token(authorization: str | None) -> str:
    if authorization is None or not authorization.lower().startswith("bearer "):
        raise HTTPException(status_code=401, detail="device API token required")
    token = authorization[7:].strip()
    if not token:
        raise HTTPException(status_code=401, detail="device API token required")
    return token


@router.get("/api/v1/device/desired-firmware", response_model=DeviceDesiredFirmware)
def get_device_desired_firmware(
    response: Response,
    authorization: str | None = Header(default=None),
    db: Session = Depends(get_db),
) -> DeviceDesiredFirmware:
    """Device-scoped read. Identity comes from the device API token, not a path id or OPA role."""
    token = _device_api_token(authorization)
    try:
        node = service.node_for_device_api_token(db, token)
    except service.DeviceAuthError as exc:
        raise HTTPException(status_code=401, detail=str(exc)) from exc
    except service.DeviceForbidden as exc:
        raise HTTPException(status_code=403, detail=str(exc)) from exc
    response.headers["Cache-Control"] = "no-store"
    return service.device_desired_firmware(db, node)


@router.post(
    "/api/v1/nodes/{node_id}/device-api-token",
    response_model=DeviceApiTokenResponse,
    dependencies=[Depends(require_policy("nodes.device_api_token.rotate", "nodes"))],
)
def rotate_device_api_token(node_id: str, response: Response, db: Session = Depends(get_db)) -> DeviceApiTokenResponse:
    try:
        body = service.rotate_device_api_token(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None
    except service.DeviceCredentialUnavailable as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    response.headers["Cache-Control"] = "no-store"
    return body


@router.post(
    "/api/v1/nodes/{node_id}/device-api-token/revoke",
    status_code=204,
    dependencies=[Depends(require_policy("nodes.device_api_token.revoke", "nodes"))],
)
def revoke_device_api_token(node_id: str, db: Session = Depends(get_db)) -> Response:
    try:
        service.revoke_device_api_token(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None
    except service.DeviceCredentialUnavailable as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    return Response(status_code=204)


@router.post(
    "/api/v1/nodes/{node_id}/recovery-provision",
    response_model=BootstrapTokenResponse,
    dependencies=[Depends(require_policy("nodes.recovery_provision.open", "nodes"))],
)
def open_recovery_provision(node_id: str, response: Response, db: Session = Depends(get_db)) -> BootstrapTokenResponse:
    try:
        token = service.open_recovery_provision(db, node_id)
        node = service.get_node(db, node_id)
    except KeyError:
        raise HTTPException(status_code=404, detail="unknown node") from None
    except service.DeviceCredentialUnavailable as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from None
    response.headers["Cache-Control"] = "no-store"
    return BootstrapTokenResponse(
        node_id=node.node_id,
        bootstrap_token=token,
        provisioning_state=node.provisioning_state,
    )


@router.post("/api/v1/provision", response_model=ProvisionResponse)
def provision(body: ProvisionRequest, response: Response, db: Session = Depends(get_db)) -> ProvisionResponse:
    try:
        result = service.provision(db, body)
    except service.AlreadyProvisioned as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except MqttSecurityError as exc:
        raise HTTPException(status_code=503, detail="MQTT credential provisioning failed") from exc
    response.headers["Cache-Control"] = "no-store"
    return result
