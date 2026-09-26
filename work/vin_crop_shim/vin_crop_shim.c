// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * vin_crop_shim.c - LD_PRELOAD: apply a sunxi-vin VIPP crop
 * (VIDIOC_S_SELECTION, V4L2_SEL_TGT_CROP) to the capture node whose S_FMT
 * matches VINCROP's w x h. Crop happens in the VIPP, before LBC compression,
 * so the encoder gets a clean w x h picture with no SPS crop needed. (The
 * encoder cannot crop: its input is LBC-compressed and freecodec ignores the
 * per-picture crop fields.) Used by r35gb, whose 1936x1096 capture has a
 * garbage margin that Protect's live view shows when left to an SPS crop.
 *
 *   VINCROP=x,y,w,h   e.g. 8,8,1920,1080
 *
 * Applied after a matching S_FMT and again just before STREAMON on that fd;
 * each attempt logs the G_SELECTION readback to stderr.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>

static int (*real_ioctl)(int, int, ...);
static int crop_fd = -1;
static int cx, cy, cw, ch, have_cfg = -1;

static void load_cfg(void)
{
    const char *e = getenv("VINCROP");

    have_cfg = e && sscanf(e, "%d,%d,%d,%d", &cx, &cy, &cw, &ch) == 4;
    real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
}

static void apply(int fd, const char *when)
{
    struct v4l2_selection s;
    int r;

    memset(&s, 0, sizeof(s));
    s.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    s.target = V4L2_SEL_TGT_CROP;
    s.r.left = cx;
    s.r.top = cy;
    s.r.width = (unsigned)cw;
    s.r.height = (unsigned)ch;
    r = real_ioctl(fd, (int)VIDIOC_S_SELECTION, &s);
    fprintf(stderr, "vincrop[%s] fd=%d S_SELECTION %d,%d %dx%d -> %d",
            when, fd, cx, cy, cw, ch, r);
    memset(&s, 0, sizeof(s));
    s.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    s.target = V4L2_SEL_TGT_CROP;
    r = real_ioctl(fd, (int)VIDIOC_G_SELECTION, &s);
    fprintf(stderr, "; G_SELECTION -> %d (%d,%d %ux%u)\n",
            r, s.r.left, s.r.top, s.r.width, s.r.height);
}

int ioctl(int fd, int req, ...)
{
    va_list ap;
    void *arg;
    int ret;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (have_cfg < 0)
        load_cfg();

    if (have_cfg && fd == crop_fd && (unsigned)req == (unsigned)VIDIOC_STREAMON)
        apply(fd, "streamon");
    ret = real_ioctl(fd, req, arg);
    if (have_cfg && ret == 0 && (unsigned)req == (unsigned)VIDIOC_S_FMT) {
        const struct v4l2_format *f = arg;

        if ((int)f->fmt.pix_mp.width == cw && (int)f->fmt.pix_mp.height == ch) {
            crop_fd = fd;
            apply(fd, "s_fmt");
        }
    }
    return ret;
}
