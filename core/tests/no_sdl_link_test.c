/*
 * no_sdl_link_test -- astro_core must carry no SDL dependency. It links
 * astro_core ALONE (not astro_session, which is where SDL lives) and calls a
 * core entry point; if any SDL symbol had crept into the core archive, the
 * link would fail. The runtime body barely matters -- the linker is the test.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "host.h"

int main(void)
{
    /* touch a few core entry points so the linker pulls the archive in */
    (void)astro_host_is_running();
    (void)astro_host_last_error();
    astro_host_audio_reset();
    fprintf(stderr, "no_sdl_link_test: PASS (astro_core links with no SDL)\n");
    return 0;
}
