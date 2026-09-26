// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * isp_control.h - runtime ISP picture-control surface for mediad.
 *
 * Phase 1b (todo.md): unifi_client_go/avclientd forwards Protect's picture
 * settings to mediad over a unix socket; mediad applies them with the vendor
 * AW_MPI_ISP_Set* API. Because stock_reg.c replays a captured stock register
 * table, the setter effect on our own computed table would otherwise be
 * discarded, so for the controls that live in the shared module-register block
 * (first 0x1000) we also overlay those words from the computed table onto the
 * replayed stock table each frame (the same trick already used for the AWB
 * gains at 0x370).
 *
 * Values on the wire are the vendor API's raw values (mpi_isp.h ranges), not
 * Protect's 0-100 UI scale - the controller-side mapping lives in goclient.
 */
#ifndef MEDIAD_ISP_CONTROL_H
#define MEDIAD_ISP_CONTROL_H

/* Overlay mask bits: which control register ranges stock_reg.c copies from our
 * computed table onto the replayed stock table. Set/cleared by isp_control.c. */
#define ISP_CTL_SATURATION (1u << 0)
#define ISP_CTL_SHARPNESS  (1u << 1)
#define ISP_CTL_NR         (1u << 2)
#define ISP_CTL_3DNR       (1u << 3)
#define ISP_CTL_PLTMWDR    (1u << 4)

/* Runtime encoder bitrate (bps) by channel name ("high"/"low"), implemented in
 * main.c. Clamped to [48 k, 3 M] internally. Used by the `bitrate` control. */
int mediad_set_bitrate(const char *name, unsigned int bps);
unsigned int mediad_get_bitrate(const char *name);
int mediad_set_venc3d(int level);     /* encoder 3D filter, all channels */
int mediad_get_venc3d(void);

/* Shutter exposure mode (0 auto, 1 preview/short, 2 night/long), main.c. */
int mediad_set_shutter(int mode);

/* Start the control-socket listener thread. `isp_dev` is the running ISP
 * device the setters act on. Returns 0 on success. Idempotent. */
int isp_control_start(int isp_dev);

/* Set the model's mounting-orientation BASE (capability table, e.g. r35gb
 * 1/1). The user's mirror/flip are applied relative to it (hardware =
 * base XOR user). Call once at boot, after the vipps/ISP are up. */
void isp_control_set_orientation(int base_mirror, int base_flip);

/* Read a mediad.conf (key=value, keys == control keys) and queue the values;
 * `webui=1` (+ `webui_port=`) also starts a small built-in slider UI. */
int isp_control_load_config(const char *path);
int isp_control_start_webui(int port);

/* Stop the listener and remove the socket. */
void isp_control_stop(void);

/* Current overlay mask (read by stock_reg.c every load-reg). */
unsigned int isp_control_overlay_mask(void);

/* Copy the masked control register ranges from `computed` onto `dst`. `len` is
 * the size of the smaller of the two buffers. Called from stock_reg.c. */
void isp_control_apply_overlay(unsigned char *dst, const unsigned char *computed,
                               unsigned int len);

/* Debug/capture: if a `dump <path>` request is pending, copy the path and
 * return 1 (and clear it). stock_reg.c then writes the computed (pre-replay)
 * register table there on the next load-reg. Used by the 7a setter harness. */
int isp_control_pending_dump(char *out, size_t n);

/* Direct edits to the replayed stock register table (Phase 1b applier
 * experiments). `off` is a byte offset into the table, `val` a 32-bit word.
 * Return 0 on success. No-ops (return -1) if there is no stock table. */
int isp_stock_reg_poke(unsigned int off, unsigned int val, unsigned int *old);
int isp_stock_reg_peek(unsigned int off, unsigned int *val);

/* Calibrated per-control transforms on the replayed table. Each is applied to a
 * fresh copy of the baseline table on every frame, so scale factors do not
 * compound. Entries are u16 (16-bit) values. */
#define ISP_XFORM_SCALE  0  /* v = v * p1 / p2 */
#define ISP_XFORM_OFFSET 1  /* v = v + p1 */
int isp_control_set_xform(int type, unsigned int off, unsigned int len,
                          int p1, int p2);
void isp_control_clear_xforms(void);
int isp_control_xform_count(void);
void isp_control_apply_xforms(unsigned char *dst, unsigned int len);

#endif /* MEDIAD_ISP_CONTROL_H */
