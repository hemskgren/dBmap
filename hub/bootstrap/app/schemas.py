from datetime import datetime, timezone
from typing import Any, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


class CreateNodeRequest(BaseModel):
    node_type: Literal["ear", "output"] = "output"
    hardware_revision: str = "OUTPUT-DEV-V1"
    desired_firmware_version: str = ""
    desired_config_version: int = 0
    simulated: bool = False

    @model_validator(mode="after")
    def simulated_nodes_must_be_ears(self) -> "CreateNodeRequest":
        if self.simulated and self.node_type != "ear":
            raise ValueError("only Ear nodes can be simulated")
        return self


class CreateNodeResponse(BaseModel):
    node_id: str
    bootstrap_token: str
    provisioning_state: str


class ProvisionRequest(BaseModel):
    bootstrap_token: str
    hardware_revision: str = "unknown"
    firmware_version: str = "0.0.0"


class MqttEndpoint(BaseModel):
    host: str
    port: int
    use_tls: bool
    username: str
    password: str
    keepalive_s: int = 30


class Topics(BaseModel):
    hello: str
    keepalive: str
    health: str
    status: str
    command: str
    ack: str
    config: str
    observation: str


class ProvisionResponse(BaseModel):
    node_id: str
    node_type: str
    mqtt: MqttEndpoint
    topics: Topics


class DesiredStateBody(BaseModel):
    firmware_version: str = ""
    config_version: int = 0
    calibration_version: int = 0
    classifier_version: str = ""
    payload: dict[str, Any] = Field(default_factory=dict)


class NodeLifecycleBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    lifecycle_state: Literal["active", "deactivated"]


class InstallationMetadataBody(BaseModel):
    latitude: float = Field(ge=-90, le=90, description="WGS84 latitude in decimal degrees")
    longitude: float = Field(ge=-180, le=180, description="WGS84 longitude in decimal degrees")
    floor: int | None = None
    height_m: float | None = Field(default=None, ge=0)
    height_accuracy_m: float | None = Field(default=None, ge=0)
    mount_type: str | None = Field(default=None, max_length=64)
    environment: str | None = Field(default=None, max_length=64)
    orientation_deg: float | None = Field(
        default=None,
        ge=0,
        lt=360,
        description="Clockwise from geographic true north to the node reference axis",
    )


class ObservationClassification(BaseModel):
    model_config = ConfigDict(extra="forbid")

    source_family: str | None = Field(default=None, min_length=1, max_length=64)
    confidence: float | None = Field(default=None, ge=0, le=1, allow_inf_nan=False)
    hints: dict[str, Any] = Field(default_factory=dict)


class ObservationBearing(BaseModel):
    model_config = ConfigDict(extra="forbid")

    deg: float = Field(ge=0, lt=360, allow_inf_nan=False)
    reference: Literal["node"] = "node"
    confidence: float | None = Field(default=None, ge=0, le=1, allow_inf_nan=False)


class ObservationBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    protocol_version: Literal[1]
    message_type: Literal["observation"]
    observation_id: UUID
    node_id: str = Field(min_length=1, max_length=64)
    sequence_number: int = Field(ge=0)
    event_time_utc: datetime
    capture_timestamp_monotonic_us: int = Field(ge=0)
    timing_quality: Literal["synchronized", "estimated", "unsynchronized"]
    clock_offset_ms: float | None = Field(default=None, allow_inf_nan=False)
    timestamp_uncertainty_ms: float = Field(ge=0, allow_inf_nan=False)
    classification: ObservationClassification | None = None
    bearing: ObservationBearing | None = None
    signal_level_dbfs: float | None = Field(default=None, le=0, allow_inf_nan=False)
    duration_ms: float = Field(ge=0, allow_inf_nan=False)

    @field_validator("event_time_utc")
    @classmethod
    def require_timezone_and_normalize_utc(cls, value: datetime) -> datetime:
        if value.tzinfo is None or value.utcoffset() is None:
            raise ValueError("event_time_utc must include a timezone")
        return value.astimezone(timezone.utc)


class ObservationView(ObservationBody):
    received_time_utc: datetime


class NodeView(BaseModel):
    node_id: str
    node_type: str
    hardware_revision: str
    provisioning_state: str
    lifecycle_state: Literal["active", "deactivated"]
    availability: str
    status: Literal["online", "offline"]
    pending_configuration_change: bool
    desired: dict[str, Any]
    reported: dict[str, Any]
    last_seen_at: str | None
    installation: InstallationMetadataBody | None = None


class DeviceSummaryView(BaseModel):
    node_id: str
    node_type: str
    hardware_revision: str
    provisioning_state: str
    lifecycle_state: Literal["active", "deactivated"]
    status: Literal["online", "offline"]
    last_seen_at: str | None
    installation: InstallationMetadataBody | None = None
