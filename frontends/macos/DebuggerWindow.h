/*
 * DebuggerWindow -- the macOS debugger window. See .m. NOT run-verified here.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

@interface DebuggerWindow : NSWindowController <NSWindowDelegate>
+ (void)showDebugger;
@end
