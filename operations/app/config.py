from urllib.parse import urlsplit

from pydantic import Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="DBMAP_", extra="ignore")

    database_url: str = "sqlite:////data/operations.db"
    admin_token: str = ""
    viewer_token: str = ""

    hub_id: str = Field(
        default="local-01",
        min_length=1,
        max_length=64,
        pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]*$",
    )
    opa_decision_url: str = "https://opa:8181/v1/data/dbmap/authz/allow"
    opa_ca_file: str = "/certs/ca.crt"
    opa_timeout_s: float = 2.0

    hub_api_host: str = "hub-bootstrap"
    hub_api_port: int = 8443
    public_base_path: str = ""
    nginx_probe_host: str = "nginx"
    nginx_probe_port: int = 8443
    nginx_probe_name: str = "hub.local"
    web_probe_host: str = "dbmap-web"
    web_probe_port: int = 443
    opa_host: str = "opa"
    opa_port: int = 8181
    mqtt_host: str = "mosquitto"
    mqtt_port: int = 8883
    mqtt_ca_file: str = "/certs/ca.crt"
    mqtt_username: str = "hub-bootstrap"
    mqtt_password: str = ""

    public_hub_url: str = ""
    public_mqtt_host: str = "hub.local"
    public_mqtt_port: int = 8883
    public_mqtt_use_tls: bool = True

    core_monitor_enabled: bool = True
    core_monitor_interval_s: float = Field(default=60.0, ge=5.0, le=3600.0)
    gap_threshold_s: float = Field(default=120.0, ge=5.0)
    retention_days: int = Field(default=30, ge=30, le=90)
    public_api_monitor_enabled: bool = False
    public_mqtt_monitor_enabled: bool = False
    public_monitor_interval_s: float = Field(default=60.0, ge=5.0, le=3600.0)
    probe_timeout_s: float = Field(default=2.0, ge=0.5, le=30.0)

    @field_validator("public_hub_url")
    @classmethod
    def validate_public_hub_url(cls, value: str) -> str:
        value = value.strip().rstrip("/")
        if not value:
            return value
        parsed = urlsplit(value)
        if (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("public_hub_url must be an HTTPS base URL")
        return value


settings = Settings()
