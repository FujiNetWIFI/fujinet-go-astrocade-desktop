/*
 * DisplayView -- the emulator display NSView. See DisplayView.m.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "astrosession.h"

@interface DisplayView : NSView
- (instancetype)initWithSession:(astrosession *)session;
- (void)start;
- (void)stop;
@end
