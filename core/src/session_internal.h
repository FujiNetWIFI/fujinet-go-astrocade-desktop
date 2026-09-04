/*
 * astrosession's private state. Not installed; only session.c, settings.c,
 * paths.c and roms.c include it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTRO_SESSION_INTERNAL_H
#define ASTRO_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "astrosession.h"

#define ASTRO_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct astrosession {
    char config_dir[ASTRO_PATH_MAX];
    char data_dir[ASTRO_PATH_MAX];
    char roms_dir[ASTRO_PATH_MAX];
    char settings_file[ASTRO_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char boip_hostport[64];   /* "127.0.0.1:11500" -- handed to the cart */
    char last_error[256];

    /* cross-thread system-action latch (see astrosession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the loaded BIOS, resolved at start from the ROM dir or the embedded
     * table; kept so a reset/reload need not re-read it */
    uint8_t *bios;            /* ASTRO_BIOS_SIZE bytes, or NULL */

    void *audio;              /* audio_sdl.c state, NULL until started */
    int running;
};

void settings_init(struct astrosession *s);
void settings_free_all(struct astrosession *s);

int paths_init(struct astrosession *s, const char *config_dir,
               const char *data_dir);

void session_set_error(struct astrosession *s, const char *fmt, ...);

/* roms.c -- BIOS resolution and import. */
/* Loads the requested BIOS variant into a freshly malloc'd ASTRO_BIOS_SIZE
 * buffer: first from the ROM directory (by canonical filename), else from the
 * embedded table (WITH_ASTROCADE_ROMS). Returns the buffer (caller frees) or
 * NULL with the error set. */
uint8_t *roms_load_bios(struct astrosession *s, int variant);
/* 1 if any known BIOS is available (ROM dir or embedded). */
int roms_any_available(const struct astrosession *s);
/* Materialise any embedded BIOS into the ROM directory on first run. */
void roms_provision_embedded(struct astrosession *s);

/* audio_sdl.c */
int  audio_start(struct astrosession *s);
void audio_stop(struct astrosession *s);

#endif /* ASTRO_SESSION_INTERNAL_H */
