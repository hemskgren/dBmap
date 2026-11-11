/**
 * Host contract tests for dual-channel capture_diag UART format strings.
 *
 * Build/run from repo root:
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/test_host/test_capture_diag_format.c \
 *     -o /tmp/test_dbmap_capture_diag_format && /tmp/test_dbmap_capture_diag_format
 *
 * Ensures bench UART lines can distinguish ch0/ch1 and share window_seq.
 */
#include "dbmap_ear_mic_capture_diag.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_summary_has_incomplete(void)
{
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_SUMMARY_FMT, "capture_diag") != NULL);
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_SUMMARY_FMT, "incomplete=%lu") != NULL);
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_SUMMARY_FMT, "live=%u") != NULL);
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_SUMMARY_FMT, "mode=%s") != NULL);
}

static void test_channel_line_has_identity_and_seq(void)
{
    const char *fmt = DBMAP_EAR_MIC_CAPTURE_DIAG_CH_FMT;
    EXPECT(strstr(fmt, "capture_diag ch%u") != NULL);
    EXPECT(strstr(fmt, "slot=%s") != NULL);
    EXPECT(strstr(fmt, "n=%lu") != NULL);
    EXPECT(strstr(fmt, "expect=%lu") != NULL);
    EXPECT(strstr(fmt, "rms=%.1f") != NULL);
    EXPECT(strstr(fmt, "peak=%.1f") != NULL);
    EXPECT(strstr(fmt, "mic=%s") != NULL);
    EXPECT(strstr(fmt, "xport=%s") != NULL);
    EXPECT(strstr(fmt, "window_seq=%lu") != NULL);
    EXPECT(strstr(fmt, "presence=%s") != NULL);
}

static void test_boot_banner_identifies_bench_diag(void)
{
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_BOOT_MSG, "capture_diag UART enabled") != NULL);
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_BOOT_MSG, "window_seq") != NULL);
    EXPECT(strstr(DBMAP_EAR_MIC_CAPTURE_DIAG_BOOT_MSG, "incomplete") != NULL);
}

/**
 * Simulate two channel lines sharing one window_seq — the correlation Test A needs.
 * (Does not run on device; documents the operator-visible pairing.)
 */
static void test_example_paired_channel_lines(void)
{
    char ch0[256];
    char ch1[256];
    snprintf(
        ch0,
        sizeof(ch0),
        DBMAP_EAR_MIC_CAPTURE_DIAG_CH_FMT,
        0u,
        "left",
        16640ul,
        16000ul,
        -40.0,
        -28.0,
        0ul,
        0ul,
        0u,
        "active",
        "signal_present",
        "ok",
        0ul,
        0ul,
        0ul,
        42ul
    );
    snprintf(
        ch1,
        sizeof(ch1),
        DBMAP_EAR_MIC_CAPTURE_DIAG_CH_FMT,
        1u,
        "right",
        16640ul,
        16000ul,
        -52.0,
        -40.0,
        0ul,
        0ul,
        0u,
        "active",
        "signal_present",
        "ok",
        0ul,
        0ul,
        0ul,
        42ul
    );
    EXPECT(strstr(ch0, "ch0") != NULL);
    EXPECT(strstr(ch0, "slot=left") != NULL);
    EXPECT(strstr(ch0, "window_seq=42") != NULL);
    EXPECT(strstr(ch1, "ch1") != NULL);
    EXPECT(strstr(ch1, "slot=right") != NULL);
    EXPECT(strstr(ch1, "window_seq=42") != NULL);
    EXPECT(strstr(ch0, "rms=-40.0") != NULL);
    EXPECT(strstr(ch1, "rms=-52.0") != NULL);
}

int main(void)
{
    test_summary_has_incomplete();
    test_channel_line_has_identity_and_seq();
    test_boot_banner_identifies_bench_diag();
    test_example_paired_channel_lines();

    if (g_failures != 0) {
        fprintf(stderr, "test_capture_diag_format: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_capture_diag_format: ok\n");
    return 0;
}
