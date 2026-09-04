/* video.c -- the Astrocade data chip: registers, magic function generator,
 * palette and per-scanline rendering.
 *
 * Transposed from MAME src/mame/midway/astrocde_v.cpp (BSD-3-Clause,
 * copyright-holders Nicola Salmoria, Mike Coates, Frank Palazzolo, Aaron
 * Giles; see COMPLIANCE.md). Register semantics per the Nutting manual
 * ("Software and Hardware for the Bally Arcade", 1978), hardware section
 * pp. 88-107; MAME's handler structure is kept so the two stay diffable.
 * Home console only: no pattern board, no ProfPac, no sparkle/stars.
 */

#include <math.h>
#include <string.h>

#include "astro_internal.h"
#include "machine.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The Astrocade has a 256 color palette: 32 colors with 8 luminance values
 * for each color. The 32 colors circle around the YUV color space, with the
 * exception of the first 8 which are grayscale. Like MAME we build 512
 * entries with an extra luminance bit (the arcade sparkle circuit's 4-bit
 * luma); the console's pens are all even but the debugger's palette strip
 * shows the full table. */
void astro_video_palette_init(uint32_t palette[512])
{
    for (int color = 0; color < 32; color++)
    {
        const double angle = (color / 32.0) * (2.0 * M_PI);
        const float ry = color ? (float)(0.75 * sin(angle)) : 0.0f;
        const float by = color ? (float)(1.15 * cos(angle)) : 0.0f;

        for (int luma = 0; luma < 16; luma++)
        {
            const float y = luma / 15.0f;

            int r = (int)((ry + y) * 255);
            int g = (int)(((y - 0.299f * (ry + y) - 0.114f * (by + y)) / 0.587f) * 255);
            int b = (int)((by + y) * 255);

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            palette[color * 16 + luma] =
                0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

uint8_t astro_video_register_r(astro_machine_t *m, uint8_t port)
{
    uint8_t result = 0xff;

    switch (port & 0x0f)
    {
    case 0x08:  /* intercept feedback */
        result = m->funcgen_intercept;
        m->funcgen_intercept = 0;
        break;

    case 0x0e:  /* vertical feedback (from lightpen interrupt) */
        result = m->vertical_feedback;
        break;

    case 0x0f:  /* horizontal feedback (from lightpen interrupt) */
        result = m->horizontal_feedback;
        break;
    }

    return result;
}

void astro_video_register_w(astro_machine_t *m, uint8_t port, uint8_t hi, uint8_t data)
{
    switch (port & 0x0f)
    {
    case 0x00:  /* color table is in registers 0-7 */
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
        m->colors[port & 7] = data;
        break;

    case 0x08:  /* mode register */
        m->video_mode = data & 1;
        break;

    case 0x09:  /* color split pixel */
        m->colorsplit = 2 * (data & 0x3f);
        m->bgdata = ((data & 0xc0) >> 6) * 0x55;
        break;

    case 0x0a:  /* vertical blank register */
        m->vblank = data;
        break;

    case 0x0b:  /* color block transfer: register index rides the upper
                 * address byte (the OTIR idiom -- B is the index) */
        m->colors[hi & 7] = data;
        break;

    case 0x0c:  /* magic (function generator control) register */
        m->funcgen_control = data;
        m->funcgen_expand_count = 0;     /* reset flip-flop for expand mode */
        m->funcgen_rotate_count = 0;     /* reset counter for rotate mode */
        m->funcgen_shift_prev_data = 0;  /* reset shift buffer */
        break;

    case 0x0d:  /* interrupt feedback (vector) */
        m->interrupt_vector = data;
        astro_machine_irq_clear(m);
        break;

    case 0x0e:  /* interrupt enable and mode */
        m->interrupt_enabl = data;
        astro_machine_irq_clear(m);
        break;

    case 0x0f:  /* interrupt line */
        m->interrupt_scanline = data;
        astro_machine_irq_clear(m);
        break;
    }
}

/* A write to 0x0000-0x0FFF: the magic function generator transforms the
 * byte and deposits it at 0x4000+offset -- screen RAM, which on the home
 * console is exactly the 4K the offset can reach. */
void astro_funcgen_w(astro_machine_t *m, uint16_t offset, uint8_t data)
{
    uint8_t prev_data;

    offset &= 0x0fff;

    /* control register:
        bit 0 = shift amount LSB
        bit 1 = shift amount MSB
        bit 2 = rotate
        bit 3 = expand
        bit 4 = OR
        bit 5 = XOR
        bit 6 = flop
    */

    /* expansion */
    if (m->funcgen_control & 0x08)
    {
        m->funcgen_expand_count ^= 1;
        data >>= 4 * m->funcgen_expand_count;
        data =  (m->funcgen_expand_color[(data >> 3) & 1] << 6) |
                (m->funcgen_expand_color[(data >> 2) & 1] << 4) |
                (m->funcgen_expand_color[(data >> 1) & 1] << 2) |
                (m->funcgen_expand_color[(data >> 0) & 1] << 0);
    }
    prev_data = m->funcgen_shift_prev_data;
    m->funcgen_shift_prev_data = data;

    /* rotate or shift */
    if (m->funcgen_control & 0x04)
    {
        /* rotate: first 4 writes accumulate data */
        if ((m->funcgen_rotate_count & 4) == 0)
        {
            m->funcgen_rotate_data[m->funcgen_rotate_count++ & 3] = data;
            return;
        }
        /* second 4 writes actually write it */
        else
        {
            uint8_t shift = 2 * (~m->funcgen_rotate_count++ & 3);
            data =  (((m->funcgen_rotate_data[3] >> shift) & 3) << 6) |
                    (((m->funcgen_rotate_data[2] >> shift) & 3) << 4) |
                    (((m->funcgen_rotate_data[1] >> shift) & 3) << 2) |
                    (((m->funcgen_rotate_data[0] >> shift) & 3) << 0);
        }
    }
    else
    {
        /* shift */
        uint8_t shift = 2 * (m->funcgen_control & 0x03);
        data = (uint8_t)((data >> shift) | (prev_data << (8 - shift)));
    }

    /* flopping */
    if (m->funcgen_control & 0x40)
        data = (uint8_t)((data >> 6) | ((data >> 2) & 0x0c) | ((data << 2) & 0x30) | (data << 6));

    /* OR/XOR */
    if (m->funcgen_control & 0x30)
    {
        uint8_t olddata = m->vram[offset];

        /* compute any intercepts: low nibble is this write, high nibble
         * latches until IN 0x08 reads it back */
        m->funcgen_intercept &= 0x0f;
        if ((olddata & 0xc0) && (data & 0xc0))
            m->funcgen_intercept |= 0x11;
        if ((olddata & 0x30) && (data & 0x30))
            m->funcgen_intercept |= 0x22;
        if ((olddata & 0x0c) && (data & 0x0c))
            m->funcgen_intercept |= 0x44;
        if ((olddata & 0x03) && (data & 0x03))
            m->funcgen_intercept |= 0x88;

        /* apply the operation */
        if (m->funcgen_control & 0x10)
            data |= olddata;
        else if (m->funcgen_control & 0x20)
            data ^= olddata;
    }

    m->vram[offset] = data;
}

void astro_expand_register_w(astro_machine_t *m, uint8_t data)
{
    m->funcgen_expand_color[0] = data & 0x03;
    m->funcgen_expand_color[1] = (data >> 2) & 0x03;
}

/* Render screen line y (0..239) from the current register state, exactly
 * MAME's screen_update_astrocde inner loop for one line. MAME renders in
 * per-scanline partial updates driven by its scanline timer; the machine
 * loop calls this right after running the line's dots, which is the same
 * granularity. */
void astro_video_render_line(astro_machine_t *m, int y)
{
    const int xystep = 2 - m->video_mode;
    uint32_t *dest = &m->fb[(size_t)y * ASTRO_FB_WIDTH];
    int destx = 0;

    /* screen line -> astrocade line (MAME mame_vpos_to_astrocade_vpos) */
    int effy = y - ASTRO_VERT_OFFSET;
    if (effy < 0)
        effy += ASTRO_LINES_PER_FRAME;

    uint16_t offset = (uint16_t)((effy / xystep) * (80 / xystep));

    /* iterate over groups of 4 pixels */
    for (int x = 0; x < 456 / 4 && destx < ASTRO_FB_WIDTH; x += xystep)
    {
        const int effx = x - ASTRO_HORZ_OFFSET / 4;
        const uint8_t *colorbase = &m->colors[(effx < m->colorsplit) ? 4 : 0];

        /* select either video data or background data; the mask keeps a
         * stray VERBL/high-res setting from reading past the console's 4K
         * (MAME's arcade sets have more screen RAM here) */
        uint8_t data = (effx >= 0 && effx < 80 && effy < m->vblank)
                       ? m->vram[offset++ & (ASTRO_VRAM_SIZE - 1)] : m->bgdata;

        for (int xx = 0; xx < 4; xx++)
        {
            const uint8_t pixdata = (data >> 6) & 3;
            const int colordata = colorbase[pixdata] << 1;
            const int luma = colordata & 0x0f;
            const uint32_t color = m->palette[(colordata & 0x1f0) | luma];

            if (destx < ASTRO_FB_WIDTH)
                dest[destx++] = color;
            if (xystep == 2 && destx < ASTRO_FB_WIDTH)
                dest[destx++] = color;
            data <<= 2;
        }
    }
}
