import hashlib
import io
import json
import os
import re
import uuid
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from sqlalchemy.orm import Session

from app.models import FirmwareCatalogEntry, utcnow

MAX_BUNDLE_BYTES = 128 * 1024 * 1024
MAX_METADATA_BYTES = 64 * 1024
FIRMWARE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
EXPECTED_COMPONENTS = {
    "bootloader.bin": "0x1000",
    "partition-table.bin": "0x8000",
    "esp-output.bin": "0x10000",
}
ESP_WEB_TOOLS_CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32c2": "ESP32-C2",
    "esp32c3": "ESP32-C3",
    "esp32c5": "ESP32-C5",
    "esp32c6": "ESP32-C6",
    "esp32c61": "ESP32-C61",
    "esp32h2": "ESP32-H2",
    "esp32p4": "ESP32-P4",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
}
MAX_COMPONENT_BYTES = 32 * 1024 * 1024
METADATA_FIELDS = {
    "firmware_id",
    "build_date_utc",
    "target",
    "node_type",
    "platform",
    "hardware_revision",
    "firmware_version",
    "esp_idf_version",
    "configuration_version",
    "artifact_sha256",
}


class InvalidFirmwareBundle(ValueError):
    pass


class FirmwareAlreadyExists(Exception):
    pass


class FirmwareNotFound(Exception):
    pass


class FirmwareNotCompatible(Exception):
    pass


class FirmwareArtifactIntegrityError(Exception):
    pass


class FirmwarePlatformUnsupported(Exception):
    pass


class FirmwarePlatformUnverified(Exception):
    pass


def validate_upload(
    firmware_id: str,
    bundle: bytes,
    metadata_bytes: bytes,
) -> tuple[dict, str]:
    if not FIRMWARE_ID_PATTERN.fullmatch(firmware_id):
        raise InvalidFirmwareBundle("firmware_id must be 1-64 safe filename characters.")
    if not bundle or len(bundle) > MAX_BUNDLE_BYTES:
        raise InvalidFirmwareBundle("Firmware ZIP must be between 1 byte and 128 MiB.")
    if not metadata_bytes or len(metadata_bytes) > MAX_METADATA_BYTES:
        raise InvalidFirmwareBundle("Metadata JSON must be between 1 byte and 64 KiB.")
    try:
        metadata = json.loads(metadata_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise InvalidFirmwareBundle("Metadata sidecar must contain valid UTF-8 JSON.") from exc
    if not isinstance(metadata, dict) or set(metadata) != METADATA_FIELDS:
        raise InvalidFirmwareBundle(
            "Metadata must contain exactly firmware_id, build_date_utc, target, platform, "
            "node_type, hardware_revision, firmware_version, esp_idf_version, configuration_version, "
            "and artifact_sha256."
        )
    if metadata["firmware_id"] != firmware_id:
        raise InvalidFirmwareBundle("Filename ID must match metadata firmware_id.")
    for field in (
        "target",
        "node_type",
        "platform",
        "hardware_revision",
        "firmware_version",
        "esp_idf_version",
    ):
        if not isinstance(metadata[field], str) or not metadata[field].strip() or len(metadata[field]) > 128:
            raise InvalidFirmwareBundle(f"Metadata field {field} must be a non-empty string of at most 128 characters.")
    if (
        not isinstance(metadata["configuration_version"], int)
        or isinstance(metadata["configuration_version"], bool)
        or metadata["configuration_version"] < 0
    ):
        raise InvalidFirmwareBundle("configuration_version must be a non-negative integer.")
    try:
        build_date = datetime.fromisoformat(metadata["build_date_utc"].replace("Z", "+00:00"))
    except (AttributeError, TypeError, ValueError) as exc:
        raise InvalidFirmwareBundle("build_date_utc must be an ISO-8601 timestamp with a timezone.") from exc
    if build_date.tzinfo is None:
        raise InvalidFirmwareBundle("build_date_utc must include a timezone.")
    actual_digest = hashlib.sha256(bundle).hexdigest()
    if metadata["artifact_sha256"] != actual_digest:
        raise InvalidFirmwareBundle("artifact_sha256 does not match the uploaded firmware ZIP.")
    try:
        with zipfile.ZipFile(io.BytesIO(bundle)) as archive:
            names = archive.namelist()
            if len(names) != 4 or set(names) != {
                *EXPECTED_COMPONENTS,
                "manifest.json",
            }:
                raise InvalidFirmwareBundle("Firmware ZIP must contain the three supported components and manifest.json only.")
            if archive.getinfo("manifest.json").file_size > MAX_METADATA_BYTES:
                raise InvalidFirmwareBundle("Firmware manifest exceeds the 64 KiB limit.")
            if any(archive.getinfo(name).file_size > MAX_COMPONENT_BYTES for name in EXPECTED_COMPONENTS):
                raise InvalidFirmwareBundle("Firmware component exceeds the 32 MiB limit.")
            if archive.testzip() is not None:
                raise InvalidFirmwareBundle("Firmware ZIP failed its CRC integrity check.")
            manifest = json.loads(archive.read("manifest.json"))
            if not isinstance(manifest, dict):
                raise InvalidFirmwareBundle("Firmware ZIP manifest must be a JSON object.")
            _validate_manifest(metadata, manifest, archive)
    except (OSError, zipfile.BadZipFile, KeyError, json.JSONDecodeError) as exc:
        raise InvalidFirmwareBundle("Firmware upload is not a valid firmware ZIP bundle.") from exc
    metadata["build_date_utc"] = build_date.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")
    return metadata, actual_digest


def _validate_manifest(metadata: dict, manifest: dict, archive: zipfile.ZipFile) -> None:
    manifest_fields = {
        "target": "target",
        "node_type": "node_type",
        "platform": "chip",
        "hardware_revision": "hardware_revision",
        "firmware_version": "firmware_version",
        "esp_idf_version": "esp_idf_version",
        "configuration_version": "configuration_version",
    }
    for metadata_field, manifest_field in manifest_fields.items():
        if manifest.get(manifest_field) != metadata[metadata_field]:
            raise InvalidFirmwareBundle(
                f"Metadata field {metadata_field} does not match firmware manifest."
            )
    if manifest.get("nvs_preserved") is not True:
        raise InvalidFirmwareBundle("Firmware manifest must confirm that NVS is preserved.")
    components = manifest.get("flash_components")
    if not isinstance(components, list) or len(components) != len(EXPECTED_COMPONENTS):
        raise InvalidFirmwareBundle("Firmware manifest has an invalid flash component list.")
    observed = set()
    for component in components:
        if not isinstance(component, dict):
            raise InvalidFirmwareBundle("Firmware manifest contains an invalid flash component.")
        name = component.get("name")
        if name not in EXPECTED_COMPONENTS or name in observed:
            raise InvalidFirmwareBundle("Firmware manifest contains an unsupported or duplicate component.")
        observed.add(name)
        if component.get("offset") != EXPECTED_COMPONENTS[name]:
            raise InvalidFirmwareBundle(f"Unexpected flash offset for {name}.")
        if archive.getinfo(name).file_size > MAX_COMPONENT_BYTES:
            raise InvalidFirmwareBundle(f"Firmware component {name} exceeds the 32 MiB limit.")
        content = archive.read(name)
        if (
            component.get("size_bytes") != len(content)
            or component.get("sha256") != hashlib.sha256(content).hexdigest()
        ):
            raise InvalidFirmwareBundle(f"Firmware component integrity check failed for {name}.")
    if observed != set(EXPECTED_COMPONENTS):
        raise InvalidFirmwareBundle("Firmware manifest is missing a supported flash component.")


def create_candidate(
    db: Session,
    catalog_dir: Path,
    firmware_id: str,
    bundle: bytes,
    metadata_bytes: bytes,
) -> FirmwareCatalogEntry:
    metadata, digest = validate_upload(firmware_id, bundle, metadata_bytes)
    if db.get(FirmwareCatalogEntry, firmware_id) is not None:
        raise FirmwareAlreadyExists(f"Firmware ID {firmware_id} already exists in the catalog.")
    catalog_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    destination = catalog_dir / f"{firmware_id}.zip"
    temporary = catalog_dir / f".{firmware_id}.{uuid.uuid4().hex}.upload"
    artifact_linked = False
    try:
        if destination.exists():
            raise FirmwareAlreadyExists(f"Firmware ID {firmware_id} already has a stored artifact.")
        descriptor = temporary.open("xb")
        with descriptor:
            descriptor.write(bundle)
        temporary.chmod(0o600)
        try:
            os.link(temporary, destination)
        except FileExistsError as exc:
            raise FirmwareAlreadyExists(
                f"Firmware ID {firmware_id} already has a stored artifact."
            ) from exc
        artifact_linked = True
        temporary.unlink()
        entry = FirmwareCatalogEntry(
            firmware_id=firmware_id,
            status="candidate",
            metadata_json=json.dumps(metadata, sort_keys=True),
            artifact_sha256=digest,
        )
        db.add(entry)
        db.commit()
        artifact_linked = False
        db.refresh(entry)
        return entry
    except Exception:
        db.rollback()
        if artifact_linked:
            destination.unlink(missing_ok=True)
        temporary.unlink(missing_ok=True)
        raise


def list_entries(db: Session, catalog_dir: Path, include_candidates: bool) -> list[dict]:
    query = db.query(FirmwareCatalogEntry)
    if not include_candidates:
        query = query.filter(FirmwareCatalogEntry.status == "registered")
    entries = query.order_by(FirmwareCatalogEntry.created_at.desc()).all()
    return [public_entry(entry, catalog_dir) for entry in entries]


def list_compatible_entries(
    db: Session,
    catalog_dir: Path,
    node_type: str,
    hardware_revision: str,
) -> list[dict]:
    if not node_type or node_type == "unknown" or not hardware_revision or hardware_revision == "unknown":
        return []
    registered = list_entries(db, catalog_dir, include_candidates=False)
    matches = [
        entry
        for entry in registered
        if entry["node_type"] == node_type
        and entry["hardware_revision"] == hardware_revision
    ]
    return [
        {
            **entry,
            "compatibility": {
                "node_type": "matched",
                "hardware_revision": "matched",
                "hardware_reference": "registered_device_record",
                "platform": "unknown",
            },
            "requires_platform_review": True,
        }
        for entry in matches
    ]


def validate_selection(
    db: Session,
    catalog_dir: Path,
    device_id: str,
    firmware_id: str,
    node_type: str,
    hardware_revision: str,
    acknowledge_unverified_platform: bool,
) -> dict:
    entry = get_entry(db, catalog_dir, firmware_id, include_candidate=False)
    metadata = json.loads(entry.metadata_json)
    if not node_type or node_type == "unknown" or not hardware_revision or hardware_revision == "unknown":
        raise FirmwareNotCompatible(
            "The registered device is missing a known node type or hardware revision."
        )
    if metadata["node_type"] != node_type:
        raise FirmwareNotCompatible("Firmware node_type does not match the registered device.")
    if metadata["hardware_revision"] != hardware_revision:
        raise FirmwareNotCompatible(
            "Firmware hardware_revision does not match the registered device."
        )
    if not acknowledge_unverified_platform:
        raise FirmwarePlatformUnverified(
            "The registered device has no verified chip platform; an administrator must "
            "explicitly acknowledge this compatibility limitation."
        )
    return {
        **public_entry(entry, catalog_dir),
        "device_id": device_id,
        "compatibility": {
            "node_type": "matched",
            "hardware_revision": "matched",
            "hardware_reference": "registered_device_record",
            "platform": "unknown",
        },
        "acknowledged_unverified_platform": True,
        "selection_persisted": False,
        "flashing_started": False,
    }


def get_entry(
    db: Session,
    catalog_dir: Path,
    firmware_id: str,
    include_candidate: bool,
) -> FirmwareCatalogEntry:
    if not FIRMWARE_ID_PATTERN.fullmatch(firmware_id):
        raise FirmwareNotFound
    entry = db.get(FirmwareCatalogEntry, firmware_id)
    if entry is None or (entry.status != "registered" and not include_candidate):
        raise FirmwareNotFound
    artifact = catalog_dir / f"{firmware_id}.zip"
    if not artifact.is_file():
        raise FirmwareNotFound
    return entry


def verify_artifact_integrity(entry: FirmwareCatalogEntry, catalog_dir: Path) -> None:
    artifact = catalog_dir / f"{entry.firmware_id}.zip"
    if not artifact.is_file():
        raise FirmwareNotFound
    with artifact.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != entry.artifact_sha256:
        raise FirmwareArtifactIntegrityError(
            f"Stored firmware artifact {entry.firmware_id} failed its SHA-256 integrity check."
        )


def get_flash_manifest(entry: FirmwareCatalogEntry, catalog_dir: Path) -> dict:
    metadata, manifest, _ = _read_validated_bundle(entry, catalog_dir)
    platform_key = metadata["platform"].lower().replace("-", "")
    chip_family = ESP_WEB_TOOLS_CHIP_FAMILIES.get(platform_key)
    if chip_family is None:
        raise FirmwarePlatformUnsupported(
            f"ESP Web Tools does not support firmware platform {metadata['platform']}."
        )
    return {
        "name": f"dBmap {metadata['node_type']}",
        "version": metadata["firmware_version"],
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": chip_family,
                "parts": [
                    {"path": component["name"], "offset": int(component["offset"], 16)}
                    for component in manifest["flash_components"]
                ],
            }
        ],
    }


def get_flash_image(
    entry: FirmwareCatalogEntry,
    catalog_dir: Path,
    component_name: str,
) -> tuple[bytes, str]:
    if component_name not in EXPECTED_COMPONENTS:
        raise FirmwareNotFound
    _, manifest, images = _read_validated_bundle(entry, catalog_dir)
    component = next(
        item for item in manifest["flash_components"] if item["name"] == component_name
    )
    return images[component_name], component["sha256"]


def _read_validated_bundle(
    entry: FirmwareCatalogEntry,
    catalog_dir: Path,
) -> tuple[dict, dict, dict[str, bytes]]:
    artifact = catalog_dir / f"{entry.firmware_id}.zip"
    if not artifact.is_file():
        raise FirmwareNotFound
    if artifact.stat().st_size > MAX_BUNDLE_BYTES:
        raise FirmwareArtifactIntegrityError(
            f"Stored firmware artifact {entry.firmware_id} exceeds the maximum bundle size."
        )
    bundle = artifact.read_bytes()
    try:
        metadata, digest = validate_upload(
            entry.firmware_id,
            bundle,
            entry.metadata_json.encode("utf-8"),
        )
    except InvalidFirmwareBundle as exc:
        raise FirmwareArtifactIntegrityError(
            f"Stored firmware artifact {entry.firmware_id} failed validation."
        ) from exc
    if digest != entry.artifact_sha256:
        raise FirmwareArtifactIntegrityError(
            f"Stored firmware artifact {entry.firmware_id} does not match its catalog checksum."
        )
    with zipfile.ZipFile(io.BytesIO(bundle)) as archive:
        manifest = json.loads(archive.read("manifest.json"))
        images = {name: archive.read(name) for name in EXPECTED_COMPONENTS}
    return metadata, manifest, images


def promote_entry(db: Session, catalog_dir: Path, firmware_id: str) -> dict:
    entry = get_entry(db, catalog_dir, firmware_id, include_candidate=True)
    entry.status = "registered"
    entry.registered_at = utcnow()
    db.commit()
    db.refresh(entry)
    return public_entry(entry, catalog_dir)


def delete_entry(db: Session, catalog_dir: Path, firmware_id: str) -> None:
    entry = get_entry(db, catalog_dir, firmware_id, include_candidate=True)
    artifact = catalog_dir / f"{firmware_id}.zip"
    entry.metadata_json = ""
    db.delete(entry)
    db.commit()
    artifact.unlink(missing_ok=True)


def public_entry(entry: FirmwareCatalogEntry, catalog_dir: Path) -> dict:
    artifact = catalog_dir / f"{entry.firmware_id}.zip"
    try:
        metadata = json.loads(entry.metadata_json)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Stored metadata for firmware {entry.firmware_id} is invalid.") from exc
    return {
        **metadata,
        "status": entry.status,
        "uploaded_at": entry.created_at.isoformat(),
        "registered_at": entry.registered_at.isoformat() if entry.registered_at else None,
        "artifact_size_bytes": artifact.stat().st_size if artifact.is_file() else None,
    }
