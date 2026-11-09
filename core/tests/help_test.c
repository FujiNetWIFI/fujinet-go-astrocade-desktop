/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astrohelp.h"
#include "astrosession.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    ASTROHELP_GNOME, ASTROHELP_KDE, ASTROHELP_WINDOWS, ASTROHELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "nobios", "keypad", "fujilog", "prefs/Machine", "prefs/ROMs",
    "debugger/console", "debugger/disassembly", "debugger/memory", "debugger/video",
    "debugger/breakpoints", "debugger/watches", "debugger/cart", "debugger/trace",
    "debugger/history", "debugger/fujinet",
};

/* The Preferences pages: what each one has, as the frontends label it. */
static const char *const labels[] = {
    "System ROM", "RAM Expansion", "Use external FujiNet", "Address:port",
    "Bally Professional Arcade (astro.bin)", "Bally Home Library Computer (ballyhlc.bin)",
    "Bally Computer System (bioswhit.bin)", "None", "Blue RAM 4K", "Blue RAM 16K", "Blue RAM 32K",
    "Viper System 1 (16K)", "Lil' WHITE RAM (32K)", "R&L 64K", "Import", "Import a Folder",
    "Bally Professional Arcade BIOS (astro.bin)", "Bally Home Library Computer BIOS (ballyhlc.bin)",
    "Bally Computer System BIOS (bioswhit.bin)", "Imported", "Not imported",
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "bios", "exp", "fujinet_external", "fujinet_external_addr", "cart", "bindings",
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+Shift+H", "⌘?" }, { "F9", "F9" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the liastro. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (astrohelp_find(context, &t, &b) == 0) {
        astrohelp_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(int p)
{
    for (size_t k = 0; k < sizeof labels / sizeof *labels; k++)
        require(p, "preferences", "label", labels[k]);
    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "preferences", "setting", settings_keys[k]);
}

/* Every default key and the shortcuts. */
static void
check_controls(int p)
{
    static const char *const keys[] = { "C", "U", "D", "P", "R", "S", "H", "X", "E", "Enter", "Space", "Backspace" };

    for (size_t k = 0; k < sizeof keys / sizeof *keys; k++)
        require(p, "default-keys", "key", keys[k]);
    require_row(p, "default-keys", "Trigger", "Space", NULL);
    require_row(p, "default-keys", "Up, Down, Left, Right", "arrow keys", NULL);
    require_row(p, "default-keys", "Reset Game", "Backspace", NULL);
    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++)
        require(p, "default-keys", "shortcut", platforms[p] == ASTROHELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", ASTROSESSION_BOIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", ASTROSESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:11500");
}

/* The debugger's buttons and its tabs. */
/* The debugger's buttons, and each prompt command in a `code` span. */
static void
check_debugger(int p)
{
    static const char *const words[] = { "Run or Stop", "Step Over", "Step Out", "Frame", "Run to Cursor",
                                         "Load Symbols", "Pop Out", "ten tabs" };
    static const char *const commands[] = { "step", "over", "out", "run", "stop", "runto", "scanline", "frame",
                                            "break", "bpr", "bpw", "bpin", "bpout", "delete", "enable", "clear",
                                            "print", "regs", "set", "mem", "poke", "vram", "vpoke", "video", "io",
                                            "map", "fujinet", "label", "labels", "disasm", "trace", "watch", "unwatch", "find",
                                            "cart", "save" };
    int t, b;

    for (size_t k = 0; k < sizeof words / sizeof *words; k++)
        require(p, "debugger", "button", words[k]);
    {
        static const char *const vwords[] = { "Display", "Decoded", "Beam", "Values", "Zoom", "Fit", "Overlays",
                                              "Pixel", "Registers", "Colours", "Lines", "Timing",
                                              "Break on writes", "Break on OUT" };
        for (size_t k = 0; k < sizeof vwords / sizeof *vwords; k++)
            require(p, "debugger/video", "control", vwords[k]);
    }
    CHECK(astrohelp_find("debugger/console", &t, &b) == 0, "no debugger/console");
    {
        const astrohelp_topic *tp = astrohelp_topic_at(t);
        int end = astrohelp_section_end(t, b);
        for (size_t k = 0; k < sizeof commands / sizeof *commands; k++) {
            const char *cmd = commands[k];
            size_t n = strlen(cmd);
            int found = 0;
            for (int bl = b; bl < end && !found; bl++) {
                const astrohelp_block *blk = &tp->blocks[bl];
                if (!(blk->platforms & platforms[p]))
                    continue;
                for (int i = 0; i < blk->nspans && !found; i++) {
                    const char *c = blk->spans[i].text;
                    if (blk->spans[i].kind != ASTROHELP_CODE)
                        continue;
                    found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
                }
            }
            if (!found) {
                fprintf(stderr, "MISSING (%s) debugger/console: command `%s`\n", platform_names[p], cmd);
                misses++;
            }
        }
    }
}

int
main(void)
{
    astrohelp_hit hits[16];
    int t, b;

    CHECK(astrohelp_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (astrohelp_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(p);
        check_controls(p);
        check_ports(p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(astrohelp_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(astrohelp_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(astrohelp_search(ASTROHELP_GNOME, "cartridge", hits, 16) > 0, "search cartridge");
    CHECK(astrohelp_search(ASTROHELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = astrohelp_search(ASTROHELP_KDE, "watchpoints", hits, 16);
    CHECK(n > 0 && astrohelp_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "watchpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(astrohelp_find("default-keys", &t, &b) == 0, "default-keys");
        astrohelp_plain_text(ASTROHELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘?") != NULL, "the Mac's Help key");
        astrohelp_plain_text(ASTROHELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+Shift+H") != NULL && !strstr(text, "⌘?"), "the PC's");
    }

    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", astrohelp_topic_count());
    return 0;
}
