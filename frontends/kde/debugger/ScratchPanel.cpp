/*
 * ScratchPanel -- see ScratchPanel.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ScratchPanel.h"

#include <cstring>

#include <QGroupBox>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QToolTip>
#include <QVBoxLayout>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;

namespace {

/* r9 is J, r10/r11 H, r12/r13 K, r14/r15 Q: the BIOS's names for the upper
 * (U) and lower (L) halves */
QString regName(int n)
{
    static const char *const names[] = { "J", "HU", "HL", "KU", "KL", "QU", "QL" };
    if (n >= 9 && n <= 15) return QStringLiteral("r%1 %2").arg(n).arg(QLatin1String(names[n - 9]));
    return QStringLiteral("r%1").arg(n);
}

QString octal3(unsigned v) { return QStringLiteral("%1").arg(v & 0xFF, 3, 8, QLatin1Char('0')); }

/* octal as the grid shows it by default; $ and 0x for hex */
bool parseByte(const QString &t, unsigned *out)
{
    const QString s = t.trimmed();
    bool ok = false;
    unsigned v;
    if (s.startsWith(QLatin1Char('$'))) v = s.mid(1).toUInt(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toUInt(&ok, 16);
    else v = s.toUInt(&ok, 8);
    if (ok && v <= 0xFF) *out = v;
    return ok && v <= 0xFF;
}

} // namespace

/* ---- the scratchpad ------------------------------------------------------------------ */

/* The 64 registers as an 8 x 8 grid of octal bytes: the ISAR's cell outlined,
 * the cells that changed at the last change in the accent, and r40-r59, where
 * the BIOS keeps its K stack, shaded. */
class ScratchGrid : public QWidget {
public:
    std::function<bool()> canEdit;
    std::function<void(int, unsigned)> onEdit;

    ScratchGrid()
    {
        setMouseTracking(true);
        setFixedSize(kHead + 8 * kW + 2, kHeadH + 8 * kH + 2);
        m_edit = new QLineEdit(this);
        m_edit->hide();
        m_edit->setFont(dbgui::monoFont(font()));
        connect(m_edit, &QLineEdit::returnPressed, this, [this] { commit(); });
        m_edit->installEventFilter(this);
    }

    void set(const uint8_t r[64], int isar)
    {
        if (m_have && memcmp(r, m_cur, 64) != 0) {
            /* a new state: what differs from the last is the news */
            for (int i = 0; i < 64; ++i) m_hot[i] = r[i] != m_cur[i];
        }
        if (!m_have || memcmp(r, m_cur, 64) != 0 || isar != m_isar) {
            memcpy(m_cur, r, 64);
            m_isar = isar;
            m_have = true;
            update();
        }
    }

protected:
    bool eventFilter(QObject *obj, QEvent *e) override
    {
        if (obj == m_edit && e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            m_edit->hide();
            return true;
        }
        if (obj == m_edit && e->type() == QEvent::FocusOut) m_edit->hide();
        return QWidget::eventFilter(obj, e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QFont mono = dbgui::monoFont(font());
        QFont tiny = font();
        tiny.setPointSizeF(qMax(6.5, font().pointSizeF() * 0.72));
        const QColor text = palette().color(QPalette::Text), base = palette().color(QPalette::Base);
        const QColor mid = palette().color(QPalette::Mid);
        QColor band = chfAccentTextColor(this);
        band.setAlpha(46);

        p.setFont(tiny);
        p.setPen(palette().color(QPalette::PlaceholderText));
        for (int c = 0; c < 8; ++c)
            p.drawText(QRect(kHead + c * kW, 0, kW, kHeadH), Qt::AlignCenter, QStringLiteral("+%1").arg(c));
        for (int r = 0; r < 8; ++r)
            p.drawText(QRect(0, kHeadH + r * kH, kHead - 6, kH), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("r%1").arg(r * 8));

        for (int n = 0; n < 64; ++n) {
            const QRect cell(kHead + (n % 8) * kW + 1, kHeadH + (n / 8) * kH + 1, kW - 1, kH - 1);
            const bool hot = m_have && m_hot[n];
            p.setPen(mid);
            p.setBrush(hot ? chfAccentColor() : base);
            p.drawRect(cell);
            if (!hot && n >= 40 && n <= 59) {
                p.setPen(Qt::NoPen);
                p.setBrush(band);
                p.drawRect(cell.adjusted(1, 1, 0, 0));
            }
            const QColor ink = hot ? chfAccentInk() : text;
            p.setFont(tiny);
            p.setPen(hot ? chfAccentInk() : palette().color(QPalette::PlaceholderText));
            p.drawText(cell.adjusted(3, 1, -2, 0), Qt::AlignLeft | Qt::AlignTop, regName(n));
            p.setFont(mono);
            p.setPen(ink);
            p.drawText(cell.adjusted(0, 6, -4, 0), Qt::AlignRight | Qt::AlignVCenter, octal3(m_cur[n]));
        }
        /* the cell the ISAR points at */
        if (m_have && m_isar >= 0 && m_isar < 64) {
            const QRect cell(kHead + (m_isar % 8) * kW + 1, kHeadH + (m_isar / 8) * kH + 1, kW - 1, kH - 1);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(text, 2));
            p.drawRect(cell.adjusted(1, 1, -1, -1));
        }
    }

    /* a click edits a cell (while the machine is stopped) */
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) edit(cellAt(e->position().toPoint()));
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        edit(cellAt(e->position().toPoint()));
    }
    void edit(int n)
    {
        if (n < 0) return;
        if (canEdit && !canEdit()) return;
        m_editing = n;
        const QRect cell(kHead + (n % 8) * kW + 1, kHeadH + (n / 8) * kH + 1, kW - 1, kH - 1);
        m_edit->setGeometry(cell);
        m_edit->setText(octal3(m_cur[n]));
        m_edit->show();
        m_edit->setFocus();
        m_edit->selectAll();
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int n = cellAt(he->pos());
            if (n < 0 || !m_have) {
                QToolTip::hideText();
            } else {
                const unsigned v = m_cur[n];
                QToolTip::showText(he->globalPos(),
                                   QStringLiteral("%1 = o%2 = $%3 = %4 = %5\nClick to edit (octal; $ for hex)")
                                       .arg(regName(n), octal3(v), hx(v, 2)).arg(v).arg(dbgui::bin8(v)), this);
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    int cellAt(const QPoint &pt) const
    {
        const int c = (pt.x() - kHead) / kW, r = (pt.y() - kHeadH) / kH;
        if (pt.x() < kHead || pt.y() < kHeadH || c < 0 || c > 7 || r < 0 || r > 7) return -1;
        return r * 8 + c;
    }

    void commit()
    {
        unsigned v;
        const int n = m_editing;
        m_edit->hide();
        if (n >= 0 && parseByte(m_edit->text(), &v) && onEdit) onEdit(n, v);
    }

    static constexpr int kHead = 34, kHeadH = 18, kW = 60, kH = 36;
    uint8_t m_cur[64] = {};
    bool m_hot[64] = {};
    bool m_have = false;
    int m_isar = -1;
    int m_editing = -1;
    QLineEdit *m_edit;
};

/* ---- the panel ------------------------------------------------------------------------ */

ScratchPanel::ScratchPanel(chfdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    setObjectName(QStringLiteral("scratchPanel"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto *scratch = new QGroupBox(QStringLiteral("Scratchpad (octal)"));
    auto *sv = new QVBoxLayout(scratch);
    m_grid = new ScratchGrid;
    m_grid->setObjectName(QStringLiteral("scratchGrid"));
    m_grid->canEdit = [this] { return !canEdit || canEdit(); };
    m_grid->onEdit = [this](int n, unsigned val) {
        if (onEdit) onEdit(n, val);
    };
    sv->addWidget(m_grid, 0, Qt::AlignLeft);
    sv->addWidget(dbgui::dimLabel(QStringLiteral("Outlined: where the ISAR points. Accent: changed at the last change. "
                                                 "Shaded: r40-r59, the BIOS's K stack. Click a cell to edit it "
                                                 "(octal; $ for hex).")));
    v->addWidget(scratch);

    auto *kbox = new QGroupBox(QStringLiteral("K stack"));
    auto *kv = new QVBoxLayout(kbox);
    m_kstack = dbgui::monoView();
    m_kstack->setObjectName(QStringLiteral("kstack"));
    m_kstack->setMinimumHeight(m_kstack->fontMetrics().lineSpacing() * 4 + 14);
    kv->addWidget(m_kstack);
    v->addWidget(kbox, 1);
}

void ScratchPanel::refresh(const chfdebug_cpu &c)
{
    m_grid->set(c.r, c.isar);

    /* the BIOS's K stack: r59 points past the saved returns in r40-r58 */
    QString k;
    if (c.k_depth < 0) {
        k = QStringLiteral("r59 = %1 (o%2): not a K-stack pointer (the BIOS's stack is not in use)")
                .arg(c.r[59]).arg(octal3(c.r[59]));
    } else {
        k = QStringLiteral("depth %1 (r59 = %2, o%3)\n").arg(c.k_depth).arg(c.r[59]).arg(octal3(c.r[59]));
        for (int i = c.k_depth - 1; i >= 0 && i < 10; --i) {
            char label[64] = "";
            chfdebug_address_label(m_dbg, c.k_stack[i], label, sizeof label);
            k += QStringLiteral("  %1: $%2 %3\n").arg(i).arg(hx(c.k_stack[i], 4), QString::fromUtf8(label));
        }
        if (c.k_depth == 0) k += QStringLiteral("  (empty)\n");
    }
    dbgui::setText(m_kstack, k.trimmed());
}
