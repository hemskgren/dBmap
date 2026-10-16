from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import shutil
import subprocess
import threading
import time
import uuid
import zipfile
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ARTIFACTS = Path(os.environ.get("DBMAP_FIRMWARE_ARTIFACTS", "/artifacts"))
JOBS = ARTIFACTS / ".jobs"
FIRMWARE_ROOT = Path(os.environ.get("DBMAP_FIRMWARE_ROOT", "/firmware"))
FIRMWARE_SOURCE = Path(os.environ.get("DBMAP_FIRMWARE_SOURCE", str(FIRMWARE_ROOT / "esp-output")))
HUB_CA_FILE = Path(os.environ.get("DBMAP_HUB_CA_FILE", "/certs/ca.crt"))
ADMIN_TOKEN = os.environ.get("DBMAP_ADMIN_TOKEN", "")
NTP_SERVER = os.environ.get("DBMAP_NTP_SERVER", "se.pool.ntp.org").strip()
BUILD_TIMEOUT_SECONDS = 1800
FIRMWARE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
FIRMWARE_VERSION_PATTERN = re.compile(
    r"(?P<major>0|[1-9][0-9]*)\.(?P<minor>0|[1-9][0-9]*)\.(?P<patch>0|[1-9][0-9]*)(?:[-+][0-9A-Za-z.-]+)?\Z"
)
TARGETS = {
    "output-esp32": {
        "project": FIRMWARE_SOURCE,
        "components": FIRMWARE_ROOT / "components",
        "chip": "esp32",
        "node_type": "output",
        "hardware_revision": "generic",
        "idf_version": "v5.4.4",
        "flash_components": {
            "0x1000": "bootloader.bin",
            "0x8000": "partition-table.bin",
            "0x10000": "esp-output.bin",
        },
    },
}
SECRET_CONFIG = (
    "CONFIG_DBMAP_WIFI_SSID",
    "CONFIG_DBMAP_WIFI_PASSWORD",
    "CONFIG_DBMAP_BOOTSTRAP_TOKEN",
)

lock = threading.Lock()
active_build: str | None = None


def _persist(job: dict) -> None:
    JOBS.mkdir(mode=0o700, parents=True, exist_ok=True)
    destination = JOBS / f"{job['id']}.json"
    temporary = destination.with_suffix(".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as state_file:
        state_file.write(json.dumps(job))
    temporary.replace(destination)


def _load_jobs(recover_interrupted: bool = False) -> list[dict]:
    JOBS.mkdir(mode=0o700, parents=True, exist_ok=True)
    jobs = []
    for path in sorted(JOBS.glob("*.json"), key=lambda item: item.stat().st_mtime, reverse=True):
        try:
            job = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if recover_interrupted and job.get("status") in {"queued", "running"}:
            job.update(
                status="failed",
                stage="failed",
                error="Builder restarted before this build completed; retry Step 4.",
                finished_at=time.time(),
            )
            _persist(job)
        jobs.append(job)
    return jobs


def _tail_log(path: Path, line_count: int = 40) -> str:
    try:
        with path.open(encoding="utf-8", errors="replace") as log_file:
            return "".join(deque(log_file, maxlen=line_count))
    except OSError:
        return ""


def _append_log(path: Path, message: str) -> None:
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        with os.fdopen(descriptor, "a", encoding="utf-8") as log_file:
            log_file.write(f"\n{message}\n")
    except OSError:
        pass


def _run_stage(command: list[str], cwd: Path, log_path: Path) -> tuple[int, str]:
    log_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with os.fdopen(descriptor, "a", encoding="utf-8") as log_file:
        log_file.write(f"\n$ {' '.join(command)}\n")
        log_file.flush()
        result = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=BUILD_TIMEOUT_SECONDS,
            env=os.environ.copy(),
        )
    return result.returncode, _tail_log(log_path)


def _artifact_entries(build_dir: Path, expected_components: dict[str, str]) -> list[dict]:
    args_file = build_dir / "flasher_args.json"
    args = json.loads(args_file.read_text(encoding="utf-8"))
    flash_files = args.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError("ESP-IDF did not provide a flash manifest.")
    expected = expected_components
    entries = []
    for offset, source in flash_files.items():
        if expected.get(offset) is None:
            raise RuntimeError(f"Unexpected ESP-IDF flash offset: {offset}")
        source_path = Path(source)
        if not source_path.is_absolute():
            source_path = build_dir / source_path
        source_path = source_path.resolve(strict=True)
        if build_dir.resolve() not in source_path.parents:
            raise RuntimeError("ESP-IDF flash manifest references a file outside the build directory.")
        if source_path.name != expected[offset]:
            raise RuntimeError(f"Unsupported flash component in ESP-IDF output: {source_path.name}")
        if not isinstance(offset, str) or not offset.startswith("0x"):
            raise RuntimeError("ESP-IDF returned an invalid flash offset.")
        data = source_path.read_bytes()
        entries.append(
            {
                "name": source_path.name,
                "offset": offset,
                "size_bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "path": source_path,
            }
        )
    if len(entries) != len(expected) or {entry["name"] for entry in entries} != set(expected.values()):
        raise RuntimeError("ESP-IDF output is missing one or more required flash components.")
    return sorted(entries, key=lambda entry: int(entry["offset"], 16))


def _clear_device_secrets(sdkconfig: Path) -> None:
    existing = sdkconfig.read_text(encoding="utf-8").splitlines()
    values = {name: f'{name}=""' for name in SECRET_CONFIG}
    output = []
    seen = set()
    for line in existing:
        key = line.split("=", 1)[0] if "=" in line else ""
        if key in values:
            output.append(values[key])
            seen.add(key)
        elif any(line == f"# {name} is not set" for name in SECRET_CONFIG):
            key = line[2:].removesuffix(" is not set")
            output.append(values[key])
            seen.add(key)
        else:
            output.append(line)
    for name, value in values.items():
        if name not in seen:
            output.append(value)
    sdkconfig.write_text("\n".join(output) + "\n", encoding="utf-8")


def _install_hub_ca(project_dir: Path) -> None:
    if not HUB_CA_FILE.is_file() or HUB_CA_FILE.stat().st_size == 0:
        raise RuntimeError(
            f"Hub CA certificate is missing or empty at {HUB_CA_FILE}; "
            "mount mqtt/certs into firmware-builder."
        )
    destination = project_dir / "main" / "root_ca.pem"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(HUB_CA_FILE, destination)


def _sdkconfig_string(sdkconfig: Path, name: str) -> str:
    prefix = f"{name}="
    for line in sdkconfig.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            value = json.loads(line[len(prefix) :])
            if isinstance(value, str):
                return value
    raise RuntimeError(f"Missing firmware version setting {name}.")


def _set_sdkconfig_string(sdkconfig: Path, name: str, value: str) -> None:
    prefix = f"{name}="
    lines = sdkconfig.read_text(encoding="utf-8").splitlines()
    matches = 0
    for index, line in enumerate(lines):
        if line.startswith(prefix):
            lines[index] = f"{name}={json.dumps(value)}"
            matches += 1
    if matches != 1:
        raise RuntimeError(f"Expected one firmware setting {name}; found {matches}.")
    sdkconfig.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _version_tuple(version: str) -> tuple[int, int, int] | None:
    match = FIRMWARE_VERSION_PATTERN.fullmatch(version)
    if match is None:
        return None
    return tuple(int(match.group(part)) for part in ("major", "minor", "patch"))


def _next_firmware_version(base_version: str) -> str:
    base = _version_tuple(base_version)
    if base is None or "-" in base_version or "+" in base_version:
        raise RuntimeError("Configured firmware version must be a numeric MAJOR.MINOR.PATCH version.")

    used_versions = []
    for job_path in JOBS.glob("*.json"):
        try:
            job = json.loads(job_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        version = job.get("firmware_version")
        if isinstance(version, str):
            parsed = _version_tuple(version)
            if parsed is not None:
                used_versions.append(parsed)

    if not used_versions or base > max(used_versions):
        next_version = base
    else:
        major, minor, patch = max(used_versions)
        next_version = (major, minor, patch + 1)
    return ".".join(str(part) for part in next_version)


def _release_firmware_id(manifest: dict, artifact_sha256: str) -> str:
    if not re.fullmatch(r"[0-9a-f]{64}", artifact_sha256):
        raise ValueError("Build artifact SHA-256 is invalid.")
    hardware_revision = manifest.get("hardware_revision", "unknown")
    firmware_id = (
        f"dbmap-{manifest['target']}-{hardware_revision}-{manifest['firmware_version']}-"
        f"{artifact_sha256[:8]}"
    )
    if not FIRMWARE_ID_PATTERN.fullmatch(firmware_id):
        raise ValueError("Build target and version do not form a safe firmware ID.")
    return firmware_id


def _check_toolchain(target: str, work_dir: Path, log_path: Path) -> None:
    code, output = _run_stage(["idf.py", "--version"], work_dir, log_path)
    if code != 0 or TARGETS[target]["idf_version"] not in output:
        raise RuntimeError("The pinned ESP-IDF toolchain check failed.\n" + output)


def _build(job_id: str, target: str) -> None:
    global active_build
    job_path = JOBS / f"{job_id}.json"
    work_root = ARTIFACTS / f".work-{job_id}"
    target_config = TARGETS.get(target)
    work_dir = work_root / target
    artifact_dir = ARTIFACTS / job_id
    log_path = JOBS / f"{job_id}.log"
    job = json.loads(job_path.read_text(encoding="utf-8"))
    try:
        descriptor = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.close(descriptor)
        log_path.chmod(0o600)
        log_path.write_text("dBmap generic firmware build log\n", encoding="utf-8")
        job.update(status="running", stage="validating", started_at=time.time())
        _persist(job)
        if target_config is None:
            raise RuntimeError("Unsupported firmware target.")
        if not target_config["project"].is_dir():
            raise RuntimeError("The firmware source directory is not mounted into the builder.")

        shutil.copytree(
            target_config["project"],
            work_dir,
            ignore=shutil.ignore_patterns("build", "sdkconfig", "sdkconfig.old"),
        )
        shutil.copytree(target_config["components"], work_root / "components")
        _install_hub_ca(work_dir)
        for defaults in work_dir.glob("sdkconfig.defaults*"):
            _clear_device_secrets(defaults)

        job.update(stage="checking_toolchain")
        _persist(job)
        _check_toolchain(target, work_dir, log_path)

        job.update(stage="configuring")
        _persist(job)
        code, output = _run_stage(["idf.py", "set-target", TARGETS[target]["chip"]], work_dir, log_path)
        if code != 0:
            raise RuntimeError("ESP-IDF target configuration failed.\n" + output)
        _clear_device_secrets(work_dir / "sdkconfig")
        sdkconfig = work_dir / "sdkconfig"
        if not re.fullmatch(r"[A-Za-z0-9.-]{1,253}", NTP_SERVER):
            raise RuntimeError("DBMAP_NTP_SERVER must be a valid DNS name or IPv4 address.")
        _set_sdkconfig_string(sdkconfig, "CONFIG_DBMAP_NTP_SERVER", NTP_SERVER)
        base_version = _sdkconfig_string(sdkconfig, "CONFIG_DBMAP_FIRMWARE_VERSION")
        firmware_version = _next_firmware_version(base_version)
        _set_sdkconfig_string(sdkconfig, "CONFIG_DBMAP_FIRMWARE_VERSION", firmware_version)
        job.update(firmware_version=firmware_version)
        _persist(job)

        job.update(stage="building")
        _persist(job)
        code, output = _run_stage(["idf.py", "build"], work_dir, log_path)
        if code != 0:
            raise RuntimeError("ESP-IDF firmware build failed.\n" + output)

        generated_config = (work_dir / "sdkconfig").read_text(encoding="utf-8")
        if any(f'{name}=""' not in generated_config for name in SECRET_CONFIG):
            raise RuntimeError("Generated firmware configuration includes device-specific credentials.")
        job.update(stage="finalizing")
        _persist(job)
        build_dir = work_dir / "build"
        entries = _artifact_entries(build_dir, target_config["flash_components"])
        artifact_dir.mkdir(mode=0o700)
        bundle_path = artifact_dir / "firmware.zip"
        manifest_entries = []
        with zipfile.ZipFile(bundle_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for entry in entries:
                archive.write(entry["path"], entry["name"])
                manifest_entries.append({key: value for key, value in entry.items() if key != "path"})
            manifest = {
                "build_id": job_id,
                "target": target,
                "chip": target_config["chip"],
                "node_type": target_config["node_type"],
                "hardware_revision": target_config["hardware_revision"],
                "esp_idf_version": target_config["idf_version"],
                "firmware_version": _sdkconfig_string(
                    work_dir / "sdkconfig",
                    "CONFIG_DBMAP_FIRMWARE_VERSION",
                ),
                "configuration_version": 0,
                "nvs_preserved": True,
                "flash_components": manifest_entries,
            }
            archive.writestr("manifest.json", json.dumps(manifest, indent=2))
        bundle_path.chmod(0o600)
        digest = hashlib.sha256(bundle_path.read_bytes()).hexdigest()
        job.update(
            status="succeeded",
            stage="complete",
            firmware_version=manifest["firmware_version"],
            artifact={"sha256": digest, "size_bytes": bundle_path.stat().st_size},
            finished_at=time.time(),
        )
        _persist(job)
    except subprocess.TimeoutExpired:
        _append_log(log_path, "ESP-IDF build exceeded the 30-minute limit.")
        job.update(
            status="failed",
            stage="failed",
            error="ESP-IDF build exceeded the 30-minute limit; retry Step 4.",
            finished_at=time.time(),
        )
        _persist(job)
        shutil.rmtree(artifact_dir, ignore_errors=True)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        _append_log(log_path, f"Build failed: {error}")
        job.update(status="failed", stage="failed", error=str(error), finished_at=time.time())
        _persist(job)
        shutil.rmtree(artifact_dir, ignore_errors=True)
    finally:
        shutil.rmtree(work_root, ignore_errors=True)
        with lock:
            active_build = None


class Handler(BaseHTTPRequestHandler):
    server_version = "dBmapFirmwareBuilder/1.0"

    def log_message(self, format: str, *args: object) -> None:
        print("firmware-builder", self.address_string(), format % args)

    def _authorized(self) -> bool:
        scheme, separator, token = self.headers.get("Authorization", "").partition(" ")
        if not ADMIN_TOKEN:
            self.send_error(503, "Firmware builder admin authorization is not configured.")
            return False
        if not separator or scheme.lower() != "bearer":
            self.send_error(401, "Admin bearer token required.")
            return False
        if not hmac.compare_digest(token.encode(), ADMIN_TOKEN.encode()):
            self.send_error(403, "Firmware builds require local administrator access.")
            return False
        return True

    def _json(self, status: int, body: dict | list) -> None:
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        path = urlsplit(self.path).path
        if path == "/health":
            self._json(200, {"status": "ok", "service": "firmware-builder"})
            return
        if not self._authorized():
            return
        if path == "/api/v1/firmware/targets":
            self._json(
                200,
                [
                    {
                        "id": target_id,
                        "chip": target["chip"],
                        "hardware_revision": target["hardware_revision"],
                        "esp_idf_version": target["idf_version"],
                    }
                    for target_id, target in TARGETS.items()
                ],
            )
            return
        if path == "/api/v1/firmware/builds":
            self._json(200, [_public_job(job) for job in _load_jobs()])
            return
        parts = path.strip("/").split("/")
        if len(parts) == 6 and parts[:4] == ["api", "v1", "firmware", "builds"] and parts[5] == "log":
            job_id = parts[4]
            if not _valid_id(job_id):
                self.send_error(404, "Build not found.")
                return
            if _get_job(job_id) is None:
                self.send_error(404, "Build not found.")
                return
            log_path = JOBS / f"{job_id}.log"
            if not log_path.is_file():
                self.send_error(404, "Build log is not available.")
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            remaining = log_path.stat().st_size
            self.send_header("Content-Length", str(remaining))
            self.end_headers()
            with log_path.open("rb") as log_file:
                while remaining > 0:
                    chunk = log_file.read(min(64 * 1024, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            return
        if len(parts) == 6 and parts[:4] == ["api", "v1", "firmware", "builds"] and parts[5] == "artifact":
            job_id = parts[4]
            if not _valid_id(job_id):
                self.send_error(404, "Build not found.")
                return
            job = _get_job(job_id)
            bundle = ARTIFACTS / job_id / "firmware.zip"
            if not job or job.get("status") != "succeeded" or not bundle.is_file():
                self.send_error(404, "Build artifact not found.")
                return
            data = bundle.read_bytes()
            if hashlib.sha256(data).hexdigest() != job["artifact"]["sha256"]:
                self.send_error(500, "Build artifact integrity check failed.")
                return
            try:
                with zipfile.ZipFile(bundle) as archive:
                    manifest = json.loads(archive.read("manifest.json"))
                firmware_id = _release_firmware_id(manifest, job["artifact"]["sha256"])
            except (KeyError, OSError, ValueError, zipfile.BadZipFile, json.JSONDecodeError):
                self.send_error(500, "Could not read build metadata from artifact.")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Disposition", f'attachment; filename="{firmware_id}.zip"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if len(parts) == 6 and parts[:4] == ["api", "v1", "firmware", "builds"] and parts[5] == "metadata":
            job_id = parts[4]
            if not _valid_id(job_id):
                self.send_error(404, "Build not found.")
                return
            job = _get_job(job_id)
            bundle = ARTIFACTS / job_id / "firmware.zip"
            if not job or job.get("status") != "succeeded" or not bundle.is_file():
                self.send_error(404, "Build artifact not found.")
                return
            data = bundle.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            if digest != job.get("artifact", {}).get("sha256"):
                self.send_error(500, "Build artifact integrity check failed.")
                return
            try:
                with zipfile.ZipFile(bundle) as archive:
                    manifest = json.loads(archive.read("manifest.json"))
                started_at = datetime.fromtimestamp(job["started_at"], timezone.utc)
                firmware_id = _release_firmware_id(manifest, digest)
                metadata = {
                    "firmware_id": firmware_id,
                    "build_date_utc": started_at.isoformat().replace("+00:00", "Z"),
                    "target": manifest["target"],
                    "node_type": manifest["node_type"],
                    "platform": manifest["chip"],
                    "hardware_revision": manifest["hardware_revision"],
                    "firmware_version": manifest["firmware_version"],
                    "esp_idf_version": manifest["esp_idf_version"],
                    "configuration_version": manifest["configuration_version"],
                    "artifact_sha256": digest,
                }
                metadata_bytes = (json.dumps(metadata, indent=2) + "\n").encode()
            except (KeyError, OSError, ValueError, zipfile.BadZipFile, json.JSONDecodeError):
                self.send_error(500, "Could not generate metadata from build artifact.")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Disposition", f'attachment; filename="{firmware_id}.json"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(metadata_bytes)))
            self.end_headers()
            self.wfile.write(metadata_bytes)
            return
        if len(parts) == 5 and parts[:4] == ["api", "v1", "firmware", "builds"]:
            if not _valid_id(parts[4]):
                self.send_error(404, "Build not found.")
                return
            job = _get_job(parts[4])
            self._json(200 if job else 404, _public_job(job) if job else {"detail": "Build not found."})
            return
        self.send_error(404, "Not found.")

    def do_POST(self) -> None:
        if urlsplit(self.path).path != "/api/v1/firmware/builds":
            self.send_error(404, "Not found.")
            return
        if not self._authorized():
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 4096:
                raise ValueError
            body = json.loads(self.rfile.read(length))
        except (ValueError, json.JSONDecodeError):
            self._json(422, {"detail": "A valid JSON build request is required."})
            return
        if (
            not isinstance(body, dict)
            or set(body) != {"target"}
            or not isinstance(body["target"], str)
            or body["target"] not in TARGETS
        ):
            self._json(422, {"detail": "Select a supported firmware target."})
            return
        key_header = self.headers.get("Idempotency-Key", "")
        try:
            idempotency_key = str(uuid.UUID(key_header))
        except ValueError:
            self._json(422, {"detail": "A valid Idempotency-Key UUID is required."})
            return
        global active_build
        with lock:
            existing = _find_idempotent_job(idempotency_key)
            if existing:
                if existing["target"] != body["target"]:
                    self._json(409, {"detail": "Idempotency key was already used for a different target."})
                    return
                self._json(
                    202 if existing["status"] in {"queued", "running"} else 200,
                    _public_job(existing),
                )
                return
            if active_build is not None:
                self._json(409, {"detail": "A firmware build is already running. Retry after it finishes."})
                return
            job_id = str(uuid.uuid4())
            active_build = job_id
            job = {
                "id": job_id,
                "target": body["target"],
                "hardware_revision": TARGETS[body["target"]]["hardware_revision"],
                "status": "queued",
                "stage": "queued",
                "created_at": time.time(),
                "idempotency_key": idempotency_key,
            }
            _persist(job)
            threading.Thread(target=_build, args=(job_id, body["target"]), daemon=True).start()
        self._json(202, _public_job(job))

    def do_DELETE(self) -> None:
        parts = urlsplit(self.path).path.strip("/").split("/")
        if len(parts) != 5 or parts[:4] != ["api", "v1", "firmware", "builds"]:
            self.send_error(404, "Not found.")
            return
        if not self._authorized():
            return
        job_id = parts[4]
        if not _valid_id(job_id):
            self.send_error(404, "Build not found.")
            return
        job = _get_job(job_id)
        if not job or job.get("status") not in {"succeeded", "failed"}:
            self.send_error(404, "Completed build not found.")
            return
        if job.get("status") == "succeeded":
            shutil.rmtree(ARTIFACTS / job_id, ignore_errors=False)
            (JOBS / f"{job_id}.log").unlink(missing_ok=True)
            job.update(status="deleted", stage="deleted", artifact=None)
            _persist(job)
        else:
            shutil.rmtree(ARTIFACTS / job_id, ignore_errors=True)
            (JOBS / f"{job_id}.log").unlink(missing_ok=True)
            (JOBS / f"{job_id}.json").unlink(missing_ok=True)
        self._json(200, _public_job(job))


def _valid_id(value: str) -> bool:
    try:
        return str(uuid.UUID(value)) == value
    except ValueError:
        return False


def _get_job(job_id: str) -> dict | None:
    try:
        return json.loads((JOBS / f"{job_id}.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _public_job(job: dict) -> dict:
    public = {key: value for key, value in job.items() if key != "idempotency_key"}
    target = TARGETS.get(str(job.get("target", "")))
    if not public.get("hardware_revision") and target:
        public["hardware_revision"] = target["hardware_revision"]
    if not public.get("firmware_version") and job.get("status") == "succeeded":
        bundle = ARTIFACTS / str(job.get("id", "")) / "firmware.zip"
        try:
            with zipfile.ZipFile(bundle) as archive:
                manifest = json.loads(archive.read("manifest.json"))
            public["firmware_version"] = manifest.get("firmware_version")
        except (OSError, KeyError, ValueError, zipfile.BadZipFile, json.JSONDecodeError):
            pass
    return public


def _find_idempotent_job(key: str) -> dict | None:
    for path in JOBS.glob("*.json"):
        job = _get_job(path.stem)
        if job and job.get("idempotency_key") == key:
            return job
    return None


if __name__ == "__main__":
    ARTIFACTS.mkdir(mode=0o700, parents=True, exist_ok=True)
    JOBS.mkdir(mode=0o700, parents=True, exist_ok=True)
    ARTIFACTS.chmod(0o700)
    JOBS.chmod(0o700)
    _load_jobs(recover_interrupted=True)
    server = ThreadingHTTPServer(("0.0.0.0", 8090), Handler)
    server.serve_forever()
