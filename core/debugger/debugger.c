/*
 * debugger.c -- the Z80 debugger engine (see astrodebug.h).
 *
 * A singleton hung off the emulator core's instruction-boundary hook. All the
 * decision-making runs on the emulator thread inside hook_cb; the UI thread
 * only sets requests (pause/resume/step) and, while paused, reads state.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <string.h>

#include "astro_internal.h"
#include "astrodebug.h"
#include "host.h"

/* the chips disassembler's implementation lives in this one TU */
#define CHIPS_UTIL_IMPL
#include "z80/z80dasm.h"

typedef enum {
    RUN = 0,        /* free-running */
    STEP,           /* run one instruction, then pause */
    PAUSE_REQ,      /* pause at the next boundary */
    PAUSED,         /* parked in the hook */
} dbg_mode;

struct astrodebug {
    int engaged;
    dbg_mode mode;
    uint64_t stop_serial;

    uint16_t breakpoints[ASTRODEBUG_MAX_BREAKPOINTS];
    int nbreak;

    int has_oneshot;
    uint16_t oneshot;
    int has_sp_guard;       /* step-out: only fire the oneshot at/above this SP */
    uint16_t sp_guard;

    pthread_mutex_t mtx;
    pthread_cond_t cond;    /* the parked emulator thread waits on this */
    astro_machine_t *m;     /* set on the first hook call */
};

static struct astrodebug s_dbg;

static int bp_hit(struct astrodebug *d, uint16_t pc)
{
    for (int i = 0; i < d->nbreak; i++)
        if (d->breakpoints[i] == pc)
            return 1;
    return 0;
}

/* Runs on the EMULATOR thread at every instruction boundary. */
static void hook_cb(astro_machine_t *m, void *user)
{
    struct astrodebug *d = user;

    pthread_mutex_lock(&d->mtx);
    d->m = m;
    const uint16_t pc = m->cpu.pc;

    int stop = 0;
    if (d->mode == PAUSE_REQ)
        stop = 1;
    else if (d->mode == STEP)
        stop = 1;                       /* one instruction has run since resume */
    else if (bp_hit(d, pc))
        stop = 1;
    else if (d->has_oneshot && pc == d->oneshot &&
             (!d->has_sp_guard || m->cpu.sp >= d->sp_guard))
        stop = 1;

    if (stop) {
        d->has_oneshot = 0;
        d->has_sp_guard = 0;
        d->mode = PAUSED;
        d->stop_serial++;
        /* park here until the UI resumes/steps */
        while (d->mode == PAUSED)
            pthread_cond_wait(&d->cond, &d->mtx);
    }
    pthread_mutex_unlock(&d->mtx);
}

astrodebug *astrodebug_get(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&s_dbg.mtx, NULL);
        pthread_cond_init(&s_dbg.cond, NULL);
        inited = 1;
    }
    return &s_dbg;
}

void astrodebug_set_engaged(astrodebug *d, int engaged)
{
    if (engaged == d->engaged)
        return;
    if (engaged) {
        d->mode = RUN;
        astro_host_set_instr_hook(hook_cb, d);
    } else {
        astrodebug_resume(d);           /* unpark before detaching */
        astro_host_set_instr_hook(NULL, NULL);
    }
    d->engaged = engaged;
}

int astrodebug_is_engaged(const astrodebug *d) { return d->engaged; }

void astrodebug_pause(astrodebug *d)
{
    pthread_mutex_lock(&d->mtx);
    if (d->mode != PAUSED)
        d->mode = PAUSE_REQ;
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_resume(astrodebug *d)
{
    pthread_mutex_lock(&d->mtx);
    d->mode = RUN;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_is_paused(const astrodebug *d)
{
    return d->mode == PAUSED;
}

void astrodebug_step(astrodebug *d)
{
    pthread_mutex_lock(&d->mtx);
    if (d->mode == PAUSED) {
        d->mode = STEP;
        pthread_cond_signal(&d->cond);
    }
    pthread_mutex_unlock(&d->mtx);
}

/* CALL, CALL cc, RST, and DJNZ push/loop -- step over them by breaking at the
 * following instruction. Everything else is a plain step. */
static int is_call_like(uint8_t op)
{
    if (op == 0xCD) return 1;                       /* CALL nn */
    if ((op & 0xC7) == 0xC4) return 1;              /* CALL cc,nn */
    if ((op & 0xC7) == 0xC7) return 1;              /* RST */
    if (op == 0x10) return 1;                       /* DJNZ */
    return 0;
}

void astrodebug_step_over(astrodebug *d)
{
    if (d->mode != PAUSED || !d->m) { astrodebug_step(d); return; }

    uint16_t pc = d->m->cpu.pc;
    uint8_t op = astro_machine_peek(d->m, pc);
    /* ED-prefixed block ops (LDIR/CPIR/...) re-fire opdone per iteration; a
     * plain step lands back on the same PC, so treat them like a call too. */
    int block = (op == 0xED);
    if (!is_call_like(op) && !block) { astrodebug_step(d); return; }

    astrodebug_dasm_line line;
    int n = astrodebug_disassemble(d, pc, &line, 1);
    uint16_t ret = (uint16_t)(pc + (n ? line.length : 1));

    pthread_mutex_lock(&d->mtx);
    d->has_oneshot = 1;
    d->oneshot = ret;
    d->has_sp_guard = 0;
    d->mode = RUN;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mtx);
}

void astrodebug_step_out(astrodebug *d)
{
    if (d->mode != PAUSED || !d->m) return;

    uint16_t sp = d->m->cpu.sp;
    uint16_t ret = (uint16_t)(astro_machine_peek(d->m, sp) |
                              (astro_machine_peek(d->m, (uint16_t)(sp + 1)) << 8));
    pthread_mutex_lock(&d->mtx);
    d->has_oneshot = 1;
    d->oneshot = ret;
    d->has_sp_guard = 1;
    d->sp_guard = (uint16_t)(sp + 2);   /* only once the frame has been popped */
    d->mode = RUN;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mtx);
}

uint64_t astrodebug_stop_serial(const astrodebug *d) { return d->stop_serial; }

int astrodebug_regs_get(astrodebug *d, astrodebug_regs *out)
{
    if (d->mode != PAUSED || !d->m)
        return 0;
    const z80_t *c = &d->m->cpu;
    out->af = c->af; out->bc = c->bc; out->de = c->de; out->hl = c->hl;
    out->af2 = c->af2; out->bc2 = c->bc2; out->de2 = c->de2; out->hl2 = c->hl2;
    out->ix = c->ix; out->iy = c->iy; out->sp = c->sp; out->pc = c->pc;
    out->wz = c->wz;
    out->i = c->i; out->r = c->r; out->im = c->im;
    out->iff1 = c->iff1; out->iff2 = c->iff2;
    out->halt = (c->pins & Z80_HALT) ? 1 : 0;
    return 1;
}

int astrodebug_read(astrodebug *d, uint16_t addr, uint8_t *dst, int n)
{
    if (d->mode != PAUSED || !d->m)
        return 0;
    for (int i = 0; i < n; i++)
        dst[i] = astro_machine_peek(d->m, (uint16_t)(addr + i));
    return n;
}

int astrodebug_write(astrodebug *d, uint16_t addr, const uint8_t *src, int n)
{
    if (d->mode != PAUSED || !d->m)
        return 0;
    for (int i = 0; i < n; i++)
        astro_machine_poke(d->m, (uint16_t)(addr + i), src[i]);
    return n;
}

/* ---- disassembly (chips z80dasm, callback-driven) ---- */

typedef struct {
    astrodebug *d;
    uint16_t addr;
    char *out;
    int out_len, out_pos;
} dasm_ctx;

static uint8_t dasm_in(void *user)
{
    dasm_ctx *c = user;
    uint8_t b = astro_machine_peek(c->d->m, c->addr++);
    return b;
}

static void dasm_out(char ch, void *user)
{
    dasm_ctx *c = user;
    if (c->out_pos < c->out_len - 1)
        c->out[c->out_pos++] = ch;
}

int astrodebug_disassemble(astrodebug *d, uint16_t addr,
                           astrodebug_dasm_line *out, int max)
{
    if (d->mode != PAUSED || !d->m)
        return 0;
    int produced = 0;
    uint16_t pc = addr;
    for (int i = 0; i < max; i++) {
        dasm_ctx c = { d, pc, out[i].text, (int)sizeof out[i].text, 0 };
        uint16_t next = z80dasm_op(pc, dasm_in, dasm_out, &c);
        c.out[c.out_pos] = '\0';
        out[i].addr = pc;
        out[i].length = (int)(uint16_t)(next - pc);
        if (out[i].length <= 0)
            out[i].length = 1;
        pc = next;
        produced++;
    }
    return produced;
}

int astrodebug_breakpoint_toggle(astrodebug *d, uint16_t addr)
{
    pthread_mutex_lock(&d->mtx);
    for (int i = 0; i < d->nbreak; i++)
        if (d->breakpoints[i] == addr) {
            d->breakpoints[i] = d->breakpoints[--d->nbreak];
            pthread_mutex_unlock(&d->mtx);
            return 0;
        }
    int rc = -1;
    if (d->nbreak < ASTRODEBUG_MAX_BREAKPOINTS) {
        d->breakpoints[d->nbreak++] = addr;
        rc = 1;
    }
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

int astrodebug_breakpoint_is_set(astrodebug *d, uint16_t addr)
{
    return bp_hit(d, addr);
}

void astrodebug_breakpoint_clear_all(astrodebug *d)
{
    pthread_mutex_lock(&d->mtx);
    d->nbreak = 0;
    pthread_mutex_unlock(&d->mtx);
}

int astrodebug_breakpoint_list(astrodebug *d, uint16_t *out, int max)
{
    int n = d->nbreak < max ? d->nbreak : max;
    /* simple insertion sort into out (ascending) */
    for (int i = 0; i < n; i++)
        out[i] = d->breakpoints[i];
    for (int i = 1; i < n; i++) {
        uint16_t v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}
