#!/usr/bin/env python3
"""Write docs/hub-api.openapi.yaml from the live Hub FastAPI routes.

The file is OpenAPI 3.0.3 so Swagger, Postman, Insomnia, Azure API Management,
and AWS API Gateway can import it. Run from the repository:

    cd hub/bootstrap && uv run --extra dev python ../../scripts/export-hub-openapi.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP = ROOT / "hub" / "bootstrap"
OUTPUT = ROOT / "docs" / "hub-api.openapi.yaml"

sys.path.insert(0, str(BOOTSTRAP))

from app.config import settings  # noqa: E402

settings.database_url = "sqlite://"
settings.admin_token = "test-admin"
settings.viewer_token = "test-viewer"
settings.mqtt_enabled = False

from app.main import create_app  # noqa: E402

PUBLIC = {("GET", "/health"), ("POST", "/api/v1/provision")}

TAGS_BY_PREFIX = (
    ("/health", "Health"),
    ("/api/v1/session", "Session"),
    ("/api/v1/users", "Users"),
    ("/api/v1/groups", "Groups"),
    ("/api/v1/devices", "Devices"),
    ("/api/v1/firmware", "Firmware"),
    ("/api/v1/observations", "Observations"),
    ("/api/v1/hub", "Hub"),
    ("/api/v1/device", "Provisioning"),
    ("/api/v1/provision", "Provisioning"),
    ("/api/v1/nodes", "Nodes"),
)

OPERATIONS = {
    ("GET", "/health"): {
        "operationId": "getHealth",
        "summary": "Process health check",
        "description": "Unauthenticated liveness probe used by Compose and Nginx.",
        "response": "HealthResponse",
    },
    ("GET", "/api/v1/session"): {
        "operationId": "getSession",
        "summary": "Current local role",
        "description": "Returns the role of the presented local bearer token after the OPA `session.read` check.",
        "response": "SessionResponse",
        "errors": ["401", "403", "503"],
    },
    ("GET", "/api/v1/users"): {
        "operationId": "listUsers",
        "summary": "List internal users",
        "errors": ["401", "403", "503"],
    },
    ("POST", "/api/v1/users"): {
        "operationId": "createUser",
        "summary": "Create an internal user",
        "errors": ["401", "403", "422", "503"],
    },
    ("GET", "/api/v1/users/{user_id}"): {
        "operationId": "getUser",
        "summary": "Get an internal user",
        "errors": ["401", "403", "404", "503"],
    },
    ("PUT", "/api/v1/users/{user_id}/status"): {
        "operationId": "putUserStatus",
        "summary": "Enable or disable a user",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("POST", "/api/v1/users/{user_id}/identities"): {
        "operationId": "linkUserIdentity",
        "summary": "Link a GitHub identity",
        "description": "Stores a GitHub numeric subject. This does not authenticate GitHub users.",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("PUT", "/api/v1/users/{user_id}/identities/{identity_id}/status"): {
        "operationId": "putIdentityStatus",
        "summary": "Enable or disable a linked identity",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("GET", "/api/v1/groups"): {
        "operationId": "listGroups",
        "summary": "List groups",
        "errors": ["401", "403", "503"],
    },
    ("POST", "/api/v1/groups"): {
        "operationId": "createGroup",
        "summary": "Create a group",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("PUT", "/api/v1/groups/{group_id}/members/{user_id}"): {
        "operationId": "putGroupMember",
        "summary": "Add or update a group member",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("DELETE", "/api/v1/groups/{group_id}/members/{user_id}"): {
        "operationId": "deleteGroupMember",
        "summary": "Remove a group member",
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("PUT", "/api/v1/groups/{group_id}/status"): {
        "operationId": "putGroupStatus",
        "summary": "Enable or disable a group",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("GET", "/api/v1/devices"): {
        "operationId": "listDevices",
        "summary": "List device summaries",
        "description": "Viewer-readable device list. Detail fields stay on `GET /api/v1/nodes/{node_id}`.",
        "errors": ["401", "403", "503"],
    },
    ("GET", "/api/v1/firmware/catalog"): {
        "operationId": "listFirmwareCatalog",
        "summary": "List firmware catalog entries",
        "description": "Includes candidate and registered bundles.",
        "response": "FirmwareCatalogEntry",
        "array": True,
        "errors": ["401", "403", "503"],
    },
    ("POST", "/api/v1/firmware/catalog"): {
        "operationId": "uploadFirmwareCandidate",
        "summary": "Upload a firmware candidate",
        "description": "Multipart upload of `{firmware_id}.zip` and `{firmware_id}.json`. The pair is stored as a candidate until promoted.",
        "response": "FirmwareCatalogEntry",
        "errors": ["401", "403", "409", "413", "422", "503"],
    },
    ("GET", "/api/v1/firmware/catalog/{firmware_id}"): {
        "operationId": "getFirmwareCatalogEntry",
        "summary": "Get firmware metadata",
        "response": "FirmwareCatalogEntry",
        "errors": ["401", "403", "404", "503"],
    },
    ("DELETE", "/api/v1/firmware/catalog/{firmware_id}"): {
        "operationId": "deleteFirmwareCatalogEntry",
        "summary": "Delete a firmware catalog entry",
        "errors": ["401", "403", "404", "503"],
    },
    ("GET", "/api/v1/firmware/catalog/{firmware_id}/artifact"): {
        "operationId": "downloadFirmwareArtifact",
        "summary": "Download the firmware ZIP",
        "binary": "application/zip",
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("GET", "/api/v1/firmware/catalog/{firmware_id}/flash-manifest"): {
        "operationId": "getFirmwareFlashManifest",
        "summary": "ESP Web Tools flash manifest",
        "response": "FlashManifest",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("GET", "/api/v1/firmware/catalog/{firmware_id}/flash-images/{component_name}"): {
        "operationId": "downloadFirmwareFlashImage",
        "summary": "Download one flash image",
        "binary": "application/octet-stream",
        "headers": {"X-Content-SHA256": "SHA-256 digest of the returned image."},
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("POST", "/api/v1/firmware/catalog/{firmware_id}/promote"): {
        "operationId": "promoteFirmwareCandidate",
        "summary": "Promote a candidate to registered",
        "response": "FirmwareCatalogEntry",
        "errors": ["401", "403", "404", "503"],
    },
    ("POST", "/api/v1/firmware/catalog/{firmware_id}/unregister"): {
        "operationId": "unregisterFirmwareRelease",
        "summary": "Return a release to candidate",
        "response": "FirmwareCatalogEntry",
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("GET", "/api/v1/nodes"): {
        "operationId": "listNodes",
        "summary": "List nodes with desired and reported state",
        "errors": ["401", "403", "503"],
    },
    ("POST", "/api/v1/nodes"): {
        "operationId": "createNode",
        "summary": "Register a node",
        "description": "Creates a pending node and one-time bootstrap token. Send `Idempotency-Key` to retry a registration attempt safely.",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("GET", "/api/v1/nodes/{node_id}"): {
        "operationId": "getNode",
        "summary": "Get node detail",
        "errors": ["401", "403", "404", "503"],
    },
    ("POST", "/api/v1/nodes/{node_id}/bootstrap-token"): {
        "operationId": "rotateBootstrapToken",
        "summary": "Issue a replacement bootstrap token",
        "description": "Allowed only while the node is still pending. The previous unused token is invalidated.",
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("GET", "/api/v1/nodes/{node_id}/ownership"): {
        "operationId": "getNodeOwnership",
        "summary": "Get ownership history",
        "errors": ["401", "403", "404", "503"],
    },
    ("PUT", "/api/v1/nodes/{node_id}/ownership"): {
        "operationId": "putNodeOwnership",
        "summary": "Assign or clear the current owner",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("PUT", "/api/v1/nodes/{node_id}/desired"): {
        "operationId": "putNodeDesired",
        "summary": "Update desired state",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("PUT", "/api/v1/nodes/{node_id}/lifecycle"): {
        "operationId": "putNodeLifecycle",
        "summary": "Activate or deactivate a node",
        "description": "Deactivation suspends broker ACLs. Reactivation restores them.",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("PUT", "/api/v1/nodes/{node_id}/hardware-revision"): {
        "operationId": "putNodeHardwareRevision",
        "summary": "Record verified hardware revision",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("PUT", "/api/v1/nodes/{node_id}/installation"): {
        "operationId": "putNodeInstallation",
        "summary": "Set installation metadata",
        "errors": ["401", "403", "404", "422", "503"],
    },
    ("POST", "/api/v1/nodes/{node_id}/commands"): {
        "operationId": "postNodeCommand",
        "summary": "Publish an output command",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("GET", "/api/v1/nodes/{node_id}/compatible-firmware"): {
        "operationId": "listCompatibleFirmware",
        "summary": "List firmware compatible with a node",
        "response": "CompatibleFirmwareEntry",
        "array": True,
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("POST", "/api/v1/nodes/{node_id}/firmware-selection"): {
        "operationId": "validateFirmwareSelection",
        "summary": "Validate a firmware selection",
        "response": "FirmwareSelectionResult",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("GET", "/api/v1/nodes/{node_id}/deployment-attempts"): {
        "operationId": "listDeploymentAttempts",
        "summary": "List deployment attempts",
        "response": "DeploymentAttempt",
        "array": True,
        "errors": ["401", "403", "404", "503"],
    },
    ("POST", "/api/v1/nodes/{node_id}/deployment-attempts"): {
        "operationId": "startDeploymentAttempt",
        "summary": "Start a USB flash attempt",
        "response": "DeploymentAttempt",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("GET", "/api/v1/nodes/{node_id}/deployment-attempts/{attempt_id}"): {
        "operationId": "getDeploymentAttempt",
        "summary": "Get one deployment attempt",
        "response": "DeploymentAttempt",
        "errors": ["401", "403", "404", "503"],
    },
    ("PATCH", "/api/v1/nodes/{node_id}/deployment-attempts/{attempt_id}"): {
        "operationId": "updateDeploymentAttempt",
        "summary": "Update flash progress",
        "response": "DeploymentAttempt",
        "errors": ["401", "403", "404", "409", "422", "503"],
    },
    ("GET", "/api/v1/observations"): {
        "operationId": "listObservations",
        "summary": "List recent observations",
        "errors": ["401", "403", "503"],
    },
    ("GET", "/api/v1/hub/status"): {
        "operationId": "getHubStatus",
        "summary": "Hub mode, addresses, and summary",
        "response": "HubStatus",
        "errors": ["401", "403", "503"],
    },
    ("GET", "/api/v1/hub/components/{component}/details"): {
        "operationId": "getHubComponentDetails",
        "summary": "On-demand component details",
        "response": "HubComponentDetails",
        "errors": ["401", "403", "404", "503"],
    },
    ("GET", "/api/v1/hub/components/{component}/logs"): {
        "operationId": "getHubComponentLogs",
        "summary": "Latest container log lines",
        "description": "Returns the last 10 lines for an allowlisted Compose service. Admin only.",
        "response": "HubComponentLogs",
        "errors": ["401", "403", "404", "503"],
    },
    ("POST", "/api/v1/provision"): {
        "operationId": "provisionNode",
        "summary": "Exchange a bootstrap token for MQTT credentials",
        "description": (
            "Device-facing. Authenticates with the one-time bootstrap token in the JSON body, not a bearer token. "
            "Initial provisioning is only accepted while the node is pending. An already provisioned node requires "
            "an admin recovery bootstrap. The response includes a one-time device API token. MQTT password handling is unchanged."
        ),
        "errors": ["400", "409", "503"],
    },
    ("GET", "/api/v1/device/desired-firmware"): {
        "operationId": "getDeviceDesiredFirmware",
        "summary": "Read Desired firmware for the authenticated device",
        "description": (
            "Device-facing. Authorization: Bearer <device API token> issued at provision or admin rotation. "
            "The Hub identifies the node from that token. This route does not use admin/viewer tokens or OPA. "
            "Returns that device's Desired firmware id, version, and esp-output.bin SHA-256/size when the "
            "Desired catalog entry is present (candidates allowed when they are the Desired id)."
        ),
        "response": "DeviceDesiredFirmware",
        "errors": ["401", "403"],
    },
    ("GET", "/api/v1/device/ota-image"): {
        "operationId": "getDeviceOtaImage",
        "summary": "Download the Desired esp-output.bin for OTA",
        "description": (
            "Device-facing. Authorization: Bearer <device API token>. Serves only the authenticated node's "
            "Desired firmware app image (esp-output.bin). Candidates are allowed when they are the Desired id. "
            "Rejects images larger than the Output OTA slot (0x1C0000). Does not take a firmware_id query or path parameter."
        ),
        "binary": "application/octet-stream",
        "headers": {"X-Content-SHA256": "SHA-256 digest of the returned image."},
        "errors": ["401", "403", "404", "409"],
    },
    ("POST", "/api/v1/nodes/{node_id}/device-api-token"): {
        "operationId": "rotateDeviceApiToken",
        "summary": "Rotate the device HTTPS API token",
        "description": "Admin only. Returns the new plaintext token once and revokes the previous one. Does not change MQTT credentials.",
        "response": "DeviceApiTokenResponse",
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("POST", "/api/v1/nodes/{node_id}/device-api-token/revoke"): {
        "operationId": "revokeDeviceApiToken",
        "summary": "Revoke the device HTTPS API token",
        "description": "Admin only. Clears the stored hash. Does not reopen provisioning and does not change MQTT credentials.",
        "errors": ["401", "403", "404", "409", "503"],
    },
    ("POST", "/api/v1/nodes/{node_id}/recovery-provision"): {
        "operationId": "openRecoveryProvision",
        "summary": "Open an authorized re-provision",
        "description": (
            "Admin only. Issues a one-time bootstrap token for a device that is already provisioned. "
            "The following POST /api/v1/provision rotates the device API token and the MQTT password. "
            "This is not ordinary firmware reflashing."
        ),
        "response": "BootstrapTokenResponse",
        "errors": ["401", "403", "404", "409", "503"],
    },
}

ERROR_RESPONSES = {
    "400": "Bad request. Used by device provisioning when the bootstrap token is invalid.",
    "401": "Missing or invalid local bearer token.",
    "403": "Authenticated, but OPA denied the action.",
    "404": "The requested resource was not found.",
    "409": "The request conflicts with current Hub state.",
    "413": "Firmware ZIP or metadata exceeds the upload size limit.",
    "422": "Request body or query failed validation.",
    "503": "OPA, MQTT, or the Docker engine is unavailable.",
}

EXTRA_SCHEMAS = {
    "ErrorMessage": {
        "type": "object",
        "required": ["detail"],
        "properties": {
            "detail": {"type": "string"},
        },
    },
    "HealthResponse": {
        "type": "object",
        "required": ["status", "service"],
        "properties": {
            "status": {"type": "string", "example": "ok"},
            "service": {"type": "string", "example": "hub-bootstrap"},
        },
    },
    "DeviceDesiredFirmware": {
        "type": "object",
        "required": ["node_id", "firmware_id", "firmware_version", "app_sha256", "app_size_bytes"],
        "properties": {
            "node_id": {"type": "string"},
            "firmware_id": {"type": "string"},
            "firmware_version": {"type": "string"},
            "app_sha256": {
                "type": "string",
                "description": "SHA-256 of Desired esp-output.bin, or empty when Desired is unset or the catalog entry is missing.",
            },
            "app_size_bytes": {
                "type": "integer",
                "description": "Size of Desired esp-output.bin in bytes, or 0 when unset or missing.",
            },
        },
    },
    "DeviceApiTokenResponse": {
        "type": "object",
        "required": ["node_id", "device_api_token"],
        "properties": {
            "node_id": {"type": "string"},
            "device_api_token": {"type": "string"},
        },
    },
    "SessionResponse": {
        "type": "object",
        "required": ["role"],
        "properties": {
            "role": {"type": "string", "enum": ["admin", "viewer"]},
        },
    },
    "FirmwareCatalogEntry": {
        "type": "object",
        "required": [
            "firmware_id",
            "build_date_utc",
            "target",
            "platform",
            "node_type",
            "hardware_revision",
            "firmware_version",
            "esp_idf_version",
            "configuration_version",
            "artifact_sha256",
            "status",
            "uploaded_at",
        ],
        "properties": {
            "firmware_id": {"type": "string"},
            "build_date_utc": {"type": "string", "format": "date-time"},
            "target": {"type": "string", "example": "output-esp32"},
            "platform": {"type": "string", "example": "esp32"},
            "node_type": {"type": "string", "enum": ["ear", "output"]},
            "hardware_revision": {"type": "string"},
            "firmware_version": {"type": "string"},
            "esp_idf_version": {"type": "string"},
            "configuration_version": {"type": "integer", "minimum": 0},
            "artifact_sha256": {"type": "string"},
            "status": {"type": "string", "enum": ["candidate", "registered"]},
            "uploaded_at": {"type": "string", "format": "date-time"},
            "registered_at": {"type": "string", "format": "date-time", "nullable": True},
            "artifact_size_bytes": {"type": "integer", "nullable": True},
        },
    },
    "FirmwareCompatibility": {
        "type": "object",
        "properties": {
            "node_type": {"type": "string"},
            "hardware_revision": {"type": "string"},
            "hardware_reference": {"type": "string"},
            "chip_platform": {"type": "string"},
        },
    },
    "CompatibleFirmwareEntry": {
        "allOf": [
            {"$ref": "#/components/schemas/FirmwareCatalogEntry"},
            {
                "type": "object",
                "properties": {
                    "compatibility": {"$ref": "#/components/schemas/FirmwareCompatibility"},
                },
            },
        ]
    },
    "FirmwareSelectionResult": {
        "allOf": [
            {"$ref": "#/components/schemas/CompatibleFirmwareEntry"},
            {
                "type": "object",
                "properties": {
                    "device_id": {"type": "string"},
                },
            },
        ]
    },
    "FlashManifestPart": {
        "type": "object",
        "required": ["path", "offset"],
        "properties": {
            "path": {"type": "string"},
            "offset": {"type": "integer"},
        },
    },
    "FlashManifestBuild": {
        "type": "object",
        "required": ["chipFamily", "parts"],
        "properties": {
            "chipFamily": {"type": "string", "example": "ESP32"},
            "parts": {"type": "array", "items": {"$ref": "#/components/schemas/FlashManifestPart"}},
        },
    },
    "FlashManifest": {
        "type": "object",
        "required": ["name", "version", "builds"],
        "properties": {
            "name": {"type": "string"},
            "version": {"type": "string"},
            "new_install_prompt_erase": {"type": "boolean"},
            "new_install_improv_wait_time": {"type": "integer"},
            "builds": {"type": "array", "items": {"$ref": "#/components/schemas/FlashManifestBuild"}},
        },
    },
    "DeploymentAttemptEvent": {
        "type": "object",
        "properties": {
            "status": {"type": "string"},
            "stage": {"type": "string"},
            "progress_percent": {"type": "integer", "nullable": True},
            "failure_code": {"type": "string", "nullable": True},
            "created_at": {"type": "string", "format": "date-time"},
        },
    },
    "DeploymentAttempt": {
        "type": "object",
        "required": ["attempt_id", "node_id", "firmware_id", "status", "stage", "events"],
        "properties": {
            "attempt_id": {"type": "string"},
            "node_id": {"type": "string"},
            "firmware_id": {"type": "string"},
            "firmware_version": {"type": "string"},
            "artifact_sha256": {"type": "string"},
            "status": {"type": "string", "enum": ["in_progress", "succeeded", "failed", "unknown"]},
            "stage": {"type": "string"},
            "progress_percent": {"type": "integer", "nullable": True},
            "failure_code": {"type": "string", "nullable": True},
            "initiated_by": {"type": "string"},
            "flasher_version": {"type": "string", "nullable": True},
            "started_at": {"type": "string", "format": "date-time"},
            "completed_at": {"type": "string", "format": "date-time", "nullable": True},
            "events": {"type": "array", "items": {"$ref": "#/components/schemas/DeploymentAttemptEvent"}},
        },
    },
    "MqttAddress": {
        "type": "object",
        "properties": {
            "host": {"type": "string"},
            "port": {"type": "integer"},
            "use_tls": {"type": "boolean"},
        },
    },
    "HubAddresses": {
        "type": "object",
        "properties": {
            "local_hub_url": {"type": "string"},
            "external_hub_url": {"type": "string", "nullable": True},
            "local_mqtt": {"$ref": "#/components/schemas/MqttAddress"},
            "external_mqtt": {"allOf": [{"$ref": "#/components/schemas/MqttAddress"}], "nullable": True},
        },
    },
    "HubComponent": {
        "type": "object",
        "required": ["id", "name", "status", "logs"],
        "properties": {
            "id": {
                "type": "string",
                "enum": ["nginx", "web", "hub-api", "database", "opa", "mosquitto", "firmware-builder"],
            },
            "name": {"type": "string"},
            "status": {"type": "string"},
            "logs": {"type": "boolean"},
        },
    },
    "HubSummary": {
        "type": "object",
        "properties": {
            "device_count": {"type": "integer"},
            "devices_by_type": {"type": "object", "additionalProperties": {"type": "integer"}},
            "devices_by_lifecycle": {"type": "object", "additionalProperties": {"type": "integer"}},
            "devices_by_availability": {"type": "object", "additionalProperties": {"type": "integer"}},
            "online_device_count": {"type": "integer"},
            "installation_completed_count": {"type": "integer"},
            "installation_incomplete_count": {"type": "integer"},
            "observation_count": {"type": "integer"},
            "observation_limit": {"type": "integer"},
            "latest_observation_at": {"type": "string", "format": "date-time", "nullable": True},
        },
    },
    "HubStatus": {
        "type": "object",
        "required": ["mode", "hub_id", "addresses", "components", "summary"],
        "properties": {
            "mode": {"type": "string", "example": "Local Hub"},
            "hub_id": {"type": "string"},
            "addresses": {"$ref": "#/components/schemas/HubAddresses"},
            "components": {"type": "array", "items": {"$ref": "#/components/schemas/HubComponent"}},
            "summary": {"$ref": "#/components/schemas/HubSummary"},
        },
    },
    "HubComponentDetails": {
        "type": "object",
        "additionalProperties": True,
        "description": "Shape depends on the component. Common fields include service and status.",
        "properties": {
            "service": {"type": "string"},
            "status": {"type": "string"},
        },
    },
    "HubComponentLogs": {
        "type": "object",
        "required": ["service", "compose_service", "lines", "line_limit"],
        "properties": {
            "service": {"type": "string"},
            "compose_service": {"type": "string"},
            "lines": {"type": "array", "items": {"type": "string"}},
            "line_limit": {"type": "integer", "example": 10},
        },
    },
}


def convert(node):
    if isinstance(node, list):
        return [convert(item) for item in node]
    if not isinstance(node, dict):
        return node
    converted = {}
    for key, value in node.items():
        if key == "const":
            converted.setdefault("enum", [value])
            continue
        if key == "type" and isinstance(value, list):
            types = [item for item in value if item != "null"]
            if "null" in value:
                converted["nullable"] = True
            converted["type"] = types[0] if len(types) == 1 else types
            continue
        if key == "exclusiveMinimum" and isinstance(value, (int, float)):
            converted["minimum"] = value
            converted["exclusiveMinimum"] = True
            continue
        if key == "exclusiveMaximum" and isinstance(value, (int, float)):
            converted["maximum"] = value
            converted["exclusiveMaximum"] = True
            continue
        converted[key] = convert(value)
    if "anyOf" in converted:
        options = converted["anyOf"]
        if (
            len(options) == 2
            and any(option == {"type": "null"} for option in options)
            and all(isinstance(option, dict) for option in options)
        ):
            other = next(option for option in options if option != {"type": "null"})
            merged = {k: v for k, v in converted.items() if k != "anyOf"}
            merged.update(other)
            merged["nullable"] = True
            return merged
    return converted


def tag_for(path: str) -> str:
    for prefix, tag in TAGS_BY_PREFIX:
        if path == prefix or path.startswith(prefix + "/") or path.startswith(prefix + "{"):
            return tag
    return "Hub"


def error_response(code: str) -> dict:
    return {
        "description": ERROR_RESPONSES[code],
        "content": {
            "application/json": {
                "schema": {"$ref": "#/components/schemas/ErrorMessage"},
            }
        },
    }


def _yaml_scalar(value: str) -> str:
    if (
        value == ""
        or value.strip() != value
        or value[0] in "-&*?|>!%@`'\",[]{}"
        or any(token in value for token in (": ", " #", "{", "}", "[", "]", ","))
        or "#" in value
        or "\n" in value
        or value.lower() in {"true", "false", "null", "yes", "no", "on", "off"}
        or value.isdigit()
    ):
        return json.dumps(value)
    return value


def _yaml_key(key) -> str:
    if isinstance(key, str):
        if key.isdigit() or key[0] in "0123456789":
            return json.dumps(key)
        return _yaml_scalar(key)
    return str(key)


def dump_yaml(value, indent=0) -> str:
    prefix = "  " * indent
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, str):
        return _yaml_scalar(value)
    if isinstance(value, list):
        if not value:
            return "[]"
        lines = []
        for item in value:
            dumped = dump_yaml(item, indent + 1)
            if isinstance(item, (dict, list)):
                first, *rest = dumped.splitlines()
                lines.append(f"{prefix}- {first.lstrip()}")
                lines.extend(rest)
            else:
                lines.append(f"{prefix}- {dumped}")
        return "\n".join(lines)
    if isinstance(value, dict):
        if not value:
            return "{}"
        lines = []
        for key, item in value.items():
            dumped = dump_yaml(item, indent + 1)
            rendered_key = _yaml_key(key)
            if isinstance(item, (dict, list)) and item:
                lines.append(f"{prefix}{rendered_key}:")
                lines.append(
                    dumped
                    if dumped.startswith("  " * (indent + 1)) or dumped.startswith("- ")
                    else f"{'  ' * (indent + 1)}{dumped}"
                )
            else:
                lines.append(f"{prefix}{rendered_key}: {dumped}")
        return "\n".join(lines)
    return json.dumps(value)


def build() -> dict:
    schema = convert(create_app().openapi())
    schema["openapi"] = "3.0.3"
    schema["info"] = {
        "title": "dBmap Hub API",
        "version": "v1",
        "description": (
            "HTTPS API served by `hub-bootstrap` and published on the LAN through Nginx. "
            "Call `https://<hub-host>:<https-port>[<public-base-path>]/api/v1/...`. "
            "Local administrators and viewers send `Authorization: Bearer <token>`. "
            "The API checks the token, then asks OPA for the named action. "
            "`GET /health` and `POST /api/v1/provision` do not use admin/viewer bearer tokens or OPA. "
            "Device provisioning uses a one-time bootstrap token in the JSON body. "
            "`GET /api/v1/device/desired-firmware` and `GET /api/v1/device/ota-image` authenticate a separate "
            "device API token in the Hub database and do not call OPA. "
            "Authorization actions for human tokens are listed in docs/authorization.md."
        ),
        "contact": {"name": "dBmap Hub"},
    }
    schema["servers"] = [
        {
            "url": "https://hub.local:8443",
            "description": "Default local Hub URL through Nginx. Replace host and port from .env.",
        },
        {
            "url": "/",
            "description": "Same-origin browser calls, including an optional public base path.",
        },
    ]
    schema["tags"] = [
        {"name": "Health", "description": "Process liveness."},
        {"name": "Session", "description": "Local bearer-token role."},
        {"name": "Users", "description": "Internal user directory and GitHub identity links."},
        {"name": "Groups", "description": "Groups and membership."},
        {"name": "Devices", "description": "Viewer-readable device summaries."},
        {"name": "Nodes", "description": "Registration, desired state, ownership, and commands."},
        {"name": "Firmware", "description": "Catalog, flash artifacts, and compatibility."},
        {"name": "Observations", "description": "Persisted Ear observations."},
        {"name": "Hub", "description": "Local mode, component details, and container logs."},
        {"name": "Provisioning", "description": "Device bootstrap-token exchange."},
    ]
    schema["security"] = [{"bearerAuth": []}]
    components = schema.setdefault("components", {})
    components["securitySchemes"] = {
        "bearerAuth": {
            "type": "http",
            "scheme": "bearer",
            "bearerFormat": "opaque",
            "description": "Local `DBMAP_ADMIN_TOKEN` or `DBMAP_VIEWER_TOKEN`. OPA then decides the action.",
        },
        "deviceApiAuth": {
            "type": "http",
            "scheme": "bearer",
            "bearerFormat": "opaque",
            "description": "Device API token from provisioning or admin rotation. The Hub hashes it and does not call OPA.",
        },
    }
    components.setdefault("schemas", {}).update(EXTRA_SCHEMAS)

    for path, methods in schema["paths"].items():
        for method, operation in list(methods.items()):
            if method.startswith("x-"):
                continue
            key = (method.upper(), path)
            meta = OPERATIONS[key]
            operation["tags"] = [tag_for(path)]
            operation["operationId"] = meta["operationId"]
            operation["summary"] = meta["summary"]
            if "description" in meta:
                operation["description"] = meta["description"]
            operation["parameters"] = [
                parameter
                for parameter in operation.get("parameters") or []
                if not (parameter.get("in") == "header" and parameter.get("name", "").lower() == "authorization")
            ]
            if not operation["parameters"]:
                operation.pop("parameters", None)
            if key in PUBLIC:
                operation["security"] = []
            elif key in {
                ("GET", "/api/v1/device/desired-firmware"),
                ("GET", "/api/v1/device/ota-image"),
            }:
                operation["security"] = [{"deviceApiAuth": []}]
            else:
                operation["security"] = [{"bearerAuth": []}]

            responses = operation.setdefault("responses", {})
            success = "201" if "201" in responses else "204" if "204" in responses else "200"
            if "response" in meta and success in responses:
                schema_ref = {"$ref": f"#/components/schemas/{meta['response']}"}
                if meta.get("array"):
                    schema_ref = {"type": "array", "items": schema_ref}
                responses[success]["content"] = {"application/json": {"schema": schema_ref}}
            if meta.get("binary") and success in responses:
                responses[success]["content"] = {
                    meta["binary"]: {"schema": {"type": "string", "format": "binary"}}
                }
                for header, description in meta.get("headers", {}).items():
                    responses[success].setdefault("headers", {})[header] = {
                        "schema": {"type": "string"},
                        "description": description,
                    }
            for code in meta.get("errors", []):
                responses[code] = error_response(code)
            if "422" in responses and "422" not in meta.get("errors", []):
                responses.pop("422")
    return schema


def main() -> None:
    document = build()
    header = (
        "# dBmap Hub API — OpenAPI 3.0.3\n"
        "# Import this file into Swagger UI, Postman, Insomnia, Azure API Management,\n"
        "# or AWS API Gateway. Regenerated by scripts/export-hub-openapi.py.\n"
    )
    OUTPUT.write_text(header + dump_yaml(document) + "\n")
    print(f"Wrote {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
