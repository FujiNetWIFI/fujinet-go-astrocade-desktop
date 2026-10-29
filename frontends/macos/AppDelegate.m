/*
 * AppDelegate -- the main window (DisplayView, or the No-BIOS page until a
 * BIOS is imported), the app menu, the keypad window, Settings, and the
 * FujiNet/BIOS actions. The BIOS import looks and acts as
 * fujinet-go-msdos-desktop's does. NOT RUN-VERIFIED on macOS in this
 * environment; transposed from the verified GNOME/KDE/Win32 frontends to be
 * validated on the macOS CI runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import "AstroKeyForward.h"
#import "DebuggerWindow.h"
#import "DisplayView.h"
#import "SettingsWindow.h"

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
    NSView *_noBios;
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

    /* the picture and the No-BIOS page share the content view; one shows */
    NSView *content = [[NSView alloc] initWithFrame:frame];
    _display = [[DisplayView alloc] initWithSession:_session];
    _display.frame = content.bounds;
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:_display];
    _noBios = [self buildNoBios:content.bounds];
    [content addSubview:_noBios];
    _window.contentView = content;
    [_display start];

    [self buildMenu];
    [self showPage];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

/* In place of the picture until a BIOS is imported. */
- (NSView *)buildNoBios:(NSRect)frame
{
    NSImageView *icon = [NSImageView imageViewWithImage:[NSApp applicationIconImage]];
    [icon.widthAnchor constraintEqualToConstant:96].active = YES;
    [icon.heightAnchor constraintEqualToConstant:96].active = YES;
    NSTextField *title = [NSTextField labelWithString:@"No Astrocade BIOS"];
    title.font = [NSFont boldSystemFontOfSize:22];
    NSTextField *desc = [NSTextField wrappingLabelWithString:
        @"Bally's BIOS ROM is not part of the app. Import the one you have — astro.bin, "
        @"ballyhlc.bin or bioswhit.bin, a zip such as MAME's astrocde set, or a folder — "
        @"and the Astrocade starts."];
    desc.alignment = NSTextAlignmentCenter;
    desc.textColor = [NSColor secondaryLabelColor];
    desc.preferredMaxLayoutWidth = 460;
    [desc.widthAnchor constraintEqualToConstant:460].active = YES;
    NSButton *files = [NSButton buttonWithTitle:@"Import BIOS…" target:self action:@selector(importBios:)];
    files.bezelColor = [NSColor controlAccentColor];
    files.keyEquivalent = @"\r";
    NSButton *folder = [NSButton buttonWithTitle:@"Import a Folder…" target:self action:@selector(importBiosFolder:)];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[files, folder]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 12;
    NSStackView *column = [NSStackView stackViewWithViews:@[icon, title, desc, buttons]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment = NSLayoutAttributeCenterX;
    column.spacing = 14;
    column.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *page = [[NSView alloc] initWithFrame:frame];
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [page addSubview:column];
    [column.centerXAnchor constraintEqualToAnchor:page.centerXAnchor].active = YES;
    [column.centerYAnchor constraintEqualToAnchor:page.centerYAnchor].active = YES;
    return page;
}

/* The picture while there is a BIOS to boot, else the No-BIOS page. */
- (void)showPage
{
    const BOOL bios = astrosession_has_system_roms(_session) != 0;
    _display.hidden = !bios;
    _noBios.hidden = bios;
    if (bios)
        [_window makeFirstResponder:_display];
}

- (void)restartSession
{
    astrosession_start_opts opts;
    astrosession_stop(_session);
    astrosession_default_opts(_session, &opts);
    if (astrosession_start(_session, &opts) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"Could not start";
        a.informativeText = @(astrosession_last_error(_session));
        [a runModal];
    }
    [self showPage];
}

/* Starts the machine once a BIOS is there to boot (after an import). */
- (void)startMachine
{
    [self showPage];
    if (!astrosession_is_running(_session) && astrosession_has_system_roms(_session))
        [self restartSession];
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
    [appMenu addItemWithTitle:@"Settings…"
                       action:@selector(showSettings:) keyEquivalent:@","].target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    [menubar addItem:machineItem];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machine addItemWithTitle:@"Open Cartridge…"
                       action:@selector(openCart:) keyEquivalent:@"o"].target = self;
    [machine addItem:[NSMenuItem separatorItem]];
    [machine addItemWithTitle:@"Import BIOS…"
                       action:@selector(importBios:) keyEquivalent:@""].target = self;
    [machine addItemWithTitle:@"Import BIOS from a Folder…"
                       action:@selector(importBiosFolder:) keyEquivalent:@""].target = self;
    machineItem.submenu = machine;

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItemWithTitle:@"Keypad" action:@selector(toggleKeypad:) keyEquivalent:@""].target = self;
    [view addItemWithTitle:@"Debugger" action:@selector(showDebugger:) keyEquivalent:@""].target = self;
    viewItem.submenu = view;

    NSMenuItem *fnItem = [[NSMenuItem alloc] init];
    [menubar addItem:fnItem];
    NSMenu *fn = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fn addItemWithTitle:@"Reset Game" action:@selector(resetGame:) keyEquivalent:@""].target = self;
    [fn addItemWithTitle:@"Reset to CONFIG" action:@selector(resetConfig:) keyEquivalent:@"r"].target = self;
    [fn addItemWithTitle:@"FujiNet Configuration…"
                  action:@selector(fujinetConfig:) keyEquivalent:@""].target = self;
    [fn addItemWithTitle:@"FujiNet Console Log…"
                  action:@selector(fujinetLog:) keyEquivalent:@""].target = self;
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

/* ---- the BIOS ---- */

/* Each chosen file (or the folder) through the import; the machine starts if
 * it now can, then one alert says what came of it all. */
- (void)importBiosURLs:(NSArray<NSURL *> *)urls
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSURL *u in urls) {
        char msg[1024];
        msg[0] = '\0';
        if (astrosession_rom_import(_session, [u fileSystemRepresentation], msg, (int)sizeof msg) == 0)
            imported++;
        if (msg[0]) [log addObject:@(msg)];
    }
    [self startMachine];
    if ([log count]) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = imported ? @"BIOS Imported" : @"Nothing Imported";
        a.informativeText = [log componentsJoinedByString:@"\n"];
        [a runModal];
    }
}

- (void)importBiosFrom:(BOOL)folder
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (MAME's astrocde set works)"
                             : @"Import BIOS: images or zips"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:!folder];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSArray<NSURL *> *urls = [panel URLs];
        /* after the sheet: what came in is an alert of its own */
        dispatch_async(dispatch_get_main_queue(), ^{ [self importBiosURLs:urls]; });
    }];
}

- (void)importBios:(id)sender { (void)sender; [self importBiosFrom:NO]; }
- (void)importBiosFolder:(id)sender { (void)sender; [self importBiosFrom:YES]; }

/* A changed setting restarts the machine; a BIOS imported there starts it. */
- (void)showSettings:(id)sender
{
    (void)sender;
    __weak AppDelegate *weakSelf = self;
    [SettingsWindow showForSession:_session
                          romsPage:NO
                          imported:^{ [weakSelf showPage]; }
                            closed:^(BOOL dirty) {
                                if (dirty) [weakSelf restartSession];
                                else [weakSelf startMachine];
                            }];
}

- (void)showDebugger:(id)sender { (void)sender; [DebuggerWindow showDebuggerForSession:_session]; }
- (void)resetGame:(id)sender { (void)sender; astrosession_reset_game(_session); }
- (void)resetConfig:(id)sender { (void)sender; astrosession_reset_to_config(_session); }

- (void)fujinetConfig:(id)sender
{
    (void)sender;
    if (astrosession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Use the web UI of that FujiNet instance.";
        [alert runModal];
        return;
    }
    NSString *url = @(astrosession_fujinet_webui_url(_session));
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
}

- (void)fujinetLog:(id)sender
{
    (void)sender;
    if (astrosession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Its console output is in the process that is running it.";
        [alert runModal];
        return;
    }
    NSWindow *w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 700, 500)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    w.title = @"FujiNet Console Log";
    w.releasedWhenClosed = NO;
    [w center];

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:w.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    NSTextView *text = [[NSTextView alloc] initWithFrame:scroll.bounds];
    text.editable = NO;
    text.font = [NSFont userFixedPitchFontOfSize:11];
    text.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = text;
    w.contentView = scroll;

    astrosession *sess = _session;
    void (^refresh)(void) = ^{
        char buf[16384];
        astrosession_fujinet_copy_log(sess, buf, sizeof buf);
        text.string = @(buf);
    };
    refresh();
    NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                     repeats:YES
                                                       block:^(NSTimer *t) {
                                                         (void)t; refresh();
                                                       }];
    /* stop the timer when the window closes */
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
        object:w queue:nil usingBlock:^(NSNotification *n) { (void)n; [timer invalidate]; }];
    [w makeKeyAndOrderFront:nil];
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
    /* exactly the buttons and their margins: 4 x 64 + 3 x 4 wide, six rows
     * of 40 + 5 x 4, the system row of 30 and a 10 gap above it */
    enum { PAD_W = 8 + 4 * 64 + 3 * 4 + 8, PAD_H = 8 + 30 + 10 + 6 * 40 + 5 * 4 + 8 };
    _keypadWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, PAD_W, PAD_H)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                    backing:NSBackingStoreBuffered defer:NO];
    _keypadWindow.title = @"Keypad";
    [_keypadWindow center];
    NSView *content = _keypadWindow.contentView;

    for (int key = 0; key < ASTROSESSION_KEY_COUNT; key++) {
        int row = key / 4, col = key % 4;
        PadButton *b = [[PadButton alloc] initWithFrame:
            NSMakeRect(8 + col * 68, PAD_H - 8 - 40 - row * 44, 64, 40)];
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
    NSButton *rg = [[NSButton alloc] initWithFrame:NSMakeRect(8, 8, 132, 30)];
    rg.title = @"Reset Game"; rg.bezelStyle = NSBezelStyleRounded;
    rg.target = self; rg.action = @selector(resetGame:);
    [content addSubview:rg];
    NSButton *rc = [[NSButton alloc] initWithFrame:NSMakeRect(144, 8, 132, 30)];
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
