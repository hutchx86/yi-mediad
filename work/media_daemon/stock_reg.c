// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * stock_reg.c - wire stock rmm's captured ISP register program into mediad.
 *
 * The linker wraps the 2019 libisp's isp_set_load_reg(), so every load-reg call
 * is handed stock's whole captured table instead of our computed one. Before
 * that we overlay the four live AWB gains (r,gr,gb,b u16 at 0x370, the same
 * offset in the 2019 and 2021 load-reg layouts) so white balance keeps
 * adapting; sensor exposure/gain remain our 3A's. Whole-table replay (not
 * per-module copies) keeps the gamma/DRC/CEM/CCM/module-enable state
 * consistent. The table is generated from the user's own camera by
 * tools/make_stock_reg.py (gitignored); without it the wrapper is inert.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device/isp_dev.h"
#include "isp_control.h"
#include "isp_config.h"

#ifdef HAVE_STOCK_REG
extern unsigned int isp_stock_reg_len;
extern unsigned char isp_stock_reg[];

/*
 * Tunable module-register overlays (first 0x1000 of the load-reg buffer, where
 * the 2019 and 2021 layouts share offsets). When the matching env knob is
 * present, copy that control's bytes from our computed table onto the replayed
 * stock table, so the setting bites without losing stock's static look.
 * Offsets from other.md / the mpi_isp setter trace:
 *   saturation 0x490, sharpness 0x420..0x43c, cnr/denoise 0x470,
 *   tdf/3dnr 0x2d0..0x2dc, pltm 0x3b0..0x3bc, wdr 0x200..0x208,
 *   rgb2yuv 0x520..0x538, wb_gain 0x370..0x378.
 * (Brightness/contrast land in the gamma/DRC table regions, banked for later.)
 */
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

/* Direct edits to the replayed stock table (Phase 1b applier experiments).
 * Offsets are byte offsets into isp_stock_reg; values are 32-bit words. */
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
                            struct isp_table_reg_map *reg)
{
    /* Enforce the tdf (3DNR) config on the final module-enable word (+0x1a0,
     * D3D = bit 5). The ISP rebuilds the flags from the stock tuning, so 3DNR
     * ran whatever mediad.conf said - the ghosting on motion and dark fabric.
     * Spatial denoise (D2D, bit 4) is deliberately left as the tuning sets it. */
    if (reg && reg->addr && reg->size >= 0x1a4) {
        static int logged;
        unsigned int m, off = 0;
        memcpy(&m, (unsigned char *)reg->addr + 0x1a0, 4);
        if (!isp_config_want_tdf())
            off |= 1u << 5;
        if (m & off) {
            if (!logged) {
                fprintf(stderr, "stock_reg: module_en %08x -> %08x (tdf off)\n",
                        m, m & ~off);
                logged = 1;
            }
            m &= ~off;
            memcpy((unsigned char *)reg->addr + 0x1a0, &m, 4);
        }
    }

    /* Debug: `mediad_ctl dump <path>` snapshots the computed (pre-replay)
     * table on the next load-reg. Kept outside the HAVE_STOCK_REG guard so the
     * clean (redistributable, no vendor data) build can still capture its own
     * register program for diagnosis. */
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
        /* Runtime picture controls (Phase 1b): overlay the register ranges the
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
