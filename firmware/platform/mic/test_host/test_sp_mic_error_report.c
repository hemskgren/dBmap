/**
 * Host tests: show_zero_error_counters emit policy (firmware reporting).
 *
 * Default (option unset / false):
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g \
 *     -I firmware/platform/mic/include \
 *     firmware/platform/mic/test_host/test_sp_mic_error_report.c \
 *     -o /tmp/test_sp_mic_error_report && /tmp/test_sp_mic_error_report
 *
 * With zeros enabled:
 *   cc ... -DCONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS=1 ...
 */
#include "sp_mic_error_report.h"

#include <stdio.h>

static int g_failures = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static int count_emitted(const uint32_t *vals, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (sp_mic_error_counter_should_emit(vals[i])) {
            c++;
        }
    }
    return c;
}

int main(void)
{
    const uint32_t all_zero[] = {0, 0, 0, 0, 0, 0, 0};
    const uint32_t mixed[] = {0, 2, 0, 1, 0, 0, 0};

#if defined(CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS) \
    && CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS
    EXPECT(sp_mic_show_zero_error_counters() == true);
    EXPECT(count_emitted(all_zero, 7) == 7);
    EXPECT(count_emitted(mixed, 7) == 7);
    printf("test_sp_mic_error_report (show_zeros=true): ok field_count zero=%d mixed=%d\n",
           count_emitted(all_zero, 7),
           count_emitted(mixed, 7));
#else
    EXPECT(sp_mic_show_zero_error_counters() == false);
    EXPECT(count_emitted(all_zero, 7) == 0);
    EXPECT(count_emitted(mixed, 7) == 2);
    printf("test_sp_mic_error_report (show_zeros=false): ok field_count zero=%d mixed=%d\n",
           count_emitted(all_zero, 7),
           count_emitted(mixed, 7));
#endif

    if (g_failures != 0) {
        fprintf(stderr, "test_sp_mic_error_report: %d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
