/*
 * host.c -- see host.h.
 *
 * The frame slot and audio ring transpose the INTV port's intv_frame.c /
 * intv_audio.c (same author, same license): latest-wins video with a
 * serial, and an audio ring that trims all the way down to a small fixed
 * cushion when the consumer lags -- trimming only to the trigger point was
 * tried there first and is wrong (it still hands back stale data).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "host.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the machine and its thread ---- */

static astro_machine_t s_machine;
static pthread_t s_thread;
static atomic_bool s_running = false;
static atomic_bool s_stop_req = false;
static atomic_bool s_reset_req = false;
static char s_error[256];

static uint8_t s_bios[ASTRO_BIOS_SIZE];
static uint8_t *s_cart_image = NULL;
static uint32_t s_cart_size = 0;
static astro_exp_t s_exp = ASTRO_EXP_NONE;
static char s_boip[128];
static bool s_have_boip = false;

/* ---- frame slot ---- */

static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_frame[ASTRO_FB_WIDTH * ASTRO_FB_HEIGHT];
static uint64_t s_frame_serial = 0;

static void frame_publish(const uint32_t *fb)
{
    pthread_mutex_lock(&s_frame_lock);
    memcpy(s_frame, fb, sizeof s_frame);
    s_frame_serial++;
    pthread_mutex_unlock(&s_frame_lock);
}

int astro_host_frame_copy(uint32_t *dst, uint64_t *serial_inout)
{
    int changed;

    pthread_mutex_lock(&s_frame_lock);
    changed = (*serial_inout != s_frame_serial);
    if (changed)
    {
        memcpy(dst, s_frame, sizeof s_frame);
        *serial_inout = s_frame_serial;
    }
    pthread_mutex_unlock(&s_frame_lock);
    return changed;
}

/* ---- audio ring (see intv_audio.c's LATENCY/PRIMING notes) ---- */

#define RING_CAPACITY          (ASTRO_AUDIO_RATE / 2)
#define RING_TARGET_SAMPLES    (ASTRO_AUDIO_RATE / 33)
#define RING_HIGHWATER_SAMPLES (ASTRO_AUDIO_RATE / 10)

static pthread_mutex_t s_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t s_ring[RING_CAPACITY];
static int s_head = 0;
static int s_count = 0;
static int s_primed = 0;

static void audio_publish(const int16_t *samples, int count)
{
    if (count <= 0)
        return;
    if (count > RING_CAPACITY)
    {
        samples += (count - RING_CAPACITY);
        count = RING_CAPACITY;
    }

    pthread_mutex_lock(&s_audio_lock);
    for (int i = 0; i < count; i++)
    {
        s_ring[s_head] = samples[i];
        s_head = (s_head + 1) % RING_CAPACITY;
    }
    s_count += count;
    if (s_count > RING_CAPACITY)
        s_count = RING_CAPACITY;
    pthread_mutex_unlock(&s_audio_lock);
}

int astro_host_audio_copy(int16_t *dst, int max_samples)
{
    pthread_mutex_lock(&s_audio_lock);

    /* Catch up on lag before copying anything: trim to a small fixed
     * cushion of RECENT audio, never scaled by the caller's request. */
    if (s_count > RING_HIGHWATER_SAMPLES)
        s_count = RING_TARGET_SAMPLES;

    /* Prime gate: no output until a target's worth has accumulated, so a
     * production burst does not dribble out as several under-filled
     * requests. Primed until the ring runs completely dry. */
    if (!s_primed)
    {
        if (s_count < RING_TARGET_SAMPLES)
        {
            pthread_mutex_unlock(&s_audio_lock);
            return 0;
        }
        s_primed = 1;
    }

    const int n = (max_samples < s_count) ? max_samples : s_count;
    int read_pos = ((s_head - s_count) % RING_CAPACITY + RING_CAPACITY)
                   % RING_CAPACITY;
    for (int i = 0; i < n; i++)
    {
        dst[i] = s_ring[read_pos];
        read_pos = (read_pos + 1) % RING_CAPACITY;
    }
    s_count -= n;
    if (s_count == 0)
        s_primed = 0;
    pthread_mutex_unlock(&s_audio_lock);
    return n;
}

void astro_host_audio_reset(void)
{
    pthread_mutex_lock(&s_audio_lock);
    s_head = 0;
    s_count = 0;
    s_primed = 0;
    pthread_mutex_unlock(&s_audio_lock);
}

/* ---- inputs ---- */

void astro_host_keypad_set(int col, int row, bool down)
{
    if (col < 0 || col > 3 || row < 0 || row > 5)
        return;
    /* the leftmost physical column reads at port 0x17 = keypad[3] */
    const int idx = 3 - col;
    const uint8_t bit = (uint8_t)(1u << row);
    if (down)
        s_machine.keypad[idx] |= bit;
    else
        s_machine.keypad[idx] &= (uint8_t)~bit;
}

void astro_host_keypad_release_all(void)
{
    memset(s_machine.keypad, 0, sizeof s_machine.keypad);
    memset(s_machine.handle, 0, sizeof s_machine.handle);
}

void astro_host_handle_set(int player, uint8_t mask)
{
    if (player < 0 || player > 3)
        return;
    s_machine.handle[player] = mask & 0x1f;
}

void astro_host_knob_set(int player, uint8_t value)
{
    if (player < 0 || player > 3)
        return;
    /* MAME's knob input is PORT_INVERT: the console reads the complement */
    s_machine.knob[player] = (uint8_t)~value;
}

/* ---- debugger plumbing ---- */

astro_machine_t *astro_host_machine(void)
{
    return &s_machine;
}

void astro_host_set_instr_hook(void (*hook)(astro_machine_t *m, void *user),
                               void *user)
{
    s_machine.instr_hook_user = user;
    s_machine.instr_hook = hook;
}

/* ---- the machine thread ---- */

static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L)
    {
        t->tv_nsec -= 1000000000L;
        t->tv_sec += 1;
    }
}

static long ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

/* Sleep until the absolute deadline `next` (with `now` already sampled).
 * clock_nanosleep(TIMER_ABSTIME) is the right tool on POSIX, but it is a
 * no-op under mingw/Wine (the emulator then free-ran at thousands of fps in
 * the Windows build -- the sibling ports' "verify the throttle per platform"
 * lesson), so Windows uses a plain relative nanosleep, which winpthreads
 * honours. Darwin has no clock_nanosleep at all (it's a glibc/POSIX.1b
 * extension the BSD-derived libc never picked up), so macOS takes the same
 * relative-nanosleep path. */
static void sleep_until(const struct timespec *next, const struct timespec *now)
{
#if defined(_WIN32) || defined(__APPLE__)
    long remain = ts_diff_ns(next, now);
    if (remain <= 0)
        return;
    struct timespec rel = { remain / 1000000000L, remain % 1000000000L };
    nanosleep(&rel, NULL);
#else
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
    (void)now;
#endif
}

static void *machine_thread(void *arg)
{
    (void)arg;

    /* the exact frame period from the exact dot clock */
    const long frame_ns =
        (long)(1e9 * (double)ASTRO_DOTS_PER_FRAME / ASTRO_DOT_CLOCK_HZ + 0.5);

    /* Bring the cart up here rather than on the caller's thread: the TCP
     * connect to the (local) BoIP listener is quick but not free, and a
     * failed link must not stall the UI -- the mailbox runs link-down and
     * the CONFIG client reports it. */
    astro_cart_start(&s_machine.cart, s_cart_image, s_cart_size,
                     s_have_boip ? s_boip : NULL);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!atomic_load(&s_stop_req))
    {
        if (atomic_exchange(&s_reset_req, false))
        {
            astro_machine_reset(&s_machine);
            astro_host_audio_reset();
        }

        for (int line = 0; line < ASTRO_LINES_PER_FRAME; line++)
            astro_machine_run_scanline(&s_machine);

        frame_publish(s_machine.fb);
        audio_publish(s_machine.snd.out, s_machine.snd.out_count);
        s_machine.snd.out_count = 0;

        /* absolute deadline ladder; resync when badly behind (laptop
         * resume, debugger pause) instead of fast-forwarding */
        add_ns(&next, frame_ns);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long behind = ts_diff_ns(&now, &next);
        if (behind > 4 * frame_ns)
            next = now;
        else if (behind < 0)
            sleep_until(&next, &now);
    }

    return NULL;
}

/* ---- lifecycle ---- */

const char *astro_host_last_error(void)
{
    return s_error;
}

bool astro_host_is_running(void)
{
    return atomic_load(&s_running);
}

void astro_host_reset(void)
{
    if (atomic_load(&s_running))
        atomic_store(&s_reset_req, true);
}

int astro_host_start(const astro_host_opts_t *opts)
{
    if (atomic_load(&s_running))
    {
        snprintf(s_error, sizeof s_error, "a session is already running");
        return -1;
    }
    if (!opts || !opts->bios)
    {
        snprintf(s_error, sizeof s_error, "no BIOS image");
        return -1;
    }

    memcpy(s_bios, opts->bios, ASTRO_BIOS_SIZE);
    free(s_cart_image);
    s_cart_image = NULL;
    s_cart_size = 0;
    if (opts->cart && opts->cart_size)
    {
        s_cart_image = malloc(opts->cart_size);
        if (!s_cart_image)
        {
            snprintf(s_error, sizeof s_error, "out of memory");
            return -1;
        }
        memcpy(s_cart_image, opts->cart, opts->cart_size);
        s_cart_size = opts->cart_size;
    }
    s_exp = opts->exp;
    s_have_boip = (opts->boip_hostport != NULL);
    if (s_have_boip)
        snprintf(s_boip, sizeof s_boip, "%s", opts->boip_hostport);

    astro_machine_init(&s_machine, s_bios, s_exp);
    astro_host_audio_reset();

    atomic_store(&s_stop_req, false);
    atomic_store(&s_reset_req, false);
    if (pthread_create(&s_thread, NULL, machine_thread, NULL) != 0)
    {
        snprintf(s_error, sizeof s_error, "cannot start the machine thread");
        astro_machine_free(&s_machine);
        return -1;
    }
    atomic_store(&s_running, true);
    return 0;
}

void astro_host_stop(void)
{
    if (!atomic_load(&s_running))
        return;
    atomic_store(&s_stop_req, true);
    pthread_join(s_thread, NULL);
    atomic_store(&s_running, false);
    astro_machine_free(&s_machine);   /* also tears the cart down */
    free(s_cart_image);
    s_cart_image = NULL;
    s_cart_size = 0;
    s_frame_serial = 0;
}
