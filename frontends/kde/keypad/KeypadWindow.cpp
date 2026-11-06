/*
 * KeypadWindow -- see KeypadWindow.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"
#include "../KeyForward.h"

#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <utility>

namespace {

const char *const kFaces[ASTROSESSION_KEY_COUNT] = {
    "C", "↑", "↓", "%",
    "MR", "MS", "CH", "÷",
    "7", "8", "9", "×",
    "4", "5", "6", "−",
    "1", "2", "3", "+",
    "CE", "0", ".", "=",
};
const char *const kGold = "background:#e8c14a; color:#3a2c00; font-weight:bold;";
const char *const kLit = "background:#b5342c; color:#ffffff; font-weight:bold;";

bool isKnobTarget(int t)
{
    return t >= ASTRO_TARGET_PLAYERS &&
           (t - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

/* A pressable button that emits press/release, not click: a control is HELD
 * for as long as the mouse is down. Releasing off the button (or losing the
 * grab) still releases. */
class PadButton : public QPushButton {
public:
    PadButton(const QString &t, std::function<void(bool)> cb)
        : QPushButton(t), m_cb(std::move(cb))
    {
        setFocusPolicy(Qt::NoFocus);
    }
protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_down = true;
        m_cb(true);
        QPushButton::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_down) { m_down = false; m_cb(false); }
        QPushButton::mouseReleaseEvent(e);
    }
private:
    std::function<void(bool)> m_cb;
    bool m_down = false;
};

} // namespace

/* ---- the dial ------------------------------------------------------------- */

KnobDial::KnobDial(astrosession *session, int player, KeypadWindow *owner)
    : QWidget(owner), m_session(session), m_player(player), m_owner(owner)
{
    setFixedSize(92, 92);
    setFocusPolicy(Qt::NoFocus);
}

void KnobDial::dragTo(const QPointF &p)
{
    const double r = std::min(width(), height()) / 2.0;
    const int v = astrosession_knob_from_point(p.x() - width() / 2.0,
                                               p.y() - height() / 2.0, r);
    if (v >= 0)
        astrosession_knob_set(m_session, m_player, (uint8_t)v);
    update();
}

void KnobDial::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    if (m_owner->m_mapState == -1) { m_owner->pickTarget(target()); return; }
    if (m_owner->m_mapState != -2) return;
    dragTo(e->position());
}

void KnobDial::mouseMoveEvent(QMouseEvent *e)
{
    if (m_owner->m_mapState != -2 || !(e->buttons() & Qt::LeftButton)) return;
    dragTo(e->position());
}

void KnobDial::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, height() / 2.0);
    const double r = std::min(width(), height()) / 2.0 - 4;
    const QColor fg = palette().color(QPalette::WindowText);
    QPen pen(fg, 2);
    p.setPen(pen);
    /* the 270 degree sweep: Qt angles are 1/16 degree, counter-clockwise from 3 o'clock */
    p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), -45 * 16, 270 * 16);
    pen.setWidth(1);
    p.setPen(pen);
    p.drawEllipse(c, r * 0.62, r * 0.62);
    const double a = astrosession_knob_angle(astrosession_knob_get(m_session, m_player)) * M_PI / 180.0;
    p.setPen(QPen(m_picked ? QColor("#b5342c") : fg, 3, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(c.x() + std::sin(a) * r * 0.25, c.y() - std::cos(a) * r * 0.25),
               QPointF(c.x() + std::sin(a) * r * 0.95, c.y() - std::cos(a) * r * 0.95));
}

/* ---- the window ----------------------------------------------------------- */

KeypadWindow::KeypadWindow(astrosession *session, QWidget *parent)
    : QWidget(parent, Qt::Dialog), m_session(session)
{
    setWindowTitle("Controllers");
    /* A Qt::Dialog with min == max size: a tiling compositor floats it. */
    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto *top = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    left->addWidget(buildKeypad());

    m_pads = new QLabel;
    m_pads->setWordWrap(true);
    m_pads->setEnabled(false);
    m_pads->setFixedHeight(40);
    m_pads->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    left->addStretch(1);
    left->addWidget(m_pads);

    auto *mapRow = new QHBoxLayout;
    m_mapBtn = new QPushButton("Map");
    m_mapBtn->setCheckable(true);
    m_mapBtn->setFocusPolicy(Qt::NoFocus);
    m_mapBtn->setStyleSheet("QPushButton:checked { background:#b5342c; color:white; }");
    m_clearBtn = new QPushButton("Clear");
    m_clearBtn->setFocusPolicy(Qt::NoFocus);
    m_clearBtn->setEnabled(false);
    auto *defaults = new QPushButton("Defaults");
    defaults->setFocusPolicy(Qt::NoFocus);
    m_hint = new QLabel;
    m_hint->setWordWrap(true);
    m_hint->setFixedHeight(40);
    m_hint->setMinimumWidth(180);
    m_hint->setEnabled(false);
    mapRow->addWidget(m_mapBtn);
    mapRow->addWidget(m_clearBtn);
    mapRow->addWidget(defaults);
    mapRow->addWidget(m_hint, 1);
    left->addLayout(mapRow);
    top->addLayout(left);

    auto *grid = new QGridLayout;
    for (int p = 0; p < ASTROSESSION_PLAYERS; ++p)
        grid->addWidget(buildPlayer(p), p / 2, p % 2);
    top->addLayout(grid);
    root->addLayout(top);

    connect(m_mapBtn, &QPushButton::clicked, this, [this](bool on) {
        setMapState(on ? -1 : -2);
    });
    connect(m_clearBtn, &QPushButton::clicked, this, [this] {
        const int target = m_mapState;
        if (target < 0) return;
        bindings_clear(m_session, target);
        setMapState(-1);
        m_hint->setText(QString("%1 cleared").arg(bindings_target_label(target)));
    });
    connect(defaults, &QPushButton::clicked, this, [this] {
        bindings_reset_defaults(m_session);
        refreshLabels();
        m_hint->setText("Bindings reset to the defaults");
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
}

QPushButton *KeypadWindow::addControl(const QString &face, int target, int w, int h)
{
    auto *btn = new PadButton(face, [this, target](bool down) { press(target, down); });
    btn->setFixedSize(w, h);
    const bool gold = target < ASTROSESSION_KEY_COUNT && target % 4 == 3;
    if (gold) btn->setStyleSheet(kGold);
    m_controls.push_back({ btn, target, face, false });
    return btn;
}

QPushButton *KeypadWindow::buttonFor(int target) const
{
    for (const Control &c : m_controls)
        if (c.target == target) return c.button;
    return nullptr;
}

QString KeypadWindow::hint() const { return m_hint->text(); }

QWidget *KeypadWindow::buildKeypad()
{
    auto *frame = new QFrame;
    frame->setFrameShape(QFrame::StyledPanel);
    auto *box = new QVBoxLayout(frame);
    auto *title = new QLabel("Console keypad");
    title->setAlignment(Qt::AlignHCenter);
    QFont f = title->font(); f.setBold(true); title->setFont(f);
    box->addWidget(title);
    auto *grid = new QGridLayout;
    for (int key = 0; key < ASTROSESSION_KEY_COUNT; ++key)
        grid->addWidget(addControl(QString::fromUtf8(kFaces[key]), ASTRO_TARGET_KEYPAD + key, 62, 40),
                        key / 4, key % 4);
    box->addLayout(grid);
    auto *sys = new QHBoxLayout;
    sys->addWidget(addControl("Reset Game",
                              ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_GAME, 119, 44));
    sys->addWidget(addControl("Reset to CONFIG",
                              ASTRO_TARGET_SYSACT + ASTROSESSION_SYSACT_RESET_CONFIG, 119, 44));
    box->addLayout(sys);
    return frame;
}

QWidget *KeypadWindow::buildPlayer(int player)
{
    auto *frame = new QFrame;
    frame->setFrameShape(QFrame::StyledPanel);
    auto *box = new QVBoxLayout(frame);
    auto *title = new QLabel(QString("Player %1").arg(player + 1));
    title->setAlignment(Qt::AlignHCenter);
    QFont f = title->font(); f.setBold(true); title->setFont(f);
    box->addWidget(title);

    auto stick = [&](const char *face, int act) {
        return addControl(QString::fromUtf8(face), ASTRO_TARGET_PLAYER(player, act), 52, 34);
    };
    auto *grid = new QGridLayout;
    grid->addWidget(stick("▲", ASTRO_ACT_UP), 0, 1);
    grid->addWidget(stick("◀", ASTRO_ACT_LEFT), 1, 0);
    grid->addWidget(stick("▶", ASTRO_ACT_RIGHT), 1, 2);
    grid->addWidget(stick("▼", ASTRO_ACT_DOWN), 2, 1);
    grid->addWidget(addControl("Trigger", ASTRO_TARGET_PLAYER(player, ASTRO_ACT_TRIGGER), 164, 40),
                    3, 0, 1, 3);
    box->addLayout(grid);

    auto *row = new QHBoxLayout;
    row->addWidget(addControl("◀", ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_DEC), 40, 34));
    auto *col = new QVBoxLayout;
    m_dials[player] = new KnobDial(m_session, player, this);
    m_captions[player] = new QLabel("Knob");
    m_captions[player]->setAlignment(Qt::AlignHCenter);
    m_captions[player]->setFixedWidth(92);
    col->addWidget(m_dials[player]);
    col->addWidget(m_captions[player]);
    row->addLayout(col);
    row->addWidget(addControl("▶", ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_INC), 40, 34));
    box->addLayout(row);
    return frame;
}

void KeypadWindow::press(int target, bool down)
{
    if (m_mapState == -1) { if (down) pickTarget(target); return; }
    if (m_mapState != -2) return;
    if (target >= ASTRO_TARGET_SYSACT && target < ASTRO_TARGET_PLAYERS) {
        /* system actions fire on release, like a real button */
        if (!down)
            astrosession_sysaction_fire(m_session,
                                        (astrosession_sysaction)(target - ASTRO_TARGET_SYSACT));
        return;
    }
    astrosession_press(m_session, target, down);
}

void KeypadWindow::setLit(Control &c, bool on)
{
    if (c.lit == on) return;
    c.lit = on;
    const bool gold = c.target < ASTROSESSION_KEY_COUNT && c.target % 4 == 3;
    c.button->setStyleSheet(on ? kLit : gold ? kGold : "");
}

void KeypadWindow::setMapState(int state)
{
    m_mapState = state;
    m_mapBtn->setChecked(state != -2);
    m_clearBtn->setEnabled(state >= 0);
    if (state >= 0) {
        m_hint->setText(QString(isKnobTarget(state)
                                    ? "Move a gamepad stick or trigger for %1"
                                    : "Press a key or gamepad button for %1")
                            .arg(bindings_target_label(state)));
        astrosession_gamepad_capture_begin(m_session);
    } else {
        astrosession_gamepad_capture_cancel(m_session);
        m_hint->setText(state == -1 ? "Click a control to remap" : "");
    }
    refreshLabels();
}

void KeypadWindow::pickTarget(int target) { setMapState(target); }

void KeypadWindow::refreshLabels()
{
    for (Control &c : m_controls) {
        if (m_mapState != -2) {
            char key[48], pad[48];
            astrosession_keysym_name(bindings_target_keysym(c.target), key, sizeof key);
            astrosession_pad_input_name(bindings_target_pad(c.target), pad, sizeof pad);
            QString text = key[0] ? QString::fromUtf8(key) : QString::fromUtf8("—");
            if (pad[0]) text += "\n" + QString::fromUtf8(pad);
            c.button->setText(text);
            c.button->setToolTip(QString::fromUtf8(bindings_target_label(c.target)));
            setLit(c, c.target == m_mapState);
        } else {
            c.button->setText(c.face);
            c.button->setToolTip(QString());
            setLit(c, false);
        }
    }
    for (int p = 0; p < ASTROSESSION_PLAYERS; ++p) {
        char pad[48] = "";
        if (m_mapState != -2)
            astrosession_pad_input_name(bindings_target_pad(m_dials[p]->target()), pad, sizeof pad);
        m_captions[p]->setText(m_mapState != -2
                                   ? (pad[0] ? QString::fromUtf8(pad) : QString::fromUtf8("—"))
                                   : "Knob");
        m_dials[p]->setPicked(m_mapState == m_dials[p]->target());
    }
}

void KeypadWindow::capturePoll()
{
    int id;
    const int target = m_mapState;
    if (!astrosession_gamepad_capture_poll(m_session, &id))
        return;
    if (isKnobTarget(target) && id < ASTRO_PAD_AXIS_BASE) {
        astrosession_gamepad_capture_begin(m_session);   /* a button cannot turn a knob */
        m_hint->setText("The knob follows an axis: move a stick or trigger");
        return;
    }
    int from = -1;
    bindings_set_pad(m_session, target, id, &from);
    char name[48];
    astrosession_pad_input_name(id, name, sizeof name);
    QString msg = QString("%1 = pad %2").arg(bindings_target_label(target), name);
    if (from >= 0) msg += QString(" (was %1)").arg(bindings_target_label(from));
    setMapState(-1);   /* stay armed: remapping several in a row is normal */
    m_hint->setText(msg);
}

void KeypadWindow::tick()
{
    QString pads;
    const int n = astrosession_gamepad_count(m_session);
    for (int i = 0; i < n; ++i) {
        char name[64];
        astrosession_gamepad_name(m_session, i, name, sizeof name);
        pads += QString("%1Player %2: %3").arg(i ? "   " : "").arg(i + 1).arg(QString::fromUtf8(name));
    }
    if (n == 0) pads = "No gamepad connected";
    if (m_pads->text() != pads) m_pads->setText(pads);
    for (int p = 0; p < ASTROSESSION_PLAYERS; ++p)
        m_dials[p]->update();
    if (m_mapState >= 0) { capturePoll(); return; }
    if (m_mapState != -2) return;
    for (Control &c : m_controls)
        setLit(c, astrosession_target_held(m_session, c.target) != 0);
}

bool KeypadWindow::mapForwardKey(uint32_t keysym)
{
    if (m_mapState == -2)
        return false;
    if (m_mapState == -1)
        return true;   /* swallow keys while choosing a target */
    const int target = m_mapState;
    if (isKnobTarget(target)) {
        m_hint->setText("The knob follows a gamepad axis: move a stick or trigger");
        return true;
    }
    int from = -1;
    bindings_set_keysym(m_session, target, keysym, &from);
    char name[48];
    astrosession_keysym_name(keysym, name, sizeof name);
    QString msg = QString("%1 = %2").arg(bindings_target_label(target), name);
    if (from >= 0) msg += QString(" (was %1)").arg(bindings_target_label(from));
    setMapState(-1);   /* stay armed */
    m_hint->setText(msg);
    return true;
}

void KeypadWindow::keyPressEvent(QKeyEvent *e)
{
    /* Map capture and the bindings first, exactly like the main window, so
     * playing works whichever window has focus; then F9. */
    if (e->isAutoRepeat()) { e->accept(); return; }
    if (astroForwardKey(m_session, this, e, true)) { e->accept(); return; }
    if (e->key() == Qt::Key_F9) { hide(); return; }
    QWidget::keyPressEvent(e);
}

void KeypadWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) { e->accept(); return; }
    astroForwardKey(m_session, this, e, false);
    e->accept();
}

void KeypadWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    m_timer->start();
    tick();
}

/* Hiding the window (close, F9, menu) leaves Map and stops the poll. */
void KeypadWindow::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
    if (m_mapState != -2) setMapState(-2);
}

/* Keys held into a focus change must not stay down. */
void KeypadWindow::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::ActivationChange && !isActiveWindow())
        astrosession_release_all(m_session);
}
