/*
 * AppDelegate -- the main window (DisplayView, or the No-BIOS page until a
 * BIOS is imported), the app menu, the Controllers window, Settings, and the
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
#import "debugger/DebuggerWindow.h"
#import "DisplayView.h"
#import "SettingsWindow.h"
#import "help/HelpWindow.h"

#include <stdlib.h>

#include "bindings.h"
#import "controllers/ControllersWindow.h"

#define TOAST_SECONDS 2.5

@implementation AppDelegate {
    astrosession *_session;
    NSWindow *_window;
    DisplayView *_display;
    id<NSObject> _activity;          /* no App Nap, no timer coalescing while we run */
    NSView *_noBios;
    AstroControllersWindow *_controllers;
    NSTextField *_toast;             /* over the picture, fades */
    unsigned _toastGeneration;       /* the pad generation already toasted */
    unsigned _overlayGeneration;     /* the overlay auto-on already toasted */
    NSUInteger _toastSerial;
    NSTimer *_sysactTimer;           /* console actions a gamepad posted, toasts */
    id _keyMonitor;                  /* F9 in the machine window */
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

    /* An emulator is latency-critical work: App Nap must not throttle the
     * machine thread or coalesce its timers when the window is behind
     * another, and Game Mode (Info.plist) asks the system to favour it. */
    _activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                          reason:@"Emulating an Astrocade"];
    NSRect frame = NSMakeRect(0, 0, 800, 650);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go Astrocade";
    /* Full screen is where Game Mode (Info.plist) engages. */
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
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
    /* layer-backed, so the display's layer and the toast over it composite in
     * the order they were added */
    content.wantsLayer = YES;
    [self buildToast:content];
    [_display start];

    __weak AppDelegate *weakSelf = self;
    _controllers = [[AstroControllersWindow alloc] initWithSession:_session
                                                           appKeys:^BOOL(uint32_t keysym) {
        return [weakSelf handleAppKey:keysym];
    }];
    _controllers.sysactionHandler = ^(int sysact) { [weakSelf runSysaction:sysact]; };
    /* No toast for the pads already attached at launch. */
    _toastGeneration = astrosession_gamepad_generation(_session);
    _overlayGeneration = astrosession_overlay_auto_generation(_session);

    [self buildMenu];
    [self showPage];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (getenv("ASTRO_OPEN_DEBUGGER"))
        [DebuggerWindow showDebuggerForSession:_session];
    if (getenv("ASTRO_OPEN_CONTROLLERS"))
        [_controllers toggle];

    /* Console actions a gamepad posted, pads coming and going and the overlay
     * turning itself on: well inside 50 ms. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.05
        repeats:YES block:^(NSTimer *tmr) { (void)tmr; [weakSelf sysactTick]; }];
    _sysactTimer.tolerance = 0.01;

    /* F9 in the machine window opens the Controllers panel (the panel handles
     * its own F9; the debugger keeps F9 for breakpoints). */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent *(NSEvent *evt) {
            AppDelegate *strong = weakSelf;
            if (strong && evt.window == strong->_window && !evt.isARepeat &&
                !(evt.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                       NSEventModifierFlagOption | NSEventModifierFlagShift)) &&
                astrosession_keysym_from_macos_keycode(evt.keyCode) == ASTROSESSION_KEYSYM_F9) {
                [strong->_controllers toggle];
                return nil;
            }
            return evt;
        }];
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
    NSButton *helpBtn = AstroHelpButton(@"nobios");
    NSStackView *buttons = [NSStackView stackViewWithViews:@[files, folder, helpBtn]];
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
    AstroSetHelpContext(_window, bios ? @"main" : @"nobios");
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
    [machine addItem:[self basicWordsItem]];
    [machine addItem:[NSMenuItem separatorItem]];
    [machine addItemWithTitle:@"Import BIOS…"
                       action:@selector(importBios:) keyEquivalent:@""].target = self;
    [machine addItemWithTitle:@"Import BIOS from a Folder…"
                       action:@selector(importBiosFolder:) keyEquivalent:@""].target = self;
    machineItem.submenu = machine;

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItemWithTitle:@"Controllers" action:@selector(toggleControllers:) keyEquivalent:@"k"].target = self;
    /* F12 toggles the debugger (Cmd+K is Controllers). A menu key equivalent is
     * tried before the display view's keyDown, so the key never reaches the
     * machine; the debugger window's own key monitor takes F12 there. */
    NSMenuItem *dbgItem = [view addItemWithTitle:@"Debugger" action:@selector(toggleDebugger:)
                                   keyEquivalent:[NSString stringWithFormat:@"%C", (unichar)NSF12FunctionKey]];
    dbgItem.target = self;
    dbgItem.keyEquivalentModifierMask = 0;
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *fsItem = [view addItemWithTitle:@"Enter Full Screen"
                                         action:@selector(toggleFullScreen:)
                                  keyEquivalent:@"f"];
    fsItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
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

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    [menubar addItem:helpItem];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [help addItemWithTitle:@"FujiNet Go Astrocade Help"
                    action:@selector(openHelp:) keyEquivalent:@"?"].target = self;
    helpItem.submenu = help;
    NSApp.helpMenu = help;
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[AstroHelpWindow shared]];
}

/* For the window in front: Settings, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[AstroHelpWindow shared] showFor:[NSApp keyWindow]];
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
                          controls:^(BOOL open) {
                              AppDelegate *strong = weakSelf;
                              if (!strong) return;
                              if (open) {
                                  [strong->_controllers show];
                              } else {
                                  [strong->_controllers refresh];
                                  [strong showToast:@"Default controls restored"];
                              }
                          }
                            closed:^(BOOL dirty) {
                                if (dirty) [weakSelf restartSession];
                                else [weakSelf startMachine];
                            }];
}

- (void)toggleDebugger:(id)sender { (void)sender; [DebuggerWindow toggleDebuggerForSession:_session]; }
- (void)resetGame:(id)sender { (void)sender; [self runSysaction:ASTROSESSION_SYSACT_RESET_GAME]; }
- (void)resetConfig:(id)sender { (void)sender; [self runSysaction:ASTROSESSION_SYSACT_RESET_CONFIG]; }

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

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 700, 464)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    NSTextView *text = [[NSTextView alloc] initWithFrame:scroll.bounds];
    text.editable = NO;
    text.font = [NSFont userFixedPitchFontOfSize:11];
    text.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = text;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 700, 500)];
        NSButton *help = AstroHelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        w.contentView = box;
    }
    AstroSetHelpContext(w, @"fujilog");

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

/* ---- Controllers window ---- */

- (void)toggleControllers:(id)sender
{
    (void)sender;
    [_controllers toggle];
}

/* The toast: a label over the top of the picture that fades. */
- (void)buildToast:(NSView *)content
{
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, content.bounds.size.height - 34, content.bounds.size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [content addSubview:_toast];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

/* A console action (ASTROSESSION_SYSACT_*) run on the UI thread, from the
 * panel, a menu or a gamepad button (a bound key runs its action inside the
 * session). */
- (void)runSysaction:(int)sysact
{
    switch (sysact) {
    case ASTROSESSION_SYSACT_RESET_GAME:
        astrosession_sysaction_fire(_session, ASTROSESSION_SYSACT_RESET_GAME);
        [self showToast:@"Game reset"];
        break;
    case ASTROSESSION_SYSACT_RESET_CONFIG:
        astrosession_sysaction_fire(_session, ASTROSESSION_SYSACT_RESET_CONFIG);
        [self showToast:@"Reset to CONFIG"];
        break;
    case ASTROSESSION_SYSACT_DEBUG_STOP:
        astrosession_sysaction_fire(_session, ASTROSESSION_SYSACT_DEBUG_STOP);
        [DebuggerWindow showDebuggerForSession:_session];
        break;
    default:
        break;
    }
}

/* What the 50 ms timer does: the console actions a gamepad button posted (the
 * pad thread cannot touch AppKit), a toast when a gamepad came or went, and one
 * when a Bally BASIC cartridge turned the overlay on. */
- (void)sysactTick
{
    const unsigned pending = astrosession_sysaction_take(_session);
    for (int act = 0; act < ASTROSESSION_SYSACT_COUNT; act++)
        if (pending & (1u << act)) [self runSysaction:act];
    const unsigned gen = astrosession_gamepad_generation(_session);
    if (gen != _toastGeneration) {
        _toastGeneration = gen;
        char msg[160];
        if (astrosession_gamepad_last_event(_session, msg, (int)sizeof msg) > 0)
            [self showToast:@(msg)];
    }
    const unsigned overlay = astrosession_overlay_auto_generation(_session);
    if (overlay != _overlayGeneration) {
        _overlayGeneration = overlay;
        [self showToast:@"Bally BASIC overlay on"];
    }
}

/* F9 and F12 are the app's and not bindable; called with a key typed in the
 * Controllers panel. YES when taken. */
- (BOOL)handleAppKey:(uint32_t)keysym
{
    if (keysym == ASTROSESSION_KEYSYM_F9) {
        [_controllers toggle];
        return YES;
    }
    if (keysym == ASTROSESSION_KEYSYM_F12) {
        [DebuggerWindow toggleDebuggerForSession:_session];
        return YES;
    }
    return NO;
}

/* Machine > Bally BASIC Word: a gold word typed as WORDS and its key. The
 * overlay's own words, never spelled out letter by letter. */
- (NSMenuItem *)basicWordsItem
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Type Bally BASIC Word" action:nil keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Type Bally BASIC Word"];
    const astro_overlay_key *ov = astrosession_overlay_layout();
    for (int key = 0; ov && key < ASTROSESSION_KEY_COUNT; key++) {
        const char *word = ov[key].legend[ASTRO_SHIFT_WORDS];
        if (!word || !word[0]) continue;
        NSMenuItem *entry = [menu addItemWithTitle:@(word) action:@selector(typeBasicWord:) keyEquivalent:@""];
        entry.target = self;
    }
    item.submenu = menu;
    return item;
}

- (void)typeBasicWord:(NSMenuItem *)sender
{
    astrosession_type_word(_session, sender.title.UTF8String);
}

/* The words are typed only with the overlay on. */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(typeBasicWord:))
        return astrosession_overlay_get(_session) != 0;
    return YES;
}

/* -[NSApplication terminate:] ends in exit(), so [app run] never returns and
 * main()'s astrosession_free is not reached. Stop the session here, or the
 * FujiNet runtime and log threads are still joinable when libfujinet's
 * static std::thread objects are destroyed, and std::terminate aborts. */
- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_display stop];
    [_sysactTimer invalidate];
    if (_keyMonitor) {
        [NSEvent removeMonitor:_keyMonitor];
        _keyMonitor = nil;
    }
    if (_activity) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }
    astrosession_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

@end
