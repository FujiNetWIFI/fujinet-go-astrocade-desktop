/*
 * overlay -- the Bally BASIC keypad overlay: the legends printed on its card,
 * the keypad sequences that type a character or a BASIC word on it, and the
 * setting that turns it on (by hand, or by loading a Bally BASIC cartridge).
 *
 * The card lies over the 24-key keypad. A key has up to five legends: the
 * white one (pressing the key), three small ones in green, red and blue (tap
 * the shift of that colour, then the key) and a gold word (tap WORDS, then
 * the key). The shifts are the bottom row of the keypad: CE green, 0 red,
 * DOT blue, EQ WORDS. All of it read from the card and the Bally BASIC manual
 * (ver 2, pp. 5-10); the legend table is in DESIGN-astrocade.md.
 *
 * Typing is a queue of keypad presses (input.c): a shift tap, then the key's
 * tap, one tap at a time. The manual's rule is "don't hold the shift key".
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <string.h>

#include "astrosession.h"
#include "input.h"
#include "session_internal.h"

#define GO     "GO"
#define DIVIDE "\xC3\xB7"          /* ÷ */
#define TIMES  "\xC3\x97"          /* × */
#define LEFT   "\xE2\x86\x90"      /* ← */
#define RIGHT  "\xE2\x86\x92"      /* → */
#define UPARR  "\xE2\x86\x91"      /* ↑ */
#define DOWNAR "\xE2\x86\x93"      /* ↓ */

/* By physical key, in astrosession_key order. legend[] is NONE (white),
 * GREEN, RED, BLUE, WORDS (gold); NULL prints nothing. */
static const astro_overlay_key k_overlay[ASTROSESSION_KEY_COUNT] = {
    /* row 0 */
    { { GO,      NULL,    NULL,   NULL,   "+10"   } },   /* C */
    { { "PAUSE", NULL,    "/",    "\\",   NULL    } },   /* UP */
    { { "HALT",  NULL,    NULL,   NULL,   "RUN"   } },   /* DOWN: hold */
    { { DIVIDE,  NULL,    "[",    "]",    "LIST"  } },   /* % */
    /* row 1 */
    { { "7",     "A",     "B",    "C",    "FOR"   } },   /* MR */
    { { "8",     "D",     "E",    "F",    "TO"    } },   /* MS */
    { { "9",     "G",     "H",    "I",    "STEP"  } },   /* CH */
    { { TIMES,   "J",     "K",    "L",    "NEXT"  } },   /* DIV */
    /* row 2 */
    { { "4",     "M",     "N",    "O",    "GOSB"  } },   /* 7 */
    { { "5",     "P",     "Q",    "R",    "RETN"  } },   /* 8 */
    { { "6",     "S",     "T",    "U",    "RND"   } },   /* 9 */
    { { "-",     "V",     "W",    "X",    "IF"    } },   /* MUL */
    /* row 3 */
    { { "1",     "Y",     "Z",    "!",    "CLEAR" } },   /* 4 */
    { { "2",     LEFT,    "'",    RIGHT,  "LINE"  } },   /* 5 */
    { { "3",     UPARR,   ".",    DOWNAR, "BOX"   } },   /* 6 */
    { { "+",     "&",     "@",    "*",    "GOTO"  } },   /* MINUS */
    /* row 4 */
    { { "SPACE", "$",     ",",    "?",    NULL    } },   /* 1 */
    { { "0",     "<",     "\"",   ">",    "INPUT" } },   /* 2 */
    { { "ERASE", "(",     ";",    ")",    NULL    } },   /* 3 */
    { { "=",     "#",     "%",    ":",    "PRINT" } },   /* PLUS */
    /* row 5: the shifts */
    { { NULL,    NULL,    NULL,   NULL,   NULL    } },   /* CE   green */
    { { NULL,    NULL,    NULL,   NULL,   NULL    } },   /* 0    red */
    { { NULL,    NULL,    NULL,   NULL,   NULL    } },   /* DOT  blue */
    { { NULL,    NULL,    NULL,   NULL,   NULL    } },   /* EQ   WORDS */
};

const astro_overlay_key *astrosession_overlay_layout(void)
{
    return k_overlay;
}

int astrosession_overlay_shift_key(int shift)
{
    switch (shift) {
    case ASTRO_SHIFT_GREEN: return ASTROSESSION_KEY_CE;
    case ASTRO_SHIFT_RED:   return ASTROSESSION_KEY_0;
    case ASTRO_SHIFT_BLUE:  return ASTROSESSION_KEY_DOT;
    case ASTRO_SHIFT_WORDS: return ASTROSESSION_KEY_EQ;
    default: return -1;
    }
}

int astrosession_overlay_key_shift(int key)
{
    switch (key) {
    case ASTROSESSION_KEY_CE:  return ASTRO_SHIFT_GREEN;
    case ASTROSESSION_KEY_0:   return ASTRO_SHIFT_RED;
    case ASTROSESSION_KEY_DOT: return ASTRO_SHIFT_BLUE;
    case ASTROSESSION_KEY_EQ:  return ASTRO_SHIFT_WORDS;
    default: return -1;
    }
}

/* The host character a legend types, or 0: a single ASCII character as it
 * is printed, and the two operator glyphs as the characters BASIC reads them
 * as ('*' for the multiply sign, '/' for the divide sign). */
static int legend_char(const char *legend)
{
    if (!legend) return 0;
    if (!strcmp(legend, TIMES)) return '*';
    if (!strcmp(legend, DIVIDE)) return '/';
    if (legend[0] && !legend[1] && (unsigned char)legend[0] < 0x80)
        return (unsigned char)legend[0];
    return 0;
}

int astrosession_overlay_sequence(uint32_t codepoint, uint8_t *keys, int max)
{
    static const int k_order[3] = { ASTRO_SHIFT_GREEN, ASTRO_SHIFT_RED, ASTRO_SHIFT_BLUE };
    int chr, key, idx;

    if (!keys || max < 2 || codepoint == 0 || codepoint > 0x7f)
        return 0;
    chr = (int)codepoint;
    if (chr >= 'a' && chr <= 'z')
        chr -= 'a' - 'A';             /* lower case types the same capital */
    if (chr == '\r' || chr == '\n') { keys[0] = ASTROSESSION_KEY_C; return 1; }   /* GO */
    if (chr == 0x08 || chr == 0x7f) { keys[0] = ASTROSESSION_KEY_3; return 1; }   /* ERASE */
    if (chr == ' ') { keys[0] = ASTROSESSION_KEY_1; return 1; }                   /* SPACE */

    /* the plain legends first (so '/' and '*' are the operator keys, not the
     * shifted characters of the same shape), then each shift's cells */
    for (key = 0; key < ASTROSESSION_KEY_COUNT; key++)
        if (legend_char(k_overlay[key].legend[ASTRO_SHIFT_NONE]) == chr) {
            keys[0] = (uint8_t)key;
            return 1;
        }
    for (idx = 0; idx < 3; idx++)
        for (key = 0; key < ASTROSESSION_KEY_COUNT; key++)
            if (legend_char(k_overlay[key].legend[k_order[idx]]) == chr) {
                keys[0] = (uint8_t)astrosession_overlay_shift_key(k_order[idx]);
                keys[1] = (uint8_t)key;
                return 2;
            }
    return 0;
}

int astrosession_overlay_word_sequence(const char *word, uint8_t *keys, int max)
{
    int key;
    if (!word || !*word || !keys || max < 2)
        return 0;
    for (key = 0; key < ASTROSESSION_KEY_COUNT; key++) {
        const char *legend = k_overlay[key].legend[ASTRO_SHIFT_WORDS];
        size_t pos;
        if (!legend || strlen(legend) != strlen(word))
            continue;
        for (pos = 0; legend[pos]; pos++)
            if (toupper((unsigned char)word[pos]) != legend[pos])
                break;
        if (!legend[pos]) {
            keys[0] = ASTROSESSION_KEY_EQ;
            keys[1] = (uint8_t)key;
            return 2;
        }
    }
    return 0;
}

/* ---- the setting ---------------------------------------------------------------- */

static unsigned g_auto_gen;

void astrosession_overlay_set(astrosession *s, int on)
{
    astrosession_set_int(s, "overlay_bally_basic", on ? 1 : 0);
}

int astrosession_overlay_get(astrosession *s)
{
    return astrosession_get_int(s, "overlay_bally_basic", 0) != 0;
}

unsigned astrosession_overlay_auto_generation(astrosession *s)
{
    (void)s;
    return g_auto_gen;
}

/* The two Bally BASIC interpreter ROMs: bbasic (1978, 4 KB, CRC 90e276aa) and
 * astrobas (Astrovision 1981, 4 KB, CRC 70514687). bbasdemo (8 KB, 007e70d5)
 * is left out until someone has booted it; astrobat (0b5298bc) is a game that
 * only shares the prefix. */
static const uint32_t k_basic_crc[] = { 0x90e276aau, 0x70514687u };

int overlay_is_basic_cart(const uint8_t *data, size_t size)
{
    uint32_t crc;
    size_t idx;
    if (!data || size != 4096)
        return 0;
    crc = roms_crc32(data, size);
    for (idx = 0; idx < sizeof k_basic_crc / sizeof k_basic_crc[0]; idx++)
        if (k_basic_crc[idx] == crc)
            return 1;
    return 0;
}

void overlay_cart_loaded(astrosession *s, const uint8_t *data, size_t size)
{
    if (!overlay_is_basic_cart(data, size))
        return;
    if (!astrosession_overlay_get(s))
        astrosession_overlay_set(s, 1);
    g_auto_gen++;               /* the toast, even if it was on already */
}

/* ---- typing ---------------------------------------------------------------------- */

int astrosession_type_char(astrosession *s, uint32_t codepoint)
{
    uint8_t keys[2];
    int count;
    if (!astrosession_overlay_get(s))
        return 0;
    count = astrosession_overlay_sequence(codepoint, keys, 2);
    if (count <= 0)
        return 0;
    return astro_input_type_keys(keys, count) == count;
}

int astrosession_type_word(astrosession *s, const char *word)
{
    uint8_t keys[2];
    int count;
    if (!astrosession_overlay_get(s))
        return 0;
    count = astrosession_overlay_word_sequence(word, keys, 2);
    if (count <= 0)
        return 0;
    return astro_input_type_keys(keys, count) == count;
}
