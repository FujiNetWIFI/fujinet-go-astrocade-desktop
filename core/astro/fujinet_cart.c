/* fujinet_cart.c -- the FujiNet cartridge device (see fujinet_cart.h).
 *
 * A C transposition of fujinet-firmware's own MAME cart model
 * (pico/astrocade/emu/fujinet.cpp, BSD-3-Clause, copyright-holders Thomas
 * Cherryhomes), kept structurally line-for-line where C allows so the two
 * stay diffable: MAME's std::vector image/stream buffers become
 * malloc/realloc pairs, machine().side_effects_disabled() becomes the
 * `commit` parameter, and the no-image case serves the baked CONFIG client
 * instead of MAME's -cart argument.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujinet_cart.h"

/* The cartridge firmware's own protocol sources, staged verbatim by
 * cmake/StageFujiProto.cmake (see that file and COMPLIANCE.md). */
#include "fuji_mailbox.h"
#include "fujimail.h"
#include "astromap.h"
#include "fujitcp.h"
#include "fujiconfigrom.h"

/* fujimail's port interface is C function pointers with no context
 * argument, so the single cart instance is reached through this. One cart
 * slot, one cart: the constraint is real hardware's too. */
static astro_cart_t *s_cart = NULL;

static const char *kind_name(astromap_kind_t kind)
{
    switch (kind)
    {
    case ASTROMAP_FLAT:    return "flat";
    case ASTROMAP_GAME256: return "256K game mapper";
    case ASTROMAP_GAME512: return "512K game mapper";
    case ASTROMAP_APPBANK: return "banked app";
    default:               return "?";
    }
}

/*-------------------------------------------------
    stream buffers (MAME's m_rx vectors)
-------------------------------------------------*/

static void rx_clear(astro_cart_t *c, int stream)
{
    c->rx_len[stream & 1] = 0;
}

static void rx_reserve(astro_cart_t *c, int stream, uint32_t size)
{
    const int i = stream & 1;
    if (size > c->rx_cap[i])
    {
        uint8_t *p = realloc(c->rx[i], size);
        if (p)
        {
            c->rx[i] = p;
            c->rx_cap[i] = size;
        }
    }
}

static void rx_append(astro_cart_t *c, int stream, const uint8_t *chunk, unsigned len)
{
    const int i = stream & 1;
    if (c->rx_len[i] + len > c->rx_cap[i])
    {
        uint32_t want = c->rx_cap[i] ? c->rx_cap[i] : 4096;
        while (want < c->rx_len[i] + len)
            want *= 2;
        uint8_t *p = realloc(c->rx[i], want);
        if (!p)
            return;
        c->rx[i] = p;
        c->rx_cap[i] = want;
    }
    memcpy(c->rx[i] + c->rx_len[i], chunk, len);
    c->rx_len[i] += len;
}

/*-------------------------------------------------
    port plumbing (MAME device methods)
-------------------------------------------------*/

static void cart_poke(astro_cart_t *c, unsigned offset, uint8_t value)
{
    c->window[offset & 0x1fff] = value;
    /* A staged image that claims the mailbox must receive the same
     * publishes, or the client would boot into stale status pages. One that
     * does not claim it keeps its bytes pristine -- and the mailbox stays
     * live on the *current* window until the swap actually happens, so the
     * client still sees BOOT_READY (deactivating at stage time is a latent
     * bug in the o2 firmware port). Every mailbox offset is >= 0x1B00, the
     * high half, which an APPBANK image always serves from the window. */
    if (c->have_staged && c->staged_claims)
        c->staged[offset & 0x1fff] = value;
}

/* Point the serve state at the live image, the emulator's copy of core1's
 * swap: c->image must already hold the full image for the banked kinds, and
 * c->window the (first) 8K. */
static void apply_serving(astro_cart_t *c, const astromap_plan_t *plan)
{
    astromap_serve_t s;

    astromap_serve_reset(plan, &s);
    switch (plan->kind)
    {
    case ASTROMAP_APPBANK:
        c->bank[0] = c->image + s.bank_off[0];
        /* The high half serves from the RAM window so mailbox repaints stay
         * visible; the low half banks out of the image store. */
        c->bank[1] = c->window + 0x1000;
        c->hot_base = ASTROMAP_HOT_OFF;
        c->hot_mask = 0;
        c->hot_image = NULL;
        c->app_store = c->image;
        c->app_npages = plan->npages;
        break;
    case ASTROMAP_GAME256:
    case ASTROMAP_GAME512:
        c->bank[0] = c->image + s.bank_off[0];
        c->bank[1] = c->image + s.bank_off[1];
        c->hot_base = s.hot_base;
        c->hot_mask = s.hot_mask;
        c->hot_image = c->image;
        c->app_store = NULL;
        c->app_npages = 0;
        break;
    default:
        c->bank[0] = c->window;
        c->bank[1] = c->window + 0x1000;
        c->hot_base = ASTROMAP_HOT_OFF;
        c->hot_mask = 0;
        c->hot_image = NULL;
        c->app_store = NULL;
        c->app_npages = 0;
        break;
    }
}

static uint8_t cart_stream_open(astro_cart_t *c, int stream, uint32_t size)
{
    uint8_t err = (stream == 0) ? astromap_gate(size) : 0;

    if (err != 0)
        return err;
    rx_clear(c, stream);
    if (size)
        rx_reserve(c, stream, size);
    return 0;
}

static uint8_t cart_stream_close(astro_cart_t *c, int stream, uint32_t got, bool aborted)
{
    const int i = stream & 1;
    astromap_plan_t plan;

    (void)got;          /* rx_len is the byte count that actually landed */

    if (c->bootdump && c->rx_len[i])
    {
        char path[512];
        snprintf(path, sizeof path, "%s%s", c->bootdump, stream ? ".cfg" : ".rom");
        FILE *f = fopen(path, "wb");
        if (f)
        {
            fwrite(c->rx[i], 1, c->rx_len[i], f);
            fclose(f);
            fprintf(stderr, "fujinet: bootdump %s (%u bytes)\n", path,
                    (unsigned)c->rx_len[i]);
        }
        else
            fprintf(stderr, "fujinet: cannot write %s\n", path);
    }

    if (aborted || stream != 0)
    {
        rx_clear(c, stream);    /* the .cfg sibling means nothing to this cartridge */
        return 0;
    }

    if (c->rx_len[0] == 0 ||
        astromap_plan(c->rx[0], c->rx_len[0], &plan) != ASTROMAP_OK)
    {
        rx_clear(c, 0);
        return FN_BOOT_ERR_NOMAP;
    }
    if (plan.kind == ASTROMAP_FLAT)
    {
        astromap_apply(c->rx[0], &plan, c->staged);
        free(c->staged_image);
        c->staged_image = NULL;
        c->staged_image_size = 0;
        rx_clear(c, 0);
    }
    else
    {
        /* move the rx buffer into the staged image (MAME's std::move) */
        free(c->staged_image);
        c->staged_image = c->rx[0];
        c->staged_image_size = c->rx_len[0];
        c->rx[0] = NULL;
        c->rx_cap[0] = 0;
        c->rx_len[0] = 0;
        memcpy(c->staged, c->staged_image, sizeof c->staged);
    }
    c->staged_plan = plan;
    c->staged_claims = plan.mailbox_ok;
    c->have_staged = true;
    return 0;
}

static void do_swap(astro_cart_t *c)
{
    memcpy(c->window, c->staged, sizeof c->window);
    free(c->image);
    c->image = c->staged_image;
    c->image_size = c->staged_image_size;
    c->staged_image = NULL;
    c->staged_image_size = 0;
    apply_serving(c, &c->staged_plan);
    c->swap_armed = false;
    c->have_staged = false;
    c->mailbox_live = c->staged_claims;
    if (c->mailbox_live)
        fujimail_paint();
    fprintf(stderr, "fujinet: swapped in the staged image (%s); mailbox %s for this session\n",
            kind_name(c->staged_plan.kind), c->mailbox_live ? "kept" : "disabled");
}

/*-------------------------------------------------
    the debugger's FujiNet trace (not in MAME's model)
-------------------------------------------------*/

static astro_cart_trace_fn s_trace = NULL;

void astro_cart_set_trace(astro_cart_trace_fn fn)
{
    s_trace = fn;
}

/* fujitcp keeps its wire buffers to itself, so the trace gets each frame
 * rebuilt. A sent frame comes from the encoder and buffer size fujitcp
 * uses, so it is byte-identical. */
static void trace_sent(size_t cap, uint8_t device, uint8_t command,
                       const fb_param_t *params, unsigned nparams,
                       const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[FN_TX_MAX + 64];
    size_t n;

    if (!s_trace || !fujitcp_active())
        return;
    n = fujibus_build_request(device, command, params, nparams, payload,
                              payload_len, frame, cap);
    if (n)
        s_trace(1, frame, n);
}

/* A received frame is re-encoded from what was parsed out of it (replies
 * and pushes carry no parameters), a chunk at a time: a reply can outgrow
 * fujibus_build_request's buffer. */
static void trace_received(uint8_t device, uint8_t command,
                           const uint8_t *data, unsigned len)
{
    const unsigned total = 6 + len;
    uint8_t hdr[6], out[256];
    unsigned i, n = 0;
    uint16_t ck = 0;

    if (!s_trace)
        return;
    hdr[0] = device;
    hdr[1] = command;
    hdr[2] = (uint8_t)total;
    hdr[3] = (uint8_t)(total >> 8);
    hdr[4] = 0;
    hdr[5] = 0;
    /* fujibus.c's checksum: a byte sum with end-around carry */
    for (i = 0; i < total; i++)
    {
        ck += i < 6 ? hdr[i] : data[i - 6];
        ck = (uint16_t)((ck >> 8) + (ck & 0xff));
    }
    hdr[4] = (uint8_t)ck;

    out[n++] = 0xC0;
    for (i = 0; i < total; i++)
    {
        const uint8_t b = i < 6 ? hdr[i] : data[i - 6];
        if (n + 3 > sizeof out)
        {
            s_trace(0, out, n);
            n = 0;
        }
        if (b == 0xC0 || b == 0xDB)
        {
            out[n++] = 0xDB;
            out[n++] = b == 0xC0 ? 0xDC : 0xDD;
        }
        else
            out[n++] = b;
    }
    out[n++] = 0xC0;
    s_trace(0, out, n);
}

static const char *err_name(uint8_t err)
{
    switch (err)
    {
    case FN_ERR_OK:       return "ok";
    case FN_ERR_NOLINK:   return "no link";
    case FN_ERR_TIMEOUT:  return "timeout";
    case FN_ERR_BADFRAME: return "bad frame";
    case FN_ERR_TOOBIG:   return "too big";
    default:              return "?";
    }
}

static const char *boot_err_name(uint8_t err)
{
    switch (err)
    {
    case FN_BOOT_ERR_TOOBIG:    return "too big";
    case FN_BOOT_ERR_TRUNCATED: return "truncated";
    case FN_BOOT_ERR_NOMAP:     return "no mapper fits";
    case FN_BOOT_ERR_STOREBUSY: return "store busy";
    default:                    return "?";
    }
}

int astro_cart_describe(const astro_cart_t *c, char *dst, int dstsz)
{
    const uint8_t *w = c->window;
    int n = 0;
#define P(...) do { if (n < dstsz) n += snprintf(dst + n, dstsz - n, __VA_ARGS__); } while (0)

    P("BoIP link     %s\n", fujitcp_active() ? "connected" : "not connected");
    if (c->app_store)
        P("Image         %s, %u pages\n", kind_name(ASTROMAP_APPBANK), c->app_npages);
    else if (c->hot_image)
        P("Image         %s\n",
          kind_name(c->hot_mask == 0x3F ? ASTROMAP_GAME256 : ASTROMAP_GAME512));
    else
        P("Image         %s 8K\n", kind_name(ASTROMAP_FLAT));
    if (c->have_staged)
        P("Staged        %s, %u bytes%s\n", kind_name(c->staged_plan.kind),
          c->staged_image ? (unsigned)c->staged_image_size : ASTRO_CART_WINDOW,
          c->swap_armed ? ", swap armed" : "");

    if (!c->mailbox_live)
    {
        P("Mailbox       dead (the image does not claim it)\n");
    }
    else
    {
        const uint8_t st = w[FN_R_STATUS], reply = w[FN_R_REPLY_CMD];
        const uint8_t boot = w[FN_R_BOOT_STATE];

        P("Mailbox       live, protocol %u\n", w[FN_R_PROTO_VER]);
        P("ACKSEQ        %02X\n", w[FN_R_ACKSEQ]);
        P("Status        %s, %s\n", (st & FN_R_STATUS_LINK) ? "link up" : "link down",
          (st & FN_R_STATUS_BUSY) ? "busy" : "idle");
        P("Last result   %u %s\n", w[FN_R_ERR], err_name(w[FN_R_ERR]));
        P("Reply         %02X%s, %u bytes\n", reply,
          reply == FUJICMD_ACK ? " ACK" : reply == FUJICMD_NAK ? " NAK" : "",
          (unsigned)(w[FN_R_RXLEN_LO] | (w[FN_R_RXLEN_HI] << 8)));
        if (boot == FN_BOOT_XFER)
            P("Boot          receiving, %u%%\n", w[FN_R_BOOT_PCT]);
        else if (boot == FN_BOOT_READY)
            P("Boot          image staged\n");
        else if (boot == FN_BOOT_FAILED)
            P("Boot          failed, %s\n", boot_err_name(w[FN_R_BOOT_ERR]));
        else
            P("Boot          idle\n");
    }
#undef P
    if (dstsz > 0 && n >= dstsz)
        n = dstsz - 1;
    return n;
}

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

static void c_poke(unsigned offset, uint8_t value)
{
    cart_poke(s_cart, offset, value);
}

static bool c_link_up(void)
{
    return fujitcp_active();
}

static fb_status_t c_transact(uint8_t device, uint8_t command,
                              const fb_param_t *params, unsigned nparams,
                              const uint8_t *payload, uint16_t payload_len,
                              uint32_t timeout_ms, fb_reply_t *reply)
{
    fb_status_t st;

    trace_sent(FN_TX_MAX + 64, device, command, params, nparams, payload,
               payload_len);
    st = fujitcp_transact(device, command, params, nparams, payload,
                          payload_len, timeout_ms, reply);
    if (st == FB_OK)
        trace_received(reply->device, reply->command, reply->data,
                       reply->data_len);
    return st;
}

static void c_send_bare(uint8_t device, uint8_t command,
                        const uint8_t *payload, uint16_t payload_len)
{
    trace_sent(64, device, command, NULL, 0, payload, payload_len);
    fujitcp_send_bare(device, command, payload, payload_len);
}

/* DBC push frames are consumed inside fujitcp_transact; the trace rebuilds
 * them from what fujimail hands the stream callbacks. */
static uint8_t c_stream_open(int stream, uint32_t size)
{
    const uint8_t open[5] = {
        (uint8_t)stream, (uint8_t)size, (uint8_t)(size >> 8),
        (uint8_t)(size >> 16), (uint8_t)(size >> 24),
    };

    trace_received(FUJI_DEVICEID_DBC, NETCMD_OPEN, open, sizeof open);
    return cart_stream_open(s_cart, stream, size);
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    trace_received(FUJI_DEVICEID_DBC, NETCMD_WRITE, chunk, len);
    rx_append(s_cart, stream, chunk, len);
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
    static const uint8_t abort_flag = 0x01;

    trace_received(FUJI_DEVICEID_DBC, NETCMD_CLOSE, &abort_flag, aborted ? 1 : 0);
    return cart_stream_close(s_cart, stream, got, aborted);
}

static void c_arm_swap(void)
{
    if (s_cart->have_staged)
        s_cart->swap_armed = true;
}

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, n = 0;

    for (k = 0; k < t->rxlen && n < sizeof txt - 1; k++)
    {
        uint8_t ch = t->rx[k];
        if (ch == 0)
            break;
        txt[n++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[n] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            n ? " \"" : "", txt, n ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect,
                     unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream, expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n",
                stream, got, aborted ? " ABORTED" : "");
}

static const fujimail_port_t astro_port = {
    c_poke,
    c_link_up,
    c_transact,
    c_send_bare,
    c_stream_open,
    c_stream_write,
    c_stream_close,
    c_arm_swap,
    NULL,           /* wait_link_ms: the socket round trip is synchronous */
    NULL,           /* bootsel: nothing to reboot into under emulation */
    c_on_txn,
    c_on_dbc,
};

static const fujimail_port_t astro_port_quiet = {
    c_poke,
    c_link_up,
    c_transact,
    c_send_bare,
    c_stream_open,
    c_stream_write,
    c_stream_close,
    c_arm_swap,
    NULL,
    NULL,
    NULL,
    NULL,
};

/*-------------------------------------------------
    lifecycle
-------------------------------------------------*/

int astro_cart_start(astro_cart_t *c, const uint8_t *rom, uint32_t size,
                     const char *boip_hostport)
{
    astromap_plan_t plan;

    /* Once only per lifetime: later console RESETs must leave the running
     * mailbox alone, exactly as the real console's RESET leaves the RP2040
     * alone (MAME device_reset's m_init_done guard). */
    if (c->started)
        return 0;
    c->started = true;
    s_cart = c;

    c->debug = getenv("FUJINET_DEBUG") != NULL;
    c->bootdump = getenv("FUJINET_BOOTDUMP");
    memset(c->window, 0xff, sizeof c->window);
    memset(c->staged, 0xff, sizeof c->staged);
    c->bank[0] = c->window;
    c->bank[1] = c->window + 0x1000;
    c->hot_base = ASTROMAP_HOT_OFF;
    c->hot_mask = 0;

    /* No image: the baked-in real CONFIG client, the same image the RP2040
     * serves at power-up (firmware fujiboot.c). */
    if (!rom)
    {
        rom = _configrom;
        size = FUJI_CONFIGROM_SIZE;
    }

    if (astromap_plan(rom, size, &plan) == ASTROMAP_OK)
    {
        if (plan.kind == ASTROMAP_FLAT)
            astromap_apply(rom, &plan, c->window);
        else
        {
            c->image = malloc(size);
            if (!c->image)
                return -1;
            memcpy(c->image, rom, size);
            c->image_size = size;
            memcpy(c->window, rom, sizeof c->window);
        }
        apply_serving(c, &plan);
        c->mailbox_live = plan.mailbox_ok;
        if (plan.kind != ASTROMAP_FLAT)
            fprintf(stderr, "fujinet: %u-byte image loaded as %s\n",
                    (unsigned)size, kind_name(plan.kind));
    }
    else
        c->mailbox_live = false;

    if (c->mailbox_live)
    {
        fujimail_init(c->debug ? &astro_port : &astro_port_quiet);
        fujitcp_init(boip_hostport);
        fujimail_paint();
    }
    else
        fprintf(stderr, "fujinet: image carries no claim signature; running with the mailbox dead\n");

    return 0;
}

void astro_cart_stop(astro_cart_t *c)
{
    if (s_cart == c)
    {
        fujitcp_close();
        s_cart = NULL;
    }
    free(c->image);
    free(c->staged_image);
    free(c->rx[0]);
    free(c->rx[1]);
    memset(c, 0, sizeof *c);
}

/*-------------------------------------------------
    the bus read
-------------------------------------------------*/

uint8_t astro_cart_read(astro_cart_t *c, uint16_t offset, bool commit)
{
    offset &= 0x1fff;

    if (offset >= c->hot_base)
    {
        /* Game bank hotspot: the read RETURNS the new bank number, exactly
         * as rom_256k/rom_512k. The debugger sees the value but must not
         * switch (stricter than MAME's own mappers, which commit). */
        uint8_t data = offset & c->hot_mask;
        if (commit)
            c->bank[1] = c->hot_image + ((uint32_t)data << 12);
        return data;
    }

    uint8_t data = c->bank[offset >> 12][offset & 0xfff];

    /* Hotspot side effects: never for the debugger, never once the mailbox
     * is dead. The swap and the bank selects are handled here rather than
     * in fujimail because they must happen inline in whatever serves the
     * bus (the RP2040 does both in core1's loop for the same reason). */
    if (commit && offset >= FN_H_REGSEL)
    {
        if (offset == FN_H_REGSEL + FN_HOT_SWAP)
        {
            if (c->swap_armed)
                do_swap(c);
        }
        else if ((offset & FN_H_PAGE_MASK) == FN_H_REGSEL
                 && (offset & 0xff) >= FN_HOT_BANK)
        {
            unsigned page = (offset & 0xff) - FN_HOT_BANK;
            if (page < c->app_npages)
                c->bank[0] = c->app_store + ((uint32_t)page << 12);
        }
        else if (c->mailbox_live)
            fujimail_read_hotspot(offset);
    }
    return data;
}
