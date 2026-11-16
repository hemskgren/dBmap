/**
 * Host-side acoustic profile extraction: WAV → Platform L0/L1/L2 → ae_signature_t.
 *
 * Uses the same DSP implementations as firmware (sp_acoustic_* + ae_signature_build).
 * Does not invent feature values. Mono WAV only (no pair metrics).
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -DCONFIG_SP_AUDIO_L0=1 -DCONFIG_SP_AUDIO_L1=1 -DCONFIG_SP_AUDIO_L2=1 \
 *     -I firmware/platform/audio/include \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     firmware/platform/audio/sp_acoustic_l0.c \
 *     firmware/platform/audio/sp_acoustic_l1.c \
 *     firmware/platform/audio/sp_acoustic_l2.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/as_profile_extract.c \
 *     -lm -o /tmp/as_profile_extract
 *
 *   /tmp/as_profile_extract <mono16k.wav> <out.csv>
 *
 * Requires PCM s16le mono at exactly 16000 Hz, non-overlapping 1000 ms windows.
 */
#include "ae_signature.h"
#include "sp_acoustic_l0.h"
#include "sp_acoustic_l1.h"
#include "sp_acoustic_l2.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SR = 16000, WIN_MS = 1000, N_SAMP = 16000, FS_BITS = 16 };

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int load_mono16k_wav(const char *path, int16_t **out_pcm, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "open failed: %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    const long file_n = ftell(f);
    if (file_n < 44) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    uint8_t *file = (uint8_t *)malloc((size_t)file_n);
    if (file == NULL || fread(file, 1, (size_t)file_n, f) != (size_t)file_n) {
        free(file);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "not RIFF/WAVE: %s\n", path);
        free(file);
        return -1;
    }

    size_t pos = 12;
    const uint8_t *data = NULL;
    size_t data_n = 0;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    while (pos + 8 <= (size_t)file_n) {
        const uint8_t *ch = file + pos;
        const uint32_t sz = rd_u32(ch + 4);
        pos += 8;
        if (pos + sz > (size_t)file_n) {
            break;
        }
        if (memcmp(ch, "fmt ", 4) == 0 && sz >= 16) {
            audio_format = rd_u16(file + pos);
            channels = rd_u16(file + pos + 2);
            sample_rate = rd_u32(file + pos + 4);
            bits = rd_u16(file + pos + 14);
        } else if (memcmp(ch, "data", 4) == 0) {
            data = file + pos;
            data_n = sz;
        }
        pos += sz + (sz & 1u);
    }

    if (data == NULL || audio_format != 1u || channels != 1u || sample_rate != SR
        || bits != 16u) {
        fprintf(
            stderr,
            "WAV require PCM s16le mono %d Hz (got fmt=%u ch=%u sr=%u bits=%u)\n",
            SR,
            (unsigned)audio_format,
            (unsigned)channels,
            (unsigned)sample_rate,
            (unsigned)bits
        );
        free(file);
        return -1;
    }

    const size_t n = data_n / 2u;
    int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
    if (pcm == NULL) {
        free(file);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (int16_t)(int16_t)(data[2 * i] | (data[2 * i + 1] << 8));
    }
    free(file);
    *out_pcm = pcm;
    *out_n = n;
    return 0;
}

static void s16_to_s32_fs16(const int16_t *in, int32_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (int32_t)in[i];
    }
}

static const char *quality_name(uint8_t q)
{
    switch (q) {
    case AE_QUALITY_OK:
        return "OK";
    case AE_QUALITY_DEGRADED:
        return "DEGRADED";
    case AE_QUALITY_GAP:
        return "GAP";
    case AE_QUALITY_INVALID:
        return "INVALID";
    default:
        return "?";
    }
}

static const char *phase_for_seq(uint32_t seq)
{
    /* Non-overlapping 1 s windows: seq N ends at t=N seconds (1-based). */
    const double t_end = (double)seq;
    const double t_start = t_end - 1.0;
    const double mid = 0.5 * (t_start + t_end);
    if (mid < 5.0) {
        return "bg_pre";
    }
    if (mid < 16.0) {
        return "heli_approach";
    }
    if (mid < 22.0) {
        return "heli_peak";
    }
    if (mid < 27.0) {
        return "heli_recede";
    }
    return "bg_post";
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <mono16k.wav> <out.csv>\n", argv[0]);
        return 2;
    }

    int16_t *pcm16 = NULL;
    size_t n16 = 0;
    if (load_mono16k_wav(argv[1], &pcm16, &n16) != 0) {
        return 1;
    }

    const size_t n_win = n16 / (size_t)N_SAMP;
    if (n_win == 0u) {
        fprintf(stderr, "no complete %d-sample windows\n", N_SAMP);
        free(pcm16);
        return 1;
    }

    sp_acoustic_l0_filter_bank_t l0_bank;
    sp_acoustic_l1_workspace_t l1_ws;
    sp_acoustic_l2_continuity_t l2_cont;
    sp_acoustic_l0_filter_bank_init(&l0_bank, SR);
    sp_acoustic_l1_workspace_init(&l1_ws);
    sp_acoustic_l2_continuity_reset(&l2_cont);

    int32_t *win32 = (int32_t *)malloc((size_t)N_SAMP * sizeof(int32_t));
    if (win32 == NULL) {
        free(pcm16);
        return 1;
    }

    FILE *out = fopen(argv[2], "w");
    if (out == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        free(win32);
        free(pcm16);
        return 1;
    }

    fprintf(
        out,
        "window_seq,t_start_s,t_end_s,phase,"
        "quality,flags,"
        "rms_dbfs,peak_dbfs,crest_db,"
        "band0_dbfs,band1_dbfs,band2_dbfs,band3_dbfs,hl_db,hl_valid,"
        "spectral_valid,centroid_hz,bandwidth_hz,dominant_hz,flatness,"
        "dynamics_valid,spectral_flux,envelope_cv,"
        "dominant_delta_valid,dominant_delta_hz,"
        "pair_valid,delta_rms_db,delta_peak_db,mic_mask\n"
    );

    uint32_t ok_n = 0u;
    for (size_t w = 0; w < n_win; w++) {
        const uint32_t seq = (uint32_t)(w + 1u);
        const int64_t end_us = (int64_t)seq * 1000000LL;
        s16_to_s32_fs16(pcm16 + w * (size_t)N_SAMP, win32, (size_t)N_SAMP);

        sp_acoustic_l0_window_t l0;
        sp_acoustic_l1_window_t l1;
        sp_acoustic_l2_window_t l2;
        memset(&l0, 0, sizeof(l0));
        memset(&l1, 0, sizeof(l1));
        memset(&l2, 0, sizeof(l2));

        sp_acoustic_l0_analyze_window(
            &l0_bank, win32, (size_t)N_SAMP, FS_BITS, WIN_MS, end_us, &l0
        );
        sp_acoustic_l2_analyze_window(
            &l1_ws,
            win32,
            (size_t)N_SAMP,
            FS_BITS,
            SR,
            WIN_MS,
            end_us,
            &l1,
            &l2
        );
        sp_acoustic_l2_continuity_push(&l2_cont, WIN_MS, &l2);

        ae_signature_t sig;
        if (!ae_signature_build(seq, &l0, &l1, &l2, NULL, false, &sig)) {
            fprintf(stderr, "ae_signature_build failed seq=%u\n", seq);
            continue;
        }
        ok_n++;

        fprintf(
            out,
            "%u,%.3f,%.3f,%s,"
            "%s,0x%02x,"
            "%.4f,%.4f,%.4f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%d,"
            "%d,%.4f,%.4f,%.4f,%.6f,"
            "%d,%.6f,%.6f,"
            "%d,%.4f,"
            "%d,%.4f,%.4f,%u\n",
            sig.window_seq,
            (double)seq - 1.0,
            (double)seq,
            phase_for_seq(seq),
            quality_name(sig.quality),
            (unsigned)sig.flags,
            (double)sig.rms_dbfs,
            (double)sig.peak_dbfs,
            (double)sig.crest_db,
            (double)sig.band_dbfs[0],
            (double)sig.band_dbfs[1],
            (double)sig.band_dbfs[2],
            (double)sig.band_dbfs[3],
            (double)sig.hl_db,
            sig.hl_valid ? 1 : 0,
            sig.spectral_valid ? 1 : 0,
            (double)sig.centroid_hz,
            (double)sig.bandwidth_hz,
            (double)sig.dominant_hz,
            (double)sig.flatness,
            sig.dynamics_valid ? 1 : 0,
            (double)sig.spectral_flux,
            (double)sig.envelope_cv,
            sig.dominant_delta_valid ? 1 : 0,
            (double)sig.dominant_delta_hz,
            sig.pair_valid ? 1 : 0,
            (double)sig.delta_rms_db,
            (double)sig.delta_peak_db,
            (unsigned)sig.mic_mask
        );
    }

    fclose(out);
    free(win32);
    free(pcm16);

    fprintf(
        stderr,
        "as_profile_extract: windows=%zu signatures=%u csv=%s\n",
        n_win,
        ok_n,
        argv[2]
    );
    return ok_n == n_win ? 0 : 1;
}
