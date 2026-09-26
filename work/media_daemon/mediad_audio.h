/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 yi-mediad contributors */
#ifndef MEDIAD_AUDIO_H
#define MEDIAD_AUDIO_H

/* Mic capture -> AAC-LC -> fshare ring, with no vendor audio code. */
int  mediad_audio_start(void);
void mediad_audio_stop(void);

#endif /* MEDIAD_AUDIO_H */
