/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 yi-mediad contributors */
/* Host test for mediad_hevc.c: a stub encoder records what mediad_hevc_configure()
 * programs, and the bitrate/QP policy is checked against its documented defaults
 * and environment overrides. No hardware, no vendor code. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vencoder.h"
#include "freecodec/venc_ext.h"
#include "mediad_hevc.h"

static fwm_venc_h265_config_t g_cfg;
static fwm_venc_h265_timing_t g_timing;
static int g_have_cfg, g_have_timing, g_track = -1, g_fps, g_bitrate, g_fast;

int VideoEncSetParameter(fwm_venc_handle_t *e, fwm_venc_param_e idx, void *p)
{
    (void)e;
    switch ((int)idx) {
    case FWM_VENC_PARAM_H265_CONFIG: g_cfg = *(fwm_venc_h265_config_t *)p; g_have_cfg = 1; break;
    case FWM_VENC_PARAM_H265_TIMING: g_timing = *(fwm_venc_h265_timing_t *)p; g_have_timing = 1; break;
    case FWM_VENC_PARAM_H265_RC_TRACK: g_track = *(int *)p; break;
    case FWM_VENC_PARAM_FRAME_RATE: g_fps = *(int *)p; break;
    case FWM_VENC_PARAM_BITRATE: g_bitrate = *(int *)p; break;
    case FWM_VENC_PARAM_FAST_ENCODE: g_fast = *(int *)p; break;
    default: break;
    }
    return 0;
}

static void reset(void)
{
    g_have_cfg = g_have_timing = 0; g_track = -1; g_fps = g_bitrate = g_fast = 0;
    memset(&g_cfg, 0, sizeof(g_cfg)); memset(&g_timing, 0, sizeof(g_timing));
    unsetenv("MEDIAD_HEVC_LEVEL"); unsetenv("MEDIAD_HEVC_IDR"); unsetenv("MEDIAD_HEVC_VUI");
    unsetenv("MEDIAD_HEVC_TRACK"); unsetenv("MEDIAD_HEVC_BITRATE"); unsetenv("MEDIAD_HEVC_RC");
    unsetenv("MEDIAD_HEVC_MINQP"); unsetenv("MEDIAD_HEVC_MAXQP");
}

static struct mediad_venc_cfg mkcfg(int chn)
{
    struct mediad_venc_cfg c;
    memset(&c, 0, sizeof(c));
    c.chn = chn; c.fps = 20; c.bitrate = 2200000; c.min_qp = 40; c.max_qp = 45; c.rc_mode = 1;
    return c;
}

int main(void)
{
    struct mediad_venc_cfg c;

    /* Defaults: level 5.0, 5 s key interval, VUI 1000/20000, tracking on for high. */
    reset(); c = mkcfg(0);
    mediad_hevc_configure(NULL, &c, 20, 2000000, 0);
    assert(g_have_cfg && g_cfg.profile_level.level == 150);
    assert(g_cfg.idr_period == 100 && g_cfg.intra_period == 100 && g_cfg.gop_size == 20);
    assert(g_have_timing && g_timing.num_units_in_tick == 1000 && g_timing.time_scale == 20000);
    assert(g_track == 1 && g_fps == 20 && g_bitrate == 2000000);

    /* Low channel: no tracking by default. */
    reset(); c = mkcfg(1);
    mediad_hevc_configure(NULL, &c, 20, 700000, 0);
    assert(g_track == 0 && g_bitrate == 700000);

    /* Overrides: level, IDR, VUI off, tracking forced on for all channels. */
    reset(); c = mkcfg(1);
    setenv("MEDIAD_HEVC_LEVEL", "123", 1); setenv("MEDIAD_HEVC_IDR", "40", 1);
    setenv("MEDIAD_HEVC_VUI", "0", 1); setenv("MEDIAD_HEVC_TRACK", "1", 1);
    mediad_hevc_configure(NULL, &c, 20, 700000, 0);
    assert(g_cfg.profile_level.level == 123 && g_cfg.idr_period == 40);
    assert(!g_have_timing && g_track == 1);
    reset(); c = mkcfg(0); setenv("MEDIAD_HEVC_LEVEL", "5", 1);          /* out of range -> 150 */
    mediad_hevc_configure(NULL, &c, 20, 2000000, 0);
    assert(g_cfg.profile_level.level == 150);

    /* Bitrate policy: high 2 Mbps (clamped), low follows the controller, 0 = follow. */
    reset();
    assert(mediad_hevc_bitrate("high", 2200000, 48000, 6000000) == 2000000);
    assert(mediad_hevc_bitrate("low", 700000, 48000, 6000000) == 700000);
    setenv("MEDIAD_HEVC_BITRATE", "0", 1);
    assert(mediad_hevc_bitrate("high", 2200000, 48000, 6000000) == 2200000);
    setenv("MEDIAD_HEVC_BITRATE", "99999999", 1);
    assert(mediad_hevc_bitrate("high", 2200000, 48000, 6000000) == 6000000);
    setenv("MEDIAD_HEVC_BITRATE", "1000", 1);
    assert(mediad_hevc_bitrate("high", 2200000, 48000, 6000000) == 48000);

    /* Channel cfg: H.265 window replaces the H.264 QP env values, CBR. */
    reset(); c = mkcfg(0);
    mediad_hevc_apply_channel_cfg(&c, "high", 2200000, 48000, 6000000);
    assert(c.bitrate == 2000000 && c.rc_mode == 0 && c.min_qp == 18 && c.max_qp == 45);
    setenv("MEDIAD_HEVC_RC", "1", 1); setenv("MEDIAD_HEVC_MINQP", "22", 1); setenv("MEDIAD_HEVC_MAXQP", "40", 1);
    mediad_hevc_apply_channel_cfg(&c, "low", 700000, 48000, 6000000);
    assert(c.bitrate == 700000 && c.rc_mode == 1 && c.min_qp == 22 && c.max_qp == 40);

    printf("test_hevc: PASS\n");
    return 0;
}
