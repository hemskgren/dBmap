#include "sp_mic_count.h"

#include <stddef.h>

const char *sp_mic_count_status_str(sp_mic_count_status_t status)
{
    switch (status) {
    case SP_MIC_COUNT_STATUS_OK:
        return "ok";
    case SP_MIC_COUNT_STATUS_DEFAULTED:
        return "defaulted";
    case SP_MIC_COUNT_STATUS_INVALID_FALLBACK:
        return "invalid_fallback";
    default:
        return "invalid_fallback";
    }
}

bool sp_mic_count_supported(uint8_t count)
{
    return count >= SP_MIC_COUNT_MIN && count <= SP_MIC_COUNT_MAX;
}

void sp_mic_count_from_nvs(
    bool found,
    uint8_t raw,
    uint8_t *out_effective,
    sp_mic_count_status_t *out_status
)
{
    uint8_t effective = SP_MIC_COUNT_DEFAULT;
    sp_mic_count_status_t status = SP_MIC_COUNT_STATUS_DEFAULTED;

    if (found) {
        if (sp_mic_count_supported(raw)) {
            effective = raw;
            status = SP_MIC_COUNT_STATUS_OK;
        } else {
            effective = SP_MIC_COUNT_DEFAULT;
            status = SP_MIC_COUNT_STATUS_INVALID_FALLBACK;
        }
    }

    if (out_effective != NULL) {
        *out_effective = effective;
    }
    if (out_status != NULL) {
        *out_status = status;
    }
}

bool sp_mic_count_from_json_int(int value, uint8_t *out_effective)
{
    if (value < 0 || value > 255) {
        return false;
    }
    const uint8_t count = (uint8_t)value;
    if (!sp_mic_count_supported(count)) {
        return false;
    }
    if (out_effective != NULL) {
        *out_effective = count;
    }
    return true;
}
