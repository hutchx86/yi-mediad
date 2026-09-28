// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* isp_control.h - runtime picture-control surface: the unix control socket
 * (fed by yi-protect's client and mediad_ctl), mediad.conf and the optional
 * web UI. */
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

/* Mounting-orientation base; user mirror/flip apply XOR on top. Call once at
 * boot, after the vipps/ISP are up. */
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

/* If a `dump <path>` request is pending, copy the path, clear it and return 1;
 * stock_reg.c then writes the computed register table there. */
int isp_control_pending_dump(char *out, size_t n);

/* 32-bit edit at byte `off` of the replayed table; 0 on success, -1 if there
 * is no stock table. */
int isp_stock_reg_poke(unsigned int off, unsigned int val, unsigned int *old);
int isp_stock_reg_peek(unsigned int off, unsigned int *val);

/* Per-control transforms on u16 entries of the replayed table, re-applied to a
 * fresh baseline copy each frame so they do not compound. */
#define ISP_XFORM_SCALE  0  /* v = v * p1 / p2 */
#define ISP_XFORM_OFFSET 1  /* v = v + p1 */
int isp_control_set_xform(int type, unsigned int off, unsigned int len,
                          int p1, int p2);
void isp_control_clear_xforms(void);
int isp_control_xform_count(void);
void isp_control_apply_xforms(unsigned char *dst, unsigned int len);

#endif /* MEDIAD_ISP_CONTROL_H */
