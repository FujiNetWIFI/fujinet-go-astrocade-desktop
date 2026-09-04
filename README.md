# FujiNet Go — Bally Astrocade

A self-contained Bally Astrocade with a built-in [FujiNet](https://fujinet.online/):
browse a network host from the console, boot images over the network, and let
the booted program keep talking to the network — all in one desktop app. The
newest member of the FujiNet Go desktop family (`fujinet-go-adam-desktop`,
`-apple2-`, `-coco-`, `-msx-`, `-intv-`).

## Status

Early. The emulator core and its FujiNet cartridge are built and validated;
the native frontends, the bundled FujiNet runtime, and packaging are in
progress. See `TODO`.

Done and verified:
- **Emulator core** — the Bally Astrocade home console (Z80, the data-chip
  video with its magic function generator, the custom I/O sound chip, the
  24-key keypad and hand controllers, RAM expansions), transposed from MAME's
  `astrocde` driver around floooh's cycle-stepped `z80.h`. Its rendered output
  is pixel-identical to MAME's (the boot menu and a running game were compared
  frame-for-frame), it paces at the real ~60.05 Hz, and it takes IM 2 vectored
  interrupts from the custom chip.
- **FujiNet cartridge** — the one cart device, transposed from the firmware's
  own MAME model and compiling the cartridge's own protocol sources verbatim
  (identical-by-construction). Serves plain ROMs, the 256K/512K homebrew
  mappers, and protocol-v2 banked images, and boots the baked CONFIG client
  when no cartridge is loaded.

## Building

```sh
cmake -B build -DFUJINET_SRC=/path/to/fujinet-firmware   # astrocade-bringup branch
cmake --build build
ctest --test-dir build --output-on-failure
```

`-DFRONTEND=none` builds just the core and its tests (what the snippet above
does until a frontend lands). The FujiNet firmware checkout is otherwise
provided automatically (a pinned submodule under `third_party/`), so a plain
`cmake -B build` works from a fresh clone.

### ROMs

The Astrocade BIOS is copyrighted Bally firmware and is not redistributed.
For local development, put `astro.bin` (and optionally the `ballyhlc.bin` /
`bioswhit.bin` variants) in `tools/roms/` and configure with
`-DWITH_ASTROCADE_ROMS=ON` to embed it. Published builds use
`-DWITH_ASTROCADE_ROMS=OFF` and import the BIOS from the user's ROM directory
at run time. See `COMPLIANCE.md`.

## Ports

FujiNet's BoIP (bus-over-IP) listener is on **11500** and its web admin UI on
**11501** — high ports of their own, so a standalone `fujinet-pc` and this app
can both run at once. The app opens the FujiNet config pages in your normal
browser (not an embedded webview) so their OAuth/JavaScript flows work.

## Layout

```
core/astro/        the emulator core (machine, video, sound, host, cart) + z80/
core/astro/fuji-generated/   staged FujiNet protocol sources (gitignored)
core/include/      the toolkit-agnostic session/debugger API
core/tests/        ctest suites
cmake/             dependency provisioning, protocol staging, FujiNet runtime
tools/             icon rendering, ROM embedding, the FujiNet runtime build
frontends/         gnome / kde / macos / windows (in progress)
build-aux/         flatpak manifests, the Windows installer
```

## License

GPL-3.0-or-later. See `LICENSE` and `COMPLIANCE.md`.
