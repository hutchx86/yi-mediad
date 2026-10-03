// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* rmm_tuning.c - see rmm_tuning.h. Extraction core shared by mediad and the
 * rmm_extract CLI; nothing firmware-specific is compiled in. */
#include "rmm_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rmm_layout.h"

/* Where mediad caches the extracted blobs on the SD card by default. */
#ifndef RMM_TUNING_CACHE_DIR
#define RMM_TUNING_CACHE_DIR "/tmp/sd/yi-protect/isp_cfg"
#endif

/* ------------------------------------------------------------------ IO --- */
static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n; unsigned char *d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    d = malloc((size_t)n);
    if (!d) { fclose(f); return NULL; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return d;
}

static int write_file(const char *dir, const char *name, const unsigned char *buf,
                      size_t len) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(buf, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* --------------------------------------------------------------- ELF ----- */
#define MAX_SEGS 8
typedef struct { uint32_t va, end, off; } seg_t;

static int load_segs(const unsigned char *d, size_t n, seg_t *segs, int *nseg) {
    uint32_t phoff; uint16_t phentsize, phnum; int i, k = 0;
    if (n < 0x34 || memcmp(d, "\177ELF", 4) != 0 || d[4] != 1 || d[5] != 1)
        return -1;
    phoff = *(uint32_t *)(d + 0x1c);
    phentsize = *(uint16_t *)(d + 0x2a);
    phnum = *(uint16_t *)(d + 0x2c);
    for (i = 0; i < phnum && k < MAX_SEGS; i++) {
        const unsigned char *p = d + phoff + (size_t)i * phentsize;
        if (p + 32 > d + n) break;
        if (*(uint32_t *)p == 1) { /* PT_LOAD */
            segs[k].off = *(uint32_t *)(p + 4);
            segs[k].va  = *(uint32_t *)(p + 8);
            segs[k].end = segs[k].va + *(uint32_t *)(p + 16);
            k++;
        }
    }
    *nseg = k;
    return k > 0 ? 0 : -1;
}

static long v2o(const seg_t *segs, int nseg, uint32_t va) {
    int i;
    for (i = 0; i < nseg; i++)
        if (segs[i].va <= va && va < segs[i].end)
            return (long)segs[i].off + (va - segs[i].va);
    return -1;
}
/* WDR flag (cfg_arr +84) of the matched entry: 0 linear, 2 WDR, -1 unknown.
 * It varies per model even for the same sensor. */
static int g_wdr = -1;

/* cfg_arr entry: name +0, w/h/fps +72, wdr/ir +84, isp_cfg_pt +92 (-> the four
 * section pointers: test, 3a, tunning, dynamic). Checked by valid_blob(). */
static int scan_sensor(const unsigned char *d, size_t n, const char *sensor,
                       unsigned int day[4], unsigned int night[4])
{
    seg_t segs[MAX_SEGS]; int nseg, got = 0;
    size_t i, nl = strlen(sensor);
    if (load_segs(d, n, segs, &nseg) != 0) return -1;
    for (i = 0; i + 96 <= n; i++) {
        long fo, o; uint32_t cp, ptrs[4]; int ir, k, ok = 1;
        if (memcmp(d + i, sensor, nl) != 0 || d[i + nl] != 0) continue;
        cp = *(uint32_t *)(d + i + 92);
        ir = *(int *)(d + i + 84 + 4);
        if (!ir && g_wdr < 0) g_wdr = *(int *)(d + i + 84); /* day entry */
        if (*(int *)(d + i + 72) <= 0) continue;
        fo = v2o(segs, nseg, cp);
        if (fo < 0 || (size_t)fo + 16 > n) continue;
        for (k = 0; k < 4; k++) {
            ptrs[k] = *(uint32_t *)(d + fo + 4*k);
            o = v2o(segs, nseg, ptrs[k]);
            if (o < 0) { ok = 0; break; }
            (ir ? night : day)[k] = (unsigned int)o;
        }
        if (ok) got |= (ir ? 2 : 1);
        i += nl; /* skip past this needle */
    }
    return got == 3 ? 0 : -1;
}

/* ------------------------------------------------------------- apply ----- */
static void apply_plan(const unsigned char *rmm, size_t n,
                       const unsigned int sec[4], unsigned char *out) {
    unsigned int i;
    memset(out, 0, RMM_BLOB_SIZE);
    for (i = 0; i < RMM_REMAP_N; i++) {
        const rmm_copy_t *t = &rmm_remap[i];
        size_t src = (size_t)sec[t->sect] + t->src;
        if (src + t->len > n || t->dst + t->len > RMM_BLOB_SIZE) continue;
        memcpy(out + t->dst, rmm + src, t->len);
    }
}

/* ---------------------------------------------------------- validate ----- */
/* Our struct layout (same for short/long: dynamic is last). */
#define OFF_TUN 6052u
#define OFF_PLTM (OFF_TUN + 87304u)
#define OFF_AE_WIN (216u + 524u)

static int valid_blob(const unsigned char *b) {
    int32_t bv = *(int32_t *)(b + OFF_PLTM + 12*4);
    int32_t bh = *(int32_t *)(b + OFF_PLTM + 13*4);
    int i, sum = 0;
    /* a handful of the isp_test_param enable flags (byte offsets) must be 0/1 */
    int flags[] = {96 /*sharp_en*/, 116 /*lsc_en*/, 124 /*gamma_en*/,
                   132 /*ae_en*/, 140 /*awb_en*/, 172 /*cnr_en*/,
                   184 /*satur_en*/, 204 /*pltm_en*/, 208 /*wdr_en*/};
    for (i = 0; i < (int)(sizeof(flags)/sizeof(flags[0])); i++) {
        int32_t v = *(int32_t *)(b + flags[i]);
        if (v != 0 && v != 1) return 0;
    }
    if (bv < 1 || bv > 64 || bh < 1 || bh > 64) return 0;
    for (i = 0; i < 256; i++) sum += b[OFF_AE_WIN + i];
    if (sum == 0) return 0;
    return 1;
}

/* ----------------------------------------------------------- in-memory --- */
static unsigned char *g_blob[2];   /* 0 day, 1 night */
static unsigned int g_blob_len[2];

static void free_blobs(void) {
    int i;
    for (i = 0; i < 2; i++) { free(g_blob[i]); g_blob[i] = NULL; g_blob_len[i] = 0; }
}

const unsigned char *rmm_tuning_blob(int ir, unsigned int *len) {
    if (ir != 0 && ir != 1) return NULL;
    if (len) *len = g_blob_len[ir];
    return g_blob[ir];
}

/* Day-entry WDR flag for `sensor`, straight from rmm: rmm_tuning_load may be
 * served from the SD cache and leave g_wdr unset. */
int rmm_tuning_probe_wdr(const char *rmm_path, const char *sensor) {
    unsigned char *rmm;
    seg_t segs[MAX_SEGS]; int nseg;
    size_t n, i, nl;
    int wdr = -1;

    if (!rmm_path) rmm_path = "/home/app/rmm";
    if (!sensor || !sensor[0]) return -1;
    rmm = read_file(rmm_path, &n);
    if (!rmm) return -1;
    if (load_segs(rmm, n, segs, &nseg) == 0) {
        nl = strlen(sensor);
        for (i = 0; i + 96 <= n; i++) {
            int ir;
            if (memcmp(rmm + i, sensor, nl) != 0 || rmm[i + nl] != 0) continue;
            ir = *(int *)(rmm + i + 84 + 4);
            if (*(int *)(rmm + i + 72) <= 0) continue;
            if (!ir) { wdr = *(int *)(rmm + i + 84); break; } /* day entry */
        }
    }
    free(rmm);
    return wdr;
}

int rmm_tuning_extract(const char *rmm_path, const char *sensor) {
    unsigned char *rmm, *out;
    unsigned int day[4], night[4];
    size_t n;
    int ir, ok = 0;

    if (!rmm_path) rmm_path = "/home/app/rmm";
    if (!sensor || !sensor[0]) {
        fprintf(stderr, "rmm_tuning: no sensor name, cannot extract\n");
        return -1;
    }

    rmm = read_file(rmm_path, &n);
    if (!rmm) { fprintf(stderr, "rmm_tuning: cannot read %s\n", rmm_path); return -1; }

    if (scan_sensor(rmm, n, sensor, day, night) != 0) {
        fprintf(stderr, "rmm_tuning: sensor %s not found in %s\n", sensor, rmm_path);
        free(rmm);
        return -1;
    }
    fprintf(stderr, "rmm_tuning: found %s config in %s\n", sensor, rmm_path);

    out = malloc(RMM_BLOB_SIZE);
    if (!out) { fprintf(stderr, "rmm_tuning: OOM\n"); free(rmm); return -1; }

    for (ir = 0; ir < 2; ir++) {
        apply_plan(rmm, n, ir ? night : day, out);
        if (!valid_blob(out)) {
            fprintf(stderr, "rmm_tuning: %s blob failed validation\n",
                    ir ? "night" : "day");
            free(out); free(rmm); free_blobs(); return -1;
        }
        free(g_blob[ir]);
        g_blob[ir] = malloc(RMM_BLOB_SIZE);
        if (!g_blob[ir]) { free(out); free(rmm); free_blobs(); return -1; }
        memcpy(g_blob[ir], out, RMM_BLOB_SIZE);
        g_blob_len[ir] = RMM_BLOB_SIZE;
        ok = 1;
    }
    free(out); free(rmm);
    fprintf(stderr, "rmm_tuning: extracted day+night blobs (%u B, abi=%s)\n",
            (unsigned)RMM_BLOB_SIZE,
#ifdef RMM_ABI_SHORT
            "short"
#else
            "long"
#endif
            );
    return ok ? 0 : -1;
}

int rmm_tuning_load(const char *rmm_path, const char *sensor, const char *cache_dir) {
    unsigned int i;
    int have_cache = 1;

    if (g_blob[0] && g_blob[1])
        return 0;   /* already loaded this run */

    if (cache_dir && cache_dir[0]) {
        for (i = 0; i < 2; i++) {
            char path[512];
            size_t n;
            unsigned char *b;
            snprintf(path, sizeof(path), "%s/extracted_%s.bin", cache_dir,
                     i ? "night" : "day");
            b = read_file(path, &n);
            if (!b || n != RMM_BLOB_SIZE) { free(b); have_cache = 0; break; }
            free(g_blob[i]);
            g_blob[i] = b;
            g_blob_len[i] = (unsigned int)n;
        }
        if (have_cache) {
            fprintf(stderr, "rmm_tuning: loaded cached blobs from %s\n", cache_dir);
            return 0;
        }
        free_blobs();
    }

    if (rmm_tuning_extract(rmm_path, sensor) != 0)
        return -1;

    if (cache_dir && cache_dir[0]) {
        (void)mkdir(cache_dir, 0755);   /* first boot: create the cache dir */
        for (i = 0; i < 2; i++)
            (void)write_file(cache_dir, i ? "extracted_night.bin" : "extracted_day.bin",
                             g_blob[i], g_blob_len[i]);
    }
    return 0;
}
