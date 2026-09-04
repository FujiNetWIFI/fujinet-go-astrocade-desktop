/* fujinet_cart.h -- the FujiNet cartridge, the one cart device this core has.
 *
 * A C transposition of the firmware's own MAME device model
 * (fujinet-firmware pico/astrocade/emu/fujinet.cpp, BSD-3-Clause, Thomas
 * Cherryhomes): an 8K window served from RAM with the read-hotspot mailbox
 * of fuji_mailbox.h decoded on offsets 0x1D00-0x1FFF and replies repainted
 * into the window. The protocol itself (fujimail.c), the wire codec
 * (fujibus.c) and the image mapper (astromap.c) are the cartridge
 * firmware's own sources, staged verbatim into fuji-generated/ by
 * cmake/StageFujiProto.cmake; this device is only the port: bytes go into
 * the served window, frames go over a TCP socket to the bundled
 * fujinet-pc's BoIP listener.
 *
 * An image that does not carry the "FUJI" claim signature runs with the
 * mailbox dead -- a plain 8K ROM, or a 256K/512K game on the established
 * homebrew mapper (exact MAME rom_256k/rom_512k hotspot semantics) -- which
 * is the "load anything" path, and also how a network-booted image behaves
 * after the swap. With no image at all, the baked-in real CONFIG client
 * (fujiconfigrom.h, generated from the fujinet-config build) is served, the
 * same image the RP2040 cartridge bakes in.
 *
 * Single instance per process: fujimail's port interface is C function
 * pointers with no context argument (the constraint is real hardware's
 * too -- one cart slot, one cart).
 */

#ifndef ASTRO_FUJINET_CART_H
#define ASTRO_FUJINET_CART_H

#include <stdbool.h>
#include <stdint.h>

#include "astromap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASTRO_CART_WINDOW 0x2000

typedef struct astro_cart {
    uint8_t window[ASTRO_CART_WINDOW];
    uint8_t staged[ASTRO_CART_WINDOW];

    /* full image for the banked kinds (FLAT lives wholly in the windows) */
    uint8_t *image;
    uint32_t image_size;
    uint8_t *staged_image;
    uint32_t staged_image_size;

    /* per-stream DBC push buffers (0 = ROM image, 1 = the .cfg sibling) */
    uint8_t *rx[2];
    uint32_t rx_len[2], rx_cap[2];
    astromap_plan_t staged_plan;

    /* what read serves: the emulator's copy of core1's serve state */
    const uint8_t *bank[2];
    uint16_t hot_base;
    uint8_t hot_mask;
    const uint8_t *hot_image;
    const uint8_t *app_store;
    unsigned app_npages;

    bool started;
    bool mailbox_live;
    bool have_staged;
    bool staged_claims;
    bool swap_armed;
    bool debug;
    const char *bootdump;
} astro_cart_t;

/* Build the serve state from `rom` (NULL: the baked CONFIG client) and, when
 * the image claims the mailbox, bring up fujimail + the TCP link to
 * `boip_hostport` ("host:port"; NULL falls back to $FUJINET_TCP then
 * 127.0.0.1:9995 -- the session always passes the real port). Idempotent
 * per lifetime: a console RESET must not touch the cart, exactly as real
 * hardware's RESET never reaches the RP2040. */
int astro_cart_start(astro_cart_t *c, const uint8_t *rom, uint32_t size,
                     const char *boip_hostport);

/* Tear down for a full session stop (RESET TO CONFIG boots a fresh cart):
 * closes the TCP link and frees the buffers, after which _start may be
 * called again. */
void astro_cart_stop(astro_cart_t *c);

/* One console read of the 8K window (offset 0x0000-0x1FFF <-> console
 * 0x2000-0x3FFF). `commit` false is the debugger's no-side-effects read:
 * the value comes back but hotspots do not fire and banks do not move. */
uint8_t astro_cart_read(astro_cart_t *c, uint16_t offset, bool commit);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_FUJINET_CART_H */
