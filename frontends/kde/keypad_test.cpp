/*
 * keypad_test -- the Qt key translation (KeyForward.cpp): the physical key
 * becomes the keysym whatever Shift or NumLock say, the app's own keys are
 * recognised, and the Bally BASIC typing path: with the overlay on a printable
 * key bound to a keypad key types its character (and Return types GO) instead
 * of pressing that key, with it off the bindings hold -- driven with synthetic
 * events on the offscreen platform against an unstarted session. (The
 * Controllers window has its own test, controllers_test.)
 *
 * Scancodes are evdev + 8, the convention both Qt platform plugins use.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QKeyEvent>
#include <QTemporaryDir>
#include <QtTest>

#include "KeyForward.h"

namespace {

constexpr quint32 kEv = 8;   /* nativeScanCode = evdev code + kEv */
enum { EV_H = 35, EV_8 = 9, EV_KP8 = 72, EV_KPENTER = 96, EV_ENTER = 28, EV_UP = 103, EV_F9 = 67, EV_F12 = 88 };

QKeyEvent keyEvent(QEvent::Type type, int key, quint32 evdev,
                   Qt::KeyboardModifiers mods = {}, const QString &text = QString())
{
    return QKeyEvent(type, key, mods, evdev ? evdev + kEv : 0, 0, 0, text, false);
}

constexpr int KEY_CH = ASTRO_TARGET_KEYPAD + ASTROSESSION_KEY_CH;
constexpr int KEY_EQ = ASTRO_TARGET_KEYPAD + ASTROSESSION_KEY_EQ;

} // namespace

class KeypadTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    astrosession *m_s = nullptr;

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

    void appKeys()
    {
        for (int key : { Qt::Key_F9, Qt::Key_F10, Qt::Key_F11, Qt::Key_F12 }) {
            QKeyEvent ev = keyEvent(QEvent::KeyPress, key, 0);
            QVERIFY(astroIsAppKey(&ev));
        }
        for (int key : { Qt::Key_F8, Qt::Key_A, Qt::Key_Escape }) {
            QKeyEvent ev = keyEvent(QEvent::KeyPress, key, 0);
            QVERIFY(!astroIsAppKey(&ev));
        }
    }

    void boundKeysDriveTheConsole()
    {
        /* overlay off: 'h' is the CH key */
        astrosession_overlay_set(m_s, 0);
        QKeyEvent down = keyEvent(QEvent::KeyPress, Qt::Key_H, EV_H, {}, "h");
        QVERIFY(astroForwardKey(m_s, &down, true));
        QVERIFY(astrosession_target_held(m_s, KEY_CH));
        QKeyEvent up = keyEvent(QEvent::KeyRelease, Qt::Key_H, EV_H, {}, "h");
        astroForwardKey(m_s, &up, false);
        QVERIFY(!astrosession_target_held(m_s, KEY_CH));
        /* and Return is the = key */
        QKeyEvent ret = keyEvent(QEvent::KeyPress, Qt::Key_Return, EV_ENTER, {}, "\r");
        QVERIFY(astroForwardKey(m_s, &ret, true));
        QVERIFY(astrosession_target_held(m_s, KEY_EQ));
        QKeyEvent retUp = keyEvent(QEvent::KeyRelease, Qt::Key_Return, EV_ENTER, {}, "\r");
        astroForwardKey(m_s, &retUp, false);
        QVERIFY(!astrosession_target_held(m_s, KEY_EQ));
    }

    void overlayTypesInsteadOfPressing()
    {
        astrosession_overlay_set(m_s, 1);
        QTRY_COMPARE_WITH_TIMEOUT(astrosession_typing_pending(m_s), 0, 3000);

        /* 'h' types H (a shift tap, then the CH key); the CH key itself is not held */
        QKeyEvent down = keyEvent(QEvent::KeyPress, Qt::Key_H, EV_H, {}, "h");
        QVERIFY(astroForwardKey(m_s, &down, true));
        QVERIFY(astrosession_typing_pending(m_s) > 0);
        QVERIFY(!astrosession_target_held(m_s, KEY_CH));
        QKeyEvent up = keyEvent(QEvent::KeyRelease, Qt::Key_H, EV_H, {}, "h");
        astroForwardKey(m_s, &up, false);
        QTRY_COMPARE_WITH_TIMEOUT(astrosession_typing_pending(m_s), 0, 3000);

        /* Shift+1 types '!' (the blue shift, then the 4 key) */
        QKeyEvent bang = keyEvent(QEvent::KeyPress, Qt::Key_Exclam, 2, Qt::ShiftModifier, "!");
        QVERIFY(astroForwardKey(m_s, &bang, true));
        QVERIFY(astrosession_typing_pending(m_s) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(astrosession_typing_pending(m_s), 0, 3000);

        /* Return types GO, and does not press the = key */
        QKeyEvent ret = keyEvent(QEvent::KeyPress, Qt::Key_Return, EV_ENTER, {}, "\r");
        QVERIFY(astroForwardKey(m_s, &ret, true));
        QVERIFY(astrosession_typing_pending(m_s) > 0);
        QVERIFY(!astrosession_target_held(m_s, KEY_EQ));
        QTRY_COMPARE_WITH_TIMEOUT(astrosession_typing_pending(m_s), 0, 3000);

        /* a key bound to the stick keeps its binding, and Ctrl never types */
        QKeyEvent arrow = keyEvent(QEvent::KeyPress, Qt::Key_Up, EV_UP);
        QVERIFY(astroForwardKey(m_s, &arrow, true));
        QCOMPARE(astrosession_typing_pending(m_s), 0);
        QKeyEvent arrowUp = keyEvent(QEvent::KeyRelease, Qt::Key_Up, EV_UP);
        astroForwardKey(m_s, &arrowUp, false);
        QKeyEvent ctrlH = keyEvent(QEvent::KeyPress, Qt::Key_H, EV_H, Qt::ControlModifier, "\b");
        QVERIFY(!astroForwardKey(m_s, &ctrlH, true));
        QCOMPARE(astrosession_typing_pending(m_s), 0);

        astrosession_overlay_set(m_s, 0);
    }
};

QTEST_MAIN(KeypadTest)
#include "keypad_test.moc"
