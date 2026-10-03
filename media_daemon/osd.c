// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* osd.c - burned-in OSD: a stats line, "<date> <time> | <name>" and a logo,
 * laid out like a UniFi camera, rendered as ARGB1555 blocks (16-aligned) and
 * re-pushed to the encoder on every control change and once a second. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "isp_control.h"   /* mediad_get_bitrate() */
#include "mediad_venc.h"
#include "osd_font.h"
#include "osd.h"
#include "framework_isp.h"   /* isp_ctx[]: AE statistics grid */

extern fwi_isp_ctx_t isp_ctx[];

#define OSD_MAX_CH 4
#define OSD_NEL 3
#define OSD_MAX_BLK 64   /* encoder overlay block limit (FWM_VENC_OVERLAY_MAX_REGIONS) */
#define OSD_MAXCOL  64   /* 16-px columns per element (INFO is 52 wide) */
/* Letter colour hysteresis: complement above HI, back below LO, after DWELL
 * consecutive samples (every OSD_SAMPLE_US). */
/* In AE-statistics units (pre-gamma): AE 80 ~ displayed luma 125, AE 92 ~ 150.
 * MEDIAD_OSD_LUMA_HI / _LO override. */
#define OSD_LUMA_HI     92
#define OSD_LUMA_LO     80
#define OSD_DWELL       2     /* consecutive 250 ms ticks */
#define OSD_TICK_NS     250000000L
enum { EL_INFO = 0, EL_STATS, EL_LOGO };

#define TEXT_SCALE_MAX 2
#define INFO_W   (52 * OSD_FONT_W * TEXT_SCALE_MAX)  /* 832: date | name */
#define STATS_W  (20 * OSD_FONT_W * TEXT_SCALE_MAX)  /* 320: bitrate */
#define TEXT_H   (OSD_FONT_H * TEXT_SCALE_MAX)       /* 32 */
#define LOGO_W   96
#define LOGO_H   48
#define PAD      16

typedef struct {
    unsigned int w, hh;
    unsigned short *buf;   /* ARGB1555, w*hh */
    unsigned short *cols;  /* same pixels re-laid as contiguous 16-px columns */
    /* Per 16-px column (letter): current colour choice and hysteresis state. */
    unsigned char ink[OSD_MAXCOL];    /* column has visible pixels (last render) */
    unsigned char dark[OSD_MAXCOL];   /* 1 = drawn in the complement colour */
    unsigned char votes[OSD_MAXCOL];  /* consecutive samples asking to switch */
    int x, y;              /* 16-aligned position in the frame */
    int show;
} osd_el;

typedef struct {
    struct mediad_venc *venc;
    int venc_chn, fw, fh;
    int max_scale;   /* 2 on >=720p, 1 on the low-res stream (LOW 640x360) */
    int shown;       /* blocks currently pushed to the encoder */
    osd_el el[OSD_NEL];
} osd_chan;

static osd_chan g_ch[OSD_MAX_CH];
static int g_nch;
static int g_ctl[OSD_NCTL];
static char g_name[64];
static pthread_t g_tid;
static volatile int g_run;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_hwinv;   /* MEDIAD_OSD_HWINV=1: hardware per-frame inversion instead */
static int g_luma_hi = OSD_LUMA_HI, g_luma_lo = OSD_LUMA_LO, g_trace;

/* ARGB1555 complement colour for visible pixels (alpha kept). */
static unsigned short ink_invert(unsigned short p)
{
    return (p & 0x8000) ? (unsigned short)((~p & 0x7fff) | 0x8000) : p;
}

/* ARGB1555: bit15 = alpha (1 = opaque), R<<10, G<<5, B. */
static unsigned short color_for(int id)
{
    static const unsigned char pal[][3] = {
        { 31, 31, 31 }, { 31, 31, 0 }, { 31, 0, 0 },
        { 0, 31, 0 },  { 0, 31, 31 },  { 31, 0, 31 },
    };
    id %= (int)(sizeof(pal) / sizeof(pal[0]));
    return (unsigned short)(0x8000u | (pal[id][0] << 10) | (pal[id][1] << 5) | pal[id][2]);
}

static void px(unsigned short *b, unsigned w, unsigned hh, int x, int y, unsigned short c)
{
    if (x < 0 || y < 0 || (unsigned)x >= w || (unsigned)y >= hh)
        return;
    b[(unsigned)y * w + (unsigned)x] = c;
}

static void draw_char(unsigned short *b, unsigned w, unsigned hh, int x, int y,
                      int scale, unsigned short c, unsigned char ch)
{
    const unsigned char *g;
    int r, col, sy, sx;

    if (ch < OSD_FONT_FIRST || ch > OSD_FONT_LAST)
        ch = '?';
    if (scale == 2) {
        /* Native 16x32 glyph (same cell as 2x the base font, drawn unscaled). */
        const unsigned short *g2 = osd_font2x[ch - OSD_FONT_FIRST];

        for (r = 0; r < 2 * OSD_FONT_H; r++)
            for (col = 0; col < 2 * OSD_FONT_W; col++)
                if (g2[r] & (0x8000 >> col))
                    px(b, w, hh, x + col, y + r, c);
        return;
    }
    g = osd_font[ch - OSD_FONT_FIRST];
    for (r = 0; r < OSD_FONT_H; r++)
        for (col = 0; col < OSD_FONT_W; col++)
            if (g[r] & (0x80 >> col))
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++)
                        px(b, w, hh, x + col * scale + sx, y + r * scale + sy, c);
}

static void draw_str(unsigned short *b, unsigned w, unsigned hh, int x, int y,
                     int scale, unsigned short c, const char *s)
{
    for (; *s; s++, x += OSD_FONT_W * scale)
        draw_char(b, w, hh, x, y, scale, c, (unsigned char)*s);
}

/* Placeholder logo: an outlined box with "CAM" (unshippable UniFi logo). */
static void draw_logo(osd_el *e, unsigned short c, int scale)
{
    int i, tw = 3 * OSD_FONT_W * scale, x0 = ((int)e->w - tw) / 2;
    int y0 = ((int)e->hh - OSD_FONT_H * scale) / 2;

    for (i = 0; i < (int)e->w; i++) {
        px(e->buf, e->w, e->hh, i, 0, c);
        px(e->buf, e->w, e->hh, i, (int)e->hh - 1, c);
    }
    for (i = 0; i < (int)e->hh; i++) {
        px(e->buf, e->w, e->hh, 0, i, c);
        px(e->buf, e->w, e->hh, (int)e->w - 1, i, c);
    }
    draw_str(e->buf, e->w, e->hh, x0 > 0 ? x0 : 0, y0 > 0 ? y0 : 0, scale, c, "CAM");
}

static int el_alloc(osd_el *e, unsigned w, unsigned hh)
{
    e->buf = calloc((size_t)w * hh, 2);
    e->cols = calloc((size_t)w * hh, 2);
    if (!e->buf || !e->cols)
        return -1;
    e->w = w;
    e->hh = hh;
    e->show = 0;
    return 0;
}

static int text_scale(void)
{
    return g_ctl[OSD_TEXT_SCALE] >= 50 ? 2 : 1;
}

/* Per-channel scale, capped by the region size chosen at start (the LOW stream
 * is 640x360, where scale 2 would be huge). */
static int chan_scale(osd_chan *ch)
{
    int s = text_scale();
    return s > ch->max_scale ? ch->max_scale : s;
}

static void render_info(osd_chan *ch, int x, int y, int show)
{
    osd_el *e = &ch->el[EL_INFO];
    time_t t;
    struct tm tm;
    char s[96];
    int n = 0;

    e->x = x;
    e->y = y;
    e->show = show;
    if (!show)
        return;
    memset(e->buf, 0, (size_t)e->w * e->hh * 2);
    if (g_ctl[OSD_DATE]) {
        t = time(NULL);
        localtime_r(&t, &tm);
        n += (int)strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &tm);
    }
    if (g_ctl[OSD_NAME]) {
        if (n)
            n += snprintf(s + n, sizeof(s) - n, " | ");
        snprintf(s + n, sizeof(s) - n, "%s", g_name);
    }
    draw_str(e->buf, e->w, e->hh, 0, 0, chan_scale(ch), color_for(g_ctl[OSD_COLOR]), s);
}

static void render_stats(osd_chan *ch, int x, int y, int show)
{
    osd_el *e = &ch->el[EL_STATS];
    char s[32];

    e->x = x;
    e->y = y;
    e->show = show;
    if (!show)
        return;
    memset(e->buf, 0, (size_t)e->w * e->hh * 2);
    {
        unsigned kbps = mediad_get_bitrate(ch->venc_chn == 0 ? "high" : "low") / 1000;
        snprintf(s, sizeof(s), "HQ %u kbps", kbps);
        draw_str(e->buf, e->w, e->hh, 0, 0, chan_scale(ch), color_for(g_ctl[OSD_COLOR]), s);
    }
}

static void render_logo(osd_chan *ch, int x, int y, int show)
{
    osd_el *e = &ch->el[EL_LOGO];

    e->x = x;
    e->y = y;
    e->show = show;
    if (!show)
        return;
    memset(e->buf, 0, (size_t)e->w * e->hh * 2);
    {
        int s = g_ctl[OSD_LOGO_SCALE] >= 50 ? 2 : 1;
        if (s > ch->max_scale)
            s = ch->max_scale;
        draw_logo(e, color_for(g_ctl[OSD_COLOR]), s);
    }
}

/* Fill one encoder overlay block from an element. Sizes/positions are
 * 16-aligned, so the macroblock range is exact. Returns 1 if shown. */
static int el_block(const osd_el *e, struct mediad_venc_ovl_blk *b)
{
    if (!e->show || !e->buf || e->w < 16 || e->hh < 16)
        return 0;
    b->start_mb_x = (unsigned short)(e->x / 16);
    b->start_mb_y = (unsigned short)(e->y / 16);
    b->end_mb_x = (unsigned short)(e->x / 16 + (int)e->w / 16 - 1);
    b->end_mb_y = (unsigned short)(e->y / 16 + (int)e->hh / 16 - 1);
    b->bits = e->buf;
    b->bytes = e->w * e->hh * 2;
    if (!g_hwinv && e->dark[0] && e->cols) {   /* whole element is one decision */
        size_t i, np = (size_t)e->w * e->hh;
        for (i = 0; i < np; i++)
            e->cols[i] = ink_invert(e->buf[i]);
        b->bits = e->cols;
    }
    ((osd_el *)e)->ink[0] = 1;
    b->hw_invert = (unsigned char)g_hwinv;
    /* Invert unit = one 16-px glyph column x the text height, so each letter
     * flips as a whole (hardware limit: 4 MB per side). */
    b->unit_w_minus1 = 0;
    b->unit_h_minus1 = (unsigned char)(e->hh / 16 - 1 > 3 ? 3 : e->hh / 16 - 1);
    return 1;
}

/* One overlay block per 16-px text column, so each letter's colour follows its
 * own background; fully transparent columns are skipped. Returns blocks written. */
static int blk_cmp(const void *a, const void *b)
{
    const struct mediad_venc_ovl_blk *x = a, *y = b;

    if (x->start_mb_y != y->start_mb_y)
        return (int)x->start_mb_y - (int)y->start_mb_y;
    return (int)x->start_mb_x - (int)y->start_mb_x;
}

static int el_col_blocks(const osd_el *e, struct mediad_venc_ovl_blk *b, int room)
{
    unsigned int k, r, c, ncol;
    int n = 0;

    if (!e->show || !e->buf || !e->cols || e->w < 16 || e->hh < 16)
        return 0;
    ncol = e->w / 16;
    for (k = 0; k < ncol && n < room; k++) {
        unsigned short *dst = e->cols + (size_t)k * 16 * e->hh;
        int opaque = 0;

        for (r = 0; r < e->hh; r++)
            for (c = 0; c < 16; c++) {
                unsigned short p = e->buf[(size_t)r * e->w + k * 16 + c];
                opaque |= (p & 0x8000) != 0;
                dst[r * 16 + c] = (!g_hwinv && k < OSD_MAXCOL && e->dark[k]) ? ink_invert(p) : p;
            }
        if (k < OSD_MAXCOL)
            ((osd_el *)e)->ink[k] = (unsigned char)opaque;
        if (!opaque)
            continue;
        b[n].start_mb_x = b[n].end_mb_x = (unsigned short)(e->x / 16 + (int)k);
        b[n].start_mb_y = (unsigned short)(e->y / 16);
        b[n].end_mb_y = (unsigned short)(e->y / 16 + (int)e->hh / 16 - 1);
        b[n].bits = dst;
        b[n].bytes = 16 * e->hh * 2;
        b[n].unit_w_minus1 = 0;
        b[n].unit_h_minus1 = (unsigned char)(e->hh / 16 - 1 > 3 ? 3 : e->hh / 16 - 1);
        b[n].hw_invert = (unsigned char)g_hwinv;
        n++;
    }
    return n;
}

static void render_chan(osd_chan *ch)
{
    int pos = ((g_ctl[OSD_POS] % 4) + 4) % 4;
    int top = (pos == 0 || pos == 1), left = (pos == 0 || pos == 2);
    int info_on = g_ctl[OSD_ENABLE] && (g_ctl[OSD_DATE] || g_ctl[OSD_NAME]);
    int stats_on = g_ctl[OSD_ENABLE] && g_ctl[OSD_BITRATE];
    int logo_on = g_ctl[OSD_ENABLE] && g_ctl[OSD_LOGO];
    int nlines = (info_on ? 1 : 0) + (stats_on ? 1 : 0);
    int th = ch->el[EL_INFO].hh;      /* per-channel (16 on LOW, 32 otherwise) */
    int iw = (int)ch->el[EL_INFO].w, sw = (int)ch->el[EL_STATS].w;
    int lw = (int)ch->el[EL_LOGO].w, lh = (int)ch->el[EL_LOGO].hh;
    int x_info = left ? PAD : ch->fw - iw - PAD;
    int x_stats = left ? PAD : ch->fw - sw - PAD;
    int y = top ? PAD : ch->fh - nlines * th - PAD;
    struct mediad_venc_ovl_blk blk[OSD_MAX_BLK];
    int n = 0;

    if (stats_on) {
        render_stats(ch, x_stats & ~15, y & ~15, 1);
        y += th;
    } else {
        render_stats(ch, x_stats & ~15, y & ~15, 0);
    }
    render_info(ch, x_info & ~15, y & ~15, info_on);
    /* The logo is always anchored bottom-right, per Protect. */
    render_logo(ch, (ch->fw - lw - PAD) & ~15, (ch->fh - lh - PAD) & ~15, logo_on);

    /* Logo first so it always fits; text split per glyph column after it. */
    n += el_block(&ch->el[EL_LOGO], &blk[n]);
    n += el_col_blocks(&ch->el[EL_INFO], &blk[n], OSD_MAX_BLK - n);
    n += el_col_blocks(&ch->el[EL_STATS], &blk[n], OSD_MAX_BLK - n);
    /* Raster order (row, then column): the hardware walks the list as it
     * encodes macroblock rows. */
    qsort(blk, (size_t)n, sizeof(blk[0]), blk_cmp);

    /* Nothing shown and nothing was shown: skip the encoder call entirely. */
    if (n == 0 && ch->shown == 0)
        return;
    if (ch->venc == NULL)
        return;
    if (mediad_venc_set_overlay(ch->venc, blk, n) == 0)
        ch->shown = n;
}

static void render_all(void)
{
    int i;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_nch; i++)
        render_chan(&g_ch[i]);
    pthread_mutex_unlock(&g_lock);
}

void osd_refresh(void)
{
    if (g_nch > 0)
        render_all();
}

static void load_default_name(void);

/* Letter colour from the background: the capture is LBC-compressed, so luma is
 * interpolated from the AE statistics' 24x16 grid at each letter's centre. */
#define AE_GX 24
#define AE_GY 16

static int grid_at(const unsigned int *g, double u, double v)
{
    double gx = u * AE_GX - 0.5, gy = v * AE_GY - 0.5, fx, fy, a, b;
    int x0, y0, x1, y1;

    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx > AE_GX - 1) gx = AE_GX - 1;
    if (gy > AE_GY - 1) gy = AE_GY - 1;
    x0 = (int)gx; y0 = (int)gy;
    x1 = x0 + 1 < AE_GX ? x0 + 1 : x0;
    y1 = y0 + 1 < AE_GY ? y0 + 1 : y0;
    fx = gx - x0; fy = gy - y0;
    a = g[y0 * AE_GX + x0] * (1 - fx) + g[y0 * AE_GX + x1] * fx;
    b = g[y1 * AE_GX + x0] * (1 - fx) + g[y1 * AE_GX + x1] * fx;
    return (int)(a * (1 - fy) + b * fy + 0.5);
}

static int decide(osd_el *e, int k, int luma)
{
    int want = e->dark[k] ? (luma > g_luma_lo) : (luma > g_luma_hi);

    if (want == e->dark[k]) {
        e->votes[k] = 0;
        return 0;
    }
    if (++e->votes[k] < OSD_DWELL)
        return 0;
    e->dark[k] = (unsigned char)want;
    e->votes[k] = 0;
    return 1;
}

/* Returns 1 if any letter changed colour. */
static int sample_background(void)
{
    unsigned int g[AE_GX * AE_GY];
    int c, el, k, changed = 0;
    static int trace_n;

    if (g_hwinv)
        return 0;
    memcpy(g, isp_ctx[0].stats.stats.ae_stats.average, sizeof(g));
    if (g_trace && (trace_n++ % 16) == 0) {
        char line[AE_GX * 4 + 1];
        int x, y;

        for (y = 0; y < AE_GY; y++) {
            for (x = 0; x < AE_GX; x++)
                snprintf(line + x * 4, 5, "%4u", g[y * AE_GX + x] > 999 ? 999 : g[y * AE_GX + x]);
            fprintf(stderr, "osd_ae[%02d]%s\n", y, line);
        }
    }
    pthread_mutex_lock(&g_lock);
    for (c = 0; c < g_nch; c++) {
        osd_chan *ch = &g_ch[c];

        if (ch->fw <= 0 || ch->fh <= 0)
            continue;
        for (el = 0; el < OSD_NEL; el++) {
            osd_el *e = &ch->el[el];
            double v = (e->y + e->hh / 2.0) / ch->fh;

            if (!e->show || !e->buf)
                continue;
            if (el == EL_LOGO) {
                changed |= decide(e, 0, grid_at(g, (e->x + e->w / 2.0) / ch->fw, v));
                continue;
            }
            for (k = 0; k < (int)(e->w / 16) && k < OSD_MAXCOL; k++)
                if (e->ink[k])
                    changed |= decide(e, k, grid_at(g, (e->x + 16 * k + 8.0) / ch->fw, v));
        }
    }
    pthread_mutex_unlock(&g_lock);
    return changed;
}

static void *osd_thread(void *arg)
{
    (void)arg;

    int tick = 0;

    while (g_run) {
        struct timespec ts = { 0, OSD_TICK_NS };
        int changed;

        nanosleep(&ts, NULL);
        if (!g_run)
            break;
        changed = sample_background();   /* letter colours, ~4 Hz */
        if (++tick < 4 && !changed)
            continue;
        tick = 0;
        /* Pick up a rename the client persisted (ChangeDeviceSettings). */
        pthread_mutex_lock(&g_lock);
        load_default_name();
        pthread_mutex_unlock(&g_lock);
        render_all();   /* clock every 1 s; name/bitrate/layout re-rendered too */
    }
    return NULL;
}

static void load_default_name(void)
{
    const char *e = getenv("MEDIAD_OSD_NAME");
    const char *files[] = {
        "/tmp/sd/yi-protect/etc/yi_protect_client_go.device-name",
        "/tmp/sd/yi-protect/etc/model_suffix",
    };
    size_t i;

    g_name[0] = 0;
    if (e && e[0]) {
        snprintf(g_name, sizeof(g_name), "%s", e);
        return;
    }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *f = fopen(files[i], "r");
        char *q;
        if (!f)
            continue;
        if (fgets(g_name, sizeof(g_name), f)) {
            fclose(f);
            q = g_name + strlen(g_name);
            while (q > g_name && (q[-1] == '\n' || q[-1] == '\r'))
                *--q = 0;
            return;
        }
        fclose(f);
    }
    snprintf(g_name, sizeof(g_name), "Camera");
}

int osd_start(const osd_chan_cfg *chans, int nchan, const char *camera_name)
{
    int i;
    const char *e;
    const char *hw = getenv("MEDIAD_OSD_HWINV");

    if (nchan <= 0 || nchan > OSD_MAX_CH)
        return -1;
    g_hwinv = (hw && hw[0] == '1');
    if ((e = getenv("MEDIAD_OSD_LUMA_HI")) && e[0])
        g_luma_hi = atoi(e);
    if ((e = getenv("MEDIAD_OSD_LUMA_LO")) && e[0])
        g_luma_lo = atoi(e);
    g_trace = ((e = getenv("MEDIAD_OSD_TRACE")) && e[0] == '1');

    memset(g_ctl, 0, sizeof(g_ctl));
    g_ctl[OSD_ENABLE] = 1;
    g_ctl[OSD_DATE] = 1;
    g_ctl[OSD_NAME] = 1;
    g_ctl[OSD_LOGO] = 1;
    g_ctl[OSD_BITRATE] = 1;
    g_ctl[OSD_TEXT_SCALE] = 50;
    g_ctl[OSD_LOGO_SCALE] = 50;
    g_ctl[OSD_COLOR] = 0;
    g_ctl[OSD_POS] = 0;
    if ((e = getenv("MEDIAD_OSD_ENABLE"))) g_ctl[OSD_ENABLE] = atoi(e);
    if ((e = getenv("MEDIAD_OSD_DATE")))   g_ctl[OSD_DATE] = atoi(e);

    if (camera_name && camera_name[0])
        snprintf(g_name, sizeof(g_name), "%s", camera_name);
    else
        load_default_name();

    g_nch = 0;
    for (i = 0; i < nchan; i++) {
        osd_chan *c = &g_ch[i];
        unsigned iw, sw, th, lw, lh;

        memset(c, 0, sizeof(*c));
        c->venc = chans[i].venc;
        c->venc_chn = chans[i].venc_chn;
        c->fw = chans[i].frame_w;
        c->fh = chans[i].frame_h;
        c->max_scale = (c->fh >= 720) ? TEXT_SCALE_MAX : 1;
        iw = INFO_W  / TEXT_SCALE_MAX * c->max_scale;
        sw = STATS_W / TEXT_SCALE_MAX * c->max_scale;
        th = TEXT_H  / TEXT_SCALE_MAX * c->max_scale;
        lw = LOGO_W  / TEXT_SCALE_MAX * c->max_scale;
        lh = (c->max_scale >= 2) ? LOGO_H : 32;   /* 16-aligned */
        if (el_alloc(&c->el[EL_INFO], iw, th) != 0 ||
            el_alloc(&c->el[EL_STATS], sw, th) != 0 ||
            el_alloc(&c->el[EL_LOGO], lw, lh) != 0) {
            fprintf(stderr, "osd: element buffer alloc failed\n");
            return -1;
        }
        g_nch++;
    }

    render_all();
    fprintf(stderr, "osd: started on %d channel(s), name=%s\n", g_nch, g_name);

    g_run = 1;
    if (pthread_create(&g_tid, NULL, osd_thread, NULL) != 0) {
        fprintf(stderr, "osd: thread create failed\n");
        g_run = 0;
        return -1;
    }
    return 0;
}

/* Hold the render lock across a codec switch (render_all takes the same lock
 * while it uses a channel's venc), then rebind the channel to the new encoder. */
void osd_quiesce_venc(int chn)
{
    (void)chn;
    pthread_mutex_lock(&g_lock);
}

void osd_rebind_venc(int chn, struct mediad_venc *venc)
{
    if (chn >= 0 && chn < g_nch) {
        g_ch[chn].venc = venc;
        g_ch[chn].shown = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void osd_stop(void)
{
    int i, j;

    if (g_run) {
        g_run = 0;
        pthread_join(g_tid, NULL);
    }
    for (i = 0; i < g_nch; i++) {
        if (g_ch[i].venc && g_ch[i].shown > 0)
            mediad_venc_set_overlay(g_ch[i].venc, NULL, 0);
        for (j = 0; j < OSD_NEL; j++) {
            free(g_ch[i].el[j].buf);
            free(g_ch[i].el[j].cols);
            g_ch[i].el[j].buf = NULL;
            g_ch[i].el[j].cols = NULL;
        }
    }
    g_nch = 0;
}

int osd_set(int key, int value)
{
    if (key < 0 || key >= OSD_NCTL)
        return -1;
    g_ctl[key] = value;
    osd_refresh();
    return 0;
}

int osd_get(int key)
{
    if (key < 0 || key >= OSD_NCTL)
        return -1;
    return g_ctl[key];
}

