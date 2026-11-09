/*
 * vidview.c -- the data-chip snapshot and its renderers (see astrovid.h).
 *
 * The pictures are the Video tab controller's job (vidtab.c). This file keeps
 * the snapshot and the console's full register text, which follows the
 * Nutting manual's Output/Input Ports tables.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "astro_internal.h"
#include "astrovid.h"
#include "debugger_internal.h"
#include "host.h"

void astrovid_snapshot_get(astrovid_snapshot *snap)
{
    astro_machine_t *m = astro_host_machine();
    memset(snap, 0, sizeof *snap);
    if (!m)
        return;
    astrovid_snapshot_from(m, snap);
}

void astrovid_snapshot_from(const astro_machine_t *m, astrovid_snapshot *snap)
{
    memset(snap, 0, sizeof *snap);

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
