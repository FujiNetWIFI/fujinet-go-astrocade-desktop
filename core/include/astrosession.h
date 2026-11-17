/*
 * astrosession -- toolkit-agnostic desktop session for FujiNet Go Astrocade.
 *
 * Owns the headless emulator core (driven on its own paced thread through
 * core/astro/host.h), the shared settings store, the ROM directory layout,
 * and the input/binding model. Frontends (GTK, Qt, AppKit, Win32) drive this
 * API and only do windowing, painting, and event translation.
 *
 * Like the CoCo/Intv ports: FujiNet listens and the emulator connects out to
 * it (the cart device is a BoIP TCP client -- see core/astro/fujinet_cart.c).
 * The FujiNet runtime build is landed in a later milestone; until then the
 * session still hands the cart its BoIP host:port and the mailbox simply runs
 * link-down when nothing is listening.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASTROSESSION_H
#define ASTROSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed geometry: the Astrocade data chip's visible raster (MAME's
 * set_raw(...,352,...,240)). Presented 4:3 by the frontends. */
#define ASTROSESSION_FB_WIDTH  352
#define ASTROSESSION_FB_HEIGHT 240

/* BoIP (Bus Over IP) loopback port -- FujiNet listens, the emulator connects
 * out -- and the FujiNet web admin UI port. High ports of their own so a
 * standalone fujinet-pc and this app can both run at once. */
#define ASTROSESSION_BOIP_PORT  11500
#define ASTROSESSION_WEBUI_PORT 11501

#define ASTROSESSION_AUDIO_RATE 48000

typedef struct astrosession astrosession;
/* The Z80 debugger engine (astrodebug.h). */
typedef struct astrodebug astrodebug;

/* All members optional (NULL = default).
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-astrocade
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-astrocade
 */
typedef struct {
    const char *config_dir;
    const char *data_dir;
} astrosession_paths;

/* Creates the session, provisions the config/data/ROM directories,
 * materialises any embedded BIOS (see WITH_ASTROCADE_ROMS) into the ROM
 * directory, and loads the shared settings store. Does not start emulation.
 * Returns NULL only on out-of-memory / unusable directories. */
astrosession *astrosession_new(const astrosession_paths *paths);
void          astrosession_free(astrosession *s);

/* ---- settings (shared INI; one store for all frontends/platforms) ------- */
int         astrosession_get_int(astrosession *s, const char *key, int def);
void        astrosession_set_int(astrosession *s, const char *key, int value);
const char *astrosession_get_str(astrosession *s, const char *key,
                                 const char *def);
void        astrosession_set_str(astrosession *s, const char *key,
                                 const char *value);
void        astrosession_settings_flush(astrosession *s);

/* ---- machine options ---------------------------------------------------- */

/* BIOS variant. The three console dumps are interchangeable at the menu
 * level; ASTRO is the common Bally Professional Arcade one. */
enum {
    ASTROSESSION_BIOS_ASTRO = 0,    /* astro.bin  (Bally Professional Arcade) */
    ASTROSESSION_BIOS_BALLYHLC,     /* ballyhlc.bin (Home Library Computer) */
    ASTROSESSION_BIOS_BIOSWHIT,     /* bioswhit.bin (Bally Computer System) */
};

/* RAM expansion (plain RAM only; the Blue RAM's I/O ports and cassette are
 * not modeled -- see core/astro/machine.c). */
enum {
    ASTROSESSION_EXP_NONE = 0,
    ASTROSESSION_EXP_BLUE_RAM_4K,
    ASTROSESSION_EXP_BLUE_RAM_16K,
    ASTROSESSION_EXP_BLUE_RAM_32K,
    ASTROSESSION_EXP_VIPER_SYS1,
    ASTROSESSION_EXP_LIL_WHITE_RAM,
    ASTROSESSION_EXP_RL64_RAM,
};

typedef struct {
    int bios;   /* ASTROSESSION_BIOS_* */
    int exp;    /* ASTROSESSION_EXP_* */
    const char *cart_path; /* NULL/"" -> the FujiNet cart boots its baked
                            * CONFIG client; otherwise a cartridge image is
                            * loaded into the FujiNet cart device. Not read
                            * from settings by astrosession_default_opts;
                            * callers persisting a "last cart" set it. */
    int external_fujinet;   /* 1: don't start the built-in FujiNet; dial the
                             * one listening at boip_connect instead */
    const char *boip_connect; /* "host:port" of that FujiNet (external only);
                               * NULL/"" = 127.0.0.1:11500 */
    int enable_gamepad;       /* start the SDL gamepad thread */
} astrosession_start_opts;

/* Fills *opts from the settings store: bios from "bios" (default ASTRO), exp
 * from "exp" (default NONE), cart_path from the "cart" key (NULL if unset),
 * enable_gamepad from "enable_gamepad" (default 1).
 * A frontend applying a machine-option change should re-call this after
 * setting the relevant key, so a new option is read in one place. The
 * returned cart_path points into settings storage owned by s. */
void astrosession_default_opts(astrosession *s, astrosession_start_opts *opts);

/* Menu label tables, NULL past the end. */
const char *astrosession_bios_name(int idx);
const char *astrosession_exp_name(int idx);

/* ---- ROM provisioning --------------------------------------------------- */

/* 1 when the ROM directory (or an embedded table) holds a usable BIOS -- a
 * frontend shows its "No Astrocade BIOS" page instead of the picture when 0. */
int astrosession_has_system_roms(const astrosession *s);

/* The ROMs page's list: one item per BIOS, index-aligned with
 * ASTROSESSION_BIOS_* (item i is variant i), all in the group "BIOS". */
typedef struct { const char *name; const char *group; } astro_rom_item;
int astrosession_rom_item_count(void);
const astro_rom_item *astrosession_rom_item(int i);      /* NULL past the end */
/* 1 when item i is imported (or embedded). */
int astrosession_rom_item_available(const astrosession *s, int i);

/* The variant a start will boot: wanted when it is available, else the first
 * one that is; -1 when there is none. */
int astrosession_resolve_bios(const astrosession *s, int wanted);

/* Imports BIOS images from a file, a zip (MAME's astrocde set works) or a
 * folder, recognising them by size and CRC32 whatever they are called and
 * copying each into the ROM directory under its own name. Returns 0 when at
 * least one file was written, -1 otherwise; msg says what is now available
 * (or why nothing was imported). */
int astrosession_rom_import(astrosession *s, const char *path, char *msg, int msgsz);

/* The ROM directory imports are copied into. */
const char *astrosession_roms_path(const astrosession *s);

/* ---- lifecycle ---------------------------------------------------------- */
int  astrosession_start(astrosession *s, const astrosession_start_opts *opts);
void astrosession_stop(astrosession *s);

/* The debugger engine (astrodebug.h): one per process, created on first use,
 * alive across session restarts with its breakpoints and attach state. */
astrodebug *astrosession_debugger(astrosession *s);
int  astrosession_is_running(const astrosession *s);
const char *astrosession_last_error(const astrosession *s);

/* Stop, set the "cart" key to path (or clear it -> the baked CONFIG client),
 * flush, and start again. Loading a different cartridge is always
 * stop-then-start (the machine is a process singleton). 0 / -1+error. */
int astrosession_load_cart(astrosession *s, const char *path);
const char *astrosession_cart_path(astrosession *s);

/* Parses the "Use external FujiNet" address field: "host:port" (host a name
 * or an IPv4 literal) or a bare "port". Empty text means the default,
 * 127.0.0.1:11500. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int astrosession_parse_hostport(const char *text, char *host, int hostsz,
                                int *port);

/* The FujiNet web admin UI URL ("http://127.0.0.1:11501/"). A frontend opens
 * this in the system browser (never an embedded webview -- the config pages'
 * OAuth/JS flows need a real browser). Valid whether or not the runtime is up;
 * the page simply won't load until FujiNet is listening. Empty while the
 * session uses an external FujiNet (that one has its own web UI). */
const char *astrosession_fujinet_webui_url(astrosession *s);

/* Copy the FujiNet runtime's recent console log into dst (for a log viewer).
 * Returns bytes written; 0 when the runtime is not loaded. */
int astrosession_fujinet_copy_log(astrosession *s, char *dst, int max);

/* 1 when this app's FujiNet runtime is up. */
int astrosession_fujinet_running(const astrosession *s);
/* 1 when the session was started to dial an external FujiNet. */
int astrosession_fujinet_external(const astrosession *s);
/* "host:port" the cartridge dials (the running session's, or the default
 * 127.0.0.1:11500 before the first start). */
const char *astrosession_boip_endpoint(const astrosession *s);
/* 1 when the FujiNet cartridge's BoIP link is connected. */
int astrosession_fujinet_link(const astrosession *s);

/* RESET TO CONFIG: clear the persisted cartridge and restart, booting the
 * FujiNet cart's baked CONFIG client -- the only way back to CONFIG once a
 * booted image has taken over the mailbox for the session. 0 / -1+error. */
int astrosession_reset_to_config(astrosession *s);

/* RESET GAME: the console panel RESET. Releases every held input, then arms
 * the emulator's one-shot reset -- CPU + sound restart while screen RAM and
 * the whole cart device (live/staged images, mailbox) survive, exactly as on
 * hardware. Does not touch the "cart" key, does not restart FujiNet, does not
 * block. 0 if running, no-op otherwise. */
int astrosession_reset_game(astrosession *s);

/* ---- video: latest-wins single slot ------------------------------------- */
int astrosession_copy_frame(astrosession *s, uint32_t *dst,
                            uint64_t *serial_inout);
/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the machine thread after each new frame
 * is published. Keep it quick (post an event and return) and do not call into
 * the session from it. NULL stops the calls and returns only after any call
 * in flight has finished, so ctx may be freed right after. */
typedef void (*astrosession_frame_cb)(void *ctx);
void astrosession_set_frame_callback(astrosession *s, astrosession_frame_cb cb,
                                     void *ctx);
/* Frames the machine has run since it started, including those identical to
 * their predecessor (not published again): "is the machine running", not
 * "is the picture changing". */
uint64_t astrosession_frame_count(astrosession *s);

/* ---- audio: drained on demand, ASTROSESSION_AUDIO_RATE mono int16 -------- */
int astrosession_render_audio(astrosession *s, int16_t *dst, int max_samples);

/* ---- input -------------------------------------------------------------- */

/* The 24 keypad keys, row-major from the top-left, matching the console's
 * physical layout (the rightmost column is the gold divide/x/-/+/= side):
 *
 *   C   UP  DOWN %          <- row 0
 *   MR  MS  CH   DIV        <- row 1
 *   7   8   9    MUL        <- row 2
 *   4   5   6    MINUS      <- row 3
 *   1   2   3    PLUS       <- row 4
 *   CE  0   DOT  EQ         <- row 5
 *
 * The core maps the physical (col,row) to the console's reversed port order
 * (leftmost column reads at 0x17). See core/astro/host.h. */
typedef enum {
    ASTROSESSION_KEY_C = 0, ASTROSESSION_KEY_UP, ASTROSESSION_KEY_DOWN, ASTROSESSION_KEY_PCT,
    ASTROSESSION_KEY_MR, ASTROSESSION_KEY_MS, ASTROSESSION_KEY_CH, ASTROSESSION_KEY_DIV,
    ASTROSESSION_KEY_7, ASTROSESSION_KEY_8, ASTROSESSION_KEY_9, ASTROSESSION_KEY_MUL,
    ASTROSESSION_KEY_4, ASTROSESSION_KEY_5, ASTROSESSION_KEY_6, ASTROSESSION_KEY_MINUS,
    ASTROSESSION_KEY_1, ASTROSESSION_KEY_2, ASTROSESSION_KEY_3, ASTROSESSION_KEY_PLUS,
    ASTROSESSION_KEY_CE, ASTROSESSION_KEY_0, ASTROSESSION_KEY_DOT, ASTROSESSION_KEY_EQ,
    ASTROSESSION_KEY_COUNT   /* trailing count for bindings/keypad walks */
} astrosession_key;


/* Hand controllers (players 0-3): an 8-way stick, a trigger and a knob. */
#define ASTROSESSION_PLAYERS        4
#define ASTROSESSION_HANDLE_UP      0x01
#define ASTROSESSION_HANDLE_DOWN    0x02
#define ASTROSESSION_HANDLE_LEFT    0x04
#define ASTROSESSION_HANDLE_RIGHT   0x08
#define ASTROSESSION_HANDLE_TRIGGER 0x10

/* The knob is a pot: an absolute position 0-255 (turning right increases)
 * that HOLDS where it was left. Whoever moved it last wins: a gamepad axis,
 * the panel's dial (astrosession_knob_set), or the knob keys, which ramp it. */
void astrosession_knob_set(astrosession *s, int player, uint8_t value);
int  astrosession_knob_get(astrosession *s, int player);
#define ASTROSESSION_KNOB_CENTRE 128
/* The panel dial's geometry, shared so the four toolkits cannot drift: the
 * knob value for a point (dx, dy) from the dial's centre, y down. The dial
 * sweeps 270 degrees, 0 at the lower left, 255 at the lower right, centre at
 * the top. Returns -1 inside the dead centre (radius * 0.2). */
int  astrosession_knob_from_point(double dx, double dy, double radius);
/* The inverse: the angle in degrees clockwise from straight up, -135..135. */
double astrosession_knob_angle(int value);

/* ---- system actions (fire/post/take latch) ------------------------------
 * RESET GAME and RESET TO CONFIG are machine-global. _fire joins threads
 * (stop/restart) so it must not be called from an SDL callback thread;
 * _post latches a bitmask a UI timer drains with _take + _fire. */
typedef enum {
    ASTROSESSION_SYSACT_RESET_GAME = 0,
    ASTROSESSION_SYSACT_RESET_CONFIG,
    ASTROSESSION_SYSACT_DEBUG_STOP,   /* break into the debugger at the next instruction */
    ASTROSESSION_SYSACT_COUNT
} astrosession_sysaction;

void astrosession_sysaction_fire(astrosession *s, astrosession_sysaction a);
void astrosession_sysaction_post(astrosession *s, astrosession_sysaction a);
/* Returns a bitmask of posted actions and clears the latch (call from a UI
 * timer, then _fire each set bit). */
unsigned astrosession_sysaction_take(astrosession *s);

/* ---- binding targets ----------------------------------------------------
 * Everything a key, a pad button or a pad axis can drive: the 24 keypad
 * keys, the 3 system actions, then 8 actions for each of the 4 players. */
typedef enum {
    ASTRO_ACT_UP = 0, ASTRO_ACT_DOWN, ASTRO_ACT_LEFT, ASTRO_ACT_RIGHT,
    ASTRO_ACT_TRIGGER,
    ASTRO_ACT_KNOB_DEC,       /* ramp the knob left while held */
    ASTRO_ACT_KNOB_INC,       /* ramp the knob right while held */
    ASTRO_ACT_KNOB,           /* the knob itself: a pad axis only */
    ASTRO_ACT_PER_PLAYER
} astro_action;
#define ASTRO_TARGET_KEYPAD   0
#define ASTRO_TARGET_SYSACT   ASTROSESSION_KEY_COUNT
#define ASTRO_TARGET_PLAYERS  (ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_COUNT)
#define ASTRO_TARGET_PLAYER(p, a) \
    (ASTRO_TARGET_PLAYERS + (p) * ASTRO_ACT_PER_PLAYER + (a))
#define ASTRO_TARGET_COUNT \
    (ASTRO_TARGET_PLAYERS + ASTROSESSION_PLAYERS * ASTRO_ACT_PER_PLAYER)

/* ---- driving the machine --------------------------------------------------
 * Each input source (the panel, the keyboard, up to 4 gamepads) keeps its own
 * holds; the machine sees their union, so one source releasing can never
 * cancel another's hold. */

/* The Controllers window's mouse pressing or releasing a target (keypad key
 * or a player's stick/trigger/knob-key): the POINTER source. System actions
 * are not pressed this way: use astrosession_sysaction_fire. */
void astrosession_press(astrosession *s, int target, int down);
/* The mouse's whole hold on one player's stick, trigger and knob keys at once
 * (a drag across the stick's wedges): bit n of act_mask is astro_action n
 * (UP..KNOB_INC), exactly those are held by the pointer and the rest of that
 * player's are let go. ASTRO_ACT_KNOB (bit 7) is ignored: use knob_set. */
void astrosession_pointer_set_player(astrosession *s, int player, unsigned act_mask);
/* Let go of everything the pointer holds (mouse up, focus lost, window
 * hidden). The keyboard's and the pads' holds are untouched. */
void astrosession_pointer_release(astrosession *s);
/* 1 while any source holds the target (the panel paints it lit). */
int  astrosession_target_held(astrosession *s, int target);
/* Release everything the keyboard holds (focus lost), and stop typing for the
 * overlay (below). The pointer's and the pads' holds are left alone. */
void astrosession_release_all(astrosession *s);

/* A key event, as a keysym (below). 1 = the key is bound and was consumed.
 * A key bound to a system action POSTS it: call astrosession_sysaction_drain
 * afterwards, from the UI thread. Call for every press and release; with
 * ctrl_down a press is never consumed (menu accelerators win), but a release
 * always clears what its press set. F9, F11 and F12 are never bound.
 * With the Bally BASIC overlay on, a printable key bound to a keypad key is
 * NOT consumed (0): the frontend then passes the character the key produced
 * to astrosession_type_char. */
int  astrosession_input_key(astrosession *s, uint32_t keysym, int down,
                            int ctrl_down);
/* take + fire everything posted. UI thread only. */
void astrosession_sysaction_drain(astrosession *s);

/* Keysyms are X11's, and name the PHYSICAL key (the unshifted US-layout
 * symbol), so Shift or NumLock never changes what a key is. */
enum {
    ASTROSESSION_KEYSYM_NONE = 0,
    ASTROSESSION_KEYSYM_UP = 0xff52, ASTROSESSION_KEYSYM_DOWN = 0xff54,
    ASTROSESSION_KEYSYM_LEFT = 0xff51, ASTROSESSION_KEYSYM_RIGHT = 0xff53,
    ASTROSESSION_KEYSYM_ESCAPE = 0xff1b, ASTROSESSION_KEYSYM_RETURN = 0xff0d,
    ASTROSESSION_KEYSYM_BACKSPACE = 0xff08, ASTROSESSION_KEYSYM_TAB = 0xff09,
    ASTROSESSION_KEYSYM_SPACE = 0x20,
    ASTROSESSION_KEYSYM_F1 = 0xffbe, ASTROSESSION_KEYSYM_F2, ASTROSESSION_KEYSYM_F3,
    ASTROSESSION_KEYSYM_F4, ASTROSESSION_KEYSYM_F5, ASTROSESSION_KEYSYM_F6,
    ASTROSESSION_KEYSYM_F7, ASTROSESSION_KEYSYM_F8, ASTROSESSION_KEYSYM_F9,
    ASTROSESSION_KEYSYM_F10, ASTROSESSION_KEYSYM_F11, ASTROSESSION_KEYSYM_F12,
    ASTROSESSION_KEYSYM_LSHIFT = 0xffe1, ASTROSESSION_KEYSYM_RSHIFT = 0xffe2,
    ASTROSESSION_KEYSYM_LCTRL = 0xffe3, ASTROSESSION_KEYSYM_RCTRL = 0xffe4,
    ASTROSESSION_KEYSYM_LALT = 0xffe9, ASTROSESSION_KEYSYM_RALT = 0xffea,
    ASTROSESSION_KEYSYM_KP_0 = 0xffb0, ASTROSESSION_KEYSYM_KP_1,
    ASTROSESSION_KEYSYM_KP_2, ASTROSESSION_KEYSYM_KP_3, ASTROSESSION_KEYSYM_KP_4,
    ASTROSESSION_KEYSYM_KP_5, ASTROSESSION_KEYSYM_KP_6, ASTROSESSION_KEYSYM_KP_7,
    ASTROSESSION_KEYSYM_KP_8, ASTROSESSION_KEYSYM_KP_9,
    ASTROSESSION_KEYSYM_KP_ENTER = 0xff8d, ASTROSESSION_KEYSYM_KP_MULTIPLY = 0xffaa,
    ASTROSESSION_KEYSYM_KP_DIVIDE = 0xffaf, ASTROSESSION_KEYSYM_KP_PERIOD = 0xffae
};
/* A key with no X11 keysym of its own is still bindable: 0x10000 + HID usage. */
#define ASTROSESSION_KEYSYM_HID_BASE 0x10000u
#define ASTROSESSION_HID_USAGE_MAX   0xE7u
/* Native key codes -> keysym, 0 when unknown: Windows scan code (set 1, with
 * the E0 flag = lParam bit 24), Linux evdev code (GTK/Qt keycode minus 8) and
 * macOS virtual key code. */
uint32_t astrosession_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t astrosession_keysym_from_evdev(unsigned code);
uint32_t astrosession_keysym_from_macos_keycode(unsigned keycode);
/* "Space", "Keypad 5", "F1", "A"; returns the length. */
int astrosession_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- gamepads -------------------------------------------------------------
 * USB and Bluetooth pads, mapped or not (a plain HID adapter is opened as a
 * raw joystick), drive the four hand controllers. A pad plays the first
 * player nobody is assigned to, in connection order, unless it is assigned
 * explicitly. An explicit assignment is remembered (setting "pad_assign": a
 * pad's GUID, else its name) and applies again when the pad is plugged back in
 * or the app restarted. Every button goes through the bindings; the left
 * stick, a raw joystick's first hat and (by default binding) the D-pad steer;
 * the two triggers are bindable buttons; the knob follows one axis
 * absolutely.
 *
 * A pad INPUT id is a button, in one of three bands, or (for the knob only) an
 * axis: the named buttons 0..ASTRO_PAD_BTN_COUNT-1, the raw buttons of an
 * unmapped joystick (64..127: button index + 64), a raw joystick's hat
 * positions (192..255: hat * 8 + direction), or ASTRO_PAD_AXIS_BASE + an
 * SDL_GamepadAxis index (LEFTX LEFTY RIGHTX RIGHTY LTRIGGER RTRIGGER), which
 * only the KNOB target takes. -1 = none. Pad `idx` below is its slot in the
 * connection order, 0 .. count - 1. */
typedef enum {
    ASTRO_PAD_BTN_NONE = -1,
    ASTRO_PAD_BTN_SOUTH = 0, ASTRO_PAD_BTN_EAST, ASTRO_PAD_BTN_WEST,
    ASTRO_PAD_BTN_NORTH, ASTRO_PAD_BTN_BACK, ASTRO_PAD_BTN_GUIDE,
    ASTRO_PAD_BTN_START, ASTRO_PAD_BTN_LEFT_STICK, ASTRO_PAD_BTN_RIGHT_STICK,
    ASTRO_PAD_BTN_LEFT_SHOULDER, ASTRO_PAD_BTN_RIGHT_SHOULDER,
    ASTRO_PAD_BTN_DPAD_UP, ASTRO_PAD_BTN_DPAD_DOWN, ASTRO_PAD_BTN_DPAD_LEFT,
    ASTRO_PAD_BTN_DPAD_RIGHT,
    ASTRO_PAD_BTN_LEFT_TRIGGER, ASTRO_PAD_BTN_RIGHT_TRIGGER,
    ASTRO_PAD_BTN_COUNT,
    ASTRO_PAD_BTN_RAW_BASE = 64, ASTRO_PAD_BTN_RAW_LAST = 127,
    ASTRO_PAD_HAT_BASE = 192, ASTRO_PAD_HAT_LAST = 255
} astro_pad_button;
#define ASTRO_PAD_HAT_DIRS 8
#define ASTRO_PAD_BTN_IS_NAMED(btn) ((btn) >= 0 && (btn) < ASTRO_PAD_BTN_COUNT)
#define ASTRO_PAD_BTN_IS_RAW(btn)   ((btn) >= ASTRO_PAD_BTN_RAW_BASE && (btn) <= ASTRO_PAD_BTN_RAW_LAST)
#define ASTRO_PAD_BTN_IS_HAT(btn)   ((btn) >= ASTRO_PAD_HAT_BASE && (btn) <= ASTRO_PAD_HAT_LAST)
#define ASTRO_PAD_AXIS_BASE   128
#define ASTRO_PAD_AXIS_COUNT  6
#define ASTRO_PAD_IS_AXIS(id) ((id) >= ASTRO_PAD_AXIS_BASE && (id) < ASTRO_PAD_AXIS_BASE + ASTRO_PAD_AXIS_COUNT)

/* "A", "D-pad Up", "Button 3", "Hat 0 Up-Right", "Right Stick X"; returns the
 * length. */
int  astrosession_pad_input_name(int id, char *dst, int dstsz);
int  astrosession_gamepad_count(astrosession *s);
/* Name of connected pad idx into dst; returns its length, or 0. */
int  astrosession_gamepad_name(astrosession *s, int idx, char *dst, int dstsz);
/* player: 0..3 for that hand controller, -1 for automatic. Remembered. */
void astrosession_gamepad_assign(astrosession *s, int idx, int player);
/* The explicit player (0..3), or -1 for automatic. */
int  astrosession_gamepad_assignment(astrosession *s, int idx);
/* The player the pad drives right now (0..3), or -1 for none. */
int  astrosession_gamepad_effective_port(astrosession *s, int idx);
/* Bumped on every pad added or removed (and carried across a restart of the
 * session), so a frontend can refresh its lists and toasts cheaply. */
unsigned astrosession_gamepad_generation(astrosession *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (player 1)",
 * "Disconnected: ..."), for a toast; returns the length, 0 if none yet. */
int  astrosession_gamepad_last_event(astrosession *s, char *dst, int dstsz);

/* ---- pad-input capture, for the Controllers window's Map mode -------------
 * A frontend arming a remap to a pad input calls _begin, then polls _poll on
 * a UI timer (pad events land on the session's own poll thread); _poll
 * returns 1 with *id set once a button is pressed, then disarms. _cancel
 * disarms without capturing. While armed every pad button event is swallowed,
 * so the press that maps a button does not also do what it used to. For the
 * KNOB target use _begin_axis: it records an axis deflected past 60% instead. */
void astrosession_gamepad_capture_begin(astrosession *s);
void astrosession_gamepad_capture_begin_axis(astrosession *s);
int  astrosession_gamepad_capture_poll(astrosession *s, int *id);
void astrosession_gamepad_capture_cancel(astrosession *s);

/* ---- the on-screen controllers (controls.c) ----------------------------------
 * One layout for the four toolkits, and its hit test. Units: a keypad key's
 * width is 1.0; a toolkit adds its own margin and scale. No colours here. The
 * console's 24-key keypad sits on top, the four pistol grips (player 1 on the
 * left) in a row below, and the console's two buttons at the bottom. Entries
 * are in paint order. Wedges are annular sectors, so their rectangles are only
 * bounding boxes: paint a wedge as the sector of its STICK disc whose
 * direction is its act_mask, between half the disc's radius and all of it, and
 * hit-test with astrosession_controls_hit (exact shapes). */
#define ASTROSESSION_ACCENT_RGB 0xE8C14Au

typedef enum {
    ASTRO_CTL_KEY = 0,      /* a keypad key (target = ASTRO_TARGET_KEYPAD + k) */
    ASTRO_CTL_STICK,        /* a player's stick: the disc (never pressed itself) */
    ASTRO_CTL_DIRECTION,    /* one of its eight wedges (act_mask = the astro_action bits) */
    ASTRO_CTL_TRIGGER,      /* the trigger (target = the player's TRIGGER) */
    ASTRO_CTL_KNOB,         /* the knob's dial (drag: astrosession_knob_from_point) */
    ASTRO_CTL_SYSBUTTON,    /* RESET GAME / Reset to CONFIG / Stop in Debugger */
    ASTRO_CTL_OVERLAY,      /* the Bally BASIC overlay card's frame (drawn, never pressed) */
    ASTRO_CTL_CAPTION       /* text: "Player 1" */
} astro_control_kind;

typedef struct {
    int kind;               /* astro_control_kind */
    const char *label;      /* "7", "Trigger", "Player 1" */
    int player;             /* 0..3, or -1 for the console */
    int target;             /* ASTRO_TARGET_*, or -1 (stick, diagonals, knob, overlay, caption) */
    unsigned act_mask;      /* astro_action bits it drives (wedges) */
    float xpos, ypos, width, height;   /* layout units, from the top-left */
} astro_control;

const astro_control *astrosession_controls_layout(int *count, float *width,
                                                  float *height);
/* The topmost pressable control under a point (layout units), or -1. */
int      astrosession_controls_hit(float xpos, float ypos);
/* The stick's direction bits (astro_action UP/DOWN/LEFT/RIGHT) for a pointer
 * at (delx, dely) from the disc's centre (+x right, +y toward the player, any
 * unit) with `radius` the disc's reach: nothing inside a quarter of it, else
 * one of eight 45-degree sectors. */
unsigned astrosession_stick_from_vector(float delx, float dely, float radius);

/* ---- the Bally BASIC overlay (overlay.c) ------------------------------------
 * Bally BASIC (and Astro BASIC) are typed on the 24-key keypad with a printed
 * overlay card: each key carries several legends, chosen by the shift colour
 * pressed before it. With the overlay on, the Controllers window paints those
 * legends on the keys, and the host keyboard types BASIC: a printable
 * character (astrosession_type_char) becomes the shift key(s) and key that
 * produce it. While it is on, host keys bound to a keypad key type their
 * character instead of pressing that key; the stick, trigger, knob and console
 * bindings are unchanged. The setting is remembered ("overlay_bally_basic");
 * loading a cartridge whose CRC is a known Bally BASIC dump turns it on. */
#define ASTRO_OVERLAY_SHIFTS 5
/* The legend columns, and the shift each needs: a legend in a colour is typed
 * by tapping the shift key of that colour, then the key. */
enum { ASTRO_SHIFT_NONE = 0, ASTRO_SHIFT_GREEN, ASTRO_SHIFT_RED, ASTRO_SHIFT_BLUE,
       ASTRO_SHIFT_WORDS };
typedef struct {
    const char *legend[ASTRO_OVERLAY_SHIFTS];   /* by ASTRO_SHIFT_*; NULL prints nothing;
                                                 * UTF-8 (the divide and multiply signs, arrows) */
} astro_overlay_key;
/* The legends of the 24 keypad keys, indexed by astrosession_key. The bottom
 * row (CE 0 DOT EQ) prints none: those four are the shifts. */
const astro_overlay_key *astrosession_overlay_layout(void);
/* The physical key that is the shift (CE green, 0 red, DOT blue, EQ WORDS), or
 * -1 for ASTRO_SHIFT_NONE; and the shift a key is, or -1 if it is not one. */
int  astrosession_overlay_shift_key(int shift);
int  astrosession_overlay_key_shift(int key);
/* The keypad taps that type a character (a Unicode code point, ASCII only;
 * lower case types the capital; Enter = GO, Backspace = ERASE, space = SPACE)
 * or a BASIC word ("PRINT", case-insensitive; the gold words): written to
 * keys[] (at most `max`, 2 is enough); returns how many, 0 if the overlay has
 * no way to type it. Pure: no session. */
int  astrosession_overlay_sequence(uint32_t codepoint, uint8_t *keys, int max);
int  astrosession_overlay_word_sequence(const char *word, uint8_t *keys, int max);
void astrosession_overlay_set(astrosession *s, int on);   /* remembered */
int  astrosession_overlay_get(astrosession *s);
/* Frontends toast "Bally BASIC overlay on" when this changes: bumped each time
 * a cartridge that is a known Bally BASIC interpreter is loaded (and so turns
 * the overlay on). */
unsigned astrosession_overlay_auto_generation(astrosession *s);
/* A character the user typed (a Unicode code point, shift-aware: the character
 * the key produced). With the overlay on and a keypad sequence for it, queues
 * the sequence (paced a few frames per press) and returns 1; else returns 0
 * and nothing happens. Call it for a key astrosession_input_key did not
 * consume. */
int  astrosession_type_char(astrosession *s, uint32_t codepoint);
/* The same for a gold BASIC word: WORDS, then its key. */
int  astrosession_type_word(astrosession *s, const char *word);
/* Pending typed presses (for tests and a busy cursor); 0 when idle. */
int  astrosession_typing_pending(astrosession *s);

#ifdef __cplusplus
}
#endif

#endif /* ASTROSESSION_H */
