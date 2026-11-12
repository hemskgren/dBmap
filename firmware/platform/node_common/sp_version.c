#include "sp_version.h"

#include "esp_log.h"

/* SP_VERSION_STRING comes from CMake (firmware/platform/VERSION). */
#ifndef SP_VERSION_STRING
#include "sp_version_gen.h"
#endif

static const char *TAG = "sp";

const char *sp_version_string(void)
{
    return SP_VERSION_STRING;
}

void sp_version_log_boot(void)
{
    ESP_LOGI(TAG, "version=%s (Sensor Platform; independent of product firmware_version)",
             SP_VERSION_STRING);
}
