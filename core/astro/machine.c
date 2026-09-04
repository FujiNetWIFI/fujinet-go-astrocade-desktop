/* machine.c -- the Bally Astrocade home console: bus dispatch, the dot-paced
 * master loop, and the custom chip's interrupt model.
 *
 * Memory and IO maps transposed from MAME src/mame/midway/astrohome.cpp,
 * interrupt generation from astrocde_v.cpp (both BSD-3-Clause; see
 * COMPLIANCE.md):
 *
 *   0000-0FFF  BIOS; writes are the magic function generator, landing at
 *              0x4000+offset (the home console maps magic writes over the
 *              first 4K ONLY -- the wider arcade window is not here)
 *   1000-1FFF  BIOS high half; writes silently dropped (Star Fortress
 *              writes here -- MAME maps it .rom() and so drops them too)
 *   2000-3FFF  the cartridge window, reads only (the edge has no /WR)
 *   4000-4FFF  4K screen RAM, the machine's only RAM
 *   5000-FFFF  only with a RAM expansion mounted
 *
 *   IO 00-0F   data chip registers (video/interrupts); the upper address
 *              byte is a second data path (color/sound block transfer)
 *   IO 10-17   custom I/O chip inputs: handles (10-13), keypad (14-17)
 *   IO 10-18   custom I/O chip sound registers on write
 *   IO 19      expand register
 *   IO 1C-1F   pots (knobs)
 *
 * Interrupts: the custom chip supplies the IM 2 vector byte during INTACK
 * (the BIOS runs IM 2 -- verified against astro.bin, ED 5E at 0x0190).
 * MAME models "hold until acknowledged" (INMOD bit 2/0 clear) as HOLD_LINE
 * with a backstop timer at vblank end, and "1 instruction" mode as a
 * one-CPU-cycle assert; here the line clears on acknowledge, at the dot
 * deadline, on a write to 0x0D/0x0E/0x0F, or at the next instruction
 * boundary for the one-instruction mode.
 */

#include <stdlib.h>
#include <string.h>

#define CHIPS_IMPL
#include "z80/z80.h"

#include "astro_internal.h"
#include "machine.h"

/* ---- expansion RAM (MAME bus/astrocde/ram.cpp; offsets relative to the
 * 0x5000 base the exp slot is installed at) ---- */

static const struct { uint32_t size; } exp_info[ASTRO_EXP_COUNT] = {
    [ASTRO_EXP_NONE]          = { 0 },
    [ASTRO_EXP_BLUE_RAM_4K]   = { 0x1000 },
    [ASTRO_EXP_BLUE_RAM_16K]  = { 0x4000 },
    [ASTRO_EXP_BLUE_RAM_32K]  = { 0x8000 },
    [ASTRO_EXP_VIPER_SYS1]    = { 0x4000 },
    [ASTRO_EXP_LIL_WHITE_RAM] = { 0x8000 },
    [ASTRO_EXP_RL64_RAM]      = { 0xb000 },
};

/* addr is 0x5000-0xFFFF; returns a pointer into exp_ram or NULL when the
 * expansion does not decode this address. */
static uint8_t *exp_slot(astro_machine_t *m, uint16_t addr)
{
    const uint32_t offset = (uint32_t)addr - 0x5000;

    if (!m->exp_ram)
        return NULL;
    switch (m->exp_kind)
    {
    case ASTRO_EXP_BLUE_RAM_4K:
    case ASTRO_EXP_BLUE_RAM_16K:
    case ASTRO_EXP_BLUE_RAM_32K:
        /* Blue RAM starts at 0x6000, up to the RAM size */
        if (offset >= 0x1000 && offset < 0x1000 + exp_info[m->exp_kind].size)
            return &m->exp_ram[offset - 0x1000];
        return NULL;
    case ASTRO_EXP_VIPER_SYS1:
        /* RAM in 0x6000-0x9FFF */
        if (offset >= 0x1000 && offset < 0xa000)
            return &m->exp_ram[offset - 0x1000];
        return NULL;
    case ASTRO_EXP_LIL_WHITE_RAM:
        /* RAM in 0x5000-0xCFFF + a mirror of the first 12K up to 0xFFFF */
        return &m->exp_ram[offset % 0x8000];
    case ASTRO_EXP_RL64_RAM:
        /* 44K installed, 0x5000-0xFFFF */
        return &m->exp_ram[offset];
    default:
        return NULL;
    }
}

/* ---- bus ---- */

static uint8_t mem_read(astro_machine_t *m, uint16_t addr, bool commit)
{
    if (addr < 0x2000)
        return m->bios[addr];
    if (addr < 0x4000)
        return astro_cart_read(&m->cart, addr & 0x1fff, commit);
    if (addr < 0x5000)
        return m->vram[addr & (ASTRO_VRAM_SIZE - 1)];
    {
        const uint8_t *p = exp_slot(m, addr);
        return p ? *p : 0xff;
    }
}

static void mem_write(astro_machine_t *m, uint16_t addr, uint8_t data)
{
    if (addr < 0x1000)
    {
        astro_funcgen_w(m, addr, data);
        return;
    }
    if (addr < 0x4000)
        return;                     /* ROM space; writes dropped */
    if (addr < 0x5000)
    {
        m->vram[addr & (ASTRO_VRAM_SIZE - 1)] = data;
        return;
    }
    {
        uint8_t *p = exp_slot(m, addr);
        if (p)
            *p = data;
    }
}

/* IN: `port` is the low address byte, matching the IO map's own decode. */
static uint8_t io_read(astro_machine_t *m, uint8_t port)
{
    if (port < 0x10)
        return astro_video_register_r(m, port);
    if (port < 0x20)
    {
        /* the custom I/O chip's read decode (sound/astrocde.cpp):
         * 0x10-0x17 pulse SO and return SI (handles then keypad columns),
         * 0x1C-0x1F the pots, the rest open bus */
        const uint8_t sel = port & 0x0f;
        if (sel < 0x08)
            return (sel & 4) ? m->keypad[sel & 3] : m->handle[sel & 3];
        if (sel >= 0x0c)
            return m->knob[sel & 3];
        return 0xff;
    }
    return 0xff;
}

static void io_write(astro_machine_t *m, uint8_t port, uint8_t hi, uint8_t data)
{
    if (port < 0x10)
    {
        astro_video_register_w(m, port, hi, data);
        return;
    }
    if (port <= 0x18)
    {
        astro_sound_write(&m->snd, port, hi, data);
        return;
    }
    if (port == 0x19)
        astro_expand_register_w(m, data);
}

/* ---- interrupt line ---- */

void astro_machine_irq_clear(astro_machine_t *m)
{
    m->irq_asserted = false;
    m->irq_one_instr = false;
    m->irq_off_dot = -1;
}

/* Assert INT with `vector` on the bus. `hold`: clear on acknowledge or at
 * `off_dot`; else clear at the next instruction boundary. */
static void irq_assert(astro_machine_t *m, uint8_t vector, bool hold, int64_t off_dot)
{
    m->irq_asserted = true;
    m->irq_vector = vector;
    m->irq_one_instr = !hold;
    m->irq_off_dot = hold ? off_dot : -1;
}

/* ---- machine lifecycle ---- */

void astro_machine_init(astro_machine_t *m, const uint8_t bios[ASTRO_BIOS_SIZE],
                        astro_exp_t exp)
{
    memset(m, 0, sizeof *m);
    memcpy(m->bios, bios, ASTRO_BIOS_SIZE);

    m->exp_kind = exp;
    if (exp != ASTRO_EXP_NONE && exp_info[exp].size)
        m->exp_ram = calloc(1, exp_info[exp].size);

    astro_video_palette_init(m->palette);
    astro_sound_init(&m->snd);
    astro_sound_reset(&m->snd);

    m->irq_off_dot = -1;
    m->pins = z80_init(&m->cpu);
}

void astro_machine_free(astro_machine_t *m)
{
    free(m->exp_ram);
    m->exp_ram = NULL;
    astro_cart_stop(&m->cart);
}

void astro_machine_reset(astro_machine_t *m)
{
    /* Console RESET: CPU and the sound section restart; screen RAM, the
     * video registers and the cartridge keep their state (the BIOS re-
     * initializes the registers itself, and the cart edge carries no reset
     * line at all). MAME's F3 resets the same set of devices. */
    m->pins = z80_reset(&m->cpu);
    astro_sound_reset(&m->snd);
    astro_machine_irq_clear(m);
}

/* ---- the master loop ---- */

static void machine_tick(astro_machine_t *m)
{
    uint64_t pins = z80_tick(&m->cpu, m->pins);

    if (pins & Z80_MREQ)
    {
        const uint16_t addr = Z80_GET_ADDR(pins);
        if (pins & Z80_RD)
        {
            Z80_SET_DATA(pins, mem_read(m, addr, true));
        }
        else if (pins & Z80_WR)
        {
            mem_write(m, addr, Z80_GET_DATA(pins));
        }
    }
    else if (pins & Z80_IORQ)
    {
        const uint16_t addr = Z80_GET_ADDR(pins);
        if (pins & Z80_M1)
        {
            /* interrupt acknowledge: the custom chip drives the vector byte
             * (IM 2 low table byte); the acknowledge also releases a held
             * line, MAME's HOLD_LINE semantics */
            Z80_SET_DATA(pins, m->irq_vector);
            m->irq_asserted = false;
            m->irq_one_instr = false;
            m->irq_off_dot = -1;
        }
        else if (pins & Z80_RD)
        {
            Z80_SET_DATA(pins, io_read(m, (uint8_t)(addr & 0xff)));
        }
        else if (pins & Z80_WR)
        {
            io_write(m, (uint8_t)(addr & 0xff), (uint8_t)(addr >> 8),
                     Z80_GET_DATA(pins));
        }
    }

    m->dot += ASTRO_DOTS_PER_CPU_TICK;
    m->line_dot += ASTRO_DOTS_PER_CPU_TICK;
    astro_sound_tick(&m->snd);

    /* deadline release for a held, unacknowledged line */
    if (m->irq_off_dot >= 0 && m->dot >= m->irq_off_dot)
    {
        m->irq_asserted = false;
        m->irq_off_dot = -1;
    }

    if (m->irq_asserted)
        pins |= Z80_INT;
    else
        pins &= ~Z80_INT;
    m->pins = pins;

    if (z80_opdone(&m->cpu))
    {
        /* the one-instruction interrupt mode: the CPU sampled the line
         * during the instruction that just finished; release it now */
        if (m->irq_one_instr)
        {
            m->irq_asserted = false;
            m->irq_one_instr = false;
            m->pins &= ~Z80_INT;
        }
        if (m->instr_hook)
            m->instr_hook(m, m->instr_hook_user);
    }
}

void astro_machine_run_scanline(astro_machine_t *m)
{
    /* Scanline interrupt, evaluated at line start exactly as MAME's
     * scanline_callback: fires when the ASTROCADE line (screen line minus
     * the 22-line vertical offset, mod 262) matches INLIN and INMOD bit 3
     * enables it. */
    int astro_line = m->scanline - ASTRO_VERT_OFFSET;
    if (astro_line < 0)
        astro_line += ASTRO_LINES_PER_FRAME;

    if (astro_line == m->interrupt_scanline && (m->interrupt_enabl & 0x08) != 0)
    {
        if ((m->interrupt_enabl & 0x04) == 0)
        {
            /* mode 0: assert until acknowledged, backstop at vblank end
             * (the start of the next frame's line 0) */
            const int64_t off = m->dot - m->line_dot
                + (int64_t)(ASTRO_LINES_PER_FRAME - m->scanline) * ASTRO_DOTS_PER_LINE;
            irq_assert(m, m->interrupt_vector, true, off);
        }
        else
        {
            /* mode 1: assert for one instruction */
            irq_assert(m, m->interrupt_vector, false, -1);
        }
    }

    /* run the line's 455 dots; a CPU tick is 4, so up to 3 dots of the last
     * instruction spill into the next line (455 = 113.75 ticks) */
    while (m->line_dot < ASTRO_DOTS_PER_LINE)
        machine_tick(m);
    m->line_dot -= ASTRO_DOTS_PER_LINE;

    if (m->scanline < ASTRO_FB_HEIGHT)
        astro_video_render_line(m, m->scanline);

    if (++m->scanline >= ASTRO_LINES_PER_FRAME)
        m->scanline = 0;
}

/* ---- debugger access ---- */

uint8_t astro_machine_peek(astro_machine_t *m, uint16_t addr)
{
    return mem_read(m, addr, false);
}

void astro_machine_poke(astro_machine_t *m, uint16_t addr, uint8_t data)
{
    mem_write(m, addr, data);
}
