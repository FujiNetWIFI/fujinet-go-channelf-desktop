/*
 * ChipTabs -- see ChipTabs.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ChipTabs.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <cstring>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;
using dbgui::yesNo;

/* ---- Sound & I/O ------------------------------------------------------------------------ */

/* The console's keys as the machine has latched them: both 12-key keypads and
 * the panel, a key held lit. */
class KeyMatrixWidget : public QWidget {
public:
    KeyMatrixWidget() { setFixedSize(2 * kPadW + kGap + kPanelW + 2, 4 * kCellH + 2); }
    void set(uint64_t keys, int machine)
    {
        if (keys == m_keys && machine == m_machine) return;
        m_keys = keys;
        m_machine = machine;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QFont small = [&] { QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.8); return f; }();
        p.setFont(small);
        auto cell = [&](const QRect &r, int key) {
            const char *name = chfsession_key_label(m_machine, key);
            const bool held = (m_keys >> key) & 1u;
            p.setPen(QPen(palette().color(QPalette::Mid)));
            p.setBrush(held ? chfAccentColor() : palette().color(QPalette::Base));
            p.drawRoundedRect(r, 3, 3);
            p.setPen(held ? QColor(0x1B, 0x14, 0x08) : palette().color(QPalette::Text));
            p.drawText(r, Qt::AlignCenter, name ? QString::fromUtf8(name) : QStringLiteral("·"));
        };
        for (int port = 0; port < 2; ++port) {
            const int x0 = port * (kPadW + kGap) + 1;
            for (int k = 0; k < 12; ++k)
                cell(QRect(x0 + (k % 3) * (kPadW / 3), 1 + (k / 3) * kCellH, kPadW / 3 - 2, kCellH - 2),
                     CHF_KEY_PAD_BASE(port) + k);
        }
        const int px = 2 * (kPadW + kGap) - kGap + 6;
        for (int k = 0; k < 3; ++k) cell(QRect(px, 1 + k * kCellH, kPanelW - 6, kCellH - 2), CHF_KEY_START + k);
    }

private:
    static constexpr int kPadW = 3 * 46, kGap = 8, kCellH = 28, kPanelW = 72;
    uint64_t m_keys = 0;
    int m_machine = 0;
};

/* One disc as the console reads it: the four directions round the fire button,
 * and the pot position as a dot in a box. */
class StickDiagram : public QWidget {
public:
    StickDiagram() { setFixedSize(3 * kCell + 2 + 12 + kBox, 3 * kCell + 2); }
    void set(unsigned bits, int potX, int potY)
    {
        if (bits == m_bits && potX == m_px && potY == m_py) return;
        m_bits = bits;
        m_px = potX;
        m_py = potY;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        /* bit 0 UP, 1 RIGHT, 2 DOWN, 3 LEFT, 4 FIRE */
        static const struct { int bit, x, y; const char *label; } pads[5] = {
            { 0, 1, 0, "▲" }, { 1, 2, 1, "▶" }, { 2, 1, 2, "▼" }, { 3, 0, 1, "◀" }, { 4, 1, 1, "●" },
        };
        for (const auto &pd : pads) {
            const QRect r(1 + pd.x * kCell, 1 + pd.y * kCell, kCell - 3, kCell - 3);
            const bool on = (m_bits >> pd.bit) & 1u;
            p.setPen(palette().color(QPalette::Mid));
            p.setBrush(on ? chfAccentColor() : palette().color(QPalette::Base));
            p.drawRoundedRect(r, 4, 4);
            p.setPen(on ? QColor(0x1B, 0x14, 0x08) : palette().color(QPalette::Text));
            p.drawText(r, Qt::AlignCenter, QString::fromUtf8(pd.label));
        }
        /* the pots: 0-255 across (X) and down (Y), centre 128 */
        const QRect box(3 * kCell + 12, 1, kBox - 2, 3 * kCell - 3);
        p.setPen(palette().color(QPalette::Mid));
        p.setBrush(palette().color(QPalette::Base));
        p.drawRect(box);
        p.drawLine(box.center().x(), box.top(), box.center().x(), box.bottom());
        p.drawLine(box.left(), box.center().y(), box.right(), box.center().y());
        p.setPen(Qt::NoPen);
        p.setBrush(chfAccentTextColor(this));
        p.drawEllipse(QPointF(box.left() + m_px / 255.0 * box.width(), box.top() + m_py / 255.0 * box.height()), 4, 4);
    }

private:
    static constexpr int kCell = 30, kBox = 90;
    unsigned m_bits = 0;
    int m_px = 128, m_py = 128;
};

IoTab::IoTab(chfdebug *debugger, chfsession *session, QWidget *parent)
    : QWidget(parent), m_dbg(debugger), m_session(session)
{
    auto *h = new QHBoxLayout(this);
    m_text = dbgui::monoView();
    h->addWidget(m_text, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(dbgui::titleLabel(QStringLiteral("Keypads and panel as the console has them")));
    m_matrix = new KeyMatrixWidget;
    right->addWidget(m_matrix);
    right->addWidget(dbgui::dimLabel(QStringLiteral("Filled: held. Left to right: controller 1, controller 2, "
                                                    "START / OPTION / SELECT.")));
    right->addSpacing(8);
    right->addWidget(dbgui::titleLabel(QStringLiteral("Discs and pots")));
    auto *sticks = new QHBoxLayout;
    for (int i = 0; i < 2; ++i) {
        auto *col = new QVBoxLayout;
        auto *name = new QLabel(QStringLiteral("Controller %1").arg(i + 1));
        name->setAlignment(Qt::AlignHCenter);
        col->addWidget(name);
        m_stick[i] = new StickDiagram;
        col->addWidget(m_stick[i], 0, Qt::AlignHCenter);
        sticks->addLayout(col);
    }
    sticks->addStretch(1);
    right->addLayout(sticks);
    m_reset = new QLabel;
    right->addWidget(m_reset);
    right->addStretch();
    h->addLayout(right);
}

void IoTab::refresh()
{
    chfdebug_io io;
    chfdebug_uvi uvi;
    chfdebug_io_get(m_dbg, &io);
    chfdebug_uvi_get(m_dbg, &uvi);
    QString t;
    t += QStringLiteral("2637 UVI sound (the beeper)\n");
    t += QStringLiteral("  pitch divider  %1 ($18FD bits 0-6)\n").arg(uvi.sound_pitch);
    t += QStringLiteral("  volume         %1 ($18FE bits 0-2)\n").arg(uvi.sound_volume);
    t += QStringLiteral("  tone %1, noise %2\n").arg(uvi.sound_tone ? "on" : "off", uvi.sound_noise ? "on" : "off");
    t += QStringLiteral("  audio samples made  %1\n\n").arg((qulonglong)io.audio_samples);

    t += QStringLiteral("Keypad columns the CPU reads ($1900-$1907)\n  ");
    for (int i = 0; i < 8; ++i) t += hx(io.pad[i], 2) + QLatin1Char(' ');
    t += QStringLiteral("\nPanel $1908     $%1  (b0 START, b1 OPTION, b2 SELECT: 1 = held)\n").arg(hx(io.panel, 2));
    t += QStringLiteral("Pots (x, y)     controller 1: %1, %2    controller 2: %3, %4\n")
             .arg(io.pot[0][0]).arg(io.pot[0][1]).arg(io.pot[1][0]).arg(io.pot[1][1]);
    t += QStringLiteral("  read at $19FF / $19FE; the axis the CPU sees is chosen by $19F9 bit 6 (%1)\n")
             .arg(uvi.pot_y ? "Y" : "X");
    for (int i = 0; i < 2; ++i) {
        t += QStringLiteral("Controller %1 disc  digital $%2").arg(i + 1).arg(hx(io.sticks[i], 2));
        if (io.disc[i][0] || io.disc[i][1])
            t += QStringLiteral("   analog %1, %2 (overrides the digital bits)").arg(io.disc[i][0]).arg(io.disc[i][1]);
        t += QLatin1Char('\n');
    }
    t += QStringLiteral("\nSENSE (vertical blank)  %1\nFLAG output             %2\n").arg(io.sense).arg(io.flag);
    t += QStringLiteral("Reset key  %1\n").arg(io.reset_held ? "held" : "up");
    t += QStringLiteral("Machine    %1%2\n").arg(QString::fromUtf8(chf_machine_name(io.machine)),
                                              io.is_pal ? " (PAL: 50 Hz, 312 lines)" : "");
    t += QStringLiteral("Cartridge  %1\n").arg(io.cart_type == 3 ? "FujiNet" : io.cart_type == 2 ? "Golf mapper"
                                                : io.cart_type == 1 ? "standard" : "none");
    dbgui::setText(m_text, t);

    m_matrix->set(io.keys, io.machine);
    for (int i = 0; i < 2; ++i) m_stick[i]->set(io.sticks[i], io.pot[i][0], io.pot[i][1]);
    m_reset->setText(QStringLiteral("RESET key: %1").arg(io.reset_held ? QStringLiteral("held") : QStringLiteral("up")));
}

/* ---- Cart ----------------------------------------------------------------------------------- */

CartTab::CartTab(chfdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    auto *h = new QHBoxLayout(this);
    m_text = dbgui::monoView();
    h->addWidget(m_text, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(dbgui::titleLabel(QStringLiteral("The cartridge's mailbox page, $2B00-$2FFF")));
    m_mailbox = dbgui::monoView();
    m_mailbox->setMinimumWidth(m_mailbox->fontMetrics().horizontalAdvance(QString(80, QLatin1Char('0'))));
    right->addWidget(m_mailbox, 1);
    h->addLayout(right);
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
        return;
    }
    const QString type = QString::fromUtf8(c.type_name ? c.type_name : "?");
    const bool fuji = type.contains(QStringLiteral("FujiNet"), Qt::CaseInsensitive);
    t += QStringLiteral("Cartridge      %1\n").arg(type);
    if (!fuji) {
        t += QStringLiteral("ROM            %1 bytes\n").arg(c.rom_size);
        dbgui::setText(m_text, t);
        dbgui::setText(m_mailbox, QStringLiteral("(not a FujiNet cartridge: it has no mailbox)"));
        return;
    }
    t += QStringLiteral("Link           %1%2\n").arg(c.link_up ? "up" : "down", c.link_off ? " (switched off)" : "");
    t += QStringLiteral("Mailbox        %1\n").arg(c.mailbox ? "open (a FujiNet app: a socket and fujimail)" : "closed");
    t += QStringLiteral("App live       %1   booted game %2\n").arg(yesNo(c.app_live), yesNo(c.booted_game));
    t += QStringLiteral("Live image     %1 bytes, CRC-32 %2\n").arg(c.live_size).arg(hx(c.live_crc, 8));
    t += QStringLiteral("Staged         %1%2\n").arg(c.staged_size ? QStringLiteral("%1 bytes, CRC-32 %2, %3")
                                                                       .arg(c.staged_size).arg(hx(c.staged_crc, 8),
                                                                            c.staged_app ? QStringLiteral("an app%1")
                                                                                               .arg(c.staged_mailbox ? QStringLiteral(" claiming the mailbox") : QString())
                                                                                         : QStringLiteral("a game"))
                                                                 : QStringLiteral("nothing"),
                                               c.armed ? "  (armed: swaps at the cartridge's next swap point)" : "");
    t += QStringLiteral("Swaps          %1\n").arg(c.swaps);
    t += QStringLiteral("Banks          %1 (showing %2)\n").arg(c.nbanks).arg(c.fbank);
    t += QStringLiteral("Boot           state %1, %2%, error %3\n").arg(c.boot_state).arg(c.boot_pct).arg(c.boot_err);
    t += QStringLiteral("\nMailbox status page\n");
    t += QStringLiteral("  ACKSEQ $%1   STATUS $%2   last error $%3\n").arg(hx(c.ackseq, 2), hx(c.status, 2), hx(c.last_error, 2));
    t += QStringLiteral("  reply command $%1   RXLEN %2   queue depth %3\n").arg(hx(c.reply_cmd, 2)).arg(c.rxlen).arg(c.queue_depth);
    dbgui::setText(m_text, t);

    QString dump;
    for (unsigned row = 0; row < sizeof c.mailbox_page / 16; ++row) {
        dump += QStringLiteral("$%1  ").arg(hx(0x2B00 + row * 16, 4));
        QString ascii;
        for (unsigned col = 0; col < 16; ++col) {
            const uint8_t b = c.mailbox_page[row * 16 + col];
            dump += hx(b, 2) + QLatin1Char(' ');
            ascii += b >= 0x20 && b < 0x7F ? QChar(b) : QLatin1Char('.');
        }
        dump += QStringLiteral(" ") + ascii + QLatin1Char('\n');
    }
    dbgui::setText(m_mailbox, dump);
}
