/*
 * DebuggerWindow -- two tabs over the shared astrodebug/astrovid engine,
 * refreshed on a 0.2 s NSTimer:
 *
 *  Z80 / Video chip -- a controls row (Pause/Resume, Step, Step Over, Step
 *      Out), a monospace view of the Z80 registers/disassembly/memory, and
 *      the full decoded data-chip + sound register text.
 *  FujiNet -- every FujiBus packet between the console and FujiNet, both
 *      ways, with hex (always traced, from session start), beside the
 *      cartridge's link and mailbox state.
 *
 * Structurally identical to the verified GNOME/KDE/Win32 debugger windows.
 *
 * NOT run-verified here (no Cocoa toolchain); validated on the macOS CI
 * runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include <float.h>

#include "astrodebug.h"
#include "astrovid.h"

#define TRACE_MAX 512

enum { PAGE_CPU, PAGE_FUJINET };

static DebuggerWindow *g_singleton;

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

@implementation DebuggerWindow {
    astrosession *_session;
    astrodebug *_dbg;
    NSTextView *_cpu;
    NSTextView *_vid;
    NSTimer *_timer;

    NSTabView *_tabs;
    NSTextField *_fujiNote;
    NSTextView *_fujiText;
    NSTextView *_fujiState;
    int _fujiCount;
    uint64_t _fujiLast;
    uint64_t _fujiSerial;
    int _fujiTicks;
}

+ (void)showDebuggerForSession:(astrosession *)session
{
    if (g_singleton) {
        [g_singleton.window makeKeyAndOrderFront:nil];
        return;
    }
    g_singleton = [[DebuggerWindow alloc] initWithSession:session];
    [g_singleton.window makeKeyAndOrderFront:nil];
}

- (instancetype)initWithSession:(astrosession *)session
{
    NSWindow *w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 880, 700)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    w.title = @"Debugger";
    [w center];
    self = [super initWithWindow:w];
    if (!self)
        return nil;
    w.delegate = self;

    _session = session;
    _dbg = astrodebug_get();
    astrodebug_set_engaged(_dbg, 1);
    _fujiCount = -1;

    NSView *content = w.contentView;
    _tabs = [[NSTabView alloc] initWithFrame:content.bounds];
    _tabs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _tabs.delegate = self;
    [content addSubview:_tabs];
    /* the pages' size; their views keep up through autoresizing either way */
    NSRect inner = _tabs.contentRect;
    if (inner.size.width < 200 || inner.size.height < 200)
        inner = NSInsetRect(content.bounds, 8, 20);
    const CGFloat W = inner.size.width, H = inner.size.height;

    /* ---- Z80 / Video chip ---- */
    NSView *cpuPage = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    NSArray *labels = @[@"Pause/Resume", @"Step", @"Step Over", @"Step Out"];
    for (NSUInteger i = 0; i < labels.count; i++) {
        NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(8 + i * 130, H - 40, 124, 28)];
        b.title = labels[i];
        b.bezelStyle = NSBezelStyleRounded;
        b.target = self;
        b.tag = (NSInteger)i;
        b.action = @selector(control:);
        b.autoresizingMask = NSViewMinYMargin;
        [cpuPage addSubview:b];
    }

    _cpu = [self makeTextView:NSMakeRect(8, 8, 430, H - 60) into:cpuPage];
    _vid = [self makeTextView:NSMakeRect(446, 8, 406, H - 60) into:cpuPage];

    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@"cpu"];
    item.label = @"Z80 / Video chip";
    item.view = cpuPage;
    [_tabs addTabViewItem:item];

    /* ---- FujiNet: the packets left (a note and Clear over them), the
     * cartridge right ---- */
    NSView *fujiPage = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    const CGFloat rw = 300, lw = W - 8 - 12 - rw - 8, rx = 8 + lw + 12;

    NSTextField *title = [self heading:@"FujiBus packets (SLIP-decoded)"];
    title.frame = NSMakeRect(8, H - 26, lw, 20);
    title.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [fujiPage addSubview:title];

    _fujiNote = [NSTextField labelWithString:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    _fujiNote.frame = NSMakeRect(8, H - 52, lw - 88, 20);
    _fujiNote.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [fujiPage addSubview:_fujiNote];

    NSButton *clear = [[NSButton alloc] initWithFrame:NSMakeRect(8 + lw - 80, H - 58, 80, 28)];
    clear.title = @"Clear";
    clear.bezelStyle = NSBezelStyleRounded;
    clear.target = self;
    clear.action = @selector(fujiClear:);
    clear.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [fujiPage addSubview:clear];

    _fujiText = [self makeTextView:NSMakeRect(8, 8, lw, H - 68) into:fujiPage];
    /* no wrapping: a packet's hex runs on, the view scrolls sideways */
    _fujiText.enclosingScrollView.hasHorizontalScroller = YES;
    _fujiText.horizontallyResizable = YES;
    _fujiText.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _fujiText.textContainer.widthTracksTextView = NO;
    _fujiText.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);

    NSTextField *stateTitle = [self heading:@"FujiNet cartridge"];
    stateTitle.frame = NSMakeRect(rx, H - 26, rw, 20);
    stateTitle.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [fujiPage addSubview:stateTitle];

    _fujiState = [self makeTextView:NSMakeRect(rx, 8, rw, H - 38) into:fujiPage];
    _fujiState.enclosingScrollView.autoresizingMask = NSViewMinXMargin | NSViewHeightSizable;

    item = [[NSTabViewItem alloc] initWithIdentifier:@"fujinet"];
    item.label = @"FujiNet";
    item.view = fujiPage;
    [_tabs addTabViewItem:item];

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

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    return l;
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

- (void)fujiClear:(id)sender
{
    (void)sender;
    astrodebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (int)currentTab
{
    return (int)[_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if ([self currentTab] == PAGE_FUJINET)
        [self refreshFujinet:NO];
}

- (void)refresh
{
    if ([self currentTab] == PAGE_FUJINET) {
        /* on a stop, and every other tick while running */
        const uint64_t s = astrodebug_stop_serial(_dbg);
        if (s != _fujiSerial || (!astrodebug_is_paused(_dbg) && ++_fujiTicks >= 2)) {
            _fujiSerial = s;
            _fujiTicks = 0;
            [self refreshFujinet:NO];
        }
        return;
    }

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

- (void)refreshFujinet:(BOOL)force
{
    static astrodebug_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];
    char state[2048];
    int n = astrodebug_fujinet_trace(_dbg, p, TRACE_MAX);
    size_t len = 0;

    if (astrosession_fujinet_link(_session))
        _fujiNote.stringValue = @"FujiNet cartridge, connected.";
    else if (astrosession_fujinet_running(_session))
        _fujiNote.stringValue = @"FujiNet cartridge, not connected.";
    else
        _fujiNote.stringValue = @"FujiNet cartridge, not connected (the FujiNet runtime is not running).";
    astrodebug_fujinet_state(_dbg, state, (int)sizeof state);
    setText(_fujiState, @(state), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    t[0] = '\0';
#define ADD(...) do { if (len < sizeof t) { \
        int k_ = snprintf(t + len, sizeof t - len, __VA_ARGS__); \
        if (k_ > 0) len = len + (size_t)k_ < sizeof t ? len + (size_t)k_ : sizeof t - 1; } } while (0)
    for (int i = 0; i < n; i++) {
        ADD("%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) ADD(" \xc3\x97%u", p[i].repeat);
        ADD("\n     ");
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++) ADD("%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) ADD("\xe2\x80\xa6");
    }
    if (n == 0) ADD("(no FujiBus packets since the trace was cleared)");
#undef ADD
    NSString *s = [NSString stringWithUTF8String:t];
    setText(_fujiText, s ? s : @"", follow);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [_timer invalidate];
    astrodebug_set_engaged(_dbg, 0);
    g_singleton = nil;
}

@end
