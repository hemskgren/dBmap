/**
 * Host tests for installation microphone_count helpers.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/components/node_common/include \
 *     -I firmware/components/node_common/test_host \
 *     firmware/components/node_common/mic_count.c \
 *     firmware/components/node_common/test_host/test_mic_count.c \
 *     -o /tmp/test_dbmap_mic_count && /tmp/test_dbmap_mic_count
 */
#include "dbmap_mic_count.h"

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
    dbmap_mic_count_status_t st = DBMAP_MIC_COUNT_STATUS_OK;
    dbmap_mic_count_from_nvs(false, 0, &eff, &st);
    EXPECT(eff == 1);
    EXPECT(st == DBMAP_MIC_COUNT_STATUS_DEFAULTED);
    EXPECT(strcmp(dbmap_mic_count_status_str(st), "defaulted") == 0);
}

static void test_valid_stored(void)
{
    uint8_t eff = 0;
    dbmap_mic_count_status_t st = DBMAP_MIC_COUNT_STATUS_DEFAULTED;
    dbmap_mic_count_from_nvs(true, 2, &eff, &st);
    EXPECT(eff == 2);
    EXPECT(st == DBMAP_MIC_COUNT_STATUS_OK);

    dbmap_mic_count_from_nvs(true, 1, &eff, &st);
    EXPECT(eff == 1);
    EXPECT(st == DBMAP_MIC_COUNT_STATUS_OK);
}

static void test_invalid_stored_fallback(void)
{
    uint8_t eff = 0;
    dbmap_mic_count_status_t st = DBMAP_MIC_COUNT_STATUS_OK;
    const uint8_t bad[] = {0, 3, 255};
    for (size_t i = 0; i < sizeof(bad); ++i) {
        dbmap_mic_count_from_nvs(true, bad[i], &eff, &st);
        EXPECT(eff == 1);
        EXPECT(st == DBMAP_MIC_COUNT_STATUS_INVALID_FALLBACK);
        EXPECT(strcmp(dbmap_mic_count_status_str(st), "invalid_fallback") == 0);
    }
}

static void test_json_int_accepts(void)
{
    uint8_t eff = 0;
    EXPECT(dbmap_mic_count_from_json_int(1, &eff) && eff == 1);
    EXPECT(dbmap_mic_count_from_json_int(2, &eff) && eff == 2);
}

static void test_json_int_rejects(void)
{
    uint8_t eff = 9;
    EXPECT(!dbmap_mic_count_from_json_int(0, &eff));
    EXPECT(!dbmap_mic_count_from_json_int(3, &eff));
    EXPECT(!dbmap_mic_count_from_json_int(-1, &eff));
    EXPECT(!dbmap_mic_count_from_json_int(100, &eff));
    EXPECT(eff == 9); /* unchanged on failure */
}

int main(void)
{
    test_missing_defaults();
    test_valid_stored();
    test_invalid_stored_fallback();
    test_json_int_accepts();
    test_json_int_rejects();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_mic_count: ok\n");
    return 0;
}
