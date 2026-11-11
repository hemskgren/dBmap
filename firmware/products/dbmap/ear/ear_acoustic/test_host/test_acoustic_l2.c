/**
 * Host golden / contract tests for L2 Acoustic Dynamics.
 *
 * Build (from repo root):
 *   cc -std=c11 -Wall -Wextra -O2 -g \
 *     -I firmware/ear_components/ear_mic/include \
 *     firmware/ear_components/ear_mic/acoustic_l1.c \
 *     firmware/ear_components/ear_mic/acoustic_l2.c \
 *     firmware/ear_components/ear_mic/test_host/test_acoustic_l2.c \
 *     -lm -o /tmp/test_acoustic_l2 && /tmp/test_acoustic_l2
 *
 * Independent double-precision reference (ref_*) is used for golden comparison;
 * it does not call dbmap_l2_* / dbmap_l1_analyze_window_ex.
 */

#include "dbmap_acoustic_l1.h"
#include "dbmap_acoustic_l2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { FS_BITS = 24, SR = 16000, WIN_MS = 1000, N_SAMP = 16000, FFT_N = 1024 };
#define FS ((double)(1LL << (FS_BITS - 1)))
#define BIN_HZ ((double)SR / (double)FFT_N)

/* Contract provisional tolerances (docs/diagnostics/ear-l2-acoustic-dynamics-contract.md). */
#define EPS_FLUX_ABS 5.0e-3   /* PROVISIONAL */
#define EPS_ENV_ABS 0.02      /* PROVISIONAL */
#define EPS_REL 0.05          /* PROVISIONAL */
#define EPS_DELTA_HZ BIN_HZ   /* 1 bin */

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int g_failures;

static int near_abs(double a, double b, double eps)
{
    return fabs(a - b) <= eps;
}

static int near_rel(double a, double b, double eps_rel, double eps_abs)
{
    const double scale = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    double lim = eps_rel * scale;
    if (lim < eps_abs) {
        lim = eps_abs;
    }
    return fabs(a - b) <= lim;
}

/* ---- PCM generators ---- */

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

static void fill_noise(int32_t *pcm, size_t n, unsigned seed, double amp_fs)
{
    unsigned s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        const double u = ((double)(s >> 8) / 16777215.0) * 2.0 - 1.0;
        pcm[i] = (int32_t)lrint(u * amp_fs * FS);
    }
}

static void fill_am_tone(
    int32_t *pcm,
    size_t n,
    double carrier_hz,
    double mod_hz,
    double depth,
    double amp_fs,
    double phase_rad
)
{
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)SR;
        const double env = 1.0 + depth * sin(2.0 * M_PI * mod_hz * t + phase_rad);
        const double x = amp_fs * env * sin(2.0 * M_PI * carrier_hz * t);
        pcm[i] = (int32_t)lrint(x * FS);
    }
}

static void fill_chirp(
    int32_t *pcm, size_t n, double f0, double f1, double amp_fs
)
{
    /* Linear FM; phase = 2π ∫ f(t) dt */
    const double T = (double)n / (double)SR;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / (double)SR;
        const double f = f0 + (f1 - f0) * (t / T);
        const double phase =
            2.0 * M_PI *
            (f0 * t + 0.5 * (f1 - f0) * (t * t) / T);
        (void)f;
        pcm[i] = (int32_t)lrint(amp_fs * sin(phase) * FS);
    }
}

static void fill_pulse_train(
    int32_t *pcm, size_t n, double period_s, double pulse_s, double amp_fs
)
{
    memset(pcm, 0, n * sizeof(int32_t));
    const size_t period = (size_t)lrint(period_s * (double)SR);
    const size_t width = (size_t)lrint(pulse_s * (double)SR);
    if (period == 0 || width == 0) {
        return;
    }
    for (size_t start = 0; start < n; start += period) {
        for (size_t k = 0; k < width && start + k < n; k++) {
            pcm[start + k] = (int32_t)lrint(amp_fs * FS);
        }
    }
}

/* ---- Independent double-precision FFT / L2 reference ---- */

static void ref_bitrev(double *re, double *im, uint32_t n)
{
    uint32_t j = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i < j) {
            double tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            double ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
        uint32_t m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
}

static void ref_fft(double *re, double *im, uint32_t n)
{
    ref_bitrev(re, im, n);
    for (uint32_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / (double)len;
        const double wlen_re = cos(ang);
        const double wlen_im = sin(ang);
        for (uint32_t i = 0; i < n; i += len) {
            double wr = 1.0;
            double wi = 0.0;
            for (uint32_t j = 0; j < len / 2; j++) {
                const uint32_t u = i + j;
                const uint32_t v = i + j + len / 2;
                const double tr = wr * re[v] - wi * im[v];
                const double ti = wr * im[v] + wi * re[v];
                re[v] = re[u] - tr;
                im[v] = im[u] - ti;
                re[u] += tr;
                im[u] += ti;
                const double nwr = wr * wlen_re - wi * wlen_im;
                wi = wr * wlen_im + wi * wlen_re;
                wr = nwr;
            }
        }
    }
}

typedef struct {
    int has_flux;
    double flux;
    uint32_t flux_pairs;
    int has_env;
    double env_cv;
    uint32_t env_n;
} ref_intra_t;

static void ref_intra_from_pcm(const int32_t *pcm, size_t count, ref_intra_t *out)
{
    memset(out, 0, sizeof(*out));
    if (pcm == NULL || count < (size_t)FFT_N) {
        return;
    }

    double hann[FFT_N];
    for (uint32_t i = 0; i < (uint32_t)FFT_N; i++) {
        hann[i] = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(FFT_N - 1)));
    }

    double prev_power[FFT_N / 2 + 1];
    int has_prev = 0;
    double flux_sum = 0.0;
    uint32_t flux_pairs = 0;
    double rms_sum = 0.0;
    double rms_sum_sq = 0.0;
    uint32_t rms_n = 0;
    const double inv_fs = 1.0 / FS;
    const double energy_floor = 1.0e-12;
    const double rms_eps = 1.0e-6;

    for (size_t offset = 0; offset + (size_t)FFT_N <= count; offset += (size_t)FFT_N) {
        double sum_sq = 0.0;
        for (uint32_t n = 0; n < (uint32_t)FFT_N; n++) {
            const double x = (double)pcm[offset + n] * inv_fs;
            sum_sq += x * x;
        }
        const double r = sqrt(sum_sq / (double)FFT_N);
        if (r > rms_eps) {
            rms_sum += r;
            rms_sum_sq += r * r;
            rms_n++;
        }

        double re[FFT_N];
        double im[FFT_N];
        for (uint32_t n = 0; n < (uint32_t)FFT_N; n++) {
            re[n] = ((double)pcm[offset + n] * inv_fs) * hann[n];
            im[n] = 0.0;
        }
        ref_fft(re, im, (uint32_t)FFT_N);

        double power[FFT_N / 2 + 1];
        for (uint32_t k = 0; k <= (uint32_t)FFT_N / 2; k++) {
            power[k] = re[k] * re[k] + im[k] * im[k];
        }

        if (has_prev) {
            double num = 0.0;
            double den = 0.0;
            double prev_e = 0.0;
            for (uint32_t k = 0; k <= (uint32_t)FFT_N / 2; k++) {
                const double f = BIN_HZ * (double)k;
                if (f < 80.0 || f > 7600.0) {
                    continue;
                }
                num += fabs(power[k] - prev_power[k]);
                den += power[k];
                prev_e += prev_power[k];
            }
            if (den > energy_floor && prev_e > energy_floor) {
                flux_sum += num / den;
                flux_pairs++;
            }
        }
        memcpy(prev_power, power, sizeof(prev_power));
        has_prev = 1;
    }

    if (flux_pairs > 0) {
        out->has_flux = 1;
        out->flux = flux_sum / (double)flux_pairs;
        out->flux_pairs = flux_pairs;
    }
    if (rms_n >= 2u) {
        const double mu = rms_sum / (double)rms_n;
        if (mu > rms_eps) {
            double var = (rms_sum_sq / (double)rms_n) - mu * mu;
            if (var < 0.0) {
                var = 0.0;
            }
            out->has_env = 1;
            out->env_cv = sqrt(var) / mu;
            out->env_n = rms_n;
        }
    }
}

static int analyze_l2(
    dbmap_l1_workspace_t *ws,
    const int32_t *pcm,
    int64_t t_end,
    dbmap_l1_window_t *l1,
    dbmap_l2_window_t *l2
)
{
    dbmap_l2_analyze_window(
        ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, t_end, l1, l2
    );
    return 0;
}

/* ---- Tests ---- */

static int test_h1_stable_tone(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);

    ref_intra_t ref;
    ref_intra_from_pcm(pcm, N_SAMP, &ref);
    EXPECT(ref.has_flux);
    EXPECT(ref.has_env);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    free(pcm);

    EXPECT(l2.eligible);
    EXPECT(l2.frame_count == 15);
    EXPECT(l2.has_spectral_flux);
    EXPECT(l2.has_envelope_cv);
    EXPECT(near_abs(l2.spectral_flux, ref.flux, EPS_FLUX_ABS));
    EXPECT(near_rel(l2.spectral_flux, ref.flux, EPS_REL, EPS_FLUX_ABS));
    EXPECT(near_abs(l2.envelope_cv, ref.env_cv, EPS_ENV_ABS));
    EXPECT(near_rel(l2.envelope_cv, ref.env_cv, EPS_REL, EPS_ENV_ABS));
    /* Steady tone: flux and CV should be small (near-zero absolute gate). */
    EXPECT(l2.spectral_flux < 0.05);
    EXPECT(l2.envelope_cv < 0.05);

    printf(
        "H1 golden (ref): flux=%.6g cv=%.6g | dut: flux=%.6g cv=%.6g\n",
        ref.flux,
        ref.env_cv,
        (double)l2.spectral_flux,
        (double)l2.envelope_cv
    );
    return 0;
}

static int test_h2_multi_tone(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_two_tone(pcm, N_SAMP, 1000.0, 0.08, 2000.0, 0.2);

    ref_intra_t ref;
    ref_intra_from_pcm(pcm, N_SAMP, &ref);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 2000000, &l1, &l2);
    free(pcm);

    EXPECT(l2.eligible);
    EXPECT(l2.has_spectral_flux);
    EXPECT(l2.has_envelope_cv);
    EXPECT(near_abs(l2.spectral_flux, ref.flux, EPS_FLUX_ABS));
    EXPECT(near_abs(l2.envelope_cv, ref.env_cv, EPS_ENV_ABS));
    EXPECT(l2.spectral_flux < 0.08);
    EXPECT(l2.envelope_cv < 0.08);
    return 0;
}

static int test_h3_noise(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm_tone = calloc(N_SAMP, sizeof(int32_t));
    int32_t *pcm_noise = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm_tone && pcm_noise);
    fill_tone(pcm_tone, N_SAMP, 1000.0, 0.2);
    fill_noise(pcm_noise, N_SAMP, 42u, 0.15);

    dbmap_l1_window_t l1t, l1n;
    dbmap_l2_window_t l2t, l2n;
    analyze_l2(&ws, pcm_tone, 1000000, &l1t, &l2t);
    analyze_l2(&ws, pcm_noise, 2000000, &l1n, &l2n);
    free(pcm_tone);
    free(pcm_noise);

    EXPECT(l2n.eligible);
    EXPECT(l2n.has_spectral_flux);
    EXPECT(l2n.has_envelope_cv);
    /* PROVISIONAL ordering: noise flux > tone flux */
    EXPECT(l2n.spectral_flux > l2t.spectral_flux);
    printf(
        "H3: noise_flux=%.6g tone_flux=%.6g (ordering ok); "
        "noise_cv=%.6g (peak-hop Δf not asserted)\n",
        (double)l2n.spectral_flux,
        (double)l2t.spectral_flux,
        (double)l2n.envelope_cv
    );
    return 0;
}

static int test_h4_am_phases(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    dbmap_l1_window_t l1_tone;
    dbmap_l2_window_t l2_tone;
    analyze_l2(&ws, pcm, 1000000, &l1_tone, &l2_tone);
    EXPECT(l2_tone.has_envelope_cv);

    const double phases[] = {
        0.0, M_PI / 4.0, M_PI / 2.0, M_PI, 5.0 * M_PI / 4.0, 3.0 * M_PI / 2.0
    };
    const int n_phases = (int)(sizeof(phases) / sizeof(phases[0]));
    int elevated = 0;
    printf("H4 AM 5 Hz phase survey (depth=0.5) vs H1 cv=%.6g:\n", (double)l2_tone.envelope_cv);
    for (int i = 0; i < n_phases; i++) {
        fill_am_tone(pcm, N_SAMP, 1000.0, 5.0, 0.5, 0.2, phases[i]);
        ref_intra_t ref;
        ref_intra_from_pcm(pcm, N_SAMP, &ref);
        dbmap_l1_window_t l1;
        dbmap_l2_window_t l2;
        analyze_l2(&ws, pcm, (int64_t)(i + 2) * 1000000LL, &l1, &l2);
        EXPECT(l2.eligible);
        EXPECT(l2.has_envelope_cv);
        EXPECT(near_abs(l2.envelope_cv, ref.env_cv, EPS_ENV_ABS));
        if (l2.envelope_cv > l2_tone.envelope_cv) {
            elevated++;
        }
        printf(
            "  phase=%.3f rad  cv=%.6g  (ref=%.6g)\n",
            phases[i],
            (double)l2.envelope_cv,
            ref.env_cv
        );
    }
    free(pcm);
    /* Qualitative: at least some phases elevated; not all required above one cut. */
    EXPECT(elevated >= 1);
    printf("H4: %d/%d phases elevated vs tone (phase-sensitive; no universal threshold)\n",
           elevated,
           n_phases);
    return 0;
}

static int test_h5_chirp(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm_tone = calloc(N_SAMP, sizeof(int32_t));
    int32_t *pcm_chirp = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm_tone && pcm_chirp);
    fill_tone(pcm_tone, N_SAMP, 1000.0, 0.2);
    fill_chirp(pcm_chirp, N_SAMP, 500.0, 3000.0, 0.2);

    dbmap_l1_window_t l1t, l1c;
    dbmap_l2_window_t l2t, l2c;
    analyze_l2(&ws, pcm_tone, 1000000, &l1t, &l2t);
    analyze_l2(&ws, pcm_chirp, 2000000, &l1c, &l2c);

    ref_intra_t ref;
    ref_intra_from_pcm(pcm_chirp, N_SAMP, &ref);
    free(pcm_tone);
    free(pcm_chirp);

    EXPECT(l2c.eligible);
    EXPECT(l2c.has_spectral_flux);
    EXPECT(near_abs(l2c.spectral_flux, ref.flux, EPS_FLUX_ABS) ||
           near_rel(l2c.spectral_flux, ref.flux, EPS_REL, EPS_FLUX_ABS));
    EXPECT(l2c.spectral_flux > l2t.spectral_flux);
    return 0;
}

static int test_h6_pulse_train(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm_tone = calloc(N_SAMP, sizeof(int32_t));
    int32_t *pcm_pulse = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm_tone && pcm_pulse);
    fill_tone(pcm_tone, N_SAMP, 1000.0, 0.2);
    fill_pulse_train(pcm_pulse, N_SAMP, 0.05, 0.005, 0.4);

    dbmap_l1_window_t l1t, l1p;
    dbmap_l2_window_t l2t, l2p;
    analyze_l2(&ws, pcm_tone, 1000000, &l1t, &l2t);
    analyze_l2(&ws, pcm_pulse, 2000000, &l1p, &l2p);
    free(pcm_tone);
    free(pcm_pulse);

    EXPECT(l2p.eligible);
    EXPECT(l2p.has_envelope_cv);
    EXPECT(l2p.has_spectral_flux);
    EXPECT(l2p.envelope_cv > l2t.envelope_cv);
    EXPECT(l2p.spectral_flux > l2t.spectral_flux);
    return 0;
}

static int test_h7_near_silence(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    EXPECT(!l2.eligible);
    EXPECT(!l2.has_spectral_flux);
    EXPECT(!l2.has_envelope_cv);

    fill_tone(pcm, N_SAMP, 1500.0, 1.0e-9);
    analyze_l2(&ws, pcm, 2000000, &l1, &l2);
    free(pcm);
    EXPECT(!l2.eligible);
    return 0;
}

static int test_h8_abrupt_delta(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    dbmap_l2_continuity_t cache;
    dbmap_l2_continuity_reset(&cache);

    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    dbmap_l1_window_t l1a;
    dbmap_l2_window_t l2a;
    analyze_l2(&ws, pcm, 1000000, &l1a, &l2a);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2a);
    EXPECT(!l2a.has_dominant_delta);

    fill_tone(pcm, N_SAMP, 3000.0, 0.2);
    dbmap_l1_window_t l1b;
    dbmap_l2_window_t l2b;
    analyze_l2(&ws, pcm, 2000000, &l1b, &l2b);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2b);
    free(pcm);

    EXPECT(l2b.has_dominant_delta);
    EXPECT(near_abs(l2b.dominant_delta_hz, 2000.0, EPS_DELTA_HZ));
    return 0;
}

static int test_h9_missing_and_invalid_prev(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    dbmap_l2_continuity_t cache;
    dbmap_l2_continuity_reset(&cache);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(cache.has_prev);

    /* Missing preceding: jump by 2 s (skip one window). */
    analyze_l2(&ws, pcm, 3000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(!l2.has_dominant_delta);
    EXPECT(cache.has_prev); /* current installed after break */

    /* Invalid/clipped preceding: clip then good. */
    fill_tone(pcm, N_SAMP, 1000.0, 1.0);
    pcm[10] = (int32_t)FS - 1;
    analyze_l2(&ws, pcm, 4000000, &l1, &l2);
    EXPECT(l2.clipped);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(!cache.has_prev);
    EXPECT(!l2.has_dominant_delta);

    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    analyze_l2(&ws, pcm, 5000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(!l2.has_dominant_delta); /* first after break */
    EXPECT(cache.has_prev);

    analyze_l2(&ws, pcm, 6000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(l2.has_dominant_delta);
    EXPECT(near_abs(l2.dominant_delta_hz, 0.0, EPS_DELTA_HZ));
    free(pcm);
    return 0;
}

static int test_h10_timestamp_gap(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    dbmap_l2_continuity_t cache;
    dbmap_l2_continuity_reset(&cache);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);

    /* Gap of 100001 µs beyond nominal 1 s (> ±100 ms). */
    analyze_l2(&ws, pcm, 1000000 + 1000000 + 100001, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(!l2.has_dominant_delta);

    /* Within tolerance: +50 ms. */
    analyze_l2(&ws, pcm, l2.window_end_mono_us + 1000000 + 50000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(l2.has_dominant_delta);
    free(pcm);
    return 0;
}

static int test_h11_episode_reset(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    dbmap_l2_continuity_t cache;
    dbmap_l2_continuity_reset(&cache);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(cache.has_prev);

    dbmap_l2_continuity_reset(&cache); /* episode END / reset */
    analyze_l2(&ws, pcm, 2000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(!l2.has_dominant_delta);
    free(pcm);
    return 0;
}

static int test_h12_interval_boundary(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    dbmap_l2_continuity_t cache;
    dbmap_l2_continuity_reset(&cache);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);

    dbmap_l2_interval_accum_t a0, a1;
    dbmap_l2_interval_accum_init(&a0, SR, WIN_MS, 0, 1000000);
    dbmap_l2_interval_accum_init(&a1, SR, WIN_MS, 1000000, 2000000);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    dbmap_l2_interval_accum_add(&a0, &l2);

    analyze_l2(&ws, pcm, 2000000, &l1, &l2);
    dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
    EXPECT(l2.has_dominant_delta);
    dbmap_l2_interval_accum_add(&a1, &l2);

    dbmap_l2_interval_t i0, i1;
    dbmap_l2_interval_accum_finalize(&a0, &i0);
    dbmap_l2_interval_accum_finalize(&a1, &i1);
    free(pcm);

    EXPECT(i0.window_count == 1);
    EXPECT(i0.dominant_delta_valid_count == 0); /* first window, no pair */
    EXPECT(i1.window_count == 1);
    EXPECT(i1.dominant_delta_valid_count == 1); /* pair attributed to later */
    EXPECT(i1.has_dominant_delta_mean);
    EXPECT(near_abs(i1.dominant_delta_mean_hz, 0.0, EPS_DELTA_HZ));
    EXPECT(!i1.has_dominant_delta_stdev);
    return 0;
}

static int test_overflow_vs_empty(void)
{
    dbmap_l2_interval_accum_t empty_acc, ov_acc;
    dbmap_l2_interval_accum_init(&empty_acc, SR, WIN_MS, 0, 1000000);
    dbmap_l2_interval_accum_init(&ov_acc, SR, WIN_MS, 0, 1000000);

    dbmap_l2_interval_t empty_out, ov_out;
    dbmap_l2_interval_accum_finalize(&empty_acc, &empty_out);
    EXPECT(!empty_out.history_overflow);
    EXPECT(empty_out.window_count == 0);
    EXPECT(empty_out.invalid_window_count == 0);
    EXPECT(empty_out.spectral_flux_valid_count == 0);
    EXPECT(!empty_out.has_spectral_flux_mean);

    /* Pretend we had data then overflow clears (Option A). */
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 500000, &l1, &l2);
    dbmap_l2_interval_accum_add(&ov_acc, &l2);
    EXPECT(ov_acc.window_count == 1);
    dbmap_l2_interval_accum_mark_overflow(&ov_acc);
    dbmap_l2_interval_accum_finalize(&ov_acc, &ov_out);
    free(pcm);

    EXPECT(ov_out.history_overflow);
    EXPECT(ov_out.window_count == 0);
    EXPECT(ov_out.invalid_window_count == 0);
    EXPECT(ov_out.spectral_flux_valid_count == 0);
    EXPECT(ov_out.envelope_cv_valid_count == 0);
    EXPECT(ov_out.dominant_delta_valid_count == 0);
    EXPECT(!ov_out.has_spectral_flux_mean);
    EXPECT(!ov_out.has_envelope_cv_mean);
    EXPECT(!ov_out.has_dominant_delta_mean);
    /* Distinguish: same zeros, different flag. */
    EXPECT(empty_out.history_overflow != ov_out.history_overflow);
    return 0;
}

static void fill_alternating_tone_silence(
    int32_t *pcm, size_t n, double freq_hz, double amp_fs
)
{
    /*
     * Even FFT frames: tone with alternating amplitudes; odd frames: silence.
     * Consecutive flux pairs always include a silent frame → M=0.
     * Envelope sees R≥2 energetic frames with level variation → envelope_cv > 0.
     */
    memset(pcm, 0, n * sizeof(int32_t));
    const size_t n_frames = n / (size_t)FFT_N;
    for (size_t fr = 0; fr < n_frames; fr++) {
        if ((fr % 2u) != 0u) {
            continue;
        }
        const double amp = ((fr % 4u) == 0u) ? amp_fs : (amp_fs * 0.35);
        for (size_t i = 0; i < (size_t)FFT_N; i++) {
            const size_t idx = fr * (size_t)FFT_N + i;
            if (idx >= n) {
                break;
            }
            const double t = (double)idx / (double)SR;
            pcm[idx] =
                (int32_t)lrint(amp * sin(2.0 * M_PI * freq_hz * t) * FS);
        }
    }
}

static int test_invalid_and_feature_specific(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    dbmap_l2_interval_accum_t acc;
    dbmap_l2_interval_accum_init(&acc, SR, WIN_MS, 0, 5000000);

    /* Ordinary invalid (silence). */
    memset(pcm, 0, N_SAMP * sizeof(int32_t));
    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);
    EXPECT(!l2.eligible);
    dbmap_l2_interval_accum_add(&acc, &l2);

    /* Eligible tone. */
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);
    analyze_l2(&ws, pcm, 2000000, &l1, &l2);
    EXPECT(l2.eligible);
    EXPECT(l2.has_spectral_flux);
    EXPECT(l2.has_envelope_cv);
    dbmap_l2_interval_accum_add(&acc, &l2);

    dbmap_l2_interval_t out;
    dbmap_l2_interval_accum_finalize(&acc, &out);
    free(pcm);

    EXPECT(out.window_count == 1);
    EXPECT(out.invalid_window_count == 1);
    EXPECT(out.spectral_flux_valid_count == 1);
    EXPECT(out.envelope_cv_valid_count == 1);
    return 0;
}

/**
 * Feature independence via production path: L2-eligible window with
 * spectral_flux absent (M=0) while envelope_cv present.
 */
static int test_feature_independence_flux_absent_env_present(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_alternating_tone_silence(pcm, N_SAMP, 1000.0, 0.2);

    ref_intra_t ref;
    ref_intra_from_pcm(pcm, N_SAMP, &ref);
    EXPECT(!ref.has_flux);
    EXPECT(ref.has_env);
    EXPECT(ref.env_n >= 2u);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t l2;
    analyze_l2(&ws, pcm, 1000000, &l1, &l2);

    EXPECT(l1.valid);
    EXPECT(!l1.clipped);
    EXPECT(l1.frame_count == 15);
    EXPECT(l2.eligible);
    EXPECT(!l2.has_spectral_flux);
    EXPECT(l2.has_envelope_cv);
    EXPECT(l2.envelope_cv > 0.05); /* level alternation on even frames */
    EXPECT(near_abs(l2.envelope_cv, ref.env_cv, EPS_ENV_ABS));

    dbmap_l2_interval_accum_t acc;
    dbmap_l2_interval_accum_init(&acc, SR, WIN_MS, 0, 2000000);
    dbmap_l2_interval_accum_add(&acc, &l2);
    dbmap_l2_interval_t out;
    dbmap_l2_interval_accum_finalize(&acc, &out);
    free(pcm);

    EXPECT(out.window_count == 1);
    EXPECT(out.invalid_window_count == 0);
    EXPECT(out.spectral_flux_valid_count == 0);
    EXPECT(!out.has_spectral_flux_mean);
    EXPECT(out.envelope_cv_valid_count == 1);
    EXPECT(out.has_envelope_cv_mean);
    EXPECT(!out.has_envelope_cv_stdev);
    printf(
        "feature_independence: eligible=1 flux_vc=0 env_vc=1 env_cv=%.6g\n",
        (double)l2.envelope_cv
    );
    return 0;
}

static void note_err(
    const char *feat,
    double dut,
    double ref,
    int present,
    double *max_abs,
    double *max_rel,
    int *n_near_zero,
    int *n_away
)
{
    if (!present) {
        return;
    }
    const double a = fabs(dut - ref);
    if (a > *max_abs) {
        *max_abs = a;
    }
    /* Relative only away from zero (contract near-zero rule). */
    if (fabs(ref) < 1.0e-12 && fabs(dut) < 1.0e-12) {
        (*n_near_zero)++;
        return;
    }
    (*n_away)++;
    const double scale = fabs(ref) > fabs(dut) ? fabs(ref) : fabs(dut);
    if (scale > 0.0) {
        const double r = a / scale;
        if (r > *max_rel) {
            *max_rel = r;
        }
    }
    (void)feat;
}

static int test_golden_float_double_error_survey(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    double max_abs_flux = 0.0, max_rel_flux = 0.0;
    double max_abs_env = 0.0, max_rel_env = 0.0;
    int nz_flux = 0, away_flux = 0, nz_env = 0, away_env = 0;

    typedef struct {
        const char *name;
        int kind; /* 0 tone, 1 two, 2 noise, 3 am, 4 chirp, 5 pulse, 6 alt */
        double a, b, c, d;
    } case_t;

    const case_t table[] = {
        {"H1_tone", 0, 1000.0, 0.2, 0, 0},
        {"H2_two_tone", 1, 1000.0, 0.08, 2000.0, 0.2},
        {"H3_noise", 2, 42.0, 0.15, 0, 0},
        {"H4_am_0", 3, 0.0, 0, 0, 0},
        {"H4_am_pi2", 3, M_PI / 2.0, 0, 0, 0},
        {"H5_chirp", 4, 500.0, 3000.0, 0.2, 0},
        {"H6_pulse", 5, 0.05, 0.005, 0.4, 0},
        {"indep_alt", 6, 1000.0, 0.2, 0, 0},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const case_t *c = &table[i];
        switch (c->kind) {
        case 0:
            fill_tone(pcm, N_SAMP, c->a, c->b);
            break;
        case 1:
            fill_two_tone(pcm, N_SAMP, c->a, c->b, c->c, c->d);
            break;
        case 2:
            fill_noise(pcm, N_SAMP, (unsigned)c->a, c->b);
            break;
        case 3:
            fill_am_tone(pcm, N_SAMP, 1000.0, 5.0, 0.5, 0.2, c->a);
            break;
        case 4:
            fill_chirp(pcm, N_SAMP, c->a, c->b, c->c);
            break;
        case 5:
            fill_pulse_train(pcm, N_SAMP, c->a, c->b, c->c);
            break;
        default:
            fill_alternating_tone_silence(pcm, N_SAMP, c->a, c->b);
            break;
        }

        ref_intra_t ref;
        ref_intra_from_pcm(pcm, N_SAMP, &ref);
        dbmap_l1_window_t l1;
        dbmap_l2_window_t l2;
        analyze_l2(&ws, pcm, (int64_t)(i + 1) * 1000000LL, &l1, &l2);

        if (ref.has_flux != (l2.has_spectral_flux ? 1 : 0)) {
            fprintf(stderr, "FAIL %s flux presence mismatch\n", c->name);
            free(pcm);
            return 1;
        }
        if (ref.has_env != (l2.has_envelope_cv ? 1 : 0)) {
            fprintf(stderr, "FAIL %s env presence mismatch\n", c->name);
            free(pcm);
            return 1;
        }
        note_err(
            "flux",
            l2.has_spectral_flux ? (double)l2.spectral_flux : 0.0,
            ref.has_flux ? ref.flux : 0.0,
            ref.has_flux,
            &max_abs_flux,
            &max_rel_flux,
            &nz_flux,
            &away_flux
        );
        note_err(
            "env",
            l2.has_envelope_cv ? (double)l2.envelope_cv : 0.0,
            ref.has_env ? ref.env_cv : 0.0,
            ref.has_env,
            &max_abs_env,
            &max_rel_env,
            &nz_env,
            &away_env
        );
        printf(
            "err_survey %-12s flux_abs=%.3g env_abs=%.3g "
            "(dut_flux=%g ref_flux=%g dut_env=%g ref_env=%g)\n",
            c->name,
            ref.has_flux ? fabs((double)l2.spectral_flux - ref.flux) : 0.0,
            ref.has_env ? fabs((double)l2.envelope_cv - ref.env_cv) : 0.0,
            l2.has_spectral_flux ? (double)l2.spectral_flux : -1.0,
            ref.has_flux ? ref.flux : -1.0,
            l2.has_envelope_cv ? (double)l2.envelope_cv : -1.0,
            ref.has_env ? ref.env_cv : -1.0
        );
    }
    free(pcm);

    printf(
        "err_survey SUMMARY flux: max_abs=%.6g max_rel=%.6g "
        "near_zero_n=%d away_n=%d | provisional abs=%.3g rel=%.3g\n",
        max_abs_flux,
        max_rel_flux,
        nz_flux,
        away_flux,
        EPS_FLUX_ABS,
        EPS_REL
    );
    printf(
        "err_survey SUMMARY env:  max_abs=%.6g max_rel=%.6g "
        "near_zero_n=%d away_n=%d | provisional abs=%.3g rel=%.3g\n",
        max_abs_env,
        max_rel_env,
        nz_env,
        away_env,
        EPS_ENV_ABS,
        EPS_REL
    );

    /* Observed host float↔double spread must fit provisional gates. */
    EXPECT(max_abs_flux <= EPS_FLUX_ABS);
    EXPECT(max_abs_env <= EPS_ENV_ABS);
    if (away_flux > 0) {
        EXPECT(max_rel_flux <= EPS_REL || max_abs_flux <= EPS_FLUX_ABS);
    }
    if (away_env > 0) {
        EXPECT(max_rel_env <= EPS_REL || max_abs_env <= EPS_ENV_ABS);
    }

    /* Dominant delta: on-bin 1 kHz→3 kHz is exactly 128 bins * 15.625. */
    {
        dbmap_l1_workspace_t ws2;
        dbmap_l1_workspace_init(&ws2);
        dbmap_l2_continuity_t cache;
        dbmap_l2_continuity_reset(&cache);
        int32_t *p2 = calloc(N_SAMP, sizeof(int32_t));
        EXPECT(p2 != NULL);
        fill_tone(p2, N_SAMP, 1000.0, 0.2);
        dbmap_l1_window_t l1;
        dbmap_l2_window_t l2;
        analyze_l2(&ws2, p2, 1000000, &l1, &l2);
        dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
        fill_tone(p2, N_SAMP, 3000.0, 0.2);
        analyze_l2(&ws2, p2, 2000000, &l1, &l2);
        dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
        free(p2);
        EXPECT(l2.has_dominant_delta);
        const double err = fabs((double)l2.dominant_delta_hz - 2000.0);
        printf(
            "err_survey delta: dut=%.6g expected=2000 abs_err=%.6g "
            "bin=%.6g (tol=1 bin)\n",
            (double)l2.dominant_delta_hz,
            err,
            BIN_HZ
        );
        EXPECT(err <= EPS_DELTA_HZ);
    }
    return 0;
}

static int test_l1_unchanged_with_ex(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);
    fill_tone(pcm, N_SAMP, 1000.0, 0.2);

    dbmap_l1_window_t a, b;
    dbmap_l2_intra_metrics_t intra;
    dbmap_l1_analyze_window(&ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 1000000, &a);
    dbmap_l1_analyze_window_ex(
        &ws, pcm, N_SAMP, FS_BITS, SR, WIN_MS, 1000000, &b, &intra
    );
    free(pcm);

    EXPECT(a.valid == b.valid);
    EXPECT(a.clipped == b.clipped);
    EXPECT(a.frame_count == b.frame_count);
    EXPECT(near_abs(a.centroid_hz, b.centroid_hz, 1e-4));
    EXPECT(near_abs(a.bandwidth_hz, b.bandwidth_hz, 1e-4));
    EXPECT(near_abs(a.dominant_hz, b.dominant_hz, 1e-4));
    EXPECT(near_abs(a.flatness, b.flatness, 1e-6));
    EXPECT(intra.has_spectral_flux);
    EXPECT(intra.has_envelope_cv);
    return 0;
}

/** Workspace init zeros prev_power; sizeof includes the L2 flux scratch. */
static int test_workspace_init_prev_power(void)
{
    dbmap_l1_workspace_t ws;
    memset(&ws, 0xA5, sizeof(ws));
    dbmap_l1_workspace_init(&ws);
    for (uint32_t k = 0; k < (uint32_t)DBMAP_L1_FFT_BINS; k++) {
        EXPECT(ws.prev_power[k] == 0.0f);
    }
    EXPECT(ws.tables_ready);
    EXPECT(sizeof(dbmap_l1_workspace_t) >= sizeof(float) * (size_t)DBMAP_L1_FFT_BINS);
    return 0;
}

/**
 * Independent workspaces must not share flux scratch: dirty prev_power in ws_a
 * must not change ws_b results (and vice versa after interleaved use).
 */
static int test_workspace_prev_power_independence(void)
{
    dbmap_l1_workspace_t ws_a, ws_b, ws_clean;
    dbmap_l1_workspace_init(&ws_a);
    dbmap_l1_workspace_init(&ws_b);
    dbmap_l1_workspace_init(&ws_clean);

    int32_t *pcm_tone = calloc(N_SAMP, sizeof(int32_t));
    int32_t *pcm_noise = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm_tone != NULL && pcm_noise != NULL);
    fill_tone(pcm_tone, N_SAMP, 1000.0, 0.25);
    fill_noise(pcm_noise, N_SAMP, 42u, 0.2);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t flux_tone_clean, flux_noise_clean, flux_tone_a, flux_noise_b;
    analyze_l2(&ws_clean, pcm_tone, 1000000, &l1, &flux_tone_clean);
    dbmap_l1_workspace_init(&ws_clean);
    analyze_l2(&ws_clean, pcm_noise, 1000000, &l1, &flux_noise_clean);

    /* Dirty ws_a with tone, then poison remaining prev_power cells. */
    analyze_l2(&ws_a, pcm_tone, 1000000, &l1, &flux_tone_a);
    for (uint32_t k = 0; k < (uint32_t)DBMAP_L1_FFT_BINS; k++) {
        ws_a.prev_power[k] = 1.0e9f;
    }
    analyze_l2(&ws_b, pcm_noise, 1000000, &l1, &flux_noise_b);

    EXPECT(flux_tone_a.has_spectral_flux == flux_tone_clean.has_spectral_flux);
    EXPECT(flux_noise_b.has_spectral_flux == flux_noise_clean.has_spectral_flux);
    if (flux_tone_clean.has_spectral_flux) {
        EXPECT(near_abs(
            flux_tone_a.spectral_flux, flux_tone_clean.spectral_flux, EPS_FLUX_ABS
        ));
    }
    if (flux_noise_clean.has_spectral_flux) {
        EXPECT(near_abs(
            flux_noise_b.spectral_flux, flux_noise_clean.spectral_flux, EPS_FLUX_ABS
        ));
    }

    /* After poisoning ws_a, a fresh tone analyze on ws_a must still match clean. */
    dbmap_l2_window_t flux_tone_a2;
    analyze_l2(&ws_a, pcm_tone, 2000000, &l1, &flux_tone_a2);
    EXPECT(flux_tone_a2.has_spectral_flux == flux_tone_clean.has_spectral_flux);
    if (flux_tone_clean.has_spectral_flux) {
        EXPECT(near_abs(
            flux_tone_a2.spectral_flux, flux_tone_clean.spectral_flux, EPS_FLUX_ABS
        ));
    }

    free(pcm_tone);
    free(pcm_noise);
    return 0;
}

/**
 * Repeated calls on one workspace must not treat the previous window's last
 * frame as the first frame of the next window (has_prev_power is per-call).
 */
static int test_no_stale_first_frame_across_calls(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    int32_t *pcm_a = calloc(N_SAMP, sizeof(int32_t));
    int32_t *pcm_b = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm_a != NULL && pcm_b != NULL);
    fill_tone(pcm_a, N_SAMP, 1000.0, 0.25);
    fill_tone(pcm_b, N_SAMP, 3000.0, 0.25);

    dbmap_l1_window_t l1;
    dbmap_l2_window_t solo, after_other;
    analyze_l2(&ws, pcm_b, 1000000, &l1, &solo);

    /* Contaminate workspace with a different spectrum, then re-run pcm_b. */
    analyze_l2(&ws, pcm_a, 2000000, &l1, &after_other);
    analyze_l2(&ws, pcm_b, 3000000, &l1, &after_other);

    EXPECT(solo.has_spectral_flux == after_other.has_spectral_flux);
    EXPECT(solo.has_envelope_cv == after_other.has_envelope_cv);
    if (solo.has_spectral_flux) {
        EXPECT(near_abs(solo.spectral_flux, after_other.spectral_flux, EPS_FLUX_ABS));
    }
    if (solo.has_envelope_cv) {
        EXPECT(near_abs(solo.envelope_cv, after_other.envelope_cv, EPS_ENV_ABS));
    }
    EXPECT(near_abs(solo.dominant_hz, after_other.dominant_hz, 1e-3));

    free(pcm_a);
    free(pcm_b);
    return 0;
}

static int test_population_stdev_interval(void)
{
    dbmap_l1_workspace_t ws;
    dbmap_l1_workspace_init(&ws);
    dbmap_l2_continuity_t cache;
    dbmap_l2_continuity_reset(&cache);
    int32_t *pcm = calloc(N_SAMP, sizeof(int32_t));
    EXPECT(pcm != NULL);

    dbmap_l2_interval_accum_t acc;
    dbmap_l2_interval_accum_init(&acc, SR, WIN_MS, 0, 5000000);

    const double freqs[] = {1000.0, 1000.0, 3000.0};
    for (int i = 0; i < 3; i++) {
        fill_tone(pcm, N_SAMP, freqs[i], 0.2);
        dbmap_l1_window_t l1;
        dbmap_l2_window_t l2;
        analyze_l2(&ws, pcm, (int64_t)(i + 1) * 1000000LL, &l1, &l2);
        dbmap_l2_continuity_push(&cache, WIN_MS, &l2);
        dbmap_l2_interval_accum_add(&acc, &l2);
    }
    free(pcm);

    dbmap_l2_interval_t out;
    dbmap_l2_interval_accum_finalize(&acc, &out);
    EXPECT(out.window_count == 3);
    EXPECT(out.dominant_delta_valid_count == 2); /* pairs: 0 then ~2000 */
    EXPECT(out.has_dominant_delta_mean);
    EXPECT(out.has_dominant_delta_stdev);
    /* population σ of {≈0, ≈2000}: mean≈1000, σ≈1000 */
    EXPECT(near_abs(out.dominant_delta_mean_hz, 1000.0, EPS_DELTA_HZ));
    EXPECT(near_abs(out.dominant_delta_stdev_hz, 1000.0, EPS_DELTA_HZ));
    return 0;
}

typedef int (*test_fn)(void);

static int run(const char *name, test_fn fn)
{
    const int rc = fn();
    if (rc != 0) {
        fprintf(stderr, "FAIL %s\n", name);
        g_failures++;
        return 1;
    }
    printf("ok %s\n", name);
    return 0;
}

int main(void)
{
    run("H1_stable_tone", test_h1_stable_tone);
    run("H2_multi_tone", test_h2_multi_tone);
    run("H3_broadband_noise", test_h3_noise);
    run("H4_am_phases", test_h4_am_phases);
    run("H5_chirp", test_h5_chirp);
    run("H6_pulse_train", test_h6_pulse_train);
    run("H7_near_silence", test_h7_near_silence);
    run("H8_abrupt_delta", test_h8_abrupt_delta);
    run("H9_missing_invalid_prev", test_h9_missing_and_invalid_prev);
    run("H10_timestamp_gap", test_h10_timestamp_gap);
    run("H11_episode_reset", test_h11_episode_reset);
    run("H12_interval_boundary", test_h12_interval_boundary);
    run("overflow_vs_empty", test_overflow_vs_empty);
    run("invalid_and_feature_counts", test_invalid_and_feature_specific);
    run("feature_independence_flux_absent_env",
        test_feature_independence_flux_absent_env_present);
    run("golden_float_double_error_survey", test_golden_float_double_error_survey);
    run("l1_unchanged_with_ex", test_l1_unchanged_with_ex);
    run("workspace_init_prev_power", test_workspace_init_prev_power);
    run("workspace_prev_power_independence", test_workspace_prev_power_independence);
    run("no_stale_first_frame_across_calls", test_no_stale_first_frame_across_calls);
    run("population_stdev_interval", test_population_stdev_interval);

    if (g_failures != 0) {
        fprintf(stderr, "%d test group(s) failed\n", g_failures);
        return 1;
    }
    printf("ok all L2 host tests\n");
    return 0;
}
