/*
 * The debugger's Video tab: everything the 2637 UVI knows.
 *
 * Left, the picture the chip draws -- the last frame, the registers
 * re-rendered with the layers and overlays you choose, or the frame as far as
 * the beam has got -- at 2x with nearest-neighbour scaling, and what is under
 * the pointer. Right, the chip in sections: the four objects with their
 * positions and rendered bitmaps, the 16 x 13 (or 26) character screen with
 * its colours, every register $1800-$1AFF decoded (editable), the control,
 * collision and sound state, and the glyph, object and palette sheets.
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
#include <vector>

#include "chfdebug.h"

class BitsWidget;
class CharScreen;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableWidget;
class UviImage;

class VideoTab : public QWidget {
public:
    VideoTab(chfdebug *debugger, QWidget *parent = nullptr);
    /* Pulls the chip's state and redraws what shows. */
    void refresh();

private:
    QWidget *buildLeft();
    QWidget *buildObjects();
    QWidget *buildScreen();
    QWidget *buildRegisters();
    QWidget *buildControl();
    QWidget *buildSheets();

    void refreshPicture();
    void refreshObjects();
    void refreshScreen();
    void refreshRegisters();
    void refreshControl();
    void refreshSheets();
    void hover(int x, int y);
    QPixmap thingPixmap(int kind, int index, int scale);
    QPixmap viewPixmap(int view, int maxWidth);
    void loadPalette();
    unsigned layers() const;
    unsigned overlays() const;

    chfdebug *m_dbg;
    chfdebug_uvi m_uvi;
    std::vector<uint32_t> m_px;
    uint32_t m_pal[8] = {};           /* the 8 colours, XRGB */

    QComboBox *m_view = nullptr;
    QCheckBox *m_layer[2] = {};
    QCheckBox *m_overlay[4] = {};
    UviImage *m_image = nullptr;
    QLabel *m_hover = nullptr;
    QLabel *m_beam = nullptr;
    QTabWidget *m_sections = nullptr;

    QTableWidget *m_objects = nullptr;
    CharScreen *m_screen = nullptr;
    QPlainTextEdit *m_screenText = nullptr;
    QTableWidget *m_regs = nullptr;
    bool m_regsFilling = false;
    std::vector<char> m_regHot = std::vector<char>(0x300, 0);   /* changed at the last look */
    QPlainTextEdit *m_ctlText = nullptr;
    BitsWidget *m_collChars = nullptr;
    BitsWidget *m_collObjects = nullptr;
    QLabel *m_fontSheet = nullptr, *m_shapeSheet = nullptr, *m_paletteSheet = nullptr;
};
