/*
 * DebuggerWindow -- a controls row (Pause/Resume, Step, Step Over, Step Out),
 * a monospace view of the Z80 registers/disassembly/memory, and the full
 * decoded data-chip + sound register text, refreshed on a 0.2 s NSTimer over
 * the shared astrodebug/astrovid engine. Structurally identical to the
 * verified GNOME/KDE/Win32 debugger windows.
 *
 * NOT run-verified here (no Cocoa toolchain); validated on the macOS CI
 * runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include "astrodebug.h"
#include "astrovid.h"

static DebuggerWindow *g_singleton;

@implementation DebuggerWindow {
    astrodebug *_dbg;
    NSTextView *_cpu;
    NSTextView *_vid;
    NSTimer *_timer;
}

+ (void)showDebugger
{
    if (g_singleton) {
        [g_singleton.window makeKeyAndOrderFront:nil];
        return;
    }
    g_singleton = [[DebuggerWindow alloc] init];
    [g_singleton.window makeKeyAndOrderFront:nil];
}

- (instancetype)init
{
    NSWindow *w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 640)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    w.title = @"Debugger";
    [w center];
    self = [super initWithWindow:w];
    if (!self)
        return nil;
    w.delegate = self;

    _dbg = astrodebug_get();
    astrodebug_set_engaged(_dbg, 1);

    NSView *content = w.contentView;

    NSArray *labels = @[@"Pause/Resume", @"Step", @"Step Over", @"Step Out"];
    for (NSUInteger i = 0; i < labels.count; i++) {
        NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(8 + i * 130, 600, 124, 28)];
        b.title = labels[i];
        b.bezelStyle = NSBezelStyleRounded;
        b.target = self;
        b.tag = (NSInteger)i;
        b.action = @selector(control:);
        [content addSubview:b];
    }

    _cpu = [self makeTextView:NSMakeRect(8, 8, 430, 580) into:content];
    _vid = [self makeTextView:NSMakeRect(446, 8, 406, 580) into:content];

    _timer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES
                                               block:^(NSTimer *t) { (void)t; [self refresh]; }];
    return self;
}

- (NSTextView *)makeTextView:(NSRect)frame into:(NSView *)parent
{
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSTextView *tv = [[NSTextView alloc] initWithFrame:scroll.bounds];
    tv.editable = NO;
    tv.font = [NSFont userFixedPitchFontOfSize:11];
    scroll.documentView = tv;
    [parent addSubview:scroll];
    return tv;
}

- (void)control:(NSButton *)sender
{
    switch (sender.tag) {
    case 0:
        if (astrodebug_is_paused(_dbg)) astrodebug_resume(_dbg);
        else astrodebug_pause(_dbg);
        break;
    case 1: astrodebug_step(_dbg); break;
    case 2: astrodebug_step_over(_dbg); break;
    case 3: astrodebug_step_out(_dbg); break;
    }
}

- (void)refresh
{
    if (astrodebug_is_paused(_dbg)) {
        astrodebug_regs r;
        if (astrodebug_regs_get(_dbg, &r)) {
            char buf[4096];
            int p = snprintf(buf, sizeof buf,
                "AF %04X  BC %04X  DE %04X  HL %04X\n"
                "AF'%04X BC'%04X DE'%04X HL'%04X\n"
                "IX %04X  IY %04X  SP %04X  PC %04X\n"
                "I %02X R %02X IM %d IFF1 %d IFF2 %d %s WZ %04X\n\n",
                r.af, r.bc, r.de, r.hl, r.af2, r.bc2, r.de2, r.hl2,
                r.ix, r.iy, r.sp, r.pc, r.i, r.r, r.im, r.iff1, r.iff2,
                r.halt ? "HALT" : "", r.wz);
            astrodebug_dasm_line lines[20];
            int n = astrodebug_disassemble(_dbg, r.pc, lines, 20);
            for (int i = 0; i < n && p < (int)sizeof buf - 80; i++)
                p += snprintf(buf + p, sizeof buf - p, "%s %04X  %s\n",
                              i == 0 ? ">" : " ", lines[i].addr, lines[i].text);
            _cpu.string = @(buf);
        }
    } else {
        _cpu.string = @"(running — Pause to inspect)";
    }

    astrovid_snapshot snap;
    astrovid_snapshot_get(&snap);
    char text[4096];
    astrovid_format_state(&snap, text, sizeof text);
    _vid.string = @(text);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [_timer invalidate];
    astrodebug_set_engaged(_dbg, 0);
    g_singleton = nil;
}

@end
