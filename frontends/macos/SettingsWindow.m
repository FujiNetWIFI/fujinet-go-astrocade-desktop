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
 *
 * NOT BUILT OR RUN-VERIFIED on macOS in this environment (no Cocoa toolchain
 * here) -- transposed from the verified GNOME/KDE/Win32 Preferences, to be
 * validated on the macOS CI runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"

#include <string.h>

#define WIN_W 560
#define WIN_H 360

static SettingsWindow *g_shown;

@interface SettingsWindow () <NSWindowDelegate>
@end

@implementation SettingsWindow {
    astrosession *_session;
    NSWindow *_window;
    NSTabView *_tabs;
    NSPopUpButton *_bios;
    NSPopUpButton *_exp;
    NSMutableArray<NSTextField *> *_romStates;
    void (^_imported)(void);
    void (^_closed)(BOOL);
    BOOL _dirty;
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

- (void)importRoms:(NSButton *)b
{
    const BOOL folder = [b tag] != 0;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (MAME's astrocde set works)"
                             : @"Import BIOS: an image or a zip"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:NO];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || ![panel URL]) return;
        NSURL *url = [panel URL];
        /* after the sheet: what came in is an alert of its own */
        dispatch_async(dispatch_get_main_queue(), ^{
            char msg[1024];
            msg[0] = '\0';
            const int rc = astrosession_rom_import(self->_session, [url fileSystemRepresentation],
                                                   msg, (int)sizeof msg);
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = rc == 0 ? @"BIOS Imported" : @"Nothing Imported";
            a.informativeText = @(msg);
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

+ (void)showForSession:(astrosession *)session
              romsPage:(BOOL)romsPage
              imported:(void (^)(void))imported
                closed:(void (^)(BOOL dirty))closed
{
    if (g_shown) {
        [g_shown->_window makeKeyAndOrderFront:nil];
        return;
    }
    SettingsWindow *s = [[SettingsWindow alloc] init];
    s->_session = session;
    s->_imported = [imported copy];
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
    [s->_window.contentView addSubview:s->_tabs];
    [s refresh];
    if (romsPage)
        [s->_tabs selectTabViewItemWithIdentifier:@"roms"];
    [s->_window center];
    [s->_window makeKeyAndOrderFront:nil];
    g_shown = s;
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
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
