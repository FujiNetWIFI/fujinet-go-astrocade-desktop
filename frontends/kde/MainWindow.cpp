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
#include <QDialog>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdlib>

extern "C" {
#include "bindings.h"
}

MainWindow::MainWindow(astrosession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle("FujiNet Go Astrocade");
    resize(800, 650);
    m_display = new DisplayWidget(session, this);
    m_noBios = buildNoBiosPage();
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_display);
    m_stack->addWidget(m_noBios);
    setCentralWidget(m_stack);
    buildMenus();
    showPage();
    m_display->setFocus();
    m_sysactTimer = startTimer(100);

    /* developer affordances, matching the GNOME/Win32 ports */
    if (getenv("ASTRO_OPEN_KEYPAD"))
        QTimer::singleShot(0, this, [this] { toggleKeypad(); });
    if (getenv("ASTRO_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { showDebugger(); });
    if (getenv("ASTRO_OPEN_PREFS"))
        QTimer::singleShot(0, this, [this] { showSettings(); });
}

/* "BIOS images and ROM sets (*.bin *.BIN ...);;All files (*)": either case,
 * as GNOME's suffix filters. */
static QString romFilter()
{
    QStringList pats;
    for (const char *e : { "bin", "rom", "zip" })
        pats << QStringLiteral("*.%1").arg(QLatin1String(e))
             << QStringLiteral("*.%1").arg(QString::fromLatin1(e).toUpper());
    return QStringLiteral("BIOS images and ROM sets (%1);;All files (*)").arg(pats.join(QLatin1Char(' ')));
}

/* Each chosen file (or the folder) through the import, then one message
 * saying what came of it all, and the machine starts if it now can. */
void MainWindow::importBios(bool folder)
{
    QStringList paths;
    if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Import BIOS from a Folder"));
        if (!dir.isEmpty())
            paths << dir;
    } else {
        paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Import BIOS"), QString(), romFilter());
    }
    QStringList log;
    int imported = 0;
    for (const QString &p : paths) {
        char msg[1024];
        if (astrosession_rom_import(m_session, p.toUtf8().constData(), msg, sizeof msg) == 0)
            imported++;
        log << QString::fromUtf8(msg);
    }
    if (!log.isEmpty())
        QMessageBox::information(this, imported ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                                 log.join(QLatin1Char('\n')));
    start();
}

/* In place of the picture until a BIOS is imported. */
QWidget *MainWindow::buildNoBiosPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addStretch(1);
    auto *title = new QLabel(QStringLiteral("No Astrocade BIOS"));
    QFont tf = title->font();
    tf.setPointSizeF((tf.pointSizeF() > 0 ? tf.pointSizeF() : 10) * 1.8);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    auto *body = new QLabel(QStringLiteral(
        "Bally's BIOS ROM is not part of the app. Import the one you have \u2014 astro.bin, "
        "ballyhlc.bin or bioswhit.bin, a zip such as MAME's astrocde set, or a folder \u2014 "
        "and the Astrocade starts."));
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    body->setFixedWidth(520);
    body->setMinimumHeight(body->heightForWidth(520));
    auto *row = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import BIOS\u2026"));
    auto *folder = new QPushButton(QStringLiteral("Import a Folder\u2026"));
    files->setDefault(true);
    connect(files, &QPushButton::clicked, this, [this] { importBios(false); });
    connect(folder, &QPushButton::clicked, this, [this] { importBios(true); });
    row->addStretch(1);
    row->addWidget(files);
    row->addWidget(folder);
    row->addStretch(1);
    v->addWidget(title);
    v->addSpacing(8);
    v->addWidget(body, 0, Qt::AlignHCenter);
    v->addSpacing(16);
    v->addLayout(row);
    v->addStretch(1);
    return page;
}

/* The picture while there is a BIOS to boot, else the No-BIOS page. */
void MainWindow::showPage()
{
    const bool bios = astrosession_has_system_roms(m_session) != 0;
    m_stack->setCurrentWidget(bios ? static_cast<QWidget *>(m_display) : m_noBios);
    if (bios)
        m_display->setFocus();
}

/* Starts the machine once a BIOS is there to boot (after an import). */
void MainWindow::start()
{
    showPage();
    if (!astrosession_is_running(m_session) && astrosession_has_system_roms(m_session))
        restartSession();
}

void MainWindow::toggleKeypad()
{
    if (!m_keypad) m_keypad = new KeypadWindow(m_session, this);
    m_keypad->setVisible(!m_keypad->isVisible());
}

void MainWindow::showDebugger()
{
    if (!m_debugger) m_debugger = new DebuggerWindow(m_session, this);
    m_debugger->show();
    m_debugger->raise();
}

/* A changed setting restarts the machine; a BIOS imported there starts it. */
void MainWindow::showSettings()
{
    const bool romsTab = qgetenv("ASTRO_OPEN_PREFS") == "ROMs";
    if (SettingsDialog::run(this, m_session, [this] { showPage(); }, romsTab))
        restartSession();
    else
        start();
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
    showPage();
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
    machine->addSeparator();
    machine->addAction("Import &BIOS…", this, [this] { importBios(false); });
    machine->addAction("Import BIOS from a &Folder…", this, [this] { importBios(true); });

    auto *view = menuBar()->addMenu("&View");
    view->addAction("Keypad", QKeySequence(Qt::Key_F9), this, [this] { toggleKeypad(); });
    view->addAction("Debugger", QKeySequence(Qt::Key_F12), this, [this] { showDebugger(); });

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
    fujinet->addAction("FujiNet Console Log…", this, [this] { showFujinetLog(); });

    auto *help = menuBar()->addMenu("&Settings");
    help->addAction("Preferences…", this, [this] { showSettings(); });
    help->addAction("About", this, [this] {
        QMessageBox::about(this, "FujiNet Go Astrocade",
            "FujiNet Go Astrocade " ASTRO_VERSION_STRING "\n\n"
            "Self-contained Bally Astrocade with built-in FujiNet.\n"
            "GPL-3.0-or-later · https://fujinet.online/");
    });
}

void MainWindow::showFujinetLog()
{
    /* a non-modal window with a monospace read-only view refreshed each
     * second; owned by the main window so it closes with it */
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("FujiNet Console Log");
    dlg->resize(700, 500);
    auto *layout = new QVBoxLayout(dlg);
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    QFont f("monospace");
    f.setStyleHint(QFont::TypeWriter);
    view->setFont(f);
    layout->addWidget(view);

    auto refresh = [this, view] {
        char buf[16384];
        astrosession_fujinet_copy_log(m_session, buf, sizeof buf);
        view->setPlainText(buf);
    };
    refresh();
    auto *timer = new QTimer(dlg);
    QObject::connect(timer, &QTimer::timeout, dlg, refresh);
    timer->start(1000);
    dlg->show();
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
    if (m.kind == ASTRO_MAP_KEY) {
        astrosession_keypad_set(m_session, (astrosession_key)m.value, down);
        return;
    }
    if (m.kind == ASTRO_MAP_HANDLE) {
        if (down) m_handleMask |= (uint8_t)m.value;
        else m_handleMask &= (uint8_t)~m.value;
        astrosession_handle_set(m_session, 0, m_handleMask);
    }
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
