/**
 * UART format contract for CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG.
 *
 * Host-testable string fragments — keep in sync with capture_diag_maybe_log().
 * Fleet images leave the Kconfig off; bench builds use
 * firmware/esp-ear/sdkconfig.defaults.capture_diag.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Summary line: stream-level incomplete stereo count + measure timing. */
#define DBMAP_EAR_MIC_CAPTURE_DIAG_SUMMARY_FMT                                         \
    "capture_diag rate=%d fmt=int32_from_i2s32_left_justified_24 mode=%s "             \
    "words/frame=%lu live=%u dma_desc=%lu dma_frame=%lu "                              \
    "dma_buf_bytes_formula=%lu (mono_formula=%lu) slot_buf=%lu "                       \
    "(mono_equiv=%lu) req_bytes=%lu measure_us=%lu incomplete=%lu "                    \
    "free_heap=%lu free_spiram=%lu"

/**
 * Per-channel line: independent ch index, L/R slot, levels, counts, statuses,
 * and window_seq (same value on both stereo channels for one measure window).
 */
#define DBMAP_EAR_MIC_CAPTURE_DIAG_CH_FMT                                              \
    "capture_diag ch%u slot=%s n=%lu expect=%lu rms=%.1f peak=%.1f "                    \
    "clip=%lu zero=%lu all_zero=%u presence=%s mic=%s xport=%s "                       \
    "timeout=%lu empty=%lu err=%lu window_seq=%lu"

/** Boot banner substring — confirms the running image has capture_diag compiled in. */
#define DBMAP_EAR_MIC_CAPTURE_DIAG_BOOT_MSG                                            \
    "capture_diag UART enabled (~5s): per-ch slot/rms/peak/n/expect/window_seq/"       \
    "mic/xport + stream incomplete (bench only; not fleet default)"

#ifdef __cplusplus
}
#endif
