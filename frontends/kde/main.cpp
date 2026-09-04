/*
 * FujiNet Go Astrocade -- KDE (Qt6 Widgets) frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>

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

    MainWindow win(session);
    win.show();

    int rc = app.exec();
    astrosession_free(session);
    return rc;
}
