/*
 * SettingsWindow -- the GNOME and KDE Preferences' twin on macOS: a fixed
 * window with two tabs.
 *
 *  Machine -- the System ROM (BIOS variant; one not imported says so) and the
 *      RAM expansion. They apply to the settings store at once and the
 *      machine restarts once when the window closes.
 *  ROMs -- what fujinet-go-msdos-desktop's ROMs page is: a note on where the
 *      BIOS comes from and where it is kept, Import… and Import a Folder…,
 *      and each BIOS, imported or not.
 *  Input -- Enable gamepads (applies when the window closes), each gamepad's
 *      player (Automatic or Player 1 to 4, at once), Reset Controls and Open
 *      Controllers. Keys and buttons are re-mapped in the Controllers window.
 *
 * NOT BUILT OR RUN-VERIFIED on macOS in this environment (no Cocoa toolchain
 * here) -- transposed from the verified GNOME/KDE/Win32 Preferences, to be
 * validated on the macOS CI runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"
#import "help/HelpWindow.h"

#include <string.h>

#include "bindings.h"

#define WIN_W 560
#define WIN_H 360

static SettingsWindow *g_shown;

@interface SettingsWindow () <NSWindowDelegate, NSTabViewDelegate>
@end

@implementation SettingsWindow {
    astrosession *_session;
    NSWindow *_window;
    NSTabView *_tabs;
    NSPopUpButton *_bios;
    NSPopUpButton *_exp;
    NSButton *_externalBox;
    NSTextField *_externalField;
    NSMutableArray<NSTextField *> *_romStates;
    void (^_imported)(void);
    void (^_controls)(BOOL);
    void (^_closed)(BOOL);
    BOOL _dirty;
    NSPopUpButton *_padList;
    NSPopUpButton *_padPlayer;
    NSTimer *_padTimer;
    unsigned _padGeneration;
}

static NSTextField *label(NSString *text, NSRect frame)
{
    NSTextField *l = [NSTextField labelWithString:text];
    [l setFrame:frame];
    return l;
}

/* The BIOS choices, each one not imported yet saying so. */
- (void)fillBios
{
    [_bios removeAllItems];
    for (int i = 0; astrosession_bios_name(i); i++) {
        NSString *t = [NSString stringWithFormat:@"%s%@", astrosession_bios_name(i),
                       astrosession_rom_item_available(_session, i) ? @"" : @" (not imported)"];
        /* through the menu: addItemWithTitle: drops an item of the same title */
        [[_bios menu] addItemWithTitle:t action:nil keyEquivalent:@""];
    }
    const int sel = astrosession_get_int(_session, "bios", ASTROSESSION_BIOS_ASTRO);
    [_bios selectItemAtIndex:sel >= 0 && sel < [_bios numberOfItems] ? sel : 0];
}

- (void)refresh
{
    for (NSUInteger r = 0; r < [_romStates count]; r++)
        [_romStates[r] setStringValue:astrosession_rom_item_available(_session, (int)r)
                                          ? @"Imported" : @"Not imported"];
    [self fillBios];
}

- (void)choiceChanged:(NSPopUpButton *)pop
{
    const BOOL bios = pop == _bios;
    const char *key = bios ? "bios" : "exp";
    const int def = bios ? ASTROSESSION_BIOS_ASTRO : ASTROSESSION_EXP_NONE;
    const int sel = (int)[pop indexOfSelectedItem];
    if (sel >= 0 && sel != astrosession_get_int(_session, key, def)) {
        astrosession_set_int(_session, key, sel);
        _dirty = YES;
    }
}

/* Use external FujiNet: the address field is live only while it is on. */
- (void)externalToggled:(NSButton *)box
{
    const int active = [box state] == NSControlStateValueOn;
    _externalField.enabled = active;
    if (astrosession_get_int(_session, "fujinet_external", 0) != active) {
        astrosession_set_int(_session, "fujinet_external", active);
        _dirty = YES;
    }
}

/* The address: valid text (or none, meaning 127.0.0.1:11500) is saved;
 * anything else is shown in red, not saved, and keeps the window open. */
- (BOOL)externalAddrValid
{
    NSString *text = [_externalField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (astrosession_parse_hostport(text.UTF8String, host, sizeof host, &port) != 0) {
        _externalField.textColor = NSColor.systemRedColor;
        return NO;
    }
    _externalField.textColor = NSColor.controlTextColor;
    if (strcmp(astrosession_get_str(_session, "fujinet_external_addr", ""), text.UTF8String) != 0) {
        astrosession_set_str(_session, "fujinet_external_addr", text.UTF8String);
        _dirty = YES;
    }
    return YES;
}

- (void)externalAddrChanged:(id)sender
{
    (void)sender;
    [self externalAddrValid];
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    [_window makeFirstResponder:nil]; /* commit a field edit */
    if ([_externalBox state] != NSControlStateValueOn || [self externalAddrValid])
        return YES;
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"That is not an address:port";
    alert.informativeText = @"Example: 127.0.0.1:11500";
    [alert beginSheetModalForWindow:_window completionHandler:nil];
    return NO;
}

- (void)importRoms:(NSButton *)b
{
    const BOOL folder = [b tag] != 0;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (MAME's astrocde set works)"
                             : @"Import BIOS: images or zips"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:!folder];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || ![[panel URLs] count]) return;
        NSArray<NSURL *> *urls = [panel URLs];
        /* after the sheet: what came in is an alert of its own */
        dispatch_async(dispatch_get_main_queue(), ^{
            NSMutableArray<NSString *> *log = [NSMutableArray array];
            int imported = 0;
            for (NSURL *url in urls) {
                char msg[1024];
                msg[0] = '\0';
                if (astrosession_rom_import(self->_session, [url fileSystemRepresentation],
                                            msg, (int)sizeof msg) == 0)
                    imported++;
                [log addObject:@(msg)];
            }
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = imported ? @"BIOS Imported" : @"Nothing Imported";
            a.informativeText = [log componentsJoinedByString:@"\n"];
            [a beginSheetModalForWindow:self->_window completionHandler:nil];
            [self refresh];
            if (self->_imported) self->_imported();
        });
    }];
}

- (NSView *)machinePage
{
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, WIN_W - 40, WIN_H - 80)];
    const CGFloat top = NSHeight(v.frame);
    [v addSubview:label(@"System ROM", NSMakeRect(16, top - 40, 120, 20))];
    _bios = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(140, top - 44, NSWidth(v.frame) - 156, 26)
                                       pullsDown:NO];
    _bios.target = self;
    _bios.action = @selector(choiceChanged:);
    [v addSubview:_bios];
    [v addSubview:label(@"RAM Expansion", NSMakeRect(16, top - 76, 120, 20))];
    _exp = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(140, top - 80, NSWidth(v.frame) - 156, 26)
                                      pullsDown:NO];
    for (int i = 0; astrosession_exp_name(i); i++)
        [[_exp menu] addItemWithTitle:@(astrosession_exp_name(i)) action:nil keyEquivalent:@""];
    const int sel = astrosession_get_int(_session, "exp", ASTROSESSION_EXP_NONE);
    [_exp selectItemAtIndex:sel >= 0 && sel < [_exp numberOfItems] ? sel : 0];
    _exp.target = self;
    _exp.action = @selector(choiceChanged:);
    [v addSubview:_exp];
    NSTextField *note = label(@"Changes take effect when the machine restarts.",
                              NSMakeRect(16, top - 116, NSWidth(v.frame) - 32, 20));
    [note setTextColor:[NSColor secondaryLabelColor]];
    [v addSubview:note];

    _externalBox = [NSButton checkboxWithTitle:@"Use external FujiNet" target:self
                                        action:@selector(externalToggled:)];
    [_externalBox setFrame:NSMakeRect(16, top - 156, NSWidth(v.frame) - 32, 20)];
    [_externalBox setState:astrosession_get_int(_session, "fujinet_external", 0)
                               ? NSControlStateValueOn : NSControlStateValueOff];
    [v addSubview:_externalBox];
    [v addSubview:label(@"Address:port", NSMakeRect(16, top - 184, 120, 20))];
    _externalField = [NSTextField textFieldWithString:@(
        astrosession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:11500";
    [_externalField setFrame:NSMakeRect(140, top - 188, NSWidth(v.frame) - 156, 24)];
    _externalField.enabled = [_externalBox state] == NSControlStateValueOn;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];
    [v addSubview:_externalField];
    NSTextField *hint = [NSTextField wrappingLabelWithString:
        @"Skips the built-in FujiNet and connects to another instance: its [BOIP] "
        @"listener's host and port."];
    [hint setFrame:NSMakeRect(16, top - 228, NSWidth(v.frame) - 32, 36)];
    [hint setTextColor:[NSColor secondaryLabelColor]];
    [v addSubview:hint];
    return v;
}

- (NSView *)romsPage
{
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, WIN_W - 40, WIN_H - 80)];
    const CGFloat w = NSWidth(v.frame);
    CGFloat y = NSHeight(v.frame) - 12;
    NSTextField *n = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:
        @"The console's BIOS ROM is not part of the app. Import it from a file, a zip or a "
        @"folder (MAME's astrocde set works); it is recognised by its checksum. Kept in %s.",
        astrosession_roms_path(_session)]];
    [n setFrame:NSMakeRect(16, y - 64, w - 32, 64)];
    [n setTextColor:[NSColor secondaryLabelColor]];
    [v addSubview:n];
    y -= 72;
    NSButton *files = [NSButton buttonWithTitle:@"Import…" target:self action:@selector(importRoms:)];
    [files setFrame:NSMakeRect(16, y - 28, 110, 28)];
    [files setTag:0];
    [v addSubview:files];
    NSButton *dir = [NSButton buttonWithTitle:@"Import a Folder…" target:self action:@selector(importRoms:)];
    [dir setFrame:NSMakeRect(132, y - 28, 150, 28)];
    [dir setTag:1];
    [v addSubview:dir];
    y -= 40;
    _romStates = [NSMutableArray array];
    const char *lastGroup = NULL;
    for (int r = 0; r < astrosession_rom_item_count(); r++) {
        const astro_rom_item *item = astrosession_rom_item(r);
        if (!item) continue;
        if (!lastGroup || strcmp(lastGroup, item->group)) {
            NSTextField *g = label(@(item->group), NSMakeRect(16, y - 22, w - 32, 20));
            [g setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
            [v addSubview:g];
            y -= 26;
            lastGroup = item->group;
        }
        [v addSubview:label(@(item->name), NSMakeRect(28, y - 22, w - 180, 20))];
        NSTextField *state = label(@"", NSMakeRect(w - 136, y - 22, 120, 20));
        [state setTextColor:[NSColor secondaryLabelColor]];
        [state setAlignment:NSTextAlignmentRight];
        [v addSubview:state];
        [_romStates addObject:state];
        y -= 24;
    }
    return v;
}

/* ---- the Input page ---- */

/* The connected gamepads and the player the chosen one is set to. */
- (void)refreshPadList
{
    const NSInteger keep = _padList.indexOfSelectedItem;
    const int count = astrosession_gamepad_count(_session);
    _padGeneration = astrosession_gamepad_generation(_session);
    [_padList removeAllItems];
    for (int idx = 0; idx < count; idx++) {
        char name[128];
        name[0] = '\0';
        astrosession_gamepad_name(_session, idx, name, (int)sizeof name);
        /* through the menu: addItemWithTitle: drops an item of the same title */
        [[_padList menu] addItemWithTitle:@(name) action:nil keyEquivalent:@""];
    }
    if (count == 0)
        [[_padList menu] addItemWithTitle:@"No gamepads connected" action:nil keyEquivalent:@""];
    _padList.enabled = count > 0;
    _padPlayer.enabled = count > 0;
    [_padList selectItemAtIndex:keep >= 0 && keep < count ? keep : 0];
    [self padSelected:_padList];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    int assign = -1;
    if (sel >= 0 && sel < astrosession_gamepad_count(_session))
        assign = astrosession_gamepad_assignment(_session, (int)sel);
    [_padPlayer selectItemAtIndex:assign < 0 || assign >= ASTROSESSION_PLAYERS ? 0 : assign + 1];
}

- (void)padPlayerChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < astrosession_gamepad_count(_session))
        astrosession_gamepad_assign(_session, (int)sel, (int)_padPlayer.indexOfSelectedItem - 1);
}

/* Enable gamepads: the pad thread starts or stops with the session. */
- (void)padsToggled:(NSButton *)box
{
    const int on = [box state] == NSControlStateValueOn;
    if (astrosession_get_int(_session, "enable_gamepad", 1) != on) {
        astrosession_set_int(_session, "enable_gamepad", on);
        _dirty = YES;
    }
}

- (void)resetControls:(id)sender
{
    (void)sender;
    bindings_reset_defaults(_session);
    if (_controls) _controls(NO);
}

- (void)openControllers:(id)sender
{
    (void)sender;
    if (_controls) _controls(YES);
}

- (NSView *)inputPage
{
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, WIN_W - 40, WIN_H - 80)];
    const CGFloat top = NSHeight(v.frame), w = NSWidth(v.frame);
    NSButton *enable = [NSButton checkboxWithTitle:@"Enable gamepads" target:self action:@selector(padsToggled:)];
    [enable setFrame:NSMakeRect(16, top - 40, w - 32, 20)];
    [enable setState:astrosession_get_int(_session, "enable_gamepad", 1)
                         ? NSControlStateValueOn : NSControlStateValueOff];
    [v addSubview:enable];
    [v addSubview:label(@"Gamepad", NSMakeRect(16, top - 74, 120, 20))];
    _padList = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(140, top - 78, 230, 26) pullsDown:NO];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    [v addSubview:_padList];
    _padPlayer = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(378, top - 78, 124, 26) pullsDown:NO];
    [_padPlayer addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2", @"Player 3", @"Player 4"]];
    _padPlayer.target = self;
    _padPlayer.action = @selector(padPlayerChanged:);
    [v addSubview:_padPlayer];
    NSButton *reset = [NSButton buttonWithTitle:@"Reset Controls" target:self action:@selector(resetControls:)];
    [reset setFrame:NSMakeRect(16, top - 122, 130, 28)];
    [v addSubview:reset];
    NSButton *open = [NSButton buttonWithTitle:@"Open Controllers" target:self action:@selector(openControllers:)];
    [open setFrame:NSMakeRect(152, top - 122, 150, 28)];
    [v addSubview:open];
    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"Keys and gamepad buttons are re-mapped in the Controllers window (View > Controllers, "
        @"Cmd+K or F9): click Map, choose a control, then press the key or the gamepad button. "
        @"Reset Controls puts the defaults back. Turning gamepads on or off applies when this "
        @"window is closed; the player a gamepad plays applies at once."];
    [note setFrame:NSMakeRect(16, top - 214, w - 32, 76)];
    [note setTextColor:[NSColor secondaryLabelColor]];
    [v addSubview:note];
    return v;
}

/* Hot-plugged gamepads, twice a second, while the window is open. */
- (void)startPadTimer
{
    __weak SettingsWindow *weakSelf = self;
    _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *tmr) {
        (void)tmr;
        SettingsWindow *strong = weakSelf;
        if (strong && astrosession_gamepad_generation(strong->_session) != strong->_padGeneration)
            [strong refreshPadList];
    }];
    _padTimer.tolerance = 0.25;
}

/* Help's context: the page that shows */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    NSString *context = @"prefs/Machine";
    if ([item.identifier isEqual:@"roms"]) context = @"prefs/ROMs";
    else if ([item.identifier isEqual:@"input"]) context = @"prefs/Input";
    AstroSetHelpContext(_window, context);
}

+ (void)showForSession:(astrosession *)session
              romsPage:(BOOL)romsPage
              imported:(void (^)(void))imported
              controls:(void (^)(BOOL open))controls
                closed:(void (^)(BOOL dirty))closed
{
    if (g_shown) {
        [g_shown->_window makeKeyAndOrderFront:nil];
        return;
    }
    SettingsWindow *s = [[SettingsWindow alloc] init];
    s->_session = session;
    s->_imported = [imported copy];
    s->_controls = [controls copy];
    s->_closed = [closed copy];
    /* fixed size: titled and closable, never resizable */
    s->_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, WIN_W, WIN_H)
                                             styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    s->_window.title = @"Settings";
    s->_window.releasedWhenClosed = NO;
    s->_window.delegate = s;
    s->_tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 12, WIN_W - 24, WIN_H - 24)];
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"machine"];
    item.label = @"Machine";
    item.view = [s machinePage];
    [s->_tabs addTabViewItem:item];
    item = [[NSTabViewItem alloc] initWithIdentifier:@"roms"];
    item.label = @"ROMs";
    item.view = [s romsPage];
    [s->_tabs addTabViewItem:item];
    item = [[NSTabViewItem alloc] initWithIdentifier:@"input"];
    item.label = @"Input";
    item.view = [s inputPage];
    [s->_tabs addTabViewItem:item];
    [s->_window.contentView addSubview:s->_tabs];
    {
        NSButton *helpBtn = AstroHelpButton(nil);
        [helpBtn setFrame:NSMakeRect(WIN_W - 12 - 28, WIN_H - 12 - 28, 26, 26)];
        [s->_window.contentView addSubview:helpBtn];
        s->_tabs.delegate = s;
        AstroSetHelpContext(s->_window, @"prefs/Machine");
    }
    [s refresh];
    [s refreshPadList];
    [s startPadTimer];
    if (romsPage)
        [s->_tabs selectTabViewItemWithIdentifier:@"roms"];
    [s tabView:s->_tabs didSelectTabViewItem:s->_tabs.selectedTabViewItem];
    [s->_window center];
    [s->_window makeKeyAndOrderFront:nil];
    g_shown = s;
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [_padTimer invalidate];
    _padTimer = nil;
    astrosession_settings_flush(_session);
    void (^closed)(BOOL) = _closed;
    const BOOL dirty = _dirty;
    /* the next show builds a fresh window; this one goes after this call */
    dispatch_async(dispatch_get_main_queue(), ^{
        g_shown = nil;
        if (closed) closed(dirty);
    });
}

@end
