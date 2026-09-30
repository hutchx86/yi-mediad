/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 yi-mediad contributors */
/* mediad_hevc.c - H.265/HEVC channel policy and encoder setup (see mediad_hevc.h).
 * The H.264 path stays in mediad_venc.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vencoder.h"
#include "freecodec/venc_ext.h"

#include "mediad_hevc.h"

void mediad_hevc_configure(void *enc_handle, const struct mediad_venc_cfg *cfg,
                           int fps, int bitrate, int fastenc)
{
    fwm_venc_handle_t *enc = (fwm_venc_handle_t *)enc_handle;
    /* H.265/HEVC. The clean H.265 device pins gop_size to 20; the key interval
     * is honoured when it is a whole number of GOPs (else it uses 40). */
    fwm_venc_h265_config_t h;

    memset(&h, 0, sizeof(h));
    h.profile_level.profile = FWM_VENC_H265_PROFILE_MAIN;
    /* Declared level: 2304x1296 needs >= 5.0 (4.1 caps a picture at 2.23 MP,
     * which this exceeds; a native Super camera declares 5.0). */
    {
        const char *e = getenv("MEDIAD_HEVC_LEVEL");
        int lv = e ? atoi(e) : 0;
        h.profile_level.level = (lv >= 90 && lv <= 186) ? lv : 150;
    }
    h.frame_rate = fps;
    h.source_frame_rate = fps;
    h.bitrate = bitrate;
    /* Key interval in frames (a multiple of the 20-frame GOP; the encoder
     * falls back to 40 otherwise). Default 100 = 5 s at 20 fps, as a native
     * Super camera. MEDIAD_HEVC_IDR overrides. */
    {
        const char *e = getenv("MEDIAD_HEVC_IDR");
        int idr = e ? atoi(e) : 100;
        h.idr_period = idr;
        h.intra_period = idr;
    }
    h.gop_size = 20;
    h.qp_init = 26;
    h.rc_mode = (cfg->rc_mode == 2) ? FWM_VENC_H265_RC_ABR
              : (cfg->rc_mode == 1) ? FWM_VENC_H265_RC_VBR
                                    : FWM_VENC_H265_RC_CBR;
    h.qp_range.qp_min = cfg->min_qp > 0 ? cfg->min_qp : 10;
    h.qp_range.qp_max = cfg->max_qp > 0 ? cfg->max_qp : 40;
    h.vbr.max_bitrate = (unsigned int)bitrate;
    h.vbr.motion_threshold = 20;
    h.vbr.quality = 10;
    h.gop.gop_control_en = 1;
    h.gop.gop_mode = FWM_VENC_H265_GOP_NORMAL_P;
    h.gop.gop_size = 20;
    VideoEncSetParameter(enc, FWM_VENC_PARAM_H265_CONFIG, &h);
    {
        /* VUI timing info in the SPS so a client can derive the frame rate
         * (a native Super camera carries it). MEDIAD_HEVC_VUI=0 disables. */
        const char *e = getenv("MEDIAD_HEVC_VUI");
        if (!e || atoi(e) != 0) {
            fwm_venc_h265_timing_t t;
            memset(&t, 0, sizeof(t));
            t.num_units_in_tick = 1000u;
            t.time_scale = (unsigned int)fps * 1000u;
            t.num_ticks_poc_diff_one = 1;
            t.framerate = (unsigned int)fps;
            VideoEncSetParameter(enc, FWM_VENC_PARAM_H265_TIMING, &t);
        }
    }
    {
        /* Closed-loop rate control: the vendor-model RC feeds its own
         * output back into the budget and, on a quiet scene, pins P frames
         * at the QP ceiling (~0.1 Mbps against a 2 Mbps target). Default on
         * for the high channel; MEDIAD_HEVC_TRACK=0/1 forces all channels. */
        const char *e = getenv("MEDIAD_HEVC_TRACK");
        int track = e ? (atoi(e) != 0) : (cfg->chn == 0);
        VideoEncSetParameter(enc, FWM_VENC_PARAM_H265_RC_TRACK, &track);
    }
    VideoEncSetParameter(enc, FWM_VENC_PARAM_FRAME_RATE, &fps);
    VideoEncSetParameter(enc, FWM_VENC_PARAM_BITRATE, &bitrate);
    VideoEncSetParameter(enc, FWM_VENC_PARAM_FAST_ENCODE, &fastenc);
    {
        /* Encoder 3D filter (not the ISP's tdf); must precede init so the
         * dynamic-ME latch matches the H.264 path. */
        unsigned char nr3d = (unsigned char)(cfg->nr3d < 0 ? 0 : cfg->nr3d);
        VideoEncSetParameter(enc, FWM_VENC_PARAM_FILTER_3D, &nr3d);
    }
    /* Shown window smaller than the coded picture (SPS conformance window),
     * as on the H.264 path; before init, which builds the SPS. */
    if (cfg->out_w > 0 && cfg->out_h > 0) {
        fwm_venc_display_size_t show = { cfg->out_w, cfg->out_h };
        if (VideoEncSetParameter(enc, FWM_VENC_PARAM_DISPLAY_SIZE, &show) != 0)
            fprintf(stderr, "[venc] chn=%d display size %dx%d not supported\n",
                    cfg->chn, cfg->out_w, cfg->out_h);
        if (cfg->out_x >= 0 || cfg->out_y >= 0) {
            fwm_venc_display_offset_t off = { cfg->out_x, cfg->out_y };
            if (VideoEncSetParameter(enc, FWM_VENC_PARAM_DISPLAY_OFFSET, &off) != 0)
                fprintf(stderr, "[venc] chn=%d display offset %d,%d not supported\n",
                        cfg->chn, cfg->out_x, cfg->out_y);
        }
    }
    fprintf(stderr, "[venc] chn=%d H.265 Main fps=%d bps=%d gop=20 idr=%d level=%d qp %d..%d 3dfilter=%d\n",
            cfg->chn, fps, bitrate, h.idr_period, h.profile_level.level, h.qp_range.qp_min, h.qp_range.qp_max,
            (cfg->nr3d < 0 ? 0 : cfg->nr3d));
}

unsigned int mediad_hevc_bitrate(const char *chan, unsigned int controller_bps,
                                 unsigned int lo, unsigned int hi)
{
    /* At the H.264 QP window this encoder produced ~0.4 Mbps against the
     * controller's 2.2; the target is the HIGH channel's only. */
    const char *e = getenv("MEDIAD_HEVC_BITRATE");
    unsigned int v = e ? (unsigned int)strtoul(e, NULL, 0) : 2000000u;

    if (strcmp(chan, "high") != 0)
        return controller_bps;
    if (v == 0)
        return controller_bps;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void mediad_hevc_apply_channel_cfg(struct mediad_venc_cfg *cfg, const char *chan,
                                   unsigned int controller_bps,
                                   unsigned int lo, unsigned int hi)
{
    const char *e;

    /* H.265 has its own rate target and QP window: the H.264 window
     * (MEDIAD_MINQP/MAXQP, tuned for the H.264 encoder) pinned HEVC at ~0.4 Mbps.
     * CBR so the encoder actually spends the target. */
    cfg->bitrate = (int)mediad_hevc_bitrate(chan, controller_bps, lo, hi);
    cfg->rc_mode = 0;
    cfg->min_qp = 18;
    cfg->max_qp = 45;
    if ((e = getenv("MEDIAD_HEVC_RC"))) cfg->rc_mode = atoi(e);
    if ((e = getenv("MEDIAD_HEVC_MINQP"))) cfg->min_qp = atoi(e);
    if ((e = getenv("MEDIAD_HEVC_MAXQP"))) cfg->max_qp = atoi(e);
}
