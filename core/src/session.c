/*
 * session.c -- the astrosession lifecycle, a thin wrapper over the emulator
 * host (core/astro/host.h), which already owns the machine thread, the frame
 * slot, the audio ring, and the reset latch. The session adds paths,
 * settings, BIOS resolution, cartridge selection, and input forwarding.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astro_internal.h"
#include "bindings.h"
#include "host.h"
#include "session_internal.h"

void session_set_error(struct astrosession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
    va_end(ap);
}

const char *astrosession_last_error(const astrosession *s)
{
    return s->last_error;
}

astrosession *astrosession_new(const astrosession_paths *paths)
{
    astrosession *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        free(s);
        return NULL;
    }
    settings_init(s);
    pthread_mutex_init(&s->sysact_mtx, NULL);
    bindings_init(s);
    roms_provision_embedded(s);
    snprintf(s->boip_hostport, sizeof s->boip_hostport, "127.0.0.1:%d",
             ASTROSESSION_BOIP_PORT);
    return s;
}

void astrosession_free(astrosession *s)
{
    if (!s) return;
    astrosession_stop(s);
    astrosession_settings_flush(s);
    settings_free_all(s);
    pthread_mutex_destroy(&s->sysact_mtx);
    free(s->bios);
    free(s);
}

/* ---- machine options ---- */

static const char *k_bios_names[] = {
    "Bally Professional Arcade (astro.bin)",
    "Bally Home Library Computer (ballyhlc.bin)",
    "Bally Computer System (bioswhit.bin)",
    NULL,
};
static const char *k_exp_names[] = {
    "None",
    "Blue RAM 4K", "Blue RAM 16K", "Blue RAM 32K",
    "Viper System 1 (16K)", "Lil' WHITE RAM (32K)", "R&L 64K",
    NULL,
};

const char *astrosession_bios_name(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof k_bios_names / sizeof *k_bios_names) - 1)
        return NULL;
    return k_bios_names[idx];
}

const char *astrosession_exp_name(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof k_exp_names / sizeof *k_exp_names) - 1)
        return NULL;
    return k_exp_names[idx];
}

void astrosession_default_opts(astrosession *s, astrosession_start_opts *opts)
{
    opts->bios = astrosession_get_int(s, "bios", ASTROSESSION_BIOS_ASTRO);
    opts->exp = astrosession_get_int(s, "exp", ASTROSESSION_EXP_NONE);
    const char *cart = astrosession_get_str(s, "cart", "");
    opts->cart_path = (cart && *cart) ? cart : NULL;
}

int astrosession_has_system_roms(const astrosession *s)
{
    return roms_any_available(s);
}

/* ---- lifecycle ---- */

/* session exp enum -> core exp enum (same order, but keep them decoupled) */
static astro_exp_t map_exp(int e)
{
    switch (e) {
    case ASTROSESSION_EXP_BLUE_RAM_4K:   return ASTRO_EXP_BLUE_RAM_4K;
    case ASTROSESSION_EXP_BLUE_RAM_16K:  return ASTRO_EXP_BLUE_RAM_16K;
    case ASTROSESSION_EXP_BLUE_RAM_32K:  return ASTRO_EXP_BLUE_RAM_32K;
    case ASTROSESSION_EXP_VIPER_SYS1:    return ASTRO_EXP_VIPER_SYS1;
    case ASTROSESSION_EXP_LIL_WHITE_RAM: return ASTRO_EXP_LIL_WHITE_RAM;
    case ASTROSESSION_EXP_RL64_RAM:      return ASTRO_EXP_RL64_RAM;
    default:                             return ASTRO_EXP_NONE;
    }
}

static uint8_t *read_whole_file(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size_out = (uint32_t)n;
    return buf;
}

int astrosession_start(astrosession *s, const astrosession_start_opts *opts)
{
    astrosession_start_opts local;
    if (!opts) {
        astrosession_default_opts(s, &local);
        opts = &local;
    }

    free(s->bios);
    s->bios = roms_load_bios(s, opts->bios);
    if (!s->bios)
        return -1;      /* error already set */

    uint8_t *cart = NULL;
    uint32_t cart_size = 0;
    if (opts->cart_path && *opts->cart_path) {
        cart = read_whole_file(opts->cart_path, &cart_size);
        if (!cart) {
            session_set_error(s, "cannot read cartridge %s", opts->cart_path);
            return -1;
        }
    }

    astro_host_opts_t hopts = {
        .bios = s->bios,
        .cart = cart,
        .cart_size = cart_size,
        .exp = map_exp(opts->exp),
        .boip_hostport = s->boip_hostport,
    };
    int rc = astro_host_start(&hopts);
    free(cart);                 /* astro_host_start copies it */
    if (rc != 0) {
        session_set_error(s, "%s", astro_host_last_error());
        return -1;
    }

    audio_start(s);             /* best-effort; silent if no device */
    s->running = 1;
    return 0;
}

void astrosession_stop(astrosession *s)
{
    if (!s->running)
        return;
    audio_stop(s);
    astro_host_stop();
    s->running = 0;
}

int astrosession_is_running(const astrosession *s)
{
    return s->running && astro_host_is_running();
}

int astrosession_load_cart(astrosession *s, const char *path)
{
    astrosession_stop(s);
    astrosession_set_str(s, "cart", (path && *path) ? path : "");
    astrosession_settings_flush(s);
    return astrosession_start(s, NULL);
}

const char *astrosession_cart_path(astrosession *s)
{
    return astrosession_get_str(s, "cart", "");
}

const char *astrosession_fujinet_webui_url(astrosession *s)
{
    (void)s;
    static const char url[] = "http://127.0.0.1:11501/";
    /* ASTROSESSION_WEBUI_PORT is 11501; kept as a literal so the string is
     * constant, with a compile-time check that they agree. */
    _Static_assert(ASTROSESSION_WEBUI_PORT == 11501, "webui url/port mismatch");
    return url;
}

int astrosession_reset_to_config(astrosession *s)
{
    return astrosession_load_cart(s, NULL);
}

int astrosession_reset_game(astrosession *s)
{
    if (!astrosession_is_running(s))
        return 0;
    astro_host_keypad_release_all();
    astro_host_reset();
    return 0;
}

/* ---- video / audio pulls ---- */

int astrosession_copy_frame(astrosession *s, uint32_t *dst, uint64_t *serial_inout)
{
    (void)s;
    return astro_host_frame_copy(dst, serial_inout);
}

int astrosession_render_audio(astrosession *s, int16_t *dst, int max_samples)
{
    (void)s;
    return astro_host_audio_copy(dst, max_samples);
}

/* ---- input ---- */

void astrosession_keypad_set(astrosession *s, astrosession_key key, int pressed)
{
    (void)s;
    if (key < 0 || key >= ASTROSESSION_KEY_COUNT)
        return;
    /* the enum is row-major from the top-left: col = key % 4, row = key / 4 */
    const int col = key % 4;
    const int row = key / 4;
    astro_host_keypad_set(col, row, pressed != 0);
}

void astrosession_handle_set(astrosession *s, int player, uint8_t mask)
{
    (void)s;
    astro_host_handle_set(player, mask);
}

void astrosession_knob_set(astrosession *s, int player, uint8_t value)
{
    (void)s;
    astro_host_knob_set(player, value);
}

/* ---- system actions ---- */

void astrosession_sysaction_fire(astrosession *s, astrosession_sysaction a)
{
    switch (a) {
    case ASTROSESSION_SYSACT_RESET_GAME:   astrosession_reset_game(s); break;
    case ASTROSESSION_SYSACT_RESET_CONFIG: astrosession_reset_to_config(s); break;
    default: break;
    }
}

void astrosession_sysaction_post(astrosession *s, astrosession_sysaction a)
{
    if (a < 0 || a >= ASTROSESSION_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&s->sysact_mtx);
    s->sysact_pending |= (1u << a);
    pthread_mutex_unlock(&s->sysact_mtx);
}

unsigned astrosession_sysaction_take(astrosession *s)
{
    pthread_mutex_lock(&s->sysact_mtx);
    unsigned p = s->sysact_pending;
    s->sysact_pending = 0;
    pthread_mutex_unlock(&s->sysact_mtx);
    return p;
}
