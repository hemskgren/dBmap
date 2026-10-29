#include "mic_model.h"

#include "dbmap_audio_metrics.h"
#include "sdkconfig.h"

static const dbmap_ear_mic_model_desc_t k_inmp441 = {
    .model = DBMAP_EAR_MIC_MODEL_INMP441,
    .id = "INMP441",
    .slot_bit_width = 32,
    .useful_bits = 24,
    .extract = dbmap_inmp441_extract_sample,
};

const dbmap_ear_mic_model_desc_t *dbmap_ear_mic_model_desc(dbmap_ear_mic_model_t model)
{
    if (model == DBMAP_EAR_MIC_MODEL_INMP441) {
        return &k_inmp441;
    }
    return NULL;
}

const dbmap_ear_mic_model_desc_t *dbmap_ear_mic_model_selected(void)
{
#if CONFIG_DBMAP_EAR_MIC_MODEL_INMP441
    return &k_inmp441;
#else
    /* Keep a single default so the component always links a model. */
    return &k_inmp441;
#endif
}
