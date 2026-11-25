/**
 * Host façade for the public microphone job API.
 * Slot publish, take, and projection are the production helpers.
 * PCM banks and the fill index stand in for ear_mic.c storage.
 */
#include "sp_mic_pcm.h"
#include "sp_mic_window_job_slot.h"

#include <string.h>

#if CONFIG_DBMAP_EAR_AE_DIAG
#include "sp_mic_bank_diag.h"

static sp_mic_bank_generation_t s_generations[2];

uint32_t sp_mic_pcm_job_bank_generation(const sp_mic_pcm_job_t *job)
{
    return sp_mic_bank_generation(&s_generations[job->bank_index]);
}
#endif

#define HOST_PCM_N 128u

static int32_t s_storage[SP_MIC_COUNT_MAX][2][HOST_PCM_N];
static int32_t *s_banks[SP_MIC_COUNT_MAX][2];
static int s_fill_idx;
static unsigned s_ready_checks;
static void (*s_on_ready_check)(void);
#if CONFIG_DBMAP_EAR_AE_DIAG
static uint32_t s_fill_generation;
#endif
static bool s_ready;
static bool s_banks_ready;
static uint32_t s_rate_hz;
static sp_mic_pcm_ready_hook_t s_hook;
static const int32_t *s_last_pcm;

static void ensure_banks(void)
{
    if (s_banks_ready) {
        return;
    }
    for (uint8_t ch = 0u; ch < SP_MIC_COUNT_MAX; ++ch) {
        for (int b = 0; b < 2; ++b) {
            s_banks[ch][b] = s_storage[ch][b];
        }
    }
    s_banks_ready = true;
}

uint32_t host_mic_pcm_n(void)
{
    return HOST_PCM_N;
}

void host_mic_begin(bool ready, uint32_t sample_rate_hz)
{
    ensure_banks();
    sp_mic_window_slot_reset_pending();
    s_fill_idx = -1;
    s_on_ready_check = NULL;
    memset(s_storage, 0, sizeof(s_storage));
    s_rate_hz = sample_rate_hz;
    s_last_pcm = NULL;
    s_ready = ready;
    if (ready && s_hook != NULL) {
        s_hook(sample_rate_hz);
    }
}

int host_mic_begin_fill(void)
{
    s_fill_idx = sp_mic_window_slot_begin_fill();
#if CONFIG_DBMAP_EAR_AE_DIAG
    if (s_fill_idx >= 0) {
        s_fill_generation = sp_mic_bank_write_start(&s_generations[s_fill_idx]);
    }
#endif
    return s_fill_idx;
}

void host_mic_on_second_ready_check(void (*hook)(void))
{
    s_on_ready_check = hook;
    s_ready_checks = 2;
}

void host_mic_set_ready(bool ready)
{
    s_ready = ready;
}

int32_t *host_mic_bank(uint8_t ch, int buf)
{
    return s_banks[ch][buf];
}

const int32_t *host_mic_last_pcm(void)
{
    return s_last_pcm;
}

int host_mic_publish(const sp_mic_window_slot_pending_t *in)
{
    if (in == NULL) {
        return -1;
    }
    sp_mic_window_slot_pending_t pub = *in;
    pub.buf_idx = s_fill_idx;
#if CONFIG_DBMAP_EAR_AE_DIAG
    pub.bank_generation = s_fill_generation;
#endif
    s_fill_idx = -1;
    return sp_mic_window_slot_publish(&pub) ? pub.buf_idx : -1;
}

void sp_mic_set_pcm_ready_hook(sp_mic_pcm_ready_hook_t hook)
{
    s_hook = hook;
}

bool sp_mic_pcm_ready(void)
{
    if (s_on_ready_check != NULL && --s_ready_checks == 0) {
        void (*hook)(void) = s_on_ready_check;
        s_on_ready_check = NULL;
        hook();
    }
    return s_ready;
}

bool sp_mic_release_pcm_job(uint64_t token)
{
    return sp_mic_window_slot_release(token);
}

#if CONFIG_DBMAP_EAR_AE_DIAG
void sp_mic_note_legacy_generation_mismatch(void)
{
    sp_mic_window_slot_note_legacy_generation_mismatch();
}
#endif

bool sp_mic_take_pcm_job(sp_mic_pcm_job_t *out)
{
    if (!s_ready) {
        return false;
    }
    sp_mic_window_slot_pending_t job;
    const bool had = sp_mic_window_slot_take(&job, out == NULL);
    if (!had) {
        return false;
    }
    const bool ok = out == NULL || sp_mic_window_slot_project_pcm(&job, s_banks, out);
    sp_mic_window_slot_note_projection(out == NULL ? 0u : job.lease_token, true, ok);
    if (ok && out != NULL) {
        s_last_pcm = out->samples;
    }
    return ok;
}

bool sp_mic_take_window_job(sp_mic_window_job_t *out)
{
    return sp_mic_take_window_job_result(out) == SP_MIC_WINDOW_PROJECTED;
}

sp_mic_window_take_result_t sp_mic_take_window_job_result(sp_mic_window_job_t *out)
{
    if (!s_ready) {
        return SP_MIC_WINDOW_EMPTY;
    }
    sp_mic_window_slot_pending_t job;
    const uint32_t rate = s_rate_hz;
    const bool had = sp_mic_window_slot_take(&job, out == NULL);
    if (!had) {
        return SP_MIC_WINDOW_EMPTY;
    }
    const bool ok = out == NULL || sp_mic_window_slot_project_window(&job, s_banks, rate, out);
    if (ok && out != NULL) {
        s_last_pcm = out->pcm.samples;
    }
    sp_mic_window_slot_note_projection(out == NULL ? 0u : job.lease_token, false, ok);
    return ok ? SP_MIC_WINDOW_PROJECTED : SP_MIC_WINDOW_PROJECTION_FAILED;
}

uint8_t sp_mic_pcm_retained_channels(void)
{
    return s_ready ? 1u : 0u;
}

uint32_t sp_mic_pcm_pending_overwrite(void)
{
    return sp_mic_window_slot_overwrite_count();
}

uint32_t sp_mic_pcm_cap_samples(void)
{
    return HOST_PCM_N;
}

uint32_t sp_mic_pcm_bytes_total(void)
{
    return HOST_PCM_N * (uint32_t)sizeof(int32_t) * 2u;
}

uint32_t sp_mic_pcm_free_heap_after_init(void)
{
    return 0u;
}

uint32_t sp_mic_pcm_free_spiram_after_init(void)
{
    return 0u;
}
