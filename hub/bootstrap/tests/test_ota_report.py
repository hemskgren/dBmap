import json
from pathlib import Path

from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-ota-report-test.db"))
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False
settings.hub_monitor_enabled = False
settings.device_monitor_enabled = True
settings.device_diag_enabled = True
settings.hub_id = "local-test"
settings.device_diag_max_events_per_device = 30

from app import device_diag, service
from app.db import SessionLocal, init_db
from app.models import DeviceDiagEvent, DeviceDiagState, Node, ReportedState
from sqlalchemy import delete, select


def _reset_db() -> None:
    init_db()
    db = SessionLocal()
    try:
        db.execute(delete(DeviceDiagEvent))
        db.execute(delete(DeviceDiagState))
        db.execute(delete(ReportedState))
        db.execute(delete(Node))
        db.commit()
    finally:
        db.close()


def _add_node(db, node_id: str = "OUTOTA001") -> None:
    db.add(
        Node(
            node_id=node_id,
            node_type="output",
            hardware_revision="generic",
            lifecycle_state="active",
            provisioning_state="active",
            availability="OPERATIONAL",
        )
    )
    db.add(ReportedState(node_id=node_id, firmware_version="0.1.4"))
    db.commit()


def test_install_report_then_boot_confirmed_status() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        install = {
            "protocol_version": 1,
            "message_type": "ota_update",
            "event": "install_succeeded",
            "ota_phase": "installed_pending_reboot",
            "reboot_reason": "ota",
            "node_id": "OUTOTA001",
            "firmware_version": "0.1.4",
            "previous_firmware_version": "0.1.4",
            "target_firmware_version": "0.1.5",
            "firmware_id": "fw-abc",
        }
        service.apply_reported(db, "OUTOTA001", install)
        state = db.get(DeviceDiagState, "OUTOTA001")
        assert state is not None
        pending = json.loads(state.pending_ota_json)
        assert pending["target_firmware_version"] == "0.1.5"
        assert pending["previous_firmware_version"] == "0.1.4"
        reported = db.get(ReportedState, "OUTOTA001")
        assert reported is not None
        assert reported.firmware_version == "0.1.4"

        boot = {
            "protocol_version": 1,
            "message_type": "ota_update",
            "event": "boot_confirmed",
            "ota_phase": "boot_confirmed",
            "reboot_reason": "ota",
            "node_id": "OUTOTA001",
            "firmware_version": "0.1.5",
            "previous_firmware_version": "0.1.4",
            "target_firmware_version": "0.1.5",
            "firmware_id": "fw-abc",
        }
        service.apply_reported(db, "OUTOTA001", boot)
        state = db.get(DeviceDiagState, "OUTOTA001")
        assert state is not None
        assert state.pending_ota_json in {"{}", ""}
        reported = db.get(ReportedState, "OUTOTA001")
        assert reported is not None
        assert reported.firmware_version == "0.1.5"
        events = {
            event.event_type: event
            for event in db.scalars(
                select(DeviceDiagEvent).where(DeviceDiagEvent.node_id == "OUTOTA001")
            )
        }
        assert "ota_install_reported" in events
        assert "ota_boot_confirmed" in events
        detail = json.loads(events["ota_boot_confirmed"].detail_json)
        assert detail["previous_firmware_version"] == "0.1.4"
        assert detail["running_firmware_version"] == "0.1.5"
    finally:
        db.close()


def test_hello_correlates_pending_ota_when_version_changes() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        service.apply_reported(
            db,
            "OUTOTA001",
            {
                "protocol_version": 1,
                "message_type": "ota_update",
                "event": "install_succeeded",
                "reboot_reason": "ota",
                "firmware_version": "0.1.4",
                "previous_firmware_version": "0.1.4",
                "target_firmware_version": "0.1.5",
            },
        )
        # Simulate MQTT unavailable for boot_confirmed; hello carries new version.
        service.apply_reported(
            db,
            "OUTOTA001",
            {
                "protocol_version": 1,
                "node_id": "OUTOTA001",
                "firmware_version": "0.1.5",
                "uptime_s": 3,
            },
        )
        state = db.get(DeviceDiagState, "OUTOTA001")
        assert state is not None
        assert state.pending_ota_json in {"{}", ""}
        events = list(
            db.scalars(
                select(DeviceDiagEvent).where(
                    DeviceDiagEvent.node_id == "OUTOTA001",
                    DeviceDiagEvent.event_type == "ota_boot_confirmed",
                )
            )
        )
        assert len(events) == 1
        detail = json.loads(events[0].detail_json)
        assert detail.get("correlated_from") == "hello_or_health"
    finally:
        db.close()


def test_software_reset_alone_does_not_confirm_ota() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        service.apply_reported(
            db,
            "OUTOTA001",
            {
                "protocol_version": 1,
                "message_type": "ota_update",
                "event": "install_succeeded",
                "reboot_reason": "ota",
                "firmware_version": "0.1.4",
                "previous_firmware_version": "0.1.4",
                "target_firmware_version": "0.1.5",
            },
        )
        # Same old version hello with SW reset in diag must not clear pending as success.
        service.apply_reported(
            db,
            "OUTOTA001",
            {
                "protocol_version": 1,
                "firmware_version": "0.1.4",
                "diag": {"last_reset_reason": "SW"},
            },
        )
        state = db.get(DeviceDiagState, "OUTOTA001")
        assert state is not None
        pending = json.loads(state.pending_ota_json)
        assert pending["target_firmware_version"] == "0.1.5"
        boot_events = list(
            db.scalars(
                select(DeviceDiagEvent).where(
                    DeviceDiagEvent.node_id == "OUTOTA001",
                    DeviceDiagEvent.event_type == "ota_boot_confirmed",
                )
            )
        )
        assert boot_events == []
    finally:
        db.close()


def test_ota_events_remain_separate_from_diag_counters() -> None:
    """OTA lifecycle events must not invent mqtt_* diagnostic bursts."""
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        service.apply_reported(
            db,
            "OUTOTA001",
            {
                "protocol_version": 1,
                "message_type": "ota_update",
                "event": "install_succeeded",
                "reboot_reason": "ota",
                "firmware_version": "0.1.4",
                "previous_firmware_version": "0.1.4",
                "target_firmware_version": "0.1.5",
            },
        )
        service.apply_reported(
            db,
            "OUTOTA001",
            {
                "protocol_version": 1,
                "message_type": "ota_update",
                "event": "boot_confirmed",
                "reboot_reason": "ota",
                "firmware_version": "0.1.5",
                "previous_firmware_version": "0.1.4",
                "target_firmware_version": "0.1.5",
            },
        )
        events = list(
            db.scalars(
                select(DeviceDiagEvent).where(DeviceDiagEvent.node_id == "OUTOTA001")
            )
        )
        types = {event.event_type for event in events}
        assert "ota_install_reported" in types
        assert "ota_boot_confirmed" in types
        assert "mqtt_disconnect_burst" not in types
        assert "mqtt_reconnect_burst" not in types
        assert "mqtt_connect_attempt_burst" not in types
    finally:
        db.close()


def test_boot_mismatch_clears_pending() -> None:
    _reset_db()
    db = SessionLocal()
    try:
        _add_node(db)
        device_diag.ingest_ota_report(
            db,
            node_id="OUTOTA001",
            payload={
                "message_type": "ota_update",
                "event": "install_succeeded",
                "firmware_version": "0.1.4",
                "previous_firmware_version": "0.1.4",
                "target_firmware_version": "0.1.5",
            },
            previous_firmware_version="0.1.4",
        )
        result = device_diag.ingest_ota_report(
            db,
            node_id="OUTOTA001",
            payload={
                "message_type": "ota_update",
                "event": "boot_mismatch",
                "firmware_version": "0.1.4",
                "previous_firmware_version": "0.1.4",
                "target_firmware_version": "0.1.5",
            },
            previous_firmware_version="0.1.4",
        )
        db.commit()
        assert result is not None
        assert result["phase"] == "boot_mismatch"
        state = db.get(DeviceDiagState, "OUTOTA001")
        assert state is not None
        assert state.pending_ota_json in {"{}", ""}
    finally:
        db.close()
