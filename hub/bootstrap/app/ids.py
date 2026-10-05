import hashlib
import hmac
import secrets
from datetime import datetime, timezone


def new_token() -> str:
    return secrets.token_urlsafe(32)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def mqtt_username(node_id: str) -> str:
    return node_id.lower()


def allocate_node_id(node_type: str, sequence: int, simulated: bool = False) -> str:
    prefix = "SIM-EAR" if simulated else ("OUT" if node_type == "output" else "EAR")
    return f"{prefix}-{sequence:03d}"


def pending_configuration_change(reported: dict, desired: dict) -> bool:
    keys = ("config_version", "firmware_version", "calibration_version", "classifier_version")
    return any(reported.get(k) != desired.get(k) for k in keys)


def timing_safe_eq(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode(), b.encode())


def isoformat(dt: datetime | None) -> str | None:
    if dt is None:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.isoformat()
