// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* stub_audio_components.c - entry points the MPP registry/system glue still
 * references (audio, VENC, ion, MP3) as no-op or failing stubs, plus the ISP
 * tuning hook parser_ini_info(), which imports the camera's own rmm tuning. */

#include "plat_type.h"
#include "plat_errno.h"
#include "mm_component.h"
#include "mpi_venc_private.h"   /* VENC region hook signatures */

/* Audio decoder component: talkback plays the FIFO directly, so this fails. */
ERRORTYPE AudioDecComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* Audio subsystem hooks called by AW_MPI_SYS_Init: audio is direct ALSA here
 * (mediad_audio.c, talkback.c). */
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

/* AO channel factory, still referenced by the registry (talkback.c plays). */
ERRORTYPE AOChannel_ComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* AI channel factory, still referenced by the registry (mediad_audio.c records). */
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

/* Clock component: talkback uses no CLOCK channel. */
ERRORTYPE ClockComponentInit(COMP_HANDLETYPE hComponent)
{
    (void)hComponent;
    return FAILURE;
}

/* VENC subsystem, factory and region hooks: mediad_venc.c drives the encoder
 * and osd.c its overlay, so these only satisfy the glue. */
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

/* Sensor tuning lookup: the camera's own tuning, extracted from its rmm at
 * runtime (rmm_tuning.c), else our generated defaults (isp_config.c). */
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

    /* First call only: extract (or load the SD cache of) the live sensor's
     * tuning; later calls reuse the blobs. */
    if (!getenv("MEDIAD_NO_RMM_TUNING")) {
        static int tuning_load_tried;
        if (!tuning_load_tried) {
            const char *rmm = getenv("MEDIAD_RMM_PATH");
            const char *cache = getenv("MEDIAD_ISP_CACHE");
            tuning_load_tried = 1;
            if (!rmm) rmm = "/home/app/rmm";
            if (!cache) cache = "/tmp/sd/yi-protect/isp_cfg";
            if (rmm_tuning_load(rmm, sensor_name, cache) != 0)
                fprintf(stderr, "parser_ini_info: no vendor tuning from %s "
                                "(sensor %s); using defaults\n", rmm, sensor_name);
        }
    }

    /* No blob, or one that does not match our struct: generated defaults. */
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
        /* The blob is imported section by section (test|3a|tunning|dyn);
         * ISP_TUNE_PARTS picks the sections, for bisection. */
        const fwi_tuning_image_t *v =
            (const fwi_tuning_image_t *)blob;
        int p_test = 1, p_3a = 1, p_tun = 1, p_dyn = 1;
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

        /* ISP_TUNE_ENABLE=cm forces the tuning's CCM on, for comparison. */
        if (p_tun && getenv("ISP_TUNE_ENABLE") &&
            strstr(getenv("ISP_TUNE_ENABLE"), "cm"))
            param->enables.colour_matrix_en = 1;

#ifdef ISP521_RTOS_ALGO
        /* PLTM stays as the tuning sets it; ISP_TUNE_DISABLE=pltm turns it off. */
#endif

    }

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

    /* Per-field gamma/AE overrides for bring-up (docs/env.md). */
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

/* ion wrapper: AW_MPI_SYS_Init needs ion_memOpen()/ion_memClose() to succeed
 * (open /dev/ion, /dev/cedar_dev); allocation calls are stubs, never used. */
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

/* The _extend variant takes an IonAllocAttr; opaque here. */
unsigned char *ion_allocMem_extend(void *pAttr)
{
    (void)pAttr;
    return (unsigned char *)0;
}

/* Audio decoder subsystem: mediad never decodes. */
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

/* MP3 encoder entry points: mediad only encodes AAC. */
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
