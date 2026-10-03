// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* stock_reg.c - isp_set_load_reg() wrapper: denoise-switch enforcement, dump
 * support and, with STOCK_REG=1, replay of a register table captured from the
 * user's own camera (tools/make_stock_reg.py) with live AWB gains overlaid. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device/isp_dev.h"
#include "isp_control.h"
#include "isp_config.h"

#ifdef HAVE_STOCK_REG
extern unsigned int isp_stock_reg_len;
extern unsigned char isp_stock_reg[];

/* When a control's env knob is set, copy its bytes from our computed table
 * onto the replayed one (offsets in the shared first 0x1000). */
struct ctl_overlay {
    const char *env;
    unsigned int off;
    unsigned int len;
};

static const struct ctl_overlay g_overlays[] = {
    { "ISP_SATURATION", 0x490, 4 },
    { "ISP_SHARPNESS",  0x420, 0x1c },
    { "ISP_NR",         0x470, 4 },
    { "ISP_3DNR",       0x2d0, 0x0c },
    { "ISP_PLTMWDR",    0x3b0, 0x0c },
};

static void overlay_controls(const unsigned char *computed, unsigned int comp_len)
{
    unsigned int i;
    for (i = 0; i < sizeof(g_overlays) / sizeof(g_overlays[0]); i++) {
        const struct ctl_overlay *o = &g_overlays[i];
        if (!getenv(o->env))
            continue;
        if (o->off + o->len <= comp_len && o->off + o->len <= isp_stock_reg_len)
            memcpy(isp_stock_reg + o->off, computed + o->off, o->len);
    }
}
#endif

/* Direct 32-bit edits to the replayed table; off is a byte offset. */
int isp_stock_reg_poke(unsigned int off, unsigned int val, unsigned int *old)
{
#ifdef HAVE_STOCK_REG
    if (off + 4 > isp_stock_reg_len)
        return -1;
    if (old)
        memcpy(old, isp_stock_reg + off, 4);
    memcpy(isp_stock_reg + off, &val, 4);
    return 0;
#else
    (void)off; (void)val; (void)old;
    return -1;
#endif
}

int isp_stock_reg_peek(unsigned int off, unsigned int *val)
{
#ifdef HAVE_STOCK_REG
    if (off + 4 > isp_stock_reg_len)
        return -1;
    memcpy(val, isp_stock_reg + off, 4);
    return 0;
#else
    (void)off; (void)val;
    return -1;
#endif
}

int __wrap_isp_set_load_reg(struct hw_isp_device *isp,
                            struct fwi_table_reg_map *reg)
{
    /* Enforce the denoise switches in the module-enable word (+0x1a0), rebuilt
     * on every tuning reload: nr2d bit 4, cnr bit 23; tdf bit 5 only cleared. */
    if (reg && reg->addr && reg->size >= 0x1a4) {
        static unsigned int logged_m = ~0u;
        unsigned int m, want;
        memcpy(&m, (unsigned char *)reg->addr + 0x1a0, 4);
        want = m;
        if (!isp_config_want_tdf())
            want &= ~(1u << 5);
        want = isp_config_want_nr2d() ? (want | (1u << 4)) : (want & ~(1u << 4));
        want = isp_config_want_cnr() ? (want | (1u << 23)) : (want & ~(1u << 23));
        if (want != m) {
            if (want != logged_m) {
                fprintf(stderr, "stock_reg: module_en %08x -> %08x (tdf=%d nr2d=%d cnr=%d)\n",
                        m, want, isp_config_want_tdf(), isp_config_want_nr2d(),
                        isp_config_want_cnr());
                logged_m = want;
            }
            memcpy((unsigned char *)reg->addr + 0x1a0, &want, 4);
        }
    }

    /* `mediad_ctl dump <path>`: snapshot the computed table on the next
     * load-reg (in every build, for diagnosis). */
    if (reg && reg->addr && reg->size) {
        char path[160];
        if (isp_control_pending_dump(path, sizeof(path))) {
            FILE *fp = fopen(path, "wb");
            if (fp) {
                fwrite(reg->addr, 1, reg->size, fp);
                fclose(fp);
            }
        }
    }

#ifdef HAVE_STOCK_REG
    static int no_replay = -1;
    if (no_replay < 0)
        no_replay = getenv("MEDIAD_NO_REPLAY") ? 1 : 0;

    if (!no_replay && isp_stock_reg_len >= 0x378) {
        /* Keep a pristine baseline so calibrated transforms are applied to a
         * fresh copy each frame (otherwise a scale factor compounds). */
        static unsigned char *orig;
        if (!orig) {
            orig = malloc(isp_stock_reg_len);
            if (orig)
                memcpy(orig, isp_stock_reg, isp_stock_reg_len);
        }
        if (orig && isp_control_xform_count() > 0) {
            memcpy(isp_stock_reg, orig, isp_stock_reg_len);
            isp_control_apply_xforms(isp_stock_reg, isp_stock_reg_len);
        }
        /* Keep our AWB live: copy the computed WB gains into the stock table. */
        if (reg->addr && reg->size >= 0x378)
            memcpy(isp_stock_reg + 0x370,
                   (unsigned char *)reg->addr + 0x370, 8);
        if (reg->addr)
            overlay_controls((unsigned char *)reg->addr, reg->size);
        /* Runtime picture controls: overlay the register ranges the
         * controller has actually set, on top of the WB/static replay. */
        if (reg->addr) {
            unsigned int n = reg->size < isp_stock_reg_len ? reg->size
                                                            : isp_stock_reg_len;
            isp_control_apply_overlay(isp_stock_reg, (unsigned char *)reg->addr, n);
        }
        reg->addr = isp_stock_reg;
        reg->size = isp_stock_reg_len;
    }
#endif
    return __real_isp_set_load_reg(isp, reg);
}
