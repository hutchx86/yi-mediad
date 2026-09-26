// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * Stub linux/ion_uapi.h.
 *
 * system/public/libion/include/ion_memmanager.h wants this vendor-specific
 * uapi header (the sunxi ion driver's ioctl/struct definitions), but it isn't
 * present anywhere in this softwinner tree (grepped - genuinely missing, not
 * just misplaced). We don't compile ion_memmanager.c or call any of its
 * functions in this VI/VENC-only prototype (memoryAdapter.c/ionAlloc.c use
 * their own ioctl wrappers, not this API) - we only need ion_memmanager.h's
 * declarations to parse because mpi_sys.c includes it unconditionally. The
 * one type it actually needs from here is ion_user_handle_t, confirmed to be
 * a plain int from the commented-out reference in
 * system/public/libion/backup/ion.h. If real ion_memmanager.c usage is ever
 * added, this stub must be replaced with the real vendor header (find it in
 * the matching kernel source/BSP, not invented further).
 */
#ifndef RMM_REPLACEMENT_STUB_ION_UAPI_H
#define RMM_REPLACEMENT_STUB_ION_UAPI_H

typedef int ion_user_handle_t;

#endif
