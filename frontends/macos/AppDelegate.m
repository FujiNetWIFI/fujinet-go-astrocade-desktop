/*
 * AppDelegate -- the main window (DisplayView), the app menu, the keypad
 * window, and the FujiNet/ROM actions. NOT RUN-VERIFIED on macOS in this
 * environment; transposed from the verified GNOME/KDE/Win32 frontends to be
 * validated on the macOS CI runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import "AstroKeyForward.h"
#import "DisplayView.h"

#include "bindings.h"

/* ---- a keypad button that reports press AND release ---- */

@interface PadButton : NSButton
@property (nonatomic) int astroKey;
@property (nonatomic, assign) astrosession *session;
@end

@implementation PadButton
- (void)mouseDown:(NSEvent *)e {
    astrosession_keypad_set(self.session, (astrosession_key)self.astroKey, 1);
    [super mouseDown:e];
}
- (void)mouseUp:(NSEvent *)e {
    astrosession_keypad_set(self.session, (astrosession_key)self.astroKey, 0);
    [super mouseUp:e];
}
@end

@implementation AppDelegate {
    astrosession *_session;
    NSWindow *_window;
    DisplayView *_display;
    NSWindow *_keypadWindow;
}

- (instancetype)initWithSession:(astrosession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    NSRect frame = NSMakeRect(0, 0, 800, 650);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go Astrocade";
    [_window center];

    _display = [[DisplayView alloc] initWithSession:_session];
    _window.contentView = _display;
    [_window makeFirstResponder:_display];
    [_display start];

    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    if (!astrosession_has_system_roms(_session)) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"No system ROM found";
        a.informativeText = @"Use Machine ▸ Import System ROM… to add astro.bin.";
        [a runModal];
    }
}

- (void)restartSession
{
    astrosession_start_opts opts;
    astrosession_stop(_session);
    astrosession_default_opts(_session, &opts);
    astrosession_start(_session, &opts);
}

/* ---- menu ---- */

- (void)buildMenu
{
    NSMenu *menubar = [[NSMenu alloc] init];
    NSApp.mainMenu = menubar;

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [menubar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About FujiNet Go Astrocade"
                       action:@selector(showAbout:) keyEquivalent:@""].target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    [menubar addItem:machineItem];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machine addItemWithTitle:@"Open Cartridge…"
                       action:@selector(openCart:) keyEquivalent:@"o"].target = self;
    [machine addItemWithTitle:@"Import System ROM…"
                       action:@selector(importRom:) keyEquivalent:@""].target = self;
    machineItem.submenu = machine;

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItemWithTitle:@"Keypad" action:@selector(toggleKeypad:) keyEquivalent:@""].target = self;
    viewItem.submenu = view;

    NSMenuItem *fnItem = [[NSMenuItem alloc] init];
    [menubar addItem:fnItem];
    NSMenu *fn = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fn addItemWithTitle:@"Reset Game" action:@selector(resetGame:) keyEquivalent:@""].target = self;
    [fn addItemWithTitle:@"Reset to CONFIG" action:@selector(resetConfig:) keyEquivalent:@"r"].target = self;
    [fn addItemWithTitle:@"FujiNet Configuration…"
                  action:@selector(fujinetConfig:) keyEquivalent:@""].target = self;
    fnItem.submenu = fn;
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"FujiNet Go Astrocade " @ASTRO_VERSION_STRING;
    a.informativeText = @"Self-contained Bally Astrocade with built-in FujiNet.\n"
                        @"GPL-3.0-or-later · https://fujinet.online/";
    [a runModal];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    if ([panel runModal] == NSModalResponseOK) {
        astrosession_load_cart(_session, panel.URL.fileSystemRepresentation);
    }
}

- (void)importRom:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    if ([panel runModal] == NSModalResponseOK) {
        char name[64];
        if (astrosession_import_rom(_session, panel.URL.fileSystemRepresentation,
                                    name, sizeof name) == 0) {
            if (!astrosession_is_running(_session))
                [self restartSession];
        } else {
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = @"Import failed";
            a.informativeText = @(astrosession_last_error(_session));
            [a runModal];
        }
    }
}

- (void)resetGame:(id)sender { (void)sender; astrosession_reset_game(_session); }
- (void)resetConfig:(id)sender { (void)sender; astrosession_reset_to_config(_session); }

- (void)fujinetConfig:(id)sender
{
    (void)sender;
    NSString *url = @(astrosession_fujinet_webui_url(_session));
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

/* ---- keypad window ---- */

- (void)toggleKeypad:(id)sender
{
    (void)sender;
    if (!_keypadWindow)
        [self buildKeypad];
    if (_keypadWindow.isVisible)
        [_keypadWindow orderOut:nil];
    else
        [_keypadWindow makeKeyAndOrderFront:nil];
}

- (void)buildKeypad
{
    static const char *labels[ASTROSESSION_KEY_COUNT] = {
        "C","↑","↓","%", "MR","MS","CH","÷", "7","8","9","×",
        "4","5","6","−", "1","2","3","+", "CE","0",".","=",
    };
    _keypadWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 300, 400)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                    backing:NSBackingStoreBuffered defer:NO];
    _keypadWindow.title = @"Keypad";
    [_keypadWindow center];
    NSView *content = _keypadWindow.contentView;

    for (int key = 0; key < ASTROSESSION_KEY_COUNT; key++) {
        int row = key / 4, col = key % 4;
        PadButton *b = [[PadButton alloc] initWithFrame:
            NSMakeRect(8 + col * 68, 400 - 44 - (row * 44), 64, 40)];
        b.title = @(labels[key]);
        b.bezelStyle = NSBezelStyleRounded;
        b.astroKey = key;
        b.session = _session;
        if (col == 3) {   /* the gold column */
            b.wantsLayer = YES;
            b.layer.backgroundColor = [[NSColor colorWithRed:0.91 green:0.76
                                                        blue:0.29 alpha:1] CGColor];
        }
        [content addSubview:b];
    }
    /* System row */
    NSButton *rg = [[NSButton alloc] initWithFrame:NSMakeRect(8, 8, 130, 30)];
    rg.title = @"Reset Game"; rg.bezelStyle = NSBezelStyleRounded;
    rg.target = self; rg.action = @selector(resetGame:);
    [content addSubview:rg];
    NSButton *rc = [[NSButton alloc] initWithFrame:NSMakeRect(146, 8, 140, 30)];
    rc.title = @"Reset to CONFIG"; rc.bezelStyle = NSBezelStyleRounded;
    rc.target = self; rc.action = @selector(resetConfig:);
    [content addSubview:rc];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

@end
