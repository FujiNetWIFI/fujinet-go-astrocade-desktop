/*
 * SettingsWindow -- the macOS Settings window. See SettingsWindow.m.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import <Cocoa/Cocoa.h>

#include "astrosession.h"

@interface SettingsWindow : NSObject
/* imported runs after a BIOS is imported from the ROMs tab; closed when the
 * window closes, dirty when a machine setting changed (restart). */
+ (void)showForSession:(astrosession *)session
              romsPage:(BOOL)romsPage
              imported:(void (^)(void))imported
                closed:(void (^)(BOOL dirty))closed;
@end
