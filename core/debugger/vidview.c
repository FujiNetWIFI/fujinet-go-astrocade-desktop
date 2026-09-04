/*
 * vidview.c -- the data-chip snapshot and its renderers (see astrovid.h).
 *
 * The screen renderer reuses the exact same per-scanline logic the live
 * display uses (core/astro/video.c), so the debugger's picture can never
 * drift from what the machine actually shows. The register decode follows the
 * Nutting manual's Output/Input Ports tables.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "astro_internal.h"
#include "astrovid.h"
#include "host.h"

void astrovid_snapshot_get(astrovid_snapshot *snap)
{
    astro_machine_t *m = astro_host_machine();
    memset(snap, 0, sizeof *snap);
    if (!m)
        return;

    memcpy(snap->colors, m->colors, sizeof snap->colors);
    snap->video_mode = m->video_mode;
    snap->colorsplit = m->colorsplit;
    snap->bgdata = m->bgdata;
    snap->vblank = m->vblank;
    snap->interrupt_vector = m->interrupt_vector;
    snap->interrupt_enabl = m->interrupt_enabl;
    snap->interrupt_scanline = m->interrupt_scanline;
    snap->expand_color[0] = m->funcgen_expand_color[0];
    snap->expand_color[1] = m->funcgen_expand_color[1];

    snap->intercept = m->funcgen_intercept;
    snap->vertical_feedback = m->vertical_feedback;
    snap->horizontal_feedback = m->horizontal_feedback;

    snap->funcgen_control = m->funcgen_control;
    snap->expand_count = m->funcgen_expand_count;
    snap->rotate_count = m->funcgen_rotate_count;
    memcpy(snap->rotate_data, m->funcgen_rotate_data, 4);
    snap->shift_prev_data = m->funcgen_shift_prev_data;

    memcpy(snap->snd_reg, m->snd.reg, 8);
    snap->master_count = m->snd.master_count;
    snap->vibrato_clock = m->snd.vibrato_clock;
    snap->noise_state = m->snd.noise_state;
    snap->a_count = m->snd.a_count; snap->a_state = m->snd.a_state;
    snap->b_count = m->snd.b_count; snap->b_state = m->snd.b_state;
    snap->c_count = m->snd.c_count; snap->c_state = m->snd.c_state;

    snap->scanline = m->scanline;
    memcpy(snap->vram, m->vram, sizeof snap->vram);
    memcpy(snap->palette, m->palette, sizeof snap->palette);
}

/* One scanline of the live render (matches astro_video_render_line), but
 * against the snapshot's frozen state and into an RGBA row. */
static void render_line(const astrovid_snapshot *snap, int y, uint32_t *dest)
{
    const int xystep = 2 - snap->video_mode;
    int destx = 0;
    int effy = y - ASTRO_VERT_OFFSET;
    if (effy < 0)
        effy += ASTRO_LINES_PER_FRAME;
    uint16_t offset = (uint16_t)((effy / xystep) * (80 / xystep));

    for (int x = 0; x < 456 / 4 && destx < ASTROVID_SCREEN_W; x += xystep) {
        const int effx = x - ASTRO_HORZ_OFFSET / 4;
        const uint8_t *colorbase = &snap->colors[(effx < snap->colorsplit) ? 4 : 0];
        uint8_t data = (effx >= 0 && effx < 80 && effy < snap->vblank)
                       ? snap->vram[offset++ & 0xfff] : snap->bgdata;
        for (int xx = 0; xx < 4; xx++) {
            const uint8_t pixdata = (data >> 6) & 3;
            const int colordata = colorbase[pixdata] << 1;
            const int luma = colordata & 0x0f;
            const uint32_t color = snap->palette[(colordata & 0x1f0) | luma];
            if (destx < ASTROVID_SCREEN_W) dest[destx++] = color;
            if (xystep == 2 && destx < ASTROVID_SCREEN_W) dest[destx++] = color;
            data <<= 2;
        }
    }
    while (destx < ASTROVID_SCREEN_W)
        dest[destx++] = 0xff000000u;
}

void astrovid_render_screen(const astrovid_snapshot *snap, uint32_t *dst,
                            int *w, int *h)
{
    *w = ASTROVID_SCREEN_W;
    *h = ASTROVID_SCREEN_H;
    for (int y = 0; y < ASTROVID_SCREEN_H; y++)
        render_line(snap, y, &dst[(size_t)y * ASTROVID_SCREEN_W]);
}

void astrovid_render_bitmap(const astrovid_snapshot *snap, uint32_t *dst,
                            int *w, int *h)
{
    /* screen RAM as raw pixels: 2 bits per pixel, 4 px/byte, colored by the
     * left palette set so the layout is legible independent of the game's
     * colorsplit. 40 bytes/line in low-res (160 px), 80 in high-res? The
     * data chip packs one 80-byte line region; low-res doubles. Show it the
     * way the mode reads it: low-res 160x102, high-res 320x204. */
    const int bytes_per_line = 40;      /* 40 bytes * 4 px = 160 px */
    int lines = (snap->video_mode) ? 204 : 102;
    int width = 160;
    if (snap->video_mode) width = 320;

    for (int y = 0; y < lines && y < ASTROVID_BITMAP_H; y++) {
        for (int bx = 0; bx < (snap->video_mode ? 80 : bytes_per_line); bx++) {
            int idx = y * (snap->video_mode ? 80 : bytes_per_line) + bx;
            uint8_t data = snap->vram[idx & 0xfff];
            for (int p = 0; p < 4; p++) {
                uint8_t pd = (data >> 6) & 3;
                int colordata = snap->colors[pd] << 1;   /* left set */
                uint32_t color = snap->palette[(colordata & 0x1f0) | (colordata & 0x0f)];
                int px = bx * 4 + p;
                if (px < width && px < ASTROVID_BITMAP_W)
                    dst[(size_t)y * ASTROVID_BITMAP_W + px] = color;
                data <<= 2;
            }
        }
        for (int px = width; px < ASTROVID_BITMAP_W; px++)
            dst[(size_t)y * ASTROVID_BITMAP_W + px] = 0xff101010u;
    }
    *w = width;
    *h = lines;
}

int astrovid_render_palette(const astrovid_snapshot *snap, uint32_t *dst,
                            int cols, int cell)
{
    int rows = (512 + cols - 1) / cols;
    int W = cols * cell;
    for (int pen = 0; pen < 512; pen++) {
        int cx = (pen % cols) * cell;
        int cy = (pen / cols) * cell;
        uint32_t color = snap->palette[pen];
        for (int yy = 0; yy < cell; yy++)
            for (int xx = 0; xx < cell; xx++)
                dst[(size_t)(cy + yy) * W + (cx + xx)] = color;
    }
    return rows;
}

static const char *onoff(int b) { return b ? "on" : "off"; }

void astrovid_format_state(const astrovid_snapshot *snap, char *dst, int len)
{
    int n = 0;
    #define P(...) do { if (n < len) n += snprintf(dst + n, len - n, __VA_ARGS__); } while (0)

    P("DATA CHIP (video)\n");
    P("  Color regs (right 0-3 / left 4-7):\n   ");
    for (int i = 0; i < 8; i++) P(" %02X", snap->colors[i]);
    P("\n");
    P("  08 Mode        : %s (%s)\n", snap->video_mode ? "1" : "0",
      snap->video_mode ? "high-res 320x204" : "low-res 160x102");
    P("  09 Color bound : split=%u  bg pixel data=%02X\n",
      snap->colorsplit, snap->bgdata);
    P("  0A VERBL       : %u (last active line)\n", snap->vblank);
    P("  0D INFBK vector: %02X\n", snap->interrupt_vector);
    P("  0E INMOD       : %02X  scanline-int %s (mode %d), lightpen %s\n",
      snap->interrupt_enabl,
      onoff(snap->interrupt_enabl & 0x08), (snap->interrupt_enabl & 0x04) ? 1 : 0,
      onoff(snap->interrupt_enabl & 0x02));
    P("  0F INLIN       : %u (scanline compare)\n", snap->interrupt_scanline);
    P("  19 XPAND       : color0=%u color1=%u\n",
      snap->expand_color[0], snap->expand_color[1]);
    P("  IN 08 intercept: %02X   0E vfeedback=%u  0F hfeedback=%u\n",
      snap->intercept, snap->vertical_feedback, snap->horizontal_feedback);
    P("\nMAGIC function generator (0C=%02X):\n", snap->funcgen_control);
    P("  shift=%u rotate=%s expand=%s OR=%s XOR=%s flop=%s\n",
      snap->funcgen_control & 3, onoff(snap->funcgen_control & 0x04),
      onoff(snap->funcgen_control & 0x08), onoff(snap->funcgen_control & 0x10),
      onoff(snap->funcgen_control & 0x20), onoff(snap->funcgen_control & 0x40));
    P("  expand_count=%u rotate_count=%u shift_prev=%02X rotate_data=%02X %02X %02X %02X\n",
      snap->expand_count, snap->rotate_count, snap->shift_prev_data,
      snap->rotate_data[0], snap->rotate_data[1], snap->rotate_data[2],
      snap->rotate_data[3]);

    P("\nSOUND CHIP:\n");
    P("  10 master=%02X  11-13 tone A/B/C=%02X %02X %02X  14 vibrato=%02X\n",
      snap->snd_reg[0], snap->snd_reg[1], snap->snd_reg[2], snap->snd_reg[3],
      snap->snd_reg[4]);
    P("  15 volC/noise=%02X  16 volA/B=%02X  17 noiseVol=%02X\n",
      snap->snd_reg[5], snap->snd_reg[6], snap->snd_reg[7]);
    P("  state: master_count=%u vibrato=%u noise=%04X  tones A=%u B=%u C=%u\n",
      snap->master_count, snap->vibrato_clock, snap->noise_state,
      snap->a_state, snap->b_state, snap->c_state);
    P("\nScanline: %d\n", snap->scanline);
    #undef P
    if (len > 0)
        dst[(n < len) ? n : len - 1] = '\0';
}
