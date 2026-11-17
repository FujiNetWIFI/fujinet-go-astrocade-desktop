/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The Bally Professional Arcade as a player sees it: the console's top, a black
 * panel in a walnut case, with the 24-key keypad on its champagne plate
 * (legends printed above the keys, a gold column down the right) and below it
 * the four pistol-grip hand controllers side by side (Player 1 on the left):
 * a black grip with a fluted orange-brown knob on top -- the paddle, a dial
 * you drag round -- ringed by eight direction wedges (the whole grip tilts as
 * the stick), a trigger at the front and a gold Bally badge. RESET and Reset
 * to CONFIG close the case. With the Bally BASIC overlay on, the plate wears
 * the printed card: a black legend bar above each row of key windows, with the
 * three shifted characters (green, red, blue) and the gold WORD of each key.
 * Everything is painted from the session's layout (astrosession_controls_layout,
 * in units of a keypad key's width), so every frontend shows the same parts in
 * the same places; the colours are this file's, read off photographs of the
 * real hardware (DESIGN-astrocade.md).
 *
 * Gestures act on mouse press AND release: the console samples its controls
 * once a frame, so a press present only for the instant of a click would fall
 * between frames. A key or the trigger is held while the mouse is down on it
 * (dragging off lets it go); a press on the grip's ring tilts it, and a drag
 * moves on from one direction to the next; a press on the knob turns it to the
 * point and a drag turns it on, and the wheel nudges it. One contact at a
 * time: chords come from the keyboard or a pad. Hiding the window and losing
 * the focus let go of everything the mouse holds.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent, so the window doubles as an input tester for a new pad.
 * Keys typed with it active go to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts, and gamepad names are elided, so
 * neither Map mode nor a long name resizes the window. A singleton, hidden
 * rather than destroyed, so a half-done remap survives closing it. The timers
 * run only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a wedge, the trigger,
 * a key, a console button, or the knob: its left half is "knob left", its
 * right half "knob right", its hub the pad axis that turns it), then press a
 * host key or a gamepad button (an axis, for the hub). Rebinding steals the
 * key from whatever held it (named in the hint). Escape cancels; while Map is
 * armed nothing is typed into the console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

extern "C" {
#include "bindings.h"
}

#include "../Accent.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

using astroctl::kCasePad;
using astroctl::kTop;
using astroctl::kUnit;

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;      /* an unpicked part while Map waits for a key */
constexpr qreal kLegendH = 0.5;       /* the legend zone above a key, in layout units */
constexpr int kKnobStep = 8;          /* one wheel notch, of the knob's 0-255 */

/* The console (photo-read or inferred, see DESIGN-astrocade.md). */
const QColor kWalnutTop(0x9A, 0x66, 0x3A);
const QColor kWalnutBottom(0x74, 0x48, 0x26);
const QColor kWalnutEdge(0x3E, 0x26, 0x14);
const QColor kPinstripe(0xB0, 0x8A, 0x3C);
const QColor kPanelBlack(0x1E, 0x1E, 0x1D);
const QColor kPanelEdge(0x0A, 0x0A, 0x0A);
const QColor kLabelWhite(0xEF, 0xEF, 0xEA);
const QColor kLabelRed(0xD2, 0x28, 0x2A);
const QColor kGoldInk(0xD4, 0xB0, 0x4C);
const QColor kPlate(0xDC, 0xCB, 0xA8);        /* the keypad's champagne plate */
const QColor kPlateFrame(0xC9, 0xA6, 0x4E);
const QColor kGoldColumn(0xCF, 0xAE, 0x5E);
const QColor kLegendInk(0x4A, 0x3A, 0x30);    /* the printed legends */
const QColor kKeyTop(0x4A, 0x36, 0x36);       /* dark brown-black keys */
const QColor kKeyBase(0x34, 0x26, 0x26);
const QColor kKeyHover(0x5C, 0x3A, 0x3C);
const QColor kKeySide(0x1A, 0x10, 0x10);
const QColor kKeyEdge(0x12, 0x0A, 0x0A);
const QColor kKeyInk(0xE8, 0xDC, 0xCB);
/* the overlay card */
const QColor kCard(0xFB, 0xEF, 0xDD);
const QColor kCardEdge(0xB9, 0xA9, 0x8C);
const QColor kBar(0x1A, 0x1A, 0x1A);
const QColor kWindow(0xF2, 0xF2, 0xF2);
const QColor kWindowHover(0xFF, 0xFF, 0xFF);
const QColor kWindowSide(0xB0, 0xA8, 0x98);
const QColor kWindowInk(0x22, 0x1E, 0x1A);
const QColor kWords(0xE6, 0xBC, 0x4C);
const QColor kWordsOnWhite(0xA8, 0x7C, 0x1C);
const QColor kGreen(0x3F, 0xA5, 0x5A);
const QColor kRed(0xE0, 0x4A, 0x42);
const QColor kBlue(0x28, 0x78, 0xB0);
/* the pistol grips */
const QColor kShellTop(0x2E, 0x2C, 0x2C);
const QColor kShellBottom(0x14, 0x13, 0x13);
const QColor kShellEdge(0x05, 0x05, 0x05);
const QColor kShellGloss(0x77, 0x74, 0x76);
const QColor kKnurl(0x2A, 0x26, 0x26);
const QColor kKnurlDot(0x4C, 0x3D, 0x3D);
const QColor kBadgeGold(0xD4, 0xA9, 0x3A);
const QColor kCaption(0xD9, 0xCB, 0xA3);
const QColor kRing(0x10, 0x0F, 0x0F);
const QColor kWedge(0x2F, 0x2D, 0x2D);
const QColor kWedgeHover(0x44, 0x40, 0x40);
const QColor kWedgeEdge(0x6B, 0x63, 0x61);
const QColor kChevron(0x9C, 0x8F, 0x6E);
const QColor kCollar(0x8A, 0x43, 0x18);
const QColor kCollarEdge(0x3A, 0x1C, 0x0C);
const QColor kKnobLit(0xE6, 0x7A, 0x2C);
const QColor kKnobMid(0xC0, 0x64, 0x2A);
const QColor kKnobShade(0x5F, 0x2E, 0x1D);
const QColor kRib(0x7A, 0x3B, 0x16);
const QColor kTriggerTop(0xB4, 0xB4, 0xBA);
const QColor kTriggerBottom(0x8A, 0x8A, 0x90);
const QColor kTriggerHover(0xC6, 0xC6, 0xCC);
const QColor kSysFace(0x15, 0x14, 0x14);
const QColor kSysHover(0x2A, 0x26, 0x24);
const QColor kSysEdge(0x5B, 0x4A, 0x22);
const QColor kDimInk(0x8A, 0x80, 0x66);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont scaledFont(QFont fnt, qreal scale, bool bold = false)
{
    fnt.setPointSizeF((fnt.pointSizeF() > 0 ? fnt.pointSizeF() : 10) * scale);
    fnt.setBold(bold);
    return fnt;
}

QFont pixelFont(QFont fnt, int pixels, bool bold)
{
    fnt.setPixelSize(pixels);
    fnt.setBold(bold);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const qreal size = fnt.pixelSize() > 0 ? fnt.pixelSize() : fnt.pointSizeF();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5.0)
            break;
        if (fnt.pixelSize() > 0) fnt.setPixelSize(fnt.pixelSize() - 1);
        else fnt.setPointSizeF(fnt.pointSizeF() - 0.5);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* A part's key as the window writes it, or empty for none. */
QString keyName(int target)
{
    char name[48] = "";
    const uint32_t keysym = bindings_target_keysym(target);
    if (keysym) astrosession_keysym_name(keysym, name, sizeof name);
    return local(name);
}

QString padName(int target)
{
    char name[48] = "";
    const int id = bindings_target_pad(target);
    if (id >= 0) astrosession_pad_input_name(id, name, sizeof name);
    return local(name);
}

/* The legend on a part: long key names shortened to fit (tooltips keep the full name). */
QString legendName(QString key)
{
    if (key.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + key.mid(7);
    if (key.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + key.mid(5);
    if (key.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + key.mid(6);
    if (key == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (key == QLatin1String("Escape")) return QStringLiteral("Esc");
    if (key == QLatin1String("Return")) return QStringLiteral("Ret");
    return key;
}

QString bindingText(int target)
{
    const QString key = keyName(target);
    return key.isEmpty() ? QStringLiteral("—") : legendName(key);
}

QString bindingTip(int target)
{
    const QString key = keyName(target), pad = padName(target);
    return QStringLiteral("%1\nKey: %2    Gamepad: %3")
        .arg(local(bindings_target_label(target)), key.isEmpty() ? QStringLiteral("none") : key,
             pad.isEmpty() ? QStringLiteral("none") : pad);
}

QPointF unit(qreal deg)   /* a direction, degrees counter-clockwise from 3 o'clock, as drawn */
{
    return QPointF(std::cos(qDegreesToRadians(deg)), -std::sin(qDegreesToRadians(deg)));
}

bool isKnobAxisTarget(int target)
{
    return target >= ASTRO_TARGET_PLAYERS &&
           (target - ASTRO_TARGET_PLAYERS) % ASTRO_ACT_PER_PLAYER == ASTRO_ACT_KNOB;
}

/* an arrow drawn as a triangle, so no font has to carry the glyph */
void paintArrow(QPainter &pnt, const QPointF &mid, qreal size, bool up, const QColor &ink)
{
    QPolygonF head;
    if (up) head << mid + QPointF(0, -size) << mid + QPointF(-size, size * 0.8) << mid + QPointF(size, size * 0.8);
    else head << mid + QPointF(0, size) << mid + QPointF(-size, -size * 0.8) << mid + QPointF(size, -size * 0.8);
    pnt.setPen(Qt::NoPen);
    pnt.setBrush(ink);
    pnt.drawPolygon(head);
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the keypad, the four hand controllers and the console buttons ---- */

class ConsoleWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    ConsoleWidget(astrosession *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        astrosession_pointer_release(m_session);
        for (unsigned &mask : m_ptrMask) mask = 0;
        m_hold = None;
        m_holdIdx = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }
    void setOverlay(bool on)
    {
        if (on == m_overlay) return;
        m_overlay = on;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse) and where each
     * knob stands; repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        quint32 keys = 0;
        for (int key = 0; key < ASTROSESSION_KEY_COUNT; ++key)
            if (astrosession_target_held(m_session, ASTRO_TARGET_KEYPAD + key)) keys |= 1u << key;
        if (keys != m_keysHeld) {
            m_keysHeld = keys;
            changed = true;
        }
        for (int player = 0; player < ASTROSESSION_PLAYERS; ++player) {
            unsigned held = 0;
            for (int act = 0; act < ASTRO_ACT_KNOB; ++act)
                if (astrosession_target_held(m_session, ASTRO_TARGET_PLAYER(player, act))) held |= 1u << act;
            if (held != m_actHeld[player]) {
                m_actHeld[player] = held;
                changed = true;
            }
            const int knob = astrosession_knob_get(m_session, player);
            if (knob != m_knob[player]) {
                m_knob[player] = knob;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        paintCase(pnt);
        paintPlate(pnt);
        for (int key = 0; key < ASTROSESSION_KEY_COUNT; ++key)
            if (m_key[key] >= 0) paintKey(pnt, m_key[key]);
        for (int player = 0; player < ASTROSESSION_PLAYERS; ++player) paintGrip(pnt, player);
        for (int sys = 0; sys < 2; ++sys)
            if (m_sys[sys] >= 0) paintSys(pnt, m_sys[sys]);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        const int idx = hitAt(pos);
        if (m_map != -2) {
            if (idx >= 0) pick(idx, pos);
            return;
        }
        if (m_hold != None || idx < 0) return;     /* one thing at a time */
        const astro_control &ctl = m_layout[idx];
        switch (ctl.kind) {
        case ASTRO_CTL_KEY:
        case ASTRO_CTL_TRIGGER:
            m_hold = Part;
            m_holdIdx = idx;
            m_partDown = true;
            astrosession_press(m_session, ctl.target, 1);
            break;
        case ASTRO_CTL_STICK:
        case ASTRO_CTL_DIRECTION:
            m_hold = Disc;
            m_dragPlayer = ctl.player;
            setPointer(ctl.player, discMask(ctl.player, pos));
            break;
        case ASTRO_CTL_KNOB:
            m_hold = Dial;
            m_dragPlayer = ctl.player;
            dragDial(ctl.player, pos);
            break;
        case ASTRO_CTL_SYSBUTTON:
            m_hold = Strip;
            m_holdIdx = idx;
            m_partDown = true;
            break;
        default: return;
        }
        update();
    }

    /* Dragging off a key or the trigger lets it go (and back on, holds it
     * again); on the grip's ring it moves from one direction to the next; on
     * the dial it turns the knob. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Disc) {
            setPointer(m_dragPlayer, discMask(m_dragPlayer, pos));
            update();
        } else if (m_hold == Dial) {
            dragDial(m_dragPlayer, pos);
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                astrosession_press(m_session, m_layout[m_holdIdx].target, over ? 1 : 0);
                update();
            }
        } else if (m_hold == Strip) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold == Strip && hitAt(evt->position()) == m_holdIdx) {
            /* fires on release, like a real button: dragging off cancels. The
             * main window's queue runs it, as it does for a key or a pad. */
            astrosession_sysaction_post(m_session,
                                        (astrosession_sysaction)(m_layout[m_holdIdx].target - ASTRO_TARGET_SYSACT));
        }
        if (m_hold != None) releaseAll();
        m_hover = hitAt(evt->position());
        update();
    }

    void wheelEvent(QWheelEvent *evt) override
    {
        if (m_map != -2 || m_hold != None) return;
        const int idx = hitAt(evt->position());
        if (idx < 0 || m_layout[idx].kind != ASTRO_CTL_KNOB) return;
        const int player = m_layout[idx].player;
        const int steps = evt->angleDelta().y() / 120;
        if (!steps) return;
        const int value = qBound(0, astrosession_knob_get(m_session, player) + steps * kKnobStep, 255);
        astrosession_knob_set(m_session, player, (uint8_t)value);
        pollHeld();
        evt->accept();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Disc, Dial, Part, Strip };

    /* one hand controller's parts, by index into the layout */
    struct Grip {
        int caption = -1, stick = -1, knob = -1, trigger = -1;
        int dir[8];
        QPointF centre;
        qreal radius = 0;       /* the stick disc */
        QPointF dialCentre;
        qreal dialRadius = 0;
        QRectF card;
    };

    void load()
    {
        float wide = 0, tall = 0;
        m_layout = astrosession_controls_layout(&m_count, &wide, &tall);
        for (int &slot : m_key) slot = -1;
        for (int &slot : m_sys) slot = -1;
        for (Grip &grip : m_grip)
            for (int &slot : grip.dir) slot = -1;
        int nextDir[ASTROSESSION_PLAYERS] = {};
        for (int idx = 0; idx < m_count; ++idx) {
            const astro_control &ctl = m_layout[idx];
            switch (ctl.kind) {
            case ASTRO_CTL_OVERLAY: m_overlayIdx = idx; continue;
            case ASTRO_CTL_KEY: {
                const int key = ctl.target - ASTRO_TARGET_KEYPAD;
                if (key >= 0 && key < ASTROSESSION_KEY_COUNT) m_key[key] = idx;
                continue;
            }
            case ASTRO_CTL_SYSBUTTON: {
                const int sys = ctl.target - ASTRO_TARGET_SYSACT;
                if (sys >= 0 && sys < 2) m_sys[sys] = idx;
                continue;
            }
            default: break;
            }
            if (ctl.player < 0 || ctl.player >= ASTROSESSION_PLAYERS) continue;
            Grip &grip = m_grip[ctl.player];
            switch (ctl.kind) {
            case ASTRO_CTL_CAPTION: grip.caption = idx; break;
            case ASTRO_CTL_STICK: grip.stick = idx; break;
            case ASTRO_CTL_KNOB: grip.knob = idx; break;
            case ASTRO_CTL_TRIGGER: grip.trigger = idx; break;
            case ASTRO_CTL_DIRECTION:
                if (nextDir[ctl.player] < 8) grip.dir[nextDir[ctl.player]++] = idx;
                break;
            default: break;
            }
        }
        for (Grip &grip : m_grip) {
            if (grip.stick < 0 || grip.knob < 0 || grip.caption < 0) continue;
            const QRectF disc = rectOf(grip.stick);
            grip.centre = disc.center();
            grip.radius = disc.width() / 2;
            const QRectF dial = rectOf(grip.knob);
            grip.dialCentre = dial.center();
            grip.dialRadius = dial.width() / 2;
            const QRectF cap = rectOf(grip.caption);
            const qreal sysTop = m_sys[0] >= 0 ? rectOf(m_sys[0]).top() : cap.top() + 5.3 * kUnit;
            grip.card = QRectF(cap.left(), cap.top(), cap.width(), sysTop - 0.35 * kUnit - cap.top());
        }
        m_panelW = qCeil(wide * kUnit) + 2 * kCasePad;
        m_panelH = kTop + qCeil(tall * kUnit) + kCasePad;
        m_panelBottom = m_grip[0].caption >= 0 ? rectOf(m_grip[0].caption).top() - 0.17 * kUnit : m_panelH / 2.0;
        setFixedSize(m_panelW, m_panelH);
    }

    QRectF rectOf(int idx) const
    {
        const astro_control &ctl = m_layout[idx];
        return QRectF(kCasePad + ctl.xpos * kUnit, kTop + ctl.ypos * kUnit, ctl.width * kUnit, ctl.height * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return astrosession_controls_hit(float((pos.x() - kCasePad) / kUnit), float((pos.y() - kTop) / kUnit));
    }

    void setPointer(int player, unsigned mask)
    {
        if (player < 0 || player >= ASTROSESSION_PLAYERS || m_ptrMask[player] == mask) return;
        m_ptrMask[player] = mask;
        astrosession_pointer_set_player(m_session, player, mask);
    }

    /* The direction a pointer at pos holds on the grip it began on: nothing
     * near the middle, and past the rim still tilts that way. */
    unsigned discMask(int player, const QPointF &pos) const
    {
        const Grip &grip = m_grip[player];
        const QPointF delta = pos - grip.centre;
        return astrosession_stick_from_vector(float(delta.x()), float(delta.y()), float(grip.radius));
    }

    /* The knob to the angle of pos (nothing in the dead centre). */
    void dragDial(int player, const QPointF &pos)
    {
        const Grip &grip = m_grip[player];
        const QPointF delta = pos - grip.dialCentre;
        const int value = astrosession_knob_from_point(delta.x(), delta.y(), grip.dialRadius);
        if (value >= 0) {
            astrosession_knob_set(m_session, player, (uint8_t)value);
            pollHeld();
        }
    }

    /* Map mode: a click on a part picks its target (a diagonal drives two
     * contacts, so it has none; the knob splits into left, right and hub). */
    void pick(int idx, const QPointF &pos)
    {
        const astro_control &ctl = m_layout[idx];
        if (ctl.kind == ASTRO_CTL_KNOB) {
            const Grip &grip = m_grip[ctl.player];
            const QPointF delta = pos - grip.dialCentre;
            if (std::hypot(delta.x(), delta.y()) < grip.dialRadius * 0.35)
                m_onPick(ASTRO_TARGET_PLAYER(ctl.player, ASTRO_ACT_KNOB));
            else
                m_onPick(ASTRO_TARGET_PLAYER(ctl.player, delta.x() < 0 ? ASTRO_ACT_KNOB_DEC : ASTRO_ACT_KNOB_INC));
        } else if (ctl.target >= 0) {
            m_onPick(ctl.target);
        } else if (ctl.kind == ASTRO_CTL_DIRECTION) {
            m_onHint(QStringLiteral("A diagonal holds two directions: map Up, Down, Left and Right."));
        } else if (ctl.kind == ASTRO_CTL_STICK) {
            m_onHint(QStringLiteral("Map the ring's four wedges: Up, Down, Left and Right."));
        }
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const astro_control &ctl = m_layout[idx];
        if (ctl.kind == ASTRO_CTL_KNOB) {
            const int player = ctl.player;
            return QStringLiteral("Knob %1: drag round to turn it, scroll to nudge it\nLeft: %2    Right: %3    Gamepad axis: %4")
                .arg(player + 1)
                .arg(keyName(ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_DEC)).isEmpty()
                         ? QStringLiteral("none") : keyName(ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_DEC)),
                     keyName(ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_INC)).isEmpty()
                         ? QStringLiteral("none") : keyName(ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_INC)),
                     padName(ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB)).isEmpty()
                         ? QStringLiteral("none") : padName(ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB)));
        }
        if (ctl.target >= 0) return bindingTip(ctl.target);
        switch (ctl.kind) {
        case ASTRO_CTL_STICK: return QStringLiteral("Drag to tilt the grip");
        case ASTRO_CTL_DIRECTION:
            return QStringLiteral("%1 (a diagonal holds two directions)").arg(local(ctl.label));
        default: return QString();
        }
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(const astro_control &ctl) const
    {
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        switch (ctl.kind) {
        case ASTRO_CTL_KEY: return (m_keysHeld >> (ctl.target - ASTRO_TARGET_KEYPAD)) & 1u;
        case ASTRO_CTL_TRIGGER: return (m_actHeld[ctl.player] >> ASTRO_ACT_TRIGGER) & 1u;
        case ASTRO_CTL_DIRECTION: return (m_actHeld[ctl.player] & ctl.act_mask) == ctl.act_mask;
        case ASTRO_CTL_SYSBUTTON: return m_hold == Strip && m_holdIdx >= 0 && &m_layout[m_holdIdx] == &ctl && m_partDown;
        default: return false;
        }
    }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(const astro_control &ctl) const { return m_map >= 0 && ctl.target != m_map; }

    /* ---- painting ---- */

    void paintCase(QPainter &pnt)
    {
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient walnut(0, 0, 0, height());
        walnut.setColorAt(0, kWalnutTop);
        walnut.setColorAt(1, kWalnutBottom);
        pnt.setPen(kWalnutEdge);
        pnt.setBrush(walnut);
        pnt.drawRoundedRect(body, 14, 14);
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(kPinstripe, 1));
        pnt.drawRoundedRect(QRectF(4.5, 4.5, width() - 9.0, height() - 9.0), 11, 11);

        /* the console's black top, with the Bally ARCADE label on its left */
        const QRectF panel(10, 10, width() - 20.0, m_panelBottom - 10.0);
        pnt.setPen(QPen(kPanelEdge, 1));
        pnt.setBrush(kPanelBlack);
        pnt.drawRoundedRect(panel, 8, 8);
        pnt.setPen(QPen(QColor(0x3A, 0x39, 0x38), 1));
        pnt.drawLine(panel.topLeft() + QPointF(10, 1.5), panel.topRight() + QPointF(-10, 1.5));

        const qreal plateLeft = m_overlayIdx >= 0 ? rectOf(m_overlayIdx).left() : width() / 3;
        const qreal plateRight = m_overlayIdx >= 0 ? rectOf(m_overlayIdx).right() : width() * 2 / 3;
        const QRectF label(panel.left() + 12, panel.top() + 14, plateLeft - panel.left() - 26, 58);
        pnt.setPen(QPen(kGoldInk, 1));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(label, 6, 6);
        QFont script = pixelFont(font(), 22, true);
        script.setItalic(true);
        pnt.setFont(script);
        pnt.setPen(kLabelWhite);
        pnt.drawText(QRectF(label.left(), label.top() + 3, label.width(), 28), Qt::AlignCenter, QStringLiteral("Bally"));
        QFont arcade = pixelFont(font(), 12, true);
        arcade.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        pnt.setFont(arcade);
        pnt.setPen(kLabelRed);
        pnt.drawText(QRectF(label.left(), label.top() + 31, label.width(), 20), Qt::AlignCenter, QStringLiteral("ARCADE"));

        QFont name = pixelFont(font(), 11, true);
        name.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        pnt.setFont(name);
        pnt.setPen(kGoldInk);
        const QRectF right(plateRight + 14, panel.top() + 14, panel.right() - plateRight - 26, 58);
        pnt.drawText(QRectF(right.left(), right.top() + 6, right.width(), 20), Qt::AlignCenter, QStringLiteral("ASTROCADE"));
        pnt.setFont(pixelFont(font(), 10, false));
        pnt.setPen(kDimInk);
        pnt.drawText(QRectF(right.left(), right.top() + 30, right.width(), 16), Qt::AlignCenter, QStringLiteral("FujiNet Go"));
    }

    /* The keypad's plate: the champagne face with its gold column and frame, or
     * the Bally BASIC card laid over it with a black legend bar over each row. */
    void paintPlate(QPainter &pnt)
    {
        if (m_overlayIdx < 0 || m_key[0] < 0 || m_key[4] < 0) return;
        const QRectF plate = rectOf(m_overlayIdx);
        const qreal zone = kLegendH * kUnit;
        if (m_overlay) {
            pnt.setPen(QPen(kCardEdge, 1));
            pnt.setBrush(kCard);
            pnt.drawRoundedRect(plate, 8, 8);
            for (int row = 0; row < 6; ++row) {
                const qreal top = rectOf(m_key[row * 4]).top() - zone;
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(kBar);
                pnt.drawRoundedRect(QRectF(plate.left() + 3, top + 0.5, plate.width() - 6, zone - 1.5), 3, 3);
            }
            return;
        }
        pnt.setPen(QPen(kPlateFrame, 2));
        pnt.setBrush(kPlate);
        pnt.drawRoundedRect(plate, 8, 8);
        /* the gold column from DIV down */
        const qreal blockTop = rectOf(m_key[4]).top() - zone - 2;
        const QRectF column(rectOf(m_key[7]).left() - 6, blockTop + 2, rectOf(m_key[7]).width() + 12,
                            plate.bottom() - blockTop - 7);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kGoldColumn);
        pnt.drawRoundedRect(column, 4, 4);
    }

    /* a key's printed legend above it, in dark brown on the plate */
    void paintLegend(QPainter &pnt, const astro_control &ctl, const QRectF &key)
    {
        const QRectF zone(key.left() - 5, key.top() - kLegendH * kUnit, key.width() + 10, kLegendH * kUnit);
        const QString text = local(ctl.label);
        pnt.setPen(kLegendInk);
        if (text == QString(QChar(0x2191)) || text == QString(QChar(0x2193))) {
            paintArrow(pnt, zone.center() + QPointF(0, 1), 4.5, text == QString(QChar(0x2191)), kLegendInk);
            return;
        }
        drawFitted(pnt, zone.adjusted(0, 2, 0, 0), text, pixelFont(font(), 12, true));
    }

    /* the overlay's legends on the bar above a key: the gold word over the three
     * shifted characters in their colours (green, red, blue) */
    void paintOverlayLegend(QPainter &pnt, int key, const QRectF &win)
    {
        const qreal zoneTop = win.top() - kLegendH * kUnit;
        const astro_overlay_key &ov = astrosession_overlay_layout()[key];
        if (key >= 20) {
            /* the bottom row are the shifts: their colour's name on the bar */
            static const char *const names[4] = { "GREEN", "RED", "BLUE", "" };
            static const QColor shades[4] = { kGreen, kRed, kBlue, kWords };
            pnt.setPen(shades[key - 20]);
            drawFitted(pnt, QRectF(win.left() - 4, zoneTop + 2, win.width() + 8, kLegendH * kUnit - 4),
                       QString::fromLatin1(names[key - 20]), pixelFont(font(), 8, true));
            return;
        }
        if (ov.legend[ASTRO_SHIFT_WORDS]) {
            pnt.setPen(kWords);
            drawFitted(pnt, QRectF(win.left() - 4, zoneTop + 1, win.width() + 8, 8),
                       local(ov.legend[ASTRO_SHIFT_WORDS]), pixelFont(font(), 8, true));
        }
        static const int shifts[3] = { ASTRO_SHIFT_GREEN, ASTRO_SHIFT_RED, ASTRO_SHIFT_BLUE };
        static const QColor fills[3] = { kGreen, kRed, kBlue };
        const qreal boxW = (win.width() - 2) / 3;
        for (int cell = 0; cell < 3; ++cell) {
            const char *legend = ov.legend[shifts[cell]];
            if (!legend) continue;
            const QRectF box(win.left() + cell * (boxW + 1), zoneTop + 9, boxW, 8);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(fills[cell]);
            pnt.drawRoundedRect(box, 2, 2);
            pnt.setPen(QColor(Qt::white));
            drawFitted(pnt, box, local(legend), pixelFont(font(), 8, true));
        }
    }

    void paintKey(QPainter &pnt, int idx)
    {
        const astro_control &ctl = m_layout[idx];
        const int key = ctl.target - ASTRO_TARGET_KEYPAD;
        const bool held = lit(ctl);
        const bool over = !held && m_hover == idx;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        if (m_overlay) paintOverlayLegend(pnt, key, full);
        else paintLegend(pnt, ctl, full);

        pnt.setPen(Qt::NoPen);
        pnt.setBrush(m_overlay ? kWindowSide : kKeySide);
        pnt.drawRoundedRect(full, 4, 4);
        const QRectF face = full.adjusted(0, 0, 0, -2);
        QColor fill = m_overlay ? (over ? kWindowHover : kWindow) : (over ? kKeyHover : kKeyBase);
        if (held) fill = astroCtlAccent();
        pnt.setPen(held ? QPen(astroCtlAccentDark(), 1.5) : QPen(m_overlay ? kWindowSide : kKeyEdge, 0.8));
        if (!held && !m_overlay) {
            QLinearGradient bevel(face.topLeft(), face.bottomLeft());
            bevel.setColorAt(0, over ? kKeyHover : kKeyTop);
            bevel.setColorAt(1, over ? kKeyBase : kKeyBase);
            pnt.setBrush(bevel);
        } else {
            pnt.setBrush(fill);
        }
        pnt.drawRoundedRect(face, 4, 4);

        const QColor ink = held ? astroCtlAccentInk() : (m_overlay ? kWindowInk : kKeyInk);
        pnt.setPen(ink);
        if (showBind()) {
            drawFitted(pnt, face.adjusted(1, 0, -1, 0), bindingText(ctl.target), pixelFont(font(), 10, true));
        } else if (m_overlay) {
            if (key >= 20 && key < 23) {
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(held ? astroCtlAccentInk() : (key == 20 ? kGreen : key == 21 ? kRed : kBlue));
                pnt.drawEllipse(face.center(), 5, 5);
            } else if (key == 23) {
                pnt.setPen(held ? astroCtlAccentInk() : kWordsOnWhite);
                drawFitted(pnt, face.adjusted(1, 0, -1, 0), QStringLiteral("WORDS"), pixelFont(font(), 8, true));
            } else if (const char *plain = astrosession_overlay_layout()[key].legend[ASTRO_SHIFT_NONE]) {
                drawFitted(pnt, face.adjusted(1, 0, -1, 0), local(plain), pixelFont(font(), 11, true));
            }
        }
        pnt.setOpacity(1.0);
    }

    /* an annular sector round the disc: direction sector * 45 degrees, a little under 45 wide */
    static QPainterPath wedgePath(const QPointF &centre, qreal rad0, qreal rad1, int sector)
    {
        const qreal start = sector * 45.0 - 22.5 + 1.0, sweep = 43.0;
        const QRectF outer(centre.x() - rad1, centre.y() - rad1, 2 * rad1, 2 * rad1);
        const QRectF inner(centre.x() - rad0, centre.y() - rad0, 2 * rad0, 2 * rad0);
        QPainterPath path;
        path.arcMoveTo(outer, start);
        path.arcTo(outer, start, sweep);
        path.arcTo(inner, start + sweep, -sweep);
        path.closeSubpath();
        return path;
    }

    void paintGrip(QPainter &pnt, int player)
    {
        const Grip &grip = m_grip[player];
        if (grip.stick < 0) return;
        const QRectF card = grip.card;

        /* the glossy black shell, its caption above */
        QLinearGradient shell(card.topLeft(), card.bottomLeft());
        shell.setColorAt(0, kShellTop);
        shell.setColorAt(1, kShellBottom);
        pnt.setPen(QPen(kShellEdge, 1));
        pnt.setBrush(shell);
        pnt.drawRoundedRect(card, 14, 14);
        pnt.setPen(QPen(kShellGloss, 1));
        pnt.drawLine(card.topLeft() + QPointF(16, 1.5), card.topRight() + QPointF(-16, 1.5));
        pnt.setPen(kCaption);
        pnt.setFont(scaledFont(font(), 0.9, true));
        pnt.drawText(rectOf(grip.caption), Qt::AlignCenter, local(m_layout[grip.caption].label));

        paintGripFront(pnt, card);
        paintRing(pnt, player);
        if (grip.trigger >= 0) paintTrigger(pnt, grip.trigger);
    }

    /* the grip below the ring: the knurled panel and the gold Bally badge */
    void paintGripFront(QPainter &pnt, const QRectF &card)
    {
        const QRectF knurl(card.left() + 1.35 * kUnit, card.top() + 4.05 * kUnit, 0.95 * kUnit, 1.1 * kUnit);
        pnt.setPen(QPen(kShellEdge, 1));
        pnt.setBrush(kKnurl);
        pnt.drawRoundedRect(knurl, 4, 4);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKnurlDot);
        for (qreal row = knurl.top() + 4; row < knurl.bottom() - 2; row += 5)
            for (qreal col = knurl.left() + 4 + (int(row - knurl.top()) / 5 % 2 ? 2.5 : 0); col < knurl.right() - 2; col += 5)
                pnt.drawEllipse(QPointF(col, row), 1, 1);

        const QPointF badge(card.left() + 0.8 * kUnit, card.top() + 4.5 * kUnit);
        const qreal rad = 0.32 * kUnit;
        pnt.setPen(QPen(QColor(0x0A, 0x0A, 0x0A), 2));
        pnt.setBrush(QColor(0x0E, 0x0D, 0x0D));
        pnt.drawEllipse(badge, rad + 2, rad + 2);
        QRadialGradient gold(badge + QPointF(-rad * 0.3, -rad * 0.3), rad * 1.6);
        gold.setColorAt(0, QColor(0xE8, 0xC2, 0x5A));
        gold.setColorAt(1, kBadgeGold.darker(120));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(gold);
        pnt.drawEllipse(badge, rad, rad);
        QFont script = pixelFont(font(), 11, true);
        script.setItalic(true);
        pnt.setFont(script);
        pnt.setPen(QColor(0x2A, 0x1E, 0x08));
        pnt.drawText(QRectF(badge.x() - rad, badge.y() - rad, 2 * rad, 2 * rad), Qt::AlignCenter, QStringLiteral("B"));
    }

    /* the ring of eight wedges and the knob in the middle of it */
    void paintRing(QPainter &pnt, int player)
    {
        const Grip &grip = m_grip[player];
        const qreal outer = grip.radius, inner = grip.dialRadius + 0.05 * kUnit;
        pnt.setPen(QPen(kWedgeEdge.darker(180), 1.5));
        pnt.setBrush(kRing);
        pnt.drawEllipse(grip.centre, outer + 3, outer + 3);
        for (int sector = 0; sector < 8; ++sector) {
            if (grip.dir[sector] < 0) continue;
            const astro_control &ctl = m_layout[grip.dir[sector]];
            const bool held = lit(ctl);
            const bool over = !held && m_hover == grip.dir[sector];
            pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
            pnt.setPen(QPen(held ? astroCtlAccentDark() : kWedgeEdge, 1));
            pnt.setBrush(held ? astroCtlAccent() : (over ? kWedgeHover : kWedge));
            pnt.drawPath(wedgePath(grip.centre, inner + 1, outer, sector));
            const QPointF mid = grip.centre + unit(sector * 45.0) * ((inner + outer) / 2);
            if (showBind() && ctl.target >= 0) {
                pnt.setPen(held ? astroCtlAccentInk() : kKeyInk);
                drawFitted(pnt, QRectF(mid.x() - 20, mid.y() - 11, 40, 22), bindingText(ctl.target),
                           pixelFont(font(), 10, true));
            } else {
                /* a chevron pointing out */
                const QPointF dir = unit(sector * 45.0), perp(-dir.y(), dir.x());
                const qreal size = sector % 2 ? 3.2 : 4.6;
                QPolygonF chevron;
                chevron << mid + dir * size << mid - dir * size + perp * (size * 1.3)
                        << mid - dir * size - perp * (size * 1.3);
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(held ? astroCtlAccentInk() : kChevron);
                pnt.drawPolygon(chevron);
            }
            pnt.setOpacity(1.0);
        }
        paintKnob(pnt, player);
    }

    /* the fluted orange-brown knob on its collar: a dial, its pointer at the knob's position */
    void paintKnob(QPainter &pnt, int player)
    {
        const Grip &grip = m_grip[player];
        const QPointF cen = grip.dialCentre;
        const qreal rad = grip.dialRadius;
        const int targets[3] = { ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB), ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_DEC),
                                 ASTRO_TARGET_PLAYER(player, ASTRO_ACT_KNOB_INC) };
        const bool picked = m_map >= 0 && (m_map == targets[0] || m_map == targets[1] || m_map == targets[2]);
        pnt.setOpacity(m_map >= 0 && !picked ? kDimAlpha : 1.0);

        pnt.setPen(QPen(kCollarEdge, 1));
        pnt.setBrush(kCollar);
        pnt.drawEllipse(cen, rad, rad);
        /* the 270 degree sweep marked on the collar: the gap is at the bottom */
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(kCollarEdge, 1.5, Qt::SolidLine, Qt::RoundCap));
        pnt.drawArc(QRectF(cen.x() - rad * 0.93, cen.y() - rad * 0.93, rad * 1.86, rad * 1.86), -45 * 16, 270 * 16);

        const qreal top = rad * 0.82;
        QRadialGradient dome(cen + QPointF(-top * 0.3, -top * 0.35), top * 1.6);
        dome.setColorAt(0, kKnobLit);
        dome.setColorAt(0.55, kKnobMid);
        dome.setColorAt(1, kKnobShade);
        pnt.setPen(QPen(kCollarEdge, 1));
        pnt.setBrush(dome);
        pnt.drawEllipse(cen, top, top);
        /* the flutes round its edge */
        pnt.setPen(QPen(kRib, 1));
        for (int rib = 0; rib < 24; ++rib) {
            const QPointF dir = unit(rib * 15.0);
            pnt.drawLine(cen + dir * (top * 0.62), cen + dir * (top * 0.97));
        }

        if (showBind()) {
            /* the knob split in two for Map: a key for each way, the axis in the hub */
            pnt.setPen(QPen(QColor(0, 0, 0, 120), 1));
            pnt.drawLine(cen + QPointF(0, -top * 0.95), cen + QPointF(0, top * 0.95));
            pnt.setBrush(kKnobShade);
            pnt.drawEllipse(cen, top * 0.32, top * 0.32);
            pnt.setPen(QColor(0xF2, 0xD9, 0xB8));
            drawFitted(pnt, QRectF(cen.x() - top * 0.9, cen.y() - 7, top * 0.58, 14),
                       bindingText(targets[1]), pixelFont(font(), 9, true));
            drawFitted(pnt, QRectF(cen.x() + top * 0.32, cen.y() - 7, top * 0.58, 14),
                       bindingText(targets[2]), pixelFont(font(), 9, true));
            drawFitted(pnt, QRectF(cen.x() - top * 0.3, cen.y() - 6, top * 0.6, 12),
                       bindings_target_pad(targets[0]) >= 0 ? QStringLiteral("axis") : QStringLiteral("—"),
                       pixelFont(font(), 8, true));
        } else {
            /* the pointer, and the player's number in the hub */
            const qreal ang = qDegreesToRadians(astrosession_knob_angle(m_knob[player]));
            const QPointF dir(std::sin(ang), -std::cos(ang));
            pnt.setPen(QPen(QColor(0x22, 0x10, 0x08, 160), 4.0, Qt::SolidLine, Qt::RoundCap));
            pnt.drawLine(cen + dir * (top * 0.34) + QPointF(0.8, 1.0), cen + dir * (top * 0.9) + QPointF(0.8, 1.0));
            pnt.setPen(QPen(QColor(0xFF, 0xE9, 0xC8), 2.5, Qt::SolidLine, Qt::RoundCap));
            pnt.drawLine(cen + dir * (top * 0.34), cen + dir * (top * 0.9));
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(kKnobShade);
            pnt.drawEllipse(cen, top * 0.34, top * 0.34);
            pnt.setPen(QColor(0xF2, 0xD9, 0xB8));
            pnt.setFont(pixelFont(font(), 10, true));
            pnt.drawText(QRectF(cen.x() - top * 0.34, cen.y() - top * 0.34, top * 0.68, top * 0.68), Qt::AlignCenter,
                         QString::number(player + 1));
        }
        pnt.setOpacity(1.0);
    }

    /* the curved lever at the front of the grip, a pale grey face */
    void paintTrigger(QPainter &pnt, int idx)
    {
        const astro_control &ctl = m_layout[idx];
        const bool held = lit(ctl);
        const bool over = !held && m_hover == idx;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(QPen(kShellEdge, 1));
        pnt.setBrush(QColor(0x0A, 0x0A, 0x0A));
        pnt.drawRoundedRect(full.translated(0, 1.5), 8, 8);
        QLinearGradient lever(full.topLeft(), full.bottomLeft());
        lever.setColorAt(0, over ? kTriggerHover : kTriggerTop);
        lever.setColorAt(1, kTriggerBottom);
        pnt.setPen(held ? QPen(astroCtlAccentDark(), 2) : QPen(kShellEdge, 1));
        pnt.setBrush(held ? QBrush(astroCtlAccent()) : QBrush(lever));
        pnt.drawRoundedRect(full, 8, 8);
        pnt.setPen(held ? astroCtlAccentInk() : QColor(0x24, 0x22, 0x22));
        if (showBind())
            drawFitted(pnt, full.adjusted(1, 2, -1, -2), bindingText(ctl.target), pixelFont(font(), 10, true));
        else
            drawFitted(pnt, full.adjusted(1, 2, -1, -2), QStringLiteral("T"), pixelFont(font(), 11, true));
        pnt.setOpacity(1.0);
    }

    /* The console's two buttons, black with gold lettering like the real RESET. */
    void paintSys(QPainter &pnt, int idx)
    {
        const astro_control &ctl = m_layout[idx];
        const bool down = m_hold == Strip && m_holdIdx == idx && m_partDown;
        const bool picked = mapping() && m_map == ctl.target;
        const bool over = !down && m_hover == idx;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(m_map >= 0 && !picked ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x08, 0x08, 0x08));
        pnt.drawRoundedRect(full, 8, 8);
        const QRectF face = down ? full.adjusted(0, 3, 0, 0) : full.adjusted(0, 0, 0, -3);
        pnt.setBrush(down || picked ? astroCtlAccent() : (over ? kSysHover : kSysFace));
        pnt.setPen(down || picked ? QPen(astroCtlAccentDark(), 1.5) : QPen(kSysEdge, 1));
        pnt.drawRoundedRect(face, 8, 8);
        pnt.setPen(down || picked ? astroCtlAccentInk() : kGoldInk);
        QString text = local(ctl.label);
        if (showBind()) text += QStringLiteral("  ") + bindingText(ctl.target);
        drawFitted(pnt, face.adjusted(3, 0, -3, 0), text, pixelFont(font(), 10, true));
        pnt.setOpacity(1.0);
    }

    astrosession *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    const astro_control *m_layout = nullptr;
    int m_count = 0;
    int m_overlayIdx = -1;
    int m_key[ASTROSESSION_KEY_COUNT];
    int m_sys[2];
    Grip m_grip[ASTROSESSION_PLAYERS];
    quint32 m_keysHeld = 0;
    unsigned m_actHeld[ASTROSESSION_PLAYERS] = {};
    int m_knob[ASTROSESSION_PLAYERS] = { 128, 128, 128, 128 };
    unsigned m_ptrMask[ASTROSESSION_PLAYERS] = {};
    bool m_showBindings = false;
    bool m_overlay = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;          /* the part the mouse holds */
    int m_dragPlayer = -1;       /* the grip the mouse began on */
    bool m_partDown = false;     /* the pointer is still over the part it pressed */
    int m_hover = -1;
    int m_panelW = 0, m_panelH = 0;
    qreal m_panelBottom = 0;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(astrosession *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, int stolen);
    void pollCapture();
    void pollSlow();
    void syncOverlay();
    void rebuildPads();
    void refreshPadPlayers();

    astrosession *m_session;
    ConsoleWidget *m_console = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays player N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QCheckBox *m_overlayBox = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button (an axis, for the knob) for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(astrosession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("astroHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_console = new ConsoleWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    m_console->setObjectName(QStringLiteral("console"));
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        bindings_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_console->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        bindings_reset_defaults(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addStretch(1);
    mapRow->addWidget(astroHelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    auto *optRow = new QHBoxLayout;
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(astrosession_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        astrosession_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_console->setShowBindings(checked);
    });
    m_console->setShowBindings(bindings->isChecked());
    m_overlayBox = new QCheckBox(QStringLiteral("Overlay: Bally BASIC"));
    m_overlayBox->setObjectName(QStringLiteral("overlay"));
    m_overlayBox->setFocusPolicy(Qt::NoFocus);
    m_overlayBox->setToolTip(QStringLiteral("Show the Bally BASIC keypad overlay, and type BASIC on the host keyboard "
                                            "(it switches on by itself when a Bally BASIC cartridge is loaded)"));
    m_overlayBox->setChecked(astrosession_overlay_get(session) != 0);
    connect(m_overlayBox, &QCheckBox::toggled, this, [this](bool checked) {
        astrosession_overlay_set(m_session, checked ? 1 : 0);
        m_console->setOverlay(checked);
    });
    m_console->setOverlay(m_overlayBox->isChecked());
    optRow->addWidget(bindings);
    optRow->addWidget(m_overlayBox);
    optRow->addStretch(1);
    root->addLayout(optRow);

    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_console->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    m_console->pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_console->releaseAll();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    astroCtlSetAccent(m_mapButton, state != -2);
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_captureTimer.stop();
        astrosession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Press a part with the mouse to hold it. Drag on a grip's ring to tilt it, "
                                       "on its knob to turn it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        astrosession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_hint->setText(QStringLiteral("Click a part to remap it (the knob: its left half, right half or hub). "
                                       "Escape leaves Map."));
    } else {
        letGo();
        if (isKnobAxisTarget(state)) {
            m_hint->setText(QStringLiteral("Move a gamepad stick or trigger for %1 (Escape cancels).")
                                .arg(local(bindings_target_label(state))));
            astrosession_gamepad_capture_begin_axis(m_session);
        } else {
            m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                                .arg(local(bindings_target_label(state))));
            astrosession_gamepad_capture_begin(m_session);
        }
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    m_console->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, int stolen)
{
    setMapState(-1);
    m_hint->setText(stolen >= 0 ? QStringLiteral("%1 is now %2 (taken from %3)")
                                      .arg(local(bindings_target_label(target)), what,
                                           local(bindings_target_label(stolen)))
                                : QStringLiteral("%1 is now %2").arg(local(bindings_target_label(target)), what));
}

void ControllersWindow::pollCapture()
{
    int id;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!astrosession_gamepad_capture_poll(m_session, &id)) return;
    char name[48] = "";
    int stolen = -1;
    const int target = m_mapState;
    bindings_set_pad(m_session, target, id, &stolen);
    astrosession_pad_input_name(id, name, sizeof name);
    bound(target, local(name), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes and the overlay turning itself on. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = astrosession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPlayers();
    }
    syncOverlay();
}

void ControllersWindow::syncOverlay()
{
    const bool on = astrosession_overlay_get(m_session) != 0;
    if (m_overlayBox->isChecked() != on) {
        const QSignalBlocker block(m_overlayBox);
        m_overlayBox->setChecked(on);
    }
    m_console->setOverlay(on);
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPlayers()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = astrosession_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("plays player %1").arg(eff + 1)
                                      : QStringLiteral("plays nobody");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the player it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = astrosession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        astrosession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0-3 = that player */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2"),
                        QStringLiteral("Player 3"), QStringLiteral("Player 4") });
        const int assign = astrosession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 3 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            astrosession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPlayers();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPlayers();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = astrosession_gamepad_generation(m_session);
    rebuildPads();
    syncOverlay();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        m_console->pollHeld();
        syncOverlay();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_console->pollHeld();
    m_heldTimer.start(25);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(evt);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        astrosession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    if (astroIsAppKey(evt)) return;      /* F10 and F11 belong to the main window */
    const uint32_t keysym = astroKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (isKnobAxisTarget(m_mapState)) {
            m_hint->setText(QStringLiteral("The knob follows a gamepad axis: move a stick or trigger."));
            return;
        }
        char name[48] = "";
        int stolen = -1;
        const int target = m_mapState;
        bindings_set_keysym(m_session, target, keysym, &stolen);
        astrosession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it: the session's bindings
     * first (a console action goes to the main window's queue, as a gamepad's
     * does), then the Bally BASIC typing. */
    if (!astroForwardKey(m_session, evt, true)) QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = astroKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!astroForwardKey(m_session, evt, false)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void astroControllersToggle(QWidget *parent, astrosession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void astroControllersShow(QWidget *parent, astrosession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool astroControllersVisible()
{
    return g_window && g_window->isVisible();
}

bool astroControllersActive()
{
    return g_window && g_window->isActiveWindow();
}

void astroControllersRefresh()
{
    if (g_window) g_window->refresh();
}
