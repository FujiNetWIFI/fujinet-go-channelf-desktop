/*
 * The debugger's chip tabs: Sound & I/O (the beeper, both hand controllers'
 * eight contacts, the panel, the ports' traffic and a 2102 cartridge's RAM
 * port) and Cart (the Videocart or the FujiNet cartridge's state, its status
 * page and mailbox, and its events).
 *
 * Each is refreshed by the debugger window while it shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "chfdebug.h"

class ContactsWidget;
class LevelMeter;
class QLabel;
class QPlainTextEdit;

class IoTab : public QWidget {
public:
    explicit IoTab(chfdebug *debugger, QWidget *parent = nullptr);
    void refresh();

private:
    chfdebug *m_dbg;
    QLabel *m_sound = nullptr;
    LevelMeter *m_meter = nullptr;
    QPlainTextEdit *m_modes = nullptr;
    ContactsWidget *m_pad[2] = {};
    ContactsWidget *m_panel = nullptr;
    QLabel *m_bytes = nullptr;
    QLabel *m_ram = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

class CartTab : public QWidget {
public:
    explicit CartTab(chfdebug *debugger, QWidget *parent = nullptr);
    void refresh();

private:
    chfdebug *m_dbg;
    QPlainTextEdit *m_text = nullptr;
    QPlainTextEdit *m_mailbox = nullptr;
    QPlainTextEdit *m_events = nullptr;
};
