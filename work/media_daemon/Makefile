# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors

# Cross-build of mediad (armhf musl). A bare `make` is the deploy build: no vendor
# object or header, clean-room tiers from freewinner. Knobs: docs/build-knobs.md.

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

# freewinner: the ./freewinner submodule if checked out, else a sibling checkout.
SIBLINGS  ?= $(abspath $(MD)/../../../..)
FW_ROOT ?= $(if $(wildcard $(MD)/../../freewinner/isp/Makefile),$(abspath $(MD)/../../freewinner),$(SIBLINGS)/freewinner/freewinner-git)
FREEWINNER ?= $(FW_ROOT)/isp
FREECODEC_DIR ?= $(FW_ROOT)/codec
FW_GIT    := $(FREEWINNER)
SHIM_DIR  := $(FW_GIT)/shim
CSRC_DIR  := $(FW_GIT)/src
CINC_DIR  := $(FW_GIT)/include
INTEG_DIR := $(SHIM_DIR)/integration

# Build knobs (docs/build-knobs.md). Defaults are the deploy build; the VENDOR_*
# knobs and clearing a FREECODEC_* knob select vendor comparison builds.
VENDOR_3A ?=
VENDOR_REG ?=
VENDOR_MODCFG ?=
VENDOR_BASE ?=
VENDOR_FW ?=
VENDOR_MEDIA_UTILS ?=
ALGO_RTOS ?= 1
FREECODEC_H264 ?= 1
FREECODEC_VENC_BASE ?= $(FREECODEC_H264)
FREECODEC_FENC ?= 1
FREECODEC_FCAP ?= 1
FREECODEC_FISP ?= $(FREECODEC_FCAP)
FREECODEC_HEADERS ?= 1

ISP := $(LIB)/libisp
ISP_ALGO_LIBDIR := $(ISP)/out
BUILD ?= build
TARGET ?= mediad

# Generated struct remap for rmm_tuning.c (no vendor bytes), in this build's
# enum ABI; the tuning itself is read from the camera's rmm at runtime.
RMM_LAYOUT_H := $(BUILD)/rmm_layout.h
RMM_ABI = $(if $(filter 1,$(ALGO_RTOS)),short,long)

# MPP feature macros: VI + ISP + VENC only.
CFG_DEFS := \
    -DAWCHIP=0x1817 \
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

# SDK include roots, used only by the vendor comparison builds.
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

# The whole daemon at -O2 fails bring-up on camera; only the clean tiers get it.
OPT ?= -O0
CLEAN_OPT ?= -O2
# Clean-room headers (freewinner mw_headers, forwarding to isp/include) go first.
ifeq ($(FREECODEC_HEADERS),1)
FREECODEC_HEADERS_DIR := $(FW_ROOT)/mw_headers
INC := -I$(FREECODEC_HEADERS_DIR) -I$(FREECODEC_HEADERS_DIR)/media \
    -I$(FREECODEC_HEADERS_DIR)/utils -I$(FREECODEC_HEADERS_DIR)/component \
    -I$(CINC_DIR) $(INC)
# <linux/videodev2.h> must resolve to the toolchain header, not the SDK copy.
INC := $(filter-out -I$(ISP)/include/V4l2Camera,$(INC))
endif

# -march=armv7ve: hardware sdiv/udiv (divide by zero yields 0, as the ISP tier
# expects); plain armv7-a's __aeabi_idiv raises SIGFPE instead.
CFLAGS := $(CFG_DEFS) $(INC) -I$(BUILD) -Wall -Wno-unused-but-set-variable -Wno-unused-variable \
    -Wno-unused-function -Wno-unused-label -Wno-multichar -fPIC -g $(OPT) -march=armv7ve \
    $(EXTRA_CFLAGS)
CXXFLAGS := $(CFLAGS)

SRC_APP := main.c stub_audio_components.c fshare.c audio_codec.c mediad_audio.c mediad_venc.c mediad_hevc.c talkback.c osd.c stock_reg.c isp_control.c isp_config.c rmm_tuning.c

# STOCK_REG=1: replay a locally captured register table (tools/make_stock_reg.py).
ifeq ($(STOCK_REG),1)
STOCK_REG_TBL := $(wildcard isp_cfg/stock_reg_tbl.c)
ifneq ($(STOCK_REG_TBL),)
SRC_APP += $(STOCK_REG_TBL)
CFLAGS += -DHAVE_STOCK_REG
CXXFLAGS += -DHAVE_STOCK_REG
endif
endif # STOCK_REG

# Vendor MPP capture layer (dropped by FREECODEC_FCAP=1).
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

# Encoder and audio are our own (mediad_venc.c, mediad_audio.c, talkback.c).
SRC_AUDIO :=


ifeq ($(VENDOR_MEDIA_UTILS),1)
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
# Clean-room media utils: 4-byte enums, as they share structs with the VI glue.
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
SRC_MEDIA_UTILS_CXX :=

ifneq ($(VENDOR_MEDIA_UTILS),1)
CLEAN_MU_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(SRC_MEDIA_UTILS))))
$(CLEAN_MU_OBJS): CFLAGS += -I$(CINC_DIR)
endif

# Vendor libisp framework (vendor comparison builds only).
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
SRC_INIPARSER :=

# Clean-room libisp framework (freewinner) unless VENDOR_FW=1.
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

# Clean-room 3A: one core + one SDK-boundary shim per module, plus the layer
# that feeds in the camera's own rmm tables.
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
    $(FW_GIT)/src/tables/pltm_presets.c \
    $(FW_GIT)/src/tables/ae_out_bias.c
# Clean register/config/base tier, each behind an SDK-ABI shim.
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

# SDK libcedarc glue (vendor comparison builds only).
SRC_CDC_BASE := $(wildcard $(CEDARC)/base/*.c)
SRC_MEMADAPTER := \
    $(CEDARC)/memory/memoryAdapter.c \
    $(CEDARC)/memory/ionMemory/ionAlloc.c
SRC_VENCODER := $(CEDARC)/vencoder/vencoder.c
SRC_VENC_BASE := \
    $(CEDARC)/vencoder/base/EncAdapter.c \
    $(CEDARC)/vencoder/base/FrameBufferManager.c \
    $(CEDARC)/vencoder/base/BitstreamManager.c

# The cedarc glue lives in libvenc_base.so, not the daemon: one copy of the
# memory-adapter state, shared with the vendor encoder .so when it is used.
ALL_C_SRC := $(SRC_APP) $(SRC_MPI) $(SRC_AUDIO) $(SRC_MEDIA_UTILS) $(SRC_ISP) $(SRC_INIPARSER) \
    $(SRC_VENCODER)
ALL_CXX_SRC := $(SRC_MEDIA_UTILS_CXX)

ifeq ($(FREECODEC_FENC),1)
ALL_C_SRC := $(filter-out $(SRC_VENCODER),$(ALL_C_SRC))
endif

ifeq ($(FREECODEC_FISP),1)
ALL_C_SRC := $(filter-out $(MEDIA)/mpi_isp.c,$(ALL_C_SRC))
endif

ifeq ($(FREECODEC_FCAP),1)
ALL_C_SRC := $(filter-out $(SRC_MPI),$(ALL_C_SRC))
endif

# The clean ISP tier uses the RTOS-521 short-enum ABI, so it needs ALGO_RTOS=1.
ifeq ($(ALGO_RTOS),1)
ALL_C_SRC += $(CLEAN_CORE_SRC) $(CLEAN_SHIM_SRC) $(CLEAN_INTEG_SRC) $(CLEAN_REG_SRC)
ALL_C_SRC += $(CLEAN_FW_SRC)
endif

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(ALL_C_SRC))) \
        $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(ALL_CXX_SRC)))

# Only TUs sharing ISP-521 structs get -fshort-enums; the encoder/VI side stays
# 4-byte (a short-enum encoder config is 188 bytes, not 200, and breaks it).
ifeq ($(ALGO_RTOS),1)
CFLAGS += -DMEDIAD_ALGO_RTOS=1
CXXFLAGS += -DMEDIAD_ALGO_RTOS=1
RTOS_ENUM_SRC := $(ISP)/isp.c $(ISP)/isp_tuning/isp_tuning.c \
                 $(ISP)/isp_manage/isp_manage.c $(ISP)/isp_manage/isp_helper.c \
                 isp_config.c stub_audio_components.c stock_reg.c \
                 mpi_isp.c isp_control.c
RTOS_ENUM_OBJS := $(addprefix $(BUILD)/,$(patsubst %.c,%.o,$(notdir $(RTOS_ENUM_SRC))))
$(RTOS_ENUM_OBJS): CFLAGS += -fshort-enums -DISP521_RTOS_ALGO=1
endif

# Clean tier: short-enum ISP-521 ABI; -fwrapv/gnu99 as in freewinner's own build.
ifeq ($(ALGO_RTOS),1)
CLEAN_CORE_OBJS  := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_CORE_SRC))))
CLEAN_SHIM_OBJS  := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_SHIM_SRC))))
CLEAN_INTEG_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_INTEG_SRC))))
CLEAN_REG_OBJS   := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_REG_SRC))))
CLEAN_INC_FLAGS  := -I$(CINC_DIR) -I$(INTEG_DIR)
$(CLEAN_CORE_OBJS) $(CLEAN_SHIM_OBJS) $(CLEAN_INTEG_OBJS) $(CLEAN_REG_OBJS): CFLAGS += \
    $(CLEAN_OPT) $(CLEAN_INC_FLAGS) -fshort-enums -DISP521_RTOS_ALGO=1 -DISP_VERSION=521 \
    -fwrapv -std=gnu99

# Framework tier: short enums, except isp_dev_uapi.o (shares structs with VI glue).
CLEAN_FW_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(CLEAN_FW_SRC))))
$(BUILD)/events.o $(BUILD)/helper.o $(BUILD)/manage.o \
$(BUILD)/tuning.o $(BUILD)/isp.o: CFLAGS += \
    $(CLEAN_OPT) $(CLEAN_INC_FLAGS) -fshort-enums -DISP521_RTOS_ALGO=1 -DISP_VERSION=521 \
    -fwrapv -std=gnu99
$(BUILD)/isp_dev_uapi.o: CFLAGS += $(CLEAN_OPT) $(CLEAN_INC_FLAGS) -DISP_VERSION=521 -fwrapv -std=gnu99

# The shims export the SDK names, so the clean spellings are renamed.
$(BUILD)/module_cfg.o: CFLAGS += -Disp_hardware_update=clean_isp_hardware_update -Disp_map_addr=clean_isp_map_addr

$(BUILD)/base.o: CFLAGS += -Dconfig_band_step=clean_config_band_step -Dconfig_lens_center=clean_config_lens_center -Dconfig_dig_gain=clean_config_dig_gain -Dconfig_gamma=clean_config_gamma -Dconfig_wdr=clean_config_wdr -Disp_apply_colormatrix=clean_isp_apply_colormatrix -D__isp_stat_dynamic_judge=clean___isp_stat_dynamic_judge -Disp_handle_stats=clean_isp_handle_stats -Disp_handle_stats_sync=clean_isp_handle_stats_sync -Disp_apply_settings=clean_isp_apply_settings -Dconfig_lens_table=clean_config_lens_table -Dconfig_msc_table=clean_config_msc_table

$(BUILD)/iso_clean.o:  CFLAGS += -Diso_init=clean_iso_init -Diso_exit=clean_iso_exit -Diso_get_params=clean_iso_get_params -Diso_set_params=clean_iso_set_params -Diso_run=clean_iso_run
$(BUILD)/ae_clean.o:   CFLAGS += -Dae_init=clean_ae_init -Dae_exit=clean_ae_exit -Dae_get_params=clean_ae_get_params -Dae_set_params=clean_ae_set_params -Dae_run=clean_ae_run -Dae_isr=clean_ae_isr -Dae_stats_t=clean_ae_stats_t -Dae_result_t=clean_ae_result_t
$(BUILD)/awb_clean.o:  CFLAGS += -Dawb_init=clean_awb_init -Dawb_exit=clean_awb_exit -Dawb_get_params=clean_awb_get_params -Dawb_set_params=clean_awb_set_params -Dawb_run=clean_awb_run -Dawb_isr=clean_awb_isr
$(BUILD)/afs_clean.o:  CFLAGS += -Dafs_clean_init=clean_afs_init -Dafs_clean_exit=clean_afs_exit -Dafs_clean_get_params=clean_afs_get_params -Dafs_clean_set_params=clean_afs_set_params -Dafs_clean_run=clean_afs_run -Dafs_clean_isr=clean_afs_isr
$(BUILD)/gtm_clean.o:  CFLAGS += -Dgtm_init=clean_gtm_init -Dgtm_exit=clean_gtm_exit -Dgtm_get_params=clean_gtm_get_params -Dgtm_set_params=clean_gtm_set_params -Dgtm_run=clean_gtm_run
$(BUILD)/pltm_clean.o: CFLAGS += -Dpltm_init=clean_pltm_init -Dpltm_exit=clean_pltm_exit -Dpltm_get_params=clean_pltm_get_params -Dpltm_set_params=clean_pltm_set_params -Dpltm_run=clean_pltm_run -Dpltm_set_default_result=clean_pltm_set_default_result

# freeisp_get_tables() is defined by five shims; rename it per module.
$(BUILD)/iso_clean.o  $(BUILD)/iso_shim.o:  CFLAGS += -Dfreeisp_get_tables=clean_iso_get_tables
$(BUILD)/ae_clean.o   $(BUILD)/ae_shim.o:   CFLAGS += -Dfreeisp_get_tables=clean_ae_get_tables
$(BUILD)/awb_clean.o  $(BUILD)/awb_shim.o:  CFLAGS += -Dfreeisp_get_tables=clean_awb_get_tables
$(BUILD)/gtm_clean.o  $(BUILD)/gtm_shim.o:  CFLAGS += -Dfreeisp_get_tables=clean_gtm_get_tables
$(BUILD)/pltm_clean.o $(BUILD)/pltm_shim.o: CFLAGS += -Dfreeisp_get_tables=clean_pltm_get_tables
endif

# VENDOR_3A: objcopy-rename the shim's SDK entry points so the framework binds
# the kept vendor member (-D cannot: the shims #undef those names).
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

CEDARC_LIBDIR := $(CEDARC)/library
AUDIO_CODEC_LIBDIR := $(LIB)/AudioLib/lib
ALSA_LIBDIR := prebuilt

# ALGO_RTOS=1: every member of the Melis RTOS algorithm archive is dropped, so
# the linked copy is empty; VENDOR_3A/VENDOR_REG/... keep chosen members.
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

ifeq ($(ALGO_RTOS),1)
$(ISP_ALGO_RTOS_FRAMEWORK): $(ISP_ALGO_RTOS_ARCHIVE) | $(BUILD)
	cp $< $@
	$(AR) d $@ $(ISP_ALGO_RTOS_DROP)
endif
CEDARC_SO_DIR  := $(CEDARC)/library
VENCBASE_SRCS  := $(SRC_VENC_BASE) $(SRC_CDC_BASE) $(SRC_MEMADAPTER)
VENCBASE_SO    := $(BUILD)/libvenc_base.so

# Clean-room H.264 encoder objects, in place of -lvenc_codec/-lVE.
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
# Clean-room H.265 encoder device. The framework's device table references
# video_encoder_h265 even when the H.264 device is the selected one, so its
# objects are always linked with the clean codec (they are inert until a
# channel selects H.265).
FREECODEC_H265_DIR  := $(FREECODEC_DIR)/src/h265
FREECODEC_H265_SRCS := \
    $(FREECODEC_H265_DIR)/regs/h265_regs.c \
    $(FREECODEC_H265_DIR)/headers/h265_headers.c \
    $(FREECODEC_H265_DIR)/rc/h265_rc.c \
    $(FREECODEC_H265_DIR)/enc/freecodec_h265_enc.c \
    $(FREECODEC_H265_DIR)/enc/freecodec_h265_enc_regs.c
FREECODEC_H265_OBJS := $(patsubst $(FREECODEC_H265_DIR)/%.c,$(BUILD)/fc_h265/%.o,$(FREECODEC_H265_SRCS))
H264_CODEC_LINK := $(FREECODEC_H264_OBJS) $(FREECODEC_H265_OBJS) -lvenc_base
# Own flag set: the codec sees only freecodec headers, never an SDK root.
FREECODEC_H264_CFLAGS := $(CFG_DEFS) -Wall -Wno-unused-but-set-variable \
    -Wno-unused-variable -I$(FREECODEC_DIR)/include $(EXTRA_CFLAGS)
$(BUILD)/fc_h264/%.o: $(FREECODEC_H264_DIR)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(FREECODEC_H264_CFLAGS) -c $< -o $@
FREECODEC_H265_CFLAGS := $(FREECODEC_H264_CFLAGS) -I$(FREECODEC_H265_DIR)/enc
$(BUILD)/fc_h265/%.o: $(FREECODEC_H265_DIR)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(FREECODEC_H265_CFLAGS) -c $< -o $@
else
FREECODEC_H265_OBJS :=
FREECODEC_H264_OBJS :=
H264_CODEC_LINK := -lvenc_base -lvenc_codec -lVE
endif

# Clean-room vencoder API (libfenc.a), in place of the SDK's vencoder.c.
FREECODEC_FENC_LIB :=
ifeq ($(FREECODEC_FENC),1)
FREECODEC_FENC_BUILD  := build_arm_fenc
FREECODEC_FENC_ARCHIVE := $(FREECODEC_DIR)/$(FREECODEC_FENC_BUILD)/libfenc.a
FREECODEC_FENC_LIB    := $(FREECODEC_FENC_ARCHIVE)
$(FREECODEC_FENC_ARCHIVE): FORCE
	$(MAKE) -C $(FREECODEC_DIR) CC=$(CC) AR=$(AR) BUILD=$(FREECODEC_FENC_BUILD) fenc
endif

# Clean-room ISP runtime (libfisp.a): the AW_MPI_ISP_* surface.
FREECODEC_FISP_LIB :=
ifeq ($(FREECODEC_FISP),1)
FREECODEC_FISP_BUILD   := build_arm_fisp
FREECODEC_FISP_ARCHIVE := $(FREEWINNER)/$(FREECODEC_FISP_BUILD)/libfisp.a
FREECODEC_FISP_LIB     := $(FREECODEC_FISP_ARCHIVE)
$(FREECODEC_FISP_ARCHIVE): FORCE
	$(MAKE) -C $(FREEWINNER) CC=$(CC) AR=$(AR) BUILD=$(FREECODEC_FISP_BUILD) fisp

# Vendor mpi_vi.o (FCAP off) also defines AW_MPI_ISP_*: strip those so
# libfisp.a supplies them.
$(BUILD)/mpi_vi.o: $(MEDIA)/mpi_vi.c | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<
	for s in `$(NM) $@ | awk '/ T AW_MPI_ISP_/ {print $$3}'`; do \
	    $(OBJCOPY) --strip-symbol=$$s $@; done
endif

# Clean-room capture runtime (libfcap.a): AW_MPI_SYS_* / AW_MPI_VI_*.
FREECODEC_FCAP_LIB :=
ifeq ($(FREECODEC_FCAP),1)
FREECODEC_FCAP_BUILD   := build_arm_fcap
FREECODEC_FCAP_ARCHIVE := $(FREEWINNER)/$(FREECODEC_FCAP_BUILD)/libfcap.a
FREECODEC_FCAP_LIB     := $(FREECODEC_FCAP_ARCHIVE)
$(FREECODEC_FCAP_ARCHIVE): FORCE
	$(MAKE) -C $(FREEWINNER) CC=$(CC) AR=$(AR) BUILD=$(FREECODEC_FCAP_BUILD) fcap
endif

# AAC-LC: freecodec over FAAC.
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

# On-device client for the control socket (isp_control.c).
mediad_ctl: tools/mediad_ctl.c
	$(CC) -O2 -Wall -o $@ $<

# Tuning extractor CLI over the same core as mediad (rmm_tuning.c).
rmm_extract: rmm_extract.c rmm_tuning.c $(RMM_LAYOUT_H)
	$(CC) -O2 -Wall -I$(BUILD) -o $@ rmm_extract.c rmm_tuning.c

$(RMM_LAYOUT_H): tools/gen_rmm_layout.py | $(BUILD)
	python3 tools/gen_rmm_layout.py --abi $(RMM_ABI) -o $@

$(BUILD)/rmm_tuning.o: $(RMM_LAYOUT_H)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MD -MP -c -o $@ $<

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MD -MP -c -o $@ $<

# libvenc_base.so: clean-room build from freewinner codec/, or the SDK glue.
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

# Always rebuilt. FAAC is passed relative so assert() strings carry no host path
# (GCC 6.4 has no -ffile-prefix-map).
FAAC_REL = $(shell realpath --relative-to=$(FREECODEC_DIR) $(MD)/../../repos/faac)
$(FREECODEC_AAC): FORCE
	test -n "$(FREECODEC_DIR)"
	rm -f $(FREECODEC_DIR)/build_arm/libfreecodec_aac.a $(FREECODEC_DIR)/build_arm/freecodec_aac.o
	rm -rf $(FREECODEC_DIR)/build_arm/faac
	$(MAKE) -C $(FREECODEC_DIR) arm-aac \
	    TC=$(TC)/bin/arm-openwrt-linux-muslgnueabi \
	    FAAC_DIR=$(FAAC_REL)

$(TARGET): $(OBJS) $(RMM_LAYOUT_H) $(VENCBASE_SO) $(FREECODEC_AAC) $(FREECODEC_H264_OBJS) $(FREECODEC_H265_OBJS) $(FREECODEC_FENC_LIB) $(FREECODEC_FISP_LIB) $(FREECODEC_FCAP_LIB)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

ifeq ($(ALGO_RTOS),1)
$(TARGET): $(ISP_ALGO_RTOS_FRAMEWORK)
endif

clean:
	rm -rf $(BUILD) $(TARGET) mediad_ctl rmm_extract

-include $(OBJS:.o=.d)

# Host test of the H.265 policy/setup unit (stub encoder, no hardware).
FW_ROOT_T := $(abspath ../../../../freewinner/freewinner-git)
.PHONY: test-hevc
test-hevc: tests/test_hevc.c mediad_hevc.c mediad_hevc.h
	cc -std=gnu11 -Wall -I. -I$(FW_ROOT_T)/mw_headers -I$(FW_ROOT_T)/codec/include -o /tmp/mediad_test_hevc tests/test_hevc.c mediad_hevc.c
	/tmp/mediad_test_hevc
	rm -f /tmp/mediad_test_hevc
