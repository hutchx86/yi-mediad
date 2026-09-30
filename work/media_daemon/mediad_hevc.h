/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 yi-mediad contributors */
#ifndef MEDIAD_HEVC_H
#define MEDIAD_HEVC_H

/* H.265/HEVC channel policy and encoder setup, kept apart from the H.264 path in
 * mediad_venc.c / main.c. Environment overrides (all optional):
 *   MEDIAD_HEVC_LEVEL    general_level_idc, default 150 (5.0)
 *   MEDIAD_HEVC_IDR      key interval in frames, default 100 (5 s at 20 fps)
 *   MEDIAD_HEVC_VUI      0 = no VUI timing info in the SPS
 *   MEDIAD_HEVC_TRACK    0/1 = closed-loop rate control on all channels
 *                        (default: high channel only)
 *   MEDIAD_HEVC_BITRATE  high-channel target bps, default 2000000, 0 = follow
 *                        the controller
 *   MEDIAD_HEVC_RC / _MINQP / _MAXQP  rate-control mode and QP window
 *                        (default CBR, 18..45; MEDIAD_MINQP/MAXQP are H.264 only) */

/* No vencoder.h here: main.c cannot include it (enum clash with the media headers). */
#include "mediad_venc.h"

/* Program a freshly created H.265 encoder from cfg. fps/bitrate/fastenc are the
 * values apply_defaults() already resolved. Must run before init (it builds the
 * SPS). */
void mediad_hevc_configure(void *enc /* fwm_venc_handle_t * */, const struct mediad_venc_cfg *cfg,
                           int fps, int bitrate, int fastenc);

/* Bitrate an H.265 channel encodes at: the high channel's HEVC target, else the
 * controller's value. Clamped to [lo, hi] when the HEVC target applies. */
unsigned int mediad_hevc_bitrate(const char *chan, unsigned int controller_bps,
                                 unsigned int lo, unsigned int hi);

/* Overwrite the rate-related fields of an already filled cfg (bitrate, rc_mode,
 * QP window) with the H.265 policy. */
void mediad_hevc_apply_channel_cfg(struct mediad_venc_cfg *cfg, const char *chan,
                                   unsigned int controller_bps,
                                   unsigned int lo, unsigned int hi);

#endif
