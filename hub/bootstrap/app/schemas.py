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
    hub_id: str
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
    """L2 source hypothesis under acoustic.classification (may be null)."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    classification_schema: Literal["l2"] = Field(default="l2", alias="schema")
    version: int = Field(default=1, ge=1)
    source_family: str | None = Field(default=None, min_length=1, max_length=64)
    confidence: float | None = Field(default=None, ge=0, le=1, allow_inf_nan=False)
    hints: dict[str, Any] = Field(default_factory=dict)


class ObservationBearing(BaseModel):
    model_config = ConfigDict(extra="forbid")

    deg: float = Field(ge=0, lt=360, allow_inf_nan=False)
    reference: Literal["node"] = "node"
    confidence: float | None = Field(default=None, ge=0, le=1, allow_inf_nan=False)


class ObservationIdentity(BaseModel):
    model_config = ConfigDict(extra="forbid")

    observation_id: UUID
    node_id: str = Field(min_length=1, max_length=64)
    hub_id: str = Field(default="", max_length=64)
    sequence_number: int = Field(ge=0)


class ObservationContext(BaseModel):
    model_config = ConfigDict(extra="forbid")

    event_time_utc: datetime
    capture_timestamp_monotonic_us: int = Field(ge=0)
    timing_quality: Literal["synchronized", "estimated", "unsynchronized"]
    clock_offset_ms: float | None = Field(default=None, allow_inf_nan=False)
    timestamp_uncertainty_ms: float = Field(ge=0, allow_inf_nan=False)
    interval_duration_ms: float = Field(ge=0, allow_inf_nan=False)
    episode_duration_ms: float = Field(ge=0, allow_inf_nan=False)
    emission_kind: Literal["start", "periodic", "end", "complete"]
    termination_reason: Literal["natural", "invalid_hold", "ota_reboot"] | None = None

    @field_validator("event_time_utc")
    @classmethod
    def require_timezone_and_normalize_utc(cls, value: datetime) -> datetime:
        if value.tzinfo is None or value.utcoffset() is None:
            raise ValueError("event_time_utc must include a timezone")
        return value.astimezone(timezone.utc)


class ObservationSpatial(BaseModel):
    model_config = ConfigDict(extra="forbid")

    bearing: ObservationBearing | None = None
    position: Any | None = None


class ObservationL0Rms(BaseModel):
    model_config = ConfigDict(extra="forbid")

    mean_dbfs: float | None = Field(default=None, le=0, allow_inf_nan=False)
    min_dbfs: float | None = Field(default=None, le=0, allow_inf_nan=False)
    max_dbfs: float | None = Field(default=None, le=0, allow_inf_nan=False)


class ObservationL0Bands(BaseModel):
    model_config = ConfigDict(extra="forbid")

    mean_dbfs: list[float | None] = Field(min_length=4, max_length=4)
    fraction: list[float | None] = Field(min_length=4, max_length=4)


class ObservationL0Hl(BaseModel):
    model_config = ConfigDict(extra="forbid")

    mean_db: float | None = Field(default=None, allow_inf_nan=False)


class ObservationL0T1(BaseModel):
    model_config = ConfigDict(extra="forbid")

    rms_stdev_db: float | None = Field(default=None, ge=0, allow_inf_nan=False)
    rms_range_db: float | None = Field(default=None, allow_inf_nan=False)


class ObservationL0T2(BaseModel):
    model_config = ConfigDict(extra="forbid")

    rms_slope_db_per_s: float | None = Field(default=None, allow_inf_nan=False)


class ObservationL0T3(BaseModel):
    model_config = ConfigDict(extra="forbid")

    crest_max_db: float | None = Field(default=None, allow_inf_nan=False)
    high_crest_count: int | None = Field(default=None, ge=0)
    high_crest_rate: float | None = Field(default=None, ge=0, le=1, allow_inf_nan=False)


class ObservationL0T6(BaseModel):
    model_config = ConfigDict(extra="forbid")

    hl_slope_db_per_s: float | None = Field(default=None, allow_inf_nan=False)


class ObservationL0Measurements(BaseModel):
    """Combined-scene L0 aggregate (not per-source). Strict nested schema."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    measurement_schema: Literal["l0"] = Field(alias="schema")
    version: int = Field(ge=1)
    band_plan_version: int = Field(ge=1)
    crest_threshold_version: int = Field(ge=1)
    crest_threshold_db: float = Field(allow_inf_nan=False)
    mic_index: int = Field(ge=0)
    sample_rate_hz: int = Field(gt=0)
    analysis_window_ms: int = Field(gt=0)
    window_count: int = Field(ge=0)
    clipped_window_count: int = Field(ge=0)
    n_crest: int = Field(ge=0)
    rms: ObservationL0Rms
    bands: ObservationL0Bands
    hl: ObservationL0Hl
    t1: ObservationL0T1
    t2: ObservationL0T2
    t3: ObservationL0T3
    t6: ObservationL0T6


class ObservationL1FeatureStats(BaseModel):
    """Mean/stdev pair for one L1 scalar feature."""

    model_config = ConfigDict(extra="forbid")

    mean: float | None = Field(default=None, allow_inf_nan=False)
    stdev: float | None = Field(default=None, ge=0, allow_inf_nan=False)


class ObservationL1Signature(BaseModel):
    """Interval-scoped L1 Acoustic Signature (schema l1)."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    signature_schema: Literal["l1"] = Field(alias="schema")
    version: int = Field(ge=1)
    sample_rate_hz: int = Field(gt=0)
    analysis_window_ms: int = Field(gt=0)
    fft_size: int = Field(gt=0)
    freq_min_hz: float = Field(ge=0, allow_inf_nan=False)
    freq_max_hz: float = Field(gt=0, allow_inf_nan=False)
    window_count: int = Field(ge=0)
    invalid_window_count: int = Field(ge=0)
    centroid_hz: ObservationL1FeatureStats
    bandwidth_hz: ObservationL1FeatureStats
    dominant_hz: ObservationL1FeatureStats
    flatness: ObservationL1FeatureStats


class ObservationL2FeatureStats(BaseModel):
    """One L2 dynamics feature: valid_count plus optional mean/stdev."""

    model_config = ConfigDict(extra="forbid")

    valid_count: int = Field(ge=0)
    mean: float | None = Field(default=None, ge=0, allow_inf_nan=False)
    stdev: float | None = Field(default=None, ge=0, allow_inf_nan=False)

    @model_validator(mode="after")
    def valid_count_null_rules(self) -> "ObservationL2FeatureStats":
        if self.valid_count == 0:
            if self.mean is not None or self.stdev is not None:
                raise ValueError(
                    "mean and stdev must be null when valid_count is 0"
                )
            return self
        if self.mean is None:
            raise ValueError("mean is required when valid_count >= 1")
        if self.valid_count == 1:
            if self.stdev is not None:
                raise ValueError("stdev must be null when valid_count is 1")
        elif self.stdev is None:
            raise ValueError("stdev is required when valid_count >= 2")
        return self


class ObservationL2Dynamics(BaseModel):
    """Interval-scoped L2 Acoustic Dynamics (schema l2, contract version 1 only)."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    dynamics_schema: Literal["l2"] = Field(alias="schema")
    version: Literal[1]
    sample_rate_hz: int = Field(gt=0)
    analysis_window_ms: int = Field(gt=0)
    fft_size: int = Field(gt=0)
    freq_min_hz: float = Field(ge=0, allow_inf_nan=False)
    freq_max_hz: float = Field(gt=0, allow_inf_nan=False)
    frame_hop_samples: int = Field(gt=0)
    window_count: int = Field(ge=0)
    invalid_window_count: int = Field(ge=0)
    history_overflow: bool
    spectral_flux: ObservationL2FeatureStats
    envelope_cv: ObservationL2FeatureStats
    dominant_delta_hz: ObservationL2FeatureStats

    @model_validator(mode="after")
    def overflow_option_a(self) -> "ObservationL2Dynamics":
        if not self.history_overflow:
            return self
        if self.window_count != 0 or self.invalid_window_count != 0:
            raise ValueError(
                "history_overflow requires window_count and invalid_window_count to be 0"
            )
        for name in ("spectral_flux", "envelope_cv", "dominant_delta_hz"):
            feature = getattr(self, name)
            if (
                feature.valid_count != 0
                or feature.mean is not None
                or feature.stdev is not None
            ):
                raise ValueError(
                    "history_overflow requires all L2 feature metrics to be cleared"
                )
        return self


class ObservationAcoustic(BaseModel):
    model_config = ConfigDict(extra="forbid")

    signal_level_dbfs: float | None = Field(default=None, le=0, allow_inf_nan=False)
    measurements: ObservationL0Measurements
    signature: ObservationL1Signature | None = None
    dynamics: ObservationL2Dynamics | None = None
    classification: ObservationClassification | None = None


class ObservationBody(BaseModel):
    """Hierarchical Observation protocol v1 (breaking; flat payloads rejected)."""

    model_config = ConfigDict(extra="forbid")

    protocol_version: Literal[1]
    message_type: Literal["observation"]
    identity: ObservationIdentity
    context: ObservationContext
    acoustic: ObservationAcoustic
    spatial: ObservationSpatial

    @model_validator(mode="after")
    def termination_reason_rules(self) -> "ObservationBody":
        kind = self.context.emission_kind
        term = self.context.termination_reason
        if kind in ("end", "complete"):
            if term is None:
                raise ValueError("termination_reason is required for end and complete")
        elif term is not None:
            raise ValueError("termination_reason is only allowed on end or complete")
        return self


class ObservationView(ObservationBody):
    received_time_utc: datetime


class PaginationMeta(BaseModel):
    """Shared list pagination metadata for Hub collection endpoints."""

    total_count: int = Field(ge=0)
    page: int = Field(ge=1)
    page_size: int = Field(ge=1)
    total_pages: int = Field(ge=0)
    order: Literal["newest", "oldest"]
    snapshot_id: int | None = None


# Backward-compatible alias used by observation OpenAPI/tests.
ObservationPaginationMeta = PaginationMeta


class ObservationListEnvelope(BaseModel):
    """Paginated observation reports. Retains resource-specific ``reports`` key."""

    pagination: PaginationMeta
    reports: list[ObservationView]


class ObservationCountView(BaseModel):
    """Observation report rows and distinct observation_id values (optional node filter)."""

    total: int = Field(ge=0, description="Observation report rows (MQTT payloads / DB rows).")
    unique_observations: int = Field(
        ge=0,
        description="Distinct identity.observation_id values among those reports.",
    )


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


class UserListEnvelope(BaseModel):
    """Paginated users. Uses the shared ``items`` collection key."""

    pagination: PaginationMeta
    items: list[UserView]


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


class GroupListEnvelope(BaseModel):
    """Paginated groups. Uses the shared ``items`` collection key."""

    pagination: PaginationMeta
    items: list[GroupView]
