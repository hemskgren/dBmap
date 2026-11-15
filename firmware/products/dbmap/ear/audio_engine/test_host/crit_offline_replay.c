/**
 * Critical Path — offline AE signature replay (host).
 *
 * Reuses firmware L0 analysis + ae_signature_build (not a Python detector).
 * Does not enable Critical Path on-device.
 *
 * From repo root:
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/products/dbmap/ear/audio_engine/include \
 *     -I firmware/products/dbmap/ear/ear_acoustic/include \
 *     firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c \
 *     firmware/products/dbmap/ear/audio_engine/ae_signature.c \
 *     firmware/products/dbmap/ear/audio_engine/test_host/crit_offline_replay.c \
 *     -lm -o /tmp/crit_offline_replay && /tmp/crit_offline_replay help
 */
#include "ae_signature.h"
#include "dbmap_acoustic_l0.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { SR = 16000, WIN_MS = 1000, N_SAMP = 16000, FS_BITS_DEVICE = 24, MAX_SIG = 8192 };

typedef enum {
    EVAL_BOTH = 0,
    EVAL_DETECT = 1,
    EVAL_MATCH = 2
} eval_mode_t;

typedef enum {
    CH_MONO = 0,
    CH_LEFT = 1,
    CH_RIGHT = 2,
    CH_MIX = 3
} channel_mode_t;

typedef enum {
    RAW_S32LE_FS24 = 0, /* device-like: int32 samples, full-scale bits = 24 */
    RAW_S16LE = 1,
    RAW_S24_IN_S32LE = 2, /* 24-bit sample in low 24 bits of int32 */
    RAW_S32LE_FS31 = 3
} raw_format_t;

typedef struct {
    uint32_t window_seq;
    int64_t end_us;
    float rms_dbfs;
    float peak_dbfs;
    float crest_db;
    float band_dbfs[AE_BAND_COUNT];
    float hl_db;
    int hl_valid;
    int clipped;
    uint8_t quality;
    uint8_t flags;
    char source[48];
    char label[48];
} crit_sig_row_t;

typedef struct {
    crit_sig_row_t rows[MAX_SIG];
    uint32_t n;
    double duration_s;
    char name[96];
    char recording_id[96];
    char role[24]; /* target / distractor / unknown */
    char origin[32]; /* field / controlled_playback / synthetic / uart */
    float applied_gain_db;
    int fs_bits;
    int start_seq_hint; /* 0 = none */
} crit_sig_set_t;

typedef struct {
    float gain_db; /* 0 = none; explicit only */
    channel_mode_t channel;
    int fs_bits_override; /* 0 = derive from format */
    int start_seq_hint;
    eval_mode_t mode;
    const char *csv_path;
    const char *label;
    const char *recording_id;
    const char *role;
    const char *origin;
} load_opts_t;

static void opts_init(load_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->channel = CH_MONO;
    o->mode = EVAL_BOTH;
    o->label = "unlabeled";
    o->recording_id = "";
    o->role = "unknown";
    o->origin = "unknown";
}

static int64_t fs_peak_bits(int fs_bits)
{
    if (fs_bits < 2) {
        fs_bits = 2;
    }
    if (fs_bits > 31) {
        fs_bits = 31;
    }
    return (1LL << (fs_bits - 1)) - 1;
}

static void apply_gain_inplace(int32_t *pcm, size_t n, int fs_bits, float gain_db)
{
    if (gain_db == 0.0f || pcm == NULL || n == 0) {
        return;
    }
    const double g = pow(10.0, (double)gain_db / 20.0);
    const int64_t lim = fs_peak_bits(fs_bits);
    for (size_t i = 0; i < n; ++i) {
        double x = (double)pcm[i] * g;
        if (x > (double)lim) {
            x = (double)lim;
        } else if (x < (double)(-lim)) {
            x = (double)(-lim);
        }
        pcm[i] = (int32_t)lrint(x);
    }
}

static void fill_silence(int32_t *pcm, size_t n)
{
    memset(pcm, 0, n * sizeof(int32_t));
}

static void add_sine(int32_t *pcm, size_t n, double hz, double amp_fs, uint32_t seed, int fs_bits)
{
    const double fs = (double)fs_peak_bits(fs_bits);
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / (double)SR;
        const double phase = 2.0 * M_PI * hz * t;
        const double j = 0.02 * sin(phase * 0.37 + (double)(seed % 97));
        double x = amp_fs * sin(phase + j);
        if (x > 0.999) {
            x = 0.999;
        }
        if (x < -0.999) {
            x = -0.999;
        }
        const int32_t s = (int32_t)lrint(x * fs);
        const int64_t sum = (int64_t)pcm[i] + (int64_t)s;
        if (sum > (int64_t)fs) {
            pcm[i] = (int32_t)fs;
        } else if (sum < -(int64_t)fs) {
            pcm[i] = (int32_t)(-(int64_t)fs);
        } else {
            pcm[i] = (int32_t)sum;
        }
    }
}

static void add_impulse(int32_t *pcm, size_t n, size_t at, double amp_fs, size_t width, int fs_bits)
{
    const double fs = (double)fs_peak_bits(fs_bits);
    if (at >= n) {
        return;
    }
    if (width < 1u) {
        width = 1u;
    }
    for (size_t k = 0; k < width && (at + k) < n; ++k) {
        const double env = exp(-(double)k / (double)(width / 3u + 1u));
        double x = amp_fs * env;
        if (x > 0.999) {
            x = 0.999;
        }
        const int32_t s = (int32_t)lrint(x * fs);
        const int64_t sum = (int64_t)pcm[at + k] + (int64_t)s;
        if (sum > (int64_t)fs) {
            pcm[at + k] = (int32_t)fs;
        } else if (sum < -(int64_t)fs) {
            pcm[at + k] = (int32_t)(-(int64_t)fs);
        } else {
            pcm[at + k] = (int32_t)sum;
        }
    }
}

static void add_noise(int32_t *pcm, size_t n, double amp_fs, uint32_t *rng, int fs_bits)
{
    const double fs = (double)fs_peak_bits(fs_bits);
    for (size_t i = 0; i < n; ++i) {
        *rng = *rng * 1664525u + 1013904223u;
        const double u = ((*rng >> 8) & 0xffffu) / 65535.0;
        const double x = amp_fs * (2.0 * u - 1.0);
        const int32_t s = (int32_t)lrint(x * fs);
        const int64_t sum = (int64_t)pcm[i] + (int64_t)s;
        if (sum > (int64_t)fs) {
            pcm[i] = (int32_t)fs;
        } else if (sum < -(int64_t)fs) {
            pcm[i] = (int32_t)(-(int64_t)fs);
        } else {
            pcm[i] = (int32_t)sum;
        }
    }
}

static int append_from_pcm(
    crit_sig_set_t *set,
    dbmap_l0_filter_bank_t *bank,
    const int32_t *pcm,
    int fs_bits,
    uint32_t seq,
    int64_t end_us,
    const char *source,
    const char *label
)
{
    if (set->n >= MAX_SIG) {
        fprintf(stderr, "signature buffer full\n");
        return -1;
    }
    dbmap_l0_window_t l0;
    memset(&l0, 0, sizeof(l0));
    dbmap_l0_analyze_window(bank, pcm, N_SAMP, fs_bits, WIN_MS, end_us, &l0);
    if (!l0.valid) {
        fprintf(stderr, "L0 invalid for seq=%u source=%s\n", seq, source);
        return -1;
    }
    ae_signature_t sig;
    if (!ae_signature_build(seq, &l0, NULL, NULL, NULL, false, &sig)) {
        fprintf(stderr, "ae_signature_build failed seq=%u\n", seq);
        return -1;
    }
    crit_sig_row_t *r = &set->rows[set->n++];
    memset(r, 0, sizeof(*r));
    r->window_seq = sig.window_seq;
    r->end_us = sig.window_end_mono_us;
    r->rms_dbfs = sig.rms_dbfs;
    r->peak_dbfs = sig.peak_dbfs;
    r->crest_db = sig.crest_db;
    for (uint32_t i = 0; i < AE_BAND_COUNT; ++i) {
        r->band_dbfs[i] = sig.band_dbfs[i];
    }
    r->hl_db = sig.hl_db;
    r->hl_valid = sig.hl_valid ? 1 : 0;
    r->clipped = (sig.flags & AE_FLAG_CLIPPED) ? 1 : 0;
    r->quality = sig.quality;
    r->flags = sig.flags;
    snprintf(r->source, sizeof(r->source), "%s", source);
    snprintf(r->label, sizeof(r->label), "%s", label);
    return 0;
}

static ae_signature_t row_as_sig(const crit_sig_row_t *r)
{
    ae_signature_t s;
    memset(&s, 0, sizeof(s));
    s.schema_version = AE_SIGNATURE_SCHEMA_VERSION;
    s.window_seq = r->window_seq;
    s.window_end_mono_us = r->end_us;
    s.window_ms = WIN_MS;
    s.rms_dbfs = r->rms_dbfs;
    s.peak_dbfs = r->peak_dbfs;
    s.crest_db = r->crest_db;
    for (uint32_t i = 0; i < AE_BAND_COUNT; ++i) {
        s.band_dbfs[i] = r->band_dbfs[i];
    }
    s.hl_db = r->hl_db;
    s.hl_valid = r->hl_valid != 0;
    s.quality = r->quality;
    s.flags = r->flags;
    return s;
}

static void print_csv_header(FILE *f)
{
    fprintf(
        f,
        "recording_id,set,label,role,origin,applied_gain_db,source,seq,end_us,"
        "rms_dbfs,peak_dbfs,crest_db,B0,B1,B2,B3,hl_db,hl_valid,clipped,quality,flags\n"
    );
}

static void print_csv_row(FILE *f, const crit_sig_set_t *set, const crit_sig_row_t *r)
{
    fprintf(
        f,
        "%s,%s,%s,%s,%s,%.3f,%s,%u,%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%u,0x%02x\n",
        set->recording_id[0] ? set->recording_id : set->name,
        set->name,
        r->label,
        set->role,
        set->origin,
        (double)set->applied_gain_db,
        r->source,
        r->window_seq,
        (long long)r->end_us,
        (double)r->rms_dbfs,
        (double)r->peak_dbfs,
        (double)r->crest_db,
        (double)r->band_dbfs[0],
        (double)r->band_dbfs[1],
        (double)r->band_dbfs[2],
        (double)r->band_dbfs[3],
        (double)r->hl_db,
        r->hl_valid,
        r->clipped,
        (unsigned)r->quality,
        (unsigned)r->flags
    );
}

static int append_pcm_stream(
    crit_sig_set_t *set,
    const int32_t *all,
    size_t n_samples,
    int fs_bits,
    const char *source,
    const char *label
)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    uint32_t seq = 1;
    int64_t t = 1000000;
    size_t off = 0;
    while (off + (size_t)N_SAMP <= n_samples) {
        if (append_from_pcm(set, &bank, all + off, fs_bits, seq++, t, source, label) != 0) {
            return -1;
        }
        t += 1000000;
        off += (size_t)N_SAMP;
    }
    set->duration_s = (double)(off) / (double)SR;
    set->fs_bits = fs_bits;
    return (int)set->n;
}

/* ---- synthetic scenarios ---- */

static int gen_scenario(crit_sig_set_t *set, const char *name)
{
    const int fs_bits = FS_BITS_DEVICE;
    int32_t *pcm = (int32_t *)calloc((size_t)N_SAMP, sizeof(int32_t));
    if (pcm == NULL) {
        return -1;
    }
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    memset(set, 0, sizeof(*set));
    snprintf(set->name, sizeof(set->name), "%s", name);
    snprintf(set->recording_id, sizeof(set->recording_id), "synth_%s", name);
    snprintf(set->origin, sizeof(set->origin), "%s", "synthetic");
    snprintf(set->role, sizeof(set->role), "%s", "synthetic");
    set->fs_bits = fs_bits;

    uint32_t rng = 1u;
    uint32_t seq = 1u;
    int64_t t = 1000000;

    if (strcmp(name, "ambient") == 0) {
        set->duration_s = 30.0;
        for (int w = 0; w < 30; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.0035, &rng, fs_bits);
            add_sine(pcm, N_SAMP, 120.0, 0.0015, rng, fs_bits);
            add_sine(pcm, N_SAMP, 240.0, 0.0010, rng + 3u, fs_bits);
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "ambient") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "speech_like") == 0) {
        set->duration_s = 20.0;
        for (int w = 0; w < 20; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.0020, &rng, fs_bits);
            const double amp = (w % 3 == 0) ? 0.020 : 0.008;
            add_sine(pcm, N_SAMP, 300.0, amp * 0.6, rng, fs_bits);
            add_sine(pcm, N_SAMP, 900.0, amp, rng + 1u, fs_bits);
            add_sine(pcm, N_SAMP, 1800.0, amp * 0.5, rng + 2u, fs_bits);
            if ((w % 5) == 2) {
                add_impulse(pcm, N_SAMP, 4000u + (size_t)(w * 100), 0.15, 8u, fs_bits);
            }
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "speech_like") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "traffic_like") == 0) {
        set->duration_s = 20.0;
        for (int w = 0; w < 20; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.008, &rng, fs_bits);
            add_sine(pcm, N_SAMP, 80.0, 0.025, rng, fs_bits);
            add_sine(pcm, N_SAMP, 160.0, 0.015, rng + 1u, fs_bits);
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "traffic_like") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "wind_like") == 0) {
        set->duration_s = 15.0;
        for (int w = 0; w < 15; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.012 + 0.004 * sin((double)w), &rng, fs_bits);
            add_sine(pcm, N_SAMP, 40.0, 0.01, rng, fs_bits);
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "wind_like") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "door_slam") == 0) {
        set->duration_s = 10.0;
        for (int w = 0; w < 10; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.0025, &rng, fs_bits);
            if (w == 4) {
                add_impulse(pcm, N_SAMP, 8000u, 0.55, 40u, fs_bits);
                add_sine(pcm, N_SAMP, 200.0, 0.05, rng, fs_bits);
            }
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "door_slam") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "impulse_single") == 0) {
        set->duration_s = 8.0;
        for (int w = 0; w < 8; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.0020, &rng, fs_bits);
            if (w == 3) {
                add_impulse(pcm, N_SAMP, 8000u, 0.90, 6u, fs_bits);
            }
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "impulse_single") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "impulse_train") == 0) {
        set->duration_s = 16.0;
        for (int w = 0; w < 16; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.0020, &rng, fs_bits);
            if (w == 2 || w == 5 || w == 8 || w == 11) {
                add_impulse(pcm, N_SAMP, 7000u + (size_t)(w * 50), 0.88, 6u, fs_bits);
            }
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "impulse_train") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "clip_burst") == 0) {
        set->duration_s = 6.0;
        for (int w = 0; w < 6; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.0020, &rng, fs_bits);
            if (w == 2) {
                add_impulse(pcm, N_SAMP, 5000u, 0.999, 20u, fs_bits);
                for (size_t i = 5000; i < 5020 && i < N_SAMP; ++i) {
                    pcm[i] = (int32_t)fs_peak_bits(fs_bits);
                }
            }
            if (append_from_pcm(set, &bank, pcm, fs_bits, seq++, t, "synth", "clip_burst") != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else if (strcmp(name, "mixed_distractors_then_impulse") == 0) {
        set->duration_s = 25.0;
        for (int w = 0; w < 25; ++w) {
            fill_silence(pcm, N_SAMP);
            add_noise(pcm, N_SAMP, 0.003, &rng, fs_bits);
            if (w < 8) {
                add_sine(pcm, N_SAMP, 90.0, 0.02, rng, fs_bits);
            } else if (w < 14) {
                add_sine(pcm, N_SAMP, 1000.0, 0.015, rng, fs_bits);
            } else if (w < 18) {
                add_noise(pcm, N_SAMP, 0.015, &rng, fs_bits);
            } else if (w == 20) {
                add_impulse(pcm, N_SAMP, 9000u, 0.92, 5u, fs_bits);
            } else if (w == 22) {
                add_impulse(pcm, N_SAMP, 9000u, 0.90, 5u, fs_bits);
            }
            if (append_from_pcm(
                    set, &bank, pcm, fs_bits, seq++, t, "synth", "mixed_distractors_then_impulse"
                )
                != 0) {
                free(pcm);
                return -1;
            }
            t += 1000000;
        }
    } else {
        fprintf(stderr, "unknown scenario: %s\n", name);
        free(pcm);
        return -1;
    }

    free(pcm);
    return 0;
}

/* ---- UART L0 log import ---- */

static int load_uart_l0(crit_sig_set_t *set, const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    memset(set, 0, sizeof(*set));
    snprintf(set->name, sizeof(set->name), "uart_l0");
    snprintf(set->recording_id, sizeof(set->recording_id), "uart_l0");
    snprintf(set->origin, sizeof(set->origin), "%s", "uart");
    snprintf(set->role, sizeof(set->role), "%s", "ambient_soak");
    set->fs_bits = FS_BITS_DEVICE;
    char line[1024];
    uint32_t seq = 0;
    int64_t first_us = -1;
    int64_t last_us = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        const char *p = strstr(line, "L0 win rms=");
        if (p == NULL) {
            continue;
        }
        float rms, peak, crest, b0, b1, b2, b3, hl;
        int clip = 0;
        long long end_us = 0;
        unsigned long n = 0;
        if (sscanf(
                p,
                "L0 win rms=%f peak=%f crest=%f clip=%d B0=%f B1=%f B2=%f B3=%f hl=%f "
                "n=%lu end_us=%lld",
                &rms,
                &peak,
                &crest,
                &clip,
                &b0,
                &b1,
                &b2,
                &b3,
                &hl,
                &n,
                &end_us
            )
            < 11) {
            continue;
        }
        if (set->n >= MAX_SIG) {
            break;
        }
        seq++;
        crit_sig_row_t *r = &set->rows[set->n++];
        memset(r, 0, sizeof(*r));
        r->window_seq = seq;
        r->end_us = end_us;
        r->rms_dbfs = rms;
        r->peak_dbfs = peak;
        r->crest_db = crest;
        r->band_dbfs[0] = b0;
        r->band_dbfs[1] = b1;
        r->band_dbfs[2] = b2;
        r->band_dbfs[3] = b3;
        r->hl_db = hl;
        r->hl_valid = 1;
        r->clipped = clip ? 1 : 0;
        r->quality = clip ? AE_QUALITY_DEGRADED : AE_QUALITY_OK;
        r->flags = clip ? AE_FLAG_CLIPPED : 0u;
        r->flags |= AE_FLAG_L1_MISSING | AE_FLAG_L2_MISSING | AE_FLAG_PAIR_MISSING;
        snprintf(r->source, sizeof(r->source), "uart");
        snprintf(r->label, sizeof(r->label), "device_soak");
        if (first_us < 0) {
            first_us = end_us;
        }
        last_us = end_us;
    }
    fclose(f);
    if (first_us >= 0 && last_us > first_us) {
        set->duration_s = (double)(last_us - first_us) / 1.0e6;
    }
    return (int)set->n;
}

/* ---- WAV (PCM / IEEE float) ---- */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_file_bytes(const char *path, uint8_t **out, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_n = (size_t)sz;
    return 0;
}

static int load_wav(
    crit_sig_set_t *set,
    const char *path,
    const load_opts_t *opts
)
{
    uint8_t *file = NULL;
    size_t file_n = 0;
    if (read_file_bytes(path, &file, &file_n) != 0) {
        return -1;
    }
    if (file_n < 44 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "WAV: not a RIFF/WAVE file: %s\n", path);
        free(file);
        return -1;
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint16_t block_align = 0;
    const uint8_t *data = NULL;
    size_t data_n = 0;

    size_t off = 12;
    while (off + 8 <= file_n) {
        const uint8_t *ch = file + off;
        uint32_t csz = rd_u32(ch + 4);
        if (off + 8 + (size_t)csz > file_n) {
            break;
        }
        if (memcmp(ch, "fmt ", 4) == 0) {
            if (csz < 16) {
                fprintf(stderr, "WAV: fmt chunk too small\n");
                free(file);
                return -1;
            }
            audio_format = rd_u16(ch + 8);
            channels = rd_u16(ch + 10);
            sample_rate = rd_u32(ch + 12);
            bits = rd_u16(ch + 22);
            block_align = rd_u16(ch + 20);
        } else if (memcmp(ch, "data", 4) == 0) {
            data = ch + 8;
            data_n = csz;
        }
        off += 8 + (size_t)csz;
        if ((csz & 1u) != 0u) {
            off++;
        }
    }

    if (data == NULL || sample_rate == 0 || channels == 0 || bits == 0) {
        fprintf(stderr, "WAV: missing fmt/data in %s\n", path);
        free(file);
        return -1;
    }
    if (sample_rate != (uint32_t)SR) {
        fprintf(
            stderr,
            "WAV: sample_rate=%u Hz not supported (require %d). Convert explicitly; see README.\n",
            sample_rate,
            SR
        );
        free(file);
        return -1;
    }
    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "WAV: audio_format=%u unsupported (need PCM=1 or IEEE float=3)\n", audio_format);
        free(file);
        return -1;
    }
    if (channels == 1) {
        if (opts->channel != CH_MONO && opts->channel != CH_LEFT && opts->channel != CH_MIX) {
            fprintf(stderr, "WAV: mono file; use --channel mono|0|mix\n");
            free(file);
            return -1;
        }
    } else if (channels == 2) {
        if (opts->channel == CH_MONO) {
            fprintf(
                stderr,
                "WAV: stereo requires explicit --channel 0|1|mix (no silent downmix)\n"
            );
            free(file);
            return -1;
        }
    } else {
        fprintf(stderr, "WAV: channels=%u unsupported (1 or 2 only)\n", channels);
        free(file);
        return -1;
    }

    int fs_bits = 0;
    if (audio_format == 1) {
        if (bits == 16) {
            fs_bits = 16;
        } else if (bits == 24) {
            fs_bits = 24;
        } else if (bits == 32) {
            fs_bits = 31;
        } else {
            fprintf(stderr, "WAV: PCM bits=%u unsupported (16/24/32)\n", bits);
            free(file);
            return -1;
        }
    } else {
        if (bits != 32) {
            fprintf(stderr, "WAV: IEEE float requires 32-bit samples\n");
            free(file);
            return -1;
        }
        fs_bits = 31;
    }
    if (opts->fs_bits_override > 0) {
        fs_bits = opts->fs_bits_override;
    }

    const size_t frame_bytes = (size_t)block_align;
    if (frame_bytes == 0 || (data_n % frame_bytes) != 0) {
        fprintf(stderr, "WAV: data size not aligned to block_align=%u\n", block_align);
        free(file);
        return -1;
    }
    const size_t n_frames = data_n / frame_bytes;
    int32_t *mono = (int32_t *)calloc(n_frames, sizeof(int32_t));
    if (mono == NULL) {
        free(file);
        return -1;
    }

    const int64_t lim = fs_peak_bits(fs_bits);
    for (size_t i = 0; i < n_frames; ++i) {
        const uint8_t *fr = data + i * frame_bytes;
        double s0 = 0.0;
        double s1 = 0.0;
        if (audio_format == 1 && bits == 16) {
            int16_t a = (int16_t)(fr[0] | (fr[1] << 8));
            s0 = (double)a;
            if (channels == 2) {
                int16_t b = (int16_t)(fr[2] | (fr[3] << 8));
                s1 = (double)b;
            }
        } else if (audio_format == 1 && bits == 24) {
            int32_t a = (int32_t)((fr[0]) | (fr[1] << 8) | (fr[2] << 16));
            if (a & 0x800000) {
                a |= ~0xFFFFFF;
            }
            s0 = (double)a;
            if (channels == 2) {
                int32_t b = (int32_t)((fr[3]) | (fr[4] << 8) | (fr[5] << 16));
                if (b & 0x800000) {
                    b |= ~0xFFFFFF;
                }
                s1 = (double)b;
            }
        } else if (audio_format == 1 && bits == 32) {
            int32_t a = (int32_t)rd_u32(fr);
            s0 = (double)a;
            if (channels == 2) {
                int32_t b = (int32_t)rd_u32(fr + 4);
                s1 = (double)b;
            }
            /* Map full int32 into fs_bits=31 range without gain change of meaning:
             * treat sample as already at 32-bit FS; L0 fs_bits=31 ≈ same FS. */
        } else {
            float fa, fb = 0.0f;
            memcpy(&fa, fr, 4);
            if (channels == 2) {
                memcpy(&fb, fr + 4, 4);
            }
            s0 = (double)fa * (double)lim;
            s1 = (double)fb * (double)lim;
        }

        double s;
        if (channels == 1 || opts->channel == CH_LEFT || opts->channel == CH_MONO) {
            s = s0;
        } else if (opts->channel == CH_RIGHT) {
            s = s1;
        } else {
            s = 0.5 * (s0 + s1);
        }
        if (s > (double)lim) {
            s = (double)lim;
        } else if (s < (double)(-lim)) {
            s = (double)(-lim);
        }
        mono[i] = (int32_t)lrint(s);
    }
    free(file);

    apply_gain_inplace(mono, n_frames, fs_bits, opts->gain_db);

    memset(set, 0, sizeof(*set));
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(set->name, sizeof(set->name), "%s", base);
    }
    snprintf(
        set->recording_id,
        sizeof(set->recording_id),
        "%s",
        opts->recording_id[0] ? opts->recording_id : set->name
    );
    snprintf(set->role, sizeof(set->role), "%s", opts->role);
    snprintf(set->origin, sizeof(set->origin), "%s", opts->origin);
    set->applied_gain_db = opts->gain_db;
    set->start_seq_hint = opts->start_seq_hint;
    set->fs_bits = fs_bits;

    const int n = append_pcm_stream(set, mono, n_frames, fs_bits, "wav", opts->label);
    free(mono);
    if (n <= 0) {
        fprintf(stderr, "WAV: no complete %d-sample windows in %s\n", N_SAMP, path);
        return -1;
    }
    printf(
        "wav path=%s frames=%zu windows=%u sr=%d fs_bits=%d ch_mode=%d gain_db=%.2f origin=%s role=%s\n",
        path,
        n_frames,
        set->n,
        SR,
        fs_bits,
        (int)opts->channel,
        (double)opts->gain_db,
        set->origin,
        set->role
    );
    return n;
}

static raw_format_t parse_raw_format(const char *s)
{
    if (strcmp(s, "s32le_fs24") == 0 || strcmp(s, "device") == 0) {
        return RAW_S32LE_FS24;
    }
    if (strcmp(s, "s16le") == 0) {
        return RAW_S16LE;
    }
    if (strcmp(s, "s24_in_s32le") == 0) {
        return RAW_S24_IN_S32LE;
    }
    if (strcmp(s, "s32le") == 0 || strcmp(s, "s32le_fs31") == 0) {
        return RAW_S32LE_FS31;
    }
    return (raw_format_t)-1;
}

static int load_raw(
    crit_sig_set_t *set,
    const char *path,
    raw_format_t fmt,
    const load_opts_t *opts
)
{
    uint8_t *file = NULL;
    size_t file_n = 0;
    if (read_file_bytes(path, &file, &file_n) != 0) {
        return -1;
    }

    int fs_bits = FS_BITS_DEVICE;
    size_t bps = 4;
    if (fmt == RAW_S16LE) {
        fs_bits = 16;
        bps = 2;
    } else if (fmt == RAW_S24_IN_S32LE) {
        fs_bits = 24;
        bps = 4;
    } else if (fmt == RAW_S32LE_FS31) {
        fs_bits = 31;
        bps = 4;
    } else {
        fs_bits = FS_BITS_DEVICE;
        bps = 4;
    }
    if (opts->fs_bits_override > 0) {
        fs_bits = opts->fs_bits_override;
    }
    if (file_n % bps != 0) {
        fprintf(stderr, "raw: size %zu not multiple of %zu\n", file_n, bps);
        free(file);
        return -1;
    }
    const size_t n = file_n / bps;
    int32_t *mono = (int32_t *)calloc(n, sizeof(int32_t));
    if (mono == NULL) {
        free(file);
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *p = file + i * bps;
        if (fmt == RAW_S16LE) {
            mono[i] = (int32_t)(int16_t)(p[0] | (p[1] << 8));
        } else if (fmt == RAW_S24_IN_S32LE) {
            /* Low 24 bits are the sample; sign-extend into int32. */
            int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000) {
                v |= ~0xFFFFFF;
            }
            mono[i] = v;
        } else {
            /* s32le_fs24 / s32le: native int32 sample values. */
            mono[i] = (int32_t)rd_u32(p);
        }
    }
    free(file);

    apply_gain_inplace(mono, n, fs_bits, opts->gain_db);

    memset(set, 0, sizeof(*set));
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(set->name, sizeof(set->name), "%s", base);
    }
    snprintf(
        set->recording_id,
        sizeof(set->recording_id),
        "%s",
        opts->recording_id[0] ? opts->recording_id : set->name
    );
    snprintf(set->role, sizeof(set->role), "%s", opts->role);
    snprintf(set->origin, sizeof(set->origin), "%s", opts->origin);
    set->applied_gain_db = opts->gain_db;
    set->start_seq_hint = opts->start_seq_hint;
    set->fs_bits = fs_bits;

    const int got = append_pcm_stream(set, mono, n, fs_bits, "raw", opts->label);
    free(mono);
    if (got <= 0) {
        fprintf(stderr, "raw: no complete windows in %s\n", path);
        return -1;
    }
    return got;
}

/* ---- evaluation: A detection / B continuation ---- */

static int usable_quality(uint8_t q)
{
    return q == AE_QUALITY_OK || q == AE_QUALITY_DEGRADED;
}

static int trigger_hit(
    const crit_sig_row_t *r,
    float crest_min,
    float peak_min,
    int require_peak,
    int require_clip,
    int crest_or_clip
)
{
    if (!usable_quality(r->quality)) {
        return 0;
    }
    const int crest_ok = r->crest_db >= crest_min;
    const int peak_ok = (!require_peak) || (r->peak_dbfs >= peak_min);
    const int clip_ok = (!require_clip) || (r->clipped != 0);
    if (crest_or_clip) {
        return (crest_ok || r->clipped != 0) && peak_ok;
    }
    return crest_ok && peak_ok && clip_ok;
}

static void eval_detection(const crit_sig_set_t *set)
{
    const float crests[] = {12.0f, 14.0f, 16.0f};
    printf(
        "\n=== A. DETECTION (experimental; not production): set=%s id=%s role=%s origin=%s "
        "n=%u duration_s=%.1f gain_db=%.2f ===\n",
        set->name,
        set->recording_id,
        set->role,
        set->origin,
        set->n,
        set->duration_s,
        (double)set->applied_gain_db
    );
    printf(
        "config,crest_min,peak_min,require_peak,require_clip,crest_or_clip,"
        "triggers,per_min,examples\n"
    );

    for (size_t ci = 0; ci < sizeof(crests) / sizeof(crests[0]); ++ci) {
        for (int mode = 0; mode < 4; ++mode) {
            const float crest_min = crests[ci];
            int require_peak = 0;
            int require_clip = 0;
            int crest_or_clip = 0;
            float peak_min = -120.0f;
            const char *cfg = "crest";
            if (mode == 1) {
                require_clip = 1;
                cfg = "crest+clip";
            } else if (mode == 2) {
                crest_or_clip = 1;
                cfg = "crest_or_clip";
            } else if (mode == 3) {
                require_peak = 1;
                peak_min = -6.0f;
                cfg = "crest+peak>=-6";
            }

            uint32_t hits = 0;
            char examples[256];
            examples[0] = '\0';
            size_t ex_used = 0;
            for (uint32_t i = 0; i < set->n; ++i) {
                const crit_sig_row_t *r = &set->rows[i];
                if (!trigger_hit(r, crest_min, peak_min, require_peak, require_clip, crest_or_clip)) {
                    continue;
                }
                hits++;
                if (ex_used < sizeof(examples) - 48) {
                    ex_used += (size_t)snprintf(
                        examples + ex_used,
                        sizeof(examples) - ex_used,
                        "%s%s#seq%u:c=%.1f/p=%.1f",
                        ex_used ? ";" : "",
                        r->label,
                        r->window_seq,
                        (double)r->crest_db,
                        (double)r->peak_dbfs
                    );
                }
            }
            double per_min = 0.0;
            if (set->duration_s > 0.0) {
                per_min = (double)hits / (set->duration_s / 60.0);
            }
            printf(
                "%s,%.1f,%.1f,%d,%d,%d,%u,%.3f,%s\n",
                cfg,
                (double)crest_min,
                (double)peak_min,
                require_peak,
                require_clip,
                crest_or_clip,
                hits,
                per_min,
                examples
            );
        }
    }
}

static int find_start_index(const crit_sig_set_t *set)
{
    if (set->start_seq_hint > 0) {
        for (uint32_t i = 0; i < set->n; ++i) {
            if ((int)set->rows[i].window_seq == set->start_seq_hint) {
                return (int)i;
            }
        }
        fprintf(stderr, "start_seq_hint=%d not found\n", set->start_seq_hint);
        return -1;
    }
    /* Experimental seed only: first crest>=12 window. Not a production rule. */
    for (uint32_t i = 0; i < set->n; ++i) {
        if (trigger_hit(&set->rows[i], 12.0f, -120.0f, 0, 0, 0)) {
            return (int)i;
        }
    }
    return -1;
}

static void eval_continuation(const crit_sig_set_t *set)
{
    printf(
        "\n=== B. CONTINUATION / MATCH (experimental): set=%s id=%s ===\n",
        set->name,
        set->recording_id
    );
    const int start_i = find_start_index(set);
    if (start_i < 0) {
        printf("no START fingerprint (no crest>=12 window and no start_seq_hint)\n");
        return;
    }
    const ae_signature_t frozen = row_as_sig(&set->rows[start_i]);
    printf(
        "START seq=%u crest=%.2f rms=%.2f peak=%.2f (frozen; no EMA)\n",
        frozen.window_seq,
        (double)frozen.crest_db,
        (double)frozen.rms_dbfs,
        (double)frozen.peak_dbfs
    );

    const float dists[] = {2.0f, 4.0f, 6.0f, 8.0f, 12.0f};
    const float crest_floors[] = {0.0f, 8.0f, 10.0f, 12.0f};

    printf(
        "D_match,C_floor,later_windows,match_count,match_labels,nonmatch_trigger_count,"
        "quiet_match_count\n"
    );
    for (size_t di = 0; di < sizeof(dists) / sizeof(dists[0]); ++di) {
        for (size_t ci = 0; ci < sizeof(crest_floors) / sizeof(crest_floors[0]); ++ci) {
            const float D = dists[di];
            const float Cf = crest_floors[ci];
            uint32_t match_n = 0;
            uint32_t nonmatch_trig = 0;
            uint32_t quiet_match = 0;
            uint32_t later = 0;
            char labels[200];
            labels[0] = '\0';
            size_t used = 0;
            for (uint32_t i = (uint32_t)start_i + 1u; i < set->n; ++i) {
                const crit_sig_row_t *r = &set->rows[i];
                later++;
                if (!usable_quality(r->quality)) {
                    continue;
                }
                const ae_signature_t cur = row_as_sig(r);
                const float d = ae_signature_distance(&frozen, &cur);
                const int match = (d <= D) && (r->crest_db >= Cf);
                const int trig = trigger_hit(r, 12.0f, -120.0f, 0, 0, 0);
                if (match) {
                    match_n++;
                    if (!trig) {
                        quiet_match++;
                    }
                    if (used < sizeof(labels) - 40) {
                        used += (size_t)snprintf(
                            labels + used,
                            sizeof(labels) - used,
                            "%s%s#seq%u:d=%.2f",
                            used ? ";" : "",
                            r->label,
                            r->window_seq,
                            (double)d
                        );
                    }
                } else if (trig) {
                    nonmatch_trig++;
                }
            }
            printf(
                "%.1f,%.1f,%u,%u,%s,%u,%u\n",
                (double)D,
                (double)Cf,
                later,
                match_n,
                labels,
                nonmatch_trig,
                quiet_match
            );
        }
    }

    printf("later_seq,label,crest,dist_to_START,trig12,note\n");
    for (uint32_t i = (uint32_t)start_i + 1u; i < set->n; ++i) {
        const crit_sig_row_t *r = &set->rows[i];
        const ae_signature_t cur = row_as_sig(r);
        const float d = ae_signature_distance(&frozen, &cur);
        const int trig = trigger_hit(r, 12.0f, -120.0f, 0, 0, 0);
        const char *note = trig ? "event_candidate" : "background";
        printf(
            "%u,%s,%.2f,%.3f,%d,%s\n",
            r->window_seq,
            r->label,
            (double)r->crest_db,
            (double)d,
            trig,
            note
        );
    }
}

static void eval_set(const crit_sig_set_t *set, eval_mode_t mode)
{
    if (mode == EVAL_DETECT || mode == EVAL_BOTH) {
        eval_detection(set);
    }
    if (mode == EVAL_MATCH || mode == EVAL_BOTH) {
        eval_continuation(set);
    }
}

static void dump_csv(const crit_sig_set_t *set, const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        return;
    }
    print_csv_header(f);
    for (uint32_t i = 0; i < set->n; ++i) {
        print_csv_row(f, set, &set->rows[i]);
    }
    fclose(f);
    printf("wrote %s (%u rows)\n", path, set->n);
}

/* ---- CLI helpers ---- */

static int parse_common_opts(int *argc, char ***argv, load_opts_t *opts)
{
    while (*argc >= 2) {
        const char *a = (*argv)[0];
        const char *v = (*argv)[1];
        if (strcmp(a, "--gain-db") == 0) {
            opts->gain_db = (float)atof(v);
        } else if (strcmp(a, "--channel") == 0) {
            if (strcmp(v, "mono") == 0) {
                opts->channel = CH_MONO;
            } else if (strcmp(v, "0") == 0 || strcmp(v, "left") == 0) {
                opts->channel = CH_LEFT;
            } else if (strcmp(v, "1") == 0 || strcmp(v, "right") == 0) {
                opts->channel = CH_RIGHT;
            } else if (strcmp(v, "mix") == 0) {
                opts->channel = CH_MIX;
            } else {
                fprintf(stderr, "bad --channel %s\n", v);
                return -1;
            }
        } else if (strcmp(a, "--fs-bits") == 0) {
            opts->fs_bits_override = atoi(v);
        } else if (strcmp(a, "--start-seq") == 0) {
            opts->start_seq_hint = atoi(v);
        } else if (strcmp(a, "--mode") == 0) {
            if (strcmp(v, "detect") == 0) {
                opts->mode = EVAL_DETECT;
            } else if (strcmp(v, "match") == 0) {
                opts->mode = EVAL_MATCH;
            } else if (strcmp(v, "both") == 0) {
                opts->mode = EVAL_BOTH;
            } else {
                fprintf(stderr, "bad --mode %s\n", v);
                return -1;
            }
        } else if (strcmp(a, "--csv") == 0) {
            opts->csv_path = v;
        } else if (strcmp(a, "--label") == 0) {
            opts->label = v;
        } else if (strcmp(a, "--recording-id") == 0) {
            opts->recording_id = v;
        } else if (strcmp(a, "--role") == 0) {
            opts->role = v;
        } else if (strcmp(a, "--origin") == 0) {
            opts->origin = v;
        } else {
            break;
        }
        *argv += 2;
        *argc -= 2;
    }
    return 0;
}

static void usage(void)
{
    fprintf(
        stderr,
        "crit_offline_replay — Critical Path offline AE replay (firmware L0 + ae_signature)\n"
        "\n"
        "Commands:\n"
        "  list-scenarios\n"
        "  synth <scenario> [options]\n"
        "  uart <logfile> [options]\n"
        "  wav <file.wav> [options]\n"
        "  raw <file.raw> [--format s32le_fs24|s16le|s24_in_s32le|s32le] [options]\n"
        "  dataset <manifest.csv> [--out-dir DIR]\n"
        "  run-all [uart_logfile]\n"
        "\n"
        "Options:\n"
        "  --gain-db N       Explicit gain (dB). Default 0; never auto-normalized.\n"
        "  --channel mono|0|1|mix   Stereo requires explicit 0|1|mix.\n"
        "  --fs-bits N       Override L0 full-scale bits.\n"
        "  --start-seq N     Continuation START window (else first crest>=12).\n"
        "  --mode detect|match|both\n"
        "  --csv path        Per-window CSV\n"
        "  --label|--recording-id|--role|--origin  Metadata tags\n"
        "\n"
        "WAV: mono or stereo PCM 16/24/32 or float32, **exactly 16000 Hz**.\n"
        "Convert other rates with convert_to_mono16k.sh (documented; not silent).\n"
        "\n"
        "Scenarios: ambient speech_like traffic_like wind_like door_slam\n"
        "           impulse_single impulse_train clip_burst\n"
        "           mixed_distractors_then_impulse\n"
    );
}

static const char *k_scenarios[] = {
    "ambient",
    "speech_like",
    "traffic_like",
    "wind_like",
    "door_slam",
    "impulse_single",
    "impulse_train",
    "clip_burst",
    "mixed_distractors_then_impulse",
};

static int cmd_synth(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 1;
    }
    load_opts_t opts;
    opts_init(&opts);
    const char *name = argv[0];
    argc--;
    argv++;
    if (parse_common_opts(&argc, &argv, &opts) != 0) {
        return 1;
    }
    crit_sig_set_t set;
    if (gen_scenario(&set, name) != 0) {
        return 1;
    }
    set.start_seq_hint = opts.start_seq_hint;
    if (opts.gain_db != 0.0f) {
        fprintf(stderr, "synth: --gain-db ignored (regenerate scenario levels in code)\n");
    }
    printf("scenario=%s windows=%u duration_s=%.1f origin=synthetic\n", set.name, set.n, set.duration_s);
    eval_set(&set, opts.mode);
    if (opts.csv_path) {
        dump_csv(&set, opts.csv_path);
    }
    return 0;
}

static int cmd_uart(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 1;
    }
    load_opts_t opts;
    opts_init(&opts);
    const char *path = argv[0];
    argc--;
    argv++;
    if (parse_common_opts(&argc, &argv, &opts) != 0) {
        return 1;
    }
    crit_sig_set_t set;
    if (load_uart_l0(&set, path) <= 0) {
        fprintf(stderr, "no L0 win lines in %s\n", path);
        return 1;
    }
    set.start_seq_hint = opts.start_seq_hint;
    printf("uart windows=%u duration_s=%.1f\n", set.n, set.duration_s);
    eval_set(&set, opts.mode);
    if (opts.csv_path) {
        dump_csv(&set, opts.csv_path);
    }
    return 0;
}

static int cmd_wav(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 1;
    }
    load_opts_t opts;
    opts_init(&opts);
    opts.origin = "unknown";
    const char *path = argv[0];
    argc--;
    argv++;
    if (parse_common_opts(&argc, &argv, &opts) != 0) {
        return 1;
    }
    crit_sig_set_t set;
    if (load_wav(&set, path, &opts) <= 0) {
        return 1;
    }
    eval_set(&set, opts.mode);
    if (opts.csv_path) {
        dump_csv(&set, opts.csv_path);
    }
    return 0;
}

static int cmd_raw(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 1;
    }
    load_opts_t opts;
    opts_init(&opts);
    opts.origin = "unknown";
    const char *path = argv[0];
    argc--;
    argv++;
    raw_format_t fmt = RAW_S32LE_FS24;
    if (argc >= 2 && strcmp(argv[0], "--format") == 0) {
        fmt = parse_raw_format(argv[1]);
        if ((int)fmt < 0) {
            fprintf(stderr, "bad --format %s\n", argv[1]);
            return 1;
        }
        argc -= 2;
        argv += 2;
    }
    if (parse_common_opts(&argc, &argv, &opts) != 0) {
        return 1;
    }
    crit_sig_set_t set;
    if (load_raw(&set, path, fmt, &opts) <= 0) {
        return 1;
    }
    eval_set(&set, opts.mode);
    if (opts.csv_path) {
        dump_csv(&set, opts.csv_path);
    }
    return 0;
}

/* CSV manifest:
 * recording_id,audio_path,label,role,origin,applied_gain_db,pcm_format,channel,start_seq_hint,notes
 * pcm_format: wav | s32le_fs24 | s16le | s24_in_s32le | s32le
 * channel: mono|0|1|mix
 */
static int csv_split(char *line, char **fields, int max_fields)
{
    int n = 0;
    char *p = line;
    while (n < max_fields) {
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (c == NULL) {
            break;
        }
        *c = '\0';
        p = c + 1;
    }
    return n;
}

static int cmd_dataset(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 1;
    }
    const char *manifest = argv[0];
    const char *out_dir = "/tmp/crit_offline_out";
    if (argc >= 3 && strcmp(argv[1], "--out-dir") == 0) {
        out_dir = argv[2];
    }
    FILE *f = fopen(manifest, "r");
    if (f == NULL) {
        perror(manifest);
        return 1;
    }
    char line[2048];
    int line_no = 0;
    int processed = 0;
    int failed = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        line_no++;
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        if (strncmp(line, "recording_id,", 13) == 0) {
            continue; /* header */
        }
        char *fields[12];
        const int nf = csv_split(line, fields, 12);
        if (nf < 6) {
            fprintf(stderr, "manifest line %d: need >=6 fields\n", line_no);
            failed++;
            continue;
        }
        const char *rid = fields[0];
        const char *audio = fields[1];
        const char *label = fields[2];
        const char *role = fields[3];
        const char *origin = fields[4];
        const float gain = (float)atof(fields[5]);
        const char *pcm_format = (nf > 6 && fields[6][0]) ? fields[6] : "wav";
        const char *channel = (nf > 7 && fields[7][0]) ? fields[7] : "mono";
        const int start_hint = (nf > 8 && fields[8][0]) ? atoi(fields[8]) : 0;

        load_opts_t opts;
        opts_init(&opts);
        opts.gain_db = gain;
        opts.label = label;
        opts.recording_id = rid;
        opts.role = role;
        opts.origin = origin;
        opts.start_seq_hint = start_hint;
        opts.mode = EVAL_BOTH;
        if (strcmp(channel, "0") == 0 || strcmp(channel, "left") == 0) {
            opts.channel = CH_LEFT;
        } else if (strcmp(channel, "1") == 0 || strcmp(channel, "right") == 0) {
            opts.channel = CH_RIGHT;
        } else if (strcmp(channel, "mix") == 0) {
            opts.channel = CH_MIX;
        } else {
            opts.channel = CH_MONO;
        }

        char csv_path[512];
        snprintf(csv_path, sizeof(csv_path), "%s/%s.csv", out_dir, rid);
        opts.csv_path = csv_path;

        printf("\n######## dataset item id=%s path=%s gain_db=%.2f origin=%s role=%s ########\n",
               rid,
               audio,
               (double)gain,
               origin,
               role);

        crit_sig_set_t set;
        int ok = 0;
        if (strcmp(pcm_format, "wav") == 0) {
            ok = load_wav(&set, audio, &opts);
        } else {
            raw_format_t fmt = parse_raw_format(pcm_format);
            if ((int)fmt < 0) {
                fprintf(stderr, "bad pcm_format %s\n", pcm_format);
                failed++;
                continue;
            }
            ok = load_raw(&set, audio, fmt, &opts);
        }
        if (ok <= 0) {
            failed++;
            continue;
        }
        eval_set(&set, opts.mode);
        dump_csv(&set, csv_path);
        processed++;
    }
    fclose(f);
    printf("\ndataset done processed=%d failed=%d\n", processed, failed);
    return failed ? 1 : 0;
}

static int cmd_run_all(int argc, char **argv)
{
    for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); ++i) {
        crit_sig_set_t set;
        if (gen_scenario(&set, k_scenarios[i]) != 0) {
            return 1;
        }
        char path[256];
        snprintf(path, sizeof(path), "/tmp/crit_offline_out/%s.csv", k_scenarios[i]);
        dump_csv(&set, path);
        eval_set(&set, EVAL_BOTH);
    }
    if (argc >= 1) {
        crit_sig_set_t set;
        if (load_uart_l0(&set, argv[0]) > 0) {
            dump_csv(&set, "/tmp/crit_offline_out/uart_l0.csv");
            eval_set(&set, EVAL_BOTH);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "list-scenarios") == 0) {
        for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); ++i) {
            printf("%s\n", k_scenarios[i]);
        }
        return 0;
    }
    if (strcmp(argv[1], "synth") == 0) {
        return cmd_synth(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "uart") == 0) {
        return cmd_uart(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "wav") == 0) {
        return cmd_wav(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "raw") == 0) {
        return cmd_raw(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "dataset") == 0) {
        return cmd_dataset(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "run-all") == 0) {
        return cmd_run_all(argc - 2, argv + 2);
    }
    usage();
    return 1;
}
