from app import api as api_module


def test_public_endpoint_checks_are_not_monitored_services(monkeypatch) -> None:
    monkeypatch.setattr(
        api_module.core_monitor,
        "build_status",
        lambda _db: {"health": {"components": []}},
    )
    monkeypatch.setattr(api_module, "_database_healthy", lambda _db: (True, None))
    monkeypatch.setattr(api_module, "_self_worker_healthy", lambda: (True, []))
    monkeypatch.setattr(api_module.settings, "public_api_monitor_enabled", True)
    monkeypatch.setattr(api_module.settings, "public_mqtt_monitor_enabled", True)

    services = api_module._service_inventory(None)
    service_ids = {service["id"] for service in services}

    assert "public-api" not in service_ids
    assert "public-mqtt" not in service_ids
    assert {"hub-api", "mqtt", "operations"} <= service_ids
    assert "database" not in service_ids
    by_id = {service["id"]: service for service in services}
    assert by_id["hub-api"]["database"]["name"] == "Hub database"
    assert by_id["operations"]["database"]["name"] == "Operations database"
    assert by_id["nginx"]["state"] == "unknown"
