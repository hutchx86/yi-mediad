// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* mediad_hum.c - mains-hum notches. See mediad_hum.h. */
#include "mediad_hum.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Q per stage: broad on the fundamental (the line drifts with the mains), then
 * narrower up the harmonics, which are much quieter. */
static const double kQ[MEDIAD_HUM_STAGES] = { 4.0, 8.0, 10.0, 10.0 };

void mediad_hum_init(mediad_hum_t *h, double f0, double fs)
{
    int i;

    memset(h, 0, sizeof(*h));
    h->f0 = f0;
    if (f0 <= 0.0 || fs <= 0.0)
        return;
    /* Highest notch is the 7th harmonic; keep it under Nyquist. */
    if (f0 * (2.0 * MEDIAD_HUM_STAGES - 1.0) >= fs / 2.0)
        return;

    for (i = 0; i < MEDIAD_HUM_STAGES; i++) {
        double w0 = 2.0 * M_PI * f0 * (2.0 * i + 1.0) / fs;
        double c = cos(w0);
        double alpha = sin(w0) / (2.0 * kQ[i]);
        double a0 = 1.0 + alpha;

        h->bq[i].b0 = 1.0 / a0;
        h->bq[i].b1 = -2.0 * c / a0;
        h->bq[i].b2 = 1.0 / a0;
        h->bq[i].a1 = -2.0 * c / a0;
        h->bq[i].a2 = (1.0 - alpha) / a0;
        h->bq[i].z1 = h->bq[i].z2 = 0.0;
    }
    h->stages = MEDIAD_HUM_STAGES;
}

void mediad_hum_process(mediad_hum_t *h, int16_t *pcm, int nsamples)
{
    int i, s;

    if (h->stages <= 0)
        return;

    for (s = 0; s < nsamples; s++) {
        double x = (double)pcm[s];

        for (i = 0; i < h->stages; i++) {
            double y = h->bq[i].b0 * x + h->bq[i].z1;

            h->bq[i].z1 = h->bq[i].b1 * x - h->bq[i].a1 * y + h->bq[i].z2;
            h->bq[i].z2 = h->bq[i].b2 * x - h->bq[i].a2 * y;
            x = y;
        }
        if (x > 32767.0)
            x = 32767.0;
        else if (x < -32768.0)
            x = -32768.0;
        pcm[s] = (int16_t)(x >= 0.0 ? x + 0.5 : x - 0.5);
    }
}

double mediad_hum_tone_amp(const int16_t *pcm, int nsamples, double f0, double fs)
{
    double w, c, s0 = 0.0, s1 = 0.0, s2 = 0.0, re, im;
    int i;

    if (nsamples <= 0 || fs <= 0.0 || f0 <= 0.0)
        return 0.0;
    w = 2.0 * M_PI * f0 / fs;
    c = 2.0 * cos(w);
    for (i = 0; i < nsamples; i++) {
        s0 = c * s1 - s2 + (double)pcm[i];
        s2 = s1;
        s1 = s0;
    }
    re = s1 - s2 * cos(w);
    im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)nsamples;
}

double mediad_hum_rms(const int16_t *pcm, int nsamples)
{
    double acc = 0.0;
    int i;

    if (nsamples <= 0)
        return 0.0;
    for (i = 0; i < nsamples; i++)
        acc += (double)pcm[i] * (double)pcm[i];
    return sqrt(acc / (double)nsamples);
}
