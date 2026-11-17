/*
 * MainWindow -- the KDE frontend main window (plain Qt6 Widgets).
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_MAINWINDOW_H
#define ASTRO_KDE_MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>

extern "C" {
#include "astrosession.h"
}

class DisplayView;
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
    void toggleControllers();
    /* the news the other threads leave: console actions, hot-plugs, the overlay */
    void tick();
    void runSysaction(int sysact);
    void showSettings();
    void showToast(const QString &msg);
    void showFujinetLog();
    void useSoftwareDisplay();

    astrosession *m_session;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_noBios = nullptr;
    DisplayView *m_display = nullptr;
    QTimer m_tickTimer;
    unsigned m_padGen = 0;
    unsigned m_overlayGen = 0;
    void changeEvent(QEvent *) override;
};

#endif
