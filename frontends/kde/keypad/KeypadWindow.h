/*
 * KeypadWindow -- the Controllers window: the 24-key console keypad, the two
 * system buttons and all four hand controllers (stick, trigger, knob dial),
 * with Map mode. Qt6 Widgets. See the GNOME keypad_window.c for the shared
 * design: raw press/release controls, a 50 ms poll that lights what the
 * machine sees held, a fixed-size floating window, keys through the same path
 * as the main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ASTRO_KDE_KEYPADWINDOW_H
#define ASTRO_KDE_KEYPADWINDOW_H

#include <QWidget>
#include <QVector>

extern "C" {
#include "astrosession.h"
#include "bindings.h"
}

class QLabel;
class QPushButton;
class QTimer;
class KnobDial;

class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    explicit KeypadWindow(astrosession *session, QWidget *parent = nullptr);

    /* consume a keystroke as a Map-mode binding; true if consumed */
    bool mapForwardKey(uint32_t keysym);
    bool mapping() const { return m_mapState != -2; }
    /* testing: the control for a target, the picked target, the hint */
    QPushButton *buttonFor(int target) const;
    KnobDial *dialFor(int player) const { return m_dials[player]; }
    int mapTarget() const { return m_mapState; }
    QString hint() const;
    void setMapState(int state);
    void pickTarget(int target);

protected:
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void changeEvent(QEvent *) override;

private:
    struct Control { QPushButton *button; int target; QString face; bool lit; };

    QPushButton *addControl(const QString &face, int target, int w, int h);
    void setLit(Control &c, bool on);
    void refreshLabels();
    void tick();
    void capturePoll();
    void press(int target, bool down);
    QWidget *buildKeypad();
    QWidget *buildPlayer(int player);

    astrosession *m_session;
    QVector<Control> m_controls;
    KnobDial *m_dials[ASTROSESSION_PLAYERS] = {};
    QLabel *m_captions[ASTROSESSION_PLAYERS] = {};
    QPushButton *m_mapBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_pads = nullptr;
    QTimer *m_timer = nullptr;
    int m_mapState = -2;   /* -2 idle, -1 armed, >= 0 a picked target */
    friend class KnobDial;
};

/* The knob: 270 degrees, drawn from astrosession_knob_get; a drag sets it. */
class KnobDial : public QWidget {
    Q_OBJECT
public:
    KnobDial(astrosession *session, int player, KeypadWindow *owner);
    int target() const { return ASTRO_TARGET_PLAYER(m_player, ASTRO_ACT_KNOB); }
    void setPicked(bool on) { m_picked = on; update(); }
    /* drag to a point inside the widget (testing and the mouse handlers) */
    void dragTo(const QPointF &p);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
private:
    astrosession *m_session;
    int m_player;
    KeypadWindow *m_owner;
    bool m_picked = false;
};

#endif
