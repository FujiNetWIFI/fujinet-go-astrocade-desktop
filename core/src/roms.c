/*
 * roms.c -- Bally system BIOS resolution and import.
 *
 * The core takes the BIOS as a plain buffer (astro_host_opts.bios). This
 * resolves that buffer from the ROM directory (a file the user imported) or,
 * failing that, from the WITH_ASTROCADE_ROMS embedded table -- and imports
 * what the user points at (single images, a zip such as MAME's astrocde set,
 * or a folder), recognising the three known BIOS dumps by size and CRC32
 * whatever they are called, so a wrong file is left alone rather than
 * half-booting. See COMPLIANCE.md.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "astro_internal.h"    /* ASTRO_BIOS_SIZE */
#include "roms_embedded.h"
#include "session_internal.h"
#include "zipread.h"

/* The three console dumps (MAME astrohome.cpp ROM defs), index-aligned with
 * ASTROSESSION_BIOS_*: item i of the ROMs list is BIOS variant i. */
static const struct {
    const char *name;           /* the file in the ROM directory */
    uint32_t crc32;
    astro_rom_item item;
} k_bios[] = {
    { "astro.bin",    0xebc77f3au,
      { "Bally Professional Arcade BIOS (astro.bin)", "BIOS" } },
    { "ballyhlc.bin", 0xd7c517bau,
      { "Bally Home Library Computer BIOS (ballyhlc.bin)", "BIOS" } },
    { "bioswhit.bin", 0x6eb53e79u,
      { "Bally Computer System BIOS (bioswhit.bin)", "BIOS" } },
};
#define N_BIOS ((int)(sizeof k_bios / sizeof k_bios[0]))

/* CRC32 (IEEE, the zip/MAME polynomial), table-free. */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

static uint8_t *read_file(const char *path, size_t want, size_t *got)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = malloc(want);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, want, f);
    /* reject anything longer than expected too */
    uint8_t extra;
    int longer = (fread(&extra, 1, 1, f) == 1);
    fclose(f);
    if (longer) { free(buf); return NULL; }
    *got = n;
    return buf;
}

static const astro_embedded_rom *embedded(const char *name)
{
    for (int i = 0; i < astro_embedded_rom_count; i++)
        if (strcmp(astro_embedded_roms[i].name, name) == 0)
            return &astro_embedded_roms[i];
    return NULL;
}

/* ---- the ROMs list ---- */

int astrosession_rom_item_count(void)
{
    return N_BIOS;
}

const astro_rom_item *astrosession_rom_item(int i)
{
    return i >= 0 && i < N_BIOS ? &k_bios[i].item : NULL;
}

int astrosession_rom_item_available(const astrosession *s, int i)
{
    if (i < 0 || i >= N_BIOS)
        return 0;
    const astro_embedded_rom *e = embedded(k_bios[i].name);
    if (e && e->size == ASTRO_BIOS_SIZE)
        return 1;
    char path[ASTRO_PATH_MAX + 32];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", s->roms_dir, k_bios[i].name);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == ASTRO_BIOS_SIZE;
}

const char *astrosession_roms_path(const astrosession *s)
{
    return s->roms_dir;
}

int astrosession_resolve_bios(const astrosession *s, int wanted)
{
    if (astrosession_rom_item_available(s, wanted))
        return wanted;
    for (int i = 0; i < N_BIOS; i++)
        if (astrosession_rom_item_available(s, i))
            return i;
    return -1;
}

int roms_any_available(const struct astrosession *s)
{
    return astrosession_resolve_bios(s, ASTROSESSION_BIOS_ASTRO) >= 0;
}

uint8_t *roms_load_bios(struct astrosession *s, int variant)
{
    if (variant < 0 || variant >= N_BIOS)
        variant = ASTROSESSION_BIOS_ASTRO;
    const char *name = k_bios[variant].name;
    char path[ASTRO_PATH_MAX + 32];
    size_t got = 0;

    /* prefer a file in the ROM dir (imported by the user) */
    snprintf(path, sizeof path, "%s/%s", s->roms_dir, name);
    uint8_t *buf = read_file(path, ASTRO_BIOS_SIZE, &got);
    if (buf && got == ASTRO_BIOS_SIZE)
        return buf;
    free(buf);

    /* else the embedded copy (WITH_ASTROCADE_ROMS=ON dev build) */
    const astro_embedded_rom *e = embedded(name);
    if (e && e->size == ASTRO_BIOS_SIZE) {
        uint8_t *b = malloc(ASTRO_BIOS_SIZE);
        if (b) memcpy(b, e->data, ASTRO_BIOS_SIZE);
        return b;
    }

    session_set_error(s, "No Astrocade BIOS yet: use Machine > Import BIOS");
    return NULL;
}

void roms_provision_embedded(struct astrosession *s)
{
    /* Materialise each embedded BIOS into the ROM dir on first run, so the
     * dir the ROMs page shows already has them and a later
     * WITH_ASTROCADE_ROMS=OFF build of the same app keeps working. */
    for (int i = 0; i < astro_embedded_rom_count; i++) {
        const astro_embedded_rom *e = &astro_embedded_roms[i];
        char path[ASTRO_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/%s", s->roms_dir, e->name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); continue; }        /* already there */
        f = fopen(path, "wb");
        if (f) { fwrite(e->data, 1, e->size, f); fclose(f); }
    }
}

/* ---- import: a file, a zip or a folder ---- */

typedef struct {
    astrosession *s;
    int imported;               /* files written */
    int got[N_BIOS];            /* which BIOS this import supplied */
} import_ctx;

/* One candidate image: every BIOS with its CRC is written under its own
 * name, whatever the file was called. */
static void take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    if (size != ASTRO_BIOS_SIZE)
        return;
    uint32_t crc = crc32_of(data, size);
    for (int i = 0; i < N_BIOS; i++) {
        char path[ASTRO_PATH_MAX + 32];
        FILE *f;
        if (k_bios[i].crc32 != crc)
            continue;
        snprintf(path, sizeof path, "%s/%s", ctx->s->roms_dir, k_bios[i].name);
        f = fopen(path, "wb");
        if (!f)
            continue;
        if (fwrite(data, 1, size, f) == size && !ctx->got[i]) {
            ctx->got[i] = 1;
            ctx->imported++;
        }
        fclose(f);
    }
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void import_zip(import_ctx *ctx, zipread *z)
{
    for (int i = 0; i < zipread_count(z); i++) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        if (zipread_size(z, i) != ASTRO_BIOS_SIZE)
            continue;
        if (zipread_extract(z, i, &data, &size, ASTRO_BIOS_SIZE) == 0) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static void import_dir(import_ctx *ctx, const char *dir, int depth)
{
    char path[ASTRO_PATH_MAX * 2];
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
}

static void import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;
    if (depth > 8 || stat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        import_dir(ctx, path, depth);
        return;
    }
    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z);
        zipread_close(z);
        return;
    }
    if (st.st_size == ASTRO_BIOS_SIZE) {
        size_t got = 0;
        uint8_t *data = read_file(path, ASTRO_BIOS_SIZE, &got);
        if (data && got == ASTRO_BIOS_SIZE)
            take(ctx, data, (uint32_t)got);
        free(data);
    }
}

int astrosession_rom_import(astrosession *s, const char *path, char *msg, int msgsz)
{
    import_ctx ctx;
    int before[N_BIOS];
    int len = 0;

    memset(&ctx, 0, sizeof ctx);
    ctx.s = s;
    for (int i = 0; i < N_BIOS; i++)
        before[i] = astrosession_rom_item_available(s, i);
    if (path && *path)
        import_path(&ctx, path, 0);

    if (msg && msgsz > 0) {
        msg[0] = '\0';
        for (int i = 0; i < N_BIOS; i++) {
            if (!before[i] && astrosession_rom_item_available(s, i))
                len += snprintf(msg + len, (size_t)(msgsz > len ? msgsz - len : 0), "%s%s",
                                len ? ", " : "Now available: ", k_bios[i].item.name);
        }
        if (!len) {
            if (ctx.imported)
                snprintf(msg, (size_t)msgsz,
                         "Imported %d file%s; %s already available", ctx.imported,
                         ctx.imported == 1 ? "" : "s", ctx.imported == 1 ? "it was" : "each was");
            else
                snprintf(msg, (size_t)msgsz, "No Astrocade BIOS was found in %s",
                         path ? path : "");
        }
    }
    return ctx.imported > 0 ? 0 : -1;
}
