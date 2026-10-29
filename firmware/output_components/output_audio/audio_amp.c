#include "audio_amp.h"

#include "dbmap_i2s.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "dbmap_amp";

esp_err_t dbmap_amp_init(void)
{
#if CONFIG_DBMAP_AUDIO_SD_MODE_GPIO >= 0
    gpio_config_t sd = {
        .pin_bit_mask = 1ULL << CONFIG_DBMAP_AUDIO_SD_MODE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "SD_MODE gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(CONFIG_DBMAP_AUDIO_SD_MODE_GPIO, 1), TAG, "SD_MODE high");
#endif

    dbmap_i2s_pins_t pins = {
        .bclk_gpio = CONFIG_DBMAP_AUDIO_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_DBMAP_AUDIO_I2S_WS_GPIO,
        .dout_gpio = CONFIG_DBMAP_AUDIO_I2S_DOUT_GPIO,
        .din_gpio = -1,
        .mclk_gpio = -1,
    };
    esp_err_t err = dbmap_i2s_init_tx(&pins, 44100);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(
        TAG,
        "MAX98357 ready bclk=%d ws=%d din=%d",
        CONFIG_DBMAP_AUDIO_I2S_BCLK_GPIO,
        CONFIG_DBMAP_AUDIO_I2S_WS_GPIO,
        CONFIG_DBMAP_AUDIO_I2S_DOUT_GPIO
    );
    return ESP_OK;
}
