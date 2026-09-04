/*
 * roms.c -- Bally system BIOS resolution and import.
 *
 * The core takes the BIOS as a plain buffer (astro_host_opts.bios). This
 * resolves that buffer from the ROM directory (a file the user imported) or,
 * failing that, from the WITH_ASTROCADE_ROMS embedded table -- and imports a
 * user-chosen file by identifying it against the three known BIOS dumps by
 * size and CRC32, so a wrong file is rejected rather than half-booting. See
 * COMPLIANCE.md.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astro_internal.h"    /* ASTRO_BIOS_SIZE */
#include "roms_embedded.h"
#include "session_internal.h"

/* The three console dumps (MAME astrohome.cpp ROM defs). */
static const struct {
    const char *name;
    uint32_t crc32;
    int variant;
} k_bios[] = {
    { "astro.bin",    0xebc77f3au, ASTROSESSION_BIOS_ASTRO },
    { "ballyhlc.bin", 0xd7c517bau, ASTROSESSION_BIOS_BALLYHLC },
    { "bioswhit.bin", 0x6eb53e79u, ASTROSESSION_BIOS_BIOSWHIT },
};
#define N_BIOS ((int)(sizeof k_bios / sizeof k_bios[0]))

static const char *variant_name(int variant)
{
    for (int i = 0; i < N_BIOS; i++)
        if (k_bios[i].variant == variant)
            return k_bios[i].name;
    return k_bios[0].name;
}

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

int roms_any_available(const struct astrosession *s)
{
    if (astro_embedded_rom_count > 0)
        return 1;
    for (int i = 0; i < N_BIOS; i++) {
        char path[ASTRO_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/%s", s->roms_dir, k_bios[i].name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

uint8_t *roms_load_bios(struct astrosession *s, int variant)
{
    const char *name = variant_name(variant);
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

    session_set_error(s, "no %s BIOS found -- import one via the ROM menu", name);
    return NULL;
}

void roms_provision_embedded(struct astrosession *s)
{
    /* Materialise each embedded BIOS into the ROM dir on first run, so the
     * dir the "Import System ROMs..." UI shows already has them and a later
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

int astrosession_import_rom(astrosession *s, const char *path,
                            char *name_out, int name_out_len)
{
    size_t got = 0;
    uint8_t *buf = read_file(path, ASTRO_BIOS_SIZE, &got);
    if (!buf || got != ASTRO_BIOS_SIZE) {
        free(buf);
        session_set_error(s, "%s is not an 8192-byte Astrocade BIOS", path);
        return -1;
    }

    uint32_t crc = crc32_of(buf, ASTRO_BIOS_SIZE);
    const char *name = NULL;
    for (int i = 0; i < N_BIOS; i++)
        if (k_bios[i].crc32 == crc) { name = k_bios[i].name; break; }
    if (!name) {
        free(buf);
        session_set_error(s, "%s is 8K but not a recognized Astrocade BIOS "
                          "(crc %08x)", path, crc);
        return -1;
    }

    char dst[ASTRO_PATH_MAX + 32];
    snprintf(dst, sizeof dst, "%s/%s", s->roms_dir, name);
    FILE *f = fopen(dst, "wb");
    if (!f) {
        free(buf);
        session_set_error(s, "cannot write %s", dst);
        return -1;
    }
    fwrite(buf, 1, ASTRO_BIOS_SIZE, f);
    fclose(f);
    free(buf);

    if (name_out && name_out_len > 0)
        snprintf(name_out, name_out_len, "%s", name);
    return 0;
}
