/*
 * ChipTabs -- see ChipTabs.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ChipTabs.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <cstring>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;
using dbgui::yesNo;

namespace {

const char *const kSoundModes[] = { "off", "1 kHz", "500 Hz", "120 Hz" };

} // namespace

/* ---- contacts ------------------------------------------------------------------------- */

/* A row of labelled contacts, a held one lit in the accent: a hand
 * controller's eight, or the panel's four buttons and RESET. */
class ContactsWidget : public QWidget {
public:
    ContactsWidget(const QString &title, const QStringList &labels) : m_title(title), m_labels(labels)
    {
        setFixedSize(m_labels.size() * kW + 2, kTitleH + kH + 2);
    }
    void set(unsigned bits)
    {
        if (bits == m_bits) return;
        m_bits = bits;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QFont f = font();
        f.setPointSizeF(qMax(7.0, f.pointSizeF() * 0.85));
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRect(0, 0, width(), kTitleH), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        p.setFont(f);
        for (int i = 0; i < m_labels.size(); ++i) {
            const QRect r(1 + i * kW, kTitleH, kW - 3, kH - 2);
            const bool on = (m_bits >> i) & 1u;
            p.setPen(palette().color(QPalette::Mid));
            p.setBrush(on ? chfAccentColor() : palette().color(QPalette::Base));
            p.drawRoundedRect(r, 4, 4);
            p.setPen(on ? chfAccentInk() : palette().color(QPalette::Text));
            p.drawText(r, Qt::AlignCenter, m_labels[i]);
        }
    }

private:
    static constexpr int kW = 78, kH = 26, kTitleH = 20;
    QString m_title;
    QStringList m_labels;
    unsigned m_bits = ~0u;
};

/* The beeper's level now, with a mark while it is forced on. */
class LevelMeter : public QWidget {
public:
    LevelMeter() { setFixedSize(300, 16); }
    void set(int level, bool forced)
    {
        if (level == m_level && forced == m_forced) return;
        m_level = level;
        m_forced = forced;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(palette().color(QPalette::Mid));
        p.setBrush(palette().color(QPalette::Base));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        p.setPen(Qt::NoPen);
        p.setBrush(m_forced ? chfAccentColor() : chfAccentTextColor(this));
        p.drawRect(1, 1, qBound(0, (width() - 2) * m_level / 32767, width() - 2), height() - 2);
    }

private:
    int m_level = -1;
    bool m_forced = false;
};

/* ---- Sound & I/O ------------------------------------------------------------------------ */

IoTab::IoTab(chfdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    auto *h = new QHBoxLayout(this);
    auto *left = new QVBoxLayout;

    auto *sound = new QGroupBox(QStringLiteral("Sound"));
    auto *sv = new QVBoxLayout(sound);
    m_sound = new QLabel;
    m_sound->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sv->addWidget(m_sound);
    m_meter = new LevelMeter;
    sv->addWidget(m_meter, 0, Qt::AlignLeft);
    sv->addWidget(dbgui::dimLabel(QStringLiteral("The level of the beeper's envelope; in the accent while it is forced "
                                                 "on (about 2 ms after a mode change). Port 5 bits 7-6 pick the mode.")));
    sv->addWidget(dbgui::titleLabel(QStringLiteral("Mode changes")));
    m_modes = dbgui::monoView();
    m_modes->setMaximumHeight(m_modes->fontMetrics().lineSpacing() * 7 + 14);
    sv->addWidget(m_modes);
    left->addWidget(sound);

    auto *pads = new QGroupBox(QStringLiteral("Controllers and panel"));
    auto *pv = new QVBoxLayout(pads);
    const QStringList contacts = { QStringLiteral("RIGHT"), QStringLiteral("LEFT"), QStringLiteral("BACK"),
                                   QStringLiteral("FORWARD"), QStringLiteral("CCW"), QStringLiteral("CW"),
                                   QStringLiteral("PULL"), QStringLiteral("PUSH") };
    m_pad[0] = new ContactsWidget(QStringLiteral("Player 1: the right controller (CPU port 1)"), contacts);
    m_pad[1] = new ContactsWidget(QStringLiteral("Player 2: the left controller (CPU port 4)"), contacts);
    m_panel = new ContactsWidget(QStringLiteral("The console's panel (port 0)"),
                                 { QStringLiteral("1 TIME"), QStringLiteral("2 MODE"), QStringLiteral("3 HOLD"),
                                   QStringLiteral("4 START"), QStringLiteral("RESET") });
    pv->addWidget(m_pad[0]);
    pv->addWidget(m_pad[1]);
    pv->addWidget(m_panel);
    m_bytes = new QLabel;
    m_bytes->setFont(dbgui::monoFont(m_bytes->font()));
    m_bytes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pv->addWidget(m_bytes);
    pv->addWidget(dbgui::dimLabel(QStringLiteral("Lit: held. The CPU reads the controllers active low, and only while "
                                                 "port 0 bit 6 is clear.")));
    left->addWidget(pads);

    auto *ram = new QGroupBox(QStringLiteral("2102 RAM (Maze, Hangman)"));
    auto *rv = new QVBoxLayout(ram);
    m_ram = new QLabel;
    m_ram->setFont(dbgui::monoFont(m_ram->font()));
    rv->addWidget(m_ram);
    left->addWidget(ram);
    left->addStretch(1);
    h->addLayout(left);

    auto *right = new QVBoxLayout;
    auto *head = new QHBoxLayout;
    head->addWidget(dbgui::titleLabel(QStringLiteral("I/O log: every port access")), 1);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_io_events_clear(m_dbg);
        refresh();
    });
    head->addWidget(clear);
    right->addLayout(head);
    m_log = dbgui::monoView();
    right->addWidget(m_log, 1);
    h->addLayout(right, 1);
}

void IoTab::refresh()
{
    chfdebug_io io;
    chfdebug_video v;
    chfdebug_io_get(m_dbg, &io);
    chfdebug_video_get(m_dbg, &v);

    const int mode = io.sound_mode & 3;
    m_sound->setText(QStringLiteral("Mode %1: %2.  Level %3.  Forced on: %4.\n%5 mode changes and %6 samples since power-on.")
                         .arg(mode).arg(QString::fromLatin1(kSoundModes[mode])).arg(io.sound_level)
                         .arg(yesNo(io.sound_forced)).arg(io.sound_changes).arg((qulonglong)io.audio_samples));
    m_meter->set(io.sound_level, io.sound_forced != 0);
    static chfdebug_sound_event ev[64];
    const int ne = chfdebug_sound_events(m_dbg, ev, 64);
    QString modes;
    for (int i = 0; i < ne; ++i)
        modes += QStringLiteral("frame %1  PC $%2  %3 → %4\n").arg(ev[i].frame, 5).arg(hx(ev[i].pc, 4),
                     QString::fromLatin1(kSoundModes[ev[i].old_mode & 3]), QString::fromLatin1(kSoundModes[ev[i].mode & 3]));
    if (!ne) modes = QStringLiteral("(none since the debugger attached)\n");
    dbgui::setText(m_modes, modes, dbgui::atEnd(m_modes));

    m_pad[0]->set(io.controller[0]);
    m_pad[1]->set(io.controller[1]);
    m_panel->set((io.panel & 0xF) | (io.reset_held ? 0x10u : 0u));
    m_bytes->setText(QStringLiteral("port 1 reads $%1 %2   port 4 reads $%3 %4\nport 0 reads $%5 %6   RESET %7")
                         .arg(hx(v.in1, 2), dbgui::bin8(v.in1), hx(v.in4, 2), dbgui::bin8(v.in4), hx(v.in0, 2),
                              dbgui::bin8(v.in0), io.reset_held ? QStringLiteral("held") : QStringLiteral("up")));
    if (io.has_2102)
        m_ram->setText(QStringLiteral("I/O ports $%1/$%2  address latch $%3  bit at it %4")
                           .arg(hx(io.ram_port, 2), hx(io.ram_port + 1, 2), hx(io.ram_addr, 3)).arg(io.ram_bit));
    else
        m_ram->setText(QStringLiteral("(the cartridge has no 2102 RAM)"));

    static chfdebug_io_event ioe[300];
    const int n = chfdebug_io_events(m_dbg, ioe, 300);
    QString t;
    for (int i = 0; i < n; ++i)
        t += QStringLiteral("frame %1  PC $%2  %3 port $%4 %5 $%6\n").arg(ioe[i].frame, 5).arg(hx(ioe[i].pc, 4),
                 ioe[i].is_write ? QStringLiteral("OUT") : QStringLiteral("IN "), hx(ioe[i].port, 2),
                 ioe[i].is_write ? QStringLiteral("<-") : QStringLiteral("->"), hx(ioe[i].value, 2));
    if (!n) t = QStringLiteral("(no port accesses since the debugger attached)\n");
    dbgui::setText(m_log, t, dbgui::atEnd(m_log));
}

/* ---- Cart ----------------------------------------------------------------------------------- */

CartTab::CartTab(chfdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    auto *h = new QHBoxLayout(this);
    m_text = dbgui::monoView();
    h->addWidget(m_text, 1);

    auto *split = new QSplitter(Qt::Vertical);
    auto *top = new QWidget;
    auto *tv = new QVBoxLayout(top);
    tv->setContentsMargins(0, 0, 0, 0);
    tv->addWidget(dbgui::titleLabel(QStringLiteral("The mailbox, $F800-$FFFF")));
    m_mailbox = dbgui::monoView();
    m_mailbox->setMinimumWidth(m_mailbox->fontMetrics().horizontalAdvance(QString(80, QLatin1Char('0'))));
    tv->addWidget(m_mailbox, 1);
    auto *bottom = new QWidget;
    auto *bv = new QVBoxLayout(bottom);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->addWidget(dbgui::titleLabel(QStringLiteral("Cartridge events")));
    m_events = dbgui::monoView();
    bv->addWidget(m_events, 1);
    split->addWidget(top);
    split->addWidget(bottom);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    h->addWidget(split);
}

void CartTab::refresh()
{
    chfdebug_cart c;
    char info[256] = "";
    chfdebug_cart_get(m_dbg, &c);
    chfdebug_cart_info(m_dbg, info, sizeof info);
    QString t = QString::fromUtf8(info) + QStringLiteral("\n\n");
    if (!c.present) {
        dbgui::setText(m_text, t);
        dbgui::setText(m_mailbox, QString());
        dbgui::setText(m_events, QString());
        return;
    }
    const QString type = QString::fromUtf8(c.type_name ? c.type_name : "?");
    const bool fuji = type.contains(QStringLiteral("FujiNet"), Qt::CaseInsensitive);
    t += QStringLiteral("Cartridge      %1\n").arg(type);
    if (!fuji) {
        if (c.softlist_name && c.softlist_name[0])
            t += QStringLiteral("Software list  %1\n").arg(QString::fromUtf8(c.softlist_name));
        if (c.description && c.description[0])
            t += QStringLiteral("Description    %1\n").arg(QString::fromUtf8(c.description));
        t += QStringLiteral("ROM            %1 bytes, CRC-32 %2\n").arg(c.rom_size).arg(hx(c.rom_crc, 8));
        t += QStringLiteral("RAM            %1\n").arg(c.ram_size ? QStringLiteral("%1 %2").arg(c.ram_size)
                                                                       .arg(c.ram_size == 1024 ? "bits (a 2102)" : "bytes")
                                                                    : QStringLiteral("none"));
        if (c.bank || c.half)
            t += QStringLiteral("Bank           %1, half %2\n").arg(c.bank).arg(c.half);
        dbgui::setText(m_text, t);
        dbgui::setText(m_mailbox, QStringLiteral("(not a FujiNet cartridge: it has no mailbox)"));
        dbgui::setText(m_events, QStringLiteral("(a Videocart reports no events)"));
        return;
    }
    t += QStringLiteral("Link           %1%2\n").arg(c.link_up ? "up" : "down", c.link_off ? " (switched off)" : "");
    t += QStringLiteral("Mailbox        %1\n").arg(c.mailbox ? "open (a FujiNet app: a socket and fujimail)" : "closed");
    t += QStringLiteral("App live       %1   booted game %2\n").arg(yesNo(c.app_live), yesNo(c.booted_game));
    t += QStringLiteral("Live image     %1 bytes, CRC-32 %2\n").arg(c.live_size).arg(hx(c.live_crc, 8));
    t += QStringLiteral("Staged         %1%2\n")
             .arg(c.staged_size ? QStringLiteral("%1 bytes, CRC-32 %2, %3").arg(c.staged_size).arg(hx(c.staged_crc, 8),
                                       c.staged_app ? QStringLiteral("an app%1").arg(c.staged_mailbox ? QStringLiteral(" claiming the mailbox") : QString())
                                                    : QStringLiteral("a game"))
                                : QStringLiteral("nothing"),
                  c.armed ? "  (armed: swaps at the cartridge's next swap point)" : "");
    t += QStringLiteral("Swaps          %1\n").arg(c.swaps);
    t += QStringLiteral("Pages          %1%2\n").arg(c.npages)
             .arg(c.npages > 0 ? (c.page >= 0 ? QStringLiteral(", page %1 at $4800").arg(c.page) : QStringLiteral(", none selected")) : QString());
    t += QStringLiteral("Boot           state %1, %2%, error %3\n").arg(c.boot_state).arg(c.boot_pct).arg(c.boot_err);

    /* $FC00-$FC0F as the console reads it */
    const uint8_t *s = c.status_page;
    t += QStringLiteral("\nStatus page $FC00-$FC0F\n");
    t += QStringLiteral("  $FC00 ACKSEQ      $%1\n").arg(hx(s[0], 2));
    t += QStringLiteral("  $FC01 STATUS      $%1  (%2%3)\n").arg(hx(s[1], 2), s[1] & 1 ? "link up" : "link down", s[1] & 2 ? ", busy" : "");
    t += QStringLiteral("  $FC02 ERR         $%1\n").arg(hx(s[2], 2));
    t += QStringLiteral("  $FC03 REPLY_CMD   $%1  (%2)\n").arg(hx(s[3], 2), s[3] == 0x06 ? "ACK" : s[3] == 0x15 ? "NAK" : "-");
    t += QStringLiteral("  $FC04 RXLEN       %1\n").arg(s[4] | (s[5] << 8));
    t += QStringLiteral("  $FC06 BOOT_STATE  %1\n").arg(s[6]);
    t += QStringLiteral("  $FC07 BOOT_PCT    %1%\n").arg(s[7]);
    t += QStringLiteral("  $FC08 BOOT_ERR    %1\n").arg(s[8]);
    t += QStringLiteral("  $FC09 MAGIC       %1%2\n").arg(QChar(s[9] >= 0x20 && s[9] < 0x7F ? s[9] : '.'),
                                                         QChar(s[10] >= 0x20 && s[10] < 0x7F ? s[10] : '.'));
    t += QStringLiteral("  $FC0B PROTO_VER   %1\n").arg(s[11]);
    t += QStringLiteral("  $FC0E TICK        $%1  (%2: the 60 Hz count)\n").arg(hx(s[14] | (s[15] << 8), 4)).arg(s[14] | (s[15] << 8));
    t += QStringLiteral("\nReply: %1 bytes waiting, last error $%2, queue depth %3\n").arg(c.rxlen).arg(hx(c.last_error, 2)).arg(c.queue_depth);
    dbgui::setText(m_text, t);

    QString dump;
    for (unsigned row = 0; row < sizeof c.mailbox_page / 16; ++row) {
        dump += QStringLiteral("$%1  ").arg(hx(0xF800 + row * 16, 4));
        QString ascii;
        for (unsigned col = 0; col < 16; ++col) {
            const uint8_t b = c.mailbox_page[row * 16 + col];
            dump += hx(b, 2) + QLatin1Char(' ');
            ascii += b >= 0x20 && b < 0x7F ? QChar(b) : QLatin1Char('.');
        }
        dump += QStringLiteral(" ") + ascii + QLatin1Char('\n');
    }
    dbgui::setText(m_mailbox, dump);

    static chfdebug_cart_event ev[200];
    const int n = chfdebug_cart_events(m_dbg, ev, 200);
    QString evs;
    for (int i = 0; i < n; ++i) {
        const chfdebug_cart_event &e = ev[i];
        QString what;
        switch (e.kind) {
        case 1: what = QStringLiteral("REG  $%1 = $%2").arg(hx(e.reg, 2), hx(e.value, 2)); break;
        case 2: what = QStringLiteral("TX   $%1").arg(hx(e.value, 2)); break;
        case 3: what = QStringLiteral("BANK page %1").arg(e.value); break;
        case 4: what = QStringLiteral("SWAP"); break;
        default: what = QStringLiteral("?    kind %1").arg(e.kind); break;
        }
        evs += QStringLiteral("frame %1  PC $%2  %3\n").arg(e.frame, 5).arg(hx(e.pc, 4), what);
    }
    if (!n) evs = QStringLiteral("(no events since the debugger attached)\n");
    dbgui::setText(m_events, evs, dbgui::atEnd(m_events));
}
