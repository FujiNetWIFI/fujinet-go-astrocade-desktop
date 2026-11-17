/*
 * FujiNet Go Astrocade -- KDE (Qt6 Widgets) frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>

#include "Accent.h"
#include "MainWindow.h"

extern "C" {
#include "astrosession.h"
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("FujiNet Go Astrocade");
    QApplication::setDesktopFileName("online.fujinet.go.astrocade.kde");

    astrosession *session = astrosession_new(nullptr);
    if (!session) {
        qFatal("could not create the session");
        return 1;
    }
    if (astrosession_has_system_roms(session)) {
        astrosession_start_opts opts;
        astrosession_default_opts(session, &opts);
        if (astrosession_start(session, &opts) != 0)
            qWarning("session start: %s", astrosession_last_error(session));
    }

    /* Toggled-on push buttons (Map while armed, the remap target) wear the
     * accent colour with dark text, light or dark. The rule spells out the
     * whole box: with only a background colour the style still draws its own
     * bevel, which darkens it (Breeze), lightens it (Fusion) or ignores it
     * (Kvantum). */
    const QColor accent = astroCtlAccent();
    app.setStyleSheet(
        QStringLiteral(
            "QPushButton[accent=\"true\"] { background-color: %1; color: %4;"
            " border: 1px solid %3; border-radius: 4px; padding: 4px 12px; }"
            "QPushButton[accent=\"true\"]:hover { background-color: %2; }")
            .arg(accent.name(), accent.lighter(112).name(), accent.darker(140).name(),
                 astroCtlAccentInk().name()));

    int rc;
    {
        /* The windows go before the session does: the Controllers window still
         * calls into it as it is hidden. */
        MainWindow win(session);
        win.show();
        rc = app.exec();
    }
    astrosession_free(session);
    return rc;
}
