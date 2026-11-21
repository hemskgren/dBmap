import hashlib
import hmac
import secrets
from datetime import UTC, datetime


def new_token() -> str:
    return secrets.token_urlsafe(32)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def mqtt_username(node_id: str) -> str:
    return node_id.lower()


def allocate_node_id(node_type: str, sequence: int, simulated: bool = False) -> str:
    prefix = "SIM-EAR" if simulated else ("OUT" if node_type == "output" else "EAR")
    return f"{prefix}-{sequence:03d}"


def _desired_requirement_set(value: object) -> bool:
    """Return True when Desired expresses an explicit requirement for a field.

    Empty / missing Desired values mean "no requirement" (not a mismatch).
    Non-empty strings and numeric revisions (including 0) are compared normally.
    """
    if value is None:
        return False
    if isinstance(value, str):
        return value.strip() != ""
    return True


def pending_configuration_change(reported: dict, desired: dict) -> bool:
    """True when any explicit Desired version differs from Reported.

    Empty Desired string fields (UI "Not set") are skipped. Firmware Desired
    HTTPS polling and classifier delivery are unchanged by this Hub-only flag.
    """
    keys = (
        "config_version",
        "firmware_version",
        "calibration_version",
        "classifier_version",
    )
    for key in keys:
        desired_value = desired.get(key)
        if not _desired_requirement_set(desired_value):
            continue
        if reported.get(key) != desired_value:
            return True
    return False


def timing_safe_eq(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


def isoformat(dt: datetime | None) -> str | None:
    if dt is None:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=UTC)
    return dt.isoformat()
