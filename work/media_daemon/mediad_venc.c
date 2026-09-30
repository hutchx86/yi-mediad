// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* mediad_venc.c - our H.264 encoder channel (spec/mediad_venc.md) over the
 * libcedarc vencoder API. Capture frames are fed zero-copy by physical address. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vencoder.h"
#include "freecodec/venc_ext.h"

#include "mediad_venc.h"

struct mediad_venc {
    fwm_venc_handle_t *enc;
    int src_w, src_h, pic_w, pic_h;
    int stride;
    int chn;
    int crop_x, crop_y;       /* input window offset, -1 = off */
    int codec;                /* 0 = H.264, 1 = H.265/HEVC */
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* cfg->profile is 0 baseline / 1 main / 2 high; an already-valid profile_idc
 * (66/77/100) is passed through. */
static fwm_venc_h264_profile_e map_profile(int p)
{
    switch (p) {
    case 0: return FWM_VENC_H264_PROFILE_BASELINE;
    case 1: return FWM_VENC_H264_PROFILE_MAIN;
    case 2: return FWM_VENC_H264_PROFILE_HIGH;
    default: return (fwm_venc_h264_profile_e)p;
    }
}

/* Program the configuration from the spec's table. */
static void apply_defaults(struct mediad_venc *v, const struct mediad_venc_cfg *cfg)
{
    fwm_venc_h264_config_t h264;
    fwm_venc_bitrate_range_t range;
    int fps = cfg->fps > 0 ? cfg->fps : 20;
    int bitrate = cfg->bitrate;
    int ifilter = 0;
    int fastenc = cfg->fastenc ? 1 : 0;

    if (v->codec == 1) {
        /* H.265/HEVC. The clean H.265 device forces idr/intra 40 and gop 20
         * regardless; the rest maps from the same config. */
        fwm_venc_h265_config_t h;

        memset(&h, 0, sizeof(h));
        h.profile_level.profile = FWM_VENC_H265_PROFILE_MAIN;
        h.profile_level.level = 123;                 /* level 4.1 */
        h.frame_rate = fps;
        h.source_frame_rate = fps;
        h.bitrate = bitrate;
        h.idr_period = 40;
        h.intra_period = 40;
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
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_H265_CONFIG, &h);
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FRAME_RATE, &fps);
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_BITRATE, &bitrate);
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FAST_ENCODE, &fastenc);
        {
            /* Encoder 3D filter (not the ISP's tdf); must precede init so the
             * dynamic-ME latch matches the H.264 path. */
            unsigned char nr3d = (unsigned char)(cfg->nr3d < 0 ? 0 : cfg->nr3d);
            VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FILTER_3D, &nr3d);
        }
        /* Shown window smaller than the coded picture (SPS conformance window),
         * as on the H.264 path; before init, which builds the SPS. */
        if (cfg->out_w > 0 && cfg->out_h > 0) {
            fwm_venc_display_size_t show = { cfg->out_w, cfg->out_h };
            if (VideoEncSetParameter(v->enc, FWM_VENC_PARAM_DISPLAY_SIZE, &show) != 0)
                fprintf(stderr, "[venc] chn=%d display size %dx%d not supported\n",
                        cfg->chn, cfg->out_w, cfg->out_h);
            if (cfg->out_x >= 0 || cfg->out_y >= 0) {
                fwm_venc_display_offset_t off = { cfg->out_x, cfg->out_y };
                if (VideoEncSetParameter(v->enc, FWM_VENC_PARAM_DISPLAY_OFFSET, &off) != 0)
                    fprintf(stderr, "[venc] chn=%d display offset %d,%d not supported\n",
                            cfg->chn, cfg->out_x, cfg->out_y);
            }
        }
        fprintf(stderr, "[venc] chn=%d H.265 Main fps=%d bps=%d gop=20 qp %d..%d 3dfilter=%d\n",
                cfg->chn, fps, bitrate, h.qp_range.qp_min, h.qp_range.qp_max,
                (cfg->nr3d < 0 ? 0 : cfg->nr3d));
        return;
    }

    memset(&h264, 0, sizeof(h264));
    h264.coding_mode = FWM_VENC_CODING_FRAME;
    h264.profile_level.profile = map_profile(cfg->profile);
    h264.profile_level.level = FWM_VENC_H264_LEVEL32;  /* stock rmm: level 32 */
    /* CABAC (~10-15% smaller than CAVLC); Baseline cannot carry it. */
    h264.cabac_en =
        (h264.profile_level.profile == FWM_VENC_H264_PROFILE_BASELINE) ? 0 : 1;
    h264.frame_rate = fps;
    h264.source_frame_rate = fps;
    h264.bitrate = bitrate;
    /* Defaults are stock rmm's encoder settings: High, level 32, IDR period 40,
     * init QP 37, QP 10..40, VBR (moving threshold 20, quality 10). */
    h264.key_interval_max = cfg->gop > 0 ? cfg->gop : 40;
    h264.qp_range.qp_min = cfg->min_qp > 0 ? cfg->min_qp : 10;
    h264.qp_range.qp_max = cfg->max_qp > 0 ? cfg->max_qp : 40;
    switch (cfg->rc_mode) {
    case 0:  h264.rate_control.mode = FWM_VENC_RC_CBR; break;
    case 2:  h264.rate_control.mode = FWM_VENC_RC_AVBR; break;
    default: h264.rate_control.mode = FWM_VENC_RC_VBR; break;
    }
    h264.rate_control.vbr.max_bitrate = (unsigned int)bitrate;
    h264.rate_control.vbr.motion_threshold = 20;
    h264.rate_control.vbr.quality = 10;
    h264.gop.gop_control_en = 1;
    h264.gop.gop_mode = FWM_VENC_GOP_NORMAL_P;

    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_H264_CONFIG, &h264);
    /* Show a smaller window than we encode (e.g. r35gb: encode the native
     * 1936x1096, show 1920x1080), via SPS cropping, before init builds it. */
    if (cfg->out_w > 0 && cfg->out_h > 0) {
        fwm_venc_display_size_t show = { cfg->out_w, cfg->out_h };
        if (VideoEncSetParameter(v->enc, FWM_VENC_PARAM_DISPLAY_SIZE, &show) != 0)
            fprintf(stderr, "[venc] chn=%d display size %dx%d not supported\n",
                    cfg->chn, cfg->out_w, cfg->out_h);
        /* Only when asked: without MEDIAD_OUT_X/Y the call is never made. */
        if (cfg->out_x >= 0 || cfg->out_y >= 0) {
            fwm_venc_display_offset_t off = { cfg->out_x, cfg->out_y };
            if (VideoEncSetParameter(v->enc, FWM_VENC_PARAM_DISPLAY_OFFSET, &off) != 0)
                fprintf(stderr, "[venc] chn=%d display offset %d,%d not supported\n",
                        cfg->chn, cfg->out_x, cfg->out_y);
        }
    }
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FRAME_RATE, &fps);
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_BITRATE, &bitrate);
    range.bitrate_min = bitrate;
    range.bitrate_max = bitrate;
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_BITRATE_RANGE, &range);
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_I_FILTER, &ifilter);
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FAST_ENCODE, &fastenc);
    {
        /* Encoder 3D filter (not the ISP's tdf). Off by default: stock's level 3
         * smears motion. */
        unsigned char nr3d = (unsigned char)(cfg->nr3d < 0 ? 0 : cfg->nr3d);
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FILTER_3D, &nr3d);
    }
}

/* Pre-init VBV sizing (without it a second channel's init can wedge): a 2 s
 * window plus a W*H threshold (max 7 MB), at least W*H*3/2, 1 KB aligned. */
static void apply_vbv(struct mediad_venc *v, const struct mediad_venc_cfg *cfg)
{
    int bitrate_kb = (cfg->bitrate > 0 ? cfg->bitrate : 1000000) >> 10;
    long nMinSize = (long)v->pic_w * v->pic_h * 3 / 2;
    long nThreshSize = (long)v->pic_w * v->pic_h;
    long nCacheSize = (long)bitrate_kb * 2 * (1024 / 8);
    long vbvSize = nCacheSize + nThreshSize;
    int vbv, thresh;

    if (nThreshSize > 7 * 1024 * 1024)
        nThreshSize = 7 * 1024 * 1024;
    if (vbvSize < nMinSize)
        vbvSize = nMinSize;
    if (vbvSize % 1024)
        vbvSize = (vbvSize + 1023) & ~1023L;

    vbv = (int)vbvSize;
    thresh = (int)nThreshSize;
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_BITSTREAM_SIZE, &vbv);
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FRAME_LENGTH_THRESHOLD, &thresh);
    fprintf(stderr, "[venc] vbv=%d thresh=%d (%d kb/s)\n", vbv, thresh, bitrate_kb);
}

struct mediad_venc *mediad_venc_open(const struct mediad_venc_cfg *cfg)
{
    struct mediad_venc *v;

    if (cfg == NULL || cfg->src_w <= 0 || cfg->src_h <= 0 ||
        cfg->pic_w <= 0 || cfg->pic_h <= 0)
        return NULL;

    v = calloc(1, sizeof(*v));
    if (v == NULL)
        return NULL;

    v->src_w = cfg->src_w;
    v->src_h = cfg->src_h;
    v->pic_w = cfg->pic_w;
    v->pic_h = cfg->pic_h;
    v->stride = (cfg->src_w + 15) & ~15;
    v->crop_x = cfg->crop_x;
    v->crop_y = cfg->crop_y;
    v->chn = cfg->chn;
    v->codec = cfg->codec ? 1 : 0;

    v->enc = VideoEncCreate(v->codec ? FWM_VENC_CODEC_H265 : FWM_VENC_CODEC_H264);
    if (v->enc == NULL) {
        fprintf(stderr, "[venc] create failed\n");
        free(v);
        return NULL;
    }
    apply_defaults(v, cfg);
    apply_vbv(v, cfg);

    if (VideoEncInit(v->enc, &(fwm_venc_base_config_t){
            /* Input window: pic_w x pic_h at (crop_x, crop_y), input == output,
             * so no scaler (a larger input than output stalls the encoder). */
            .input_width = (unsigned)(v->crop_x >= 0 ? cfg->pic_w : cfg->src_w),
            .input_height = (unsigned)(v->crop_x >= 0 ? cfg->pic_h : cfg->src_h),
            .input_stride = (unsigned)v->stride,
            .output_width = (unsigned)cfg->pic_w,
            .output_height = (unsigned)cfg->pic_h,
            /* Must match the capture format (LBC 2.5X, or YVU420SP for NV21);
             * a mismatch corrupts the colour. */
            .input_format = mediad_capture_nv21() ? FWM_VENC_PIXEL_YVU420SP
                                                  : FWM_VENC_PIXEL_LBC,
            .lbc_lossy_2_5x_en = mediad_capture_nv21() ? 0 : 1,
            .lbc_lossy_2x_en = 0,
            .bitstream_uncached_en = 1,
        }) != 0) {
        fprintf(stderr, "[venc] init failed\n");
        VideoEncDestroy(v->enc);
        free(v);
        return NULL;
    }

    if (v->chn >= 0)
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_CHANNEL, &v->chn);

    fprintf(stderr, "[venc] chn=%d %dx%d (stride %d) -> %dx%d @%d fps %d bps gop %d profile %d\n",
            v->chn, cfg->src_w, cfg->src_h, v->stride, cfg->pic_w, cfg->pic_h,
            cfg->fps, cfg->bitrate, cfg->gop, cfg->profile);
    return v;
}

void mediad_venc_close(struct mediad_venc *v)
{
    if (v == NULL)
        return;
    if (v->enc)
        VideoEncDestroy(v->enc);
    free(v);
}

int mediad_venc_spspps(struct mediad_venc *v, unsigned char *out, size_t out_cap)
{
    fwm_venc_header_blob_t h;

    if (v == NULL || v->enc == NULL || out == NULL)
        return 0;
    memset(&h, 0, sizeof(h));
    if (VideoEncGetParameter(v->enc,
            v->codec ? FWM_VENC_PARAM_H265_HEADER : FWM_VENC_PARAM_H264_SPS_PPS, &h) != 0 ||
        h.data == NULL || h.length == 0 || (size_t)h.length > out_cap)
        return 0;
    memcpy(out, h.data, h.length);
    return (int)h.length;
}

/* Copy the encoder's output descriptor into our transport struct. Every field
 * is kept because FreeOneBitStreamFrame needs the descriptor back intact. */
static void fill_frame(struct mediad_venc_frame *out, const fwm_venc_output_frame_t *ob)
{
    out->addr0 = ob->data0;
    out->len0 = ob->size0;
    out->addr1 = ob->data1;
    out->len1 = ob->size1;
    out->addr2 = ob->data2;
    out->len2 = ob->size2;
    out->flag = ob->flags;
    out->id = ob->id;
    out->pts = (uint64_t)ob->pts;
    out->curr_qp = ob->stats.qp;
    out->av_qp = ob->stats.qp_average;
    out->gop_index = ob->stats.gop_index;
    out->frame_index = ob->stats.frame_index;
    out->total_index = ob->stats.total_index;
}

int mediad_venc_ready(struct mediad_venc *v)
{
    if (v == NULL || v->enc == NULL)
        return 0;
    return ValidBitstreamFrameNum(v->enc);
}

int mediad_venc_next(struct mediad_venc *v, struct mediad_venc_frame *out)
{
    fwm_venc_output_frame_t ob;

    if (v == NULL || v->enc == NULL || out == NULL)
        return -1;
    if (ValidBitstreamFrameNum(v->enc) <= 0)
        return 1;                       /* nothing ready */
    memset(&ob, 0, sizeof(ob));
    if (GetOneBitstreamFrame(v->enc, &ob) != 0)
        return 1;
    memset(out, 0, sizeof(*out));
    fill_frame(out, &ob);
    return 0;
}

int mediad_venc_encode(struct mediad_venc *v, const struct cov1 *cov,
                       struct mediad_venc_frame *out)
{
    fwm_venc_input_picture_t in;
    int ret;

    if (v == NULL || v->enc == NULL || cov == NULL || out == NULL)
        return -1;
    if (cov->phyY == NULL)
        return -1;

    /* Zero-copy: AddInputBuffer copies only the descriptor, so the capture
     * buffer's physical addresses can be handed over directly. */
    memset(&in, 0, sizeof(in));
    in.luma_phys = cov->phyY;
    in.chroma_phys = cov->phyC;
    in.luma_virt = cov->virY;
    in.chroma_virt = cov->virC;
    in.pool_owned_en = 0;
    in.pts = (long long)now_us();
    if (v->crop_x >= 0) {
        in.crop_en = 1;
        in.crop.left = v->crop_x;
        in.crop.top = v->crop_y;
        in.crop.width = v->pic_w;
        in.crop.height = v->pic_h;
    }

    {
        uint64_t t0 = now_us(), t1, t2, t3, t4;
        int r;

        if (AddOneInputBuffer(v->enc, &in) != 0)
            return -2;                  /* input pool exhausted; caller resets */
        t1 = now_us();
        r = VideoEncodeOneFrame(v->enc);
        t2 = now_us();
        /* Drain the used-input slot even on failure; otherwise the pool loses
         * one buffer per error until every AddInputBuffer fails. */
        (void)AlreadyUsedInputBuffer(v->enc, &in);
        t3 = now_us();
        if (r != 0)
            return -1;
        r = mediad_venc_next(v, out);
        t4 = now_us();
        if (t1 - t0 > 100000 || t2 - t1 > 100000 || t3 - t2 > 100000 || t4 - t3 > 100000)
            fprintf(stderr, "[venc] enc split: add=%llu enc=%llu rel=%llu next=%llu us\n",
                    (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
                    (unsigned long long)(t3 - t2), (unsigned long long)(t4 - t3));
        return r;
    }
}

void mediad_venc_release(struct mediad_venc *v, const struct mediad_venc_frame *f)
{
    fwm_venc_output_frame_t ob;

    if (v == NULL || v->enc == NULL || f == NULL)
        return;
    /* Hand the descriptor back field-for-field: the encoder frees the slot by
     * id/flags, so a zeroed descriptor never releases it. */
    memset(&ob, 0, sizeof(ob));
    ob.id = f->id;
    ob.pts = (long long)f->pts;
    ob.flags = f->flag;
    ob.size0 = (unsigned int)f->len0;
    ob.size1 = (unsigned int)f->len1;
    ob.size2 = (unsigned int)f->len2;
    ob.data0 = (unsigned char *)f->addr0;
    ob.data1 = (unsigned char *)f->addr1;
    ob.data2 = (unsigned char *)f->addr2;
    ob.stats.qp = f->curr_qp;
    ob.stats.qp_average = f->av_qp;
    ob.stats.gop_index = f->gop_index;
    ob.stats.frame_index = f->frame_index;
    ob.stats.total_index = f->total_index;
    FreeOneBitStreamFrame(v->enc, &ob);
}

int mediad_venc_reset(struct mediad_venc *v)
{
    int force = 1;

    if (v == NULL || v->enc == NULL)
        return -1;
    if (VideoEncoderReset(v->enc) != 0)
        return -1;
    VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FORCE_KEY_FRAME, &force);
    return 0;
}

void mediad_venc_request_idr(struct mediad_venc *v)
{
    int force = 1;

    if (v && v->enc)
        VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FORCE_KEY_FRAME, &force);
}

int mediad_venc_set_bitrate(struct mediad_venc *v, int bps)
{
    if (v == NULL || v->enc == NULL || bps <= 0)
        return -1;
    return VideoEncSetParameter(v->enc, FWM_VENC_PARAM_BITRATE, &bps) == 0 ? 0 : -1;
}

int mediad_venc_set_filter3d(struct mediad_venc *v, int strength)
{
    int s = strength < 0 ? 0 : strength > 511 ? 511 : strength;

    if (v == NULL || v->enc == NULL)
        return -1;
    /* The full 9-bit hardware threshold (freecodec extension); the vendor
     * levels 1..3 only reach T = 1..6, which is barely visible. */
    return VideoEncSetParameter(v->enc, FWM_VENC_PARAM_FILTER_3D_STRENGTH, &s) == 0 ? 0 : -1;
}

/* Burned-in overlay via FWM_VENC_PARAM_OVERLAY. ARGB1555 with extra_alpha_flag=0
 * keeps the per-pixel alpha bit: only drawn pixels are burned in. */
int mediad_venc_set_overlay(struct mediad_venc *v,
                            const struct mediad_venc_ovl_blk *blks, int n)
{
    fwm_venc_overlay_t info;
    int i;

    if (v == NULL || v->enc == NULL || n < 0 || n > FWM_VENC_OVERLAY_MAX_REGIONS)
        return -1;
    if (n > 0 && blks == NULL)
        return -1;

    memset(&info, 0, sizeof(info));
    info.argb_type = FWM_VENC_OVERLAY_ARGB1555;
    for (i = 0; i < n; i++) {
        fwm_venc_overlay_region_t *h = &info.regions[i];

        h->start_mb_x = blks[i].start_mb_x;
        h->start_mb_y = blks[i].start_mb_y;
        h->end_mb_x = blks[i].end_mb_x;
        h->end_mb_y = blks[i].end_mb_y;
        h->extra_alpha_en = 0;   /* use the bitmap's per-pixel alpha bit */
        h->extra_alpha = 0;
        h->overlay_type = blks[i].hw_invert ? FWM_VENC_OVERLAY_LUMA_REVERSE : FWM_VENC_OVERLAY_NORMAL;
        h->bitmap = (unsigned char *)blks[i].bits;
        h->bitmap_size = blks[i].bytes;
        h->reverse_unit_mb_w_minus1 = blks[i].unit_w_minus1;
        h->reverse_unit_mb_h_minus1 = blks[i].unit_h_minus1;
    }
    info.region_count = (unsigned char)n;

    return VideoEncSetParameter(v->enc, FWM_VENC_PARAM_OVERLAY, &info) == 0 ? 0 : -1;
}
