// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * fshare.h - producer side of stock rmm's shared-memory frame ring
 * (/dev/shm/fshare_frame_buf), the contract unifi_flv_bridge/FlvPush consumes.
 *
 * mediad is a drop-in rmm replacement, so it publishes encoded frames into the
 * same ring stock rmm used, keeping FshareReader/FlvPush untouched. The byte
 * layout and update protocol below match what the stock consumers read.
 *
 * Geometry (y623/h52ga, and every 28-byte-header model):
 *   total mmap size  0xbf170 (782704)
 *   data area        [368, 0xbf170), i.e. 0xbf000 (782336) bytes
 *   frame header     28 bytes
 */
#ifndef MEDIAD_FSHARE_H
#define MEDIAD_FSHARE_H

#include <stddef.h>
#include <stdint.h>

/* Frame type bits, matching stock rmm and the yi-hack readers. */
#define FSHARE_TYPE_IDR    0x0001
#define FSHARE_TYPE_SPS    0x0002
#define FSHARE_TYPE_PPS    0x0004
#define FSHARE_TYPE_VPS    0x0008
#define FSHARE_TYPE_PREFIX 0x0020
#define FSHARE_TYPE_HEVC   0x0040
#define FSHARE_TYPE_AAC    0x0100
#define FSHARE_TYPE_HIGH   0x0400
#define FSHARE_TYPE_LOW    0x0800

/* Create/map the ring and open/create the lock semaphores. Idempotent-safe to
 * call once at startup. Returns 0 on success, -1 on failure (errno set). */
int fshare_init(void);

/* Unmap the ring (does not shm_unlink: the object outlives a single run, same
 * as stock rmm). */
void fshare_close(void);

/*
 * Publish one encoded frame.
 *
 *   payload/len     the frame bytes (one NAL for H.264)
 *   type            FSHARE_TYPE_* bits for the channel + NAL type
 *   time            presentation time in milliseconds (same unit stock rmm
 *                   used; FlvPush derives wall-clock deltas from it)
 *   stream_counter  monotonically increasing u16 per channel
 *   prefix/prefix_len  optional bytes inserted between the 28-byte frame
 *                   header and the payload; non-zero sets FSHARE_TYPE_PREFIX
 *                   and folds prefix_len into the header's len field. SPS
 *                   frames MUST carry a 6-byte prefix (readers strip exactly
 *                   6 bytes whenever FSHARE_TYPE_SPS is set).
 *
 * Returns 0 on success, -1 on failure. Thread-safe via the write lock.
 */
int fshare_publish(const void *payload, size_t len, uint16_t type,
                   uint32_t time, uint16_t stream_counter,
                   const void *prefix, size_t prefix_len);

#endif /* MEDIAD_FSHARE_H */
