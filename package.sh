#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors
#
# package.sh - build mediad and assemble the SD-card package into dist/.
#
# The result is an overlay for an existing yi-protect SD card: it adds mediad
# plus the scripts that make it run in place of the stock `rmm`. Copy dist/ to
# the card's /tmp/sd/unifi (or run dist/install-mediad.sh on the device).
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$REPO"

[ -d repos/lindenis-v536-prebuilt ] || {
    echo "== dependencies not fetched; running build.sh first"
    ./build.sh
}

TC=repos/lindenis-v536-prebuilt/gcc/linux-x86/arm/toolchain-sunxi-musl/toolchain/bin
STRIP="$TC/arm-openwrt-linux-muslgnueabi-strip"

# Deploy build: clean-room ISP tier (ALGO_RTOS=1), clean-room H.264 encoder
# (FREECODEC_H264=1, which also selects the clean-room libvenc_base.so), and the
# clean-room MPP middleware (FENC/FISP/FCAP/HEADERS=1).  The last four drop every
# vendor eyesee-mpp object and header from the build, so the shipped binary is
# ours alone.  Needs the freewinner sibling checkout (see
# work/media_daemon/Makefile, SIBLINGS).
#
# Reproducible build stamp: main.c prints __DATE__/__TIME__ unless overridden,
# so without this every build differs by the compile clock (measured: two clean
# builds differed by exactly 2 bytes in .data).
#
# The stamp is derived from the CONTENT of the inputs that build the artifact,
# not from the commit that happens to be HEAD -- a commit-date stamp is
# self-invalidating (committing the build changes the stamp and so changes the
# artifact).  Hash the build inputs only, so editing an unrelated doc (or a
# comment in this script outside these paths) cannot change the artifact.
#
# Space-free: a value with spaces does not survive the shell->make->shell trip
# through EXTRA_CFLAGS.
REV=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo dev)
STAMP_INPUTS="work/media_daemon package package.sh"
SRC_HASH=$(git -C "$REPO" ls-files -z -- $STAMP_INPUTS 2>/dev/null \
    | xargs -0 -r sha1sum 2>/dev/null | sha1sum | cut -c1-12)
[ -n "$SRC_HASH" ] || SRC_HASH=000000000000
STAMP="src-$SRC_HASH"

echo "== building mediad (clean build: ALGO_RTOS=1 FREECODEC_H264=1 FENC/FISP/FCAP/HEADERS=1; rev $REV stamp $STAMP)"
# -W main.c: the stamp is a -D flag, which make's dependency tracking cannot
# see, so recompile main.c (the only user) every time; otherwise an old stamp
# survives and dist/'s binary and SOURCE.txt disagree.
make -C work/media_daemon -W main.c BUILD=build-rtos-v TARGET=mediad_rtos_v ALGO_RTOS=1 \
    FREECODEC_H264=1 FREECODEC_FENC=1 FREECODEC_FISP=1 FREECODEC_FCAP=1 FREECODEC_HEADERS=1 \
    EXTRA_CFLAGS="-DMEDIAD_BUILD_STAMP='\"$STAMP\"'"
grep -q "$STAMP" work/media_daemon/mediad_rtos_v || \
    { echo "ERROR: built mediad does not carry stamp $STAMP"; exit 1; }

echo "== assembling dist/"
rm -rf dist
mkdir -p dist/unifi/bin dist/unifi/etc dist/unifi/script dist/unifi/lib
cp work/media_daemon/mediad_rtos_v dist/unifi/bin/mediad
"$STRIP" dist/unifi/bin/mediad 2>/dev/null || true
cp package/unifi/etc/mediad.conf dist/unifi/etc/mediad.conf
cp package/unifi/etc/mediad.*.env dist/unifi/etc/
cp package/unifi/script/mediad.sh dist/unifi/script/mediad.sh
cp package/install.sh dist/install-mediad.sh
cp package/README.md dist/README.md
# Licence notices that must travel with the binaries (mediad embeds the
# Terminus OSD font data: SIL OFL 1.1 requires its licence alongside).
mkdir -p dist/licenses
cp LICENSE LICENSE-EXCEPTION NOTICE dist/licenses/
cp work/media_daemon/fonts/OFL-Terminus.txt dist/licenses/OFL-Terminus.txt
cp repos/faac/COPYING dist/licenses/COPYING-FAAC
# Corresponding source for this exact build (GPL-3.0 / AGPL-3.0 / LGPL-2.1).
FW_DIR="$REPO/../../freewinner/freewinner-git"
fw_rev=$(git -C "$FW_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
fw_url=$(git -C "$FW_DIR" remote get-url origin 2>/dev/null || echo "(not yet published)")
md_rev=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)
faac_ref=$(sed -n 's/^FAAC_REF="\${FAAC_REF:-\([0-9a-f]*\)}"$/\1/p' build.sh)
md_url=$(git -C "$REPO" remote get-url origin 2>/dev/null || echo "(not yet published)")
dirty=""
git -C "$REPO" diff --quiet HEAD -- 2>/dev/null || dirty=" (+ uncommitted changes)"
git -C "$FW_DIR" diff --quiet HEAD -- 2>/dev/null || dirty="$dirty (freewinner: + uncommitted changes)"
cat > dist/licenses/SOURCE.txt <<SRC
Corresponding source for this mediad build (build stamp $STAMP)
===============================================================

mediad and the freewinner tier it links are AGPL-3.0-only (with the section 7
permission in LICENSE-EXCEPTION); FAAC is LGPL-2.1-or-later. The complete
corresponding source is:

  yi-mediad   $md_url
              commit $md_rev
  freewinner  $fw_url
              commit $fw_rev
  FAAC        https://github.com/knik0/faac
              commit ${faac_ref:-unknown} (pinned in build.sh)
$dirty
Rebuild with ./build.sh (fetches the toolchain and FAAC) then ./package.sh.
The licence texts are in this directory.
SRC

# libvenc_base.so (clean-room, freewinner codec/src/base) is the only shared
# library mediad needs besides the camera's own C/C++ runtime and ALSA; it is
# found through the $ORIGIN/../lib rpath. No vendor encoder libraries ship.
cp work/media_daemon/build-rtos-v/libvenc_base.so dist/unifi/lib/libvenc_base.so
"$STRIP" dist/unifi/lib/libvenc_base.so 2>/dev/null || true
# vin_crop_shim.so: LD_PRELOAD VIPP crop (VIDIOC_S_SELECTION) for models whose
# capture margin is garbage; enabled per model by mediad.<model>.env (r35gb).
"$TC/arm-openwrt-linux-muslgnueabi-gcc" -O2 -Wall -fPIC -shared \
    -o dist/unifi/lib/vin_crop_shim.so work/vin_crop_shim/vin_crop_shim.c -ldl
"$STRIP" dist/unifi/lib/vin_crop_shim.so 2>/dev/null || true
chmod 0755 dist/unifi/bin/mediad dist/unifi/script/mediad.sh dist/install-mediad.sh dist/unifi/lib/*.so

echo "== dist/ ready:"
find dist -type f | sort
