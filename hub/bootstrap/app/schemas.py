from datetime import datetime, timezone
from typing import Any, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


def normalize_chip_mac(value: str | None) -> str | None:
    if value is None:
        return None
    hex_digits = "".join(character for character in value if character.isalnum())
    if len(hex_digits) != 12 or any(
        character not in "0123456789abcdefABCDEF" for character in hex_digits
    ):
        raise ValueError("chip_mac must be a 12-digit hexadecimal MAC address")
    pairs = [hex_digits[index : index + 2].upper() for index in range(0, 12, 2)]
    return ":".join(pairs)


class CreateNodeRequest(BaseModel):
    node_type: Literal["ear", "output"] = "output"
    hardware_revision: str = "unknown"
    chip_mac: str | None = Field(default=None, max_length=32)
    chip_variant: str | None = Field(default=None, max_length=128)
    firmware_id: str | None = Field(
        default=None,
        min_length=1,
        max_length=64,
        pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]*$",
    )
    desired_firmware_version: str = ""
    desired_config_version: int = 0
    simulated: bool = False
    owner_type: Literal["user", "group"] | None = None
    owner_id: str | None = None
    replace_existing_chip_mac: bool = False

    @field_validator("chip_mac")
    @classmethod
    def validate_chip_mac(cls, value: str | None) -> str | None:
        return normalize_chip_mac(value)

    @field_validator("chip_variant")
    @classmethod
    def normalize_chip_variant(cls, value: str | None) -> str | None:
        if value is None:
            return None
        normalized = value.strip()
        if not normalized:
            return None
        if len(normalized) > 128:
            raise ValueError("chip_variant must be at most 128 characters")
        return normalized

    @model_validator(mode="after")
    def validate_creation_options(self) -> "CreateNodeRequest":
        if self.simulated and self.node_type != "ear":
            raise ValueError("only Ear nodes can be simulated")
        if (self.owner_type is None) != (self.owner_id is None):
            raise ValueError("owner_type and owner_id must be provided together")
        return self


class HardwareRevisionBody(BaseModel):
    hardware_revision: str = Field(min_length=1, max_length=128)

    @field_validator("hardware_revision")
    @classmethod
    def normalize_hardware_revision(cls, value: str) -> str:
        normalized = value.strip()
        if not normalized:
            raise ValueError("hardware_revision must not be blank")
        return normalized


class CreateNodeResponse(BaseModel):
    node_id: str
    bootstrap_token: str
    provisioning_state: str


class BootstrapTokenResponse(BaseModel):
    node_id: str
    bootstrap_token: str
    provisioning_state: str


class FirmwareSelectionBody(BaseModel):
    firmware_id: str = Field(
        min_length=1, max_length=64, pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]*$"
    )
    acknowledge_unverified_platform: bool = False
    allow_candidate: bool = False


class DeploymentAttemptCreateBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    firmware_id: str = Field(
        min_length=1, max_length=64, pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]*$"
    )
    acknowledge_unverified_platform: bool = False
    allow_candidate: bool = False
    flasher_version: str | None = Field(default=None, min_length=1, max_length=64)


class DeploymentAttemptUpdateBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    status: Literal["in_progress", "succeeded", "failed", "unknown"]
    stage: Literal[
        "connecting",
        "erasing",
        "writing",
        "verifying",
        "completed",
        "failed",
        "unknown",
    ]
    progress_percent: int | None = Field(default=None, ge=0, le=100)
    failure_code: (
        Literal[
            "device_not_connected",
            "serial_connection_lost",
            "flash_failed",
            "cancelled",
            "browser_interrupted",
            "integrity_failed",
            "other",
        ]
        | None
    ) = None

    @model_validator(mode="after")
    def validate_attempt_state(self) -> "DeploymentAttemptUpdateBody":
        terminal_stages = {"completed", "failed", "unknown"}
        if self.status == "in_progress":
            if (
                self.stage in terminal_stages
                or self.progress_percent is None
                or self.failure_code
            ):
                raise ValueError(
                    "in-progress updates require a non-terminal stage and progress percentage"
                )
            if self.progress_percent >= 100:
                raise ValueError(
                    "in-progress deployment attempts cannot be 100 percent complete"
                )
        elif self.status == "succeeded":
            if (
                self.stage != "completed"
                or self.progress_percent != 100
                or self.failure_code
            ):
                raise ValueError(
                    "successful attempts require completed stage and 100 percent progress"
                )
        elif self.status == "failed":
            if self.stage != "failed" or self.failure_code is None:
                raise ValueError(
                    "failed attempts require failed stage and a failure code"
                )
            if self.progress_percent == 100:
                raise ValueError("failed attempts cannot report 100 percent completion")
        elif self.status == "unknown":
            if self.stage != "unknown" or self.failure_code is None:
                raise ValueError(
                    "unknown attempts require unknown stage and a failure code"
                )
            if self.progress_percent == 100:
                raise ValueError(
                    "unknown attempts cannot report 100 percent completion"
                )
        return self


class ProvisionRequest(BaseModel):
    bootstrap_token: str
    hardware_revision: str = "unknown"
    firmware_version: str = "0.0.0"
    mqtt_profile: Literal["local", "external"] | None = None


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


class DeviceApiCredential(BaseModel):
    token: str


class ProvisionResponse(BaseModel):
    node_id: str
    node_type: str
    mqtt: MqttEndpoint
    topics: Topics
    device_api: DeviceApiCredential


class DeviceDesiredFirmware(BaseModel):
    node_id: str
    firmware_id: str
    firmware_version: str
    app_sha256: str = ""
    app_size_bytes: int = 0


class DeviceApiTokenResponse(BaseModel):
    node_id: str
    device_api_token: str


class DesiredStateBody(BaseModel):
    firmware_version: str = ""
    config_version: int = 0
    calibration_version: int = 0
    classifier_version: str = ""
    payload: dict[str, Any] = Field(default_factory=dict)


class NodeLifecycleBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    lifecycle_state: Literal["active", "deactivated"]


class OutputCommandBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    protocol_version: Literal[1] = 1
    message_type: Literal["output-test"] | None = None
    action: Literal[
        "RELAY_1_ON",
        "RELAY_1_OFF",
        "AUDIO_PLAY_HTTPS",
        "AUDIO_PLAY_FILE",
        "AUDIO_STOP",
    ]
    duration_s: int | None = Field(default=None, ge=1, le=3600)
    url: str | None = Field(default=None, min_length=12, max_length=384)
    path: str | None = Field(default=None, min_length=1, max_length=192)
    action_type: Literal["output-test"] | None = None
    expires_at_utc: datetime | None = None

    @field_validator("expires_at_utc")
    @classmethod
    def require_timezone(cls, value: datetime | None) -> datetime | None:
        if value is None:
            return None
        if value.tzinfo is None or value.utcoffset() is None:
            raise ValueError("expires_at_utc must include a timezone")
        return value.astimezone(timezone.utc)

    @field_validator("url")
    @classmethod
    def require_https_url(cls, value: str | None) -> str | None:
        if value is None:
            return None
        normalized = value.strip()
        if not normalized.startswith("https://"):
            raise ValueError("url must be an https:// URL")
        return normalized

    @field_validator("path")
    @classmethod
    def normalize_audio_path(cls, value: str | None) -> str | None:
        if value is None:
            return None
        normalized = value.strip()
        if not normalized or ".." in normalized or "\x00" in normalized:
            raise ValueError(
                "path must be a relative media filename or /media/... path"
            )
        return normalized

    @model_validator(mode="after")
    def validate_command_fields(self) -> "OutputCommandBody":
        if self.action == "RELAY_1_ON":
            if self.duration_s is None:
                raise ValueError("RELAY_1_ON requires duration_s")
            if self.url is not None or self.path is not None:
                raise ValueError("RELAY_1_ON does not take url or path")
        elif self.action == "RELAY_1_OFF":
            if self.duration_s is not None:
                raise ValueError("RELAY_1_OFF does not take duration_s")
            if self.url is not None or self.path is not None:
                raise ValueError("RELAY_1_OFF does not take url or path")
        elif self.action == "AUDIO_PLAY_HTTPS":
            if self.url is None:
                raise ValueError("AUDIO_PLAY_HTTPS requires url")
            if self.duration_s is not None or self.path is not None:
                raise ValueError("AUDIO_PLAY_HTTPS does not take duration_s or path")
        elif self.action == "AUDIO_PLAY_FILE":
            if self.path is None:
                raise ValueError("AUDIO_PLAY_FILE requires path")
            if self.duration_s is not None or self.url is not None:
                raise ValueError("AUDIO_PLAY_FILE does not take duration_s or url")
        elif self.action == "AUDIO_STOP":
            if (
                self.duration_s is not None
                or self.url is not None
                or self.path is not None
            ):
                raise ValueError("AUDIO_STOP does not take duration_s, url, or path")
        if (
            self.action_type is not None
            and self.message_type is not None
            and self.action_type != self.message_type
        ):
            raise ValueError("action_type and message_type must match")
        message_type = self.message_type or self.action_type
        if message_type is not None and self.action != "RELAY_1_ON":
            raise ValueError("message_type is only valid for RELAY_1_ON")
        if message_type != self.message_type:
            return self.model_copy(update={"message_type": message_type})
        return self


class OutputCommandView(BaseModel):
    node_id: str
    topic: str
    action: str
    duration_s: int | None = None
    url: str | None = None
    path: str | None = None
    protocol_version: Literal[1] = 1
    message_type: str | None = None
    action_type: str | None = None
    expires_at_utc: datetime
    published: Literal[True] = True


class InstallationMetadataBody(BaseModel):
    latitude: float = Field(
        ge=-90, le=90, description="WGS84 latitude in decimal degrees"
    )
    longitude: float = Field(
        ge=-180, le=180, description="WGS84 longitude in decimal degrees"
    )
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
    chip_mac: str | None = None
    chip_variant: str | None = None
    provisioning_state: str
    lifecycle_state: Literal["active", "deactivated"]
    availability: str
    status: Literal["online", "offline"]
    pending_configuration_change: bool
    desired: dict[str, Any]
    reported: dict[str, Any]
    last_seen_at: str | None
    installation: InstallationMetadataBody | None = None
    owner_type: Literal["user", "group"] | None = None
    owner_id: str | None = None
    owner_display_name: str | None = None
    monitoring: dict[str, Any] | None = None


class DeviceSummaryView(BaseModel):
    node_id: str
    node_type: str
    hardware_revision: str
    chip_mac: str | None = None
    chip_variant: str | None = None
    provisioning_state: str
    lifecycle_state: Literal["active", "deactivated"]
    status: Literal["online", "offline"]
    last_seen_at: str | None
    installation: InstallationMetadataBody | None = None
    owner_type: Literal["user", "group"] | None = None
    owner_id: str | None = None
    owner_display_name: str | None = None


class CreateUserBody(BaseModel):
    display_name: str = Field(min_length=1, max_length=128)

    @field_validator("display_name")
    @classmethod
    def normalize_display_name(cls, value: str) -> str:
        normalized = value.strip()
        if not normalized:
            raise ValueError("display_name must not be blank")
        return normalized


class UserStatusBody(BaseModel):
    status: Literal["active", "disabled"]


class LinkExternalIdentityBody(BaseModel):
    provider: Literal["github"]
    provider_subject: str = Field(pattern=r"^[0-9]{1,32}$")


class UserIdentityView(BaseModel):
    identity_id: str
    provider: str
    provider_subject: str
    status: Literal["active", "disabled"]
    created_at: datetime
    last_authenticated_at: datetime | None


class UserView(BaseModel):
    user_id: str
    display_name: str
    status: Literal["active", "disabled"]
    created_at: datetime
    updated_at: datetime
    external_identities: list[UserIdentityView]


class DeviceOwnerBody(BaseModel):
    model_config = ConfigDict(extra="forbid")

    owner_type: Literal["user", "group"] | None = None
    owner_id: str | None = None

    @model_validator(mode="after")
    def require_owner_pair(self) -> "DeviceOwnerBody":
        if (self.owner_type is None) != (self.owner_id is None):
            raise ValueError("owner_type and owner_id must be provided together")
        return self


class OwnershipAssignmentView(BaseModel):
    assignment_id: str
    owner_type: Literal["user", "group"]
    owner_id: str
    owner_display_name: str
    valid_from: datetime
    valid_to: datetime | None


class DeviceOwnershipView(BaseModel):
    node_id: str
    current_owner_type: Literal["user", "group"] | None
    current_owner_id: str | None
    current_owner_display_name: str | None
    assignments: list[OwnershipAssignmentView]


class CreateGroupBody(BaseModel):
    name: str = Field(min_length=1, max_length=128)
    owner_user_id: str | None = None

    @field_validator("name")
    @classmethod
    def normalize_group_name(cls, value: str) -> str:
        normalized = value.strip()
        if not normalized:
            raise ValueError("name must not be blank")
        return normalized


class GroupMemberBody(BaseModel):
    role: Literal["owner", "member", "viewer"]


class GroupStatusBody(BaseModel):
    status: Literal["active", "disabled"]


class GroupMemberView(BaseModel):
    user_id: str
    display_name: str
    role: Literal["owner", "member", "viewer"]


class GroupView(BaseModel):
    group_id: str
    name: str
    status: Literal["active", "disabled"]
    created_by_user_id: str
    created_at: datetime
    updated_at: datetime
    members: list[GroupMemberView]
