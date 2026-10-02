#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors

# package.sh - build mediad and assemble dist/, an overlay for an existing
# yi-protect SD card (see package/README.md).
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$REPO"

[ -d repos/lindenis-v536-prebuilt ] || {
    echo "== dependencies not fetched; running build.sh first"
    ./build.sh
}

TC=repos/lindenis-v536-prebuilt/gcc/linux-x86/arm/toolchain-sunxi-musl/toolchain/bin
STRIP="$TC/arm-openwrt-linux-muslgnueabi-strip"

# Reproducible build stamp: a hash of the build inputs' content (not HEAD), so
# unrelated edits leave the binary unchanged. Space-free for EXTRA_CFLAGS.
REV=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo dev)
STAMP_INPUTS="work/media_daemon package package.sh"
SRC_HASH=$(git -C "$REPO" ls-files -z -- $STAMP_INPUTS 2>/dev/null \
    | xargs -0 -r sha1sum 2>/dev/null | sha1sum | cut -c1-12)
[ -n "$SRC_HASH" ] || SRC_HASH=000000000000
STAMP="src-$SRC_HASH"

echo "== building mediad (clean build: ALGO_RTOS=1 FREECODEC_H264=1 FENC/FISP/FCAP/HEADERS=1; rev $REV stamp $STAMP)"
# The deploy flags (also the Makefile defaults). -W main.c: make cannot see a
# changed -D stamp, so always recompile its only user.
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
for env in package/unifi/etc/mediad.*.env; do
    [ -f "$env" ] && cp "$env" dist/unifi/etc/
done
cp package/unifi/script/mediad.sh dist/unifi/script/mediad.sh
cp package/install.sh dist/install-mediad.sh
cp package/README.md dist/README.md
# Licences that travel with the binaries (the OSD font is SIL OFL 1.1).
mkdir -p dist/licenses
cp LICENSE LICENSE-EXCEPTION NOTICE dist/licenses/
cp work/media_daemon/fonts/OFL-Terminus.txt dist/licenses/OFL-Terminus.txt
cp repos/faac/COPYING dist/licenses/COPYING-FAAC
# Corresponding source for this exact build (GPL-3.0 / AGPL-3.0 / LGPL-2.1).
# Same resolution as the Makefile's FW_ROOT: the submodule when checked out.
if [ -f "$REPO/freewinner/isp/Makefile" ]; then
    FW_DIR="$REPO/freewinner"
else
    FW_DIR="$REPO/../../freewinner/freewinner-git"
fi
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

# libvenc_base.so (clean-room) is mediad's only shared library besides the
# camera's C/C++ runtime and ALSA; found via the $ORIGIN/../lib rpath.
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
