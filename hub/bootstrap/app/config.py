from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="DBMAP_", extra="ignore")

    database_url: str = "sqlite:////data/bootstrap.db"
    admin_token: str = ""
    viewer_token: str = ""
    opa_decision_url: str = "http://opa:8181/v1/data/dbmap/authz/allow"
    opa_timeout_s: float = 2.0

    mqtt_host: str = "mosquitto"
    mqtt_port: int = 8883
    mqtt_ca_file: str = "/certs/ca.crt"
    mqtt_username: str = "hub-bootstrap"
    mqtt_password: str = ""
    mqtt_admin_username: str = "admin"
    mqtt_admin_password: str = ""
    mqtt_enabled: bool = True

    advertised_mqtt_host: str = "hub.local"
    advertised_mqtt_port: int = 8883
    advertised_mqtt_use_tls: bool = True

    site_id: str = "local"


settings = Settings()
