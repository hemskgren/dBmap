#pragma once

#include "esp_err.h"

/* MAX98357 SD_MODE (optional) + I2S TX via node_common. */
esp_err_t dbmap_amp_init(void);
