/*
 * MainWindow -- see MainWindow.h. Menu + display, keyboard capture routed to
 * the console through the bound key table; F9 keypad, F11 fullscreen, F12
 * debugger. FujiNet Configuration opens the system browser.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include "DebuggerWindow.h"
#include "DisplayWidget.h"
#include "KeyForward.h"
#include "KeypadWindow.h"
#include "SettingsDialog.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QUrl>

extern "C" {
#include "bindings.h"
}

MainWindow::MainWindow(astrosession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle("FujiNet Go Astrocade");
    resize(800, 650);
    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);
    buildMenus();
    m_display->setFocus();
    m_sysactTimer = startTimer(100);

    if (!astrosession_has_system_roms(session))
        showToast("No system ROM found — use Machine ▸ Import System ROM…");
}

void MainWindow::showToast(const QString &msg)
{
    statusBar()->showMessage(msg, 4000);
}

void MainWindow::restartSession()
{
    astrosession_start_opts opts;
    astrosession_stop(m_session);
    astrosession_default_opts(m_session, &opts);
    if (astrosession_start(m_session, &opts) != 0)
        showToast(astrosession_last_error(m_session));
}

void MainWindow::buildMenus()
{
    auto *machine = menuBar()->addMenu("&Machine");
    machine->addAction("Open Cartridge…", this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Open Cartridge");
        if (!path.isEmpty() &&
            astrosession_load_cart(m_session, path.toUtf8().constData()) != 0)
            showToast(astrosession_last_error(m_session));
    });
    machine->addAction("Import System ROM…", this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Import System ROM (astro.bin)");
        if (path.isEmpty()) return;
        char name[64];
        if (astrosession_import_rom(m_session, path.toUtf8().constData(),
                                    name, sizeof name) == 0) {
            showToast(QString("Imported %1").arg(name));
            if (!astrosession_is_running(m_session))
                restartSession();
        } else {
            showToast(astrosession_last_error(m_session));
        }
    });

    auto *view = menuBar()->addMenu("&View");
    view->addAction("Keypad", QKeySequence(Qt::Key_F9), this, [this] {
        if (!m_keypad) m_keypad = new KeypadWindow(m_session, this);
        m_keypad->setVisible(!m_keypad->isVisible());
    });
    view->addAction("Debugger", QKeySequence(Qt::Key_F12), this, [this] {
        if (!m_debugger) m_debugger = new DebuggerWindow(m_session, this);
        m_debugger->show();
        m_debugger->raise();
    });

    auto *fujinet = menuBar()->addMenu("&FujiNet");
    fujinet->addAction("Reset Game", QKeySequence(Qt::Key_Backspace), this, [this] {
        astrosession_reset_game(m_session);
        showToast("Reset Game");
    });
    fujinet->addAction("Reset to CONFIG", QKeySequence("Ctrl+R"), this, [this] {
        if (astrosession_reset_to_config(m_session) != 0)
            showToast(astrosession_last_error(m_session));
        else showToast("Reset to CONFIG");
    });
    fujinet->addAction("FujiNet Configuration…", this, [this] {
        QDesktopServices::openUrl(
            QUrl(astrosession_fujinet_webui_url(m_session)));
    });

    auto *help = menuBar()->addMenu("&Settings");
    help->addAction("Preferences…", this, [this] {
        if (SettingsDialog::run(this, m_session))
            restartSession();
    });
    help->addAction("About", this, [this] {
        QMessageBox::about(this, "FujiNet Go Astrocade",
            "FujiNet Go Astrocade " ASTRO_VERSION_STRING "\n\n"
            "Self-contained Bally Astrocade with built-in FujiNet.\n"
            "GPL-3.0-or-later · https://fujinet.online/");
    });
}

void MainWindow::forwardKey(int keysym, bool down)
{
    if (down && m_keypad && m_keypad->mapForwardKey(keysym))
        return;
    astro_mapping m = bindings_resolve_keysym(keysym);
    if (m.kind == ASTRO_MAP_SYSACT) {
        if (down) {
            if (!m_sysactDown[m.value]) {
                m_sysactDown[m.value] = true;
                astrosession_sysaction_fire(m_session, (astrosession_sysaction)m.value);
            }
        } else {
            m_sysactDown[m.value] = false;
        }
        return;
    }
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(m_session, (astrosession_key)m.value, down);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F9:
        if (!m_keypad) m_keypad = new KeypadWindow(m_session, this);
        m_keypad->setVisible(!m_keypad->isVisible());
        return;
    case Qt::Key_F11:
        setWindowState(windowState() ^ Qt::WindowFullScreen);
        return;
    case Qt::Key_F12:
        if (!m_debugger) m_debugger = new DebuggerWindow(m_session, this);
        m_debugger->show();
        return;
    default:
        break;
    }
    if (e->isAutoRepeat()) { e->accept(); return; }
    forwardKey(astro_keysym_from_qt(e), true);
    e->accept();
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) { e->accept(); return; }
    forwardKey(astro_keysym_from_qt(e), false);
    e->accept();
}

void MainWindow::timerEvent(QTimerEvent *ev)
{
    if (ev->timerId() != m_sysactTimer)
        return;
    unsigned pending = astrosession_sysaction_take(m_session);
    for (int a = 0; a < ASTROSESSION_SYSACT_COUNT; ++a)
        if (pending & (1u << a))
            astrosession_sysaction_fire(m_session, (astrosession_sysaction)a);
}
