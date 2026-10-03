// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 yi-mediad contributors
/* mediad_ctl.c - client for mediad's control socket (MEDIAD_CTL_SOCK, default
 * /tmp/mediad_ctl.sock), e.g. `mediad_ctl set saturation 60`, `mediad_ctl list`. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DEFAULT_SOCK "/tmp/mediad_ctl.sock"

int main(int argc, char **argv)
{
    const char *path = getenv("MEDIAD_CTL_SOCK");
    struct sockaddr_un sa;
    char cmd[256], reply[512];
    int fd, n, done = 0;

    if (!path || !path[0])
        path = DEFAULT_SOCK;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s {list|reset|get <key>|set <key> <value>|pin <key> <value>|"
                "unpin <key>|pinned <key>|dump <path>|"
                "peek <off>|poke <off> <val>|scale <off> <len> <num> <den>|"
                "offset <off> <len> <delta>|xclear}\n", argv[0]);
        return 2;
    }
    cmd[0] = 0;
    if (!strcmp(argv[1], "list") || !strcmp(argv[1], "reset")) {
        snprintf(cmd, sizeof(cmd), "%s\n", argv[1]);
    } else if (!strcmp(argv[1], "get") && argc == 3) {
        snprintf(cmd, sizeof(cmd), "get %s\n", argv[2]);
    } else if (!strcmp(argv[1], "set") && argc == 4) {
        snprintf(cmd, sizeof(cmd), "set %s %s\n", argv[2], argv[3]);
    } else if (!strcmp(argv[1], "pin") && argc == 4) {
        snprintf(cmd, sizeof(cmd), "pin %s %s\n", argv[2], argv[3]);
    } else if ((!strcmp(argv[1], "unpin") || !strcmp(argv[1], "pinned")) && argc == 3) {
        snprintf(cmd, sizeof(cmd), "%s %s\n", argv[1], argv[2]);
    } else if (!strcmp(argv[1], "dump") && argc == 3) {
        snprintf(cmd, sizeof(cmd), "dump %s\n", argv[2]);
    } else if (!strcmp(argv[1], "peek") && argc == 3) {
        snprintf(cmd, sizeof(cmd), "peek %s\n", argv[2]);
    } else if (!strcmp(argv[1], "poke") && argc == 4) {
        snprintf(cmd, sizeof(cmd), "poke %s %s\n", argv[2], argv[3]);
    } else if (!strcmp(argv[1], "scale") && argc == 6) {
        snprintf(cmd, sizeof(cmd), "scale %s %s %s %s\n",
                 argv[2], argv[3], argv[4], argv[5]);
    } else if (!strcmp(argv[1], "offset") && argc == 5) {
        snprintf(cmd, sizeof(cmd), "offset %s %s %s\n",
                 argv[2], argv[3], argv[4]);
    } else if (!strcmp(argv[1], "xclear")) {
        snprintf(cmd, sizeof(cmd), "xclear\n");
    } else {
        fprintf(stderr,
                "usage: %s {list|reset|get <key>|set <key> <value>|pin <key> <value>|"
                "unpin <key>|pinned <key>|dump <path>|"
                "peek <off>|poke <off> <val>|scale <off> <len> <num> <den>|"
                "offset <off> <len> <delta>|xclear}\n", argv[0]);
        return 2;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
        perror("write");
        close(fd);
        return 1;
    }
    while (!done && (n = read(fd, reply, sizeof(reply) - 1)) > 0) {
        int i;
        for (i = 0; i < n; i++)
            if (reply[i] == '\n')
                done = 1;
        reply[n] = 0;
        fputs(reply, stdout);
    }
    close(fd);
    return 0;
}
