/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The console as a player sees it: a charcoal case with a walnut strip, the
 * panel (RESET and the buttons 1 TIME, 2 MODE, 3 HOLD, 4 START) across the top
 * and the two hand controllers below, the left one Player 2 and the right one
 * Player 1. Everything is painted from the session's layout
 * (chfsession_controls_layout, in units of a console button's width), so every
 * frontend shows the same parts in the same places.
 *
 * Each controller is one knob with four axes and is drawn as two views of it:
 * from above, the knob in a ring of eight direction wedges (each the sector of
 * the stick disc between half its radius and all of it), shifting toward the
 * held direction, turning when twisted, shrinking when pushed and growing when
 * pulled, with the twist arrows either side; and from the side, the handle with
 * PULL above and PUSH below and the knob's height between them.
 *
 * Gestures act on mouse press AND release: the console samples its controls
 * once a frame, so a press present only for the instant of a click would fall
 * between frames. A drag on the stick tilts it eight ways
 * (chfsession_stick_from_vector); a left-press on the knob pushes, a
 * right-press pulls; the wheel (or a trackpad) twists; the wedges, the arrows
 * and the buttons are held while the mouse is down on them. Hiding the window
 * and losing the focus let go of everything the mouse holds.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent, so the window doubles as an input tester for a new pad.
 * Keys typed with it active go to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts, and gamepad names are elided, so
 * neither Map mode nor a long name resizes the window. A singleton, hidden
 * rather than destroyed, so a half-done remap survives closing it. A timer
 * runs only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a wedge, an arrow, a
 * button, or one of the app's actions along the bottom), then press a host key
 * or a gamepad button. Rebinding steals the key from whatever held it (named in
 * the hint). Escape cancels; while Map is armed nothing is typed into the
 * console.
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

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kUnit = 40.0;        /* pixels per layout unit (a console button's width) */
constexpr int kPadX = 24;            /* the case's margin round the controls */
constexpr int kWood = 16;            /* the walnut strip along the top */
constexpr int kTop = kWood + 20;     /* where the layout's origin sits */
constexpr int kBottom = 44;          /* room for the app's actions in Map mode */
constexpr int kMaxPadRows = 3;

const QColor kInk(0xD8, 0xD4, 0xCC);       /* light text on the case */
const QColor kDimInk(0x8E, 0x8A, 0x84);
const QColor kCase(0x30, 0x2F, 0x2F);      /* the case's charcoal */
const QColor kWalnut(0x47, 0x33, 0x2A);
const QColor kPlate(0x3B, 0x3A, 0x39);     /* a part's resting face */
const QColor kPlateEdge(0x1A, 0x19, 0x19);

QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

QFont scaledFont(QFont f, qreal scale, bool bold = false)
{
    f.setPointSizeF((f.pointSizeF() > 0 ? f.pointSizeF() : 10) * scale);
    f.setBold(bold);
    return f;
}

/* Lines centred in r, the font shrunk until they fit. */
void drawFitted(QPainter &p, const QRectF &r, const QString &text, QFont f)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF fm(f);
        qreal w = 0;
        for (const QString &l : lines) w = qMax(w, fm.horizontalAdvance(l));
        if ((w <= r.width() && fm.lineSpacing() * lines.size() <= r.height() + 2) || f.pointSizeF() <= 5.0)
            break;
        f.setPointSizeF(f.pointSizeF() - 0.5);
    }
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, text);
}

QString keyName(chfsession *s, int target)
{
    const chf_binding b = chfsession_binding_get(s, target);
    char key[48] = "";
    if (b.keysym) chfsession_keysym_name(b.keysym, key, sizeof key);
    return local(key);
}

/* A part's binding as Map mode shows it: the key over the gamepad button. */
QString bindingText(chfsession *s, int target)
{
    const chf_binding b = chfsession_binding_get(s, target);
    const QString key = keyName(s, target);
    QString text = key.isEmpty() ? QStringLiteral("—") : key;
    if (b.button != CHF_PAD_BTN_NONE) text += QLatin1Char('\n') + local(chf_pad_button_name(b.button));
    return text;
}

QString bindingTip(chfsession *s, int target)
{
    const chf_binding b = chfsession_binding_get(s, target);
    const QString key = keyName(s, target);
    QString tip = QStringLiteral("%1: %2").arg(local(chf_target_name(target)),
                                               key.isEmpty() ? QStringLiteral("no key") : key);
    if (b.button != CHF_PAD_BTN_NONE) tip += QStringLiteral(", ") + local(chf_pad_button_name(b.button));
    return tip;
}

QPointF unit(qreal deg)   /* a direction, degrees counter-clockwise from 3 o'clock, as drawn */
{
    return QPointF(std::cos(qDegreesToRadians(deg)), -std::sin(qDegreesToRadians(deg)));
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
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the console and its two controllers -------------------------------- */

class ConsoleWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    ConsoleWidget(chfsession *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        load();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        setPointer(0, 0);
        setPointer(1, 0);
        if (m_holdTarget >= 0) {
            chfsession_press(m_session, m_holdTarget, 0);
            m_holdTarget = -1;
        }
        m_hold = None;
        m_wheelAcc = 0;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool on)
    {
        m_showBindings = on;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse); repaints
     * only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int port = 0; port < CHF_PLAYERS; ++port) {
            const unsigned h = chfsession_buttons_held(m_session, port);
            if (h != m_held[port]) {
                m_held[port] = h;
                changed = true;
            }
        }
        for (int k = 0; k < CHF_KEY_COUNT; ++k) {
            const char on = chfsession_key_held(m_session, k) != 0;
            if (on != m_keyHeld[k]) {
                m_keyHeld[k] = on;
                changed = true;
            }
        }
        const char reset = chfsession_switch_held(m_session, CHF_SW_RESET) != 0;
        if (reset != m_resetHeld) {
            m_resetHeld = reset;
            changed = true;
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paintCase(p);
        for (int i = 0; i < m_n; ++i) {
            const chf_control &c = m_layout[i];
            if (c.kind == CHF_CTL_BUTTON || c.kind == CHF_CTL_RESET) paintButton(p, c);
        }
        for (int port = 1; port >= 0; --port) paintController(p, port);
        paintActions(p);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton) return;
        const QPointF pos = e->position();
        if (m_map != -2) {
            if (e->button() != Qt::LeftButton) return;
            pick(pos);
            return;
        }
        if (m_hold != None) return;          /* one thing at a time */
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const chf_control &c = m_layout[idx];
        switch (c.kind) {
        case CHF_CTL_BUTTON:
        case CHF_CTL_RESET:
            m_hold = Part;
            m_holdIdx = idx;
            m_holdTarget = c.target;
            chfsession_press(m_session, c.target, 1);
            break;
        case CHF_CTL_TWIST:
        case CHF_CTL_AXIS:
            if (e->button() != Qt::LeftButton) return;
            m_hold = Part;
            m_holdIdx = idx;
            setPointer(c.port, c.act_mask);
            break;
        case CHF_CTL_STICK:
        case CHF_CTL_DIRECTION:
            m_hold = Disc;
            m_dragPort = c.port;
            /* the knob: a left-press pushes it, a right-press pulls it; a
             * wedge holds its direction, and a drag moves on from either */
            m_capMask = c.kind == CHF_CTL_STICK
                            ? (e->button() == Qt::LeftButton ? 1u << CHF_ACT_PUSH : 1u << CHF_ACT_PULL) : 0u;
            setPointer(c.port, discMask(c.port, pos));
            break;
        default: return;
        }
        update();
    }

    /* Dragging off a button or an arrow lets it go (and back on, holds it
     * again); on the stick it moves from one direction to the next. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPointF pos = e->position();
        if (m_hold == Disc) {
            setPointer(m_dragPort, discMask(m_dragPort, pos));
            update();
        } else if (m_hold == Part) {
            const bool on = hitAt(pos) == m_holdIdx;
            const chf_control &c = m_layout[m_holdIdx];
            if (c.kind == CHF_CTL_BUTTON || c.kind == CHF_CTL_RESET) {
                if (on && m_holdTarget < 0) {
                    m_holdTarget = c.target;
                    chfsession_press(m_session, c.target, 1);
                } else if (!on && m_holdTarget >= 0) {
                    chfsession_press(m_session, m_holdTarget, 0);
                    m_holdTarget = -1;
                }
            } else {
                setPointer(c.port, on ? c.act_mask : 0);
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton) return;
        if (m_hold != None) releaseAll();
    }

    /* A wheel notch (or a trackpad's worth of scrolling) over a controller
     * twists it: up is clockwise, as turning a dial up. */
    void wheelEvent(QWheelEvent *e) override
    {
        e->accept();
        if (m_map != -2) return;
        const int port = portAtX(e->position().x());
        if (port < 0 || e->position().y() < kTop + m_blockTop * kUnit - 6) return;
        m_wheelAcc += e->angleDelta().y();
        while (m_wheelAcc >= 120) {
            chfsession_twist_pulse(m_session, port, +1);
            m_wheelAcc -= 120;
        }
        while (m_wheelAcc <= -120) {
            chfsession_twist_pulse(m_session, port, -1);
            m_wheelAcc += 120;
        }
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const QString tip = tipAt(he->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(he->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(e);
    }

private:
    enum Hold { None, Disc, Part };

    /* one controller's parts, by index into the layout */
    struct Pad {
        int caption = -1, stick = -1, twistCcw = -1, twistCw = -1, pull = -1, side = -1, push = -1;
        int dir[8];
        QPointF centre;
        qreal radius = 0;
        QRectF block;
    };

    void load()
    {
        float w = 0, h = 0;
        m_layout = chfsession_controls_layout(CHF_MACHINE_CHANNELF, &m_n, &w, &h);
        for (int port = 0; port < CHF_PLAYERS; ++port)
            for (int &d : m_pad[port].dir) d = -1;
        int nextDir[CHF_PLAYERS] = {};
        for (int i = 0; i < m_n; ++i) {
            const chf_control &c = m_layout[i];
            if (c.port < 0 || c.port >= CHF_PLAYERS) continue;
            Pad &pd = m_pad[c.port];
            switch (c.kind) {
            case CHF_CTL_CAPTION: pd.caption = i; break;
            case CHF_CTL_STICK: pd.stick = i; break;
            case CHF_CTL_DIRECTION:
                if (nextDir[c.port] < 8) pd.dir[nextDir[c.port]++] = i;
                break;
            case CHF_CTL_TWIST:
                (c.act_mask & (1u << CHF_ACT_TWIST_CCW) ? pd.twistCcw : pd.twistCw) = i;
                break;
            case CHF_CTL_AXIS:
                (c.act_mask & (1u << CHF_ACT_PULL) ? pd.pull : pd.push) = i;
                break;
            case CHF_CTL_SIDE: pd.side = i; break;
            default: break;
            }
        }
        for (Pad &pd : m_pad) {
            if (pd.stick < 0 || pd.caption < 0) continue;
            const QRectF s = rectOf(pd.stick);
            pd.centre = s.center();
            pd.radius = s.width() / 2;
            const QRectF cap = rectOf(pd.caption);
            pd.block = QRectF(cap.left() - 0.15 * kUnit, cap.top() - 0.1 * kUnit, cap.width() + 0.3 * kUnit,
                              (h - (m_layout[pd.caption].y)) * kUnit + 0.2 * kUnit);
        }
        m_blockTop = m_layout[m_pad[0].caption].y;
        m_panelW = qCeil(w * kUnit) + 2 * kPadX;
        m_panelH = kTop + qCeil(h * kUnit) + kBottom;
        setFixedSize(m_panelW, m_panelH);
    }

    QRectF rectOf(int i) const
    {
        const chf_control &c = m_layout[i];
        return QRectF(kPadX + c.x * kUnit, kTop + c.y * kUnit, c.w * kUnit, c.h * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return chfsession_controls_hit(CHF_MACHINE_CHANNELF, float((pos.x() - kPadX) / kUnit),
                                       float((pos.y() - kTop) / kUnit));
    }

    /* the controller whose block is under x, or -1 */
    int portAtX(qreal x) const
    {
        for (int port = 0; port < CHF_PLAYERS; ++port)
            if (m_pad[port].caption >= 0 && x >= m_pad[port].block.left() && x < m_pad[port].block.right())
                return port;
        return -1;
    }

    void setPointer(int port, unsigned mask)
    {
        if (port < 0 || port >= CHF_PLAYERS || m_ptrMask[port] == mask) return;
        m_ptrMask[port] = mask;
        chfsession_pointer_set(m_session, port, mask);
    }

    /* What a pointer at `pos` holds on the stick it began on: inside the knob,
     * what the press began with (push, pull, or nothing); anywhere else, the
     * direction it points (nothing near the middle, and past the rim still
     * tilts that way). */
    unsigned discMask(int port, const QPointF &pos) const
    {
        const Pad &pd = m_pad[port];
        const QPointF d = pos - pd.centre;
        if (std::hypot(d.x(), d.y()) < pd.radius * 0.5) return m_capMask;
        return chfsession_stick_from_vector(float(d.x()), float(d.y()), float(pd.radius));
    }

    /* Map mode: a click on a part picks its target (a diagonal drives two
     * contacts, so it has none). */
    void pick(const QPointF &pos)
    {
        for (int sa = 0; sa < CHF_SYSACT_COUNT; ++sa)
            if (actionRect(sa).contains(pos)) {
                m_onPick(CHF_TARGET_SYSACT(sa));
                return;
            }
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const chf_control &c = m_layout[idx];
        if (c.target >= 0) {
            m_onPick(c.target);
        } else if (c.kind == CHF_CTL_DIRECTION) {
            m_onHint(QStringLiteral("A diagonal holds two directions: map Forward, Back, Left and Right."));
        } else if (c.kind == CHF_CTL_STICK) {
            m_onHint(QStringLiteral("Map the knob's parts: the wedges, the twist arrows, PULL and PUSH."));
        }
    }

    QString tipAt(const QPoint &pos) const
    {
        for (int sa = 0; sa < CHF_SYSACT_COUNT; ++sa)
            if (showActions() && actionRect(sa).contains(pos)) return bindingTip(m_session, CHF_TARGET_SYSACT(sa));
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const chf_control &c = m_layout[idx];
        if (c.target >= 0) return bindingTip(m_session, c.target);
        switch (c.kind) {
        case CHF_CTL_STICK:
            return QStringLiteral("Drag to tilt; left-press to push, right-press to pull; the wheel twists");
        case CHF_CTL_DIRECTION: return QStringLiteral("%1 (a diagonal holds two contacts)").arg(local(c.label));
        default: return QString();
        }
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }
    bool showActions() const { return showBind(); }

    /* the app's own actions, along the bottom (Map mode and "show bindings") */
    QRectF actionRect(int sa) const
    {
        const qreal w = 190, h = 28, gap = 14;
        const qreal total = CHF_SYSACT_COUNT * w + (CHF_SYSACT_COUNT - 1) * gap;
        return QRectF((width() - total) / 2 + sa * (w + gap), height() - kBottom + 8, w, h);
    }

    bool lit(const chf_control &c) const
    {
        if (mapping()) return c.target >= 0 && c.target == m_map;
        switch (c.kind) {
        case CHF_CTL_BUTTON: return m_keyHeld[c.target - CHF_TARGET_KEY(0)] != 0;
        case CHF_CTL_RESET: return m_resetHeld != 0;
        case CHF_CTL_TWIST:
        case CHF_CTL_AXIS: return (m_held[c.port] & c.act_mask) != 0;
        case CHF_CTL_DIRECTION: return (m_held[c.port] & c.act_mask) == c.act_mask;
        default: return false;
        }
    }

    /* ---- painting ---- */

    void paintCase(QPainter &p)
    {
        /* the charcoal case, a walnut strip along the top */
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient charcoal(0, 0, 0, height());
        charcoal.setColorAt(0, kCase.lighter(112));
        charcoal.setColorAt(1, kCase.darker(120));
        p.setPen(QColor(0x0C, 0x0C, 0x0D));
        p.setBrush(charcoal);
        p.drawRoundedRect(body, 14, 14);

        QPainterPath clip;
        clip.addRoundedRect(body, 14, 14);
        p.save();
        p.setClipPath(clip);
        const QRectF strip(0, 0, width(), kWood);
        QLinearGradient wood(strip.topLeft(), strip.bottomLeft());
        wood.setColorAt(0, kWalnut.lighter(125));
        wood.setColorAt(0.5, kWalnut);
        wood.setColorAt(1, kWalnut.darker(135));
        p.setPen(Qt::NoPen);
        p.setBrush(wood);
        p.drawRect(strip);
        p.setPen(QPen(QColor(0x2A, 0x1A, 0x12, 120), 1));      /* the grain */
        for (int y = 4; y < kWood; y += 5) p.drawLine(QPointF(0, y), QPointF(width(), y + 1));
        p.restore();

        /* the plates the controllers sit on */
        for (int port = 0; port < CHF_PLAYERS; ++port) {
            const Pad &pd = m_pad[port];
            if (pd.caption < 0) continue;
            p.setPen(QPen(kPlateEdge, 1));
            p.setBrush(QColor(0x27, 0x26, 0x26));
            p.drawRoundedRect(pd.block, 10, 10);
            p.setPen(kInk);
            p.setFont(scaledFont(font(), 0.9, true));
            p.drawText(rectOf(pd.caption), Qt::AlignCenter, local(m_layout[pd.caption].label));
        }
    }

    void paintButton(QPainter &p, const chf_control &c)
    {
        const bool reset = c.kind == CHF_CTL_RESET;
        const bool on = lit(c);
        const QRectF r = rectOf(int(&c - m_layout)).adjusted(2, 2, -2, -2);
        /* the cap's side, then its face */
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0E, 0x0D, 0x0D));
        p.drawRoundedRect(r.translated(0, 2.5), 7, 7);
        QLinearGradient face(r.topLeft(), r.bottomLeft());
        if (on) {
            face.setColorAt(0, chfAccentColor().lighter(125));
            face.setColorAt(1, chfAccentColor());
        } else if (reset) {
            face.setColorAt(0, QColor(0x4A, 0x48, 0x47));
            face.setColorAt(1, QColor(0x22, 0x21, 0x21));
        } else {
            face.setColorAt(0, QColor(0xE4, 0xE0, 0xD8));
            face.setColorAt(1, QColor(0xB4, 0xB0, 0xA8));
        }
        p.setBrush(face);
        p.setPen(on ? QPen(chfAccentDarkColor(), 1.5) : QPen(kPlateEdge, 0.8));
        p.drawRoundedRect(r, 7, 7);

        if (reset) {
            /* the reset key carries a ring */
            p.setPen(QPen(on ? chfAccentInk() : QColor(0xE8, 0xE4, 0xDC), 1.8));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(r.center(), r.width() * 0.22, r.width() * 0.22);
        } else {
            p.setPen(chfAccentInk());
            if (!on) p.setPen(QColor(0x25, 0x23, 0x21));
            p.setFont(scaledFont(font(), 1.5, true));
            p.drawText(r, Qt::AlignCenter, local(c.label));
        }
        /* the legend below the cap: its name, or its binding */
        const QRectF leg(r.left() - 8, r.bottom() + 5, r.width() + 16, kUnit * 0.7 - 8);
        if (showBind()) {
            p.setPen(on ? chfAccentColor() : kInk);
            drawFitted(p, leg, bindingText(m_session, c.target), scaledFont(font(), 0.75, true));
        } else {
            p.setPen(reset ? chfAccentColor() : (on ? chfAccentColor() : kInk));
            p.setFont(scaledFont(font(), 0.78, true));
            p.drawText(leg, Qt::AlignHCenter | Qt::AlignTop, local(c.legend ? c.legend : c.label));
        }
    }

    /* an annular sector round the disc: direction `centreDeg`, 45 degrees wide */
    static QPainterPath wedgePath(const QPointF &c, qreal r0, qreal r1, int k)
    {
        const qreal a0 = k * 45.0 - 22.5 + 1.0, sweep = 43.0;
        const QRectF outer(c.x() - r1, c.y() - r1, 2 * r1, 2 * r1), inner(c.x() - r0, c.y() - r0, 2 * r0, 2 * r0);
        QPainterPath path;
        path.arcMoveTo(outer, a0);
        path.arcTo(outer, a0, sweep);
        path.arcTo(inner, a0 + sweep, -sweep);
        path.closeSubpath();
        return path;
    }

    void paintController(QPainter &p, int port)
    {
        const Pad &pd = m_pad[port];
        if (pd.stick < 0) return;
        const unsigned held = mapping() ? 0u : m_held[port];

        /* the twist arrows */
        for (int cw = 0; cw < 2; ++cw) {
            const int idx = cw ? pd.twistCw : pd.twistCcw;
            if (idx < 0) continue;
            const chf_control &c = m_layout[idx];
            const bool on = lit(c);
            const QRectF r = rectOf(idx);
            p.setPen(QPen(on ? chfAccentDarkColor() : kPlateEdge, on ? 1.5 : 1));
            p.setBrush(on ? chfAccentColor() : kPlate);
            p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 8, 8);
            const QPointF ac = r.center() + QPointF(0, -r.height() * 0.12);
            const qreal ra = r.width() * 0.30;
            const QRectF arc(ac.x() - ra, ac.y() - ra, 2 * ra, 2 * ra);
            const QColor ink = on ? chfAccentInk() : kInk;
            QPainterPath path;
            if (!cw) {
                path.arcMoveTo(arc, 60);
                path.arcTo(arc, 60, 240);
            } else {
                path.arcMoveTo(arc, 120);
                path.arcTo(arc, 120, -240);
            }
            p.setPen(QPen(ink, 2.4, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            /* the head, at the end of the arc, pointing along it */
            const qreal endDeg = cw ? -120 : 300;
            const QPointF tip = ac + unit(endDeg) * ra;
            const qreal er = qDegreesToRadians(endDeg);
            /* the direction of travel: counter-clockwise on the left, clockwise on the right */
            const QPointF d = cw ? QPointF(std::sin(er), std::cos(er)) : QPointF(-std::sin(er), -std::cos(er));
            const QPointF perp(-d.y(), d.x());
            QPolygonF head;
            head << tip + d * 8 << tip - d * 1.5 + perp * 4.2 << tip - d * 1.5 - perp * 4.2;
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawPolygon(head);
            /* the label */
            p.setPen(ink);
            if (showBind() && c.target >= 0) {
                /* the legend may run a little past the arrow's plate */
                drawFitted(p, QRectF(r.left() - 5, r.bottom() - 40, r.width() + 10, 34),
                           bindingText(m_session, c.target), scaledFont(font(), 0.72, true));
            } else {
                p.setFont(scaledFont(font(), 0.72, true));
                p.drawText(QRectF(r.left(), r.bottom() - 26, r.width(), 20), Qt::AlignCenter,
                           cw ? QStringLiteral("CW") : QStringLiteral("CCW"));
            }
        }

        /* the stick: eight wedges round the knob */
        const qreal R = pd.radius, r0 = R * 0.5;
        p.setPen(QPen(kPlateEdge, 1.5));
        p.setBrush(QColor(0x1B, 0x1A, 0x1A));
        p.drawEllipse(pd.centre, R + 3, R + 3);
        for (int k = 0; k < 8; ++k) {
            if (pd.dir[k] < 0) continue;
            const chf_control &c = m_layout[pd.dir[k]];
            const bool on = lit(c);
            const QPainterPath wedge = wedgePath(pd.centre, r0 + 1, R, k);
            p.setPen(QPen(on ? chfAccentDarkColor() : kPlateEdge, 1));
            p.setBrush(on ? chfAccentColor() : kPlate);
            p.drawPath(wedge);
            const QPointF mid = pd.centre + unit(k * 45.0) * ((r0 + R) / 2);
            if (showBind() && c.target >= 0) {
                p.setPen(on ? chfAccentInk() : kInk);
                drawFitted(p, QRectF(mid.x() - 24, mid.y() - 13, 48, 26), bindingText(m_session, c.target),
                           scaledFont(font(), 0.7, true));
            } else {
                /* a chevron pointing out */
                const QPointF dir = unit(k * 45.0), perp(-dir.y(), dir.x());
                const qreal s = k % 2 ? 3.5 : 5;
                QPolygonF chev;
                chev << mid + dir * s << mid - dir * s + perp * (s * 1.3) << mid - dir * s - perp * (s * 1.3);
                p.setPen(Qt::NoPen);
                p.setBrush(on ? chfAccentInk() : kDimInk);
                p.drawPolygon(chev);
            }
        }
        /* the hub the knob sits in */
        p.setPen(QPen(kPlateEdge, 1.5));
        p.setBrush(QColor(0x14, 0x13, 0x13));
        p.drawEllipse(pd.centre, r0 - 1, r0 - 1);
        paintKnob(p, pd, held);

        /* the side view: PULL above, PUSH below, the handle between */
        for (int pull = 0; pull < 2; ++pull) {
            const int idx = pull ? pd.pull : pd.push;
            if (idx < 0) continue;
            const chf_control &c = m_layout[idx];
            const bool on = lit(c);
            const QRectF r = rectOf(idx).adjusted(2, 2, -2, -2);
            p.setPen(QPen(on ? chfAccentDarkColor() : kPlateEdge, on ? 1.5 : 1));
            p.setBrush(on ? chfAccentColor() : kPlate);
            p.drawRoundedRect(r, 7, 7);
            const QColor ink = on ? chfAccentInk() : kInk;
            const QPointF a(r.left() + 14, r.center().y());
            QPolygonF arrow;
            if (pull) arrow << a + QPointF(0, -7) << a + QPointF(-6, 5) << a + QPointF(6, 5);
            else arrow << a + QPointF(0, 7) << a + QPointF(-6, -5) << a + QPointF(6, -5);
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawPolygon(arrow);
            p.setPen(ink);
            const QRectF text(r.left() + 24, r.top(), r.width() - 26, r.height());
            if (showBind() && c.target >= 0)
                drawFitted(p, text, bindingText(m_session, c.target), scaledFont(font(), 0.72, true));
            else {
                p.setFont(scaledFont(font(), 0.85, true));
                p.drawText(text, Qt::AlignCenter, local(c.label));
            }
        }
        if (pd.side >= 0) paintSide(p, rectOf(pd.side).adjusted(2, 0, -2, 0), held);
    }

    /* the knob from above: tilted toward the held direction, turned by the
     * twist, smaller pushed in and larger pulled up */
    void paintKnob(QPainter &p, const Pad &pd, unsigned held)
    {
        const qreal r0 = pd.radius * 0.5;
        auto bit = [held](int act) { return int((held >> act) & 1u); };
        qreal vx = bit(CHF_ACT_RIGHT) - bit(CHF_ACT_LEFT);
        qreal vy = bit(CHF_ACT_BACK) - bit(CHF_ACT_FORWARD);
        const qreal len = std::hypot(vx, vy);
        if (len > 1) { vx /= len; vy /= len; }
        const bool pull = (held >> CHF_ACT_PULL) & 1, push = (held >> CHF_ACT_PUSH) & 1;
        const bool ccw = (held >> CHF_ACT_TWIST_CCW) & 1, cw = (held >> CHF_ACT_TWIST_CW) & 1;
        const QPointF c = pd.centre + QPointF(vx, vy) * (r0 * 0.40);
        const qreal scale = 1.0 + (pull ? 0.2 : 0.0) - (push ? 0.2 : 0.0);
        const qreal rk = r0 * 0.78 * scale;
        const qreal angle = (cw ? 25.0 : 0.0) - (ccw ? 25.0 : 0.0);

        /* its shadow grows as the knob rises */
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawEllipse(c + QPointF(1.5, 2.5 + (pull ? 3 : 0)), rk + (pull ? 2 : 0), rk + (pull ? 2 : 0));

        p.save();
        p.translate(c);
        p.rotate(angle);
        QRadialGradient g(QPointF(-rk * 0.3, -rk * 0.35), rk * 1.5);
        g.setColorAt(0, QColor(0x6A, 0x68, 0x66));
        g.setColorAt(1, QColor(0x1E, 0x1D, 0x1D));
        p.setBrush(g);
        p.setPen(QPen(pull || push ? chfAccentColor() : QColor(0x0A, 0x0A, 0x0A), pull || push ? 2.5 : 1.2));
        p.drawEllipse(QPointF(0, 0), rk, rk);
        /* the finger grip, a bar across the knob, so a twist shows */
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x90, 0x8D, 0x89));
        p.drawRoundedRect(QRectF(-rk * 0.10, -rk * 0.78, rk * 0.20, rk * 0.62), 2, 2);
        p.setBrush(QColor(0x2A, 0x29, 0x29));
        p.drawEllipse(QPointF(0, 0), rk * 0.16, rk * 0.16);
        p.restore();
    }

    /* the handle from the side: its knob rises when pulled, sinks when pushed */
    void paintSide(QPainter &p, const QRectF &r, unsigned held)
    {
        const bool pull = (held >> CHF_ACT_PULL) & 1, push = (held >> CHF_ACT_PUSH) & 1;
        const int level = pull && !push ? 1 : push && !pull ? -1 : 0;
        p.setPen(QPen(kPlateEdge, 1));
        p.setBrush(QColor(0x17, 0x16, 0x16));
        p.drawRoundedRect(r, 7, 7);
        /* the three heights, as ticks */
        const qreal top = r.top() + 10, bottom = r.bottom() - 15;
        const qreal ys[3] = { top, (top + bottom) / 2, bottom - 4 };   /* up, middle, down */
        const int cur = level > 0 ? 0 : level < 0 ? 2 : 1;
        for (int i = 0; i < 3; ++i) {
            p.setPen(QPen(i == cur ? chfAccentColor() : QColor(0x55, 0x53, 0x50), i == cur ? 2 : 1));
            p.drawLine(QPointF(r.left() + 6, ys[i]), QPointF(r.left() + 13, ys[i]));
        }
        const qreal cx = r.center().x() + 4;
        /* the base, the shaft, the knob */
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x4A, 0x48, 0x46));
        p.drawRoundedRect(QRectF(cx - 17, r.bottom() - 9, 34, 6), 2, 2);
        const qreal ky = ys[cur];
        p.setBrush(QColor(0x7A, 0x77, 0x73));
        p.drawRect(QRectF(cx - 3, ky, 6, r.bottom() - 9 - ky));
        QRadialGradient g(QPointF(cx - 4, ky - 3), 20);
        g.setColorAt(0, QColor(0x74, 0x72, 0x70));
        g.setColorAt(1, QColor(0x22, 0x21, 0x21));
        p.setBrush(g);
        p.setPen(QPen(level != 0 ? chfAccentColor() : QColor(0x0A, 0x0A, 0x0A), level != 0 ? 2 : 1));
        p.drawEllipse(QPointF(cx, ky), 14, 6);
    }

    /* the app's own actions, along the bottom, while Map mode shows bindings */
    void paintActions(QPainter &p)
    {
        if (!showActions()) return;
        for (int sa = 0; sa < CHF_SYSACT_COUNT; ++sa) {
            const int target = CHF_TARGET_SYSACT(sa);
            const bool on = mapping() && m_map == target;
            const QRectF r = actionRect(sa);
            p.setPen(QPen(on ? chfAccentDarkColor() : kPlateEdge, on ? 1.5 : 1));
            p.setBrush(on ? chfAccentColor() : kPlate);
            p.drawRoundedRect(r, 8, 8);
            p.setPen(on ? chfAccentInk() : kInk);
            QString text = local(chf_target_short_name(target));
            const QString b = bindingText(m_session, target).replace(QLatin1Char('\n'), QStringLiteral(" · "));
            drawFitted(p, r.adjusted(6, 0, -6, 0), text + QStringLiteral("  ") + b, scaledFont(font(), 0.78, true));
        }
    }

    chfsession *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    const chf_control *m_layout = nullptr;
    int m_n = 0;
    Pad m_pad[CHF_PLAYERS];
    qreal m_blockTop = 0;
    unsigned m_held[CHF_PLAYERS] = {};
    char m_keyHeld[CHF_KEY_COUNT] = {};
    char m_resetHeld = 0;
    unsigned m_ptrMask[CHF_PLAYERS] = {};
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;          /* the part the mouse holds */
    int m_holdTarget = -1;       /* ... and its target, while it is down on it (buttons) */
    int m_dragPort = -1;         /* the stick the mouse began on */
    unsigned m_capMask = 0;      /* what a press on the knob holds: push, pull, or nothing */
    int m_wheelAcc = 0;
    int m_panelW = 0, m_panelH = 0;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(chfsession *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const char *stolen);
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    chfsession *m_session;
    ConsoleWidget *m_console = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "drives player N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(chfsession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("chfHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_console = new ConsoleWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *gv = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowH = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowH * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    gv->addWidget(m_padArea);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        chfsession_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key and gamepad button each part has"));
    bindings->setChecked(chfsession_get_int(session, "controllers_bindings", 0) != 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool on) {
        chfsession_set_int(m_session, "controllers_bindings", on ? 1 : 0);
        m_console->setShowBindings(on);
    });
    m_console->setShowBindings(bindings->isChecked());
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(bindings);
    maprow->addStretch(1);
    QAbstractButton *help = chfHelpButton(QStringLiteral("controllers"));
    maprow->addWidget(help);
    root->addLayout(maprow);
    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    pollHeld();
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
    if (state == -2) {
        m_captureTimer.stop();
        chfsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Press a part with the mouse to hold it. Drag on the knob to tilt, "
                                       "left-press to push, right-press to pull; the wheel twists."));
    } else if (state == -1) {
        m_captureTimer.stop();
        chfsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(chfAccentFill());
        m_hint->setText(QStringLiteral("Click a part to remap it. Escape leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(chf_target_name(state))));
        chfsession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    m_hint->setText(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3)").arg(local(chf_target_name(target)), what, local(stolen))
                              : QStringLiteral("%1 is now %2").arg(local(chf_target_name(target)), what));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!chfsession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    chfsession_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(chf_pad_button_name(button)), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* What the console sees held, from every source, lit (25 Hz while visible). */
void ControllersWindow::pollHeld()
{
    m_console->pollHeld();
}

/* Hot-plugged pads and auto-assignment changes. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = chfsession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPorts()
{
    for (int i = 0; i < (int)m_padEff.size(); ++i) {
        const int eff = chfsession_gamepad_effective_port(m_session, i);
        const QString text = eff >= 0 ? QStringLiteral("drives player %1").arg(eff + 1) : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)i]->text() != text) m_padEff[(size_t)i]->setText(text);
    }
}

/* One row per pad: its name, the player it drives and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) delete w;
    m_padRows.clear();
    m_padEff.clear();
    const int n = chfsession_gamepad_count(m_session);
    if (n == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < n && i < kMaxPadRows; ++i) {
        char name[128];
        chfsession_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0-1 = that player */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
        const int assign = chfsession_gamepad_assignment(m_session, i);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            chfsession_gamepad_assign(m_session, i, idx <= 0 ? -1 : idx - 1);
            refreshPadPorts();
        });
        h->addWidget(label, 1);
        h->addWidget(eff);
        h->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (n > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(n - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = chfsession_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    pollHeld();
    m_heldTimer.start(20);
    m_slowTimer.start(250);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(e);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        chfsession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_F9) {
        if (!e->isAutoRepeat()) hide();
        return;
    }
    if (e->key() == Qt::Key_F12) {
        if (!e->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    const uint32_t ks = chfKeysymFromQt(e);
    if (m_swallow && ks == m_swallow) return;

    if (m_mapState != -2) {
        if (e->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (e->key() == Qt::Key_Escape) {
            m_swallow = ks;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !ks) return;      /* armed, or nothing to bind: nothing is typed */
        if (chfIsAppKey(e)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 are the app's and cannot be bound."));
            return;
        }
        char stolen[128] = "", name[48] = "";
        const int target = m_mapState;
        chfsession_binding_set_key(m_session, target, ks, stolen, sizeof stolen);
        chfsession_keysym_name(ks, name, sizeof name);
        m_swallow = ks;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it; a system action goes to
     * the main window's queue, as a gamepad's does. */
    if (e->isAutoRepeat()) return;
    if (!ks || chfIsAppKey(e)) { QWidget::keyPressEvent(e); return; }
    const int sa = chfsession_key_sysaction(m_session, ks);
    if (sa >= 0) {
        chfsession_sysaction_post(m_session, sa);
        return;
    }
    if (!chfsession_key(m_session, ks, 1)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = chfKeysymFromQt(e);
    if (m_swallow && ks == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (m_mapState != -2 || !ks) return;
    if (!chfsession_key(m_session, ks, 0)) QWidget::keyReleaseEvent(e);
}

QPointer<ControllersWindow> g_window;

} // namespace

void chfControllersToggle(QWidget *parent, chfsession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

bool chfControllersVisible()
{
    return g_window && g_window->isVisible();
}

void chfControllersRefresh()
{
    if (g_window) g_window->refresh();
}
