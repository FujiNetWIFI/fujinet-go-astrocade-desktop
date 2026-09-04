/*
 * keypad_matrix_test -- every one of the 24 keypad keys reads back on the
 * right IN port and bit, per the Nutting manual p.102 matrix (columns
 * 0x14 rightmost/gold .. 0x17 leftmost; bits 0 top row .. 5 bottom).
 * Runs a tiny ROM that copies the four column ports into screen RAM, so
 * this exercises the real io_read decode, not the struct fields directly.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "astro_internal.h"

static astro_machine_t m;

/* read all four keypad columns into vram[0..3] and spin */
static void load_reader(uint8_t *bios)
{
    static const uint8_t prog[] = {
        0xF3,                       /* DI */
        0xDB, 0x14, 0x32, 0x00, 0x40,   /* IN A,(0x14); LD (0x4000),A */
        0xDB, 0x15, 0x32, 0x01, 0x40,   /* IN A,(0x15); LD (0x4001),A */
        0xDB, 0x16, 0x32, 0x02, 0x40,   /* IN A,(0x16); LD (0x4002),A */
        0xDB, 0x17, 0x32, 0x03, 0x40,   /* IN A,(0x17); LD (0x4003),A */
        0x18, 0xEA,                     /* JR back to the first IN */
    };
    memset(bios, 0, ASTRO_BIOS_SIZE);
    memcpy(bios, prog, sizeof prog);
}

static void settle(void)
{
    for (int l = 0; l < 262; l++)
        astro_machine_run_scanline(&m);
}

int main(void)
{
    uint8_t bios[ASTRO_BIOS_SIZE];
    int failures = 0;

    load_reader(bios);
    astro_machine_init(&m, bios, ASTRO_EXP_NONE);
    settle();
    for (int p = 0; p < 4; p++)
        if (m.vram[p] != 0)
        {
            fprintf(stderr, "FAIL idle: port 0x1%d = %02X (want 00)\n",
                    4 + p, m.vram[p]);
            failures++;
        }

    /* col 0..3 map to ports 0x17..0x14; test each column/row bit alone */
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 6; row++)
        {
            /* host col->port: leftmost physical col (0) is port 0x17 */
            const int port_idx = 3 - col;          /* keypad[] index */
            const uint8_t bit = (uint8_t)(1u << row);
            memset(m.keypad, 0, sizeof m.keypad);
            m.keypad[port_idx] = bit;
            settle();
            /* vram[k] holds IN 0x1(4+k) = keypad[k] */
            for (int k = 0; k < 4; k++)
            {
                uint8_t want = (k == port_idx) ? bit : 0;
                if (m.vram[k] != want)
                {
                    fprintf(stderr, "FAIL col=%d row=%d: vram[%d]=%02X want %02X\n",
                            col, row, k, m.vram[k], want);
                    failures++;
                }
            }
        }

    /* the gold column is port 0x14 = keypad[0] (division/multiply/... side) */
    memset(m.keypad, 0, sizeof m.keypad);
    m.keypad[0] = 0x01;             /* '%' top of the gold column */
    settle();
    if (m.vram[0] != 0x01)
    {
        fprintf(stderr, "FAIL gold '%%': vram[0]=%02X want 01\n", m.vram[0]);
        failures++;
    }

    fprintf(stderr, "keypad_matrix_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
