/* memtools: search parsing, watch list, trace filters (no ROMs needed). */
#include <stdio.h>
#include <string.h>

#include "astromemtools.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void)
{
    uint8_t pat[ASTROSEARCH_MAX], mask;
    int n, flags;
    astrodebug_trace t = { .pc = 0x8033, .len = 3, .bytes = { 0xCD, 0x50, 0x80 } };
    astrodebug_trace u = { .pc = 0x9000, .len = 1, .bytes = { 0x00 } };
    char buf[128];

    CHECK(astrosearch_parse(ASTROSEARCH_HEX, "5A 3c,$FF 0x10", pat, &n, &mask, &flags), "hex parses");
    CHECK(n == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask/flags");
    CHECK(astrosearch_parse(ASTROSEARCH_HEX, "5", pat, &n, &mask, &flags) && n == 1 && pat[0] == 5, "lone digit");
    CHECK(!astrosearch_parse(ASTROSEARCH_HEX, "zz", pat, &n, &mask, &flags), "bad hex");
    CHECK(!astrosearch_parse(ASTROSEARCH_HEX, "  ", pat, &n, &mask, &flags), "empty hex");
    CHECK(astrosearch_parse(ASTROSEARCH_TEXT, "Ab", pat, &n, &mask, &flags) && n == 2 &&
          pat[0] == 'A' && (flags & ASTRODEBUG_SEARCH_NOCASE), "text");

    astrowatch_clear();
    CHECK(astrowatch_add("  $4100 ") == 0 && astrowatch_add("score") == 1, "add");
    CHECK(astrowatch_add("   ") == -1, "empty add refused");
    CHECK(astrowatch_count() == 2 && !strcmp(astrowatch_text(0), "$4100"), "text trimmed");
    {
        unsigned g = astrowatch_generation();
        astrowatch_remove(0);
        CHECK(astrowatch_count() == 1 && !strcmp(astrowatch_text(0), "score"), "remove");
        CHECK(astrowatch_generation() != g, "generation bumps");
    }
    astrowatch_clear();
    CHECK(astrowatch_count() == 0, "clear");

    CHECK(astrodebug_trace_match(&t, ASTRODEBUG_TRACE_ALL, 0, 0), "all");
    CHECK(astrodebug_trace_match(&t, ASTRODEBUG_TRACE_BRANCHES, 0, 0), "CALL is a branch");
    CHECK(!astrodebug_trace_match(&u, ASTRODEBUG_TRACE_BRANCHES, 0, 0), "NOP is not");
    CHECK(astrodebug_trace_match(&t, ASTRODEBUG_TRACE_RANGE, 0x8000, 0x80FF), "in range");
    CHECK(!astrodebug_trace_match(&u, ASTRODEBUG_TRACE_RANGE, 0x8000, 0x80FF), "out of range");
    CHECK(astrodebug_trace_match(&t, ASTRODEBUG_TRACE_RANGE, 0x80FF, 0x8000), "range swaps");
    astrodebug_trace_format(&t, buf, sizeof buf);
    CHECK(!strncmp(buf, "8033  CALL $8050", 16), "trace format");

    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
