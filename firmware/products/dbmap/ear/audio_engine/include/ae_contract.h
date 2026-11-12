/**
 * Consumer-facing Audio Engine contract (v1 foundation).
 *
 * Signatures are the public AE product. Consumers (CE, diag, future modules)
 * must use ``ae_signature_t`` + listener/history APIs — not AE internals,
 * PCM buffers, or analyze timing.
 *
 * ## Window identity
 * - ``window_seq``: Platform acquisition sequence (primary continuity key).
 * - ``window_end_mono_us``: acquisition-tied end time (CE clock). Never use
 *   analyze completion or task wall time as a substitute.
 *
 * ## Quality
 * - ``quality`` + ``flags`` + feature ``*_valid`` bits are authoritative.
 * - Do not invent confidence from absent fields.
 *
 * ## Stereo guarantees (DIN0 Mic0/Mic1) — not TDOA
 * Guaranteed when Platform stereo + AE stereo_ok:
 * - Channel identity: mic_mask bit0=mic0 (left slot), bit1=mic1 (right slot)
 *   per Platform/IDF slot contract (physical L/R strap still needs Test A).
 * - Window coherence: same ``window_seq`` / shared measure deadline for the pair.
 * - Timestamp relationship: pair metrics use the same ``window_end_mono_us``.
 * - Availability: ``pair_valid`` / stereo state reflect presence; missing channels
 *   are never fabricated as silence OK.
 *
 * Not guaranteed:
 * - Inter-sample phase sync, cross-correlation, TDOA/DOA, DIN0↔DIN1 coherence.
 */
#pragma once

#include "ae_engine.h"
#include "ae_signature.h"
#include "ae_stereo.h"
#include "ae_time.h"
#include "ae_types.h"
