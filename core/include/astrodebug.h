/*
 * astrodebug -- the debugger contract: a Z80 debugger engine over the
 * Astrocade core (core/debugger/) behind a C API the native debugger windows
 * share. Same shape as the SMS, MSX, ADAM and Coleco siblings' debug headers,
 * so the windows carry over; the engine is this repository's own, built on
 * the core's instruction-boundary hook (astro_machine.instr_hook) and its bus
 * hook (astro_machine.bus_hook, for the read/write/IN/OUT watchpoints).
 *
 * The engine only engages while a window holds it attached:
 * astrodebug_attach() when the window opens -- which also stops the machine,
 * as on every sibling -- and astrodebug_detach() when it closes, which takes
 * every hook out of the core and lets the machine run on at full speed.
 * Breakpoints are kept for the next attach. The FujiNet trace is the
 * exception: it records from session start, so it shows what happened before
 * anyone opened a window.
 *
 * Stopping parks the emulation thread between instructions; "stopped" means
 * it is parked. Everything that changes the machine (registers, step, run)
 * needs it stopped; reads are always allowed and answer with whatever is
 * current while it runs. A window learns of stops, breakpoint hits and edits
 * by polling astrodebug_generation() (every 100 ms is plenty); the optional
 * stop callback fires on the emulator thread.
 *
 * Restarting the session (a new cartridge, RESET TO CONFIG, a preferences
 * change) while stopped does not hang: the parked thread is released first.
 * The engine, its breakpoints and its attach state survive the restart; an
 * attached engine stops the new machine at its first instruction.
 *
 * Numbers in the console and in conditions: $C000, 0xC000, C000h (a name
 * wins over a hex literal), 49152 (decimal), %1010.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRODEBUG_H
#define ASTRODEBUG_H

#include <stdint.h>

#include "astrosession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The engine, lazily created by astrosession_debugger(s) (astrosession.h) and
 * alive as long as the process. */
/* The process-wide engine itself, for code with no session object (tests). */
astrodebug *astrodebug_get(void);

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. The emulation thread
 * notices within a frame (<= ~17 ms); poll astrodebug_is_stopped() or the
 * generation. Idempotent. Before the session has started it stops the
 * machine as soon as it exists. Call it when the debugger window opens. */
void astrodebug_attach(astrodebug *d);
/* Disengage and let the machine run. Breakpoints are kept for the next
 * attach. Returns once nothing is installed in the core any more (bounded
 * wait). Call it when the debugger window closes. */
void astrodebug_detach(astrodebug *d);
int  astrodebug_is_attached(astrodebug *d);

/* 1 once the emulation thread is parked. 0 while running, stepping, before
 * the session starts, and right after resume/step/detach. */
int  astrodebug_is_stopped(astrodebug *d);
void astrodebug_stop(astrodebug *d);       /* break at the next instruction */
void astrodebug_resume(astrodebug *d);     /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint: exec $8033",
 * "write $6000 = $03", "read $6000 = $03", "out $FF <- $9F", "in $FC -> $1F",
 * "step", "step over", "step out", "run to", "scanline", "frame") and the PC
 * it stopped at; "running" and -1 while it runs. Returns the length. */
int  astrodebug_stop_reason(astrodebug *d, char *dst, int dstsz, int *address);
/* Bumped whenever the machine stopped or resumed, a command ran, a register,
 * memory or label was edited, or the breakpoint list changed, so a window
 * knows when to refresh. */
unsigned astrodebug_generation(astrodebug *d);

/* Optional push for tests: fires on the EMULATOR thread, with no engine lock
 * held, each time the machine parks (after the state is visible to the
 * calls above; it may call resume/step). Windows should poll the generation
 * instead. */
typedef enum {
    ASTRODBG_STOP_PAUSE = 0,   /* stop request, or attach */
    ASTRODBG_STOP_BREAKPOINT,  /* an execute breakpoint (before it runs) */
    ASTRODBG_STOP_WATCH,       /* a read/write/IN/OUT breakpoint (after the
                                 accessing instruction) */
    ASTRODBG_STOP_STEP,        /* step, over, out, scanline or frame done */
    ASTRODBG_STOP_RUNTO        /* run-to address reached */
} astrodebug_stop_kind;
void astrodebug_set_stop_callback(astrodebug *d,
                                 void (*cb)(void *ud, int kind, uint16_t pc),
                                 void *ud);

/* ---- the console ------------------------------------------------------------ */
/* Run one command line. "help" lists them: step [n], over, out, run, stop,
 * runto <addr>, scanline [n], frame [n], break <addr>[-<end>] [cond],
 * bpr/bpw <addr>[-<end>] [cond], bpin/bpout <port>[-<end>] [cond],
 * delete <id|addr>, enable/disable <id>, clear, breaks, print <expr>, regs,
 * set <reg> <expr>, mem <addr> [len], poke <addr> <val...>, vram <addr>
 * [len], vpoke <addr> <val...>, video, io (the memory map and ports), fujinet,
 * label <addr> <name>, labels [file], disasm [addr] [n], trace
 * [on|off|clear|n], save <kind> <file>. Expressions: numbers (see above),
 * registers (a f b c d e h l af bc de hl ix iy sp pc i r im iff1 iff2, primed
 * af' bc' de' hl'), flags (cf nf pf hf zf sf yf xf), [addr] byte and {addr}
 * word reads, labels, line frame cycles, and in a breakpoint condition
 * `value` and `addr` for the access; C operators with C precedence.
 * The stepping commands wait up to a quarter second for the machine to stop
 * again and then say where it is; run/go return at once.
 * Output text into dst; returns its length. */
int  astrodebug_command(astrodebug *d, const char *command, char *dst, int dstsz);
/* Completions for the end of a line (commands, registers, labels, save
 * kinds), one per line. Returns the count. */
int  astrodebug_completions(astrodebug *d, const char *prefix, char *dst,
                           int dstsz);
/* Evaluate an expression (the grammar above, no `value`/`addr`): for an
 * address field that should take labels and arithmetic. 0 and *value on
 * success, -1 with the reason in err. */
int  astrodebug_eval(astrodebug *d, const char *text, long *value, char *err,
                    int errsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
/* All need the machine stopped (they do nothing otherwise) and return at
 * once; the machine runs and stops again, bumping the generation. The first
 * instruction after a stop always runs, whatever sits at the PC. Breakpoints
 * and watchpoints stop any of them early, with their own reason. */
void astrodebug_step(astrodebug *d);        /* F7: one instruction */
void astrodebug_step_over(astrodebug *d);   /* F8: over a CALL/RST/DJNZ/block
                                             repeat/HALT; else one step */
void astrodebug_step_out(astrodebug *d);    /* Shift+F8: to the RET (the return
                                             address on the stack, with an SP
                                             guard for recursion) */
/* n scanlines (the beam line wraps 261 -> 0) / n frames: the end of the
 * frame in progress is the first, so "frame" always lands on a frame
 * boundary with the whole picture drawn. */
void astrodebug_scanline(astrodebug *d, int n);
void astrodebug_frame(astrodebug *d, int n);
/* Run until PC == addr. */
void astrodebug_run_to(astrodebug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, af, bc, de, hl, ix, iy, wz;
    int af2, bc2, de2, hl2;                /* the shadow set */
    int i, r, im, iff1, iff2, halted;
    int sf, zf, yf, hf, xf, pf, nf, cf;    /* the flags in F */
    uint64_t cycles;                       /* T-states since power-on */
    int line;                              /* beam scanline 0-261 of the frame in
                                              progress (it can lag one line right
                                              after the line's last instruction) */
    uint32_t frame;                        /* frames completed since power-on */
} astrodebug_cpu;
void astrodebug_cpu_get(astrodebug *d, astrodebug_cpu *out);
typedef enum {
    ASTRO_REG_PC = 0, ASTRO_REG_SP, ASTRO_REG_AF, ASTRO_REG_BC, ASTRO_REG_DE,
    ASTRO_REG_HL, ASTRO_REG_IX, ASTRO_REG_IY, ASTRO_REG_AF2, ASTRO_REG_BC2,
    ASTRO_REG_DE2, ASTRO_REG_HL2, ASTRO_REG_I, ASTRO_REG_R, ASTRO_REG_IM,
    ASTRO_REG_IFF1, ASTRO_REG_IFF2,
    ASTRO_REG_A, ASTRO_REG_F, ASTRO_REG_B, ASTRO_REG_C, ASTRO_REG_D, ASTRO_REG_E,
    ASTRO_REG_H, ASTRO_REG_L,
    ASTRO_FLAG_S, ASTRO_FLAG_Z, ASTRO_FLAG_H, ASTRO_FLAG_PV, ASTRO_FLAG_N,
    ASTRO_FLAG_C,
    ASTRO_REG_COUNT
} astrodebug_reg;
/* Needs the machine stopped. Setting PC redirects the next instruction. */
void astrodebug_cpu_set(astrodebug *d, int reg, int value);

/* ---- memory ------------------------------------------------------------------- */
/* CPU-bus reads through the live bank map, without side effects. Returns n
 * (0 with the session not running). */
int  astrodebug_read(astrodebug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit through the same map: RAM takes it, ROM ignores it, and a
 * cartridge bank register is not touched. */
void astrodebug_write(astrodebug *d, uint16_t addr, uint8_t value);

/* What is mapped where: $0000-$1FFF the BIOS ROM (writes to $0000-$0FFF go
 * to the magic function generator), $2000-$3FFF the cartridge slot (the
 * FujiNet cartridge), $4000-$4FFF screen RAM (the video chip's 4K, mirrored),
 * and RAM expansion from $5000 up (which exp). */
typedef struct {
    const char *bios_name;   /* "Bally BIOS" ... */
    const char *exp_name;    /* "none", "Blue RAM 16K", ... */
    int exp_bytes;           /* installed expansion RAM, 0 for none */
    int exp_base;            /* its first address, 0 for none */
    int video_mode;          /* OUT $08 bit 0: 0 consumer/low-res, 1 commercial */
    int cart_present;
} astrodebug_memmap;
void astrodebug_memmap_get(astrodebug *d, astrodebug_memmap *out);
/* The memory map and the data chip's ports and registers (OUT $00-$0F, the
 * magic function generator, the sound chip's) as monospace text lines
 * (the Memory Map & I/O tab; ~2K is plenty). Returns the length. */
int  astrodebug_io_state(astrodebug *d, char *dst, int dstsz);

/* ---- disassembly -------------------------------------------------------------- */
#define ASTRODASM_JUMP     0x01
#define ASTRODASM_CALL     0x02
#define ASTRODASM_RET      0x04
#define ASTRODASM_COND     0x08
#define ASTRODASM_RELATIVE 0x10
#define ASTRODASM_BLOCK    0x20 /* LDIR/CPIR/INIR/OTIR family */
#define ASTRODASM_HALT     0x40

typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;      /* an execute breakpoint here, enabled or not */
    int breakpoint_enabled;
    int length;
    char bytes[16];          /* "CD 50 80" */
    char label[48];          /* the label at this address, or "" */
    char disasm[64];         /* "CALL $8050" */
    char comment[64];        /* a jump/call target's label, an IN/OUT port's name */
    uint16_t target;         /* jump/call destination when flags say so */
    uint8_t flags;           /* ASTRODASM_* */
} astrodebug_line;
/* Up to `max` lines starting at `addr`, through the current memory map.
 * Returns the count; *pc_line is the index of the PC's line in `out`, -1
 * when not among them (may be NULL). */
int  astrodebug_disassemble(astrodebug *d, uint16_t addr, astrodebug_line *out,
                           int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down).
 * Backwards is a best guess, as on every Z80 debugger. Clicks in the view
 * should map through the lines' `address`, never through a row number. */
int  astrodebug_row_address(astrodebug *d, uint16_t addr, int rows);

/* ---- symbols and labels -------------------------------------------------------
 * .sym format: "HHHH NAME [; comment]" per line, '#' comments; also read:
 * WLA-DX ("BB:AAAA name"), z88dk .map ("name = $AAAA ; ...") and SDCC .noi
 * ("DEF name 0xAAAA"). The built-in BIOS table (the Astrocade BIOS entry
 * points and the data chip's I/O ports) loads when the engine is created. */
/* Load a symbol file as table "file" (replacing the last one). With path
 * NULL, <cartridge>.sym/.map/.noi next to the opened cartridge. Message into
 * msg; returns labels loaded or -1. */
int  astrodebug_load_symbols(astrodebug *d, const char *path, char *msg,
                            int msgsz);
/* Address of a label, or -1. Label at an address into dst (exact match;
 * empty if none). A label set here goes into table "user". */
int  astrodebug_label_address(astrodebug *d, const char *label);
int  astrodebug_address_label(astrodebug *d, uint16_t addr, char *dst,
                             int dstsz);
/* Label addr (NULL or "" removes the user label there). Returns 0. */
int  astrodebug_set_label(astrodebug *d, uint16_t addr, const char *label);
/* Lower-level: load a file into a named table; the nearest label at or
 * below addr with its offset; a name's address (1 found). */
int  astrodebug_symbols_load(astrodebug *d, const char *path, const char *table);
const char *astrodebug_symbol_at(astrodebug *d, uint16_t addr,
                                uint16_t *offset);
int  astrodebug_symbol_find(astrodebug *d, const char *name, uint16_t *addr);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    ASTRODEBUG_BP_EXEC = 1, ASTRODEBUG_BP_READ = 2, ASTRODEBUG_BP_WRITE = 4,
    ASTRODEBUG_BP_IN = 8, ASTRODEBUG_BP_OUT = 16
} astrodebug_bp_type;
typedef struct {
    int id;                  /* never reused */
    int type;                /* astrodebug_bp_type bits */
    uint16_t start, end;     /* an address range, or a port range (0-255) for
                                IN/OUT */
    int enabled;
    unsigned hits;           /* times it fired (its condition true) */
    char condition[128];     /* an expression, "" for none */
} astrodebug_breakpoint;
/* READ ignores opcode and operand fetches. WRITE, READ, IN and OUT stop
 * after the accessing instruction; EXEC stops before the instruction runs.
 * A condition that fails to evaluate at run time stops rather than hides.
 * Conditions see the registers as they are in the middle of the instruction
 * for the access kinds. */
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  astrodebug_breakpoint_toggle(astrodebug *d, uint16_t addr);
/* 1 when an execute breakpoint is at addr (enabled or not). */
int  astrodebug_breakpoint_check(astrodebug *d, uint16_t addr);
/* type: any combination of the bits (IN/OUT with ports only). Returns the
 * new id, or -1 with the reason in err (a condition that does not parse or
 * names something unknown, a port above 255, a full table). */
int  astrodebug_breakpoint_add(astrodebug *d, int type, uint16_t start,
                              uint16_t end, const char *condition, char *err,
                              int errsz);
void astrodebug_breakpoint_remove(astrodebug *d, int id);
void astrodebug_breakpoint_enable(astrodebug *d, int id, int enabled);
/* Copies up to max, in creation order. Returns the count. */
int  astrodebug_breakpoint_list(astrodebug *d, astrodebug_breakpoint *out,
                               int max);
void astrodebug_breakpoint_clear(astrodebug *d);

/* ---- the instruction trace ring ---------------------------------------------- */
/* Records only while attached with tracing on (it needs the exec hook). */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len, bytes[4];
    uint64_t cycles;
    int16_t line;
} astrodebug_trace;
void astrodebug_trace_enable(astrodebug *d, int on);
int  astrodebug_trace_enabled(astrodebug *d);
/* Newest first; returns entries copied. */
int  astrodebug_trace_read(astrodebug *d, astrodebug_trace *out, int max);
void astrodebug_trace_clear(astrodebug *d);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $0000-$FFFF as mapped now), "mem" (the 64K the CPU sees),
 * "vram" (the 4K of screen RAM), "regs" (CPU and video chip registers as text). Message
 * into msg; returns 0 or -1. */
int  astrodebug_save(astrodebug *d, const char *kind, const char *path,
                    char *msg, int msgsz);

/* ---- FujiNet: the cartridge's FujiBus packets, both ways -----------------
 * Always on from session start (safe from any thread). The decoded packets
 * are astrodebug_fuji_packet (core/debugger/fujitrace.h's record). */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> Astrocade, 0: Astrocade -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "Astrocade -> FujiNet 70 E8 GET ADAPTERCONFIG (6 bytes)" */
    uint32_t repeat;        /* consecutive identical exchanges folded in (x N) */
} astrodebug_fuji_packet;

/* Start the trace: the session calls this before the machine starts. */
void astrodebug_fujinet_install(astrodebug *d);
/* The newest max packets, oldest first. Returns how many. */
int  astrodebug_fujinet_trace(astrodebug *d, astrodebug_fuji_packet *out, int max);
void astrodebug_fujinet_trace_clear(astrodebug *d);
/* The cartridge's BoIP link and mailbox, as text lines. Returns its length. */
int  astrodebug_fujinet_state(astrodebug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* ASTRODEBUG_H */
