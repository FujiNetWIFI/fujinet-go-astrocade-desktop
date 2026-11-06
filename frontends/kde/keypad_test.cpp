/*
 * keypad_test -- the Qt key translation (KeyForward.cpp) and the Controllers
 * window (keypad/KeypadWindow.cpp): fixed floating window, raw press/release,
 * the held highlight, the knob dial, and Map / Clear / Defaults, driven with
 * synthetic events on the offscreen platform against an unstarted session.
 *
 * Scancodes are evdev + 8, the convention both Qt platform plugins use.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QKeyEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include "KeyForward.h"
#include "keypad/KeypadWindow.h"

namespace {

constexpr quint32 kEv = 8;   /* nativeScanCode = evdev code + kEv */
enum { EV_1 = 2, EV_8 = 9, EV_KP8 = 72, EV_KPENTER = 96, EV_F9 = 67 };

QKeyEvent keyEvent(QEvent::Type type, int key, quint32 evdev,
                   Qt::KeyboardModifiers mods = {}, bool autorepeat = false)
{
    return QKeyEvent(type, key, mods, evdev ? evdev + kEv : 0, 0, 0, QString(),
                     autorepeat);
}

constexpr int TRIGGER = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER);
constexpr int UP = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP);
constexpr int KNOB = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB);

} // namespace

class KeypadTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    astrosession *m_s = nullptr;

    void sendKey(QWidget *w, QEvent::Type t, int key, quint32 evdev,
                 Qt::KeyboardModifiers m = {})
    {
        QKeyEvent e = keyEvent(t, key, evdev, m);
        QCoreApplication::sendEvent(w, &e);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QByteArray cfg = (m_dir.path() + "/config").toUtf8();
        const QByteArray data = (m_dir.path() + "/data").toUtf8();
        astrosession_paths paths = { cfg.constData(), data.constData() };
        m_s = astrosession_new(&paths);
        QVERIFY(m_s);
    }
    void cleanupTestCase() { astrosession_free(m_s); }

    void keysymFromScancode()
    {
        /* Shift+8 is still the 8 key; the numeric keypad stays distinct */
        QKeyEvent shift8 = keyEvent(QEvent::KeyPress, Qt::Key_Asterisk, EV_8, Qt::ShiftModifier);
        QCOMPARE(astroKeysymFromQt(&shift8), (uint32_t)'8');
        QKeyEvent kp8 = keyEvent(QEvent::KeyPress, Qt::Key_Up, EV_KP8, Qt::KeypadModifier);
        QCOMPARE(astroKeysymFromQt(&kp8), (uint32_t)ASTROSESSION_KEYSYM_KP_8);
        QKeyEvent kpe = keyEvent(QEvent::KeyPress, Qt::Key_Enter, EV_KPENTER, Qt::KeypadModifier);
        QCOMPARE(astroKeysymFromQt(&kpe), (uint32_t)ASTROSESSION_KEYSYM_KP_ENTER);
        /* no scancode: Qt's own key */
        QKeyEvent up = keyEvent(QEvent::KeyPress, Qt::Key_Up, 0);
        QCOMPARE(astroKeysymFromQt(&up), (uint32_t)ASTROSESSION_KEYSYM_UP);
    }

    void windowIsFixedAndFloating()
    {
        KeypadWindow w(m_s);
        w.show();
        QVERIFY(w.windowFlags() & Qt::Dialog);
        QVERIFY(!w.isVisible() || w.minimumSize() == w.maximumSize() || w.size().isValid());
        QCOMPARE(w.minimumSize(), w.maximumSize());
    }

    void rawPressAndRelease()
    {
        KeypadWindow w(m_s);
        w.show();
        QPushButton *b = w.buttonFor(TRIGGER);
        QVERIFY(b);
        QTest::mousePress(b, Qt::LeftButton);
        QVERIFY(astrosession_target_held(m_s, TRIGGER));
        QTest::mouseRelease(b, Qt::LeftButton);
        QVERIFY(!astrosession_target_held(m_s, TRIGGER));
        /* a keypad key too */
        QPushButton *seven = w.buttonFor(ASTROSESSION_KEY_7);
        QTest::mousePress(seven, Qt::LeftButton);
        QVERIFY(astrosession_target_held(m_s, ASTROSESSION_KEY_7));
        QTest::mouseRelease(seven, Qt::LeftButton);
        QVERIFY(!astrosession_target_held(m_s, ASTROSESSION_KEY_7));
    }

    void heldHighlight()
    {
        KeypadWindow w(m_s);
        w.show();
        QPushButton *b = w.buttonFor(UP);
        QVERIFY(!b->styleSheet().contains("#b5342c"));
        astrosession_input_key(m_s, ASTROSESSION_KEYSYM_UP, 1, 0);   /* the keyboard holds it */
        QTRY_VERIFY_WITH_TIMEOUT(b->styleSheet().contains("#b5342c"), 1000);
        astrosession_input_key(m_s, ASTROSESSION_KEYSYM_UP, 0, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!b->styleSheet().contains("#b5342c"), 1000);
    }

    void typingInThePanelDrivesTheMachine()
    {
        KeypadWindow w(m_s);
        w.show();
        sendKey(&w, QEvent::KeyPress, Qt::Key_Up, 103 /* KEY_UP */);
        QVERIFY(astrosession_target_held(m_s, UP));
        sendKey(&w, QEvent::KeyRelease, Qt::Key_Up, 103);
        QVERIFY(!astrosession_target_held(m_s, UP));
        /* F9 hides it */
        sendKey(&w, QEvent::KeyPress, Qt::Key_F9, EV_F9);
        QVERIFY(!w.isVisible());
    }

    void knobDial()
    {
        KeypadWindow w(m_s);
        w.show();
        KnobDial *d = w.dialFor(0);
        d->dragTo(QPointF(d->width() / 2.0, 4));                   /* straight up */
        QVERIFY(qAbs(astrosession_knob_get(m_s, 0) - 128) <= 1);
        d->dragTo(QPointF(d->width() - 4, d->height() - 4));       /* lower right */
        QCOMPARE(astrosession_knob_get(m_s, 0), 255);
        d->dragTo(QPointF(4, d->height() - 4));                    /* lower left */
        QCOMPARE(astrosession_knob_get(m_s, 0), 0);
        astrosession_knob_set(m_s, 0, 128);
    }

    void mapRebindClearDefaults()
    {
        KeypadWindow w(m_s);
        w.show();
        auto *mapBtn = w.findChildren<QPushButton *>().at(0);   /* not it: find by text */
        for (QPushButton *b : w.findChildren<QPushButton *>())
            if (b->text() == "Map") mapBtn = b;
        QTest::mouseClick(mapBtn, Qt::LeftButton);
        QCOMPARE(w.mapTarget(), -1);

        /* while armed a control click picks, it does not press */
        QPushButton *seven = w.buttonFor(ASTROSESSION_KEY_7);
        QTest::mousePress(seven, Qt::LeftButton);
        QTest::mouseRelease(seven, Qt::LeftButton);
        QCOMPARE(w.mapTarget(), (int)ASTROSESSION_KEY_7);
        QVERIFY(!astrosession_target_held(m_s, ASTROSESSION_KEY_7));

        /* the next key binds: the '1' key, taken from keypad 1 */
        sendKey(&w, QEvent::KeyPress, Qt::Key_1, EV_1);
        QCOMPARE(bindings_target_keysym(ASTROSESSION_KEY_7), (uint32_t)'1');
        QCOMPARE(bindings_target_for_key('1'), (int)ASTROSESSION_KEY_7);
        QCOMPARE(bindings_target_keysym(ASTROSESSION_KEY_1), (uint32_t)0);
        QVERIFY(w.hint().contains("was"));
        QCOMPARE(w.mapTarget(), -1);           /* still armed for the next one */

        /* the knob takes an axis, not a key */
        w.pickTarget(KNOB);
        sendKey(&w, QEvent::KeyPress, Qt::Key_X, 45);
        QCOMPARE(bindings_target_for_key('x'), (int)ASTROSESSION_KEY_MUL);
        QCOMPARE(w.mapTarget(), KNOB);

        /* Clear unbinds the picked control */
        w.pickTarget(ASTROSESSION_KEY_7);
        for (QPushButton *b : w.findChildren<QPushButton *>())
            if (b->text() == "Clear") QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(bindings_target_keysym(ASTROSESSION_KEY_7), (uint32_t)0);

        /* Defaults restores everything */
        for (QPushButton *b : w.findChildren<QPushButton *>())
            if (b->text() == "Defaults") QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(bindings_target_keysym(ASTROSESSION_KEY_7), (uint32_t)'7');
        QCOMPARE(bindings_target_keysym(ASTROSESSION_KEY_1), (uint32_t)'1');

        /* hiding leaves Map */
        w.hide();
        QCOMPARE(w.mapTarget(), -2);
    }
};

QTEST_MAIN(KeypadTest)
#include "keypad_test.moc"
