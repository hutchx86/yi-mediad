#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors
#
# build.sh - fetch and install everything needed to build `mediad`.
#
# Our own source is in work/media_daemon/. This script brings in the third-party
# pieces the build links (see NOTICE): the musl cross toolchain, the Allwinner
# V833 SDK (which also carries the ISP-3A / hardware-encoder / AAC prebuilts),
# the Melis-RTOS ISP algorithm archive used by the deploy (ALGO_RTOS) build, and
# a link-time libasound.so. It lands in ./repos/ and
# work/media_daemon/prebuilt/, both gitignored.
#
# Note on licensing: the fetched Allwinner binaries (ISP 3A, encoder engine, AAC)
# are closed-source blobs with no license grant. They are acquired here for
# interoperability; they are not redistributed by this repository. See NOTICE.
#
# Usage:
#   ./build.sh [-h] [--repos DIR] [--build]
# Env:
#   ASOUND_LIB=/path/to/libasound.so   skip the alsa-lib build and use this file
#   SDK_REF=<git ref>                  SDK commit/branch (default: pinned below)
#   TC_REF=<git ref>                   toolchain commit/branch (default: pinned)
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEPS="$REPO_ROOT/repos"
PREBUILT="$REPO_ROOT/work/media_daemon/prebuilt"
PATCHES="$REPO_ROOT/work/media_daemon/patches"

# Pinned refs the tree is known to build against (verified 2026-09-15).
TC_REPO="https://github.com/lindenis-org/lindenis-v536-prebuilt.git"
TC_REF="${TC_REF:-2f0d7ef75aff64b7f6d008319b6490c3dc944288}"
SDK_REPO="https://github.com/lindenis-org/lindenis-v833-softwinner.git"
SDK_REF="${SDK_REF:-834a5afe83ec037a38ed0dbed522ff65439eabdf}"
RTOS_COMMIT="71ab8faeee960e27ef83a9ff2e4ae4b7f33eda20"
RTOS_RAW="https://raw.githubusercontent.com/lindenis-org/lindenis-v833-RTOS-melis-4.0/$RTOS_COMMIT/source/ekernel/subsys/avframework/eyesee-mpp/middleware/sun8iw19p1/media/LIBRARY/libisp/out/libisp_algo.a"
ALSA_VER="1.1.4.1"
ALSA_TARBALL="https://www.alsa-project.org/files/pub/lib/alsa-lib-$ALSA_VER.tar.bz2"

DO_BUILD=no
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --repos) shift; DEPS="$1" ;;
        --build) DO_BUILD=yes ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

say "checking host tools"
for t in git curl make python3 patch tar; do
    have "$t" || die "missing required tool: $t"
done
mkdir -p "$DEPS" "$PREBUILT"

# freewinner (clean-room ISP + codec) is a git submodule at ./freewinner, pinned
# to the commit this tree was built against; a plain `git clone` leaves it
# empty. Without .gitmodules (a development checkout) the Makefile uses the
# sibling workspace instead.
if [ -f "$REPO_ROOT/.gitmodules" ] && [ ! -f "$REPO_ROOT/freewinner/isp/Makefile" ]; then
    say "freewinner submodule"
    git -C "$REPO_ROOT" submodule update --init freewinner
fi

say "cross toolchain ($TC_REPO @ ${TC_REF%${TC_REF#???????}})"
TC_DIR="$DEPS/lindenis-v536-prebuilt"
if [ -d "$TC_DIR/.git" ]; then
    echo "  already present: $TC_DIR"
else
    git clone "$TC_REPO" "$TC_DIR"
    git -C "$TC_DIR" checkout -q "$TC_REF"
fi
TC_BIN="$TC_DIR/gcc/linux-x86/arm/toolchain-sunxi-musl/toolchain/bin"
[ -x "$TC_BIN/arm-openwrt-linux-muslgnueabi-gcc" ] || \
    die "toolchain not found at $TC_BIN"

say "Allwinner V833 SDK ($SDK_REPO)"
SDK_DIR="$DEPS/lindenis-v833-softwinner"
if [ -d "$SDK_DIR/.git" ]; then
    echo "  already present: $SDK_DIR"
else
    # Sparse: the build only needs eyesee-mpp/{middleware/sun8iw19p1,system/public}.
    git clone --depth 1 --filter=blob:none --sparse "$SDK_REPO" "$SDK_DIR"
    git -C "$SDK_DIR" checkout -q "$SDK_REF" 2>/dev/null || true
    git -C "$SDK_DIR" sparse-checkout set \
        eyesee-mpp/middleware/sun8iw19p1 eyesee-mpp/system/public
fi
[ -d "$SDK_DIR/eyesee-mpp/middleware/sun8iw19p1/media" ] || \
    die "SDK middleware missing at $SDK_DIR/eyesee-mpp/middleware/sun8iw19p1/media"

say "SDK patch: v833-rtos-algo-ctx-lock (needed by the ALGO_RTOS build)"
ISP_MANAGE="$SDK_DIR/eyesee-mpp/middleware/sun8iw19p1/media/LIBRARY/libisp/include/isp_manage.h"
if grep -q "_ctx_lock_pad" "$ISP_MANAGE" 2>/dev/null; then
    echo "  already applied"
elif patch -p1 -d "$SDK_DIR" --forward < "$PATCHES/v833-rtos-algo-ctx-lock.patch"; then
    echo "  applied"
else
    echo "  WARNING: patch did not apply; the ALGO_RTOS build will likely fail"
fi

say "FAAC source (freecodec AAC backend) -> $DEPS/faac"
FAAC_SRC_DIR="$DEPS/faac"
FAAC_REPO="https://github.com/knik0/faac"
FAAC_REF="${FAAC_REF:-c3e082cb861923484b7ae1bd5416038176edb305}"
if [ -f "$FAAC_SRC_DIR/include/faac.h" ] && [ -d "$FAAC_SRC_DIR/libfaac" ]; then
    echo "  already present: $FAAC_SRC_DIR"
else
    git clone "$FAAC_REPO" "$FAAC_SRC_DIR"
    git -C "$FAAC_SRC_DIR" checkout -q "$FAAC_REF" || \
        die "FAAC ref $FAAC_REF not found"
fi

say "Melis-RTOS ISP algorithm archive -> prebuilt/libisp_algo_rtos.a"
if [ -s "$PREBUILT/libisp_algo_rtos.a" ]; then
    echo "  already present ($(wc -c < "$PREBUILT/libisp_algo_rtos.a") bytes)"
else
    curl -fL --retry 3 -o "$PREBUILT/libisp_algo_rtos.a" "$RTOS_RAW" || \
        die "failed to download the RTOS ISP algorithm archive"
    echo "  $(wc -c < "$PREBUILT/libisp_algo_rtos.a") bytes"
fi

say "link-time libasound.so -> prebuilt/libasound.so"
if [ -s "$PREBUILT/libasound.so" ]; then
    echo "  already present"
elif [ -n "${ASOUND_LIB:-}" ]; then
    cp "$ASOUND_LIB" "$PREBUILT/libasound.so"
    echo "  copied from ASOUND_LIB ($ASOUND_LIB)"
else
    # Build alsa-lib for arm-musl; used only at link time - the camera ships its
    # own libasound.so.2 at runtime. (Set ASOUND_LIB= to skip this and use the
    # device's own library instead.)
    echo "  building alsa-lib $ALSA_VER (needs a host toolchain; ~1 min)"
    ALSA_DIR="$DEPS/alsa-lib"
    P=arm-openwrt-linux-muslgnueabi
    mkdir -p "$ALSA_DIR"
    [ -f "$ALSA_DIR/alsa-lib-$ALSA_VER.tar.bz2" ] || \
        curl -fL --retry 3 -o "$ALSA_DIR/alsa-lib-$ALSA_VER.tar.bz2" "$ALSA_TARBALL"
    [ -d "$ALSA_DIR/alsa-lib-$ALSA_VER" ] || \
        tar xjf "$ALSA_DIR/alsa-lib-$ALSA_VER.tar.bz2" -C "$ALSA_DIR"
    (
        cd "$ALSA_DIR/alsa-lib-$ALSA_VER"
        # --host=arm-linux + explicit tools: config.sub rejects the
        # "-muslgnueabi" suffix, and its default tool prefix differs.
        env CC="$TC_BIN/$P-gcc" AR="$TC_BIN/$P-ar" \
            RANLIB="$TC_BIN/$P-ranlib" STRIP="$TC_BIN/$P-strip" \
            ./configure --host=arm-linux --target=arm-linux \
                --disable-aload --disable-topology --disable-ucm --disable-alisp \
                --disable-python --without-debug --with-versioned=no
        env CC="$TC_BIN/$P-gcc" AR="$TC_BIN/$P-ar" \
            RANLIB="$TC_BIN/$P-ranlib" STRIP="$TC_BIN/$P-strip" \
            make -j"$(nproc 2>/dev/null || echo 2)"
    ) || die "alsa-lib build failed; set ASOUND_LIB=/path/to/libasound.so and re-run"
    cp "$ALSA_DIR/alsa-lib-$ALSA_VER/src/.libs/libasound.so.2.0.0" "$PREBUILT/libasound.so"
    echo "  $(wc -c < "$PREBUILT/libasound.so") bytes"
fi

say "done"
echo "  toolchain : $TC_DIR"
echo "  SDK       : $SDK_DIR"
echo "  prebuilt  : $PREBUILT"
echo
echo "Build:"
echo "  make -C work/media_daemon"
echo "  make -C work/media_daemon BUILD=build-rtos-v TARGET=mediad_rtos_v ALGO_RTOS=1 FREECODEC_H264=1 FREECODEC_FENC=1 FREECODEC_FISP=1 FREECODEC_FCAP=1 FREECODEC_HEADERS=1   # clean deploy build"

if [ "$DO_BUILD" = yes ]; then
    say "building (default target)"
    make -C "$REPO_ROOT/work/media_daemon"
fi
