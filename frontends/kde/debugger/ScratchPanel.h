/*
 * ScratchPanel -- the Fairchild F8's 64-byte scratchpad as an 8 x 8 octal
 * grid (the ISAR's cell outlined, J/H/K/Q named, the cells that changed in the
 * accent, r40-r59, the BIOS's K stack, shaded; a click edits a cell while the
 * machine is stopped) with the K-stack list under it. It lives in the
 * debugger's Memory tab while the SCRATCH space is selected.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>
#include <functional>

#include "chfdebug.h"

class QLabel;
class QPlainTextEdit;
class ScratchGrid;

class ScratchPanel : public QWidget {
public:
    explicit ScratchPanel(chfdebug *debugger, QWidget *parent = nullptr);
    /* from the registers the window already fetched */
    void refresh(const chfdebug_cpu &cpu);

    /* may the machine be edited now (it says why not), and a cell was edited */
    std::function<bool()> canEdit;
    std::function<void(int, unsigned)> onEdit;

private:
    chfdebug *m_dbg;
    ScratchGrid *m_grid = nullptr;
    QPlainTextEdit *m_kstack = nullptr;
};
