/*
 * KeypadWindow -- see KeypadWindow.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"
#include "KeyForward.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <functional>
#include <utility>

extern "C" {
#include "bindings.h"
}

static const char *k_labels[ASTROSESSION_KEY_COUNT] = {
    "C", "↑", "↓", "%",
    "MR", "MS", "CH", "÷",
    "7", "8", "9", "×",
    "4", "5", "6", "−",
    "1", "2", "3", "+",
    "CE", "0", ".", "=",
};

/* A pressable keypad button that emits press/release, not click. */
class PadButton : public QPushButton {
public:
    PadButton(const QString &t, std::function<void(bool)> cb)
        : QPushButton(t), m_cb(std::move(cb)) {}
protected:
    void mousePressEvent(QMouseEvent *e) override { m_cb(true); QPushButton::mousePressEvent(e); }
    void mouseReleaseEvent(QMouseEvent *e) override { m_cb(false); QPushButton::mouseReleaseEvent(e); }
private:
    std::function<void(bool)> m_cb;
};

KeypadWindow::KeypadWindow(astrosession *session, QWidget *parent)
    : QWidget(parent, Qt::Window), m_session(session)
{
    setWindowTitle("Keypad");
    auto *outer = new QVBoxLayout(this);

    auto *grid = new QGridLayout;
    m_keyButtons.resize(ASTROSESSION_KEY_COUNT);
    for (int key = 0; key < ASTROSESSION_KEY_COUNT; ++key) {
        int row = key / 4, col = key % 4;
        auto *btn = new PadButton(QString::fromUtf8(k_labels[key]),
            [this, key](bool down) {
                if (m_mapState == PickTarget) { if (down) pickTarget(key); return; }
                if (m_mapState == WaitInput) return;
                astrosession_keypad_set(m_session, (astrosession_key)key, down);
            });
        btn->setMinimumSize(56, 44);
        if (col == 3)
            btn->setStyleSheet("background:#e8c14a; color:#3a2c00; font-weight:bold;");
        m_keyButtons[key] = btn;
        grid->addWidget(btn, row, col);
    }
    outer->addLayout(grid);

    auto *sysRow = new QHBoxLayout;
    const char *sysNames[ASTROSESSION_SYSACT_COUNT] = { "Reset Game", "Reset to CONFIG" };
    for (int a = 0; a < ASTROSESSION_SYSACT_COUNT; ++a) {
        auto *btn = new QPushButton(sysNames[a]);
        connect(btn, &QPushButton::clicked, this, [this, a] {
            int target = ASTRO_TARGET_SYSACT + a;
            if (m_mapState == PickTarget) { pickTarget(target); return; }
            if (m_mapState == WaitInput) return;
            astrosession_sysaction_fire(m_session, (astrosession_sysaction)a);
        });
        m_sysButtons[a] = btn;
        sysRow->addWidget(btn);
    }
    outer->addLayout(sysRow);

    /* Hand controller row: Up/Down/Left/Right/Trigger, level-held like the
     * keypad buttons (not fire-once like the sysact row above) since these
     * drive astrosession_handle_set's live bitmask. Doubles as a way to test
     * a binding on-screen without a physical gamepad. */
    auto *handleRow = new QHBoxLayout;
    static const char *handleNames[] = { "\xE2\x86\x91", "\xE2\x86\x93", "\xE2\x86\x90", "\xE2\x86\x92", "Trigger" };
    /* order matches ASTRO_TARGET_HANDLE's layout (see bindings.c's k_handle_bit) */
    static const uint8_t handleBits[] = {
        ASTROSESSION_HANDLE_UP, ASTROSESSION_HANDLE_DOWN,
        ASTROSESSION_HANDLE_LEFT, ASTROSESSION_HANDLE_RIGHT,
        ASTROSESSION_HANDLE_TRIGGER,
    };
    for (int h = 0; h < ASTRO_HANDLE_ACTION_COUNT; ++h) {
        int target = ASTRO_TARGET_HANDLE + h;
        uint8_t bit = handleBits[h];
        auto *btn = new PadButton(QString::fromUtf8(handleNames[h]),
            [this, target, bit](bool down) {
                if (m_mapState == PickTarget) { if (down) pickTarget(target); return; }
                if (m_mapState == WaitInput) return;
                if (down) m_handleMask |= bit;
                else m_handleMask &= (uint8_t)~bit;
                astrosession_handle_set(m_session, 0, m_handleMask);
            });
        m_handleButtons[h] = btn;
        handleRow->addWidget(btn);
    }
    outer->addLayout(handleRow);

    auto *mapRow = new QHBoxLayout;
    m_mapBtn = new QPushButton("Map");
    connect(m_mapBtn, &QPushButton::clicked, this, [this] {
        if (m_mapState == Idle) {
            m_mapState = PickTarget;
            m_mapBtn->setText("Cancel");
            m_status->setText("Click a key to remap, then press its new key");
        } else {
            if (m_mapTarget >= 0) setHighlight(m_mapTarget, false);
            m_mapTarget = -1; m_mapState = Idle;
            m_mapBtn->setText("Map"); m_status->clear();
        }
    });
    auto *resetBtn = new QPushButton("Reset Bindings");
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        bindings_reset_defaults(m_session);
        m_status->setText("Bindings reset to defaults");
    });
    m_status = new QLabel;
    mapRow->addWidget(m_mapBtn);
    mapRow->addWidget(resetBtn);
    mapRow->addWidget(m_status, 1);
    outer->addLayout(mapRow);
}

QPushButton *KeypadWindow::buttonForTarget(int target)
{
    if (target >= 0 && target < ASTROSESSION_KEY_COUNT)
        return m_keyButtons[target];
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_HANDLE)
        return m_sysButtons[target - ASTRO_TARGET_SYSACT];
    if (target >= ASTRO_TARGET_HANDLE && target < ASTRO_TARGET_COUNT)
        return m_handleButtons[target - ASTRO_TARGET_HANDLE];
    return nullptr;
}

void KeypadWindow::setHighlight(int target, bool on)
{
    QPushButton *b = buttonForTarget(target);
    if (!b) return;
    /* keep the gold column's tint; add a border when highlighted */
    QString base = (target < ASTROSESSION_KEY_COUNT && target % 4 == 3)
                       ? "background:#e8c14a; color:#3a2c00; font-weight:bold;"
                       : "";
    b->setStyleSheet(on ? base + " border:2px solid #8b0000;" : base);
}

void KeypadWindow::pickTarget(int target)
{
    m_mapTarget = target;
    m_mapState = WaitInput;
    setHighlight(target, true);
    m_status->setText(QString("Press a key to bind to %1 (Esc cancels)")
                          .arg(bindings_target_label(target)));
}

bool KeypadWindow::mapForwardKey(int keysym)
{
    if (m_mapState != WaitInput || m_mapTarget < 0)
        return false;
    if (keysym == ASTROSESSION_KEYSYM_ESCAPE) {
        setHighlight(m_mapTarget, false);
        m_mapTarget = -1; m_mapState = Idle;
        m_mapBtn->setText("Map"); m_status->setText("Cancelled");
        return true;
    }
    int stole = -1;
    bindings_set_keysym(m_session, m_mapTarget, keysym, &stole);
    QString msg = QString("%1 mapped").arg(bindings_target_label(m_mapTarget));
    if (stole >= 0)
        msg += QString(" (taken from %1)").arg(bindings_target_label(stole));
    setHighlight(m_mapTarget, false);
    m_mapTarget = -1; m_mapState = Idle;
    m_mapBtn->setText("Map"); m_status->setText(msg);
    return true;
}

void KeypadWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_F9) { hide(); return; }
    int keysym = astro_keysym_from_qt(e);
    if (mapForwardKey(keysym)) return;
    astro_mapping m = bindings_resolve_keysym(keysym);
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(m_session, (astrosession_key)m.value, 1);
    else if (m.kind == ASTRO_MAP_SYSACT)
        astrosession_sysaction_fire(m_session, (astrosession_sysaction)m.value);
    else if (m.kind == ASTRO_MAP_HANDLE) {
        m_handleMask |= (uint8_t)m.value;
        astrosession_handle_set(m_session, 0, m_handleMask);
    }
}

void KeypadWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (m_mapState != Idle) return;
    astro_mapping m = bindings_resolve_keysym(astro_keysym_from_qt(e));
    if (m.kind == ASTRO_MAP_KEY)
        astrosession_keypad_set(m_session, (astrosession_key)m.value, 0);
    else if (m.kind == ASTRO_MAP_HANDLE) {
        m_handleMask &= (uint8_t)~m.value;
        astrosession_handle_set(m_session, 0, m_handleMask);
    }
}
