/*
 * astrohelp -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTROHELP_H
#define ASTROHELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ASTROHELP_GNOME = 1, ASTROHELP_KDE = 2, ASTROHELP_WINDOWS = 4, ASTROHELP_MACOS = 8
};

typedef enum {
    ASTROHELP_H2 = 0, ASTROHELP_H3,
    ASTROHELP_PARA,
    ASTROHELP_BULLET, ASTROHELP_BULLET2,    /* "- " and "  - " */
    ASTROHELP_NUMBERED,                     /* a step; number is its own */
    ASTROHELP_NOTE,
    ASTROHELP_PRE,                          /* one TEXT span, lines as they are */
    ASTROHELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    ASTROHELP_TABLE_ROW
} astrohelp_block_kind;

typedef enum {
    ASTROHELP_TEXT = 0, ASTROHELP_BOLD, ASTROHELP_ITALIC, ASTROHELP_CODE,
    ASTROHELP_KBD,                          /* keys; mac_text is the Mac's */
    ASTROHELP_LINK,                         /* target "#slug" or a URL */
    ASTROHELP_CELL                          /* tables: the next column starts */
} astrohelp_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} astrohelp_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* ASTROHELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const astrohelp_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} astrohelp_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const astrohelp_block *blocks;
    int nblocks;
} astrohelp_topic;

int astrohelp_topic_count(void);
const astrohelp_topic *astrohelp_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int astrohelp_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *astrohelp_span_text(const astrohelp_span *sp, unsigned platform);

/* A heading block's text. */
const char *astrohelp_heading(const astrohelp_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int astrohelp_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int astrohelp_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} astrohelp_hit;
int astrohelp_search(unsigned platform, const char *query, astrohelp_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int astrohelp_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* ASTROHELP_H */
