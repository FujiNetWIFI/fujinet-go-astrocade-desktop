/*
 * astrosession path layout: XDG config/data directories and the ROM
 * directory. Transposed from the Intv port's paths.c (same author, same
 * license), trimmed to what the Astrocade session needs -- the FujiNet
 * runtime library resolution and its data-tree provisioning land with the
 * FujiNet milestone (see cmake/FujiNetRuntime.cmake).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#endif

static int is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int make_dir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int mkdir_p(const char *path)
{
    char buf[ASTRO_PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (is_sep(*p)) {
            char save = *p;
            *p = '\0';
            if (make_dir(buf) != 0 && errno != EEXIST) return -1;
            *p = save;
        }
    }
    if (make_dir(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Per-user config/data directory. On Windows this is %APPDATA% (config) or
 * %LOCALAPPDATA% (data); elsewhere the XDG variable, then $HOME/suffix. */
static void default_dir(char *dst, size_t dstsz, const char *xdg_env,
                        const char *win_env, const char *home_suffix)
{
#if defined(_WIN32)
    const char *v = getenv(win_env);
    (void)xdg_env;
    (void)home_suffix;
    snprintf(dst, dstsz, "%s\\fujinet-go-astrocade", (v && *v) ? v : ".");
#else
    const char *v = getenv(xdg_env);
    (void)win_env;
    if (v && *v) {
        snprintf(dst, dstsz, "%s/fujinet-go-astrocade", v);
    } else {
        const char *home = getenv("HOME");
        snprintf(dst, dstsz, "%s/%s/fujinet-go-astrocade", home ? home : ".",
                 home_suffix);
    }
#endif
}

int paths_init(struct astrosession *s, const char *config_dir,
               const char *data_dir)
{
    if (config_dir && *config_dir)
        snprintf(s->config_dir, sizeof s->config_dir, "%s", config_dir);
    else
        default_dir(s->config_dir, sizeof s->config_dir,
                    "XDG_CONFIG_HOME", "APPDATA", ".config");

    if (data_dir && *data_dir)
        snprintf(s->data_dir, sizeof s->data_dir, "%s", data_dir);
    else
        default_dir(s->data_dir, sizeof s->data_dir,
                    "XDG_DATA_HOME", "LOCALAPPDATA", ".local/share");

    /* INTV_ROM_DIR-style override so a dev build can point at an existing
     * ROM stash without copying it into the data dir. */
    const char *rom_override = getenv("ASTRO_ROM_DIR");
    if (rom_override && *rom_override)
        snprintf(s->roms_dir, sizeof s->roms_dir, "%s", rom_override);
    else
        snprintf(s->roms_dir, sizeof s->roms_dir, "%s/roms", s->data_dir);

    snprintf(s->settings_file, sizeof s->settings_file, "%s/settings.ini",
             s->config_dir);

    if (mkdir_p(s->config_dir) != 0) {
        session_set_error(s, "cannot create config dir %s", s->config_dir);
        return -1;
    }
    if (mkdir_p(s->data_dir) != 0) {
        session_set_error(s, "cannot create data dir %s", s->data_dir);
        return -1;
    }
    if (mkdir_p(s->roms_dir) != 0) {
        session_set_error(s, "cannot create ROM dir %s", s->roms_dir);
        return -1;
    }
    return 0;
}
