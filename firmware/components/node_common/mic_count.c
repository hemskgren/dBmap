#include "dbmap_mic_count.h"

#include <stddef.h>

const char *dbmap_mic_count_status_str(dbmap_mic_count_status_t status)
{
    switch (status) {
    case DBMAP_MIC_COUNT_STATUS_OK:
        return "ok";
    case DBMAP_MIC_COUNT_STATUS_DEFAULTED:
        return "defaulted";
    case DBMAP_MIC_COUNT_STATUS_INVALID_FALLBACK:
        return "invalid_fallback";
    default:
        return "invalid_fallback";
    }
}

bool dbmap_mic_count_supported(uint8_t count)
{
    return count >= DBMAP_MIC_COUNT_MIN && count <= DBMAP_MIC_COUNT_MAX;
}

void dbmap_mic_count_from_nvs(
    bool found,
    uint8_t raw,
    uint8_t *out_effective,
    dbmap_mic_count_status_t *out_status
)
{
    uint8_t effective = DBMAP_MIC_COUNT_DEFAULT;
    dbmap_mic_count_status_t status = DBMAP_MIC_COUNT_STATUS_DEFAULTED;

    if (found) {
        if (dbmap_mic_count_supported(raw)) {
            effective = raw;
            status = DBMAP_MIC_COUNT_STATUS_OK;
        } else {
            effective = DBMAP_MIC_COUNT_DEFAULT;
            status = DBMAP_MIC_COUNT_STATUS_INVALID_FALLBACK;
        }
    }

    if (out_effective != NULL) {
        *out_effective = effective;
    }
    if (out_status != NULL) {
        *out_status = status;
    }
}

bool dbmap_mic_count_from_json_int(int value, uint8_t *out_effective)
{
    if (value < 0 || value > 255) {
        return false;
    }
    const uint8_t count = (uint8_t)value;
    if (!dbmap_mic_count_supported(count)) {
        return false;
    }
    if (out_effective != NULL) {
        *out_effective = count;
    }
    return true;
}
