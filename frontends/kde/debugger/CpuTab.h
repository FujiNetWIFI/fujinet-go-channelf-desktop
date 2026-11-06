/*
 * The debugger's CPU & RAM tab: the F8's registers (A, W as S C Z O ICB with
 * S set meaning the result was positive, ISAR in octal as upper.lower, PC0 and
 * the one return address PC1, DC0 and DC1, J H K Q, the data bus), the 64-byte
 * scratchpad as an 8 x 8 octal grid with the BIOS's names, the K stack, and a
 * hex view over the five address spaces. Values are edited in place while the
 * machine is stopped.
 *
 * Refreshed by the debugger window, only while this tab shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>
#include <cstdint>
#include <functional>
#include <vector>

#include "chfdebug.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class ScratchGrid;

class CpuTab : public QWidget {
public:
    explicit CpuTab(chfdebug *debugger, QWidget *parent = nullptr);
    void refresh();

    /* a note for the window's status line, and "something was edited" */
    std::function<void(const QString &)> onMessage;
    std::function<void()> onEdited;

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    QWidget *buildRegisters();
    QWidget *buildMemory();
    void refreshRegisters();
    void refreshRam();
    void setRegister(int reg, QLineEdit *edit);
    bool addressOf(const QString &text, long *out);
    bool stopped(const QString &what);

    static constexpr int kMemRows = 24;      /* the memory view: 24 rows of 16 */

    chfdebug *m_dbg;
    QLineEdit *m_reg[CHF_REG_COUNT] = {};
    QLineEdit *m_dbus = nullptr;
    QCheckBox *m_flag[5] = {};               /* S C Z O ICB */
    ScratchGrid *m_grid = nullptr;
    QPlainTextEdit *m_kstack = nullptr;
    QLabel *m_info = nullptr;

    QComboBox *m_memSpace = nullptr;
    QLineEdit *m_memGo = nullptr;
    uint32_t m_memBase = 0;
    QTableWidget *m_hex = nullptr;
    std::vector<uint8_t> m_hexPrev;
    bool m_hexFilling = false;
};
