/*
 * Debugger window (Qt6 Widgets) over the Fairchild F8 / Channel F video
 * debugger engine, via core/include/chfdebug.h. Mirrors the other frontends
 * tab for tab: Prompt, CPU & RAM, Disassembly, Video (the VRAM, its palettes
 * and the four ports), Sound & I/O, Breakpoints, Cart, FujiNet; each with a
 * Help button.
 *
 * The engine is attached while the window is shown (which stops the machine,
 * as on every sibling) and detached when it is hidden, which lets the
 * machine run on. A timer runs only while it is shown, and refreshes only the
 * tab that shows.
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
#include <vector>

extern "C" {
#include "chfdebug.h"
#include "chfsession.h"
}

class CartTab;
class CpuTab;
class IoTab;
class VideoTab;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, chfsession *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, chfsession *session);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(chfsession *session, QWidget *parent);
    QWidget *buildToolbar();
    QWidget *buildPrompt();
    QWidget *buildDisasm();
    QWidget *buildBreaks();
    QWidget *buildFujinet();

    void refreshAll();
    void refreshTab(int index);
    void refreshStatus();
    void refreshDisasm();
    void refreshBps();
    void refreshFujinet(bool force);
    void tick();
    void toggleRun();
    void runToCursor();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);
    void stepAnd(void (*fn)(chfdebug *));
    void loadSymbols(bool fromFile);
    void addBreakpoint();
    void note(const QString &text);
    bool addressOf(const QString &text, long *out);

    chfsession *m_session;
    chfdebug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    QPlainTextEdit *m_promptOut = nullptr;
    QLineEdit *m_promptIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    CpuTab *m_cpu = nullptr;

    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    std::vector<uint16_t> m_lineAddr;
    int m_selLine = -1;            /* the line Run to Cursor runs to */
    int m_selAddr = -1;

    VideoTab *m_video = nullptr;
    IoTab *m_io = nullptr;
    CartTab *m_cart = nullptr;

    QTableWidget *m_bpTable = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    bool m_bpFilling = false;

    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};
