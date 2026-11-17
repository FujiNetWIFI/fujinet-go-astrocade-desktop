/*
 * AstroControllersWindow -- the AppKit Controllers panel: the console's 24-key
 * calculator keypad, the four Bally pistol-grip hand controllers and the two
 * console buttons drawn from the session's layout (astrosession_controls_layout)
 * with NSBezierPath, so every frontend shows the same console; the gamepads and
 * the player each one plays; and Map mode.
 *
 * The console is a walnut case with the keypad in a gold-framed black recess:
 * a champagne plate, a gold column down the right, dark brown keys with their
 * legends printed above them. With the Bally BASIC overlay on, the plate
 * becomes the overlay card (ivory, black legend bars): each key shows its white
 * legend and gold word, the green / red / blue boxes above it hold the
 * characters its shift colour types, and the bottom row (CE 0 . =) is the four
 * shifts. Each hand controller is a black pistol grip: the ribbed orange-brown
 * knob on top is the dial (a paddle: drag it round, scroll nudges it), ringed
 * by eight direction wedges (the whole grip tilts as the stick), a trigger at
 * the front, and the gold Bally badge. One custom-drawn view (AstroConsoleView)
 * paints it and takes the mouse; the layout, the exact hit shapes and the
 * stick and dial maths are the core's, the colours are here.
 *
 * Gestures: press a key or the trigger and it is held until the button is
 * released or dragged off; press a wedge for that direction; press the disc and
 * drag for eight ways (nothing near the middle); drag on the knob to turn it
 * (the knob holds where it is left). The pointer is its own input source, so
 * the panel lets go of everything the mouse holds when it is hidden or loses
 * key status. One pointer holds one control at a time: chords need the
 * keyboard or a pad. What the machine sees held -- host keyboard, gamepads and
 * the mouse -- is painted in the accent (the family's Bally gold), so the panel
 * doubles as an input tester for a new gamepad. Keys typed with the panel key
 * go to the machine as in the main window (with the overlay on, a character
 * types BASIC); no view here takes the keyboard, so the panel itself is the
 * first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents.
 * Hidden at start, ordered out rather than closed, so its position survives.
 * The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all ASTRO_TARGET_COUNT targets, the console actions
 * among them), then press a key or a gamepad button (for a knob, move a gamepad
 * axis); the next one is bound and whatever held it is told so in the status
 * line. Esc cancels the pick. Rebinding steals the key from its old target. F9,
 * F11 and F12 are the app's and are never bound.
 *
 * Written to the family's AppKit pattern (see the ADAM and Coleco panels);
 * built and verified on CI only (no Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../AstroKeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#include "bindings.h"

#define UNIT         30.0    /* points per layout unit (a keypad key's width) */
#define CASE_PAD     16.0    /* the case round the controls */
#define BRAND_H      22.0    /* the brand strip along the top */
#define TOP_GAP      10.0
#define MAX_CTL      96      /* room for the core's 75 controls */
#define MARGIN       16.0
#define TITLE_H      18.0
#define ROW_H        24.0
#define EFF_W       120.0
#define POPUP_W     108.0
#define BTN_H        28.0
#define MIN_PAD_ROWS 2       /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8       /* the 30 Hz tick's 250 ms part */
#define DIM_ALPHA    0.4     /* parts that are not the Map pick */
#define BADGE_NAME   "Bally"

/* ---- colours (DESIGN-astrocade.md: photo-read where the photos agree, else
 * inferred) ------------------------------------------------------------------ */

#define COL_CASE_TOP    0x9A6A3E     /* walnut */
#define COL_CASE_BOT    0x7A4C28
#define COL_CASE_EDGE   0x4A2C14
#define COL_PINSTRIPE   0xB08A3C
#define COL_BRAND_TEXT  0xEBDDB8
#define COL_CAPTION     0xEBDDB8
#define COL_RECESS_TOP  0x2A2928     /* the black top the keypad sits in */
#define COL_RECESS_BOT  0x161515
#define COL_RECESS_RIM  0xC9A64E
#define COL_PLATE_TOP   0xE2CFAE     /* champagne */
#define COL_PLATE_BOT   0xD3BF98
#define COL_PLATE_RIM   0x8A6F3C
#define COL_GOLD_COL    0xCFAE5E
#define COL_KEY         0x342626
#define COL_KEY_TOP     0x4A3A3A
#define COL_KEY_HOT     0x5C4545
#define COL_KEY_SIDE    0x1E1414
#define COL_KEY_INK     0xE2CFAE
#define COL_LEGEND      0x4A3A30
#define COL_CARD        0xFBEFDD     /* the Bally BASIC overlay */
#define COL_CARD_RIM    0x6F6650
#define COL_BAR         0x1A1A1A
#define COL_PLAIN       0xF2F2F2
#define COL_WORDS       0xE6BC4C
#define COL_GREEN       0x3FA55A
#define COL_RED         0xE04A42
#define COL_BLUE        0x2878B0
#define COL_SHELL_TOP   0x262424     /* the pistol grip */
#define COL_SHELL_BOT   0x131212
#define COL_SHELL_RIM   0x777476
#define COL_KNURL       0x2A2626
#define COL_KNURL_DOT   0x4C3D3D
#define COL_BADGE       0xD4A93A
#define COL_DISC        0x0F0F0F
#define COL_DISC_RIM    0x3A3737
#define COL_WEDGE       0x2E2C2C
#define COL_WEDGE_HOT   0x433F3F
#define COL_WEDGE_INK   0x8A8484
#define COL_KNOB_LIGHT  0xE67A2C
#define COL_KNOB_MID    0xC0642A
#define COL_KNOB_DARK   0x8A4318
#define COL_KNOB_CAP    0xA44E20
#define COL_KNOB_INK    0xF6E4C8
#define COL_TRIG_TOP    0xB4B4BA
#define COL_TRIG_BOT    0x8A8A90
#define COL_TRIG_EDGE   0x2A2A2E
#define COL_SYS_TOP     0x242424
#define COL_SYS_BOT     0x0E0E0E
#define COL_SYS_HOT     0x3A3A3A
#define COL_SYS_INK     0xD4B04C

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) astrosession_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSString *padInputName(int ident)
{
    char name[64];
    name[0] = '\0';
    if (ident >= 0) astrosession_pad_input_name(ident, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

/* The accent: the Bally gold, with black text on it. */
static NSColor *accent(void) { return rgb(ASTROSESSION_ACCENT_RGB); }

/* Toggled-on look for a push button: accent bezel, black title. */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accent();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.blackColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

static BOOL isKnobTarget(int target)
{
    return target >= ASTRO_TARGET_PLAYERS &&
           (target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

/* A vertical gradient clipped to a path (view coordinates: the views here are
 * flipped, so "top" is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    const CGFloat comps[8] = {
        ((topRgb >> 16) & 0xff) / 255.0, ((topRgb >> 8) & 0xff) / 255.0, (topRgb & 0xff) / 255.0, 1.0,
        ((botRgb >> 16) & 0xff) / 255.0, ((botRgb >> 8) & 0xff) / 255.0, (botRgb & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(NSMinX(bnd), NSMinY(bnd)),
                                CGPointMake(NSMinX(bnd), NSMaxY(bnd)), 0);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A radial gradient clipped to a path: the light colour at lit, the dark one
 * at the rim of the circle round `centre` with `radius`. */
static void fillRadial(NSBezierPath *path, NSPoint lit, NSPoint centre, CGFloat radius,
                       uint32_t lightRgb, uint32_t darkRgb)
{
    const CGFloat comps[8] = {
        ((lightRgb >> 16) & 0xff) / 255.0, ((lightRgb >> 8) & 0xff) / 255.0, (lightRgb & 0xff) / 255.0, 1.0,
        ((darkRgb >> 16) & 0xff) / 255.0, ((darkRgb >> 8) & 0xff) / 255.0, (darkRgb & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawRadialGradient(ctx, grad, CGPointMake(lit.x, lit.y), 0, CGPointMake(centre.x, centre.y),
                                radius, kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* Lines of text centred in rect, the font shrunk (to 5 pt) until they fit. */
static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat widest = 0;
        lineHeight = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > widest) widest = extent.width;
            if (extent.height > lineHeight) lineHeight = extent.height;
        }
        if ((widest <= rect.size.width && lineHeight * count <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
        ypos += lineHeight;
    }
}

/* The sector of a ring between radii inner and outer, centred on centreDeg
 * and halfDeg either side of it. Angles are mathematical (counter-clockwise
 * as seen, 0 = right); the view is flipped, so y is subtracted. Straight
 * segments, so nothing depends on how AppKit turns arcs in a flipped view. */
static NSBezierPath *ringSector(NSPoint ctr, CGFloat inner, CGFloat outer, double centreDeg, double halfDeg)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    const int steps = 8;
    for (int idx = 0; idx <= steps; idx++) {
        const double ang = (centreDeg - halfDeg + 2.0 * halfDeg * idx / steps) * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(ctr.x + outer * cos(ang), ctr.y - outer * sin(ang));
        if (idx == 0) [path moveToPoint:pnt]; else [path lineToPoint:pnt];
    }
    for (int idx = steps; idx >= 0; idx--) {
        const double ang = (centreDeg - halfDeg + 2.0 * halfDeg * idx / steps) * M_PI / 180.0;
        [path lineToPoint:NSMakePoint(ctr.x + inner * cos(ang), ctr.y - inner * sin(ang))];
    }
    [path closePath];
    return path;
}

/* The direction a wedge points, in degrees, from its action bits. */
static double wedgeAngle(unsigned acts)
{
    const int dirx = (int)((acts >> ASTRO_ACT_RIGHT) & 1u) - (int)((acts >> ASTRO_ACT_LEFT) & 1u);
    const int diry = (int)((acts >> ASTRO_ACT_UP) & 1u) - (int)((acts >> ASTRO_ACT_DOWN) & 1u);
    return atan2((double)diry, (double)dirx) * 180.0 / M_PI;
}

static double ease(double cur, double goal)
{
    const double diff = goal - cur;
    return fabs(diff) < 0.004 ? goal : cur + diff * 0.4;
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface AstroFlippedView : NSView
@end

@implementation AstroFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class AstroControllersWindow;
@interface AstroControllersPanel : NSPanel
@property (nonatomic, weak) AstroControllersWindow *owner;
@end

@interface AstroControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)runSysaction:(int)sysact;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
- (BOOL)mapKeyFromApp:(uint32_t)keysym;
@end

/* How far a grip's knob is shifted now, eased toward where the held contacts
 * put it: -1..1, +x right, +y toward the player. */
typedef struct { double tiltx, tilty; } AstroTilt;

/* The console: the case with its brand strip, the keypad, the four hand
 * controllers and the console buttons. Everything on it is drawn here and
 * hit-tested by the core (except the dial drag, which is the core's maths too). */
@interface AstroConsoleView : AstroFlippedView
@property (nonatomic) astrosession *session;
@property (nonatomic, weak) AstroControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic) BOOL overlay;          /* the Bally BASIC overlay card is on */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Reads what the machine sees held and moves the grips toward it; redraws when
 * anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation AstroConsoleView {
    const astro_control *_ctl;
    int _num;
    float _layW, _layH;
    NSSize _size;
    CGFloat _pitchX, _zoneH;     /* a keypad key's pitch, and its legend zone's height (points) */
    BOOL _lit[MAX_CTL];
    unsigned _stick[ASTROSESSION_PLAYERS];
    int _knob[ASTROSESSION_PLAYERS];
    AstroTilt _tilt[ASTROSESSION_PLAYERS];
    int _hover;                  /* the control under the pointer, or -1 */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downPlayer;             /* the player whose stick the pointer drives, or -1 */
    unsigned _downMask;
    BOOL _stickDrag;
    BOOL _dialDrag;
    BOOL _sysDown;               /* the pressed control is a console button */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downPlayer = -1;
    _hover = -1;
    _mapTarget = -1;
    for (int pl = 0; pl < ASTROSESSION_PLAYERS; pl++) _knob[pl] = -1;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

- (void)configure
{
    _ctl = astrosession_controls_layout(&_num, &_layW, &_layH);
    if (!_ctl) _num = 0;
    if (_num > MAX_CTL) _num = MAX_CTL;
    _size = NSMakeSize(ceil(_layW * UNIT) + 2 * CASE_PAD,
                       BRAND_H + TOP_GAP + ceil(_layH * UNIT) + CASE_PAD);
    /* the keypad's pitch, from its first keys: the legend zone is what is left
     * above a key in a pitch */
    _pitchX = 1.35 * UNIT;
    _zoneH = 0.5 * UNIT;
    const astro_control *key0 = NULL, *key1 = NULL, *key4 = NULL;
    for (int idx = 0; idx < _num; idx++) {
        if (_ctl[idx].kind != ASTRO_CTL_KEY) continue;
        const int key = _ctl[idx].target - ASTRO_TARGET_KEYPAD;
        if (key == 0) key0 = &_ctl[idx];
        if (key == 1) key1 = &_ctl[idx];
        if (key == 4) key4 = &_ctl[idx];
    }
    if (key0 && key1 && key4) {
        _pitchX = (key1->xpos - key0->xpos) * UNIT;
        _zoneH = (key4->ypos - key0->ypos - key0->height) * UNIT;
    }
    memset(_lit, 0, sizeof _lit);
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (void)setOverlay:(BOOL)overlay
{
    if (overlay == _overlay) return;
    _overlay = overlay;
    [self setNeedsDisplay:YES];
}

/* ---- geometry ------------------------------------------------------------------ */

- (NSPoint)caseOrigin
{
    return NSMakePoint(CASE_PAD, BRAND_H + TOP_GAP);
}

- (NSRect)rectOf:(const astro_control *)ctl
{
    const NSPoint org = [self caseOrigin];
    return NSMakeRect(org.x + ctl->xpos * UNIT, org.y + ctl->ypos * UNIT, ctl->width * UNIT, ctl->height * UNIT);
}

/* The control under a view point, or -1. */
- (int)hitAt:(NSPoint)pnt
{
    const NSPoint org = [self caseOrigin];
    const int idx = astrosession_controls_hit((float)((pnt.x - org.x) / UNIT), (float)((pnt.y - org.y) / UNIT));
    return idx >= 0 && idx < _num ? idx : -1;
}

/* The STICK disc of a player: its centre and its radius, in view points. */
- (BOOL)stickOfPlayer:(int)player centre:(NSPoint *)centre radius:(CGFloat *)radius
{
    for (int idx = 0; idx < _num; idx++) {
        const astro_control *ctl = &_ctl[idx];
        if (ctl->kind != ASTRO_CTL_STICK || ctl->player != player) continue;
        const NSRect rect = [self rectOf:ctl];
        *centre = NSMakePoint(NSMidX(rect), NSMidY(rect));
        *radius = rect.size.width / 2;
        return YES;
    }
    return NO;
}

/* The target a control stands for in Map mode: the dial's is its player's knob
 * axis; a diagonal wedge, the disc and the captions have none. */
- (int)mapTargetOf:(const astro_control *)ctl
{
    if (ctl->kind == ASTRO_CTL_KNOB && ctl->player >= 0 && ctl->player < ASTROSESSION_PLAYERS)
        return ASTRO_TARGET_PLAYER(ctl->player, ASTRO_ACT_KNOB);
    return ctl->target;
}

/* ---- the held state --------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    const unsigned dirbits = (1u << ASTRO_ACT_UP) | (1u << ASTRO_ACT_DOWN) |
                             (1u << ASTRO_ACT_LEFT) | (1u << ASTRO_ACT_RIGHT);
    for (int pl = 0; pl < ASTROSESSION_PLAYERS; pl++) {
        unsigned mask = 0;
        if (!self.mapping) {
            /* what the machine sees, opposed directions already cancelled */
            for (int act = 0; act < ASTRO_ACT_KNOB_DEC; act++)
                if (astrosession_target_held(_session, ASTRO_TARGET_PLAYER(pl, act)))
                    mask |= 1u << act;
        }
        _stick[pl] = mask & dirbits;
        const int knob = astrosession_knob_get(_session, pl);
        if (knob != _knob[pl]) {
            _knob[pl] = knob;
            dirty = YES;
        }
    }

    for (int idx = 0; idx < _num; idx++) {
        const astro_control *ctl = &_ctl[idx];
        BOOL lit = NO;
        switch (ctl->kind) {
        case ASTRO_CTL_KEY:
        case ASTRO_CTL_TRIGGER:
            lit = !self.mapping && ctl->target >= 0 && astrosession_target_held(_session, ctl->target);
            break;
        case ASTRO_CTL_DIRECTION:
            /* the wedge the stick is in: a diagonal lights only its own */
            lit = ctl->player >= 0 && ctl->player < ASTROSESSION_PLAYERS && ctl->act_mask != 0 &&
                  _stick[ctl->player] == ctl->act_mask;
            break;
        default:
            break;
        }
        if (lit != _lit[idx]) {
            _lit[idx] = lit;
            dirty = YES;
        }
    }

    for (int pl = 0; pl < ASTROSESSION_PLAYERS; pl++) {
        double tiltx = (double)((_stick[pl] >> ASTRO_ACT_RIGHT) & 1u) - (double)((_stick[pl] >> ASTRO_ACT_LEFT) & 1u);
        double tilty = (double)((_stick[pl] >> ASTRO_ACT_DOWN) & 1u) - (double)((_stick[pl] >> ASTRO_ACT_UP) & 1u);
        if (tiltx != 0 && tilty != 0) { tiltx *= 0.7071; tilty *= 0.7071; }
        AstroTilt tilt = _tilt[pl];
        tilt.tiltx = ease(tilt.tiltx, tiltx);
        tilt.tilty = ease(tilt.tilty, tilty);
        if (tilt.tiltx != _tilt[pl].tiltx || tilt.tilty != _tilt[pl].tilty) {
            _tilt[pl] = tilt;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

- (NSString *)legendFor:(const astro_control *)ctl
{
    if (!(self.mapping || self.showKeys)) return nil;
    const int target = [self mapTargetOf:ctl];
    return target < 0 ? nil : [self.owner legendForTarget:target];
}

- (BOOL)selected:(const astro_control *)ctl
{
    const int target = [self mapTargetOf:ctl];
    return self.mapping && target >= 0 && target == self.mapTarget;
}

/* Whether a part is drawn faint: in Map mode everything but the pick (or, while
 * nothing is picked yet, the part under the pointer). */
- (BOOL)dimmed:(int)idx
{
    if (!self.mapping) return NO;
    const astro_control *ctl = &_ctl[idx];
    if (ctl->kind == ASTRO_CTL_CAPTION || ctl->kind == ASTRO_CTL_OVERLAY) return NO;
    if ([self selected:ctl]) return NO;
    return !(self.mapTarget < 0 && idx == _hover);
}

- (void)beginDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, DIM_ALPHA);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* A held part: the accent fill with a white outline. */
- (void)strokeHeld:(NSBezierPath *)path
{
    [[NSColor whiteColor] setStroke];
    path.lineWidth = 2;
    [path stroke];
}

/* The key's legend zone: the strip above it, a pitch wide. */
- (NSRect)zoneOfKeyRect:(NSRect)keyRect
{
    const CGFloat extra = (_pitchX - keyRect.size.width) / 2;
    return NSMakeRect(keyRect.origin.x - extra, keyRect.origin.y - _zoneH, _pitchX, _zoneH);
}

/* The keypad plate: the black recess with its gold frame, and in it either the
 * champagne plate with the gold column, or the overlay card with its black
 * legend bars. Drawn, never pressed. */
- (void)drawPlate:(const astro_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *recess = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -7, -7)
                                                           xRadius:12 yRadius:12];
    fillGradient(recess, COL_RECESS_TOP, COL_RECESS_BOT);
    [rgb(COL_RECESS_RIM) setStroke];
    recess.lineWidth = 1.5;
    [recess stroke];

    NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
    if (self.overlay) {
        [rgb(COL_CARD) setFill];
        [plate fill];
        [rgb(COL_CARD_RIM) setStroke];
        plate.lineWidth = 1;
        [plate stroke];
        /* a black legend bar above each row of keys */
        for (int row = 0; row < 6; row++) {
            for (int idx = 0; idx < _num; idx++) {
                const astro_control *key = &_ctl[idx];
                if (key->kind != ASTRO_CTL_KEY || key->target != ASTRO_TARGET_KEYPAD + row * 4) continue;
                const NSRect zone = [self zoneOfKeyRect:[self rectOf:key]];
                NSBezierPath *bar = [NSBezierPath bezierPathWithRoundedRect:
                    NSMakeRect(rect.origin.x + 5, zone.origin.y + 1, rect.size.width - 10, zone.size.height - 2)
                                                                    xRadius:3 yRadius:3];
                [rgb(COL_BAR) setFill];
                [bar fill];
            }
        }
        return;
    }
    fillGradient(plate, COL_PLATE_TOP, COL_PLATE_BOT);
    [rgb(COL_PLATE_RIM) setStroke];
    plate.lineWidth = 1;
    [plate stroke];
    /* the gold column down the right, from the second row (DIV) to the bottom */
    for (int idx = 0; idx < _num; idx++) {
        const astro_control *key = &_ctl[idx];
        if (key->kind != ASTRO_CTL_KEY || key->target != ASTRO_TARGET_KEYPAD + 7) continue;
        const NSRect first = [self zoneOfKeyRect:[self rectOf:key]];
        NSBezierPath *gold = [NSBezierPath bezierPathWithRoundedRect:
            NSMakeRect(first.origin.x + 2, first.origin.y, first.size.width - 4,
                       NSMaxY(rect) - first.origin.y - 3)
                                                            xRadius:4 yRadius:4];
        [rgb(COL_GOLD_COL) setFill];
        [gold fill];
        break;
    }
}

/* The overlay's three small boxes (green, red, blue) in a key's legend zone, or
 * for a shift key its colour's dot. */
- (void)drawOverlayZone:(int)key zone:(NSRect)zone
{
    const astro_overlay_key *ov = astrosession_overlay_layout();
    const int shift = astrosession_overlay_key_shift(key);
    static const uint32_t cols[ASTRO_OVERLAY_SHIFTS] = { 0, COL_GREEN, COL_RED, COL_BLUE, COL_WORDS };
    if (shift >= 0) {
        if (shift == ASTRO_SHIFT_WORDS) return;     /* the key says WORDS itself */
        const CGFloat dia = MIN(zone.size.height - 4, 9);
        NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(NSMidX(zone) - dia / 2, NSMidY(zone) - dia / 2, dia, dia)];
        [rgb(cols[shift]) setFill];
        [dot fill];
        return;
    }
    if (!ov) return;
    const CGFloat boxw = (zone.size.width - 6) / 3;
    for (int col = 0; col < 3; col++) {
        const char *text = ov[key].legend[ASTRO_SHIFT_GREEN + col];
        if (!text || !text[0]) continue;
        const NSRect box = NSMakeRect(zone.origin.x + 2 + col * (boxw + 1), zone.origin.y + 2.5,
                                      boxw, zone.size.height - 5);
        NSBezierPath *rounded = [NSBezierPath bezierPathWithRoundedRect:box xRadius:2 yRadius:2];
        [rgb(cols[ASTRO_SHIFT_GREEN + col]) setFill];
        [rounded fill];
        drawFitted(nsstr(text), NSInsetRect(box, 0.5, 0), 8, YES, [NSColor whiteColor]);
    }
}

/* A keypad key: dark brown with its legend printed above it (or, with the
 * overlay on, the overlay's); the bound key on its face in Map / Show bindings. */
- (void)drawKey:(const astro_control *)ctl idx:(int)idx lit:(BOOL)lit
{
    const int key = ctl->target - ASTRO_TARGET_KEYPAD;
    const BOOL held = lit || [self selected:ctl];
    const BOOL showing = self.mapping || self.showKeys;
    const NSRect keyRect = [self rectOf:ctl];
    const NSRect rect = NSInsetRect(keyRect, 0.5, 0.5);
    NSBezierPath *side = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:3.5 yRadius:3.5];
    [rgb(COL_KEY_SIDE) setFill];
    [side fill];
    const NSRect topRect = NSMakeRect(rect.origin.x, rect.origin.y, rect.size.width, rect.size.height - 3);
    NSBezierPath *top = [NSBezierPath bezierPathWithRoundedRect:topRect xRadius:3.5 yRadius:3.5];
    if (held) {
        [accent() setFill];
        [top fill];
        [self strokeHeld:top];
    } else {
        fillGradient(top, idx == _hover && !self.mapping ? COL_KEY_HOT : COL_KEY_TOP, COL_KEY);
    }
    const NSRect inner = NSInsetRect(topRect, 1.5, 0.5);

    /* the face */
    NSString *bound = [self legendFor:ctl];
    if (showing) {
        if (bound.length)
            drawFitted(bound, inner, 8, NO, held ? [NSColor blackColor] : rgb(COL_KEY_INK));
    } else if (self.overlay && key >= 0 && key < ASTROSESSION_KEY_COUNT) {
        const astro_overlay_key *ov = astrosession_overlay_layout();
        const int shift = astrosession_overlay_key_shift(key);
        if (shift >= 0) {
            static const char *const names[ASTRO_OVERLAY_SHIFTS] = { "", "GREEN", "RED", "BLUE", "WORDS" };
            static const uint32_t cols[ASTRO_OVERLAY_SHIFTS] = { 0, COL_GREEN, COL_RED, COL_BLUE, COL_WORDS };
            drawFitted(nsstr(names[shift]), inner, 8, YES, held ? [NSColor blackColor] : rgb(cols[shift]));
        } else if (ov) {
            const char *plain = ov[key].legend[ASTRO_SHIFT_NONE];
            const char *word = ov[key].legend[ASTRO_SHIFT_WORDS];
            NSColor *plainInk = held ? [NSColor blackColor] : rgb(COL_PLAIN);
            if (word && word[0]) {
                const CGFloat splitH = floor(inner.size.height * 0.52);
                drawFitted(nsstr(plain), NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, splitH),
                           11, YES, plainInk);
                drawFitted(nsstr(word), NSMakeRect(inner.origin.x, inner.origin.y + splitH, inner.size.width,
                                                   inner.size.height - splitH),
                           7, YES, held ? [NSColor blackColor] : rgb(COL_WORDS));
            } else {
                drawFitted(nsstr(plain), inner, 11, YES, plainInk);
            }
        }
    }

    /* the legend zone above the key */
    const NSRect zone = [self zoneOfKeyRect:keyRect];
    if (self.overlay && key >= 0 && key < ASTROSESSION_KEY_COUNT)
        [self drawOverlayZone:key zone:zone];
    else
        drawFitted(nsstr(ctl->label), NSInsetRect(zone, 1, 1), 11, YES, rgb(COL_LEGEND));
}

/* A hand controller's black pistol-grip shell and its name above it. The caption
 * control's rectangle is the top of the card; the card is CARD_W (3.6) units
 * wide. */
- (void)drawShell:(const astro_control *)ctl
{
    const NSRect cap = [self rectOf:ctl];
    const CGFloat ox = cap.origin.x, oy = cap.origin.y;
#define U(v) ((v) * UNIT)
    NSBezierPath *shell = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(ox + U(0.1), oy + U(0.55), U(3.4), U(3.4)) xRadius:U(1.0) yRadius:U(1.0)];
    [shell appendBezierPath:[NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(ox + U(0.7), oy + U(3.3), U(1.9), U(1.95)) xRadius:U(0.4) yRadius:U(0.4)]];
    fillGradient(shell, COL_SHELL_TOP, COL_SHELL_BOT);
    [rgb(COL_SHELL_RIM) setStroke];
    shell.lineWidth = 1;
    [shell stroke];

    /* the gloss down the left of the head, and the parting line down the grip */
    NSBezierPath *gloss = [NSBezierPath bezierPath];
    [gloss moveToPoint:NSMakePoint(ox + U(0.26), oy + U(1.3))];
    [gloss curveToPoint:NSMakePoint(ox + U(0.26), oy + U(2.9))
          controlPoint1:NSMakePoint(ox + U(0.2), oy + U(1.8))
          controlPoint2:NSMakePoint(ox + U(0.2), oy + U(2.4))];
    gloss.lineWidth = 2;
    gloss.lineCapStyle = NSLineCapStyleRound;
    [[rgb(COL_SHELL_RIM) colorWithAlphaComponent:0.55] setStroke];
    [gloss stroke];
    NSBezierPath *parting = [NSBezierPath bezierPath];
    [parting moveToPoint:NSMakePoint(ox + U(1.65), oy + U(3.95))];
    [parting lineToPoint:NSMakePoint(ox + U(1.65), oy + U(5.2))];
    parting.lineWidth = 1;
    [[rgb(COL_SHELL_RIM) colorWithAlphaComponent:0.35] setStroke];
    [parting stroke];

    /* the recessed knurled panel down the front of the grip */
    const NSRect knurl = NSMakeRect(ox + U(1.75), oy + U(3.95), U(0.7), U(1.15));
    NSBezierPath *panel = [NSBezierPath bezierPathWithRoundedRect:knurl xRadius:3 yRadius:3];
    [rgb(COL_KNURL) setFill];
    [panel fill];
    [rgb(COL_KNURL_DOT) setFill];
    for (CGFloat py = knurl.origin.y + 3.5; py < NSMaxY(knurl) - 2; py += 4.5) {
        const int rowi = (int)((py - knurl.origin.y) / 4.5);
        for (CGFloat px = knurl.origin.x + 3 + (rowi % 2 ? 2.2 : 0); px < NSMaxX(knurl) - 2; px += 4.4)
            NSRectFill(NSMakeRect(px, py, 1.6, 1.6));
    }

    /* the gold Bally badge on the shoulder of the grip */
    const NSRect badge = NSMakeRect(ox + U(0.98) - U(0.24), oy + U(4.35) - U(0.24), U(0.48), U(0.48));
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:badge];
    [rgb(COL_BADGE) setFill];
    [ring fill];
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(badge, 1.8, 1.8)];
    [rgb(0x161515) setFill];
    [disc fill];
    NSDictionary *script = @{ NSFontAttributeName: [NSFont fontWithName:@"Zapfino" size:6] ?: [NSFont boldSystemFontOfSize:6],
                              NSForegroundColorAttributeName: rgb(COL_BADGE) };
    NSString *name = @BADGE_NAME;
    const NSSize ext = [name sizeWithAttributes:script];
    [name drawAtPoint:NSMakePoint(NSMidX(badge) - ext.width / 2, NSMidY(badge) - ext.height / 2)
       withAttributes:script];
#undef U

    drawFitted(nsstr(ctl->label), cap, 12, YES, rgb(COL_CAPTION));
}

/* The recessed disc the wedges ring. */
- (void)drawStick:(const astro_control *)ctl
{
    const NSRect rect = NSInsetRect([self rectOf:ctl], -1, -1);
    NSBezierPath *rim = [NSBezierPath bezierPathWithOvalInRect:rect];
    [rgb(COL_DISC_RIM) setFill];
    [rim fill];
    NSBezierPath *dish = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 1.5, 1.5)];
    [rgb(COL_DISC) setFill];
    [dish fill];
}

/* One of the eight wedges round the knob. */
- (void)drawWedge:(const astro_control *)ctl idx:(int)idx centre:(NSPoint)ctr radius:(CGFloat)rad lit:(BOOL)lit
{
    const BOOL held = lit || [self selected:ctl];
    const double ang = wedgeAngle(ctl->act_mask);
    /* between the dial's collar (0.95 of 1.6 layout units) and the disc's edge */
    NSBezierPath *wedge = ringSector(ctr, rad * 0.6 + 1.5, rad - 2, ang, 21.0);
    NSColor *fill = held ? accent() : rgb(idx == _hover && !self.mapping ? COL_WEDGE_HOT : COL_WEDGE);
    [fill setFill];
    [wedge fill];
    if (held) [self strokeHeld:wedge];
    NSColor *ink = held ? [NSColor blackColor] : rgb(COL_WEDGE_INK);
    const double rads = ang * M_PI / 180.0;
    const CGFloat mid = rad * 0.8;
    const NSPoint spot = NSMakePoint(ctr.x + mid * cos(rads), ctr.y - mid * sin(rads));
    NSString *key = [self legendFor:ctl];
    if (key.length) {
        drawFitted(key, NSMakeRect(spot.x - 14, spot.y - 6, 28, 12), 8, NO, ink);
        return;
    }
    /* the arrow: a small triangle pointing out */
    const NSPoint out = NSMakePoint(cos(rads), -sin(rads));
    const NSPoint side = NSMakePoint(-out.y, out.x);
    NSBezierPath *arrow = [NSBezierPath bezierPath];
    [arrow moveToPoint:NSMakePoint(spot.x + out.x * 4.5, spot.y + out.y * 4.5)];
    [arrow lineToPoint:NSMakePoint(spot.x - out.x * 2.5 + side.x * 4.5, spot.y - out.y * 2.5 + side.y * 4.5)];
    [arrow lineToPoint:NSMakePoint(spot.x - out.x * 2.5 - side.x * 4.5, spot.y - out.y * 2.5 - side.y * 4.5)];
    [arrow closePath];
    [ink setFill];
    [arrow fill];
}

/* The knob: a ribbed orange-brown dial that turns with the pot (a pointer shows
 * where) and rides a little toward the held direction. Its player's number is
 * on it; in Map and Show bindings the gamepad axis it follows is. */
- (void)drawDial:(const astro_control *)ctl idx:(int)idx dim:(BOOL)dim
{
    const NSRect rect = [self rectOf:ctl];
    const int pl = ctl->player >= 0 && ctl->player < ASTROSESSION_PLAYERS ? ctl->player : 0;
    const AstroTilt tilt = _tilt[pl];
    const CGFloat rad = rect.size.width / 2;
    const NSPoint cen = NSMakePoint(NSMidX(rect) + (CGFloat)tilt.tiltx * rad * 0.1,
                                    NSMidY(rect) + (CGFloat)tilt.tilty * rad * 0.1);
    const int value = _knob[pl] >= 0 ? _knob[pl] : ASTROSESSION_KNOB_CENTRE;
    const double deg = astrosession_knob_angle(value);   /* clockwise from straight up */
    [self beginDim:dim];

    /* the dark collar the knob sits in */
    NSBezierPath *collar = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cen.x - rad - 1.5, cen.y - rad - 1.5, 2 * rad + 3, 2 * rad + 3)];
    [rgb(COL_KNOB_DARK) setFill];
    [collar fill];
    NSBezierPath *body = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cen.x - rad, cen.y - rad, 2 * rad, 2 * rad)];
    fillRadial(body, NSMakePoint(cen.x - rad * 0.3, cen.y - rad * 0.35), cen, rad, COL_KNOB_LIGHT, COL_KNOB_MID);
    if (idx == _hover && !self.mapping) {
        [[[NSColor whiteColor] colorWithAlphaComponent:0.22] setFill];
        [body fill];
    }

    /* the flutes turn with the knob */
    NSBezierPath *ribs = [NSBezierPath bezierPath];
    for (int rib = 0; rib < 28; rib++) {
        const double ang = (deg + rib * 360.0 / 28.0) * M_PI / 180.0;
        [ribs moveToPoint:NSMakePoint(cen.x + rad * 0.62 * sin(ang), cen.y - rad * 0.62 * cos(ang))];
        [ribs lineToPoint:NSMakePoint(cen.x + rad * 0.97 * sin(ang), cen.y - rad * 0.97 * cos(ang))];
    }
    ribs.lineWidth = 1.4;
    [[rgb(COL_KNOB_DARK) colorWithAlphaComponent:0.8] setStroke];
    [ribs stroke];

    /* the pointer, and the cap the number is on */
    const double pang = deg * M_PI / 180.0;
    NSBezierPath *pointer = [NSBezierPath bezierPath];
    [pointer moveToPoint:NSMakePoint(cen.x + rad * 0.4 * sin(pang), cen.y - rad * 0.4 * cos(pang))];
    [pointer lineToPoint:NSMakePoint(cen.x + rad * 0.93 * sin(pang), cen.y - rad * 0.93 * cos(pang))];
    pointer.lineWidth = 2.5;
    pointer.lineCapStyle = NSLineCapStyleRound;
    [rgb(COL_KNOB_INK) setStroke];
    [pointer stroke];
    const CGFloat capR = rad * 0.42;
    NSBezierPath *cap = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cen.x - capR, cen.y - capR, 2 * capR, 2 * capR)];
    fillRadial(cap, NSMakePoint(cen.x - capR * 0.3, cen.y - capR * 0.35), cen, capR, COL_KNOB_MID, COL_KNOB_CAP);
    [rgb(COL_KNOB_DARK) setStroke];
    cap.lineWidth = 1;
    [cap stroke];
    NSString *legend = [self legendFor:ctl];
    if (legend.length)
        drawFitted(legend, NSInsetRect(NSMakeRect(cen.x - capR, cen.y - capR, 2 * capR, 2 * capR), 1, 4),
                   8, YES, rgb(COL_KNOB_INK));
    else
        drawFitted([NSString stringWithFormat:@"%d", pl + 1],
                   NSMakeRect(cen.x - capR, cen.y - capR, 2 * capR, 2 * capR), 14, YES, rgb(COL_KNOB_INK));
    [self endDim:dim];
}

/* The trigger: a curved pale-grey lever at the front of the grip. */
- (void)drawTrigger:(const astro_control *)ctl idx:(int)idx lit:(BOOL)lit
{
    const BOOL held = lit || [self selected:ctl];
    const NSRect rect = NSInsetRect([self rectOf:ctl], 0.5, 0.5);
    NSBezierPath *edge = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:9 yRadius:9];
    [rgb(COL_TRIG_EDGE) setFill];
    [edge fill];
    const NSRect face = NSMakeRect(rect.origin.x + 1.5, rect.origin.y + 1.5, rect.size.width - 3,
                                   rect.size.height - 4.5);
    NSBezierPath *lever = [NSBezierPath bezierPathWithRoundedRect:face xRadius:8 yRadius:8];
    if (held) {
        [accent() setFill];
        [lever fill];
        [self strokeHeld:lever];
    } else {
        fillGradient(lever, idx == _hover && !self.mapping ? 0xCACAD0 : COL_TRIG_TOP, COL_TRIG_BOT);
    }
    NSString *key = [self legendFor:ctl];
    if (key.length)
        drawFitted(key, NSInsetRect(face, 1, 2), 8, YES, rgb(COL_TRIG_EDGE));
}

/* A console button: black with gold lettering, acting on release. */
- (void)drawSys:(const astro_control *)ctl idx:(int)idx
{
    const BOOL picked = [self selected:ctl];
    const BOOL pressed = _sysDown && _downIdx == idx;
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *shape = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
    if (picked || pressed) {
        [accent() setFill];
        [shape fill];
        if (picked) [self strokeHeld:shape];
    } else {
        fillGradient(shape, idx == _hover && !self.mapping ? COL_SYS_HOT : COL_SYS_TOP, COL_SYS_BOT);
    }
    [rgb(COL_RECESS_RIM) setStroke];
    shape.lineWidth = 1;
    [shape stroke];
    NSString *name = nsstr(ctl->label);
    NSString *key = [self legendFor:ctl];
    if (key.length) name = [NSString stringWithFormat:@"%@  (%@)", name, key];
    drawFitted(name, NSInsetRect(rect, 4, 0), 12, YES, picked || pressed ? [NSColor blackColor] : rgb(COL_SYS_INK));
}

/* The brand strip along the top of the case. */
- (void)drawBrand:(NSRect)bnd
{
    NSBezierPath *strip = [NSBezierPath bezierPathWithRect:NSMakeRect(0, 0, bnd.size.width, BRAND_H)];
    fillGradient(strip, 0x6A4220, 0x4F2F14);
    [rgb(COL_PINSTRIPE) setFill];
    NSRectFill(NSMakeRect(0, BRAND_H - 2, bnd.size.width, 1));
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:10],
                             NSForegroundColorAttributeName: rgb(COL_BRAND_TEXT),
                             NSParagraphStyleAttributeName: para,
                             NSKernAttributeName: @2.0 };
    [@"BALLY ASTROCADE" drawInRect:NSMakeRect(0, 4, bnd.size.width, BRAND_H - 6) withAttributes:attrs];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bnd = [self bounds];

    /* the walnut case */
    NSBezierPath *frame = [NSBezierPath bezierPathWithRect:bnd];
    fillGradient(frame, COL_CASE_TOP, COL_CASE_BOT);
    [self drawBrand:bnd];
    NSBezierPath *edge = [NSBezierPath bezierPathWithRect:NSInsetRect(bnd, 0.5, 0.5)];
    [rgb(COL_CASE_EDGE) setStroke];
    edge.lineWidth = 1;
    [edge stroke];

    /* the parts, in the layout's paint order: a card's shell, then its disc and
     * wedges, then the knob and trigger over them */
    for (int idx = 0; idx < _num; idx++) {
        const astro_control *ctl = &_ctl[idx];
        const BOOL dim = [self dimmed:idx];
        [self beginDim:dim && ctl->kind != ASTRO_CTL_KNOB];
        switch (ctl->kind) {
        case ASTRO_CTL_OVERLAY:
            [self drawPlate:ctl];
            break;
        case ASTRO_CTL_CAPTION:
            [self drawShell:ctl];
            break;
        case ASTRO_CTL_STICK:
            [self drawStick:ctl];
            break;
        case ASTRO_CTL_DIRECTION: {
            NSPoint ctr;
            CGFloat rad;
            if ([self stickOfPlayer:ctl->player centre:&ctr radius:&rad])
                [self drawWedge:ctl idx:idx centre:ctr radius:rad lit:_lit[idx]];
            break;
        }
        case ASTRO_CTL_KNOB:
            break;   /* drawn below, after its dim is settled */
        case ASTRO_CTL_TRIGGER:
            [self drawTrigger:ctl idx:idx lit:_lit[idx]];
            break;
        case ASTRO_CTL_KEY:
            [self drawKey:ctl idx:idx lit:_lit[idx]];
            break;
        case ASTRO_CTL_SYSBUTTON:
            [self drawSys:ctl idx:idx];
            break;
        default:
            break;
        }
        [self endDim:dim && ctl->kind != ASTRO_CTL_KNOB];
        if (ctl->kind == ASTRO_CTL_KNOB)
            [self drawDial:ctl idx:idx dim:dim];
    }

    /* Map mode: the picked control is called out */
    if (self.mapping && self.mapTarget >= 0) {
        for (int idx = 0; idx < _num; idx++) {
            const astro_control *ctl = &_ctl[idx];
            if ([self mapTargetOf:ctl] != self.mapTarget || ctl->kind == ASTRO_CTL_DIRECTION) continue;
            const NSRect rect = NSInsetRect([self rectOf:ctl], -2, -2);
            NSBezierPath *ring = ctl->kind == ASTRO_CTL_KNOB
                ? [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, -2, -2)]
                : [NSBezierPath bezierPathWithRect:rect];
            ring.lineWidth = 2;
            [accent() setStroke];
            [ring stroke];
        }
    }
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)setPointerMask:(unsigned)mask
{
    if (_downPlayer < 0) return;
    _downMask = mask;
    astrosession_pointer_set_player(_session, _downPlayer, mask);
}

- (void)letGo
{
    astrosession_pointer_release(_session);
    _downIdx = -1;
    _downPlayer = -1;
    _downMask = 0;
    _stickDrag = NO;
    _dialDrag = NO;
    _sysDown = NO;
    [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent *)evt
{
    const int hover = [self hitAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (hover != _hover) {
        _hover = hover;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}

/* The dial: the knob goes where the pointer's angle puts it. */
- (void)dragDialAt:(NSPoint)pnt
{
    if (_downIdx < 0) return;
    const astro_control *ctl = &_ctl[_downIdx];
    const NSRect rect = [self rectOf:ctl];
    const int value = astrosession_knob_from_point(pnt.x - NSMidX(rect), pnt.y - NSMidY(rect),
                                                   rect.size.width / 2);
    if (value >= 0) astrosession_knob_set(_session, ctl->player, (uint8_t)value);
    [self poll];
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    const int idx = [self hitAt:pnt];
    if (idx < 0) return;
    const astro_control *ctl = &_ctl[idx];

    if (self.mapping) {
        /* Map: a part with a target of its own is picked (the dial's is its
         * knob axis); a diagonal drives two contacts and the disc none */
        const int target = [self mapTargetOf:ctl];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    switch (ctl->kind) {
    case ASTRO_CTL_SYSBUTTON:
        _downIdx = idx;
        _sysDown = YES;      /* fires on release over the button */
        break;
    case ASTRO_CTL_KEY:
    case ASTRO_CTL_TRIGGER:
        _downIdx = idx;
        astrosession_press(_session, ctl->target, 1);
        break;
    case ASTRO_CTL_DIRECTION:
        _downIdx = idx;
        _downPlayer = ctl->player;
        _stickDrag = YES;
        [self setPointerMask:ctl->act_mask];
        break;
    case ASTRO_CTL_STICK:
        /* the disc: nothing in the middle, eight ways out of it */
        _downIdx = idx;
        _downPlayer = ctl->player;
        _stickDrag = YES;
        [self dragTo:pnt];
        break;
    case ASTRO_CTL_KNOB:
        _downIdx = idx;
        _dialDrag = YES;
        [self dragDialAt:pnt];
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)dragTo:(NSPoint)pnt
{
    if (_downIdx < 0 || _sysDown) return;
    if (_dialDrag) {
        [self dragDialAt:pnt];
        return;
    }
    if (_stickDrag) {
        NSPoint ctr;
        CGFloat rad;
        if (![self stickOfPlayer:_downPlayer centre:&ctr radius:&rad]) return;
        const unsigned mask = astrosession_stick_from_vector((float)(pnt.x - ctr.x), (float)(pnt.y - ctr.y),
                                                              (float)rad);
        if (mask != _downMask) {
            [self setPointerMask:mask];
            [self poll];
        }
        return;
    }
    /* a key or the trigger: dragging off lets go */
    if (!NSPointInRect(pnt, [self rectOf:&_ctl[_downIdx]])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseDragged:(NSEvent *)evt
{
    [self dragTo:[self convertPoint:[evt locationInWindow] fromView:nil]];
}

- (void)mouseUp:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    if (_sysDown) {
        /* a console button acts on release over it, like a real one */
        const astro_control *ctl = &_ctl[_downIdx];
        const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
        const BOOL inside = NSPointInRect(pnt, [self rectOf:ctl]);
        const int target = ctl->target;
        [self letGo];
        if (inside && target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS)
            [self.owner runSysaction:target - ASTRO_TARGET_SYSACT];
        return;
    }
    [self letGo];
    [self poll];
}

/* The wheel over a dial nudges the knob (up turns it right). */
- (void)scrollWheel:(NSEvent *)evt
{
    if (self.mapping) return;
    const int idx = [self hitAt:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx < 0 || _ctl[idx].kind != ASTRO_CTL_KNOB) return;
    const CGFloat step = [evt hasPreciseScrollingDeltas] ? [evt scrollingDeltaY] * 0.6
                                                          : [evt scrollingDeltaY] * 6.0;
    int value = astrosession_knob_get(_session, _ctl[idx].player) + (int)lround(step);
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    astrosession_knob_set(_session, _ctl[idx].player, (uint8_t)value);
    [self poll];
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const int idx = [self hitAt:pnt];
    if (idx < 0) return nil;
    const int target = [self mapTargetOf:&_ctl[idx]];
    if (target < 0) return nil;
    NSString *key = bindings_target_keysym(target) ? keyName(bindings_target_keysym(target)) : @"none";
    const int pad = bindings_target_pad(target);
    NSString *padText = pad >= 0 ? padInputName(pad) : @"none";
    if (isKnobTarget(target))
        return [NSString stringWithFormat:@"%@\nGamepad axis: %@", nsstr(bindings_target_label(target)), padText];
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(bindings_target_label(target)),
            key, padText];
}

/* One tooltip rect over the whole console; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation AstroControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape cancels a Map pick; it is not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static __weak AstroControllersWindow *g_window;

@implementation AstroControllersWindow {
    astrosession *_session;
    AstroAppKeyHandler _appKeys;
    AstroControllersPanel *_panel;
    AstroFlippedView *_content;
    AstroConsoleView *_console;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "plays player N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSButton *_overlayBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button (an axis, for a knob) to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(astrosession *)session appKeys:(AstroAppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;
    g_window = self;

    _panel = [[AstroControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 500, 700)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    AstroSetHelpContext(_panel, @"controllers");

    _content = [[AstroFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 500, 700)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[AstroConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.session = _session;
    _console.mapTarget = -1;
    {
        /* the older spellings are read when the key is absent */
        int saved = astrosession_get_int(_session, "controllers_show_bindings", -1);
        if (saved < 0) saved = astrosession_get_int(_session, "controllers_bindings", -1);
        if (saved < 0) saved = astrosession_get_int(_session, "controllers_key_names", -1);
        if (saved < 0) saved = astrosession_get_int(_session, "show_bindings", -1);
        _console.showKeys = saved > 0;
    }
    _console.overlay = astrosession_overlay_get(_session) != 0;
    [_content addSubview:_console];

    /* Gamepads: one row per pad, its name, the player it plays and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* -1 = automatic (connection order), 0 to 3 = that player */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2", @"Player 3", @"Player 4"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _overlayBox = [NSButton checkboxWithTitle:@"Overlay: Bally BASIC" target:self action:@selector(toggleOverlay:)];
    [_overlayBox setState:_console.overlay ? NSControlStateValueOn : NSControlStateValueOff];
    [_overlayBox setRefusesFirstResponder:YES];
    [_content addSubview:_overlayBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int target = 0; target < ASTRO_TARGET_COUNT; target++)
        [[_pickPopup menu] addItemWithTitle:nsstr(bindings_target_label(target)) action:nil keyEquivalent:@""];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = AstroHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_console configure];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the console ------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize csize = _console.contentSize;
    const CGFloat wide = csize.width;
    const CGFloat inner = wide - 2 * MARGIN;
    const CGFloat padsY = csize.height + 10;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat pickY = mapY + BTN_H + 6;
    const CGFloat boxY = pickY + 26 + 4;
    const CGFloat hintY = boxY + 22 + 2;
    const CGFloat total = hintY + 30 + MARGIN;

    [self fitPanel:NSMakeSize(wide, total)];
    [_console setFrame:NSMakeRect(0, 0, csize.width, csize.height)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat ypos = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, ypos + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, ypos + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, ypos - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(wide - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 54, pickY, inner - 54, 25)];
    [_overlayBox setFrame:NSMakeRect(MARGIN, boxY, 180, 22)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 30)];
    [_console setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console letGo];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    /* a knob follows a gamepad axis; every other target has a key */
    if (isKnobTarget(target)) return padInputName(bindings_target_pad(target));
    return keyName(bindings_target_keysym(target));
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    if (isKnobTarget(target)) {
        const int pad = bindings_target_pad(target);
        return pad >= 0 ? [NSString stringWithFormat:@"axis %@", padInputName(pad)] : @"no axis";
    }
    NSString *key = keyName(bindings_target_keysym(target));
    NSString *text = key.length ? key : @"no key";
    const int pad = bindings_target_pad(target);
    if (pad >= 0) text = [NSString stringWithFormat:@"%@, pad %@", text, padInputName(pad)];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _console.mapping = _mapState != -2;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_console setNeedsDisplay:YES];
    [_console poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        if (isKnobTarget(state)) {
            astrosession_gamepad_capture_begin_axis(_session);
            _hint.stringValue = [NSString stringWithFormat:
                @"Move a gamepad stick or trigger for %@ (now: %@). The knob takes an axis, not a key. "
                @"Esc cancels.", nsstr(bindings_target_label(state)), [self bindingText:state]];
        } else {
            astrosession_gamepad_capture_begin(_session);
            _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                                 nsstr(bindings_target_label(state)), [self bindingText:state]];
        }
    } else {
        astrosession_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control, or choose one from the list, to remap it."
            : @"Click and hold the keys and the trigger, drag the disc (eight ways) or turn the knob. "
              @"Keys and gamepads work here too.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _pickPopup.enabled = state != -2;
    if (state >= 0 && state < [_pickPopup numberOfItems]) [_pickPopup selectItemAtIndex:state];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < ASTRO_TARGET_COUNT) [self setMapState:(int)sel];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(int)stolen
{
    NSString *what = nsstr(bindings_target_label(target));
    _hint.stringValue = stolen >= 0 && stolen < ASTRO_TARGET_COUNT
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(bindings_target_label(stolen))]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym
{
    const int target = _mapState;
    if (isKnobTarget(target)) {
        _hint.stringValue = @"The knob follows a gamepad axis: move a stick or trigger. Esc cancels.";
        return;
    }
    int stolen = -1;
    bindings_set_keysym(_session, target, keysym, &stolen);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int ident = ASTRO_PAD_BTN_NONE;
    if (_mapState < 0 || !astrosession_gamepad_capture_poll(_session, &ident)) return;
    int stolen = -1;
    const int target = _mapState;
    bindings_set_pad(_session, target, ident, &stolen);
    [self setMapState:-1];
    [self showBound:padInputName(ident) target:target stolen:stolen];
}

/* A key typed while Map waits for one, from any window (AstroKeyForward's
 * hook): Esc backs out of the pick, F9, F11 and F12 are the app's. */
- (BOOL)mapKeyFromApp:(uint32_t)keysym
{
    if (_mapState < 0) return NO;
    if (keysym == ASTROSESSION_KEYSYM_F9 || keysym == ASTROSESSION_KEYSYM_F11 ||
        keysym == ASTROSESSION_KEYSYM_F12)
        return NO;
    if (keysym == ASTROSESSION_KEYSYM_ESCAPE) [self setMapState:-1];
    else [self bindKey:keysym];
    return YES;
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    bindings_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(bindings_target_label(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    bindings_reset_defaults(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    astrosession_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _console.showKeys = show;
    [_console setNeedsDisplay:YES];
}

/* The Bally BASIC overlay: the legends on the keys, and the host keyboard types
 * BASIC (the core remembers the choice). */
- (void)toggleOverlay:(id)sender
{
    (void)sender;
    astrosession_overlay_set(_session, [_overlayBox state] == NSControlStateValueOn ? 1 : 0);
    _console.overlay = astrosession_overlay_get(_session) != 0;
}

/* A console button on the panel was released over itself. */
- (void)runSysaction:(int)sysact
{
    if (self.sysactionHandler) self.sysactionHandler(sysact);
    else astrosession_sysaction_fire(_session, (astrosession_sysaction)sysact);
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPlayers
{
    for (int row = 0; row < _padRows; row++) {
        const int player = astrosession_gamepad_effective_port(_session, row);
        NSString *text = player >= 0 ? [NSString stringWithFormat:@"plays player %d", player + 1]
                                     : @"plays nobody";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = astrosession_gamepad_count(_session);
    _padGeneration = astrosession_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more
     * pads are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        astrosession_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        const int assign = astrosession_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= ASTROSESSION_PLAYERS ? 0 : assign + 1)];
    }
    [self refreshPadPlayers];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    astrosession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPlayers];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, the auto-assignment and the overlay. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (astrosession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPlayers];
        /* a Bally BASIC cartridge turns the overlay on by itself */
        const BOOL overlay = astrosession_overlay_get(_session) != 0;
        if (overlay != _console.overlay) _console.overlay = overlay;
        const NSControlStateValue want = overlay ? NSControlStateValueOn : NSControlStateValueOff;
        if ([_overlayBox state] != want) [_overlayBox setState:want];
    }
    [_console poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak AstroControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command
     * is down, so forwarding one would leave the machine holding the key. */
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = AstroKeysymFromEvent(evt);
    if (!keysym) return;
    if (keysym == ASTROSESSION_KEYSYM_F9 || keysym == ASTROSESSION_KEYSYM_F12) {
        if (![evt isARepeat] && _appKeys) _appKeys(keysym);
        return;
    }
    if (keysym == ASTROSESSION_KEYSYM_F11) return;
    if (_mapState >= 0) {
        if ([evt isARepeat]) return;
        /* the next key is the binding; Esc backs out of the pick */
        [self mapKeyFromApp:keysym];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* the main window's path: a bound key drives its controller (or runs its
     * console action), anything else is typed (BASIC, with the overlay on);
     * the main window does not repeat a key either */
    if ([evt isARepeat]) return;
    AstroForwardKeyEvent(_session, evt, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    if (!AstroKeysymFromEvent(evt)) return;
    AstroForwardKeyEvent(_session, evt, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise they
 * are the machine's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    if (_mapState == -1) return;
    AstroForwardModifierEvent(_session, evt);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    astrosession_release_all(_session);
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self refreshPads];
    [self setMapState:-2];
    _console.overlay = astrosession_overlay_get(_session) != 0;
    [_overlayBox setState:_console.overlay ? NSControlStateValueOn : NSControlStateValueOff];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    astrosession_release_all(_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end

BOOL AstroControllersMapKey(uint32_t keysym)
{
    AstroControllersWindow *window = g_window;
    return window ? [window mapKeyFromApp:keysym] : NO;
}
