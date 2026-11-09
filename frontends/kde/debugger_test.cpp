/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running
 * Astrocade session (RL64 expansion RAM) running a hand-assembled loop
 * written through the debugger (needs a BIOS; skips without one): the tabs, attach/detach with show and hide, F12,
 * the stepping keys and buttons, the console (transcript, Tab completion,
 * history), the disassembly (the PC's line, click / double-click / F9 for a
 * breakpoint, selection and Run to Cursor, the wheel, the jump box), the
 * register and memory entries, the Breakpoints tab, and F12 and the View
 * menu from the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "astrodebug.h"
#include "astromemtools.h"
#include "astrosession.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in RL64 expansion RAM at $8000 (interrupts off):
 *   8000  LD SP,$FF00
 *   8003  LD HL,$8100
 *   8006  XOR A
 *   8007  LD B,3
 *   8009  CALL $8050     ; INC C / RET
 *   800C  DJNZ $8009
 *   800E  INC A
 *   800F  LD (HL),A      ; write $8100
 *   8010  OUT ($10),A
 *   8012  IN A,($10)
 *   8014  LD A,(HL)      ; read $8100
 *   8015  JP $8006 */
const uint8_t kProg[] = {
    0x31, 0x00, 0xFF, 0x21, 0x00, 0x81, 0xAF, 0x06, 0x03,
    0xCD, 0x50, 0x80, 0x10, 0xFB, 0x3C, 0x77, 0xD3, 0x10,
    0xDB, 0x10, 0x7E, 0xC3, 0x06, 0x80,
};
const uint8_t kSub[] = { 0x0C, 0xC9 };

/* With the machine stopped: the program in place, PC at its LD B,3. */
void loadProgram(astrodebug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) astrodebug_write(d, (uint16_t)(0x8000 + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSub; i++) astrodebug_write(d, (uint16_t)(0x8050 + i), kSub[i]);
    astrodebug_cpu_set(d, ASTRO_REG_IFF1, 0);
    astrodebug_cpu_set(d, ASTRO_REG_IFF2, 0);
    astrodebug_cpu_set(d, ASTRO_REG_SP, 0xFF00);
    astrodebug_cpu_set(d, ASTRO_REG_HL, 0x8100);
    astrodebug_cpu_set(d, ASTRO_REG_PC, 0x8007);
    astrodebug_cpu_set(d, ASTRO_REG_B, 3);
}

int imOf(astrodebug *d)
{
    astrodebug_cpu c;
    astrodebug_cpu_get(d, &c);
    return c.im;
}

int pcOf(astrodebug *d)
{
    astrodebug_cpu c;
    astrodebug_cpu_get(d, &c);
    return c.pc;
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    astrosession *m_s = nullptr;
    astrodebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    astrosession_start_opts m_opts = {};

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        astrosession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        m_s = astrosession_new(&paths);
        QVERIFY(m_s);
        if (!astrosession_has_system_roms(m_s)) QSKIP("no Astrocade BIOS (build with -DWITH_ASTROCADE_ROMS=ON)");
        m_d = astrosession_debugger(m_s);
        QVERIFY(m_d);

        astrosession_default_opts(m_s, &m_opts);
        m_opts.exp = ASTROSESSION_EXP_RL64_RAM;
        m_opts.cart_path = nullptr;
        QCOMPARE(astrosession_start(m_s, &m_opts), 0);

        /* the window opens on the tab ASTRO_DEBUGGER_TAB names */
        qputenv("ASTRO_DEBUGGER_TAB", "i/o");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("ASTRO_DEBUGGER_TAB");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_s) {
            astrosession_stop(m_s);
            astrosession_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_attached(m_d), 2000);
        if (!astrodebug_is_stopped(m_d)) astrodebug_stop(m_d);
        waitStopped();
        astrodebug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Cart && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video chip"), QStringLiteral("FujiNet") }));
        QVERIFY(astrodebug_is_attached(m_d));
        QVERIFY(astrodebug_is_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!astrodebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0x8007);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8009, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the CALL */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x800C, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step over")));
        /* the buttons do the same */
        astrodebug_write(m_d, 0xFEFE, 0x0C);          /* a return address, as a CALL left it */
        astrodebug_write(m_d, 0xFEFF, 0x80);
        astrodebug_cpu_set(m_d, ASTRO_REG_SP, 0xFEFE);
        astrodebug_cpu_set(m_d, ASTRO_REG_PC, 0x8050);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x800C, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step out")));
        /* Frame: Shift+F7 lands on a frame boundary */
        astrodebug_cpu c, c2;
        astrodebug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d) &&
                                     (astrodebug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!astrodebug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("flags")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $8003"));
        QCOMPARE(pcOf(m_d), 0x8003);
        typeInto(in, QStringLiteral("break 32777"));    /* decimal, the engine's rule: $8009 */
        QVERIFY(astrodebug_breakpoint_check(m_d, 0x8009));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $8009")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), 0x8009);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $8009")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break 32777"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* Load Symbols > Next to the Cartridge (no cartridge: said, not silent),
     * and a .sym file loaded through the engine shows in the views. */
    void symbols()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the Cartridge")) next = a;
        QVERIFY(next);
        const int before = consoleView()->document()->blockCount();
        next->trigger();
        QVERIFY(consoleView()->document()->blockCount() > before);
        QCOMPARE(astrodebug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("800C afterwards\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QCOMPARE(astrodebug_label_address(m_d, "afterwards"), 0x800C);
        QVERIFY(consoleView()->toPlainText().contains(QStringLiteral("1")));
        /* the stop reason names where it is */
        typeInto(in, QStringLiteral("break afterwards"));
        QVERIFY(astrodebug_breakpoint_check(m_d, 0x800C));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d) && pcOf(m_d) == 0x800C, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);
        astrodebug_set_label(m_d, 0x800C, nullptr);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8007) >= 0, 2000);
        const int pcLine = lineOf(v, 0x8007);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's line: the accent fill with white ink, applied last */
        auto sels = v->extraSelections();
        QVERIFY(!sels.isEmpty());
        const auto &pc = sels.last();
        QCOMPARE(pc.cursor.blockNumber(), pcLine);
        QVERIFY(pc.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(pc.format.background().color(), QColor(QStringLiteral("#b5342c")));
        QCOMPARE(pc.format.foreground().color(), QColor(Qt::white));

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        astrodebug_breakpoint_toggle(m_d, 0x8009);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x8009)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, 0x8009)), QColor(0xe0, 0x1b, 0x24));
        astrodebug_breakpoint b;
        QCOMPARE(astrodebug_breakpoint_list(m_d, &b, 1), 1);
        astrodebug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x8009)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x8009)), QColor(0x9a, 0x99, 0x96));
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x800C) >= 0, 2000);
        /* nothing selected: Run to Cursor and F9 say so */
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Select a line")));
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0x800C, 30);
        QVERIFY(!astrodebug_breakpoint_check(m_d, 0x800C));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, 0x800C) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0x800E, 30);
        QVERIFY(!astrodebug_breakpoint_check(m_d, 0x800C));
        QVERIFY(!astrodebug_breakpoint_check(m_d, 0x800E));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0x800F, 0);
        QVERIFY(astrodebug_breakpoint_check(m_d, 0x800F));
        clickLine(0x800F, 0);
        QVERIFY(!astrodebug_breakpoint_check(m_d, 0x800F));
        clickLine(0x8010, 30, true);
        QVERIFY(astrodebug_breakpoint_check(m_d, 0x8010));
        astrodebug_breakpoint_toggle(m_d, 0x8010);
        clickLine(0x8012, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(astrodebug_breakpoint_check(m_d, 0x8012));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!astrodebug_breakpoint_check(m_d, 0x8012));
        astrodebug_breakpoint b[4];
        QCOMPARE(astrodebug_breakpoint_list(m_d, b, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x800F) >= 0, 2000);
        clickLine(0x800F, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x800F, 3000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("run to")));
        /* the selection survives the stop, drawn under the PC's fill */
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x800F) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8007) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, 0x8007) < 0 || lineOf(v, 0x8007) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("8050"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8050) >= 0, 2000);
        astrodebug_set_label(m_d, 0x800C, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x800C) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$8050+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8051) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8007) >= 0, 2000);
        astrodebug_set_label(m_d, 0x800C, nullptr);
    }

    void registersFlagsMemory()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), 15);                  /* PC..IY, AF'..HL', I, R, IM; IFF1/2 are the boxes */
        QTRY_COMPARE_WITH_TIMEOUT(regs[0]->text(), QStringLiteral("8007"), 2000);   /* PC */
        QCOMPARE(regs[14]->text(), QString::number(imOf(m_d)));
        typeInto(regs[0], QStringLiteral("800C"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x800C, 2000);
        typeInto(regs[8], QStringLiteral("1234"));  /* AF' */
        astrodebug_cpu c;
        astrodebug_cpu_get(m_d, &c);
        QCOMPARE(c.af2, 0x1234);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        astrodebug_cpu_set(m_d, ASTRO_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        astrodebug_cpu_get(m_d, &c);
        QCOMPARE(c.cf, 1);
        QCheckBox *iff1 = check(QStringLiteral("IFF1"));
        QTest::mouseClick(iff1, Qt::LeftButton, {}, QPoint(6, iff1->height() / 2));
        astrodebug_cpu_get(m_d, &c);
        QCOMPARE(c.iff1, 1);
        astrodebug_cpu_set(m_d, ASTRO_REG_IFF1, 0);
        astrodebug_cpu_set(m_d, ASTRO_FLAG_C, 0);
        /* the beam / cycles line */
        bool beam = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("scanline"))) beam = l->text().contains(QLatin1String("T-states"));
        QVERIFY(beam);
        /* memory: write through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$C000"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("8110"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        astrodebug_read(m_d, 0x8110, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$8111"));
        typeInto(val, QStringLiteral("$A5"));
        astrodebug_read(m_d, 0x8111, &b, 1);
        QCOMPARE((int)b, 0xA5);
        astrodebug_write(m_d, 0x8112, 0x11);
        addr->setText(QStringLiteral("8112"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        astrodebug_read(m_d, 0x8112, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        QVERIFY(go);
        typeInto(go, QStringLiteral("8000"));
        QPlainTextEdit *mem = nullptr;
        for (QPlainTextEdit *e : m_tabs->currentWidget()->findChildren<QPlainTextEdit *>()) mem = e;
        QVERIFY(mem);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$8020: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("31 00 FF")));
        /* the row Go to found is marked in the accent colour, white ink */
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == QColor(QStringLiteral("#b5342c")) &&
                      s.format.foreground().color() == QColor(Qt::white);
        QVERIFY(marked);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!astrodebug_is_stopped(m_d), 2000);
        typeInto(regs[0], QStringLiteral("8000"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("cpu"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("start"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QPushButton *add = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) add = b;
        QPushButton *clear = button(QStringLiteral("Clear All"));
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        /* exec at a hex address */
        typeInto(start, QStringLiteral("8009"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(t->item(0, 1)->text(), QStringLiteral("Exec  $8009"));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("8100"));
        end->setText(QStringLiteral("$81FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QCOMPARE(t->item(1, 1)->text(), QStringLiteral("Write  $8100-$81FF  if value > 0"));
        /* errors are shown, not swallowed */
        cond->setText(QStringLiteral("nosuchname == 1"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        type->setCurrentText(QStringLiteral("Port In"));
        cond->clear();
        start->setText(QStringLiteral("300"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d) && pcOf(m_d) == 0x8009, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $8009")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        astrodebug_breakpoint list[2];
        QCOMPARE(astrodebug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("write $8100")),
                                 4000);
        QVERIFY(pcOf(m_d) != 0x8009);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(astrodebug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(astrodebug_breakpoint_list(m_d, list, 2), 0);
    }

    QPlainTextEdit *memView()
    {
        return m_tabs->widget(2)->findChild<QPlainTextEdit *>();
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        astrowatch_clear();
        astrodebug_write(m_d, 0x8110, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$8110"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$8110")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        astrodebug_write(m_d, 0x8110, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$8120-$812F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!astrodebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        astrodebug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        astrodebug_breakpoint bp[2];
        QCOMPARE(astrodebug_breakpoint_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & ASTRODEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0x8110);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        astrodebug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$8120: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(astrowatch_count(), 1);
        astrowatch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        for (int i = 0; i < 4; ++i) astrodebug_write(m_d, (uint16_t)(0x8200 + i), pat[i]);
        for (int i = 0; i < 4; ++i) astrodebug_write(m_d, (uint16_t)(0x8300 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        typeInto(go, QStringLiteral("8200-8203"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$8200: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        astrodebug_set_label(m_d, 0x8300, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$8300: DE EE")));
        astrodebug_set_label(m_d, 0x8300, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("8200-8203"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $8300"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $8200"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        for (int i = 0; i < 5; ++i) astrodebug_write(m_d, (uint16_t)(0x8400 + i), (uint8_t)"HeLLo"[i]);
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("8000"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $8400"));
        shot(QStringLiteral("search"));
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Hex"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!astrodebug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        astrodebug_trace_enable(m_d, 0);
        astrodebug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(astrodebug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!astrodebug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("INC A")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves jumps, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("JP ")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("800F-800F"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("LD (HL),A")) &&
                                     !hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(astrodebug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!astrodebug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 9);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Astrocade Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("astroHelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        astrodebug_breakpoint_toggle(m_d, 0x8009);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        astrodebug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 10, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 9);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Astrocade Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 10, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 9; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("Astrocade Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 10, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Cart && I/O"));
    }

    /* A new stop pulls the disassembly back to the PC from wherever it was
     * scrolled, and the Breakpoints tab says why it stopped. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8007) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QVERIFY(stop->text().startsWith(QLatin1String("Stopped")));
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        /* Clear Hits and the full set of types */
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Read/Write"), QStringLiteral("Port In"),
                                      QStringLiteral("Port Out"), QStringLiteral("Port In/Out") }));
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    void otherTabs()
    {
        /* Memory Map & I/O: the engine's text */
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *io = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(!io->toPlainText().isEmpty(), 2000);
        QVERIFY(!io->toPlainText().isEmpty());
        shot(QStringLiteral("memmap"));
        /* Video chip: the pictures and the decoded registers */
        m_tabs->setCurrentIndex(8);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                int pix = 0;
                for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
                    pix += !l->pixmap(Qt::ReturnByValue).isNull();
                return pix == 3;                /* screen, screen RAM, palette */
            }(), 2000);
        QPlainTextEdit *regs = nullptr;
        for (QPlainTextEdit *e : m_tabs->currentWidget()->findChildren<QPlainTextEdit *>()) regs = e;
        QVERIFY(regs);
        QVERIFY(!regs->toPlainText().isEmpty());
        shot(QStringLiteral("video"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet cartridge"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!astrodebug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change keeps the engine attached: the new
     * machine stops at its first instruction and the window follows. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        astrodebug_breakpoint_toggle(m_d, 0x8009);
        astrosession_stop(m_s);
        QCOMPARE(astrosession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d), 3000);
        QCOMPARE(pcOf(m_d), 0x0000);
        QVERIFY(astrodebug_breakpoint_check(m_d, 0x8009));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">0000")), 2000);
        /* and, given the program again, it runs from there to the breakpoint */
        loadProgram(m_d);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_stopped(m_d) && pcOf(m_d) == 0x8009, 5000);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!astrodebug_is_attached(m_d));
        astrodebug_cpu c, c2;
        astrodebug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((astrodebug_cpu_get(m_d, &c2), c2.frame > c.frame + 3), 3000);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!astrodebug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(astrodebug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
