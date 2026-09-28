// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* main.c - mediad: VI capture -> ISP -> H.264 encode -> fshare ring.
 * Two VI devices, one encoder each (vi_dev 0 -> HIGH, vi_dev 1 -> LOW); two
 * virtual channels on one vipp deadlock the VI refcount. Env: docs/env.md. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "media/mm_comm_vi.h"
#include "media/mpi_vi.h"
#include "media/mpi_isp.h"
#include "media/mpi_sys.h"
#include "mm_common.h"
#include "mm_comm_vi.h"
#include "mm_comm_video.h"
#include "mm_comm_venc.h"   /* VENC_PACK_S only; the VENC MPI itself is gone */

#include "fshare.h"
#include "isp_control.h"
#include "isp_config.h"
#include "talkback.h"
#include "mediad_audio.h"
#include "mediad_venc.h"
#include "osd.h"
#include "rmm_tuning.h"
#ifdef MEDIAD_ALGO_RTOS
#include "freeisp_shim_tables.h"
#endif

#define SRC_FPS 20
#define MAX_GETSTREAM_FAILS 20  /* exit rather than wedge holding ion buffers */
#define MAX_VENC_FAILS      10  /* encoder input-pool errors before we exit */
/* An input PTS gap larger than this forces an IDR so the decoder and the
 * controller's recorder resync at the resume point. */
#define MEDIAD_INPUT_GAP_US 500000u

#define STATS_EVERY_N_FRAMES (SRC_FPS * 60) /* one heartbeat line per minute */

typedef struct {
    const char *name;
    VI_DEV vi_dev;
    VI_CHN vi_chn;
    VENC_CHN venc_chn;
    uint16_t base_type;   /* FSHARE_TYPE_HIGH / FSHARE_TYPE_LOW */
    int cap_w, cap_h;     /* VI capture size */
    int pic_w, pic_h;     /* encoded size (== cap here, as stock) */
    uint32_t bitrate;
    struct mediad_venc *venc;  /* our H.264 encoder (mediad_venc.c) */
    unsigned char *spspps;
    size_t spspps_len;
    uint16_t stream_counter;
    unsigned char *assemble;   /* per-channel: only this channel's own thread uses it */
    size_t assemble_cap;
    uint64_t last_pts;         /* last encoded PTS (us); used to detect input gaps */
    uint64_t next_due_us;      /* frame-rate throttle: PTS at which the next encode is due */
    pthread_t tid;
} media_chan;

static media_chan g_chans[] = {
    /* Default bitrates; runtime values come from mediad.bitrate / Protect. */
    { "high", 0, 0, 0, FSHARE_TYPE_HIGH, 2304, 1296, 2304, 1296, 2800000, NULL, 0, 0, 0 },
    { "low",  1, 0, 1, FSHARE_TYPE_LOW,   640,  360,  640,  360,  700000, NULL, 0, 0, 0 },
};
#define NCHAN ((int)(sizeof(g_chans) / sizeof(g_chans[0])))

static volatile sig_atomic_t g_stop;
static volatile uint32_t g_last_frame_ms;

/* Encode fps; MEDIAD_FPS lowers it when the single A7 runs out of headroom. */
static int g_fps = SRC_FPS;

/* Monotonic microseconds, for per-call timing in the stream thread. */
static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Pin mediad in RAM (MCL_CURRENT|MCL_FUTURE); the default RLIMIT_MEMLOCK is
 * only 64 KB, so raise it first. */
static void lock_memory(void)
{
    struct rlimit rl;

    if (getenv("MEDIAD_NO_MLOCK") != NULL) {
        fprintf(stderr, "mediad: memory locking disabled (MEDIAD_NO_MLOCK)\n");
        return;
    }
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0)
        fprintf(stderr, "mediad: setrlimit(RLIMIT_MEMLOCK) failed: %s\n",
                strerror(errno));
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "mediad: mlockall failed: %s (may be swapped)\n",
                strerror(errno));
    else
        fprintf(stderr, "mediad: memory locked (never swapped)\n");
}

/* MEDIAD_LOW=0 runs HIGH only. */
static int active_chans(void)
{
    const char *e = getenv("MEDIAD_LOW");
    return (e && e[0] == '0') ? 1 : NCHAN;
}

/* HIGH-channel geometry is keyed on the live sensor name (V4L2 subdev sysfs),
 * never the model: models share sensors. Unknown sensors get a 16:9 best effort. */
typedef struct {
    const char *sensor;
    int cap_w, cap_h;   /* VI capture */
    int pic_w, pic_h;   /* encoded */
    int wdr;            /* 1 = sensor-commanding WDR (vendor profile) */
} sensor_geo;

static const sensor_geo g_sensors[] = {
    { "gc3003_mipi", 2304, 1296, 2304, 1296, 1 },
    { "gc2053_mipi", 1936, 1096, 1920, 1080, 0 },
};

static int g_sensor_known;
static int g_sensor_wdr = 1;    /* sensor-table fallback */
static char g_sensor[64];       /* live sensor name */
static int g_vendor_wdr = -1;   /* authoritative WDR flag from cfg_arr (+84) */
static int g_mirror, g_flip;    /* per-model mounting orientation */

static void read_sensor_name(char *out, size_t n)
{
    const char *e = getenv("MEDIAD_SENSOR");
    DIR *d;
    struct dirent *de;

    out[0] = 0;
    if (e && e[0]) { snprintf(out, n, "%s", e); return; }
    d = opendir("/sys/class/video4linux");
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        char path[256], name[64];
        FILE *f;
        if (strncmp(de->d_name, "v4l-subdev", 10) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", de->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof(name), f)) {
            char *q = name + strlen(name);
            while (q > name && (q[-1] == '\n' || q[-1] == '\r'))
                *--q = 0;
            /* sensors are "<part>_mipi"; skip the vin/isp/csi bus subdevs */
            if (strstr(name, "_mipi") && !strstr(name, "sunxi") && !strstr(name, "vin")) {
                snprintf(out, n, "%s", name);
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
}

static void apply_geometry(void)
{
    int cap_w = 2304, cap_h = 1296, pic_w = 2304, pic_h = 1296;
    const char *e;
    size_t i;

    read_sensor_name(g_sensor, sizeof(g_sensor));
    for (i = 0; i < sizeof(g_sensors) / sizeof(g_sensors[0]); i++) {
        if (strcmp(g_sensor, g_sensors[i].sensor) == 0) {
            cap_w = g_sensors[i].cap_w;
            cap_h = g_sensors[i].cap_h;
            pic_w = g_sensors[i].pic_w;
            pic_h = g_sensors[i].pic_h;
            g_sensor_wdr = g_sensors[i].wdr;
            g_sensor_known = 1;
            break;
        }
    }
    if ((e = getenv("MEDIAD_CAP_W")) && e[0]) cap_w = atoi(e);
    if ((e = getenv("MEDIAD_CAP_H")) && e[0]) cap_h = atoi(e);
    if ((e = getenv("MEDIAD_PIC_W")) && e[0]) pic_w = atoi(e);
    if ((e = getenv("MEDIAD_PIC_H")) && e[0]) pic_h = atoi(e);
    g_chans[0].cap_w = cap_w;
    g_chans[0].cap_h = cap_h;
    g_chans[0].pic_w = pic_w;
    g_chans[0].pic_h = pic_h;
    fprintf(stderr, "mediad: sensor=%s (%s) high cap=%dx%d pic=%dx%d\n",
            g_sensor[0] ? g_sensor : "(unknown)",
            g_sensor_known ? "known" : "best-effort",
            cap_w, cap_h, pic_w, pic_h);
}

/* Mounting orientation is per model (r35gb's gc2053 is mounted rotated 180),
 * keyed on MEDIAD_MODEL or the SD model_suffix. */
typedef struct {
    const char *model;
    int mirror, flip;
} model_caps;

static const model_caps g_caps[] = {
    { "r35gb", 1, 1 },
};

static void read_model_suffix(char *out, size_t n)
{
    const char *files[] = {
        "/tmp/sd/unifi/etc/model_suffix",
        "/tmp/sd/yi-hack/model_suffix",
    };
    const char *e = getenv("MEDIAD_MODEL");
    size_t i;

    out[0] = 0;
    if (e && e[0]) { snprintf(out, n, "%s", e); return; }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *f = fopen(files[i], "r");
        char *p, *q;
        if (!f)
            continue;
        if (!fgets(out, (int)n, f)) { fclose(f); continue; }
        fclose(f);
        p = out;
        while (*p == ' ' || *p == '\t') p++;
        if (p != out) memmove(out, p, strlen(p) + 1);
        q = out + strlen(out);
        while (q > out && (q[-1] == '\n' || q[-1] == '\r' ||
                           q[-1] == ' ' || q[-1] == '\t'))
            *--q = 0;
        return;
    }
}

static void apply_capabilities(void)
{
    char model[64];
    const char *e;
    size_t i;

    read_model_suffix(model, sizeof(model));
    for (i = 0; i < sizeof(g_caps) / sizeof(g_caps[0]); i++) {
        if (strcmp(model, g_caps[i].model) == 0) {
            g_mirror = g_caps[i].mirror;
            g_flip = g_caps[i].flip;
            break;
        }
    }
    if ((e = getenv("MEDIAD_MIRROR")) && e[0]) g_mirror = atoi(e);
    if ((e = getenv("MEDIAD_FLIP")) && e[0]) g_flip = atoi(e);
    fprintf(stderr, "mediad: model=%s caps mirror=%d flip=%d\n",
            model[0] ? model : "(unknown)", g_mirror, g_flip);
}

/* Read the tuning's WDR flag before vi_start picks the capture mode.
 * Returns 0 linear / 2 WDR, or -1 if unavailable. */
static int load_vendor_profile(void)
{
    const char *rmm;

    if (getenv("MEDIAD_NO_RMM_TUNING") || !g_sensor[0])
        return -1;
    rmm = getenv("MEDIAD_RMM_PATH");
    if (!rmm) rmm = "/home/app/rmm";
    /* Scan rmm itself: the later rmm_tuning_load may be served from the SD cache. */
    return rmm_tuning_probe_wdr(rmm, g_sensor);
}

#ifdef MEDIAD_ALGO_RTOS
/* Feed the camera's own 3A tables to the clean-room shims (SD cache first, else
 * located in rmm); must precede AW_MPI_ISP_Run. Failure keeps shim defaults. */
static void load_shim_tables(void)
{
    const char *rmm, *bundle;

    if (getenv("MEDIAD_NO_RMM_TUNING"))
        return;
    rmm = getenv("MEDIAD_RMM_PATH");
    if (!rmm)
        rmm = "/home/app/rmm";
    bundle = getenv("MEDIAD_TABLE_BUNDLE");   /* NULL = FREEISP_TABLE_BUNDLE_PATH */
    if (freeisp_shim_tables_from_rmm_or_cache(rmm, bundle) != 0)
        fprintf(stderr, "mediad: clean-shim tables unavailable (bundle/rmm %s); "
                        "using shim defaults\n", rmm);
    else
        fprintf(stderr, "mediad: clean-shim tables installed (cache-first); "
                        "pltm presets: %s; ae out bias: %s\n",
                freeisp_shim_pltm_presets_status(), freeisp_shim_ae_out_bias_status());
}
#endif

/* Runtime bitrate from Protect, persisted because the controller re-sends it
 * only on connect/change; a restart would otherwise fall back to the default. */
#define MEDIAD_BITRATE_MIN 48000u
#define MEDIAD_BITRATE_MAX 6000000u
#define MEDIAD_BITRATE_FILE_DEFAULT "/tmp/sd/unifi/etc/mediad.bitrate"

static const char *bitrate_file(void)
{
    const char *e = getenv("MEDIAD_BITRATE_FILE");
    return (e && e[0]) ? e : MEDIAD_BITRATE_FILE_DEFAULT;
}

static void save_bitrate(const char *name, unsigned int bps)
{
    FILE *f = fopen(bitrate_file(), "w");
    if (f == NULL)
        return;
    fprintf(f, "%s=%u\n", name, bps);
    fclose(f);
}

static unsigned int load_bitrate(const char *name)
{
    char line[64], key[32];
    unsigned int v;
    FILE *f = fopen(bitrate_file(), "r");

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31[^=]=%u", key, &v) == 2 && strcmp(key, name) == 0) {
            fclose(f);
            return v;
        }
    }
    fclose(f);
    return 0;
}

static unsigned int clamp_bitrate(unsigned int bps)
{
    if (bps < MEDIAD_BITRATE_MIN) bps = MEDIAD_BITRATE_MIN;
    if (bps > MEDIAD_BITRATE_MAX) bps = MEDIAD_BITRATE_MAX;
    return bps;
}

int mediad_set_bitrate(const char *name, unsigned int bps)
{
    int i;

    bps = clamp_bitrate(bps);
    for (i = 0; i < NCHAN; i++) {
        if (strcmp(g_chans[i].name, name) != 0)
            continue;
        if (bps == g_chans[i].bitrate)
            return 0;
        if (mediad_venc_set_bitrate(g_chans[i].venc, (int)bps) != 0)
            return -1;
        g_chans[i].bitrate = bps;
        save_bitrate(name, bps);
        fprintf(stderr, "mediad: %s bitrate -> %u bps\n", name, bps);
        return 0;
    }
    return -1;
}

/* Encoder 3D-filter strength (0 off .. 511 hardware threshold), all channels. */
static int g_venc3d;

int mediad_set_venc3d(int level)
{
    int i, rc = 0;

    level = level < 0 ? 0 : level > 511 ? 511 : level;
    for (i = 0; i < NCHAN; i++)
        if (g_chans[i].venc && mediad_venc_set_filter3d(g_chans[i].venc, level) != 0)
            rc = -1;
    if (rc == 0 && level != g_venc3d)
        fprintf(stderr, "mediad: encoder 3D filter -> %d\n", level);
    if (rc == 0)
        g_venc3d = level;
    return rc;
}

int mediad_get_venc3d(void) { return g_venc3d; }

unsigned int mediad_get_bitrate(const char *name)
{
    int i;
    for (i = 0; i < NCHAN; i++)
        if (strcmp(g_chans[i].name, name) == 0)
            return g_chans[i].bitrate;
    return 0;
}

/* Shutter mode: 0 auto, 1 short, 2 long. Here, not in isp_control.c, because
 * the VI struct has enums and isp_control.c is built -fshort-enums. */
int mediad_set_shutter(int mode)
{
    fwm_vi_shutter_cfg_t cfg;

    if (mode < 0 || mode > 2)
        return -1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.shutter_mode = (VI_SHUTTIME_MODE_E)mode;
    cfg.reset_mode = VI_SHUTTIME_RESET_AUTO_DELAY;
    if (AW_MPI_VI_SetVippShutterTime(0, &cfg) != SUCCESS)
        return -1;
    AW_MPI_VI_SetVippShutterTime(1, &cfg);
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Same OOM exemption stock rmm gets from init.sh. */
static void protect_from_oom(void)
{
    int fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "-1000", 5) != 5)
            fprintf(stderr, "mediad: warning: could not set oom_score_adj\n");
        close(fd);
    }
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Publish one Annex-B NAL with its start code. SPS entries carry a 6-byte
 * prefix: readers strip 6 bytes whenever the SPS bit is set. */
static void publish_nal(media_chan *ch, const unsigned char *nal, size_t n, uint32_t ts)
{
    static const unsigned char sps_prefix[6] = { 0, 0, 0, 0, 0, 0 };
    const void *prefix = NULL;
    size_t prefix_len = 0;
    size_t sc = 0;
    uint16_t type = ch->base_type;

    if (n >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1)
        sc = 3;
    else if (n >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1)
        sc = 4;
    if (sc == 0 || sc >= n)
        return;

    switch (nal[sc] & 0x1f) {
    case 5: type |= FSHARE_TYPE_IDR; break;
    case 7: type |= FSHARE_TYPE_SPS; prefix = sps_prefix; prefix_len = sizeof(sps_prefix); break;
    case 8: type |= FSHARE_TYPE_PPS; break;
    default: break; /* SEI / non-IDR slice: channel base type only */
    }

    (void)fshare_publish(nal, n, type, ts, ch->stream_counter++,
                         prefix, prefix_len);
}

/* Split an Annex-B byte stream into NALs and publish each, keeping each NAL's
 * own start code. */
static void publish_annexb(media_chan *ch, const unsigned char *buf, size_t len, uint32_t ts)
{
    size_t i = 0, cur = 0;
    int have = 0;

    while (i + 3 <= len) {
        size_t sc = 0;

        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            sc = 3;
        } else if (i + 4 <= len && buf[i] == 0 && buf[i + 1] == 0 &&
                   buf[i + 2] == 0 && buf[i + 3] == 1) {
            sc = 4;
        }
        if (sc == 0) {
            i++;
            continue;
        }
        if (have && i > cur)
            publish_nal(ch, buf + cur, i - cur, ts);
        cur = i;
        have = 1;
        i += sc;
    }
    if (have && cur < len)
        publish_nal(ch, buf + cur, len - cur, ts);
    else if (!have && len > 0)
        publish_nal(ch, buf, len, ts); /* no start code: treat as a single NAL */
}

/* Does this Annex-B buffer contain an IDR slice (NAL type 5)? */
static int has_idr(const unsigned char *buf, size_t len)
{    size_t i = 0;

    while (i + 3 <= len) {
        size_t sc;

        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            sc = 3;
        } else if (i + 4 <= len && buf[i] == 0 && buf[i + 1] == 0 &&
                   buf[i + 2] == 0 && buf[i + 3] == 1) {
            sc = 4;
        } else {
            i++;
            continue;
        }
        if (i + sc < len && (buf[i + sc] & 0x1f) == 5)
            return 1;
        i += sc;
    }
    return 0;
}

/* Concatenate the VENC pack's up-to-two buffers (a NAL may straddle them) and
 * publish. */
static void publish_pack(media_chan *ch, const fwm_venc_pack_t *pack)
{
    size_t a = pack->addr0 ? pack->len0 : 0;
    size_t b = pack->addr1 ? pack->len1 : 0;
    size_t total = a + b;

    if (total == 0)
        return;
    if (total > ch->assemble_cap) {
        unsigned char *grown = realloc(ch->assemble, total);
        if (!grown)
            return;
        ch->assemble = grown;
        ch->assemble_cap = total;
    }
    if (a)
        memcpy(ch->assemble, pack->addr0, a);
    if (b)
        memcpy(ch->assemble + a, pack->addr1, b);

    /* Encoder PTS, not publish time: publish-time stamps are bursty and make
     * FlvPush misjudge the frame rate. */
    uint32_t ts = pack->pts ? (uint32_t)(pack->pts / 1000) : now_ms();

    /* Re-emit SPS+PPS before every IDR, as stock rmm does; readers that wait
     * for an SPS (imggrabber) would otherwise never start. */
    if (ch->spspps_len && has_idr(ch->assemble, total))
        publish_annexb(ch, ch->spspps, ch->spspps_len, ts);

    publish_annexb(ch, ch->assemble, total, ts);
    g_last_frame_ms = now_ms();
}

/* Exit after 15 s without frames rather than hold ion memory while wedged;
 * the watchdog restarts us. */
static void *watchdog_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        sleep(3);
        if (g_last_frame_ms != 0 &&
            (uint32_t)(now_ms() - g_last_frame_ms) > 15000) {
            fprintf(stderr, "mediad: no frames for >15s, exiting to avoid wedging\n");
            fflush(stderr);
            _exit(1);
        }
    }
    return NULL;
}

static void *ae_telemetry_thread(void *arg)
{
    ISP_DEV isp = (ISP_DEV)(long)arg;
    while (!g_stop) {
        int exp = -1, gain = -1, line = -1, ev = -32768, mode = -1, met = -1;
        AW_MPI_ISP_AE_GetExposure(isp, &exp);
        AW_MPI_ISP_AE_GetGain(isp, &gain);
        AW_MPI_ISP_AE_GetExposureLine(isp, &line);
        AW_MPI_ISP_AE_GetEvIdx(isp, &ev);
        AW_MPI_ISP_AE_GetMode(isp, &mode);
        AW_MPI_ISP_AE_GetMetering(isp, &met);
        fprintf(stderr, "mediad[ae]: exp=%d line=%d gain=%d ev=%d mode=%d metering=%d\n",
                exp, line, gain, ev, mode, met);
        fflush(stderr);
        sleep(3);
    }
    return NULL;
}

/* Per channel: VI frame -> mediad_venc encode -> publish. */
static void *get_stream_thread(void *arg)
{
    media_chan *ch = arg;
    long count = 0;
    long fails = 0;
    long venc_fails = 0;
    time_t start = time(NULL);
    uint64_t max_get = 0, max_enc = 0, max_pub = 0;

    while (!g_stop) {
        fwm_video_frame_info_t fi;
        struct mediad_venc_frame ef;
        struct cov1 cov;
        int ret;
        uint64_t t0, t1, t2, t3, t4;

        memset(&fi, 0, sizeof(fi));
        t0 = now_us();
        ret = AW_MPI_VI_GetFrame(ch->vi_dev, ch->vi_chn, &fi, 2000);
        t1 = now_us();
        if (t1 - t0 > max_get)
            max_get = t1 - t0;
        if (t1 - t0 > 100000)
            fprintf(stderr, "[%s] SLOW getframe %llu ms\n", ch->name,
                    (unsigned long long)(t1 - t0) / 1000);
        if (ret < 0) {
            if (++fails >= MAX_GETSTREAM_FAILS) {
                fprintf(stderr, "[%s] VI GetFrame failing persistently (%d); "
                                "giving up to avoid wedging the box\n",
                        ch->name, (int)fails);
                g_stop = 1;
                break;
            }
            mediad_venc_request_idr(ch->venc);
            continue;
        }
        fails = 0;

        /* Drop source frames to reach g_fps, phase-locked to the capture PTS;
         * a late frame resyncs instead of bursting. */
        if (g_fps > 0 && g_fps < SRC_FPS) {
            uint64_t interval = 1000000ull / (uint64_t)g_fps;

            if (ch->next_due_us == 0)
                ch->next_due_us = ((const fwm_video_frame_t *)&fi.v_frame)->mpts;
            if (((const fwm_video_frame_t *)&fi.v_frame)->mpts < ch->next_due_us) {
                AW_MPI_VI_ReleaseFrame(ch->vi_dev, ch->vi_chn, &fi);
                continue;
            }
            ch->next_due_us += interval;
            if (ch->next_due_us <= ((const fwm_video_frame_t *)&fi.v_frame)->mpts)   /* fell behind: resync */
                ch->next_due_us = ((const fwm_video_frame_t *)&fi.v_frame)->mpts + interval;
        }

        if (ch->last_pts && ((const fwm_video_frame_t *)&fi.v_frame)->mpts > ch->last_pts &&
            ((const fwm_video_frame_t *)&fi.v_frame)->mpts - ch->last_pts > MEDIAD_INPUT_GAP_US) {
            fprintf(stderr, "[%s] input gap %llu ms, forcing IDR\n", ch->name,
                    (unsigned long long)(((const fwm_video_frame_t *)&fi.v_frame)->mpts - ch->last_pts) / 1000);
            mediad_venc_request_idr(ch->venc);
        }
        ch->last_pts = ((const fwm_video_frame_t *)&fi.v_frame)->mpts;

        cov.virY = ((const fwm_video_frame_t *)&fi.v_frame)->vir_addr[0];
        cov.virC = ((const fwm_video_frame_t *)&fi.v_frame)->vir_addr[1];
        cov.phyY = (void *)(unsigned long)((const fwm_video_frame_t *)&fi.v_frame)->phy_addr[0];
        cov.phyC = (void *)(unsigned long)((const fwm_video_frame_t *)&fi.v_frame)->phy_addr[1];
        cov.stride = (int)((const fwm_video_frame_t *)&fi.v_frame)->stride[0];

        memset(&ef, 0, sizeof(ef));
        t2 = now_us();
        ret = mediad_venc_encode(ch->venc, &cov, &ef);
        t3 = now_us();
        if (t3 - t2 > max_enc)
            max_enc = t3 - t2;
        if (t3 - t2 > 100000)
            fprintf(stderr, "[%s] SLOW encode %llu ms\n", ch->name,
                    (unsigned long long)(t3 - t2) / 1000);
        if (ret == 0) {
            /* Drain every queued bitstream unit: one left occupied starves the
             * pool and kills the encoder. */
            do {
                fwm_venc_pack_t pack;
                memset(&pack, 0, sizeof(pack));
                pack.addr0 = (unsigned char *)ef.addr0;
                pack.len0 = ef.len0;
                pack.addr1 = (unsigned char *)ef.addr1;
                pack.len1 = ef.len1;
                pack.pts = ef.pts;
                publish_pack(ch, &pack);
                mediad_venc_release(ch->venc, &ef);
                memset(&ef, 0, sizeof(ef));
            } while (mediad_venc_ready(ch->venc) > 0 &&
                     mediad_venc_next(ch->venc, &ef) == 0);
            venc_fails = 0;
        } else if (ret == -2) {
            /* Input pool exhausted: reset the managers and force an IDR. */
            if (++venc_fails >= MAX_VENC_FAILS) {
                fprintf(stderr, "[%s] encoder input pool dead (%d); giving up to "
                                "avoid wedging the box\n", ch->name, (int)venc_fails);
                g_stop = 1;
                AW_MPI_VI_ReleaseFrame(ch->vi_dev, ch->vi_chn, &fi);
                break;
            }
            fprintf(stderr, "[%s] encoder input pool exhausted; resetting\n", ch->name);
            mediad_venc_reset(ch->venc);
        }

        t4 = now_us();
        if (t4 - t3 > max_pub)
            max_pub = t4 - t3;
        if (t4 - t3 > 100000)
            fprintf(stderr, "[%s] SLOW publish %llu ms\n", ch->name,
                    (unsigned long long)(t4 - t3) / 1000);

        AW_MPI_VI_ReleaseFrame(ch->vi_dev, ch->vi_chn, &fi);

        count++;
        if (count % STATS_EVERY_N_FRAMES == 0) {
            long uptime = (long)(time(NULL) - start);
            printf("[%s] frames=%ld uptime=%lds maxget=%llums maxenc=%llums maxpub=%llums\n",
                   ch->name, count, uptime,
                   (unsigned long long)max_get / 1000,
                   (unsigned long long)max_enc / 1000,
                   (unsigned long long)max_pub / 1000);
            fflush(stdout);
            max_get = max_enc = max_pub = 0;
        }
    }
    printf("[%s] stopping after %ld frames\n", ch->name, count);
    return NULL;
}

static int vi_start(media_chan *ch)
{
    fwm_vi_attr_t attr;
    int ret;

    memset(&attr, 0, sizeof(attr));
    attr.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    attr.memtype = V4L2_MEMORY_MMAP;
    /* LBC 2.5X compressed capture, as stock rmm; NV21 (MEDIAD_NV21=1) needs
     * about twice the frame-buffer memory. */
    attr.format.pixelformat = map_PIXEL_FORMAT_E_to_V4L2_PIX_FMT(
        mediad_capture_nv21() ? FWM_MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420
                              : FWM_MM_PIXEL_FORMAT_YUV_AW_LBC_2_5X);
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = ch->cap_w;
    attr.format.height = ch->cap_h;
    attr.fps = g_fps;
    attr.nbufs = 3;          /* driver rejects 2; 5 costs ~9 MB and OOMs */
    attr.nplanes = 2;
    attr.use_current_window = 0;

    /* Sensor WDR mode follows the extracted tuning's flag, else the sensor
     * table; MEDIAD_WDR forces it. */
    {
        const char *e = getenv("MEDIAD_WDR");
        int wdr = e ? (e[0] != '0')
                    : (g_vendor_wdr >= 0 ? (g_vendor_wdr != 0) : g_sensor_wdr);
        if (wdr) {
            attr.capturemode = 2;
            attr.wdr_mode = 2;
            attr.drop_frame_count = 5;
            fprintf(stderr, "[%s] WDR sensor mode: capturemode=2 wdr_mode=2 drop=5\n",
                    ch->name);
        }
    }

    AW_MPI_VI_CreateVipp(ch->vi_dev);
    {
        /* Unknown sensor: walk a 16:9 candidate list until the driver accepts one. */
        static const int fb_w[] = { 2304, 1920, 1280, 640 };
        static const int fb_h[] = { 1296, 1080, 720, 360 };
        int max_try = (ch->vi_dev == 0 && !g_sensor_known) ? 4 : 1;
        int try = 0;

        for (;;) {
            attr.format.width = ch->cap_w;
            attr.format.height = ch->cap_h;
            ret = AW_MPI_VI_SetVippAttr(ch->vi_dev, &attr);
            if (ret >= 0)
                break;
            fprintf(stderr, "[%s] AW_MPI_VI_SetVippAttr %dx%d failed: %d\n",
                    ch->name, ch->cap_w, ch->cap_h, ret);
            if (++try >= max_try)
                return -1;
            ch->cap_w = fb_w[try];
            ch->cap_h = fb_h[try];
            if (ch->pic_w > ch->cap_w) ch->pic_w = ch->cap_w;
            if (ch->pic_h > ch->cap_h) ch->pic_h = ch->cap_h;
        }
        /* Adapt to what the driver actually negotiated (it may clamp to a
         * sensor mode); keep the encoder in step with the capture. */
        {
            fwm_vi_attr_t got;
            memset(&got, 0, sizeof(got));
            if (AW_MPI_VI_GetVippAttr(ch->vi_dev, &got) == SUCCESS &&
                (int)got.format.width > 0 && (int)got.format.height > 0 &&
                ((int)got.format.width != ch->cap_w ||
                 (int)got.format.height != ch->cap_h)) {
                fprintf(stderr, "[%s] VI negotiated %dx%d -> %dx%d\n",
                        ch->name, ch->cap_w, ch->cap_h,
                        (int)got.format.width, (int)got.format.height);
                ch->cap_w = (int)got.format.width;
                ch->cap_h = (int)got.format.height;
                if (ch->pic_w > ch->cap_w) ch->pic_w = ch->cap_w;
                if (ch->pic_h > ch->cap_h) ch->pic_h = ch->cap_h;
            }
        }
    }
    ret = AW_MPI_VI_EnableVipp(ch->vi_dev);
    if (ret < 0) {
        fprintf(stderr, "[%s] AW_MPI_VI_EnableVipp failed: %d\n", ch->name, ret);
        return -1;
    }
    return 0;
}

static int vi_virchn_create(media_chan *ch)
{
    int ret = AW_MPI_VI_CreateVirChn(ch->vi_dev, ch->vi_chn, NULL);
    if (ret < 0) {
        fprintf(stderr, "[%s] AW_MPI_VI_CreateVirChn(%d,%d) failed: %d\n",
                ch->name, ch->vi_dev, ch->vi_chn, ret);
        return -1;
    }
    AW_MPI_VI_SetVirChnAttr(ch->vi_dev, ch->vi_chn, NULL); /* no-op in this SDK */
    return 0;
}

/* Open the encoder and cache SPS/PPS. Virchns are enabled only after every
 * channel is set up. */
static int venc_start(media_chan *ch)
{
    struct mediad_venc_cfg cfg;
    unsigned char hdr[256];
    int n;

    /* Defaults are stock rmm's encoder settings (High, VBR, QP 10..40) except
     * encoder 3DNR, which smears motion. Overrides: docs/env.md. */
    {
        const char *e;
        cfg.src_w = ch->cap_w;
        cfg.src_h = ch->cap_h;
        cfg.pic_w = ch->pic_w;
        cfg.pic_h = ch->pic_h;
        cfg.out_w = cfg.out_h = 0;
        cfg.out_x = cfg.out_y = -1;
        cfg.crop_x = cfg.crop_y = -1;
        cfg.fps = g_fps;
        cfg.bitrate = (int)ch->bitrate;
        cfg.gop = g_fps * (strcmp(ch->name, "high") == 0 ? 5 : 1);
        cfg.profile = 2;
        cfg.min_qp = 10;
        cfg.max_qp = 40;
        cfg.rc_mode = 1;      /* VBR */
        cfg.nr3d = 0;
        cfg.fastenc = 0;
        cfg.chn = (int)ch->venc_chn;
        if ((e = getenv("MEDIAD_PROFILE"))) cfg.profile = atoi(e);
        if ((e = getenv("MEDIAD_RC"))) cfg.rc_mode = atoi(e);
        if ((e = getenv("MEDIAD_MINQP"))) cfg.min_qp = atoi(e);
        if ((e = getenv("MEDIAD_MAXQP"))) cfg.max_qp = atoi(e);
        if ((e = getenv("MEDIAD_GOP"))) cfg.gop = atoi(e);
        if ((e = getenv(strcmp(ch->name, "high") == 0 ? "MEDIAD_GOP_HIGH" : "MEDIAD_GOP_LOW")))
            cfg.gop = atoi(e);
        if ((e = getenv("MEDIAD_3DNR"))) cfg.nr3d = atoi(e);
        if (strcmp(ch->name, "high") == 0) {
            if ((e = getenv("MEDIAD_CROP_X"))) cfg.crop_x = atoi(e);
            if ((e = getenv("MEDIAD_CROP_Y"))) cfg.crop_y = atoi(e);
            if (cfg.crop_x < 0 || cfg.crop_y < 0)
                cfg.crop_x = cfg.crop_y = -1;
            if ((e = getenv("MEDIAD_OUT_W"))) cfg.out_w = atoi(e);
            if ((e = getenv("MEDIAD_OUT_H"))) cfg.out_h = atoi(e);
            if ((e = getenv("MEDIAD_OUT_X"))) cfg.out_x = atoi(e);
            if ((e = getenv("MEDIAD_OUT_Y"))) cfg.out_y = atoi(e);
        }
        if ((e = getenv("MEDIAD_FASTENC"))) cfg.fastenc = atoi(e);
        fprintf(stderr, "[%s] venc profile=%d rc=%s minqp=%d maxqp=%d gop=%d 3dnr=%d\n",
                ch->name, cfg.profile,
                cfg.rc_mode == 0 ? "cbr" : (cfg.rc_mode == 2 ? "avbr" : "vbr"),
                cfg.min_qp, cfg.max_qp, cfg.gop, cfg.nr3d);
    }

    ch->venc = mediad_venc_open(&cfg);
    if (ch->venc == NULL) {
        fprintf(stderr, "[%s] encoder open failed\n", ch->name);
        return -1;
    }

    n = mediad_venc_spspps(ch->venc, hdr, sizeof(hdr));
    fprintf(stderr, "[%s] spspps n=%d first=%02x %02x %02x %02x %02x %02x\n",
            ch->name, n, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]);
    if (n > 0) {
        /* Cache our own copy: publish_pack re-emits it before every IDR. */
        ch->spspps = malloc((size_t)n);
        if (ch->spspps) {
            memcpy(ch->spspps, hdr, (size_t)n);
            ch->spspps_len = (size_t)n;
            publish_annexb(ch, ch->spspps, ch->spspps_len, now_ms());
        }
    }

    printf("[mediad] %s channel ready: vi_dev=%d vi_chn=%d %dx%d @%d kbps (own venc)\n",
           ch->name, ch->vi_dev, ch->vi_chn, ch->pic_w, ch->pic_h,
           ch->bitrate / 1000);
    return 0;
}

static void chan_teardown(media_chan *ch)
{
    AW_MPI_VI_DisableVirChn(ch->vi_dev, ch->vi_chn);
    mediad_venc_close(ch->venc);
    ch->venc = NULL;
    AW_MPI_VI_DestoryVirChn(ch->vi_dev, ch->vi_chn);
    AW_MPI_VI_DisableVipp(ch->vi_dev);
    AW_MPI_VI_DestoryVipp(ch->vi_dev);
    free(ch->spspps);
    ch->spspps = NULL;
    ch->spspps_len = 0;
}

/* Reproducible builds pass -DMEDIAD_BUILD_STAMP=<token> (package.sh: a hash of
 * the build inputs); it must contain no spaces to survive EXTRA_CFLAGS. */
#ifndef MEDIAD_BUILD_STAMP
#define MEDIAD_BUILD_STAMP __DATE__ " " __TIME__
#endif

int main(void)
{
    ISP_DEV isp_dev = 0;
    fwm_sys_config_t sys_conf;
    int i, ret, nc;

    printf("mediad build %s\n", MEDIAD_BUILD_STAMP);
    protect_from_oom();
    nc = active_chans();
    {
        const char *e;
        if ((e = getenv("MEDIAD_HIGH_BPS")))
            g_chans[0].bitrate = (uint32_t)strtoul(e, NULL, 0);
        else {
            unsigned int v = load_bitrate("high");
            if (v)
                g_chans[0].bitrate = v;
        }
        if (NCHAN > 1) {
            if ((e = getenv("MEDIAD_LOW_BPS")))
                g_chans[1].bitrate = (uint32_t)strtoul(e, NULL, 0);
            else {
                unsigned int v = load_bitrate("low");
                if (v)
                    g_chans[1].bitrate = v;
            }
        }
        g_chans[0].bitrate = clamp_bitrate(g_chans[0].bitrate);
        if (NCHAN > 1)
            g_chans[1].bitrate = clamp_bitrate(g_chans[1].bitrate);
        fprintf(stderr, "mediad: bitrate high=%u low=%u\n",
                g_chans[0].bitrate, NCHAN > 1 ? g_chans[1].bitrate : 0);
        if ((e = getenv("MEDIAD_FPS"))) {
            g_fps = atoi(e);
            if (g_fps < 5)
                g_fps = 5;
            if (g_fps > 30)
                g_fps = 30;
        }
        fprintf(stderr, "mediad: fps=%d\n", g_fps);
    }
    apply_geometry();
    g_vendor_wdr = load_vendor_profile();
    if (g_vendor_wdr >= 0)
        fprintf(stderr, "mediad: vendor profile wdr=%d\n", g_vendor_wdr);
    apply_capabilities();
#ifdef MEDIAD_ALGO_RTOS
    load_shim_tables();
#endif
    printf("mediad: %d channel(s)\n", nc);

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    /* Control/web clients time out and close; a reply to a closed socket must
     * fail with EPIPE, not kill the daemon. */
    signal(SIGPIPE, SIG_IGN);

    memset(&sys_conf, 0, sizeof(sys_conf));
    sys_conf.align_width = 32;
    AW_MPI_SYS_SetConf(&sys_conf);

    ret = AW_MPI_SYS_Init();
    if (ret < 0) {
        fprintf(stderr, "AW_MPI_SYS_Init failed: %d\n", ret);
        return 1;
    }

    if (fshare_init() < 0) {
        fprintf(stderr, "fshare_init failed\n");
        return 1;
    }

    /* Order: all vipps, ISP_Run (needs a configured sensor subdev), virchns,
     * encoders, then enable virchns. */
    for (i = 0; i < nc; i++) {
        if (vi_start(&g_chans[i]) < 0)
            return 1;
    }

    AW_MPI_ISP_Run(isp_dev);

    /* Protect's mirror/flip toggles apply relative to the mounting orientation. */
    isp_control_set_orientation(g_mirror, g_flip);

    {
        const char *b = getenv("ISP_AE_BIAS");
        const char *m;
        if (b) {
            int v = atoi(b);
            int r = AW_MPI_ISP_AE_SetExposureBias(isp_dev, v);
            fprintf(stderr, "mediad: AE exposure bias %d => %d\n", v, r);
        }
        if ((m = getenv("ISP_AE_METERING"))) {
            int r = AW_MPI_ISP_AE_SetMetering(isp_dev, atoi(m));
            fprintf(stderr, "mediad: AE metering %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_AE_MODE"))) {
            int r = AW_MPI_ISP_AE_SetMode(isp_dev, atoi(m));
            fprintf(stderr, "mediad: AE mode %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_BRIGHTNESS"))) {
            int r = AW_MPI_ISP_SetBrightness(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP brightness %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_CONTRAST"))) {
            int r = AW_MPI_ISP_SetContrast(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP contrast %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_SATURATION"))) {
            int r = AW_MPI_ISP_SetSaturation(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP saturation %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_SHARPNESS"))) {
            int r = AW_MPI_ISP_SetSharpness(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP sharpness %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_3DNR"))) {
            int r = AW_MPI_ISP_Set3NRAttr(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP 3DNR %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_NR"))) {
            int r = AW_MPI_ISP_SetNRAttr(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP NR %d => %d\n", atoi(m), r);
        }
        if ((m = getenv("ISP_PLTMWDR"))) {
            int r = AW_MPI_ISP_SetPltmWDR(isp_dev, atoi(m));
            fprintf(stderr, "mediad: ISP pltmwdr %d => %d\n", atoi(m), r);
        }
    }

    /* Control surface for Protect's picture settings; failure is non-fatal. */
    if (isp_control_start(isp_dev) < 0)
        fprintf(stderr, "mediad: ISP control surface unavailable\n");

    for (i = 0; i < nc; i++) {
        if (vi_virchn_create(&g_chans[i]) < 0) {
            isp_control_stop();
            return 1;
        }
    }
    for (i = 0; i < nc; i++) {
        if (venc_start(&g_chans[i]) < 0) {
            isp_control_stop();
            return 1;
        }
    }
    for (i = 0; i < nc; i++)
        AW_MPI_VI_EnableVirChn(g_chans[i].vi_dev, g_chans[i].vi_chn);

    /* Burned-in OSD (date/name/logo/bitrate) on the VENC channels. */
    {
        osd_chan_cfg oc[NCHAN];
        for (i = 0; i < nc; i++) {
            oc[i].venc = g_chans[i].venc;
            oc[i].venc_chn = g_chans[i].venc_chn;
            oc[i].frame_w = g_chans[i].pic_w;
            oc[i].frame_h = g_chans[i].pic_h;
        }
        osd_start(oc, nc, NULL);
    }

    if (!getenv("MEDIAD_NO_AUDIO")) {
        if (mediad_audio_start() != 0)
            fprintf(stderr, "mediad: audio unavailable; continuing video-only\n");
        /* Talkback: /tmp/audio_in_fifo -> ALSA playback. */
        if (talkback_start() != 0)
            fprintf(stderr, "mediad: talkback unavailable; continuing\n");
    } else {
        printf("mediad: audio disabled by MEDIAD_NO_AUDIO\n");
    }

    for (i = 0; i < nc; i++)
        pthread_create(&g_chans[i].tid, NULL, get_stream_thread, &g_chans[i]);

    g_last_frame_ms = now_ms();
    {
        pthread_t wd, at;
        if (pthread_create(&wd, NULL, watchdog_thread, NULL) == 0)
            pthread_detach(wd);
        if (pthread_create(&at, NULL, ae_telemetry_thread,
                           (void *)(long)isp_dev) == 0)
            pthread_detach(at);
    }

    /* Everything is allocated by now; pin it. */
    lock_memory();

    for (i = 0; i < nc; i++)
        pthread_join(g_chans[i].tid, NULL);

    osd_stop();
    talkback_stop();
    mediad_audio_stop();

    for (i = 0; i < nc; i++)
        chan_teardown(&g_chans[i]);

    isp_control_stop();

    AW_MPI_ISP_Stop(isp_dev);
    AW_MPI_ISP_Exit();
    AW_MPI_SYS_Exit();

    fshare_close();
    for (i = 0; i < nc; i++) {
        free(g_chans[i].assemble);
        g_chans[i].assemble = NULL;
        g_chans[i].assemble_cap = 0;
    }
    printf("done\n");
    return 0;
}
