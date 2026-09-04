/*
 * DisplayView: paints the emulator's latest frame with CoreGraphics,
 * letterboxed to a fixed 4:3 (the Astrocade's pixels are not square, and the
 * frame geometry never varies at runtime). The core paces itself, so a plain
 * NSTimer drives the repaint poll -- same reasoning as the GNOME/Windows
 * ports.
 *
 * NOT BUILT OR RUN-VERIFIED on macOS in this environment (no Cocoa
 * toolchain here) -- transposed line-for-line from the verified GNOME/KDE/
 * Win32 frontends and the sibling Intv port's own macOS files, to be
 * validated on the macOS CI runner. See main.m's header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import "AstroKeyForward.h"

#define TICK_INTERVAL (1.0 / 60.0)

@implementation DisplayView {
    astrosession *_session;
    NSTimer *_timer;
    uint32_t *_buf;
    uint64_t _serial;
    CGImageRef _image;
}

- (instancetype)initWithSession:(astrosession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 800, 650)];
    if (!self)
        return nil;
    _session = session;
    _buf = calloc((size_t)ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT,
                  sizeof(uint32_t));
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_image)
        CGImageRelease(_image);
    free(_buf);
}

- (void)start
{
    if (_timer)
        return;
    __weak DisplayView *weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:TICK_INTERVAL
                                             repeats:YES
                                               block:^(NSTimer *t) {
                                                 (void)t;
                                                 [weakSelf pullFrame];
                                               }];
}

- (void)stop
{
    [_timer invalidate];
    _timer = nil;
}

- (void)pullFrame
{
    if (!astrosession_copy_frame(_session, _buf, &_serial))
        return;

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, _buf,
        (size_t)ASTROSESSION_FB_WIDTH * ASTROSESSION_FB_HEIGHT * 4, NULL);
    /* the core's XRGB8888 is 0x00RRGGBB; on a little-endian host the bytes
     * run B,G,R,X, which kCGBitmapByteOrder32Little + AlphaNoneSkipFirst
     * reads exactly (same conclusion the GNOME display draws). */
    CGImageRef image = CGImageCreate(
        ASTROSESSION_FB_WIDTH, ASTROSESSION_FB_HEIGHT, 8, 32,
        ASTROSESSION_FB_WIDTH * 4, space,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);

    if (_image)
        CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (NSRect)destRect
{
    const CGFloat w = self.bounds.size.width;
    const CGFloat h = self.bounds.size.height;
    const CGFloat aspect = 4.0 / 3.0;
    CGFloat dw, dh;

    if (w / h > aspect) { dh = h; dw = h * aspect; }
    else                { dw = w; dh = w / aspect; }
    return NSMakeRect((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!_image)
        return;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect([self destRect]), _image);
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)event
{
    if (event.isARepeat)
        return;
    AstroForwardKeyEvent(_session, event, 1);
}

- (void)keyUp:(NSEvent *)event
{
    AstroForwardKeyEvent(_session, event, 0);
}

@end
