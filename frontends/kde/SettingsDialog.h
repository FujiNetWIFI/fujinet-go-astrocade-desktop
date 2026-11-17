/*
 * SettingsDialog -- BIOS variant + RAM expansion (restart to apply), the Input
 * tab (gamepads and their players, Reset Controls, Open Controllers), and the
 * ROMs tab: each BIOS imported or not, with the Machine menu's import.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_SETTINGSDIALOG_H
#define ASTRO_KDE_SETTINGSDIALOG_H

#include <QDialog>
#include <QString>

#include <functional>

extern "C" {
#include "astrosession.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* returns true if a machine option changed (caller restarts the session);
     * imported runs after each import from the ROMs tab. startTab names the tab
     * to open on ("Input" or "ROMs"; anything else is Machine). *openControllers
     * is set when the Input tab's Open Controllers closed the dialog (as OK):
     * a modal dialog would block the window it opens. */
    static bool run(QWidget *parent, astrosession *session,
                    std::function<void()> imported, const QString &startTab = QString(),
                    bool *openControllers = nullptr);
};

#endif
