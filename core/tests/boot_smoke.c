/*
 * boot_smoke -- boot the real BIOS and prove the machine is alive.
 *
 * Three claims, each of which has failed for a sibling port at least once:
 *   1. the BIOS reaches its menu: after 120 frames the framebuffer is not
 *      a single color, and the BIOS has armed the scanline interrupt (its
 *      whole timing model hangs off INLIN/INMOD -- a machine that never
 *      fires it sits in a busy loop with a black screen);
 *   2. the CPU is really taking IM 2 vectored interrupts (I register set,
 *      IM 2 selected by the BIOS init we verified in the ROM bytes);
 *   3. the host thread paces at the real frame rate, ~60.05 fps -- the
 *      INTV port once ran at >33000% because a null platform layer
 *      defaulted its throttle off (its TODO M5), so this is measured, not
 *      assumed.
 *
 * Usage: boot_smoke <bios.bin>; exits 77 (ctest SKIP) when the BIOS is not
 * available, per the family's ROM-less build convention.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "astro_internal.h"
#include "host.h"

static int fail(const char *msg)
{
    fprintf(stderr, "boot_smoke: FAIL: %s\n", msg);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: boot_smoke <bios.bin>\n");
        return 77;
    }

    uint8_t bios[ASTRO_BIOS_SIZE];
    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        fprintf(stderr, "boot_smoke: SKIP: no BIOS at %s\n", argv[1]);
        return 77;
    }
    if (fread(bios, 1, sizeof bios, f) != sizeof bios)
    {
        fclose(f);
        return fail("short BIOS read");
    }
    fclose(f);

    /* ---- claims 1 and 2: deterministic, single-threaded machine ---- */
    static astro_machine_t m;
    astro_machine_init(&m, bios, ASTRO_EXP_NONE);
    /* no cart image: the FujiNet cart serves the baked CONFIG client (the
     * TCP link will be down here; the mailbox tolerates that) */
    astro_cart_start(&m.cart, NULL, 0, "127.0.0.1:1");

    for (int frame = 0; frame < 120; frame++)
        for (int line = 0; line < ASTRO_LINES_PER_FRAME; line++)
            astro_machine_run_scanline(&m);

    uint32_t first = m.fb[0];
    int distinct = 0;
    for (size_t i = 0; i < ASTRO_FB_WIDTH * ASTRO_FB_HEIGHT; i++)
        if (m.fb[i] != first)
        {
            distinct = 1;
            break;
        }
    if (!distinct)
        return fail("framebuffer is a single color after 120 frames");
    if ((m.interrupt_enabl & 0x08) == 0)
        return fail("BIOS never armed the scanline interrupt (INMOD bit 3)");
    if (m.cpu.im != 2)
        return fail("CPU is not in IM 2 after BIOS init");
    if (m.vblank == 0)
        return fail("VERBL was never programmed");
    fprintf(stderr, "boot_smoke: menu up, IM %d, I=%02X, VERBL=%u, INLIN=%u\n",
            m.cpu.im, m.cpu.i, m.vblank, m.interrupt_scanline);
    astro_machine_free(&m);

    /* ---- claim 3: the host thread paces at the real frame rate ---- */
    astro_host_opts_t opts = {
        .bios = bios,
        .cart = NULL,
        .cart_size = 0,
        .exp = ASTRO_EXP_NONE,
        .boip_hostport = "127.0.0.1:1",
    };
    if (astro_host_start(&opts) != 0)
        return fail(astro_host_last_error());

    static uint32_t fb[ASTRO_FB_WIDTH * ASTRO_FB_HEIGHT];
    uint64_t serial = 0;
    struct timespec t0, t1;

    /* Let the thread warm up, then count frame serials over three seconds:
     * the serial is sampled at frame granularity, so a 1 s window carries a
     * built-in +-1 fps boundary error -- 3 s brings that under +-0.4. */
    usleep(200 * 1000);
    astro_host_frame_copy(fb, &serial);
    uint64_t s0 = serial;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    usleep(3000 * 1000);
    astro_host_frame_copy(fb, &serial);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double fps = (double)(serial - s0) / secs;
    fprintf(stderr, "boot_smoke: host pacing %.2f fps\n", fps);
    astro_host_stop();

    if (fps < 58.5 || fps > 61.5)
        return fail("host is not pacing at ~60.05 fps");

    fprintf(stderr, "boot_smoke: PASS\n");
    return 0;
}
