// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* rmm_tuning.h - read the running sensor's day/night ISP tuning out of the
 * camera's own rmm (/home/app/rmm) into our struct layout, cached on SD.
 * No addresses or vendor data are compiled in; only the rmm_layout.h remap. */
#ifndef MEDIAD_RMM_TUNING_H
#define MEDIAD_RMM_TUNING_H

#include <stddef.h>

/* Extract day+night blobs for `sensor` (e.g. "gc3003_mipi") from rmm_path into
 * memory. Returns 0, or -1 if no valid tuning was found. */
int rmm_tuning_extract(const char *rmm_path, const char *sensor);

/* Load from cache_dir/extracted_{day,night}.bin if valid, else extract and
 * rewrite the cache (NULL/"" disables it). Returns 0 or -1. */
int rmm_tuning_load(const char *rmm_path, const char *sensor, const char *cache_dir);

/* Blob for ir (0 day, 1 night) or NULL; *len gets its length. Valid until the
 * next extract/load. */
const unsigned char *rmm_tuning_blob(int ir, unsigned int *len);

/* WDR flag of the extracted entry (0 linear, 2 WDR; -1 before extraction): the
 * authoritative capture WDR mode, which varies per model for one sensor. */
int rmm_tuning_wdr(void);

/* Day-entry WDR flag for `sensor` read directly from rmm_path (0, 2 or -1),
 * even when rmm_tuning_load would serve the SD cache. */
int rmm_tuning_probe_wdr(const char *rmm_path, const char *sensor);

#endif /* MEDIAD_RMM_TUNING_H */
