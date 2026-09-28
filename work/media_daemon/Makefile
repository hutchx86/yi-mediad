# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors
#
# Makefile for the rmm_replacement VI->VENC prototype.
#
# Hand-written (NOT derived from the tina.mk/build.mk build-system files in the
# vendor tree - those assume the full tina build harness we don't have). It
# invokes the toolchain-sunxi-musl cross-gcc directly.
#
# Scope: cross-compile + link only. This is never run locally (target is armhf
# musl).
#
# Most vendor middleware (mpi_vi.c, mpi_venc.c, mpi_isp.c, mpi_sys.c, the ISP
# driver layer under libisp/) ships as source in this tree, so the non-deploy
# default build compiles it; the package.sh deploy build compiles none of it.
#
# What is linked from the vendor side: nothing, in the package.sh deploy build.
#   - the H.264 encoder is the clean-room freecodec engine (static objects) with
#     the clean-room libvenc_base.so (codec/src/base); no vendor encoder blob.
#   - the AAC-LC encoder is our own freewinner/freecodec-git (FAAC backend).
#   - the ISP 3A tier is the freewinner clean-room build (ALGO_RTOS=1), and the
#     libisp framework tier is the freewinner clean-room replacement too
#     (CLEAN_FW_SRC); the vendor SRC_ISP files are dropped.

PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
MD := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

TC := $(PROJECT_ROOT)/repos/lindenis-v536-prebuilt/gcc/linux-x86/arm/toolchain-sunxi-musl/toolchain
CC := $(TC)/bin/arm-openwrt-linux-muslgnueabi-gcc
CXX := $(TC)/bin/arm-openwrt-linux-muslgnueabi-g++
AR := $(TC)/bin/arm-openwrt-linux-muslgnueabi-gcc-ar
OBJCOPY := $(TC)/bin/arm-openwrt-linux-muslgnueabi-objcopy
NM := $(TC)/bin/arm-openwrt-linux-muslgnueabi-nm

SW := $(PROJECT_ROOT)/repos/lindenis-v833-softwinner/eyesee-mpp
MW := $(SW)/middleware/sun8iw19p1
MEDIA := $(MW)/media
LIB := $(MEDIA)/LIBRARY
CEDARC := $(LIB)/libcedarc

# ---------------------------------------------------------------------------
# Clean-room ISP tier (the freewinner project, a sibling checkout).
#
# The six 3A modules (AE/AWB/AFS/ISO/GTM/PLTM) and the register/config/base tier
# (reg_writers.c + module_cfg.c + base.c/comp_ref.c, each through its SDK-ABI
# shim, A8/A9/A10) are rebuilt from clean-room sources and linked ahead of the
# vendor archive; the archive then contributes nothing (empty).  See
# $FREEWINNER/shim/INTEGRATION.md.
#
# FREEWINNER defaults to the sibling checkout's isp/ subtree (its src/, shim/
# and include/ live there); override to point elsewhere.
# ---------------------------------------------------------------------------
# Sibling projects (freewinner/freecodec) live beside the workspace dir that
# contains yi-mediad-git. Derive from MD (the Makefile's own dir, not overridable)
# so this still works when PROJECT_ROOT is overridden (e.g. the CLAUDE.md
# verification command passes PROJECT_ROOT=$PWD).
SIBLINGS  ?= $(abspath $(MD)/../../../..)
FREEWINNER ?= $(SIBLINGS)/freewinner/freewinner-git/isp
# Our AAC backend (freewinner/freecodec-git); its headers are on INC below.
FREECODEC_DIR ?= $(SIBLINGS)/freewinner/freecodec-git
FW_GIT    := $(FREEWINNER)
SHIM_DIR  := $(FW_GIT)/shim
CSRC_DIR  := $(FW_GIT)/src
CINC_DIR  := $(FW_GIT)/include
INTEG_DIR := $(SHIM_DIR)/integration

# Bring-up knob: for each module listed here, keep its vendor archive member and
# rename the clean shim's SDK entry points so the framework binds the vendor
# one, instead of the clean 3A module.  Space-separated subset of
#   iso ae awb afs gtm pltm
# Empty (default) = all six clean.  Used to bisect which clean module diverges.
VENDOR_3A ?=

# Bring-up knob: 1 = keep the vendor register/config tier (isp_module_cfg.o,
# isp521_reg_cfg.o, isp_base.o) and do not build the clean reg tier.  Default 0
# = clean reg tier (A9/A10).  Finer knobs: VENDOR_MODCFG=1 keeps only
# isp_module_cfg.o/isp521_reg_cfg.o (clean base still built); VENDOR_BASE=1 keeps
# only isp_base.o (clean module_cfg/reg_writers still built).
VENDOR_REG ?=
VENDOR_MODCFG ?=
VENDOR_BASE ?=

# Bring-up knob: 1 = keep the nine vendor libisp framework files (SRC_ISP); the
# default builds the clean-room framework instead.  A/B knob for the on-camera
# framework differential (see freewinner/on-camera-differential.md).
VENDOR_FW ?=

# Bring-up knob: 1 = keep the vendor media/utils files (SRC_MEDIA_UTILS); the
# default builds the clean-room media-utils instead (freewinner
# media-utils-tier.md section 7).  A/B knob for the on-camera media-utils
# differential.  The clean utils stay 4-byte-enum (never -fshort-enums).
VENDOR_MEDIA_UTILS ?=

# Bring-up knob: 1 = replace the vendor H.264 encoder blobs (libvenc_codec.so +
# libVE.so) with the freewinner/freecodec-git clean objects.  Default 0 keeps
# the vendor blobs.  Our objects provide GetVeOpsS + video_encoder_h264_ver2 and
# use libvenc_base.so for the cedarc glue.  The deploy build (package.sh) sets
# this to 1, so libvenc_base.so is the clean-room build too (see
# Deploy defaults: a bare `make` builds the clean-room binary (same flags as
# package.sh).  Each knob stays overridable, e.g. `make FREECODEC_H264=` for
# the vendor-encoder comparison build.
ALGO_RTOS ?= 1

# FREECODEC_VENC_BASE below).
FREECODEC_H264 ?= 1

# 1 = libvenc_base.so from the clean-room freewinner build (codec/src/base)
# instead of the SDK cedarc glue sources. Follows FREECODEC_H264 by default:
# validated on y623 with the clean encoder only, not with the vendor blobs.
FREECODEC_VENC_BASE ?= $(FREECODEC_H264)

# Bring-up knob: 1 = replace the vendor libcedarc vencoder API
# ($(CEDARC)/vencoder/vencoder.c) with the clean-room fenc_ package
# (freewinner/freecodec-git, src/fenc -> libfenc.a).  Default 0 keeps the vendor
# object.  The clean objects keep the prebuilt 4-byte-enum ABI, so they inherit
# the base CFLAGS (never -fshort-enums) and link libvenc_base.so as before.
FREECODEC_FENC ?= 1

# Bring-up knob: 1 = replace the vendor ISP control plane with the clean-room
# fisp_ package (freewinner-git/isp/src/fisp -> libfisp.a).  The 25
# AW_MPI_ISP_* symbols live in TWO vendor files: mpi_isp.c (the lifecycle
# Init/Exit/Run/Stop/GetEnvLV) and mpi_vi.c (the AE + picture-control setters and
# getters).  So this switch drops mpi_isp.c and objcopy-strips the
# AW_MPI_ISP_* definitions out of mpi_vi.o, keeping that file's AW_MPI_VI_*
# (owned by fcap_/package A).  Default 0 keeps the vendor objects.
# Follows FREECODEC_FCAP: dropping SRC_MPI removes mpi_isp.c, so fcap_ needs the
# fisp archive for the whole AW_MPI_ISP_* surface.
FREECODEC_FISP ?= $(FREECODEC_FCAP)

# Bring-up knob: 1 = resolve the daemon's own headers from the clean-room
# header tree (freewinner-git/mw_headers, package D) instead of the SDK's
# mpi_vi.h / mpi_isp.h / vencoder.h / isp_manage.h / ...  Prepended to INC so our
# headers win for the app TUs.  Set to 1 in the deploy build: with it on (and
# FREECODEC_FCAP=1) no TU in the deploy build includes a vendor header.  The SDK
# -I roots still appear on the command line (for non-deploy targets' vendor
# TUs); they are unused in the deploy build.  Default 0 keeps the SDK headers.
FREECODEC_HEADERS ?= 1

# ---------------------------------------------------------------------------
# ISP source selection.
#
#   default : the Lindenis V833 (sun8iw19p1) SDK libisp - ISP_VERSION 521, the
#             correct generation/source for this chip (AW_V459, sun8iw19p1),
#             with ABI-matched prebuilt 3A in libisp/out/. Its
#             struct isp_param_config is the native 521 layout, so a vendor
#             tuning extracted from stock rmm (`isp521-ipc`) is imported
#             verbatim - no field remap.
#
#   (The vendor tuning is no longer selected at build time: it is extracted
#   on-camera at boot by rmm_tuning.c. See below.)
# ---------------------------------------------------------------------------
ISP := $(LIB)/libisp
ISP_ALGO_LIBDIR := $(ISP)/out
BUILD ?= build
TARGET ?= mediad

# ---------------------------------------------------------------------------
# On-camera tuning extraction.
#
# No vendor tuning blob is compiled in. rmm_layout.h (the stock-521 -> V833
# struct remap only - no vendor bytes, no per-firmware data) is generated at
# build time, in the enum ABI matching this build (ALGO_RTOS uses
# -fshort-enums). rmm_tuning.c then reads the tuning straight out of the
# camera's own /home/app/rmm at runtime, keyed on the live sensor name.
# ---------------------------------------------------------------------------
RMM_LAYOUT_H := $(BUILD)/rmm_layout.h
RMM_ABI = $(if $(filter 1,$(ALGO_RTOS)),short,long)

# ---------------------------------------------------------------------------
# Macro config: pick just VI + ISP + VENC out of the MPPCFG_* feature set that
# mpp_config.mk would otherwise derive from a full tina board config.
# ---------------------------------------------------------------------------
CFG_DEFS := \
    -DAWCHIP=0x1817 \
    -DISP_TUNE_NATIVE=1 \
    -DCONF_USE_IOMMU=1 \
    -DCDXCFG_FILE_SYSTEM=0 \
    -DCDXCFG_HW_DISPLAY=0 \
    -DMPPCFG_VI=1 \
    -DMPPCFG_VO=0 \
    -DMPPCFG_TEXTENC=0 \
    -DMPPCFG_VENC=1 \
    -DMPPCFG_VDEC=0 \
    -DMPPCFG_MUXER=0 \
    -DMPPCFG_DEMUXER=0 \
    -DMPPCFG_ISE=0 \
    -DMPPCFG_EIS=0 \
    -DUSE_STD_LOG \
    -D__OS_LINUX

# ---------------------------------------------------------------------------
# Include paths (union of the dirs tina_mpp_static.mk / tina_mpp_isp.mk /
# tina_libISP.mk / sample tina.mk list, minus anything that only exists under
# a built STAGING_DIR we don't have).
# ---------------------------------------------------------------------------
INC := \
    -I$(SW)/system/public/include \
    -I$(SW)/system/public/include/utils \
    -I$(SW)/system/public/include/vo \
    -I$(SW)/system/public/libion/include \
    -I$(MW)/config \
    -I$(MW)/include \
    -I$(MW)/include/utils \
    -I$(MW)/include/media \
    -I$(MW)/include/media/utils \
    -I$(MEDIA) \
    -I$(MEDIA)/include \
    -I$(MEDIA)/include/utils \
    -I$(MEDIA)/include/component \
    -I$(MEDIA)/include/include_render \
    -I$(MEDIA)/include/videoIn \
    -I$(MEDIA)/include/audio \
    -I$(MEDIA)/librender \
    -I$(MEDIA)/utils \
    -I$(CEDARC)/include \
    -I$(CEDARC)/base/include \
    -I$(CEDARC)/vencoder/base/include \
    -I$(LIB)/include_stream \
    -I$(LIB)/include_FsWriter \
    -I$(LIB)/include_demux \
    -I$(LIB)/include_muxer \
    -I$(ISP)/include \
    -I$(ISP)/include/V4l2Camera \
    -I$(ISP)/include/device \
    -I$(ISP)/isp_dev \
    -I$(ISP)/isp_tuning \
    -I$(ISP)/isp_manage \
    -I$(ISP)/isp_events \
    -I$(ISP)/iniparser/src \
    -I$(ISP) \
    -I$(ISP)/isp_cfg \
    -I$(INTEG_DIR) \
    -I$(LIB)/libISE/include \
    -I$(LIB)/AudioLib/midware/encoding \
    -I$(LIB)/AudioLib/midware/encoding/include \
    -I$(LIB)/AudioLib/midware/decoding/include \
    -I$(LIB)/AudioLib/osal \
    -I$(LIB)/aec_lib/include \
    -I$(LIB)/libcedarx/libcore/common/plugin \
    -I$(LIB)/libcedarx/libcore/include \
    -I$(LIB)/libcedarx/libcore/base/include \
    -I$(LIB)/libcedarx/libcore/common/iniparser \
    -I$(LIB)/libcedarx/libcore/parser/include \
    -I$(LIB)/libcedarx/libcore/stream/include \
    -I$(LIB)/libcedarx \
    -I$(LIB)/textEncLib/include \
    -I$(LIB)/audioEffectLib/include \
    -I$(FREECODEC_DIR)/include \
    -Iinclude

# -march=armv7ve (the V833 is a Cortex-A7, ARMv7VE): the toolchain default is
# plain armv7-a, for which GCC lowers integer division to __aeabi_idiv/uidiv;
# those raise SIGFPE on a zero divisor (or INT_MIN/-1), whereas the vendor
# objects (and all prebuilt libs) use the hardware sdiv/udiv - zero divisor
# silently yields 0.  The clean-room ISP tier divides unguarded data-dependent
# values, so without this flag the on-camera clean build dies with SIGFPE in
# clean_config_wdr.  Match the vendor ISA so the semantics are identical.
#
# OPT: optimisation for the unvalidated bulk (vendor middleware source + app
# glue).  CLEAN_OPT is separate: the clean-room ISP tiers are validated at -O2
# (freewinner-git/isp/Makefile:10 and every shim/Makefile) and were the -O0
# throughput problem, but raising the *whole* daemon to -O2 crashes bring-up
# on-camera (2026-09-22: dies after fshare init, before any channel ready), so
# only the validated clean tiers get -O2 and everything else stays -O0.
OPT ?= -O0
CLEAN_OPT ?= -O2
# Package D: put the clean-room header tree first on the include path so the app
# TUs (main.c, isp_control.c, isp_config.c, mediad_venc.c,
# stub_audio_components.c, stock_reg.c) resolve media/mpi_vi.h, media/mpi_isp.h,
# vencoder.h, isp_manage.h, ... from our declarations.  The vendor roots remain in
# INC for the non-deploy vendor TUs only; the codec TU gets its own clean flags.
ifeq ($(FREECODEC_HEADERS),1)
# Package D lives in the freewinner repo (mw_headers/), copied from the private
# cleanroom/ workspace so a published checkout builds.
FREECODEC_HEADERS_DIR := $(SIBLINGS)/freewinner/freewinner-git/mw_headers
# mw_headers/*.h are thin forwarding shims onto the generated clean-room ABI
# headers (isp/include, single source of truth per 31-abi-headers.md SS5), so
# the app TUs need CINC_DIR on the general path too, not just CLEAN_MU_OBJS.
INC := -I$(FREECODEC_HEADERS_DIR) -I$(FREECODEC_HEADERS_DIR)/media \
    -I$(FREECODEC_HEADERS_DIR)/utils -I$(FREECODEC_HEADERS_DIR)/component \
    -I$(CINC_DIR) $(INC)
# Drop the SDK's V4l2Camera root so our mm_comm_vi.h's `#include
# <linux/videodev2.h>` resolves to the toolchain header (SPEC section 3.4); the
# SDK copy is only needed by the vendor V4l2 sources, which are not in this
# build once FCAP is on.
INC := $(filter-out -I$(ISP)/include/V4l2Camera,$(INC))
endif

CFLAGS := $(CFG_DEFS) $(INC) -I$(BUILD) -Wall -Wno-unused-but-set-variable -Wno-unused-variable \
    -Wno-unused-function -Wno-unused-label -Wno-multichar -fPIC -g $(OPT) -march=armv7ve \
    $(EXTRA_CFLAGS)
CXXFLAGS := $(CFLAGS)

# ---------------------------------------------------------------------------
# Source groups
# ---------------------------------------------------------------------------

# Our own code.
SRC_APP := main.c stub_audio_components.c fshare.c audio_codec.c mediad_audio.c mediad_venc.c talkback.c osd.c stock_reg.c isp_control.c isp_config.c rmm_tuning.c

# Separately gated: replay the captured whole-register table (isp_cfg/
# stock_reg_tbl.c, tools/make_stock_reg.py). This is the "lean on rmm's
# registers" path and is NOT part of the from-scratch or vendor-config builds.
# Default off; enable with STOCK_REG=1.
ifeq ($(STOCK_REG),1)
STOCK_REG_TBL := $(wildcard isp_cfg/stock_reg_tbl.c)
ifneq ($(STOCK_REG_TBL),)
SRC_APP += $(STOCK_REG_TBL)
CFLAGS += -DHAVE_STOCK_REG
CXXFLAGS += -DHAVE_STOCK_REG
endif
endif # STOCK_REG

# Core MPI (media control plane) - mpi_*.c + the component/videoIn glue they need.
SRC_MPI := \
    $(MEDIA)/mpi_sys.c \
    $(MEDIA)/mpi_vi.c \
    $(MEDIA)/mpi_isp.c \
    $(MEDIA)/mpi_region.c \
    $(MEDIA)/ChannelRegionInfo.c \
    $(MEDIA)/component/mm_component.c \
    $(MEDIA)/component/ComponentsRegistryTable.c \
    $(MEDIA)/component/VideoVirVi_Component.c \
    $(MEDIA)/videoIn/videoInputHw.c \
    $(MEDIA)/videoIn/VIPPDrawOSD_V5.c \
    $(LIB)/libcedarx/libcore/base/cdx_log.c

# VENC is now our own (mediad_venc.c): mpi_venc.c, VideoEnc_Component.c and the
# media_common VENC helpers are no longer compiled in. The libcedarc vencoder
# framework (SRC_VENCODER/libvenc_base.so) + the shared encoder device stay.

# Audio is now our own: mic capture + AAC-LC in mediad_audio.c (direct ALSA +
# freecodec), talkback playback in talkback.c (direct ALSA). The whole vendor
# audio stack (mpi_ai/mpi_aenc/mpi_ao/mpi_clock, AIChannel_/AudioEnc_/AOChannel_/
# Clock_Component, audio/audio_hw, audio/alsa_interface, audio/pcmBufferManager,
# AudioLib/.../aencoder, pcm_enc) is no longer compiled in. Links the device's
# own musl libasound.so.2.
SRC_AUDIO :=


# media/utils - shared helpers (pixel format conversion, message queues, etc).
# NOTE: PIXEL_FORMAT_E_g2d_format_convert.c / EPIXELFORMAT_g2d_format_convert.c
# are deliberately excluded - they're only used by the VO/render pixel-conversion
# path (nothing in our VI->VENC call graph references them, confirmed by grep),
# and their <linux/g2d_driver.h> only exists under sample/sample_g2d/ in this
# tree with a __s64/__u64 typedef that conflicts with the toolchain's own
# kernel headers. Not worth carrying for code we never call.
ifeq ($(VENDOR_MEDIA_UTILS),1)
# Vendor rollback set.  mm_comm_venc.c and cedarx_avs_counter.c are 0-xref and
# are dropped in both variants (freewinner media-utils-tier.md section 6 phase 1).
SRC_MEDIA_UTILS := \
    $(MEDIA)/utils/BITMAP_S.c \
    $(MEDIA)/utils/SystemBase.c \
    $(MEDIA)/utils/tmessage.c \
    $(MEDIA)/utils/tsemaphore.c \
    $(MEDIA)/utils/VIDEO_FRAME_INFO_S.c \
    $(MEDIA)/utils/media_common.c \
    $(MEDIA)/utils/mpi_videoformat_conversion.c \
    $(MEDIA)/utils/video_buffer_manager.c
else
# Clean-room media-utils (freewinner/freewinner-git/src/utils).  Must stay
# 4-byte-enum: these objects cross PIXEL_FORMAT_E / VIDEO_FRAME_INFO_S with the
# VI glue, so they deliberately do NOT get -fshort-enums.
SRC_MEDIA_UTILS := \
    $(CSRC_DIR)/utils/bitmap.c \
    $(CSRC_DIR)/utils/systime.c \
    $(CSRC_DIR)/utils/msgqueue.c \
    $(CSRC_DIR)/utils/semaphore.c \
    $(CSRC_DIR)/utils/frame_size.c \
    $(CSRC_DIR)/utils/media_helpers.c \
    $(CSRC_DIR)/utils/pixel_format.c \
    $(CSRC_DIR)/utils/frame_pool.c
endif
# Dropped as dead (0 xrefs from any object in the link): dup2SeldomUsedFd.c and
# CedarXMetaData.cpp (268 symbols / ~318 KB .text, all unreferenced).
SRC_MEDIA_UTILS_CXX :=

# The clean media-utils need the freewinner include root for media_utils_abi.h
# and utils/*.h (their only include path); base flags otherwise, no short enums.
ifneq ($(VENDOR_MEDIA_UTILS),1)
CLEAN_MU_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(SRC_MEDIA_UTILS))))
$(CLEAN_MU_OBJS): CFLAGS += -I$(CINC_DIR)
endif

# libISP (isp.c + tuning/manage/events + the isp_dev V4L2/media-controller layer
# + iniparser). Links against the prebuilt libisp_ae/af/.../rolloff algorithm libs.
SRC_ISP := \
    $(ISP)/isp_math/matrix.c \
    $(ISP)/isp_math/isp_math_util.c \
    $(ISP)/isp.c \
    $(ISP)/isp_tuning/isp_tuning.c \
    $(ISP)/isp_manage/isp_manage.c \
    $(ISP)/isp_manage/isp_helper.c \
    $(ISP)/isp_events/events.c \
    $(ISP)/isp_dev/isp_dev.c \
    $(ISP)/isp_dev/isp_v4l2_helper.c \
    $(ISP)/isp_dev/media.c \
    $(ISP)/isp_dev/video.c
# isp_math/matrix.c + isp_math/isp_math_util.c are REQUIRED by the non-RTOS
# default build: the prebuilt libisp_gtm.a/libisp_iso.a reference ValueInterp,
# ArraySum, ArrayStdVar, ArrayFindMaxIndex and Conv. (The ALGO_RTOS=1 path does
# not need them -- its vendor RTOS archive carries the math.) The iniparser
# sources stay dropped (guarded by ISP_LIB_USE_INIPARSER, which is undefined, so
# they compile to empty objects).
SRC_INIPARSER :=

# Clean-room libisp framework tier (freewinner).  VENDOR_FW=1 keeps the nine
# vendor files above; the default clears SRC_ISP and builds the clean
# replacement.  isp_dev_uapi.c carries the V4L2/device layer formerly in
# isp_dev.c/isp_v4l2_helper.c/media.c/video.c.
ifeq ($(ALGO_RTOS),1)
ifeq ($(VENDOR_FW),1)
else
SRC_ISP :=
CLEAN_FW_SRC := \
    $(CSRC_DIR)/framework/events.c \
    $(CSRC_DIR)/framework/helper.c \
    $(CSRC_DIR)/framework/manage.c \
    $(CSRC_DIR)/framework/tuning.c \
    $(CSRC_DIR)/framework/isp.c \
    $(CSRC_DIR)/framework/isp_dev_uapi.c
endif
endif

# Clean-room 3A shims: one clean core + one SDK-boundary shim per module, plus
# the integration layer that feeds the camera's own rmm tables in.  ISO was the
# pilot and lives at the shim root; the other five are shim/<mod>/<mod>_shim.c.
CLEAN_CORE_SRC := \
    $(CSRC_DIR)/iso/iso_clean.c \
    $(CSRC_DIR)/ae/ae_clean.c \
    $(CSRC_DIR)/awb/awb_clean.c \
    $(CSRC_DIR)/afs/afs_clean.c \
    $(CSRC_DIR)/gtm/gtm_clean.c \
    $(CSRC_DIR)/pltm/pltm_clean.c
CLEAN_SHIM_SRC := \
    $(SHIM_DIR)/iso_shim.c \
    $(SHIM_DIR)/ae/ae_shim.c \
    $(SHIM_DIR)/awb/awb_shim.c \
    $(SHIM_DIR)/afs/afs_shim.c \
    $(SHIM_DIR)/gtm/gtm_shim.c \
    $(SHIM_DIR)/pltm/pltm_shim.c
CLEAN_INTEG_SRC := \
    $(INTEG_DIR)/freeisp_shim_tables.c \
    $(FW_GIT)/src/tables/rmm_tables.c \
    $(FW_GIT)/src/tables/pltm_presets.c
# Register/hardware tier (A8/A9): reg_writers.c (the bsp_isp_*/bspp_isp_*
# writers) and module_cfg.c (the clean dispatch), presented to the framework
# through the SDK-ABI adapter shim/module_cfg/module_cfg_shim.c, which
# translates struct isp_module_config (37040 B, pointer payloads) <-> the clean
# module_cfg.h type (67224 B, embedded arrays).  Replaces isp_module_cfg.o and
# isp521_reg_cfg.o.
ifeq ($(VENDOR_REG),1)
CLEAN_REG_SRC :=
else
CLEAN_REG_SRC := $(CSRC_DIR)/reg/reg_writers.c
ifeq ($(VENDOR_MODCFG),1)
else
CLEAN_REG_SRC += $(CSRC_DIR)/reg/module_cfg.c $(SHIM_DIR)/module_cfg/module_cfg_shim.c
endif
ifeq ($(VENDOR_BASE),1)
else
CLEAN_REG_SRC += $(CSRC_DIR)/reg/base.c $(CSRC_DIR)/reg/comp_ref.c $(SHIM_DIR)/base/base_shim.c
endif
endif

# libcedarc glue: cdc_base, MemAdapter, vencoder, venc_base - these wrap the
# prebuilt hw encoder (libvenc_codec.a/libVE.a).
SRC_CDC_BASE := $(wildcard $(CEDARC)/base/*.c)
SRC_MEMADAPTER := \
    $(CEDARC)/memory/memoryAdapter.c \
    $(CEDARC)/memory/ionMemory/ionAlloc.c
SRC_VENCODER := $(CEDARC)/vencoder/vencoder.c
SRC_VENC_BASE := \
    $(CEDARC)/vencoder/base/EncAdapter.c \
    $(CEDARC)/vencoder/base/FrameBufferManager.c \
    $(CEDARC)/vencoder/base/BitstreamManager.c

# The cedarc glue (base/*.c, memory/*.c, vencoder/base/*.c) is packaged into
# libvenc_base.so below instead of being compiled into the daemon: the vendor
# shared H.264 encoder (libvenc_codec.so) imports BitStream*/EncAdapter*/CdcIon*
# and NEEDs libvenc_base.so, and a single definition in the shared library keeps
# one copy of the memory-adapter global state.
ALL_C_SRC := $(SRC_APP) $(SRC_MPI) $(SRC_AUDIO) $(SRC_MEDIA_UTILS) $(SRC_ISP) $(SRC_INIPARSER) \
    $(SRC_VENCODER)
ALL_CXX_SRC := $(SRC_MEDIA_UTILS_CXX)

# FREECODEC_FENC=1: drop the vendor vencoder.c from the link; the clean-room
# fenc_ archive (FREECODEC_FENC_LIB, built below) supplies the same entry points.
ifeq ($(FREECODEC_FENC),1)
ALL_C_SRC := $(filter-out $(SRC_VENCODER),$(ALL_C_SRC))
endif

# FREECODEC_FISP=1: drop the vendor mpi_isp.c; the clean-room fisp_ archive
# supplies the AW_MPI_ISP_* it defined, and mpi_vi.o is post-processed to remove
# the AW_MPI_ISP_* it also defines (see the objcopy rule below).
ifeq ($(FREECODEC_FISP),1)
ALL_C_SRC := $(filter-out $(MEDIA)/mpi_isp.c,$(ALL_C_SRC))
endif

# FREECODEC_FCAP=1: drop the entire vendor MPP capture layer (SRC_MPI) - the
# clean-room fcap_ archive supplies the 3 AW_MPI_SYS_* + 16 AW_MPI_VI_* it
# defined, and its SYS_Init installs the VI->ISP resolver fisp_ needs.  Because
# SRC_MPI also contains mpi_isp.c, this implies the FREECODEC_FISP boundary; the
# two switches may be used together (FCAP alone still needs the fisp archive for
# the AW_MPI_ISP_* surface, so wire them as a pair).
ifeq ($(FREECODEC_FCAP),1)
ALL_C_SRC := $(filter-out $(SRC_MPI),$(ALL_C_SRC))
endif

# Bring-up knob: 1 = replace the whole vendor MPP capture layer with the
# clean-room fcap_ package (freewinner-git/isp/src/fcap -> libfcap.a).  Drops
# mpi_sys.c, mpi_vi.c, mpi_region.c, ChannelRegionInfo.c, the component/
# registry TUs, videoIn/*.c and cdx_log.c; the clean-room utils (bitmap,
# pixel_format, frame_size, msgqueue, frame_pool, semaphore, systime,
# media_helpers) stay, as fcap_ calls them.  fcap_'s SYS_Init also installs the
# VI->ISP resolver that fisp_ needs, so FREECODEC_FCAP=1 implies the fisp
# boundary too.  Default 0 keeps the vendor objects.
FREECODEC_FCAP ?= 1

# The clean tier speaks the RTOS-521 short-enum ABI; build it only for the
# ALGO_RTOS deploy target (the default target links the non-RTOS libisp-*/out
# prebuilts and keeps the vendor objects).
ifeq ($(ALGO_RTOS),1)
ALL_C_SRC += $(CLEAN_CORE_SRC) $(CLEAN_SHIM_SRC) $(CLEAN_INTEG_SRC) $(CLEAN_REG_SRC)
ALL_C_SRC += $(CLEAN_FW_SRC)
endif

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(ALL_C_SRC))) \
        $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(ALL_CXX_SRC)))

# ALGO_RTOS=1: the Lindenis V833 Melis algorithm archive was built with
# -fshort-enums (its isp_dynamic_triger_t is 17 bytes vs our 68), so only the
# objects that share libisp structs with it need that enum ABI - not the whole
# build (which also links 4-byte-enum prebuilt codec/audio libs).
# ALGO_RTOS=1: only the libisp files that share structs with the Melis-built
# 521 algorithm archive (isp_lib_context / isp_param_config / the 3A param
# structs) may use that archive's ABI: -fshort-enums, plus isp_manage.h padding
# ctx_lock to Melis's 64-byte pthread_mutex_t (ISP521_RTOS_ALGO).
#
# Everything else keeps the normal 4-byte-enum ABI, because it shares structs
# with the *prebuilt* VENC codec/cedarc: compiling the whole tree short made
# fwm_venc_h264_config_t 188 instead of 200 bytes, so the codec read coding_mode at 0x24
# as 0x101 -> "do not support the picEncmode:257" -> every frame rejected.
# isp_dev.c/video.c/media.c/events.c are deliberately NOT short: they share
# struct video_fmt / hw_isp_device with the 4-byte VI glue and never touch
# isp_lib_context.
ifeq ($(ALGO_RTOS),1)
# main.c's clean-shim table feed (freeisp_shim_tables.h + load_shim_tables())
# exists only in this build; the default target has no clean tier.
CFLAGS += -DMEDIAD_ALGO_RTOS=1
CXXFLAGS += -DMEDIAD_ALGO_RTOS=1
RTOS_ENUM_SRC := $(ISP)/isp.c $(ISP)/isp_tuning/isp_tuning.c \
                 $(ISP)/isp_manage/isp_manage.c $(ISP)/isp_manage/isp_helper.c \
                 isp_config.c stub_audio_components.c stock_reg.c \
                 mpi_isp.c isp_control.c
RTOS_ENUM_OBJS := $(addprefix $(BUILD)/,$(patsubst %.c,%.o,$(notdir $(RTOS_ENUM_SRC))))
$(RTOS_ENUM_OBJS): CFLAGS += -fshort-enums -DISP521_RTOS_ALGO=1
endif

# Clean-room tier: same short-enum ISP521 ABI as the Melis archive and the
# framework objects they share isp_lib_context with; -fwrapv/gnu99 mirror the
# modules' own Makefiles.  The clean objects build against
# freeisp/sdk_interop.h, so they need only the freewinner include roots (the
# SDK INC above is for the framework).
ifeq ($(ALGO_RTOS),1)
CLEAN_CORE_OBJS  := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_CORE_SRC))))
CLEAN_SHIM_OBJS  := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_SHIM_SRC))))
CLEAN_INTEG_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_INTEG_SRC))))
CLEAN_REG_OBJS   := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_REG_SRC))))
CLEAN_INC_FLAGS  := -I$(CINC_DIR) -I$(INTEG_DIR)
$(CLEAN_CORE_OBJS) $(CLEAN_SHIM_OBJS) $(CLEAN_INTEG_OBJS) $(CLEAN_REG_OBJS): CFLAGS += \
    $(CLEAN_OPT) $(CLEAN_INC_FLAGS) -fshort-enums -DISP521_RTOS_ALGO=1 -DISP_VERSION=521 \
    -fwrapv -std=gnu99

# Clean-room framework tier: the five logic units share the isp_lib_context
# short-enum ABI; isp_dev_uapi.o stays long-enum (it shares video_fmt/
# hw_isp_device with the 4-byte VI glue).  CLEAN_FW_SRC is empty under
# VENDOR_FW=1, so this is inert for the vendor build.
CLEAN_FW_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_FW_SRC))))
$(BUILD)/events.o $(BUILD)/helper.o $(BUILD)/manage.o \
$(BUILD)/tuning.o $(BUILD)/isp.o: CFLAGS += \
    $(CLEAN_OPT) $(CLEAN_INC_FLAGS) -fshort-enums -DISP521_RTOS_ALGO=1 -DISP_VERSION=521 \
    -fwrapv -std=gnu99
$(BUILD)/isp_dev_uapi.o: CFLAGS += $(CLEAN_OPT) $(CLEAN_INC_FLAGS) -DISP_VERSION=521 -fwrapv -std=gnu99

# module_cfg.c: the two SDK entry points are exported by module_cfg_shim.c, so
# rename the clean spellings here (the shim's CLEAN_DEFS); its isp_config_*/
# isp_enable_*/isp_module_attrs names are internal to the clean tier and do not
# collide (vendor isp_module_cfg.o is dropped).
$(BUILD)/module_cfg.o: CFLAGS += -Disp_hardware_update=clean_isp_hardware_update -Disp_map_addr=clean_isp_map_addr

# base.c (A10): the 12 isp_base.h entry points are exported by base_shim.c.
$(BUILD)/base.o: CFLAGS += -Dconfig_band_step=clean_config_band_step -Dconfig_lens_center=clean_config_lens_center -Dconfig_dig_gain=clean_config_dig_gain -Dconfig_gamma=clean_config_gamma -Dconfig_wdr=clean_config_wdr -Disp_apply_colormatrix=clean_isp_apply_colormatrix -D__isp_stat_dynamic_judge=clean___isp_stat_dynamic_judge -Disp_handle_stats=clean_isp_handle_stats -Disp_handle_stats_sync=clean_isp_handle_stats_sync -Disp_apply_settings=clean_isp_apply_settings -Dconfig_lens_table=clean_config_lens_table -Dconfig_msc_table=clean_config_msc_table

# Clean cores: rename the public entry points so the shim can export the SDK
# names (mirrors each module Makefile's CLEAN_DEFS).
$(BUILD)/iso_clean.o:  CFLAGS += -Diso_init=clean_iso_init -Diso_exit=clean_iso_exit -Diso_get_params=clean_iso_get_params -Diso_set_params=clean_iso_set_params -Diso_run=clean_iso_run
$(BUILD)/ae_clean.o:   CFLAGS += -Dae_init=clean_ae_init -Dae_exit=clean_ae_exit -Dae_get_params=clean_ae_get_params -Dae_set_params=clean_ae_set_params -Dae_run=clean_ae_run -Dae_isr=clean_ae_isr -Dae_stats_t=clean_ae_stats_t -Dae_result_t=clean_ae_result_t
$(BUILD)/awb_clean.o:  CFLAGS += -Dawb_init=clean_awb_init -Dawb_exit=clean_awb_exit -Dawb_get_params=clean_awb_get_params -Dawb_set_params=clean_awb_set_params -Dawb_run=clean_awb_run -Dawb_isr=clean_awb_isr
$(BUILD)/afs_clean.o:  CFLAGS += -Dafs_clean_init=clean_afs_init -Dafs_clean_exit=clean_afs_exit -Dafs_clean_get_params=clean_afs_get_params -Dafs_clean_set_params=clean_afs_set_params -Dafs_clean_run=clean_afs_run -Dafs_clean_isr=clean_afs_isr
$(BUILD)/gtm_clean.o:  CFLAGS += -Dgtm_init=clean_gtm_init -Dgtm_exit=clean_gtm_exit -Dgtm_get_params=clean_gtm_get_params -Dgtm_set_params=clean_gtm_set_params -Dgtm_run=clean_gtm_run
$(BUILD)/pltm_clean.o: CFLAGS += -Dpltm_init=clean_pltm_init -Dpltm_exit=clean_pltm_exit -Dpltm_get_params=clean_pltm_get_params -Dpltm_set_params=clean_pltm_set_params -Dpltm_run=clean_pltm_run -Dpltm_set_default_result=clean_pltm_set_default_result

# Five shims each define freeisp_get_tables() (their clean core calls it); only
# one may exist per process, so rename it per module on BOTH the clean core and
# its shim.  Without this the link fails with multiple definitions.
$(BUILD)/iso_clean.o  $(BUILD)/iso_shim.o:  CFLAGS += -Dfreeisp_get_tables=clean_iso_get_tables
$(BUILD)/ae_clean.o   $(BUILD)/ae_shim.o:   CFLAGS += -Dfreeisp_get_tables=clean_ae_get_tables
$(BUILD)/awb_clean.o  $(BUILD)/awb_shim.o:  CFLAGS += -Dfreeisp_get_tables=clean_awb_get_tables
$(BUILD)/gtm_clean.o  $(BUILD)/gtm_shim.o:  CFLAGS += -Dfreeisp_get_tables=clean_gtm_get_tables
$(BUILD)/pltm_clean.o $(BUILD)/pltm_shim.o: CFLAGS += -Dfreeisp_get_tables=clean_pltm_get_tables
endif

# Bring-up bisection: for each module in VENDOR_3A the vendor archive member is
# kept (see the drop list) and the clean shim's SDK entry points are renamed with
# objcopy so the framework binds the vendor module.  (A command-line -D cannot do
# it: the shim sources #undef those names.)  The feed keeps linking against the
# shim setter, which then feeds an object the framework never calls.
V3A_SRC_iso  := $(SHIM_DIR)/iso_shim.c
V3A_SRC_ae   := $(SHIM_DIR)/ae/ae_shim.c
V3A_SRC_awb  := $(SHIM_DIR)/awb/awb_shim.c
V3A_SRC_afs  := $(SHIM_DIR)/afs/afs_shim.c
V3A_SRC_gtm  := $(SHIM_DIR)/gtm/gtm_shim.c
V3A_SRC_pltm := $(SHIM_DIR)/pltm/pltm_shim.c
ifeq ($(ALGO_RTOS),1)
ifeq ($(filter iso,$(VENDOR_3A)),iso)
$(BUILD)/iso_shim.o: $(V3A_SRC_iso) | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	$(OBJCOPY) --redefine-sym iso_init=off_iso_init --redefine-sym iso_exit=off_iso_exit $@
endif
ifeq ($(filter ae,$(VENDOR_3A)),ae)
$(BUILD)/ae_shim.o: $(V3A_SRC_ae) | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	$(OBJCOPY) --redefine-sym ae_init=off_ae_init --redefine-sym ae_exit=off_ae_exit $@
endif
ifeq ($(filter awb,$(VENDOR_3A)),awb)
$(BUILD)/awb_shim.o: $(V3A_SRC_awb) | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	$(OBJCOPY) --redefine-sym awb_init=off_awb_init --redefine-sym awb_exit=off_awb_exit $@
endif
ifeq ($(filter afs,$(VENDOR_3A)),afs)
$(BUILD)/afs_shim.o: $(V3A_SRC_afs) | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	$(OBJCOPY) --redefine-sym afs_init=off_afs_init --redefine-sym afs_exit=off_afs_exit $@
endif
ifeq ($(filter gtm,$(VENDOR_3A)),gtm)
$(BUILD)/gtm_shim.o: $(V3A_SRC_gtm) | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	$(OBJCOPY) --redefine-sym gtm_init=off_gtm_init --redefine-sym gtm_exit=off_gtm_exit $@
endif
ifeq ($(filter pltm,$(VENDOR_3A)),pltm)
$(BUILD)/pltm_shim.o: $(V3A_SRC_pltm) | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	$(OBJCOPY) --redefine-sym pltm_init=off_pltm_init --redefine-sym pltm_exit=off_pltm_exit $@
endif
endif

vpath %.c $(sort $(dir $(ALL_C_SRC)))
vpath %.cpp $(sort $(dir $(ALL_CXX_SRC)))

# ---------------------------------------------------------------------------
# Prebuilt libs
# ---------------------------------------------------------------------------
# ISP_ALGO_LIBDIR is set by the ISP revision switch above.
CEDARC_LIBDIR := $(CEDARC)/library
AUDIO_CODEC_LIBDIR := $(LIB)/AudioLib/lib
ALSA_LIBDIR := prebuilt

# ALGO_RTOS=1: link the Lindenis V833 Melis (RTOS) algorithm archive, reduced to
# a framework-only archive. Its isp_version.h records FROM_REPO_BRANCH
# "isp521-ipc" / FROM_REPO_COMMIT efa5ea93 - the same algorithm branch lineage
# stock rmm used (feb055b7) - and it was built against essentially the same
# framework headers, so it is link/ABI compatible where the isp522 prebuilts
# were not. See other.md.
# The clean 3A shims and the clean register/config/base tier (each
# with its SDK-ABI shim, A8/A9/A10) replace every reference, so the drop list
# removes the six 3A members, isp_module_cfg.o, isp521_reg_cfg.o, isp_base.o and
# the three dead stubs (isp_3a_af.o, isp_motion_detect.o, isp_rolloff.o) —
# leaving an empty archive that contributes nothing vendor.
ifeq ($(ALGO_RTOS),1)
ISP_ALGO_RTOS_ARCHIVE   := $(MD)/prebuilt/libisp_algo_rtos.a
ISP_ALGO_RTOS_FRAMEWORK := $(BUILD)/libisp_algo_rtos_framework.a
# Module -> archive member (for the VENDOR_3A bring-up fallback).
VENDOR_3A_MEMBER_iso  := isp_iso_config.o
VENDOR_3A_MEMBER_ae   := isp_3a_ae.o
VENDOR_3A_MEMBER_awb  := isp_3a_awb.o
VENDOR_3A_MEMBER_afs  := isp_3a_afs.o
VENDOR_3A_MEMBER_gtm  := isp_tone_mapping.o
VENDOR_3A_MEMBER_pltm := isp_pltm.o
ISP_ALGO_RTOS_3A_ALL  := isp_3a_ae.o isp_3a_afs.o isp_3a_awb.o \
                         isp_iso_config.o isp_pltm.o isp_tone_mapping.o
ISP_ALGO_RTOS_DROP      := $(filter-out $(foreach m,$(VENDOR_3A),$(VENDOR_3A_MEMBER_$(m))),$(ISP_ALGO_RTOS_3A_ALL)) \
                           $(if $(filter 1,$(VENDOR_REG) $(VENDOR_MODCFG)),,isp_module_cfg.o isp521_reg_cfg.o) \
                           $(if $(filter 1,$(VENDOR_REG) $(VENDOR_BASE)),,isp_base.o) \
                           isp_3a_af.o isp_motion_detect.o isp_rolloff.o
ISP_ALGO_LIBS := $(ISP_ALGO_RTOS_FRAMEWORK)
else
ISP_ALGO_LIBS := -lisp_ae -lisp_af -lisp_afs -lisp_awb -lisp_base -lisp_gtm -lisp_iso -lisp_md -lisp_pltm -lisp_rolloff
endif

# Generated from the untouched prebuilt archive; make clean removes it.
ifeq ($(ALGO_RTOS),1)
$(ISP_ALGO_RTOS_FRAMEWORK): $(ISP_ALGO_RTOS_ARCHIVE) | $(BUILD)
	cp $< $@
	$(AR) d $@ $(ISP_ALGO_RTOS_DROP)
endif
# Vendor H.264 encoder path (NOT the deploy build): with FREECODEC_H264 unset the
# SDK's shared libvenc_codec.so + libVE.so are linked (musl ARM EABI5) instead of
# the static .a, so the encoder is not embedded in the daemon.  The deploy build
# (package.sh) sets FREECODEC_H264=1, which links the clean freecodec objects
# instead and builds libvenc_base.so from codec/src/base (see
# FREECODEC_VENC_BASE below); no vendor encoder .so is used or shipped.
CEDARC_SO_DIR  := $(CEDARC)/library
VENCBASE_SRCS  := $(SRC_VENC_BASE) $(SRC_CDC_BASE) $(SRC_MEMADAPTER)
VENCBASE_SO    := $(BUILD)/libvenc_base.so

# Clean-room H.264 encoder (freewinner/freecodec-git) substitution.  When
# FREECODEC_H264=1 the seven objects replace -lvenc_codec/-lVE; they keep the
# prebuilt 4-byte-enum ABI, so they inherit the base CFLAGS (never
# -fshort-enums).  libvenc_base.so still supplies BitStream*/EncAdapter*/ion.
ifeq ($(FREECODEC_H264),1)
FREECODEC_H264_DIR  := $(FREECODEC_DIR)/src/h264
FREECODEC_H264_SRCS := \
    $(FREECODEC_H264_DIR)/regs/h264_regs.c \
    $(FREECODEC_H264_DIR)/headers/h264_headers.c \
    $(FREECODEC_H264_DIR)/rc/h264_rc.c \
    $(FREECODEC_H264_DIR)/isp/h264_isp.c \
    $(FREECODEC_H264_DIR)/gop/h264_gop.c \
    $(FREECODEC_H264_DIR)/ve/ve_driver.c \
    $(FREECODEC_H264_DIR)/enc/freecodec_enc.c \
    $(FREECODEC_H264_DIR)/enc/freecodec_enc_regs.c
FREECODEC_H264_OBJS := $(patsubst $(FREECODEC_H264_DIR)/%.c,$(BUILD)/fc_h264/%.o,$(FREECODEC_H264_SRCS))
H264_CODEC_LINK := $(FREECODEC_H264_OBJS) -lvenc_base
# The codec's own sources need only the freecodec clean headers plus the config
# defines; give them their own flag set so this TU provably touches no vendor
# header (the rest of INC is for the legacy MPP TUs).
FREECODEC_H264_CFLAGS := $(CFG_DEFS) -Wall -Wno-unused-but-set-variable \
    -Wno-unused-variable -I$(FREECODEC_DIR)/include $(EXTRA_CFLAGS)
$(BUILD)/fc_h264/%.o: $(FREECODEC_H264_DIR)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(FREECODEC_H264_CFLAGS) -c $< -o $@
else
FREECODEC_H264_OBJS :=
H264_CODEC_LINK := -lvenc_base -lvenc_codec -lVE
endif

# libvideoengine.a (decoder plugin manager) and libawh264.a (H.264 decoder) are
# NOT linked: MPPCFG_VDEC=0, so they contribute 0 used symbols. Verify after a
# relink with: nm mediad_rtos_v | grep -E 'CreateH264Decoder|VideoEngineCreate'
# (returns nothing; it already did before they were dropped).

# Clean-room libcedarc vencoder API (freewinner/freecodec-git mk/fenc.mk).  When
# FREECODEC_FENC=1 this archive replaces the vendor vencoder.o and supplies the
# 23 VideoEnc* entry points mediad_venc.c calls.  Built with the same toolchain
# and 4-byte-enum ABI as the rest of the daemon.
FREECODEC_FENC_LIB :=
ifeq ($(FREECODEC_FENC),1)
FREECODEC_FENC_BUILD  := build_arm_fenc
FREECODEC_FENC_ARCHIVE := $(FREECODEC_DIR)/$(FREECODEC_FENC_BUILD)/libfenc.a
FREECODEC_FENC_LIB    := $(FREECODEC_FENC_ARCHIVE)
$(FREECODEC_FENC_ARCHIVE): FORCE
	$(MAKE) -C $(FREECODEC_DIR) CC=$(CC) AR=$(AR) BUILD=$(FREECODEC_FENC_BUILD) fenc
endif

# Clean-room ISP runtime (freewinner-git isp mk/fisp.mk).  When FREECODEC_FISP=1
# this archive supplies the AW_MPI_ISP_* surface that vendor mpi_isp.c and
# mpi_vi.c defined.  mk/fisp.mk already builds with the RTOS short-enum ISP521
# ABI the framework tier expects.
FREECODEC_FISP_LIB :=
ifeq ($(FREECODEC_FISP),1)
FREECODEC_FISP_BUILD   := build_arm_fisp
FREECODEC_FISP_ARCHIVE := $(FREEWINNER)/$(FREECODEC_FISP_BUILD)/libfisp.a
FREECODEC_FISP_LIB     := $(FREECODEC_FISP_ARCHIVE)
$(FREECODEC_FISP_ARCHIVE): FORCE
	$(MAKE) -C $(FREEWINNER) CC=$(CC) AR=$(AR) BUILD=$(FREECODEC_FISP_BUILD) fisp

# mpi_vi.o also defines the 53 AW_MPI_ISP_* setter/getter symbols (package B
# owns those; fisp_ supplies the 25 mediad calls).  Strip the AW_MPI_ISP_*
# definitions out of the compiled object so the vendor VI symbols it also
# defines (AW_MPI_VI_*, owned by package A) survive, then let libfisp.a resolve
# the ISP ones.  The symbol list is read from the object at recipe time (a
# make-time $(shell) would run before the object exists).
$(BUILD)/mpi_vi.o: $(MEDIA)/mpi_vi.c | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	for s in `$(NM) $@ | awk '/ T AW_MPI_ISP_/ {print $$3}'`; do \
	    $(OBJCOPY) --strip-symbol=$$s $@; done
endif

# Clean-room capture runtime (freewinner-git isp mk/fcap.mk).  When
# FREECODEC_FCAP=1 this archive supplies the AW_MPI_SYS_* / AW_MPI_VI_* surface
# and installs the VI->ISP resolver via SYS_Init.  mk/fcap.mk keeps the default
# 4-byte enum (never -fshort-enums) to match the VI glue ABI.
FREECODEC_FCAP_LIB :=
ifeq ($(FREECODEC_FCAP),1)
FREECODEC_FCAP_BUILD   := build_arm_fcap
FREECODEC_FCAP_ARCHIVE := $(FREEWINNER)/$(FREECODEC_FCAP_BUILD)/libfcap.a
FREECODEC_FCAP_LIB     := $(FREECODEC_FCAP_ARCHIVE)
$(FREECODEC_FCAP_ARCHIVE): FORCE
	$(MAKE) -C $(FREEWINNER) CC=$(CC) AR=$(AR) BUILD=$(FREECODEC_FCAP_BUILD) fcap
endif

# AAC-LC is our own freewinner/freecodec-git replacement (LGPL FAAC backend);
# the vendor libaacenc.a/libmp3enc.a are no longer linked. (libmp3enc's MP3 entry
# points were already stubbed in stub_audio_components.c.)
FREECODEC_AAC ?= $(FREECODEC_DIR)/build_arm/libfreecodec_aac.a
AUDIO_CODEC_LIBS := $(FREECODEC_AAC)

LDFLAGS := \
    -L$(ISP_ALGO_LIBDIR) \
    -Wl,-Bstatic \
    -Wl,--start-group $(ISP_ALGO_LIBS) $(AUDIO_CODEC_LIBS) $(FREECODEC_FENC_LIB) $(FREECODEC_FISP_LIB) $(FREECODEC_FCAP_LIB) -Wl,--end-group \
    -Wl,-Bdynamic \
    -L$(BUILD) -L$(CEDARC_SO_DIR) \
    $(H264_CODEC_LINK) \
    -Wl,-rpath-link,$(BUILD) \
    -L$(ALSA_LIBDIR) -lasound \
    -lpthread -lm -ldl \
    -Wl,-rpath,'$$ORIGIN/../lib' \
    -Wl,--export-dynamic \
    -Wl,--wrap=isp_set_load_reg

.DEFAULT_GOAL := all
.PHONY: all clean
all: $(TARGET) mediad_ctl rmm_extract

# Tiny on-device client for the ISP control socket (isp_control.c). Standalone:
# pure POSIX, no vendor includes, so it compiles with the same cross-gcc.
mediad_ctl: tools/mediad_ctl.c
	$(CC) -O2 -Wall -o $@ $<

# On-camera tuning extractor CLI: same core as mediad (rmm_tuning.c), useful
# for host testing. Emits the ABI this build actually uses.
rmm_extract: rmm_extract.c rmm_tuning.c $(RMM_LAYOUT_H)
	$(CC) -O2 -Wall -I$(BUILD) -o $@ rmm_extract.c rmm_tuning.c

# Generated stock-521 -> V833 struct remap (no vendor bytes) for rmm_tuning.c.
$(RMM_LAYOUT_H): tools/gen_rmm_layout.py | $(BUILD)
	python3 tools/gen_rmm_layout.py --abi $(RMM_ABI) -o $@

$(BUILD)/rmm_tuning.o: $(RMM_LAYOUT_H)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MD -MP -c -o $@ $<

# libvenc_base.so: the cedarc glue (base/memory/vencoder-base) the vendor shared
# encoder imports. One shared copy. FREECODEC_VENC_BASE=1 takes the clean-room
# build from freewinner codec/ (mk/venc_base.mk) instead of the SDK source.
ifeq ($(FREECODEC_VENC_BASE),1)
FREECODEC_VB_BUILD := build_arm_vb
$(VENCBASE_SO): FORCE | $(BUILD)
	$(MAKE) -C $(FREECODEC_DIR) BUILD=$(FREECODEC_VB_BUILD) \
	    CC=$(CC) AR=$(AR) $(FREECODEC_VB_BUILD)/libvenc_base.so
	cp $(FREECODEC_DIR)/$(FREECODEC_VB_BUILD)/libvenc_base.so $@
.PHONY: FORCE
FORCE:
else
$(VENCBASE_SO): $(VENCBASE_SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libvenc_base.so -o $@ $(VENCBASE_SRCS) \
	    -lpthread -lm -ldl
endif

# Our AAC-LC archive (freewinner/freecodec-git); FAAC source comes from build.sh.
# Rebuilt every time (it used to be a stale prebuilt): FAAC is passed as a path
# relative to the codec dir so its assert() __FILE__ strings carry no host
# path (GCC 6.4 has no -ffile-prefix-map).
FAAC_REL = $(shell realpath --relative-to=$(FREECODEC_DIR) $(MD)/../../repos/faac)
$(FREECODEC_AAC): FORCE
	test -n "$(FREECODEC_DIR)"
	rm -f $(FREECODEC_DIR)/build_arm/libfreecodec_aac.a $(FREECODEC_DIR)/build_arm/freecodec_aac.o
	rm -rf $(FREECODEC_DIR)/build_arm/faac
	$(MAKE) -C $(FREECODEC_DIR) arm-aac \
	    TC=$(TC)/bin/arm-openwrt-linux-muslgnueabi \
	    FAAC_DIR=$(FAAC_REL)

$(TARGET): $(OBJS) $(RMM_LAYOUT_H) $(VENCBASE_SO) $(FREECODEC_AAC) $(FREECODEC_H264_OBJS) $(FREECODEC_FENC_LIB) $(FREECODEC_FISP_LIB) $(FREECODEC_FCAP_LIB)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

# ALGO_RTOS: the framework archive is a generated artifact, not in OBJS.
ifeq ($(ALGO_RTOS),1)
$(TARGET): $(ISP_ALGO_RTOS_FRAMEWORK)
endif

clean:
	rm -rf $(BUILD) $(TARGET) mediad_ctl rmm_extract

-include $(OBJS:.o=.d)
