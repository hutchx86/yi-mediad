// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* talkback.c - see talkback.h. An empty FIFO is padded with silence to keep
 * the stream fed (talkback_rx gates the amp, so it is inaudible). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <alsa/asoundlib.h>

#include "talkback.h"

#define TB_FIFO_PATH   "/tmp/audio_in_fifo"
#define TB_RATE        16000
#define TB_CHANNELS    1
#define TB_SAMPLES     1024                 /* 64 ms @ 16 kHz, as the FIFO writer uses */
#define TB_FRAME_BYTES (TB_SAMPLES * TB_CHANNELS * 2)
#define TB_PERIOD      1024                 /* play period, matches TB_SAMPLES */
#define TB_BUFFER      4096                 /* 256 ms @ 16 kHz; DTB max (~4 s) is too deep */
#define TB_CARD_CODEC  "default"
#define TB_CARD_DAUDIO "hw:1,0"

static volatile int g_run;
static int g_up;
static pthread_t g_tid;
static snd_pcm_t *g_pcm;
static const char *g_card = TB_CARD_CODEC;

/* Best-effort codec speaker mixer path; the daudio card has no such controls. */
static void tb_set_codec_controls(void)
{
    snd_mixer_t *m = NULL;
    snd_mixer_elem_t *e;

    if (snd_mixer_open(&m, 0) < 0)
        return;
    if (snd_mixer_attach(m, TB_CARD_CODEC) < 0 ||
        snd_mixer_selem_register(m, NULL, NULL) < 0 ||
        snd_mixer_load(m) < 0) {
        snd_mixer_close(m);
        return;
    }
    for (e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
        const char *n = snd_mixer_selem_get_name(e);

        if (!strcmp(n, "External Speaker") && snd_mixer_selem_has_playback_switch(e)) {
            snd_mixer_selem_set_playback_switch(e, 0, 1);
        } else if (!strcmp(n, "Speaker PA shutdown pin high level") &&
                   snd_mixer_selem_has_playback_switch(e)) {
            snd_mixer_selem_set_playback_switch(e, 0, 1);
        } else if (!strcmp(n, "LINEOUT volume") &&
                   snd_mixer_selem_has_playback_volume(e)) {
            long min = 0, max = 0;
            if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) == 0) {
                long v = min + (max - min) * 9 / 10;
                snd_mixer_selem_set_playback_volume(e, 0, v);
                snd_mixer_selem_set_playback_volume(e, 1, v);
            }
        } else if ((!strcmp(n, "Left LINEOUT Mux") || !strcmp(n, "Right LINEOUT Mux")) &&
                   snd_mixer_selem_is_enumerated(e)) {
            snd_mixer_selem_set_enum_item(e, 0, 0); /* LOMIX */
        }
    }
    snd_mixer_close(m);
}

static void *tb_thread(void *arg)
{
    int fifo = -1;
    unsigned char buf[TB_FRAME_BYTES];
    long sent = 0;
    int logged_audio = 0;

    (void)arg;
    while (g_run) {
        ssize_t n;

        if (fifo < 0) {
            fifo = open(TB_FIFO_PATH, O_RDONLY | O_NONBLOCK);
            if (fifo < 0) {
                usleep(200000);
                continue;
            }
            fprintf(stderr, "talkback: playing %s\n", TB_FIFO_PATH);
        }

        n = read(fifo, buf, TB_FRAME_BYTES);
        if (n < 0)
            n = 0;
        if (n > 0 && !logged_audio) {
            fprintf(stderr, "talkback: got PCM from FIFO (%d bytes)\n", (int)n);
            logged_audio = 1;
        }
        if ((size_t)n < TB_FRAME_BYTES)
            memset(buf + n, 0, TB_FRAME_BYTES - (size_t)n);

        {
            snd_pcm_sframes_t w = snd_pcm_writei(g_pcm, buf, TB_SAMPLES);
            if (w < 0) {
                snd_pcm_recover(g_pcm, (int)w, 1);
            } else if (++sent % 200 == 0) {
                fprintf(stderr, "talkback: %ld frames sent\n", sent);
            }
        }
    }

    if (fifo >= 0)
        close(fifo);
    return NULL;
}

int talkback_start(void)
{
    snd_pcm_hw_params_t *hw;
    unsigned int rate = TB_RATE;
    snd_pcm_uframes_t period = TB_PERIOD;
    snd_pcm_uframes_t bufsz = TB_BUFFER;
    const char *e;
    int ret;

    if (g_up)
        return 0;

    /* MEDIAD_AO_CARD: 0 = internal codec ("default", the speaker), 1 = daudio
     * ("hw:1,0", the AEC loopback). */
    e = getenv("MEDIAD_AO_CARD");
    g_card = (e && e[0] == '1') ? TB_CARD_DAUDIO : TB_CARD_CODEC;

    ret = snd_pcm_open(&g_pcm, g_card, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        fprintf(stderr, "talkback: playback open %s: %s\n", g_card, snd_strerror(ret));
        g_pcm = NULL;
        return -1;
    }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(g_pcm, hw, TB_CHANNELS);
    snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0);
    /* Bound the ring: the DTB's 65536-frame default plus the always-full FIFO
     * would add ~4 s of latency. */
    ret = snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period, NULL);
    if (ret < 0)
        fprintf(stderr, "talkback: set_period_size_near: %s\n", snd_strerror(ret));
    ret = snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &bufsz);
    if (ret < 0)
        fprintf(stderr, "talkback: set_buffer_size_near: %s\n", snd_strerror(ret));
    ret = snd_pcm_hw_params(g_pcm, hw);
    if (ret < 0) {
        fprintf(stderr, "talkback: hw_params: %s\n", snd_strerror(ret));
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    snd_pcm_prepare(g_pcm);

    if (g_card == TB_CARD_CODEC)
        tb_set_codec_controls();

    g_run = 1;
    if (pthread_create(&g_tid, NULL, tb_thread, NULL) != 0) {
        fprintf(stderr, "talkback: pthread_create failed\n");
        g_run = 0;
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    g_up = 1;
    fprintf(stderr, "talkback: ALSA playback on %s, %u Hz mono S16\n", g_card, rate);
    return 0;
}

void talkback_stop(void)
{
    if (!g_up)
        return;
    g_run = 0;
    if (g_pcm)
        snd_pcm_drop(g_pcm);
    pthread_join(g_tid, NULL);
    if (g_pcm) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
    g_up = 0;
}
