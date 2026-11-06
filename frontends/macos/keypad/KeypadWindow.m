/*
 * AstroControllers -- the macOS Controllers window: the console's 24-key
 * calculator keypad, the two system buttons and all four hand controllers
 * (stick, trigger, knob dial), with Map mode. One panel, one custom view that
 * paints every control and hit-tests it itself, so a press and a release are
 * real mouse down/up and a control is HELD for as long as the mouse is down
 * -- the same design as the GNOME, KDE and Win32 windows (same layout, in
 * points).
 *
 * FIXED SIZE and floating: an NSPanel with no resize style mask. Hidden at
 * startup. Map mode takes a key, a gamepad button or (for a knob) an axis;
 * leaving Map or hiding the window cancels the capture.
 *
 * NOT COMPILED OR RUN HERE (no Cocoa toolchain on this machine): written from
 * the Win32 window's model, checked on the macOS CI runner.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeypadWindow.h"

#include <math.h>
#include <string.h>

#import "../AstroKeyForward.h"
#include "bindings.h"

enum { SP_NONE = 0, SP_MAP, SP_CLEAR, SP_DEFAULTS, SP_DIAL };

#define KEY_W 62
#define KEY_H 40
#define KEY_GAP 6
#define COL_X 12
#define COL_W 290
#define PL_X 314
#define PL_W 208
#define PL_H 320
#define PL_GAP 12
#define CLIENT_W (PL_X + 2 * PL_W + PL_GAP + 12)
#define CLIENT_H (12 + 2 * PL_H + PL_GAP + 12)

typedef struct {
    NSRect r;
    int target;
    int special;
    int player;
    const char *face;     /* UTF-8 */
    BOOL lit;
} Ctl;

static const char *const kFaces[ASTROSESSION_KEY_COUNT] = {
    "C", "\xE2\x86\x91", "\xE2\x86\x93", "%",
    "MR", "MS", "CH", "\xC3\xB7",
    "7", "8", "9", "\xC3\x97",
    "4", "5", "6", "\xE2\x88\x92",
    "1", "2", "3", "+",
    "CE", "0", ".", "=",
};

static BOOL IsKnobTarget(int t)
{
    return t >= ASTRO_TARGET_PLAYERS &&
           (t - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

static NSColor *Accent(void) { return [NSColor colorWithRed:0.71 green:0.20 blue:0.17 alpha:1]; }

@class ControllersView;
static ControllersView *g_view;

@interface ControllersView : NSView {
    astrosession *_session;
    Ctl _ctl[ASTRO_TARGET_COUNT + 8];
    int _n;
    NSRect _frames[1 + ASTROSESSION_PLAYERS];
    NSRect _padsText, _hintText;
    int _down, _dragDial;
    NSString *_hint, *_pads;
    NSTimer *_timer;
@public
    int mapState;   /* -2 idle, -1 armed, >= 0 a picked target */
}
- (instancetype)initWithSession:(astrosession *)s;
- (BOOL)mapKey:(uint32_t)keysym;
- (void)setMapState:(int)state;
- (void)start;
- (void)stop;
@end

@implementation ControllersView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)add:(NSRect)r target:(int)t special:(int)sp player:(int)p face:(const char *)f
{
    Ctl *c = &_ctl[_n++];
    memset(c, 0, sizeof *c);
    c->r = r; c->target = t; c->special = sp; c->player = p; c->face = f;
}

- (instancetype)initWithSession:(astrosession *)s
{
    self = [super initWithFrame:NSMakeRect(0, 0, CLIENT_W, CLIENT_H)];
    if (!self) return nil;
    _session = s;
    _down = -1;
    _dragDial = -1;
    mapState = -2;
    _hint = @"";
    _pads = @"No gamepad connected";

    _frames[0] = NSMakeRect(COL_X, 12, COL_W, 370);
    for (int k = 0; k < ASTROSESSION_KEY_COUNT; k++)
        [self add:NSMakeRect(COL_X + 12 + (k % 4) * (KEY_W + KEY_GAP),
                             46 + (k / 4) * (KEY_H + KEY_GAP), KEY_W, KEY_H)
           target:ASTRO_TARGET_KEYPAD + k special:SP_NONE player:0 face:kFaces[k]];
    [self add:NSMakeRect(COL_X + 12, 326, 119, 44)
       target:ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME special:SP_NONE player:0 face:"Reset Game"];
    [self add:NSMakeRect(COL_X + 12 + 119 + 6, 326, 119, 44)
       target:ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_CONFIG special:SP_NONE player:0 face:"Reset to CONFIG"];
    _padsText = NSMakeRect(COL_X, 392, COL_W, 36);
    [self add:NSMakeRect(COL_X, 436, 72, 30) target:-1 special:SP_MAP player:0 face:"Map"];
    [self add:NSMakeRect(COL_X + 78, 436, 72, 30) target:-1 special:SP_CLEAR player:0 face:"Clear"];
    [self add:NSMakeRect(COL_X + 156, 436, 84, 30) target:-1 special:SP_DEFAULTS player:0 face:"Defaults"];
    _hintText = NSMakeRect(COL_X, 474, COL_W, 40);

    static const char *const arrows[4] = { "\xE2\x96\xB2", "\xE2\x97\x80", "\xE2\x96\xB6", "\xE2\x96\xBC" };
    for (int p = 0; p < ASTROSESSION_PLAYERS; p++) {
        const CGFloat fx = PL_X + (p % 2) * (PL_W + PL_GAP), fy = 12 + (p / 2) * (PL_H + PL_GAP);
        const CGFloat gx = fx + 22, gy = fy + 32;
        _frames[1 + p] = NSMakeRect(fx, fy, PL_W, PL_H);
        [self add:NSMakeRect(gx + 56, gy, 52, 34) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_UP) special:SP_NONE player:p face:arrows[0]];
        [self add:NSMakeRect(gx, gy + 38, 52, 34) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_LEFT) special:SP_NONE player:p face:arrows[1]];
        [self add:NSMakeRect(gx + 112, gy + 38, 52, 34) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_RIGHT) special:SP_NONE player:p face:arrows[2]];
        [self add:NSMakeRect(gx + 56, gy + 76, 52, 34) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_DOWN) special:SP_NONE player:p face:arrows[3]];
        [self add:NSMakeRect(gx, gy + 116, 164, 40) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_TRIGGER) special:SP_NONE player:p face:"Trigger"];
        [self add:NSMakeRect(fx + 12, gy + 168 + 29, 40, 34) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB_DEC) special:SP_NONE player:p face:arrows[1]];
        [self add:NSMakeRect(fx + 58, gy + 168, 92, 92) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB) special:SP_DIAL player:p face:"Knob"];
        [self add:NSMakeRect(fx + PL_W - 12 - 40, gy + 168 + 29, 40, 34) target:ASTRO_TARGET_PLAYER(p, ASTRO_ACT_KNOB_INC) special:SP_NONE player:p face:arrows[2]];
    }
    return self;
}

- (void)start
{
    if (_timer) return;
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(poll:)
                                            userInfo:nil repeats:YES];
}

- (void)stop
{
    [_timer invalidate];
    _timer = nil;
    [self releaseMouseHold:NO];
    if (mapState != -2) [self setMapState:-2];
}

- (void)setHint:(NSString *)h { _hint = h; [self setNeedsDisplay:YES]; }

- (void)setMapState:(int)state
{
    mapState = state;
    if (state >= 0) {
        [self setHint:[NSString stringWithFormat:IsKnobTarget(state)
                           ? @"Move a gamepad stick or trigger for %s"
                           : @"Press a key or gamepad button for %s", bindings_target_label(state)]];
        astrosession_gamepad_capture_begin(_session);
    } else {
        astrosession_gamepad_capture_cancel(_session);
        [self setHint:state == -1 ? @"Click a control to remap" : @""];
    }
    [self setNeedsDisplay:YES];
}

- (void)pressTarget:(int)t down:(BOOL)down
{
    if (t >= ASTRO_TARGET_SYSACT && t < ASTRO_TARGET_PLAYERS) {
        if (!down)   /* system actions fire on release, like a real button */
            astrosession_sysaction_fire(_session,
                (astrosession_sysaction)(t - ASTRO_TARGET_SYSACT));
        return;
    }
    astrosession_press(_session, t, down);
}

- (int)hit:(NSPoint)p
{
    for (int i = 0; i < _n; i++)
        if (NSPointInRect(p, _ctl[i].r)) return i;
    return -1;
}

- (void)dragDial:(int)i to:(NSPoint)p
{
    const NSRect r = _ctl[i].r;
    const int v = astrosession_knob_from_point(p.x - NSMidX(r), p.y - NSMidY(r), r.size.width / 2.0);
    if (v >= 0) astrosession_knob_set(_session, _ctl[i].player, (uint8_t)v);
    [self setNeedsDisplay:YES];
}

- (void)releaseMouseHold:(BOOL)inside
{
    if (_down >= 0) {
        const Ctl *c = &_ctl[_down];
        if (c->special == SP_NONE && c->target >= 0) {
            const BOOL sys = c->target >= ASTRO_TARGET_SYSACT && c->target < ASTRO_TARGET_PLAYERS;
            if (!sys || inside) [self pressTarget:c->target down:NO];
        }
    }
    _down = -1;
    _dragDial = -1;
}

- (void)mouseDown:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const int i = [self hit:p];
    if (i < 0) return;
    const Ctl *c = &_ctl[i];
    if (c->special == SP_NONE || c->special == SP_DIAL) {
        if (mapState == -1) { [self setMapState:c->target]; return; }
        if (mapState != -2) return;
    }
    if (c->special == SP_DIAL) { _down = i; _dragDial = i; [self dragDial:i to:p]; return; }
    if (c->special != SP_NONE) { [self special:c->special]; return; }
    _down = i;
    [self pressTarget:c->target down:YES];
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)e
{
    if (_dragDial >= 0)
        [self dragDial:_dragDial to:[self convertPoint:e.locationInWindow fromView:nil]];
}

- (void)mouseUp:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const int i = [self hit:p];
    [self releaseMouseHold:(i >= 0 && i == _down)];
    [self setNeedsDisplay:YES];
}

- (void)special:(int)sp
{
    if (sp == SP_MAP) {
        [self setMapState:mapState == -2 ? -1 : -2];
    } else if (sp == SP_CLEAR && mapState >= 0) {
        const int t = mapState;
        bindings_clear(_session, t);
        [self setMapState:-1];
        [self setHint:[NSString stringWithFormat:@"%s cleared", bindings_target_label(t)]];
    } else if (sp == SP_DEFAULTS) {
        bindings_reset_defaults(_session);
        [self setHint:@"Bindings reset to the defaults"];
    }
    [self setNeedsDisplay:YES];
}

- (BOOL)mapKey:(uint32_t)keysym
{
    const int target = mapState;
    if (target == -2) return NO;
    if (target == -1) return YES;   /* swallow keys while choosing a target */
    if (IsKnobTarget(target)) {
        [self setHint:@"The knob follows a gamepad axis: move a stick or trigger"];
        return YES;
    }
    int from = -1;
    char name[48];
    bindings_set_keysym(_session, target, keysym, &from);
    astrosession_keysym_name(keysym, name, sizeof name);
    NSString *msg = [NSString stringWithFormat:@"%s = %s", bindings_target_label(target), name];
    if (from >= 0)
        msg = [msg stringByAppendingFormat:@" (was %s)", bindings_target_label(from)];
    [self setMapState:-1];   /* stay armed */
    [self setHint:msg];
    return YES;
}

- (void)poll:(NSTimer *)t
{
    (void)t;
    if (!self.window.isVisible) return;
    char all[256] = "", name[64];
    const int n = astrosession_gamepad_count(_session);
    for (int i = 0; i < n; i++) {
        astrosession_gamepad_name(_session, i, name, sizeof name);
        snprintf(all + strlen(all), sizeof all - strlen(all), "%sPlayer %d: %s", i ? "   " : "", i + 1, name);
    }
    NSString *pads = n ? @(all) : @"No gamepad connected";
    if (![pads isEqualToString:_pads]) { _pads = pads; [self setNeedsDisplay:YES]; }

    if (mapState >= 0) {
        int id, from = -1;
        const int target = mapState;
        if (!astrosession_gamepad_capture_poll(_session, &id)) return;
        if (IsKnobTarget(target) && id < ASTRO_PAD_AXIS_BASE) {
            astrosession_gamepad_capture_begin(_session);   /* a button cannot turn a knob */
            [self setHint:@"The knob follows an axis: move a stick or trigger"];
            return;
        }
        bindings_set_pad(_session, target, id, &from);
        astrosession_pad_input_name(id, name, sizeof name);
        NSString *msg = [NSString stringWithFormat:@"%s = pad %s", bindings_target_label(target), name];
        if (from >= 0) msg = [msg stringByAppendingFormat:@" (was %s)", bindings_target_label(from)];
        [self setMapState:-1];
        [self setHint:msg];
        return;
    }
    BOOL changed = YES;   /* the dials follow the knob: repaint, it is cheap */
    if (mapState == -2)
        for (int i = 0; i < _n; i++) {
            if (_ctl[i].special != SP_NONE || _ctl[i].target < 0) continue;
            _ctl[i].lit = astrosession_target_held(_session, _ctl[i].target) != 0;
        }
    if (changed) [self setNeedsDisplay:YES];
}

/* ---- keyboard ------------------------------------------------------------ */

- (void)keyDown:(NSEvent *)e
{
    if (e.isARepeat) return;
    if (!AstroForwardKeyEvent(_session, e, 1) && e.keyCode == 40 /* K */ &&
        (e.modifierFlags & NSEventModifierFlagCommand))
        [self.window orderOut:nil];
}
- (void)keyUp:(NSEvent *)e { AstroForwardKeyEvent(_session, e, 0); }
- (void)flagsChanged:(NSEvent *)e { AstroForwardModifierEvent(_session, e); }

/* ---- painting -------------------------------------------------------------- */

- (void)text:(NSString *)s in:(NSRect)r color:(NSColor *)c size:(CGFloat)sz bold:(BOOL)bold
       align:(NSTextAlignment)al
{
    NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
    ps.alignment = al;
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary *a = @{ NSFontAttributeName: bold ? [NSFont boldSystemFontOfSize:sz] : [NSFont systemFontOfSize:sz],
                         NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps };
    const NSSize ts = [s sizeWithAttributes:a];
    if (ts.height < r.size.height) {   /* centre vertically unless it is a paragraph */
        r.origin.y += (r.size.height - ts.height) / 2;
        r.size.height = ts.height;
    }
    [s drawInRect:r withAttributes:a];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor windowBackgroundColor] setFill];
    NSRectFill(self.bounds);
    static NSString *const titles[1 + ASTROSESSION_PLAYERS] = {
        @"Console keypad", @"Player 1", @"Player 2", @"Player 3", @"Player 4" };
    for (int f = 0; f < 1 + ASTROSESSION_PLAYERS; f++) {
        NSBezierPath *bp = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(_frames[f], 0.5, 0.5) xRadius:8 yRadius:8];
        [[NSColor separatorColor] setStroke];
        [bp stroke];
        [self text:titles[f] in:NSMakeRect(_frames[f].origin.x, _frames[f].origin.y + 8, _frames[f].size.width, 22)
             color:[NSColor labelColor] size:13 bold:YES align:NSTextAlignmentCenter];
    }
    for (int i = 0; i < _n; i++) {
        const Ctl *c = &_ctl[i];
        if (c->special == SP_DIAL) { [self drawDial:c]; continue; }
        const BOOL gold = c->target >= 0 && c->target < ASTROSESSION_KEY_COUNT && c->target % 4 == 3;
        const BOOL lit = c->lit || (mapState >= 0 && c->target == mapState) ||
                         (c->special == SP_MAP && mapState != -2) || _down == i;
        NSColor *face = [NSColor colorWithWhite:0.5 alpha:0.2], *tc = [NSColor labelColor];
        if (c->special == SP_CLEAR && mapState < 0) tc = [NSColor disabledControlTextColor];
        if (gold) { face = [NSColor colorWithRed:0.91 green:0.76 blue:0.29 alpha:1]; tc = [NSColor colorWithRed:0.23 green:0.17 blue:0 alpha:1]; }
        if (lit) { face = Accent(); tc = [NSColor whiteColor]; }
        [face setFill];
        [[NSBezierPath bezierPathWithRoundedRect:c->r xRadius:8 yRadius:8] fill];
        NSString *label = @(c->face);
        CGFloat sz = c->target >= 0 && c->target < ASTROSESSION_KEY_COUNT ? 16 : 13;
        BOOL bold = c->target >= 0 && c->target < ASTROSESSION_KEY_COUNT;
        if (mapState != -2 && c->special == SP_NONE && c->target >= 0) {
            char key[48], pad[48];
            astrosession_keysym_name(bindings_target_keysym(c->target), key, sizeof key);
            astrosession_pad_input_name(bindings_target_pad(c->target), pad, sizeof pad);
            label = pad[0] ? [NSString stringWithFormat:@"%s\n%s", key[0] ? key : "\xE2\x80\x94", pad]
                           : [NSString stringWithUTF8String:key[0] ? key : "\xE2\x80\x94"];
            sz = 11; bold = NO;
        }
        [self text:label in:NSInsetRect(c->r, 2, 2) color:tc size:sz bold:bold align:NSTextAlignmentCenter];
    }
    for (int i = 0; i < _n; i++) {
        if (_ctl[i].special != SP_DIAL) continue;
        NSString *cap = @"Knob";
        if (mapState != -2) {
            char pad[48];
            astrosession_pad_input_name(bindings_target_pad(_ctl[i].target), pad, sizeof pad);
            cap = [NSString stringWithUTF8String:pad[0] ? pad : "\xE2\x80\x94"];
        }
        NSRect r = _ctl[i].r;
        r.origin.y = NSMaxY(r) + 2; r.size.height = 18; r.origin.x -= 20; r.size.width += 40;
        [self text:cap in:r color:[NSColor secondaryLabelColor] size:11 bold:NO align:NSTextAlignmentCenter];
    }
    [self text:_pads in:_padsText color:[NSColor secondaryLabelColor] size:11 bold:NO align:NSTextAlignmentLeft];
    [self text:_hint in:_hintText color:[NSColor secondaryLabelColor] size:11 bold:NO align:NSTextAlignmentLeft];
}

- (void)drawDial:(const Ctl *)c
{
    const CGFloat cx = NSMidX(c->r), cy = NSMidY(c->r), rad = c->r.size.width / 2 - 4;
    NSColor *fg = [NSColor labelColor];
    const BOOL picked = mapState == c->target;
    NSBezierPath *arc = [NSBezierPath bezierPath];
    for (int d = -135; d <= 135; d += 5) {
        const double a = d * M_PI / 180.0;
        const NSPoint pt = NSMakePoint(cx + sin(a) * rad, cy - cos(a) * rad);
        if (d == -135) [arc moveToPoint:pt]; else [arc lineToPoint:pt];
    }
    [fg setStroke];
    arc.lineWidth = 2;
    [arc stroke];
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rad * 0.62, cy - rad * 0.62, rad * 1.24, rad * 1.24)];
    ring.lineWidth = 1;
    [ring stroke];
    const double a = astrosession_knob_angle(astrosession_knob_get(_session, c->player)) * M_PI / 180.0;
    NSBezierPath *hand = [NSBezierPath bezierPath];
    [hand moveToPoint:NSMakePoint(cx + sin(a) * rad * 0.25, cy - cos(a) * rad * 0.25)];
    [hand lineToPoint:NSMakePoint(cx + sin(a) * rad * 0.95, cy - cos(a) * rad * 0.95)];
    hand.lineWidth = 3;
    hand.lineCapStyle = NSLineCapStyleRound;
    [(picked ? Accent() : fg) setStroke];
    [hand stroke];
}

@end

/* ---- the window ---------------------------------------------------------- */

static NSPanel *g_panel;

@implementation AstroControllers

+ (void)toggleForSession:(astrosession *)session
{
    if (!g_panel) {
        g_view = [[ControllersView alloc] initWithSession:session];
        g_panel = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, CLIENT_W, CLIENT_H)
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskUtilityWindow)
                        backing:NSBackingStoreBuffered defer:NO];
        g_panel.title = @"Controllers";
        g_panel.floatingPanel = YES;
        g_panel.hidesOnDeactivate = NO;
        g_panel.releasedWhenClosed = NO;
        g_panel.contentView = g_view;
        [g_panel center];
        [g_panel makeFirstResponder:g_view];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
            object:g_panel queue:nil usingBlock:^(NSNotification *n) {
                (void)n;
                astrosession_release_all(session);
            }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
            object:g_panel queue:nil usingBlock:^(NSNotification *n) { (void)n; [g_view stop]; }];
    }
    if (g_panel.isVisible) {
        [g_panel orderOut:nil];
        [g_view stop];
    } else {
        [g_view start];
        [g_panel makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible { return g_panel.isVisible; }

@end

BOOL AstroControllersMapKey(uint32_t keysym)
{
    return g_panel.isVisible && [g_view mapKey:keysym];
}
