/*
 * funcgen_test -- the magic function generator, against hand-computed
 * vectors from the Nutting manual (hardware pp. 94-100) and MAME's
 * astrocade_funcgen_w. Each mode is exercised in isolation on known screen
 * RAM so a regression names the exact transform that broke.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "astro_internal.h"

static astro_machine_t m;
static int failures;

static void check(const char *what, int got, int want)
{
    if (got != want)
    {
        fprintf(stderr, "FAIL %s: got %02X want %02X\n", what, got, want);
        failures++;
    }
}

/* set the magic control register the way an OUT 0x0C would */
static void magic(uint8_t control)
{
    astro_video_register_w(&m, 0x0c, 0, control);
}

int main(void)
{
    uint8_t bios[ASTRO_BIOS_SIZE];
    memset(bios, 0, sizeof bios);
    astro_machine_init(&m, bios, ASTRO_EXP_NONE);

    /* ---- plain write (no transform): control 0 shifts by 0 ---- */
    magic(0x00);
    astro_funcgen_w(&m, 0x000, 0xB4);
    check("plain", m.vram[0x000], 0xB4);

    /* ---- shift by 1 group (2 px): control bit0=1 -> shift 2, prev=0 ---- */
    magic(0x01);
    astro_funcgen_w(&m, 0x010, 0xC3);        /* prev=0 -> 0xC3>>2 = 0x30 */
    check("shift1", m.vram[0x010], 0x30);

    /* ---- OR: 0x0F over existing 0xF0 = 0xFF, no intercept (disjoint) ---- */
    m.vram[0x020] = 0xF0;
    magic(0x10);
    m.funcgen_intercept = 0;
    astro_funcgen_w(&m, 0x020, 0x0F);
    check("or_result", m.vram[0x020], 0xFF);
    check("or_no_intercept", m.funcgen_intercept, 0x00);

    /* ---- OR with a collision in every pixel pair -> all intercept bits ---- */
    m.vram[0x021] = 0xFF;
    magic(0x10);
    m.funcgen_intercept = 0;
    astro_funcgen_w(&m, 0x021, 0xFF);
    check("or_intercept", m.funcgen_intercept, 0xFF);

    /* ---- XOR: 0xAA ^ 0x0F = 0xA5 ---- */
    m.vram[0x030] = 0xAA;
    magic(0x20);
    astro_funcgen_w(&m, 0x030, 0x0F);
    check("xor", m.vram[0x030], 0xA5);

    /* ---- flop: bit-pair reverse of 0x1B (00 01 10 11) -> 11 10 01 00 = 0xE4 ---- */
    magic(0x40);
    astro_funcgen_w(&m, 0x040, 0x1B);
    check("flop", m.vram[0x040], 0xE4);

    /* ---- expand: control bit3, expand colors 0->1, 1->2 (XPAND=0x09) ----
     * MAME toggles expand_count to 1 first, so the FIRST write consumes the
     * HIGH nibble: 0x5A >> 4 = 0x5 = 0b0101. Per-bit (bit3..0 = 0,1,0,1) the
     * expand colors are color[0],color[1],color[0],color[1] = 1,2,1,2,
     * packed 2 bits each into pixels 3..0: 01 10 01 10 = 0x66. */
    astro_expand_register_w(&m, 0x09);       /* color[0]=1, color[1]=2 */
    magic(0x08);
    astro_funcgen_w(&m, 0x050, 0x5A);
    check("expand_hi", m.vram[0x050], 0x66);

    if (failures)
        fprintf(stderr, "funcgen_test: %d failure(s)\n", failures);
    else
        fprintf(stderr, "funcgen_test: PASS\n");
    return failures ? 1 : 0;
}
