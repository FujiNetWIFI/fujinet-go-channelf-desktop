/*
 * CpuTab -- see CpuTab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CpuTab.h"

#include <cstring>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

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

/* ---- the tab ------------------------------------------------------------------------- */

CpuTab::CpuTab(chfdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    auto *h = new QHBoxLayout(this);
    auto *left = new QVBoxLayout;
    left->addWidget(buildRegisters());

    auto *scratch = new QGroupBox(QStringLiteral("Scratchpad (octal)"));
    auto *sv = new QVBoxLayout(scratch);
    m_grid = new ScratchGrid;
    m_grid->canEdit = [this] { return stopped(QStringLiteral("a register")); };
    m_grid->onEdit = [this](int n, unsigned v) {
        chfdebug_cpu_set(m_dbg, CHF_REG_R0 + n, (int)v);
        if (onEdited) onEdited();
    };
    sv->addWidget(m_grid, 0, Qt::AlignLeft);
    sv->addWidget(dbgui::dimLabel(QStringLiteral("Outlined: where the ISAR points. Accent: changed at the last change. "
                                                 "Shaded: r40-r59, the BIOS's K stack. Click a cell to edit it.")));
    left->addWidget(scratch);

    auto *kbox = new QGroupBox(QStringLiteral("K stack"));
    auto *kv = new QVBoxLayout(kbox);
    m_kstack = dbgui::monoView();
    m_kstack->setMaximumHeight(m_kstack->fontMetrics().lineSpacing() * 6 + 14);
    kv->addWidget(m_kstack);
    left->addWidget(kbox);

    m_info = dbgui::dimLabel();
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left->addWidget(m_info);
    left->addStretch(1);
    h->addLayout(left);
    h->addWidget(buildMemory(), 1);
}

QWidget *CpuTab::buildRegisters()
{
    auto *box = new QGroupBox(QStringLiteral("Registers"));
    auto *g = new QGridLayout(box);
    struct Field { int reg; int digits; const char *name, *tip; };
    static const Field rows[3][5] = {
        { { CHF_REG_PC, 4, "PC", "The instruction about to run (the CPU has already fetched its opcode, so PC0 is one more)" },
          { CHF_REG_PC0, 4, "PC0", "The program counter as the bus sees it" },
          { CHF_REG_PC1, 4, "PC1", "The one return address: PI copies PC0 here, POP copies it back" },
          { CHF_REG_DC0, 4, "DC0", "The data counter" },
          { CHF_REG_DC1, 4, "DC1", "The alternate data counter (XDC swaps them)" } },
        { { CHF_REG_A, 2, "A", "The accumulator" },
          { CHF_REG_W, 2, "W", "Status: bit 0 S (1 = positive), 1 C, 2 Z, 3 O, 4 ICB" },
          { CHF_REG_ISAR, 3, "ISAR", "The scratchpad pointer, six bits, in octal as upper.lower (3.5 is r29)" },
          { -1, 2, "DBUS", "The data bus: the opcode just fetched" },
          { -1, 0, nullptr, nullptr } },
        { { CHF_REG_J, 2, "J", "r9" },
          { CHF_REG_H, 4, "H", "r10:r11" },
          { CHF_REG_K, 4, "K", "r12:r13" },
          { CHF_REG_Q, 4, "Q", "r14:r15" },
          { -1, 0, nullptr, nullptr } },
    };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 5; ++c) {
            const Field &f = rows[r][c];
            if (!f.name) continue;
            auto *l = new QLabel(QString::fromLatin1(f.name));
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            auto *e = new QLineEdit;
            e->setFont(dbgui::monoFont(e->font()));
            e->setMaxLength(f.digits + 1);
            e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(f.digits + 2, QLatin1Char('0'))) + 14);
            e->setToolTip(QString::fromLatin1(f.tip));
            if (f.reg >= 0) {
                m_reg[f.reg] = e;
                connect(e, &QLineEdit::returnPressed, this, [this, f, e] { setRegister(f.reg, e); });
            } else {
                m_dbus = e;
                e->setReadOnly(true);
            }
            g->addWidget(l, r, c * 2);
            g->addWidget(e, r, c * 2 + 1);
        }
    /* W's bits */
    static const struct { const char *name, *tip; int reg; } flags[5] = {
        { "S (positive)", "Sign: set when the result was positive (bit 7 clear)", CHF_REG_SIGN },
        { "C", "Carry", CHF_REG_CARRY },
        { "Z", "Zero", CHF_REG_ZERO },
        { "O", "Overflow", CHF_REG_OVERFLOW },
        { "ICB", "The interrupt control bit (the Channel F never interrupts)", CHF_REG_ICB },
    };
    auto *fl = new QHBoxLayout;
    for (int i = 0; i < 5; ++i) {
        m_flag[i] = new QCheckBox(QString::fromLatin1(flags[i].name));
        m_flag[i]->setToolTip(QString::fromLatin1(flags[i].tip));
        m_flag[i]->setFocusPolicy(Qt::NoFocus);
        const int reg = flags[i].reg;
        QCheckBox *cb = m_flag[i];
        connect(cb, &QCheckBox::clicked, this, [this, reg, cb](bool on) {
            if (!stopped(QStringLiteral("a flag"))) {
                refreshRegisters();
                return;
            }
            chfdebug_cpu_set(m_dbg, reg, on ? 1 : 0);
            if (onEdited) onEdited();
        });
        fl->addWidget(m_flag[i]);
    }
    fl->addStretch(1);
    g->addLayout(fl, 3, 0, 1, 10);
    g->setColumnStretch(10, 1);
    return box;
}

QWidget *CpuTab::buildMemory()
{
    auto *box = new QGroupBox(QStringLiteral("Memory"));
    auto *v = new QVBoxLayout(box);
    auto *row = new QHBoxLayout;
    m_memSpace = new QComboBox;
    m_memSpace->addItem(QStringLiteral("PROG: the CPU's view, $0000-$FFFF"), CHF_SPACE_PROG);
    m_memSpace->addItem(QStringLiteral("SCRATCH: r0-r63"), CHF_SPACE_SCRATCH);
    m_memSpace->addItem(QStringLiteral("VRAM: row × 128 + column"), CHF_SPACE_VRAM);
    m_memSpace->addItem(QStringLiteral("CART: the cartridge's own memory"), CHF_SPACE_CART);
    m_memSpace->addItem(QStringLiteral("IO: the ports as read now"), CHF_SPACE_IO);
    m_memSpace->setToolTip(QStringLiteral("Reads have no side effects. An edit goes in as the CPU's write would: "
                                          "the BIOS and ROM are not writable"));
    m_memGo = new QLineEdit;
    m_memGo->setPlaceholderText(QStringLiteral("$address or label"));
    m_memGo->setMaximumWidth(150);
    auto *prev = new QPushButton(QStringLiteral("◀"));
    auto *next = new QPushButton(QStringLiteral("▶"));
    prev->setFocusPolicy(Qt::NoFocus);
    next->setFocusPolicy(Qt::NoFocus);
    prev->setToolTip(QStringLiteral("The previous 384 bytes"));
    next->setToolTip(QStringLiteral("The next 384 bytes"));
    row->addWidget(m_memSpace, 1);
    row->addWidget(m_memGo);
    row->addWidget(prev);
    row->addWidget(next);
    v->addLayout(row);

    auto *t = m_hex = new QTableWidget(kMemRows, 16);
    QStringList cols;
    for (int c = 0; c < 16; ++c) cols << hx(c, 1);
    t->setHorizontalHeaderLabels(cols);
    t->setFont(dbgui::monoFont(t->font()));
    const QFontMetrics fm(t->font());
    const int cw = fm.horizontalAdvance(QStringLiteral("00")) + 14, ch = fm.height() + 8;
    t->horizontalHeader()->setDefaultSectionSize(cw);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    t->verticalHeader()->setDefaultSectionSize(ch);
    t->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    const int vw = fm.horizontalAdvance(QStringLiteral("$0000")) + 20;
    t->verticalHeader()->setFixedWidth(vw);
    t->setFixedSize(vw + 16 * cw + 2 * t->frameWidth() + 24,
                    t->horizontalHeader()->sizeHint().height() + kMemRows * ch + 2 * t->frameWidth() + 8);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int r = 0; r < kMemRows; ++r)
        for (int c = 0; c < 16; ++c) {
            auto *it = new QTableWidgetItem(QStringLiteral("00"));
            it->setTextAlignment(Qt::AlignCenter);
            t->setItem(r, c, it);
        }
    t->viewport()->installEventFilter(this);
    connect(t, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_hexFilling) return;
        bool ok = false;
        const unsigned val = it->text().trimmed().toUInt(&ok, 16);
        if (ok && val <= 0xFF)
            chfdebug_write(m_dbg, m_memSpace->currentData().toInt(), m_memBase + (uint32_t)(it->row() * 16 + it->column()),
                           (uint8_t)val);
        /* once the editor has closed, so the byte shows as two digits */
        QTimer::singleShot(0, this, [this] {
            refreshRam();
            if (onEdited) onEdited();
        });
    });
    v->addWidget(t, 0, Qt::AlignLeft);
    v->addWidget(dbgui::dimLabel(QStringLiteral("Double-click a byte to edit it (two hex digits, Enter). The wheel "
                                                "scrolls. Changed since the last look: accent.")));
    v->addStretch(1);

    connect(m_memSpace, &QComboBox::currentIndexChanged, this, [this] {
        m_memBase = 0;
        m_hexPrev.clear();
        refreshRam();
    });
    connect(m_memGo, &QLineEdit::returnPressed, this, [this] {
        long a;
        if (!addressOf(m_memGo->text(), &a)) {
            if (onMessage) onMessage(QStringLiteral("No such address or label"));
            return;
        }
        m_memBase = (uint32_t)a & ~0xFu;
        m_hexPrev.clear();
        refreshRam();
    });
    connect(prev, &QPushButton::clicked, this, [this] {
        m_memBase = m_memBase >= kMemRows * 16u ? m_memBase - kMemRows * 16u : 0;
        m_hexPrev.clear();
        refreshRam();
    });
    connect(next, &QPushButton::clicked, this, [this] {
        const uint32_t size = (uint32_t)chfdebug_space_size(m_dbg, m_memSpace->currentData().toInt());
        if (size && m_memBase + kMemRows * 16u < size) m_memBase += kMemRows * 16u;
        m_hexPrev.clear();
        refreshRam();
    });
    return box;
}

/* The wheel scrolls the memory view a few rows at a time. */
bool CpuTab::eventFilter(QObject *obj, QEvent *e)
{
    if (m_hex && obj == m_hex->viewport() && e->type() == QEvent::Wheel) {
        const int rows = -static_cast<QWheelEvent *>(e)->angleDelta().y() / 40;
        if (rows) {
            const long size = chfdebug_space_size(m_dbg, m_memSpace->currentData().toInt());
            long base = (long)m_memBase + rows * 16L;
            if (base > size - 16) base = size - 16;
            if (base < 0) base = 0;
            m_memBase = (uint32_t)base & ~0xFu;
            m_hexPrev.clear();
            refreshRam();
        }
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

bool CpuTab::addressOf(const QString &text, long *out)
{
    const int lbl = chfdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return dbgui::parseNum(text, out);
}

/* Edits go in only while the machine is stopped: a running CPU would
 * overwrite them at once. */
bool CpuTab::stopped(const QString &what)
{
    if (chfdebug_is_stopped(m_dbg)) return true;
    if (onMessage) onMessage(QStringLiteral("Stop the machine to edit %1").arg(what));
    return false;
}

void CpuTab::setRegister(int reg, QLineEdit *edit)
{
    long val = 0;
    bool ok;
    if (!stopped(QStringLiteral("a register"))) {
        ok = false;
    } else if (reg == CHF_REG_ISAR) {
        /* upper.lower in octal, or a plain octal number */
        const QString t = edit->text().trimmed();
        const QStringList parts = t.split(QLatin1Char('.'));
        bool a = false, b = false;
        if (parts.size() == 2) {
            const int u = parts[0].toInt(&a, 8), l = parts[1].toInt(&b, 8);
            ok = a && b && u >= 0 && u < 8 && l >= 0 && l < 8;
            val = u * 8 + l;
        } else {
            val = t.toLong(&a, 8);
            ok = a && val >= 0 && val < 64;
        }
    } else {
        ok = dbgui::parseNum(edit->text(), &val);
    }
    if (ok) chfdebug_cpu_set(m_dbg, reg, (int)val);
    edit->clearFocus();
    refreshRegisters();
    if (ok && onEdited) onEdited();
}

void CpuTab::refresh()
{
    refreshRegisters();
    refreshRam();
}

void CpuTab::refreshRegisters()
{
    chfdebug_cpu c;
    chfdebug_cpu_get(m_dbg, &c);
    auto set = [&](QLineEdit *e, const QString &t) {
        if (!e || e->hasFocus()) return;
        if (e->text() != t) e->setText(t);
    };
    auto hex = [&](int reg, unsigned v, int digits) { set(m_reg[reg], hx(v, digits)); };
    hex(CHF_REG_PC, (unsigned)c.pc & 0xFFFF, 4);
    hex(CHF_REG_PC0, c.pc0, 4);
    hex(CHF_REG_PC1, c.pc1, 4);
    hex(CHF_REG_DC0, c.dc0, 4);
    hex(CHF_REG_DC1, c.dc1, 4);
    hex(CHF_REG_A, c.a, 2);
    hex(CHF_REG_W, c.w, 2);
    set(m_reg[CHF_REG_ISAR], QStringLiteral("%1.%2").arg((c.isar >> 3) & 7).arg(c.isar & 7));
    hex(CHF_REG_J, c.j_h_k_q[0], 2);
    hex(CHF_REG_H, c.j_h_k_q[1], 4);
    hex(CHF_REG_K, c.j_h_k_q[2], 4);
    hex(CHF_REG_Q, c.j_h_k_q[3], 4);
    set(m_dbus, hx(c.dbus, 2));
    const int flags[5] = { c.sign, c.carry, c.zero, c.overflow, c.icb };
    for (int i = 0; i < 5; ++i) {
        const QSignalBlocker b(m_flag[i]);
        m_flag[i]->setChecked(flags[i] != 0);
    }
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

    const char *bios = c.bios == CHF_ROM_SL31253 ? "sl31253" : c.bios == CHF_ROM_SL90025 ? "sl90025" : "the app's stand-in";
    m_info->setText(QStringLiteral("Cycles %1, instructions %2. Frame %3, beam line %4 cycle %5. %6; BIOS: %7.%8")
                        .arg((qulonglong)c.cycles).arg((qulonglong)c.instructions).arg(c.frame).arg(c.vpos).arg(c.hpos)
                        .arg(QString::fromUtf8(chf_machine_name(c.machine)), QString::fromUtf8(bios),
                             c.illegal_count ? QStringLiteral(" %1 illegal opcode(s), the last at $%2.")
                                                   .arg(c.illegal_count).arg(hx((unsigned)c.illegal_pc & 0xFFFF, 4))
                                             : QString()));
}

void CpuTab::refreshRam()
{
    QTableWidget *t = m_hex;
    if (!t || dbgui::editing(t)) return;
    const int space = m_memSpace->currentData().toInt();
    const int size = kMemRows * 16;
    uint8_t buf[kMemRows * 16] = {};
    chfdebug_read(m_dbg, space, m_memBase, buf, size);
    const bool first = m_hexPrev.empty();
    m_hexFilling = true;
    for (int r = 0; r < kMemRows; ++r) {
        const QString head = QStringLiteral("$") + hx(m_memBase + (uint32_t)r * 16, 4);
        if (t->verticalHeaderItem(r)) t->verticalHeaderItem(r)->setText(head);
        else t->setVerticalHeaderItem(r, new QTableWidgetItem(head));
    }
    for (int i = 0; i < size; ++i) {
        QTableWidgetItem *it = t->item(i / 16, i % 16);
        const QString text = hx(buf[i], 2);
        if (it->text() != text) it->setText(text);
        const bool changed = !first && m_hexPrev[(size_t)i] != buf[i];
        it->setForeground(changed ? QBrush(chfAccentTextColor(t)) : QBrush());
    }
    m_hexFilling = false;
    m_hexPrev.assign(buf, buf + size);
}
