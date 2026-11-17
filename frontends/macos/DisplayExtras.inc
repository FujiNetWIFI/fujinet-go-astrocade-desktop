/*
 * DisplayExtras.inc -- the keyboard, included into the display view's
 * @implementation (see DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

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

- (void)flagsChanged:(NSEvent *)event
{
    AstroForwardModifierEvent(_session, event);
}

/* a key held into a focus change never gets its key-up */
- (BOOL)resignFirstResponder
{
    astrosession_release_all(_session);
    return [super resignFirstResponder];
}
