/*
 * KeypadWindow -- the Astrocade 24-key keypad as a 6x4 grid (gold right
 * column), plus System (Reset Game / Reset to CONFIG) and Map rows. Qt6
 * Widgets. See the GNOME keypad_window.c for the shared design.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_KEYPADWINDOW_H
#define ASTRO_KDE_KEYPADWINDOW_H

#include <QWidget>
#include <QVector>

extern "C" {
#include "astrosession.h"
}

class QLabel;
class QPushButton;

class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    explicit KeypadWindow(astrosession *session, QWidget *parent = nullptr);

    /* consume a keystroke as a Map-mode binding; true if consumed */
    bool mapForwardKey(int keysym);

protected:
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;

private:
    enum MapState { Idle, PickTarget, WaitInput };

    void pickTarget(int target);
    void setHighlight(int target, bool on);
    QPushButton *buttonForTarget(int target);

    astrosession *m_session;
    QVector<QPushButton *> m_keyButtons;   /* 24 */
    QPushButton *m_sysButtons[ASTROSESSION_SYSACT_COUNT] = {};
    QPushButton *m_mapBtn = nullptr;
    QLabel *m_status = nullptr;
    MapState m_mapState = Idle;
    int m_mapTarget = -1;
};

#endif
