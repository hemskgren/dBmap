#pragma once

#include "sp_mic_window_job_slot.h"

#include <stdbool.h>
#include <stdint.h>

void host_mic_begin(bool ready, uint32_t sample_rate_hz);
int host_mic_begin_fill(void);
void host_mic_on_second_ready_check(void (*hook)(void));
void host_mic_set_ready(bool ready);
int32_t *host_mic_bank(uint8_t ch, int buf);
const int32_t *host_mic_last_pcm(void);
uint32_t host_mic_pcm_n(void);

/** Publish using the reserved fill bank. */
int host_mic_publish(const sp_mic_window_slot_pending_t *in);
