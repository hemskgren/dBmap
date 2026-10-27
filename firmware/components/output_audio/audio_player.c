#include "dbmap_audio.h"

#include <stdio.h>
#include <string.h>

#include "audio_amp.h"
#include "audio_media.h"
#include "dbmap_hub_http.h"
#include "dbmap_i2s.h"
#include "dbmap_types.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3dec.h"

static const char *TAG = "dbmap_audio";

enum {
    kSrcNone = 0,
    kSrcFile,
    kSrcHttps,
};

enum {
    kReadBuf = 4096,
    kPcmMax = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP,
};

typedef struct {
    int kind;
    char path_or_url[384];
} play_job_t;

static char s_hub_http_url[DBMAP_HOST_MAX];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_playing;
static volatile bool s_stop;
static play_job_t s_job;

static bool url_is_hub(const char *url)
{
    if (s_hub_http_url[0] == '\0' || url == NULL) {
        return false;
    }
    size_t n = strlen(s_hub_http_url);
    return strncmp(url, s_hub_http_url, n) == 0;
}

static void resolve_media_path(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL || out_len == 0) {
        return;
    }
    if (path[0] == '/') {
        snprintf(out, out_len, "%s", path);
        return;
    }
    snprintf(out, out_len, "%s/%s", DBMAP_AUDIO_MEDIA_MOUNT, path);
}

/* Returns header+size length from an ID3v2 header at buf[0..9], or 0 if not ID3. */
static int id3v2_total_len(const unsigned char *buf, int len)
{
    if (len < 10 || memcmp(buf, "ID3", 3) != 0) {
        return 0;
    }
    int size = ((buf[6] & 0x7f) << 21) | ((buf[7] & 0x7f) << 14) | ((buf[8] & 0x7f) << 7) | (buf[9] & 0x7f);
    return 10 + size;
}

static esp_err_t write_pcm_frame(const short *pcm, const MP3FrameInfo *info)
{
    if (info->nChans == 2) {
        return dbmap_i2s_write_pcm(pcm, (size_t)info->outputSamps);
    }
    /* Mono → stereo for MAX98357. */
    int frames = info->outputSamps;
    int16_t stereo[kPcmMax];
    if (frames * 2 > kPcmMax) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < frames; i++) {
        stereo[i * 2] = pcm[i];
        stereo[i * 2 + 1] = pcm[i];
    }
    return dbmap_i2s_write_pcm(stereo, (size_t)(frames * 2));
}

typedef struct {
    FILE *file;
    esp_http_client_handle_t http;
    unsigned char buf[kReadBuf];
    int avail;
    int offset;
    bool eof;
} source_t;

static esp_err_t source_fill(source_t *src)
{
    if (src->avail - src->offset > 0) {
        memmove(src->buf, src->buf + src->offset, (size_t)(src->avail - src->offset));
        src->avail -= src->offset;
        src->offset = 0;
    } else {
        src->avail = 0;
        src->offset = 0;
    }

    int space = kReadBuf - src->avail;
    if (space <= 0) {
        return ESP_OK;
    }

    int n = 0;
    if (src->file != NULL) {
        n = (int)fread(src->buf + src->avail, 1, (size_t)space, src->file);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            src->eof = true;
        }
    } else if (src->http != NULL) {
        n = esp_http_client_read(src->http, (char *)src->buf + src->avail, space);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            src->eof = true;
        }
    } else {
        return ESP_ERR_INVALID_STATE;
    }
    src->avail += n;
    return ESP_OK;
}

static esp_err_t decode_stream(source_t *src)
{
    HMP3Decoder decoder = MP3InitDecoder();
    if (decoder == NULL) {
        return ESP_ERR_NO_MEM;
    }

    short pcm[kPcmMax];
    bool rate_set = false;
    bool id3_checked = false;
    int id3_remaining = 0;
    esp_err_t result = ESP_OK;

    while (!s_stop) {
        if (src->avail - src->offset < 2048 && !src->eof) {
            result = source_fill(src);
            if (result != ESP_OK) {
                break;
            }
        }
        if (src->avail - src->offset <= 0) {
            break;
        }

        unsigned char *ptr = src->buf + src->offset;
        int left = src->avail - src->offset;

        if (id3_remaining > 0) {
            int n = id3_remaining < left ? id3_remaining : left;
            src->offset += n;
            id3_remaining -= n;
            continue;
        }
        if (!id3_checked) {
            int total = id3v2_total_len(ptr, left);
            if (total > 0) {
                int n = total < left ? total : left;
                src->offset += n;
                id3_remaining = total - n;
                id3_checked = true;
                continue;
            }
            if (left >= 10 || src->eof) {
                id3_checked = true;
            } else {
                continue;
            }
        }

        int sync = MP3FindSyncWord(ptr, left);
        if (sync < 0) {
            if (src->eof) {
                break;
            }
            /* Keep a few bytes in case a sync word straddles the buffer edge. */
            if (left > 2) {
                src->offset += left - 2;
            }
            continue;
        }
        ptr += sync;
        left -= sync;
        src->offset += sync;

        unsigned char *before = ptr;
        int err = MP3Decode(decoder, &ptr, &left, pcm, 0);
        src->offset += (int)(ptr - before);

        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (src->eof) {
                break;
            }
            continue;
        }
        if (err != ERR_MP3_NONE) {
            if (src->offset < src->avail) {
                src->offset++;
            }
            continue;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(decoder, &info);
        if (!rate_set) {
            result = dbmap_i2s_set_sample_rate(info.samprate);
            if (result != ESP_OK) {
                break;
            }
            rate_set = true;
            ESP_LOGI(TAG, "MP3 %d Hz %d ch %d kbps", info.samprate, info.nChans, info.bitrate / 1000);
        }
        result = write_pcm_frame(pcm, &info);
        if (result != ESP_OK) {
            if (result == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "I2S output stalled; aborting playback");
            }
            break;
        }
    }

    MP3FreeDecoder(decoder);
    dbmap_i2s_tx_clear();
    if (s_stop) {
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t play_file_job(const char *path)
{
    char resolved[384];
    resolve_media_path(path, resolved, sizeof(resolved));
    FILE *f = fopen(resolved, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", resolved);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "playing file %s", resolved);
    source_t src = { .file = f };
    esp_err_t err = decode_stream(&src);
    fclose(f);
    return err;
}

static esp_err_t play_https_job(const char *url)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .buffer_size = kReadBuf,
    };
    if (url_is_hub(url)) {
        http_cfg.cert_pem = dbmap_hub_root_ca();
    } else {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "HTTPS open failed (%s)", esp_err_to_name(err));
        return err;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "HTTPS play HTTP %d", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "streaming %s", url);
    source_t src = { .http = client };
    err = decode_stream(&src);
    esp_http_client_cleanup(client);
    return err;
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        play_job_t job;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            job = s_job;
            s_playing = true;
            s_stop = false;
            xSemaphoreGive(s_lock);
        } else {
            continue;
        }

        esp_err_t err = ESP_OK;
        if (job.kind == kSrcFile) {
            err = play_file_job(job.path_or_url);
        } else if (job.kind == kSrcHttps) {
            err = play_https_job(job.path_or_url);
        }

        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_playing = false;
            s_job.kind = kSrcNone;
            xSemaphoreGive(s_lock);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "playback finished");
        } else if (!s_stop) {
            ESP_LOGW(TAG, "playback ended (%s)", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "playback stopped");
        }
    }
}

static esp_err_t enqueue_job(int kind, const char *path_or_url)
{
    if (path_or_url == NULL || path_or_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(path_or_url) >= sizeof(s_job.path_or_url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)dbmap_audio_stop();

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_job.kind = kind;
    snprintf(s_job.path_or_url, sizeof(s_job.path_or_url), "%s", path_or_url);
    s_stop = false;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t dbmap_audio_init(const char *hub_http_url)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (hub_http_url != NULL) {
        snprintf(s_hub_http_url, sizeof(s_hub_http_url), "%s", hub_http_url);
    }

    esp_err_t err = dbmap_media_mount();
    if (err != ESP_OK) {
        return err;
    }
    err = dbmap_amp_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(player_task, "audio_play", 12288, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio ready (local %s + HTTPS stream)", DBMAP_AUDIO_MEDIA_MOUNT);
    return ESP_OK;
}

esp_err_t dbmap_audio_play_file(const char *path)
{
    return enqueue_job(kSrcFile, path);
}

esp_err_t dbmap_audio_play_https(const char *url)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_job(kSrcHttps, url);
}

esp_err_t dbmap_audio_stop(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop = true;
    /* Wait briefly for the player to leave the playing state. */
    for (int i = 0; i < 50; i++) {
        if (!s_playing) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_playing ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool dbmap_audio_is_playing(void)
{
    return s_playing;
}
