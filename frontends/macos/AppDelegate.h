/*
 * AppDelegate -- the macOS app delegate (window, menu, keypad). See .m.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "astrosession.h"

@interface AppDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithSession:(astrosession *)session;
@end
