/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPointer>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>

#include "../FujiNetWindows.h"
#include "ChipTabs.h"
#include "DebugUi.h"
#include "VideoTab.h"

#define DISASM_WINDOW 48

using dbgui::atEnd;
using dbgui::hx;
using dbgui::monoView;
using dbgui::setText;
using dbgui::stripControl;
using dbgui::titleLabel;

namespace {

/* $0400, 0x0400, 400h, #1024; a bare number is hex, as everywhere in a
 * debugger. */
bool parseNum(const QString &t, long *out)
{
    QString s = t.trimmed();
    bool ok = false;
    long v = 0;
    if (s.startsWith('$')) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith('#')) v = s.mid(1).toLong(&ok, 10);
    else if (s.endsWith('h', Qt::CaseInsensitive)) v = s.chopped(1).toLong(&ok, 16);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

/* The order of the tabs, and their names for CHF_DEBUGGER_TAB. */
enum { kTabPrompt, kTabCpu, kTabDisasm, kTabVideo, kTabIo, kTabBreaks, kTabCart, kTabFujinet };
const char *const kTabNames[] = { "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet" };
/* other names a tab answers to */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "ram", kTabCpu }, { "memory", kTabCpu }, { "uvi", kTabVideo }, { "2637", kTabVideo }, { "sound", kTabIo },
    { "keys", kTabIo }, { "mailbox", kTabCart },
};

QPointer<DebuggerWindow> s_win;

} // namespace

void DebuggerWindow::showFor(QWidget *parent, chfsession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    /* Opening the debugger stops the machine, as on every sibling. */
    chfdebug_attach(s_win->m_dbg);
    s_win->refreshAll();
}

void DebuggerWindow::toggleFor(QWidget *parent, chfsession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(chfsession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(chfdebug_get(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1400, 900);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = m_tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Prompt"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    m_video = new VideoTab(m_dbg);
    tabs->addTab(m_video, QStringLiteral("Video"));
    m_io = new IoTab(m_dbg, m_session);
    tabs->addTab(m_io, QStringLiteral("Sound && I/O"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints"));
    m_cart = new CartTab(m_dbg);
    tabs->addTab(m_cart, QStringLiteral("Cart"));
    tabs->addTab(buildFujinet(), QStringLiteral("FujiNet"));
    /* CHF_DEBUGGER_TAB: an index, or a tab's name (or the start of one) */
    if (qEnvironmentVariableIsSet("CHF_DEBUGGER_TAB")) {
        const QString want = qEnvironmentVariable("CHF_DEBUGGER_TAB").trimmed().toLower();
        bool ok = false;
        int n = want.toInt(&ok);
        if (!ok) {
            n = -1;
            for (int i = 0; i < (int)(sizeof kTabNames / sizeof kTabNames[0]) && !want.isEmpty(); ++i)
                if (QString::fromUtf8(kTabNames[i]).startsWith(want)) { n = i; break; }
            for (const auto &a : kTabAliases)
                if (n < 0 && !want.isEmpty() && QString::fromUtf8(a.name).startsWith(want)) n = a.tab;
        }
        if (n >= 0 && n < tabs->count()) tabs->setCurrentIndex(n);
    }
    connect(tabs, &QTabWidget::currentChanged, this, [this](int page) { refreshTab(page); });
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    /* started on show, stopped on hide: no wakeups while the window is away */
}

/* ---- the toolbar --------------------------------------------------------------- */

void DebuggerWindow::stepAnd(void (*fn)(chfdebug *))
{
    fn(m_dbg);
    refreshAll();
}

void DebuggerWindow::toggleRun()
{
    if (chfdebug_is_stopped(m_dbg)) chfdebug_resume(m_dbg); else chfdebug_stop(m_dbg);
    refreshAll();
}

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, auto fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), [this] { stepAnd(chfdebug_step); });
    add(QStringLiteral("Over (F8)"), [this] { stepAnd(chfdebug_step_over); });
    add(QStringLiteral("Out (Shift+F8)"), [this] { stepAnd(chfdebug_step_out); });
    add(QStringLiteral("Scanline+1"), [this] { chfdebug_scanline(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Frame+1"), [this] { chfdebug_frame(m_dbg, 1); refreshAll(); });

    auto *sym = new QPushButton(QStringLiteral("Load Symbols..."));
    sym->setFocusPolicy(Qt::NoFocus);
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File..."), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Beside the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    h->addWidget(sym);

    auto *save = new QPushButton(QStringLiteral("Save..."));
    save->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (the cartridge, as mapped)", "disassembly.asm" },
        { "mailbox", "Mailbox page ($2B00-$2FFF)", "mailbox.bin" },
        { "uvi", "UVI registers (768 bytes)", "uvi.bin" },
        { "font", "Glyph table (64 x 8 bytes)", "font.bin" },
        { "cart", "Cartridge image", "cartridge.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "regs", "CPU, UVI and I/O state", "registers.txt" } };
    for (const auto &s : saves) {
        const char *kind = s.kind;
        const char *title = s.title;
        const char *file = s.file;
        menu->addAction(QString::fromUtf8(title), this, [this, kind, title, file] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)),
                                                              QString::fromUtf8(file));
            if (path.isEmpty()) return;
            char msg[512];
            chfdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
            const QString text = stripControl(msg);
            appendPrompt(text + QLatin1Char('\n'));
            statusBar()->showMessage(text, 6000);
        });
    }
    save->setMenu(menu);
    h->addWidget(save);

    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(m_status, 1);
    return bar;
}

/* A symbol file the user picks, or (fromFile false) the <cart>.map/.lst/.sym
 * beside the opened cartridge. */
void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(chfsession_cart_path(m_session));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.map *.lst *.sym *.txt);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512];
    const QByteArray p = path.toLocal8Bit();
    chfdebug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    const QString text = stripControl(msg);
    appendPrompt(text + QLatin1Char('\n'));
    statusBar()->showMessage(text, 6000);
    refreshAll();
}

/* ---- Prompt ----------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("Debugger prompt. Type 'help' for every command.\n"));
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, print, mem, poke, runto, disasm, uvi, objects, screen, io, keys, cart ...)"
                                                  " — Tab completes, Up/Down recall"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    v->addWidget(m_promptOut, 1);
    v->addWidget(m_promptIn);
    return w;
}

/* ---- CPU & RAM -------------------------------------------------------------------------- */

QLineEdit *DebuggerWindow::regEdit(int reg, int digits, const QString &tip)
{
    auto *e = m_reg[reg] = new QLineEdit;
    e->setMaxLength(digits);
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits + 1, QLatin1Char('0'))) + 14);
    e->setToolTip(tip);
    connect(e, &QLineEdit::returnPressed, this, [this, reg, e] { setRegister(reg, e); });
    return e;
}

/* A hex editor over one of the address spaces: sixteen to a row, the byte a
 * double-click (or a typed digit) edits, the ones that changed since the last
 * look in the accent. The space and the base address are picked above it. */
QTableWidget *DebuggerWindow::hexTable(int rows)
{
    auto *t = new QTableWidget(rows, 16);
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
                    t->horizontalHeader()->sizeHint().height() + rows * ch + 2 * t->frameWidth() + 2);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < 16; ++c) {
            auto *it = new QTableWidgetItem(QStringLiteral("00"));
            it->setTextAlignment(Qt::AlignCenter);
            t->setItem(r, c, it);
        }
    connect(t, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_hexFilling) return;
        bool ok = false;
        const unsigned v = it->text().trimmed().toUInt(&ok, 16);
        if (ok && v <= 0xFF)
            chfdebug_write(m_dbg, m_memSpace->currentData().toInt(), m_memBase + (uint32_t)(it->row() * 16 + it->column()),
                           (uint8_t)v);
        /* once the editor has closed, so the byte shows as two digits */
        QTimer::singleShot(0, this, [this] { refreshRam(); });
    });
    return t;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;

    /* the registers; an edit goes in with Enter, while the machine is stopped */
    auto *regs = new QGroupBox(QStringLiteral("General registers"));
    auto *g = new QGridLayout(regs);
    g->addWidget(new QLabel(QStringLiteral("R0")), 1, 0, Qt::AlignRight);
    g->addWidget(regEdit(CHF_REG_R0, 2, QStringLiteral("R0: the same register in both banks")), 1, 1);
    for (int b = 0; b < 2; ++b) {
        m_bankHead[b] = new QLabel(QStringLiteral("Bank %1").arg(b));
        m_bankHead[b]->setAlignment(Qt::AlignCenter);
        g->addWidget(m_bankHead[b], 0, 3 + b * 3, 1, 2);
    }
    static const int bankRegs[2][3] = { { CHF_REG_R1, CHF_REG_R2, CHF_REG_R3 }, { CHF_REG_R1B, CHF_REG_R2B, CHF_REG_R3B } };
    for (int b = 0; b < 2; ++b)
        for (int n = 0; n < 3; ++n) {
            g->addWidget(new QLabel(QStringLiteral("R%1%2").arg(n + 1).arg(b ? "'" : "")), 1 + n, 3 + b * 3, Qt::AlignRight);
            g->addWidget(regEdit(bankRegs[b][n], 2, QStringLiteral("R%1 of register bank %2 (the one RS selects is in use)")
                                                        .arg(n + 1).arg(b)),
                         1 + n, 4 + b * 3);
        }
    g->setColumnMinimumWidth(2, 16);
    g->setColumnMinimumWidth(5, 16);
    g->setColumnStretch(8, 1);
    left->addWidget(regs);

    auto *addr = new QGroupBox(QStringLiteral("Address"));
    auto *ag = new QGridLayout(addr);
    ag->addWidget(new QLabel(QStringLiteral("PC")), 0, 0, Qt::AlignRight);
    ag->addWidget(regEdit(CHF_REG_PC, 4, QStringLiteral("The 15-bit address of the instruction about to run: the page (A14-A13) and the IAR")), 0, 1);
    ag->addWidget(new QLabel(QStringLiteral("IAR")), 0, 3, Qt::AlignRight);
    ag->addWidget(regEdit(CHF_REG_IAR, 4, QStringLiteral("The instruction address register: A12-A0, wrapping inside the 8K page")), 0, 4);
    m_pageLabel = new QLabel;
    ag->addWidget(m_pageLabel, 0, 6);
    ag->setColumnStretch(7, 1);
    left->addWidget(addr);

    /* PSU and PSL, byte and bits */
    auto *psw = new QGroupBox(QStringLiteral("Program status"));
    auto *pg = new QGridLayout(psw);
    struct Field { int reg, digits; const char *name, *tip; };
    static const Field psu[] = {
        { CHF_REG_PSU, 2, "PSU", "Program status upper: S F II - - SP2 SP1 SP0" },
        { CHF_REG_F, 1, "F", "The FLAG output" },
        { CHF_REG_II, 1, "II", "Interrupt inhibit" },
        { CHF_REG_SP, 1, "SP", "The return-stack pointer, 0-7" },
    };
    static const Field psl[] = {
        { CHF_REG_PSL, 2, "PSL", "Program status lower: CC1 CC0 IDC RS WC OVF COM C" },
        { CHF_REG_CC, 1, "CC", "The condition code: 0 zero, 1 positive, 2 negative" },
        { CHF_REG_IDC, 1, "IDC", "Inter-digit carry" },
        { CHF_REG_RS, 1, "RS", "The register bank in use" },
        { CHF_REG_WC, 1, "WC", "With carry (arithmetic uses C)" },
        { CHF_REG_OVF, 1, "OVF", "Overflow" },
        { CHF_REG_COM, 1, "COM", "Logical compare (unsigned)" },
        { CHF_REG_C, 1, "C", "Carry" },
    };
    for (int i = 0; i < 4; ++i) {
        pg->addWidget(new QLabel(QString::fromUtf8(psu[i].name)), 0, i * 2, Qt::AlignRight);
        pg->addWidget(regEdit(psu[i].reg, psu[i].digits, QString::fromUtf8(psu[i].tip)), 0, i * 2 + 1);
    }
    for (int i = 0; i < 8; ++i) {
        pg->addWidget(new QLabel(QString::fromUtf8(psl[i].name)), 1 + i / 4, (i % 4) * 2, Qt::AlignRight);
        pg->addWidget(regEdit(psl[i].reg, psl[i].digits, QString::fromUtf8(psl[i].tip)), 1 + i / 4, (i % 4) * 2 + 1);
    }
    pg->setColumnStretch(8, 1);
    left->addWidget(psw);

    /* the return address stack: a call increments SP and stores at ras[SP] */
    auto *ras = new QGroupBox(QStringLiteral("Return address stack (8 deep)"));
    auto *rg = new QGridLayout(ras);
    for (int i = 0; i < 8; ++i) {
        m_rasLabel[i] = new QLabel(QStringLiteral("%1").arg(i));
        rg->addWidget(m_rasLabel[i], i / 4, (i % 4) * 2, Qt::AlignRight);
        rg->addWidget(regEdit(CHF_REG_RAS0 + i, 4, QStringLiteral("Return address slot %1 (the live returns are slots 1 to SP)").arg(i)),
                      i / 4, (i % 4) * 2 + 1);
    }
    rg->setColumnStretch(8, 1);
    left->addWidget(ras);

    /* what the CPU reads, not editable */
    auto *stateBox = new QGroupBox(QStringLiteral("Inputs and state"));
    auto *sg = new QGridLayout(stateBox);
    static const struct { const char *name, *tip; } states[3] = {
        { "S (SENSE)", "The SENSE input: the UVI's vertical blank" },
        { "Halted", "HALT executed: waiting for an interrupt" },
        { "IRQ line", "The IRQ input (the Channel F never drives it)" },
    };
    for (int i = 0; i < 3; ++i) {
        m_state[i] = new QCheckBox(QString::fromUtf8(states[i].name));
        m_state[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_state[i]->setFocusPolicy(Qt::NoFocus);
        m_state[i]->setToolTip(QString::fromUtf8(states[i].tip));
        sg->addWidget(m_state[i], 0, i);
    }
    left->addWidget(stateBox);
    m_cpuInfo = dbgui::dimLabel();
    m_cpuInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left->addWidget(m_cpuInfo);
    left->addStretch();
    h->addLayout(left);

    auto *right = new QVBoxLayout;
    right->addWidget(titleLabel(QStringLiteral("Memory")));
    auto *row = new QHBoxLayout;
    m_memSpace = new QComboBox;
    m_memSpace->addItem(QStringLiteral("Program (CPU view, 15-bit)"), CHF_SPACE_PROG);
    m_memSpace->addItem(QStringLiteral("UVI registers ($000-$2FF = CPU $1800-$1AFF)"), CHF_SPACE_UVI);
    m_memSpace->addItem(QStringLiteral("Cartridge image"), CHF_SPACE_CART);
    m_memSpace->setToolTip(QStringLiteral("Reads have no side effects. An edit goes into the UVI with its write "
                                          "effects, or into the cartridge's own buffer; the rest is ROM"));
    m_memGo = new QLineEdit;
    m_memGo->setPlaceholderText(QStringLiteral("$address or label"));
    m_memGo->setMaximumWidth(140);
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
    right->addLayout(row);
    m_hex = hexTable(kMemRows);
    right->addWidget(m_hex);
    right->addWidget(dbgui::dimLabel(QStringLiteral("Double-click a byte to edit it (two hex digits, Enter). "
                                                    "Changed since the last look: gold.")));
    right->addStretch();
    h->addLayout(right, 1);
    connect(m_memSpace, &QComboBox::currentIndexChanged, this, [this] {
        m_memBase = 0;
        m_hexPrev.clear();
        refreshRam();
    });
    connect(m_memGo, &QLineEdit::returnPressed, this, [this] {
        long a;
        if (!addressOf(m_memGo->text(), &a)) { statusBar()->showMessage(QStringLiteral("No such address or label"), 4000); return; }
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
    return w;
}

/* A register typed in, with Enter: taken while the machine is stopped (a
 * running CPU would overwrite it at once). */
void DebuggerWindow::setRegister(int reg, QLineEdit *edit)
{
    long val;
    if (!chfdebug_is_stopped(m_dbg))
        statusBar()->showMessage(QStringLiteral("Stop the machine to edit a register"), 4000);
    else if (parseNum(edit->text(), &val))
        chfdebug_cpu_set(m_dbg, reg, (int)val);
    edit->clearFocus();
    refreshAll();
}

void DebuggerWindow::refreshCpu()
{
    chfdebug_cpu c;
    chfdebug_cpu_get(m_dbg, &c);
    auto set = [&](int reg, unsigned v, int digits) {
        QLineEdit *e = m_reg[reg];
        if (!e || e->hasFocus()) return;
        const QString t = hx(v, digits);
        if (e->text() != t) e->setText(t);
    };
    set(CHF_REG_R0, c.r0, 2);
    for (int n = 0; n < 3; ++n) {
        set(CHF_REG_R1 + n, c.r[0][n + 1], 2);
        set(CHF_REG_R1B + n, c.r[1][n + 1], 2);
    }
    set(CHF_REG_PC, (unsigned)c.pc & 0x7FFF, 4);
    set(CHF_REG_IAR, (unsigned)c.iar & 0x1FFF, 4);
    set(CHF_REG_PSU, (unsigned)c.psu & 0xFF, 2);
    set(CHF_REG_PSL, (unsigned)c.psl & 0xFF, 2);
    set(CHF_REG_F, (unsigned)c.flag & 1, 1);
    set(CHF_REG_II, (unsigned)c.ii & 1, 1);
    set(CHF_REG_SP, (unsigned)c.sp & 7, 1);
    set(CHF_REG_CC, (unsigned)c.cc & 3, 1);
    set(CHF_REG_IDC, (unsigned)c.idc & 1, 1);
    set(CHF_REG_RS, (unsigned)c.rs & 1, 1);
    set(CHF_REG_WC, (unsigned)c.wc & 1, 1);
    set(CHF_REG_OVF, (unsigned)c.ovf & 1, 1);
    set(CHF_REG_COM, (unsigned)c.com & 1, 1);
    set(CHF_REG_C, (unsigned)c.c & 1, 1);
    for (int i = 0; i < 8; ++i) {
        set(CHF_REG_RAS0 + i, c.ras[i] & 0x7FFF, 4);
        m_rasLabel[i]->setText(i == c.sp ? QStringLiteral("▶%1").arg(i) : QStringLiteral(" %1").arg(i));
    }
    for (int b = 0; b < 2; ++b)
        m_bankHead[b]->setText(c.rs == b ? QStringLiteral("Bank %1 (in use)").arg(b) : QStringLiteral("Bank %1").arg(b));
    m_pageLabel->setText(QStringLiteral("page $%1 (A14-A13 = %2)").arg(hx((unsigned)c.page, 4)).arg(c.page >> 13));
    const bool flags[3] = { c.sense != 0, c.halted != 0, c.irq_line != 0 };
    for (int i = 0; i < 3; ++i) m_state[i]->setChecked(flags[i]);
    m_cpuInfo->setText(QStringLiteral("Cycles %1, instructions %2, beam line %3 dot %4, frame %5. %6.%7")
                           .arg((qulonglong)c.cycles).arg((qulonglong)c.instructions).arg(c.vpos).arg(c.hpos)
                           .arg(c.frame).arg(QString::fromUtf8(chf_machine_name(c.machine)),
                                             c.illegal_count ? QStringLiteral(" %1 illegal opcode(s), the last at $%2.")
                                                                   .arg(c.illegal_count).arg(hx((unsigned)c.illegal_pc & 0x7FFF, 4))
                                                             : QString()));
}

void DebuggerWindow::refreshRam()
{
    QTableWidget *t = m_hex;
    if (!t || dbgui::editing(t)) return;
    const int space = m_memSpace->currentData().toInt();
    const int size = kMemRows * 16;
    uint8_t buf[kMemRows * 16] = {};
    chfdebug_read(m_dbg, space, m_memBase, buf, size);
    const bool first = m_hexPrev.empty();
    m_hexFilling = true;
    for (int r = 0; r < kMemRows; ++r)
        t->verticalHeaderItem(r) ? t->verticalHeaderItem(r)->setText(QStringLiteral("$") + hx(m_memBase + (uint32_t)r * 16, 4))
                                 : t->setVerticalHeaderItem(r, new QTableWidgetItem(QStringLiteral("$") + hx(m_memBase + (uint32_t)r * 16, 4)));
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

/* ---- Disassembly -------------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::toggled, this, [this](bool on) { if (on) refreshDisasm(); });
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("$address or label"));
    m_jump->setMaximumWidth(160);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(m_jump->text()); });
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Go to")));
    row->addWidget(m_jump);
    row->addWidget(new QLabel(QStringLiteral("Click a line to toggle its breakpoint; scroll to browse")));
    row->addStretch();
    v->addLayout(row);
    m_disasm = monoView(false);
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

void DebuggerWindow::refreshDisasm()
{
    static chfdebug_line lines[DISASM_WINDOW];
    if (m_followPc->isChecked()) {
        chfdebug_cpu c;
        chfdebug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int pcLine = -1;
    const int n = chfdebug_disassemble(m_dbg, m_disasmTop, lines, DISASM_WINDOW, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);
    m_lineAddr.clear();
    QString text;
    char buf[256];
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        char label[48];
        snprintf(label, sizeof label, "%.*s", labelWidth, lines[i].label);
        /* the gutter: a breakpoint dot, then the PC's arrow */
        snprintf(buf, sizeof buf, " %c %04X  %-9s %-*s %-22s", lines[i].is_pc ? '>' : ' ', lines[i].address,
                 lines[i].bytes, labelWidth, label, lines[i].disasm);
        text += lines[i].has_breakpoint ? QStringLiteral("●") : QStringLiteral(" ");
        text += QString::fromUtf8(buf);
        if (lines[i].comment[0]) text += QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
        text += QLatin1Char('\n');
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)\n");
    m_disasm->setPlainText(text);
    for (int i = 0; i < n; ++i) {
        if (i == pcLine) {
            QTextCursor cur(m_disasm->document()->findBlockByNumber(i));
            cur.select(QTextCursor::LineUnderCursor);
            QTextCharFormat fmt;
            fmt.setBackground(chfAccentColor());
            fmt.setForeground(QColor(0x1B, 0x14, 0x08));
            cur.setCharFormat(fmt);
        } else if (lines[i].has_breakpoint) {
            QTextCursor cur(m_disasm->document()->findBlockByNumber(i));
            cur.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            fmt.setForeground(chfAccentTextColor(m_disasm));
            cur.setCharFormat(fmt);
        }
    }
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    if (!addressOf(text, &a)) { statusBar()->showMessage(QStringLiteral("No such address or label"), 4000); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, (uint16_t)(a & 0x7FFF), -(DISASM_WINDOW / 3));
    refreshDisasm();
}

/* ---- Breakpoints ------------------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Exec"), QStringLiteral("Read"), QStringLiteral("Write") });
    m_bpType->setToolTip(QStringLiteral("Exec stops before an instruction runs; Read and Write on a data access to "
                                        "program space (the UVI's registers at $1800-$1AFF are the write targets)"));
    m_bpStart = new QLineEdit;
    m_bpStart->setPlaceholderText(QStringLiteral("start ($400 or label)"));
    m_bpStart->setMinimumWidth(m_bpStart->fontMetrics().horizontalAdvance(m_bpStart->placeholderText()) + 16);
    m_bpStart->setMaximumWidth(200);
    m_bpEnd = new QLineEdit;
    m_bpEnd->setPlaceholderText(QStringLiteral("end (optional)"));
    m_bpEnd->setMaximumWidth(140);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. r0 == $FF or value & $80)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        static const int types[3] = { CHFDEBUG_BP_EXEC, CHFDEBUG_BP_READ, CHFDEBUG_BP_WRITE };
        const int type = m_bpType->currentIndex();
        long a = 0, b = 0;
        if (m_bpStart->text().trimmed().isEmpty()) return;
        if (!addressOf(m_bpStart->text(), &a)) { statusBar()->showMessage(QStringLiteral("Bad start address"), 4000); return; }
        b = a;
        if (!m_bpEnd->text().trimmed().isEmpty() && !addressOf(m_bpEnd->text(), &b)) {
            statusBar()->showMessage(QStringLiteral("Bad end address"), 4000);
            return;
        }
        const int space = CHF_SPACE_PROG;
        const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
        if (chfdebug_breakpoint_add(m_dbg, types[type], space, (uint16_t)a, (uint16_t)b, cond.constData()) < 0) {
            statusBar()->showMessage(QStringLiteral("The condition does not parse"), 6000);
            return;
        }
        m_bpStart->clear();
        m_bpEnd->clear();
        m_bpCond->clear();
        refreshAll();
    };
    connect(add, &QPushButton::clicked, this, doAdd);
    connect(m_bpStart, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpEnd, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpCond, &QLineEdit::returnPressed, this, doAdd);
        row->addWidget(m_bpType);
    row->addWidget(new QLabel(QStringLiteral("program space")));
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_bpTable = new QTableWidget(0, 7);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("ID"), QStringLiteral("Type"),
                                           QStringLiteral("Where"), QStringLiteral("Range"), QStringLiteral("Hits"),
                                           QStringLiteral("Condition") });
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        chfdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
    });
    v->addWidget(m_bpTable, 1);

    auto *btns = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_bpTable->currentRow();
        if (r < 0) return;
        chfdebug_breakpoint_remove(m_dbg, m_bpTable->item(r, 0)->data(Qt::UserRole).toInt());
        refreshAll();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_breakpoint_clear(m_dbg);
        refreshAll();
    });
    btns->addWidget(remove);
    btns->addWidget(clear);
    btns->addStretch();
    v->addLayout(btns);
    return w;
}

void DebuggerWindow::refreshBps()
{
    chfdebug_breakpoint bps[256];
    const int n = chfdebug_breakpoint_list(m_dbg, bps, 256);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        m_bpTable->setItem(i, 0, on);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(QString::number(bps[i].id)));
        QStringList type;
        if (bps[i].type & CHFDEBUG_BP_EXEC) type << QStringLiteral("Exec");
        if (bps[i].type & CHFDEBUG_BP_READ) type << QStringLiteral("Read");
        if (bps[i].type & CHFDEBUG_BP_WRITE) type << QStringLiteral("Write");
        m_bpTable->setItem(i, 2, new QTableWidgetItem(type.join(QLatin1Char('/'))));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(QStringLiteral("program")));
        QString range = QStringLiteral("$") + hx(bps[i].start, 4);
        if (bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, 4);
        m_bpTable->setItem(i, 4, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 5, new QTableWidgetItem(QString::number(bps[i].hits)));
        m_bpTable->setItem(i, 6, new QTableWidgetItem(QString::fromUtf8(bps[i].condition)));
    }
    m_bpFilling = false;
}

/* ---- FujiNet ------------------------------------------------------------------------------------- */

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = dbgui::dimLabel();
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView(false);
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's mailbox")));
    m_fujiState = monoView(false);
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static chfdebug_fuji_packet p[512];
    static char state[2048];
    const int n = chfdebug_fujinet_trace(m_dbg, p, 512);
    char status[128];
    chfsession_cart_status(m_session, status, sizeof status);
    if (chfsession_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1. Cartridge: %2.")
                                .arg(QString::fromUtf8(chfsession_boip_endpoint(m_session)), QString::fromUtf8(status)));
    else if (!chfsession_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences > FujiNet)."));
    else
        m_fujiNote->setText(QStringLiteral("FujiNet is on. Cartridge: %1.").arg(QString::fromUtf8(status)));
    state[0] = '\0';
    chfdebug_fujinet_state(m_dbg, state, sizeof state);
    setText(m_fujiState, QString::fromUtf8(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            t += QStringLiteral("%1 ").arg(hx(p[i].data[k], 2));
        if (p[i].nbytes > (int)sizeof p[i].data) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, force || atEnd(m_fujiText));
}

/* ---- refreshing ------------------------------------------------------------------------------------ */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr;
    const bool stopped = chfdebug_is_stopped(m_dbg) != 0;
    chfdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    /* the engine says "stopped" when nothing more can be said */
    const bool why = reason[0] && strcmp(reason, "stopped") != 0;
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(why ? ": " : "", why ? QString::fromUtf8(reason) : QString())
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? chfAccentFill() : QString());
}

/* The tab that shows; the others catch up when they are shown. */
void DebuggerWindow::refreshTab(int index)
{
    switch (index) {
    case kTabCpu: refreshCpu(); refreshRam(); break;
    case kTabDisasm: refreshDisasm(); break;
    case kTabVideo: m_video->refresh(); break;
    case kTabIo: m_io->refresh(); break;
    case kTabBreaks: refreshBps(); break;
    case kTabCart: m_cart->refresh(); break;
    case kTabFujinet: refreshFujinet(false); break;
    default: break;
    }
}

void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshTab(m_tabs->currentIndex());
}

/* Every chfdebug call takes the host's run lock, so the live tabs are safe to
 * refresh while the machine runs: the picture ten times a second, the rest
 * twice a second. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = chfdebug_generation(m_dbg);
    const bool stopped = chfdebug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen;
        m_wasStopped = stopped;
        refreshAll();
        return;
    }
    if (stopped) return;
    const int tab = m_tabs->currentIndex();
    if (tab == kTabPrompt) return;
    if (tab == kTabVideo || ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshAll();
    }
}

/* ---- the prompt and the rest ----------------------------------------------------------------------------- */

void DebuggerWindow::appendPrompt(const QString &text)
{
    m_promptOut->moveCursor(QTextCursor::End);
    m_promptOut->insertPlainText(text);
    m_promptOut->verticalScrollBar()->setValue(m_promptOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runPrompt()
{
    static char out[65536];
    const QString cmd = m_promptIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = m_history.size();
    appendPrompt(QStringLiteral("> %1\n").arg(cmd));
    chfdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    appendPrompt(stripControl(out) + QLatin1Char('\n'));
    m_promptIn->clear();
    refreshAll();
}

/* A label, or a number as parseNum reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const int lbl = chfdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return parseNum(text, out);
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_promptIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            const QString text = m_promptIn->text();
            const int sp = text.lastIndexOf(QLatin1Char(' '));
            const QString word = sp >= 0 ? text.mid(sp + 1) : text;
            char comps[4096];
            const int n = chfdebug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
            if (n == 1) {
                QString c = QString::fromUtf8(comps).section(QLatin1Char('\n'), 0, 0);
                m_promptIn->setText(text.left(sp + 1) + c + QLatin1Char(' '));
            } else if (n > 1) {
                appendPrompt(QString::fromUtf8(comps));
            }
            return true;
        }
        /* the history: Up for older, Down back towards an empty line */
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_promptIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            const int line = m_disasm->cursorForPosition(me->pos()).blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                chfdebug_breakpoint_toggle(m_dbg, m_lineAddr[(size_t)line]);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(e);
            const int rows = -we->angleDelta().y() / 40;
            if (rows != 0) {
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); return;
    case Qt::Key_F7: stepAnd(chfdebug_step); return;
    case Qt::Key_F8:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? chfdebug_step_out : chfdebug_step_over);
        return;
    case Qt::Key_F12: hide(); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

/* Hidden by F12, the close button or the main window: the engine goes
 * away and the machine runs on. The close button is a plain close, which
 * only hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    m_timer.start(100);
    QMainWindow::showEvent(e);
}

void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    chfdebug_detach(m_dbg);
    QMainWindow::hideEvent(e);
}
