#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Shared I2S std/Philips helpers. Pins are board-specific; pass I2S_GPIO_UNUSED (-1) for unused lines. */

typedef struct {
    int bclk_gpio;
    int ws_gpio;
    int dout_gpio; /* TX data; unused for RX-only */
    int din_gpio;  /* RX data; unused for TX-only */
    int mclk_gpio; /* optional; pass -1 if unused */
} dbmap_i2s_pins_t;

esp_err_t dbmap_i2s_init_tx(const dbmap_i2s_pins_t *pins, int sample_rate_hz);
esp_err_t dbmap_i2s_init_rx(const dbmap_i2s_pins_t *pins, int sample_rate_hz);

esp_err_t dbmap_i2s_set_sample_rate(int rate_hz);
esp_err_t dbmap_i2s_write_pcm(const int16_t *samples, size_t sample_count);
esp_err_t dbmap_i2s_read_pcm(int16_t *samples, size_t sample_count, size_t *out_count, uint32_t timeout_ms);

/* Disable/enable TX to drop residual samples without tearing down the channel. */
void dbmap_i2s_tx_clear(void);
