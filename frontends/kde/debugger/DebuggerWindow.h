/*
 * Debugger window (Qt6 Widgets) over the Fairchild F8 / Channel F video
 * debugger engine, via core/include/chfdebug.h. Mirrors the other frontends
 * tab for tab, and the Studio II / SMS / Astrocade KDE debuggers' design: a
 * QTabWidget under a toolbar, the registers in a strip under the tabs.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on. The window is never deleted by its close button: it
 * is hidden, and comes back as it was.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include "chfdebug.h"
#include "chfsession.h"
}

class VideoTab;

class CartIoTab;
class QScrollArea;
class ScratchPanel;
class SoundTab;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, chfsession *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, chfsession *session);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(chfsession *session, QWidget *parent);
    ~DebuggerWindow() override;
    QWidget *buildToolbar();
    QWidget *buildConsole();
    QWidget *buildRegs();
    QWidget *buildDisasm();
    QWidget *buildMemory();
    QWidget *buildSearchBar();
    QWidget *buildVideo();
    QWidget *buildBreaks();
    QWidget *buildWatches();
    QWidget *buildHist(int idx);
    QWidget *buildSymbols();
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshDisasm();
    void refreshVideo(bool force);
    void refreshBps();
    void refreshWatches(bool force);
    void refreshCart();
    void refreshSound();
    void refreshHist(int idx, bool force);
    void refreshSymbols(bool force);
    void refreshFujinet(bool force);
    void refreshPage(int page, bool live);
    void refreshAll();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();
    void recentreOnStop();

    void consoleAppend(const QString &text);
    void runConsole();
    void completeConsole();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(const std::function<void()> &fn);
    void runToCursor();
    void toggleBreakpoint(int addr);
    void toggleSelected();
    void addBreakpoint();
    void breakAt();
    void setReg(int reg);
    void setKDepth();
    void memGoto();
    void memSearch(bool backward);
    void showInMemory(int space, uint32_t lo, uint32_t hi);
    void showInDisassembly(uint16_t addr);
    void scrollMemory(uint32_t addr);
    void setMemSpace(int space);
    int memSpace() const;
    void popOut(int tab);
    void dock(int tab);
    int currentTab() const;
    bool tabVisible(int tab) const;
    void addWatch(const QString &text);
    void watchBreak(int index);
    void memWrite();
    void jumpTo();
    void loadSymbols(bool fromFile);
    void clearSymbols();
    void save(const char *kind, const QString &title, const QString &file);
    int visibleRows() const;
    bool numberOf(const QString &text, long *out, QString *why = nullptr);
    bool addressOf(const QString &text, long *out, QString *why = nullptr);
    bool rangeOf(const QString &startText, const QString &endText, int type, int space, uint16_t *lo, uint16_t *hi,
                 QString *why);
    void setEntry(QLineEdit *e, const QString &text);

    chfsession *m_session;
    chfdebug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;
    QLineEdit *m_breakAt = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* registers strip: m_reg is indexed by chfdebug_reg (K depth has no
     * register of its own: it is r59 and has an entry beside them) */
    QLineEdit *m_reg[CHF_REG_COUNT] = {};
    QLineEdit *m_kdepth = nullptr;
    QCheckBox *m_flag[5] = {};          /* S C Z O ICB */
    QLabel *m_opcode = nullptr;
    QLabel *m_beam = nullptr;

    /* memory: the space chosen (PROG, SCRATCH, VRAM, CART, IO), 16 bytes a row */
    QComboBox *m_memSpace = nullptr;
    QPlainTextEdit *m_mem = nullptr;
    QLabel *m_memTitle = nullptr;
    QScrollArea *m_scratchScroll = nullptr;
    ScratchPanel *m_scratch = nullptr;  /* beside the dump while SCRATCH is the space */
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    bool m_hl = false;                  /* a range is highlighted in the dump */
    int m_hlSpace = 0;
    uint32_t m_hlLo = 0, m_hlHi = 0;
    int m_memShown = -1;                /* the space the dump was last built for */
    QComboBox *m_srchKind = nullptr;
    QLineEdit *m_srchEntry = nullptr;
    QLabel *m_srchNote = nullptr;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    uint64_t m_seenSerial = 0;          /* chfdebug_stop_serial last followed */
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* video */
    VideoTab *m_video = nullptr;
    bool m_videoForce = true;           /* the tab was just shown: redraw everything */

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    QLabel *m_bpStop = nullptr;

    /* watches */
    QComboBox *m_wtSpace = nullptr;
    QLineEdit *m_wtEntry = nullptr;
    QTableWidget *m_wtTable = nullptr;
    unsigned m_wtGen = ~0u;

    /* cart and I/O, sound */
    CartIoTab *m_cartIo = nullptr;
    SoundTab *m_soundTab = nullptr;

    /* symbols */
    QLineEdit *m_symFilter = nullptr;
    QTableWidget *m_symTable = nullptr;
    QLabel *m_symCount = nullptr;
    QString m_symSig;

    /* trace (0, live) and history (1, on a stop) share the engine's ring */
    QPushButton *m_hRec[2] = {};
    QComboBox *m_hFilter[2] = {};
    QLineEdit *m_hRange[2] = {};
    QPlainTextEdit *m_hText[2] = {};
    QLabel *m_hNote[2] = {};
    int m_hSeen[2] = { -1, -1 };
    bool m_hSync = false;

    /* tabs peeled off into windows of their own */
    QWidget *m_pages[16] = {};
    QWidget *m_popped[16] = {};
    bool m_closing = false;

    /* fujinet */
    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};
