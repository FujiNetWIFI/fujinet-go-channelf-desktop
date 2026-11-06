/*
 * SettingsDialog -- see SettingsDialog.h.
 *
 * Pages, as the Help describes them: Machine, Display & Audio, Input,
 * FujiNet, ROMs. The settings keys are the session's (the shared
 * settings.ini): machine bios aspect smooth enable_audio volume
 * enable_gamepad right_stick enable_fujinet fujinet_external
 * fujinet_external_addr, and the bindings.
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
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <vector>

#include "KeyForward.h"
#include "help/HelpWindow.h"

namespace {

const char *const kRomFilter =
    "BIOS images and ROM sets (*.zip *.rom *.bin *.ZIP *.ROM *.BIN);;All files (*)";

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
    int machine, bios, fujinet, external, audio, gamepad;
    QString addr;
    bool operator==(const RestartState &o) const
    {
        return machine == o.machine && bios == o.bios && fujinet == o.fujinet && external == o.external
               && audio == o.audio && gamepad == o.gamepad && addr == o.addr;
    }
};

RestartState restartState(chfsession *s)
{
    return { chfsession_get_int(s, "machine", CHF_MACHINE_CHANNELF),
             chfsession_get_int(s, "bios", CHF_BIOS_AUTO),
             chfsession_get_int(s, "enable_fujinet", 1),
             chfsession_get_int(s, "fujinet_external", 0),
             chfsession_get_int(s, "enable_audio", 1),
             chfsession_get_int(s, "enable_gamepad", 1),
             local(chfsession_get_str(s, "fujinet_external_addr", "")) };
}

/* The Help topic of each page (chfhelp contexts), by tab index. */
const char *const kPageContext[] = { "prefs/Machine", "prefs/Display", "prefs/Input", "prefs/FujiNet", "prefs/ROMs" };

class PrefsDialog : public QDialog {
public:
    PrefsDialog(QWidget *parent, chfsession *session);
    bool restartNeeded() const { return m_imported || !(m_start == restartState(m_s)); }

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
        chfsession_gamepad_capture_cancel(m_s);
        QDialog::done(r);
    }
    /* While a control waits for its key, every key is the answer. */
    bool eventFilter(QObject *obj, QEvent *e) override
    {
        if (m_armed >= 0 && e->type() == QEvent::KeyPress && (obj == m_controls || obj == m_controls->viewport())) {
            keyForControl(static_cast<QKeyEvent *>(e));
            return true;
        }
        return QDialog::eventFilter(obj, e);
    }

private:
    QWidget *machinePage();
    QWidget *displayPage();
    QWidget *inputPage();
    QWidget *fujinetPage();
    QWidget *romsPage();
    void refreshMachineNotes();
    void rebuildPads();
    void refreshRoms();
    void importRomsAndRefresh(bool folder);
    void fillControls();
    void arm(int target);
    void disarm(const QString &hint);
    void keyForControl(QKeyEvent *e);
    void pollPad();

    chfsession *m_s;
    RestartState m_start;
    bool m_imported = false;

    QComboBox *m_machine = nullptr, *m_bios = nullptr;
    QLabel *m_machineNote = nullptr, *m_biosNote = nullptr;
    QFormLayout *m_padForm = nullptr;
    std::vector<QWidget *> m_padRows;
    unsigned m_padGen = 0;
    QCheckBox *m_external = nullptr;
    QLineEdit *m_addr = nullptr;
    std::vector<QLabel *> m_romState;
    QTableWidget *m_controls = nullptr;
    QLabel *m_hint = nullptr;
    QTimer m_padTimer;
    int m_armed = -1;                  /* the target waiting for a key or pad button */
};

PrefsDialog::PrefsDialog(QWidget *parent, chfsession *session)
    : QDialog(parent), m_s(session), m_start(restartState(session))
{
    setWindowTitle(QStringLiteral("Preferences"));
    resize(700, 800);
    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    tabs->addTab(scrolled(machinePage()), QStringLiteral("Machine"));
    tabs->addTab(scrolled(displayPage()), QStringLiteral("Display && Audio"));
    tabs->addTab(scrolled(inputPage()), QStringLiteral("Input"));
    tabs->addTab(scrolled(fujinetPage()), QStringLiteral("FujiNet"));
    tabs->addTab(scrolled(romsPage()), QStringLiteral("ROMs"));
    outer->addWidget(tabs, 1);
    outer->addWidget(note(QStringLiteral("The picture, the volume and the controls apply as you change them; the "
                                         "machine, BIOS, audio, gamepad and FujiNet options take effect by "
                                         "power-cycling the console when this window closes.")));

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal
     * dialog) open the page's own */
    auto helpContext = [this, tabs] {
        setProperty("chfHelpContext", QString::fromLatin1(kPageContext[tabs->currentIndex()]));
    };
    helpContext();
    connect(tabs, &QTabWidget::currentChanged, this, helpContext);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Help);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::helpRequested, this, [this] { chfHelpFor(this); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), this, [this] { chfHelpFor(this); });
    outer->addWidget(buttons);

    refreshMachineNotes();
    refreshRoms();
    connect(&m_padTimer, &QTimer::timeout, this, [this] { pollPad(); });
}

/* ---- Machine --------------------------------------------------------------- */

QWidget *PrefsDialog::machinePage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);

    m_machine = new QComboBox;
    for (int i = 0; i < CHF_MACHINE_COUNT; ++i)
        m_machine->addItem(local(chf_machine_name(i)));
    const int cur = chfsession_get_int(m_s, "machine", CHF_MACHINE_CHANNELF);
    m_machine->setCurrentIndex(cur >= 0 && cur < CHF_MACHINE_COUNT ? cur : 0);
    connect(m_machine, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0) return;
        chfsession_set_int(m_s, "machine", idx);
        refreshMachineNotes();
    });
    form->addRow(QStringLiteral("Machine"), m_machine);
    m_machineNote = note(QString());
    form->addRow(m_machineNote);

    m_bios = new QComboBox;
    for (int i = 0; i < CHF_BIOS_CHOICE_COUNT; ++i)
        m_bios->addItem(local(chf_bios_choice_name(i)));
    const int bios = chfsession_get_int(m_s, "bios", CHF_BIOS_AUTO);
    m_bios->setCurrentIndex(bios >= 0 && bios < CHF_BIOS_CHOICE_COUNT ? bios : 0);
    m_bios->setToolTip(QStringLiteral("Which BIOS runs. The console's own BIOS is imported on the ROMs page; "
                                      "the app's stand-in runs CONFIG and the FujiNet apps."));
    connect(m_bios, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0) return;
        chfsession_set_int(m_s, "bios", idx);
        refreshMachineNotes();
    });
    form->addRow(QStringLiteral("BIOS"), m_bios);
    m_biosNote = note(QString());
    form->addRow(m_biosNote);
    return page;
}

void PrefsDialog::refreshMachineNotes()
{
    const int machine = m_machine->currentIndex();
    m_machineNote->setText(chf_machine_is_pal(machine)
                               ? QStringLiteral("PAL: the F3850 at 2 MHz, 312 lines, 50 frames a second.")
                               : QStringLiteral("NTSC: the F3850 at 1.79 MHz, 264 lines, 60.5 frames a second."));
    QString text;
    if (m_bios->currentIndex() == CHF_BIOS_STANDIN)
        text = QStringLiteral("The app's stand-in always runs: CONFIG and the FujiNet apps, but no Videocart.");
    else if (chfsession_have_bios(m_s, machine))
        text = QStringLiteral("The imported BIOS runs, so Videocarts work.");
    else
        text = QStringLiteral("No BIOS for this machine is imported, so the app's stand-in runs (CONFIG and the "
                              "FujiNet apps; a Videocart needs the BIOS: import it on the ROMs page).");
    m_biosNote->setText(text);
}

/* ---- Display & Audio ---------------------------------------------------------- */

QWidget *PrefsDialog::displayPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    /* "aspect" is 0 for the TV's 4:3 (the default), 1 for square pixels */
    auto *aspect = new QComboBox;
    aspect->addItems({ QStringLiteral("Television (4:3)"), QStringLiteral("Square pixels") });
    aspect->setCurrentIndex(chfsession_get_int(m_s, "aspect", 0) == 0 ? 0 : 1);
    connect(aspect, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx >= 0) chfsession_set_int(m_s, "aspect", idx);
    });
    form->addRow(QStringLiteral("Aspect"), aspect);
    auto *smooth = check(QStringLiteral("Smooth scaling"),
                         QStringLiteral("Blend the pixels when the picture is stretched; off keeps them sharp"),
                         m_s, "smooth", 0);
    connect(smooth, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "smooth", on ? 1 : 0); });
    form->addRow(smooth);

    auto *snd = check(QStringLiteral("Enable audio"), QStringLiteral("Open the system audio device"),
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

/* ---- Input ----------------------------------------------------------------------- */

QWidget *PrefsDialog::inputPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    auto *top = new QFormLayout;
    auto *gp = check(QStringLiteral("Enable gamepads"), QStringLiteral("Poll USB and Bluetooth gamepads"),
                     m_s, "enable_gamepad", 1);
    connect(gp, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_int(m_s, "enable_gamepad", on ? 1 : 0); });
    top->addRow(gp);
    auto *rs = check(QStringLiteral("Right stick works the knob"),
                     QStringLiteral("A gamepad's right stick twists (left and right), pulls (up) and pushes (down)"),
                     m_s, "right_stick", 1);
    connect(rs, &QCheckBox::toggled, this, [this](bool on) { chfsession_set_right_stick(m_s, on ? 1 : 0); });
    top->addRow(rs);
    v->addLayout(top);

    /* one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *pv = new QVBoxLayout(pads);
    pv->addWidget(note(QStringLiteral("The first pad is Player 1, the next Player 2, unless chosen here.")));
    m_padForm = new QFormLayout;
    pv->addLayout(m_padForm);
    v->addWidget(pads);
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

    /* every control with its key and gamepad button: choose one, then press */
    auto *controls = new QGroupBox(QStringLiteral("Controls"));
    auto *cv = new QVBoxLayout(controls);
    m_controls = new QTableWidget(CHF_TARGET_COUNT, 3);
    m_controls->setHorizontalHeaderLabels({ QStringLiteral("Control"), QStringLiteral("Key"), QStringLiteral("Gamepad") });
    m_controls->verticalHeader()->setVisible(false);
    m_controls->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_controls->setSelectionMode(QAbstractItemView::SingleSelection);
    m_controls->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_controls->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_controls->setMinimumHeight(240);
    m_controls->installEventFilter(this);
    m_controls->viewport()->installEventFilter(this);
    fillControls();
    connect(m_controls, &QTableWidget::cellClicked, this, [this](int row, int) { arm(row); });
    cv->addWidget(m_controls);
    m_hint = note(QStringLiteral("Choose a control, then press the key or the gamepad button to give it. "
                                 "A key does one thing: it is taken from its old job."));
    cv->addWidget(m_hint);
    auto *row = new QHBoxLayout;
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    clear->setToolTip(QStringLiteral("Take the key and the gamepad button off the chosen control"));
    connect(clear, &QPushButton::clicked, this, [this] {
        const int t = m_controls->currentRow();
        if (t < 0) return;
        chfsession_binding_set_key(m_s, t, 0, nullptr, 0);
        chfsession_binding_set_button(m_s, t, CHF_PAD_BTN_NONE, nullptr, 0);
        disarm(QStringLiteral("%1 has no key or button now.").arg(local(chf_target_name(t))));
        fillControls();
    });
    auto *reset = new QPushButton(QStringLiteral("Reset Controls"));
    connect(reset, &QPushButton::clicked, this, [this] {
        chfsession_bindings_reset(m_s);
        disarm(QStringLiteral("Every control is back to its default."));
        fillControls();
    });
    row->addWidget(clear);
    row->addWidget(reset);
    row->addStretch(1);
    cv->addLayout(row);
    v->addWidget(controls, 1);
    return page;
}

void PrefsDialog::fillControls()
{
    const int sel = m_controls->currentRow();
    for (int t = 0; t < CHF_TARGET_COUNT; ++t) {
        const chf_binding b = chfsession_binding_get(m_s, t);
        char key[48] = "";
        if (b.keysym) chfsession_keysym_name(b.keysym, key, sizeof key);
        const QString texts[3] = { local(chf_target_name(t)), key[0] ? local(key) : QStringLiteral("—"),
                                   b.button != CHF_PAD_BTN_NONE ? local(chf_pad_button_name(b.button))
                                                                : QStringLiteral("—") };
        for (int c = 0; c < 3; ++c) {
            QTableWidgetItem *it = m_controls->item(t, c);
            if (!it) {
                it = new QTableWidgetItem;
                m_controls->setItem(t, c, it);
            }
            it->setText(texts[c]);
        }
    }
    if (sel >= 0) m_controls->selectRow(sel);
}

void PrefsDialog::arm(int target)
{
    m_armed = target;
    chfsession_gamepad_capture_begin(m_s);
    m_padTimer.start(50);
    m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                        .arg(local(chf_target_name(target))));
}

void PrefsDialog::disarm(const QString &hint)
{
    m_armed = -1;
    m_padTimer.stop();
    chfsession_gamepad_capture_cancel(m_s);
    m_hint->setText(hint);
}

void PrefsDialog::keyForControl(QKeyEvent *e)
{
    if (e->isAutoRepeat() || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt || e->key() == Qt::Key_Meta)
        return;
    if (e->key() == Qt::Key_Escape) {
        disarm(QStringLiteral("Cancelled."));
        return;
    }
    const uint32_t ks = chfKeysymFromQt(e);
    if (!ks) return;
    if (chfIsAppKey(e)) {
        m_hint->setText(QStringLiteral("F9, F11 and F12 are the app's and cannot be given to a control."));
        return;
    }
    char stolen[128] = "", name[48] = "";
    const int target = m_armed;
    chfsession_binding_set_key(m_s, target, ks, stolen, sizeof stolen);
    chfsession_keysym_name(ks, name, sizeof name);
    disarm(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3).").arg(local(chf_target_name(target)), local(name), local(stolen))
                     : QStringLiteral("%1 is now %2.").arg(local(chf_target_name(target)), local(name)));
    fillControls();
}

void PrefsDialog::pollPad()
{
    int button;
    if (m_armed < 0) { m_padTimer.stop(); return; }
    if (!chfsession_gamepad_capture_poll(m_s, &button)) return;
    char stolen[128] = "";
    const int target = m_armed;
    chfsession_binding_set_button(m_s, target, button, stolen, sizeof stolen);
    disarm(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3).")
                           .arg(local(chf_target_name(target)), local(chf_pad_button_name(button)), local(stolen))
                     : QStringLiteral("%1 is now %2.").arg(local(chf_target_name(target)), local(chf_pad_button_name(button))));
    fillControls();
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
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
        const int assign = chfsession_gamepad_assignment(m_s, i);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        const int eff = chfsession_gamepad_effective_port(m_s, i);
        box->setToolTip(eff >= 0 ? QStringLiteral("Driving player %1").arg(eff + 1) : QStringLiteral("Driving no player"));
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
                                      "network and a link-down CONFIG."), m_s, "enable_fujinet", 1);
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
    form->addRow(QStringLiteral("Address"), m_addr);
    form->addRow(note(QStringLiteral("host:port, or just a port; empty means 127.0.0.1:%1. The cartridge dials "
                                     "this BoIP address, as it does the built-in FujiNet's.")
                          .arg(CHFSESSION_BOIP_PORT)));

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

/* ---- ROMs ------------------------------------------------------------------ */

QWidget *PrefsDialog::romsPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addWidget(note(QStringLiteral("The console's BIOS is not part of the app: CONFIG and the FujiNet apps run on "
                                     "its own stand-in, and a Videocart needs the real one. Import your own dumps "
                                     "from files, a zip or a folder (MAME's channelf.zip works); each is recognised "
                                     "by its size and checksum, whatever it is called. Kept in:")));
    auto *path = new QLabel(local(chfsession_roms_path(m_s)));
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    v->addWidget(path);

    auto *row = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import…"));
    auto *folder = new QPushButton(QStringLiteral("Import a Folder…"));
    connect(files, &QPushButton::clicked, this, [this] { importRomsAndRefresh(false); });
    connect(folder, &QPushButton::clicked, this, [this] { importRomsAndRefresh(true); });
    row->addWidget(files);
    row->addWidget(folder);
    row->addStretch(1);
    v->addLayout(row);

    /* one group per `group`, each ROM imported or not */
    QFormLayout *form = nullptr;
    QString last;
    for (int i = 0; i < chfsession_rom_item_count(); ++i) {
        const chf_rom_item *item = chfsession_rom_item(i);
        if (!item) continue;
        const QString group = local(item->group);
        if (!form || group != last) {
            auto *box = new QGroupBox(group);
            form = new QFormLayout(box);
            v->addWidget(box);
            last = group;
        }
        auto *name = new QLabel(local(item->name));
        name->setToolTip(QStringLiteral("%1, %2 bytes, CRC-32 %3").arg(local(item->file)).arg(item->size)
                             .arg(item->crc, 8, 16, QLatin1Char('0')));
        auto *state = new QLabel;
        form->addRow(name, state);
        m_romState.push_back(state);
    }
    v->addStretch(1);
    return page;
}

void PrefsDialog::refreshRoms()
{
    for (size_t i = 0; i < m_romState.size(); ++i) {
        const bool have = chfsession_rom_item_available(m_s, (int)i) != 0;
        m_romState[i]->setText(have ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
        m_romState[i]->setStyleSheet(have ? QString() : QStringLiteral("color: palette(placeholder-text);"));
    }
    if (m_machineNote) refreshMachineNotes();
}

void PrefsDialog::importRomsAndRefresh(bool folder)
{
    if (SettingsDialog::importRoms(this, m_s, folder) > 0) m_imported = true;
    refreshRoms();
}

} // namespace

/* ---- the public entry points ------------------------------------------------ */

int SettingsDialog::importRoms(QWidget *parent, chfsession *session, bool folder, const QString &path)
{
    QStringList inputs;
    if (!path.isEmpty()) {
        inputs << path;
    } else if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(parent, QStringLiteral("Import BIOS from a Folder"));
        if (dir.isEmpty()) return -1;
        inputs << dir;
    } else {
        inputs = QFileDialog::getOpenFileNames(parent, QStringLiteral("Import BIOS"), QString(),
                                               QString::fromUtf8(kRomFilter));
        if (inputs.isEmpty()) return -1;
    }

    int total = 0;
    bool trouble = false;
    QStringList said;
    for (const QString &in : inputs) {
        char msg[1024] = "";
        const int rc = chfsession_rom_import(session, in.toLocal8Bit().constData(), msg, sizeof msg);
        if (rc > 0) total += rc;
        if (rc <= 0) trouble = true;
        QString text = local(msg[0] ? msg : chfsession_last_error(session));
        if (inputs.size() > 1) text = QStringLiteral("%1: %2").arg(QFileInfo(in).fileName(), text);
        said << text;
    }
    /* the session's message always says what happened: show it whole */
    const QString text = said.join(QStringLiteral("\n\n"));
    if (total == 0)
        QMessageBox::warning(parent, QStringLiteral("Nothing Imported"), text);
    else if (trouble)
        QMessageBox::warning(parent, QStringLiteral("BIOS Imported"), text);
    else
        QMessageBox::information(parent, QStringLiteral("BIOS Imported"), text);
    return total;
}

bool SettingsDialog::run(QWidget *parent, chfsession *session)
{
    PrefsDialog dlg(parent, session);
    dlg.exec();
    return dlg.restartNeeded();
}
