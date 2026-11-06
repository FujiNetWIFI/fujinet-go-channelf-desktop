/*
 * The debugger's Video tab: everything the Channel F's video knows.
 *
 * Left, the picture in the view you choose -- the last frame, the VRAM as it
 * is now, what a CRT shows (the beam), the whole 128 x 64 VRAM, its raw values
 * or the four palettes -- at a chosen scale with the overlays you tick, and
 * the cell under the pointer (click to pin it, double-click to break on
 * writes to it). Right, in sections: the cell in full, the row palette strip
 * and the 4 x 4 palette table, the four ports (raw, complemented, decoded and
 * as the CPU would read them now) and the VRAM write log with its filters.
 *
 * Refreshed by the debugger window: live while the machine runs (at most ten
 * times a second) and on every stop, only while this tab shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <vector>

#include "chfdebug.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class RowStrip;
class VideoImage;

class VideoTab : public QWidget {
public:
    explicit VideoTab(chfdebug *debugger, QWidget *parent = nullptr);
    /* Pulls the video's state and redraws what shows. */
    void refresh();

    std::function<void(const QString &)> onMessage;

private:
    QWidget *buildLeft();
    QWidget *buildCell();
    QWidget *buildPalettes();
    QWidget *buildPorts();
    QWidget *buildLog();

    void refreshPicture();
    void refreshCell();
    void refreshPalettes();
    void refreshPorts();
    void refreshLog();
    void hover(int x, int y);
    void click(int x, int y);
    void doubleClick(int x, int y);
    bool hitAt(int x, int y, int *row, int *col);
    QRect cellRect(int row, int col) const;
    QString cellReadout(int row, int col);
    unsigned overlays() const;
    int view() const;
    int scale() const;

    chfdebug *m_dbg;
    chfdebug_video m_v;
    std::vector<uint32_t> m_px;

    QComboBox *m_view = nullptr, *m_scale = nullptr;
    QCheckBox *m_overlay[8] = {};
    VideoImage *m_image = nullptr;
    QLabel *m_hover = nullptr;
    QTabWidget *m_sections = nullptr;

    int m_hoverRow = -1, m_hoverCol = -1;
    int m_pinRow = -1, m_pinCol = -1;
    QPlainTextEdit *m_cellText = nullptr;

    QTableWidget *m_palTable = nullptr;
    RowStrip *m_strip = nullptr;
    QTableWidget *m_ports = nullptr;
    QLabel *m_portNote = nullptr;

    QSpinBox *m_row0 = nullptr, *m_row1 = nullptr, *m_col0 = nullptr, *m_col1 = nullptr;
    QCheckBox *m_changesOnly = nullptr, *m_thisFrame = nullptr, *m_palCols = nullptr;
    QLabel *m_logCounts = nullptr;
    QPlainTextEdit *m_logText = nullptr;
};
