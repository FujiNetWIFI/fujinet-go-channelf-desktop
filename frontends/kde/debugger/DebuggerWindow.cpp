/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QDialogButtonBox>
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
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>

#include "../FujiNetWindows.h"
#include "../help/HelpWindow.h"
#include "ChipTabs.h"
#include "CpuTab.h"
#include "DebugUi.h"
#include "VideoTab.h"

#define DISASM_WINDOW 48

using dbgui::atEnd;
using dbgui::hx;
using dbgui::monoView;
using dbgui::parseNum;
using dbgui::setText;
using dbgui::stripControl;
using dbgui::titleLabel;

namespace {

/* The order of the tabs, their names for CHF_DEBUGGER_TAB, and their Help
 * contexts (chfhelp). */
enum { kTabPrompt, kTabCpu, kTabDisasm, kTabVideo, kTabIo, kTabBreaks, kTabCart, kTabFujinet };
const char *const kTabNames[] = { "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet" };
/* other names a tab answers to */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "ram", kTabCpu }, { "memory", kTabCpu }, { "scratchpad", kTabCpu }, { "vram", kTabVideo }, { "sound", kTabIo },
    { "ports", kTabIo }, { "mailbox", kTabCart }, { "packets", kTabFujinet },
};

/* What the Breakpoints tab adds: a kind, the space its range is in and a hint
 * for the range fields. */
struct BpKind {
    const char *name;
    int type, space;
    const char *hint;
};
const BpKind kBpKinds[] = {
    { "Execute", CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, "address or label ($0802)" },
    { "Read", CHFDEBUG_BP_READ, CHF_SPACE_PROG, "address or label ($8000)" },
    { "Write", CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, "address or label ($8000)" },
    { "Port in", CHFDEBUG_BP_IN, CHF_SPACE_IO, "port ($01)" },
    { "Port out", CHFDEBUG_BP_OUT, CHF_SPACE_IO, "port ($05)" },
    { "VRAM write", CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, "row,col (12,40)" },
    { "Scratchpad write", CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, "register (r9 or 9)" },
};

QString cellText(int cell) { return QStringLiteral("row %1 col %2").arg(cell / 128).arg(cell % 128); }

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
    m_cpu = new CpuTab(m_dbg);
    m_cpu->onMessage = [this](const QString &t) { note(t); };
    m_cpu->onEdited = [this] { refreshAll(); };
    tabs->addTab(m_cpu, QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    m_video = new VideoTab(m_dbg);
    m_video->onMessage = [this](const QString &t) { note(t); };
    tabs->addTab(m_video, QStringLiteral("Video"));
    m_io = new IoTab(m_dbg);
    tabs->addTab(m_io, QStringLiteral("Sound && I/O"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints"));
    m_cart = new CartTab(m_dbg);
    tabs->addTab(m_cart, QStringLiteral("Cart"));
    tabs->addTab(buildFujinet(), QStringLiteral("FujiNet"));

    /* a Help button for the tab that shows, and F1 */
    auto *help = qobject_cast<QToolButton *>(chfHelpButton(QString()));
    help->setAutoRaise(false);
    help->setText(QStringLiteral("Help"));
    help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tabs->setCornerWidget(help, Qt::TopRightCorner);
    auto setContext = [this](int page) {
        setProperty("chfHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabNames[page])));
    };
    setContext(0);
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { chfHelpFor(this); });

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
    setContext(tabs->currentIndex());
    connect(tabs, &QTabWidget::currentChanged, this, [this, setContext](int page) {
        setContext(page);
        refreshTab(page);
    });
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    /* started on show, stopped on hide: no wakeups while the window is away */
}

void DebuggerWindow::note(const QString &text)
{
    statusBar()->showMessage(text, 5000);
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

/* On to the line selected in the Disassembly. */
void DebuggerWindow::runToCursor()
{
    if (m_selAddr < 0) {
        note(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    chfdebug_run_to(m_dbg, (uint16_t)m_selAddr);
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
    add(QStringLiteral("Step Over (F8)"), [this] { stepAnd(chfdebug_step_over); });
    add(QStringLiteral("Step Out (Shift+F8)"), [this] { stepAnd(chfdebug_step_out); });
    add(QStringLiteral("Frame"), [this] { chfdebug_frame(m_dbg, 1); refreshAll(); })
        ->setToolTip(QStringLiteral("Run on to the next frame"));
    add(QStringLiteral("Scanline"), [this] { chfdebug_scanline(m_dbg, 1); refreshAll(); })
        ->setToolTip(QStringLiteral("Run on to the next line of the picture"));
    add(QStringLiteral("Run to Cursor"), [this] { runToCursor(); })
        ->setToolTip(QStringLiteral("Run on to the line selected in the Disassembly"));

    auto *save = new QPushButton(QStringLiteral("Save..."));
    save->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (the cartridge, as mapped)", "disassembly.asm" },
        { "mailbox", "Mailbox ($F800-$FFFF)", "mailbox.bin" },
        { "vram", "Video RAM (8192 bytes)", "vram.bin" },
        { "scratch", "Scratchpad (64 bytes)", "scratch.bin" },
        { "cart", "Cartridge image", "cartridge.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "vramshot", "Whole video RAM (PPM)", "vram.ppm" },
        { "regs", "CPU, ports and I/O state", "registers.txt" } };
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
            note(text);
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

/* A symbol file the user picks, or (fromFile false) the <cart>.map/.lst
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
    note(text);
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
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpv, print, mem, poke, runto, disasm, "
                                                  "scratch, kstack, vram, pal, ports, sound, cart ...)"
                                                  " — Tab completes, Up/Down recall"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    v->addWidget(m_promptOut, 1);
    v->addWidget(m_promptIn);
    return w;
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
    auto *sym = new QPushButton(QStringLiteral("Symbols..."));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Load labels from an AS .map or listing, or a list of addresses and names"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File..."), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Beside the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Go to")));
    row->addWidget(m_jump);
    row->addWidget(sym);
    row->addWidget(new QLabel(QStringLiteral("Click a line to select it, double-click it (or its dot) for a "
                                             "breakpoint; scroll to browse")));
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
    m_selLine = -1;
    QString text;
    char buf[256];
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        if (m_selAddr == lines[i].address) m_selLine = i;
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
    setText(m_disasm, text);

    /* the selected line, the PC's line in the accent, the breakpoints' dots */
    QList<QTextEdit::ExtraSelection> sels;
    auto line = [&](int i, const QBrush &bg, const QBrush &fg) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
        s.format.setBackground(bg);
        if (fg.style() != Qt::NoBrush) s.format.setForeground(fg);
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        sels.append(s);
    };
    if (m_selLine >= 0 && m_selLine != pcLine) {
        QColor hi = palette().color(QPalette::Highlight);
        hi.setAlpha(110);
        line(m_selLine, hi, QBrush());
    }
    if (pcLine >= 0) line(pcLine, chfAccentColor(), chfAccentInk());
    for (int i = 0; i < n; ++i) {
        if (!lines[i].has_breakpoint) continue;
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
        s.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        s.format.setForeground(i == pcLine ? QBrush(chfAccentInk()) : QBrush(chfAccentTextColor(m_disasm)));
        sels.append(s);
    }
    m_disasm->setExtraSelections(sels);
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    if (!addressOf(text, &a)) { note(QStringLiteral("No such address or label")); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, (uint16_t)(a & 0xFFFF), -(DISASM_WINDOW / 3));
    refreshDisasm();
}

/* ---- Breakpoints ------------------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    for (const BpKind &k : kBpKinds) m_bpType->addItem(QString::fromUtf8(k.name));
    m_bpType->setToolTip(QStringLiteral("Execute stops before an instruction runs; Read and Write on a data access "
                                        "to program space; Port in and out on the F8's I/O ports; VRAM write on a "
                                        "strobe to a cell (or a rectangle of them); Scratchpad write on a register"));
    m_bpStart = new QLineEdit;
    m_bpEnd = new QLineEdit;
    m_bpStart->setMaximumWidth(220);
    m_bpEnd->setMaximumWidth(220);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. value == 2 or a & $80)"));
    auto retext = [this](int i) {
        const BpKind &k = kBpKinds[i];
        m_bpStart->setPlaceholderText(QStringLiteral("start: %1").arg(QString::fromUtf8(k.hint)));
        m_bpEnd->setPlaceholderText(QStringLiteral("end (optional)"));
        m_bpStart->setMinimumWidth(m_bpStart->fontMetrics().horizontalAdvance(m_bpStart->placeholderText()) + 24);
    };
    connect(m_bpType, &QComboBox::currentIndexChanged, this, retext);
    retext(0);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, &DebuggerWindow::addBreakpoint);
    connect(m_bpStart, &QLineEdit::returnPressed, this, &DebuggerWindow::addBreakpoint);
    connect(m_bpEnd, &QLineEdit::returnPressed, this, &DebuggerWindow::addBreakpoint);
    connect(m_bpCond, &QLineEdit::returnPressed, this, &DebuggerWindow::addBreakpoint);
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("ID"), QStringLiteral("Type"),
                                           QStringLiteral("Where"), QStringLiteral("Hits"),
                                           QStringLiteral("Condition") });
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
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

/* A breakpoint from the fields: the kind decides the space and how the range
 * is written. */
void DebuggerWindow::addBreakpoint()
{
    const BpKind &k = kBpKinds[m_bpType->currentIndex()];
    auto parse = [&](const QString &text, long *out) -> bool {
        const QString t = text.trimmed();
        if (k.type == CHFDEBUG_BP_VRAM) {
            const QStringList rc = t.split(QLatin1Char(','));
            bool a = false, b = false;
            const int r = rc.value(0).trimmed().toInt(&a), c = rc.value(1).trimmed().toInt(&b);
            if (rc.size() != 2 || !a || !b || r < 0 || r > 63 || c < 0 || c > 127) return false;
            *out = r * 128 + c;
            return true;
        }
        if (k.type == CHFDEBUG_BP_SCRATCH) {
            bool a = false;
            const long v = (t.startsWith(QLatin1Char('r'), Qt::CaseInsensitive) ? t.mid(1) : t).toLong(&a);
            if (!a || v < 0 || v > 63) return false;
            *out = v;
            return true;
        }
        return addressOf(t, out);
    };
    if (m_bpStart->text().trimmed().isEmpty()) return;
    long a = 0, b = 0;
    if (!parse(m_bpStart->text(), &a)) { note(QStringLiteral("Bad start: use %1").arg(QString::fromUtf8(k.hint))); return; }
    b = a;
    if (!m_bpEnd->text().trimmed().isEmpty() && !parse(m_bpEnd->text(), &b)) {
        note(QStringLiteral("Bad end: use %1").arg(QString::fromUtf8(k.hint)));
        return;
    }
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (chfdebug_breakpoint_add(m_dbg, k.type, k.space, (uint16_t)a, (uint16_t)b, cond.constData()) < 0) {
        note(QStringLiteral("The condition does not parse, or the range is outside the %1").arg(QString::fromUtf8(k.name)));
        return;
    }
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    refreshAll();
}

void DebuggerWindow::refreshBps()
{
    chfdebug_breakpoint bps[256];
    const int n = chfdebug_breakpoint_list(m_dbg, bps, 256);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const chfdebug_breakpoint &b = bps[i];
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(b.enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, b.id);
        m_bpTable->setItem(i, 0, on);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(QString::number(b.id)));
        QString kind = QStringLiteral("?"), where;
        for (const BpKind &k : kBpKinds)
            if (k.type == b.type) kind = QString::fromUtf8(k.name);
        switch (b.type) {
        case CHFDEBUG_BP_VRAM:
            where = cellText(b.start);
            if (b.end != b.start) where += QStringLiteral(" to ") + cellText(b.end);
            break;
        case CHFDEBUG_BP_SCRATCH:
            where = QStringLiteral("r%1").arg(b.start);
            if (b.end != b.start) where += QStringLiteral("-r%1").arg(b.end);
            break;
        case CHFDEBUG_BP_IN:
        case CHFDEBUG_BP_OUT:
            where = QStringLiteral("port $") + hx(b.start, 2);
            if (b.end != b.start) where += QStringLiteral("-$") + hx(b.end, 2);
            break;
        default:
            where = QStringLiteral("$") + hx(b.start, 4);
            if (b.end != b.start) where += QStringLiteral("-$") + hx(b.end, 4);
            break;
        }
        m_bpTable->setItem(i, 2, new QTableWidgetItem(kind));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(where));
        m_bpTable->setItem(i, 4, new QTableWidgetItem(QString::number(b.hits)));
        m_bpTable->setItem(i, 5, new QTableWidgetItem(QString::fromUtf8(b.condition)));
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
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's link and mailbox")));
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
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)."));
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
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(why ? ": " : "", why ? stripControl(reason) : QString())
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? chfAccentFill() : QString());
}

/* The tab that shows; the others catch up when they are shown. */
void DebuggerWindow::refreshTab(int index)
{
    switch (index) {
    case kTabCpu: m_cpu->refresh(); break;
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
        /* the dot in the gutter, or a double-click, toggles a breakpoint; a
         * click selects the line Run to Cursor runs to */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            const QTextCursor cur = m_disasm->cursorForPosition(me->pos());
            const int line = cur.blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                const bool gutter = cur.positionInBlock() <= 1 && me->pos().x() < m_disasm->fontMetrics().horizontalAdvance(QLatin1Char('0')) * 3;
                if (gutter || e->type() == QEvent::MouseButtonDblClick) {
                    chfdebug_breakpoint_toggle(m_dbg, m_lineAddr[(size_t)line]);
                }
                m_selAddr = m_lineAddr[(size_t)line];
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
