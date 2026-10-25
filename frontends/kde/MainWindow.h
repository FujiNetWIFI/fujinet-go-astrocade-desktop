/*
 * MainWindow -- the KDE frontend main window (plain Qt6 Widgets).
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_MAINWINDOW_H
#define ASTRO_KDE_MAINWINDOW_H

#include <QMainWindow>

extern "C" {
#include "astrosession.h"
}

class DisplayWidget;
class KeypadWindow;
class DebuggerWindow;
class QStackedWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(astrosession *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;

private:
    void buildMenus();
    void forwardKey(int keysym, bool down);
    void restartSession();
    void start();
    void showPage();
    void importBios(bool folder);
    QWidget *buildNoBiosPage();
    void toggleKeypad();
    void showDebugger();
    void showSettings();
    void showToast(const QString &msg);
    void showFujinetLog();

    astrosession *m_session;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_noBios = nullptr;
    DisplayWidget *m_display = nullptr;
    KeypadWindow *m_keypad = nullptr;
    DebuggerWindow *m_debugger = nullptr;
    bool m_sysactDown[ASTROSESSION_SYSACT_COUNT] = {};
    uint8_t m_handleMask = 0;
    int m_sysactTimer = 0;
    void timerEvent(QTimerEvent *) override;
};

#endif
