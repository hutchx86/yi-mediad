// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* talkback.h - speaker playback: 16 kHz mono S16_LE PCM from
 * /tmp/audio_in_fifo (written by talkback_rx) to ALSA "default", or hw:1,0
 * with MEDIAD_AO_CARD=1. talkback_start() returns 0, or -1 on failure. */
#ifndef MEDIAD_TALKBACK_H
#define MEDIAD_TALKBACK_H

int talkback_start(void);
void talkback_stop(void);

#endif /* MEDIAD_TALKBACK_H */
