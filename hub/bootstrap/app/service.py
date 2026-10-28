import json
import logging
from datetime import datetime, timedelta, timezone
from pathlib import Path

from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app import device_diag, device_monitor, firmware_catalog
from app.config import settings

log = logging.getLogger("hub-bootstrap.service")
from app.ids import (
    allocate_node_id,
    isoformat,
    new_token,
    pending_configuration_change,
    token_hash,
)
from app.models import (
    BootstrapToken,
    DesiredState,
    DeviceOwnership,
    InstallationMetadata,
    Node,
    NodeCredential,
    Observation,
    RegistrationRequest,
    ReportedState,
)
from app.mqtt_security import ensure_node_security, set_node_security_active
from app.ownership_service import add_initial_owner, get_current_owner
from app.schemas import (
    CreateNodeRequest,
    CreateNodeResponse,
    DesiredStateBody,
    DeviceApiCredential,
    DeviceApiTokenResponse,
    DeviceDesiredFirmware,
    InstallationMetadataBody,
    MqttEndpoint,
    NodeLifecycleBody,
    NodeView,
    ObservationBody,
    ObservationView,
    OutputCommandBody,
    OutputCommandView,
    ProvisionRequest,
    ProvisionResponse,
    Topics,
)
from app.topics import topics_for


class DuplicateRegistration(Exception):
    def __init__(self, node_id: str) -> None:
        self.node_id = node_id


class DuplicateChipMac(Exception):
    def __init__(self, node_id: str, chip_mac: str) -> None:
        self.node_id = node_id
        self.chip_mac = chip_mac


class OutputCommandUnavailable(ValueError):
    """Raised when an Output command cannot be accepted or published."""


class DesiredFirmwareInconsistent(ValueError):
    """Raised when Desired firmware_version does not match the catalog entry for firmware_id."""


class BootstrapTokenRotationUnavailable(Exception):
    pass


class AlreadyProvisioned(Exception):
    pass


class DeviceCredentialUnavailable(Exception):
    pass


class DeviceAuthError(Exception):
    pass


class DeviceForbidden(Exception):
    pass


def _node_id_is_taken(db: Session, node_id: str) -> bool:
    """True when a node id is already used by a node or leftover related rows."""
    if db.get(Node, node_id) is not None:
        return True
    if (
        db.scalar(
            select(RegistrationRequest.node_id).where(
                RegistrationRequest.node_id == node_id
            )
        )
        is not None
    ):
        return True
    if (
        db.scalar(
            select(DeviceOwnership.assignment_id)
            .where(DeviceOwnership.node_id == node_id)
            .limit(1)
        )
        is not None
    ):
        return True
    if (
        db.scalar(
            select(BootstrapToken.node_id)
            .where(BootstrapToken.node_id == node_id)
            .limit(1)
        )
        is not None
    ):
        return True
    if db.get(DesiredState, node_id) is not None:
        return True
    if db.get(ReportedState, node_id) is not None:
        return True
    return False


def _allocate_available_node_id(
    db: Session, node_type: str, simulated: bool = False
) -> str:
    """Pick the next free OUT-/EAR- id, skipping gaps and orphan related rows."""
    seq = 1
    while seq < 10000:
        node_id = allocate_node_id(node_type, seq, simulated)
        if not _node_id_is_taken(db, node_id):
            return node_id
        seq += 1
    raise RuntimeError("could not allocate a free node id")


def create_node(
    db: Session,
    body: CreateNodeRequest,
    idempotency_key: str | None = None,
) -> CreateNodeResponse:
    if idempotency_key is not None:
        previous = db.get(RegistrationRequest, idempotency_key)
        if previous is not None:
            raise DuplicateRegistration(previous.node_id)

    if body.chip_mac is not None:
        existing_mac = db.scalars(
            select(Node).where(Node.chip_mac == body.chip_mac)
        ).first()
        if existing_mac is not None:
            if not body.replace_existing_chip_mac:
                raise DuplicateChipMac(existing_mac.node_id, body.chip_mac)
            existing_mac.chip_mac = None
            if body.chip_variant:
                # Keep historical variant text on the prior node; new node gets the current reading.
                pass

    node_id = _allocate_available_node_id(db, body.node_type, body.simulated)

    node = Node(
        node_id=node_id,
        node_type=body.node_type,
        hardware_revision=body.hardware_revision,
        chip_mac=body.chip_mac,
        chip_variant=body.chip_variant,
        provisioning_state="pending",
        availability="SERVICE",
    )
    add_initial_owner(db, node_id, body.owner_type, body.owner_id)
    db.add(node)
    db.add(
        DesiredState(
            node_id=node_id,
            firmware_id=body.firmware_id or "",
            firmware_version=body.desired_firmware_version,
            config_version=body.desired_config_version,
        )
    )
    db.add(ReportedState(node_id=node_id, hardware_revision=body.hardware_revision))

    token = new_token()
    db.add(BootstrapToken(token_hash=token_hash(token), node_id=node_id))
    if idempotency_key is not None:
        db.add(RegistrationRequest(idempotency_key=idempotency_key, node_id=node_id))
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        if idempotency_key is not None:
            previous = db.get(RegistrationRequest, idempotency_key)
            if previous is not None:
                raise DuplicateRegistration(previous.node_id) from exc
        if body.chip_mac is not None:
            conflict = db.scalars(
                select(Node).where(Node.chip_mac == body.chip_mac)
            ).first()
            if conflict is not None:
                raise DuplicateChipMac(conflict.node_id, body.chip_mac) from exc
        raise
    return CreateNodeResponse(
        node_id=node_id, bootstrap_token=token, provisioning_state="pending"
    )


def rotate_pending_bootstrap_token(db: Session, node_id: str) -> str:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    if node.provisioning_state != "pending":
        raise BootstrapTokenRotationUnavailable(
            "bootstrap tokens can only be replaced while the device is pending"
        )

    now = datetime.now(timezone.utc)
    active_tokens = db.scalars(
        select(BootstrapToken)
        .where(
            BootstrapToken.node_id == node.node_id, BootstrapToken.consumed_at.is_(None)
        )
        .with_for_update()
    ).all()
    for row in active_tokens:
        row.consumed_at = now
    db.flush()
    token = new_token()
    db.add(BootstrapToken(token_hash=token_hash(token), node_id=node.node_id))
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        raise BootstrapTokenRotationUnavailable(
            "a bootstrap token was just issued; reload the device and retry"
        ) from exc
    return token


def provision(db: Session, body: ProvisionRequest) -> ProvisionResponse:
    if body.mqtt_profile == "external" and not settings.public_hub_url.strip():
        raise ValueError("external call-home route is not configured")
    row = db.get(BootstrapToken, token_hash(body.bootstrap_token))
    if row is None or row.consumed_at is not None:
        raise ValueError("invalid or consumed bootstrap token")

    node = db.get(Node, row.node_id)
    if node is None:
        raise ValueError("node missing")
    if node.lifecycle_state != "active":
        raise ValueError("node is deactivated")

    initial = node.provisioning_state == "pending"
    recovery = node.recovery_opened_at is not None and node.provisioning_state in {
        "provisioned",
        "active",
    }
    if not initial and not recovery:
        raise AlreadyProvisioned("device is already provisioned")

    password = new_token()
    username = node.node_id.lower()
    cred = db.get(NodeCredential, node.node_id)
    if cred is None:
        cred = NodeCredential(node_id=node.node_id, mqtt_username=username)
        db.add(cred)
    cred.mqtt_username = username
    cred.issued_at = datetime.now(timezone.utc)
    device_api_token = _store_device_api_token(cred)
    if settings.mqtt_enabled:
        ensure_node_security(node.node_type, node.node_id, password)

    if initial:
        node.provisioning_state = "provisioned"
    node.recovery_opened_at = None
    node.hardware_revision = body.hardware_revision
    node.availability = "RECOVERING"

    reported = db.get(ReportedState, node.node_id)
    if reported is None:
        reported = ReportedState(node_id=node.node_id)
        db.add(reported)
    reported.hardware_revision = body.hardware_revision
    reported.firmware_version = body.firmware_version

    row.consumed_at = datetime.now(timezone.utc)
    db.commit()

    t = topics_for(node.node_type, node.node_id)
    return ProvisionResponse(
        node_id=node.node_id,
        node_type=node.node_type,
        hub_id=settings.hub_id,
        mqtt=MqttEndpoint(
            host=(
                settings.local_mqtt_host
                if body.mqtt_profile == "local"
                else settings.public_mqtt_host
            ),
            port=(
                settings.local_mqtt_port
                if body.mqtt_profile == "local"
                else settings.public_mqtt_port
            ),
            use_tls=(
                settings.public_mqtt_use_tls if body.mqtt_profile is None else True
            ),
            username=username,
            password=password,
        ),
        topics=Topics(**t),
        device_api=DeviceApiCredential(token=device_api_token),
    )


def _store_device_api_token(cred: NodeCredential) -> str:
    """Replace the device HTTPS credential. Returns plaintext once; only the hash is stored."""
    token = new_token()
    cred.device_api_token_hash = token_hash(token)
    cred.device_api_issued_at = datetime.now(timezone.utc)
    cred.device_api_revoked_at = None
    return token


def _require_issued_device(db: Session, node_id: str) -> tuple[Node, NodeCredential]:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    if node.provisioning_state == "pending":
        raise DeviceCredentialUnavailable("device has not completed provisioning")
    cred = db.get(NodeCredential, node.node_id)
    if cred is None:
        raise DeviceCredentialUnavailable("device has no credential record")
    return node, cred


def rotate_device_api_token(db: Session, node_id: str) -> DeviceApiTokenResponse:
    node, cred = _require_issued_device(db, node_id)
    token = _store_device_api_token(cred)
    db.commit()
    return DeviceApiTokenResponse(node_id=node.node_id, device_api_token=token)


def revoke_device_api_token(db: Session, node_id: str) -> None:
    _node, cred = _require_issued_device(db, node_id)
    if cred.device_api_token_hash is None:
        raise DeviceCredentialUnavailable("device API token is already revoked")
    cred.device_api_token_hash = None
    cred.device_api_revoked_at = datetime.now(timezone.utc)
    db.commit()


def open_recovery_provision(db: Session, node_id: str) -> str:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    if node.lifecycle_state != "active":
        raise DeviceCredentialUnavailable("node is deactivated")
    if node.provisioning_state not in {"provisioned", "active"}:
        raise DeviceCredentialUnavailable(
            "recovery provisioning is only for an already provisioned device"
        )

    now = datetime.now(timezone.utc)
    active_tokens = db.scalars(
        select(BootstrapToken)
        .where(
            BootstrapToken.node_id == node.node_id, BootstrapToken.consumed_at.is_(None)
        )
        .with_for_update()
    ).all()
    for row in active_tokens:
        row.consumed_at = now
    db.flush()
    token = new_token()
    db.add(BootstrapToken(token_hash=token_hash(token), node_id=node.node_id))
    node.recovery_opened_at = now
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        raise DeviceCredentialUnavailable(
            "a recovery bootstrap token was just issued; retry"
        ) from exc
    return token


def node_for_device_api_token(db: Session, token: str) -> Node:
    if not token:
        raise DeviceAuthError("device API token required")
    cred = db.scalar(
        select(NodeCredential).where(
            NodeCredential.device_api_token_hash == token_hash(token)
        )
    )
    if cred is None:
        raise DeviceAuthError("device API token required")
    node = db.get(Node, cred.node_id)
    if node is None:
        raise DeviceAuthError("device API token required")
    if node.lifecycle_state != "active":
        raise DeviceForbidden("node is deactivated")
    return node


def catalog_entry_firmware_version(entry: object) -> str:
    metadata = json.loads(entry.metadata_json)
    return str(metadata.get("firmware_version") or "")


def device_desired_firmware(db: Session, node: Node) -> DeviceDesiredFirmware:
    desired = db.get(DesiredState, node.node_id)
    firmware_id = desired.firmware_id if desired else ""
    firmware_version = desired.firmware_version if desired else ""
    app_sha256 = ""
    app_size_bytes = 0
    if firmware_id:
        catalog_dir = Path(settings.firmware_catalog_dir)
        try:
            # Candidates are allowed when they are already the Desired id (admin intent).
            entry = firmware_catalog.get_entry(
                db,
                catalog_dir,
                firmware_id,
                include_candidate=True,
            )
            catalog_version = catalog_entry_firmware_version(entry)
            # Version-only Desired edits leave a stale firmware_id; withhold the image so the
            # device cannot OTA-loop the same binary under a new Desired version string.
            if (
                firmware_version
                and catalog_version
                and catalog_version != firmware_version
            ):
                pass
            else:
                image, digest = firmware_catalog.get_flash_image(
                    entry,
                    catalog_dir,
                    "esp-output.bin",
                )
                app_sha256 = digest
                app_size_bytes = len(image)
        except (
            firmware_catalog.FirmwareNotFound,
            firmware_catalog.FirmwareArtifactIntegrityError,
        ):
            pass
    return DeviceDesiredFirmware(
        node_id=node.node_id,
        firmware_id=firmware_id or "",
        firmware_version=firmware_version or "",
        app_sha256=app_sha256,
        app_size_bytes=app_size_bytes,
    )


def list_nodes(db: Session) -> list[NodeView]:
    nodes = db.scalars(select(Node).order_by(Node.node_id)).all()
    return [node_view(db, n) for n in nodes]


def get_node(db: Session, node_id: str) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    return node_view(db, node, include_monitoring=True)


def get_node_by_chip_mac(db: Session, chip_mac: str) -> NodeView:
    from app.schemas import normalize_chip_mac

    normalized = normalize_chip_mac(chip_mac)
    if normalized is None:
        raise KeyError(chip_mac)
    node = db.scalars(select(Node).where(Node.chip_mac == normalized)).first()
    if node is None:
        raise KeyError(chip_mac)
    return node_view(db, node, include_monitoring=True)


def set_desired(db: Session, node_id: str, body: DesiredStateBody) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    desired = db.get(DesiredState, node.node_id)
    if desired is None:
        desired = DesiredState(node_id=node.node_id)
        db.add(desired)
    if desired.firmware_id:
        catalog_dir = Path(settings.firmware_catalog_dir)
        try:
            entry = firmware_catalog.get_entry(
                db,
                catalog_dir,
                desired.firmware_id,
                include_candidate=True,
            )
            catalog_version = catalog_entry_firmware_version(entry)
            if catalog_version and body.firmware_version != catalog_version:
                raise DesiredFirmwareInconsistent(
                    "desired firmware_version "
                    f"{body.firmware_version!r} does not match catalog entry "
                    f"{desired.firmware_id!r} version {catalog_version!r}; "
                    "use POST /api/v1/nodes/{node_id}/firmware-selection to change Desired firmware"
                )
        except firmware_catalog.FirmwareNotFound:
            pass
    desired.firmware_version = body.firmware_version
    desired.config_version = body.config_version
    desired.calibration_version = body.calibration_version
    desired.classifier_version = body.classifier_version
    desired.payload_json = json.dumps(body.payload)
    db.commit()
    return node_view(db, node)


def set_desired_firmware(
    db: Session, node_id: str, firmware_id: str, firmware_version: str
) -> None:
    """Record the firmware an admin selected for this node as Desired state.

    Does not commit; callers that already validate a catalog entry decide when to commit.
    """
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    desired = db.get(DesiredState, node.node_id)
    if desired is None:
        desired = DesiredState(node_id=node.node_id)
        db.add(desired)
    desired.firmware_id = firmware_id
    desired.firmware_version = firmware_version


def set_node_lifecycle(
    db: Session,
    node_id: str,
    body: NodeLifecycleBody,
) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)

    if body.lifecycle_state == "deactivated" and settings.mqtt_enabled:
        set_node_security_active(node.node_type, node.node_id, active=False)

    node.lifecycle_state = body.lifecycle_state
    db.commit()

    if body.lifecycle_state == "active" and settings.mqtt_enabled:
        set_node_security_active(node.node_type, node.node_id, active=True)

    return node_view(db, node)


def set_hardware_revision(
    db: Session,
    node_id: str,
    hardware_revision: str,
) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    node.hardware_revision = hardware_revision
    db.commit()
    return node_view(db, node)


def set_installation(
    db: Session, node_id: str, body: InstallationMetadataBody
) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    installation = db.get(InstallationMetadata, node.node_id)
    if installation is None:
        installation = InstallationMetadata(node_id=node.node_id)
        db.add(installation)
    for field, value in body.model_dump().items():
        setattr(installation, field, value)
    db.commit()
    return node_view(db, node)


def ingest_observation(db: Session, topic_node_id: str, body: ObservationBody) -> bool:
    node_id = topic_node_id.upper()
    if body.node_id.upper() != node_id:
        raise ValueError("observation node_id does not match topic")

    node = db.get(Node, node_id)
    if node is None or node.node_type != "ear":
        raise ValueError("observation topic does not belong to a registered Ear")
    if node.lifecycle_state != "active":
        raise ValueError("node is deactivated")
    body.node_id = node_id

    observation_id = str(body.observation_id)
    existing = db.get(Observation, observation_id)
    if existing is not None:
        if existing.node_id != node_id:
            raise ValueError("observation ID is already registered to another node")
        return False

    db.add(
        Observation(
            observation_id=observation_id,
            node_id=node_id,
            sequence_number=body.sequence_number,
            event_time_utc=body.event_time_utc,
            payload_json=body.model_dump_json(),
        )
    )
    db.commit()
    return True


def list_observations(
    db: Session,
    node_id: str | None,
    limit: int,
) -> list[ObservationView]:
    statement = select(Observation).order_by(Observation.received_time_utc.desc())
    if node_id is not None:
        statement = statement.where(Observation.node_id == node_id.upper())
    rows = db.scalars(statement.limit(limit)).all()
    observations = []
    for row in rows:
        payload = json.loads(row.payload_json)
        received_at = row.received_time_utc
        if received_at.tzinfo is None:
            received_at = received_at.replace(tzinfo=timezone.utc)
        observations.append(ObservationView(**payload, received_time_utc=received_at))
    return observations


def apply_reported(db: Session, node_id: str, payload: dict) -> None:
    node_id = node_id.upper()
    node = db.get(Node, node_id)
    if node is None:
        return

    reported = db.get(ReportedState, node_id)
    if reported is None:
        reported = ReportedState(node_id=node_id)
        db.add(reported)

    desired = db.get(DesiredState, node_id)
    desired_dict = {
        "firmware_version": desired.firmware_version if desired else "",
        "config_version": desired.config_version if desired else 0,
        "calibration_version": desired.calibration_version if desired else 0,
        "classifier_version": desired.classifier_version if desired else "",
    }
    merged = {
        "firmware_version": payload.get("firmware_version", reported.firmware_version),
        "config_version": int(
            payload.get("config_version", reported.config_version) or 0
        ),
        "calibration_version": int(
            payload.get("calibration_version", reported.calibration_version) or 0
        ),
        "classifier_version": payload.get(
            "classifier_version", reported.classifier_version
        ),
    }
    pending = pending_configuration_change(merged, desired_dict)

    previous_payload_json = reported.payload_json
    previous_firmware_version = reported.firmware_version
    reported.firmware_version = str(merged["firmware_version"])
    reported.hardware_revision = str(
        payload.get("hardware_revision", reported.hardware_revision)
    )
    reported.config_version = int(merged["config_version"])
    reported.calibration_version = int(merged["calibration_version"])
    reported.classifier_version = str(merged["classifier_version"])
    reported.uptime_s = int(payload.get("uptime_s", reported.uptime_s) or 0)
    reported.pending_configuration_change = 1 if pending else 0
    reported.payload_json = json.dumps(payload)
    reported.last_seen_at = datetime.now(timezone.utc)

    if node.provisioning_state == "provisioned":
        node.availability = "OPERATIONAL"
        node.provisioning_state = "active"

    try:
        device_diag.ingest_ota_report(
            db,
            node_id=node_id,
            payload=payload,
            previous_firmware_version=previous_firmware_version,
        )
    except Exception:
        log.exception("device OTA report ingest failed for %s", node_id)

    try:
        device_diag.ingest_reported_diag(
            db,
            node_id=node_id,
            previous_payload_json=previous_payload_json,
            new_payload=payload,
            online=True,
        )
    except Exception:
        log.exception("device diag ingest failed for %s", node_id)

    db.commit()


def node_view(
    db: Session, node: Node, *, include_monitoring: bool = False
) -> NodeView:
    desired = db.get(DesiredState, node.node_id)
    reported = db.get(ReportedState, node.node_id)
    installation = db.get(InstallationMetadata, node.node_id)
    owner = get_current_owner(db, node.node_id)
    desired_dict = {
        "firmware_id": desired.firmware_id if desired else "",
        "firmware_version": desired.firmware_version if desired else "",
        "config_version": desired.config_version if desired else 0,
        "calibration_version": desired.calibration_version if desired else 0,
        "classifier_version": desired.classifier_version if desired else "",
        "payload": json.loads(desired.payload_json) if desired else {},
    }
    payload = (
        json.loads(reported.payload_json) if reported and reported.payload_json else {}
    )
    if isinstance(payload, dict):
        # Hub-only flag must never appear as device-reported telemetry.
        payload.pop("pending_configuration_change", None)
    else:
        payload = {}
    reported_dict = {
        "firmware_version": reported.firmware_version if reported else "",
        "hardware_revision": reported.hardware_revision if reported else "",
        "config_version": reported.config_version if reported else 0,
        "calibration_version": reported.calibration_version if reported else 0,
        "classifier_version": reported.classifier_version if reported else "",
        "uptime_s": reported.uptime_s if reported else 0,
        "payload": payload,
    }
    # Hub-computed only — exposed on NodeView, not inside reported.
    pending = pending_configuration_change(reported_dict, desired_dict)
    installation_view = None
    if installation is not None:
        try:
            installation_view = InstallationMetadataBody(
                latitude=installation.latitude,
                longitude=installation.longitude,
                floor=installation.floor,
                height_m=installation.height_m,
                height_accuracy_m=installation.height_accuracy_m,
                mount_type=installation.mount_type,
                environment=installation.environment,
                orientation_deg=installation.orientation_deg,
            )
        except ValueError:
            # Invalid legacy rows are treated like incomplete installations.
            installation_view = None
    last_seen_at = reported.last_seen_at if reported else None
    if last_seen_at is not None and last_seen_at.tzinfo is None:
        last_seen_at = last_seen_at.replace(tzinfo=timezone.utc)
    online = device_monitor.is_online(last_seen_at)
    monitoring = None
    if include_monitoring:
        try:
            monitoring = device_monitor.build_monitoring_summary(db, node.node_id)
        except Exception:  # noqa: BLE001 - detail must still return without monitoring
            monitoring = {
                "enabled": settings.device_monitor_enabled,
                "phase": "D0",
                "error": "monitoring summary unavailable",
            }
    return NodeView(
        node_id=node.node_id,
        node_type=node.node_type,
        hardware_revision=node.hardware_revision,
        chip_mac=node.chip_mac,
        chip_variant=node.chip_variant,
        provisioning_state=node.provisioning_state,
        lifecycle_state=node.lifecycle_state,
        availability=node.availability,
        status="online" if online else "offline",
        pending_configuration_change=pending,
        desired=desired_dict,
        reported=reported_dict,
        last_seen_at=isoformat(reported.last_seen_at) if reported else None,
        installation=installation_view,
        owner_type=owner[0] if owner else None,
        owner_id=owner[1] if owner else None,
        owner_display_name=owner[2] if owner else None,
        monitoring=monitoring,
    )


def send_output_command(
    db: Session, node_id: str, body: OutputCommandBody
) -> OutputCommandView:
    from app.mqtt_listener import MqttPublishError, publish_json

    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    if node.node_type != "output":
        raise OutputCommandUnavailable("commands are only supported for Output nodes")
    if node.lifecycle_state != "active":
        raise OutputCommandUnavailable("deactivated nodes cannot receive commands")
    if node.provisioning_state not in {"provisioned", "active"}:
        raise OutputCommandUnavailable("the node has not completed Hub provisioning")

    now = datetime.now(timezone.utc)
    if body.expires_at_utc is None:
        if body.duration_s is not None:
            lifetime_s = body.duration_s + 30
        elif body.action.startswith("AUDIO_"):
            lifetime_s = 120
        else:
            lifetime_s = 60
        expires_at = now + timedelta(seconds=lifetime_s)
    else:
        expires_at = body.expires_at_utc
    if expires_at <= now:
        raise OutputCommandUnavailable("expires_at_utc is already in the past")

    payload: dict = {
        "protocol_version": body.protocol_version,
        "action": body.action,
        "expires_at_utc": expires_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    if body.duration_s is not None:
        payload["duration_s"] = body.duration_s
    if body.url is not None:
        payload["url"] = body.url
    if body.path is not None:
        payload["path"] = body.path
    if body.message_type is not None:
        payload["message_type"] = body.message_type

    topic = topics_for(node.node_type, node.node_id)["command"]
    try:
        publish_json(topic, payload, qos=1)
    except MqttPublishError as exc:
        raise OutputCommandUnavailable(str(exc)) from exc
    return OutputCommandView(
        node_id=node.node_id,
        topic=topic,
        action=body.action,
        duration_s=body.duration_s,
        url=body.url,
        path=body.path,
        protocol_version=body.protocol_version,
        message_type=body.message_type,
        action_type=body.message_type,
        expires_at_utc=expires_at,
        published=True,
    )
