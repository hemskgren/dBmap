#pragma once

#include <stdint.h>

#include "sp_mic.h"

typedef struct {
    sp_mic_model_t model;
    const char *id;
    int slot_bit_width;
    int useful_bits;
    int32_t (*extract)(uint32_t slot_word);
} sp_mic_model_desc_t;

const sp_mic_model_desc_t *sp_mic_model_desc(sp_mic_model_t model);
const sp_mic_model_desc_t *sp_mic_model_selected(void);
