This is the text of FujiNet Go Astrocade's built-in Help (Help ▸ FujiNet Go
Astrocade Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to
the frontend it is about; here you see them all. Every topic below is one page
in the app.

# Getting started

FujiNet Go Astrocade is a Bally Astrocade with a FujiNet cartridge in its slot.
The console is MAME's: the Z80, the data-chip video with its magic function
generator, the custom I/O sound chip, the calculator keypad and the hand
controllers. The FujiNet runs inside the app, so CONFIG comes up at once and can
load games from the network.

1. **Import the BIOS.** The console will not start without its BIOS, and none
   comes with the app. **Import BIOS…** takes your own copy. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a game in CONFIG.** Browse from the keypad: choose a host and an
   image on it, and FujiNet loads it into the cartridge. See
   [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…** takes an image
   file. See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window work the
   keypad and four hand controllers. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the machine.** [Preferences](#preferences) picks the BIOS and the
   RAM expansion, and sets up FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-astrocade-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**: see [Cartridges](#cartridges).
- **Import BIOS…** and **Import BIOS from a Folder…**: see
  [BIOS and ROMs](#bios-and-roms).
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **Reset Game** and **Reset to CONFIG**.
- **FujiNet Configuration…** and **FujiNet Console Log…**.
- **Preferences**, **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, View, FujiNet, Settings and Help. Preferences is in
the Settings menu.

<!-- end -->

<!-- only: windows -->

The menu bar has Machine, View, FujiNet and Help. The Machine menu has the
cartridge and BIOS items, Preferences and Exit.

<!-- end -->

<!-- only: macos -->

On a Mac, Settings and Quit are in the app menu, and the cartridge and BIOS
items are in the Machine menu. **Controllers** is ⌘K and **Reset to CONFIG**
⌘R.

<!-- end -->

<!-- context: nobios -->
## No BIOS

Until a BIOS is imported, the window shows **No Astrocade BIOS**, with an
**Import BIOS…** button. Bally's BIOS is copyrighted firmware, so the app
does not include it. See [BIOS and ROMs](#bios-and-roms).

## Reset

- **Reset Game** (<kbd>Backspace</kbd>) restarts the cartridge in the slot, the
  game or CONFIG, as the console's own reset does.
- **Reset to CONFIG** ejects an opened cartridge and boots FujiNet's CONFIG
  again.

# Controllers and keys

The Astrocade has a calculator-style keypad of 24 keys, and up to four hand
controllers, each a stick, a trigger and a knob.

## Default keys

The keypad's keys are on the keyboard where the calculator has them:

| Keypad | Keys |
|---|---|
| C, ↑, ↓, % | <kbd>C</kbd> <kbd>U</kbd> <kbd>D</kbd> <kbd>P</kbd> |
| MR, MS, CH, ÷ | <kbd>R</kbd> <kbd>S</kbd> <kbd>H</kbd> <kbd>/</kbd> |
| 7, 8, 9, × | <kbd>7</kbd> <kbd>8</kbd> <kbd>9</kbd> <kbd>X</kbd> |
| 4, 5, 6, − | <kbd>4</kbd> <kbd>5</kbd> <kbd>6</kbd> <kbd>-</kbd> |
| 1, 2, 3, + | <kbd>1</kbd> <kbd>2</kbd> <kbd>3</kbd> <kbd>=</kbd> |
| CE, 0, ., = | <kbd>E</kbd> <kbd>0</kbd> <kbd>.</kbd> <kbd>Enter</kbd> |

Player 1 plays from the keyboard:

| Player 1 | Key |
|---|---|
| Up, Down, Left, Right | the arrow keys |
| Trigger | <kbd>Space</kbd> |
| Knob left, Knob right | <kbd>[</kbd> <kbd>]</kbd> |

| Console | Key |
|---|---|
| Reset Game | <kbd>Backspace</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde macos -->

<kbd>Ctrl+R</kbd> is Reset to CONFIG.

<!-- end -->

<!-- only: gnome kde -->

<kbd>F11</kbd> toggles full screen.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. Every pad plays its own player, in the order it was plugged in: up to
four. The pad's <kbd>A</kbd> button is the trigger and its right stick's
left and right is the knob; the D-pad or the left stick steers. Other buttons
and axes can be given jobs in the Controllers window.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, click a control (a knob takes an axis), then press the keyboard key or
the gamepad button to give it. The control's old key or button is taken from it.
**Defaults** puts every control back. The map is kept in the settings, and only
what differs from the defaults is written.

<!-- context: keypad -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) shows the console's 24-key keypad, the
two system buttons (Reset Game and Reset to CONFIG) and all four hand
controllers, each with a stick, a trigger and a knob dial, and presses them with
the mouse. It floats at a fixed size, and stays hidden rather than closed. The
controls that are held light up in the accent colour, whoever holds them: the
keyboard, a gamepad or the mouse, so it is also an input tester. The dials show
the live knob.

| With the mouse | Does |
|---|---|
| Press a control | Holds it for as long as the button is down |
| Drag off a control | Lets it go |

The console reads its controls once a frame, so a control is *held*, not
pulsed: a value present for an instant falls between frames and a polling game
would never see it. Keys typed in the Controllers window go to the console too.
**Map** and **Defaults** change the controls (see
[Changing the controls](#changing-the-controls)).

# Machines

There is one machine: the Bally Astrocade, NTSC, with a Z80. Preferences pick
what is in it:

- **System ROM**: which on-board BIOS to boot: *Bally Professional Arcade
  (astro.bin)*, *Bally Home Library Computer (ballyhlc.bin)* or *Bally Computer
  System (bioswhit.bin)*. If the one chosen is not imported, the machine boots
  one that is.
- **RAM Expansion**: the extra RAM cartridge, if any: *None*, *Blue RAM 4K*,
  *Blue RAM 16K*, *Blue RAM 32K*, *Viper System 1 (16K)*, *Lil' WHITE RAM (32K)*
  or *R&L 64K*.

# BIOS and ROMs

Bally's BIOS is copyrighted firmware, so none comes with the app, and the
console will not start without one.

## Importing it

**Import BIOS…** takes your own copy: the single file, several files at once, a
zip such as MAME's `astrocde.zip`, or a folder that holds them (**Import BIOS
from a Folder…**). It is in the Machine menu, on the No Astrocade BIOS page and
on the ROMs page of Preferences. Each file is recognised by its size and
checksum whatever it is called, copied into the app's `roms` folder under its
own name, and the message says what is now available. The three BIOS files the
app knows are `astro.bin` (the Bally Professional Arcade), `ballyhlc.bin` (the
Bally Home Library Computer) and `bioswhit.bin` (the Bally Computer System).
Once one is in, the Astrocade starts.

# Cartridges

A cartridge is an image file. The cartridge in the slot is always the FujiNet
one: a game is loaded into it, from CONFIG or from a file.

## Open Cartridge

**Open Cartridge…** serves a local image from power-on in place of CONFIG. It is
remembered and starts again next time; **Reset to CONFIG** forgets it and boots
CONFIG again.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. The cartridge runs the
real cartridge firmware's own protocol sources, so it behaves as the hardware
does. It dials FujiNet over a local socket (BoIP, FujiBus over TCP), so the app
starts FujiNet first and waits for it.

## CONFIG

CONFIG is FujiNet's program for choosing what to run, driven from the keypad:

- **Hosts** are where games come from: the SD folder and servers on the
  network.
- Pick a host and an image on it. FujiNet loads it into the cartridge and the
  console starts it.
- **Reset to CONFIG** brings CONFIG back.

## The FujiNet menu

- **FujiNet Configuration…** opens FujiNet's own web page in your normal
  browser, at `http://127.0.0.1:11501/`. Only this computer can reach it.
- **FujiNet Console Log…** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the **Address:port** you give, for example
`127.0.0.1:11500`. Set that instance's `[BOIP]` listener to that address.
FujiNet Configuration and the console log belong to the built-in FujiNet, so
they say the FujiNet is external; use the other instance's own. The Debugger's
FujiNet tab still shows the traffic.

## Behind the scenes

- FujiNet's BoIP listener is on port 11500 and its web UI on 11501, on this
  computer only. They are the app's own, so a standalone `fujinet-pc` or
  another FujiNet Go app on the same computer does not collide with it.
- FujiNet's settings and its SD folder are in the app's `fujinet` folder.

# Preferences

Preferences set up the machine. A changed setting restarts the machine when
Preferences closes; a BIOS imported there starts it. The settings are one file,
shared by the GNOME and KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (⌘,).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **System ROM**: which on-board BIOS to boot: *Bally Professional Arcade
  (astro.bin)*, *Bally Home Library Computer (ballyhlc.bin)* or *Bally Computer
  System (bioswhit.bin)* (setting `bios`).
- **RAM Expansion**: the extra RAM cartridge, if any: *None*, *Blue RAM 4K*,
  *Blue RAM 16K*, *Blue RAM 32K*, *Viper System 1 (16K)*, *Lil' WHITE RAM
  (32K)* or *R&L 64K* (setting `exp`).

## FujiNet settings

- **Use external FujiNet**: skip the built-in FujiNet and connect to another
  instance instead (setting `fujinet_external`).
- **Address:port**: where to connect, for example `127.0.0.1:11500` (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

<!-- context: prefs/ROMs -->
## ROMs settings

The ROMs page lists the three BIOS files and whether each is imported:

- **Import…** and **Import a Folder…** add your own BIOS (see
  [Importing it](#importing-it)).
- **Bally Professional Arcade BIOS (astro.bin)**, **Bally Home Library Computer
  BIOS (ballyhlc.bin)** and **Bally Computer System BIOS (bioswhit.bin)**: each
  shows *Imported* or *Not imported*.

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `cart` (the opened cartridge,
empty for CONFIG) and `bindings` (the controls that differ from the defaults).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has ten tabs, and the registers show under all of them; closing it lets the
console go on as it was. Opening it stops the machine. **Pop Out** moves any
tab into a window of its own, and **Dock** (or closing that window) puts it
back.

| Button | Does |
|---|---|
| Run or Stop (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a call, `RST`, `DJNZ`, block operation or `HALT` |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Frame (<kbd>Shift+F7</kbd>) | On to the next frame |
| Run to Cursor | On to the selected line |
| Load Symbols | Reads `.sym`, `.map` and `.noi` files |
| Save | Saves the disassembly, memory, screen RAM or registers to a file |
| Help | Opens the topic of the tab that shows |
| Pop Out | Moves the tab that shows into its own window |

<!-- context: debugger/console -->
## Console

The Console takes commands, with completion on <kbd>Tab</kbd> and recall on
<kbd>Up</kbd> and <kbd>Down</kbd>. Numbers are `$C000`, `0xC000`, `C000h`,
`49152` or `%1010`; expressions can use registers, flags, `[addr]` and `{addr}`
for memory, labels, and C's operators. `help` lists the commands.

| Command | Does |
|---|---|
| `step`, `over`, `out` | One instruction; over a call; to the return |
| `run`, `go`, `stop` | Go on or break |
| `runto` | Runs to an address |
| `scanline`, `frame` | Runs some beam lines or frames |
| `break`, `bp` | An execute breakpoint |
| `bpr`, `bpw` | Memory reads or writes |
| `bpin`, `bpout` | I/O port reads or writes |
| `delete`, `enable`, `clear` | Manage breakpoints (also `disable`, `breaks`) |
| `print` | Evaluates an expression |
| `regs`, `set` | Shows or sets the registers |
| `mem`, `dump`, `poke` | Dumps memory; writes bytes |
| `vram`, `vpoke`, `video` | The screen RAM and the video chip |
| `io`, `map` | The memory map, ports and chip registers |
| `fujinet` | The FujiNet link |
| `label`, `labels`, `symbols` | Name an address; load labels |
| `disasm`, `dis` | Disassembles |
| `trace` | The instruction trace |
| `watch`, `unwatch` | The watch list: `watch score`, `unwatch 0` |
| `find` | Searches memory for bytes (`find 3E 5A`) or text (`find text HELLO`) |
| `cart` | The cartridge image and its banks |
| `save` | Saves state to a file |

<!-- context: debugger/disassembly -->
## Disassembly

The Z80 disassembled at the PC, annotated with any labels you load. The
current instruction is in the app's accent colour and breakpoints are red dots
(grey when disabled). Click a line to select it; double-click, <kbd>F9</kbd> or
the gutter toggles its breakpoint. **Follow PC** keeps the current line in
view, and every stop brings the view back to it, even if you had scrolled
away. Astrocade games run as bytecode the BIOS interprets, so the PC mostly
stays in the BIOS.

The registers and flags sit in a strip under the tabs. Edit a value while
stopped and press <kbd>Enter</kbd>.

<!-- context: debugger/memory -->
## Memory

The 64K the Z80 sees, 16 bytes to a row. **Go To** takes an address, a label,
an expression or a range (`$4000-$40FF`, `score..score+3`) and highlights what
it found. **Poke** writes bytes. The search bar finds hex bytes (`3E 5A`) or
text (not case-sensitive) with **Find Next** and **Find Previous**, wrapping
once; the machine has to be stopped.

<!-- context: debugger/video -->
## Video chip

The Astrocade's data-chip video: its registers, the colour palette and the
screen RAM as the chip draws it.

<!-- context: debugger/breakpoints -->
## Breakpoints

Execute breakpoints with conditions, address ranges and hit counts, and memory
read and write and port in and out watchpoints, each of which can be disabled.
A port breakpoint takes a port number (0 to 255). A condition can use `value`
and `addr`, and on a write `old`, the byte that was there before: `value != old`
stops only when a write changes something. The line at the top says why the
machine last stopped. **Add** makes a breakpoint, **Clear Hits** zeroes the
counts and **Clear All** removes them.

<!-- context: debugger/watches -->
## Watches

Things to keep an eye on: type an address, a label, an expression or a range
and press **Add**. Each shows its value while the machine is stopped (`-` while
it runs, `?` if it means nothing yet, which a later **Load Symbols** may fix).
**Memory** shows it in the Memory tab, **Break on Change** stops the machine
when something writes a different value there, and **Remove** drops it.

<!-- context: debugger/cart -->
## Cart and I/O

The cartridge: its file, the kind of image (flat 8K, a 256K or 512K game, or an
app with banked pages), its size, which pages are in the banks and whether the
FujiNet mailbox is live. Under it the memory map as the console sees it, then
its I/O ports and the chip registers.

<!-- context: debugger/trace -->
## Trace

Every instruction the Z80 runs, live. **Record** turns it on (it slows the
console a little, so it starts off), **Clear** empties it, and the filter
shows everything, only the jumps, calls and returns, or only a range of
addresses. Trace and History share one recording.

<!-- context: debugger/history -->
## History

The same recording, read at a stop: the lead-up to a breakpoint or a pause,
oldest first, with the registers at each instruction. Turn **Record** on
before you run to the place you want to look at.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link, both ways, named and in hex,
recorded from the moment the machine starts. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-astrocade/settings.ini` and
the data is in `~/.local/share/fujinet-go-astrocade` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-astrocade\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-astrocade`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-astrocade/settings.ini` and
the data is in `~/.local/share/fujinet-go-astrocade`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS files |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`) and FujiNet's data |

# Troubleshooting

- **No Astrocade BIOS**: none is imported. See
  [BIOS and ROMs](#bios-and-roms).
- **A BIOS was not recognised**: files are recognised by size and checksum;
  MAME's `astrocde.zip` and the three files above work.
- **link down**: FujiNet is off, not started yet, or the external address is
  wrong. Check Preferences ▸ FujiNet.
- **Keys don't reach the console**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A game never sees the controller**: a control must be held, not tapped in
  an instant. The Controllers window holds a control as long as the button is
  down.
- **A gamepad does nothing**: it may be playing another player, or its buttons
  may be given other jobs.
- **A port is taken**: another program is using 11500 or 11501. Close the other
  FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Astrocade is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MAME** (the Bally Astrocade driver, BSD-3-Clause) is what the console is
  modelled on, around floooh's `z80.h` (zlib).
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own protocol sources are the firmware's (BSD-3-Clause).
- No BIOS or other copyrighted ROM is part of the app; you import your own.
  "Bally" and "Astrocade" are trademarks of their owners, used to name the
  machine emulated; this project is not affiliated with them.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-astrocade-desktop](https://github.com/FujiNetWIFI/fujinet-go-astrocade-desktop)
