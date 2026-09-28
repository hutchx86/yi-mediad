// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* mediad_audio.c - mic capture (ALSA, 16 kHz mono S16_LE) -> AAC-LC (freecodec)
 * -> fshare ring as type 0x0100 ADTS frames, as stock rmm publishes them. Also
 * supplies the encoder's GetPcmDataSize()/ReadPcmDataForEnc() PCM pull. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <alsa/asoundlib.h>

#include "fshare.h"
#include "mediad_audio.h"
#include "freecodec/aac_iface.h"

#define AUD_RATE       16000
#define AUD_CHANNELS   1
#define AUD_BYTES_SMP  2
#define AUD_FRAME_SMP  1024
#define AUD_FRAME_BYTES (AUD_FRAME_SMP * AUD_CHANNELS * AUD_BYTES_SMP)
#define AUD_RING_BYTES  (AUD_FRAME_BYTES * 32)
#define AUD_OUT_CAP     8192            /* >= freecodec FC_OUT_CAP (4096) */

static volatile int g_run;
static int g_up;
static pthread_t g_tid;
static snd_pcm_t *g_pcm;
static struct __AudioENC_AC320 *g_enc;
static __audio_enc_inf_t g_inf;
static __com_internal_prameter_t g_com;
static __pcm_buf_manager_t g_ring;
static unsigned char g_ring_buf[AUD_RING_BYTES];
static unsigned char g_read_buf[AUD_FRAME_BYTES];
static unsigned char g_out[AUD_OUT_CAP];

/* Software mic gain (Q12): the codec's MIC1 gain tops out near 0 dB.
 * MEDIAD_MIC_GAIN_DB overrides the default (0 disables). */
#define AUD_GAIN_DB_DEFAULT 12.0
static int32_t g_gain_q12 = 4096;

static void apply_gain(unsigned char *buf, int nsamp)
{
    int16_t *p = (int16_t *)buf;

    if (g_gain_q12 == 4096)
        return;
    for (int i = 0; i < nsamp; i++) {
        int32_t v = ((int32_t)p[i] * g_gain_q12) >> 12;
        p[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static uint32_t aud_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* --- PCM ring: single producer (capture) / single consumer (EncFrame) --- */

static void ring_push(const unsigned char *src, int n)
{
    int woff = (int)(g_ring.pBufWritPtr - g_ring.pBufStart);
    int first = AUD_RING_BYTES - woff;
    if (first > n)
        first = n;
    memcpy(g_ring.pBufStart + woff, src, (size_t)first);
    if (n > first)
        memcpy(g_ring.pBufStart, src + first, (size_t)(n - first));
    g_ring.pBufWritPtr = g_ring.pBufStart + (woff + n) % AUD_RING_BYTES;
    g_ring.uDataLen += n;
    g_ring.uFreeBufSize -= n;
}

int GetPcmDataSize(__pcm_buf_manager_t *m)
{
    return m ? m->uDataLen : 0;
}

int ReadPcmDataForEnc(void *dst, int len, __pcm_buf_manager_t *m)
{
    int roff, n, first;

    if (m == NULL || dst == NULL || len <= 0)
        return 0;
    n = len < m->uDataLen ? len : m->uDataLen;
    if (n <= 0)
        return 0;
    roff = (int)(m->pBufReadPtr - m->pBufStart);
    first = AUD_RING_BYTES - roff;
    if (first > n)
        first = n;
    memcpy(dst, m->pBufStart + roff, (size_t)first);
    if (n > first)
        memcpy((unsigned char *)dst + first, m->pBufStart, (size_t)(n - first));
    m->pBufReadPtr = m->pBufStart + (roff + n) % AUD_RING_BYTES;
    m->uDataLen -= n;
    m->uFreeBufSize += n;
    return n;
}

/* --- capture + encode --- */

static void *audio_thread(void *arg)
{
    uint16_t seq = 0;
    long count = 0, fails = 0;

    (void)arg;
    while (g_run) {
        snd_pcm_sframes_t got = snd_pcm_readi(g_pcm, g_read_buf, AUD_FRAME_SMP);

        if (got < 0) {
            if (++fails % 10 == 1)
                fprintf(stderr, "[audio] ALSA read: %s\n", snd_strerror((int)got));
            snd_pcm_recover(g_pcm, (int)got, 1);
            continue;
        }
        fails = 0;
        if (got > 0) {
            apply_gain(g_read_buf, (int)got * AUD_CHANNELS);
            ring_push(g_read_buf, (int)got * AUD_CHANNELS * AUD_BYTES_SMP);
        }

        for (;;) {
            int outlen = 0;
            int r = g_enc->EncFrame(g_enc, (char *)g_out, &outlen);
            if (r == ERR_AUDIO_ENC_NONE && outlen > 0) {
                (void)fshare_publish(g_out, (size_t)outlen, FSHARE_TYPE_AAC,
                                     aud_now_ms(), seq++, NULL, 0);
                count++;
            } else {
                break;
            }
        }
    }
    fprintf(stderr, "[audio] stopping after %ld frames\n", count);
    return NULL;
}

int mediad_audio_start(void)
{
    snd_pcm_hw_params_t *hw;
    unsigned int rate = AUD_RATE;
    snd_pcm_uframes_t period = AUD_FRAME_SMP, bufsz = AUD_FRAME_SMP * 8;
    const char *gain_env = getenv("MEDIAD_MIC_GAIN_DB");
    double gain_db = gain_env ? atof(gain_env) : AUD_GAIN_DB_DEFAULT;
    int ret;

    if (gain_db < 0.0)
        gain_db = 0.0;
    if (gain_db > 30.0)
        gain_db = 30.0;
    g_gain_q12 = (int32_t)(4096.0 * pow(10.0, gain_db / 20.0) + 0.5);

    if (g_up)
        return 0;

    /* Hub first: with it disabled every snd_pcm_readi() returns EIO. */
    {
        extern int audio_codec_hub_enable(void);
        if (audio_codec_hub_enable() < 0)
            fprintf(stderr, "[audio] codec hub enable failed; capture may EIO\n");
    }

    ret = snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        fprintf(stderr, "[audio] capture open failed: %s\n", snd_strerror(ret));
        g_pcm = NULL;
        return -1;
    }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(g_pcm, hw, AUD_CHANNELS);
    snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0);
    /* One AAC frame per period: the driver default (8192 frames, 512 ms)
     * published audio in 8-frame bursts that starved the live player. */
    snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period, 0);
    snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &bufsz);
    ret = snd_pcm_hw_params(g_pcm, hw);
    if (ret < 0) {
        fprintf(stderr, "[audio] capture hw_params failed: %s\n", snd_strerror(ret));
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    snd_pcm_prepare(g_pcm);

    memset(&g_ring, 0, sizeof(g_ring));
    g_ring.pBufStart = g_ring_buf;
    g_ring.pBufReadPtr = g_ring_buf;
    g_ring.pBufWritPtr = g_ring_buf;
    g_ring.uBufTotalLen = AUD_RING_BYTES;
    g_ring.uFreeBufSize = AUD_RING_BYTES;

    memset(&g_inf, 0, sizeof(g_inf));
    g_inf.InSamplerate = AUD_RATE;
    g_inf.InChan = AUD_CHANNELS;
    g_inf.bitrate = 0;               /* backend default */
    g_inf.SamplerBits = 16;
    g_inf.OutSamplerate = AUD_RATE;
    g_inf.OutChan = AUD_CHANNELS;
    g_inf.frame_style = 0;           /* 0 = ADTS header per frame */
    memset(&g_com, 0, sizeof(g_com));

    g_enc = AudioAACENCEncInit();
    if (g_enc == NULL) {
        fprintf(stderr, "[audio] freecodec AAC init failed\n");
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    g_enc->pPcmBufManager = &g_ring;
    g_enc->AudioBsEncInf = &g_inf;
    g_enc->EncoderCom = &g_com;
    if (g_enc->EncInit(g_enc) != ERR_AUDIO_ENC_NONE) {
        fprintf(stderr, "[audio] AAC EncInit failed\n");
        AudioAACENCEncExit(g_enc);
        g_enc = NULL;
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }

    g_run = 1;
    if (pthread_create(&g_tid, NULL, audio_thread, NULL) != 0) {
        fprintf(stderr, "[audio] thread create failed\n");
        g_run = 0;
        g_enc->EncExit(g_enc);
        AudioAACENCEncExit(g_enc);
        g_enc = NULL;
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    g_up = 1;
    printf("[audio] ALSA capture (%s) -> freecodec AAC-LC ADTS, %u Hz mono, period %lu buffer %lu, gain %+.1f dB\n",
           snd_pcm_name(g_pcm), rate, (unsigned long)period, (unsigned long)bufsz, gain_db);
    return 0;
}

void mediad_audio_stop(void)
{
    if (!g_up)
        return;
    g_run = 0;
    if (g_pcm)
        snd_pcm_drop(g_pcm);         /* unblock snd_pcm_readi */
    pthread_join(g_tid, NULL);
    if (g_enc) {
        g_enc->EncExit(g_enc);
        AudioAACENCEncExit(g_enc);
        g_enc = NULL;
    }
    if (g_pcm) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
    g_up = 0;
}
