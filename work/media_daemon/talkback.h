// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * talkback.h - desktop talkback (speaker playback) for mediad.
 *
 * Stock rmm's other audio half: talkback_rx decodes the desktop's audio and
 * writes 16 kHz mono S16_LE PCM into /tmp/audio_in_fifo; the media daemon is
 * the FIFO reader and plays it to the speaker. mediad originally drove only the
 * capture (AI -> AENC) direction, so talkback went nowhere.
 *
 * talkback_start() brings up a direct ALSA playback stream on the board's
 * codec (card 0, "default"; MEDIAD_AO_CARD=1 selects the daudio card hw:1,0)
 * and feeds it from the FIFO; talkback_stop() tears it down. Returns 0 on
 * success, -1 if playback could not be set up.
 */
#ifndef MEDIAD_TALKBACK_H
#define MEDIAD_TALKBACK_H

int talkback_start(void);
void talkback_stop(void);

#endif /* MEDIAD_TALKBACK_H */
