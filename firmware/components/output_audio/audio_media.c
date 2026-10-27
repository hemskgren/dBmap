#include "audio_media.h"

#include "dbmap_audio.h"
#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "dbmap_media";
static bool s_mounted;

esp_err_t dbmap_media_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = DBMAP_AUDIO_MEDIA_MOUNT,
        .partition_label = "media",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed (%s)", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_littlefs_info("media", &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "media LittleFS mounted at %s (%u/%u bytes used)", DBMAP_AUDIO_MEDIA_MOUNT, (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "media LittleFS mounted at %s", DBMAP_AUDIO_MEDIA_MOUNT);
    }
    s_mounted = true;
    return ESP_OK;
}
