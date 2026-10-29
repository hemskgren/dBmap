import http.client
import json
import sys
import threading
import zipfile
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import server


@pytest.fixture
def builder(monkeypatch, tmp_path):
    artifacts = tmp_path / "artifacts"
    jobs = artifacts / ".jobs"
    artifacts.mkdir()
    jobs.mkdir()
    monkeypatch.setattr(server, "ARTIFACTS", artifacts)
    monkeypatch.setattr(server, "JOBS", jobs)
    monkeypatch.setattr(server, "ADMIN_TOKEN", "test-admin-token")
    monkeypatch.setattr(server, "active_build", None)
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    yield httpd, artifacts, jobs
    httpd.shutdown()
    httpd.server_close()
    thread.join()


def request(builder, method, path, body=None, token=None, idempotency_key=None):
    httpd, _, _ = builder
    connection = http.client.HTTPConnection(*httpd.server_address)
    headers = {}
    payload = None
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if idempotency_key:
        headers["Idempotency-Key"] = idempotency_key
    if body is not None:
        payload = json.dumps(body)
        headers["Content-Type"] = "application/json"
    connection.request(method, path, body=payload, headers=headers)
    response = connection.getresponse()
    result = response.read()
    connection.close()
    try:
        return response.status, json.loads(result)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return response.status, result


def test_access_log_uses_the_forwarded_client(capsys) -> None:
    handler = server.Handler.__new__(server.Handler)
    handler.client_address = ("172.19.0.8", 45864)
    handler.headers = {"X-Real-IP": "2001:db8::10"}
    handler.log_message('"%s" %s', "GET /health HTTP/1.1", "200")

    captured = capsys.readouterr()
    assert captured.out == ""
    assert "2001:db8::10 - - [" in captured.err
    assert '"GET /health HTTP/1.1" 200' in captured.err
    assert "172.19.0.8" not in captured.err


def test_access_log_keeps_the_peer_when_forwarding_is_absent(capsys) -> None:
    handler = server.Handler.__new__(server.Handler)
    handler.client_address = ("172.19.0.8", 45864)
    handler.headers = {}
    handler.log_message("%s", "GET /health HTTP/1.1")

    captured = capsys.readouterr()
    assert captured.out == ""
    assert "172.19.0.8 - - [" in captured.err


def test_builder_requires_admin_token(builder):
    assert request(builder, "GET", "/api/v1/firmware/targets")[0] == 401
    assert request(builder, "GET", "/api/v1/firmware/targets", token="viewer-token")[0] == 403
    status, targets = request(builder, "GET", "/api/v1/firmware/targets", token="test-admin-token")
    assert status == 200
    assert [target["id"] for target in targets] == ["output-esp32", "ear-esp32-s3"]
    assert targets[0]["hardware_revision"] == "generic"
    assert targets[1]["chip"] == "esp32s3"
    assert targets[1]["node_type"] == "ear"
    assert targets[1]["hardware_revision"] == "esp32-s3-n16r8"


@pytest.mark.parametrize("body", [None, {}, {"target": "unlisted"}, {"target": []}])
def test_builder_rejects_invalid_build_requests(builder, body):
    status, response = request(
        builder,
        "POST",
        "/api/v1/firmware/builds",
        body=body,
        token="test-admin-token",
    )
    assert status == 422
    assert "detail" in response


def test_build_submission_and_status_polling_preserve_queued_state(builder, monkeypatch):
    monkeypatch.setattr(server, "_build", lambda job_id, target: None)
    status, queued = request(
        builder,
        "POST",
        "/api/v1/firmware/builds",
        body={"target": "output-esp32"},
        token="test-admin-token",
        idempotency_key="d8e1d748-9582-4a65-aa85-4d149b8afc9f",
    )
    assert status == 202
    assert queued["status"] == "queued"
    status, listed = request(
        builder, "GET", "/api/v1/firmware/builds", token="test-admin-token"
    )
    assert status == 200
    assert listed[0]["status"] == "queued"
    assert server.active_build == queued["id"]
    assert "idempotency_key" not in listed[0]
    status, retried = request(
        builder,
        "POST",
        "/api/v1/firmware/builds",
        body={"target": "output-esp32"},
        token="test-admin-token",
        idempotency_key="d8e1d748-9582-4a65-aa85-4d149b8afc9f",
    )
    assert status == 202
    assert retried["id"] == queued["id"]


def test_builder_clears_device_secrets_from_sdkconfig(tmp_path):
    sdkconfig = tmp_path / "sdkconfig"
    sdkconfig.write_text(
        'CONFIG_DBMAP_WIFI_SSID="private-network"\n'
        'CONFIG_DBMAP_WIFI_PASSWORD="private-password"\n'
        'CONFIG_DBMAP_BOOTSTRAP_TOKEN="private-token"\n',
        encoding="utf-8",
    )
    server._clear_device_secrets(sdkconfig)
    result = sdkconfig.read_text(encoding="utf-8")
    for name in server.SECRET_CONFIG:
        assert f'{name}=""' in result
    assert "private-" not in result


def test_install_hub_ca_copies_trust_anchor_into_build_project(tmp_path, monkeypatch):
    ca_file = tmp_path / "ca.crt"
    ca_file.write_text("public hub CA certificate", encoding="utf-8")
    project_dir = tmp_path / "project"
    monkeypatch.setattr(server, "HUB_CA_FILE", ca_file)

    server._install_hub_ca(project_dir)

    assert (project_dir / "main" / "root_ca.pem").read_text(encoding="utf-8") == (
        "public hub CA certificate"
    )


def test_install_hub_ca_fails_with_actionable_message(tmp_path, monkeypatch):
    for ca_file in (tmp_path / "missing-ca.crt", tmp_path / "empty-ca.crt"):
        ca_file.touch(exist_ok=True)
        monkeypatch.setattr(server, "HUB_CA_FILE", ca_file)

        with pytest.raises(RuntimeError, match="mount shared/trust/ca.crt into firmware-builder"):
            server._install_hub_ca(tmp_path / "project")


def test_sdkconfig_string_reads_firmware_version(tmp_path):
    sdkconfig = tmp_path / "sdkconfig"
    sdkconfig.write_text('CONFIG_DBMAP_FIRMWARE_VERSION="1.2.3"\n', encoding="utf-8")

    assert server._sdkconfig_string(sdkconfig, "CONFIG_DBMAP_FIRMWARE_VERSION") == "1.2.3"


def test_sdkconfig_syncs_app_project_ver_from_defaults(tmp_path):
    sdkconfig = tmp_path / "sdkconfig"
    sdkconfig.write_text(
        '# CONFIG_APP_PROJECT_VER_FROM_CONFIG is not set\n'
        'CONFIG_APP_PROJECT_VER="0.1.4"\n',
        encoding="utf-8",
    )
    server._ensure_sdkconfig_flag(sdkconfig, "CONFIG_APP_PROJECT_VER_FROM_CONFIG")
    server._set_sdkconfig_string(sdkconfig, "CONFIG_APP_PROJECT_VER", "0.1.15")
    text = sdkconfig.read_text(encoding="utf-8")
    assert "CONFIG_APP_PROJECT_VER_FROM_CONFIG=y" in text
    assert server._sdkconfig_string(sdkconfig, "CONFIG_APP_PROJECT_VER") == "0.1.15"


def test_short_error_keeps_job_list_to_one_line():
    dump = "ESP-IDF firmware build failed.\n[969/1011] Building C object esp-idf/spiffs\n" + ("x" * 400)
    assert server._short_error(dump) == "ESP-IDF firmware build failed."
    assert "Building C object" not in server._short_error(dump)
    long_line = "error: " + ("a" * 200)
    summary = server._short_error(long_line)
    assert "\n" not in summary
    assert len(summary) <= 160
    assert summary.endswith("…")


def test_toolchain_check_requires_pinned_esp_idf_version(tmp_path, monkeypatch):
    log_path = tmp_path / "toolchain.log"
    monkeypatch.setattr(server, "_run_stage", lambda command, cwd, log_path: (0, "ESP-IDF v5.4.4"))
    server._check_toolchain("output-esp32", tmp_path, log_path)

    monkeypatch.setattr(server, "_run_stage", lambda command, cwd, log_path: (0, "ESP-IDF v5.3.0"))
    with pytest.raises(RuntimeError, match="pinned ESP-IDF toolchain check failed"):
        server._check_toolchain("output-esp32", tmp_path, log_path)


def test_build_list_keeps_older_artifacts_visible(builder):
    _, _, jobs = builder
    for number in range(101):
        job_id = f"00000000-0000-0000-0000-{number:012d}"
        (jobs / f"{job_id}.json").write_text(
            json.dumps({"id": job_id, "status": "succeeded", "target": "output-esp32"}),
            encoding="utf-8",
        )

    status, result = request(builder, "GET", "/api/v1/firmware/builds", token="test-admin-token")

    assert status == 200
    assert len(result) == 101


def test_artifact_flash_manifest_contains_only_nvs_preserving_offsets(tmp_path):
    build_dir = tmp_path / "build"
    components = {
        "0x1000": ("bootloader/bootloader.bin", b"bootloader"),
        "0x8000": ("partition_table/partition-table.bin", b"partitions"),
        "0x10000": ("ota_data_initial.bin", b"otadata"),
        "0x20000": ("esp-output.bin", b"application"),
    }
    flash_files = {}
    for offset, (relative_path, content) in components.items():
        path = build_dir / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        flash_files[offset] = relative_path
    (build_dir / "flasher_args.json").write_text(
        json.dumps({"flash_files": flash_files}),
        encoding="utf-8",
    )
    entries = server._artifact_entries(
        build_dir,
        {
            "0x1000": "bootloader.bin",
            "0x8000": "partition-table.bin",
            "0x10000": "ota_data_initial.bin",
            "0x20000": "esp-output.bin",
        },
    )
    assert [(entry["offset"], entry["name"]) for entry in entries] == [
        ("0x1000", "bootloader.bin"),
        ("0x8000", "partition-table.bin"),
        ("0x10000", "ota_data_initial.bin"),
        ("0x20000", "esp-output.bin"),
    ]

    flash_files["0x9000"] = "esp-output.bin"
    (build_dir / "flasher_args.json").write_text(
        json.dumps({"flash_files": flash_files}),
        encoding="utf-8",
    )
    with pytest.raises(RuntimeError, match="Unexpected ESP-IDF flash offset"):
        server._artifact_entries(
            build_dir,
            {
                "0x1000": "bootloader.bin",
                "0x8000": "partition-table.bin",
                "0x10000": "ota_data_initial.bin",
                "0x20000": "esp-output.bin",
            },
        )


def test_artifact_download_and_explicit_delete(builder):
    _, artifacts, jobs = builder
    build_id = "a13b1d81-e93d-49b0-b31c-5b88dc1722a1"
    artifact_dir = artifacts / build_id
    artifact_dir.mkdir()
    bundle = artifact_dir / "firmware.zip"
    with zipfile.ZipFile(bundle, "w") as archive:
        archive.writestr(
            "manifest.json",
            json.dumps({"target": "output-esp32", "firmware_version": "0.1.4"}),
        )
    bundle.chmod(0o600)
    job = {
        "id": build_id,
        "target": "output-esp32",
        "status": "succeeded",
        "stage": "complete",
        "artifact": {
            "sha256": server.hashlib.sha256(bundle.read_bytes()).hexdigest(),
            "size_bytes": bundle.stat().st_size,
        },
    }
    (jobs / f"{build_id}.json").write_text(json.dumps(job), encoding="utf-8")
    status, downloaded = request(
        builder,
        "GET",
        f"/api/v1/firmware/builds/{build_id}/artifact",
        token="test-admin-token",
    )
    assert status == 200
    assert downloaded[:2] == b"PK"
    status, deleted = request(
        builder,
        "DELETE",
        f"/api/v1/firmware/builds/{build_id}",
        token="test-admin-token",
    )
    assert status == 200
    assert deleted["status"] == "deleted"
    assert not artifact_dir.exists()
