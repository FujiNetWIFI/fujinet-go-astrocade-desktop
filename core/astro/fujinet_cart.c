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

static uint8_t c_stream_open(int stream, uint32_t size)
{
    return cart_stream_open(s_cart, stream, size);
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    rx_append(s_cart, stream, chunk, len);
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
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
    fujitcp_transact,
    fujitcp_send_bare,
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
    fujitcp_transact,
    fujitcp_send_bare,
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
