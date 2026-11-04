// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* Host test for the hum notches: synthetic 50/60 Hz hum + odd harmonics, and
 * a speech-band check that the notches do not eat content outside their bands. */
#include "../mediad_hum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 16000
#define N  (FS * 2)
#define CHK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); \
} while (0)

static double db(double before, double after)
{
    if (after < 1e-9)
        return 120.0;
    return 20.0 * log10(before / after);
}

/* hum fundamental + 3rd + 5th harmonic + broadband noise (rms 100). */
static void make(double *s, double hum_hz, unsigned seed)
{
    int i;

    srand(seed);
    for (i = 0; i < N; i++) {
        double t = (double)i / FS;
        double v = 800.0 * sin(2.0 * M_PI * hum_hz * t)
                 + 200.0 * sin(2.0 * M_PI * hum_hz * 3.0 * t)
                 + 100.0 * sin(2.0 * M_PI * hum_hz * 5.0 * t)
                 + 173.2 * ((double)rand() / RAND_MAX - 0.5);
        s[i] = v;
    }
}

static void to_pcm(int16_t *p, const double *s)
{
    int i;

    for (i = 0; i < N; i++) {
        double v = s[i];
        p[i] = (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
    }
}

int main(void)
{
    static double s[N];
    static int16_t pcm[N], ref[N];
    mediad_hum_t h;
    int fails = 0;
    double a50_b, a150_b, a250_b, a50_a, a150_a, a250_a;

    /* 50 Hz site (the reported case). */
    make(s, 50.0, 1);
    to_pcm(pcm, s);
    a50_b = mediad_hum_tone_amp(pcm, N, 50.0, FS);
    a150_b = mediad_hum_tone_amp(pcm, N, 150.0, FS);
    a250_b = mediad_hum_tone_amp(pcm, N, 250.0, FS);
    mediad_hum_init(&h, 50.0, FS);
    mediad_hum_process(&h, pcm, N);
    a50_a = mediad_hum_tone_amp(pcm, N, 50.0, FS);
    a150_a = mediad_hum_tone_amp(pcm, N, 150.0, FS);
    a250_a = mediad_hum_tone_amp(pcm, N, 250.0, FS);
    printf("50 Hz site: 50Hz %.1f -> %.1f (%.1f dB), 150Hz %.1f -> %.1f (%.1f dB), "
           "250Hz %.1f -> %.1f (%.1f dB)\n",
           a50_b, a50_a, db(a50_b, a50_a), a150_b, a150_a, db(a150_b, a150_a),
           a250_b, a250_a, db(a250_b, a250_a));
    CHK(db(a50_b, a50_a) > 25.0, "50 Hz attenuated > 25 dB");
    CHK(db(a150_b, a150_a) > 15.0, "150 Hz attenuated > 15 dB");
    CHK(db(a250_b, a250_a) > 15.0, "250 Hz attenuated > 15 dB");

    /* Speech-band content outside the notches must survive. */
    {
        double keep[] = { 400.0, 600.0, 1000.0, 2000.0 };
        unsigned k;

        for (k = 0; k < sizeof(keep) / sizeof(keep[0]); k++) {
            int i;
            double b, a;
            char msg[96];

            for (i = 0; i < N; i++) {
                double t = (double)i / FS;
                s[i] = 500.0 * sin(2.0 * M_PI * keep[k] * t);
            }
            to_pcm(pcm, s);
            b = mediad_hum_tone_amp(pcm, N, keep[k], FS);
            mediad_hum_init(&h, 50.0, FS);
            mediad_hum_process(&h, pcm, N);
            a = mediad_hum_tone_amp(pcm, N, keep[k], FS);
            snprintf(msg, sizeof(msg), "%.0f Hz kept (%.2f dB)", keep[k], db(b, a));
            CHK(db(b, a) < 1.0, msg);
        }
    }

    /* 60 Hz site. */
    make(s, 60.0, 2);
    to_pcm(pcm, s);
    {
        double b = mediad_hum_tone_amp(pcm, N, 60.0, FS);
        double a;

        mediad_hum_init(&h, 60.0, FS);
        mediad_hum_process(&h, pcm, N);
        a = mediad_hum_tone_amp(pcm, N, 60.0, FS);
        printf("60 Hz site: 60Hz %.1f -> %.1f (%.1f dB)\n", b, a, db(b, a));
        CHK(db(b, a) > 25.0, "60 Hz attenuated > 25 dB");
    }

    /* Disabled = bit-exact passthrough. */
    make(s, 50.0, 3);
    to_pcm(pcm, s);
    memcpy(ref, pcm, sizeof(ref));
    mediad_hum_init(&h, 0.0, FS);
    mediad_hum_process(&h, pcm, N);
    CHK(memcmp(ref, pcm, sizeof(ref)) == 0, "f0=0 is a passthrough");

    /* 48 kHz capture (not used today, but the API must not misbehave). */
    mediad_hum_init(&h, 50.0, 48000.0);
    CHK(h.stages == MEDIAD_HUM_STAGES, "48 kHz init has all stages");

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("all hum checks passed\n");
    return 0;
}
