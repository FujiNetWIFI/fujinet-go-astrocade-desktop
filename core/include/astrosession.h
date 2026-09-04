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
} astrosession_start_opts;

/* Fills *opts from the settings store: bios from "bios" (default ASTRO), exp
 * from "exp" (default NONE), cart_path from the "cart" key (NULL if unset).
 * A frontend applying a machine-option change should re-call this after
 * setting the relevant key, so a new option is read in one place. The
 * returned cart_path points into settings storage owned by s. */
void astrosession_default_opts(astrosession *s, astrosession_start_opts *opts);

/* Menu label tables, NULL past the end. */
const char *astrosession_bios_name(int idx);
const char *astrosession_exp_name(int idx);

/* ---- ROM provisioning --------------------------------------------------- */

/* 1 when the ROM directory (or an embedded table) holds a usable BIOS -- a
 * frontend refuses to start and prompts "Import System ROMs..." when 0. */
int astrosession_has_system_roms(const astrosession *s);

/* Copy a user-chosen file into the ROM directory, identifying it by size +
 * CRC32 against the three known BIOS dumps. Returns 0 and (if name_out) the
 * canonical filename it was stored as; -1 with astrosession_last_error() set
 * when the file is not a recognized Astrocade BIOS. */
int astrosession_import_rom(astrosession *s, const char *path,
                            char *name_out, int name_out_len);

/* ---- lifecycle ---------------------------------------------------------- */
int  astrosession_start(astrosession *s, const astrosession_start_opts *opts);
void astrosession_stop(astrosession *s);
int  astrosession_is_running(const astrosession *s);
const char *astrosession_last_error(const astrosession *s);

/* Stop, set the "cart" key to path (or clear it -> the baked CONFIG client),
 * flush, and start again. Loading a different cartridge is always
 * stop-then-start (the machine is a process singleton). 0 / -1+error. */
int astrosession_load_cart(astrosession *s, const char *path);
const char *astrosession_cart_path(astrosession *s);

/* The FujiNet web admin UI URL ("http://127.0.0.1:11501/"). A frontend opens
 * this in the system browser (never an embedded webview -- the config pages'
 * OAuth/JS flows need a real browser). Valid whether or not the runtime is up;
 * the page simply won't load until FujiNet is listening. */
const char *astrosession_fujinet_webui_url(astrosession *s);

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

void astrosession_keypad_set(astrosession *s, astrosession_key key, int pressed);

/* Hand controllers (players 0-3). The core applies the knob inversion. */
#define ASTROSESSION_HANDLE_UP      0x01
#define ASTROSESSION_HANDLE_DOWN    0x02
#define ASTROSESSION_HANDLE_LEFT    0x04
#define ASTROSESSION_HANDLE_RIGHT   0x08
#define ASTROSESSION_HANDLE_TRIGGER 0x10
void astrosession_handle_set(astrosession *s, int player, uint8_t mask);
void astrosession_knob_set(astrosession *s, int player, uint8_t value);

/* ---- system actions (fire/post/take latch) ------------------------------
 * RESET GAME and RESET TO CONFIG are machine-global. _fire joins threads
 * (stop/restart) so it must not be called from an SDL callback thread;
 * _post latches a bitmask a UI timer drains with _take + _fire. */
typedef enum {
    ASTROSESSION_SYSACT_RESET_GAME = 0,
    ASTROSESSION_SYSACT_RESET_CONFIG,
    ASTROSESSION_SYSACT_COUNT
} astrosession_sysaction;

void astrosession_sysaction_fire(astrosession *s, astrosession_sysaction a);
void astrosession_sysaction_post(astrosession *s, astrosession_sysaction a);
/* Returns a bitmask of posted actions and clears the latch (call from a UI
 * timer, then _fire each set bit). */
unsigned astrosession_sysaction_take(astrosession *s);

/* ---- keyboard -> keypad mapping (pure; shared by every frontend) --------
 * A frontend translates its toolkit key event to one of the ASTROSESSION_
 * KEYSYM_* symbols below, then astrosession_key_from_keysym() resolves the
 * default keypad key (or -1). F9/F12 are reserved for the frontends (keypad
 * window / debugger) and are not in the table. */
enum {
    ASTROSESSION_KEYSYM_NONE = 0,
    /* printable keys use their ASCII value directly (0x20-0x7e) */
    ASTROSESSION_KEYSYM_UP = 0x100, ASTROSESSION_KEYSYM_DOWN,
    ASTROSESSION_KEYSYM_LEFT, ASTROSESSION_KEYSYM_RIGHT,
    ASTROSESSION_KEYSYM_RETURN, ASTROSESSION_KEYSYM_BACKSPACE,
    ASTROSESSION_KEYSYM_ESCAPE, ASTROSESSION_KEYSYM_SPACE,
};

/* Returns the keypad key for a keysym under the default binding, or -1. */
int astrosession_key_from_keysym(int keysym);

#ifdef __cplusplus
}
#endif

#endif /* ASTROSESSION_H */
