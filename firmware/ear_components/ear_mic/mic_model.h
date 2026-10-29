#pragma once

#include <stdint.h>

#include "dbmap_ear_mic.h"

typedef struct {
    dbmap_ear_mic_model_t model;
    const char *id;
    int slot_bit_width;
    int useful_bits;
    int32_t (*extract)(uint32_t slot_word);
} dbmap_ear_mic_model_desc_t;

const dbmap_ear_mic_model_desc_t *dbmap_ear_mic_model_desc(dbmap_ear_mic_model_t model);
const dbmap_ear_mic_model_desc_t *dbmap_ear_mic_model_selected(void);
