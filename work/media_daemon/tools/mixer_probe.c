// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* mixer_probe.c - list the codec hub/daudio ALSA simple-mixer controls with
 * their enum items and current index (to find the mic-path controls). */
#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

int main(int argc, char **argv)
{
    static const char *cards[] = { "default", "hw:0", "hw:1", "hw:2" };
    int c;
    (void)argc; (void)argv;

    for (c = 0; c < (int)(sizeof(cards) / sizeof(cards[0])); c++) {
        snd_mixer_t *m = NULL;
        snd_mixer_elem_t *e;
        printf("==== card %s ====\n", cards[c]);
        if (snd_mixer_open(&m, 0) < 0) { printf("  open fail\n"); continue; }
        if (snd_mixer_attach(m, cards[c]) < 0) { printf("  attach fail\n"); snd_mixer_close(m); continue; }
        snd_mixer_selem_register(m, NULL, NULL);
        if (snd_mixer_load(m) < 0) { printf("  load fail\n"); snd_mixer_close(m); continue; }
        for (e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
            const char *n = snd_mixer_selem_get_name(e);
            int interesting = (strstr(n, "hub") || strstr(n, "daudio") ||
                               strstr(n, "loopback") || strstr(n, "codec"));
            if (interesting) {
                int en = snd_mixer_selem_is_enumerated(e);
                printf("  CTRL '%-34s' enum=%d", n, en);
                if (en) {
                    unsigned int idx = 0, i;
                    char nm[64];
                    if (snd_mixer_selem_get_enum_item(e, 0, &idx) == 0)
                        printf(" current=%u", idx);
                    printf("\n");
                    for (i = 0; i < 16; i++) {
                        if (snd_mixer_selem_get_enum_item_name(e, i, sizeof(nm), nm) < 0)
                            break;
                        printf("      [%u] %s\n", i, nm);
                    }
                } else {
                    printf("\n");
                }
            }
        }
        snd_mixer_close(m);
    }
    return 0;
}
