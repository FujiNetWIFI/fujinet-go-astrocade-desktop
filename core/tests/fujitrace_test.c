/*
 * fujitrace_test -- the debugger's FujiNet trace decoder on synthetic bytes:
 * both directions, SLIP escapes, input split anywhere, a cut-short reply,
 * repeats folded into x N, the ring's limit, and the empty state. No ROM, no
 * network.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fujibus.h"
#include "fujitrace.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static fujitrace t;                     /* large: not on the stack */
static astrodebug_fuji_packet p[FUJITRACE_MAX + 8];

/* A frame as the cart sends it (dir 1) or receives it (dir 0). */
static size_t frame(uint8_t device, uint8_t command, const uint8_t *data,
                    uint16_t len, uint8_t *out, size_t cap)
{
    return fujibus_build_request(device, command, NULL, 0, data, len, out, cap);
}

static void feed_frame(int dir, uint8_t device, uint8_t command,
                       const uint8_t *data, uint16_t len)
{
    uint8_t buf[1024];
    size_t n = frame(device, command, data, len, buf, sizeof buf);
    fujitrace_feed(&t, dir, buf, n);
}

int main(void)
{
    uint8_t buf[1024];
    size_t n;
    int k;

    /* empty */
    CHECK(fujitrace_copy(&t, p, FUJITRACE_MAX) == 0, "empty trace has no packets");

    /* a request: C0 70 E8 06 00 sum 00 C0 */
    feed_frame(1, 0x70, 0xE8, NULL, 0);
    k = fujitrace_copy(&t, p, FUJITRACE_MAX);
    CHECK(k == 1, "one request");
    CHECK(p[0].to_host == 0, "request goes to FujiNet");
    CHECK(p[0].device == 0x70 && p[0].command == 0xE8, "request header");
    CHECK(p[0].length == 6 && p[0].nbytes == 6, "request length");
    CHECK(p[0].repeat == 1, "request once");
    CHECK(strcmp(p[0].text, "Astrocade \xe2\x86\x92 FujiNet 70 E8 GET ADAPTERCONFIG (6 bytes)") == 0,
          "request text");

    /* a reply carrying both SLIP specials, fed a byte at a time */
    {
        const uint8_t data[] = { 0xC0, 0x01, 0xDB, 0x02 };
        n = frame(0x70, 0x06, data, sizeof data, buf, sizeof buf);
        CHECK(n == 2 + 6 + 4 + 2, "two escapes on the wire");
        for (size_t i = 0; i < n; i++)
            fujitrace_feed(&t, 0, buf + i, 1);
        k = fujitrace_copy(&t, p, FUJITRACE_MAX);
        CHECK(k == 2, "request and reply");
        CHECK(p[1].to_host == 1, "reply comes from FujiNet");
        CHECK(p[1].nbytes == 10 && p[1].length == 10, "reply unescaped to 10 bytes");
        CHECK(memcmp(p[1].data + 6, data, sizeof data) == 0, "reply payload unescaped");
        CHECK(strstr(p[1].text, "FujiNet \xe2\x86\x92 Astrocade 70 06 ACK (10 bytes)") != NULL,
              "reply text");
        CHECK(p[1].serial > p[0].serial, "serials rise");
    }

    /* a reply cut short, then the next request: the stub is dropped */
    fujitrace_clear(&t);
    CHECK(fujitrace_copy(&t, p, FUJITRACE_MAX) == 0, "cleared");
    n = frame(0x70, 0x06, (const uint8_t *)"abcdef", 6, buf, sizeof buf);
    fujitrace_feed(&t, 0, buf, n - 4);
    feed_frame(1, 0x70, 0xF4, NULL, 0);
    feed_frame(0, 0x70, 0x06, (const uint8_t *)"xy", 2);
    k = fujitrace_copy(&t, p, FUJITRACE_MAX);
    CHECK(k == 2, "cut-short reply dropped");
    CHECK(k == 2 && p[0].command == 0xF4 && p[1].nbytes == 8, "request, then the whole reply");

    /* a short frame between delimiters */
    fujitrace_clear(&t);
    {
        const uint8_t junk[] = { 0xC0, 0x12, 0x34, 0xC0 };
        fujitrace_feed(&t, 0, junk, sizeof junk);
        k = fujitrace_copy(&t, p, FUJITRACE_MAX);
        CHECK(k == 1 && p[0].device == -1 && p[0].nbytes == 2, "short frame kept, no header");
    }

    /* an exchange repeating folds into x N on both lines */
    fujitrace_clear(&t);
    for (int i = 0; i < 3; i++) {
        feed_frame(1, 0x70, 0xFA, NULL, 0);
        feed_frame(0, 0x70, 0x06, (const uint8_t *)"\x03", 1);
    }
    k = fujitrace_copy(&t, p, FUJITRACE_MAX);
    CHECK(k == 2 && p[0].repeat == 3 && p[1].repeat == 3, "exchange x3");
    /* the same request again: listed while its reply is outstanding */
    feed_frame(1, 0x70, 0xFA, NULL, 0);
    k = fujitrace_copy(&t, p, FUJITRACE_MAX);
    CHECK(k == 3 && p[0].repeat == 3 && p[2].repeat == 1, "a fourth request shows");
    CHECK(k == 3 && p[2].serial > p[1].serial, "and changes the last serial");
    /* a different reply: no fold */
    feed_frame(0, 0x70, 0x06, (const uint8_t *)"\x04", 1);
    k = fujitrace_copy(&t, p, FUJITRACE_MAX);
    CHECK(k == 4, "a different reply ends the run");
    CHECK(k == 4 && p[0].repeat == 3 && p[1].repeat == 3 && p[2].repeat == 1 &&
          p[3].repeat == 1, "x3, x3, then once each");

    /* a lone packet repeating (no reply) folds into one line */
    fujitrace_clear(&t);
    for (int i = 0; i < 4; i++)
        feed_frame(1, 0x70, 0xD2, NULL, 0);
    k = fujitrace_copy(&t, p, FUJITRACE_MAX);
    CHECK(k == 1 && p[0].repeat == 4, "lone request x4");

    /* long packets: hex stops at 64 bytes, but repeats compare all of it */
    fujitrace_clear(&t);
    {
        uint8_t big[200];
        memset(big, 0x55, sizeof big);
        feed_frame(0, 0xFF, 0x57, big, sizeof big);
        big[150] = 0x66;
        feed_frame(0, 0xFF, 0x57, big, sizeof big);
        k = fujitrace_copy(&t, p, FUJITRACE_MAX);
        CHECK(k == 2, "packets differing past 64 bytes stay apart");
        CHECK(k >= 1 && p[0].nbytes == 206 && p[0].data[63] == 0x55, "long packet length and hex");
        CHECK(k >= 1 && strstr(p[0].text, "FF 57 WRITE (206 bytes)") != NULL, "push frame named");
    }

    /* the ring keeps the newest FUJITRACE_MAX, oldest first */
    fujitrace_clear(&t);
    for (int i = 0; i < FUJITRACE_MAX + 100; i++) {
        uint8_t v[2] = { (uint8_t)i, (uint8_t)(i >> 8) };
        feed_frame(1, 0x70, 0x52, v, 2);
    }
    k = fujitrace_copy(&t, p, FUJITRACE_MAX + 8);
    CHECK(k == FUJITRACE_MAX, "ring holds FUJITRACE_MAX");
    CHECK(k == FUJITRACE_MAX && p[0].data[6] == 100 && p[k - 1].data[6] == (uint8_t)(FUJITRACE_MAX + 99),
          "oldest dropped, newest last");
    k = fujitrace_copy(&t, p, 10);
    CHECK(k == 10 && p[9].data[6] == (uint8_t)(FUJITRACE_MAX + 99), "newest 10");

    fprintf(stderr, "fujitrace_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
