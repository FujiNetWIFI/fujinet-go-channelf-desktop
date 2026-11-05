/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The console as photographed: a charcoal case with a wood strip, two hand
 * controllers (a gold disc over a gold 12-key keypad, the fire button on the
 * side) and the START / OPTION / SELECT / RESET keys between them. The keys
 * are painted from the session's layout table (chfsession_keyboard_layout:
 * every cap at its x/y/w/h in key units), so every frontend shows the same
 * keys.
 *
 * Keys and disc controls act on mouse press AND release, not "clicked": the
 * console scans its keypads and samples its pots once a frame, so a press
 * present only for the instant of a click would fall between frames. A key
 * is HELD while the mouse is down on it; dragging off it releases it (on a
 * disc, dragging moves from one direction to the next), and so do hiding the
 * window and losing the focus (only what the mouse holds here: the main
 * window releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent (the keypad's gold), so the window doubles as an input
 * tester for a new pad; the disc's knob shows the pot position, analog from a
 * pad's stick. Keys typed with it active go to the console as in the main
 * window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per key unit, Map
 * legends are elided into the caps, and gamepad names are elided, so neither
 * Map mode nor a long name resizes the window. A singleton, hidden rather
 * than destroyed, so a half-done remap survives closing it. A timer runs only
 * while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a key cap, a disc direction or
 * a fire button, then press a host key or a gamepad button. Rebinding steals
 * the key from whatever held it (named in the hint). Esc cancels; while Map is
 * armed nothing is typed into the console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cmath>
#include "ControllersWindow.h"

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
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"

namespace {

constexpr int kUnit = 42;        /* pixels per key unit */
constexpr int kLeft = 48;        /* the keys' origin inside the case (room for the fire buttons) */
constexpr int kWood = 16;        /* the wood strip along the top */
constexpr int kDiscTop = 36;
constexpr int kDiscR = 56;       /* the disc's radius */
constexpr int kKeysTop = kDiscTop + 2 * kDiscR + 18;
constexpr int kBottom = 40;
constexpr int kMaxPads = 8;

const QString kGray = QStringLiteral("color: gray;");

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

/* A cap's binding target: its matrix position, or the console's RESET. */
int capTarget(const chf_keycap &c)
{
    return c.target >= 0 ? CHF_TARGET_KEY(c.target) : CHF_TARGET_SWITCH(CHF_SW_RESET);
}

/* Whether the console sees `target` held now, from any source, or the mouse
 * is holding it here. */
bool heldNow(chfsession *s, int target, int mouseTarget)
{
    if (target == mouseTarget) return true;
    if (target < 2 * CHF_ACT_PER_PORT)
        return ((chfsession_buttons_held(s, target / CHF_ACT_PER_PORT) >> (target % CHF_ACT_PER_PORT)) & 1u) != 0;
    if (target < CHF_TARGET_SWITCH(0)) return chfsession_key_held(s, target - CHF_TARGET_KEY(0)) != 0;
    return chfsession_switch_held(s, target - CHF_TARGET_SWITCH(0)) != 0;
}

QString keyName(chfsession *s, int target)
{
    const chf_binding b = chfsession_binding_get(s, target);
    char key[48] = "";
    if (b.keysym) chfsession_keysym_name(b.keysym, key, sizeof key);
    return local(key);
}

/* Map mode on a control: the key over the gamepad button. */
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

/* ---- ConsoleWidget: the console, as photographed ------------------------------------- */

/* The charcoal case with its wood strip, the START / OPTION / SELECT / RESET
 * keys between, and the two hand controllers: each a gold disc over a gold
 * 12-key keypad, with the side fire button on its outer edge. The caps come
 * from the session's layout (the keypads and the panel keys); the discs and
 * the fire buttons are drawn here. */
class ConsoleWidget : public QWidget {
public:
    ConsoleWidget(chfsession *session, std::function<void(int, bool)> onPress)
        : m_session(session), m_onPress(std::move(onPress))
    {
        setFocusPolicy(Qt::NoFocus);
        load();
    }

    /* Lets go of the disc a drag holds (also on focus loss and rebuild). */
    void releaseDisc()
    {
        if (m_analogDisc < 0) return;
        chfsession_disc_set(m_session, m_analogDisc, 0, 0);
        m_analogDisc = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setKeyNames(bool on)
    {
        m_keyNames = on;
        update();
    }
    /* What the console sees held (and the control the mouse holds); repaints
     * only on a change. */
    void pollHeld(int mouseTarget)
    {
        bool changed = false;
        for (size_t i = 0; i < m_caps.size(); ++i) {
            const char on = heldNow(m_session, m_caps[i].target, mouseTarget);
            if (on != m_held[i]) {
                m_held[i] = on;
                changed = true;
            }
        }
        for (int port = 0; port < 2; ++port) {
            for (int act = 0; act < CHF_ACT_PER_PORT; ++act) {
                const char on = heldNow(m_session, CHF_TARGET_PORT(port, act), mouseTarget);
                if (on != m_dheld[port][act]) {
                    m_dheld[port][act] = on;
                    changed = true;
                }
            }
            int x = 0, y = 0;
            chfsession_disc_get(m_session, port, &x, &y);
            if (x != m_dx[port] || y != m_dy[port]) {
                m_dx[port] = x;
                m_dy[port] = y;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paintCase(p);
        for (int port = 0; port < 2; ++port) paintController(p, port);
        for (size_t i = 0; i < m_caps.size(); ++i) paintCap(p, i);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int disc = discPortAt(e->position());
        if (disc >= 0 && m_map == -2 && !(m_caps.size() && capAt(e->position()) >= 0)) {
            /* a drag on a disc is analog: the pots follow the pointer */
            m_analogDisc = disc;
            discFollow(e->position());
            return;
        }
        const int t = targetAt(e->position(), -1);
        if (t < 0) return;
        m_pressed = t;
        m_pressedDisc = disc;
        m_onPress(t, true);
    }

    /* Dragging off a key releases it; on a disc it follows the pointer from
     * one direction to the next. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_analogDisc >= 0) { discFollow(e->position()); return; }
        if (m_pressed < 0) return;
        const int t = targetAt(e->position(), m_pressedDisc);
        if (t == m_pressed) return;
        const int old = m_pressed;
        m_pressed = -1;
        m_onPress(old, false);
        if (t >= 0 && m_pressedDisc >= 0) {
            m_pressed = t;
            m_onPress(t, true);
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_analogDisc >= 0) { releaseDisc(); return; }
        if (e->button() != Qt::LeftButton || m_pressed < 0) return;
        const int t = m_pressed;
        m_pressed = -1;
        m_pressedDisc = -1;
        m_onPress(t, false);
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int t = targetAt(he->pos(), -1);
            if (t < 0)
                QToolTip::hideText();
            else
                QToolTip::showText(he->globalPos(), bindingTip(m_session, t), this);
            return true;
        }
        return QWidget::event(e);
    }

private:
    struct Cap {
        chf_keycap cap;
        int target;
        QRectF rect;
    };

    void load()
    {
        int n = 0;
        float w = 0, h = 0;
        const chf_keycap *caps = chfsession_keyboard_layout(CHF_MACHINE_ARCADIA, &n, &w, &h);
        m_caps.clear();
        for (int i = 0; i < n; ++i) {
            Cap c;
            c.cap = caps[i];
            c.target = capTarget(caps[i]);
            c.rect = QRectF(kLeft + caps[i].x * kUnit, kKeysTop + caps[i].y * kUnit, caps[i].w * kUnit,
                            caps[i].h * kUnit);
            m_caps.push_back(c);
        }
        m_held.assign(m_caps.size(), 0);
        m_panelW = qCeil(w * kUnit) + 2 * kLeft;
        m_panelH = kKeysTop + qCeil(h * kUnit) + kBottom;
        setFixedSize(m_panelW, m_panelH);
    }

    /* The controller's keypad (the caps of `port`) and its disc. */
    QRectF keypadRect(int port) const
    {
        QRectF r;
        for (const Cap &c : m_caps)
            if (c.cap.port == port) r = r.isNull() ? c.rect : r.united(c.rect);
        return r;
    }
    QPointF discCentre(int port) const
    {
        const QRectF k = keypadRect(port);
        return QPointF(k.center().x(), kDiscTop + kDiscR);
    }
    QRectF fireRect(int port) const
    {
        const QRectF k = keypadRect(port);
        const qreal w = 24;
        const qreal x = port == 0 ? k.left() - w - 10 : k.right() + 10;
        return QRectF(x, k.top() + 8, w, k.height() - 16);
    }
    /* The panel's red RESET sits at the centre in the layout: a cap of its own. */

    int capAt(const QPointF &pos) const
    {
        for (const Cap &c : m_caps)
            if (c.rect.contains(pos)) return c.target;
        return -1;
    }

    /* The pointer's place on the disc is the analog deflection: the offset
     * from the centre over the radius, clamped to the rim, with a small dead
     * zone, so a corner is a true diagonal. */
    void discFollow(const QPointF &pos)
    {
        const QPointF d = (pos - discCentre(m_analogDisc)) / kDiscR;
        qreal len = std::hypot(d.x(), d.y());
        QPointF n = d;
        if (len > 1.0) { n = d / len; len = 1.0; }
        int ax = 0, ay = 0;
        if (len > 0.12) {
            ax = int(std::lround(n.x() * 127.0));
            ay = int(std::lround(n.y() * 127.0));
        }
        chfsession_disc_set(m_session, m_analogDisc, ax, ay);
        update();
    }

    /* Which disc `pos` is over (inside its circle, or a bit past), or -1. */
    int discPortAt(const QPointF &pos) const
    {
        for (int port = 0; port < 2; ++port) {
            const QPointF d = pos - discCentre(port);
            if (QPointF::dotProduct(d, d) <= (kDiscR + 6.0) * (kDiscR + 6.0)) return port;
        }
        return -1;
    }

    /* The direction a point on a disc means: the dominant axis, nothing
     * inside the hub unless mapping (a click on a wedge picks it). */
    int discTarget(int port, const QPointF &pos) const
    {
        const QPointF d = pos - discCentre(port);
        const qreal ax = qAbs(d.x()), ay = qAbs(d.y());
        if (m_map == -2 && ax < 14 && ay < 14) return -1;
        int act;
        if (ax > ay) act = d.x() > 0 ? CHF_ACT_RIGHT : CHF_ACT_LEFT;
        else act = d.y() > 0 ? CHF_ACT_DOWN : CHF_ACT_UP;
        return CHF_TARGET_PORT(port, act);
    }

    /* The control under pos, or -1. With `discOnly` >= 0 only that disc's
     * directions count (a drag that began on it). */
    int targetAt(const QPointF &pos, int discOnly) const
    {
        if (discOnly >= 0) {
            const QPointF d = pos - discCentre(discOnly);
            if (QPointF::dotProduct(d, d) > 4.0 * kDiscR * kDiscR) return -1;
            return discTarget(discOnly, pos);
        }
        for (const Cap &c : m_caps)
            if (c.rect.contains(pos)) return c.target;
        for (int port = 0; port < 2; ++port) {
            if (fireRect(port).contains(pos)) return CHF_TARGET_PORT(port, CHF_ACT_FIRE);
            if (discPortAt(pos) == port) return discTarget(port, pos);
        }
        return -1;
    }

    void paintCase(QPainter &p)
    {
        /* the charcoal case, a wood strip along the top, the gold name plate */
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient charcoal(0, 0, 0, height());
        charcoal.setColorAt(0, QColor(0x3A, 0x3A, 0x3C));
        charcoal.setColorAt(1, QColor(0x1E, 0x1E, 0x20));
        p.setPen(QColor(0x0C, 0x0C, 0x0D));
        p.setBrush(charcoal);
        p.drawRoundedRect(body, 14, 14);

        QPainterPath clip;
        clip.addRoundedRect(body, 14, 14);
        p.save();
        p.setClipPath(clip);
        const QRectF strip(0, 0, width(), kWood);
        QLinearGradient wood(strip.topLeft(), strip.bottomLeft());
        wood.setColorAt(0, QColor(0x9A, 0x66, 0x3A));
        wood.setColorAt(0.5, QColor(0x7C, 0x4C, 0x28));
        wood.setColorAt(1, QColor(0x5E, 0x38, 0x1C));
        p.setPen(Qt::NoPen);
        p.setBrush(wood);
        p.drawRect(strip);
        p.setPen(QPen(QColor(0x4A, 0x2C, 0x16, 120), 1));      /* the grain */
        for (int y = 5; y < kWood; y += 6) p.drawLine(QPointF(0, y), QPointF(width(), y + 1));
        p.restore();

        const QRectF mid(keypadRect(0).right() + 14, kWood + 16, keypadRect(1).left() - keypadRect(0).right() - 28, 56);
        p.setPen(chfAccentColor());
        drawFitted(p, mid, QStringLiteral("CHANNELF\n2001"), scaledFont(font(), 1.5, true));
        p.setPen(QColor(0x9A, 0x9A, 0xA0));
        p.setFont(scaledFont(font(), 0.65, true));
        p.drawText(mid.translated(0, 58), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("with FujiNet"));
    }

    void paintController(QPainter &p, int port)
    {
        const bool mapping = m_map != -2;
        const QRectF k = keypadRect(port);
        /* the controller body behind its keypad and disc */
        const QRectF body(k.left() - 16 - (port == 0 ? 36 : 0), kWood + 8, k.width() + 32 + 36, height() - kWood - 16);
        (void)body;

        /* the keypad's tray */
        p.setPen(QColor(0, 0, 0));
        p.setBrush(QColor(0x14, 0x14, 0x16));
        p.drawRoundedRect(k.adjusted(-8, -8, 8, 8), 8, 8);

        /* the disc: a gold dish, four wedges, a hub, and a knob that leans the
         * way the pot is */
        const QPointF c = discCentre(port);
        QRadialGradient dish(c + QPointF(-12, -14), kDiscR * 1.5);
        dish.setColorAt(0, QColor(0xC8, 0xAE, 0x82));
        dish.setColorAt(1, QColor(0x7A, 0x63, 0x40));
        p.setPen(QPen(QColor(0x1A, 0x14, 0x0A), 1.5));
        p.setBrush(dish);
        p.drawEllipse(c, kDiscR, kDiscR);

        const QFont small = scaledFont(font(), 0.62, true);
        for (int act = 0; act < 4; ++act) {
            const int target = CHF_TARGET_PORT(port, act);
            const bool lit = mapping ? target == m_map : m_dheld[port][act] != 0;
            /* the wedge: a 90-degree sector centred on its direction */
            const int centreDeg = act == CHF_ACT_UP ? 90 : act == CHF_ACT_RIGHT ? 0 : act == CHF_ACT_DOWN ? 270 : 180;
            QPainterPath wedge;
            wedge.moveTo(c);
            wedge.chfTo(QRectF(c.x() - kDiscR + 3, c.y() - kDiscR + 3, 2 * kDiscR - 6, 2 * kDiscR - 6),
                        centreDeg - 45, 90);
            wedge.closeSubpath();
            if (lit) {
                p.setPen(Qt::NoPen);
                p.setBrush(chfAccentColor().lighter(135));
                p.drawPath(wedge);
            }
            const QPointF dir(act == CHF_ACT_RIGHT ? 1 : act == CHF_ACT_LEFT ? -1 : 0,
                              act == CHF_ACT_DOWN ? 1 : act == CHF_ACT_UP ? -1 : 0);
            const QPointF tip = c + dir * (kDiscR - 14);
            if (mapping) {
                p.setPen(QColor(0x1B, 0x14, 0x08));
                const QPointF at = c + dir * (dir.x() != 0 ? kDiscR - 30 : kDiscR - 24);
                const qreal w = dir.x() != 0 ? 46 : 70;
                drawFitted(p, QRectF(at.x() - w / 2, at.y() - 13, w, 26), bindingText(m_session, target), small);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(lit ? QColor(0x1B, 0x14, 0x08) : QColor(0x4A, 0x3A, 0x20));
                QPolygonF tri;
                const QPointF perp(-dir.y(), dir.x());
                tri << tip + dir * 5 << tip - dir * 5 + perp * 7 << tip - dir * 5 - perp * 7;
                p.drawPolygon(tri);
            }
        }
        p.setPen(QPen(QColor(0x1A, 0x14, 0x0A, 160), 1));
        p.drawLine(c + QPointF(-kDiscR + 3, 0), c + QPointF(kDiscR - 3, 0));
        p.drawLine(c + QPointF(0, -kDiscR + 3), c + QPointF(0, kDiscR - 3));

        /* the knob: where the pot is (analog when a pad drives it, else the
         * digital ends) */
        qreal dx = m_dx[port] / 127.0, dy = m_dy[port] / 127.0;
        if (m_dx[port] == 0 && m_dy[port] == 0) {
            dx = (m_dheld[port][CHF_ACT_RIGHT] ? 1 : 0) - (m_dheld[port][CHF_ACT_LEFT] ? 1 : 0);
            dy = (m_dheld[port][CHF_ACT_DOWN] ? 1 : 0) - (m_dheld[port][CHF_ACT_UP] ? 1 : 0);
        }
        const QPointF knobAt = c + QPointF(dx, dy) * (kDiscR - 24);
        QRadialGradient knob(knobAt + QPointF(-4, -4), 20);
        knob.setColorAt(0, QColor(0x4C, 0x4C, 0x52));
        knob.setColorAt(1, QColor(0x14, 0x14, 0x16));
        p.setPen(QColor(0, 0, 0));
        p.setBrush(knob);
        p.drawEllipse(knobAt, 15, 15);

        /* the side fire button */
        const int ft = CHF_TARGET_PORT(port, CHF_ACT_FIRE);
        const bool flit = mapping ? ft == m_map : m_dheld[port][CHF_ACT_FIRE] != 0;
        const QRectF fr = fireRect(port);
        QLinearGradient fg(fr.topLeft(), fr.topRight());
        const QColor ff = flit ? chfAccentColor().lighter(135) : QColor(0xB0, 0x30, 0x34);
        fg.setColorAt(0, ff.lighter(120));
        fg.setColorAt(1, ff.darker(115));
        p.setPen(flit ? QPen(QColor(0xFF, 0xF4, 0xDC), 1.5) : QPen(QColor(0, 0, 0), 1));
        p.setBrush(fg);
        p.drawRoundedRect(fr, 8, 8);
        p.setPen(flit ? QColor(0x1B, 0x14, 0x08) : Qt::white);
        /* the legend runs up the button */
        p.save();
        p.translate(fr.center());
        p.rotate(-90);
        const QRectF rot(-fr.height() / 2, -fr.width() / 2, fr.height(), fr.width());
        if (mapping) {
            drawFitted(p, rot.adjusted(4, 0, -4, 0), bindingText(m_session, ft).replace(QLatin1Char('\n'), QLatin1Char(' ')), small);
        } else {
            p.setFont(small);
            p.drawText(rot, Qt::AlignCenter, QStringLiteral("FIRE"));
        }
        p.restore();
        p.setPen(QColor(0x9A, 0x9A, 0xA0));
        p.setFont(small);
        p.drawText(QRectF(k.left(), k.bottom() + 8, k.width(), 14), Qt::AlignCenter,
                   QStringLiteral("CONTROLLER %1").arg(port + 1));
    }

    void paintCap(QPainter &p, size_t i)
    {
        const Cap &c = m_caps[i];
        const bool mapping = m_map != -2;
        const bool lit = mapping ? c.target == m_map : m_held[i] != 0;
        const bool reset = c.cap.kind == CHF_KEYCAP_RESET;
        const bool panel = c.cap.port < 0;
        const QRectF r = c.rect.adjusted(0.5, 0.5, -0.5, -2.5);

        QColor top, bottom, legend, side;
        if (lit) {
            top = chfAccentColor().lighter(135);
            bottom = chfAccentColor();
            legend = QColor(0x1B, 0x14, 0x08);
            side = chfAccentDarkColor();
        } else if (reset) {
            top = QColor(0xC0, 0x38, 0x3C);
            bottom = QColor(0x8E, 0x1E, 0x24);
            legend = Qt::white;
            side = QColor(0x4A, 0x0C, 0x10);
        } else if (panel) {
            top = QColor(0x5A, 0x5A, 0x60);
            bottom = QColor(0x3A, 0x3A, 0x3E);
            legend = QColor(0xE6, 0xD3, 0xA8);
            side = QColor(0x16, 0x16, 0x18);
        } else {
            top = QColor(0xB8, 0x9E, 0x74);
            bottom = QColor(0x8C, 0x74, 0x4E);
            legend = QColor(0x1B, 0x14, 0x08);
            side = QColor(0x4E, 0x3E, 0x24);
        }
        /* the cap's side, then its face */
        p.setPen(Qt::NoPen);
        p.setBrush(side);
        p.drawRoundedRect(r.translated(0, 2.5), 6, 6);
        QLinearGradient face(r.topLeft(), r.bottomLeft());
        face.setColorAt(0, top);
        face.setColorAt(1, bottom);
        p.setBrush(face);
        p.setPen(lit ? QPen(QColor(0xFF, 0xF4, 0xDC), 1.5) : QPen(QColor(0x1A, 0x14, 0x0A), 0.8));
        p.drawRoundedRect(r, 6, 6);

        p.setPen(legend);
        const QRectF in = r.adjusted(2, 1, -2, -1);
        const QString label = QString::fromUtf8(c.cap.label);
        if (mapping) {
            drawFitted(p, in, bindingText(m_session, c.target), scaledFont(font(), 0.7, true));
            return;
        }
        if (reset) {
            /* the console's reset key carries a triangle, not a name */
            const QPointF c0 = in.center() + QPointF(0, m_keyNames ? -3 : 0);
            p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(QPolygonF({ c0 + QPointF(0, -7), c0 + QPointF(-8, 6), c0 + QPointF(8, 6) }));
        } else {
            drawFitted(p, in.adjusted(0, m_keyNames ? -3 : 0, 0, 0), label,
                       scaledFont(font(), label.size() > 1 ? 0.72 : 1.15, true));
        }
        if (m_keyNames) {
            const QString name = keyName(m_session, c.target);
            if (!name.isEmpty()) {
                const QFont nf = scaledFont(font(), 0.6);
                p.setFont(nf);
                p.setPen(lit ? QColor(0x1B, 0x14, 0x08, 200) : (panel ? QColor(255, 255, 255, 170) : QColor(0x2A, 0x20, 0x10, 200)));
                p.drawText(in.adjusted(0, 0, -1, 0), Qt::AlignRight | Qt::AlignBottom,
                           QFontMetrics(nf).elidedText(name, Qt::ElideRight, qFloor(in.width())));
            }
        }
    }

    chfsession *m_session;
    std::function<void(int, bool)> m_onPress;
    int m_analogDisc = -1;
    std::vector<Cap> m_caps;
    std::vector<char> m_held;
    char m_dheld[2][CHF_ACT_PER_PORT] = {};
    int m_dx[2] = {}, m_dy[2] = {};
    bool m_keyNames = false;
    int m_map = -2;
    int m_pressed = -1;          /* the target the mouse holds */
    int m_pressedDisc = -1;      /* ... and the disc it began on, if any */
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
    void rebuild();
    void onPress(int target, bool down);
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
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "drives controller N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QCheckBox *m_keyNames = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    int m_mouseTarget = -1;               /* the target the mouse holds here, or -1 */
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(chfsession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size keys and buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its controls and kept there: a resizable panel gets tiled and
     * its keys stretched, and nothing here wants more room. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto press = [this](int target, bool down) { onPress(target, down); };
    m_console = new ConsoleWidget(session, press);
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    m_padBox = new QVBoxLayout(pads);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        chfsession_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every key and control is back to its default."));
    });
    m_keyNames = new QCheckBox(QStringLiteral("Key names"));
    m_keyNames->setFocusPolicy(Qt::NoFocus);
    m_keyNames->setToolTip(QStringLiteral("Show the host key bound to each key cap"));
    m_keyNames->setChecked(chfsession_get_int(session, "controllers_key_names", 0) != 0);
    connect(m_keyNames, &QCheckBox::toggled, this, [this](bool on) {
        chfsession_set_int(m_session, "controllers_key_names", on ? 1 : 0);
        m_console->setKeyNames(on);
    });
    m_console->setKeyNames(m_keyNames->isChecked());
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_keyNames);
    maprow->addStretch(1);
    root->addLayout(maprow);
    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    rebuild();
}

void ControllersWindow::rebuild()
{
    letGo();
    pollHeld();
}

/* ---- pressing -------------------------------------------------------------------------- */

/* A key cap, a disc direction or a fire button was pressed or let go with the mouse. */
void ControllersWindow::onPress(int target, bool down)
{
    if (m_mapState != -2) {
        if (down) setMapState(target);    /* armed: picks (or changes) the target */
        return;
    }
    if (down) {
        letGo();
        m_mouseTarget = target;
        chfsession_press(m_session, target, 1);
    } else if (m_mouseTarget == target) {
        m_mouseTarget = -1;
        chfsession_press(m_session, target, 0);
    }
    pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_console->releaseDisc();
    if (m_mouseTarget >= 0) {
        chfsession_press(m_session, m_mouseTarget, 0);
        m_mouseTarget = -1;
    }
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
        m_hint->setText(QStringLiteral("Click and hold a key, a disc direction or a fire button to press it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        chfsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(chfAccentFill());
        m_hint->setText(QStringLiteral("Click a key cap, a disc direction or a fire button to remap it. Esc leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Esc cancels).")
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
    m_console->pollHeld(m_mouseTarget);
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
        const QString text = eff >= 0 ? QStringLiteral("drives controller %1").arg(eff + 1) : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)i]->text() != text) m_padEff[(size_t)i]->setText(text);
    }
}

/* One row per pad: its name, the controller it drives and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) delete w;
    m_padRows.clear();
    m_padEff.clear();
    const int n = chfsession_gamepad_count(m_session);
    if (n == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(kGray);
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < n && i < kMaxPads; ++i) {
        char name[128];
        chfsession_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(kGray);
        /* -1 = automatic (connection order), 0-1 = that controller */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Controller 1"), QStringLiteral("Controller 2") });
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
    if (isVisible()) rebuild();
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    pollHeld();
    m_heldTimer.start(40);
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
