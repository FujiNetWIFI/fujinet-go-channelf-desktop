/*
 * SettingsDialog -- see SettingsDialog.h.
 *
 * Pages, like the GNOME and Windows frontends: Machine, Display,
 * Audio, Input, FujiNet. The settings keys are the session's (the shared
 * settings.ini): machine aspect smooth enable_audio volume
 * enable_gamepad analog_joystick enable_fujinet fujinet_external
 * fujinet_external_addr.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <vector>

namespace {

QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

/* A hint under a row: word-wrapped, dimmed. */
QLabel *note(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    return l;
}

QCheckBox *check(const QString &text, const QString &tip, chfsession *s, const char *key, int def)
{
    auto *b = new QCheckBox(text);
    b->setToolTip(tip);
    b->setChecked(chfsession_get_int(s, key, def) != 0);
    return b;
}

/* A page that scrolls when the dialog is smaller than it. */
QWidget *scrolled(QWidget *page)
{
    auto *area = new QScrollArea;
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

/* What needs a power cycle to take effect. */
struct RestartState {
    int machine, fujinet, external, audio, gamepad;
    QString addr;
    bool operator==(const RestartState &o) const
    {
        return machine == o.machine && fujinet == o.fujinet
               && external == o.external && audio == o.audio && gamepad == o.gamepad && addr == o.addr;
    }
};

RestartState restartState(chfsession *s)
{
    return { chfsession_get_int(s, "machine", CHF_MACHINE_ARCADIA),
             chfsession_get_int(s, "enable_fujinet", 1),
             chfsession_get_int(s, "fujinet_external", 0), chfsession_get_int(s, "enable_audio", 1),
             chfsession_get_int(s, "enable_gamepad", 1),
             local(chfsession_get_str(s, "fujinet_external_addr", "")) };
}

class PrefsDialog : public QDialog {
public:
    PrefsDialog(QWidget *parent, chfsession *session);
    bool restartNeeded() const { return !(m_start == restartState(m_s)); }

protected:
    /* Closing with the external FujiNet switched on and an address that is
     * not host:port would only fail at the restart: say so now. */
    void done(int r) override
    {
        char host[64];
        int port;
        if (m_external->isChecked() &&
            chfsession_parse_hostport(m_addr->text().trimmed().toUtf8().constData(), host, sizeof host, &port) != 0) {
            QMessageBox::warning(this, QStringLiteral("External FujiNet"),
                                 QStringLiteral("\"%1\" is not an address:port. Example: 127.0.0.1:%2")
                                     .arg(m_addr->text()).arg(CHFSESSION_BOIP_PORT));
            m_addr->setFocus();
            return;
        }
        QDialog::done(r);
    }

private:
    QWidget *machinePage();
    QWidget *displayPage();
    QWidget *audioPage();
    QWidget *inputPage();
    QWidget *fujinetPage();
    void refreshMachineNotes();
    void rebuildPads();

    chfsession *m_s;
    RestartState m_start;

    QComboBox *m_machine = nullptr;
    QLabel *m_machineNote = nullptr;
    QFormLayout *m_padForm = nullptr;
    std::vector<QWidget *> m_padRows;
    unsigned m_padGen = 0;
    QCheckBox *m_external = nullptr;
    QLineEdit *m_addr = nullptr;
};

PrefsDialog::PrefsDialog(QWidget *parent, chfsession *session)
    : QDialog(parent), m_s(session), m_start(restartState(session))
{
    setWindowTitle(QStringLiteral("Preferences"));
    resize(660, 600);
    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    tabs->addTab(scrolled(machinePage()), QStringLiteral("Machine"));
    tabs->addTab(scrolled(displayPage()), QStringLiteral("Display"));
    tabs->addTab(scrolled(audioPage()), QStringLiteral("Audio"));
    tabs->addTab(scrolled(inputPage()), QStringLiteral("Input"));
    tabs->addTab(scrolled(fujinetPage()), QStringLiteral("FujiNet"));
    outer->addWidget(tabs, 1);
    outer->addWidget(note(QStringLiteral("Changes apply as you make them; the machine, audio, "
                                         "gamepad and FujiNet options take effect by power-cycling the console "
                                         "when this window closes.")));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);
    refreshMachineNotes();
}

/* ---- Machine --------------------------------------------------------------- */

QWidget *PrefsDialog::machinePage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);

    m_machine = new QComboBox;
    for (int i = 0; i < CHF_MACHINE_COUNT; ++i)
        m_machine->addItem(local(chf_machine_name(i)));
    const int cur = chfsession_get_int(m_s, "machine", CHF_MACHINE_ARCADIA);
    m_machine->setCurrentIndex(cur >= 0 && cur < CHF_MACHINE_COUNT ? cur : 0);
    connect(m_machine, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0) return;
        chfsession_set_int(m_s, "machine", idx);
        refreshMachineNotes();
    });
    form->addRow(QStringLiteral("Console"), m_machine);
    m_machineNote = note(QString());
    form->addRow(m_machineNote);
    form->addRow(note(QStringLiteral("The console has no BIOS, so there is nothing to import: games open from "
                                     "Machine > Open Cartridge. The 16-key Palladium is not supported yet.")));
    return page;
}

void PrefsDialog::refreshMachineNotes()
{
    const int machine = m_machine->currentIndex();
    QString text = chf_machine_is_pal(machine)
                       ? QStringLiteral("PAL clone, 50 Hz, 312 lines, 4.433 MHz / 5. Not verified against MAME, "
                                        "which runs every clone at NTSC timing.")
                       : QStringLiteral("NTSC, 60 Hz, 262 lines, Signetics 2650 at 3.58 MHz / 4.");
    m_machineNote->setText(text);
}

/* ---- Display, Audio, Input ---------------------------------------------------- */

QWidget *PrefsDialog::displayPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    /* "aspect" is 0 for the TV's 4:3 (the default), 1 for square pixels:
     * the box shows the first */
    auto *tv = new QCheckBox(QStringLiteral("TV aspect"));
    tv->setToolTip(QStringLiteral("Show the picture across a 4:3 television, as the console does; off shows "
                                  "square pixels"));
    tv->setChecked(chfsession_get_int(m_s, "aspect", 0) == 0);
    connect(tv, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "aspect", on ? 0 : 1); });
    auto *smooth = check(QStringLiteral("Smooth scaling"),
                         QStringLiteral("Filter the picture when scaling it; off keeps the pixels sharp"),
                         m_s, "smooth", 0);
    connect(smooth, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "smooth", on ? 1 : 0); });
    form->addRow(tv);
    form->addRow(smooth);
    return page;
}

QWidget *PrefsDialog::audioPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    auto *snd = check(QStringLiteral("Audio output"), QStringLiteral("Open the system audio device"),
                      m_s, "enable_audio", 1);
    connect(snd, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "enable_audio", on ? 1 : 0); });
    form->addRow(snd);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(chfsession_get_int(m_s, "volume", 100));
    auto *percent = new QLabel(QStringLiteral("%1%").arg(volume->value()));
    percent->setMinimumWidth(percent->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    connect(volume, &QSlider::valueChanged, this, [this, percent](int v) {
        chfsession_set_volume(m_s, v);
        percent->setText(QStringLiteral("%1%").arg(v));
    });
    auto *row = new QHBoxLayout;
    row->addWidget(volume, 1);
    row->addWidget(percent);
    form->addRow(QStringLiteral("Volume"), row);
    return page;
}

QWidget *PrefsDialog::inputPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    auto *top = new QFormLayout;
    auto *gp = check(QStringLiteral("Gamepads"), QStringLiteral("Poll USB and Bluetooth gamepads"),
                     m_s, "enable_gamepad", 1);
    connect(gp, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "enable_gamepad", on ? 1 : 0); });
    top->addRow(gp);
    auto *aj = check(QStringLiteral("Analog sticks steer too"),
                     QStringLiteral("A gamepad's left stick moves the disc continuously; off, it acts as a D-pad"),
                     m_s, "analog_joystick", 1);
    connect(aj, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_analog(m_s, on ? 1 : 0); });
    top->addRow(aj);
    v->addLayout(top);

    /* one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (given a controller in connection order unless chosen here)"));
    m_padForm = new QFormLayout(pads);
    v->addWidget(pads);
    v->addStretch(1);
    m_padGen = chfsession_gamepad_generation(m_s);
    rebuildPads();
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        const unsigned gen = chfsession_gamepad_generation(m_s);
        if (gen == m_padGen) return;
        m_padGen = gen;
        rebuildPads();
    });
    timer->start(1000);
    return page;
}

void PrefsDialog::rebuildPads()
{
    for (QWidget *w : m_padRows) m_padForm->removeRow(w);
    m_padRows.clear();
    const int n = chfsession_gamepad_count(m_s);
    if (n == 0) {
        auto *l = note(QStringLiteral("No gamepads connected: plug one in and it is picked up as it appears."));
        m_padForm->addRow(l);
        m_padRows.push_back(l);
        return;
    }
    for (int i = 0; i < n && i < 8; ++i) {
        char name[128];
        chfsession_gamepad_name(m_s, i, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Controller 1"), QStringLiteral("Controller 2") });
        box->setCurrentIndex(chfsession_gamepad_assignment(m_s, i) + 1);
        const int eff = chfsession_gamepad_effective_port(m_s, i);
        box->setToolTip(eff >= 0 ? QStringLiteral("Driving controller %1").arg(eff + 1)
                                 : QStringLiteral("Driving no controller"));
        connect(box, &QComboBox::currentIndexChanged, box, [this, i](int idx) {
            chfsession_gamepad_assign(m_s, i, idx - 1);
        });
        m_padForm->addRow(local(name), box);
        m_padRows.push_back(box);
    }
}

/* ---- FujiNet ---------------------------------------------------------------- */

QWidget *PrefsDialog::fujinetPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    auto *fuji = check(QStringLiteral("Enable FujiNet"),
                       QStringLiteral("Run the in-process FujiNet the cartridge dials into. Off means no "
                                      "network and a link-down CONFIG client."), m_s, "enable_fujinet", 1);
    connect(fuji, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "enable_fujinet", on ? 1 : 0); });
    form->addRow(fuji);

    m_external = check(QStringLiteral("Use external FujiNet"),
                       QStringLiteral("Skip the built-in FujiNet and dial another instance at this address. "
                                      "Start that instance listening on the same port."),
                       m_s, "fujinet_external", 0);
    connect(m_external, &QCheckBox::toggled, this, [this](bool on) {
        chfsession_set_int(m_s, "fujinet_external", on ? 1 : 0);
        m_addr->setEnabled(on);
    });
    form->addRow(m_external);

    m_addr = new QLineEdit(local(chfsession_get_str(m_s, "fujinet_external_addr", "")));
    m_addr->setPlaceholderText(QStringLiteral("127.0.0.1:%1").arg(CHFSESSION_BOIP_PORT));
    m_addr->setEnabled(m_external->isChecked());
    /* valid text (or none, meaning the default) is saved; anything else is
     * shown in red and not saved */
    connect(m_addr, &QLineEdit::textChanged, this, [this](const QString &text) {
        char host[64];
        int port;
        const QByteArray addr = text.trimmed().toUtf8();
        if (chfsession_parse_hostport(addr.constData(), host, sizeof host, &port) != 0) {
            m_addr->setStyleSheet(QStringLiteral("QLineEdit { color: #d83b3b; }"));
            return;
        }
        m_addr->setStyleSheet(QString());
        chfsession_set_str(m_s, "fujinet_external_addr", addr.constData());
    });
    form->addRow(QStringLiteral("Address:port"), m_addr);
    form->addRow(note(QStringLiteral("The cartridge dials this BoIP address, as it does the built-in "
                                     "FujiNet's.")));

    const bool external = chfsession_fujinet_external(m_s) != 0;
    const QString url = local(chfsession_fujinet_webui_url(m_s));
    auto *web = new QLabel(external ? QStringLiteral("FujiNet is external") : url.toHtmlEscaped());
    web->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *open = new QPushButton(QStringLiteral("Open"));
    open->setToolTip(QStringLiteral("Open FujiNet's web UI in the browser"));
    open->setEnabled(!external && chfsession_fujinet_running(m_s));
    connect(open, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(QUrl(url)); });
    auto *row = new QHBoxLayout;
    row->addWidget(web, 1);
    row->addWidget(open);
    form->addRow(QStringLiteral("Web UI"), row);
    return page;
}

} // namespace

bool SettingsDialog::run(QWidget *parent, chfsession *session)
{
    PrefsDialog dlg(parent, session);
    dlg.exec();
    return dlg.restartNeeded();
}
