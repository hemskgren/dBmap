/**
 * Candidate Engine log macros. Tag is "ce".
 * Host builds do not pull ESP-IDF; device builds log lifecycle events only.
 */
#pragma once

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define CE_LOGI(fmt, ...) ESP_LOGI("ce", fmt, ##__VA_ARGS__)
#define CE_LOGW(fmt, ...) ESP_LOGW("ce", fmt, ##__VA_ARGS__)
#else
#define CE_LOGI(...) ((void)0)
#define CE_LOGW(...) ((void)0)
#endif
