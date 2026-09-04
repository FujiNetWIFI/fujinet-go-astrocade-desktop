/*
 * FujiNet Go Astrocade -- macOS (AppKit) frontend entry point.
 *
 * NOT BUILT OR RUN-VERIFIED in this environment: there is no Cocoa toolchain
 * on the Linux dev box this was written on, so unlike the GNOME, KDE and
 * Windows frontends (all run-verified -- the last under Wine), the macOS
 * frontend is transposed from those and from the sibling Intv port's own
 * macOS files, and is validated on the macOS CI runner (.github/workflows/
 * macos.yml). Kept structurally identical to the verified frontends so a
 * reviewer with a Mac can trust the shape.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "astrosession.h"

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    @autoreleasepool {
        astrosession *session = astrosession_new(NULL);
        if (!session) {
            NSLog(@"fatal: could not create the session");
            return 1;
        }
        if (astrosession_has_system_roms(session)) {
            astrosession_start_opts opts;
            astrosession_default_opts(session, &opts);
            if (astrosession_start(session, &opts) != 0)
                NSLog(@"session start: %s", astrosession_last_error(session));
        }

        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [[AppDelegate alloc] initWithSession:session];
        app.delegate = delegate;
        [app run];

        astrosession_free(session);
    }
    return 0;
}
