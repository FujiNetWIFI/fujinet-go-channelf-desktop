/*
 * The external-FujiNet address field: "host:port" (a name or an IPv4
 * literal), a bare port, empty text for the default, and everything that
 * must be refused.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "chfsession.h"

static int failures;

static void accepts(const char *text, const char *want_host, int want_port)
{
    char host[64] = "";
    int port = 0;
    int rc = chfsession_parse_hostport(text, host, sizeof(host), &port);
    if (rc != 0 || strcmp(host, want_host) || port != want_port) {
        fprintf(stderr, "FAIL \"%s\": rc %d, got %s:%d, want %s:%d\n", text,
                rc, host, port, want_host, want_port);
        failures++;
    }
}

static void refuses(const char *text)
{
    char host[64] = "keep";
    int port = 7;
    if (chfsession_parse_hostport(text, host, sizeof(host), &port) == 0 ||
        strcmp(host, "keep") || port != 7) {
        fprintf(stderr, "FAIL \"%s\" should be refused untouched\n", text);
        failures++;
    }
}

int main(void)
{
    accepts("", "127.0.0.1", 11526);
    accepts("   ", "127.0.0.1", 11526);
    accepts(NULL, "127.0.0.1", 11526);
    accepts("65300", "127.0.0.1", 65300);
    accepts("192.168.1.20:9985", "192.168.1.20", 9985);
    accepts("  127.0.0.1:1  ", "127.0.0.1", 1);
    accepts("localhost:65535", "localhost", 65535);
    accepts("fujinet.local:11526", "fujinet.local", 11526);
    accepts("my-box_2:80", "my-box_2", 80);

    refuses("0");
    refuses("65536");
    refuses("-5");
    refuses("abc");
    refuses("127.0.0.1:");
    refuses(":11526");
    refuses("127.0.0.1:abc");
    refuses("256.1.1.1:11526");
    refuses("1.2.3:11526");
    refuses("1.2.3.4.5:11526");
    refuses("1.2.3.4:11526x");
    refuses("bad host:11526");
    refuses("a..b:11526");
    refuses(".local:11526");
    refuses("::1:11526");
    return failures ? 1 : 0;
}
