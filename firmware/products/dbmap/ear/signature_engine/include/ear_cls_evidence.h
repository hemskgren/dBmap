/**
 * Progressive-classification evidence payload (ALARM taxonomy feeder).
 *
 * Shared by Signature Engine AS-002 publish/copy APIs and
 * ear_cls_progressive. Lives in signature_engine so public engine headers
 * do not depend on ear_observation include paths.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool alarm_ok;
    float alarm_confidence;
    bool se_public_alert_ok;
    float se_public_alert_confidence;
    bool faran_over_ok;
    float faran_over_confidence;
    uint32_t faran_over_support_ms;
    bool vma_ok;
    float vma_confidence;
    uint32_t vma_support_ms;
} ear_cls_evidence_t;

#ifdef __cplusplus
}
#endif
