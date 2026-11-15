/*
 * Debugger window (Qt6 Widgets) over the Fairchild F8 / Channel F video
 * debugger engine, via core/include/chfdebug.h. Mirrors the GNOME one tab for
 * tab, and follows the Studio II, SMS and Astrocade KDE debuggers.
 *
 * Tabs, in this order (CHF_DEBUGGER_TAB picks the one a window opens on, by
 * index or by name -- 0 console, 1 disassembly, 2 memory, 3 breakpoints,
 * 4 watches, 5 cart, 6 trace, 7 history, 8 video, 9 sound, 10 symbols,
 * 11 fujinet -- or by the start of a label): the Console (the engine's command
 * line, with a history and Tab completion), the Disassembly (click a line to
 * select it for Run to Cursor; click its left edge, double-click it or press
 * F9 for its breakpoint), Memory (five spaces: PROG, SCRATCH, VRAM, CART and
 * IO; Go to a label / address / range, a hex or text search, Poke; the
 * SCRATCH space adds the 8 x 8 octal scratchpad grid and the K stack),
 * Breakpoints (execute, read, write, access, port IN and OUT, VRAM and
 * scratchpad writes, with a condition), Watches (a value list, Break on
 * change), Cart & I/O (the Videocart or the FujiNet cartridge, the memory map,
 * the controllers and panel, the ports, the 2102 latch, the I/O log), Trace
 * and History (the engine's instruction ring: live, and read at a stop),
 * Video (the VRAM's pictures, inspector, registers, colours, lines and
 * writes), Sound (the beeper), Symbols (every label, filtered) and FujiNet
 * (the FujiBus packets on the cartridge's link). The registers sit in a strip
 * under the tabs, on every one of them. Any tab can be popped out into a
 * window of its own (CHF_POPOUT names one at start). The toolbar carries the
 * stepping controls, Break at, symbol loading and Save; the family's keys
 * apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7
 * frame, F9 breakpoint, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches, which takes the breakpoints out of the core
 * and lets the machine run on.
 *
 * A short timer polls the engine's generation and stop serial and refreshes
 * the tab showing when they moved; while the machine runs, the live tabs
 * refresh every 400 ms. Every new stop re-centres every view on the spot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "ChipTabs.h"
#include "ScratchPanel.h"
#include "VideoTab.h"
#include "../FujiNetWindows.h"
#include "../help/HelpWindow.h"

extern "C" {
#include "chfmemtools.h"
}

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>


#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_MAX 0x18000         /* the largest space (CART: 64K image, 32K arena) */
#define MEM_ROW_TEXT 140        /* bytes of text a Memory row can take */
#define BP_MAX 256
#define TRACE_MAX 512
#define HIST_SHOWN 2000         /* trace lines shown */
#define RING_MAX 65537
#define CONSOLE_LINES 5000
#define REGION_MAX 64

namespace {

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY, TAB_VIDEO,
    TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names CHF_DEBUGGER_TAB takes, their labels (which
 * it takes too), and their Help contexts (debugger/<help>). */
const struct { const char *name, *label, *help; } kTabs[TAB_COUNT] = {
    { "console", "Console", "console" },           { "disassembly", "Disassembly", "disassembly" },
    { "memory", "Memory", "memory" },              { "breakpoints", "Breakpoints", "breakpoints" },
    { "watches", "Watches", "watches" },           { "cart", "Cart & I/O", "cart" },
    { "trace", "Trace", "trace" },                 { "history", "History", "history" },
    { "video", "Video", "video" },                 { "sound", "Sound", "sound" },
    { "symbols", "Symbols", "symbols" },           { "fujinet", "FujiNet", "fujinet" },
};

/* The register entries of the strip, in the order shown, and the chfdebug_reg
 * each sets (A and W are hex bytes, ISAR is octal upper.lower, PC0/PC1/DC0/DC1
 * hex words). J, H, K and Q are the scratchpad grid's. */
const struct { const char *name; int id, digits; const char *tip; } kRegs[] = {
    { "PC", CHF_REG_PC, 4, "The instruction about to run (the CPU has already fetched its opcode, so PC0 is one more)" },
    { "PC0", CHF_REG_PC0, 4, "The program counter as the bus sees it" },
    { "PC1", CHF_REG_PC1, 4, "The one return address: PI copies PC0 here, POP copies it back" },
    { "DC0", CHF_REG_DC0, 4, "The data counter" },
    { "DC1", CHF_REG_DC1, 4, "The alternate data counter (XDC swaps them)" },
    { "A", CHF_REG_A, 2, "The accumulator" },
    { "W", CHF_REG_W, 2, "Status: bit 0 S (1 = positive), 1 C, 2 Z, 3 O, 4 ICB" },
    { "ISAR", CHF_REG_ISAR, 3, "The scratchpad pointer, six bits, in octal as upper.lower (3.5 is r29)" },
};

/* W's bits, as the checkboxes beside it show them. */
const struct { const char *name, *tip; int reg; } kFlags[5] = {
    { "S", "Sign: set when the result was positive (bit 7 clear)", CHF_REG_SIGN },
    { "C", "Carry", CHF_REG_CARRY },
    { "Z", "Zero", CHF_REG_ZERO },
    { "O", "Overflow", CHF_REG_OVERFLOW },
    { "ICB", "The interrupt control bit (the Channel F never interrupts)", CHF_REG_ICB },
};

/* The breakpoint kinds the Breakpoints tab offers, in the order of its chooser:
 * the engine's type, the space its range is in and a hint for the fields. */
const struct { const char *label; int type, space; const char *hint, *endHint; } kBpTypes[] = {
    { "Execute", CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, "address, range or label ($0802)", "end (optional)" },
    { "Read", CHFDEBUG_BP_READ, CHF_SPACE_PROG, "address, range or label ($8000)", "end (optional)" },
    { "Write", CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, "address, range or label ($8000)", "end (optional)" },
    { "Access", CHFDEBUG_BP_ACCESS, CHF_SPACE_PROG, "address, range or label ($8000)", "end (optional)" },
    { "Port IN", CHFDEBUG_BP_IN, CHF_SPACE_IO, "port ($01)", "end port (optional)" },
    { "Port OUT", CHFDEBUG_BP_OUT, CHF_SPACE_IO, "port ($05)", "end port (optional)" },
    { "VRAM write", CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, "cell: row,col (12,40)", "opposite corner (optional)" },
    { "Scratchpad write", CHFDEBUG_BP_SCRATCH, CHF_SPACE_SCRATCH, "register (r9, j, 9)", "last register (optional)" },
};
const int kBpTypeCount = (int)(sizeof kBpTypes / sizeof kBpTypes[0]);

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line and a marked memory row: the Videocarts' yellow. It is
 * pale, so every fill of it carries dark ink. */
QColor accentColor()
{
    return chfAccentColor();
}

QColor accentInk()
{
    return chfAccentInk();
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(accentColor());
    f.setForeground(accentInk());
    return f;
}

/* A watched range in the dump: a blue the accent does not hide. */
QTextCharFormat watchFormat()
{
    QTextCharFormat f;
    f.setBackground(QColor(0x1c, 0x71, 0xd8));
    f.setForeground(Qt::white);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

/* "Stopped" and the engine's reason: "stopped at $0038" reads "Stopped at
 * $0038", anything else "Stopped: <reason>". */
QString stoppedText(const QString &reason)
{
    if (reason.isEmpty()) return QStringLiteral("Stopped");
    if (reason.startsWith(QLatin1String("stopped"))) return QStringLiteral("S") + reason.mid(1);
    return QStringLiteral("Stopped: ") + reason;
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the checked style (the accent, a box of its own), which is
 * not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* CHF_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("i/o" ...). The names of the
 * tabs the older windows had still reach their successors: prompt is the
 * Console, cpu / ram / scratchpad the Memory, io / ports / mailbox the Cart &
 * I/O tab, packets the FujiNet one. */
int tabFromEnv(const char *var = "CHF_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(var).trimmed();
    if (tab.isEmpty()) return -1;
    static const struct { const char *name; int tab; } aliases[] = {
        { "io", TAB_CART },      { "prompt", TAB_CONSOLE }, { "cpu", TAB_MEMORY },     { "ram", TAB_MEMORY },
        { "scratchpad", TAB_MEMORY }, { "ports", TAB_CART }, { "mailbox", TAB_CART }, { "packets", TAB_FUJINET },
        { "vram", TAB_VIDEO },
    };
    for (const auto &alias : aliases)
        if (tab.compare(QLatin1String(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

QPointer<DebuggerWindow> s_win;

/* The breakpoint kind of a chooser row. */
int bpKindAt(int row)
{
    return qBound(0, row, kBpTypeCount - 1);
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, chfsession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent attaches, which stops the machine */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggleFor(QWidget *parent, chfsession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(chfsession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(chfdebug_get(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Channel F Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    m_pages[TAB_CONSOLE] = buildConsole();
    m_pages[TAB_DISASM] = buildDisasm();
    m_pages[TAB_MEMORY] = buildMemory();
    m_pages[TAB_BREAKS] = buildBreaks();
    m_pages[TAB_WATCHES] = buildWatches();
    m_pages[TAB_CART] = m_cartIo = new CartIoTab(m_dbg);
    m_pages[TAB_TRACE] = buildHist(0);
    m_pages[TAB_HISTORY] = buildHist(1);
    m_pages[TAB_VIDEO] = buildVideo();
    m_pages[TAB_SOUND] = m_soundTab = new SoundTab(m_dbg);
    m_pages[TAB_SYMBOLS] = buildSymbols();
    m_pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* a page's place in the tab widget is not its identity once tabs move
         * into windows of their own: each carries its own number */
        m_pages[i]->setProperty("tab", i);
        m_tabs->addTab(m_pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    {
        /* the older "cpu", "ram" and "scratchpad" names open the scratchpad */
        const QString want = qEnvironmentVariable("CHF_DEBUGGER_TAB").trimmed().toLower();
        if (want == QLatin1String("cpu") || want == QLatin1String("ram") || want == QLatin1String("scratchpad"))
            setMemSpace(CHF_SPACE_SCRATCH);
    }
    /* Help's contexts, one per tab */
    auto setHelpContext = [this](int page) {
        if (page >= 0 && page < TAB_COUNT)
            setProperty("chfHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].help)));
    };
    setHelpContext(currentTab());
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { chfHelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        const int page = currentTab();
        setHelpContext(page);
        if (page == TAB_VIDEO) m_videoForce = true;
        if (page == TAB_HISTORY) m_hSeen[1] = -1;
        refreshPage(page, false);
        if (page == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    /* for captures and tests: CHF_WATCH adds watches (a comma list; one may
     * begin "s:", "v:", "c:" or "io:" for the SCRATCH, VRAM, CART or IO space),
     * CHF_WATCH_BREAK a watch with a break-on-change over it, CHF_MEM_GOTO
     * shows a range in Memory (in the space CHF_MEM_SPACE names, PROG
     * otherwise), CHF_HISTORY records the trace and CHF_POPOUT peels a tab off */
    {
        const QByteArray mg = qgetenv("CHF_MEM_GOTO");
        const QString wb = qEnvironmentVariable("CHF_WATCH_BREAK").trimmed();
        auto watchSpace = [](QString &text) {
            static const struct { const char *prefix; int space; } prefixes[] = {
                { "s:", CHF_SPACE_SCRATCH }, { "v:", CHF_SPACE_VRAM }, { "c:", CHF_SPACE_CART }, { "io:", CHF_SPACE_IO },
            };
            for (const auto &entry : prefixes)
                if (text.startsWith(QLatin1String(entry.prefix), Qt::CaseInsensitive)) {
                    text = text.mid((int)strlen(entry.prefix)).trimmed();
                    return entry.space;
                }
            return (int)CHF_SPACE_PROG;
        };
        for (QString text : qEnvironmentVariable("CHF_WATCH").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            text = text.trimmed();
            const int space = watchSpace(text);
            if (!text.isEmpty()) chfwatch_add(space, text.toUtf8().constData());
        }
        if (!wb.isEmpty()) {
            QString text = wb;
            const int space = watchSpace(text);
            const int index = chfwatch_add(space, text.toUtf8().constData());
            if (index >= 0) watchBreak(index);
        }
        const QString spaceName = qEnvironmentVariable("CHF_MEM_SPACE").trimmed();
        if (!spaceName.isEmpty()) {
            bool isNum = false;
            int space = spaceName.toInt(&isNum);
            if (!isNum) {
                space = -1;
                for (int i = 0; i <= CHF_SPACE_IO; ++i)
                    if (spaceName.compare(QLatin1String(chfmem_space_name(i)), Qt::CaseInsensitive) == 0) space = i;
            }
            if (space >= 0 && space <= CHF_SPACE_IO) setMemSpace(space);
        }
        uint32_t lo, hi;
        if (!mg.isEmpty() && chfsym_parse_range(m_dbg, memSpace(), mg.constData(), &lo, &hi))
            showInMemory(memSpace(), lo, hi);
        if (tabFromEnv("CHF_POPOUT") >= 0) popOut(tabFromEnv("CHF_POPOUT"));
    }

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

/* Only at exit, with the main window: the session outlives its windows. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;       /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
    if (chfdebug_is_attached(m_dbg)) chfdebug_detach(m_dbg);
}

/* ---- building ------------------------------------------------------------- */

/* Two rows: the stepping controls, Pop Out and Help; then Break at, the
 * symbols, Save and the status line, which has the room that is left and
 * wraps rather than being cut. */
QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    auto *h = new QHBoxLayout;
    h->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(h);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: its
     * stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    m_runBtn->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:%2; }").arg(accentColor().name(), accentInk().name()));
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { chfdebug_step(m_dbg); }); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a call (PI, PK, the BIOS's PUSHK): on to the instruction after it"),
        [this] { stepAnd([this] { chfdebug_step_over(m_dbg); }); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the routine's return (POP, PK, or the BIOS's POPK)"),
        [this] { stepAnd([this] { chfdebug_step_out(m_dbg); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Scanline+1"), QStringLiteral("On to the next line of the picture"),
        [this] { stepAnd([this] { chfdebug_scanline(m_dbg, 1); }); });
    add(QStringLiteral("Frame+1 (Shift+F7)"), QStringLiteral("On to the end of the frame"),
        [this] { stepAnd([this] { chfdebug_frame(m_dbg, 1); }); });
    add(QStringLiteral("Clear Breakpoints"), QStringLiteral("Remove every breakpoint"), [this] {
        chfdebug_breakpoint_clear(m_dbg);
        resync();
    });
    fixToggleSize(m_runBtn, { QStringLiteral("Stop (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());
    h->addStretch(1);
    auto *pop = new QPushButton(QStringLiteral("Pop Out"));
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(pop, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    h->addWidget(pop);
    auto *help = qobject_cast<QToolButton *>(chfHelpButton(QString()));
    if (help) {
        help->setAutoRaise(false);
        help->setText(QStringLiteral("Help"));
        help->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    h->addWidget(help);

    auto *second = new QHBoxLayout;
    second->setContentsMargins(0, 0, 0, 0);
    rows->addLayout(second);
    second->addWidget(new QLabel(QStringLiteral("Break at:")));
    m_breakAt = smallEntry(QStringLiteral("symbol or $addr"), 18);
    m_breakAt->setToolTip(QStringLiteral("A symbol or a hex address, then Enter: an execute breakpoint there"));
    connect(m_breakAt, &QLineEdit::returnPressed, this, [this] { breakAt(); });
    second->addWidget(m_breakAt);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from an AS .map or .lst, or an \"AAAA name\" list"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    second->addWidget(sym);
    auto *clearSym = new QPushButton(QStringLiteral("Clear Symbols"));
    clearSym->setFocusPolicy(Qt::NoFocus);
    clearSym->setToolTip(QStringLiteral("Drop every loaded symbol and label; the built-in ones stay"));
    connect(clearSym, &QPushButton::clicked, this, [this] { clearSymbols(); });
    second->addWidget(clearSym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save…"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (the cartridge, as mapped)", "disassembly.asm" },
        { "mailbox", "Mailbox ($F800-$FFFF)", "mailbox.bin" },
        { "vram", "Video RAM (8192 bytes)", "vram.bin" },
        { "scratch", "Scratchpad (64 bytes)", "scratch.bin" },
        { "cart", "Cartridge image", "cartridge.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "vramshot", "Whole video RAM (PPM)", "vram.ppm" },
        { "regs", "CPU, ports and I/O state", "registers.txt" } };
    for (const auto &entry : saves) {
        const char *kind = entry.kind;
        const QString title = QString::fromUtf8(entry.title), file = QString::fromUtf8(entry.file);
        menu->addAction(title, this, [this, kind, title, file] { save(kind, title, file); });
    }
    saveBtn->setMenu(menu);
    second->addWidget(saveBtn);

    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    second->addWidget(m_status, 1);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $0900 or 0x900 "
                                          "for hex, o17 for octal, bare digits are decimal.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, bpa, bpin, bpv, print, mem, poke, "
                                               "runto, disasm, watch, find, scratch, kstack, video ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The registers, under the tabs and so on every one of them: the program
 * and data counters, A, W with its flags, the ISAR in octal and the K stack's
 * depth (Enter sets one, only while stopped), then the data bus and the beam.
 * J, H, K and Q are the scratchpad grid's, in the Memory tab. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto *regs = new QHBoxLayout;
    regs->setSpacing(8);
    auto addReg = [&](int reg, const QString &name, int digits, const QString &tip) {
        auto *label = new QLabel(name);
        label->setStyleSheet(kGray);
        regs->addWidget(label);
        QLineEdit *e = m_reg[reg] = new QLineEdit;
        setMono(e);
        e->setMaxLength(12);
        setDigits(e, digits);
        e->setToolTip(tip + QStringLiteral(" (Enter sets it, only while stopped)"));
        connect(e, &QLineEdit::returnPressed, this, [this, reg] { setReg(reg); });
        regs->addWidget(e);
    };
    for (const auto &entry : kRegs) {
        addReg(entry.id, QString::fromUtf8(entry.name), entry.digits, QString::fromUtf8(entry.tip));
        if (entry.id == CHF_REG_W) {
            for (int i = 0; i < 5; ++i) {
                m_flag[i] = new QCheckBox(QString::fromUtf8(kFlags[i].name));
                m_flag[i]->setToolTip(QString::fromUtf8(kFlags[i].tip) + QStringLiteral(" (a click sets it, only while stopped)"));
                m_flag[i]->setFocusPolicy(Qt::NoFocus);
                const int reg = kFlags[i].reg;
                QCheckBox *box = m_flag[i];
                connect(box, &QCheckBox::clicked, this, [this, reg, box](bool on) {
                    if (!chfdebug_is_stopped(m_dbg)) {
                        message(QStringLiteral("Stop the machine first"));
                        refreshCpu();
                        return;
                    }
                    chfdebug_cpu_set(m_dbg, reg, on ? 1 : 0);
                    box->clearFocus();
                    resync();
                });
                regs->addWidget(box);
            }
        }
    }
    auto *kLabel = new QLabel(QStringLiteral("K depth"));
    kLabel->setStyleSheet(kGray);
    regs->addWidget(kLabel);
    m_kdepth = new QLineEdit;
    setMono(m_kdepth);
    m_kdepth->setMaxLength(4);
    setDigits(m_kdepth, 2);
    m_kdepth->setToolTip(QStringLiteral("How many returns the BIOS's K stack holds: (r59 - 40) / 2, \"-\" when r59 is "
                                        "not a K-stack pointer. Enter sets r59 (only while stopped)"));
    connect(m_kdepth, &QLineEdit::returnPressed, this, [this] { setKDepth(); });
    regs->addWidget(m_kdepth);
    regs->addStretch(1);
    v->addLayout(regs);

    /* the data bus and the beam: shown, not set */
    auto *flags = new QHBoxLayout;
    m_opcode = new QLabel;
    setMono(m_opcode);
    m_opcode->setToolTip(QStringLiteral("The data bus: the opcode the CPU has just fetched. J, H, K and Q are the "
                                        "scratchpad grid's (Memory, SCRATCH)"));
    flags->addWidget(m_opcode);
    flags->addSpacing(16);
    m_beam = new QLabel;
    setMono(m_beam);
    m_beam->setStyleSheet(kGray);
    flags->addWidget(m_beam, 1);
    v->addLayout(flags);
    return w;
}

/* The dump of the space chosen (with the scratchpad grid beside it for
 * SCRATCH), Go to (a label, address, expression or range, which it marks), the
 * write control, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *head = new QHBoxLayout;
    m_memTitle = titleLabel(QStringLiteral("Memory"));
    head->addWidget(m_memTitle);
    m_memSpace = new QComboBox;
    m_memSpace->addItem(QStringLiteral("PROG: the CPU's view, $0000-$FFFF"), (int)CHF_SPACE_PROG);
    m_memSpace->addItem(QStringLiteral("SCRATCH: r0-r63"), (int)CHF_SPACE_SCRATCH);
    m_memSpace->addItem(QStringLiteral("VRAM: row × 128 + column"), (int)CHF_SPACE_VRAM);
    m_memSpace->addItem(QStringLiteral("CART: the cartridge's own memory"), (int)CHF_SPACE_CART);
    m_memSpace->addItem(QStringLiteral("IO: the ports as read now"), (int)CHF_SPACE_IO);
    m_memSpace->setToolTip(QStringLiteral("Reads have no side effects. An edit goes in as the CPU's write would: "
                                          "the BIOS and ROM are not writable. Go to, Search, Poke and the marks "
                                          "work in the space chosen (p[] s[] v[] c[] io[] in an expression)"));
    head->addWidget(m_memSpace);
    head->addStretch(1);
    v->addLayout(head);

    auto *body = new QHBoxLayout;
    m_mem = monoView();
    body->addWidget(m_mem, 1);
    m_scratch = new ScratchPanel(m_dbg);
    m_scratch->canEdit = [this] {
        if (chfdebug_is_stopped(m_dbg)) return true;
        message(QStringLiteral("Stop the machine to edit a register"));
        return false;
    };
    m_scratch->onEdit = [this](int n, unsigned val) {
        chfdebug_cpu_set(m_dbg, CHF_REG_R0 + n, (int)val);
        resync();
    };
    m_scratchScroll = new QScrollArea;
    m_scratchScroll->setWidget(m_scratch);
    m_scratchScroll->setWidgetResizable(true);
    m_scratchScroll->setFrameShape(QFrame::NoFrame);
    m_scratchScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scratchScroll->setFixedWidth(m_scratch->sizeHint().width() + m_scratchScroll->verticalScrollBar()->sizeHint().width() + 4);
    m_scratchScroll->hide();
    body->addWidget(m_scratchScroll);
    v->addLayout(body, 1);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$0802, label or lo-hi"), 20);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("$8000"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addWidget(hintLabel(QStringLiteral("Cartridge RAM takes it; the BIOS and ROM do not")), 1);
    v->addLayout(nav);
    v->addWidget(buildSearchBar());

    connect(m_memSpace, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_hl && m_hlSpace != memSpace()) m_hl = false;
        m_memShown = -1;
        m_scratchScroll->setVisible(memSpace() == CHF_SPACE_SCRATCH);
        m_srchNote->clear();
        refreshMem();
        m_mem->verticalScrollBar()->setValue(0);
        if (m_hl) scrollMemory(m_hlLo);
    });
    return w;
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 5A) or text, one byte per character"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    setMono(m_srchEntry);
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text to find"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    row->addWidget(m_srchNote);
    return bar;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    auto *sym = new QPushButton(QStringLiteral("Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Load labels from an AS .map or listing, or a list of addresses and names"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Beside the Cartridge"), this, [this] { loadSymbols(false); });
    symMenu->addAction(QStringLiteral("Clear Symbols"), this, [this] { clearSymbols(); });
    sym->setMenu(symMenu);
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addWidget(sym);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_dbg);
    return m_video;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(QStringLiteral("font-weight: bold;"));
    m_bpStop->setWordWrap(true);
    v->addWidget(m_bpStop);

    v->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                          "access. Port breakpoints take a port (0, 1, 4, 5, or a cartridge's); VRAM "
                                          "writes a cell (row,col) or two corners; a scratchpad write a register (r9, "
                                          "j), and stops on a change. A condition may use old and the "
                                          "registers. Breakpoints are kept while the debugger is closed, but only "
                                          "act while it is open.")));

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Breakpoint"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        chfdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    for (const auto &entry : kBpTypes) m_bpType->addItem(QString::fromUtf8(entry.label));
    m_bpType->setToolTip(QStringLiteral("What to stop on; the range is in the space it names"));
    m_bpStart = smallEntry(QString::fromUtf8(kBpTypes[0].hint), 28);
    m_bpEnd = smallEntry(QString::fromUtf8(kBpTypes[0].endHint), 22);
    connect(m_bpType, &QComboBox::currentIndexChanged, this, [this](int row) {
        m_bpStart->setPlaceholderText(QString::fromUtf8(kBpTypes[bpKindAt(row)].hint));
        m_bpEnd->setPlaceholderText(QString::fromUtf8(kBpTypes[bpKindAt(row)].endHint));
    });
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == 5 && value != old)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear Breakpoints"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        chfdebug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtSpace = new QComboBox;
    for (int space = 0; space <= CHF_SPACE_IO; ++space)
        m_wtSpace->addItem(QString::fromUtf8(chfmem_space_name(space)), space);
    m_wtSpace->setToolTip(QStringLiteral("The space the watch reads"));
    row->addWidget(m_wtSpace);
    m_wtEntry = new QLineEdit;
    setMono(m_wtEntry);
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(m_wtEntry->text()); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(m_wtEntry->text()); });
    row->addWidget(m_wtEntry, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_wtTable = new QTableWidget(0, 2);
    m_wtTable->setHorizontalHeaderLabels({ QStringLiteral("Watch"), QString() });
    m_wtTable->verticalHeader()->setVisible(false);
    m_wtTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_wtTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_wtTable->setColumnWidth(1, 350);
    m_wtTable->verticalHeader()->setDefaultSectionSize(34);
    m_wtTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_wtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wtTable->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_wtTable, 1);
    v->addWidget(hintLabel(QStringLiteral("The values are read at a stop and show - while the machine runs. A watch "
                                          "is kept as text, so a label that loads later starts resolving. "
                                          "Break on change stops after a write that alters the value (PROG, "
                                          "VRAM, SCRATCH and IO watches).")));
    return w;
}


/* Trace (idx 0) reads the instruction ring live; History (idx 1) reads it at
 * a stop. They share the one ring and recorder, so the two Record buttons
 * follow each other. */
QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);
    m_hRec[idx]->setFocusPolicy(Qt::NoFocus);
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    m_hRec[idx]->setStyleSheet(
        QStringLiteral("QPushButton:checked { background:%1; color:%2; }").arg(accentColor().name(), accentInk().name()));
    fixToggleSize(m_hRec[idx], { QStringLiteral("Record") }, m_hRec[idx]->sizeHint().height());
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync) return;
        chfdebug_trace_enable(m_dbg, on);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                               QStringLiteral("In range") });
    m_hRange[idx] = new QLineEdit;
    setMono(m_hRange[idx]);
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, idx](int) { refreshHist(idx, true); });
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, [this, idx] { refreshHist(idx, true); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_trace_clear(m_dbg);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setStyleSheet(kGray);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    v->addLayout(bar);
    m_hText[idx] = monoView();
    v->addWidget(m_hText[idx], 1);
    return w;
}


/* A filter over a table of address, name and bank; activating a row opens
 * that address in the Disassembly. */
QWidget *DebuggerWindow::buildSymbols()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    m_symFilter->setPlaceholderText(QStringLiteral("part of a name"));
    m_symFilter->setClearButtonEnabled(true);
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(true); });
    row->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    m_symCount->setStyleSheet(kGray);
    row->addWidget(m_symCount);
    v->addLayout(row);

    m_symTable = new QTableWidget(0, 3);
    m_symTable->setHorizontalHeaderLabels({ QStringLiteral("Address"), QStringLiteral("Name"), QStringLiteral("Bank") });
    m_symTable->verticalHeader()->setVisible(false);
    m_symTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_symTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_symTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_symTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_symTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMono(m_symTable);
    auto activate = [this](int row) {
        QTableWidgetItem *item = row >= 0 ? m_symTable->item(row, 0) : nullptr;
        if (item) showInDisassembly((uint16_t)item->data(Qt::UserRole).toUInt());
    };
    connect(m_symTable, &QTableWidget::cellDoubleClicked, this, [activate](int row, int) { activate(row); });
    connect(m_symTable, &QTableWidget::itemActivated, this, [activate](QTableWidgetItem *item) { activate(item->row()); });
    v->addWidget(m_symTable, 1);
    v->addWidget(hintLabel(QStringLiteral("Double-click a row (or press Enter on it) to open its address in the "
                                          "Disassembly.")));
    return w;
}

/* The packets left (a note and Clear over them), the link and counts
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's mailbox")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool stopped = chfdebug_is_stopped(m_dbg) != 0;
    reason[0] = '\0';
    chfdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    QString text = QStringLiteral("Running");
    if (stopped) {
        text = stoppedText(stripControl(reason));
        /* where, by name when it has one */
        char name[64];
        if (addr >= 0 && chfdebug_address_label(m_dbg, (uint16_t)addr, name, sizeof name) > 0 && name[0])
            text += QStringLiteral("  (%1)").arg(QString::fromUtf8(name));
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
    }
    const QString run = stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stopped) m_runBtn->setChecked(stopped);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    chfdebug_cpu c;
    chfdebug_cpu_get(m_dbg, &c);
    setEntry(m_reg[CHF_REG_PC], hx((unsigned)c.pc & 0xFFFF, 4));
    setEntry(m_reg[CHF_REG_PC0], hx(c.pc0, 4));
    setEntry(m_reg[CHF_REG_PC1], hx(c.pc1, 4));
    setEntry(m_reg[CHF_REG_DC0], hx(c.dc0, 4));
    setEntry(m_reg[CHF_REG_DC1], hx(c.dc1, 4));
    setEntry(m_reg[CHF_REG_A], hx(c.a, 2));
    setEntry(m_reg[CHF_REG_W], hx(c.w, 2));
    setEntry(m_reg[CHF_REG_ISAR], QStringLiteral("%1.%2").arg((c.isar >> 3) & 7).arg(c.isar & 7));
    setEntry(m_kdepth, c.k_depth < 0 ? QStringLiteral("-") : QString::number(c.k_depth));
    const int flags[5] = { c.sign, c.carry, c.zero, c.overflow, c.icb };
    for (int i = 0; i < 5; ++i)
        if (m_flag[i]->isChecked() != (flags[i] != 0)) m_flag[i]->setChecked(flags[i] != 0);
    m_opcode->setText(QStringLiteral("DBUS $%1").arg(hx(c.dbus, 2)));
    const char *bios = c.bios == CHF_ROM_SL31253 ? "sl31253" : c.bios == CHF_ROM_SL90025 ? "sl90025" : "stand-in BIOS";
    m_beam->setText(QStringLiteral("frame %1  line %2 cycle %3  %4 clocks  %5 instructions  %6%7")
                        .arg(c.frame).arg(c.vpos, 3).arg(c.hpos, 3).arg((qulonglong)c.cycles)
                        .arg((qulonglong)c.instructions).arg(QString::fromUtf8(bios),
                             c.illegal_count ? QStringLiteral("  %1 illegal, last at $%2").arg(c.illegal_count)
                                                   .arg(hx((unsigned)c.illegal_pc & 0xFFFF, 4))
                                             : QString()));
}

void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_MAX];
    static char text[MEM_MAX / 16 * MEM_ROW_TEXT + 256];
    static chfdebug_region map[REGION_MAX];
    const int space = memSpace();
    int size = chfdebug_space_size(m_dbg, space);
    if (size > MEM_MAX) size = MEM_MAX;
    m_memTitle->setText(QStringLiteral("Memory: %1").arg(QString::fromUtf8(chfmem_space_name(space))));
    if (space == CHF_SPACE_SCRATCH) {
        chfdebug_cpu c;
        chfdebug_cpu_get(m_dbg, &c);
        m_scratch->refresh(c);
    }
    if (size <= 0) {
        setText(m_mem, QStringLiteral("(this space is not available: the debugger is not attached)"));
        m_mem->setExtraSelections({});
        return;
    }
    memset(mem, 0, sizeof mem);
    chfdebug_read(m_dbg, space, 0, mem, size);
    const int regions = space == CHF_SPACE_PROG ? chfdebug_memory_map(m_dbg, map, REGION_MAX) : 0;
    const int rows = (size + 15) / 16;
    const int digits = size > 0x10000 ? 5 : 4;

    int len = snprintf(text, sizeof text, "%*s", digits + 3, "");
    for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%X  ", col);
    for (int row = 0; row < rows; ++row) {
        const uint8_t *bytes = mem + row * 16;
        len += snprintf(text + len, sizeof text - (size_t)len, "\n$%0*X: ", digits, row * 16);
        for (int col = 0; col < 16; ++col) len += snprintf(text + len, sizeof text - (size_t)len, "%02X ", bytes[col]);
        text[len++] = ' ';
        for (int col = 0; col < 16; ++col) text[len++] = bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.';
        char name[64] = "";
        if (space == CHF_SPACE_PROG)
            chfmem_region_name(map, regions, (uint16_t)(row * 16), name, sizeof name);
        else if (space == CHF_SPACE_VRAM)
            snprintf(name, sizeof name, "row %d cols %d-%d", row / 8, (row % 8) * 16, (row % 8) * 16 + 15);
        else if (space == CHF_SPACE_SCRATCH)
            snprintf(name, sizeof name, "r%d-r%d", row * 16, row * 16 + 15);
        else if (space == CHF_SPACE_IO)
            snprintf(name, sizeof name, "ports $%02X-$%02X", row * 16, row * 16 + 15);
        len += snprintf(text + len, sizeof text - (size_t)len, "  %s", name);
    }
    setText(m_mem, QString::fromLatin1(text, len));

    /* the ranges marked: each row's bytes in the hex and in the text column */
    QList<QTextEdit::ExtraSelection> sel;
    const int prefix = digits + 3;
    auto mark = [&](uint32_t from, uint32_t to, const QTextCharFormat &fmt) {
        if (to >= (uint32_t)size) to = (uint32_t)size - 1;
        for (uint32_t row = from / 16; from <= to && row <= to / 16; ++row) {
            const int first = qMax((int)from - (int)row * 16, 0), last = qMin((int)to - (int)row * 16, 15);
            sel << spanSelection(m_mem, 1 + (int)row, prefix + first * 3, (last - first) * 3 + 2, fmt);
            sel << spanSelection(m_mem, 1 + (int)row, prefix + 48 + 1 + first, last - first + 1, fmt);
        }
    };
    for (int i = 0; i < chfwatch_count(); ++i) {
        uint32_t lo, hi;
        if (chfwatch_space(i) == space && chfwatch_range(m_dbg, i, &lo, &hi)) mark(lo, hi, watchFormat());
    }
    if (m_hl && m_hlSpace == space) mark(m_hlLo, m_hlHi, accentFormat());
    m_mem->setExtraSelections(sel);
    m_memShown = space;
}

void DebuggerWindow::scrollMemory(uint32_t addr)
{
    const int line = 1 + (int)(addr / 16);
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* The space the Memory tab shows, and a switch to another (which marks and
 * scrolls nothing itself). */
int DebuggerWindow::memSpace() const
{
    return m_memSpace ? m_memSpace->currentData().toInt() : (int)CHF_SPACE_PROG;
}

void DebuggerWindow::setMemSpace(int space)
{
    const int index = m_memSpace->findData(space);
    if (index >= 0 && index != m_memSpace->currentIndex()) m_memSpace->setCurrentIndex(index);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static chfdebug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        chfdebug_cpu c;
        chfdebug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = chfdebug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[128];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? QStringLiteral("●") : lines[i].bp_disabled ? QStringLiteral("○")
                                                                                          : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-11.11s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[48];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0] && i != pcLine)
            grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth ? (int)strlen(lines[i].label)
                                                                                      : labelWidth });
        snprintf(buf, sizeof buf, "%-22s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].comment[0]) {
            const QString comment = QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
            if (i != pcLine) grays.push_back({ i, (int)ln.size(), (int)comment.size() });
            ln += comment;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    /* The PC's line is the accent bar: the block's own background, not an
     * extra selection -- a full-width selection paints over the gutter's
     * red dot, a block background lies under it. */
    if (pcLine >= 0 && pcLine < n) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        QTextBlockFormat bf;
        bf.setBackground(accentColor());
        cur.setBlockFormat(bf);
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat cf;
        cf.setForeground(accentInk());
        if (lines[pcLine].address == m_selected) {
            cf.setFontUnderline(true);
            cf.setFontWeight(QFont::Bold);
        }
        cur.mergeCharFormat(cf);
    }

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and comments in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red (white on the PC's bar), a disabled one grey */
        if (lines[i].has_breakpoint || lines[i].bp_disabled) {
            QTextCharFormat f;
            if (lines[i].has_breakpoint) {
                f.setForeground(i == pcLine ? QColor(0xa0, 0x10, 0x18) : QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(i == pcLine ? QColor(0x55, 0x55, 0x55) : QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined (the PC's own is done above) */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    m_disasm->setExtraSelections(sel);
}

/* The Video tab takes its own snapshot; a stopped machine redraws only when
 * the core says something changed. */
void DebuggerWindow::refreshVideo(bool force)
{
    m_video->refresh(force);
}

void DebuggerWindow::refreshBps()
{
    static chfdebug_breakpoint bps[BP_MAX];
    const int n = chfdebug_breakpoint_list(m_dbg, bps, BP_MAX);

    if (chfdebug_is_stopped(m_dbg)) {
        char reason[160];
        int addr = -1;
        reason[0] = '\0';
        chfdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        m_bpStop->setText(stoppedText(stripControl(reason)) + QLatin1Char('.'));
    } else {
        m_bpStop->setText(QStringLiteral("Running."));
    }

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        char desc[256];
        chfbp_describe(m_dbg, &bps[i], desc, sizeof desc);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(stripControl(desc)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 3; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            chfdebug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 3, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

void DebuggerWindow::refreshCart()
{
    m_cartIo->refresh();
}

void DebuggerWindow::refreshSound()
{
    m_soundTab->refresh();
}

/* The labels the engine holds, filtered, in the order the filter gives
 * (address, then name). The table is rebuilt only when what it would show
 * changed: the engine's own index order is not stable, so the entries are
 * fetched anew each time and compared by content. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const int total = chfdebug_symbols_count(m_dbg);
    const QByteArray filter = m_symFilter->text().trimmed().toUtf8();
    std::vector<int> indices((size_t)qMax(total, 0));
    const int shown = total > 0 ? chfdebug_symbols_filter(m_dbg, filter.constData(), indices.data(), total) : 0;

    struct Row { uint16_t addr; char name[96]; int bank; };
    std::vector<Row> rows;
    rows.reserve((size_t)shown);
    QString sig = QStringLiteral("%1/%2/%3:").arg(total).arg(shown).arg(QString::fromUtf8(filter));
    for (int i = 0; i < shown; ++i) {
        Row row = {};
        if (!chfdebug_symbols_get(m_dbg, indices[(size_t)i], &row.addr, row.name, (int)sizeof row.name, &row.bank)) continue;
        rows.push_back(row);
        sig += QStringLiteral("%1 %2 %3;").arg(row.addr).arg(row.bank).arg(QString::fromUtf8(row.name));
    }
    if (!force && sig == m_symSig) return;
    m_symSig = sig;

    m_symCount->setText(QStringLiteral("%1 of %2 symbols").arg(rows.size()).arg(total));
    const int top = m_symTable->verticalScrollBar()->value();
    m_symTable->setUpdatesEnabled(false);
    m_symTable->setRowCount((int)rows.size());
    for (int i = 0; i < (int)rows.size(); ++i) {
        auto *addr = new QTableWidgetItem(QStringLiteral("$") + hx(rows[(size_t)i].addr, 4));
        addr->setData(Qt::UserRole, rows[(size_t)i].addr);
        m_symTable->setItem(i, 0, addr);
        m_symTable->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(rows[(size_t)i].name)));
        m_symTable->setItem(i, 2, new QTableWidgetItem(rows[(size_t)i].bank >= 0 ? QString::number(rows[(size_t)i].bank)
                                                                                 : QString()));
    }
    m_symTable->setUpdatesEnabled(true);
    m_symTable->verticalScrollBar()->setValue(top);
}

void DebuggerWindow::addWatch(const QString &entry)
{
    const QByteArray text = entry.trimmed().toUtf8();
    if (text.isEmpty()) return;
    if (chfwatch_add(m_wtSpace->currentData().toInt(), text.constData()) < 0) {
        message(QStringLiteral("The watch list is full"));
        return;
    }
    m_wtEntry->clear();
    refreshWatches(true);
}

void DebuggerWindow::watchBreak(int index)
{
    uint32_t lo, hi;
    if (!chfwatch_range(m_dbg, index, &lo, &hi)) {
        message(QStringLiteral("That watch does not resolve yet"));
        return;
    }
    int type = CHFDEBUG_BP_WRITE;
    switch (chfwatch_space(index)) {
    case CHF_SPACE_PROG: break;
    case CHF_SPACE_VRAM: type = CHFDEBUG_BP_VRAM; break;
    case CHF_SPACE_SCRATCH: type = CHFDEBUG_BP_SCRATCH; break;
    case CHF_SPACE_IO: type = CHFDEBUG_BP_OUT; break;
    default:
        message(QStringLiteral("A cartridge-space watch cannot break on change"));
        return;
    }
    if (chfdebug_breakpoint_add(m_dbg, type, chfwatch_space(index), (uint16_t)lo, (uint16_t)hi, "value != old") < 0)
        message(QStringLiteral("The breakpoint could not be added"));
    else
        message(QStringLiteral("Breaking when it changes"));
    resync();
}

/* One row a watch, its value live (a dash while the machine runs). The rows
 * are rebuilt when the list changed; otherwise only their text moves. */
void DebuggerWindow::refreshWatches(bool force)
{
    const int n = chfwatch_count();
    const unsigned gen = chfwatch_generation();
    QFont mono = m_wtTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    if (force || gen != m_wtGen || m_wtTable->rowCount() != n) {
        m_wtGen = gen;
        m_wtTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            auto *item = new QTableWidgetItem;
            item->setFont(mono);
            m_wtTable->setItem(i, 0, item);
            auto *box = new QWidget;
            auto *h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto *mem = new QPushButton(QStringLiteral("Memory"));
            mem->setToolTip(QStringLiteral("Show it in the Memory tab"));
            connect(mem, &QPushButton::clicked, this, [this, i] {
                uint32_t lo, hi;
                if (!chfwatch_range(m_dbg, i, &lo, &hi)) {
                    message(QStringLiteral("That watch does not resolve yet"));
                    return;
                }
                if (m_popped[TAB_MEMORY]) m_popped[TAB_MEMORY]->activateWindow();
                else m_tabs->setCurrentWidget(m_pages[TAB_MEMORY]);
                showInMemory(chfwatch_space(i), lo, hi);
            });
            auto *brk = new QPushButton(QStringLiteral("Break on change"));
            brk->setToolTip(QStringLiteral("Stop after a write that alters it"));
            connect(brk, &QPushButton::clicked, this, [this, i] { watchBreak(i); });
            auto *rm = new QPushButton(QStringLiteral("Remove"));
            connect(rm, &QPushButton::clicked, this, [this, i] {
                chfwatch_remove(i);
                QTimer::singleShot(0, this, [this] { refreshWatches(true); });
            });
            for (QPushButton *b : { mem, brk, rm }) {
                b->setFocusPolicy(Qt::NoFocus);
                h->addWidget(b);
            }
            m_wtTable->setCellWidget(i, 1, box);
        }
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        chfwatch_format(m_dbg, i, line, sizeof line);
        const QString text = stripControl(line);
        if (m_wtTable->item(i, 0)->text() != text) m_wtTable->item(i, 0)->setText(text);
    }
}

/* Trace (live) and History (at a stop): the ring newest first, shown
 * oldest first, the newest HIST_SHOWN lines that pass the filter. */
void DebuggerWindow::refreshHist(int idx, bool force)
{
    static chfdebug_trace buf[RING_MAX];
    const bool stopped = chfdebug_is_stopped(m_dbg) != 0;
    const bool on = chfdebug_trace_enabled(m_dbg) != 0;
    const int count = chfdebug_trace_count(m_dbg);

    m_hSync = true;                     /* both Record buttons follow the engine */
    for (int i = 0; i < 2; ++i)
        if (m_hRec[i]->isChecked() != on) m_hRec[i]->setChecked(on);
    m_hSync = false;

    if (force) m_hSeen[idx] = -1;
    /* History reads at a stop (it keeps what it showed while the machine
     * runs); at a stop neither view rebuilds for the same count */
    if (idx == 1 && !stopped && m_hSeen[1] >= 0) return;
    if (stopped && count == m_hSeen[idx]) return;
    m_hSeen[idx] = count;

    uint32_t lo = 0, hi = 0xFFFF;
    int filter = CHFDEBUG_TRACE_ALL;
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = CHFDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (chfsym_parse_range(m_dbg, CHF_SPACE_PROG, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi))
            filter = CHFDEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    const int n = chfdebug_trace_read(m_dbg, buf, RING_MAX);
    QList<int> hit;
    for (int i = 0; i < n && hit.size() < HIST_SHOWN; ++i)
        if (chfdebug_trace_match(&buf[i], filter, (uint16_t)lo, (uint16_t)hi)) hit.append(i);
    QString t;
    if (!on && n == 0) t = QStringLiteral("Recording is off. Press Record, then let the machine run.");
    char line[256];
    for (int k = (int)hit.size() - 1; k >= 0; --k) {
        chfdebug_trace_format(&buf[hit[k]], line, sizeof line);
        t += QString::fromUtf8(line);
        if (k) t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(on ? QStringLiteral("recording, %1 instructions").arg(n)
                             : n ? QStringLiteral("recording off, %1 instructions").arg(n)
                                 : QStringLiteral("recording off"));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static chfdebug_fuji_packet p[TRACE_MAX];
    static char state[4096];
    const int n = chfdebug_fujinet_trace(m_dbg, p, TRACE_MAX);
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
    chfdebug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: refreshMem(); break;
    case TAB_VIDEO: refreshVideo(std::exchange(m_videoForce, false)); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(false); break;
    case TAB_CART: refreshCart(); break;
    case TAB_TRACE: refreshHist(0, false); break;
    case TAB_HISTORY: if (!live) refreshHist(1, false); break;
    case TAB_SOUND: refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The registers show everywhere; then the tab showing and any in a window of
 * its own. */
void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshCpu();
    refreshPage(currentTab(), false);
    for (int t = 0; t < TAB_COUNT; ++t)
        if (m_popped[t]) refreshPage(t, false);
}

/* A page's number, whichever place it holds in the tab widget now. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* A new stop brings every view to the spot, whatever the reader had
 * scrolled to: the Disassembly to the PC, Memory to the range it marks (or
 * the PC), the Trace and History to their newest line, the Video tab to a
 * redraw. */
void DebuggerWindow::recentreOnStop()
{
    m_followPc->setChecked(true);
    m_disasm->verticalScrollBar()->setValue(0);
    m_hSeen[0] = m_hSeen[1] = -1;
    m_videoForce = true;
    refreshAll();
    chfdebug_cpu c;
    chfdebug_cpu_get(m_dbg, &c);
    scrollMemory(m_hl && m_hlSpace == memSpace() ? m_hlLo : memSpace() == CHF_SPACE_PROG ? (uint32_t)c.pc : 0u);
    for (QPlainTextEdit *view : m_hText)
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then every view follows it. */
void DebuggerWindow::resync()
{
    const bool stopped = chfdebug_is_stopped(m_dbg) != 0;
    const uint64_t serial = chfdebug_stop_serial(m_dbg);
    const bool newStop = stopped && serial != m_seenSerial;
    if (newStop) {
        char reason[160];
        int addr;
        reason[0] = '\0';
        chfdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        /* a step already says where it is */
        if (reason[0] && strncmp(reason, "step", 4) != 0)
            consoleAppend(QStringLiteral("— %1\n").arg(stripControl(reason)));
    }
    m_seenSerial = serial;
    m_wasStopped = stopped;
    m_seenGen = chfdebug_generation(m_dbg);
    m_runningTicks = 0;
    if (newStop) recentreOnStop();
    else refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !chfdebug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation and the stop serial, so
 * this polls them: a refresh when they moved, and the live values twice a
 * second while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = chfdebug_generation(m_dbg);
    const bool stopped = chfdebug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped || chfdebug_stop_serial(m_dbg) != m_seenSerial) {
        resync();
    } else if (tabVisible(TAB_WATCHES) && chfwatch_generation() != m_wtGen) {
        refreshWatches(false);          /* the list changed elsewhere (the console) */
    } else if (!stopped && ++m_runningTicks >= 4) {
        m_runningTicks = 0;
        refreshStatus();
        refreshCpu();
        refreshPage(currentTab(), true);
        for (int t = 0; t < TAB_COUNT; ++t)
            if (m_popped[t]) refreshPage(t, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    chfdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = chfdebug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (chfdebug_is_stopped(m_dbg)) {
        chfdebug_resume(m_dbg);
        resync();
    } else {
        chfdebug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!chfdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { chfdebug_run_to(m_dbg, (uint16_t)m_selected); });
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    chfdebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* The toolbar's Break at: a symbol or a hex address, an execute breakpoint
 * there (one already set stays). */
void DebuggerWindow::breakAt()
{
    const QString text = m_breakAt->text().trimmed();
    if (text.isEmpty()) return;
    long addr;
    QString why;
    if (!addressOf(text, &addr, &why)) {
        message(why);
        return;
    }
    if (chfdebug_breakpoint_check(m_dbg, (uint16_t)addr))
        message(QStringLiteral("A breakpoint is already set at $%1").arg(hx((unsigned)addr, 4)));
    else {
        chfdebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
        message(QStringLiteral("Breakpoint at $%1").arg(hx((unsigned)addr, 4)));
    }
    m_breakAt->clear();
    resync();
}

/* A number typed into an entry: a label first, then bare digits as hex (as in
 * the sibling debuggers), then whatever the engine evaluates ($0900, 0x900,
 * 900h, r3+4, label+2; the console itself reads bare digits as decimal).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    uint32_t lo, hi;
    const int label = chfdebug_label_address(m_dbg, utf8.constData());
    if (label >= 0) {
        v = label;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else if (chfsym_parse_range(m_dbg, CHF_SPACE_PROG, utf8.constData(), &lo, &hi) && lo == hi) {
        v = lo;
    } else {
        reason = QStringLiteral("Not a number ($0900, or a label): %1").arg(t);
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address: 0 to $FFFF. */
bool DebuggerWindow::addressOf(const QString &text, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    if (v < 0 || v > 0xFFFF) {
        const QString reason = QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

bool DebuggerWindow::rangeOf(const QString &startText, const QString &endText, int type, int space, uint16_t *lo,
                             uint16_t *hi, QString *why)
{
    Q_UNUSED(type);
    uint32_t first = 0, last = 0;
    const QString start = startText.trimmed(), end = endText.trimmed();
    if (end.isEmpty()) {
        if (!chfsym_parse_range(m_dbg, space, start.toUtf8().constData(), &first, &last)) {
            *why = QStringLiteral("Not %1 in the %2 space: %3")
                       .arg(space == CHF_SPACE_VRAM ? QStringLiteral("a cell (row,col) or range")
                                                    : space == CHF_SPACE_SCRATCH ? QStringLiteral("a register (r9, j, 9)")
                                                                                 : QStringLiteral("an address, label or range"),
                            QString::fromUtf8(chfmem_space_name(space)), start);
            return false;
        }
    } else {
        uint32_t ignore;
        if (!chfsym_parse_range(m_dbg, space, start.toUtf8().constData(), &first, &ignore)) {
            *why = QStringLiteral("Not an address in the %1 space: %2").arg(QString::fromUtf8(chfmem_space_name(space)), start);
            return false;
        }
        if (!chfsym_parse_range(m_dbg, space, end.toUtf8().constData(), &last, &ignore)) {
            *why = QStringLiteral("Not an address in the %1 space: %2").arg(QString::fromUtf8(chfmem_space_name(space)), end);
            return false;
        }
    }
    if (last < first) {
        *why = QStringLiteral("The end is before the start");
        return false;
    }
    if (last > 0xFFFF) {
        *why = QStringLiteral("Out of range");
        return false;
    }
    *lo = (uint16_t)first;
    *hi = (uint16_t)last;
    return true;
}

void DebuggerWindow::setReg(int reg)
{
    QLineEdit *e = m_reg[reg];
    long v = 0;
    QString why;
    bool ok;
    if (reg == CHF_REG_ISAR) {
        /* upper.lower in octal, or a plain octal number */
        const QString t = e->text().trimmed();
        const QStringList parts = t.split(QLatin1Char('.'));
        bool a = false, b = false;
        if (parts.size() == 2) {
            const int upper = parts[0].toInt(&a, 8), lower = parts[1].toInt(&b, 8);
            ok = a && b && upper >= 0 && upper < 8 && lower >= 0 && lower < 8;
            v = upper * 8 + lower;
        } else {
            v = t.toLong(&a, 8);
            ok = a && v >= 0 && v < 64;
        }
        if (!ok) why = QStringLiteral("The ISAR is octal, upper.lower (3.5) or 0-77");
    } else {
        ok = numberOf(e->text(), &v, &why);
    }
    if (!chfdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok) message(why);
    else chfdebug_cpu_set(m_dbg, reg, (int)v);
    e->clearFocus();
    resync();
}

/* The K depth entry: r59 = 40 + 2 * depth, the BIOS's stack pointer. */
void DebuggerWindow::setKDepth()
{
    bool ok = false;
    const int depth = m_kdepth->text().trimmed().toInt(&ok, 10);
    if (!chfdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok || depth < 0 || depth > 9) message(QStringLiteral("The K stack holds 0-9 returns"));
    else chfdebug_cpu_set(m_dbg, CHF_REG_R0 + 59, 40 + 2 * depth);
    m_kdepth->clearFocus();
    resync();
}

void DebuggerWindow::showInMemory(int space, uint32_t lo, uint32_t hi)
{
    setMemSpace(space);
    m_hl = true;
    m_hlSpace = space;
    m_hlLo = lo;
    m_hlHi = hi;
    refreshMem();
    scrollMemory(lo);
}

/* Opens an address in the Disassembly (the tab, or its window), the PC no
 * longer followed, the line selected. */
void DebuggerWindow::showInDisassembly(uint16_t addr)
{
    if (m_popped[TAB_DISASM]) m_popped[TAB_DISASM]->activateWindow();
    else m_tabs->setCurrentWidget(m_pages[TAB_DISASM]);
    m_followPc->setChecked(false);
    m_selected = addr;
    m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, addr, -(visibleRows() / 3));
    refreshDisasm();
}

void DebuggerWindow::memGoto()
{
    const QString text = m_memGoto->text().trimmed();
    if (text.isEmpty()) return;
    uint32_t lo, hi;
    if (!chfsym_parse_range(m_dbg, memSpace(), text.toUtf8().constData(), &lo, &hi)) {
        m_hl = false;
        refreshMem();
        message(QStringLiteral("Not an address, label or range in %1: %2")
                    .arg(QString::fromUtf8(chfmem_space_name(memSpace())), text));
        return;
    }
    showInMemory(memSpace(), lo, hi);
}

void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[CHFSEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!chfsearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &n, &mask,
                         &flags) || n <= 0) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 5A) or some text."));
        return;
    }
    const int space = memSpace();
    uint32_t from = 0, found = 0;
    if (m_hl && m_hlSpace == space) from = backward ? m_hlLo : m_hlLo + 1;
    const int r = chfdebug_search_next(m_dbg, space, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        m_srchNote->setText(QStringLiteral("Stop the machine first"));
        message(QStringLiteral("Stop the machine first"));
    } else if (r == 0) {
        m_srchNote->setText(QStringLiteral("Not found"));
        message(QStringLiteral("Not found"));
    } else {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, space == CHF_SPACE_CART ? 5 : 4)));
        showInMemory(space, found, found + (uint32_t)n - 1);
    }
}

void DebuggerWindow::memWrite()
{
    uint32_t a, ignore;
    long v;
    const int space = memSpace();
    if (!chfsym_parse_range(m_dbg, space, m_memAddr->text().toUtf8().constData(), &a, &ignore)) {
        message(QStringLiteral("Not an address in the %1 space: %2")
                    .arg(QString::fromUtf8(chfmem_space_name(space)), m_memAddr->text().trimmed()));
        return;
    }
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    chfdebug_write(m_dbg, space, a, (uint8_t)v);
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

void DebuggerWindow::addBreakpoint()
{
    const auto &kind = kBpTypes[bpKindAt(m_bpType->currentIndex())];
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    uint16_t lo = 0, hi = 0;
    QString why;
    if (!rangeOf(start, end, kind.type, kind.space, &lo, &hi, &why)) {
        m_bpMsg->setText(why);
        return;
    }
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (chfdebug_breakpoint_add(m_dbg, kind.type, kind.space, lo, hi, cond.constData()) < 0) {
        m_bpMsg->setText(QStringLiteral("The condition does not parse"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* A symbol file the user picks, or (fromFile false) the <cart>.sym/.map/.noi
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
    char msg[512] = "";
    const QByteArray p = path.toLocal8Bit();
    chfdebug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::clearSymbols()
{
    chfdebug_clear_symbols(m_dbg);
    say(QStringLiteral("Symbols cleared (the built-in ones stay)"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    chfdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs in windows of their own ----------------------------------------- */

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;     /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("debugger-pop-%1").arg(QLatin1String(kTabs[tab].name)));
    win->setWindowTitle(QStringLiteral("Channel F Debugger - %1").arg(QLatin1String(kTabs[tab].label)));
    win->setProperty("chfHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].help)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch(1);
    bar->addWidget(chfHelpButton(QString()));
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    static_cast<PopWindow *>(win)->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    /* the stepping keys work in the window too */
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { chfHelpFor(win); });
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { toggleRun(); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { stepAnd([this] { chfdebug_step(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F7), win, [this] { stepAnd([this] { chfdebug_frame(m_dbg, 1); }); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { stepAnd([this] { chfdebug_step_over(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { stepAnd([this] { chfdebug_step_out(m_dbg); }); });
    if (isVisible()) win->show();
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; ++t)       /* back where it was in the order */
        if (!m_popped[t]) ++pos;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    m_tabs->setCurrentIndex(pos);
    if (tab == TAB_VIDEO) m_videoForce = true;
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)chfdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? std::function<void()>([this] { chfdebug_frame(m_dbg, 1); })
                                   : std::function<void()>([this] { chfdebug_step(m_dbg); })); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { chfdebug_step_out(m_dbg); })
                                   : std::function<void()>([this] { chfdebug_step_over(m_dbg); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_timer.start(100);
    if (e->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
    chfdebug_attach(m_dbg);
    static bool historyOnce = true;     /* CHF_HISTORY: record from the first show */
    if (historyOnce && qEnvironmentVariableIsSet("CHF_HISTORY")) chfdebug_trace_enable(m_dbg, 1);
    historyOnce = false;
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        chfdebug_detach(m_dbg);
    }
    QMainWindow::hideEvent(e);
}
