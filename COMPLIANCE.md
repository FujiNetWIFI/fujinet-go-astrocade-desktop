# Licensing and redistribution

This is a combined work distributed under **GPL-3.0-or-later** (see `LICENSE`).
The parts have their own provenance:

## The emulator core — MAME, BSD-3-Clause

`core/astro/machine.c`, `video.c`, `sound.c` and the register/timing model in
`astro_internal.h` are transposed to C from MAME's Bally Astrocade
home-console driver:

- `src/mame/midway/astrohome.cpp` — memory/IO maps, keypad matrix, machine
  config (copyright-holders Nicola Salmoria, Mike Coates, Frank Palazzolo,
  Aaron Giles, Dirk Best)
- `src/mame/midway/astrocde_v.cpp` — data-chip video registers, the magic
  function generator, palette, rendering, interrupt generation (same holders,
  less Dirk Best)
- `src/devices/sound/astrocde.cpp` — the custom I/O chip's sound section
  (copyright-holders Aaron Giles, Frank Palazzolo)

All three are `// license:BSD-3-Clause`. BSD-3-Clause requires only that the
copyright notice and disclaimer be retained (done in each transposed file's
header) and permits relicensing the combined work under the GPL. The
arcade-only pieces (pattern board, ProfPac 16-color board, sparkle/star
circuit) are not carried over.

## The CPU — floooh/chips, zlib

`core/astro/z80/z80.h` and `z80/z80dasm.h` are the `z80.h` / `util/z80dasm.h`
single-file headers from https://github.com/floooh/chips (© Andre Weissflog,
zlib/libpng license), vendored verbatim. zlib permits use and redistribution;
the notice is retained in each header.

## The FujiNet protocol and cartridge — fujinet-firmware, mixed

`core/astro/fujinet_cart.c` is transposed from the firmware's own MAME cart
device `pico/astrocade/emu/fujinet.cpp` (BSD-3-Clause, © Thomas Cherryhomes).
The shared protocol sources it compiles — `fujimail.c`, `fujibus.c`,
`astromap.c`, `fujitcp.c` and their headers, plus the baked CONFIG client
`fujiconfigrom.h` — are staged verbatim from the pinned fujinet-firmware
checkout by `cmake/StageFujiProto.cmake` (never committed here; see
`core/astro/fuji-generated/`). fujinet-firmware is GPL-3.0-or-later. The pin
is the `astrocade-bringup` branch, recorded in `cmake/Dependencies.cmake` and
both flatpak manifests.

## The Bally system ROMs — NOT redistributed

The Astrocade on-board 8K BIOS (`astro.bin` and the Bally Computer System /
Home Library Computer variants `bioswhit.bin` / `ballyhlc.bin`) is copyrighted
Bally firmware with no redistribution grant.

- `-DWITH_ASTROCADE_ROMS=ON` embeds the BIOS so a development build boots
  straight to a live machine (or, with no cartridge, the FujiNet CONFIG
  client). **This is the local-development default and MUST NOT be
  redistributed.**
- `-DWITH_ASTROCADE_ROMS=OFF` embeds no ROM; the app imports the BIOS from the
  user's ROM directory at run time. **Every artifact this repository publishes
  is built this way.** `core/tests/no_embedded_roms.py`, registered as a ctest
  against each shipped binary, scans it for a distinctive slice of each BIOS
  and fails if any is present.

The baked FujiNet CONFIG client (`fujiconfigrom.h`) is FujiNet's own GPL code
and may be embedded in release builds; it is not a Bally ROM.
