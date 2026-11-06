/*
 * The built-in symbol table: the few fixed places of the Astrocade memory map
 * that the programmer's documentation names (HVGLIB's equates, which transcribe
 * the Bally Arcade technical description): the cartridge window, screen RAM,
 * and two cells in the on-board ROM. The BIOS routines are reached through
 * RST 56 system-call INDEXES, not addresses, so there are no entry points to
 * list. Verified against the BIOS image: $0214 holds 3F 3F 3F 3F (the all-keys
 * mask) and $0218 the head pointer of the on-board menu chain.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"

const char astrodebug_builtin_bios_sym[] =
    "0214 ALKEYS\n"
    "0218 MENUST\n"
    "2000 FIRSTC\n"
    "4000 NORMEM\n";
