/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 yi-mediad contributors */
#ifndef MEDIAD_HUM_H
#define MEDIAD_HUM_H

#include <stdint.h>

/* Mains-hum rejection for the mic path: a cascade of biquad notches at the
 * fundamental and its first odd harmonics (odd only -- even harmonics are
 * absent in the measured hum), so speech outside those narrow bands is
 * untouched. Four stages is what the 50 Hz recordings need; see
 * docs/env.md (MEDIAD_HUM_HZ). */

#define MEDIAD_HUM_STAGES 4

typedef struct {
    int stages;
    double f0;
    struct {
        double b0, b1, b2, a1, a2;
        double z1, z2;
    } bq[MEDIAD_HUM_STAGES];
} mediad_hum_t;

/* f0 = 0 disables (process becomes a no-op). fs is the actual capture rate. */
void mediad_hum_init(mediad_hum_t *h, double f0, double fs);

/* In-place, int16 mono/stereo-interleaved, any length. */
void mediad_hum_process(mediad_hum_t *h, int16_t *pcm, int nsamples);

/* Goertzel amplitude of a sine at f0 (same units as the samples), used by the
 * start-up detector and by the host test. */
double mediad_hum_tone_amp(const int16_t *pcm, int nsamples, double f0, double fs);
double mediad_hum_rms(const int16_t *pcm, int nsamples);

#endif /* MEDIAD_HUM_H */
