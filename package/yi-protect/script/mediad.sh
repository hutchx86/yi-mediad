#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors

# mediad.sh {start|stop|restart|status|candidate FILE} - run mediad under a
# rollback guard (bin/mediad.known-good) and a boot latch; see package/README.md.

YIP_PREFIX="${YIP_PREFIX:-/tmp/sd/yi-protect}"
MEDIAD="$YIP_PREFIX/bin/mediad"
CONF="$YIP_PREFIX/etc/mediad.conf"
PIDFILE=/tmp/mediad.pid
GOOD="$YIP_PREFIX/bin/mediad.known-good"
LATCH="$YIP_PREFIX/.boot-pending"
ROLLBACK_SECS="${MEDIAD_ROLLBACK_SECS:-60}"
RING=/dev/shm/fshare_frame_buf
BOOTLOG=/tmp/sd/mediad-boot.log
SEQ=/tmp/sd/mediad-pass.seq

log() {
    m=$(printf '%s [up %s] mediad.sh: %s' "$(date +'%H:%M:%S')" "$(cut -d' ' -f1 /proc/uptime)" "$*")
    echo "$m"
    echo "$m" >> "$BOOTLOG" 2>/dev/null
}

pid_of() {
    [ -f "$PIDFILE" ] || return 1
    p=$(cat "$PIDFILE" 2>/dev/null)
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    [ -d "/proc/$p" ] || return 1
    echo "$p"
}

running() { pid_of >/dev/null 2>&1; }

# The ring word at +4 advances every frame; busybox has no od/xxd, so hash it.
ring_sample() {
    # Absent ring -> empty, so a ring created between samples is not "flowing".
    [ -f "$RING" ] || return 0
    dd if="$RING" bs=1 skip=4 count=4 2>/dev/null | md5sum | cut -d' ' -f1
}
# Always sleeps, so a polling caller spends real time in the rollback window.
frames_flowing() {
    a=$(ring_sample)
    sleep 3
    b=$(ring_sample)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ]
}

# Start $MEDIAD once. 0 = frames flowing, 1 = daemon exited, 2 = alive but silent.
run_pass() {
    [ -f "$CONF" ] && MEDIAD_CONF="$CONF"
    export MEDIAD_CONF
    # Optional per-deploy knobs: etc/mediad.env, `export KEY=value` lines
    # (docs/env.md).
    [ -f "$YIP_PREFIX/etc/mediad.env" ] && . "$YIP_PREFIX/etc/mediad.env"
    # Per-model overrides (etc/mediad.<model>.env, model from etc/model_suffix),
    # loaded last so model-specific facts win over per-deploy knobs.
    model=$(cat "$YIP_PREFIX/etc/model_suffix" 2>/dev/null)
    [ -n "$model" ] && [ -f "$YIP_PREFIX/etc/mediad.$model.env" ] && . "$YIP_PREFIX/etc/mediad.$model.env"
    # Per-pass log on the SD, numbered by a counter (no clock before timesync).
    n=$(cat "$SEQ" 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0 ;; esac
    n=$((n + 1)); echo "$n" > "$SEQ"
    LOG="/tmp/sd/mediad-pass-$n.log"
    ( cd /home/app 2>/dev/null || true; exec "$MEDIAD" ) > "$LOG" 2>&1 &
    p=$!
    echo $p > "$PIDFILE"
    [ -d "/proc/$p" ] && echo -1000 > "/proc/$p/oom_score_adj" 2>/dev/null
    log "started pid $p (log $LOG)"
    i=0
    while [ "$i" -lt "$ROLLBACK_SECS" ]; do
        if frames_flowing; then rm -f "$LATCH"; sync; log "pid $p frames flowing"; return 0; fi
        i=$((i + 3))
        if ! running; then log "pid $p exited"; return 1; fi
    done
    log "pid $p alive, no frames after ${ROLLBACK_SECS}s"
    return 2
}

stop() {
    p=$(pid_of) && { kill "$p" 2>/dev/null; sleep 1; kill -9 "$p" 2>/dev/null; }
    rm -f "$PIDFILE"
    log "stopped"
}

start() {
    # `start force` (from candidate): skip the latch restore so the candidate runs.
    force="$1"
    running && { log "already running (pid $(pid_of))"; return 0; }
    [ -x "$MEDIAD" ] || { log "not found: $MEDIAD"; return 1; }

    # A pending latch means the previous boot never saw frames: the current
    # bin/mediad is unproven (likely wedged the box), so fall back before trying.
    if [ -x "$GOOD" ] && [ -f "$LATCH" ] && [ "$force" != "force" ]; then
        log "boot latch: last attempt never produced frames; restoring known-good"
        cp -f "$GOOD" "$MEDIAD"
    fi
    : > "$LATCH"
    sync

    if [ "${MEDIAD_NO_ROLLBACK:-}" = "1" ]; then
        run_pass; rc=$?
        [ "$rc" = 0 ] && return 0
        log "no frames within ${ROLLBACK_SECS}s (rollback disabled)"
        return 1
    fi

    if [ ! -x "$GOOD" ]; then                 # first boot: seed the snapshot
        run_pass; rc=$?
        if [ "$rc" = 0 ]; then
            cp -f "$MEDIAD" "$GOOD"
            log "healthy; seeded known-good snapshot"
            return 0
        fi
        log "no frames within ${ROLLBACK_SECS}s and no known-good to fall back to"
        return 1
    fi

    run_pass; rc=$?
    [ "$rc" = 0 ] && return 0

    if [ "$rc" = 2 ]; then
        # Alive but silent: leave it running. It may just be slow (cold boot),
        # and killing a media daemon mid-init can wedge the VI/ISP.
        log "daemon alive but silent; leaving it running (window ${ROLLBACK_SECS}s)"
        return 1
    fi

    log "daemon exited; ROLLING BACK to known-good"
    stop
    cp -f "$GOOD" "$MEDIAD"
    run_pass; rc=$?
    [ "$rc" = 0 ] && { log "rollback OK; now running known-good"; return 0; }
    log "known-good did not produce frames within ${ROLLBACK_SECS}s (rc=$rc)"
    return 1
}

# Install a candidate under the guard. If the current daemon is healthy its
# binary becomes the known-good snapshot first.
candidate() {
    src="$1"
    [ -n "$src" ] && [ -r "$src" ] || { log "candidate: no such file: $src"; return 2; }
    if running && frames_flowing; then
        cp -f "$MEDIAD" "$GOOD"
        log "snapshotted running binary as known-good"
    fi
    stop
    cp -f "$src" "$MEDIAD"
    log "installed candidate $(md5sum "$MEDIAD" 2>/dev/null | cut -d' ' -f1)"
    # Arm the latch so a candidate that hangs the box is abandoned next boot.
    : > "$LATCH"
    sync
    start force
}

case "$1" in
    start)     start ;;
    stop)      stop ;;
    restart)   stop; start ;;
    status)    running && exit 0 || exit 1 ;;
    candidate) candidate "$2" ;;
    *) echo "usage: $0 {start|stop|restart|status|candidate <file>}" >&2; exit 2 ;;
esac
