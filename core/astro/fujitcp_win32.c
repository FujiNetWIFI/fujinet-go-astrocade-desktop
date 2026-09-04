/* fujitcp_win32.c -- the Winsock twin of the staged POSIX fujitcp.c.
 *
 * The staged emu/fujitcp.c is POSIX sockets (netdb.h, sys/socket.h); a
 * Windows build compiles this instead, against the SAME staged fujitcp.h, so
 * the FujiBus/SLIP-over-TCP wire behaviour is identical -- only the socket
 * calls differ. Kept line-for-line with the POSIX original where Winsock
 * allows, so the two stay diffable (see cmake -- core/CMakeLists.txt swaps
 * this in under WIN32).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (matches the staged fujitcp.c it mirrors; copyright-holders Thomas Cherryhomes)
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujitcp.h"
#include "fujimail.h"
#include "fuji_mailbox.h"

#define RX_RAW_MAX 1088

static SOCKET fd = INVALID_SOCKET;
static int wsa_started = 0;

bool fujitcp_active(void)
{
    return fd != INVALID_SOCKET;
}

int fujitcp_init(const char *hostport)
{
    char host[256], *colon;
    int port = 9995;
    BOOL one = TRUE;
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];

    if (!wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "fujinet: WSAStartup failed\n");
            return -1;
        }
        wsa_started = 1;
    }

    if (hostport == NULL)
        hostport = getenv("FUJINET_TCP");
    if (hostport == NULL)
        hostport = "127.0.0.1:9995";
    snprintf(host, sizeof host, "%s", hostport);
    colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    if (host[0] == '\0')
        snprintf(host, sizeof host, "127.0.0.1");
    snprintf(portstr, sizeof portstr, "%d", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        fprintf(stderr, "fujinet: cannot resolve %s:%d\n", host, port);
        return -1;
    }
    /* walk every result: "localhost" usually resolves to ::1 first, while
     * fujinet-pc's BoIP listener binds 127.0.0.1 only */
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        closesocket(fd);
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) {
        fprintf(stderr, "fujinet: cannot connect to %s:%d\n", host, port);
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    fprintf(stderr, "fujinet: connected to %s:%d\n", host, port);
    return 0;
}

void fujitcp_close(void)
{
    if (fd != INVALID_SOCKET) {
        closesocket(fd);
        fd = INVALID_SOCKET;
    }
}

/* Read one complete SLIP frame (two 0xC0 delimiters) or time out. */
static fb_status_t read_frame(uint8_t *buf, size_t cap, size_t *out_len, int secs)
{
    size_t n = 0;
    int ends = 0;
    DWORD deadline = GetTickCount() + (DWORD)secs * 1000;

    for (;;) {
        fd_set rf;
        struct timeval tv;
        DWORD now = GetTickCount();
        long remain_ms = (long)(deadline - now);
        if (remain_ms <= 0)
            return FB_ETIMEOUT;
        tv.tv_sec = remain_ms / 1000;
        tv.tv_usec = (remain_ms % 1000) * 1000;

        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        if (select(0, &rf, NULL, NULL, &tv) <= 0)
            return FB_ETIMEOUT;

        while (n < cap) {
            u_long avail = 0;
            char c;
            int r;
            /* non-blocking single-byte read via a peek on availability */
            if (ioctlsocket(fd, FIONREAD, &avail) != 0 || avail == 0)
                break;
            r = recv(fd, &c, 1, 0);
            if (r <= 0)
                break;
            buf[n++] = (uint8_t)c;
            if ((uint8_t)c == 0xC0 && ++ends == 2) {
                *out_len = n;
                return FB_OK;
            }
        }
        if (n >= cap)
            return FB_ETOOBIG;
    }
}

fb_status_t fujitcp_transact(uint8_t device, uint8_t command,
                             const fb_param_t *params, unsigned nparams,
                             const uint8_t *payload, uint16_t payload_len,
                             uint32_t timeout_ms, fb_reply_t *reply)
{
    static uint8_t req[FN_TX_MAX + 64];
    static uint8_t raw[RX_RAW_MAX];
    size_t reqlen, rawlen;
    fb_status_t st;
    int secs = (int)((timeout_ms + 999) / 1000);

    if (!fujitcp_active())
        return FB_ENOLINK;

    reqlen = fujibus_build_request(device, command, params, nparams,
                                   payload, payload_len, req, sizeof req);
    if (reqlen == 0)
        return FB_ETOOBIG;
    if (send(fd, (const char *)req, (int)reqlen, 0) != (int)reqlen)
        return FB_ENOLINK;

    for (;;) {
        st = read_frame(raw, sizeof raw, &rawlen, secs);
        if (st != FB_OK)
            return st;
        if (!fujibus_parse_reply(raw, rawlen, reply))
            return FB_EBADFRAME;
        if (!fujimail_inbound(reply))
            return FB_OK;
    }
}

void fujitcp_send_bare(uint8_t device, uint8_t command,
                       const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[64];
    size_t n = fujibus_build_request(device, command, NULL, 0,
                                     payload, payload_len, frame, sizeof frame);
    if (n && fd != INVALID_SOCKET)
        send(fd, (const char *)frame, (int)n, 0);
}
