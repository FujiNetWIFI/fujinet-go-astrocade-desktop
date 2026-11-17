/*
 * controllers_test -- the KDE Controllers window, offscreen: hidden at start,
 * fixed size, the mouse holding a key, the trigger and the grip's ring (and
 * letting go when it drags off, and when the window hides), the knob's dial
 * turned by a drag and the wheel, keys reaching the console, Map taking a key
 * and a gamepad button (and an axis for the knob), F9 hiding it, the Bally
 * BASIC overlay check box, and a pad's row with its Automatic / Player 1-4
 * picker -- on an SDL virtual gamepad, with no machine and no BIOS. Waits poll
 * with a five second ceiling, so a slow runner is merely slow. SKIPs (77) where
 * SDL has no virtual joysticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "astrosession.h"
#include "bindings.h"
#include "controllers/ControllersWindow.h"
#include "gamepad_sdl.h"

namespace {

using astroctl::kCasePad;
using astroctl::kTop;
using astroctl::kUnit;

int failures = 0;

void check(bool pass, const char *what)
{
    std::printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

/* Waits until the target is held (or not); the window's own timers and the pad
 * thread both need the event loop. */
void expectHeld(astrosession *sess, int target, bool want, const char *what)
{
    for (int tries = 0; tries < 250; tries++) {
        if ((astrosession_target_held(sess, target) != 0) == want) {
            std::printf("ok: %s\n", what);
            return;
        }
        QTest::qWait(20);
    }
    std::printf("FAIL: %s\n", what);
    failures++;
}

QWidget *findWindow()
{
    for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == QLatin1String("controllers")) return top;
    return nullptr;
}

QWidget *findConsole(QWidget *win)
{
    return win->findChild<QWidget *>(QStringLiteral("console"), Qt::FindDirectChildrenOnly);
}

/* the pixel centre of a labelled part: player -1 is the console */
QPoint centreOf(const char *label, int player)
{
    int count = 0;
    float wide = 0, tall = 0;
    const astro_control *layout = astrosession_controls_layout(&count, &wide, &tall);
    for (int idx = 0; idx < count; idx++) {
        if (layout[idx].player != player || !layout[idx].label || std::strcmp(layout[idx].label, label) != 0)
            continue;
        return QPoint(int(kCasePad + (layout[idx].xpos + layout[idx].width / 2) * kUnit),
                      int(kTop + (layout[idx].ypos + layout[idx].height / 2) * kUnit));
    }
    return QPoint(-1, -1);
}

QPushButton *findButton(QWidget *win, const char *text)
{
    for (QPushButton *button : win->findChildren<QPushButton *>())
        if (button->text() == QLatin1String(text)) return button;
    return nullptr;
}

QComboBox *waitForPadRow(QWidget *win)
{
    for (int tries = 0; tries < 250; tries++) {
        for (QComboBox *box : win->findChildren<QComboBox *>())
            if (box->count() == 5 && box->itemText(1) == QLatin1String("Player 1")) return box;
        QTest::qWait(20);
    }
    return nullptr;
}

constexpr int UP0 = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_UP);
constexpr int RIGHT0 = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_RIGHT);
constexpr int TRIG0 = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_TRIGGER);
constexpr int TRIG1 = ASTRO_TARGET_PLAYER(1, ASTRO_ACT_TRIGGER);
constexpr int KEY7 = ASTRO_TARGET_KEYPAD + ASTROSESSION_KEY_7;

void keyTests(astrosession *sess, QWidget *win)
{
    /* the arrow keys steer player 1 and Space pulls its trigger (the defaults) */
    QTest::keyPress(win, Qt::Key_Up);
    expectHeld(sess, UP0, true, "a typed Up steers player 1");
    QTest::keyRelease(win, Qt::Key_Up);
    expectHeld(sess, UP0, false, "and its release lets go");

    QTest::keyPress(win, Qt::Key_Space);
    QTest::keyPress(win, Qt::Key_Right);
    expectHeld(sess, TRIG0, true, "the trigger and a direction together: trigger");
    expectHeld(sess, RIGHT0, true, "and the direction");
    QTest::keyRelease(win, Qt::Key_Right);
    QTest::keyRelease(win, Qt::Key_Space);
    expectHeld(sess, TRIG0, false, "both released");

    /* a bound key presses its keypad key */
    QTest::keyPress(win, Qt::Key_7);
    expectHeld(sess, KEY7, true, "the 7 key presses keypad 7");
    QTest::keyRelease(win, Qt::Key_7);
    expectHeld(sess, KEY7, false, "and lets go");
}

void mouseTests(astrosession *sess, QWidget *win)
{
    QWidget *console = findConsole(win);
    check(console != nullptr, "the console widget exists");
    if (!console) return;

    const QPoint key7 = centreOf("7", -1);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, key7);
    expectHeld(sess, KEY7, true, "pressing key 7 holds it");
    QTest::mouseMove(console, QPoint(2, 2));
    expectHeld(sess, KEY7, false, "dragging off lets it go");
    QTest::mouseMove(console, key7);
    expectHeld(sess, KEY7, true, "and back on holds it again");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, key7);
    expectHeld(sess, KEY7, false, "releasing lets go");

    const QPoint trigger = centreOf("Trigger", 1);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, trigger);
    expectHeld(sess, TRIG1, true, "player 2's trigger");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, trigger);
    expectHeld(sess, TRIG1, false, "and its release");

    /* the grip: a press on the ring tilts it, a drag moves on from one direction to the next */
    const QPoint stick = centreOf("Stick", 0);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, stick + QPoint(44, 0));
    expectHeld(sess, RIGHT0, true, "a press right of the knob tilts right");
    QTest::mouseMove(console, stick + QPoint(0, -44));
    expectHeld(sess, UP0, true, "dragging round moves to up");
    expectHeld(sess, RIGHT0, false, "and lets go of right");
    QTest::mouseMove(console, stick + QPoint(31, -31));
    expectHeld(sess, UP0, true, "and the up-right diagonal holds up");
    expectHeld(sess, RIGHT0, true, "and right");
    win->hide();
    expectHeld(sess, UP0, false, "hiding the window lets go of what the mouse held");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, stick);
    win->show();
}

void knobTests(astrosession *sess, QWidget *win)
{
    QWidget *console = findConsole(win);
    if (!console) return;
    const QPoint dial = centreOf("Knob", 2);
    astrosession_knob_set(sess, 2, 128);
    QTest::mousePress(console, Qt::LeftButton, Qt::NoModifier, dial + QPoint(0, -20));
    /* the test's integer pixel is a degree or two off the dial's true centre */
    check(qAbs(astrosession_knob_get(sess, 2) - 128) <= 4, "a press straight up centres the knob");
    QTest::mouseMove(console, dial + QPoint(20, 20));
    check(astrosession_knob_get(sess, 2) >= 251, "dragging to the lower right turns it full right");
    QTest::mouseMove(console, dial + QPoint(-20, 20));
    check(astrosession_knob_get(sess, 2) <= 4, "and to the lower left full left");
    QTest::mouseRelease(console, Qt::LeftButton, Qt::NoModifier, dial + QPoint(-20, 20));
    astrosession_knob_set(sess, 2, 128);
    QWheelEvent wheel(QPointF(dial), QPointF(console->mapToGlobal(dial)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(console, &wheel);
    check(astrosession_knob_get(sess, 2) == 136, "a wheel notch nudges the knob");
    astrosession_knob_set(sess, 2, 128);
}

void mapTests(astrosession *sess, QWidget *win, SDL_Joystick *vjoy)
{
    QPushButton *mapButton = findButton(win, "Map");
    QWidget *console = findConsole(win);
    check(mapButton && console, "the Map button exists");
    if (!mapButton || !console) return;

    mapButton->click();
    check(mapButton->text() == QLatin1String("Done"), "Map arms");
    QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("7", -1));
    QTest::keyClick(win, Qt::Key_Q);
    check(bindings_target_keysym(KEY7) == 'q', "the next key is bound to the picked part");
    expectHeld(sess, KEY7, false, "a key taken by Map is not typed into the console");

    /* the knob: its left half takes a key, its hub only an axis */
    QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("Knob", 0) + QPoint(-18, 8));
    QTest::keyClick(win, Qt::Key_Z);
    check(bindings_target_keysym(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB_DEC)) == 'z', "the knob's left half takes a key");
    QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("Knob", 0));
    QTest::keyClick(win, Qt::Key_X);
    check(bindings_target_keysym(ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB)) == 0, "the knob's hub takes no key");

    QTest::keyClick(win, Qt::Key_F9);
    check(!win->isVisible(), "F9 hides the window");
    win->show();

    mapButton = findButton(win, "Map");
    check(mapButton != nullptr, "the window comes back");
    if (!mapButton) return;
    mapButton->click();
    QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("7", -1));
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        for (int tries = 0; tries < 250 && bindings_target_pad(KEY7) != ASTRO_PAD_BTN_RIGHT_STICK; tries++)
            QTest::qWait(20);
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        check(bindings_target_pad(KEY7) == ASTRO_PAD_BTN_RIGHT_STICK, "the next gamepad button is bound to the picked part");

        /* the knob's hub waits for an axis */
        QTest::mouseClick(console, Qt::LeftButton, Qt::NoModifier, centreOf("Knob", 0));
        SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTY, 30000);
        const int knobTarget = ASTRO_TARGET_PLAYER(0, ASTRO_ACT_KNOB);
        for (int tries = 0; tries < 250 && bindings_target_pad(knobTarget) != ASTRO_PAD_AXIS_BASE + SDL_GAMEPAD_AXIS_RIGHTY;
             tries++)
            QTest::qWait(20);
        SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTY, 0);
        check(bindings_target_pad(knobTarget) == ASTRO_PAD_AXIS_BASE + SDL_GAMEPAD_AXIS_RIGHTY,
              "the knob's hub takes a deflected axis");
    }
    QTest::keyClick(win, Qt::Key_Escape);
    QTest::keyClick(win, Qt::Key_Escape);
    check(mapButton->text() == QLatin1String("Map"), "Escape twice leaves Map");

    bindings_reset_defaults(sess);
    check(bindings_target_keysym(KEY7) == '7', "Defaults put the key back");
}

/* the Bally BASIC overlay check box, and typing through it */
void overlayTests(astrosession *sess, QWidget *win)
{
    auto *box = win->findChild<QCheckBox *>(QStringLiteral("overlay"));
    check(box != nullptr, "the overlay check box exists");
    if (!box) return;
    check(!box->isChecked() && !astrosession_overlay_get(sess), "the overlay starts off");
    box->click();
    check(astrosession_overlay_get(sess) == 1, "the check box turns the overlay on, remembered");
    QTest::keyClick(win, Qt::Key_H, Qt::NoModifier);
    check(astrosession_typing_pending(sess) > 0, "a typed H is typed through the overlay");
    expectHeld(sess, ASTRO_TARGET_KEYPAD + ASTROSESSION_KEY_CH, false, "instead of pressing its bound key");
    for (int tries = 0; tries < 250 && astrosession_typing_pending(sess); tries++)
        QTest::qWait(20);
    astrosession_overlay_set(sess, 0);
    for (int tries = 0; tries < 250 && box->isChecked(); tries++)
        QTest::qWait(20);
    check(!box->isChecked(), "the check box follows the setting (an auto-on, or Preferences)");
}

void padRowTests(astrosession *sess, QWidget *win, const char *padName)
{
    QComboBox *box = waitForPadRow(win);
    check(box != nullptr, "a pad's row appears with its five choices");
    if (!box) return;
    int slot = -1;
    for (int idx = 0; idx < astrosession_gamepad_count(sess); idx++) {
        char got[64];
        astrosession_gamepad_name(sess, idx, got, sizeof got);
        if (std::strcmp(got, padName) == 0) slot = idx;
    }
    check(slot >= 0, "the pad is in the session");
    if (slot < 0) return;
    check(box->currentIndex() == 0, "it starts on Automatic");
    box->setCurrentIndex(3);
    check(astrosession_gamepad_assignment(sess, slot) == 2, "choosing Player 3 assigns it");
    check(astrosession_gamepad_effective_port(sess, slot) == 2, "and it plays player 3");
    box->setCurrentIndex(0);
    check(astrosession_gamepad_assignment(sess, slot) == -1, "Automatic clears the assignment");
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir config, data;
    if (!config.isValid() || !data.isValid()) return 1;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    static const char padName[] = "astrotest kde pad";
    desc.name = padName;
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        return 77;
    }
    SDL_Joystick *vjoy = SDL_OpenJoystick(vid);

    const QByteArray cfg = config.path().toLocal8Bit(), dat = data.path().toLocal8Bit();
    astrosession_paths paths;
    std::memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg.constData();
    paths.data_dir = dat.constData();
    astrosession *sess = astrosession_new(&paths);
    /* the pad layer, which the session starts with a machine: here it runs alone */
    if (!sess || astro_gamepad_start(sess) != 0) {
        std::printf("SKIP: the gamepad layer would not start\n");
        return 77;
    }

    QWidget host;
    check(!astroControllersVisible(), "hidden at start");
    astroControllersToggle(&host, sess);
    check(astroControllersVisible(), "F9 / the menu shows it");
    QWidget *win = findWindow();
    check(win != nullptr, "the window exists");
    if (win) {
        check(win->windowTitle() == QLatin1String("Controllers"), "titled Controllers");
        check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
              "fixed size: minimum == maximum == size");
        keyTests(sess, win);
        mouseTests(sess, win);
        knobTests(sess, win);
        win->show();
        mapTests(sess, win, vjoy);
        overlayTests(sess, win);
        padRowTests(sess, win, padName);
        astroControllersToggle(&host, sess);
        check(!astroControllersVisible(), "toggling again hides it");
    }

    astro_gamepad_stop();
    if (vjoy) SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    astrosession_free(sess);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
