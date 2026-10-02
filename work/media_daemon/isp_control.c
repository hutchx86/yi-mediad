// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* isp_control.c - control socket (one request line, one reply line: set, get,
 * list, reset, ping, pin, unpin, ...; see README), mediad.conf, night vision
 * and the optional web UI. Keys and ranges: g_controls[]. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <time.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "media/mpi_isp.h"
#include "media/mpi_vi.h"
#include "media/mm_comm_vi.h"

#include "isp_control.h"
#include "isp_config.h"
#include "osd.h"

#define DEFAULT_SOCK "/tmp/mediad_ctl.sock"

struct ctl {
    const char *key;
    unsigned int mask;      /* register-overlay bit, 0 = none */
    int vmin, vmax;
    int deft;               /* value restored by `reset` (if mask != 0) */
    int (*set)(int dev, int v);
    int (*get)(int dev, int *v);
    unsigned char locked;   /* not settable over the socket; conf / web UI only */
    unsigned char pinned;   /* socket sets ACKed but ignored (conf/web UI value wins) */
};

static int set_saturation(int d, int v) { (void)d; return isp_config_set_saturation(v); }
static int set_aebias(int d, int v)     { return AW_MPI_ISP_AE_SetExposureBias(d, v); }
static int get_aebias(int d, int *v)    { return AW_MPI_ISP_AE_GetExposureBias(d, v); }
static int get_saturation_cfg(int d, int *v) { (void)d; *v = isp_config_get_saturation(); return 0; }
static int set_brightness_cfg(int d, int v) { (void)d; return isp_config_set_brightness(v); }
static int get_brightness_cfg(int d, int *v) { (void)d; *v = isp_config_get_brightness(); return 0; }
static int set_contrast_cfg(int d, int v) { (void)d; return isp_config_set_contrast(v); }
static int get_contrast_cfg(int d, int *v) { (void)d; *v = isp_config_get_contrast(); return 0; }
static int set_denoise_cfg(int d, int v) { (void)d; return isp_config_set_denoise(v); }
static int get_denoise_cfg(int d, int *v) { (void)d; *v = isp_config_get_denoise(); return 0; }
static int set_exposure_cfg(int d, int v) { (void)d; return isp_config_set_exposure(v); }
static int get_exposure_cfg(int d, int *v) { (void)d; *v = isp_config_get_exposure(); return 0; }
static int set_gamma_cfg(int d, int v) { (void)d; return isp_config_set_gamma(v); }
static int get_gamma_cfg(int d, int *v) { (void)d; *v = isp_config_get_gamma(); return 0; }
static int set_hue_cfg(int d, int v) { (void)d; return isp_config_set_hue(v); }
static int get_hue_cfg(int d, int *v) { (void)d; *v = isp_config_get_hue(); return 0; }
static int set_sharpness(int d, int v) { (void)d; return isp_config_set_sharpness(v); }
static int get_sharpness_cfg(int d, int *v) { (void)d; *v = isp_config_get_sharpness(); return 0; }
static int set_pltm_cfg(int d, int v)   { (void)d; return isp_config_set_pltm(v); }
static int get_pltm_cfg(int d, int *v)  { (void)d; *v = isp_config_get_pltm(); return 0; }
static int set_tdf_cfg(int d, int v)    { (void)d; return isp_config_set_tdf(v); }
static int set_nr2d_cfg(int d, int v)   { (void)d; return isp_config_set_nr2d(v); }
static int get_nr2d_cfg(int d, int *v)  { (void)d; *v = isp_config_want_nr2d(); return 0; }
static int set_cnr_cfg(int d, int v)    { (void)d; return isp_config_set_cnr(v); }
static int get_cnr_cfg(int d, int *v)   { (void)d; *v = isp_config_want_cnr(); return 0; }
static int set_venc3d(int d, int v)     { (void)d; return mediad_set_venc3d(v); }
static int get_venc3d(int d, int *v)    { (void)d; *v = mediad_get_venc3d(); return 0; }
static int get_tdf_cfg(int d, int *v)   { (void)d; *v = isp_config_get_tdf(); return 0; }
static int set_wdr(int d, int v)        { return AW_MPI_ISP_SetPltmWDR(d, v); }
static int set_flicker(int d, int v)    { return AW_MPI_ISP_SetFlicker(d, v); }
/* Orientation: hardware = model base XOR user setting; getters report the user
 * value (what Protect shows). */
static int g_orient_base_mirror, g_orient_base_flip;
static int g_user_mirror, g_user_flip;

static int orient_apply(int d)
{
    int em = g_user_mirror ^ g_orient_base_mirror;
    int ef = g_user_flip   ^ g_orient_base_flip;
    int r = AW_MPI_VI_SetVippMirror(d, em);
    if (AW_MPI_VI_SetVippFlip(d, ef) != 0 && r == 0)
        r = -1;
    return r;
}
static int set_mirror(int d, int v)     { g_user_mirror = v ? 1 : 0; return orient_apply(d); }
static int set_flip(int d, int v)       { g_user_flip = v ? 1 : 0;   return orient_apply(d); }

/* Called once at boot by main.c with the capability-table base; applies the
 * base to both vipps and starts the user view at 0/0. */
void isp_control_set_orientation(int base_mirror, int base_flip)
{
    g_orient_base_mirror = base_mirror ? 1 : 0;
    g_orient_base_flip = base_flip ? 1 : 0;
    g_user_mirror = g_user_flip = 0;
    orient_apply(0);
    orient_apply(1);
    fprintf(stderr, "mediad: orientation base mirror=%d flip=%d (user 0/0)\n",
            g_orient_base_mirror, g_orient_base_flip);
}
static int set_bitrate(int d, int v)    { (void)d; return mediad_set_bitrate("high", (unsigned)v); }
static int get_bitrate(int d, int *v)   { (void)d; *v = (int)mediad_get_bitrate("high"); return 0; }
/* Video codec, per channel: 0 = H.264, 1 = H.265/HEVC. Protect selects it per
 * stream (video1 -> high, video2/3 -> low), so the channels switch independently
 * and a mixed settings object no longer churns the encoder. `codec` sets both. */
static int set_codec(int d, int v)      { (void)d; return mediad_set_codec("all", v); }
static int get_codec(int d, int *v)     { (void)d; *v = mediad_get_codec("high"); return 0; }
static int set_codec_high(int d, int v) { (void)d; return mediad_set_codec("high", v); }
static int set_codec_low(int d, int v)  { (void)d; return mediad_set_codec("low", v); }
static int get_codec_high(int d, int *v){ (void)d; *v = mediad_get_codec("high"); return 0; }
static int get_codec_low(int d, int *v) { (void)d; *v = mediad_get_codec("low"); return 0; }

/* Burned-in OSD (osd.c). Keys are int-valued; the camera name is a string
 * sourced from MEDIAD_OSD_NAME / the device-name file. */
static int set_osd(int d, int v)         { (void)d; return osd_set(OSD_ENABLE, v); }
static int get_osd(int d, int *v)        { (void)d; *v = osd_get(OSD_ENABLE); return 0; }
static int set_osd_date(int d, int v)    { (void)d; return osd_set(OSD_DATE, v); }
static int get_osd_date(int d, int *v)   { (void)d; *v = osd_get(OSD_DATE); return 0; }
static int set_osd_name(int d, int v)    { (void)d; return osd_set(OSD_NAME, v); }
static int get_osd_name(int d, int *v)   { (void)d; *v = osd_get(OSD_NAME); return 0; }
static int set_osd_logo(int d, int v)    { (void)d; return osd_set(OSD_LOGO, v); }
static int get_osd_logo(int d, int *v)   { (void)d; *v = osd_get(OSD_LOGO); return 0; }
static int set_osd_br(int d, int v)      { (void)d; return osd_set(OSD_BITRATE, v); }
static int get_osd_br(int d, int *v)     { (void)d; *v = osd_get(OSD_BITRATE); return 0; }
static int set_osd_tscale(int d, int v)  { (void)d; return osd_set(OSD_TEXT_SCALE, v); }
static int get_osd_tscale(int d, int *v) { (void)d; *v = osd_get(OSD_TEXT_SCALE); return 0; }
static int set_osd_lscale(int d, int v)  { (void)d; return osd_set(OSD_LOGO_SCALE, v); }
static int get_osd_lscale(int d, int *v) { (void)d; *v = osd_get(OSD_LOGO_SCALE); return 0; }
static int set_osd_color(int d, int v)   { (void)d; return osd_set(OSD_COLOR, v); }
static int get_osd_color(int d, int *v)  { (void)d; *v = osd_get(OSD_COLOR); return 0; }
static int set_osd_pos(int d, int v)     { (void)d; return osd_set(OSD_POS, v); }
static int get_osd_pos(int d, int *v)    { (void)d; *v = osd_get(OSD_POS); return 0; }

/* Night vision: IR-cut filter and IR LED via /dev/cpld_periph ioctls, plus the
 * ISP day/night swap (isp_config_set_daynight()). */
#define CPLD_DEV       "/dev/cpld_periph"
#define CPLD_IRCUT_OUT 0x7015
#define CPLD_IRCUT_IN  0x7016
#define CPLD_LED_LEVEL 0x7013

static int cpld_req(unsigned long req, void *arg)
{
    int fd = open(CPLD_DEV, O_RDWR);
    int r;
    if (fd < 0)
        return -1;
    r = ioctl(fd, req, arg);
    close(fd);
    return r < 0 ? -1 : 0;
}

static int g_ir_cut;    /* 1 = filter out (night/IR-passing), 0 = in (day) */
static int g_ir_led;    /* 0..100 */
static int g_nightvision = 2; /* 0 day, 1 night, 2 auto */
static int g_shutter;   /* VI_SHUTTIME_MODE_E */

/* Day(0)/night(1), in stock order: filter first (0x7015 out, 0x7016 in), wait
 * for it to travel, then the LED level (0x7013, int32 pointer). */
static void night_state_save(void);

static void daynight_apply(int night)
{
    int32_t lvl = night ? 100 : 0;
    if (night) {
        if (cpld_req(CPLD_IRCUT_OUT, 0) != 0)
            fprintf(stderr, "mediad: CPLD ircut-out (0x7015) failed\n");
        usleep(250 * 1000);            /* filter travel (stock waits 250 ms) */
        if (cpld_req(CPLD_LED_LEVEL, &lvl) != 0)
            fprintf(stderr, "mediad: CPLD led 100 (0x7013) failed\n");
    } else {
        if (cpld_req(CPLD_IRCUT_IN, 0) != 0)
            fprintf(stderr, "mediad: CPLD ircut-in (0x7016) failed\n");
        usleep(200 * 1000);            /* stock waits 200 ms */
        if (cpld_req(CPLD_LED_LEVEL, &lvl) != 0)
            fprintf(stderr, "mediad: CPLD led 0 (0x7013) failed\n");
    }
    g_ir_cut = night ? 1 : 0;
    g_ir_led = (int)lvl;
    night_state_save();
    isp_config_set_daynight(night);
    /* Re-apply the CCM so night's forced monochrome takes effect regardless of
     * the saturation control. */
    isp_config_set_saturation(isp_config_get_saturation());
}

/* Auto day/night from the AE light value (1/100 EV): lux ~= 2.5 * 2^(lv/100),
 * uncalibrated. 10 lv hysteresis, NIGHT_DEBOUNCE consecutive 3 s polls. */
#define NIGHT_LV_PER_EV 100.0
#define NIGHT_HYST_LV   40        /* 0.4 EV band, so a flickering scene can't oscillate */
#define NIGHT_POLL_SEC  3
#define NIGHT_DEBOUNCE  5         /* consecutive agreeing polls before switching */
#define NIGHT_MIN_SWITCH_TICKS 10 /* >=30 s between switches */
#define NIGHT_AMBIENT_PERIOD 120  /* IR-off daylight confirm at most every 2 min */
#define NIGHT_AMBIENT_MAX_PERIOD 900 /* ... backing off to 15 min while it stays dark */
#define NIGHT_SETTLE_POLLS 4      /* polls (12 s) for AE to settle under IR */
#define NIGHT_AMBIENT_RISE_LV 100 /* 1 EV above the IR-lit baseline = real ambient light */

/* Switch-to-night threshold, lux: the estimate floors at about 4-8 "lux" in
 * the dark, so it must sit above that. */
static double g_night_lux = 20.0;
static int    g_lv_live;           /* last measured lv (calibration readout) */
static pthread_t g_night_tid;
static volatile int g_night_run;

static double lv_to_lux(int lv)
{
    if (lv < 0) lv = 0;
    if (lv > 4000) lv = 4000;               /* avoid double->int overflow */
    return 2.5 * pow(2.0, (double)lv / NIGHT_LV_PER_EV);
}
static int lux_to_lv(double lux)
{
    if (lux < 0.01) lux = 0.01;
    return (int)lround(NIGHT_LV_PER_EV * (log2(lux) - log2(2.5)));
}

static void *night_poller(void *arg)
{
    int is_night = g_ir_cut, cross = 0, tick = 0, resync = 1;
    int last_switch_tick = 0;
    time_t amb_next = 0;
    int amb_period = NIGHT_AMBIENT_PERIOD;
    /* With the IR on, lv is inflated: track its settled minimum (ir_base) and
     * consider day only once lv rises clearly above it. */
    int prev_night = -1, ir_base = -1, settle = 0;
    int prev_lv = -1000, steady = 0;    /* decisions only on a settled AE */
    (void)arg;
    while (g_night_run) {
        struct timespec ts = { NIGHT_POLL_SEC, 0 };
        int mode, lv, thr, want;
        nanosleep(&ts, NULL);
        if (!g_night_run)
            break;
        mode = g_nightvision;
        if (mode != 2) { cross = 0; resync = 1; continue; }
        /* Re-read the filter state every poll: manual switches change g_ir_cut
         * behind this loop's back. */
        is_night = g_ir_cut;
        if (is_night != prev_night) {       /* entered night (ours or manual) */
            prev_night = is_night;
            ir_base = -1;
            settle = NIGHT_SETTLE_POLLS;
            amb_period = NIGHT_AMBIENT_PERIOD;
        }
        lv = AW_MPI_ISP_GetEnvLV(0);
        if (lv <= 0)
            continue;                       /* AE not valid yet */
        g_lv_live = lv;
        if (++tick % 10 == 0)
            fprintf(stderr, "mediad: auto day/night poll lv=%d lux=%.1f\n",
                    lv, lv_to_lux(lv));
        /* Decide only once the AE has settled (three polls within +-20 lv); it
         * can take well over 12 s after a start. */
        steady = (abs(lv - prev_lv) <= 20) ? steady + 1 : 0;
        prev_lv = lv;
        if (steady < 2)
            continue;
        thr = lux_to_lv(g_night_lux);
        /* Night below thr-hyst; back to day only after lv rises
         * NIGHT_AMBIENT_RISE_LV above the IR baseline and passes the IR-off check. */
        if (is_night) {
            if (settle > 0) {
                settle--;
                want = 1;
            } else {
                if (ir_base < 0 || lv < ir_base)
                    ir_base = lv;
                want = !(lv >= ir_base + NIGHT_AMBIENT_RISE_LV);
            }
        } else {
            want = lv < thr - NIGHT_HYST_LV;
        }
        if (want == is_night && !resync) { cross = 0; continue; }
        if (!resync && ++cross < NIGHT_DEBOUNCE)
            continue;
        if (!resync && tick - last_switch_tick < NIGHT_MIN_SWITCH_TICKS)
            continue;                       /* anti-thrash: >=30 s between switches */
        cross = 0;
        /* First decision after entering auto skips the debounce and re-asserts
         * the hardware even if it already agrees. */
        resync = 0;
        if (is_night && want == 0) {
            /* The IR-lit lv cannot tell daylight from IR: occasionally confirm
             * with the LED briefly off. */
            time_t now = time(NULL);
            int32_t off = 0, on = 100;
            int lv2 = 0, prev = -1000, stable = 0, i;
            if (now < amb_next)
                continue;
            cpld_req(CPLD_LED_LEVEL, &off);
            /* Let the AE converge without the IR; stop early once clearly dark
             * or once lv holds steady for ~1 s. */
            for (i = 0; i < 20; i++) {
                usleep(300 * 1000);
                lv2 = AW_MPI_ISP_GetEnvLV(0);
                if (lv2 > 0 && lv2 < thr - NIGHT_HYST_LV)
                    break;
                stable = (lv2 > 0 && abs(lv2 - prev) <= 8) ? stable + 1 : 0;
                prev = lv2;
                if (stable >= 3)
                    break;
            }
            if (!(lv2 > 0 && lv2 >= thr + NIGHT_HYST_LV)) {
                cpld_req(CPLD_LED_LEVEL, &on);   /* still dark: restore */
                g_ir_led = 100;
                amb_next = now + amb_period;
                if (amb_period < NIGHT_AMBIENT_MAX_PERIOD)
                    amb_period *= 2;
                ir_base = -1;                   /* re-baseline after the blink */
                settle = NIGHT_SETTLE_POLLS;
                fprintf(stderr, "mediad: auto day/night IR-off check lv=%d -> stay night "
                        "(next check in %d s)\n", lv2, (int)(amb_next - now));
                continue;
            }
            fprintf(stderr, "mediad: auto day/night IR-off check lv=%d -> day\n", lv2);
            /* fall through: LED already off */
        }
        is_night = want;
        last_switch_tick = tick;
        daynight_apply(is_night);
        fprintf(stderr, "mediad: auto day/night -> %s (lv=%d lux=%.1f "
                "thr_lux=%.1f)\n", is_night ? "night" : "day", lv,
                lv_to_lux(lv), g_night_lux);
    }
    return NULL;
}

static int set_ir_cut(int d, int v)
{
    (void)d;
    if (cpld_req(v ? CPLD_IRCUT_OUT : CPLD_IRCUT_IN, 0) != 0)
        return -1;
    g_ir_cut = v ? 1 : 0;
    return isp_config_set_daynight(g_ir_cut);
}
static int get_ir_cut(int d, int *v) { (void)d; *v = g_ir_cut; return 0; }

static int set_ir_led(int d, int v)
{
    int32_t lvl;
    (void)d;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    lvl = v;
    if (cpld_req(CPLD_LED_LEVEL, &lvl) != 0)
        return -1;
    g_ir_led = v;
    return 0;
}
static int get_ir_led(int d, int *v) { (void)d; *v = g_ir_led; return 0; }

/* Day/night mode and threshold persist across restarts (Protect sends them only
 * on change); loaded before mediad.conf, so a pin there still wins. */
#define MEDIAD_NIGHT_FILE "/tmp/sd/unifi/etc/mediad.night"
static int g_night_loaded;          /* don't rewrite the file while loading it */

static void night_state_save(void)
{
    char tmp[] = MEDIAD_NIGHT_FILE ".tmp";
    FILE *f;

    if (!g_night_loaded)
        return;
    f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "nightvision=%d\nnight_lux=%d\nir_cut=%d\n", g_nightvision,
            (int)(g_night_lux + 0.5), g_ir_cut);
    if (fclose(f) == 0)
        rename(tmp, MEDIAD_NIGHT_FILE);
}

static int set_nightvision(int d, int v)
{
    (void)d;
    if (v < 0 || v > 2)
        return -1;
    if (v != g_nightvision) {
        g_nightvision = v;
        night_state_save();
    }
    if (v == 1)                /* always on (night) */
        daynight_apply(1);
    else if (v == 0)           /* always off (day) */
        daynight_apply(0);
    /* v == 2 auto: driven by night_poller() */
    return 0;
}
static int get_nightvision(int d, int *v) { (void)d; *v = g_nightvision; return 0; }

static int set_night_lux(int d, int v)
{
    double lux = v > 0 ? (double)v : 0.0;

    (void)d;
    if (lux != g_night_lux) {
        g_night_lux = lux;
        night_state_save();
    }
    return 0;
}
static int get_night_lux(int d, int *v) { (void)d; *v = (int)(g_night_lux + 0.5); return 0; }
static int set_lux_ro(int d, int v) { (void)d; (void)v; return -1; } /* read-only */
static int get_lux(int d, int *v) { (void)d; *v = (int)(lv_to_lux(g_lv_live) + 0.5); return 0; }
static int get_lv(int d, int *v) { (void)d; *v = g_lv_live; return 0; } /* raw AE LV, read-only */

static int set_shutter(int d, int v)
{
    (void)d;
    if (mediad_set_shutter(v) != 0)
        return -1;
    g_shutter = v;
    return 0;
}
static int get_shutter(int d, int *v) { (void)d; *v = g_shutter; return 0; }

static int get_wdr(int d, int *v)        { return AW_MPI_ISP_GetPltmWDR(d, v); }
static int get_flicker(int d, int *v)    { return AW_MPI_ISP_GetFlicker(d, v); }
static int get_mirror(int d, int *v)     { (void)d; *v = g_user_mirror; return 0; }
static int get_flip(int d, int *v)       { (void)d; *v = g_user_flip; return 0; }

/* Register ranges overlaid onto a replayed stock table (shared first 0x1000).
 * Brightness/contrast (gamma/DRC tables) and WDR (0x200) are left as stock. */
static struct ctl g_controls[] = {
    /* 0..100 picture levels, 50 == stock/neutral: brightness/contrast map to
     * (v-50)*4, saturation/hue to the CCM (identity at 50). */
    { "brightness", 0,                  0,   100, 50,  set_brightness_cfg, get_brightness_cfg },
    { "contrast",   0,                  0,   100, 50,  set_contrast_cfg,   get_contrast_cfg },
    { "saturation", ISP_CTL_SATURATION,  0,   100, 50,  set_saturation, get_saturation_cfg },
    { "hue",        0,                  0,   100, 50,  set_hue_cfg,    get_hue_cfg },
    { "sharpness",  ISP_CTL_SHARPNESS,  0,   10,  5,   set_sharpness,  get_sharpness_cfg },
    /* Denoise strength ramp, 0..100 (0 = the tuning's own thresholds); the
     * tdf/nr2d/cnr switches pick which modules run. */
    { "denoise",    ISP_CTL_NR,         0,   100, 100, set_denoise_cfg, get_denoise_cfg },
    { "exposure",   0,                  0,   100, 50,  set_exposure_cfg, get_exposure_cfg, 1 },
    { "aebias",     0,                  0,   8,   4,   set_aebias,     get_aebias,       1 },
    /* Gamma transforms the camera's own curve (50 == stock); locked from the
     * socket, adjustable from mediad.conf or the web UI. */
    { "gamma",      0,                  0,   100, 50,  set_gamma_cfg,  get_gamma_cfg, 1 },
    /* 3DNR module on/off (Protect enable3dnr). Default on; it can leave a faint
     * ghost behind motion in low light. */
    { "tdf",        ISP_CTL_3DNR,       0,   1,   1,   set_tdf_cfg,    get_tdf_cfg },
    /* ISP spatial and chroma denoise switches, and the encoder 3D filter
     * strength (hardware threshold 0..511; vendor levels 1..3 are about 1..6). */
    { "nr2d",       0,                  0,   1,   1,   set_nr2d_cfg,   get_nr2d_cfg },
    { "cnr",        0,                  0,   1,   0,   set_cnr_cfg,    get_cnr_cfg },
    { "venc3d",     0,                  0,   511, 0,   set_venc3d,     get_venc3d },
    /* `wdr` (PLTM strength) is locked from the socket: runtime strength changes
     * corrupt the WDR pipeline. `pltm` below is the usable HDR on/off. */
    { "wdr",        ISP_CTL_PLTMWDR,    0,   255, 0,   set_wdr,        get_wdr, 1 },
    /* PLTM module on/off (1 == stock): what Protect's "WDR off" needs. */
    { "pltm",       0,                  0,   1,   1,   set_pltm_cfg,   get_pltm_cfg },
    { "flicker",    0,                  0,   3,   3,   set_flicker,    get_flicker },
    { "mirror",     0,                  0,   1,   0,   set_mirror,     get_mirror },
    { "flip",       0,                  0,   1,   0,   set_flip,       get_flip },
    /* HIGH encoder bitrate (bps), driven by Protect's bitRateCbrAvg/VbrMax;
     * LOW stays at its fixed 0.7 Mbps. */
    { "bitrate",     0, 48000, 4000000, 2800000, set_bitrate,     get_bitrate },
    /* Video codec: 0 = H.264, 1 = H.265/HEVC. Both channels switch; the change
     * re-creates the encoder on each channel's own thread at a frame boundary. */
    { "codec",       0,                  0,   1,   0,   set_codec,      get_codec },
    { "codec_high",  0,                  0,   1,   0,   set_codec_high, get_codec_high },
    { "codec_low",   0,                  0,   1,   0,   set_codec_low,  get_codec_low },
    /* HDR / frequency are aliases for the raw wdr/flicker controls (the
     * caller does the UI encoding). */
    { "hdr",         0,                  0,   255, 0,   set_wdr,         get_wdr, 1 },
    { "frequency",   0,                  0,   3,   3,   set_flicker,     get_flicker },
    /* Burned-in OSD: date / name / logo (placeholder) / bitrate. */
    { "osd",            0,               0,   1,   1,   set_osd,       get_osd },
    { "osd_date",       0,               0,   1,   1,   set_osd_date,  get_osd_date },
    { "osd_name",       0,               0,   1,   1,   set_osd_name,  get_osd_name },
    { "osd_logo",       0,               0,   1,   1,   set_osd_logo,  get_osd_logo },
    { "osd_bitrate",    0,               0,   1,   1,   set_osd_br,    get_osd_br },
    { "osd_text_scale", 0,               0,   100, 50,  set_osd_tscale, get_osd_tscale },
    { "osd_logo_scale", 0,               0,   100, 50,  set_osd_lscale, get_osd_lscale },
    { "osd_color",      0,               0,   5,   0,   set_osd_color, get_osd_color },
    { "osd_pos",        0,               0,   3,   0,   set_osd_pos,   get_osd_pos },
    /* Night vision + shutter exposure. */
    { "nightvision", 0,                  0,   2,   2,   set_nightvision, get_nightvision },
    { "ir_cut",      0,                  0,   1,   0,   set_ir_cut,      get_ir_cut },
    { "ir_led",      0,                  0,   100, 0,   set_ir_led,      get_ir_led },
    { "night_lux",   0,                  0,   100000, 20, set_night_lux, get_night_lux },
    { "lux",         0,                  0,   100000, 0, set_lux_ro,    get_lux },
    { "lv",          0,                  0,   100000, 0, set_lux_ro,    get_lv },
    { "shutter",     0,                  0,   2,   0,   set_shutter,     get_shutter },
};
#define NCTL ((int)(sizeof(g_controls) / sizeof(g_controls[0])))

static int g_isp_dev;
static volatile unsigned int g_mask;
static volatile int g_running;
static int g_listen_fd = -1;

/* Coalescing apply worker: a burst of `set`s (Protect resends every field) is
 * reduced to the latest value per key and applied once; repeats are no-ops. */
#define PEND_SETTLE_MS 300
static pthread_mutex_t g_pend_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pend_cv = PTHREAD_COND_INITIALIZER;
static int g_pend_val[NCTL];
static unsigned char g_pend_valid[NCTL];
static int g_applied_val[NCTL];
static unsigned char g_applied_valid[NCTL];
static pthread_t g_apply_tid;
static volatile int g_apply_run;

static void *apply_thread(void *arg)
{
    (void)arg;
    while (g_apply_run) {
        int i, any = 0;
        int pv[NCTL];
        unsigned char pvalid[NCTL];

        pthread_mutex_lock(&g_pend_mu);
        for (i = 0; i < NCTL; i++)
            if (g_pend_valid[i]) { any = 1; break; }
        if (!any) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L;               /* 100 ms poll */
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_pend_cv, &g_pend_mu, &ts);
            pthread_mutex_unlock(&g_pend_mu);
            continue;
        }
        /* settle window: keep swallowing sets so a burst coalesces */
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += PEND_SETTLE_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_pend_cv, &g_pend_mu, &ts);
        }
        for (i = 0; i < NCTL; i++) {
            pv[i] = g_pend_val[i];
            pvalid[i] = g_pend_valid[i];
            g_pend_valid[i] = 0;
        }
        pthread_mutex_unlock(&g_pend_mu);

        for (i = 0; i < NCTL; i++) {
            if (!pvalid[i])
                continue;
            if (g_applied_valid[i] && g_applied_val[i] == pv[i])
                continue;                          /* already applied */
            g_controls[i].set(g_isp_dev, pv[i]);
            if (g_controls[i].mask)
                g_mask |= g_controls[i].mask;
            g_applied_val[i] = pv[i];
            g_applied_valid[i] = 1;
        }
    }
    return NULL;
}

/* Queue a validated set (called from the accept thread). */
static int pend_set(int idx, int value)
{
    if (idx < 0 || idx >= NCTL)
        return -1;
    pthread_mutex_lock(&g_pend_mu);
    g_pend_val[idx] = value;
    g_pend_valid[idx] = 1;
    pthread_cond_signal(&g_pend_cv);
    pthread_mutex_unlock(&g_pend_mu);
    return 0;
}

/* Last desired value for a key (the applied value may lag by the settle). */
static int pend_last(int idx, int *v)
{
    if (idx < 0 || idx >= NCTL)
        return -1;
    pthread_mutex_lock(&g_pend_mu);
    if (g_pend_valid[idx]) { *v = g_pend_val[idx]; pthread_mutex_unlock(&g_pend_mu); return 0; }
    if (g_applied_valid[idx]) { *v = g_applied_val[idx]; pthread_mutex_unlock(&g_pend_mu); return 0; }
    pthread_mutex_unlock(&g_pend_mu);
    return 1;
}
static char g_sock[108 + 1];
static pthread_t g_thread;

/* Pending `dump <path>` request for the next load-reg (see header). */
static char g_dump_path[160];
static volatile int g_dump_pending;

/* Calibrated base transforms (see header). */
struct xform {
    int type;
    unsigned int off;
    unsigned int len;
    int p1, p2;
};
static struct xform g_xforms[8];
static int g_nxforms;
static pthread_mutex_t g_xform_lock = PTHREAD_MUTEX_INITIALIZER;

int isp_control_set_xform(int type, unsigned int off, unsigned int len,
                          int p1, int p2)
{
    pthread_mutex_lock(&g_xform_lock);
    if (type == ISP_XFORM_SCALE && p2 == 0) {
        pthread_mutex_unlock(&g_xform_lock);
        return -1;
    }
    g_xforms[0].type = type;
    g_xforms[0].off = off;
    g_xforms[0].len = len;
    g_xforms[0].p1 = p1;
    g_xforms[0].p2 = p2;
    g_nxforms = 1;
    pthread_mutex_unlock(&g_xform_lock);
    return 0;
}

void isp_control_clear_xforms(void)
{
    pthread_mutex_lock(&g_xform_lock);
    g_nxforms = 0;
    pthread_mutex_unlock(&g_xform_lock);
}

int isp_control_xform_count(void)
{
    return g_nxforms;
}

void isp_control_apply_xforms(unsigned char *dst, unsigned int len)
{
    int i;

    pthread_mutex_lock(&g_xform_lock);
    for (i = 0; i < g_nxforms; i++) {
        const struct xform *x = &g_xforms[i];
        unsigned int o;
        for (o = x->off; o + 2 <= x->off + x->len && o + 2 <= len; o += 2) {
            unsigned short v;
            long r;
            memcpy(&v, dst + o, 2);
            if (x->type == ISP_XFORM_SCALE)
                r = (long)v * x->p1 / x->p2;
            else
                r = (long)v + x->p1;
            if (r < 0)
                r = 0;
            if (r > 65535)
                r = 65535;
            v = (unsigned short)r;
            memcpy(dst + o, &v, 2);
        }
    }
    pthread_mutex_unlock(&g_xform_lock);
}

int isp_control_pending_dump(char *out, size_t n)
{
    if (!g_dump_pending)
        return 0;
    snprintf(out, n, "%s", g_dump_path);
    g_dump_pending = 0;
    return 1;
}

/* mediad.conf path and its writer for `pin`/`unpin`, so mediad stays the only
 * writer of its config (yi-protect's settings page saves through these). */
static char g_conf_path[160] = "/tmp/sd/unifi/etc/mediad.conf";

/* Set `key=val`, or drop key= / pin_key= lines when val is NULL; other lines
 * are kept and the file is replaced atomically by rename. */
static int conf_store(const char *key, const int *val)
{
    char tmp[176], line[256];
    FILE *in, *out;
    int done = 0;
    size_t kl = strlen(key);

    snprintf(tmp, sizeof(tmp), "%s.tmp", g_conf_path);
    out = fopen(tmp, "w");
    if (!out)
        return -1;
    in = fopen(g_conf_path, "r");
    while (in && fgets(line, sizeof(line), in)) {
        const char *k = line;

        if (strncmp(k, "pin_", 4) == 0)
            k += 4;
        if (line[0] != '#' && strncmp(k, key, kl) == 0 &&
            (k[kl] == '=' || k[kl] == ' ' || k[kl] == '\t')) {
            if (val && !done)
                fprintf(out, "%s=%d\n", key, *val);
            done = 1;
            continue;
        }
        fputs(line, out);
    }
    if (in)
        fclose(in);
    if (val && !done)
        fprintf(out, "%s=%d\n", key, *val);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
        fclose(out);
        unlink(tmp);
        return -1;
    }
    fclose(out);
    return rename(tmp, g_conf_path);
}

static const struct ctl *find_ctl(const char *key)
{
    int i;
    for (i = 0; i < NCTL; i++)
        if (strcmp(g_controls[i].key, key) == 0)
            return &g_controls[i];
    return NULL;
}

/* Copy the masked module-register ranges from our computed table onto the
 * replayed stock table. Runs every load-reg (isp.c:217), i.e. once per frame. */
void isp_control_apply_overlay(unsigned char *dst, const unsigned char *computed,
                               unsigned int len)
{
    unsigned int mask = g_mask;

    if (!dst || !computed || mask == 0)
        return;
    if ((mask & ISP_CTL_SATURATION) && len >= 0x494)
        memcpy(dst + 0x490, computed + 0x490, 4);
    if ((mask & ISP_CTL_SHARPNESS) && len >= 0x43c)
        memcpy(dst + 0x420, computed + 0x420, 0x1c);
    if ((mask & ISP_CTL_NR) && len >= 0x474)
        memcpy(dst + 0x470, computed + 0x470, 4);
    if ((mask & ISP_CTL_3DNR) && len >= 0x2dc)
        memcpy(dst + 0x2d0, computed + 0x2d0, 0x0c);
    if ((mask & ISP_CTL_PLTMWDR) && len >= 0x3bc)
        memcpy(dst + 0x3b0, computed + 0x3b0, 0x0c);
}

/* Read one control's current value. Returns 0 on success, -1 if the getter
 * fails. */
static int ctl_read(const struct ctl *c, int *out)
{
    int r;
    *out = 0;
    r = c->get(g_isp_dev, out);
    return (r == 0) ? 0 : -1;
}

static void handle_conn(int fd)
{
    FILE *f = fdopen(fd, "r");
    char line[256];

    if (!f) {
        close(fd);
        return;
    }
    /* Read timeout: an idle client cannot block isp_control_stop() forever
     * (handle_conn runs in the accept thread). */
    while (g_running && fgets(line, sizeof(line), f)) {
        char key[64];
        char patharg[160];
        char a1[32], a2[32];
        int value;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;

        if (strncmp(line, "ping", 4) == 0 && line[4] == 0) {
            dprintf(fd, "ok pong\n");
        } else if (strncmp(line, "list", 4) == 0 && line[4] == 0) {
            int i, first = 1;
            dprintf(fd, "ok list ");
            for (i = 0; i < NCTL; i++) {
                if (g_controls[i].locked)
                    continue;   /* config/webui-only: not advertised to Protect */
                dprintf(fd, "%s%s", first ? "" : ",", g_controls[i].key);
                first = 0;
            }
            dprintf(fd, "\n");
        } else if (strncmp(line, "reset", 5) == 0 && line[5] == 0) {
            /* Restore every unpinned key to its stock default. */
            int i;
            for (i = 0; i < NCTL; i++) {
                if (g_controls[i].pinned)
                    continue;   /* pinned in mediad.conf: keep the pinned value */
                pend_set(i, g_controls[i].deft);
            }
            g_mask = 0;
            dprintf(fd, "ok reset\n");
        } else if (sscanf(line, "set %63s %d", key, &value) == 2) {
            const struct ctl *c = find_ctl(key);
            if (!c) {
                dprintf(fd, "err unknown key %s\n", key);
            } else if (c->locked) {
                /* not settable over the socket (Protect has no slider for it);
                 * set it in mediad.conf or the built-in web UI */
                dprintf(fd, "err %s is config/webui-only\n", key);
            } else if (c->pinned) {
                /* Pinned: ACK but ignore, or Protect's connect-time re-assert
                 * would overwrite the conf value. */
                dprintf(fd, "ok %s %d\n", key, value);
            } else if (value < c->vmin || value > c->vmax) {
                dprintf(fd, "err %s set failed (%d)\n", key, value);
            } else {
                /* queue it; the apply worker coalesces bursts (see above) */
                pend_set((int)(c - g_controls), value);
                dprintf(fd, "ok %s %d\n", key, value);
            }
        } else if (sscanf(line, "pin %63s %d", key, &value) == 2) {
            /* The firmware web page's save: apply the value regardless of any
             * pin/lock, pin it against Protect, and persist it to mediad.conf. */
            const struct ctl *c = find_ctl(key);
            if (!c) {
                dprintf(fd, "err unknown key %s\n", key);
            } else if (value < c->vmin || value > c->vmax) {
                dprintf(fd, "err %s out of range [%d,%d]\n", key, c->vmin, c->vmax);
            } else {
                int i = (int)(c - g_controls);
                pend_set(i, value);
                g_controls[i].pinned = 1;
                if (conf_store(key, &value) != 0)
                    dprintf(fd, "err %s applied but not saved\n", key);
                else
                    dprintf(fd, "ok %s %d pinned\n", key, value);
            }
        } else if (sscanf(line, "unpin %63s", key) == 1) {
            /* Hand the key back to Protect and drop it from mediad.conf. The
             * current value stays until Protect (or `set`) changes it. */
            const struct ctl *c = find_ctl(key);
            if (!c) {
                dprintf(fd, "err unknown key %s\n", key);
            } else {
                g_controls[c - g_controls].pinned = 0;
                if (conf_store(key, NULL) != 0)
                    dprintf(fd, "err %s unpinned but not saved\n", key);
                else
                    dprintf(fd, "ok %s unpinned\n", key);
            }
        } else if (sscanf(line, "pinned %63s", key) == 1) {
            const struct ctl *c = find_ctl(key);
            if (!c)
                dprintf(fd, "err unknown key %s\n", key);
            else
                dprintf(fd, "ok %s %d\n", key, c->pinned ? 1 : 0);
        } else if (sscanf(line, "dump %159s", patharg) == 1) {
            if (strncmp(patharg, "/tmp/", 5) != 0) {
                dprintf(fd, "err dump path must be under /tmp/\n");
            } else {
                snprintf(g_dump_path, sizeof(g_dump_path), "%s", patharg);
                g_dump_pending = 1;
                dprintf(fd, "ok dump %s\n", patharg);
            }
        } else if (sscanf(line, "poke %63s %63s", key, patharg) == 2) {
            unsigned int off = (unsigned int)strtoul(key, NULL, 0);
            unsigned int val = (unsigned int)strtoul(patharg, NULL, 0);
            unsigned int old = 0;
            if (isp_stock_reg_poke(off, val, &old) != 0)
                dprintf(fd, "err poke failed (no stock table or bad offset)\n");
            else
                dprintf(fd, "ok poke 0x%x 0x%x (was 0x%x)\n", off, val, old);
        } else if (sscanf(line, "peek %63s", key) == 1) {
            unsigned int off = (unsigned int)strtoul(key, NULL, 0);
            unsigned int val = 0;
            if (isp_stock_reg_peek(off, &val) != 0)
                dprintf(fd, "err peek failed\n");
            else
                dprintf(fd, "ok peek 0x%x 0x%x\n", off, val);
        } else if (sscanf(line, "scale %31s %31s %31s %31s",
                          key, a1, a2, patharg) == 4) {
            unsigned int off = (unsigned int)strtoul(key, NULL, 0);
            unsigned int len = (unsigned int)strtoul(a1, NULL, 0);
            int num = atoi(a2);
            int den = atoi(patharg);
            if (isp_control_set_xform(ISP_XFORM_SCALE, off, len, num, den) != 0)
                dprintf(fd, "err scale failed\n");
            else
                dprintf(fd, "ok scale 0x%x %u %d/%d\n", off, len, num, den);
        } else if (sscanf(line, "offset %31s %31s %31s",
                          key, a1, a2) == 3) {
            unsigned int off = (unsigned int)strtoul(key, NULL, 0);
            unsigned int len = (unsigned int)strtoul(a1, NULL, 0);
            int delta = atoi(a2);
            if (isp_control_set_xform(ISP_XFORM_OFFSET, off, len, delta, 0) != 0)
                dprintf(fd, "err offset failed\n");
            else
                dprintf(fd, "ok offset 0x%x %u %d\n", off, len, delta);
        } else if (strncmp(line, "xclear", 6) == 0 && line[6] == 0) {
            isp_control_clear_xforms();
            dprintf(fd, "ok xclear\n");
        } else if (sscanf(line, "get %63s", key) == 1) {
            const struct ctl *c = find_ctl(key);
            int out;
            int pv;
            if (!c) {
                dprintf(fd, "err unknown key %s\n", key);
            } else if (pend_last((int)(c - g_controls), &pv) == 0) {
                /* desired value (may be a few ms ahead of the apply worker) */
                dprintf(fd, "ok %s %d\n", key, pv);
            } else if (ctl_read(c, &out) != 0) {
                dprintf(fd, "err %s get failed\n", key);
            } else {
                dprintf(fd, "ok %s %d\n", key, out);
            }
        } else if (line[0] == 0) {
            /* ignore blank */
        } else {
            dprintf(fd, "err bad request\n");
        }
    }
    fclose(f);
}

static void *accept_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (!g_running)
                break;
            usleep(100000);
            continue;
        }
        {
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        handle_conn(fd);
    }
    return NULL;
}

int isp_control_start(int isp_dev)
{
    struct sockaddr_un sa;
    const char *path = getenv("MEDIAD_CTL_SOCK");
    pthread_t t;

    if (g_running)
        return 0;
    g_isp_dev = isp_dev;
    g_mask = 0;
    if (!path || !path[0])
        path = DEFAULT_SOCK;
    snprintf(g_sock, sizeof(g_sock), "%s", path);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        fprintf(stderr, "mediad: control socket() failed: %s\n", strerror(errno));
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", g_sock);
    unlink(g_sock);
    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "mediad: control bind(%s) failed: %s\n", g_sock,
                strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }
    chmod(g_sock, 0666);
    if (listen(g_listen_fd, 4) < 0) {
        fprintf(stderr, "mediad: control listen failed: %s\n", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    g_running = 1;
    if (pthread_create(&t, NULL, accept_thread, NULL) != 0) {
        fprintf(stderr, "mediad: control thread create failed\n");
        close(g_listen_fd);
        g_listen_fd = -1;
        g_running = 0;
        return -1;
    }
    g_thread = t;
    /* Auto day/night poller (idle until nightvision==2). */
    g_night_run = 1;
    pthread_create(&g_night_tid, NULL, night_poller, NULL);
    /* Coalescing apply worker. */
    g_apply_run = 1;
    pthread_create(&g_apply_tid, NULL, apply_thread, NULL);
    printf("mediad: ISP control socket at %s\n", g_sock);
    /* Restore the last day/night mode + threshold, then mediad.conf (pins win). */
    {
        FILE *f = fopen(MEDIAD_NIGHT_FILE, "r");
        char line[64];
        int v, ir = -1;

        while (f && fgets(line, sizeof(line), f)) {
            if (sscanf(line, "nightvision=%d", &v) == 1 && v >= 0 && v <= 2) {
                pend_set((int)(find_ctl("nightvision") - g_controls), v);
                fprintf(stderr, "mediad: restored nightvision=%d\n", v);
            } else if (sscanf(line, "ir_cut=%d", &v) == 1 && (v == 0 || v == 1)) {
                ir = v;
            } else if (sscanf(line, "night_lux=%d", &v) == 1 && v > 0) {
                pend_set((int)(find_ctl("night_lux") - g_controls), v);
                fprintf(stderr, "mediad: restored night_lux=%d\n", v);
            }
        }
        if (f)
            fclose(f);
        g_night_loaded = 1;
        /* Re-assert the saved day/night state: the IR LEDs survive a restart,
         * and an IR-lit scene would otherwise read as daylight. */
        if (ir == 1) {
            daynight_apply(1);
            fprintf(stderr, "mediad: restored ir_cut=1 (night)\n");
        }
    }
    /* Apply mediad.conf startup values (+ optional web UI). */
    isp_control_load_config(getenv("MEDIAD_CONF"));
    return 0;
}

void isp_control_stop(void)
{
    if (!g_running)
        return;
    g_running = 0;
    g_night_run = 0;
    g_apply_run = 0;
    pthread_cond_broadcast(&g_pend_cv);
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_join(g_thread, NULL);
    pthread_join(g_night_tid, NULL);
    pthread_join(g_apply_tid, NULL);
    unlink(g_sock);
}

/* mediad.conf (key=value, pinned; pin_key=value unpinned) and the optional
 * built-in slider page (webui=1, webui_port, default 8099). */
static int g_webui_fd = -1;
static pthread_t g_webui_tid;
static volatile int g_webui_run;

static void http_reply(int fd, const char *ctype, const char *body, int len)
{
    char hdr[128];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n", ctype, len);
    if (write(fd, hdr, (size_t)n) < 0) return;
    write(fd, body, (size_t)len);
}

static void webui_handle(int fd)
{
    char req[512];
    int n = read(fd, req, sizeof(req) - 1);
    if (n <= 0)
        return;
    req[n] = 0;

    if (strncmp(req, "GET /set?", 9) == 0) {
        char *kp = strstr(req, "k="), *vp = strstr(req, "v=");
        char key[64] = "";
        int val = 0;
        if (kp) sscanf(kp + 2, "%63[^& ]", key);
        if (vp) val = atoi(vp + 2);
        {
            const struct ctl *c = find_ctl(key);
            if (c && val >= c->vmin && val <= c->vmax)
                pend_set((int)(c - g_controls), val);
        }
        http_reply(fd, "text/plain", "ok\n", 3);
    } else if (strncmp(req, "GET / ", 6) == 0) {
        static char page[16384];
        int off = 0, i;
        off += snprintf(page + off, sizeof(page) - off,
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>mediad</title><body style='font-family:sans-serif;margin:1rem;max-width:640px'>"
            "<h3>mediad ISP controls</h3><div id=c></div><script>const C=[");
        for (i = 0; i < NCTL; i++)
            off += snprintf(page + off, sizeof(page) - off, "%s['%s',%d,%d,%d]",
                i ? "," : "", g_controls[i].key, g_controls[i].vmin,
                g_controls[i].vmax, g_controls[i].deft);
        off += snprintf(page + off, sizeof(page) - off,
            "];const root=document.getElementById('c');"
            "for(const k of C){const d=document.createElement('div');"
            "d.innerHTML=\"<label style='display:inline-block;width:9rem'>\"+k[0]+\"</label>\""
            "+'<input type=range min='+k[1]+' max='+k[2]+' value='+k[3]+' id=r_'+k[0]+'>'"
            "+' <span id=v_'+k[0]+'>'+k[3]+'</span>';root.appendChild(d);"
            "const e=d.querySelector('input');e.oninput=()=>{"
            "document.getElementById('v_'+k[0]).textContent=e.value;"
            "fetch('/set?k='+k[0]+'&v='+e.value);};}"
            "</script>");
        http_reply(fd, "text/html; charset=utf-8", page, off);
    } else {
        http_reply(fd, "text/plain", "not found\n", 10);
    }
}

static void *webui_thread(void *arg)
{
    (void)arg;
    while (g_webui_run) {
        int fd = accept(g_webui_fd, NULL, NULL);
        if (fd < 0)
            break;
        webui_handle(fd);
        close(fd);
    }
    return NULL;
}

int isp_control_start_webui(int port)
{
    struct sockaddr_in sa;
    int one = 1;
    if (port <= 0)
        port = 8099;
    g_webui_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_webui_fd < 0)
        return -1;
    setsockopt(g_webui_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(g_webui_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(g_webui_fd, 4) < 0) {
        close(g_webui_fd);
        g_webui_fd = -1;
        return -1;
    }
    g_webui_run = 1;
    pthread_create(&g_webui_tid, NULL, webui_thread, NULL);
    fprintf(stderr, "mediad: web UI at http://0.0.0.0:%d/\n", port);
    return 0;
}

int isp_control_load_config(const char *path)
{
    FILE *f;
    char line[256];
    int webui = 0, port = 8099;

    if (!path || !path[0])
        path = "/tmp/sd/unifi/etc/mediad.conf";
    snprintf(g_conf_path, sizeof(g_conf_path), "%s", path);
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *eq, *k = line, *v, *nl = strchr(line, '\n');
        char *e;
        if (nl)
            *nl = 0;
        if (line[0] == '#' || line[0] == 0)
            continue;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        if (!strcmp(k, "webui")) { webui = atoi(v); continue; }
        if (!strcmp(k, "webui_port")) { port = atoi(v); continue; }
        {
            /* `key=value` is pinned (socket sets ignored, `reset` skips it);
             * `pin_key=value` sets it without pinning. */
            const struct ctl *c;
            const char *ck = k;
            int pin = 1;
            int val = atoi(v);

            if (strncmp(ck, "pin_", 4) == 0) {
                ck = k + 4;
                pin = 0;
            }
            c = find_ctl(ck);
            if (!c) {
                fprintf(stderr, "mediad: conf: unknown key '%s'\n", ck);
            } else if (val < c->vmin || val > c->vmax) {
                fprintf(stderr, "mediad: conf: %s=%d out of range [%d,%d]\n",
                        ck, val, c->vmin, c->vmax);
            } else {
                if (pin)
                    g_controls[c - g_controls].pinned = 1;
                pend_set((int)(c - g_controls), val);
                fprintf(stderr, "mediad: conf: %s=%d%s\n", ck, val,
                        pin ? " (pinned)" : "");
            }
        }
    }
    fclose(f);
    if (webui)
        isp_control_start_webui(port);
    return 0;
}
