/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 yi-mediad contributors */
#ifndef MEDIAD_VENC_H
#define MEDIAD_VENC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct mediad_venc;

struct mediad_venc_cfg {
    int src_w, src_h;         /* capture (VI) size */
    int pic_w, pic_h;         /* encoded size */
    int out_w, out_h;         /* displayed window (SPS crop), 0 = pic size */
    int out_x, out_y;         /* window offset in the picture, -1 = centred */
    int crop_x, crop_y;       /* read a pic_w x pic_h window at this offset of
                               * the capture (no scaling); -1 = off */
    int fps;
    int bitrate;              /* bps */
    int gop;                  /* max key interval, frames */
    int profile;              /* 0 baseline / 1 main / 2 high */
    int max_qp;               /* RC ceiling */
    int min_qp;               /* RC floor */
    int rc_mode;              /* 0 CBR / 1 VBR / 2 AVBR (rmm: VBR) */
    int nr3d;                 /* encoder 3D-filter level 0-3 (rmm: 3) */
    int fastenc;
    int chn;                  /* encoder channel index (FWM_VENC_PARAM_CHANNEL) */
};

/* One encoded frame. FreeOneBitStreamFrame needs the encoder's fwm_venc_output_frame_t
 * back verbatim (id/flags identify the bitstream slot), so carry every field
 * we might have to hand back - dropping them leaks bitstream slots until the
 * encoder's PutBits path fails (seen on r35gb 2026-09-19, ~21 min in). */
struct mediad_venc_frame {
    const unsigned char *addr0;
    size_t len0;
    const unsigned char *addr1;
    size_t len1;
    const unsigned char *addr2;   /* fwm_venc_output_frame_t.data2 */
    size_t len2;
    unsigned int flag;            /* fwm_venc_output_frame_t.flags */
    int id;                       /* fwm_venc_output_frame_t.id */
    uint64_t pts;                 /* microseconds */
    /* fwm_venc_frame_stats_t passthrough */
    int curr_qp, av_qp, gop_index, frame_index, total_index;
};

struct mediad_venc *mediad_venc_open(const struct mediad_venc_cfg *cfg);
void mediad_venc_close(struct mediad_venc *v);

/* Cache SPS/PPS ("00 00 00 01" prefixed) for re-emission; returns the byte
 * length copied into out (0 if unavailable). */
int mediad_venc_spspps(struct mediad_venc *v, unsigned char *out, size_t out_cap);

/* One captured luma buffer (VIRTUAL address + stride) -> one encoded frame.
 * Fills the fwm_venc_input_picture_t from cov[] and runs the encoder clock. */
struct cov1 {
    void *virY;
    void *virC;
    void *phyY;               /* captured frame physical addresses (zero-copy) */
    void *phyC;
    int stride;
};
int mediad_venc_encode(struct mediad_venc *v, const struct cov1 *cov,
                       struct mediad_venc_frame *out);

/* The encoder can queue more than one bitstream unit per input; drain the rest
 * with these so no slot is left occupied. mediad_venc_ready() > 0 means a call
 * to mediad_venc_next() will produce a frame. */
int mediad_venc_ready(struct mediad_venc *v);
int mediad_venc_next(struct mediad_venc *v, struct mediad_venc_frame *out);

/* Release the buffer handed back by mediad_venc_encode/mediad_venc_next. */
void mediad_venc_release(struct mediad_venc *v, const struct mediad_venc_frame *f);

void mediad_venc_request_idr(struct mediad_venc *v);

/* Recovery after a persistent AddInputBuffer/encode error: reset the frame and
 * bitstream managers, then force an IDR. Returns 0 on success. */
int mediad_venc_reset(struct mediad_venc *v);

/* Runtime bitrate change (Protect ChangeVideoSettings); returns 0 on success. */
int mediad_venc_set_bitrate(struct mediad_venc *v, int bps);

/* Runtime encoder 3D-filter level, 0 off .. 3 (rmm: 3); returns 0 on success. */
int mediad_venc_set_filter3d(struct mediad_venc *v, int level);

/* One burned-in OSD block for the encoder's overlay engine. Position is in
 * 16x16 macroblock units relative to the encoded frame; bits points at an
 * ARGB1555 bitmap (bit15 = per-pixel alpha, 1 = opaque) whose dimensions are
 * 16-aligned so [start..end] covers exactly bits' w*h pixels. The encoder
 * copies the bitmap during the call, so the caller owns/reuses the buffer. */
struct mediad_venc_ovl_blk {
    unsigned short start_mb_x, start_mb_y, end_mb_x, end_mb_y;
    const void *bits;
    unsigned int bytes;
    unsigned char unit_w_minus1, unit_h_minus1; /* luma-invert unit, MB - 1 */
    unsigned char hw_invert;   /* 1: LUMA_REVERSE block (hardware re-decides per frame) */
};

/* Push the whole overlay set to the encoder; n == 0 clears it. Maps to
 * libcedarc VideoEncSetParameter(FWM_VENC_PARAM_OVERLAY) - the same call the
 * vendor middleware makes (VideoEnc_Component.c:3197). Returns 0 on success. */
int mediad_venc_set_overlay(struct mediad_venc *v,
                            const struct mediad_venc_ovl_blk *blks, int n);


/* MEDIAD_NV21=1 selects uncompressed NV21 capture/encoder input instead of the
 * default LBC 2.5X (stock rmm's format). Read once. */
static inline int mediad_capture_nv21(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MEDIAD_NV21");
        v = (e != NULL && e[0] == '1') ? 1 : 0;
    }
    return v;
}

#endif /* MEDIAD_VENC_H */
