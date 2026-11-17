/*
 * overlay_test -- the Bally BASIC overlay: the legend table, the keypad taps
 * that type a character or a BASIC word, the setting, and the cartridge CRC
 * that turns it on. The CRC test forges a 4 KB image with the right CRC32 (the
 * real ROMs are not in the repo).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "astrosession.h"
#include "session_internal.h"
#include "host.h"
#include "input.h"
#include "test_tmpdir.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* The taps a character types, as "[shift]key" key names for a readable test:
 * returns 1 if the sequence is exactly (a) or (a, b). */
static int seq1(uint32_t chr, int a)
{
    uint8_t keys[2] = { 0xff, 0xff };
    return astrosession_overlay_sequence(chr, keys, 2) == 1 && keys[0] == a;
}
static int seq2(uint32_t chr, int a, int b)
{
    uint8_t keys[2] = { 0xff, 0xff };
    return astrosession_overlay_sequence(chr, keys, 2) == 2 && keys[0] == a && keys[1] == b;
}


/* which keypad keys the machine sees pressed right now, as a bit per key */
static uint32_t matrix_keys(void)
{
    uint32_t keys = 0;
    const astro_machine_t *m = astro_host_machine();
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        if (m->keypad[3 - (k % 4)] & (1u << (k / 4)))
            keys |= 1u << k;
    return keys;
}

/* Types `n` ERASEs then a character through the real queue and returns the
 * presses the machine saw, in order (rising edges of the keypad matrix). */
static int watch_typing(astrosession *s, int erases, uint32_t chr, int *seen, int max)
{
    int count = 0;
    uint32_t last = 0;
    for (int i = 0; i < erases; i++)
        astrosession_type_char(s, 8);
    astrosession_type_char(s, chr);
    for (int waited = 0; waited < 8000; waited++) {
        const uint32_t now = matrix_keys();
        const uint32_t rise = now & ~last;
        for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
            if ((rise >> k) & 1 && count < max)
                seen[count++] = k;
        last = now;
        if (!astrosession_typing_pending(s) && !now)
            break;
        usleep(1000);
    }
    return count;
}

/* ---- forge a buffer with a chosen CRC-32 ---------------------------------- */

static uint32_t g_table[256];
static void make_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
        g_table[i] = c;
    }
}

/* Sets the last four bytes of buf so its CRC-32 is `target`. */
static void forge_crc(uint8_t *buf, size_t size, uint32_t target)
{
    uint32_t reg = 0xffffffffu, want = ~target, idx[4], back[5];
    int i, k;
    for (size_t pos = 0; pos < size - 4; pos++)
        reg = (reg >> 8) ^ g_table[(reg ^ buf[pos]) & 0xff];
    back[4] = want;
    for (i = 3; i >= 0; i--) {
        for (k = 0; k < 256; k++)
            if ((g_table[k] >> 24) == (back[i + 1] >> 24)) break;
        idx[i] = (uint32_t)k;
        back[i] = (back[i + 1] ^ g_table[k]) << 8;
    }
    for (i = 0; i < 4; i++) {
        buf[size - 4 + (size_t)i] = (uint8_t)(idx[i] ^ (reg & 0xff));
        reg = (reg >> 8) ^ g_table[idx[i]];
    }
}

int main(void)
{
    const astro_overlay_key *lay = astrosession_overlay_layout();
    uint8_t keys[2];

    CHECK(lay != NULL, "the layout exists");

    /* the legends read off the card */
    CHECK(!strcmp(lay[ASTROSESSION_KEY_C].legend[ASTRO_SHIFT_NONE], "GO"), "C is GO");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_C].legend[ASTRO_SHIFT_WORDS], "+10"), "WORDS GO is +10");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_DOWN].legend[ASTRO_SHIFT_NONE], "HALT"), "the down key is HALT");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_DOWN].legend[ASTRO_SHIFT_WORDS], "RUN"), "WORDS HALT is RUN");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_MR].legend[ASTRO_SHIFT_NONE], "7") &&
          !strcmp(lay[ASTROSESSION_KEY_MR].legend[ASTRO_SHIFT_GREEN], "A") &&
          !strcmp(lay[ASTROSESSION_KEY_MR].legend[ASTRO_SHIFT_RED], "B") &&
          !strcmp(lay[ASTROSESSION_KEY_MR].legend[ASTRO_SHIFT_BLUE], "C") &&
          !strcmp(lay[ASTROSESSION_KEY_MR].legend[ASTRO_SHIFT_WORDS], "FOR"), "MR: 7 A B C FOR");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_PLUS].legend[ASTRO_SHIFT_WORDS], "PRINT"), "WORDS + is PRINT");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_MINUS].legend[ASTRO_SHIFT_WORDS], "GOTO"), "WORDS - is GOTO");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_1].legend[ASTRO_SHIFT_NONE], "SPACE"), "key 1 is SPACE");
    CHECK(!strcmp(lay[ASTROSESSION_KEY_3].legend[ASTRO_SHIFT_NONE], "ERASE"), "key 3 is ERASE");
    for (int k = ASTROSESSION_KEY_CE; k <= ASTROSESSION_KEY_EQ; k++)
        for (int sh = 0; sh < ASTRO_OVERLAY_SHIFTS; sh++)
            CHECK(lay[k].legend[sh] == NULL, "the shift row prints no legends");

    /* the shifts: CE green, 0 red, DOT blue, EQ WORDS, and back */
    CHECK(astrosession_overlay_shift_key(ASTRO_SHIFT_GREEN) == ASTROSESSION_KEY_CE &&
          astrosession_overlay_shift_key(ASTRO_SHIFT_RED) == ASTROSESSION_KEY_0 &&
          astrosession_overlay_shift_key(ASTRO_SHIFT_BLUE) == ASTROSESSION_KEY_DOT &&
          astrosession_overlay_shift_key(ASTRO_SHIFT_WORDS) == ASTROSESSION_KEY_EQ &&
          astrosession_overlay_shift_key(ASTRO_SHIFT_NONE) == -1, "the four shift keys");
    for (int sh = ASTRO_SHIFT_GREEN; sh <= ASTRO_SHIFT_WORDS; sh++)
        CHECK(astrosession_overlay_key_shift(astrosession_overlay_shift_key(sh)) == sh, "key_shift inverts shift_key");
    CHECK(astrosession_overlay_key_shift(ASTROSESSION_KEY_7) == -1, "an ordinary key is no shift");

    /* the sequences of the design table */
    CHECK(seq1('1', ASTROSESSION_KEY_4) && seq1('2', ASTROSESSION_KEY_5) && seq1('3', ASTROSESSION_KEY_6),
          "1 2 3 are the physical 4 5 6");
    CHECK(seq1('4', ASTROSESSION_KEY_7) && seq1('5', ASTROSESSION_KEY_8) && seq1('6', ASTROSESSION_KEY_9),
          "4 5 6 are the physical 7 8 9");
    CHECK(seq1('7', ASTROSESSION_KEY_MR) && seq1('8', ASTROSESSION_KEY_MS) && seq1('9', ASTROSESSION_KEY_CH),
          "7 8 9 are MR MS CH");
    CHECK(seq1('0', ASTROSESSION_KEY_2), "0 is the physical 2");
    CHECK(seq1(' ', ASTROSESSION_KEY_1), "space is the physical 1");
    CHECK(seq1('+', ASTROSESSION_KEY_MINUS) && seq1('-', ASTROSESSION_KEY_MUL) &&
          seq1('*', ASTROSESSION_KEY_DIV) && seq1('/', ASTROSESSION_KEY_PCT) &&
          seq1('=', ASTROSESSION_KEY_PLUS), "+ - * / = are the operator legends on their keys");
    CHECK(seq2('A', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_MR) &&
          seq2('B', ASTROSESSION_KEY_0, ASTROSESSION_KEY_MR) &&
          seq2('C', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_MR), "A B C: green, red, blue on MR");
    CHECK(seq2('a', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_MR), "lower case types the same capital");
    CHECK(seq2('H', ASTROSESSION_KEY_0, ASTROSESSION_KEY_CH), "H is red CH (the manual's HI THERE!)");
    CHECK(seq2('I', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_CH), "I is blue CH");
    CHECK(seq2('T', ASTROSESSION_KEY_0, ASTROSESSION_KEY_9) && seq2('E', ASTROSESSION_KEY_0, ASTROSESSION_KEY_MS) &&
          seq2('R', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_8) && seq2('!', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_4),
          "T E R ! as the manual types them");
    CHECK(seq2('X', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_MUL) && seq2('Z', ASTROSESSION_KEY_0, ASTROSESSION_KEY_4) &&
          seq2('Y', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_4), "X Z Y");
    CHECK(seq2('\'', ASTROSESSION_KEY_0, ASTROSESSION_KEY_5) && seq2('.', ASTROSESSION_KEY_0, ASTROSESSION_KEY_6),
          "' and . are red 5 and red 6");
    CHECK(seq2('&', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_MINUS) && seq2('@', ASTROSESSION_KEY_0, ASTROSESSION_KEY_MINUS),
          "& and @ are green and red MINUS");
    CHECK(seq2('$', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_1) && seq2(',', ASTROSESSION_KEY_0, ASTROSESSION_KEY_1) &&
          seq2('?', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_1), "$ , ? on key 1");
    CHECK(seq2('<', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_2) && seq2('"', ASTROSESSION_KEY_0, ASTROSESSION_KEY_2) &&
          seq2('>', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_2), "< \" > on key 2");
    CHECK(seq2('(', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_3) && seq2(';', ASTROSESSION_KEY_0, ASTROSESSION_KEY_3) &&
          seq2(')', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_3), "( ; ) on key 3");
    CHECK(seq2('#', ASTROSESSION_KEY_CE, ASTROSESSION_KEY_PLUS) && seq2('%', ASTROSESSION_KEY_0, ASTROSESSION_KEY_PLUS) &&
          seq2(':', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_PLUS), "# % : on PLUS");
    CHECK(seq2('[', ASTROSESSION_KEY_0, ASTROSESSION_KEY_PCT) && seq2(']', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_PCT) &&
          seq2('\\', ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_UP), "[ ] and \\");
    CHECK(seq1('\n', ASTROSESSION_KEY_C) && seq1('\r', ASTROSESSION_KEY_C), "Enter is GO");
    CHECK(seq1(8, ASTROSESSION_KEY_3) && seq1(127, ASTROSESSION_KEY_3), "Backspace is ERASE");
    CHECK(astrosession_overlay_sequence('~', keys, 2) == 0 && astrosession_overlay_sequence('`', keys, 2) == 0 &&
          astrosession_overlay_sequence('^', keys, 2) == 0 && astrosession_overlay_sequence('_', keys, 2) == 0 &&
          astrosession_overlay_sequence('{', keys, 2) == 0 && astrosession_overlay_sequence('|', keys, 2) == 0,
          "characters the card does not have give nothing");
    CHECK(astrosession_overlay_sequence(0, keys, 2) == 0 && astrosession_overlay_sequence(0x20ac, keys, 2) == 0 &&
          astrosession_overlay_sequence('A', keys, 1) == 0 && astrosession_overlay_sequence('A', NULL, 2) == 0,
          "NUL, non-ASCII, a short buffer and NULL give nothing");

    /* every ASCII legend on the card can be typed, and the last key's legend at that shift is the character */
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        for (int sh = ASTRO_SHIFT_NONE; sh <= ASTRO_SHIFT_BLUE; sh++) {
            const char *leg = lay[k].legend[sh];
            if (leg && leg[0] && !leg[1]) {
                const int got = astrosession_overlay_sequence((unsigned char)leg[0], keys, 2);
                CHECK(got == 1 || got == 2, "a single-character legend can be typed");
            }
        }

    /* the gold words */
    CHECK(astrosession_overlay_word_sequence("PRINT", keys, 2) == 2 &&
          keys[0] == ASTROSESSION_KEY_EQ && keys[1] == ASTROSESSION_KEY_PLUS, "PRINT: WORDS, PLUS");
    CHECK(astrosession_overlay_word_sequence("goto", keys, 2) == 2 &&
          keys[0] == ASTROSESSION_KEY_EQ && keys[1] == ASTROSESSION_KEY_MINUS, "goto is case-insensitive");
    CHECK(astrosession_overlay_word_sequence("RUN", keys, 2) == 2 && keys[1] == ASTROSESSION_KEY_DOWN, "RUN: WORDS, DOWN");
    CHECK(astrosession_overlay_word_sequence("+10", keys, 2) == 2 && keys[1] == ASTROSESSION_KEY_C, "+10: WORDS, GO");
    CHECK(astrosession_overlay_word_sequence("PRIN", keys, 2) == 0 && astrosession_overlay_word_sequence("", keys, 2) == 0 &&
          astrosession_overlay_word_sequence(NULL, keys, 2) == 0 && astrosession_overlay_word_sequence("NOPE", keys, 2) == 0,
          "not a word of the card");
    {
        static const char *const words[] = { "FOR", "TO", "STEP", "NEXT", "GOSB", "RETN", "RND", "IF", "CLEAR",
            "LINE", "BOX", "GOTO", "INPUT", "PRINT", "LIST", "RUN", "+10" };
        for (unsigned i = 0; i < sizeof words / sizeof words[0]; i++)
            CHECK(astrosession_overlay_word_sequence(words[i], keys, 2) == 2, "every gold word can be typed");
    }

    /* the setting and the cartridge CRC */
    {
        char tmpl[512], cfg[512], data[512];
        test_tmpl(tmpl, sizeof tmpl, "astro_overlay_test");
        char *dir = mkdtemp(tmpl);
        if (!dir) return 1;
        snprintf(cfg, sizeof cfg, "%s/config", dir);
        snprintf(data, sizeof data, "%s/data", dir);
        astrosession_paths paths = { cfg, data };
        astrosession *s = astrosession_new(&paths);
        if (!s) return 1;

        make_table();
        CHECK(!astrosession_overlay_get(s), "off by default");
        astrosession_overlay_set(s, 1);
        astrosession_free(s);
        s = astrosession_new(&paths);
        if (!s) return 1;
        CHECK(astrosession_overlay_get(s), "the setting is remembered across a restart");
        astrosession_overlay_set(s, 0);

        static uint8_t rom[4096], odd[4096], big[8192];
        for (size_t i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i * 7 + 3);
        memcpy(odd, rom, sizeof odd);
        memcpy(big, rom, sizeof rom);
        forge_crc(rom, sizeof rom, 0x90e276aau);                       /* bbasic */
        CHECK(roms_crc32(rom, sizeof rom) == 0x90e276aau, "(the forged image has the CRC it should)");
        CHECK(overlay_is_basic_cart(rom, sizeof rom), "Bally BASIC (bbasic) is recognised");
        forge_crc(odd, sizeof odd, 0x70514687u);                       /* astrobas */
        CHECK(overlay_is_basic_cart(odd, sizeof odd), "Astro BASIC (astrobas) is recognised");
        for (size_t i = 0; i < sizeof odd; i++) odd[i] = (uint8_t)(i * 3 + 1);
        CHECK(!overlay_is_basic_cart(odd, sizeof odd), "another 4 KB cartridge is not");
        forge_crc(big, sizeof big, 0x90e276aau);
        CHECK(!overlay_is_basic_cart(big, sizeof big), "the right CRC at the wrong size is not");
        forge_crc(odd, sizeof odd, 0x0b5298bcu);                       /* astrobat, a game */
        CHECK(!overlay_is_basic_cart(odd, sizeof odd), "Astro Battle (which shares a prefix) is not");
        CHECK(!overlay_is_basic_cart(NULL, 4096), "no image is not");

        const unsigned gen0 = astrosession_overlay_auto_generation(s);
        overlay_cart_loaded(s, odd, sizeof odd);
        CHECK(!astrosession_overlay_get(s) && astrosession_overlay_auto_generation(s) == gen0,
              "loading a game leaves the overlay alone");
        overlay_cart_loaded(s, rom, sizeof rom);
        CHECK(astrosession_overlay_get(s) && astrosession_overlay_auto_generation(s) == gen0 + 1,
              "loading Bally BASIC turns it on and bumps the generation");
        overlay_cart_loaded(s, rom, sizeof rom);
        CHECK(astrosession_overlay_auto_generation(s) == gen0 + 2, "and again on a second load (the toast)");

        /* typing: a burst of ERASE taps then a character loses no tap. Each
         * of the 6 ERASEs, then H (the red shift, then CH), is one press the
         * machine sees, in order, even though the queue drains and refills. */
        {
            int seen[32], want[2], n;
            CHECK(astrosession_overlay_sequence('H', (uint8_t[2]){0}, 2) == 2, "(H is two taps)");
            uint8_t hk[2];
            astrosession_overlay_sequence('H', hk, 2);
            want[0] = hk[0]; want[1] = hk[1];
            for (int round = 0; round < 3; round++) {
                n = watch_typing(s, 6, 'H', seen, 32);
                CHECK(n == 8, "ERASE x6 then H is eight presses");
                for (int i = 0; i < 6 && i < n; i++)
                    CHECK(seen[i] == ASTROSESSION_KEY_3, "each of the six is the ERASE key");
                CHECK(n >= 8 && seen[6] == want[0] && seen[7] == want[1], "H follows intact (shift, then CH)");
            }
        }
        /* the Backspace key is ERASE with the overlay on, and Reset Game off it */
        astrosession_sysaction_take(s);
        CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 1, 0) == 1, "Backspace is consumed");
        CHECK(astrosession_typing_pending(s) > 0, "and queues an ERASE tap");
        CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 1, 0) == 1 &&
              astrosession_typing_pending(s) <= 1, "a repeat adds nothing");
        CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 0, 0) == 1, "its release is consumed");
        CHECK(astrosession_sysaction_take(s) == 0, "Reset Game was not posted");
        astro_input_typing_stop();
        CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 1, 1) == 0 &&
              astrosession_typing_pending(s) == 0, "Ctrl+Backspace is left alone (no ERASE)");
        astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 0, 1);
        astrosession_overlay_set(s, 0);
        CHECK(astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 1, 0) == 1 &&
              astrosession_sysaction_take(s) != 0 && astrosession_typing_pending(s) == 0,
              "with the overlay off Backspace is Reset Game");
        astrosession_input_key(s, ASTROSESSION_KEYSYM_BACKSPACE, 0, 0);

        astrosession_free(s);
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd)) { /* ignore */ }
    }

    fprintf(stderr, "overlay_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
