// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* fshare.h - producer side of stock rmm's shared-memory frame ring
 * (/dev/shm/fshare_frame_buf), byte-compatible with the stock readers
 * (FshareReader/FlvPush, imggrabber). Geometry: fshare.c. */
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

/* Publish one frame (time in ms, stream_counter a per-channel u16). A prefix
 * sets FSHARE_TYPE_PREFIX; SPS frames need a 6-byte one. Thread-safe; 0 or -1. */
int fshare_publish(const void *payload, size_t len, uint16_t type,
                   uint32_t time, uint16_t stream_counter,
                   const void *prefix, size_t prefix_len);

#endif /* MEDIAD_FSHARE_H */
