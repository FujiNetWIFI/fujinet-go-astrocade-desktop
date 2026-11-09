/*
 * debugger.c -- the astrodebug engine (core/include/astrodebug.h).
 *
 * Built on the core's two hooks (astro_machine.instr_hook, fired at every
 * instruction boundary, and astro_machine.bus_hook, fired for each memory
 * read/write and IN/OUT). Both run on the EMULATION thread. Stopping PARKS
 * that thread inside the instruction hook on a condition variable until a
 * window resumes it, so "stopped" means parked:
 *   - the instruction hook is installed while a window holds the engine
 *     attached. At each boundary it records the instruction's address, feeds
 *     the trace ring, and decides whether to park: a stop request, a pending
 *     watchpoint, an enabled execute breakpoint, or what the current action
 *     (step, over, out, run-to, scanline, frame) is waiting for;
 *   - the bus hook is installed only while an enabled READ, WRITE, IN or OUT
 *     breakpoint exists. A hit sets `pending`, and the instruction hook parks
 *     the machine at the NEXT boundary, so a watchpoint stops AFTER the
 *     accessing instruction, as on the ADAM engine this ports. READ ignores
 *     the instruction's own operand fetches (opcode fetches never reach the
 *     hook: the core reports data reads only).
 * Detached, both hooks are taken out of the core and nothing is paid.
 *
 * The boundary. The chips Z80 reports "opdone" in the overlapped M1 cycle of
 * the NEXT instruction: its address is already on the address bus (m->pins)
 * and cpu.pc has moved one past it. So the PC the debugger shows is the
 * address on the bus, and the opcode byte already read for it is in m->pins'
 * data lines, where an edit of that byte has to follow it (refresh_fetch).
 *
 * Threading. UI threads call the public API under d->mtx; the hooks take the
 * same mutex. d->m is the machine while the host runs it, set by
 * astrodebug_core_started() and cleared by astrodebug_core_stopping() --
 * which the host calls BEFORE joining the machine thread, so a parked thread
 * is released and no UI call touches a machine that is being freed. The
 * engine, its breakpoints and its attach state outlive any number of
 * machines; an attached engine stops each new machine at its first
 * instruction.
 *
 * The FujiNet trace stands apart: the cart's tap feeds it from session
 * start, attached or not, under its own lock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "astro_internal.h"
#include "debugger_internal.h"
#include "expr.h"
#include "fujitrace.h"
#include "host.h"
#include "z80dasm.h"

#define MAX_BP   128
#define TRACE_N  65536
#define PATH_MAX_ 1024

enum { ACT_RUN = 0, ACT_STEP, ACT_OVER, ACT_OUT, ACT_RUNTO, ACT_SCANLINE,
       ACT_FRAME };

typedef struct {
    int pc, sp, af, bc, de, hl, ix, iy, wz;
    int af2, bc2, de2, hl2;
    int i, r, im, iff1, iff2, halted;
    int f;
    uint64_t cycles;
    int line;
    uint64_t frame;
} zregs;

struct astrodebug {
    pthread_mutex_t mtx;
    pthread_cond_t cv;      /* parks the emulation thread */
    pthread_cond_t evcv;    /* the machine parked */

    astro_machine_t *m;     /* the live machine, or NULL */
    int releasing;          /* the host is stopping the machine */

    int attached;
    int stopped;            /* the emulation thread is parked */
    int stop_req;           /* park at the next boundary */
    int act;                /* what the machine does once released */
    long count;             /* instructions / lines / frames still to go */
    uint16_t target, sp_guard;
    int last_line;
    uint64_t last_frame;

    char reason[128];
    int reason_addr;
    unsigned generation;
    uint64_t stop_serial;       /* bumped by every park */

    void (*stop_cb)(void *ud, int kind, uint16_t pc);
    void *stop_ud;

    /* The breakpoint table, and what the hooks look up. */
    astrodebug_breakpoint bp[MAX_BP];
    int nbp, next_id, nwatch;
    uint8_t map_exec[65536];    /* nonzero: an enabled execute bp covers it */
    uint8_t map_mem[65536];     /* ASTRODEBUG_BP_READ / _WRITE bits */
    uint8_t map_io[256];        /* ASTRODEBUG_BP_IN / _OUT bits */

    /* Hook-private state (emulation thread, mtx held). */
    uint16_t ins_start;         /* the instruction now executing */
    int pending;                /* a watchpoint fired during the instruction */
    char pend_reason[128];

    /* The expression context (mtx held while evaluating). */
    zregs rs;
    const astro_machine_t *ec;
    long acc_value, acc_addr, acc_old;
    int in_access;

    pthread_mutex_t trace_mtx;
    int trace_on;
    astrodebug_trace *ring;     /* TRACE_N entries, allocated on first enable */
    int ring_head, ring_count;

    uint8_t shadow[65536];      /* last value seen written, for `old` */

    sym_tables syms;
    char cart_path[PATH_MAX_];

    pthread_mutex_t fuji_mtx;
    fujitrace fuji;
};

static struct astrodebug s_dbg = {
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .evcv = PTHREAD_COND_INITIALIZER,
    .trace_mtx = PTHREAD_MUTEX_INITIALIZER,
    .fuji_mtx = PTHREAD_MUTEX_INITIALIZER,
};

/* ---- the machine ------------------------------------------------------------ */

/* The address of the instruction at the boundary (see the header comment). */
static uint16_t cur_pc(const astro_machine_t *m)
{
    return Z80_GET_ADDR(m->pins);
}

static uint8_t peek_m(astro_machine_t *m, uint16_t addr)
{
    return astro_machine_peek(m, addr);
}

static uint8_t peek(astrodebug *d, uint16_t addr)
{
    return d->m ? peek_m(d->m, addr) : 0xFF;
}

static void fill_regs(const astro_machine_t *m, zregs *r, int boundary)
{
    const z80_t *c = &m->cpu;

    r->pc = boundary ? cur_pc(m) : c->pc;
    r->sp = c->sp; r->af = c->af; r->bc = c->bc; r->de = c->de; r->hl = c->hl;
    r->ix = c->ix; r->iy = c->iy; r->wz = c->wz;
    r->af2 = c->af2; r->bc2 = c->bc2; r->de2 = c->de2; r->hl2 = c->hl2;
    r->i = c->i; r->r = c->r; r->im = c->im;
    r->iff1 = c->iff1; r->iff2 = c->iff2;
    r->halted = (c->pins & Z80_HALT) != 0;
    r->f = c->f;
    r->cycles = (uint64_t)m->dot / ASTRO_DOTS_PER_CPU_TICK;
    r->line = m->scanline;
    r->frame = m->frame;
}

/* After an edit while parked: the opcode already fetched for the PC is in the
 * data lines of m->pins and the CPU latches it from there on the next tick,
 * so an edit of that byte (or a new PC) has to land there too. */
static void refresh_fetch(astro_machine_t *m)
{
    if ((m->pins & (Z80_M1 | Z80_RD)) == (Z80_M1 | Z80_RD)) {
        const uint8_t b = peek_m(m, Z80_GET_ADDR(m->pins));
        Z80_SET_DATA(m->pins, b);
    }
}

static void bump_locked(astrodebug *d)
{
    d->generation++;
}

void astrodebug_touch(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* A wait on a condition variable for at most ms (mtx held). */
static void cond_wait_ms(pthread_cond_t *cv, pthread_mutex_t *mtx, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    pthread_cond_timedwait(cv, mtx, &ts);
}

/* ---- expressions ------------------------------------------------------------ */

const char *const astrodebug_reg_names[] = {
    "a", "f", "b", "c", "d", "e", "h", "l", "af", "bc", "de", "hl", "ix", "iy",
    "sp", "pc", "i", "r", "im", "iff1", "iff2", "af'", "bc'", "de'", "hl'",
    "cf", "nf", "pf", "hf", "zf", "sf", "yf", "xf", "halted", "line", "frame",
    "cycles", "value", "old", "addr", NULL,
};

static uint8_t expr_read(void *user, uint16_t addr)
{
    astrodebug *d = user;
    return d->ec ? peek_m((astro_machine_t *)d->ec, addr) : 0xFF;
}

static int expr_name(void *user, const char *name, long *out)
{
    astrodebug *d = user;
    const zregs *r = &d->rs;
    char n[16];
    size_t i;
    uint16_t addr;

    for (i = 0; name[i] && i + 1 < sizeof n; i++)
        n[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    n[i] = '\0';
    if (name[i]) n[0] = '\0'; /* too long to be a register */

    if (!strcmp(n, "value") || !strcmp(n, "addr") || !strcmp(n, "old")) {
        if (!d->in_access)
            return 0;
        *out = n[0] == 'v' ? d->acc_value : n[0] == 'o' ? d->acc_old
                                                         : d->acc_addr;
        return 1;
    }
#define REG(s, v) if (!strcmp(n, s)) { *out = (v); return 1; }
    REG("a", (r->af >> 8) & 0xFF) REG("f", r->af & 0xFF)
    REG("b", (r->bc >> 8) & 0xFF) REG("c", r->bc & 0xFF)
    REG("d", (r->de >> 8) & 0xFF) REG("e", r->de & 0xFF)
    REG("h", (r->hl >> 8) & 0xFF) REG("l", r->hl & 0xFF)
    REG("af", r->af) REG("bc", r->bc) REG("de", r->de) REG("hl", r->hl)
    REG("ix", r->ix) REG("iy", r->iy) REG("sp", r->sp) REG("pc", r->pc)
    REG("i", r->i) REG("r", r->r) REG("im", r->im)
    REG("iff1", r->iff1) REG("iff2", r->iff2)
    REG("af'", r->af2) REG("bc'", r->bc2) REG("de'", r->de2) REG("hl'", r->hl2)
    REG("cf", r->f & 1) REG("nf", (r->f >> 1) & 1) REG("pf", (r->f >> 2) & 1)
    REG("yf", (r->f >> 5) & 1) REG("xf", (r->f >> 3) & 1)
    REG("hf", (r->f >> 4) & 1) REG("zf", (r->f >> 6) & 1)
    REG("sf", (r->f >> 7) & 1)
    REG("halted", r->halted) REG("cycles", (long)r->cycles)
    REG("line", r->line) REG("frame", (long)r->frame)
#undef REG
    if (symtabs_find(&d->syms, name, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

/* Evaluate with mtx held; `boundary` says the PC is the instruction address
 * (stopped, or at the instruction hook) rather than the mid-instruction one. */
static int eval_ex_locked(astrodebug *d, const char *text, long *out,
                          char *err, int errsz, int boundary)
{
    expr_ctx ctx;

    ctx.name = expr_name;
    ctx.read8 = expr_read;
    ctx.user = d;
    if (d->m) {
        fill_regs(d->m, &d->rs, boundary);
        d->ec = d->m;
    } else {
        memset(&d->rs, 0, sizeof d->rs);
        d->ec = NULL;
    }
    return expr_eval(text, &ctx, out, err, errsz);
}

static int eval_locked(astrodebug *d, const char *text, long *out, char *err,
                       int errsz)
{
    return eval_ex_locked(d, text, out, err, errsz, 1);
}

int astrodebug_eval(astrodebug *d, const char *text, long *value, char *err,
                    int errsz)
{
    int rc;
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    d->in_access = 0;
    rc = eval_locked(d, text, value, err, errsz);
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

static int cond_true_locked(astrodebug *d, const astrodebug_breakpoint *b,
                            int boundary)
{
    long v = 0;
    char err[64];
    if (!b->condition[0])
        return 1;
    if (eval_ex_locked(d, b->condition, &v, err, sizeof err, boundary) != 0)
        return 1; /* a condition that no longer evaluates stops, not hides */
    return v != 0;
}

/* ---- keeping the core's hooks in step with the engine ------------------------ */

static void instr_hook(astro_machine_t *m, void *user);
static void bus_hook(astro_machine_t *m, int kind, uint16_t addr, uint8_t val,
                     void *user);

/* mtx held. Installs or removes the hooks to match what the engine wants:
 * the instruction hook whenever attached to a live machine, the bus hook only
 * while a watchpoint exists. */
static void sync_hooks_locked(astrodebug *d)
{
    const int live = d->m != NULL && !d->releasing;

    if (live && d->attached) {
        astro_host_set_instr_hook(instr_hook, d);
        if (d->nwatch > 0)
            astro_host_set_bus_hook(bus_hook, d);
        else
            astro_host_set_bus_hook(NULL, NULL);
    } else {
        astro_host_set_instr_hook(NULL, NULL);
        astro_host_set_bus_hook(NULL, NULL);
    }
}

/* `old` for a write is the byte before it. The hook runs after the store, so
 * remember the last value of every watched-for-write address. */
static void seed_shadow_locked(astrodebug *d)
{
    unsigned a;
    if (!d->m || !d->nwatch)
        return;
    for (a = 0; a < 65536; a++)
        if (d->map_mem[a] & ASTRODEBUG_BP_WRITE)
            d->shadow[a] = peek_m(d->m, (uint16_t)a);
}

static void rebuild_locked(astrodebug *d)
{
    int i;
    unsigned a;

    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    memset(d->map_io, 0, sizeof d->map_io);
    d->nwatch = 0;
    for (i = 0; i < d->nbp; i++) {
        const astrodebug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        for (a = b->start; a <= b->end; a++) {
            if (b->type & ASTRODEBUG_BP_EXEC)
                d->map_exec[a] = 1;
            if (b->type & (ASTRODEBUG_BP_READ | ASTRODEBUG_BP_WRITE))
                d->map_mem[a] |= (uint8_t)(b->type & (ASTRODEBUG_BP_READ |
                                                      ASTRODEBUG_BP_WRITE));
            if ((b->type & (ASTRODEBUG_BP_IN | ASTRODEBUG_BP_OUT)) && a < 256)
                d->map_io[a] |= (uint8_t)(b->type & (ASTRODEBUG_BP_IN |
                                                     ASTRODEBUG_BP_OUT));
        }
        if (b->type & (ASTRODEBUG_BP_READ | ASTRODEBUG_BP_WRITE |
                       ASTRODEBUG_BP_IN | ASTRODEBUG_BP_OUT))
            d->nwatch++;
    }
    if (!d->nwatch)
        d->pending = 0;
    seed_shadow_locked(d);
    sync_hooks_locked(d);
}

/* ---- the trace ring ---------------------------------------------------------- */

static int decode_m(astro_machine_t *m, uint16_t pc, z80d_insn *ins)
{
    uint8_t code[4];
    int i;
    for (i = 0; i < 4; i++)
        code[i] = m ? peek_m(m, (uint16_t)(pc + i)) : 0xFF;
    return z80_disassemble(ins, pc, code);
}

static int decode_at(astrodebug *d, uint16_t pc, z80d_insn *ins)
{
    return decode_m(d->m, pc, ins);
}

static void trace_record(astrodebug *d, astro_machine_t *m, uint16_t pc)
{
    astrodebug_trace *t;
    z80d_insn ins;

    if (!d->ring)
        return;
    decode_m(m, pc, &ins);
    pthread_mutex_lock(&d->trace_mtx);
    t = &d->ring[d->ring_head];
    t->pc = pc;
    t->sp = m->cpu.sp;
    t->af = m->cpu.af;
    t->bc = m->cpu.bc;
    t->de = m->cpu.de;
    t->hl = m->cpu.hl;
    t->len = ins.len;
    memcpy(t->bytes, ins.bytes, 4);
    t->cycles = (uint64_t)m->dot / ASTRO_DOTS_PER_CPU_TICK;
    t->line = (int16_t)m->scanline;
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
    pthread_mutex_unlock(&d->trace_mtx);
}

/* ---- the hooks (emulation thread) -------------------------------------------- */

static void bus_hook(astro_machine_t *m, int kind, uint16_t addr, uint8_t val,
                     void *user)
{
    astrodebug *d = user;
    int type, i, io = 0;

    switch (kind) {
    case 0:
        type = ASTRODEBUG_BP_READ;
        if (!(d->map_mem[addr] & type)) return;
        break;
    case 1:
        type = ASTRODEBUG_BP_WRITE;
        if (!(d->map_mem[addr] & type)) return;
        break;
    case 2:
        type = ASTRODEBUG_BP_IN;
        io = 1;
        if (!(d->map_io[addr & 0xFF] & type)) return;
        break;
    case 3:
        type = ASTRODEBUG_BP_OUT;
        io = 1;
        if (!(d->map_io[addr & 0xFF] & type)) return;
        break;
    default:
        return;
    }
    pthread_mutex_lock(&d->mtx);
    if (!d->attached || d->releasing || d->m != m || d->pending) {
        pthread_mutex_unlock(&d->mtx);
        return; /* the first hit in an instruction is the one reported */
    }
    if (type == ASTRODEBUG_BP_READ) {
        /* The chips Z80 reads an instruction's immediate operands and
         * displacement as plain memory reads, indistinguishable from data
         * reads by the pins. They are the bytes after the opcode, read at
         * pc-1 (the CPU advances pc as it reads): ignore those. */
        const uint16_t from = (uint16_t)(d->ins_start + 1);
        z80d_insn ins;
        decode_m(m, d->ins_start, &ins);
        if (addr == (uint16_t)(m->cpu.pc - 1) &&
            (uint16_t)(addr - from) < (uint16_t)(ins.len - 1)) {
            pthread_mutex_unlock(&d->mtx);
            return;
        }
    }
    if (io)
        addr &= 0xFF;
    d->acc_value = val;
    d->acc_addr = addr;
    d->acc_old = val;
    if (type == ASTRODEBUG_BP_WRITE) {
        d->acc_old = d->shadow[addr];
        d->shadow[addr] = val;
    }
    d->in_access = 1;
    for (i = 0; i < d->nbp; i++) {
        astrodebug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & type) && addr >= b->start &&
            addr <= b->end && cond_true_locked(d, b, 0)) {
            b->hits++;
            if (io)
                snprintf(d->pend_reason, sizeof d->pend_reason,
                         "%s $%02X %s $%02X",
                         type == ASTRODEBUG_BP_IN ? "in" : "out", addr,
                         type == ASTRODEBUG_BP_IN ? "->" : "<-", val);
            else
                snprintf(d->pend_reason, sizeof d->pend_reason,
                         "%s $%04X = $%02X",
                         type == ASTRODEBUG_BP_READ ? "read" : "write", addr,
                         val);
            d->pending = 1;
            break;
        }
    }
    d->in_access = 0;
    pthread_mutex_unlock(&d->mtx);
}

/* An enabled execute breakpoint at pc whose condition holds. */
static int exec_hit_locked(astrodebug *d, uint16_t pc)
{
    int i, hit = 0;

    if (!d->map_exec[pc])
        return 0;
    d->acc_addr = pc;
    d->acc_value = peek(d, pc);
    d->acc_old = d->acc_value;
    d->in_access = 1;
    for (i = 0; i < d->nbp; i++) {
        astrodebug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & ASTRODEBUG_BP_EXEC) && pc >= b->start &&
            pc <= b->end && cond_true_locked(d, b, 1)) {
            b->hits++;
            hit = 1;
        }
    }
    d->in_access = 0;
    return hit;
}

/* Park (mtx held; drops it around the picture and the callback). */
static void park_locked(astrodebug *d, astro_machine_t *m, const char *reason,
                        int kind, uint16_t pc)
{
    void (*cb)(void *, int, uint16_t);
    void *ud;

    snprintf(d->reason, sizeof d->reason, "%s", reason);
    d->reason_addr = pc;
    d->stopped = 1;
    d->stop_req = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    d->stop_serial++;
    bump_locked(d);
    pthread_cond_broadcast(&d->evcv);
    cb = d->stop_cb;
    ud = d->stop_ud;
    pthread_mutex_unlock(&d->mtx);
    /* the picture as far as the beam has drawn it */
    astro_host_publish_frame();
    if (cb)
        cb(ud, kind, pc);
    pthread_mutex_lock(&d->mtx);
    while (d->stopped && d->attached && !d->releasing && d->m == m)
        pthread_cond_wait(&d->cv, &d->mtx);
}

static void instr_hook(astro_machine_t *m, void *user)
{
    astrodebug *d = user;
    char why[128];
    int kind = 0, stop = 0;
    uint16_t pc;

    pthread_mutex_lock(&d->mtx);
    if (!d->attached || d->releasing || d->m != m) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    pc = cur_pc(m);
    d->ins_start = pc;
    if (d->trace_on)
        trace_record(d, m, pc);

    if (d->stop_req) {
        snprintf(why, sizeof why, "stopped");
        kind = ASTRODBG_STOP_PAUSE;
        stop = 1;
    } else if (d->pending) {
        snprintf(why, sizeof why, "%s", d->pend_reason);
        kind = ASTRODBG_STOP_WATCH;
        stop = 1;
    } else if (exec_hit_locked(d, pc)) {
        snprintf(why, sizeof why, "breakpoint: exec $%04X", pc);
        kind = ASTRODBG_STOP_BREAKPOINT;
        stop = 1;
    } else {
        switch (d->act) {
        case ACT_STEP:
            if (--d->count <= 0) {
                snprintf(why, sizeof why, "step");
                kind = ASTRODBG_STOP_STEP;
                stop = 1;
            }
            break;
        case ACT_RUNTO:
            if (pc == d->target) {
                snprintf(why, sizeof why, "run to");
                kind = ASTRODBG_STOP_RUNTO;
                stop = 1;
            }
            break;
        case ACT_OVER:
            if (pc == d->target && m->cpu.sp >= d->sp_guard &&
                !(m->cpu.pins & Z80_HALT)) {
                snprintf(why, sizeof why, "step over");
                kind = ASTRODBG_STOP_STEP;
                stop = 1;
            }
            break;
        case ACT_OUT:
            if (pc == d->target && m->cpu.sp > d->sp_guard) {
                snprintf(why, sizeof why, "step out");
                kind = ASTRODBG_STOP_STEP;
                stop = 1;
            }
            break;
        case ACT_SCANLINE:
            if (m->scanline != d->last_line) {
                d->last_line = m->scanline;
                if (--d->count <= 0) {
                    snprintf(why, sizeof why, "scanline");
                    kind = ASTRODBG_STOP_STEP;
                    stop = 1;
                }
            }
            break;
        case ACT_FRAME:
            if (m->frame != d->last_frame) {
                d->last_frame = m->frame;
                if (--d->count <= 0) {
                    snprintf(why, sizeof why, "frame");
                    kind = ASTRODBG_STOP_STEP;
                    stop = 1;
                }
            }
            break;
        default:
            break;
        }
    }
    if (stop)
        park_locked(d, m, why, kind, pc);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- the host's side (debugger_internal.h) ------------------------------------ */

static void init_once(void)
{
    s_dbg.next_id = 1;
    s_dbg.reason_addr = -1;
    snprintf(s_dbg.reason, sizeof s_dbg.reason, "running");
    symtabs_load_text(&s_dbg.syms, astrodebug_builtin_bios_sym, "bios");
}

astrodebug *astrodebug_get(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_once);
    return &s_dbg;
}

void astrodebug_core_started(struct astro_machine *m)
{
    astrodebug *d = astrodebug_get();

    pthread_mutex_lock(&d->mtx);
    d->m = m;
    d->releasing = 0;
    d->stopped = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    d->ins_start = 0;
    /* an attached engine stops the new machine at its first instruction */
    d->stop_req = d->attached;
    bump_locked(d);
    sync_hooks_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_core_stopping(void)
{
    astrodebug *d = astrodebug_get();

    pthread_mutex_lock(&d->mtx);
    d->releasing = 1;
    d->m = NULL;
    d->stopped = 0;
    d->stop_req = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    sync_hooks_locked(d);
    snprintf(d->reason, sizeof d->reason, "running");
    d->reason_addr = -1;
    bump_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_cond_broadcast(&d->evcv);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_set_cart_path(astrodebug *d, const char *path)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    snprintf(d->cart_path, sizeof d->cart_path, "%s", path ? path : "");
    pthread_mutex_unlock(&d->mtx);
}

/* ---- attach / stop / go ------------------------------------------------------- */

void astrodebug_attach(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->attached) {
        d->attached = 1;
        d->stop_req = 1;
        sync_hooks_locked(d);
    }
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_detach(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->attached = 0;
    d->stop_req = 0;
    d->stopped = 0;
    d->pending = 0;
    d->act = ACT_RUN;
    sync_hooks_locked(d);       /* nothing is installed in the core now */
    snprintf(d->reason, sizeof d->reason, "running");
    d->reason_addr = -1;
    pthread_cond_broadcast(&d->cv);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_is_attached(astrodebug *d)
{
    int a;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    a = d->attached;
    pthread_mutex_unlock(&d->mtx);
    return a;
}

int astrodebug_is_stopped(astrodebug *d)
{
    int s;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    s = d->stopped;
    pthread_mutex_unlock(&d->mtx);
    return s;
}

void astrodebug_stop(astrodebug *d)
{
    if (!d) return;
    if (!astrodebug_is_attached(d)) {
        astrodebug_attach(d);
        return;
    }
    pthread_mutex_lock(&d->mtx);
    if (!d->stopped)
        d->stop_req = 1;
    pthread_mutex_unlock(&d->mtx);
}

/* Leave the parked state with an action (mtx held, stopped). */
static void release_locked(astrodebug *d, int act)
{
    d->act = act;
    d->stopped = 0;
    d->stop_req = 0;
    seed_shadow_locked(d); /* pokes made while parked */
    bump_locked(d);
    pthread_cond_broadcast(&d->cv);
}

void astrodebug_resume(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped)
        release_locked(d, ACT_RUN);
    else if (d->stop_req) /* asked to stop, not parked yet: never mind */
        d->stop_req = 0;
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_stop_reason(astrodebug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", d->stopped ? d->reason : "running");
    if (address)
        *address = d->stopped ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->mtx);
    return n < dstsz ? n : dstsz - 1;
}

unsigned astrodebug_generation(astrodebug *d)
{
    unsigned g;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    g = d->generation;
    pthread_mutex_unlock(&d->mtx);
    return g;
}

int astrodebug_wait_stop(astrodebug *d, unsigned gen, int ms)
{
    int waited = 0, ok = 0;
    pthread_mutex_lock(&d->mtx);
    while (waited < ms) {
        if (d->stopped && d->generation != gen) {
            ok = 1;
            break;
        }
        cond_wait_ms(&d->evcv, &d->mtx, 5);
        waited += 5;
    }
    pthread_mutex_unlock(&d->mtx);
    return ok;
}

void astrodebug_set_stop_callback(astrodebug *d,
                                  void (*cb)(void *, int, uint16_t), void *ud)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_ud = ud;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- stepping ------------------------------------------------------------------ */

void astrodebug_step_count(astrodebug *d, long n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_STEP);
    }
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_step(astrodebug *d)
{
    astrodebug_step_count(d, 1);
}

void astrodebug_step_over(astrodebug *d)
{
    z80d_insn ins;
    uint16_t pc;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->m || !d->stopped || !d->attached) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    pc = cur_pc(d->m);
    decode_at(d, pc, &ins);
    if ((ins.flags & (ASTRODASM_CALL | ASTRODASM_BLOCK | ASTRODASM_HALT)) ||
        ((ins.flags & ASTRODASM_RELATIVE) && (ins.flags & ASTRODASM_COND) &&
         ins.bytes[0] == 0x10)) {
        /* CALL/RST, the block repeats, HALT and DJNZ: run to the next
         * instruction with the stack back at (or above) this level, which
         * survives recursion and an interrupt that fires on the way */
        d->target = (uint16_t)(pc + ins.len);
        d->sp_guard = d->m->cpu.sp;
        release_locked(d, ACT_OVER);
    } else {
        d->count = 1;
        release_locked(d, ACT_STEP);
    }
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_step_out(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->m && d->stopped && d->attached) {
        /* to the return address the CALL pushed, with an SP guard;
         * approximate: a routine that juggles its own return address
         * defeats it, and then the machine runs on rather than stopping
         * wrong */
        const uint16_t sp = d->m->cpu.sp;
        d->target = (uint16_t)(peek(d, sp) | (peek(d, (uint16_t)(sp + 1)) << 8));
        d->sp_guard = sp;
        release_locked(d, ACT_OUT);
    }
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_scanline(astrodebug *d, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->m && d->stopped && d->attached) {
        d->last_line = d->m->scanline;
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_SCANLINE);
    }
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_frame(astrodebug *d, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->m && d->stopped && d->attached) {
        d->last_frame = d->m->frame;
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_FRAME);
    }
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_run_to(astrodebug *d, uint16_t addr)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->target = addr;
        release_locked(d, ACT_RUNTO);
    }
    pthread_mutex_unlock(&d->mtx);
}

/* ---- CPU ------------------------------------------------------------------------ */

void astrodebug_cpu_get(astrodebug *d, astrodebug_cpu *o)
{
    zregs r;
    int live = 0;

    memset(o, 0, sizeof *o);
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->m) {
        fill_regs(d->m, &r, d->stopped);
        live = 1;
    }
    pthread_mutex_unlock(&d->mtx);
    if (!live)
        return;
    o->pc = r.pc;
    o->sp = r.sp;
    o->af = r.af;
    o->bc = r.bc;
    o->de = r.de;
    o->hl = r.hl;
    o->ix = r.ix;
    o->iy = r.iy;
    o->wz = r.wz;
    o->af2 = r.af2;
    o->bc2 = r.bc2;
    o->de2 = r.de2;
    o->hl2 = r.hl2;
    o->i = r.i;
    o->r = r.r;
    o->im = r.im;
    o->iff1 = r.iff1;
    o->iff2 = r.iff2;
    o->halted = r.halted;
    o->sf = (r.f >> 7) & 1;
    o->zf = (r.f >> 6) & 1;
    o->yf = (r.f >> 5) & 1;
    o->hf = (r.f >> 4) & 1;
    o->xf = (r.f >> 3) & 1;
    o->pf = (r.f >> 2) & 1;
    o->nf = (r.f >> 1) & 1;
    o->cf = r.f & 1;
    o->cycles = r.cycles;
    o->line = r.line;
    o->frame = (uint32_t)r.frame;
}

static void set_flag(uint16_t *af, int bit, int on)
{
    if (on)
        *af = (uint16_t)(*af | (1u << bit));
    else
        *af = (uint16_t)(*af & ~(1u << bit));
}

void astrodebug_cpu_set(astrodebug *d, int reg, int v)
{
    astro_machine_t *m;
    z80_t *c;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    m = d->m;
    if (!m || !d->stopped) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    c = &m->cpu;
    switch (reg) {
    case ASTRO_REG_PC:
        astro_machine_redirect(m, (uint16_t)v);
        d->ins_start = (uint16_t)v;
        d->reason_addr = v & 0xFFFF;
        break;
    case ASTRO_REG_SP:  c->sp = (uint16_t)v; break;
    case ASTRO_REG_AF:  c->af = (uint16_t)v; break;
    case ASTRO_REG_BC:  c->bc = (uint16_t)v; break;
    case ASTRO_REG_DE:  c->de = (uint16_t)v; break;
    case ASTRO_REG_HL:  c->hl = (uint16_t)v; break;
    case ASTRO_REG_IX:  c->ix = (uint16_t)v; break;
    case ASTRO_REG_IY:  c->iy = (uint16_t)v; break;
    case ASTRO_REG_AF2: c->af2 = (uint16_t)v; break;
    case ASTRO_REG_BC2: c->bc2 = (uint16_t)v; break;
    case ASTRO_REG_DE2: c->de2 = (uint16_t)v; break;
    case ASTRO_REG_HL2: c->hl2 = (uint16_t)v; break;
    case ASTRO_REG_I:   c->i = (uint8_t)v; break;
    case ASTRO_REG_R:   c->r = (uint8_t)v; break;
    case ASTRO_REG_IM:  c->im = (uint8_t)((v & 3) == 3 ? 2 : (v & 3)); break;
    case ASTRO_REG_IFF1: c->iff1 = v != 0; break;
    case ASTRO_REG_IFF2: c->iff2 = v != 0; break;
    case ASTRO_REG_A:   c->a = (uint8_t)v; break;
    case ASTRO_REG_F:   c->f = (uint8_t)v; break;
    case ASTRO_REG_B:   c->b = (uint8_t)v; break;
    case ASTRO_REG_C:   c->c = (uint8_t)v; break;
    case ASTRO_REG_D:   c->d = (uint8_t)v; break;
    case ASTRO_REG_E:   c->e = (uint8_t)v; break;
    case ASTRO_REG_H:   c->h = (uint8_t)v; break;
    case ASTRO_REG_L:   c->l = (uint8_t)v; break;
    case ASTRO_FLAG_S:  set_flag(&c->af, 7, v); break;
    case ASTRO_FLAG_Z:  set_flag(&c->af, 6, v); break;
    case ASTRO_FLAG_H:  set_flag(&c->af, 4, v); break;
    case ASTRO_FLAG_PV: set_flag(&c->af, 2, v); break;
    case ASTRO_FLAG_N:  set_flag(&c->af, 1, v); break;
    case ASTRO_FLAG_C:  set_flag(&c->af, 0, v); break;
    default:
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    refresh_fetch(m);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- memory --------------------------------------------------------------------- */

int astrodebug_read(astrodebug *d, uint16_t addr, uint8_t *dst, int n)
{
    astro_machine_t *m;
    int i;

    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    m = d->m;
    for (i = 0; i < n; i++)
        dst[i] = m ? peek_m(m, (uint16_t)(addr + i)) : 0;
    pthread_mutex_unlock(&d->mtx);
    return m ? n : 0;
}

void astrodebug_write(astrodebug *d, uint16_t addr, uint8_t value)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->m) {
        astro_machine_poke(d->m, addr, value);
        if (d->stopped)
            refresh_fetch(d->m);
        bump_locked(d);
    }
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_vram_read(astrodebug *d, uint16_t addr, uint8_t *dst, int n)
{
    int i;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < n; i++)
        dst[i] = d->m ? d->m->vram[(addr + i) & (ASTRO_VRAM_SIZE - 1)] : 0;
    pthread_mutex_unlock(&d->mtx);
    return d->m ? n : 0;
}

int astrodebug_vram_write(astrodebug *d, uint16_t addr, const uint8_t *src, int n)
{
    int i, ok;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    ok = d->m != NULL;
    for (i = 0; ok && i < n; i++)
        d->m->vram[(addr + i) & (ASTRO_VRAM_SIZE - 1)] = src[i];
    if (ok)
        bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return ok ? n : 0;
}

void astrodebug_video_snapshot(astrodebug *d, astrovid_snapshot *out)
{
    pthread_mutex_lock(&d->mtx);
    if (d->m)
        astrovid_snapshot_get(out);
    else
        memset(out, 0, sizeof *out);
    pthread_mutex_unlock(&d->mtx);
}

static const struct { const char *name; int bytes, base; } exp_desc[ASTRO_EXP_COUNT] = {
    [ASTRO_EXP_NONE]          = { "none", 0, 0 },
    [ASTRO_EXP_BLUE_RAM_4K]   = { "Blue RAM 4K", 0x1000, 0x6000 },
    [ASTRO_EXP_BLUE_RAM_16K]  = { "Blue RAM 16K", 0x4000, 0x6000 },
    [ASTRO_EXP_BLUE_RAM_32K]  = { "Blue RAM 32K", 0x8000, 0x6000 },
    [ASTRO_EXP_VIPER_SYS1]    = { "Viper System 1 RAM 16K", 0x4000, 0x6000 },
    [ASTRO_EXP_LIL_WHITE_RAM] = { "Lil' White RAM 32K", 0x8000, 0x5000 },
    [ASTRO_EXP_RL64_RAM]      = { "RL64 RAM 44K", 0xB000, 0x5000 },
};

static void memmap_locked(astrodebug *d, astrodebug_memmap *o)
{
    memset(o, 0, sizeof *o);
    o->bios_name = "Bally BIOS";
    o->exp_name = "none";
    if (!d->m)
        return;
    if ((int)d->m->exp_kind >= 0 && d->m->exp_kind < ASTRO_EXP_COUNT) {
        o->exp_name = exp_desc[d->m->exp_kind].name;
        o->exp_bytes = exp_desc[d->m->exp_kind].bytes;
        o->exp_base = exp_desc[d->m->exp_kind].base;
    }
    o->video_mode = d->m->video_mode & 1;
    o->cart_present = 1;
}

void astrodebug_memmap_get(astrodebug *d, astrodebug_memmap *o)
{
    pthread_mutex_lock(&d->mtx);
    memmap_locked(d, o);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_cart_info(astrodebug *d, astrodebug_cart *o)
{
    memset(o, 0, sizeof *o);
    snprintf(o->kind, sizeof o->kind, "none");
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    snprintf(o->path, sizeof o->path, "%s", d->cart_path);
    if (d->m) {
        const astro_cart_t *c = &d->m->cart;
        o->present = 1;
        o->mailbox_live = c->mailbox_live;
        o->staged = c->have_staged;
        o->image_bytes = c->image ? c->image_size : 0;
        o->pages = c->app_npages;
        if (c->app_store && c->app_npages) {
            snprintf(o->kind, sizeof o->kind, "APPBANK");
        } else if (c->hot_image) {
            snprintf(o->kind, sizeof o->kind, "%s",
                     c->hot_mask == 0x3F ? "GAME256" : "GAME512");
            o->pages = o->image_bytes / 4096;
        } else {
            snprintf(o->kind, sizeof o->kind, "FLAT");
            o->image_bytes = ASTRO_CART_WINDOW;
        }
        if (c->bank[0] && c->image)
            o->bank[0] = (int)((c->bank[0] - c->image) / 4096);
        else
            o->bank[0] = -1;
        if (c->bank[1] && c->image)
            o->bank[1] = (int)((c->bank[1] - c->image) / 4096);
        else
            o->bank[1] = -1;
    }
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_memory_map(astrodebug *d, astrodebug_region *out, int max)
{
    astrodebug_memmap mm;
    int n = 0;
#define REG_(l, h, r, w, nm) do { if (n < max) { out[n].lo = (l); \
        out[n].hi = (h); out[n].readable = (r); out[n].writable = (w); \
        snprintf(out[n].name, sizeof out[n].name, "%s", nm); n++; } } while (0)

    if (!d || !out || max <= 0) return 0;
    astrodebug_memmap_get(d, &mm);
    if (!mm.cart_present) return 0;
    REG_(0x0000, 0x0FFF, 1, 1, "BIOS (writes: magic function generator)");
    REG_(0x1000, 0x1FFF, 1, 0, mm.bios_name);
    REG_(0x2000, 0x3FFF, 1, 0, "Cartridge slot (FujiNet cartridge)");
    REG_(0x4000, 0x4FFF, 1, 1, "Screen RAM");
    if (mm.exp_bytes) {
        unsigned hi = (unsigned)mm.exp_base + (unsigned)mm.exp_bytes - 1;
        REG_((uint16_t)mm.exp_base, (uint16_t)(hi > 0xFFFF ? 0xFFFF : hi), 1, 1,
             mm.exp_name);
        if (hi < 0xFFFF)
            REG_((uint16_t)(hi + 1), 0xFFFF, 0, 0, "Open bus");
    } else {
        REG_(0x5000, 0xFFFF, 0, 0, "Open bus (no expansion RAM)");
    }
#undef REG_
    return n;
}

/* The data chip's and the custom I/O chip's ports (HVGLIB's names, and
 * machine.c's io_read/io_write for which ports exist). */
static const char *port_name(uint8_t port, int is_in)
{
    static const char *const out_names[0x1A] = {
        "COL0R color 0 right", "COL1R color 1 right", "COL2R color 2 right",
        "COL3R color 3 right", "COL0L color 0 left", "COL1L color 1 left",
        "COL2L color 2 left", "COL3L color 3 left",
        "CONCM resolution mode", "HORCB color boundary, background",
        "VERBL vertical blank line", "color block transfer",
        "MAGIC function generator", "INFBK interrupt vector",
        "INMOD interrupt mode", "INLIN interrupt line",
        "TONMO master oscillator", "tone A frequency", "tone B frequency",
        "tone C frequency", "vibrato", "VOLC tone C volume, noise mode",
        "VOLAB tone A/B volume", "VOLN noise volume", "sound block transfer",
        "XPAND expand register",
    };
    if (is_in) {
        static const char *const sw[4] = { "SW0 player 1 handle",
            "SW1 player 2 handle", "SW2 player 3 handle", "SW3 player 4 handle" };
        static const char *const key[4] = { "KEY0 keypad column 0 (right)",
            "KEY1 keypad column 1", "KEY2 keypad column 2",
            "KEY3 keypad column 3 (left)" };
        static const char *const pot[4] = { "POT0 player 1 knob",
            "POT1 player 2 knob", "POT2 player 3 knob", "POT3 player 4 knob" };
        if (port == 0x08) return "INTST intercept feedback";
        if (port == 0x0E) return "VERAF vertical address feedback";
        if (port == 0x0F) return "HORAF horizontal address feedback";
        if (port >= 0x10 && port <= 0x13) return sw[port - 0x10];
        if (port >= 0x14 && port <= 0x17) return key[port - 0x14];
        if (port >= 0x1C && port <= 0x1F) return pot[port - 0x1C];
        return "";
    }
    return port < 0x1A ? out_names[port] : "";
}

int astrodebug_io_state(astrodebug *d, char *dst, int dstsz)
{
    astrodebug_memmap mm;
    astrovid_snapshot *snap;
    int n = 0, i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    snap = malloc(sizeof *snap);
    if (!snap) return 0;
    pthread_mutex_lock(&d->mtx);
    memmap_locked(d, &mm);
    if (d->m)
        astrovid_snapshot_get(snap);
    else
        memset(snap, 0, sizeof *snap);
    pthread_mutex_unlock(&d->mtx);

#define PUT(...) do { if (n < dstsz - 1) { \
        int w_ = snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); \
        if (w_ > 0) n += w_ < dstsz - n ? w_ : dstsz - n - 1; } } while (0)

    if (!mm.cart_present) {
        PUT("The machine is not running.\n");
        free(snap);
        return n;
    }
    PUT("Memory map (video %s)\n", mm.video_mode ? "commercial, 320x204"
                                                 : "consumer, 160x102");
    PUT("  $0000-$0FFF  %s (writes go to the magic function generator)\n",
        mm.bios_name);
    PUT("  $1000-$1FFF  %s (writes dropped)\n", mm.bios_name);
    PUT("  $2000-$3FFF  cartridge slot: FujiNet cartridge (reads only; see the Cart tab)\n");
    PUT("  $4000-$4FFF  screen RAM, 4K\n");
    if (mm.exp_bytes)
        PUT("  $%04X-$%04X  expansion RAM: %s\n", mm.exp_base,
            mm.exp_base + mm.exp_bytes - 1 > 0xFFFF ? 0xFFFF
                                                     : mm.exp_base + mm.exp_bytes - 1,
            mm.exp_name);
    else
        PUT("  $5000-$FFFF  open bus (no expansion RAM)\n");
    PUT("\nData chip OUT  colors R");
    for (i = 0; i < 4; i++) PUT(" %02X", snap->colors[i]);
    PUT("  L");
    for (i = 4; i < 8; i++) PUT(" %02X", snap->colors[i]);
    PUT("\n  $08 mode %d  $09 split %u bg %02X  $0A vblank %u\n",
        snap->video_mode, snap->colorsplit, snap->bgdata, snap->vblank);
    PUT("  $0C magic %02X  $0D vector %02X  $0E mode %02X  $0F line %u\n",
        snap->funcgen_control, snap->interrupt_vector, snap->interrupt_enabl,
        snap->interrupt_scanline);
    PUT("  $19 expand %u/%u   IN $08 intercept %02X  $0E/$0F pen %u/%u\n",
        snap->expand_color[0], snap->expand_color[1], snap->intercept,
        snap->vertical_feedback, snap->horizontal_feedback);
    PUT("Sound OUT $10-$17");
    for (i = 0; i < 8; i++) PUT(" %02X", snap->snd_reg[i]);
    PUT("\n\nPorts (low address byte; the high byte is a second data path)\n");
    PUT("  OUT $00-$07 colors   $08 mode   $09 boundary   $0A vblank   $0B color xfer\n");
    PUT("      $0C magic   $0D-$0F interrupt vector/mode/line\n");
    PUT("      $10-$17 sound   $18 sound block xfer   $19 expand\n");
    PUT("  IN  $08 intercept   $0E-$0F pen feedback   $10-$13 handles\n");
    PUT("      $14-$17 keypad   $1C-$1F knobs\n");
#undef PUT
    free(snap);
    return n;
}

/* ---- disassembly ---------------------------------------------------------------- */

static void label_for_locked(astrodebug *d, uint16_t addr, char *dst, int dstsz,
                             int with_offset)
{
    const char *l = symtabs_exact(&d->syms, addr);
    dst[0] = '\0';
    if (l) {
        snprintf(dst, (size_t)dstsz, "%s", l);
    } else if (with_offset) {
        uint16_t off = 0;
        l = symtabs_at(&d->syms, addr, &off);
        if (l && off > 0 && off <= 16)
            snprintf(dst, (size_t)dstsz, "%s+$%X", l, off);
    }
}

static void fill_line_locked(astrodebug *d, uint16_t addr, astrodebug_line *l,
                             uint16_t pc)
{
    z80d_insn ins;
    int i, o = 0;

    memset(l, 0, sizeof *l);
    decode_at(d, addr, &ins);
    l->address = addr;
    l->length = ins.len;
    l->is_pc = addr == pc;
    l->target = ins.target;
    l->flags = ins.flags;
    for (i = 0; i < d->nbp; i++) {
        const astrodebug_breakpoint *b = &d->bp[i];
        if ((b->type & ASTRODEBUG_BP_EXEC) && addr >= b->start &&
            addr <= b->end) {
            l->has_breakpoint = 1;
            if (b->enabled)
                l->breakpoint_enabled = 1;
        }
    }
    for (i = 0; i < ins.len && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X",
                      i ? " " : "", ins.bytes[i]);
    label_for_locked(d, addr, l->label, sizeof l->label, 0);
    snprintf(l->disasm, sizeof l->disasm, "%s", ins.text);

    if (ins.flags & (ASTRODASM_JUMP | ASTRODASM_CALL)) {
        label_for_locked(d, ins.target, l->comment, sizeof l->comment, 1);
    } else if ((ins.bytes[0] == 0xDB || ins.bytes[0] == 0xD3) && ins.len == 2) {
        /* IN A,(n) / OUT (n),A: the port's name */
        snprintf(l->comment, sizeof l->comment, "%s",
                 port_name(ins.bytes[1], ins.bytes[0] == 0xDB));
    } else {
        /* an absolute operand that names a place: LD (nn),A, LD HL,nn ... */
        const char *p = strchr(ins.text, '$');
        if (p && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
            isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
            unsigned v;
            if (sscanf(p + 1, "%4x", &v) == 1)
                label_for_locked(d, (uint16_t)v, l->comment,
                                 sizeof l->comment, 0);
        }
    }
}

int astrodebug_disassemble(astrodebug *d, uint16_t addr, astrodebug_line *out,
                           int max, int *pc_line)
{
    int n = 0;
    uint16_t pc;

    if (pc_line) *pc_line = -1;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->m) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    pc = d->stopped ? cur_pc(d->m) : d->m->cpu.pc;
    while (n < max) {
        fill_line_locked(d, addr, &out[n], pc);
        if (out[n].is_pc && pc_line && *pc_line < 0)
            *pc_line = n;
        addr = (uint16_t)(addr + out[n].length);
        n++;
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

static int row_address_locked(astrodebug *d, uint16_t addr, int rows)
{
    z80d_insn ins;
    int back;

    if (!d->m || rows == 0)
        return addr;
    if (rows > 0) {
        int i;
        for (i = 0; i < rows; i++)
            addr = (uint16_t)(addr + decode_at(d, addr, &ins));
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on addr,
     * from as far back as the rows could take. */
    rows = -rows;
    for (back = rows * 4; back >= 1; back--) {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)(addr - back);
        while (count < 256) {
            int dist = (uint16_t)(addr - a);
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)(a + decode_at(d, a, &ins));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (uint16_t)(addr - rows);
}

int astrodebug_row_address(astrodebug *d, uint16_t addr, int rows)
{
    int a;
    if (!d) return addr;
    pthread_mutex_lock(&d->mtx);
    a = row_address_locked(d, addr, rows);
    pthread_mutex_unlock(&d->mtx);
    return a;
}

/* ---- symbols and labels ---------------------------------------------------------- */

int astrodebug_symbols_load(astrodebug *d, const char *path, const char *table)
{
    int rc;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_load_file(&d->syms, path, table);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

const char *astrodebug_symbol_at(astrodebug *d, uint16_t addr, uint16_t *offset)
{
    return symtabs_at(&d->syms, addr, offset);
}

int astrodebug_symbol_find(astrodebug *d, const char *name, uint16_t *addr)
{
    return symtabs_find(&d->syms, name, addr);
}

int astrodebug_load_symbols(astrodebug *d, const char *path, char *msg,
                            int msgsz)
{
    char cand[PATH_MAX_ + 8];
    int n;

    if (!d) return -1;
    if (msg && msgsz > 0) msg[0] = '\0';
    if (!path || !*path) {
        static const char *const exts[] = { ".sym", ".map", ".noi", NULL };
        char cart[PATH_MAX_];
        const char *dot, *slash;
        size_t stem;
        int i;

        pthread_mutex_lock(&d->mtx);
        snprintf(cart, sizeof cart, "%s", d->cart_path);
        pthread_mutex_unlock(&d->mtx);
        if (!cart[0]) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz,
                         "No cartridge file is open; choose a symbol file.");
            return -1;
        }
        dot = strrchr(cart, '.');
        slash = strrchr(cart, '/');
#ifdef _WIN32
        {
            const char *bs = strrchr(cart, '\\');
            if (bs && (!slash || bs > slash)) slash = bs;
        }
#endif
        stem = (dot && (!slash || dot > slash)) ? (size_t)(dot - cart)
                                                : strlen(cart);
        path = NULL;
        for (i = 0; exts[i]; i++) {
            FILE *f;
            snprintf(cand, sizeof cand, "%.*s%s", (int)stem, cart, exts[i]);
            f = fopen(cand, "r");
            if (f) {
                fclose(f);
                path = cand;
                break;
            }
        }
        if (!path) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz,
                         "No .sym, .map or .noi next to %s", cart);
            return -1;
        }
    }
    pthread_mutex_lock(&d->mtx);
    n = symtabs_load_file(&d->syms, path, "file");
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    if (msg && msgsz > 0) {
        if (n < 0)
            snprintf(msg, (size_t)msgsz, "Cannot read %s", path);
        else
            snprintf(msg, (size_t)msgsz, "%d label%s from %s", n,
                     n == 1 ? "" : "s", path);
    }
    return n;
}

int astrodebug_label_address(astrodebug *d, const char *label)
{
    uint16_t a;
    int rc;
    if (!d || !label) return -1;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_find(&d->syms, label, &a) ? (int)a : -1;
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

int astrodebug_address_label(astrodebug *d, uint16_t addr, char *dst, int dstsz)
{
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    label_for_locked(d, addr, dst, dstsz, 0);
    pthread_mutex_unlock(&d->mtx);
    return (int)strlen(dst);
}

int astrodebug_set_label(astrodebug *d, uint16_t addr, const char *label)
{
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    symtabs_set_user(&d->syms, addr, label);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return 0;
}

int astrodebug_label_completions(astrodebug *d, const char *word, char *dst,
                                 int dstsz, int maxn)
{
    size_t wl = strlen(word);
    const char *name;
    int i, count = 0, n = 0;

    if (!wl || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&d->mtx);
    for (i = 0; (name = symtabs_name_at(&d->syms, i)) != NULL && count < maxn;
         i++) {
        size_t k;
        for (k = 0; k < wl && name[k]; k++) {
            char a = name[k], b = word[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (k == wl) {
            int w = snprintf(dst + n, (size_t)(dstsz - n), "%s\n", name);
            if (w < 0 || w >= dstsz - n) break;
            n += w;
            count++;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    return count;
}

/* ---- breakpoints ----------------------------------------------------------------- */

static void set_err(char *err, int errsz, const char *msg)
{
    if (err && errsz > 0)
        snprintf(err, (size_t)errsz, "%s", msg);
}

int astrodebug_breakpoint_add(astrodebug *d, int type, uint16_t start,
                              uint16_t end, const char *cond, char *err,
                              int errsz)
{
    astrodebug_breakpoint *b;
    int id;

    if (err && errsz > 0) err[0] = '\0';
    if (!d) return -1;
    if (!(type & 31) || (type & ~31)) {
        set_err(err, errsz, "no such breakpoint type");
        return -1;
    }
    if (end < start) {
        uint16_t t = start;
        start = end;
        end = t;
    }
    if ((type & (ASTRODEBUG_BP_IN | ASTRODEBUG_BP_OUT)) &&
        (type & ~(ASTRODEBUG_BP_IN | ASTRODEBUG_BP_OUT)) == 0 && end > 255) {
        set_err(err, errsz, "ports are 0-255");
        return -1;
    }
    pthread_mutex_lock(&d->mtx);
    if (cond && *cond) {
        long v;
        char why[96];
        d->in_access = 1;
        d->acc_value = d->acc_addr = 0;
        if (eval_locked(d, cond, &v, why, sizeof why) != 0 &&
            strcmp(why, "division by zero") != 0) {
            d->in_access = 0;
            pthread_mutex_unlock(&d->mtx);
            set_err(err, errsz, why);
            return -1;
        }
        d->in_access = 0;
    }
    if (d->nbp >= MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        set_err(err, errsz, "too many breakpoints");
        return -1;
    }
    b = &d->bp[d->nbp++];
    memset(b, 0, sizeof *b);
    b->id = id = d->next_id++;
    b->type = type;
    b->start = start;
    b->end = end;
    b->enabled = 1;
    snprintf(b->condition, sizeof b->condition, "%s", cond ? cond : "");
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return id;
}

void astrodebug_breakpoint_remove(astrodebug *d, int id)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++) {
        if (d->bp[i].id == id) {
            memmove(&d->bp[i], &d->bp[i + 1],
                    (size_t)(d->nbp - i - 1) * sizeof d->bp[0]);
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_breakpoint_enable(astrodebug *d, int id, int enabled)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i].id == id)
            d->bp[i].enabled = enabled != 0;
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_breakpoint_list(astrodebug *d, astrodebug_breakpoint *out, int max)
{
    int n;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void astrodebug_breakpoint_clear_hits(astrodebug *d)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        d->bp[i].hits = 0;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_breakpoint_clear(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->nbp = 0;
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_breakpoint_check(astrodebug *d, uint16_t addr)
{
    int i, found = 0;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if ((d->bp[i].type & ASTRODEBUG_BP_EXEC) && addr >= d->bp[i].start &&
            addr <= d->bp[i].end)
            found = 1;
    pthread_mutex_unlock(&d->mtx);
    return found;
}

int astrodebug_breakpoint_toggle(astrodebug *d, uint16_t addr)
{
    int i, found = -1;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i].type == ASTRODEBUG_BP_EXEC && d->bp[i].start == addr &&
            d->bp[i].end == addr) {
            found = d->bp[i].id;
            break;
        }
    pthread_mutex_unlock(&d->mtx);
    if (found >= 0) {
        astrodebug_breakpoint_remove(d, found);
        return 0;
    }
    return astrodebug_breakpoint_add(d, ASTRODEBUG_BP_EXEC, addr, addr, NULL,
                                     NULL, 0) >= 0;
}

/* ---- trace ------------------------------------------------------------------------ */

void astrodebug_trace_enable(astrodebug *d, int on)
{
    if (!d) return;
    if (on) {
        pthread_mutex_lock(&d->trace_mtx);
        if (!d->ring)
            d->ring = calloc(TRACE_N, sizeof *d->ring);
        if (!d->ring)
            on = 0;
        pthread_mutex_unlock(&d->trace_mtx);
    }
    pthread_mutex_lock(&d->mtx);
    d->trace_on = on != 0;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_trace_enabled(astrodebug *d)
{
    return d && d->trace_on;
}

int astrodebug_trace_read(astrodebug *d, astrodebug_trace *out, int max)
{
    int n, i;
    if (!d) return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = !d->ring ? 0 : d->ring_count < max ? d->ring_count : max;
    for (i = 0; i < n; i++)
        out[i] = d->ring[(d->ring_head - 1 - i + TRACE_N) % TRACE_N];
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

uint64_t astrodebug_stop_serial(astrodebug *d)
{
    uint64_t v;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    v = d->stop_serial;
    pthread_mutex_unlock(&d->mtx);
    return v;
}

int astrodebug_trace_count(astrodebug *d)
{
    int n;
    if (!d) return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = d->ring_count;
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

int astrodebug_trace_format(const astrodebug_trace *t, char *dst, int dstsz)
{
    z80d_insn ins;
    uint8_t code[4];
    int i;

    if (!t || !dst || dstsz <= 0) return 0;
    for (i = 0; i < 4; i++) code[i] = t->bytes[i];
    z80_disassemble(&ins, t->pc, code);
    return snprintf(dst, (size_t)dstsz,
                    "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X",
                    t->pc, ins.text, t->af, t->bc, t->de, t->hl, t->sp);
}

int astrodebug_trace_match(const astrodebug_trace *t, int filter, uint16_t lo,
                           uint16_t hi)
{
    z80d_insn ins;
    uint8_t code[4];
    int i;

    if (!t) return 0;
    if (filter == ASTRODEBUG_TRACE_RANGE) {
        if (lo > hi) { uint16_t x = lo; lo = hi; hi = x; }
        return t->pc >= lo && t->pc <= hi;
    }
    if (filter == ASTRODEBUG_TRACE_BRANCHES) {
        for (i = 0; i < 4; i++) code[i] = t->bytes[i];
        z80_disassemble(&ins, t->pc, code);
        return (ins.flags & (ASTRODASM_JUMP | ASTRODASM_CALL |
                             ASTRODASM_RET)) != 0;
    }
    return 1;
}

/* ---- memory search ------------------------------------------------------------ */

int astrodebug_search(astrodebug *d, const uint8_t *pat, int n, uint8_t mask,
                      int flags, uint16_t from, uint16_t to, uint16_t *out,
                      int max)
{
    unsigned a, last;
    int found = 0, k;

    if (!d || !pat || n <= 0 || !out || max <= 0) return -1;
    pthread_mutex_lock(&d->mtx);
    if (!d->m || !d->stopped) {
        pthread_mutex_unlock(&d->mtx);
        return -1;
    }
    if (from > to) { uint16_t x = from; from = to; to = x; }
    last = to;
    for (a = from; a <= last && a + (unsigned)n - 1 <= 0xFFFF && found < max;
         a++) {
        for (k = 0; k < n; k++) {
            uint8_t b = peek_m(d->m, (uint16_t)(a + (unsigned)k));
            uint8_t p = pat[k];
            if (flags & ASTRODEBUG_SEARCH_NOCASE) {
                if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 32);
                if (p >= 'a' && p <= 'z') p = (uint8_t)(p - 32);
            }
            if ((b & mask) != (p & mask))
                break;
        }
        if (k == n)
            out[found++] = (uint16_t)a;
    }
    pthread_mutex_unlock(&d->mtx);
    return found;
}

int astrodebug_search_next(astrodebug *d, const uint8_t *pat, int n,
                           uint8_t mask, int flags, uint16_t from,
                           int backward, uint16_t *found)
{
    uint16_t hit[1];
    int r;

    if (!found) return -1;
    if (!backward) {
        unsigned start = (unsigned)from + 1;
        if (start <= 0xFFFF) {
            r = astrodebug_search(d, pat, n, mask, flags, (uint16_t)start,
                                  0xFFFF, hit, 1);
            if (r < 0) return -1;
            if (r > 0) { *found = hit[0]; return 1; }
        }
        r = astrodebug_search(d, pat, n, mask, flags, 0, from, hit, 1);
        if (r <= 0) return r;
        *found = hit[0];
        return 1;
    } else {
        uint16_t last = 0;
        int have = 0;
        uint16_t buf[64];
        unsigned lo = 0;
        /* the last match strictly before `from`, else the last overall */
        for (int pass = 0; pass < 2 && !have; pass++) {
            unsigned a = pass == 0 ? 0 : (unsigned)from;
            unsigned end = pass == 0 ? (from ? (unsigned)from - 1 : 0)
                                     : 0xFFFF;
            if (pass == 0 && from == 0) continue;
            lo = a;
            while (lo <= end) {
                r = astrodebug_search(d, pat, n, mask, flags, (uint16_t)lo,
                                      (uint16_t)end, buf, 64);
                if (r < 0) return -1;
                if (r == 0) break;
                last = buf[r - 1];
                have = 1;
                if (r < 64) break;
                lo = (unsigned)buf[r - 1] + 1;
            }
        }
        if (!have) return 0;
        *found = last;
        return 1;
    }
}

void astrodebug_trace_clear(astrodebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->trace_mtx);
    d->ring_head = d->ring_count = 0;
    pthread_mutex_unlock(&d->trace_mtx);
    astrodebug_touch(d);
}

/* ---- files ------------------------------------------------------------------------ */

static int write_bytes(const char *path, const uint8_t *data, size_t n,
                       char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        if (f) fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
        return -1;
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

static void not_running(char *msg, int msgsz)
{
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "The machine is not running.");
}

int astrodebug_save(astrodebug *d, const char *kind, const char *path, char *msg,
                    int msgsz)
{
    FILE *f;
    int live;

    if (!d || !kind || !path) {
        not_running(msg, msgsz);
        return -1;
    }
    if (!strcmp(kind, "mem") || !strcmp(kind, "vram")) {
        const int mem = kind[0] == 'm';
        const size_t n = mem ? 0x10000 : ASTRO_VRAM_SIZE;
        uint8_t *buf = malloc(0x10000);
        int rc;

        if (!buf) return -1;
        pthread_mutex_lock(&d->mtx);
        live = d->m != NULL;
        if (live) {
            if (mem) {
                uint32_t a;
                for (a = 0; a < 0x10000; a++)
                    buf[a] = peek_m(d->m, (uint16_t)a);
            } else {
                memcpy(buf, d->m->vram, ASTRO_VRAM_SIZE);
            }
        }
        pthread_mutex_unlock(&d->mtx);
        if (!live) {
            free(buf);
            not_running(msg, msgsz);
            return -1;
        }
        rc = write_bytes(path, buf, n, msg, msgsz);
        free(buf);
        return rc;
    }
    if (!strcmp(kind, "dis")) {
        uint32_t a = 0;

        pthread_mutex_lock(&d->mtx);
        live = d->m != NULL;
        f = live ? fopen(path, "w") : NULL;
        if (f) {
            const uint16_t pc = d->stopped ? cur_pc(d->m) : d->m->cpu.pc;
            while (a < 0x10000) {
                astrodebug_line l;
                fill_line_locked(d, (uint16_t)a, &l, pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %04X  %-12s %-24s%s%s\n", l.address, l.bytes,
                        l.disasm, l.comment[0] ? "; " : "", l.comment);
                a += (uint32_t)l.length;
            }
            fclose(f);
        }
        pthread_mutex_unlock(&d->mtx);
        if (!live) {
            not_running(msg, msgsz);
            return -1;
        }
        if (!f) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (!strcmp(kind, "regs")) {
        astrodebug_cpu cp;
        astrovid_snapshot *snap = malloc(sizeof *snap);
        char text[2048];

        if (!snap) return -1;
        astrodebug_cpu_get(d, &cp);
        pthread_mutex_lock(&d->mtx);
        live = d->m != NULL;
        if (live)
            astrovid_snapshot_get(snap);
        pthread_mutex_unlock(&d->mtx);
        if (!live) {
            free(snap);
            not_running(msg, msgsz);
            return -1;
        }
        f = fopen(path, "w");
        if (!f) {
            free(snap);
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        fprintf(f, "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X "
                   "IX=%04X IY=%04X\n",
                cp.pc, cp.sp, cp.af, cp.bc, cp.de, cp.hl, cp.ix, cp.iy);
        fprintf(f, "AF'=%04X BC'=%04X DE'=%04X HL'=%04X I=%02X R=%02X "
                   "IM=%d IFF1=%d IFF2=%d\n",
                cp.af2, cp.bc2, cp.de2, cp.hl2, cp.i, cp.r, cp.im, cp.iff1,
                cp.iff2);
        fprintf(f, "cycles=%llu frame=%u line=%d\n",
                (unsigned long long)cp.cycles, cp.frame, cp.line);
        astrovid_format_state(snap, text, sizeof text);
        fprintf(f, "\n%s", text);
        fclose(f);
        free(snap);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz,
                 "Unknown save kind %s (dis, mem, vram, regs)", kind);
    return -1;
}

/* ---- FujiNet ------------------------------------------------------------------------ */

/* The cart's tap: the emulator thread, any time. */
static void fuji_tap(int dir, const uint8_t *buf, size_t len)
{
    astrodebug *d = &s_dbg;

    pthread_mutex_lock(&d->fuji_mtx);
    fujitrace_feed(&d->fuji, dir, buf, len);
    pthread_mutex_unlock(&d->fuji_mtx);
}

void astrodebug_fujinet_install(astrodebug *d)
{
    (void)d;
    astro_cart_set_trace(fuji_tap);
}

int astrodebug_fujinet_trace(astrodebug *d, astrodebug_fuji_packet *out, int max)
{
    int n;
    pthread_mutex_lock(&d->fuji_mtx);
    n = fujitrace_copy(&d->fuji, out, max);
    pthread_mutex_unlock(&d->fuji_mtx);
    return n;
}

void astrodebug_fujinet_trace_clear(astrodebug *d)
{
    pthread_mutex_lock(&d->fuji_mtx);
    fujitrace_clear(&d->fuji);
    pthread_mutex_unlock(&d->fuji_mtx);
}

int astrodebug_fujinet_state(astrodebug *d, char *dst, int dstsz)
{
    (void)d;
    if (dstsz <= 0)
        return 0;
    if (!astro_host_is_running())
        return snprintf(dst, (size_t)dstsz, "The Astrocade is off\n");
    return astro_cart_describe(&astro_host_machine()->cart, dst, dstsz);
}
