/*
 * The debugger's Video chip tab (AppKit).
 *
 * Everything that decides what to show lives in the core (astrovidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) over a copy of
 * that buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#include <stdio.h>
#include <string.h>

#include "astrovidtab.h"

#define MAX_OV 8
#define NSWATCH 8
#define PAL_CELL 14
#define PAL_COLS 16

/* The Astrocade dark red (the rest of the app's accent). */
static NSColor *accentColor(void)
{
    return [NSColor colorWithSRGBRed:0.71 green:0.20 blue:0.17 alpha:1.0];
}

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *c)
{
    NSString *s = c ? [NSString stringWithUTF8String:c] : nil;
    return s ? s : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *l = [NSTextField wrappingLabelWithString:@""];
    l.font = monoFont11();
    l.selectable = YES;
    if (width > 0) l.preferredMaxLayoutWidth = width;
    return l;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface AstroVideoPicture : NSView
@property (nonatomic, assign) astrovid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* a double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation AstroVideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int w = 0, h = 0;
    const uint32_t *px = astrovid_tab_picture(_tab, &w, &h);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (px && w > 0 && h > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)px, (CFIndex)w * h * 4);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, prov, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(prov);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(w, h);
    } else {
        const char *why = astrovid_tab_why(_tab);
        _message = str(why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *a = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                             NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *msg = _message ? _message : @"";
        const NSSize sz = [msg sizeWithAttributes:a];
        [msg drawAtPoint:NSMakePoint((self.bounds.size.width - sz.width) / 2,
                                     (self.bounds.size.height - sz.height) / 2) withAttributes:a];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(ctx);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextTranslateCTM(ctx, 0, _size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(ctx);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = astrovid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5, hover[2] - 1, hover[3] - 1)];
        p.lineWidth = 1;
        [p stroke];
    }
    if (marks & 2) {
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.1 alpha:1] setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1, pin[2] - 2, pin[3] - 2)];
        p.lineWidth = 2;
        [p stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)e { return [self convertPoint:e.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint p = [self local:event];
    if (astrovid_tab_hover(_tab, (int)p.x, (int)p.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (astrovid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint p = [self local:event];
    if (event.clickCount >= 2) {
        astrovid_tab_hover(_tab, (int)p.x, (int)p.y);
        if (_onBreak) _onBreak();
    } else {
        astrovid_tab_pin(_tab, (int)p.x, (int)p.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface AstroSwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation AstroSwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 16); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    p.lineWidth = 1;
    [p stroke];
}
@end

/* ---- the palette --------------------------------------------------------------------- */

@interface AstroVideoPalette : NSView
@property (nonatomic, assign) astrovid_tab *tab;
@property (nonatomic, copy) void (^onDetail)(NSString *text);
@end

@implementation AstroVideoPalette {
    int _hover;
    NSTrackingArea *_area;
}

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithFrame:(NSRect)f
{
    self = [super initWithFrame:f];
    _hover = -1;
    return self;
}

- (NSSize)intrinsicContentSize
{
    return NSMakeSize(PAL_COLS * (PAL_CELL + 2), PAL_COLS * (PAL_CELL + 2));
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (!_tab) return;
    const astrovid_pal_entry *e = NULL;
    const int n = astrovid_tab_palette(_tab, &e);
    if (!e) return;
    for (int i = 0; i < n; i++) {
        const NSRect r = NSMakeRect((i % PAL_COLS) * (PAL_CELL + 2), (i / PAL_COLS) * (PAL_CELL + 2), PAL_CELL, PAL_CELL);
        [colorFromRGB(e[i].rgb) setFill];
        NSRectFill(r);
        if (e[i].inspected || i == _hover) {
            [(e[i].inspected ? accentColor() : NSColor.labelColor) setStroke];
            NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -1, -1)];
            p.lineWidth = 2;
            [p stroke];
        }
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (void)mouseMoved:(NSEvent *)event
{
    const astrovid_pal_entry *e = NULL;
    const int n = _tab ? astrovid_tab_palette(_tab, &e) : 0;
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int i = ((int)p.y / (PAL_CELL + 2)) * PAL_COLS + (int)p.x / (PAL_CELL + 2);
    if (p.x < 0 || p.y < 0 || i < 0 || i >= n || !e) i = -1;
    if (i != _hover) {
        _hover = i;
        [self setNeedsDisplay:YES];
    }
    if (i < 0) {
        if (_onDetail) _onDetail(@"");
        return;
    }
    /* "reg 5A, palette $0B4, #RRGGBB, N pixels, used by COL2L" */
    NSMutableString *s = [NSMutableString stringWithFormat:@"reg %02X, palette $%03X, #%06X, %ld pixels",
                                                           e[i].reg & 255, e[i].pal & 0xFFF, e[i].rgb & 0xFFFFFF, e[i].uses];
    const astrovid_swatch *sw = NULL;
    const int ns = astrovid_tab_swatches(_tab, &sw);
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (int b = 0; b < NSWATCH && b < ns; b++)
        if ((e[i].regs_mask & (1 << b)) && sw) [names addObject:str(sw[b].name)];
    if (names.count) [s appendFormat:@", used by %@", [names componentsJoinedByString:@" "]];
    if (_onDetail) _onDetail(s);
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    _hover = -1;
    [self setNeedsDisplay:YES];
    if (_onDetail) _onDetail(@"");
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface AstroVideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation AstroVideoTab {
    astrovid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPop, *_zoomPop;
    NSButton *_ov[MAX_OV];
    NSTextField *_status, *_note, *_pixelText, *_timingText, *_palDetail;
    NSTextField *_swLabel[NSWATCH];
    AstroSwatchView *_swView[NSWATCH];
    AstroVideoPicture *_pic;
    NSScrollView *_scroll;
    NSTabView *_sections;
    NSTableView *_regs, *_lines;
    AstroVideoPalette *_pal;
    NSProgressIndicator *_beam;
    NSString *_regsSig, *_linesSig;
    BOOL _updating;
    int _nregs, _nlines;
    int _vpW, _vpH;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    _tab = astrovid_tab_new();
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) astrovid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int n = 0;
    const astrovid_overlay_info *od = astrovid_overlays(&n);
    if (n > MAX_OV) n = MAX_OV;
    int nz = 0;
    const int *zooms = astrovid_zooms(&nz);

    _viewPop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < ASTROVID_V_COUNT; i++) [_viewPop addItemWithTitle:str(astrovid_view_name(i))];
    _viewPop.target = self;
    _viewPop.action = @selector(onView:);
    _viewPop.toolTip = @"Display: the last finished frame, as the TV showed it.\n"
                        "Decoded: screen RAM redrawn through each line's own registers.\n"
                        "Beam: this frame above the beam, the last one dimmed below.\n"
                        "Values: the raw pens 0-3 (warm = right set, cool = left set).";
    _zoomPop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int i = 0; i < nz; i++)
        [_zoomPop addItemWithTitle:zooms[i] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[i]]];
    _zoomPop.target = self;
    _zoomPop.action = @selector(onZoom:);
    NSButton *brk = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
    brk.toolTip = @"Add write breakpoints on the inspected pixel's screen RAM byte and its mirror";
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPop,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPop, brk]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        _ov[i] = [NSButton checkboxWithTitle:str(od[i].label) target:self action:@selector(onOverlay:)];
        [(i < 4 ? row1 : row2) addObject:_ov[i]];
    }
    NSStackView *ovRow1 = [NSStackView stackViewWithViews:row1], *ovRow2 = [NSStackView stackViewWithViews:row2];
    ovRow1.orientation = ovRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    ovRow1.spacing = ovRow2.spacing = 10;

    _status = monoLabel(0);
    _pic = [[AstroVideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _pic.tab = _tab;
    __weak AstroVideoTab *weak = self;
    _pic.onChanged = ^{ [weak updatePixel]; };
    _pic.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _pic;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, ovRow1, ovRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in astrovid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(370);
        NSButton *pbrk = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *btns = [NSStackView stackViewWithViews:@[pbrk, unpin]];
        btns.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        btns.spacing = 6;
        NSStackView *v = [NSStackView stackViewWithViews:@[_pixelText, btns]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [self addSection:@"Pixel" content:v];
    }
    {   /* Registers */
        _regs = [[NSTableView alloc] init];
        [self addSection:@"Registers" content:[self scrollForTable:_regs
                                                           columns:@[@"name", @"val", @"meaning"]
                                                            titles:@[@"Register", @"Value", @"Meaning"]
                                                            widths:@[@80, @50, @300]]];
    }
    {   /* Colours */
        NSMutableArray<NSView *> *rows = [NSMutableArray array];
        for (int i = 0; i < NSWATCH; i++) {
            _swView[i] = [[AstroSwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 16)];
            _swLabel[i] = monoLabel(0);
            _swLabel[i].selectable = NO;
            NSButton *b = [NSButton buttonWithTitle:@"Break on OUT" target:self action:@selector(onSwatchBreak:)];
            b.tag = i;
            b.controlSize = NSControlSizeSmall;
            b.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
            NSStackView *r = [NSStackView stackViewWithViews:@[_swView[i], _swLabel[i], b]];
            r.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            r.spacing = 8;
            [rows addObject:r];
        }
        _pal = [[AstroVideoPalette alloc] initWithFrame:NSMakeRect(0, 0, PAL_COLS * (PAL_CELL + 2), PAL_COLS * (PAL_CELL + 2))];
        _pal.tab = _tab;
        _palDetail = monoLabel(370);
        _pal.onDetail = ^(NSString *t) { [weak setPaletteDetail:t]; };
        [rows addObject:[NSTextField labelWithString:@"Reachable palette (256 entries)"]];
        [rows addObject:_pal];
        [rows addObject:_palDetail];
        NSStackView *page = [NSStackView stackViewWithViews:rows];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 6;
        page.translatesAutoresizingMaskIntoConstraints = NO;
        NSScrollView *sc = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        sc.hasVerticalScroller = YES;
        sc.drawsBackground = NO;
        sc.documentView = page;
        NSClipView *clip = sc.contentView;
        [page.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
        [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
        [self addSection:@"Colours" content:sc];
    }
    {   /* Lines */
        _lines = [[NSTableView alloc] init];
        [self addSection:@"Lines" content:[self scrollForTable:_lines
                                                       columns:@[@"line", @"astro", @"r", @"l", @"bound", @"verbl", @"mode"]
                                                        titles:@[@"Line", @"Astro", @"R0-3", @"L0-3", @"Bound", @"VERBL", @"Mode"]
                                                        widths:@[@42, @46, @92, @92, @46, @50, @40]]];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(370);
        NSStackView *v = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        v.orientation = NSUserInterfaceLayoutOrientationVertical;
        v.alignment = NSLayoutAttributeLeading;
        v.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
        [self addSection:@"Timing" content:v];
    }
    [_sections.widthAnchor constraintEqualToConstant:400].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the ASTRO_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPop selectItemAtIndex:astrovid_tab_view(_tab)];
    for (int i = 0; i < nz; i++)
        if (zooms[i] == astrovid_tab_zoom(_tab)) [_zoomPop selectItemAtIndex:i];
    for (int i = 0; i < n; i++)
        _ov[i].state = (astrovid_tab_overlays(_tab) & od[i].bit) ? NSControlStateValueOn : NSControlStateValueOff;
    const int sec = astrovid_tab_env_section(_tab);
    if (sec > 0 && sec < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:sec];
    _updating = NO;
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)t
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger i = 0; i < ids.count; i++) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
        c.title = titles[i];
        c.width = widths[i].doubleValue;
        [t addTableColumn:c];
    }
    t.dataSource = self;
    t.delegate = self;
    t.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *sc = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    sc.hasVerticalScroller = YES;
    sc.documentView = t;
    return sc;
}

- (void)setPaletteDetail:(NSString *)text
{
    _palDetail.stringValue = text;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    astrovid_tab_set_view(_tab, (int)_viewPop.indexOfSelectedItem);
    [self refreshView:ASTROVID_TAB_PICTURE | ASTROVID_TAB_TABLES];
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int nz = 0;
    const int *zooms = astrovid_zooms(&nz);
    const NSInteger i = _zoomPop.indexOfSelectedItem;
    if (_updating || i < 0 || i >= nz) return;
    astrovid_tab_set_zoom(_tab, zooms[i]);
    [self refreshView:ASTROVID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int n = 0;
    const astrovid_overlay_info *od = astrovid_overlays(&n);
    if (n > MAX_OV) n = MAX_OV;
    unsigned m = 0;
    for (int i = 0; i < n; i++)
        if (_ov[i].state == NSControlStateValueOn) m |= od[i].bit;
    astrovid_tab_set_overlays(_tab, m);
    [self refreshView:ASTROVID_TAB_PICTURE];
}

/* The pixel's two write breakpoints ($4000+ and its mirror), with a note of what happened. */
- (void)breakOnPixel
{
    const astrovid_pixel *p = astrovid_tab_pixel(_tab);
    const unsigned addr = p ? p->addr : 0, mirror = p ? p->mirror : 0;
    const int ident = astrovid_tab_break_on_pixel(_tab);
    _note.stringValue = ident >= 0
        ? [NSString stringWithFormat:@"Break on writes to $%04X and $%04X (see Breakpoints)", addr, mirror]
        : @"Nothing to break on";
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

- (void)onSwatchBreak:(id)sender
{
    const int i = (int)((NSButton *)sender).tag;
    const astrovid_swatch *sw = NULL;
    const int n = astrovid_tab_swatches(_tab, &sw);
    const int ident = astrovid_tab_break_on_swatch(_tab, i);
    if (ident >= 0 && sw && i >= 0 && i < n)
        _note.stringValue = [NSString stringWithFormat:@"Break on OUT to port $%02X (%s) (see Breakpoints)",
                                                       sw[i].port & 255, sw[i].name];
    else
        _note.stringValue = @"Nothing to break on";
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    astrovid_tab_unpin(_tab);
    [_pic setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:ASTROVID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize vp = _scroll.contentSize;
    if ((int)vp.width == _vpW && (int)vp.height == _vpH) return;
    if (astrovid_tab_zoom(_tab) == 0) [self refreshView:ASTROVID_TAB_PICTURE];
    else {
        _vpW = (int)vp.width;
        _vpH = (int)vp.height;
        astrovid_tab_set_viewport(_tab, _vpW > 2 ? _vpW - 2 : 1, _vpH > 2 ? _vpH - 2 : 1);
    }
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const astrovid_pixel *p = astrovid_tab_pixel(_tab);
    _pixelText.stringValue = p ? str(p->text)
                               : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                  "Double-click to break on writes to it.";
    [_pal setNeedsDisplay:YES];
}

static NSString *hex(const uint8_t *v, int n)
{
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) [s appendFormat:@"%s%02X", i ? " " : "", v[i]];
    return s;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t
{
    return t == _lines ? _nlines : _nregs;
}

- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    NSString *text = @"";
    BOOL hot = NO;
    if (t == _regs) {
        const astrovid_reg *r = NULL;
        const int n = astrovid_tab_regs(_tab, &r);
        if (!r || row < 0 || row >= n) return nil;
        if ([col.identifier isEqualToString:@"name"]) text = str(r[row].name);
        else if ([col.identifier isEqualToString:@"val"]) text = str(r[row].value);
        else text = str(r[row].meaning);
        hot = r[row].age < ASTROVID_REG_FADE;
    } else {
        const astrovid_line_row *r = NULL;
        const int n = astrovid_tab_lines(_tab, &r);
        if (!r || row < 0 || row >= n) return nil;
        NSString *c = col.identifier;
        if ([c isEqualToString:@"line"]) text = [NSString stringWithFormat:@"%d", r[row].line];
        else if ([c isEqualToString:@"astro"]) text = [NSString stringWithFormat:@"%d", r[row].astro_line];
        else if ([c isEqualToString:@"r"]) text = hex(r[row].colors, 4);
        else if ([c isEqualToString:@"l"]) text = hex(r[row].colors + 4, 4);
        else if ([c isEqualToString:@"bound"]) text = [NSString stringWithFormat:@"%02X", r[row].colorsplit];
        else if ([c isEqualToString:@"verbl"]) text = [NSString stringWithFormat:@"%02X", r[row].vblank];
        else text = r[row].mode ? @"high" : @"low";
        hot = r[row].changed_mask != 0;
    }
    NSTextField *f = [t makeViewWithIdentifier:col.identifier owner:self];
    if (![f isKindOfClass:[NSTextField class]]) {
        f = [NSTextField labelWithString:@""];
        f.identifier = col.identifier;
        f.font = monoFont11();
    }
    f.stringValue = text;
    f.drawsBackground = hot;
    f.backgroundColor = hot ? accentColor() : NSColor.clearColor;
    f.textColor = hot ? NSColor.whiteColor : NSColor.labelColor;
    return f;
}

- (void)fillRegs
{
    const astrovid_reg *r = NULL;
    const int n = astrovid_tab_regs(_tab, &r);
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; r && i < n; i++)
        [sig appendFormat:@"%s%c", r[i].value, r[i].age < ASTROVID_REG_FADE ? '*' : '.'];
    [sig appendFormat:@"%d", n];
    if ([sig isEqualToString:_regsSig]) return;
    _regsSig = sig;
    _nregs = r ? n : 0;
    [_regs reloadData];
}

- (void)fillLines
{
    const astrovid_line_row *r = NULL;
    const int n = astrovid_tab_lines(_tab, &r);
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; r && i < n; i++) {
        [sig appendFormat:@"%d:", r[i].line];
        [sig appendString:hex(r[i].colors, 8)];
        [sig appendFormat:@"%02X%02X%02X%d%d;", r[i].colorsplit, r[i].vblank, r[i].mode, r[i].changed_mask, r[i].astro_line];
    }
    [sig appendFormat:@"%d", n];
    if ([sig isEqualToString:_linesSig]) return;
    _linesSig = sig;
    _nlines = r ? n : 0;
    [_lines reloadData];
}

- (void)fillColours
{
    const astrovid_swatch *sw = NULL;
    const int n = astrovid_tab_swatches(_tab, &sw);
    for (int i = 0; i < NSWATCH; i++) {
        if (!sw || i >= n) {
            _swLabel[i].stringValue = @"";
            _swView[i].color = nil;
            continue;
        }
        _swView[i].color = colorFromRGB(sw[i].rgb);
        _swLabel[i].stringValue = [NSString stringWithFormat:@"%-5s  $%02X  #%06X  %ld pixels", sw[i].name,
                                                             sw[i].value & 255, sw[i].rgb & 0xFFFFFF, sw[i].uses];
    }
    [_pal setNeedsDisplay:YES];
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int n = astrovid_tab_timing(_tab, &lines);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; lines && i < n; i++) {
        NSString *line = str(lines[i]);
        const NSRange t = [line rangeOfString:@"\t"];
        if (t.location != NSNotFound)
            [s appendFormat:@"%-13s %@\n", [[line substringToIndex:t.location] UTF8String],
                            [line substringFromIndex:t.location + 1]];
        else
            [s appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = n ? s : @"The machine is off";
    _beam.doubleValue = astrovid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & ASTROVID_TAB_PICTURE) {
        const NSSize vp = _scroll.contentSize;
        _vpW = (int)vp.width;
        _vpH = (int)vp.height;
        if (vp.width > 4 && vp.height > 4) astrovid_tab_set_viewport(_tab, (int)vp.width - 2, (int)vp.height - 2);
        [_pic sync];
        [_pic setFrameSize:_pic.intrinsicContentSize];
    }
    if (what & ASTROVID_TAB_TABLES) {
        _status.stringValue = str(astrovid_tab_status(_tab));
        switch ([_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem]) {
        case ASTROVID_S_REGISTERS: [self fillRegs]; break;
        case ASTROVID_S_COLOURS: [self fillColours]; break;
        case ASTROVID_S_LINES: [self fillLines]; break;
        case ASTROVID_S_TIMING: [self fillTiming]; break;
        default: break;
        }
        [self updatePixel];
    }
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = astrovid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= ASTROVID_TAB_PICTURE | ASTROVID_TAB_TABLES;
    if (what & ASTROVID_TAB_OFF) what |= ASTROVID_TAB_PICTURE | ASTROVID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end
