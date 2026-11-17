/*
 * MainWindow -- see MainWindow.h. Menu + display, keyboard capture routed to
 * the console through the bound key table; F9 Controllers, F11 fullscreen, F12
 * debugger. FujiNet Configuration opens the system browser.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include "debugger/DebuggerWindow.h"
#include "DisplayWidget.h"
#include "SoftDisplayWidget.h"
#include "KeyForward.h"
#include "controllers/ControllersWindow.h"
#include "SettingsDialog.h"
#include "help/HelpWindow.h"

#include <QApplication>
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
#include <QShortcut>
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
    setProperty("astroHelpContext", QStringLiteral("main"));
    resize(800, 650);
    auto *gpu = new DisplayWidget(session, this);
    m_display = gpu;
    /* No usable GPU device: fall back to painting in software */
    connect(gpu, &QRhiWidget::renderFailed, this, &MainWindow::useSoftwareDisplay,
            Qt::QueuedConnection);
    m_noBios = buildNoBiosPage();
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(gpu);
    m_stack->addWidget(m_noBios);
    setCentralWidget(m_stack);
    buildMenus();
    showPage();
    m_display->widget()->setFocus();

    /* One timer for the news the other threads leave: the gamepad thread
     * cannot call into Qt, so it posts console actions and hot-plug events and
     * this takes them. The baselines keep pads already attached, and an
     * overlay already on, from announcing themselves. */
    m_padGen = astrosession_gamepad_generation(session);
    m_overlayGen = astrosession_overlay_auto_generation(session);
    connect(&m_tickTimer, &QTimer::timeout, this, &MainWindow::tick);
    m_tickTimer.start(50);

    /* developer affordances, matching the GNOME/Win32 ports (ASTRO_OPEN_KEYPAD
     * is the Controllers window's old name) */
    if (getenv("ASTRO_OPEN_CONTROLLERS") || getenv("ASTRO_OPEN_KEYPAD"))
        QTimer::singleShot(0, this, [this] { toggleControllers(); });
    if (getenv("ASTRO_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { DebuggerWindow::showFor(this, m_session); });
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
    row->addWidget(astroHelpButton(QStringLiteral("nobios")));
    row->addStretch(1);
    v->addWidget(title);
    v->addSpacing(8);
    v->addWidget(body, 0, Qt::AlignHCenter);
    v->addSpacing(16);
    v->addLayout(row);
    v->addStretch(1);
    return page;
}

void MainWindow::useSoftwareDisplay()
{
    if (dynamic_cast<SoftDisplayWidget *>(m_display))
        return;
    QWidget *old = m_display->widget();
    auto *soft = new SoftDisplayWidget(m_session, this);
    const bool shown = m_stack->currentWidget() == old;
    m_stack->removeWidget(old);
    m_display = soft;
    m_stack->insertWidget(0, soft);
    delete old;   /* stops its feed */
    if (shown) {
        m_stack->setCurrentWidget(soft);
        soft->setFocus();
    }
}

/* The picture while there is a BIOS to boot, else the No-BIOS page. */
void MainWindow::showPage()
{
    const bool bios = astrosession_has_system_roms(m_session) != 0;
    m_stack->setCurrentWidget(bios ? m_display->widget() : m_noBios);
    setProperty("astroHelpContext", bios ? QStringLiteral("main") : QStringLiteral("nobios"));
    if (bios)
        m_display->widget()->setFocus();
}

/* Starts the machine once a BIOS is there to boot (after an import). */
void MainWindow::start()
{
    showPage();
    if (!astrosession_is_running(m_session) && astrosession_has_system_roms(m_session))
        restartSession();
}

void MainWindow::toggleControllers()
{
    /* Hidden, not destroyed: a remap in progress and the window's position
     * both survive closing it. */
    astroControllersToggle(this, m_session);
}

void MainWindow::tick()
{
    const unsigned posted = astrosession_sysaction_take(m_session);
    for (int act = 0; act < ASTROSESSION_SYSACT_COUNT; ++act)
        if (posted & (1u << act))
            runSysaction(act);

    const unsigned gen = astrosession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        char msg[160];
        if (astrosession_gamepad_last_event(m_session, msg, sizeof msg) > 0)
            showToast(QString::fromUtf8(msg));
    }
    const unsigned overlay = astrosession_overlay_auto_generation(m_session);
    if (overlay != m_overlayGen) {
        m_overlayGen = overlay;
        showToast(QStringLiteral("Bally BASIC overlay on"));
    }
}

/* A console action, from a key, a pad button, the Controllers window or the
 * menu: always here, on the UI thread. */
void MainWindow::runSysaction(int sysact)
{
    switch (sysact) {
    case ASTROSESSION_SYSACT_RESET_GAME:
        astrosession_sysaction_fire(m_session, ASTROSESSION_SYSACT_RESET_GAME);
        showToast("Reset Game");
        break;
    case ASTROSESSION_SYSACT_RESET_CONFIG:
        if (astrosession_reset_to_config(m_session) != 0)
            showToast(astrosession_last_error(m_session));
        else
            showToast("Reset to CONFIG");
        break;
    case ASTROSESSION_SYSACT_DEBUG_STOP:
        astrosession_sysaction_fire(m_session, ASTROSESSION_SYSACT_DEBUG_STOP);
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

/* A changed setting restarts the machine; a BIOS imported there starts it. */
void MainWindow::showSettings()
{
    bool openControllers = false;
    const bool changed = SettingsDialog::run(this, m_session, [this] { showPage(); },
                                             QString::fromLocal8Bit(qgetenv("ASTRO_OPEN_PREFS")),
                                             &openControllers);
    astroControllersRefresh();
    if (changed)
        restartSession();
    else
        start();
    if (openControllers)
        astroControllersShow(this, m_session);
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
    view->addAction("Controllers", QKeySequence(Qt::Key_F9), this, [this] { toggleControllers(); });
    view->addAction("Debugger", QKeySequence(Qt::Key_F12), this, [this] { DebuggerWindow::toggleFor(this, m_session); });

    auto *fujinet = menuBar()->addMenu("&FujiNet");
    fujinet->addAction("Reset Game", this, [this] { runSysaction(ASTROSESSION_SYSACT_RESET_GAME); });
    fujinet->addAction("Reset to CONFIG", QKeySequence("Ctrl+R"), this,
                       [this] { runSysaction(ASTROSESSION_SYSACT_RESET_CONFIG); });
    fujinet->addAction("FujiNet Configuration…", this, [this] {
        if (astrosession_fujinet_external(m_session)) {
            showToast(QStringLiteral("FujiNet is external: use its own web UI"));
            return;
        }
        QDesktopServices::openUrl(
            QUrl(astrosession_fujinet_webui_url(m_session)));
    });
    fujinet->addAction("FujiNet Console Log…", this, [this] { showFujinetLog(); });

    auto *help = menuBar()->addMenu("&Settings");
    help->addAction("Preferences…", this, [this] { showSettings(); });
    {
        /* works from every window of the app (the Controllers window and the
         * Debugger have the keyboard too) */
        QAction *helpAct = help->addAction(QStringLiteral("FujiNet Go Astrocade &Help"));
        helpAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
        helpAct->setShortcutContext(Qt::ApplicationShortcut);
        addAction(helpAct);
        connect(helpAct, &QAction::triggered, this, [this] {
            astroHelpFor(QApplication::activeWindow() ? QApplication::activeWindow() : this);
        });
    }
    help->addAction("About", this, [this] {
        QMessageBox::about(this, "FujiNet Go Astrocade",
            "FujiNet Go Astrocade " ASTRO_VERSION_STRING "\n\n"
            "Self-contained Bally Astrocade with built-in FujiNet.\n"
            "GPL-3.0-or-later · https://fujinet.online/");
    });
}

void MainWindow::showFujinetLog()
{
    if (astrosession_fujinet_external(m_session)) {
        showToast(QStringLiteral("FujiNet is external: see its own console"));
        return;
    }
    /* a non-modal window with a monospace read-only view refreshed each
     * second; owned by the main window so it closes with it */
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("FujiNet Console Log");
    dlg->resize(700, 500);
    dlg->setProperty("astroHelpContext", QStringLiteral("fujilog"));
    new QShortcut(QKeySequence(Qt::Key_F1), dlg, [dlg] { astroHelpFor(dlg); });
    auto *layout = new QVBoxLayout(dlg);
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    QFont f("monospace");
    f.setStyleHint(QFont::TypeWriter);
    view->setFont(f);
    layout->addWidget(view);
    {
        auto *bar = new QHBoxLayout;
        bar->setContentsMargins(6, 2, 6, 4);
        bar->addStretch(1);
        bar->addWidget(astroHelpButton(QStringLiteral("fujilog")));
        layout->addLayout(bar);
    }

    auto refresh = [this, view] {
        char buf[16384];
        astrosession_fujinet_copy_log(m_session, buf, sizeof buf);
        const QString text = QString::fromUtf8(buf);
        if (view->toPlainText() != text)   /* unchanged: keep selection and scroll */
            view->setPlainText(text);
    };
    refresh();
    auto *timer = new QTimer(dlg);
    QObject::connect(timer, &QTimer::timeout, dlg, refresh);
    timer->start(1000);
    dlg->show();
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F9:
        toggleControllers();
        return;
    case Qt::Key_F11:
        setWindowState(windowState() ^ Qt::WindowFullScreen);
        return;
    case Qt::Key_F12:
        DebuggerWindow::toggleFor(this, m_session);
        return;
    default:
        break;
    }
    if (e->isAutoRepeat()) { e->accept(); return; }
    astroForwardKey(m_session, e, true);
    e->accept();
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) { e->accept(); return; }
    astroForwardKey(m_session, e, false);
    e->accept();
}

/* Keys held into a focus change must not stay down. */
void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::ActivationChange && !isActiveWindow() &&
        !astroControllersActive())
        astrosession_release_all(m_session);
}
