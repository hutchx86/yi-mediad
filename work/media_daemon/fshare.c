// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * fshare.c - see fshare.h. Implements the producer side of the
 * /dev/shm/fshare_frame_buf ring contract described there.
 */
#include "fshare.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FSHARE_SHM_NAME   "/fshare_frame_buf"
#define FSHARE_WLOCK_NAME "/fshare_write_lock"
#define FSHARE_RLOCK_NAME "/fshare_read_lock"

#define FSHARE_TOTAL_SIZE  0xbf170u /* 782704 */
#define FSHARE_DATA_OFFSET 368u
#define FSHARE_DATA_SIZE   0xbf000u /* 782336, the ring portion */
#define FSHARE_HDR_SIZE    28u
#define FSHARE_NOTIFY_COUNT 17u

/* Ring control header offsets, relative to the start of the mapping. */
#define FSHARE_OFF_LEN      4u
#define FSHARE_OFF_ENDOFF   12u
#define FSHARE_OFF_START    16u
#define FSHARE_OFF_COUNTER  24u

static unsigned char *g_ring;
static sem_t *g_write_lock;
static sem_t *g_read_lock;

static uint32_t hdr_get(size_t off)
{
    uint32_t v;
    memcpy(&v, g_ring + off, sizeof(v));
    return v;
}

static void hdr_set(size_t off, uint32_t v)
{
    memcpy(g_ring + off, &v, sizeof(v));
}

/* Wrapped read/write inside the data area. off must be < FSHARE_DATA_SIZE. */
static void data_write(size_t off, const void *src, size_t n)
{
    const unsigned char *p = src;
    size_t first = FSHARE_DATA_SIZE - off;

    if (first >= n) {
        memcpy(g_ring + FSHARE_DATA_OFFSET + off, p, n);
        return;
    }
    memcpy(g_ring + FSHARE_DATA_OFFSET + off, p, first);
    memcpy(g_ring + FSHARE_DATA_OFFSET, p + first, n - first);
}

static void data_read(size_t off, void *dst, size_t n)
{
    unsigned char *p = dst;
    size_t first = FSHARE_DATA_SIZE - off;

    if (first >= n) {
        memcpy(p, g_ring + FSHARE_DATA_OFFSET + off, n);
        return;
    }
    memcpy(p, g_ring + FSHARE_DATA_OFFSET + off, first);
    memcpy(p + first, g_ring + FSHARE_DATA_OFFSET, n - first);
}

int fshare_init(void)
{
    int fd;
    int i;
    void *map;

    if (g_ring)
        return 0;

    fd = shm_open(FSHARE_SHM_NAME, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, FSHARE_TOTAL_SIZE) < 0) {
        close(fd);
        return -1;
    }
    map = mmap(NULL, FSHARE_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return -1;
    g_ring = map;

    /* Reset the control header so a stale ring (e.g. left by stock rmm, or a
     * previous process whose ring wrapped) can't desync FshareReader: it uses
     * +16 start to locate data, and if that is stale it never matches endOff.
     * Leave +0 (reader refcount) alone. */
    hdr_set(FSHARE_OFF_LEN, 0);
    hdr_set(FSHARE_OFF_ENDOFF, 0);
    hdr_set(FSHARE_OFF_START, 0);
    hdr_set(FSHARE_OFF_COUNTER, 0);

    g_write_lock = sem_open(FSHARE_WLOCK_NAME, O_CREAT, 0644, 1);
    if (g_write_lock == SEM_FAILED) {
        g_write_lock = NULL;
        return -1;
    }
    g_read_lock = sem_open(FSHARE_RLOCK_NAME, O_CREAT, 0644, 1);
    if (g_read_lock == SEM_FAILED) {
        g_read_lock = NULL;
        return -1;
    }
    for (i = 0; i < (int)FSHARE_NOTIFY_COUNT; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/fshare_read_notify_%d", i);
        /* Created for parity with stock rmm; nothing in our path waits on
         * them, so a failure here is non-fatal. */
        (void)sem_open(name, O_CREAT, 0644, 0);
    }

    fprintf(stderr, "fshare: ring %s ready (%u bytes, data offset %u)\n",
            FSHARE_SHM_NAME, FSHARE_TOTAL_SIZE, FSHARE_DATA_OFFSET);
    return 0;
}

void fshare_close(void)
{
    if (g_write_lock) {
        sem_close(g_write_lock);
        g_write_lock = NULL;
    }
    if (g_read_lock) {
        sem_close(g_read_lock);
        g_read_lock = NULL;
    }
    if (g_ring) {
        munmap(g_ring, FSHARE_TOTAL_SIZE);
        g_ring = NULL;
    }
}

int fshare_publish(const void *payload, size_t len, uint16_t type,
                   uint32_t time, uint16_t stream_counter,
                   const void *prefix, size_t prefix_len)
{
    uint32_t total;
    uint32_t length, start, end, counter;
    uint32_t frame_len;
    uint16_t hdr_type;
    unsigned char hdr[FSHARE_HDR_SIZE];

    if (!g_ring || !g_write_lock) {
        errno = EINVAL;
        return -1;
    }
    total = FSHARE_HDR_SIZE + (uint32_t)prefix_len + (uint32_t)len;
    if (total > FSHARE_DATA_SIZE) {
        errno = EINVAL;
        return -1;
    }

    sem_wait(g_write_lock);

    length = hdr_get(FSHARE_OFF_LEN);
    start = hdr_get(FSHARE_OFF_START);
    if (start >= FSHARE_DATA_SIZE)
        start = 0;
    if (length > FSHARE_DATA_SIZE)
        length = 0;

    /* Make room by dropping the oldest whole frames from `start`, as the stock
     * producer does. A frame's total size is its own header plus its len field. */
    while (length + total > FSHARE_DATA_SIZE) {
        unsigned char old[FSHARE_HDR_SIZE];
        uint32_t old_len, old_total;

        if (length < FSHARE_HDR_SIZE) {
            length = 0;
            start = 0;
            break;
        }
        data_read(start, old, FSHARE_HDR_SIZE);
        memcpy(&old_len, old, sizeof(old_len));
        old_total = FSHARE_HDR_SIZE + old_len;
        if (old_total == 0 || old_total > length) {
            length = 0;
            start = 0;
            break;
        }
        start = (start + old_total) % FSHARE_DATA_SIZE;
        length -= old_total;
    }

    end = (start + length) % FSHARE_DATA_SIZE;

    counter = hdr_get(FSHARE_OFF_COUNTER) + 1;
    if (counter == 0)
        counter = 1;

    frame_len = (uint32_t)prefix_len + (uint32_t)len;
    hdr_type = (uint16_t)(type | (prefix_len ? FSHARE_TYPE_PREFIX : 0));

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, &frame_len, sizeof(frame_len));
    memcpy(hdr + 4, &counter, sizeof(counter));
    memcpy(hdr + 16, &time, sizeof(time));
    memcpy(hdr + 20, &hdr_type, sizeof(hdr_type));
    memcpy(hdr + 22, &stream_counter, sizeof(stream_counter));

    /* Bytes first, control header last: a lock-free reader that sees the
     * advanced header is guaranteed the payload is already present. Stock rmm
     * does the opposite order under its write lock; this is strictly safer and
     * still satisfies FshareReader's start/len/endOff consistency check. */
    data_write(end, hdr, FSHARE_HDR_SIZE);
    if (prefix_len)
        data_write((end + FSHARE_HDR_SIZE) % FSHARE_DATA_SIZE, prefix, prefix_len);
    if (len)
        data_write((end + FSHARE_HDR_SIZE + prefix_len) % FSHARE_DATA_SIZE, payload, len);

    hdr_set(FSHARE_OFF_START, start);
    hdr_set(FSHARE_OFF_LEN, length + total);
    hdr_set(FSHARE_OFF_ENDOFF, (end + total) % FSHARE_DATA_SIZE);
    hdr_set(FSHARE_OFF_COUNTER, counter);

    sem_post(g_write_lock);
    return 0;
}
