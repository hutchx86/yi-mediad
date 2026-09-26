// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * rmm_tuning.h - on-camera ISP-tuning extraction, in-process.
 *
 * The vendor tuning is never compiled into mediad. At startup mediad reads the
 * stock rmm binary already on the camera (/home/app/rmm), scans it for the
 * running sensor's config entry, and copies the day/night isp_param_config blob
 * into *our* struct layout, cached on SD for later boots. No addresses are
 * hardcoded: the sensor name comes from the live ISP and the section pointers
 * are followed at runtime. The only compiled-in table is the stock-521 -> V833
 * struct remap (rmm_layout.h).
 */
#ifndef MEDIAD_RMM_TUNING_H
#define MEDIAD_RMM_TUNING_H

#include <stddef.h>

/*
 * Extract day+night blobs for `sensor` (the stock rmm sensor needle, e.g.
 * "gc3003_mipi") from the rmm ELF at rmm_path and keep them in memory. Always
 * re-reads rmm_path. Returns 0 on success, -1 if no valid tuning was found.
 */
int rmm_tuning_extract(const char *rmm_path, const char *sensor);

/*
 * Load tuning, preferring the on-SD cache in cache_dir: if both
 * extracted_{day,night}.bin are present and RMM_BLOB_SIZE, load them; otherwise
 * rmm_tuning_extract() from rmm_path and (re)write the cache. Either way the
 * blobs are left in memory. cache_dir NULL/"" disables caching. Returns 0 on
 * success, -1 on failure.
 */
int rmm_tuning_load(const char *rmm_path, const char *sensor, const char *cache_dir);

/*
 * Blob for ir (0 day, 1 night), or NULL if unavailable. *len (if non-NULL)
 * receives the blob length. The pointer is owned by this module and stays valid
 * until the next extract/load.
 */
const unsigned char *rmm_tuning_blob(int ir, unsigned int *len);

/*
 * Vendor WDR flag (cfg_arr +84) of the extracted sensor entry: 0 = linear,
 * 2 = sensor-commanding WDR. Returns -1 until a valid extraction has run.
 * This is the authoritative source for the capture WDR mode (varies per model
 * even for the same sensor), superseding any per-model/sensor guess.
 */
int rmm_tuning_wdr(void);

/*
 * Scan rmm_path's cfg_arr for `sensor` and return the day entry's WDR flag
 * (0 linear / 2 WDR), or -1. Does not build/validate blobs, so it works even
 * when rmm_tuning_load would serve the on-SD cache instead of reading rmm.
 */
int rmm_tuning_probe_wdr(const char *rmm_path, const char *sensor);

#endif /* MEDIAD_RMM_TUNING_H */
