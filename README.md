# FujiNet Go — Bally Astrocade

A self-contained Bally Astrocade with a built-in [FujiNet](https://fujinet.online/):
browse a network host from the console, boot images over the network, and let
the booted program keep talking to the network — all in one desktop app. The
newest member of the FujiNet Go desktop family (`fujinet-go-adam-desktop`,
`-apple2-`, `-coco-`, `-msx-`, `-intv-`).

## Status

Complete. All targets build; the Linux (GNOME + KDE) and Windows frontends are
run-verified on the dev machine, macOS is validated on CI. See `TODO` for the
milestone-by-milestone log.

- **Emulator core** — the Bally Astrocade home console (Z80, the data-chip
  video with its magic function generator, the custom I/O sound chip, the
  24-key keypad and hand controllers, RAM expansions), transposed from MAME's
  `astrocde` driver around floooh's cycle-stepped `z80.h`. Its rendered output
  is pixel-identical to MAME's (the boot menu and a running game were compared
  frame-for-frame), it paces at the real ~60 Hz, and it takes IM 2 vectored
  interrupts. Verified Windows-portable under Wine.
- **FujiNet** — the cartridge device compiles the firmware's own protocol
  sources verbatim (identical-by-construction) and dials a real fujinet-pc
  built in-process as `libfujinet` (BoIP on **11500**, web UI on **11501**).
  The bundled CONFIG client browses hosts end-to-end through the cart against a
  live TNFS host.
- **Frontends** — GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), Windows (Win32/
  GDI) and macOS (AppKit), each with the emulator display, the 24-key keypad
  window (gold column, RESET / RESET TO CONFIG), a two-tab debugger exposing
  every Z80 and video-chip register, gamepad hand controllers, ROM import, and
  a FujiNet console-log viewer. Config pages open in the system browser.
- **Packaging** — per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows NSIS
  installer, and a signed macOS bundle, all wired through GitHub Actions.

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
