from __future__ import annotations

import hashlib
import hmac
import json
import os
import shutil
import subprocess
import threading
import time
import uuid
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ARTIFACTS = Path(os.environ.get("DBMAP_FIRMWARE_ARTIFACTS", "/artifacts"))
JOBS = ARTIFACTS / ".jobs"
FIRMWARE_SOURCE = Path(os.environ.get("DBMAP_FIRMWARE_SOURCE", "/firmware/esp-output"))
HUB_CA_FILE = Path(os.environ.get("DBMAP_HUB_CA_FILE", "/certs/ca.crt"))
ADMIN_TOKEN = os.environ.get("DBMAP_ADMIN_TOKEN", "")
BUILD_TIMEOUT_SECONDS = 1800
TARGETS = {
    "esp-output-esp32": {
        "project": FIRMWARE_SOURCE,
        "chip": "esp32",
        "node_type": "output",
        "hardware_revision": "OUTPUT-DEV-V1",
        "idf_version": "v5.4.2",
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


def _run_stage(command: list[str], cwd: Path) -> tuple[int, str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
        timeout=BUILD_TIMEOUT_SECONDS,
        env=os.environ.copy(),
    )
    return result.returncode, "\n".join((result.stdout + result.stderr).splitlines()[-40:])


def _artifact_entries(build_dir: Path) -> list[dict]:
    args_file = build_dir / "flasher_args.json"
    args = json.loads(args_file.read_text(encoding="utf-8"))
    flash_files = args.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError("ESP-IDF did not provide a flash manifest.")
    expected = {
        "0x1000": "bootloader.bin",
        "0x8000": "partition-table.bin",
        "0x10000": "esp-output.bin",
    }
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


def _check_toolchain(target: str, work_dir: Path) -> None:
    code, output = _run_stage(["idf.py", "--version"], work_dir)
    if code != 0 or TARGETS[target]["idf_version"] not in output:
        raise RuntimeError("The pinned ESP-IDF toolchain check failed.\n" + output)


def _build(job_id: str, target: str) -> None:
    global active_build
    job_path = JOBS / f"{job_id}.json"
    work_root = ARTIFACTS / f".work-{job_id}"
    work_dir = work_root / "esp-output"
    artifact_dir = ARTIFACTS / job_id
    job = json.loads(job_path.read_text(encoding="utf-8"))
    try:
        job.update(status="running", stage="validating", started_at=time.time())
        _persist(job)
        if not FIRMWARE_SOURCE.is_dir():
            raise RuntimeError("The firmware source directory is not mounted into the builder.")
        if target not in TARGETS:
            raise RuntimeError("Unsupported firmware target.")

        shutil.copytree(
            TARGETS[target]["project"],
            work_dir,
            ignore=shutil.ignore_patterns("build", "sdkconfig", "sdkconfig.old"),
        )
        shutil.copytree(FIRMWARE_SOURCE.parent / "components", work_root / "components")
        _install_hub_ca(work_dir)
        for defaults in work_dir.glob("sdkconfig.defaults*"):
            _clear_device_secrets(defaults)

        job.update(stage="checking_toolchain")
        _persist(job)
        _check_toolchain(target, work_dir)

        job.update(stage="configuring")
        _persist(job)
        code, output = _run_stage(["idf.py", "set-target", TARGETS[target]["chip"]], work_dir)
        if code != 0:
            raise RuntimeError("ESP-IDF target configuration failed.\n" + output)
        _clear_device_secrets(work_dir / "sdkconfig")

        job.update(stage="building")
        _persist(job)
        code, output = _run_stage(["idf.py", "build"], work_dir)
        if code != 0:
            raise RuntimeError("ESP-IDF firmware build failed.\n" + output)

        generated_config = (work_dir / "sdkconfig").read_text(encoding="utf-8")
        if any(f'{name}=""' not in generated_config for name in SECRET_CONFIG):
            raise RuntimeError("Generated firmware configuration includes device-specific credentials.")
        job.update(stage="finalizing")
        _persist(job)
        build_dir = work_dir / "build"
        entries = _artifact_entries(build_dir)
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
                "chip": TARGETS[target]["chip"],
                "node_type": TARGETS[target]["node_type"],
                "hardware_revision": TARGETS[target]["hardware_revision"],
                "esp_idf_version": TARGETS[target]["idf_version"],
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
            artifact={"sha256": digest, "size_bytes": bundle_path.stat().st_size},
            finished_at=time.time(),
        )
        _persist(job)
    except subprocess.TimeoutExpired:
        job.update(
            status="failed",
            stage="failed",
            error="ESP-IDF build exceeded the 30-minute limit; retry Step 4.",
            finished_at=time.time(),
        )
        _persist(job)
        shutil.rmtree(artifact_dir, ignore_errors=True)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError, zipfile.BadZipFile) as error:
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
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Disposition", f'attachment; filename="dBmap-{job_id}.zip"')
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
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
        if not job or job.get("status") != "succeeded":
            self.send_error(404, "Completed build artifact not found.")
            return
        shutil.rmtree(ARTIFACTS / job_id, ignore_errors=False)
        job.update(status="deleted", stage="deleted", artifact=None)
        _persist(job)
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
    return {key: value for key, value in job.items() if key != "idempotency_key"}


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
