// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* mixer_dump.c - dump every ALSA simple-mixer control (card, name, values) as
 * sorted text, so the state with stock rmm vs mediad can be diffed to find the
 * mic-path controls the SDK init misses. */
#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

static const char *CARDS[] = { "hw:0", "hw:1" };

int main(void)
{
    unsigned int c;
    for (c = 0; c < sizeof(CARDS) / sizeof(CARDS[0]); c++) {
        snd_mixer_t *m = NULL;
        snd_mixer_elem_t *e;
        if (snd_mixer_open(&m, 0) < 0) continue;
        if (snd_mixer_attach(m, CARDS[c]) < 0) { snd_mixer_close(m); continue; }
        snd_mixer_selem_register(m, NULL, NULL);
        if (snd_mixer_load(m) < 0) { snd_mixer_close(m); continue; }
        for (e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
            const char *n = snd_mixer_selem_get_name(e);
            if (snd_mixer_selem_is_enumerated(e)) {
                unsigned int idx = 0;
                char nm[64] = "?";
                snd_mixer_selem_get_enum_item(e, 0, &idx);
                snd_mixer_selem_get_enum_item_name(e, idx, sizeof(nm), nm);
                printf("%s|%s|enum|%u:%s\n", CARDS[c], n, idx, nm);
            } else {
                long v = 0;
                if (snd_mixer_selem_has_playback_switch(e)) {
                    int sw = 0; snd_mixer_selem_get_playback_switch(e, 0, &sw);
                    printf("%s|%s|pbswitch|%d\n", CARDS[c], n, sw);
                }
                if (snd_mixer_selem_has_capture_switch(e)) {
                    int sw = 0; snd_mixer_selem_get_capture_switch(e, 0, &sw);
                    printf("%s|%s|capswitch|%d\n", CARDS[c], n, sw);
                }
                if (snd_mixer_selem_has_playback_volume(e)) {
                    snd_mixer_selem_get_playback_volume(e, 0, &v);
                    printf("%s|%s|pbvol|%ld\n", CARDS[c], n, v);
                }
                if (snd_mixer_selem_has_capture_volume(e)) {
                    snd_mixer_selem_get_capture_volume(e, 0, &v);
                    printf("%s|%s|capvol|%ld\n", CARDS[c], n, v);
                }
            }
        }
        snd_mixer_close(m);
    }
    return 0;
}
