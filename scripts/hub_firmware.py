#!/usr/bin/env python3
"""Inspect and upload firmware bundles in the dBmap Hub catalog."""

import argparse
import hashlib
import json
import re
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from _hub_cli import (
    add_hub_connection_arguments,
    load_project_env,
    print_help_without_arguments,
    validate_hub_connection,
)

MAX_BUNDLE_BYTES = 128 * 1024 * 1024
MAX_METADATA_BYTES = 64 * 1024
MAX_MANIFEST_BYTES = MAX_METADATA_BYTES
FIRMWARE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


class HubFirmwareError(RuntimeError):
    pass


def create_metadata(bundle_path: Path, build_date_utc: str) -> Path:
    firmware_id = bundle_path.stem
    if not FIRMWARE_ID_PATTERN.fullmatch(firmware_id):
        raise HubFirmwareError(
            "Firmware ZIP filename must use a safe 1-64 character firmware ID."
        )
    if (
        not bundle_path.is_file()
        or not 0 < bundle_path.stat().st_size <= MAX_BUNDLE_BYTES
    ):
        raise HubFirmwareError("Firmware ZIP must be between 1 byte and 128 MiB.")
    try:
        build_date = datetime.fromisoformat(build_date_utc.replace("Z", "+00:00"))
    except ValueError as exc:
        raise HubFirmwareError(
            "--build-date-utc must be an ISO-8601 timestamp with a timezone."
        ) from exc
    if build_date.tzinfo is None:
        raise HubFirmwareError("--build-date-utc must include a timezone.")
    try:
        with zipfile.ZipFile(bundle_path) as archive:
            manifest_info = archive.getinfo("manifest.json")
            if manifest_info.file_size > MAX_MANIFEST_BYTES:
                raise HubFirmwareError("Firmware manifest exceeds the 64 KiB limit.")
            manifest = json.loads(archive.read("manifest.json"))
    except (
        OSError,
        UnicodeDecodeError,
        zipfile.BadZipFile,
        KeyError,
        json.JSONDecodeError,
    ) as exc:
        raise HubFirmwareError(
            "Firmware ZIP must contain a valid manifest.json."
        ) from exc
    if not isinstance(manifest, dict):
        raise HubFirmwareError("Firmware manifest must be a JSON object.")
    required_strings = (
        "target",
        "node_type",
        "chip",
        "hardware_revision",
        "firmware_version",
        "esp_idf_version",
    )
    if any(
        not isinstance(manifest.get(field), str) or not manifest[field].strip()
        for field in required_strings
    ):
        raise HubFirmwareError("Firmware manifest is missing required build metadata.")
    configuration_version = manifest.get("configuration_version")
    if (
        not isinstance(configuration_version, int)
        or isinstance(configuration_version, bool)
        or configuration_version < 0
    ):
        raise HubFirmwareError(
            "Firmware manifest has an invalid configuration_version."
        )
    metadata = {
        "firmware_id": firmware_id,
        "build_date_utc": build_date.astimezone(timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
        "target": manifest["target"],
        "node_type": manifest["node_type"],
        "platform": manifest["chip"],
        "hardware_revision": manifest["hardware_revision"],
        "firmware_version": manifest["firmware_version"],
        "esp_idf_version": manifest["esp_idf_version"],
        "configuration_version": configuration_version,
        "artifact_sha256": hashlib.sha256(bundle_path.read_bytes()).hexdigest(),
    }
    metadata_path = bundle_path.with_suffix(".json")
    try:
        with metadata_path.open("x", encoding="utf-8") as output:
            output.write(json.dumps(metadata, indent=2) + "\n")
        metadata_path.chmod(0o600)
    except FileExistsError as exc:
        raise HubFirmwareError(
            f"Metadata sidecar already exists: {metadata_path}"
        ) from exc
    except OSError as exc:
        raise HubFirmwareError(f"Could not write metadata sidecar: {exc}") from exc
    return metadata_path


def _request(
    base_url: str,
    ca_file: str,
    admin_token: str,
    method: str,
    path: str,
    data: bytes | None = None,
    content_type: str = "application/json",
) -> tuple[int, Any]:
    headers = {"Authorization": f"Bearer {admin_token}"}
    if data is not None:
        headers["Content-Type"] = content_type
    request = urllib.request.Request(
        f"{base_url}{path}",
        data=data,
        headers=headers,
        method=method,
    )
    context = ssl.create_default_context(cafile=ca_file)
    try:
        with urllib.request.urlopen(request, context=context, timeout=30) as response:
            body = response.read()
            try:
                return response.status, json.loads(body) if body else None
            except (UnicodeDecodeError, json.JSONDecodeError):
                return response.status, body
    except urllib.error.HTTPError as exc:
        body = exc.read(4096)
        try:
            detail = json.loads(body).get("detail", body.decode(errors="replace"))
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            detail = body.decode(errors="replace")
        return exc.code, {"detail": detail}
    except urllib.error.URLError as exc:
        raise HubFirmwareError(f"could not reach the Hub API: {exc.reason}") from exc


def _multipart(
    bundle: bytes, metadata: bytes, firmware_name: str, metadata_name: str
) -> tuple[bytes, str]:
    boundary = f"----dBmapFirmware{uuid.uuid4().hex}"
    chunks = []
    for field, filename, content_type, content in (
        ("firmware", firmware_name, "application/zip", bundle),
        ("metadata", metadata_name, "application/json", metadata),
    ):
        chunks.extend(
            [
                f"--{boundary}\r\n".encode(),
                (
                    f'Content-Disposition: form-data; name="{field}"; '
                    f'filename="{filename}"\r\n'
                ).encode(),
                f"Content-Type: {content_type}\r\n\r\n".encode(),
                content,
                b"\r\n",
            ]
        )
    chunks.append(f"--{boundary}--\r\n".encode())
    return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


def upload(args: argparse.Namespace, base_url: str, admin_token: str) -> dict[str, Any]:
    if not Path(args.ca).is_file():
        raise HubFirmwareError(f"CA file does not exist: {args.ca}")
    if len(admin_token) < 32:
        raise HubFirmwareError(
            "DBMAP_ADMIN_TOKEN must be loaded from the private .env file."
        )
    bundle_path = Path(args.bundle)
    metadata_path = Path(args.metadata)
    if not bundle_path.is_file() or not metadata_path.is_file():
        raise HubFirmwareError("Firmware ZIP and metadata JSON files must both exist.")
    if not 0 < bundle_path.stat().st_size <= MAX_BUNDLE_BYTES:
        raise HubFirmwareError("Firmware ZIP must be between 1 byte and 128 MiB.")
    if not 0 < metadata_path.stat().st_size <= MAX_METADATA_BYTES:
        raise HubFirmwareError("Metadata JSON must be between 1 byte and 64 KiB.")
    try:
        bundle = bundle_path.read_bytes()
        metadata_bytes = metadata_path.read_bytes()
        metadata = json.loads(metadata_bytes)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise HubFirmwareError(
            f"could not read a valid firmware ZIP and metadata JSON: {exc}"
        ) from exc
    if not isinstance(metadata, dict) or not isinstance(
        metadata.get("firmware_id"), str
    ):
        raise HubFirmwareError("metadata JSON must contain a string firmware_id.")
    firmware_id = metadata["firmware_id"]
    if (
        bundle_path.name != f"{firmware_id}.zip"
        or metadata_path.name != f"{firmware_id}.json"
    ):
        raise HubFirmwareError(
            "Upload filenames must be <firmware_id>.zip and <firmware_id>.json."
        )
    digest = hashlib.sha256(bundle).hexdigest()
    if metadata.get("artifact_sha256") != digest:
        raise HubFirmwareError(
            "metadata artifact_sha256 does not match the selected ZIP."
        )
    encoded_id = urllib.parse.quote(firmware_id, safe="")
    status, current = _request(
        base_url,
        args.ca,
        admin_token,
        "GET",
        f"/api/v1/firmware/catalog/{encoded_id}",
    )
    if status == 200:
        raise HubFirmwareError(
            f"firmware ID {firmware_id} is already present in the catalog."
        )
    if status != 404:
        detail = (
            current.get("detail", current) if isinstance(current, dict) else current
        )
        raise HubFirmwareError(
            f"firmware catalog preflight failed (HTTP {status}): {detail}"
        )
    payload, content_type = _multipart(
        bundle,
        metadata_bytes,
        bundle_path.name,
        metadata_path.name,
    )
    status, result = _request(
        base_url,
        args.ca,
        admin_token,
        "POST",
        "/api/v1/firmware/catalog",
        payload,
        content_type,
    )
    if status != 201 or not isinstance(result, dict):
        detail = result.get("detail", result) if isinstance(result, dict) else result
        raise HubFirmwareError(f"firmware upload failed (HTTP {status}): {detail}")
    return result


def list_firmware(
    ca_file: str, admin_token: str, base_url: str
) -> list[dict[str, Any]]:
    if not Path(ca_file).is_file():
        raise HubFirmwareError(f"CA file does not exist: {ca_file}")
    if len(admin_token) < 32:
        raise HubFirmwareError(
            "DBMAP_ADMIN_TOKEN must be loaded from the private .env file."
        )
    status, result = _request(
        base_url,
        ca_file,
        admin_token,
        "GET",
        "/api/v1/firmware/catalog",
    )
    if (
        status != 200
        or not isinstance(result, list)
        or not all(isinstance(entry, dict) for entry in result)
    ):
        detail = result.get("detail", result) if isinstance(result, dict) else result
        raise HubFirmwareError(
            f"firmware catalog request failed (HTTP {status}): {detail}"
        )
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="List and upload firmware bundles in the dBmap Hub catalog."
    )
    add_hub_connection_arguments(parser)
    commands = parser.add_subparsers(dest="command")
    commands.add_parser(
        "list", help="List registered firmware and unregistered candidates"
    )
    upload_parser = commands.add_parser(
        "upload", help="Upload an ID-matched ZIP and JSON pair"
    )
    upload_parser.add_argument("bundle", help="Firmware bundle named <firmware_id>.zip")
    upload_parser.add_argument("metadata", help="Sidecar named <firmware_id>.json")
    metadata_parser = commands.add_parser(
        "metadata", help="Create a matching JSON sidecar from a firmware ZIP"
    )
    metadata_parser.add_argument(
        "bundle", help="Firmware bundle named <firmware_id>.zip"
    )
    metadata_parser.add_argument(
        "--build-date-utc",
        required=True,
        help="The actual build time as an ISO-8601 timestamp including a timezone",
    )
    return parser


def main() -> int:
    try:
        load_project_env()
    except HubFirmwareError as exc:
        print(f"Firmware configuration failed: {exc}", file=sys.stderr)
        return 1
    parser = build_parser()
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    if args.command == "metadata":
        try:
            metadata_path = create_metadata(Path(args.bundle), args.build_date_utc)
        except HubFirmwareError as exc:
            print(f"Firmware metadata creation failed: {exc}", file=sys.stderr)
            return 1
        print(f"Created metadata sidecar: {metadata_path}")
        return 0
    base_url, admin_token = validate_hub_connection(args, parser)
    if args.command == "list":
        try:
            entries = list_firmware(args.ca, admin_token, base_url)
        except HubFirmwareError as exc:
            print(f"Firmware catalog listing failed: {exc}", file=sys.stderr)
            return 1
        if not entries:
            print("No firmware catalog entries.")
            return 0
        print("STATUS\tFIRMWARE ID\tTARGET\tVERSION\tPLATFORM\tHARDWARE REVISION")
        for entry in entries:
            print(
                f"{entry.get('status', 'unknown')}\t{entry.get('firmware_id', 'unknown')}\t"
                f"{entry.get('target', 'unknown')}\t{entry.get('firmware_version', 'unknown')}\t"
                f"{entry.get('platform', 'unknown')}\t{entry.get('hardware_revision', 'unknown')}"
            )
        return 0
    try:
        result = upload(args, base_url, admin_token)
    except HubFirmwareError as exc:
        print(f"Firmware upload failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"Uploaded candidate {result['firmware_id']} "
        f"({result['platform']} {result['firmware_version']}); "
        "an administrator must verify and promote it before it is available for deployment."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
