/* AUTO-GENERATED pre-migration golden fixtures. Do not edit by hand. */
/* Tolerances: abs 1e-4 dB/features for near-zero; rel 1e-5 for nonzero floats. */
#pragma once
#include <stdint.h>

#define SP_GOLDEN_SR_HZ 16000
#define SP_GOLDEN_N 16000
#define SP_GOLDEN_FS_BITS 24

/* case tone_1k amp0.25 */
static const float SP_GOLDEN_TONE_L0_RMS = -15.0514994f;
static const float SP_GOLDEN_TONE_L0_PEAK = -12.0412006f;
static const float SP_GOLDEN_TONE_L0_CREST = 3.01029873f;
static const float SP_GOLDEN_TONE_L0_BAND[4] = {-39.3289375f, -18.0789356f, -18.0705948f, -43.1071281f};
static const float SP_GOLDEN_TONE_L0_HL = -0.0105081685f;
static const int SP_GOLDEN_TONE_L0_HL_VALID = 1;
static const float SP_GOLDEN_TONE_L1_CENT = 1000.01483f;
static const float SP_GOLDEN_TONE_L1_BW = 13.0808125f;
static const float SP_GOLDEN_TONE_L1_DOM = 1000.0f;
static const float SP_GOLDEN_TONE_L1_FLAT = 1.2211829e-05f;
static const int SP_GOLDEN_TONE_L2_ELIG = 1;
static const float SP_GOLDEN_TONE_L2_FLUX = 0.0f;
static const float SP_GOLDEN_TONE_L2_ENV = 0.0f;
/* case tone_250 amp0.5 */
static const float SP_GOLDEN_T250_L0_RMS = -9.0309f;
static const float SP_GOLDEN_T250_L0_PEAK = -6.02060032f;
static const float SP_GOLDEN_T250_L0_CREST = 3.01029968f;
static const float SP_GOLDEN_T250_L0_BAND[4] = {-12.0439034f, -12.0650272f, -33.333931f, -61.300621f};
static const float SP_GOLDEN_T250_L1_CENT = 250.019714f;
static const float SP_GOLDEN_T250_L1_DOM = 250.0f;
static const float SP_GOLDEN_T250_L2_FLUX = 0.0f;
static const float SP_GOLDEN_T250_L2_ENV = 0.0f;
/* case noise seed42 amp0.2 */
static const float SP_GOLDEN_NOISE_L0_RMS = -18.7816353f;
static const float SP_GOLDEN_NOISE_L0_PEAK = -13.9808693f;
static const float SP_GOLDEN_NOISE_L0_CREST = 4.80076599f;
static const float SP_GOLDEN_NOISE_L0_BAND[4] = {-33.9567719f, -28.686264f, -23.1665688f, -21.9978867f};
static const float SP_GOLDEN_NOISE_L1_CENT = 3862.20532f;
static const float SP_GOLDEN_NOISE_L1_FLAT = 0.991705775f;
static const float SP_GOLDEN_NOISE_L2_FLUX = 1.01198542f;
static const float SP_GOLDEN_NOISE_L2_ENV = 0.0152481068f;
