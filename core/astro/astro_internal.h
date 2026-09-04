/* astro_internal.h -- the emulated machine, shared by the core modules.
 *
 * The machine logic is transposed from MAME's Bally Astrocade home-console
 * driver (BSD-3-Clause; see COMPLIANCE.md):
 *
 *   src/mame/midway/astrohome.cpp   memory/IO maps, keypad, machine config
 *   src/mame/midway/astrocde_v.cpp  data-chip video registers, the magic
 *                                   function generator, palette, rendering,
 *                                   interrupt generation
 *   src/devices/sound/astrocde.cpp  the custom I/O chip's sound section
 *
 * Field names deliberately keep MAME's own (minus the m_ prefix) so the two
 * implementations can be diffed side by side; the arcade-only pieces
 * (pattern board, ProfPac 16-color board, sparkle/star circuit) are not
 * carried over -- the home console has none of them.
 *
 * The CPU is floooh's cycle-stepped chips z80.h (zlib, vendored in z80/),
 * not MAME's: one z80_tick() is one CPU clock = 4 dots of the 7.159 MHz
 * master clock, and the whole machine is paced in DOT units -- a scanline
 * is 455 dots, which is NOT a whole number of CPU ticks (113.75), so per-
 * line cycle rounding would drift by 65 lines every frame.
 */

#ifndef ASTRO_INTERNAL_H
#define ASTRO_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "z80/z80.h"

#include "fujinet_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Master timing (MAME: m_screen->set_raw(ASTROCADE_CLOCK, 455, 0, 352, 262, 0, 240)).
 * ASTROCADE_CLOCK is 14318181/2 = 7159090.5 Hz; the .5 matters only for
 * wall-clock pacing, where the host uses the exact double below. */
#define ASTRO_DOT_CLOCK_HZ     7159090.5
#define ASTRO_DOTS_PER_LINE    455
#define ASTRO_LINES_PER_FRAME  262
#define ASTRO_DOTS_PER_FRAME   (ASTRO_DOTS_PER_LINE * ASTRO_LINES_PER_FRAME)
#define ASTRO_DOTS_PER_CPU_TICK 4

#define ASTRO_FB_WIDTH         352
#define ASTRO_FB_HEIGHT        240
#define ASTRO_VERT_OFFSET      22   /* MAME VERT_OFFSET: screen line 22 is astrocade line 0 */
#define ASTRO_HORZ_OFFSET      16   /* MAME HORZ_OFFSET: 16 border pixels left of the data area */

#define ASTRO_BIOS_SIZE        0x2000
#define ASTRO_VRAM_SIZE        0x1000

/* RAM expansions (MAME bus/astrocde/ram.cpp; sizes from ram.h). Plain RAM
 * only: the Blue RAM's INS8154 I/O ports and cassette are not modeled. */
typedef enum {
    ASTRO_EXP_NONE = 0,
    ASTRO_EXP_BLUE_RAM_4K,     /* 4K at 0x6000 */
    ASTRO_EXP_BLUE_RAM_16K,    /* 16K at 0x6000 */
    ASTRO_EXP_BLUE_RAM_32K,    /* 32K at 0x6000 */
    ASTRO_EXP_VIPER_SYS1,      /* 16K at 0x6000-0x9FFF */
    ASTRO_EXP_LIL_WHITE_RAM,   /* 32K at 0x5000-0xCFFF, first 12K mirrored to 0xD000-0xFFFF */
    ASTRO_EXP_RL64_RAM,        /* 44K at 0x5000-0xFFFF */
    ASTRO_EXP_COUNT
} astro_exp_t;

/* ---- sound section of the custom I/O chip (sound/astrocde.cpp) ----
 *
 * The DSP loop runs at the chip clock (= the CPU clock, 1.789772 MHz); MAME
 * allocates its stream at that rate and this transposition clocks it once
 * per CPU tick, then box-averages down to 48 kHz. */
typedef struct {
    uint8_t reg[8];
    uint8_t master_count;       /* 8-bit up counter */
    uint16_t vibrato_clock;     /* 13-bit */
    uint8_t noise_clock;        /* 6-bit */
    uint16_t noise_state;       /* 15-bit LFSR */
    uint8_t a_count, a_state;
    uint8_t b_count, b_state;
    uint8_t c_count, c_state;
    uint8_t bitswap[256];

    /* decimation to ASTRO_AUDIO_RATE, box average over each output window */
    uint32_t dec_acc;           /* += rate*4 per tick; sample out at >= dot clock */
    uint32_t dec_sum;           /* sum of cursample over the window */
    uint32_t dec_count;
    float dc_x1, dc_y1;         /* one-pole DC blocker state */

    /* one video frame of output at 48 kHz is < 810 samples */
    int16_t out[1024];
    int out_count;
} astro_sound_t;

#define ASTRO_AUDIO_RATE 48000

void astro_sound_init(astro_sound_t *s);
void astro_sound_reset(astro_sound_t *s);
/* Register write; `port` is the low IO address byte (0x10-0x18), `hi` the
 * upper byte (the register index for the 0x18 block transfer). */
void astro_sound_write(astro_sound_t *s, uint8_t port, uint8_t hi, uint8_t data);
/* Advance one chip clock (call once per CPU tick). Appends any completed
 * 48 kHz samples to s->out. */
void astro_sound_tick(astro_sound_t *s);

/* ---- the machine ---- */

typedef struct astro_machine astro_machine_t;

struct astro_machine {
    z80_t cpu;
    uint64_t pins;

    /* memory */
    uint8_t bios[ASTRO_BIOS_SIZE];
    uint8_t vram[ASTRO_VRAM_SIZE];
    uint8_t *exp_ram;           /* NULL when no expansion mounted */
    astro_exp_t exp_kind;

    astro_cart_t cart;
    astro_sound_t snd;

    /* ---- data-chip video registers (astrocde_v.cpp names, sans m_) ---- */
    uint8_t colors[8];          /* OUT 0x00-0x07: 0-3 right of boundary, 4-7 left */
    uint8_t colorsplit;         /* OUT 0x09 low 6 bits * 2 (in 4-pixel groups) */
    uint8_t bgdata;             /* OUT 0x09 bits 7-6, replicated to all 4 pixels */
    uint8_t vblank;             /* OUT 0x0A: first non-display astrocade line */
    uint8_t video_mode;         /* OUT 0x08 bit 0: 0 consumer/low-res, 1 commercial */
    uint8_t interrupt_enabl;    /* OUT 0x0E */
    uint8_t interrupt_vector;   /* OUT 0x0D (INFBK) */
    uint8_t interrupt_scanline; /* OUT 0x0F (INLIN) */
    uint8_t vertical_feedback;  /* IN 0x0E, latched at lightpen trigger */
    uint8_t horizontal_feedback;/* IN 0x0F */

    /* magic function generator */
    uint8_t funcgen_expand_color[2]; /* OUT 0x19 (XPAND) */
    uint8_t funcgen_control;    /* OUT 0x0C (MAGIC) */
    uint8_t funcgen_expand_count;
    uint8_t funcgen_rotate_count;
    uint8_t funcgen_rotate_data[4];
    uint8_t funcgen_shift_prev_data;
    uint8_t funcgen_intercept;  /* IN 0x08, cleared on read */

    /* ---- interrupt line model ----
     * MAME asserts INT with a vector and clears it on acknowledge
     * (HOLD_LINE), at a timer deadline (vblank end / the feedback line), on
     * a write to 0x0D/0x0E/0x0F, or -- mode 1 -- after one instruction. */
    bool irq_asserted;
    uint8_t irq_vector;         /* byte driven onto the bus at INTAK */
    int64_t irq_off_dot;        /* absolute dot deadline, or <0 for none */
    bool irq_one_instr;         /* clear at the next instruction boundary */

    /* ---- timing ---- */
    int64_t dot;                /* absolute dot counter since power-on */
    int scanline;               /* current screen line, 0..261 */
    int line_dot;               /* dots consumed within the current line */

    /* ---- inputs (active high, MAME astrohome.cpp / bus/astrocde/joy.cpp) ---- */
    uint8_t keypad[4];          /* column bytes for IN 0x14-0x17, bits 0-5 */
    uint8_t handle[4];          /* IN 0x10-0x13: b0 up b1 down b2 left b3 right b4 trigger */
    uint8_t knob[4];            /* IN 0x1C-0x1F, already inverted (MAME PORT_INVERT) */

    /* ---- output ---- */
    uint32_t fb[ASTRO_FB_WIDTH * ASTRO_FB_HEIGHT];  /* XRGB8888 */
    uint32_t palette[512];

    /* ---- debugger ----
     * instr_hook fires at every instruction boundary (z80_opdone), on the
     * emulator thread, BEFORE the next instruction's first tick; it may
     * block (that is how the debugger pauses the machine). */
    void (*instr_hook)(astro_machine_t *m, void *user);
    void *instr_hook_user;
};

/* machine.c */
void astro_machine_init(astro_machine_t *m, const uint8_t bios[ASTRO_BIOS_SIZE],
                        astro_exp_t exp);
void astro_machine_free(astro_machine_t *m);
/* Console RESET: CPU + sound + pending interrupt; screen RAM, the video
 * registers and the whole cart device survive, as on real hardware. */
void astro_machine_reset(astro_machine_t *m);
/* Run one scanline: evaluate the scanline interrupt at line start, run 455
 * dots of CPU (with the carried remainder), render the line if visible. */
void astro_machine_run_scanline(astro_machine_t *m);
/* Debugger accessors: peek never disturbs the machine (cart hotspots do not
 * fire, intercept feedback is not cleared); poke is a real bus write, magic
 * function generator included. */
uint8_t astro_machine_peek(astro_machine_t *m, uint16_t addr);
void astro_machine_poke(astro_machine_t *m, uint16_t addr, uint8_t data);

/* video.c */
void astro_video_palette_init(uint32_t palette[512]);
uint8_t astro_video_register_r(astro_machine_t *m, uint8_t port);
void astro_video_register_w(astro_machine_t *m, uint8_t port, uint8_t hi, uint8_t data);
void astro_funcgen_w(astro_machine_t *m, uint16_t offset, uint8_t data);
void astro_expand_register_w(astro_machine_t *m, uint8_t data);
void astro_video_render_line(astro_machine_t *m, int y);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_INTERNAL_H */
