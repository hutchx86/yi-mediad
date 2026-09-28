// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* osd.h - burned-in OSD (date / camera name / logo / bitrate) for Protect's
 * ChangeOsdSettings: ARGB1555 blocks rendered here and handed to the encoder's
 * overlay engine via mediad_venc_set_overlay(). */
#ifndef MEDIAD_OSD_H
#define MEDIAD_OSD_H

struct mediad_venc;

typedef struct {
    struct mediad_venc *venc;  /* encoder to push the overlay to (may be NULL) */
    int venc_chn;
    int frame_w, frame_h;
} osd_chan_cfg;

/* Control keys (int-valued; exposed by isp_control.c on the socket/web-UI). */
enum {
    OSD_ENABLE = 0,   /* master (Protect enableOverlay) */
    OSD_DATE,         /* show date/time */
    OSD_NAME,         /* show camera name */
    OSD_LOGO,         /* show logo (placeholder) */
    OSD_BITRATE,      /* show encoder bitrate (streamer-stats line) */
    OSD_TEXT_SCALE,   /* 0..100 (Protect textScale) */
    OSD_LOGO_SCALE,   /* 0..100 (Protect logoScale) */
    OSD_COLOR,        /* overlayColorId: palette index */
    OSD_POS,          /* text block position: 0 TL, 1 TR, 2 BL, 3 BR */
    OSD_NCTL
};

/* Start drawing on the given venc channels (must already exist). camera_name
 * may be NULL. Returns 0 on success. */
int osd_start(const osd_chan_cfg *chans, int nchan, const char *camera_name);
void osd_stop(void);

/* Re-render + re-upload everything (call after a control change). */
void osd_refresh(void);

/* Generic control access (OSD_NCTL keys). set returns 0 on success. */
int osd_set(int key, int value);
int osd_get(int key);

/* Camera name source (Protect's ChangeOsdSettings.tag); empty resets to the
 * default (env / device-name file / model). */
void osd_set_name(const char *name);
const char *osd_get_name(void);

#endif /* MEDIAD_OSD_H */
