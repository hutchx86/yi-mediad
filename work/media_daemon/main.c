// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * main.c - standalone VI(capture) -> ISP -> VENC (H.264 hw encode) -> fshare
 * ring daemon. This is mediad, the drop-in stock-rmm replacement.
 *
 * Structure mirrors stock rmm's own MPP setup:
 * TWO separate VI devices, each with its own capture resolution and its own
 * single VENC channel - not two virtual channels on one vipp (that deadlocks
 * the VI refcount: VENC GetStream returns EN_ERR_BUF_EMPTY and the vipp runs
 * out of buffers).
 *
 *   vi_dev 0 @2304x1296 -> VENC 0 -> HIGH (0x0400)   [1920x1080 on h52ga]
 *   vi_dev 1 @ 640x360  -> VENC 1 -> LOW  (0x0800)
 *
 * unifi_flv_bridge/FlvPush aliases MED to the LOW frames, so Protect's live
 * view (video3) is covered by the single LOW encode. Stock order is reproduced:
 * both vipps set up, both virchn created, both VENCs created/bound/StartRecvPic,
 * and only THEN both virchn enabled. Ring contract and helper: fshare.h/.c.
 *
 * Burned-in OSD (date/name/logo/bitrate) is pushed to each encoder's overlay
 * engine by osd.c; it is applied to every encoded frame, not composited in
 * software. Runs until SIGINT/SIGTERM; exits on sustained VENC failure so it
 * can't wedge the box.
 */

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
/* Encoder-input stalls surface as PTS jumps. Force a keyframe so the decoder
 * and the controller's recorder resync at the resume point instead of smearing
 * or rejecting frames until the next scheduled GOP. */
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
    /* HIGH bitrate default 2.8 Mbps (Protect's own target; the runtime value
     * comes from mediad.bitrate / Protect and is clamped by the isp_control
     * table - see isp_control.c). */
    { "high", 0, 0, 0, FSHARE_TYPE_HIGH, 2304, 1296, 2304, 1296, 2800000, NULL, 0, 0, 0 },
    { "low",  1, 0, 1, FSHARE_TYPE_LOW,   640,  360,  640,  360,  700000, NULL, 0, 0, 0 },
};
#define NCHAN ((int)(sizeof(g_chans) / sizeof(g_chans[0])))

static volatile sig_atomic_t g_stop;
static volatile uint32_t g_last_frame_ms;

/* Capture/encode fps. SRC_FPS is the stock 20; MEDIAD_FPS overrides it at
 * startup so the single A7 can be throttled when 20 fps at a high bitrate runs
 * out of headroom (visible as lag + blockiness that appear after a while). */
static int g_fps = SRC_FPS;

/* Monotonic microseconds, for per-call timing in the stream thread. */
static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/*
 * Pin mediad's pages in RAM so the kernel can never swap it out.  The default
 * RLIMIT_MEMLOCK is 64 KB, so raise it first.  MCL_CURRENT locks everything
 * already mapped (the VI/encoder/ion buffers and the heap) and MCL_FUTURE keeps
 * later allocations resident too.  MEDIAD_NO_MLOCK=1 disables it.
 */
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

/* Stock rmm uses two vipps: HIGH on vi_dev 0, LOW on vi_dev 1. The earlier
 * second-vipp SIGFPE was the vendor test param (wdr_mode/capturemode), which we
 * no longer import, so both run. Set MEDIAD_LOW=0 for HIGH-only (bisection). */
static int active_chans(void)
{
    const char *e = getenv("MEDIAD_LOW");
    return (e && e[0] == '0') ? 1 : NCHAN;
}

/* HIGH-channel geometry, keyed on the LIVE SENSOR NAME (read from the V4L2
 * subdev sysfs - the same signal the vendor tuning extraction uses), NOT the
 * model string, so a mislabeled model_suffix cannot pick the wrong mode.
 *
 * Geometry is a property of the SENSOR, not the model: several models can share
 * one sensor (r35gb and h52ga both use gc2053_mipi), so the model is never
 * inferred from the sensor. (Mounting orientation is the per-model bit - see
 * g_caps below.)
 *
 * Known sensor -> its tested capture/encode geometry (table below):
 * gc3003_mipi -> 2304x1296; gc2053_mipi -> 1936x1096 capture, 1920x1080 encode
 * (stock's own H.264 SPS). Unknown sensor -> best effort: request a 16:9
 * target; vi_start() reads back the size the driver actually negotiated
 * (AW_MPI_VI_GetVippAttr) and walks a candidate list if it is rejected.
 * MEDIAD_SENSOR overrides the detected name; MEDIAD_CAP_* and MEDIAD_PIC_*
 * override the geometry (bring-up). wdr is the known vendor profile
 * (MEDIAD_WDR overrides); LOW/vi_dev 1 stays 640x360. */
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

/* Mounting orientation is a per-MODEL property, not a sensor property: r35gb
 * uses the same gc2053_mipi as h52ga but its sensor is mounted rotated 180
 * (mirror + flip), so raw frames come out upside-down. Keyed on the model
 * string (MEDIAD_MODEL, else the SD model_suffix). One row per model that needs
 * it; MEDIAD_MIRROR/MEDIAD_FLIP override. */
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

/* Extract the vendor tuning before vi_start so the WDR flag (cfg_arr +84) is
 * known when the capture mode is chosen; parser_ini_info's later load is then a
 * no-op. Returns the vendor wdr (0 linear / 2 WDR), or -1 if unavailable. */
static int load_vendor_profile(void)
{
    const char *rmm;

    if (getenv("MEDIAD_NO_RMM_TUNING") || !g_sensor[0])
        return -1;
    rmm = getenv("MEDIAD_RMM_PATH");
    if (!rmm) rmm = "/home/app/rmm";
    /* Always scan rmm for the WDR flag - rmm_tuning_load (called later by
     * parser_ini_info) may serve the on-SD cache and never touch rmm. */
    return rmm_tuning_probe_wdr(rmm, g_sensor);
}

#ifdef MEDIAD_ALGO_RTOS
/* Install the camera's own libisp constant tables (AE/AWB/AFS/ISO/GTM/PLTM) into
 * the clean-room shims.  The shims latch their table pointers in
 * clean_<mod>_init(), called by isp_ctx_algo_init() during AW_MPI_ISP_Run(), so
 * this must run first.
 *
 * Cache-first: a bundle written on a previous boot is installed as-is and the
 * vendor `rmm` image is not read at all; only a missing/corrupt cache falls
 * back to locating the tables in `rmm`, which then (re)writes the bundle for
 * the next boot.
 *
 * Failure is NOT fatal: the shims keep their built-in pilot defaults and the
 * capture path still starts (degraded tuning).  MEDIAD_NO_RMM_TUNING disables
 * the whole feed (cache included); MEDIAD_TABLE_BUNDLE overrides the cache path
 * (empty string = never cache, pure locator). */
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
        fprintf(stderr, "mediad: clean-shim tables installed (cache-first)\n");
}
#endif

/* Runtime encoder bitrate, from Protect's ChangeVideoSettings (forwarded by
 * goclient via mediad_ctl). Applied with our own VideoEncSetParameter
 * (FWM_VENC_PARAM_BITRATE). The value is persisted so a mediad restart cannot
 * silently drop it back to the 1.5 Mbps compile-time default: the controller
 * only re-sends it on connect / settings change, so the first restart after a
 * deploy used to halve the stream (visible compression artifacts, r35gb
 * 2026-09-19). Env MEDIAD_{HIGH,LOW}_BPS still wins. */
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

unsigned int mediad_get_bitrate(const char *name)
{
    int i;
    for (i = 0; i < NCHAN; i++)
        if (strcmp(g_chans[i].name, name) == 0)
            return g_chans[i].bitrate;
    return 0;
}

/* Shutter exposure mode (Protect Auto / Frame Capture / Best Low Light) via
 * AW_MPI_VI_SetVippShutterTime. Lives here, not isp_control.c, because
 * VI_SHUTTIME_CFG_S carries enums and isp_control.c is compiled -fshort-enums
 * while the VI glue is 4-byte. mode: 0 auto, 1 preview (short), 2 night (long). */
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

/* Stock rmm keeps itself out of the OOM killer's path (init.sh sets -1000 for
 * it) so a snapshot's ~9 MB RSS doesn't take down the encoder. Do the same in
 * process: the GetStream-failure guard above still lets us exit cleanly rather
 * than wedge. */
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

/* Publish one H.264 NAL, start code included. The ring stores Annex-B NALs with
 * their start codes (imggrabber feeds them straight to libavcodec; FlvPush
 * splits Annex-B itself), so the payload must not be stripped. SPS frames must
 * carry a 6-byte prefix - readers strip exactly 6 bytes whenever the SPS bit is
 * set, regardless of content, before seeing the start code. */
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

    /* Presentation time: use the encoder's own PTS (microseconds -> ms) so the
     * clock is a smooth media clock. Timestamping at publish time gave bursty
     * deltas (69/13/13/11 ms) that made FlvPush declare 45->19 fps and caused
     * controller jitter and drops. */
    uint32_t ts = pack->pts ? (uint32_t)(pack->pts / 1000) : now_ms();

    /* Stock rmm re-emits SPS+PPS with every IDR. The VENC GetStream output
     * carries only the slice NALs, so without this the SPS/PPS published once
     * at init age out of the ring and consumers that wait for an SPS to begin
     * (imggrabber) spin forever. */
    if (ch->spspps_len && has_idr(ch->assemble, total))
        publish_annexb(ch, ch->spspps, ch->spspps_len, ts);

    publish_annexb(ch, ch->assemble, total, ts);
    g_last_frame_ms = now_ms();
}

/* Safety net: if the vendor pipeline wedges and stops producing frames, exit
 * rather than sit there holding ion memory (and, being OOM-protected, wedging
 * the whole box). The stock watchdog / our supervisor can restart us. */
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

/* Pull a VI capture frame, run it through our H.264 encoder, and publish the
 * bitstream. This is the vendor "VENC GetStream" thread rewritten to own the
 * whole encode path (see mediad_venc.c) instead of going through the VENC MPI
 * component. PTS/gap handling is kept: a >500 ms input gap forces an IDR so the
 * decoder/recorder resync at the resume point. */
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

        /* Genuine frame-rate throttle. The VI/sensor runs at SRC_FPS; when a
         * lower rate is configured, drop source frames here so the encoder and
         * everything downstream really run at g_fps instead of merely declaring
         * it. The capture PTS keeps the emit points phase-locked to the source,
         * and a late frame resyncs rather than causing a catch-up burst. (The
         * VI/ISP capture rate itself is a separate matter and not throttled by
         * this.) */
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
            /* Publish this frame, then drain any further bitstream units the
             * encoder queued for it. Leaving one occupied starves the
             * bitstream pool -> PutBits error -> the whole encoder dies. */
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
            /* Input pool exhausted: reset the frame/bitstream managers and
             * force an IDR rather than leak the rest of the session. */
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
    /* LBC 2.5X, matching stock rmm: its VI vipps 0/1 and VENC channels use
     * MM_PIXEL_FORMAT_YUV_AW_LBC_2_5X (0x23) (rmm VI attr at 0x21780 writes
     * 0x23; mppVencCfg[3]=0x101 byte1=1), so the frames are captured
     * compressed. Uncompressed NV21 needs ~2x the frame buffers; rmm reserves
     * NV21 for the snapshot/JPEG vipp only. */
    /* MEDIAD_NV21=1: capture uncompressed NV21 instead (A/B against LBC). */
    attr.format.pixelformat = map_PIXEL_FORMAT_E_to_V4L2_PIX_FMT(
        mediad_capture_nv21() ? FWM_MM_PIXEL_FORMAT_YVU_SEMIPLANAR_420
                              : FWM_MM_PIXEL_FORMAT_YUV_AW_LBC_2_5X);
    attr.format.field = V4L2_FIELD_NONE;
    attr.format.width = ch->cap_w;
    attr.format.height = ch->cap_h;
    attr.fps = g_fps;
    attr.nbufs = 3;          /* stock rmm: 3. The VI driver rejects 2 (returns
                              * "too many buffers" -> EnableVipp fails), so 3 is
                              * the floor; 5 used ~9 MB more and caused OOM. */
    attr.nplanes = 2;
    attr.use_current_window = 0;

    /* Sensor-commanding WDR (capturemode=2/wdr_mode=2) matches the camera's own
     * ISP tuning we extract at boot (the vendor profile is WDR; extractor
     * wdr=2), so default it on. If no tuning could be extracted we fall back to
     * the open-SDK linear pipeline - MEDIAD_WDR=0 forces that. */
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
        /* Known sensor: the table geometry should be exact (one try). Unknown
         * sensor: walk a 16:9 candidate list until the driver accepts one. */
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

/* Create our H.264 encoder channel over the VI virchn and cache SPS/PPS. The
 * virchn is NOT enabled here - all virchns are enabled only after every channel
 * is set up. */
static int venc_start(media_chan *ch)
{
    struct mediad_venc_cfg cfg;
    unsigned char hdr[256];
    int n;

    /* HW-encoder quality levers, all in the hardware codec. Defaults mirror
     * stock rmm's own encoder dump (rmm_stdout.log:158/173), not the SDK
     * defaults: profile 100 High, level 32, VBR, i/p_qp [10~40], idr_period 40.
     * (rmm also enables its encoder 3DNR at level 3, but that smears motion, so
     * we leave it off.) Override per-test:
     *   MEDIAD_PROFILE  0 baseline / 1 main / 2 high (default 2 = rmm)
     *   MEDIAD_RC       0 CBR / 1 VBR / 2 AVBR (default 1 = rmm)
     *   MEDIAD_MINQP / MEDIAD_MAXQP  QP floor / cap (default 10 / 40 = rmm)
     *   MEDIAD_CROP_X / MEDIAD_CROP_Y  HIGH: encode a PIC_W x PIC_H window of
     *                   the capture at this offset (input crop, no scaling)
     *   MEDIAD_OUT_W / MEDIAD_OUT_H  HIGH displayed size (SPS crop of the
     *                   encoded picture; default = encoded size)
     *   MEDIAD_GOP      keyframe interval in frames, both channels
     *   MEDIAD_GOP_HIGH / MEDIAD_GOP_LOW  per channel, override MEDIAD_GOP
     *                   (default 5 s HIGH / 1 s LOW: what Protect requests via
     *                   nMultiplier, and a real G3's measured 5.000 s HIGH)
     *   MEDIAD_3DNR     encoder 3D-filter level 0-3 (default 0: rmm's 3 smears)
     *   MEDIAD_FASTENC  encoder fast-encode flag (default 0) */
    {
        const char *e;
        cfg.src_w = ch->cap_w;
        cfg.src_h = ch->cap_h;
        cfg.pic_w = ch->pic_w;
        cfg.pic_h = ch->pic_h;
        cfg.out_w = cfg.out_h = 0;
        cfg.crop_x = cfg.crop_y = -1;
        cfg.fps = g_fps;
        cfg.bitrate = (int)ch->bitrate;
        cfg.gop = g_fps * (strcmp(ch->name, "high") == 0 ? 5 : 1);
        cfg.profile = 2;
        cfg.min_qp = 10;
        cfg.max_qp = 40;
        cfg.rc_mode = 1;      /* VBR */
        cfg.nr3d = 0;         /* rmm runs 3DNR at level 3 but it smears motion */
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

/* Build stamp.  Defaults to the compiler's __DATE__/__TIME__; a reproducible
 * build passes -DMEDIAD_BUILD_STAMP=<no-spaces string> so the shipped binary is
 * byte-identical across build hosts (package.sh sets it to the source revision
 * date).  A stamp with spaces cannot survive EXTRA_CFLAGS, hence the single
 * token. */
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
    /*
     * Never let a dead peer kill the daemon. The control socket and the web UI
     * both write replies to a client fd (isp_control.c handle_conn/http_reply);
     * the goclient uses a 1 s deadline and closes the connection on timeout
     * (mediad_ctl.go mediadCommand), so a slow reply -- e.g. while the ISP/VI
     * pipeline is busy -- turns the next write() into EPIPE, whose default
     * disposition is SIGPIPE (process death, rc 141). Ignoring it makes those
     * writes fail with EPIPE/-1 instead, which every call site already handles
     * or ignores.
     */
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

    /* 1. all vipps, THEN ISP_Run: the ISP's sensor subdev is only initialized
     *    once a vipp has configured the sensor, so running the ISP first fails
     *    with "unable to initialize sensor subdev" and the encoder gets black.
     * 2. virchn, 3. VENCs bound+started, 4. enable virchn.
     * This is the proven order (the original single-channel mediad also did
     * vi_start before ISP_Run); stock rmm gets there via its JPEG vipp. */
    for (i = 0; i < nc; i++) {
        if (vi_start(&g_chans[i]) < 0)
            return 1;
    }

    AW_MPI_ISP_Run(isp_dev);

    /* Mounting orientation (e.g. r35gb is rotated 180): the capability table is
     * the BASE; Protect's mirror/flip toggle is applied relative to it. */
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

    /* Phase 1b control surface: avclientd connects here and forwards Protect's
     * picture settings. Failure is non-fatal (video continues without it). */
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
        /* Desktop talkback: read /tmp/audio_in_fifo and play via ALSA.
         * Independent of the capture direction above. */
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

    /* Everything is allocated by now; pin mediad in RAM before the steady
     * state so the page reclaimer can never swap it out. */
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
