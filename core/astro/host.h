/*
 * host.h -- the emulator host: owns the machine, runs it on its own thread
 * at real speed, and publishes video frames and audio for the session layer.
 *
 * Process-singleton by design, like every core in this family (and the
 * FujiNet cart below it is one too: fujimail's port interface is context-
 * free C function pointers). astro_host_start refuses a second concurrent
 * session.
 *
 * Pacing: the machine is advanced a whole frame (262 scanlines, 119210
 * dots) at a time against an absolute monotonic deadline ladder at the
 * exact frame rate (7159090.5 Hz dot clock -> ~60.054 Hz). The ladder
 * resyncs when the host falls more than a few frames behind, so a laptop
 * resume does not fast-forward. The sibling ports' hardest-won lesson
 * applies here too: VERIFY the pacing (the INTV port once ran at >33000%
 * because a null platform layer defaulted its throttle off) -- the
 * boot_smoke test measures it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_HOST_H
#define ASTRO_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "astro_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *bios;        /* exactly ASTRO_BIOS_SIZE bytes, copied */
    const uint8_t *cart;        /* cart image, copied; NULL boots the baked
                                 * FujiNet CONFIG client */
    uint32_t cart_size;
    astro_exp_t exp;            /* RAM expansion */
    const char *boip_hostport;  /* fujinet-pc BoIP listener, "host:port";
                                 * NULL keeps fujitcp's own default chain */
} astro_host_opts_t;

/* Start the machine thread. Returns 0, or -1 with astro_host_last_error()
 * set (already running, allocation failure, thread failure). */
int astro_host_start(const astro_host_opts_t *opts);

/* Stop and join the machine thread and tear the cart down. Safe when not
 * running. */
void astro_host_stop(void);

bool astro_host_is_running(void);
const char *astro_host_last_error(void);

/* Console RESET, the panel button: a one-shot latch the machine thread
 * applies at the next frame boundary -- CPU and sound restart, screen RAM,
 * video registers and the whole cart device (live/staged images, mailbox)
 * survive, exactly as on hardware, where the cart edge has no reset line.
 * Cheap and non-blocking; callable from any thread. */
void astro_host_reset(void);

/* ---- video: latest-wins single slot ----
 * Copies the newest frame into dst (ASTRO_FB_WIDTH*ASTRO_FB_HEIGHT XRGB
 * words) when its serial differs from *serial_inout; returns nonzero and
 * updates the serial when it copied. */
int astro_host_frame_copy(uint32_t *dst, uint64_t *serial_inout);

/* ---- audio: 48 kHz mono int16 ring with self-correcting backlog ---- */
int astro_host_audio_copy(int16_t *dst, int max_samples);
void astro_host_audio_reset(void);

/* ---- inputs (callable from any thread; single-byte stores) ----
 * col: 0 = leftmost keypad column (C/MR/7/4/1/CE) .. 3 = rightmost gold
 * column (%,division,multiply,minus,plus,equals); row: 0 = top .. 5 =
 * bottom. The core maps col to the console's reversed port order (the
 * leftmost column reads at 0x17). */
void astro_host_keypad_set(int col, int row, bool down);
void astro_host_keypad_release_all(void);

#define ASTRO_HANDLE_UP      0x01   /* bus/astrocde/joy.cpp, active high */
#define ASTRO_HANDLE_DOWN    0x02
#define ASTRO_HANDLE_LEFT    0x04
#define ASTRO_HANDLE_RIGHT   0x08
#define ASTRO_HANDLE_TRIGGER 0x10
void astro_host_handle_set(int player, uint8_t mask);

/* Knob position 0-255 in USER terms (turning right increases); the core
 * applies the hardware inversion (MAME's PORT_INVERT). */
void astro_host_knob_set(int player, uint8_t value);

/* ---- debugger plumbing ----
 * The live machine; valid only while running, and only safe to inspect
 * while the debugger holds the machine paused inside the instruction hook
 * (core/debugger/debugger.c owns that discipline). */
astro_machine_t *astro_host_machine(void);

/* Instruction-boundary hook (fires on the machine thread; may block --
 * that is how the debugger pauses the machine). */
void astro_host_set_instr_hook(void (*hook)(astro_machine_t *m, void *user),
                               void *user);

#ifdef __cplusplus
}
#endif

#endif /* ASTRO_HOST_H */
