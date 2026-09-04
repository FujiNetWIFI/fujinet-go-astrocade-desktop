/*
 * SettingsDialog -- BIOS variant + RAM expansion (restart to apply).
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_SETTINGSDIALOG_H
#define ASTRO_KDE_SETTINGSDIALOG_H

#include <QDialog>

extern "C" {
#include "astrosession.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* returns true if a machine option changed (caller restarts the session) */
    static bool run(QWidget *parent, astrosession *session);
};

#endif
