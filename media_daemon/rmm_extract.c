// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
//
// rmm_extract - CLI wrapper around rmm_tuning (see rmm_tuning.h).
//
// Extracts the stock rmm's ISP tuning into our struct layout and writes
// extracted_{day,night}.bin to <outdir>. The vendor bytes never leave the
// camera; mediad does the same extraction in-process at first init.
//
// Usage: rmm_extract <rmm_path> <sensor> [outdir]
//   e.g. rmm_extract /home/app/rmm gc3003_mipi /tmp/sd/yi-protect/isp_cfg
//   rmm_path/sensor default from RMM_PATH / RMM_SENSOR; outdir from
//   RMM_OUTDIR (default /tmp/sd/yi-protect/isp_cfg).

#include <stdio.h>
#include <stdlib.h>

#include "rmm_tuning.h"

static int write_out(const char *dir, const char *name,
                     const unsigned char *buf, unsigned int len) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(buf, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *rmm_path = argc > 1 ? argv[1] : getenv("RMM_PATH");
    const char *sensor = argc > 2 ? argv[2] : getenv("RMM_SENSOR");
    const char *outdir = argc > 3 ? argv[3] : getenv("RMM_OUTDIR");
    const unsigned char *day, *night;
    unsigned int day_len, night_len;

    if (!rmm_path) rmm_path = "/home/app/rmm";
    if (!outdir) outdir = "/tmp/sd/yi-protect/isp_cfg";
    if (!sensor) {
        fprintf(stderr, "rmm_extract: usage: %s <rmm_path> <sensor> [outdir]\n", argv[0]);
        return 2;
    }

    if (rmm_tuning_extract(rmm_path, sensor) != 0) {
        fprintf(stderr, "rmm_extract: extraction failed; nothing written\n");
        return 1;
    }
    day = rmm_tuning_blob(0, &day_len);
    night = rmm_tuning_blob(1, &night_len);
    if (!day || !night) return 1;
    if (write_out(outdir, "extracted_day.bin", day, day_len) != 0 ||
        write_out(outdir, "extracted_night.bin", night, night_len) != 0) {
        fprintf(stderr, "rmm_extract: cannot write blobs to %s\n", outdir);
        return 1;
    }
    fprintf(stderr, "rmm_extract: wrote extracted_{day,night}.bin (%u B)\n", day_len);
    return 0;
}
