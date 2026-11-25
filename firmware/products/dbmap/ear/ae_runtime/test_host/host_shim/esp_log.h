#pragma once

#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#if CONFIG_DBMAP_EAR_AE_DIAG
#include <stdio.h>
/* Type-check diagnostic format strings without flooding test output. */
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#else
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#endif
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGV(tag, fmt, ...) ((void)(tag))
