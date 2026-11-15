/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running Channel F
 * session (the CONFIG client, the stand-in BIOS, FujiNet off) whose cartridge
 * RAM holds a hand-assembled loop written through the debugger: the tabs,
 * attach/detach with show and hide, F12, the stepping keys and buttons, the
 * console (transcript, Tab completion, history), the disassembly (the PC's
 * accent bar, click / double-click / F9 for a breakpoint, selection and Run
 * to Cursor, the wheel, the jump box), the register strip and the Memory
 * tab's five spaces (the scratchpad grid and K stack, Go to, Find, Poke), the
 * Breakpoints tab (every kind), Watches, Trace and History, Symbols, Break at,
 * pop-out and dock, Cart & I/O, Sound, the Video tab, and F12 and the View
 * menu from the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "chfdebug.h"
#include "chfmemtools.h"
#include "chfsession.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in the cartridge's RAM at $8100 (the machine runs it
 * once the PC is set there):
 *   8100  DCI $8200                    DC0 = the counter
 *   8103  LI $10 / LR 1,A              r1 = $10 (the start of a run is here)
 *   8106  PI $8150                     the call
 *   8109  LM                           read $8200
 *   810A  AI 1
 *   810C  DCI $8200
 *   810F  ST                           write $8200
 *   8110  JMP $8100
 *   8150  LR A,1 / INC / LR 1,A / POP  the subroutine, back to the PI's return */
const uint8_t kProg[] = {
    0x2A, 0x82, 0x00, 0x20, 0x10, 0x51, 0x28, 0x81, 0x50, 0x16, 0x24, 0x01, 0x2A, 0x82, 0x00, 0x17, 0x29, 0x81, 0x00,
};
const uint8_t kSub[] = { 0x41, 0x1F, 0x51, 0x1C };
const int kStart = 0x8100, kBegin = 0x8103, kCall = 0x8106, kAfter = 0x8109, kStore = 0x810F, kSubAt = 0x8150,
          kCounter = 0x8200;

/* The accent: the Videocarts' yellow. */
const QColor kAccent(0xF2, 0xC3, 0x00);
const QColor kInk(0x1B, 0x16, 0x00);

void put(chfdebug *d, uint32_t addr, uint8_t value)
{
    chfdebug_write(d, CHF_SPACE_PROG, addr, value);
}

uint8_t peek(chfdebug *d, uint32_t addr, int space = CHF_SPACE_PROG)
{
    uint8_t v = 0;
    chfdebug_read(d, space, addr, &v, 1);
    return v;
}

/* With the machine stopped: the program in place, PC at its second instruction. */
void loadProgram(chfdebug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) put(d, (uint32_t)(kStart + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSub; i++) put(d, (uint32_t)(kSubAt + i), kSub[i]);
    chfdebug_cpu_set(d, CHF_REG_W, 0);
    chfdebug_cpu_set(d, CHF_REG_DC0, kCounter);
    chfdebug_cpu_set(d, CHF_REG_PC, kBegin);
}

int pcOf(chfdebug *d)
{
    chfdebug_cpu c;
    chfdebug_cpu_get(d, &c);
    return c.pc;
}

int regOf(chfdebug *d, int n)
{
    chfdebug_cpu c;
    chfdebug_cpu_get(d, &c);
    return c.r[n];
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    chfsession *m_s = nullptr;
    chfdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    chfsession_start_opts m_opts = {};

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        chfsession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = chfsession_new(&paths);
        QVERIFY(m_s);
        m_d = chfsession_debugger(m_s);
        QVERIFY(m_d);

        chfsession_default_opts(m_s, &m_opts);
        m_opts.cart_path = nullptr;
        m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        QCOMPARE(chfsession_start(m_s, &m_opts), 0);

        /* the window opens on the tab CHF_DEBUGGER_TAB names, and CHF_WATCH,
         * CHF_WATCH_BREAK and CHF_MEM_GOTO are read at creation */
        qputenv("CHF_DEBUGGER_TAB", "i/o");
        qputenv("CHF_WATCH", "$8270, $8280-$828F");
        qputenv("CHF_WATCH_BREAK", "$8290");
        qputenv("CHF_MEM_GOTO", "$82A0-$82AF");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("CHF_DEBUGGER_TAB");
        qunsetenv("CHF_WATCH");
        qunsetenv("CHF_WATCH_BREAK");
        qunsetenv("CHF_MEM_GOTO");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        QCOMPARE(chfwatch_count(), 3);                      /* two watches and the break-on-change one */
        chfdebug_breakpoint list[4];
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 4), 1);
        QCOMPARE((int)list[0].start, 0x8290);
        QVERIFY(QString::fromUtf8(list[0].condition).contains(QStringLiteral("old")));
        chfwatch_clear();
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_s) {
            chfsession_stop(m_s);
            chfsession_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_attached(m_d), 2000);
        if (!chfdebug_is_stopped(m_d)) chfdebug_stop(m_d);
        waitStopped();
        chfdebug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Cart && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video"), QStringLiteral("Sound"),
                                      QStringLiteral("Symbols"), QStringLiteral("FujiNet") }));
        QVERIFY(chfdebug_is_attached(m_d));
        QVERIFY(chfdebug_is_stopped(m_d));
        /* Help's context follows the tab: the twelve names of the Help's debugger section */
        const char *const contexts[] = { "console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                         "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int i = 0; i < 12; ++i) {
            m_tabs->setCurrentIndex(i);
            QCOMPARE(m_win->property("chfHelpContext").toString(),
                     QStringLiteral("debugger/") + QLatin1String(contexts[i]));
        }
        m_tabs->setCurrentIndex(0);
        /* no tab sets the window's minimum height beyond a laptop screen */
        QVERIFY2(m_win->minimumSizeHint().height() <= 800, qPrintable(QString::number(m_win->minimumSizeHint().height())));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        QVERIFY(run->styleSheet().contains(kAccent.name()));
        QVERIFY(run->styleSheet().contains(kInk.name()));
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
        /* the toolbar has the family's buttons */
        for (const char *label : { "Step (F7)", "Step Over", "Step Out", "Run to Cursor", "Scanline+1", "Frame+1",
                                   "Clear Breakpoints", "Load Symbols", "Clear Symbols", "Save", "Pop Out" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), kBegin);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kBegin + 2, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the PI call: run to the instruction after it */
        chfdebug_cpu_set(m_d, CHF_REG_PC, kCall);
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step over")));
        /* the button steps out of the subroutine: to the return the POP takes */
        chfdebug_cpu_set(m_d, CHF_REG_PC1, kAfter);
        chfdebug_cpu_set(m_d, CHF_REG_PC, kSubAt);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kAfter, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step out")));
        /* Frame: Shift+F7 lands on a frame boundary */
        chfdebug_cpu c, c2;
        chfdebug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) &&
                                     (chfdebug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* Scanline+1 */
        chfdebug_cpu_get(m_d, &c);
        QTest::mouseClick(button(QStringLiteral("Scanline+1")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) &&
                                     (chfdebug_cpu_get(m_d, &c2), c2.vpos != c.vpos || c2.frame != c.frame), 3000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    /* The stop reason is the status line, and it is never cut: the label has
     * the width its whole text needs at the default window width. */
    void stopReasonNotElided()
    {
        chfdebug_breakpoint_add(m_d, CHFDEBUG_BP_WRITE, CHF_SPACE_PROG, kCounter, kCounter, "value != old");
        put(m_d, kCounter, 0x77);
        chfdebug_cpu_set(m_d, CHF_REG_PC, kStart);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote $8200")), 2000);
        QLabel *status = statusLabel();
        QVERIFY(status->text().contains(QLatin1String("(was $77)")));
        QVERIFY2(status->width() >= status->fontMetrics().horizontalAdvance(status->text()),
                 qPrintable(QStringLiteral("status %1 wide for %2").arg(status->width())
                                .arg(status->fontMetrics().horizontalAdvance(status->text()))));
        QVERIFY(!status->text().contains(QChar(0x2026)));
        shot(QStringLiteral("stopreason"));
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("DC0")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $8105"));
        QCOMPARE(pcOf(m_d), 0x8105);
        typeInto(in, QStringLiteral("break 33030"));    /* decimal, the engine's rule: $8106 */
        QVERIFY(chfdebug_breakpoint_check(m_d, kCall));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $8106")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), kCall);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $8106")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break 33030"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* Load Symbols > Next to the Cartridge (no cartridge file: said, not
     * silent), a symbol file loaded through the engine shows in the views, and
     * Break at takes a symbol or a hex address. */
    void symbolsAndBreakAt()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the Cartridge")) next = a;
        QVERIFY(next);
        const int before = consoleView()->document()->blockCount();
        next->trigger();
        QVERIFY(consoleView()->document()->blockCount() > before);
        QCOMPARE(chfdebug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("8109 afterwards\n8150 helper\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QCOMPARE(chfdebug_label_address(m_d, "afterwards"), kAfter);

        /* Break at: a symbol, then a hex address, then a repeat */
        QLineEdit *breakAt = entry(QStringLiteral("symbol or"));
        QVERIFY(breakAt);
        typeInto(breakAt, QStringLiteral("afterwards"));
        QVERIFY(chfdebug_breakpoint_check(m_d, kAfter));
        QVERIFY(breakAt->text().isEmpty());
        typeInto(breakAt, QStringLiteral("810A"));
        QVERIFY(chfdebug_breakpoint_check(m_d, 0x810A));
        typeInto(breakAt, QStringLiteral("810A"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("already")));
        chfdebug_breakpoint list[4];
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 4), 2);
        typeInto(breakAt, QStringLiteral("nosuchsymbol"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Not a number")));
        chfdebug_breakpoint_clear(m_d);

        /* the stop reason names where it is */
        typeInto(in, QStringLiteral("break afterwards"));
        QVERIFY(chfdebug_breakpoint_check(m_d, kAfter));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) && pcOf(m_d) == kAfter, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);

        /* the Symbols tab lists them, filtered; activating a row opens the Disassembly there */
        m_tabs->setCurrentIndex(10);
        QTableWidget *table = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *filter = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(table && filter);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 2, 2000);
        shot(QStringLiteral("symbols"));
        const int all = table->rowCount();
        filter->setText(QStringLiteral("AFTERW"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("afterwards"));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("$8109"));
        table->setCurrentCell(0, 0);
        QTest::keyClick(table, Qt::Key_Return);             /* activating a row */
        QCOMPARE(m_tabs->currentIndex(), 1);
        QVERIFY(!check(QStringLiteral("Follow PC"))->isChecked());
        QVERIFY(lineOf(disasmView(), kAfter) >= 0);
        m_tabs->setCurrentIndex(10);
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), all, 2000);

        /* Clear Symbols drops the loaded ones */
        QTest::mouseClick(button(QStringLiteral("Clear Symbols")), Qt::LeftButton);
        QCOMPARE(chfdebug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() < all, 2000);
        m_tabs->setCurrentIndex(0);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        if (!follow->isChecked()) QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kBegin) >= 0, 2000);
        const int pcLine = lineOf(v, kBegin);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY2(pcLine >= rows / 3 - 3 && pcLine <= rows / 3 + 3, qPrintable(QStringLiteral("pc line %1 of %2").arg(pcLine).arg(rows)));
        /* the PC's line: the accent as the block's own background, so it lies under the gutter */
        const QTextBlock pcBlock = v->document()->findBlockByNumber(pcLine);
        QCOMPARE(pcBlock.blockFormat().background().color(), kAccent);
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(v->document()->findBlockByNumber(pcLine + 1).blockFormat().background().style(), Qt::NoBrush);

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        chfdebug_breakpoint_toggle(m_d, kCall);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kCall)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, kCall)), QColor(0xe0, 0x1b, 0x24));
        chfdebug_breakpoint b;
        QCOMPARE(chfdebug_breakpoint_list(m_d, &b, 1), 1);
        chfdebug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kCall)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, kCall)), QColor(0x9a, 0x99, 0x96));
        /* a breakpoint on the PC's own line keeps its dot, dark red on the yellow bar */
        chfdebug_breakpoint_toggle(m_d, kBegin);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, kBegin)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, kBegin)), QColor(0xa0, 0x10, 0x18));
        /* the PC's line is dark ink on the yellow */
        bool ink = false;
        QTextCursor cur(v->document()->findBlockByNumber(lineOf(v, kBegin)));
        cur.setPosition(cur.block().position() + 10);
        ink = cur.charFormat().foreground().color() == kInk;
        QVERIFY(ink);
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0, 2000);
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(kAfter, 30);
        QVERIFY(!chfdebug_breakpoint_check(m_d, kAfter));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, kAfter) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0x810A, 30);
        QVERIFY(!chfdebug_breakpoint_check(m_d, kAfter));
        QVERIFY(!chfdebug_breakpoint_check(m_d, 0x810A));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(kStore, 0);
        QVERIFY(chfdebug_breakpoint_check(m_d, kStore));
        clickLine(kStore, 0);
        QVERIFY(!chfdebug_breakpoint_check(m_d, kStore));
        clickLine(0x8110, 30, true);
        QVERIFY(chfdebug_breakpoint_check(m_d, 0x8110));
        chfdebug_breakpoint_toggle(m_d, 0x8110);
        clickLine(kCall, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(chfdebug_breakpoint_check(m_d, kCall));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!chfdebug_breakpoint_check(m_d, kCall));
        chfdebug_breakpoint list[4];
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kStore) >= 0, 2000);
        clickLine(kStore, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), kStore, 3000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("run to")));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kStore) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kBegin) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, kBegin) < 0 || lineOf(v, kBegin) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("8150"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kSubAt) >= 0, 2000);
        chfdebug_set_label(m_d, kAfter, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$8150+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8151) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kBegin) >= 0, 2000);
        chfdebug_set_label(m_d, kAfter, nullptr);
    }

    /* The register strip: every register an entry that sets it (while
     * stopped), W's flags as boxes, the ISAR in octal. */
    void registerStrip()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().contains(QLatin1String("Enter sets"))) regs << e;
        QCOMPARE(regs.size(), 9);   /* PC PC0 PC1 DC0 DC1 A W ISAR, K depth */
        QLineEdit *pc = regs[0], *pc1 = regs[2], *dc0 = regs[3], *areg = regs[5], *wreg = regs[6], *isar = regs[7],
                  *kdepth = regs[8];
        QTRY_COMPARE_WITH_TIMEOUT(pc->text(), QStringLiteral("8103"), 2000);
        typeInto(pc, QStringLiteral("810A"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x810A, 2000);
        typeInto(pc1, QStringLiteral("8150"));
        typeInto(dc0, QStringLiteral("$8200+2"));       /* an expression */
        chfdebug_cpu c;
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE((int)c.pc1, 0x8150);
        QCOMPARE((int)c.dc0, 0x8202);
        typeInto(areg, QStringLiteral("5a"));
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE((int)c.a, 0x5A);
        QTRY_COMPARE_WITH_TIMEOUT(areg->text(), QStringLiteral("5A"), 2000);
        typeInto(wreg, QStringLiteral("04"));            /* Z */
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE((int)c.w, 0x04);
        QCOMPARE(c.zero, 1);
        QCheckBox *zero = check(QStringLiteral("Z"));
        QVERIFY(zero);
        QTRY_VERIFY_WITH_TIMEOUT(zero->isChecked(), 2000);
        QTest::mouseClick(check(QStringLiteral("C")), Qt::LeftButton, {}, QPoint(4, check(QStringLiteral("C"))->height() / 2));
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE(c.carry, 1);
        chfdebug_cpu_set(m_d, CHF_REG_W, 0);
        /* the ISAR is octal, upper.lower */
        typeInto(isar, QStringLiteral("3.5"));
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE((int)c.isar, 29);
        QTRY_COMPARE_WITH_TIMEOUT(isar->text(), QStringLiteral("3.5"), 2000);
        typeInto(isar, QStringLiteral("77"));
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE((int)c.isar, 63);
        typeInto(isar, QStringLiteral("9"));             /* not octal: refused */
        chfdebug_cpu_get(m_d, &c);
        QCOMPARE((int)c.isar, 63);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("octal")));
        /* the K depth is r59 = 40 + 2 * depth */
        typeInto(kdepth, QStringLiteral("3"));
        QCOMPARE(regOf(m_d, 59), 46);
        QTRY_COMPARE_WITH_TIMEOUT(kdepth->text(), QStringLiteral("3"), 2000);
        chfdebug_cpu_set(m_d, CHF_REG_R0 + 59, 0);
        QTRY_COMPARE_WITH_TIMEOUT(kdepth->text(), QStringLiteral("-"), 2000);
        /* the data bus and the beam line are shown, not set */
        bool beam = false, bus = false;
        for (QLabel *l : m_win->findChildren<QLabel *>()) {
            if (l->text().contains(QLatin1String("clocks"))) beam = true;
            if (l->text().startsWith(QLatin1String("DBUS $"))) bus = true;
        }
        QVERIFY(beam && bus);
        /* the registers are under every tab */
        for (int tab : { 0, 1, 3, 8, 11 }) {
            m_tabs->setCurrentIndex(tab);
            QVERIFY(pc->isVisible());
        }
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        typeInto(pc, QStringLiteral("8100"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("registers"));
        m_tabs->setCurrentIndex(0);
    }

    /* Memory: the five spaces, the region names, Go to, Poke, the marks. */
    void memorySpaces()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QComboBox *space = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(space);
        QCOMPARE(space->count(), 5);
        QCOMPARE(space->currentData().toInt(), (int)CHF_SPACE_PROG);
        QPlainTextEdit *mem = memView();
        QLineEdit *addr = entryIs(QStringLiteral("$8000"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QLineEdit *go = entryIs(QStringLiteral("$0802, label or lo-hi"));
        QVERIFY(addr && val && go && mem);

        /* PROG: poke into the cartridge's RAM; the ROM refuses */
        addr->setText(QStringLiteral("8270"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        QCOMPARE((int)peek(m_d, 0x8270), 0x5A);
        addr->setText(QStringLiteral("$8271"));
        typeInto(val, QStringLiteral("$A5"));
        QCOMPARE((int)peek(m_d, 0x8271), 0xA5);
        put(m_d, 0x8272, 0x11);
        addr->setText(QStringLiteral("8272"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        QCOMPARE((int)peek(m_d, 0x8272), 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));

        typeInto(go, QStringLiteral("8100"));
        /* all 64K are there: the first row and the last, each named by its region */
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$FFF0: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$8100: 2A 82 00 20 10 51")));
        QCOMPARE(mem->document()->blockCount(), 4097);
        const QString first = mem->document()->findBlockByNumber(1).text();
        const QString ram = mem->document()->findBlockByNumber(1 + 0x8100 / 16).text();
        QVERIFY2(!first.mid(72).trimmed().isEmpty(), qPrintable(first));
        QVERIFY2(ram.contains(QLatin1String("RAM")), qPrintable(ram));
        /* Go to scrolled the row into view and marked it in the accent, dark ink */
        QVERIFY(mem->verticalScrollBar()->value() >= 0x8100 / 16 - 40);
        bool marked = false;
        for (const auto &sel : mem->extraSelections())
            marked |= sel.format.background().color() == kAccent && sel.format.foreground().color() == kInk;
        QVERIFY(marked);

        /* SCRATCH: 64 bytes, the grid and the K stack appear beside the dump */
        QVERIFY(!scratchPanel()->isVisible());
        space->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(scratchPanel()->isVisible(), 1000);
        QCOMPARE(mem->document()->blockCount(), 5);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0030: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("r48-r63")));
        chfdebug_write(m_d, CHF_SPACE_SCRATCH, 20, 0x2B);
        typeInto(go, QStringLiteral("r20"));
        QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("$0010: ")), 1000);
        QCOMPARE((int)peek(m_d, 20, CHF_SPACE_SCRATCH), 0x2B);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("2B")));
        QVERIFY(!mem->extraSelections().isEmpty());      /* r20 marked */
        addr->setText(QStringLiteral("r21"));
        typeInto(val, QStringLiteral("$44"));
        QCOMPARE((int)peek(m_d, 21, CHF_SPACE_SCRATCH), 0x44);
        shot(QStringLiteral("memory-scratch"));
        space->setCurrentIndex(2);                       /* VRAM */
        QTRY_VERIFY_WITH_TIMEOUT(!scratchPanel()->isVisible(), 1000);
        QCOMPARE(mem->document()->blockCount(), 8192 / 16 + 1);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("row 0 cols 0-15")));
        chfdebug_write(m_d, CHF_SPACE_VRAM, 5 * 128 + 4, 2);
        typeInto(go, QStringLiteral("5,4"));
        QTRY_VERIFY_WITH_TIMEOUT(mem->toPlainText().contains(QStringLiteral("$0280: 00 00 00 00 02")), 1000);
        shot(QStringLiteral("memory-vram"));
        space->setCurrentIndex(4);                       /* IO */
        QTRY_COMPARE_WITH_TIMEOUT(mem->document()->blockCount(), 17, 1000);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("ports $00-$0F")));
        space->setCurrentIndex(3);                       /* CART */
        QTRY_VERIFY_WITH_TIMEOUT(mem->document()->blockCount() > 4097, 1000);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$10000: ")));       /* the arena: five digits */
        shot(QStringLiteral("memory-cart"));
        space->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    QComboBox *searchKind()
    {
        for (QComboBox *c : m_tabs->widget(2)->findChildren<QComboBox *>())
            if (c->findText(QStringLiteral("Hex")) >= 0) return c;
        return nullptr;
    }

    QWidget *scratchPanel()
    {
        return m_tabs->widget(2)->findChild<QWidget *>(QStringLiteral("scratchPanel"));
    }

    /* The scratchpad grid: a click on a cell edits it while stopped, the
     * ISAR's cell is outlined, the K stack lists its returns. */
    void scratchGridAndKStack()
    {
        m_tabs->setCurrentIndex(2);
        QComboBox *space = m_tabs->currentWidget()->findChild<QComboBox *>();
        space->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(scratchPanel()->isVisible(), 1000);
        QWidget *grid = scratchPanel()->findChild<QWidget *>(QStringLiteral("scratchGrid"));
        QVERIFY(grid);
        /* the K stack: depth 2 holds two saved returns */
        chfdebug_write(m_d, CHF_SPACE_SCRATCH, 40, 0x81);
        chfdebug_write(m_d, CHF_SPACE_SCRATCH, 41, 0x09);
        chfdebug_write(m_d, CHF_SPACE_SCRATCH, 42, 0x82);
        chfdebug_write(m_d, CHF_SPACE_SCRATCH, 43, 0x00);
        chfdebug_cpu_set(m_d, CHF_REG_R0 + 59, 44);
        QPlainTextEdit *kstack = scratchPanel()->findChild<QPlainTextEdit *>(QStringLiteral("kstack"));
        QVERIFY(kstack);
        QTRY_VERIFY_WITH_TIMEOUT(kstack->toPlainText().contains(QStringLiteral("depth 2")), 2000);
        QVERIFY2(kstack->toPlainText().contains(QStringLiteral("$8109")), qPrintable(kstack->toPlainText()));
        QVERIFY(kstack->toPlainText().contains(QStringLiteral("$8200")));
        /* a click on r20's cell (column 4 of row 2) edits it, octal */
        const QPoint cell(34 + 4 * 60 + 30, 18 + 2 * 36 + 18);
        QTest::mouseClick(grid, Qt::LeftButton, {}, cell);
        QLineEdit *edit = grid->findChild<QLineEdit *>();
        QVERIFY(edit && edit->isVisible());
        typeInto(edit, QStringLiteral("123"));
        QCOMPARE((int)peek(m_d, 20, CHF_SPACE_SCRATCH), 0123);
        /* running refuses an edit */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        QTest::mouseClick(grid, Qt::LeftButton, {}, cell);
        QVERIFY(!edit->isVisible());
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        chfdebug_cpu_set(m_d, CHF_REG_R0 + 59, 0);
        shot(QStringLiteral("memory-scratch-grid"));
        space->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("address, range"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QPushButton *add = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) add = b;
        QPushButton *clear = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Breakpoints")) clear = b;
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Access"), QStringLiteral("Port IN"), QStringLiteral("Port OUT"),
                                      QStringLiteral("VRAM write"), QStringLiteral("Scratchpad write") }));

        /* exec at a hex address */
        typeInto(start, QStringLiteral("8106"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY2(t->item(0, 1)->text().contains(QLatin1String("$8106")), qPrintable(t->item(0, 1)->text()));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition on old */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("8200"));
        end->setText(QStringLiteral("$820F"));
        cond->setText(QStringLiteral("value != old"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QVERIFY2(t->item(1, 1)->text().contains(QLatin1String("$8200-$820F")), qPrintable(t->item(1, 1)->text()));
        QVERIFY(t->item(1, 1)->text().contains(QLatin1String("value != old")));
        /* the start may itself be a range */
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Access"));
        typeInto(start, QStringLiteral("8300-830F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
        QVERIFY2(t->item(2, 1)->text().contains(QLatin1String("$8300-$830F")), qPrintable(t->item(2, 1)->text()));
        chfdebug_breakpoint list[16];
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 16), 3);
        QCOMPARE(list[2].type, (int)CHFDEBUG_BP_ACCESS);
        /* the port kinds */
        type->setCurrentText(QStringLiteral("Port OUT"));
        QVERIFY(start->placeholderText().startsWith(QLatin1String("port")));
        typeInto(start, QStringLiteral("5"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 4, 2000);
        type->setCurrentText(QStringLiteral("Port IN"));
        typeInto(start, QStringLiteral("0"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 5, 2000);
        /* VRAM: a cell as row,col; scratchpad: a register by name */
        type->setCurrentText(QStringLiteral("VRAM write"));
        QVERIFY(start->placeholderText().contains(QLatin1String("row,col")));
        typeInto(start, QStringLiteral("12,40"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 6, 2000);
        QVERIFY2(t->item(5, 1)->text().contains(QLatin1String("12")), qPrintable(t->item(5, 1)->text()));
        type->setCurrentText(QStringLiteral("Scratchpad write"));
        typeInto(start, QStringLiteral("r9"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 7, 2000);
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 16), 7);
        QCOMPARE(list[3].space, (int)CHF_SPACE_IO);
        QCOMPARE(list[5].type, (int)CHFDEBUG_BP_VRAM);
        QCOMPARE(list[5].space, (int)CHF_SPACE_VRAM);
        QCOMPARE((int)list[5].start, 12 * 128 + 40);
        QCOMPARE(list[6].type, (int)CHFDEBUG_BP_SCRATCH);
        QCOMPARE((int)list[6].start, 9);
        /* the ranges are checked */
        const int rowsBefore = t->rowCount();
        type->setCurrentText(QStringLiteral("Port IN"));
        typeInto(start, QStringLiteral("100"));          /* port $100: past the ports */
        QVERIFY2(!msg->text().isEmpty(), "a port past $FF is refused");
        QCOMPARE(t->rowCount(), rowsBefore);
        type->setCurrentText(QStringLiteral("Scratchpad write"));
        typeInto(start, QStringLiteral("64"));
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        /* errors are shown, not swallowed */
        type->setCurrentText(QStringLiteral("Execute"));
        cond->setText(QStringLiteral("a == (1 +"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        cond->clear();
        type->setCurrentText(QStringLiteral("Port IN"));
        start->setText(QStringLiteral("7"));
        end->setText(QStringLiteral("5"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        shot(QStringLiteral("breakpoints-types"));

        /* back to two: the exec one and the conditional write */
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        typeInto(start, QStringLiteral("8109"));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("8200"));
        end->setText(QStringLiteral("$820F"));
        cond->setText(QStringLiteral("value != old"));
        QTest::mouseClick(add, Qt::LeftButton);
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);

        /* the exec breakpoint stops the run, with its hits counted */
        put(m_d, kCounter, 0);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) && pcOf(m_d) == kAfter, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $8109")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        /* a disabled breakpoint is the hollow grey dot in the Disassembly */
        {
            m_tabs->setCurrentIndex(1);
            QPlainTextEdit *v = disasmView();
            QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kAfter) >= 0 &&
                                         v->document()->findBlockByNumber(lineOf(v, kAfter)).text().startsWith(
                                             QStringLiteral("○")), 2000);
            m_tabs->setCurrentIndex(3);
        }
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $8200")),
                                 4000);
        /* Clear Hits zeroes the counts */
        QPushButton *clearHits = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Hits")) clearHits = b;
        QVERIFY(clearHits);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(1, 2) && t->item(1, 2)->text().toInt() >= 1, 2000);
        QTest::mouseClick(clearHits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(1, 2)->text(), QStringLiteral("0"), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(chfdebug_breakpoint_list(m_d, list, 2), 0);
    }

    QPlainTextEdit *memView()
    {
        return m_tabs->widget(2)->findChild<QPlainTextEdit *>();
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        chfwatch_clear();
        put(m_d, 0x8270, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$8270"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$8270")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        put(m_d, 0x8270, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$8280-$828F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        chfdebug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        chfdebug_breakpoint bp[2];
        QCOMPARE(chfdebug_breakpoint_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & CHFDEBUG_BP_WRITE);
        QCOMPARE(bp[0].space, (int)CHF_SPACE_PROG);
        QCOMPARE((int)bp[0].start, 0x8270);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        chfdebug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$8280: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* and every watch is marked in the dump, in its own colour */
        bool watched = false;
        for (const auto &s : memView()->extraSelections())
            watched |= s.format.background().color() == QColor(0x1c, 0x71, 0xd8);
        QVERIFY(watched);

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(chfwatch_count(), 1);
        chfwatch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        for (int i = 0; i < 4; ++i) put(m_d, (uint32_t)(0x8300 + i), pat[i]);
        for (int i = 0; i < 4; ++i) put(m_d, (uint32_t)(0x8380 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$0802, label or lo-hi"));
        typeInto(go, QStringLiteral("8300-8303"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$8300: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        chfdebug_set_label(m_d, 0x8380, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$8380: DE EE")));
        chfdebug_set_label(m_d, 0x8380, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("8300-8303"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QVERIFY2(note->text().startsWith(QLatin1String("Found at $")), qPrintable(note->text()));
        QCOMPARE(note->text(), QStringLiteral("Found at $8380"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $8300"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        for (int i = 0; i < 5; ++i) put(m_d, (uint32_t)(0x83A0 + i), (uint8_t)"HeLLo"[i]);
        searchKind()->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("8200"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $83A0"));
        shot(QStringLiteral("search"));
        searchKind()->setCurrentText(QStringLiteral("Hex"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        chfdebug_trace_enable(m_d, 0);
        chfdebug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(chfdebug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!chfdebug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("INC")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        /* newest at the bottom: the view is scrolled to its end */
        QVERIFY(hist->verticalScrollBar()->value() >= hist->verticalScrollBar()->maximum() - 1);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves branches, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("PI")) || hist->toPlainText().contains(QStringLiteral("JMP")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("810F-810F"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("ST")) &&
                                     !hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(chfdebug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!chfdebug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Channel F Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("chfHelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        chfdebug_breakpoint_toggle(m_d, kCall);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        chfdebug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Channel F Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 11; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("Channel F Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Cart && I/O"));
        QCOMPARE(names.at(10), QStringLiteral("Symbols"));
    }

    /* A new stop pulls every view back to the spot from wherever it was
     * scrolled: the disassembly to the PC, Memory to the PC, the Trace and
     * History to their newest line; and the Breakpoints tab says why. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, kBegin) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* Memory scrolled to the top, History away from its end */
        m_tabs->setCurrentIndex(2);
        memView()->verticalScrollBar()->setValue(0);
        m_tabs->setCurrentIndex(1);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(memView()->verticalScrollBar()->value() >= pcOf(m_d) / 16 - 80, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    void otherTabs()
    {
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QWidget *page = m_tabs->currentWidget();
        QPlainTextEdit *info = nullptr;
        for (QPlainTextEdit *v : page->findChildren<QPlainTextEdit *>())
            if (v->toPlainText().contains(QStringLiteral("Memory map"))) info = v;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (QPlainTextEdit *v : page->findChildren<QPlainTextEdit *>())
                if (v->toPlainText().contains(QStringLiteral("Memory map"))) return true;
            return false;
        }(), 2000);
        for (QPlainTextEdit *v : page->findChildren<QPlainTextEdit *>())
            if (v->toPlainText().contains(QStringLiteral("Memory map"))) info = v;
        QVERIFY(info);
        const QString text = info->toPlainText();
        QCOMPARE(text.count(QStringLiteral("Memory map")), 1);
        QVERIFY(text.contains(QStringLiteral("BIOS")));
        QVERIFY(text.contains(QStringLiteral("$8000")));
        QVERIFY(text.contains(QStringLiteral("Cartridge")));
        /* the lamps, the ports, the mailbox, the events and the I/O log */
        bool pads = false, ports = false, ram = false, mailbox = false;
        for (QLabel *l : page->findChildren<QLabel *>()) {
            pads |= l->text().contains(QLatin1String("right controller"));
            ports |= l->text().startsWith(QLatin1String("port  latch"));
            ram |= l->text().startsWith(QLatin1String("2102 RAM"));
            mailbox |= l->text().contains(QLatin1String("The mailbox"));
        }
        QVERIFY2(ports && ram && mailbox, qPrintable(QStringLiteral("%1 %2 %3 %4").arg(pads).arg(ports).arg(ram).arg(mailbox)));
        shot(QStringLiteral("cart"));
        /* a write to port 5 shows in the I/O log and the ports */
        /* Sound: the beeper, its meter and the mode timeline */
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QWidget *sound = m_tabs->currentWidget();
        bool mode = false, timeline = false;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (QLabel *l : sound->findChildren<QLabel *>()) {
                if (l->text().startsWith(QLatin1String("Mode "))) mode = true;
                if (l->text().contains(QLatin1String("Sound-mode timeline"))) timeline = true;
            }
            return mode && timeline;
        }(), 2000);
        QVERIFY(!sound->findChild<QPlainTextEdit *>()->toPlainText().contains(QStringLiteral("Memory map")));
        shot(QStringLiteral("sound"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* A port write shows in the Cart & I/O tab's log and the Sound tab's
     * timeline: the machine runs OUTS 5 for a mode and stops. */
    void portsAndSound()
    {
        /* LI $40 / OUTS 5 (1 kHz) / LI 0 / OUTS 5 (off) / JMP the loop */
        const uint8_t code[] = { 0x20, 0x40, 0xB5, 0x20, 0x00, 0xB5, 0x29, 0x81, 0x00 };
        for (size_t i = 0; i < sizeof code; i++) put(m_d, (uint32_t)(0x8400 + i), code[i]);
        chfdebug_cpu_set(m_d, CHF_REG_PC, 0x8400);
        chfdebug_breakpoint_add(m_d, CHFDEBUG_BP_EXEC, CHF_SPACE_PROG, 0x8406, 0x8406, "");
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) && pcOf(m_d) == 0x8406, 3000);
        m_tabs->setCurrentIndex(5);
        bool logged = false;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (QPlainTextEdit *v : m_tabs->currentWidget()->findChildren<QPlainTextEdit *>())
                if (v->toPlainText().contains(QStringLiteral("OUT port $05")) && v->toPlainText().contains(QStringLiteral("$40")))
                    logged = true;
            return logged;
        }(), 3000);
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->findChild<QPlainTextEdit *>()->toPlainText().contains(
                                     QStringLiteral("off → 1 kHz")), 3000);
        shot(QStringLiteral("sound-modes"));
        m_tabs->setCurrentIndex(0);
    }

    void videoTab()
    {
        chfdebug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(8);
        QWidget *page = m_tabs->currentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(page->isVisible(), 1000);
        auto combos = page->findChildren<QComboBox *>();
        QCOMPARE(combos.size(), 2);
        QComboBox *view = combos[0], *zoom = combos[1];
        QStringList views;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Display"), QStringLiteral("Decoded"), QStringLiteral("Beam"),
                                      QStringLiteral("Values"), QStringLiteral("Live") }));
        QCOMPARE(zoom->itemText(0), QStringLiteral("Fit"));
        QWidget *pic = page->findChild<QWidget *>(QStringLiteral("videoPicture"));
        QVERIFY(pic);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100 && pic->height() >= 100, 2000);
        shot(QStringLiteral("video-display"));

        /* a change of view redraws, at the sizes the core names (integer zoom only) */
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));
        view->setCurrentIndex(0);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 204, 1000);
        QCOMPARE(pic->height(), 232);
        view->setCurrentIndex(1);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 256, 1000);
        QCOMPARE(pic->height(), 256);
        shot(QStringLiteral("video-decoded"));
        view->setCurrentIndex(2);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 204, 1000);
        shot(QStringLiteral("video-beam"));
        view->setCurrentIndex(3);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 256, 1000);
        shot(QStringLiteral("video-values"));
        view->setCurrentIndex(4);
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 204, 1000);
        shot(QStringLiteral("video-live"));
        view->setCurrentIndex(1);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 512, 1000);
        QCOMPARE(pic->height(), 512);
        view->setCurrentIndex(0);

        /* an overlay checkbox */
        QCheckBox *grid = nullptr;
        for (QCheckBox *box : page->findChildren<QCheckBox *>())
            if (box->text().contains(QLatin1String("rid"))) grid = box;
        QVERIFY(grid);
        grid->setChecked(true);
        QTest::qWait(50);
        shot(QStringLiteral("video-grid"));
        grid->setChecked(false);

        /* hover shows, a click pins, the same spot unpins */
        auto *tabs = page->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QStringList sections;
        for (int i = 0; i < tabs->count(); ++i) sections << tabs->tabText(i);
        QCOMPARE(sections, (QStringList{ QStringLiteral("Pixel"), QStringLiteral("Registers"), QStringLiteral("Colours"),
                                         QStringLiteral("Lines"), QStringLiteral("Writes"), QStringLiteral("Timing") }));
        tabs->setCurrentIndex(0);
        QLabel *pixel = nullptr;
        for (QLabel *l : tabs->widget(0)->findChildren<QLabel *>()) pixel = l;
        QVERIFY(pixel);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(pic->width() / 2, pic->height() / 2);
        QTest::mouseMove(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("Move over")), 1000);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, at + QPoint(30, 30));
        QVERIFY(!pixel->text().contains(QLatin1String("Move over")));   /* the pin stays */
        shot(QStringLiteral("video-pixel"));
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, QPoint(-5, -5));

        /* the tables */
        QTableWidget *regs = nullptr, *lines = nullptr, *colours = nullptr;
        for (QTableWidget *t : page->findChildren<QTableWidget *>()) {
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Name")) regs = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Row")) lines = t;
            if (t->horizontalHeaderItem(1) && t->horizontalHeaderItem(1)->text() == QLatin1String("Palette") &&
                t->horizontalHeaderItem(2)->text() == QLatin1String("Value")) colours = t;
        }
        QVERIFY(regs && lines && colours);
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(regs->rowCount() >= 4, 1000);                 /* ports 0 1 4 5 and the derived rows */
        shot(QStringLiteral("video-registers"));
        tabs->setCurrentIndex(2);
        QTRY_COMPARE_WITH_TIMEOUT(colours->rowCount(), 16, 1000);              /* four palettes of four */
        shot(QStringLiteral("video-colours"));
        tabs->setCurrentIndex(3);
        QTRY_COMPARE_WITH_TIMEOUT(lines->rowCount(), 64, 1000);
        shot(QStringLiteral("video-lines"));
        tabs->setCurrentIndex(4);
        QPlainTextEdit *writes = tabs->widget(4)->findChild<QPlainTextEdit *>();
        QVERIFY(writes);
        QTest::qWait(50);
        shot(QStringLiteral("video-writes-empty"));
        tabs->setCurrentIndex(5);
        QTest::qWait(50);
        shot(QStringLiteral("video-timing"));
        tabs->setCurrentIndex(0);

        /* Break on writes: with nothing under the pointer the button is off; pinned on a
         * cell it adds a VRAM write breakpoint over that cell */
        QPushButton *brk = nullptr;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Break on writes")) brk = b;
        QVERIFY(brk);
        QVERIFY(!brk->isEnabled());
        view->setCurrentIndex(1);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_COMPARE_WITH_TIMEOUT(pic->width(), 512, 1000);
        const QPoint cell(4 * 40 + 2, 8 * 20 + 3);           /* row 20, column 40 of the decoded VRAM at 2x */
        QTest::mouseMove(pic, cell);
        QTest::mouseClick(pic, Qt::LeftButton, {}, cell);
        QTRY_VERIFY_WITH_TIMEOUT(brk->isEnabled(), 2000);
        QTest::mouseClick(brk, Qt::LeftButton);
        chfdebug_breakpoint bps[8];
        const int made = chfdebug_breakpoint_list(m_d, bps, 8);
        QCOMPARE(made, 1);
        QCOMPARE(bps[0].type, (int)CHFDEBUG_BP_VRAM);
        QCOMPARE(bps[0].space, (int)CHF_SPACE_VRAM);
        QVERIFY2(bps[0].start / 128 >= 19 && bps[0].start / 128 <= 21, qPrintable(QString::number(bps[0].start)));
        shot(QStringLiteral("video-pixel-pinned"));

        /* a program that plots the cell shows in the Writes section: LI 2, OUTS 1 (colour),
         * LI col, OUTS 4, LI row, OUTS 5, with the strobe bit raised by port 0 */
        const uint8_t plot[] = { 0x20, 0xBF, 0xB1, 0x20, 0xD7, 0xB4, 0x20, 0x2B, 0xB5, 0x20, 0x60, 0xB0, 0x20, 0x50, 0xB0,
                                 0x29, 0x81, 0x00 };
        for (size_t i = 0; i < sizeof plot; i++) put(m_d, (uint32_t)(0x8500 + i), plot[i]);
        chfdebug_cpu_set(m_d, CHF_REG_PC, 0x8500);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d), 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("VRAM write")),
                                  qPrintable(statusLabel()->text()), 2000);
        tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(!writes->toPlainText().startsWith(QLatin1String("(no VRAM writes")), 2000);
        shot(QStringLiteral("video-writes"));
        chfdebug_breakpoint_clear(m_d);
        QTest::mouseClick(pic, Qt::LeftButton, {}, cell);     /* unpin */
        QTest::mouseMove(pic, QPoint(-5, -5));
        QTRY_VERIFY_WITH_TIMEOUT(!brk->isEnabled(), 2000);
        tabs->setCurrentIndex(0);
        zoom->setCurrentIndex(0);
        view->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!chfdebug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("&Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change keeps the engine attached: the new
     * machine stops at its first instruction and the window follows. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        chfdebug_breakpoint_toggle(m_d, kCall);
        chfsession_stop(m_s);
        QCOMPARE(chfsession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d), 3000);
        QVERIFY(chfdebug_breakpoint_check(m_d, kCall));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">")), 2000);
        /* and, given the program again, it runs from there to the breakpoint */
        loadProgram(m_d);
        put(m_d, kCounter, 0);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_stopped(m_d) && pcOf(m_d) == kCall, 5000);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!chfdebug_is_attached(m_d));
        chfdebug_cpu c, c2;
        chfdebug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((chfdebug_cpu_get(m_d, &c2), c2.frame > c.frame + 3), 3000);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!chfdebug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(chfdebug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
