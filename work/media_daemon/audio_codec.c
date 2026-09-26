// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/*
 * audio_codec.c - enable the SoC codec / DAUDIO AI hub before ALSA capture.
 *
 * Stock rmm enables these as part of its audio init; the SDK's alsaOpenMixer
 * does not, so after we removed rmm from the boot path the AI (mic) PCM read
 * fails with EIO and AENC gets no input. Controls (seen via tools/mixer_probe):
 *   card 0: "codec hub mode"               enum: 0=disable 1=enable
 *   card 1: "sunxi daudio audio hub mode"  enum: 0=null 1=disable 2=enable
 *   card 1: "sunxi daudio loopback debug"  switch
 * Stock sets them to 1 / 2 / 1. See other.md.
 */
#include <string.h>
#include <alsa/asoundlib.h>

static const char *CARDS[] = { "default", "hw:0", "hw:1", "hw:2" };

static void apply_on_card(const char *card)
{
    snd_mixer_t *m = NULL;
    snd_mixer_elem_t *e;

    if (snd_mixer_open(&m, 0) < 0)
        return;
    if (snd_mixer_attach(m, card) < 0) {
        snd_mixer_close(m);
        return;
    }
    snd_mixer_selem_register(m, NULL, NULL);
    if (snd_mixer_load(m) < 0) {
        snd_mixer_close(m);
        return;
    }
    for (e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
        const char *n = snd_mixer_selem_get_name(e);
        if (!strcmp(n, "codec hub mode") && snd_mixer_selem_is_enumerated(e)) {
            snd_mixer_selem_set_enum_item(e, 0, 1); /* hub_enable */
        } else if (!strcmp(n, "sunxi daudio audio hub mode") &&
                   snd_mixer_selem_is_enumerated(e)) {
            snd_mixer_selem_set_enum_item(e, 0, 2); /* hub_enable */
        } else if (!strcmp(n, "sunxi daudio loopback debug")) {
            if (snd_mixer_selem_has_playback_switch(e))
                snd_mixer_selem_set_playback_switch(e, 0, 1);
            if (snd_mixer_selem_has_capture_switch(e))
                snd_mixer_selem_set_capture_switch(e, 0, 1);
        } else if (!strcmp(n, "Left Input Mixer MIC1 Boost")) {
            /* The SDK's alsaOpenMixer sets AUDIO_LADC_MIC1_SWITCH, but that macro
             * name does not match this control, so the mic stays off. */
            if (snd_mixer_selem_has_playback_switch(e))
                snd_mixer_selem_set_playback_switch(e, 0, 1);
        } else if (!strcmp(n, "Left Input Mixer LINEINL")) {
            if (snd_mixer_selem_has_playback_switch(e))
                snd_mixer_selem_set_playback_switch(e, 0, 1);
        } else if (!strcmp(n, "MIC1 gain volume")) {
            if (snd_mixer_selem_has_capture_volume(e))
                snd_mixer_selem_set_capture_volume(e, 0, 30);   /* match stock */
            if (snd_mixer_selem_has_playback_volume(e))
                snd_mixer_selem_set_playback_volume(e, 0, 30);
        }
    }
    snd_mixer_close(m);
}

int audio_codec_hub_enable(void)
{
    unsigned int i;
    for (i = 0; i < sizeof(CARDS) / sizeof(CARDS[0]); i++)
        apply_on_card(CARDS[i]);
    return 0;
}
