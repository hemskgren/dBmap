"""Exercise the firmware JSON enricher on the host without the I2S/RTOS runtime."""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
MIC = ROOT / "firmware/platform/mic"
COMMON = ROOT / "firmware/platform/node_common"

HOST_STATE = """
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "sp_audio_metrics.h"
#include "sp_background_rms.h"
#include "sp_mic_count.h"
#include "sp_mic_error_report.h"

#define CONFIG_SP_MIC_BACKGROUND 1
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)

typedef struct {
    struct { const char *hardware_revision; } identity;
    uint8_t microphone_count;
    sp_mic_count_status_t microphone_count_status;
} sp_effective_t;

typedef struct { const char *id; } host_mic_model_t;
static const host_mic_model_t model = {"INMP441"};
static const host_mic_model_t *s_model = &model;
static const struct { int sample_rate_hz; } s_mic_cfg = {16000};
static const char *s_slot_mode_name;
static uint8_t s_live_mic_count;
static uint32_t s_slot_words_per_frame, s_incomplete_stereo_frames_window;
static uint32_t s_dma_desc_num, s_dma_frame_num, s_request_bytes;
static uint32_t s_measure_window_us_last, s_slot_buf[2][512];
static sp_bg_rms_hist_t s_bg_hist[SP_MIC_COUNT_MAX];
static sp_bg_rms_sample_t entries[SP_MIC_COUNT_MAX];
static int64_t esp_timer_get_time(void) { return 7000000; }
"""

HOST_MAIN = """
int main(int argc, char **argv)
{
    sp_effective_t cfg = {
        .identity.hardware_revision = "host",
        .microphone_count = argc - 1,
        .microphone_count_status = SP_MIC_COUNT_STATUS_OK,
    };
    s_live_mic_count = cfg.microphone_count;
    s_slot_mode_name = s_live_mic_count == 1 ? "mono_left" : "stereo_lr";
    s_slot_words_per_frame = s_live_mic_count;
    for (uint8_t i = 0; i < s_live_mic_count; ++i) {
        strcpy(s_latest[i].microphone_status, argv[i + 1]);
        strcpy(s_latest[i].transport_status, "ok");
        s_latest[i].rms_dbfs = -40 - 10 * i;
        s_latest[i].peak_dbfs = -20 - 10 * i;
        s_latest[i].sample_count = 16128;
        s_latest[i].window_ms = 1000;
        s_latest[i].measured_at_us = 6000000;
        /* Seed every ring so the test exercises the visibility gate itself. */
        sp_bg_rms_hist_init(&s_bg_hist[i], &entries[i], 1, 180000);
        sp_bg_rms_push(&s_bg_hist[i], -45 - 10 * i, 6000000);
    }
    cJSON *state = cJSON_CreateObject();
    enrich_health(state, &cfg, true);
    char *payload = cJSON_PrintUnformatted(state);
    puts(payload);
    cJSON_free(payload);
    cJSON_Delete(state);
    return 0;
}
"""


@pytest.fixture(scope="module")
def enricher(tmp_path_factory):
    cc = shutil.which(os.environ.get("CC", "cc"))
    idf = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf"))
    cjson = idf / "components/json/cJSON"
    if cc is None:
        pytest.skip("Host C compiler unavailable")
    if (cjson / "cJSON.c").is_file():
        json_args = [f"-I{cjson}", str(cjson / "cJSON.c")]
    elif shutil.which("pkg-config") and subprocess.run(
        ["pkg-config", "--exists", "libcjson"], check=False
    ).returncode == 0:
        json_args = subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcjson"], text=True
        ).split()
    else:
        pytest.skip("Set IDF_PATH or install host libcjson development files")

    # Compile the actual snapshot type and serializers with host state in place
    # of the hardware globals. JSON and background history use the real code.
    source = (MIC / "ear_mic.c").read_text()
    snapshot = source[source.index("typedef struct {") : source.index("} ear_audio_snapshot_t;")]
    start = source.index("static void enrich_mic_error_counters_diag(")
    end = source.index("static const char *slot_mode_name_for_live(", start)
    serializers = source[start:end]
    build = tmp_path_factory.mktemp("ear-mic-report")
    harness = build / "report.c"
    harness.write_text(
        HOST_STATE + snapshot + "} ear_audio_snapshot_t;\n"
        "static ear_audio_snapshot_t s_latest[SP_MIC_COUNT_MAX];\n"
        + serializers + HOST_MAIN
    )
    binary = build / "report"
    subprocess.run(
        [
            cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            f"-I{MIC / 'include'}", f"-I{COMMON / 'include'}", str(harness),
            str(MIC / "background_rms.c"), str(MIC / "audio_metrics.c"),
            str(COMMON / "mic_count.c"), *json_args, "-lm", "-o", str(binary),
        ],
        check=True,
    )
    return binary


HEALTHY = "signal_present"
UNHEALTHY = "noise_suspected"


@pytest.mark.parametrize(
    ("statuses", "visible"),
    [
        ((UNHEALTHY, HEALTHY), (1,)),
        ((HEALTHY, UNHEALTHY), (0,)),
        ((HEALTHY, HEALTHY), (0, 1)),
        ((HEALTHY,), (0,)),
        ((UNHEALTHY,), ()),
    ],
)
def test_background_visibility_and_payload(enricher, statuses, visible):
    payload = json.loads(subprocess.check_output([enricher, *statuses], text=True))
    audio = payload["audio"]
    assert audio["microphone_status"] == statuses[0]
    assert audio["audio_transport_status"] == audio["subsystem_status"] == "ok"
    assert audio["live_microphone_count"] == audio["configured_microphone_count"] == len(statuses)
    assert payload["capabilities"]["role"] == "ear"
    microphones = audio["microphones"]
    assert len(microphones) == len(statuses)
    for index, (mic, status) in enumerate(zip(microphones, statuses, strict=True)):
        assert mic["index"] == index
        assert mic["microphone_status"] == status
        assert mic["rms_dbfs"] == -40 - 10 * index
        assert mic["peak_dbfs"] == -20 - 10 * index
        assert mic["sample_count"] == 16128
        assert mic["expected_sample_count"] == 16000
        if index in visible:
            level = -45 - 10 * index
            assert mic["background"] == {
                "window_ms": 180000,
                "sample_count": 1,
                "status": "partial",
                "rms_min_dbfs": level,
                "rms_max_dbfs": level,
                "rms_mean_dbfs": level,
            }
        else:
            assert "background" not in mic
