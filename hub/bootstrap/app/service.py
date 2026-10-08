import json
from datetime import datetime, timezone

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.config import settings
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
    InstallationMetadata,
    Node,
    NodeCredential,
    Observation,
    ReportedState,
)
from app.mqtt_security import ensure_node_security, set_node_security_active
from app.schemas import (
    CreateNodeRequest,
    CreateNodeResponse,
    DesiredStateBody,
    InstallationMetadataBody,
    MqttEndpoint,
    NodeLifecycleBody,
    NodeView,
    ObservationBody,
    ObservationView,
    ProvisionRequest,
    ProvisionResponse,
    Topics,
)
from app.topics import topics_for


def _next_sequence(db: Session, node_type: str, simulated: bool = False) -> int:
    prefix = "SIM-EAR-%" if simulated else ("OUT-%" if node_type == "output" else "EAR-%")
    count = db.scalar(select(func.count()).select_from(Node).where(Node.node_id.like(prefix))) or 0
    return int(count) + 1


def create_node(db: Session, body: CreateNodeRequest) -> CreateNodeResponse:
    seq = _next_sequence(db, body.node_type, body.simulated)
    node_id = allocate_node_id(body.node_type, seq, body.simulated)
    while db.get(Node, node_id) is not None:
        seq += 1
        node_id = allocate_node_id(body.node_type, seq, body.simulated)

    node = Node(
        node_id=node_id,
        node_type=body.node_type,
        hardware_revision=body.hardware_revision,
        provisioning_state="pending",
        availability="SERVICE",
    )
    db.add(node)
    db.add(
        DesiredState(
            node_id=node_id,
            firmware_version=body.desired_firmware_version,
            config_version=body.desired_config_version,
        )
    )
    db.add(ReportedState(node_id=node_id, hardware_revision=body.hardware_revision))

    token = new_token()
    db.add(BootstrapToken(token_hash=token_hash(token), node_id=node_id))
    db.commit()
    return CreateNodeResponse(node_id=node_id, bootstrap_token=token, provisioning_state="pending")


def provision(db: Session, body: ProvisionRequest) -> ProvisionResponse:
    row = db.get(BootstrapToken, token_hash(body.bootstrap_token))
    if row is None or row.consumed_at is not None:
        raise ValueError("invalid or consumed bootstrap token")

    node = db.get(Node, row.node_id)
    if node is None:
        raise ValueError("node missing")
    if node.lifecycle_state != "active":
        raise ValueError("node is deactivated")

    password = new_token()
    username = node.node_id.lower()
    cred = db.get(NodeCredential, node.node_id)
    if cred is None:
        cred = NodeCredential(node_id=node.node_id, mqtt_username=username)
        db.add(cred)
    cred.mqtt_username = username
    cred.issued_at = datetime.now(timezone.utc)
    if settings.mqtt_enabled:
        ensure_node_security(node.node_type, node.node_id, password)

    node.provisioning_state = "provisioned"
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
        mqtt=MqttEndpoint(
            host=settings.advertised_mqtt_host,
            port=settings.advertised_mqtt_port,
            use_tls=settings.advertised_mqtt_use_tls,
            username=username,
            password=password,
        ),
        topics=Topics(**t),
    )


def list_nodes(db: Session) -> list[NodeView]:
    nodes = db.scalars(select(Node).order_by(Node.node_id)).all()
    return [node_view(db, n) for n in nodes]


def get_node(db: Session, node_id: str) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    return node_view(db, node)


def set_desired(db: Session, node_id: str, body: DesiredStateBody) -> NodeView:
    node = db.get(Node, node_id.upper())
    if node is None:
        raise KeyError(node_id)
    desired = db.get(DesiredState, node.node_id)
    if desired is None:
        desired = DesiredState(node_id=node.node_id)
        db.add(desired)
    desired.firmware_version = body.firmware_version
    desired.config_version = body.config_version
    desired.calibration_version = body.calibration_version
    desired.classifier_version = body.classifier_version
    desired.payload_json = json.dumps(body.payload)
    db.commit()
    return node_view(db, node)


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


def set_installation(db: Session, node_id: str, body: InstallationMetadataBody) -> NodeView:
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
        "config_version": int(payload.get("config_version", reported.config_version) or 0),
        "calibration_version": int(payload.get("calibration_version", reported.calibration_version) or 0),
        "classifier_version": payload.get("classifier_version", reported.classifier_version),
    }
    pending = payload.get("pending_configuration_change")
    if pending is None:
        pending = pending_configuration_change(merged, desired_dict)

    reported.firmware_version = str(merged["firmware_version"])
    reported.hardware_revision = str(payload.get("hardware_revision", reported.hardware_revision))
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

    db.commit()


def node_view(db: Session, node: Node) -> NodeView:
    desired = db.get(DesiredState, node.node_id)
    reported = db.get(ReportedState, node.node_id)
    installation = db.get(InstallationMetadata, node.node_id)
    desired_dict = {
        "firmware_version": desired.firmware_version if desired else "",
        "config_version": desired.config_version if desired else 0,
        "calibration_version": desired.calibration_version if desired else 0,
        "classifier_version": desired.classifier_version if desired else "",
        "payload": json.loads(desired.payload_json) if desired else {},
    }
    reported_dict = {
        "firmware_version": reported.firmware_version if reported else "",
        "hardware_revision": reported.hardware_revision if reported else "",
        "config_version": reported.config_version if reported else 0,
        "calibration_version": reported.calibration_version if reported else 0,
        "classifier_version": reported.classifier_version if reported else "",
        "uptime_s": reported.uptime_s if reported else 0,
        "pending_configuration_change": bool(reported.pending_configuration_change) if reported else False,
        "payload": json.loads(reported.payload_json) if reported and reported.payload_json else {},
    }
    pending = pending_configuration_change(reported_dict, desired_dict)
    return NodeView(
        node_id=node.node_id,
        node_type=node.node_type,
        hardware_revision=node.hardware_revision,
        provisioning_state=node.provisioning_state,
        lifecycle_state=node.lifecycle_state,
        availability=node.availability,
        pending_configuration_change=pending,
        desired=desired_dict,
        reported=reported_dict,
        last_seen_at=isoformat(reported.last_seen_at) if reported else None,
        installation=(
            InstallationMetadataBody(
                latitude=installation.latitude,
                longitude=installation.longitude,
                floor=installation.floor,
                height_m=installation.height_m,
                height_accuracy_m=installation.height_accuracy_m,
                mount_type=installation.mount_type,
                environment=installation.environment,
                orientation_deg=installation.orientation_deg,
            )
            if installation
            else None
        ),
    )
