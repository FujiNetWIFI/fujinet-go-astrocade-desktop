/* sound.c -- the sound section of the Astrocade custom I/O chip.
 *
 * Transposed from MAME src/devices/sound/astrocde.cpp (BSD-3-Clause,
 * copyright-holders Aaron Giles, Frank Palazzolo; see COMPLIANCE.md).
 * MAME runs the DSP loop at the chip clock (1.789772 MHz, the CPU clock);
 * this transposition clocks it once per CPU tick -- sample-exact against
 * the original -- and box-averages each 48 kHz output window rather than
 * picking samples, because the noise channel aliases audibly otherwise.
 *
 * Register map (from the MAME file header / Nutting manual p.106):
 *   0: master oscillator frequency        4: D7-D6 vibrato speed, D5-D0 depth
 *   1: tone A frequency                   5: D5 noise AM, D4 mux, D3-D0 vol C
 *   2: tone B frequency                   6: D7-D4 vol B, D3-D0 vol A
 *   3: tone C frequency                   7: noise volume
 */

#include <string.h>

#include "astro_internal.h"

void astro_sound_init(astro_sound_t *s)
{
    memset(s, 0, sizeof *s);
    /* generate a bitswap table for the noise */
    for (int i = 0; i < 256; i++)
    {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1 << b))
                v |= (uint8_t)(1 << (7 - b));
        s->bitswap[i] = v;
    }
}

void astro_sound_reset(astro_sound_t *s)
{
    memset(s->reg, 0, sizeof s->reg);
    s->master_count = 0;
    s->vibrato_clock = 0;
    s->noise_clock = 0;
    s->noise_state = 0;
    s->a_count = 0; s->a_state = 0;
    s->b_count = 0; s->b_state = 0;
    s->c_count = 0; s->c_state = 0;
}

void astro_sound_write(astro_sound_t *s, uint8_t port, uint8_t hi, uint8_t data)
{
    /* MAME decode: port 0x18 (bit 3 set) is the sound block transfer -- the
     * register index rides the upper address byte (OTIR, B = index). */
    unsigned reg = (port & 0x08) ? (hi & 7) : (port & 7);
    s->reg[reg] = data;
}

void astro_sound_tick(astro_sound_t *s)
{
    /* one chip clock of the MAME sound_stream_update loop */
    int32_t cursample = 0;

    if (s->a_state)
        cursample += s->reg[6] & 0x0f;
    if (s->b_state)
        cursample += s->reg[6] >> 4;
    if (s->c_state)
        cursample += s->reg[5] & 0x0f;

    /* add in the noise if enabled, based on the top bit of the LFSR */
    if ((s->reg[5] & 0x20) && (s->noise_state & 0x4000))
        cursample += s->reg[7] >> 4;

    /* clock the noise; a 6-bit counter clocks the LFSR (and vibrato) */
    if (++s->noise_clock >= 64)
    {
        /* 15-bit LFSR, feedback from the XOR of the top two bits */
        s->noise_state = (uint16_t)((s->noise_state << 1) |
                                    (~((s->noise_state >> 14) ^ (s->noise_state >> 13)) & 1));
        s->noise_clock = 0;
        s->vibrato_clock++;
    }

    /* clock the master oscillator; an 8-bit up counter */
    if (++s->master_count == 0)
    {
        /* reload based on mux value -- the register value is negative logic */
        s->master_count = (uint8_t)~s->reg[0];

        if ((s->reg[5] & 0x10) == 0)
        {
            /* vibrato: speed (reg 4 bits 6-7) selects one of the top 4 bits
             * of the 13-bit vibrato clock (0 = highest freq) */
            if (!((s->vibrato_clock >> (s->reg[4] >> 6)) & 0x0200))
                s->master_count += s->reg[4] & 0x3f;
        }
        else
        {
            /* noise: top 8 LFSR bits, bit-reversed, ANDed with the noise
             * volume register */
            s->master_count += s->bitswap[(s->noise_state >> 7) & 0xff] & s->reg[7];
        }

        if (++s->a_count == 0)
        {
            s->a_state ^= 1;
            s->a_count = (uint8_t)~s->reg[1];
        }
        if (++s->b_count == 0)
        {
            s->b_state ^= 1;
            s->b_count = (uint8_t)~s->reg[2];
        }
        if (++s->c_count == 0)
        {
            s->c_state ^= 1;
            s->c_count = (uint8_t)~s->reg[3];
        }
    }

    /* ---- decimation to 48 kHz ---- */
    s->dec_sum += (uint32_t)cursample;
    s->dec_count++;
    s->dec_acc += ASTRO_AUDIO_RATE * ASTRO_DOTS_PER_CPU_TICK;
    if (s->dec_acc >= 7159090u)     /* integer dot clock; 0.07 ppm off the exact .5 */
    {
        s->dec_acc -= 7159090u;

        /* box average, then a one-pole DC blocker: the chip's output is
         * unipolar (0..60) and the raw step at reset would thump. */
        float x = (float)s->dec_sum / (float)(s->dec_count ? s->dec_count : 1);
        float y = (x - s->dc_x1) + 0.9985f * s->dc_y1;
        s->dc_x1 = x;
        s->dc_y1 = y;
        s->dec_sum = 0;
        s->dec_count = 0;

        float v = y * (32767.0f / 60.0f);
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        if (s->out_count < (int)(sizeof s->out / sizeof s->out[0]))
            s->out[s->out_count++] = (int16_t)v;
    }
}
