// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * isp_config.h - our from-scratch ISP tuning + controls (see isp_config.c).
 */
#ifndef MEDIAD_ISP_CONFIG_H
#define MEDIAD_ISP_CONFIG_H

#include "framework_isp.h"

/* Fill the config with our generated tables + module enables. Called from
 * parser_ini_info() during ISP init. */
void isp_config_fill_param(fwi_tuning_image_t *param);

/* Runtime controls. Both edit the config and re-run the libisp config path. */
int isp_config_set_saturation(int v); /* 0..100, 50 == neutral */
int isp_config_set_hue(int v);        /* 0..100, 50 == neutral */
int isp_config_set_brightness(int v); /* 0..100, 50 == neutral */
int isp_config_set_contrast(int v);   /* 0..100, 50 == neutral */
int isp_config_set_denoise(int v);    /* 0..100, 50 == neutral */
int isp_config_set_exposure(int v);   /* 0..100, 50 == neutral (AE EV bias) */
int isp_config_set_sharpness(int v);  /* 0..10, 5 == our default */
int isp_config_set_gamma(int v);      /* 0..100, 50 == neutral (enables gamma) */
int isp_config_set_pltm(int on);      /* 0/1 module enable; 1 == vendor stock */
int isp_config_set_tdf(int on);       /* 0/1 module enable (enable3dnr); 1 == stock */
/* Day/night ISP tuning swap: 0 = load the day config, 1 = the night config
 * (both extracted from the device's rmm.bin), then re-run the config path. */
int isp_config_set_daynight(int night);
int isp_config_get_saturation(void);
int isp_config_get_hue(void);
int isp_config_get_brightness(void);
int isp_config_get_contrast(void);
int isp_config_get_denoise(void);
int isp_config_get_exposure(void);
int isp_config_get_sharpness(void);
int isp_config_get_gamma(void);
int isp_config_get_pltm(void);
int isp_config_get_tdf(void);
int isp_config_want_tdf(void);
int isp_config_set_nr2d(int on);      /* spatial (2D) denoise; 1 == tuning */
int isp_config_want_nr2d(void);
int isp_config_set_cnr(int on);       /* chroma denoise; 1 == tuning */
int isp_config_want_cnr(void);

#endif /* MEDIAD_ISP_CONFIG_H */
