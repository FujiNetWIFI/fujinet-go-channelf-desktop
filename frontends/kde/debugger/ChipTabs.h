/*
 * The debugger's chip tabs: Sound (the beeper's mode, level and the
 * sound-mode timeline) and Cart & I/O (the Videocart or the FujiNet
 * cartridge's state, its status page, mailbox and events and the memory map;
 * the hand controllers' and the panel's contacts, the ports raw, complemented
 * and decoded, a 2102 cartridge's RAM latch and the I/O log).
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

class SoundTab : public QWidget {
public:
    explicit SoundTab(chfdebug *debugger, QWidget *parent = nullptr);
    void refresh();

private:
    chfdebug *m_dbg;
    QLabel *m_sound = nullptr;
    LevelMeter *m_meter = nullptr;
    QPlainTextEdit *m_modes = nullptr;
};

class CartIoTab : public QWidget {
public:
    explicit CartIoTab(chfdebug *debugger, QWidget *parent = nullptr);
    void refresh();

private:
    void refreshCart();
    void refreshIo();

    chfdebug *m_dbg;
    ContactsWidget *m_pad[2] = {};
    ContactsWidget *m_panel = nullptr;
    QLabel *m_ports = nullptr;
    QLabel *m_ram = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QPlainTextEdit *m_mailbox = nullptr;
    QPlainTextEdit *m_events = nullptr;
};
