#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define DBMAP_AUDIO_MEDIA_MOUNT "/media"

/* Mount LittleFS media partition, init I2S for MAX98357. hub_http_url may be NULL. */
esp_err_t dbmap_audio_init(const char *hub_http_url);

/* Play a file under the media mount (absolute "/media/..." or relative "alert.mp3"). */
esp_err_t dbmap_audio_play_file(const char *path);

/* Stream an MP3 over HTTPS. Hub-local URLs use the Hub CA; others use the IDF cert bundle. */
esp_err_t dbmap_audio_play_https(const char *url);

esp_err_t dbmap_audio_stop(void);
bool dbmap_audio_is_playing(void);
