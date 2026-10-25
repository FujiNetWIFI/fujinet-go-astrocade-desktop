/*
 * rom_import_test -- BIOS import from a file, a zip and a folder, without a
 * Bally ROM: each image is 8 KB of filler whose last four bytes are forged
 * so its CRC32 is a real BIOS's (CRC32 is linear, so four bytes reach any
 * value). zlib's own crc32() checks the forgery.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "astrosession.h"
#include "test_tmpdir.h"

#define BIOS_SIZE 8192

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static const uint32_t k_crc[3] = { 0xebc77f3au, 0xd7c517bau, 0x6eb53e79u };

static uint32_t T[256];
static uint8_t inv[256];        /* T's top bytes are a permutation */

static void crc_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
        T[i] = c;
        inv[c >> 24] = (uint8_t)i;
    }
}

/* 8 KB of filler (seeded) ending in the four bytes that make its CRC32
 * `want`: pick the four table entries whose sum is the final register, then
 * the bytes that select them from the register the filler leaves. */
static void forge(uint8_t *buf, uint32_t seed, uint32_t want)
{
    uint32_t r = 0xffffffffu, v = ~want;
    uint8_t idx[4];
    for (int i = 0; i < BIOS_SIZE - 4; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
        r = (r >> 8) ^ T[(r ^ buf[i]) & 0xff];
    }
    idx[3] = inv[v >> 24];         v ^= T[idx[3]];
    idx[2] = inv[(v >> 16) & 0xff]; v ^= T[idx[2]] >> 8;
    idx[1] = inv[(v >> 8) & 0xff];  v ^= T[idx[1]] >> 16;
    idx[0] = inv[v & 0xff];
    for (int k = 0; k < 4; k++) {
        buf[BIOS_SIZE - 4 + k] = (uint8_t)(idx[k] ^ (r & 0xff));
        r = (r >> 8) ^ T[idx[k]];
    }
}

static void write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void make_dir(const char *path)
{
#if defined(_WIN32)
    mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

typedef struct { const char *name; const uint8_t *data; uint32_t size; int deflate; } zentry;

/* A zip of stored and raw-deflated entries, the way MAME's sets come. */
static void write_zip(const char *path, const zentry *e, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t off[8], csz[8], crc[8];
    uint8_t *comp[8];
    if (!f || n > 8) { perror(path); exit(1); }
    for (int i = 0; i < n; i++) {
        crc[i] = (uint32_t)crc32(0, e[i].data, e[i].size);
        if (e[i].deflate) {
            z_stream st;
            memset(&st, 0, sizeof st);
            comp[i] = malloc(e[i].size + 1024);
            deflateInit2(&st, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            st.next_in = (Bytef *)e[i].data;
            st.avail_in = e[i].size;
            st.next_out = comp[i];
            st.avail_out = e[i].size + 1024;
            deflate(&st, Z_FINISH);
            csz[i] = (uint32_t)st.total_out;
            deflateEnd(&st);
        } else {
            comp[i] = malloc(e[i].size);
            memcpy(comp[i], e[i].data, e[i].size);
            csz[i] = e[i].size;
        }
        off[i] = (uint32_t)ftell(f);
        put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, e[i].deflate ? 8 : 0);
        put16(f, 0); put16(f, 0); put32(f, crc[i]); put32(f, csz[i]); put32(f, e[i].size);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0);
        fputs(e[i].name, f);
        fwrite(comp[i], 1, csz[i], f);
    }
    uint32_t cd = (uint32_t)ftell(f);
    for (int i = 0; i < n; i++) {
        put32(f, 0x02014b50); put16(f, 20); put16(f, 20); put16(f, 0);
        put16(f, e[i].deflate ? 8 : 0); put16(f, 0); put16(f, 0);
        put32(f, crc[i]); put32(f, csz[i]); put32(f, e[i].size);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0); put16(f, 0);
        put16(f, 0); put16(f, 0); put32(f, 0); put32(f, off[i]);
        fputs(e[i].name, f);
        free(comp[i]);
    }
    uint32_t cd_end = (uint32_t)ftell(f);
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, (unsigned)n); put16(f, (unsigned)n);
    put32(f, cd_end - cd); put32(f, cd); put16(f, 0);
    fclose(f);
}

static astrosession *open_session(const char *dir, const char *name)
{
    char cfg[600], data[600];
    snprintf(cfg, sizeof cfg, "%s/%s-config", dir, name);
    snprintf(data, sizeof data, "%s/%s-data", dir, name);
    astrosession_paths paths = { cfg, data };
    astrosession *s = astrosession_new(&paths);
    if (!s) { fprintf(stderr, "astrosession_new failed\n"); exit(1); }
    return s;
}

/* The file the import wrote: the right name, size and CRC. */
static int stored_ok(astrosession *s, int item)
{
    static const char *names[3] = { "astro.bin", "ballyhlc.bin", "bioswhit.bin" };
    char path[1200];
    uint8_t buf[BIOS_SIZE + 1];
    snprintf(path, sizeof path, "%s/%s", astrosession_roms_path(s), names[item]);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n == BIOS_SIZE && (uint32_t)crc32(0, buf, BIOS_SIZE) == k_crc[item];
}

int main(void)
{
    static uint8_t bios[3][BIOS_SIZE], junk[BIOS_SIZE];
    char tmpl[512], path[1200], msg[512];
    crc_tables();

    test_tmpl(tmpl, sizeof tmpl, "rom_import_test");
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }

    for (int i = 0; i < 3; i++) {
        forge(bios[i], 0x1234u + (uint32_t)i, k_crc[i]);
        CHECK((uint32_t)crc32(0, bios[i], BIOS_SIZE) == k_crc[i], "forged CRC");
    }
    forge(junk, 99, 0x12345678u);       /* 8 KB, but no BIOS */

    astrosession *s = open_session(dir, "a");
    /* A WITH_ASTROCADE_ROMS=ON build has every BIOS before any import:
     * only the import itself and its "already available" message apply. */
    const int embedded = astrosession_has_system_roms(s);

    CHECK(astrosession_rom_item_count() == 3, "three items");
    CHECK(astrosession_rom_item(3) == NULL, "item past the end");
    CHECK(strcmp(astrosession_rom_item(1)->group, "BIOS") == 0, "group");
    if (!embedded) {
        CHECK(astrosession_resolve_bios(s, ASTROSESSION_BIOS_ASTRO) == -1, "nothing to boot");
        CHECK(astrosession_start(s, NULL) != 0, "start refuses with no BIOS");
    }

    /* not a BIOS: nothing written, and the message says so */
    snprintf(path, sizeof path, "%s/junk.bin", dir);
    write_file(path, junk, BIOS_SIZE);
    CHECK(astrosession_rom_import(s, path, msg, sizeof msg) == -1, "junk rejected");
    CHECK(strncmp(msg, "No Astrocade BIOS was found in ", 31) == 0, msg);

    /* a short copy of a BIOS is not one either */
    snprintf(path, sizeof path, "%s/short.bin", dir);
    write_file(path, bios[0], BIOS_SIZE - 1);
    CHECK(astrosession_rom_import(s, path, msg, sizeof msg) == -1, "short file rejected");

    /* a single file, whatever it is called */
    snprintf(path, sizeof path, "%s/My Bally.ROM", dir);
    write_file(path, bios[1], BIOS_SIZE);
    CHECK(astrosession_rom_import(s, path, msg, sizeof msg) == 0, "file imported");
    CHECK(stored_ok(s, 1), "ballyhlc.bin stored");
    if (!embedded) {
        CHECK(strcmp(msg, "Now available: Bally Home Library Computer BIOS (ballyhlc.bin)") == 0, msg);
        CHECK(astrosession_rom_item_available(s, 1), "item 1 available");
        CHECK(!astrosession_rom_item_available(s, 0), "item 0 still missing");
        CHECK(astrosession_has_system_roms(s), "has_system_roms");
        /* the default (astro.bin) is missing: a start boots this one */
        CHECK(astrosession_resolve_bios(s, ASTROSESSION_BIOS_ASTRO) == ASTROSESSION_BIOS_BALLYHLC,
              "resolve falls back");
    }
    CHECK(astrosession_resolve_bios(s, ASTROSESSION_BIOS_BALLYHLC) == ASTROSESSION_BIOS_BALLYHLC,
          "resolve keeps an available choice");

    /* again: written, but nothing new */
    CHECK(astrosession_rom_import(s, path, msg, sizeof msg) == 0, "re-import");
    CHECK(strcmp(msg, "Imported 1 file; it was already available") == 0, msg);

    /* a zip: stored and deflated entries, in a subfolder, beside junk */
    {
        zentry e[] = {
            { "astro.bin", bios[0], BIOS_SIZE, 0 },
            { "astrocdw/whatever.bin", bios[2], BIOS_SIZE, 1 },
            { "readme.txt", (const uint8_t *)"hello", 5, 1 },
            { "astrocdl/junk.bin", junk, BIOS_SIZE, 1 },
        };
        snprintf(path, sizeof path, "%s/astrocde.zip", dir);
        write_zip(path, e, 4);
        CHECK(astrosession_rom_import(s, path, msg, sizeof msg) == 0, "zip imported");
        CHECK(stored_ok(s, 0) && stored_ok(s, 2), "zip entries stored");
        if (!embedded)
            CHECK(strcmp(msg, "Now available: Bally Professional Arcade BIOS (astro.bin), "
                              "Bally Computer System BIOS (bioswhit.bin)") == 0, msg);
        for (int i = 0; i < 3; i++)
            CHECK(astrosession_rom_item_available(s, i), "all three available");
    }
    astrosession_free(s);

    /* a folder, nested, in a fresh session */
    s = open_session(dir, "b");
    {
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/set", dir);
        make_dir(sub);
        snprintf(sub, sizeof sub, "%s/set/deep", dir);
        make_dir(sub);
        snprintf(path, sizeof path, "%s/set/deep/arcade.bin", dir);
        write_file(path, bios[0], BIOS_SIZE);
        snprintf(path, sizeof path, "%s/set/other.bin", dir);
        write_file(path, junk, BIOS_SIZE);
        snprintf(path, sizeof path, "%s/set", dir);
        CHECK(astrosession_rom_import(s, path, msg, sizeof msg) == 0, "folder imported");
        CHECK(stored_ok(s, 0), "folder BIOS stored");
        if (!embedded) {
            CHECK(strcmp(msg, "Now available: Bally Professional Arcade BIOS (astro.bin)") == 0, msg);
            CHECK(!astrosession_rom_item_available(s, 1), "folder: no ballyhlc");
        }
        /* an empty folder: nothing */
        snprintf(sub, sizeof sub, "%s/empty", dir);
        make_dir(sub);
        CHECK(astrosession_rom_import(s, sub, msg, sizeof msg) == -1, "empty folder");
    }
    astrosession_free(s);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) { /* ignore */ }

    fprintf(stderr, "rom_import_test: %s%s\n", failures ? "FAIL" : "PASS",
            embedded ? " (embedded ROMs: availability checks skipped)" : "");
    return failures ? 1 : 0;
}
