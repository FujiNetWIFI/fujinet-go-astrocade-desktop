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

class DisplayView;
class KeypadWindow;
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
    void restartSession();
    void start();
    void showPage();
    void importBios(bool folder);
    QWidget *buildNoBiosPage();
    void toggleKeypad();
    void showSettings();
    void showToast(const QString &msg);
    void showFujinetLog();
    void useSoftwareDisplay();

    astrosession *m_session;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_noBios = nullptr;
    DisplayView *m_display = nullptr;
    KeypadWindow *m_keypad = nullptr;
    int m_sysactTimer = 0;
    void timerEvent(QTimerEvent *) override;
    void changeEvent(QEvent *) override;
};

#endif
