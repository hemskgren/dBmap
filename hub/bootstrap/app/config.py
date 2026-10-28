from urllib.parse import urlsplit

from pydantic import Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="DBMAP_", extra="ignore")

    database_url: str = "sqlite:////data/bootstrap.db"
    firmware_catalog_dir: str = "/firmware-catalog"
    admin_token: str = ""
    viewer_token: str = ""
    opa_decision_url: str = "https://opa:8181/v1/data/dbmap/authz/allow"
    opa_ca_file: str = "/certs/ca.crt"
    opa_timeout_s: float = 2.0
    public_base_path: str = ""
    nginx_probe_host: str = "nginx"
    nginx_probe_port: int = 8443
    nginx_probe_name: str = "hub.local"
    web_probe_host: str = "dbmap-web"
    web_probe_port: int = 443
    firmware_probe_host: str = "firmware-builder"
    firmware_probe_port: int = 8443
    edge_probe_timeout_s: float = 2.0
    docker_socket: str = "/var/run/docker.sock"
    compose_project: str = ""
    hub_monitor_enabled: bool = True
    hub_monitor_interval_s: float = Field(default=60.0, ge=5.0, le=3600.0)
    hub_monitor_retention_days: int = Field(default=30, ge=30, le=90)

    # Device connectivity / Hub-observed availability (D0). Tunable via .env.
    device_online_window_s: float = Field(default=90.0, ge=30.0, le=600.0)
    device_monitor_enabled: bool = True
    device_monitor_retention_days: int = Field(default=90, ge=30, le=90)
    device_monitor_max_samples_per_device: int = Field(default=500, ge=50, le=5000)
    device_monitor_max_events_per_device: int = Field(default=500, ge=50, le=5000)

    # Device-reported diagnostics ingest (D2). Hard ceiling of 30 events/device.
    device_diag_enabled: bool = True
    device_diag_max_events_per_device: int = Field(default=30, ge=5, le=30)
    device_diag_rssi_degraded_dbm: int = Field(default=-80, ge=-100, le=-30)
    device_diag_aggregate_window_s: float = Field(default=3600.0, ge=60.0, le=86400.0)

    mqtt_host: str = "mosquitto"
    mqtt_port: int = 8883
    mqtt_ca_file: str = "/certs/ca.crt"
    mqtt_username: str = "hub-bootstrap"
    mqtt_password: str = ""
    mqtt_admin_username: str = "admin"
    mqtt_admin_password: str = ""
    mqtt_enabled: bool = True

    public_mqtt_host: str = "hub.local"
    public_mqtt_port: int = 8883
    public_mqtt_use_tls: bool = True
    local_mqtt_host: str = "hub.local"
    local_mqtt_port: int = 8883
    public_hub_url: str = ""

    def local_hub_url(self) -> str:
        """Configured Local Hub HTTPS base for device call-home (not the browser origin)."""
        base = self.public_base_path.strip().rstrip("/")
        return f"https://{self.local_mqtt_host}:{self.nginx_probe_port}{base}"

    @field_validator("public_hub_url")
    @classmethod
    def validate_public_hub_url(cls, value: str) -> str:
        value = value.strip().rstrip("/")
        parsed = urlsplit(value)
        if value and (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError(
                "public_hub_url must be an HTTPS base URL without query or fragment"
            )
        return value

    hub_id: str = Field(
        default="local-01",
        min_length=1,
        max_length=64,
        pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]*$",
    )
    group_creation_limit_per_user: int = Field(default=1, ge=0, le=100)


settings = Settings()
