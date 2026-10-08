import logging

from fastapi import FastAPI

from app.api import router
from app.config import settings
from app.db import init_db
from app.mqtt_listener import start_mqtt_thread
from app.mqtt_security import ensure_broker_security

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")


def _validate_security_settings() -> None:
    required_secrets = {
        "DBMAP_ADMIN_TOKEN": settings.admin_token,
        "DBMAP_VIEWER_TOKEN": settings.viewer_token,
        "DBMAP_MQTT_PASSWORD": settings.mqtt_password,
        "DBMAP_MQTT_ADMIN_PASSWORD": settings.mqtt_admin_password,
    }
    for name, secret in required_secrets.items():
        if len(secret) < 32:
            raise RuntimeError(f"{name} must be a generated secret of at least 32 characters")
    if not settings.advertised_mqtt_use_tls:
        raise RuntimeError("device MQTT connections must use TLS")


def create_app() -> FastAPI:
    init_db()
    if settings.mqtt_enabled:
        _validate_security_settings()
        ensure_broker_security()
        start_mqtt_thread()
    app = FastAPI(title="dBmap hub-bootstrap", version="0.1.0")
    app.include_router(router)
    return app


app = create_app()
