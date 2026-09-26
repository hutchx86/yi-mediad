#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors
#
# install.sh - overlay mediad onto an existing yi-protect SD card.
#
# It installs the mediad binary/conf/launcher, points the client's IS_MEDIAD flag
# at it, and adjusts the two lines of the yi-protect init/watchdog that deal with
# the stock encoder (`rmm` -> mediad). Everything it edits is backed up first.
#
# Usage:  ./install.sh [SD_ROOT]        (default /tmp/sd)
#
# Run it on the device (with the SD card mounted) or against a mounted card.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SD="${1:-/tmp/sd}"
PREFIX="$SD/unifi"
INIT="$PREFIX/script/init.sh"
WATCHDOG="$PREFIX/script/watchdog.sh"
CFG="$PREFIX/etc/unifi.cfg"

say() { printf '== %s\n' "$*"; }
warn() { printf '** %s\n' "$*" >&2; }

[ -x "$HERE/unifi/bin/mediad" ] || {
    warn "package/unifi/bin/mediad is missing."
    warn "Build it first, from the repo root:  ./package.sh"
    exit 1
}
[ -d "$PREFIX/script" ] || { warn "no yi-protect instance at $PREFIX"; exit 1; }

say "install binary / conf / launcher"
install -d "$PREFIX/bin" "$PREFIX/etc" "$PREFIX/lib"
install -m 0755 "$HERE/unifi/bin/mediad"        "$PREFIX/bin/mediad"
install -m 0755 "$HERE/unifi/script/mediad.sh"  "$PREFIX/script/mediad.sh"
# libvenc_base.so; mediad resolves it via its $ORIGIN/../lib rpath.
if [ -d "$HERE/unifi/lib" ]; then
    install -m 0755 "$HERE/unifi/lib/"*.so "$PREFIX/lib/"
    echo "  installed $(ls "$HERE/unifi/lib/"*.so | wc -l) shared lib(s)"
else
    warn "no $HERE/unifi/lib - mediad will fail to load libvenc_base.so"
fi
if [ -f "$PREFIX/etc/mediad.conf" ]; then
    echo "  kept existing mediad.conf"
else
    install -m 0644 "$HERE/unifi/etc/mediad.conf" "$PREFIX/etc/mediad.conf"
    echo "  installed default mediad.conf"
fi

say "unifi.cfg: IS_MEDIAD=yes"
if [ -f "$CFG" ]; then
    if grep -q '^IS_MEDIAD=' "$CFG"; then
        sed -i 's/^IS_MEDIAD=.*/IS_MEDIAD=yes/' "$CFG"
    else
        printf 'IS_MEDIAD=yes\n' >> "$CFG"
    fi
    echo "  set ($CFG)"
else
    warn "no $CFG - set IS_MEDIAD=yes yourself so the client forwards controls"
fi

say "init.sh: launch mediad instead of the stock rmm"
if [ -f "$INIT" ] && grep -q 'mediad' "$INIT"; then
    echo "  already mediad-aware - no patch needed"
elif [ -f "$INIT" ] && grep -q '^\./rmm > /tmp/rmm\.log 2>&1 &$' "$INIT"; then
    cp -a "$INIT" "$INIT.pre-mediad"
    sed -i 's#^\./rmm > /tmp/rmm\.log 2>&1 &$#"$UNIFI_PREFIX/script/mediad.sh" start#' "$INIT"
    echo "  patched (backup: $INIT.pre-mediad)"
else
    warn "could not find the stock-rmm launch line in $INIT."
    warn "Replace it manually with:  \"\$UNIFI_PREFIX/script/mediad.sh\" start"
fi

say "watchdog.sh: watch mediad instead of rmm"
if [ -f "$WATCHDOG" ] && grep -q 'mediad' "$WATCHDOG"; then
    echo "  already mediad-aware - no patch needed"
elif [ -f "$WATCHDOG" ] && grep -q "alive './rmm'" "$WATCHDOG"; then
    cp -a "$WATCHDOG" "$WATCHDOG.pre-mediad"
    sed -i "s#alive './rmm'#alive 'mediad'#" "$WATCHDOG"
    echo "  patched (backup: $WATCHDOG.pre-mediad)"
else
    warn "could not find 'alive ./rmm' in $WATCHDOG."
    warn "Change the encoder check to:  alive 'mediad'"
fi

say "done"
echo "  Reboot, or:  $PREFIX/script/mediad.sh start"
echo "  (Stop the stock rmm first if it is running.)"
