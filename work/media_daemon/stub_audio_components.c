// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * stub_audio_components.c
 *
 * A handful of vendor entry points are referenced unconditionally by code we
 * do need (mostly media/component/ComponentsRegistryTable.c's cdx_comp_table[],
 * which is not guarded by the MPPCFG_* #ifdefs the way the VI/VENC paths are),
 * but their real implementations either:
 *   (a) pull in the entire cedarx audio stack (codecs, alsa/tinyalsa, pcm ring
 *       buffers) that a VI->VENC-only prototype has no use for, or
 *   (b) are sensor-tuning-table lookups for sensors this camera doesn't have
 *       (isp_ini_parse.c's parser_ini_info/parser_sync_info ship hardcoded
 *       .ini tables for imx317/imx258/imx278/imx386/imx335 - no GC3003 table
 *       exists in this vendor tree at all), or
 *   (c) a vendor ion allocator wrapper (system/public/libion/ion_memmanager.c)
 *       whose real <linux/ion_uapi.h> uapi header is missing from this tree
 *       entirely (see include/linux/ion_uapi.h's own comment) - the real
 *       buffer allocation path in this MPI stack goes through MemAdapter/
 *       ionAlloc.c instead, not this API.
 *
 * None are reachable from our VI->VENC path, so stubbing them (fail/no-op) is
 * a legitimate scope reduction; replace with the real implementation if one is
 * ever wired in.
 */

#include "plat_type.h"
#include "plat_errno.h"
#include "mm_component.h"
#include "mpi_venc_private.h"   /* VENC region hook signatures */

/* --- ComponentsRegistryTable.c: audio *decoder* component init. Decoding is
 * the talkback/AO direction, which mediad does not drive (talkback plays the
 * FIFO directly), so this factory fails. --- */
ERRORTYPE AudioDecComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* mpi_sys.c still calls the vendor audio subsystem's construction/lookup entry
 * points during AW_MPI_SYS_Init. None of these subsystems are in the link now
 * (audio is direct ALSA + freecodec in mediad_audio.c/talkback.c), so satisfy
 * them with no-op/failing stubs. Only AENC/AI/AO/CLOCK are stubbed; ADEC already
 * was. */
typedef int MPP_AUDIO_ERR;
MPP_AUDIO_ERR AENC_Construct(void)          { return 0; }
MPP_AUDIO_ERR AENC_Destruct(void)           { return 0; }
MPP_AUDIO_ERR audioHw_Construct(void)       { return 0; }
MPP_AUDIO_ERR audioHw_Destruct(void)        { return 0; }
MPP_AUDIO_ERR CLOCK_Construct(void)         { return 0; }
MPP_AUDIO_ERR CLOCK_Destruct(void)          { return 0; }
void *AENC_GetChnComp(void *pChn)           { (void)pChn; return NULL; }
void *audioHw_AI_GetChnComp(void *pChn)     { (void)pChn; return NULL; }
void *audioHw_AO_GetChnComp(void *pChn)     { (void)pChn; return NULL; }
void *CLOCK_GetChnComp(void *pChn)          { (void)pChn; return NULL; }

/* The vendor AOChannel_Component no longer exists in the link (talkback is
 * direct ALSA in talkback.c). The registry table still references its factory,
 * so provide a failing stub. */
ERRORTYPE AOChannel_ComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* Same for the vendor AI channel: mic capture is now direct ALSA in
 * mediad_audio.c, so the AIChannel component is gone but the registry still
 * references its factory. */
ERRORTYPE AIChannel_ComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* And the vendor AAC component: the encoder is our freecodec backend driven
 * directly by mediad_audio.c. */
ERRORTYPE AudioEncComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* And the vendor clock component (talkback no longer uses the CLOCK channel). */
ERRORTYPE ClockComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* --- VENC middleware removed (mediad_venc.c drives the encoder directly) ---
 * mpi_sys.c still calls the VENC subsystem's construct/lookup hooks and the
 * registry references its factory; mpi_region.c calls four VENC region hooks.
 * OSD is now burned in by the encoder directly (osd.c -> mediad_venc_set_overlay),
 * so these region hooks exist only to satisfy the registry/region glue. */
ERRORTYPE VENC_Construct(void)              { return 0; }
ERRORTYPE VENC_Destruct(void)               { return 0; }
MM_COMPONENTTYPE *VENC_GetChnComp(fwm_chn_t *pMppChn) { (void)pMppChn; return NULL; }
ERRORTYPE VideoEncComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}
ERRORTYPE AW_MPI_VENC_SetRegion(VENC_CHN chn, RGN_HANDLE handle, fwm_region_attr_t *rgn,
                                const fwm_region_chn_attr_t *chn_attr, fwm_bitmap_t *bmp)
{
    (void)chn; (void)handle; (void)rgn; (void)chn_attr; (void)bmp;
    return FAILURE;
}
ERRORTYPE AW_MPI_VENC_DeleteRegion(VENC_CHN chn, RGN_HANDLE handle)
{
    (void)chn; (void)handle;
    return FAILURE;
}
ERRORTYPE AW_MPI_VENC_UpdateRegionChnAttr(VENC_CHN chn, RGN_HANDLE handle,
                                          const fwm_region_chn_attr_t *chn_attr)
{
    (void)chn; (void)handle; (void)chn_attr;
    return FAILURE;
}
ERRORTYPE AW_MPI_VENC_UpdateOverlayBitmap(VENC_CHN chn, RGN_HANDLE handle, fwm_bitmap_t *bmp)
{
    (void)chn; (void)handle; (void)bmp;
    return FAILURE;
}

/* --- isp_tuning.c: sensor tuning table lookup.
 *
 * The open SDK ships imx tuning only. The GC3003 tables are compiled into the
 * stock `rmm`; tools/extract_isp_cfg.py remaps them into the open tree's 2019
 * `struct isp_param_config` layout and emits isp_cfg/gc3003_cfg.c. We just
 * memcpy the day config here. Vendor data is generated from the user's own
 * rmm.bin at build time and is not committed. --- */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "isp_config.h"
#include "rmm_tuning.h"

#include <stdlib.h>

static void disable_flag(fwi_tuning_enables_t *t, const char *name)
{
    if (!strcmp(name, "manual")) t->manual_mode_en = 0;
    else if (!strcmp(name, "sharp")) t->sharpen_en = 0;
    else if (!strcmp(name, "contrast")) t->local_contrast_en = 0;
    else if (!strcmp(name, "denoise")) t->denoise_2d_en = 0;
    else if (!strcmp(name, "drc")) t->drc_en = 0;
    else if (!strcmp(name, "cem")) t->colour_enhance_en = 0;
    else if (!strcmp(name, "lsc")) t->lens_shading_en = 0;
    else if (!strcmp(name, "gamma")) t->gamma_en = 0;
    else if (!strcmp(name, "cm")) t->colour_matrix_en = 0;
    else if (!strcmp(name, "ae")) t->auto_exposure_en = 0;
    else if (!strcmp(name, "awb")) t->auto_wb_en = 0;
    else if (!strcmp(name, "hist")) t->histogram_en = 0;
    else if (!strcmp(name, "blc")) t->black_level_en = 0;
    else if (!strcmp(name, "wb")) t->wb_gain_en = 0;
    else if (!strcmp(name, "cfa")) t->demosaic_en = 0;
    else if (!strcmp(name, "tdf")) t->denoise_3d_en = 0;
    else if (!strcmp(name, "cnr")) t->chroma_denoise_en = 0;
    else if (!strcmp(name, "satur")) t->saturation_en = 0;
    else if (!strcmp(name, "defog")) t->dehaze_en = 0;
    else if (!strcmp(name, "gtm")) t->global_tone_en = 0;
    else if (!strcmp(name, "pltm")) t->local_tone_en = 0;
    else if (!strcmp(name, "wdr")) t->wdr_merge_en = 0;
    else if (!strcmp(name, "ctc")) t->crosstalk_en = 0;
}

static void enable_flag(fwi_tuning_enables_t *t, const char *name)
{
    if (!strcmp(name, "manual")) t->manual_mode_en = 1;
    else if (!strcmp(name, "sharp")) t->sharpen_en = 1;
    else if (!strcmp(name, "contrast")) t->local_contrast_en = 1;
    else if (!strcmp(name, "denoise")) t->denoise_2d_en = 1;
    else if (!strcmp(name, "drc")) t->drc_en = 1;
    else if (!strcmp(name, "cem")) t->colour_enhance_en = 1;
    else if (!strcmp(name, "lsc")) t->lens_shading_en = 1;
    else if (!strcmp(name, "gamma")) t->gamma_en = 1;
    else if (!strcmp(name, "cm")) t->colour_matrix_en = 1;
    else if (!strcmp(name, "ae")) t->auto_exposure_en = 1;
    else if (!strcmp(name, "awb")) t->auto_wb_en = 1;
    else if (!strcmp(name, "hist")) t->histogram_en = 1;
    else if (!strcmp(name, "blc")) t->black_level_en = 1;
    else if (!strcmp(name, "wb")) t->wb_gain_en = 1;
    else if (!strcmp(name, "cfa")) t->demosaic_en = 1;
    else if (!strcmp(name, "tdf")) t->denoise_3d_en = 1;
    else if (!strcmp(name, "cnr")) t->chroma_denoise_en = 1;
    else if (!strcmp(name, "satur")) t->saturation_en = 1;
    else if (!strcmp(name, "defog")) t->dehaze_en = 1;
    else if (!strcmp(name, "gtm")) t->global_tone_en = 1;
    else if (!strcmp(name, "pltm")) t->local_tone_en = 1;
    else if (!strcmp(name, "wdr")) t->wdr_merge_en = 1;
    else if (!strcmp(name, "ctc")) t->crosstalk_en = 1;
}

int parser_ini_info(fwi_tuning_image_t *param, char *sensor_name,
                     int w, int h, int fps, int wdr, int ir, int sync_mode, int isp_id)
{
    const unsigned char *blob;
    unsigned int len;
    const char *off, *dis, *en;
    char buf[256];

    (void)w; (void)h; (void)fps; (void)wdr; (void)sync_mode; (void)isp_id;
    if (param == NULL || sensor_name == NULL)
        return -1;

    /* First init: locate the vendor tuning in the camera's own rmm and cache it.
     * sensor_name is the live sensor (e.g. "gc3003_mipi"), so nothing is
     * hardcoded and no model map is needed. One-shot; later calls (and later
     * boots) use the loaded/cached blobs. */
    if (!getenv("MEDIAD_NO_RMM_TUNING")) {
        static int tuning_load_tried;
        if (!tuning_load_tried) {
            const char *rmm = getenv("MEDIAD_RMM_PATH");
            const char *cache = getenv("MEDIAD_ISP_CACHE");
            tuning_load_tried = 1;
            if (!rmm) rmm = "/home/app/rmm";
            if (!cache) cache = "/tmp/sd/unifi/isp_cfg";
            if (rmm_tuning_load(rmm, sensor_name, cache) != 0)
                fprintf(stderr, "parser_ini_info: no vendor tuning from %s "
                                "(sensor %s); using defaults\n", rmm, sensor_name);
        }
    }

    /* Sensor-agnostic: import whatever was extracted for this camera's sensor.
     * If there is none, or it doesn't match our struct (ABI), fall back to our
     * own generated tuning. */
    blob = rmm_tuning_blob(ir, &len);
    if (!blob || len != sizeof(*param)) {
        if (blob)
            fprintf(stderr, "parser_ini_info: blob len %u != struct len %u; defaults\n",
                    len, (unsigned)sizeof(*param));
        isp_config_fill_param(param);
        return 0;
    }

    off = getenv("ISP_TUNE_OFF");
    if (off && off[0] == '1') {
        fprintf(stderr, "parser_ini_info: tuning disabled by ISP_TUNE_OFF\n");
        isp_config_fill_param(param);
        return 0;
    }
    {
        /* The blob is a flat struct isp_param_config (test|3a|tunning|iso in
         * that order), so we can import it section-by-section.
         *
         * Default = the subset proven to map cleanly from the 2021 tables into
         * the 2019 struct on y623 (see other.md): the vendor `isp_3a_param`
         * (AE/AWB tables) and `isp_tunning_param` (CCM), while keeping the
         * open-SDK test flags (so the AE/AWB/gamma/... modules run with their
         * open defaults). The vendor `isp_test_param` is NOT imported: it
         * enables LSC/DRC/denoise/CEM/... whose 2021 tables do NOT map to 2019
         * (dot-grid / black frame) and its `awb_en=0` kills white balance. The
         * vendor `isp_dynamic_param` is NOT imported either - its 2021 dynamic
         * AE/tone-map values drive the 2019 AE dark and red. ISP_TUNE_PARTS
         * overrides the sections imported (test,3a,tunning,dyn,all; default
         * "3a,tunning"), and ISP_TUNE_ENABLE/ISP_TUNE_DISABLE the module flags,
         * for bisection. */
        const fwi_tuning_image_t *v =
            (const fwi_tuning_image_t *)blob;
        /* ISP521=1 (-DISP_TUNE_NATIVE): the blob and the compiled libisp are
         * both native 521, so import everything and keep the vendor values.
         * The 2019 build keeps the proven subset (see the 2019 notes below). */
#ifdef ISP_TUNE_NATIVE
        int p_test = 1, p_3a = 1, p_tun = 1, p_dyn = 1;
#else
        int p_test = 0, p_3a = 1, p_tun = 1, p_dyn = 0;
#endif
        const char *parts = getenv("ISP_TUNE_PARTS");
        char *tok, *save = NULL;

        if (parts) {
            p_test = p_3a = p_tun = p_dyn = 0;
            snprintf(buf, sizeof(buf), "%s", parts);
            for (tok = strtok_r(buf, ",", &save); tok;
                 tok = strtok_r(NULL, ",", &save)) {
                if (!strcmp(tok, "test")) p_test = 1;
                else if (!strcmp(tok, "3a")) p_3a = 1;
                else if (!strcmp(tok, "tunning")) p_tun = 1;
                else if (!strcmp(tok, "dyn")) p_dyn = 1;
                else if (!strcmp(tok, "all")) p_test = p_3a = p_tun = p_dyn = 1;
            }
        }
        if (getenv("ISP_KEEP_3A"))
            p_3a = 0;

        if (p_test) param->enables = v->enables;
        if (p_3a)   param->a3 = v->a3;
        if (p_tun)  param->modules = v->modules;
        if (p_dyn)  param->by_iso = v->by_iso;

        /* The vendor CCM is NOT enabled by default: the 2019 glue always uses
         * color_matrix_ini[0] (the 2800K matrix) with no colour-temp
         * interpolation, which visibly reddens the image. ISP_TUNE_ENABLE=cm
         * turns it back on for comparison. */
        if (p_tun && getenv("ISP_TUNE_ENABLE") &&
            strstr(getenv("ISP_TUNE_ENABLE"), "cm"))
            param->enables.colour_matrix_en = 1;

#ifdef ISP521_RTOS_ALGO
        /* ALGO_RTOS build: PLTM stays enabled (the vendor isp_test_param has
         * pltm_en=1). It used to SIGILL in the 2020 merge_tbl_gen because the
         * generated vendor blob was in stock rmm's isp_tunning_param layout,
         * 8 bytes off from the V833 tree's: pltm_cfg[BLOCK_V_NUM] read 0, so
         * the PLTM merge indexed a 1024-entry stack table with block_len=1296
         * and smashed the stack canary. Fixed at extraction time
         * (tools/extract_isp_cfg.py fix_tunning_v833). ISP_TUNE_DISABLE=pltm
         * turns it back off for A/B. */
#endif

#ifndef ISP_TUNE_NATIVE
        /* The vendor AE runs histogram mode (ae_hist_mod_en=1), which makes
         * the 2019 AE's get_ae_avg_lum_q8 divide by zero (SIGFPE) and kill
         * mediad. Keep the vendor AE tables/scalars but force stats mode.
         * Native 521 runs the vendor algorithm, so this is not applied. */
        if (p_3a)
            param->a3.ae_hist_mode_en = 0;
#endif
    }

#ifndef ISP_TUNE_NATIVE
    /* The vendor set targets a 2-frame WDR + PLTM pipeline; the 2019 ISP AE
     * stats divide by zero (SIGFPE) with wdr_en/pltm_en. Native 521 accepts
     * them, but the VI must also be in the matching WDR mode (MEDIAD_WDR=1 /
     * capturemode=2) for the result to be meaningful - see main.c and todo.md.
     * Off by default in the 2019 build; ISP_TUNE_DISABLE=<list> can clear
     * more for bisection. */
    param->enables.wdr_merge_en = 0;
    param->enables.local_tone_en = 0;
#endif

    dis = getenv("ISP_TUNE_DISABLE");
    if (dis) {
        char *tok, *save = NULL;
        snprintf(buf, sizeof(buf), "%s", dis);
        for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
            disable_flag(&param->enables, tok);
    }
    en = getenv("ISP_TUNE_ENABLE");
    if (en) {
        char *tok, *save = NULL;
        snprintf(buf, sizeof(buf), "%s", en);
        for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
            enable_flag(&param->enables, tok);
    }

    /* Per-field AE overrides for tuning the open-SDK AE against the GC3003.
     * The vendor isp_3a tables crash the 2019 AE (get_ae_avg_lum_q8), so the
     * useful knobs are its scalar fields. */
    {
        const char *v;
        if (getenv("ISP_GAMMA_INVERT")) {
            int t, c, k;
            for (t = 0; t < 5; t++) {
                for (c = 0; c < 3; c++) {
                    uint16_t *src = &param->modules.gamma_tbl_init[t][c * 1024];
                    uint16_t tmp[1024];
                    for (k = 0; k < 1024; k++) {
                        unsigned target = (unsigned)((unsigned long long)k * 4090 / 1023);
                        int x = 0;
                        while (x < 1023 && src[x] < target) x++;
                        tmp[k] = (uint16_t)(x * 4090 / 1023);
                    }
                    for (k = 0; k < 1024; k++) src[k] = tmp[k];
                }
            }
        }
        if ((v = getenv("ISP_GAMMA_TYPE")))
            param->modules.gamma_type = atoi(v);
        if ((v = getenv("ISP_GTM_TYPE")))
            param->modules.gtm_type = atoi(v);
        if ((v = getenv("ISP_GAMMA_NUM")))
            param->modules.gamma_count = atoi(v);
        if ((v = getenv("ISP_AE_HIST")))
            param->a3.ae_hist_mode_en = atoi(v);
        if ((v = getenv("ISP_AE_HISTSEL")))
            param->a3.ae_hist_select = atoi(v);
        if ((v = getenv("ISP_AE_DEFTBL")))
            param->a3.define_ae_table = atoi(v);
        if ((v = getenv("ISP_AE_KI")))
            param->a3.ae_ki = atoi(v);
        if ((v = getenv("ISP_AE_STATSEL")))
            param->a3.ae_stat_select = atoi(v);
        if ((v = getenv("ISP_AE_MAXLV")))
            param->a3.ae_max_level = atoi(v);
        if ((v = getenv("ISP_AE_ISO2GAIN")))
            param->a3.ae_iso2gain_ratio = atoi(v);
        if ((v = getenv("ISP_AE_GAIN_RANGE"))) {
            char tmp[64];
            char *t, *sv = NULL;
            int i = 0;
            snprintf(tmp, sizeof(tmp), "%s", v);
            for (t = strtok_r(tmp, ",", &sv); t && i < 4;
                 t = strtok_r(NULL, ",", &sv))
                param->a3.ae_gain_range[i++] = atoi(t);
        }
    }
    fprintf(stderr, "parser_ini_info: applied %s %s tuning (disable=%s enable=%s)\n",
            sensor_name, ir ? "night" : "day", dis ? dis : "-", en ? en : "-");
    return 0;
}

int parser_sync_info(fwi_tuning_image_t *param, char *isp_cfg_name, int isp_id)
{
    (void)param; (void)isp_cfg_name; (void)isp_id;
    return -1;
}

/* --- mpi_sys.c: vendor ion allocator wrapper (system/public/libion/
 * ion_memmanager.c). Its real <linux/ion_uapi.h> is missing from this tree;
 * actual buffer allocation in this MPI stack goes through
 * libcedarc/memory/memoryAdapter.c + ionMemory/ionAlloc.c instead.
 *
 * CORRECTED 2026-09-09 (was wrongly stubbed to always-fail): a live test on
 * y623 showed AW_MPI_SYS_Init()'s ion_memOpen() check gates its own
 * VENC_Construct()/RegionManager_Construct() calls and gSysManager.mState
 * ever reaching MPI_SYS_STATE_STARTED (mpi_sys.c, right after the
 * ion_memOpen check) - i.e. this WAS reachable from the VI->VENC path,
 * contrary to the original comment here. ion_memOpen()/ion_memClose()
 * themselves only need open()/stat()/close() (no ion_uapi.h ioctl structs),
 * so they're now a faithful port of the real
 * system/public/libion/ion_memmanager.c logic. The genuinely uapi-dependent
 * calls below (ion_getMemPhyAddr/ion_freeMem/ion_flushCache/ion_allocMem -
 * actual alloc/mmap/ioctl) remain stubbed - real allocation still goes
 * through ionAlloc.c as before. --- */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_ion_dev_fd = -1;
static int g_ion_cedar_fd = -1;

static int ion_stub_iommu_enabled(void)
{
    struct stat st;
    return (stat("/sys/class/iommu", &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

int ion_memOpen(void)
{
    if (g_ion_dev_fd >= 0) {
        return 0;
    }
    g_ion_dev_fd = open("/dev/ion", O_RDWR);
    if (g_ion_dev_fd < 0) {
        return -1;
    }
    if (ion_stub_iommu_enabled()) {
        g_ion_cedar_fd = open("/dev/cedar_dev", O_RDONLY, 0);
        if (g_ion_cedar_fd < 0) {
            close(g_ion_dev_fd);
            g_ion_dev_fd = -1;
            return -1;
        }
    }
    return 0;
}

int ion_memClose(void)
{
    if (g_ion_cedar_fd >= 0) {
        close(g_ion_cedar_fd);
        g_ion_cedar_fd = -1;
    }
    if (g_ion_dev_fd >= 0) {
        close(g_ion_dev_fd);
        g_ion_dev_fd = -1;
    }
    return 0;
}

unsigned int ion_getMemPhyAddr(void *vir_ptr)
{
    (void)vir_ptr;
    return 0;
}

int ion_freeMem(void *vir_ptr)
{
    (void)vir_ptr;
    return -1;
}

int ion_flushCache(void *vir_ptr, unsigned int size)
{
    (void)vir_ptr; (void)size;
    return -1;
}

unsigned char *ion_allocMem(unsigned int size)
{
    (void)size;
    return (unsigned char *)0;
}

/* mpi_sys.c calls the _extend variant too (the sun8iw19p1 tree uses it). Real
 * type is IonAllocAttr from ion_memmanager.h; declare an opaque mirror - the
 * stub only needs to satisfy the symbol/pointer. */
unsigned char *ion_allocMem_extend(void *pAttr)
{
    (void)pAttr;
    return (unsigned char *)0;
}

/* --- mpi_sys.c: audio *decoder* subsystem lifecycle + channel lookup (real
 * implementation is mpi_adec.c, not built - we only encode, never decode). --- */
#include "mm_common.h"

ERRORTYPE ADEC_Construct(void)
{
    return SUCCESS;
}

ERRORTYPE ADEC_Destruct(void)
{
    return SUCCESS;
}

MM_COMPONENTTYPE *ADEC_GetChnComp(fwm_chn_t *pMppChn)
{
    (void)pMppChn;
    return NULL;
}

/* --- MP3 encoder. midware/encoding/aencoder.c references these two
 * unconditionally (in its AUDIO_ENCODER_MP3_TYPE branches), but mediad only
 * ever selects AAC (mpi_aenc side uses AUDIO_ENCODER_AAC_TYPE), so the MP3
 * branch is never taken. Stubbing them lets us drop libmp3enc.a (the
 * LAME-derived, LGPL vendor blob) and link one fewer binary. Real prototypes
 * live in midware/encoding/mp3encApi.h:
 *   struct __AudioENC_AC320 *AudioMP3ENCEncInit(void);
 *   int AudioMP3ENCEncExit(struct __AudioENC_AC320 *p);
 * If MP3 encoding is ever wired in, swap these for the genuine libmp3enc.a. --- */
struct __AudioENC_AC320;

struct __AudioENC_AC320 *AudioMP3ENCEncInit(void)
{
    return (struct __AudioENC_AC320 *)0;
}

int AudioMP3ENCEncExit(struct __AudioENC_AC320 *p)
{
    (void)p;
    return -1;
}
