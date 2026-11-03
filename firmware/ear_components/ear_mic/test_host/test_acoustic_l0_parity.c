/**
 * Numerical parity: production float L0 analyze vs reference double hot path.
 *
 * Tolerances (justified):
 * - valid / clipped / clipped_sample_count: exact match (decision must not flip)
 * - hl_valid: exact match
 * - rms/peak/crest: 0.05 dB typical; 0.15 dB when |rms| < -80 dBFS
 * - band_dbfs: 0.10 dB typical; 0.25 dB when band < -80 dBFS
 * - hl_db: 0.15 dB when both hl_valid
 * - band_energy_linear: relative 2e-3 or abs 1e-14
 * - derived band fractions: 0.01 abs when total energy > 1e-9
 */

#include "dbmap_acoustic_l0.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { FS_BITS = 24, SR = 16000, WIN_MS = 1000, N_SAMP = 16000 };
#define FS ((double)(1LL << (FS_BITS - 1)))

#define FAIL(msg)                                                              \
    do {                                                                       \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);           \
        return 1;                                                              \
    } while (0)

static double full_scale_magnitude(int full_scale_bits)
{
    if (full_scale_bits < 2) {
        full_scale_bits = 2;
    }
    if (full_scale_bits > 31) {
        full_scale_bits = 31;
    }
    return (double)(1LL << (full_scale_bits - 1));
}

static float power_to_dbfs_d(double power_rel)
{
    if (!(power_rel > 0.0)) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    double dbfs = 10.0 * log10(power_rel);
    if (dbfs < DBMAP_L0_SILENCE_DBFS) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    if (dbfs > 0.0) {
        return 0.0f;
    }
    return (float)dbfs;
}

static float amp_to_dbfs_d(double magnitude, double full_scale)
{
    if (!(magnitude > 0.0) || !(full_scale > 0.0)) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    double ratio = magnitude / full_scale;
    if (ratio <= 0.0) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    double dbfs = 20.0 * log10(ratio);
    if (dbfs < DBMAP_L0_SILENCE_DBFS) {
        return DBMAP_L0_SILENCE_DBFS;
    }
    if (dbfs > 0.0) {
        return 0.0f;
    }
    return (float)dbfs;
}

static float biquad_process(dbmap_l0_biquad_t *bq, float x)
{
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

/** Pre-opt double hot-path reference (same filters/topology). */
static void analyze_window_ref_double(
    dbmap_l0_filter_bank_t *bank,
    const int32_t *samples,
    size_t count,
    int full_scale_bits,
    uint32_t window_ms,
    int64_t window_end_mono_us,
    dbmap_l0_window_t *out
)
{
    memset(out, 0, sizeof(*out));
    out->window_end_mono_us = window_end_mono_us;
    out->window_ms = window_ms;
    out->rms_dbfs = DBMAP_L0_SILENCE_DBFS;
    out->peak_dbfs = DBMAP_L0_SILENCE_DBFS;
    for (int i = 0; i < DBMAP_L0_BAND_COUNT; ++i) {
        out->band_dbfs[i] = DBMAP_L0_SILENCE_DBFS;
    }
    if (bank == NULL || samples == NULL || count == 0) {
        out->valid = false;
        return;
    }
    dbmap_l0_filter_bank_reset(bank);

    const double fs_mag = full_scale_magnitude(full_scale_bits);
    const double inv_fs = 1.0 / fs_mag;
    double sum_sq = 0.0;
    int32_t peak_abs = 0;
    uint32_t clipped = 0;
    const int32_t clip_level = (int32_t)(fs_mag - 1.0);
    double band_sum_sq[DBMAP_L0_BAND_COUNT];
    memset(band_sum_sq, 0, sizeof(band_sum_sq));

    for (size_t n = 0; n < count; ++n) {
        int32_t s = samples[n];
        int32_t a = s;
        if (a < 0) {
            a = (a == INT32_MIN) ? INT32_MAX : -a;
        }
        if (a > peak_abs) {
            peak_abs = a;
        }
        if (a >= clip_level) {
            clipped++;
        }
        double xn = (double)s * inv_fs;
        sum_sq += xn * xn;
        float xf = (float)xn;
        for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
            float y = biquad_process(&bank->bands[b].hp, xf);
            y = biquad_process(&bank->bands[b].lp, y);
            band_sum_sq[b] += (double)y * (double)y;
        }
    }

    out->sample_count = (uint32_t)count;
    out->clipped_sample_count = clipped;
    out->valid = true;
    out->rms_dbfs = amp_to_dbfs_d(sqrt(sum_sq / (double)count), 1.0);
    out->peak_dbfs = amp_to_dbfs_d((double)peak_abs * inv_fs, 1.0);
    out->crest_db = out->peak_dbfs - out->rms_dbfs;
    out->clipped = (clipped > 0) || (out->peak_dbfs >= DBMAP_L0_CLIP_PEAK_DBFS);

    double e_low = 0.0;
    double e_high = 0.0;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        double e = band_sum_sq[b] / (double)count;
        out->band_energy_linear[b] = e;
        out->band_dbfs[b] = power_to_dbfs_d(e);
        if (b <= DBMAP_L0_BAND_B1) {
            e_low += e;
        } else {
            e_high += e;
        }
    }

    if (e_low > DBMAP_L0_VALID_ENERGY_FLOOR && e_high > 0.0) {
        out->hl_valid = true;
        out->hl_db = (float)(10.0 * log10(e_high / e_low));
    } else if (e_high > DBMAP_L0_VALID_ENERGY_FLOOR && e_low > 0.0) {
        out->hl_valid = true;
        out->hl_db = (float)(10.0 * log10(e_high / e_low));
    } else {
        out->hl_valid = false;
        out->hl_db = 0.0f;
    }
}

static void fill_tone(int32_t *pcm, size_t n, double freq_hz, double amp_fs)
{
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)SR;
        pcm[i] = (int32_t)lrint(amp_fs * sin(2.0 * M_PI * freq_hz * t) * FS);
    }
}

static void fill_two_tone(
    int32_t *pcm, size_t n, double f0, double a0, double f1, double a1
)
{
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)SR;
        const double x =
            a0 * sin(2.0 * M_PI * f0 * t) + a1 * sin(2.0 * M_PI * f1 * t);
        pcm[i] = (int32_t)lrint(x * FS);
    }
}

static void fill_impulse(int32_t *pcm, size_t n, double amp_fs)
{
    memset(pcm, 0, n * sizeof(int32_t));
    pcm[n / 2] = (int32_t)lrint(amp_fs * FS);
}

static int near_db(float a, float b, float tol_lo, float tol_hi, float level_db)
{
    const float tol = (level_db < -80.0f) ? tol_hi : tol_lo;
    return fabsf(a - b) <= tol;
}

static int compare_windows(const dbmap_l0_window_t *ref, const dbmap_l0_window_t *opt, const char *tag)
{
    if (ref->valid != opt->valid) {
        fprintf(stderr, "%s: valid ref=%d opt=%d\n", tag, ref->valid, opt->valid);
        return 1;
    }
    if (!ref->valid) {
        return 0;
    }
    if (ref->clipped != opt->clipped) {
        fprintf(stderr, "%s: clipped ref=%d opt=%d\n", tag, ref->clipped, opt->clipped);
        return 1;
    }
    if (ref->clipped_sample_count != opt->clipped_sample_count) {
        fprintf(
            stderr,
            "%s: clipped_sample_count ref=%lu opt=%lu\n",
            tag,
            (unsigned long)ref->clipped_sample_count,
            (unsigned long)opt->clipped_sample_count
        );
        return 1;
    }
    if (ref->hl_valid != opt->hl_valid) {
        fprintf(stderr, "%s: hl_valid ref=%d opt=%d\n", tag, ref->hl_valid, opt->hl_valid);
        return 1;
    }
    if (!near_db(ref->rms_dbfs, opt->rms_dbfs, 0.05f, 0.15f, ref->rms_dbfs)) {
        fprintf(stderr, "%s: rms ref=%.4f opt=%.4f\n", tag, ref->rms_dbfs, opt->rms_dbfs);
        return 1;
    }
    if (!near_db(ref->peak_dbfs, opt->peak_dbfs, 0.05f, 0.15f, ref->peak_dbfs)) {
        fprintf(stderr, "%s: peak ref=%.4f opt=%.4f\n", tag, ref->peak_dbfs, opt->peak_dbfs);
        return 1;
    }
    if (!near_db(ref->crest_db, opt->crest_db, 0.05f, 0.15f, ref->rms_dbfs)) {
        fprintf(stderr, "%s: crest ref=%.4f opt=%.4f\n", tag, ref->crest_db, opt->crest_db);
        return 1;
    }
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        if (!near_db(ref->band_dbfs[b], opt->band_dbfs[b], 0.10f, 0.25f, ref->band_dbfs[b])) {
            fprintf(
                stderr,
                "%s: band%d dbfs ref=%.4f opt=%.4f\n",
                tag,
                b,
                ref->band_dbfs[b],
                opt->band_dbfs[b]
            );
            return 1;
        }
        const double er = ref->band_energy_linear[b];
        const double eo = opt->band_energy_linear[b];
        const double abs_err = fabs(er - eo);
        const double rel = (fabs(er) > 1e-18) ? abs_err / fabs(er) : abs_err;
        if (abs_err > 1e-14 && rel > 2e-3) {
            fprintf(
                stderr,
                "%s: band%d energy ref=%.6e opt=%.6e rel=%.3e\n",
                tag,
                b,
                er,
                eo,
                rel
            );
            return 1;
        }
    }
    if (ref->hl_valid) {
        if (!near_db(ref->hl_db, opt->hl_db, 0.15f, 0.15f, -40.0f)) {
            fprintf(stderr, "%s: hl ref=%.4f opt=%.4f\n", tag, ref->hl_db, opt->hl_db);
            return 1;
        }
    }

    double tot_r = 0.0;
    double tot_o = 0.0;
    for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
        tot_r += ref->band_energy_linear[b];
        tot_o += opt->band_energy_linear[b];
    }
    if (tot_r > 1e-9 && tot_o > 1e-9) {
        for (int b = 0; b < DBMAP_L0_BAND_COUNT; ++b) {
            const float fr = (float)(ref->band_energy_linear[b] / tot_r);
            const float fo = (float)(opt->band_energy_linear[b] / tot_o);
            if (fabsf(fr - fo) > 0.01f) {
                fprintf(stderr, "%s: frac%d ref=%.4f opt=%.4f\n", tag, b, fr, fo);
                return 1;
            }
        }
    }
    return 0;
}

static int run_case(const char *tag, int32_t *pcm)
{
    dbmap_l0_filter_bank_t bank_ref;
    dbmap_l0_filter_bank_t bank_opt;
    dbmap_l0_filter_bank_init(&bank_ref, SR);
    dbmap_l0_filter_bank_init(&bank_opt, SR);

    dbmap_l0_window_t ref;
    dbmap_l0_window_t opt;
    analyze_window_ref_double(&bank_ref, pcm, N_SAMP, FS_BITS, WIN_MS, 1000000, &ref);
    dbmap_l0_analyze_window(&bank_opt, pcm, N_SAMP, FS_BITS, WIN_MS, 1000000, &opt);
    if (compare_windows(&ref, &opt, tag)) {
        return 1;
    }
    return 0;
}

static int test_stages_sample_loop_dominates(void)
{
    dbmap_l0_filter_bank_t bank;
    dbmap_l0_filter_bank_init(&bank, SR);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    if (pcm == NULL) {
        FAIL("oom");
    }
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    dbmap_l0_window_t w;
    dbmap_l0_analyze_window(&bank, pcm, N_SAMP, FS_BITS, WIN_MS, 1, &w);
    free(pcm);

    dbmap_l0_analyze_stages_t st;
    dbmap_l0_get_last_analyze_stages(&st);
    const uint32_t sum = st.reset_us + st.sample_loop_us + st.post_us;
    if (sum == 0) {
        FAIL("stage timers returned zero (clock unavailable?)");
    }
    /* Sample loop must dominate reset+post on host as well. */
    if (st.sample_loop_us * 5u < sum * 4u) {
        fprintf(
            stderr,
            "sample loop not dominant: reset=%u loop=%u post=%u\n",
            st.reset_us,
            st.sample_loop_us,
            st.post_us
        );
        return 1;
    }
    printf(
        "stages_us reset=%u loop=%u post=%u sum=%u loop_frac=%.3f\n",
        st.reset_us,
        st.sample_loop_us,
        st.post_us,
        sum,
        (double)st.sample_loop_us / (double)sum
    );
    return 0;
}

int main(void)
{
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    if (pcm == NULL) {
        FAIL("oom");
    }

    if (test_stages_sample_loop_dominates()) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    if (run_case("ambient_1k", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 100.0, 0.15);
    if (run_case("band0_100hz", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 500.0, 0.15);
    if (run_case("band1_500hz", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 2000.0, 0.15);
    if (run_case("band2_2k", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 5000.0, 0.15);
    if (run_case("band3_5k", pcm)) {
        free(pcm);
        return 1;
    }

    fill_two_tone(pcm, N_SAMP, 120.0, 0.12, 3000.0, 0.12);
    if (run_case("two_tone_hl", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 1000.0, 1e-5);
    if (run_case("near_floor", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 1000.0, 1e-6);
    if (run_case("quieter_near_floor", pcm)) {
        free(pcm);
        return 1;
    }

    memset(pcm, 0, N_SAMP * sizeof(int32_t));
    if (run_case("silence", pcm)) {
        free(pcm);
        return 1;
    }

    fill_tone(pcm, N_SAMP, 1000.0, 0.999);
    if (run_case("strong_near_fs", pcm)) {
        free(pcm);
        return 1;
    }

    fill_impulse(pcm, N_SAMP, 0.99);
    if (run_case("impulse_high_crest", pcm)) {
        free(pcm);
        return 1;
    }

    /* Force clip: full-scale sample */
    memset(pcm, 0, N_SAMP * sizeof(int32_t));
    pcm[100] = (int32_t)(1LL << (FS_BITS - 1)) - 1;
    if (run_case("single_clip_sample", pcm)) {
        free(pcm);
        return 1;
    }

    free(pcm);
    puts("ok");
    return 0;
}
