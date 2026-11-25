#include "../sp_mic_frame_cursor.h"
#include "sp_audio_metrics.h"
#include "sp_i2s_stereo.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

enum { TARGET = 16000, BATCH = 256, WINDOWS = 4 };

static void test_groups(uint8_t channels)
{
    sp_mic_frame_cursor_t cursors[2] = {0};
    uint32_t scratch[2][BATCH * 2];
    int32_t pcm[4][TARGET];
    const uint8_t dins = (channels + 1u) / 2u;
    unsigned next[2] = {0};
    unsigned read_count[2] = {0};
    unsigned carried_windows = 0;
    for (unsigned win = 0; win < WINDOWS; ++win) {
        const unsigned carry = sp_mic_frame_group(cursors, dins, TARGET);
        carried_windows += carry > 0u;
        unsigned used = 0;
        double squared[4] = {0};
        int32_t peak[4] = {0};
        while (used < TARGET) {
            const uint16_t group = (uint16_t)sp_mic_frame_group(cursors, dins, TARGET - used);
            for (uint8_t din = 0; din < dins; ++din) {
                sp_mic_frame_cursor_t *cur = &cursors[din];
                const uint8_t words = sp_i2s_channels_on_din(channels, din);
                if (group == 0) {
                    if (cur->frames != 0) {
                        continue;
                    }
                    for (unsigned i = 0; i < BATCH; ++i) {
                        for (uint8_t slot = 0; slot < words; ++slot) {
                            const unsigned ch = sp_i2s_mic_index_for_din_slot(din, slot);
                            scratch[din][i * words + slot] = (next[din] + i) * 4 + ch;
                        }
                    }
                    next[din] += BATCH;
                    ++read_count[din];
                    const sp_mic_frame_read_t read = sp_mic_frame_read(
                        cur, BATCH * words * sizeof(uint32_t), words * sizeof(uint32_t), true
                    );
                    assert(!read.incomplete_frame && read.discarded_frames == 0);
                    continue;
                }
                assert(cur->first_frame == win * TARGET + used);
                const uint8_t *bytes = (const uint8_t *)scratch[din] + cur->offset;
                for (unsigned i = 0; i < group; ++i) {
                    for (uint8_t slot = 0; slot < words; ++slot) {
                        const unsigned ch = sp_i2s_mic_index_for_din_slot(din, slot);
                        const int32_t value = (int32_t)sp_mic_frame_slot(
                            bytes + (i * words + slot) * sizeof(uint32_t)
                        );
                        assert(value == (int32_t)((win * TARGET + used + i) * 4 + ch));
                        pcm[ch][used + i] = value;
                        squared[ch] += (double)value * value;
                        peak[ch] = value;
                    }
                }
                sp_mic_frame_consume(cur, group, words * sizeof(uint32_t));
            }
            used += group;
        }
        for (uint8_t ch = 0; ch < channels; ++ch) {
            const int32_t rms_proxy = (int32_t)(sqrt(squared[ch] / TARGET) + 0.5);
            const float rms = sp_audio_rms_dbfs(pcm[ch], TARGET, 24);
            assert(fabsf(rms - sp_audio_rms_dbfs(&rms_proxy, 1, 24)) < 0.001f);
            assert(sp_audio_peak_dbfs(pcm[ch], TARGET, 24) == sp_audio_peak_dbfs(&peak[ch], 1, 24));
        }
        for (uint8_t din = 0; din < dins; ++din) {
            assert(next[din] == (win + 1u) * TARGET + cursors[din].frames);
            assert(read_count[din] == ((win + 1u) * TARGET + BATCH - 1u) / BATCH);
        }
    }
    assert(carried_windows == 2u);
}

static void test_reads_and_recovery(void)
{
    sp_mic_frame_cursor_t cur = {0};
    /* A timed-out read with complete frames is usable. A trailing L is not. */
    sp_mic_frame_read_t read = sp_mic_frame_read(&cur, 20, 8, true);
    assert(read.incomplete_frame && cur.frames == 2 && cur.skip_bytes == 4);
    sp_mic_frame_consume(&cur, 2, 8);
    read = sp_mic_frame_read(&cur, 20, 8, true);
    assert(!read.incomplete_frame && cur.offset == 4 && cur.frames == 2);
    assert(cur.first_frame == 3 && cur.next_frame == 5);
    sp_mic_frame_consume(&cur, 2, 8);
    assert(sp_mic_frame_read(&cur, 16, 8, false).discarded_frames == 2);
    assert(cur.frames == 0 && cur.next_frame == 7);
    assert(!sp_mic_frame_read(&cur, 0, 8, true).incomplete_frame);
    assert(cur.next_frame == 7);
    assert(sp_mic_frame_read(&cur, 3, 8, true).incomplete_frame);
    assert(cur.skip_bytes == 5 && cur.next_frame == 8);
    assert(!sp_mic_frame_read(&cur, 2, 8, true).incomplete_frame);
    assert(cur.skip_bytes == 3 && cur.frames == 0);
    assert(!sp_mic_frame_read(&cur, 11, 8, true).incomplete_frame);
    assert(cur.offset == 3 && cur.frames == 1 && cur.first_frame == 8);

    sp_mic_frame_cursor_t peer = {0};
    (void)sp_mic_frame_read(&peer, 160, 8, true);
    assert(sp_mic_frame_align(&peer, cur.first_frame, 8) == 8);
    assert(peer.first_frame == cur.first_frame && peer.frames == 12);
    assert(sp_mic_frame_stop(249, 0, 500, 250) == SP_MIC_SPAN_COMPLETE);
    assert(sp_mic_frame_stop(250, 0, 500, 250) == SP_MIC_SPAN_STALLED);
    assert(sp_mic_frame_stop(500, 499, 500, 250) == SP_MIC_SPAN_DEADLINE);
    assert(strcmp(sp_audio_window_status(0, TARGET, 0, 1, 1), "error") == 0);
    assert(strcmp(sp_audio_window_status(200, TARGET, 0, 1, 0), "error") == 0);
    assert(strcmp(sp_audio_window_status(9000, TARGET, 0, 1, 0), "degraded") == 0);
}

static void test_short_then_empty_then_resume(void)
{
    sp_mic_frame_cursor_t cursors[2] = {0};
    const uint16_t target = 10;
    (void)sp_mic_frame_read(&cursors[0], 8 * 8, 8, true);
    (void)sp_mic_frame_read(&cursors[1], 4 * 4, 4, true);
    uint16_t count = (uint16_t)sp_mic_frame_group(cursors, 2, target);
    assert(count == 4);
    sp_mic_frame_consume(&cursors[0], count, 8);
    sp_mic_frame_consume(&cursors[1], count, 4);

    /* DIN1 stops. The short attempt ends with four paired frames. */
    assert(sp_mic_frame_stop(250, 0, 500, 250) == SP_MIC_SPAN_STALLED);
    assert(sp_mic_frame_group(cursors, 2, target - count) == 0);
    assert(cursors[0].first_frame == 4 && cursors[0].frames == 4);
    /* A following empty attempt consumes nothing and leaves the peer suffix. */
    (void)sp_mic_frame_read(&cursors[1], 0, 4, true);
    assert(sp_mic_frame_group(cursors, 2, target) == 0);
    assert(cursors[0].frames == 4);

    (void)sp_mic_frame_read(&cursors[1], 8 * 4, 4, true);
    assert(cursors[0].first_frame == cursors[1].first_frame);
    count = (uint16_t)sp_mic_frame_group(cursors, 2, target);
    assert(count == 4);
    sp_mic_frame_consume(&cursors[0], count, 8);
    sp_mic_frame_consume(&cursors[1], count, 4);
    assert(cursors[0].next_frame == 8 && cursors[1].first_frame == 8);
}

int main(void)
{
    uint32_t target = 0;
    assert(sp_mic_frame_target(16000, 1000, &target) && target == TARGET);
    assert(sp_mic_frame_target(44100, 1000, &target) && target == 44100);
    assert(!sp_mic_frame_target(44100, 101, &target));
    assert(!sp_mic_frame_target(UINT32_MAX, UINT32_MAX, &target));
    assert(!sp_mic_frame_target(0, 1000, &target));
    for (uint8_t channels = 1; channels <= 4; ++channels) {
        test_groups(channels);
    }
    test_reads_and_recovery();
    test_short_then_empty_then_resume();
    puts("frame assembly: 1–4 channels, carry, fragments, transport and stalls PASS");
    return 0;
}
