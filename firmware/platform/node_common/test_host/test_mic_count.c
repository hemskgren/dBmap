/**
 * Host tests for installation microphone_count helpers.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/platform/node_common/include \
 *     -I firmware/platform/node_common/test_host \
 *     firmware/platform/node_common/mic_count.c \
 *     firmware/platform/node_common/test_host/test_mic_count.c \
 *     -o /tmp/test_sp_mic_count && /tmp/test_sp_mic_count
 */
#include "sp_mic_count.h"

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

static void test_missing_defaults(void)
{
    uint8_t eff = 99;
    sp_mic_count_status_t st = SP_MIC_COUNT_STATUS_OK;
    sp_mic_count_from_nvs(false, 0, &eff, &st);
    EXPECT(eff == 1);
    EXPECT(st == SP_MIC_COUNT_STATUS_DEFAULTED);
    EXPECT(strcmp(sp_mic_count_status_str(st), "defaulted") == 0);
}

static void test_valid_stored(void)
{
    uint8_t eff = 0;
    sp_mic_count_status_t st = SP_MIC_COUNT_STATUS_DEFAULTED;
    for (uint8_t n = 1; n <= 4; ++n) {
        sp_mic_count_from_nvs(true, n, &eff, &st);
        EXPECT(eff == n);
        EXPECT(st == SP_MIC_COUNT_STATUS_OK);
    }
}

static void test_invalid_stored_fallback(void)
{
    uint8_t eff = 0;
    sp_mic_count_status_t st = SP_MIC_COUNT_STATUS_OK;
    const uint8_t bad[] = {0, 5, 255};
    for (size_t i = 0; i < sizeof(bad); ++i) {
        sp_mic_count_from_nvs(true, bad[i], &eff, &st);
        EXPECT(eff == 1);
        EXPECT(st == SP_MIC_COUNT_STATUS_INVALID_FALLBACK);
        EXPECT(strcmp(sp_mic_count_status_str(st), "invalid_fallback") == 0);
    }
}

static void test_json_int_accepts(void)
{
    uint8_t eff = 0;
    EXPECT(sp_mic_count_from_json_int(1, &eff) && eff == 1);
    EXPECT(sp_mic_count_from_json_int(2, &eff) && eff == 2);
    EXPECT(sp_mic_count_from_json_int(3, &eff) && eff == 3);
    EXPECT(sp_mic_count_from_json_int(4, &eff) && eff == 4);
}

static void test_json_int_rejects(void)
{
    uint8_t eff = 9;
    EXPECT(!sp_mic_count_from_json_int(0, &eff));
    EXPECT(!sp_mic_count_from_json_int(5, &eff));
    EXPECT(!sp_mic_count_from_json_int(-1, &eff));
    EXPECT(!sp_mic_count_from_json_int(100, &eff));
    EXPECT(eff == 9); /* unchanged on failure */
}

static void test_acquire_clamp(void)
{
    EXPECT(sp_mic_acquire_count(1) == 1);
    EXPECT(sp_mic_acquire_count(2) == 2);
    EXPECT(sp_mic_acquire_count(3) == SP_MIC_ACQUIRE_MAX);
    EXPECT(sp_mic_acquire_count(4) == SP_MIC_ACQUIRE_MAX);
    EXPECT(sp_mic_acquire_count(0) == SP_MIC_COUNT_DEFAULT);
    EXPECT(SP_MIC_ACQUIRE_MAX == 2);
    EXPECT(SP_MIC_COUNT_MAX == 4);
}

int main(void)
{
    test_missing_defaults();
    test_valid_stored();
    test_invalid_stored_fallback();
    test_json_int_accepts();
    test_json_int_rejects();
    test_acquire_clamp();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_mic_count: ok\n");
    return 0;
}
