/*
 * SettingsDialog -- BIOS variant + RAM expansion (restart to apply), and the
 * ROMs tab: each BIOS imported or not, with the Machine menu's import.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_SETTINGSDIALOG_H
#define ASTRO_KDE_SETTINGSDIALOG_H

#include <QDialog>

#include <functional>

extern "C" {
#include "astrosession.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* returns true if a machine option changed (caller restarts the session);
     * imported runs after each import from the ROMs tab, which romsTab opens on */
    static bool run(QWidget *parent, astrosession *session,
                    std::function<void()> imported, bool romsTab = false);
};

#endif
