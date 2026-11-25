"""Compile the shared USB identity logger with a deterministic clock."""

import shutil
import subprocess
from pathlib import Path

import pytest

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware"


def test_usb_identity_from_both_status_loops(tmp_path: Path) -> None:
    for product in ("esp-ear", "esp-output"):
        source = (FIRMWARE / product / "main/app_main.c").read_text()
        assert "int64_t next_usb_identity_us = 0;" in source
        assert source.count("&s_cfg, &next_usb_identity_us);") == 2
        assert "&s_cfg, &next_usb_identity_us);" in source.split("while (1) {")[-1]

    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("A host C compiler is required for the USB identity logger test")
    stub_headers = {
        "cJSON.h": """typedef struct cJSON cJSON;
cJSON *cJSON_CreateObject(void);
void cJSON_AddNumberToObject(cJSON *, const char *, double);
void cJSON_AddStringToObject(cJSON *, const char *, const char *);
char *cJSON_PrintUnformatted(const cJSON *);
void cJSON_Delete(cJSON *);
""",
        "esp_err.h": "typedef int esp_err_t;\n#define ESP_OK 0\n",
        "esp_log.h": "void ESP_LOGI(const char *tag, const char *format, ...);\n",
        "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "mqtt_client.h": "typedef void *esp_mqtt_client_handle_t;\n",
        "sdkconfig.h": '#define CONFIG_DBMAP_FIRMWARE_VERSION "0.8.99"\n',
    }
    for name, contents in stub_headers.items():
        (tmp_path / name).write_text(contents)
    harness = tmp_path / "identity.c"
    harness.write_text("""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sp_health.h"

static int64_t now_us;
static unsigned logs;
static char last[120];

int64_t esp_timer_get_time(void) { return now_us; }

void ESP_LOGI(const char *tag, const char *format, ...) {
    va_list args;
    (void)tag;
    va_start(args, format);
    vsnprintf(last, sizeof(last), format, args);
    va_end(args);
    logs++;
}

int main(void) {
    sp_effective_t cfg = {0};
    int64_t next = 0;
    sp_log_usb_identity("platform_boot", &cfg, &next);
    assert(logs == 1 && !strcmp(last, "node_id=UNPROVISIONED firmware_version=0.8.99 uptime_s=0"));
    strcpy(cfg.identity.node_id, "EAR-001");
    now_us = 59999999;
    sp_log_usb_identity("platform_boot", &cfg, &next);
    assert(logs == 1);
    now_us = 60000000;
    sp_log_usb_identity("platform_boot", &cfg, &next);
    assert(logs == 2 && !strcmp(last, "node_id=EAR-001 firmware_version=0.8.99 uptime_s=60"));
    strcpy(cfg.identity.node_id, "OUT-001");
    now_us = 181000000;
    sp_log_usb_identity("esp-output", &cfg, &next);
    assert(logs == 3 && !strcmp(last, "node_id=OUT-001 firmware_version=0.8.99 uptime_s=181"));
    sp_log_usb_identity("esp-output", &cfg, &next);
    assert(logs == 3);
}
""")
    platform = FIRMWARE / "platform"
    executable = tmp_path / "identity"
    subprocess.run(
        [
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
            "-Wl,--gc-sections", "-I", str(tmp_path),
            "-I", str(platform / "node_common/include"), "-I", str(platform / "mic/include"),
            str(platform / "node_common/health.c"), str(harness), "-o", str(executable),
        ],
        check=True,
    )
    subprocess.run([executable], check=True)
