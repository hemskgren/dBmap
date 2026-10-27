#include "dbmap_i2s.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "dbmap_i2s";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static int s_rate_hz;
static bool s_tx_ready;
static bool s_rx_ready;

static int gpio_or_unused(int gpio)
{
    return gpio < 0 ? I2S_GPIO_UNUSED : gpio;
}

static esp_err_t apply_std_mode(i2s_chan_handle_t chan, const dbmap_i2s_pins_t *pins, int sample_rate_hz, bool is_tx)
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = gpio_or_unused(pins->mclk_gpio),
            .bclk = pins->bclk_gpio,
            .ws = pins->ws_gpio,
            .dout = is_tx ? gpio_or_unused(pins->dout_gpio) : I2S_GPIO_UNUSED,
            .din = is_tx ? I2S_GPIO_UNUSED : gpio_or_unused(pins->din_gpio),
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(chan, &std_cfg), TAG, "init std");
    return i2s_channel_enable(chan);
}

esp_err_t dbmap_i2s_init_tx(const dbmap_i2s_pins_t *pins, int sample_rate_hz)
{
    if (pins == NULL || sample_rate_hz <= 0 || pins->bclk_gpio < 0 || pins->ws_gpio < 0 || pins->dout_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_ready) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "new tx channel");
    ESP_RETURN_ON_ERROR(apply_std_mode(s_tx, pins, sample_rate_hz, true), TAG, "tx std");
    s_rate_hz = sample_rate_hz;
    s_tx_ready = true;
    ESP_LOGI(TAG, "I2S TX ready bclk=%d ws=%d dout=%d rate=%d", pins->bclk_gpio, pins->ws_gpio, pins->dout_gpio, sample_rate_hz);
    return ESP_OK;
}

esp_err_t dbmap_i2s_init_rx(const dbmap_i2s_pins_t *pins, int sample_rate_hz)
{
    if (pins == NULL || sample_rate_hz <= 0 || pins->bclk_gpio < 0 || pins->ws_gpio < 0 || pins->din_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rx_ready) {
        return ESP_OK;
    }

    /* Prefer I2S_NUM_1 when TX already owns port 0 (Output amp + future mic on one board). */
    i2s_port_t port = s_tx_ready ? I2S_NUM_1 : I2S_NUM_0;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "new rx channel");
    ESP_RETURN_ON_ERROR(apply_std_mode(s_rx, pins, sample_rate_hz, false), TAG, "rx std");
    if (!s_tx_ready) {
        s_rate_hz = sample_rate_hz;
    }
    s_rx_ready = true;
    ESP_LOGI(TAG, "I2S RX ready bclk=%d ws=%d din=%d rate=%d port=%d", pins->bclk_gpio, pins->ws_gpio, pins->din_gpio, sample_rate_hz, (int)port);
    return ESP_OK;
}

esp_err_t dbmap_i2s_set_sample_rate(int rate_hz)
{
    if (rate_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx_ready && !s_rx_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rate_hz == s_rate_hz) {
        return ESP_OK;
    }

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz);
    if (s_tx_ready) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "tx disable");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG, "tx clock");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
    }
    if (s_rx_ready) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx), TAG, "rx disable");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_rx, &clk), TAG, "rx clock");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");
    }
    s_rate_hz = rate_hz;
    ESP_LOGI(TAG, "I2S sample rate %d Hz", rate_hz);
    return ESP_OK;
}

esp_err_t dbmap_i2s_write_pcm(const int16_t *samples, size_t sample_count)
{
    if (!s_tx_ready || s_tx == NULL || samples == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t bytes = sample_count * sizeof(int16_t);
    size_t written = 0;
    int idle_retries = 0;
    while (written < bytes) {
        size_t n = 0;
        esp_err_t err = i2s_channel_write(
            s_tx,
            (const char *)samples + written,
            bytes - written,
            &n,
            pdMS_TO_TICKS(500)
        );
        if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && n == 0)) {
            idle_retries++;
            if (idle_retries >= 6) {
                ESP_LOGW(
                    TAG,
                    "I2S TX stalled after %u/%u bytes (check MAX98357 BCLK/WS/DIN wiring and power)",
                    (unsigned)written,
                    (unsigned)bytes
                );
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (err != ESP_OK) {
            return err;
        }
        idle_retries = 0;
        written += n;
    }
    return ESP_OK;
}

esp_err_t dbmap_i2s_read_pcm(int16_t *samples, size_t sample_count, size_t *out_count, uint32_t timeout_ms)
{
    if (!s_rx_ready || s_rx == NULL || samples == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t bytes = sample_count * sizeof(int16_t);
    size_t n = 0;
    esp_err_t err = i2s_channel_read(s_rx, samples, bytes, &n, pdMS_TO_TICKS(timeout_ms));
    if (out_count != NULL) {
        *out_count = n / sizeof(int16_t);
    }
    return err;
}

void dbmap_i2s_tx_clear(void)
{
    if (!s_tx_ready || s_tx == NULL) {
        return;
    }
    (void)i2s_channel_disable(s_tx);
    (void)i2s_channel_enable(s_tx);
}
