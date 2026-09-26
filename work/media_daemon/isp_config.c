// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * isp_config.c - our from-scratch ISP tuning + controls for mediad.
 *
 * No vendor data and no other sensor's tuning: we start from the libisp's own
 * initializer (ae/awb/wb/hist) and layer only our generated tables/controls on
 * top, through the libisp's own path (parser_ini_info -> isp_tuning_init ->
 * isp_ctx_config_init -> __isp_ctx_cfg_mod -> prebuilt packers).
 *
 * Validated on y623: CCM (saturation/hue) works reliably; gamma and the tone
 * modules (pltm/drc/cem) need a real tuning and break the stream when enabled
 * without one; brightness/denoise depend on algorithm data we do not have.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <stdint.h>
#include <stdlib.h>

#include "isp_config.h"   /* fwi_* types + HW_ISP_CFG_* via framework_isp.h */

/* (The V536 520 tree's generic `isp_default_ini_4v5.h` is not present in the
 * sun8iw19p1 521 tree; we no longer need it - the 3A algorithm libs default
 * themselves and we fill our own tables.) */

#define GAMMA_POINTS 1024
#define ISP_DEV 0

extern fwi_isp_ctx_t isp_ctx[];

static int g_saturation = 50;
static int g_daynight;         /* 1 = night (IR): CCM forced monochrome */
static int g_hue = 50;
static int g_sharpness = 5;
static int g_brightness = 50;
static int g_contrast = 50;
static int g_denoise = 0;   /* 0 = denoise/3DNR off (stock ghosts on motion) */
static int g_tdf_want = 0;  /* configured tdf (see isp_config_want_tdf) */
static int g_nr2d_want = 1; /* spatial (2D) denoise; 1 == the tuning's own */
static int g_cnr_want = 1;  /* chroma denoise; 1 == the tuning's own */
static int g_exposure = 50;
static int g_gamma = 50;   /* 0 = darkest, 50 = neutral (exp 0.85), 100 = light */

/* Gamma slider exponent: 50 == the imported vendor curve unchanged (exp 1.0);
 * up brightens (lower exponent), down darkens. */
static double gamma_exp(int v)
{
    return pow(2.0, (50.0 - v) / 100.0);
}

/* Generate our 5-LV gamma LUT for the given exponent. */
static void fill_gamma_tbl(fwi_tuning_modules_t *t, double exp)
{
    int tbl, ch, i;

    for (tbl = 0; tbl < 5; tbl++)
        for (ch = 0; ch < 3; ch++)
            for (i = 0; i < GAMMA_POINTS; i++) {
                double x = (double)i / (GAMMA_POINTS - 1);
                double v = 4095.0 * pow(x, exp);
                t->gamma_tbl_init[tbl][ch * GAMMA_POINTS + i] = (uint16_t)(v + 0.5);
            }
}

static int cal_on(const char *name, int def)
{
    FILE *f = fopen("/tmp/sd/isp_cal.cfg", "r");
    char line[128];
    int v = def;

    if (!f)
        return def;
    while (fgets(line, sizeof(line), f)) {
        char k[64];
        int val;
        if (sscanf(line, "%63[^= \n]=%d", k, &val) == 2 && strcmp(k, name) == 0) {
            v = val;
            break;
        }
    }
    fclose(f);
    return v;
}

static fwi_tuning_modules_t *tunning(void)
{
    return &isp_ctx[ISP_DEV].tuning.modules;
}

/* Per-frame module_cfg edits (packers for these are real functions). Called
 * from isp_manage.c right before isp_hardware_update(). NOTE: this is the one
 * control still done at register/module_cfg level; denoise's SDK equivalent is
 * HW_ISP_CFG_DYNAMIC_DENOISE/TDF (per-LV value arrays), which needs its ramp
 * re-expressed before it can replace this. */
void isp_control_hook(fwi_hw_module_cfg_t *cfg)
{
    int i;

    /* The per-frame enables must agree with the denoise switches, which
     * stock_reg.c also enforces on the load-reg word: two writers disagreeing
     * flipped a module between/within frames (dark horizontal bands, r35gb
     * 2026-09-26, denoise=50 from Protect with nr2d/cnr off). */
    cfg->module_enable_flag = g_nr2d_want ? (cfg->module_enable_flag | FWI_ISP_FEATURES_D2D)
                                          : (cfg->module_enable_flag & ~FWI_ISP_FEATURES_D2D);
    cfg->module_enable_flag = g_cnr_want ? (cfg->module_enable_flag | FWI_ISP_FEATURES_CHROMA_DENOISE)
                                         : (cfg->module_enable_flag & ~FWI_ISP_FEATURES_CHROMA_DENOISE);
    if (!g_tdf_want)
        cfg->module_enable_flag &= ~FWI_ISP_FEATURES_D3D;
    if (g_denoise > 0) {
        /* strength ramp: thresholds only; which modules run is the switches' call */
        for (i = 0; i < ISP_REG_TBL_LENGTH; i++) {
            int th = (8 + i * 3) * g_denoise / 25;
            cfg->bayer_denoise_cfg.bayer_denoise_threshold[i] = (uint16_t)th;
            cfg->denoise_3d_cfg.temporal_denoise_threshold[i] = (uint16_t)th;
            cfg->denoise_3d_cfg.temporal_denoise_ref_noise[i] = (uint16_t)(th / 2);
        }
        for (i = 0; i < ISP_REG_TBL_LENGTH - 1; i++)
            cfg->denoise_3d_cfg.temporal_denoise_k[i] = 31;
    }
}

/* Combined saturation + hue as one RGB colour matrix, Q8 (rows sum to 256). */
static void set_ccm(fwi_rgb2rgb_gain_offset_t *out, int sat_q8, int hue_deg)
{
    const double kr = 0.299, kg = 0.587, kb = 0.114;
    double sat = sat_q8 / 256.0;
    double th = hue_deg * 3.14159265358979 / 180.0;
    double cc = cos(th), ss = sin(th), vv = 1.0 - cc;
    double rt3 = 1.73205080756888;
    double R[3][3] = {
        { cc + vv / 3, vv / 3 - ss / rt3, vv / 3 + ss / rt3 },
        { vv / 3 + ss / rt3, cc + vv / 3, vv / 3 - ss / rt3 },
        { vv / 3 - ss / rt3, vv / 3 + ss / rt3, cc + vv / 3 },
    };
    double S[3][3];
    int r, c, i;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) {
            double k = (c == 0) ? kr : (c == 1) ? kg : kb;
            S[r][c] = (r == c ? sat : 0.0) + (1.0 - sat) * k;
        }

    for (i = 0; i < 3; i++) {
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                double m = R[r][0] * S[0][c] + R[r][1] * S[1][c] +
                           R[r][2] * S[2][c];
                out[i].matrix[r][c] = (int16_t)(int)(m * 256 + 0.5);
            }
            out[i].offset[r] = 0;
        }
    }
}

/* Runtime control appliers: isp_set_cfg + isp_update, the same calls the
 * AW_MPI_ISP_Set* wrappers make; nothing pokes isp_ini_cfg/module_cfg. */

static void apply_ccm(void)
{
    static const uint16_t trig[3] = { 2800, 4000, 6500 };
    fwi_ccm_arg_t cfg[3];
    fwi_rgb2rgb_gain_offset_t m[3];
    int i;

    /* Night (IR) is monochrome: force saturation 0 regardless of the user
     * control, so a client re-sending saturation cannot recolour the IR image. */
    set_ccm(m, (g_daynight ? 0 : g_saturation) * 2 * 256 / 100,
            (g_hue - 50) * 36 / 10);
    for (i = 0; i < 3; i++) {
        cfg[i].temperature = trig[i];
        cfg[i].value = m[i];
    }
    isp_set_cfg(ISP_DEV, HW_ISP_CFG_TUNING,
                HW_ISP_CFG_TUNING_CCM_LOW | HW_ISP_CFG_TUNING_CCM_MID |
                HW_ISP_CFG_TUNING_CCM_HIGH, cfg);
}

static void apply_sharp(void)
{
    fwi_sharp_table_arg_t s;
    int i;

    memset(&s, 0, sizeof(s));
    for (i = 0; i < ISP_REG_TBL_LENGTH; i++) {
        int base = (i == 0) ? 0 : 66 + i * 6;
        int lum = i * 8;
        if (base > 256)
            base = 256;
        if (lum > 256)
            lum = 256;
        s.value[i] = (uint16_t)(base * g_sharpness / 5);
        s.lum[i] = (uint16_t)(lum * g_sharpness / 5);
    }
    isp_set_cfg(ISP_DEV, HW_ISP_CFG_TUNING_TABLES, HW_ISP_CFG_TUNING_SHARP, &s);
}

// NOTE (2026-09-14): the dynamic-saturation cfg (apply_sat) was REMOVED. It
// writes HW_ISP_CFG_DYNAMIC_SATURATION with cb/cr = (sat-50)*4, and at neutral
// that lands as a zeroed matrix -> the ISP logs
// "isp_config_saturation: Saturation SUM != 16, sum = 0" on every frame.
// Saturation is handled by the CCM (apply_ccm) instead.

static int apply_all(void)
{
    /* Live per-frame fields: plain struct writes the algorithms read each
     * frame (isp_manage.c:318 GTM reads adjust.brightness/contrast;
     * isp_manage.c:265 AE copies ae_settings). No ISP reconfiguration, so this
     * is always safe. */
    isp_ctx[ISP_DEV].adjust_ctl.brightness = (g_brightness - 50) * 4;
    isp_ctx[ISP_DEV].adjust_ctl.contrast = (g_contrast - 50) * 4;
    isp_ctx[ISP_DEV].ae_ctl.exposure_compensation = (g_exposure - 50) * 4;

    /* The isp_set_cfg() + isp_update() + FULL isp_ctx_config_init() path is
     * what applies the CCM/sharp tables (and the day/night tuning swap), but on
     * this RTOS/vendor-WDR build it corrupts the running ISP+VI pipeline - it
     * zeros the saturation matrix (flooding isp_config_saturation) and wedges
     * the sensor/board (only a power cycle recovers). It is therefore OFF by
     * default; set MEDIAD_ISP_APPLY=1 to exercise it for experiments. */
    if (getenv("MEDIAD_ISP_APPLY")) {
        apply_ccm();
        apply_sharp();
        isp_update(ISP_DEV);
        isp_ctx_config_init(&isp_ctx[ISP_DEV]);
        /* the re-init resets adjust, so re-assert the live fields after it */
        isp_ctx[ISP_DEV].adjust_ctl.brightness = (g_brightness - 50) * 4;
        isp_ctx[ISP_DEV].adjust_ctl.contrast = (g_contrast - 50) * 4;
        isp_ctx[ISP_DEV].ae_ctl.exposure_compensation = (g_exposure - 50) * 4;
    }
    return 0;
}

void isp_config_fill_param(fwi_tuning_image_t *param)
{
    fwi_tuning_enables_t *test = &param->enables;
    fwi_tuning_modules_t *t = &param->modules;
    int i;

    /* AE highlight (blowout) prevention: the open-SDK tuning enables this
     * (ae_blowout_pre_en=1, attr=30). Without it the AE exposes the scene's
     * direct lights into clip and they render as the blue/black "highlight
     * alert" zones. We only fill test/tunning settings from scratch, so the 3a
     * block (where these live) must be set here. */
    param->a3.ae_highlight_guard_en = cal_on("blowout", 1);
    param->a3.ae_highlight_guard_level = cal_on("blowout_attr", 30);

    /* AE weighting grid. Every SDK sensor config sets this; the from-scratch
     * build left it all-zero, which makes the AE's window-weight sum zero and
     * `get_ae_avg_lum_q8` divide by zero (SIGFPE) as soon as the table-based AE
     * path runs - which is exactly what commanding-WDR (`wdr_mode=2`) selects.
     * The 8x8 centre-weighted pattern below is the SDK's generic one. */
    {
        static const int32_t win[64] = {
            1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 2, 2, 1, 1, 1,
            1, 1, 1, 2, 2, 1, 1, 1,
            1, 1, 2, 2, 2, 2, 1, 1,
            1, 2, 2, 2, 2, 2, 2, 1,
        };
        memcpy(param->a3.ae_zone_weight, win, sizeof(win));
    }

    /* Our generated gamma curve, exponent controlled by the `gamma` runtime
     * control. A strong darkening curve (>1) makes the AE ramp gain and blooms
     * direct lights; 0.85 renders clean on y623. See other.md. */
    fill_gamma_tbl(t, gamma_exp(g_gamma));
    t->gamma_trig_cfg[0] = 1300;
    t->gamma_trig_cfg[1] = 1100;
    t->gamma_trig_cfg[2] = 900;
    t->gamma_trig_cfg[3] = 600;
    t->gamma_trig_cfg[4] = 300;
    t->gamma_count = 5;
    t->gamma_type = 1;

    set_ccm(t->colour_matrix_init, g_saturation * 2 * 256 / 100,
            (g_hue - 50) * 36 / 10);
    t->colour_matrix_trigger[0] = 2800;
    t->colour_matrix_trigger[1] = 4000;
    t->colour_matrix_trigger[2] = 6500;

    for (i = 0; i < ISP_REG_TBL_LENGTH; i++) {
        int v = (i == 0) ? 0 : 66 + i * 6;
        t->sharp_val[i] = (uint16_t)(v > 256 ? 256 : v);
        t->sharp_luminance[i] = (uint16_t)((i * 8) > 256 ? 256 : i * 8);
    }

    /* Gamma stays default-OFF: the 520 libisp gamma stage is incompatible with
     * this V536/521 pipeline (black highlight holes with any table, incl. the
     * SDK's own 4v5 table). See other.md. `gamma=1` enables it for testing. */
    test->gamma_en = cal_on("gamma", 0);
    test->colour_matrix_en = cal_on("cm", 1);
    test->sharpen_en = cal_on("sharp", 1);
    test->lens_shading_en = cal_on("lsc", 0);
    test->global_tone_en = cal_on("gtm", 1);
    test->chroma_denoise_en = cal_on("cnr", 1);
    /* 3DNR (tdf) and temporal denoise default OFF: stock's temporal denoise
     * leaves a ghosting "arm shadow" on motion. Both are overridable
     * (MEDIAD_CONF tdf=/denoise= or Protect enable3dnr). */
    test->denoise_3d_en = cal_on("tdf", 0);
    g_tdf_want = test->denoise_3d_en;
    test->denoise_2d_en = cal_on("denoise", 0);
    test->drc_en = cal_on("drc", 0);
    test->colour_enhance_en = cal_on("cem", 0);
    test->local_tone_en = cal_on("pltm", 0);
    test->saturation_en = cal_on("satur", 0);

    fprintf(stderr, "isp_config: ours (gamma=%d cm=%d sharp=%d lsc=%d gtm=%d "
            "cnr=%d tdf=%d denoise=%d drc=%d cem=%d pltm=%d satur=%d)\n",
            test->gamma_en, test->colour_matrix_en, test->sharpen_en, test->lens_shading_en,
            test->global_tone_en, test->chroma_denoise_en, test->denoise_3d_en, test->denoise_2d_en,
            test->drc_en, test->colour_enhance_en, test->local_tone_en, test->saturation_en);
}

/* ---- runtime controls ----------------------------------------------------- */

int isp_config_set_saturation(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_saturation = v;
    return apply_all();
}

int isp_config_set_hue(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_hue = v;
    return apply_all();
}

int isp_config_set_sharpness(int v)
{
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    g_sharpness = v;
    return apply_all();
}

int isp_config_set_brightness(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_brightness = v;
    return apply_all();
}

int isp_config_set_contrast(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_contrast = v;
    return apply_all();
}

int isp_config_set_denoise(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_denoise = v;
    return apply_all();
}

int isp_config_set_exposure(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_exposure = v;
    return apply_all();
}

/* Snapshot of the camera's own gamma curve (imported from its rmm tuning), kept
 * so the slider *transforms* it instead of replacing it - v=50 == the stock
 * curve. Captured lazily on first use; the tuning is still the imported curve
 * then (nothing else writes gamma_tbl_ini before the first slider move). */
static uint16_t g_gamma_base[5][3 * GAMMA_POINTS];
static int g_gamma_base_ok;

static void gamma_capture(void)
{
    fwi_tuning_modules_t *t = tunning();

    memcpy(g_gamma_base, t->gamma_tbl_init, sizeof(g_gamma_base));
    g_gamma_base_ok = 1;
}

/* Gamma slider via the SDK tuning API: isp_set_cfg(HW_ISP_CFG_TUNING,
 * HW_ISP_CFG_TUNING_GAMMA) carries the 5-LV table, HW_ISP_CFG_TEST_ENABLE turns
 * the gamma module on, then isp_update() applies. No direct struct pokes. */
int isp_config_set_gamma(int v)
{
    fwi_gamma_table_arg_t attr;
    fwi_enable_arg_t en;
    fwi_tuning_enables_t *ts = &isp_ctx[ISP_DEV].tuning.enables;
    fwi_tuning_modules_t *t = tunning();
    double exp;
    int tbl, ch, i;

    if (v < 0) v = 0;
    if (v > 100) v = 100;
    g_gamma = v;
    exp = gamma_exp(v);
    if (!g_gamma_base_ok)
        gamma_capture();

    memset(&attr, 0, sizeof(attr));
    attr.number = 5;
    for (tbl = 0; tbl < 5; tbl++)
        for (ch = 0; ch < 3; ch++)
            for (i = 0; i < GAMMA_POINTS; i++) {
                double b = g_gamma_base[tbl][ch * GAMMA_POINTS + i] / 4095.0;
                attr.value[tbl][ch * GAMMA_POINTS + i] =
                    (uint16_t)(4095.0 * pow(b, exp) + 0.5);
            }
    for (i = 0; i < 5; i++)
        attr.level_triggers[i] = (uint16_t)t->gamma_trig_cfg[i];

    /* Preserve the current module enables; just add gamma. */
    memset(&en, 0, sizeof(en));
    en.manual_mode_en = ts->manual_mode_en; en.flicker_detect_en = ts->flicker_detect_en; en.sharpen_en = ts->sharpen_en;
    en.local_contrast_en = ts->local_contrast_en; en.denoise_2d_en = ts->denoise_2d_en; en.drc_en = ts->drc_en;
    en.colour_enhance_en = ts->colour_enhance_en; en.lens_shading_en = ts->lens_shading_en; en.gamma_en = 1; en.colour_matrix_en = ts->colour_matrix_en;
    en.auto_exposure_en = ts->auto_exposure_en; en.auto_focus_en = ts->auto_focus_en; en.auto_wb_en = ts->auto_wb_en; en.histogram_en = ts->histogram_en;
    en.black_level_en = ts->black_level_en; en.sensor_offset_en = ts->sensor_offset_en; en.wb_gain_en = ts->wb_gain_en;
    en.defect_pixel_en = ts->defect_pixel_en; en.demosaic_en = ts->demosaic_en; en.denoise_3d_en = ts->denoise_3d_en;
    en.chroma_denoise_en = ts->chroma_denoise_en; en.saturation_en = ts->saturation_en; en.dehaze_en = ts->dehaze_en;
    en.linearize_en = ts->linearize_en; en.global_tone_en = ts->global_tone_en; en.digital_gain_en = ts->digital_gain_en;
    en.local_tone_en = ts->local_tone_en; en.wdr_merge_en = ts->wdr_merge_en; en.crosstalk_en = ts->crosstalk_en;

    ts->gamma_en = 1;
    isp_set_cfg(ISP_DEV, HW_ISP_CFG_TEST, HW_ISP_CFG_TEST_ENABLE, &en);
    isp_set_cfg(ISP_DEV, HW_ISP_CFG_TUNING, HW_ISP_CFG_TUNING_GAMMA, &attr);
    isp_update(ISP_DEV);
    return 0;
}

int isp_config_get_gamma(void) { return g_gamma; }

/* Copy the live module enables into an isp_test_enable_cfg. The runtime
 * enable/disable controls (pltm, tdf) edit one field and re-apply this, so every
 * other module keeps its current (stock/vendor) state. */
static void enable_cfg_from(fwi_enable_arg_t *en,
                            const fwi_tuning_enables_t *ts)
{
    memset(en, 0, sizeof(*en));
    en->manual_mode_en = ts->manual_mode_en; en->flicker_detect_en = ts->flicker_detect_en; en->sharpen_en = ts->sharpen_en;
    en->local_contrast_en = ts->local_contrast_en; en->denoise_2d_en = ts->denoise_2d_en; en->drc_en = ts->drc_en;
    en->colour_enhance_en = ts->colour_enhance_en; en->lens_shading_en = ts->lens_shading_en; en->gamma_en = ts->gamma_en; en->colour_matrix_en = ts->colour_matrix_en;
    en->auto_exposure_en = ts->auto_exposure_en; en->auto_focus_en = ts->auto_focus_en; en->auto_wb_en = ts->auto_wb_en; en->histogram_en = ts->histogram_en;
    en->black_level_en = ts->black_level_en; en->sensor_offset_en = ts->sensor_offset_en; en->wb_gain_en = ts->wb_gain_en;
    en->defect_pixel_en = ts->defect_pixel_en; en->demosaic_en = ts->demosaic_en; en->denoise_3d_en = ts->denoise_3d_en;
    en->chroma_denoise_en = ts->chroma_denoise_en; en->saturation_en = ts->saturation_en; en->dehaze_en = ts->dehaze_en;
    en->linearize_en = ts->linearize_en; en->global_tone_en = ts->global_tone_en; en->digital_gain_en = ts->digital_gain_en;
    en->local_tone_en = ts->local_tone_en; en->wdr_merge_en = ts->wdr_merge_en; en->crosstalk_en = ts->crosstalk_en;
}

/* PLTM (this ISP's HDR/tone-map) on/off. The vendor tuning ships pltm_en=1, so
 * 1 == stock. Unlike the `wdr` strength control (AW_MPI_ISP_SetPltmWDR, which
 * only writes tune.pltmwdr_level), this toggles the module itself, so it can
 * express Protect's "wdr = Off". Uses the same test-enable + isp_update path as
 * isp_config_set_gamma(); needs y623 validation (runtime PLTM reconfig has
 * historically perturbed the WDR pipeline). */
int isp_config_set_pltm(int on)
{
    fwi_enable_arg_t en;
    fwi_tuning_enables_t *ts = &isp_ctx[ISP_DEV].tuning.enables;

    ts->local_tone_en = on ? 1 : 0;
    enable_cfg_from(&en, ts);
    isp_set_cfg(ISP_DEV, HW_ISP_CFG_TEST, HW_ISP_CFG_TEST_ENABLE, &en);
    isp_update(ISP_DEV);
    return 0;
}
int isp_config_get_pltm(void)
{
    return isp_ctx[ISP_DEV].tuning.enables.local_tone_en ? 1 : 0;
}

/* 3DNR (tdf) on/off; vendor tdf_en=1, so 1 == stock. Matches Protect's
 * `enable3dnr` boolean. */
static int set_module_en(int32_t *field, int *want, int on);

int isp_config_set_tdf(int on)
{
    return set_module_en(&isp_ctx[ISP_DEV].tuning.enables.denoise_3d_en, &g_tdf_want, on);
}
/* mediad's configured intent. isp_test_settings.tdf_en itself gets refreshed
 * from the stock tuning at runtime, so it cannot be trusted to stay 0. */
int isp_config_want_tdf(void) { return g_tdf_want; }

int isp_config_get_tdf(void) { return g_tdf_want; }

/* Denoise module switches (tdf, spatial 2D, chroma). Edit the LIVE tuning's
 * enable and re-apply it with isp_ctx_config_update(), the same path the
 * day/night import uses. NOT isp_set_cfg(TEST_ENABLE) + isp_update(): that
 * copies the stored (day) tuning back over the live one, so a toggle at night
 * put the day CCM on the IR image - a purple cast until restart (r35gb
 * 2026-09-26). The final module-enable word is also enforced at load-reg
 * (stock_reg.c), since a later tuning reload rebuilds it. */
static int set_module_en(int32_t *field, int *want, int on)
{
    *field = on ? 1 : 0;
    *want = *field;
    isp_ctx_config_update(&isp_ctx[ISP_DEV]);
    return 0;
}

int isp_config_set_nr2d(int on)
{
    return set_module_en(&isp_ctx[ISP_DEV].tuning.enables.denoise_2d_en, &g_nr2d_want, on);
}
int isp_config_want_nr2d(void) { return g_nr2d_want; }

int isp_config_set_cnr(int on)
{
    return set_module_en(&isp_ctx[ISP_DEV].tuning.enables.chroma_denoise_en, &g_cnr_want, on);
}
int isp_config_want_cnr(void) { return g_cnr_want; }

/* Day/night tuning swap (calls.md "Night Vision"). parser_ini_info() re-fills
 * isp_ini_cfg from the vendor day/night blob (`ir` selects it); apply_all()
 * re-runs the ISP config path and re-asserts the picture controls (which the
 * blob import would otherwise reset). */
extern int parser_ini_info(fwi_tuning_image_t *param, char *sensor_name,
                           int w, int h, int fps, int wdr, int ir,
                           int sync_mode, int isp_id);
int isp_config_set_daynight(int night)
{
    fwi_isp_ctx_t *c = &isp_ctx[ISP_DEV];

    g_daynight = night ? 1 : 0;
    if (c->sensor.name == NULL)
        return -1;

    /* Import the camera's OWN day/night tuning (the night blob changes the whole
     * profile - AE tables, gamma, CCM, PLTM, denoise, sharp, contrast, CEM - not
     * just a monochrome tweak) and apply it through the tuning UPDATE path
     * (`__isp_ctx_update` via `isp_ctx_config_update`): it re-copies the tuning
     * into module_cfg and re-inits the per-module cfgs, but does NOT run
     * `__isp_ctx_cfg_lib` (the runtime-state reset that was the destabiliser).
     * NOTE: call `isp_ctx_config_update()` directly, NOT `isp_update()` - the
     * latter first overwrites ctx->isp_ini_cfg from tuning->params, discarding
     * the import. */
    parser_ini_info(&c->tuning, c->sensor.name,
                    c->sensor.sensor_width, c->sensor.sensor_height,
                    (int)c->sensor.fps_fixed, 0, night ? 1 : 0, 0, ISP_DEV);
    isp_ctx_config_update(c);
    return 0;
}

int isp_config_get_daynight(void) { return g_daynight; }

int isp_config_get_saturation(void) { return g_saturation; }
int isp_config_get_hue(void)        { return g_hue; }
int isp_config_get_sharpness(void)  { return g_sharpness; }
int isp_config_get_brightness(void) { return g_brightness; }
int isp_config_get_contrast(void)   { return g_contrast; }
int isp_config_get_denoise(void)    { return g_denoise; }
int isp_config_get_exposure(void)   { return g_exposure; }
