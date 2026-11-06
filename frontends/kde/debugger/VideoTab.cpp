/*
 * VideoTab -- see VideoTab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "VideoTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cstring>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;

namespace {

/* the overlay checkboxes: label, the engine's flag, what it marks */
const struct { const char *label; unsigned flag; const char *tip; bool on; } kOverlays[8] = {
    { "Visible area", CHFDEBUG_F_VISIBLE, "VISIBLE: the TV's rectangle, columns 4-105 and rows 4-61 (VRAM and Values)", true },
    { "Grid", CHFDEBUG_F_GRID, "GRID: cell borders (from scale 2)", false },
    { "Palette columns", CHFDEBUG_F_PALCOLS, "PALCOLS: columns 125-127, where the row's palette is chosen (VRAM and Values)", false },
    { "Beam", CHFDEBUG_F_BEAM, "BEAM: the line the beam is on", false },
    { "Write cursor", CHFDEBUG_F_CURSOR, "CURSOR: the cell the next strobe writes (ports 4 and 5)", false },
    { "Heat", CHFDEBUG_F_HEAT, "HEAT: cells written this frame, brighter the more", false },
    { "Changed", CHFDEBUG_F_DIRTY, "DIRTY: cells that changed since the last frame", false },
    { "Palette rows", CHFDEBUG_F_PALROWS, "PALROWS: a tick at the left of the rows whose palette changed this frame", false },
};

const char *const kViews[] = { "Frame", "Live", "Beam", "VRAM", "Values", "Palettes" };
const char *const kViewTips[] = {
    "The last frame the machine finished: what the window shows",
    "The video RAM as it is now, drawn the same way",
    "What a CRT shows: rows the beam has passed this frame as they were when it drew them, the rest from the last frame, dimmed",
    "All 128 × 64 cells, the hidden columns and rows hatched",
    "The raw values 0-3 in false colour, whatever the palettes say",
    "The four palettes",
};
const char *const kSoundModes[] = { "off", "1 kHz", "500 Hz", "120 Hz" };

QColor fromXrgb(uint32_t v) { return QColor((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

QColor inkOn(const QColor &c) { return c.lightness() > 140 ? QColor(0, 0, 0) : QColor(255, 255, 255); }

void setCell(QTableWidget *t, int row, int col, const QString &text)
{
    QTableWidgetItem *it = t->item(row, col);
    if (!it) {
        it = new QTableWidgetItem;
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        t->setItem(row, col, it);
    }
    if (it->text() != text) it->setText(text);
}

QTableWidget *plainTable(const QStringList &headers, int rows)
{
    auto *t = new QTableWidget(rows, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setFocusPolicy(Qt::NoFocus);
    t->setFont(dbgui::monoFont(t->font()));
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(false);
    t->setFixedHeight(t->horizontalHeader()->sizeHint().height() + rows * t->verticalHeader()->defaultSectionSize() + 6);
    return t;
}

} // namespace

/* ---- the picture ---------------------------------------------------------------- */

/* The chosen view, exactly as the engine drew it (it scales), with a box round
 * the cell under the pointer and a second round the pinned one. */
class VideoImage : public QWidget {
public:
    std::function<void(int, int)> onHover;   /* a pixel, or -1 -1 when the pointer leaves */
    std::function<void(int, int)> onClick, onDouble;

    VideoImage()
    {
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        setFixedSize(204 * 2, 232 * 2);
    }
    void setImage(const QImage &img)
    {
        m_img = img;
        if (size() != img.size()) setFixedSize(img.size());
        update();
    }
    void setMarks(const QRect &hover, const QRect &pin)
    {
        if (hover == m_hover && pin == m_pin) return;
        m_hover = hover;
        m_pin = pin;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (m_img.isNull()) return;
        p.drawImage(0, 0, m_img);
        p.setBrush(Qt::NoBrush);
        auto box = [&](const QRect &r, const QColor &c) {
            if (r.isNull()) return;
            p.setPen(QPen(Qt::black, 4));
            p.drawRect(r.adjusted(-2, -2, 2, 2));
            p.setPen(QPen(c, 2));
            p.drawRect(r.adjusted(-2, -2, 2, 2));
        };
        box(m_hover, Qt::white);
        box(m_pin, chfAccentColor());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (onHover) onHover(e->position().toPoint().x(), e->position().toPoint().y());
    }
    void leaveEvent(QEvent *) override
    {
        if (onHover) onHover(-1, -1);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onClick) onClick(e->position().toPoint().x(), e->position().toPoint().y());
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onDouble) onDouble(e->position().toPoint().x(), e->position().toPoint().y());
    }

private:
    QImage m_img;
    QRect m_hover, m_pin;
};

/* ---- the row palette strip ------------------------------------------------------- */

/* All 64 rows' palettes: each row's number, its background colour and the
 * palette its columns 125 and 126 chose. A row whose palette changed since
 * the last look is marked in the accent for a few looks. */
class RowStrip : public QWidget {
public:
    RowStrip()
    {
        setFixedSize(2 * kW + 12, 32 * kH + 4);
        memset(m_pal, 0, sizeof m_pal);
        memset(m_age, 0, sizeof m_age);
    }
    void set(const uint8_t pal[64], const uint32_t bg[64])
    {
        bool changed = false;
        for (int i = 0; i < 64; ++i) {
            if (m_have && pal[i] != m_pal[i]) m_age[i] = 12;
            else if (m_age[i] > 0) m_age[i]--;
            if (pal[i] != m_pal[i] || bg[i] != m_bg[i] || m_age[i] != m_shown[i]) changed = true;
            m_pal[i] = pal[i];
            m_bg[i] = bg[i];
            m_shown[i] = m_age[i];
        }
        m_have = true;
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QFont small = dbgui::monoFont(font());
        small.setPointSizeF(qMax(7.0, font().pointSizeF() * 0.85));
        p.setFont(small);
        for (int i = 0; i < 64; ++i) {
            const int x = 2 + (i / 32) * (kW + 4), y = 2 + (i % 32) * kH;
            const QRect cell(x, y, kW, kH - 1);
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRect(x, y, 26, kH - 1), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("r%1").arg(i));
            const QRect sw(x + 30, y + 1, 96, kH - 3);
            p.setPen(palette().color(QPalette::Mid));
            p.setBrush(fromXrgb(m_bg[i]));
            p.drawRect(sw);
            p.setPen(inkOn(fromXrgb(m_bg[i])));
            p.drawText(sw, Qt::AlignCenter, QStringLiteral("palette %1").arg(m_pal[i]));
            if (m_age[i] > 0) {
                p.setPen(Qt::NoPen);
                p.setBrush(chfAccentColor());
                p.drawEllipse(QPoint(x + 136, y + kH / 2), 4, 4);
                p.setPen(palette().color(QPalette::Text));
                p.drawText(QRect(x + 144, y, kW - 144, kH - 1), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("changed"));
            }
            (void)cell;
        }
    }

private:
    static constexpr int kW = 220, kH = 16;
    uint8_t m_pal[64];
    uint32_t m_bg[64] = {};
    uint8_t m_age[64], m_shown[64] = {};
    bool m_have = false;
};

/* ---- the tab ---------------------------------------------------------------------- */

VideoTab::VideoTab(chfdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    memset(&m_v, 0, sizeof m_v);
    auto *h = new QHBoxLayout(this);
    h->addWidget(buildLeft());

    m_sections = new QTabWidget;
    m_sections->addTab(buildCell(), QStringLiteral("Cell"));
    m_sections->addTab(buildPalettes(), QStringLiteral("Palettes"));
    m_sections->addTab(buildPorts(), QStringLiteral("Ports"));
    m_sections->addTab(buildLog(), QStringLiteral("Write log"));
    if (qEnvironmentVariableIsSet("CHF_DEBUGGER_SECTION")) {
        const int n = qEnvironmentVariable("CHF_DEBUGGER_SECTION").toInt();
        if (n >= 0 && n < m_sections->count()) m_sections->setCurrentIndex(n);
    }
    connect(m_sections, &QTabWidget::currentChanged, this, [this] { refresh(); });
    h->addWidget(m_sections, 1);
}

QWidget *VideoTab::buildLeft()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);

    auto *row = new QHBoxLayout;
    m_view = new QComboBox;
    for (int i = 0; i < 6; ++i) {
        m_view->addItem(QString::fromLatin1(kViews[i]));
        m_view->setItemData(i, QString::fromLatin1(kViewTips[i]), Qt::ToolTipRole);
    }
    m_scale = new QComboBox;
    for (int s = 1; s <= 4; ++s) m_scale->addItem(QStringLiteral("%1×").arg(s), s);
    m_scale->setCurrentIndex(1);
    row->addWidget(new QLabel(QStringLiteral("View")));
    row->addWidget(m_view);
    row->addWidget(new QLabel(QStringLiteral("Scale")));
    row->addWidget(m_scale);
    row->addStretch(1);
    v->addLayout(row);

    auto *ov = new QGridLayout;
    for (int i = 0; i < 8; ++i) {
        m_overlay[i] = new QCheckBox(QString::fromLatin1(kOverlays[i].label));
        m_overlay[i]->setToolTip(QString::fromLatin1(kOverlays[i].tip));
        m_overlay[i]->setChecked(kOverlays[i].on);
        m_overlay[i]->setFocusPolicy(Qt::NoFocus);
        connect(m_overlay[i], &QCheckBox::toggled, this, [this] { refreshPicture(); });
        ov->addWidget(m_overlay[i], i / 4, i % 4);
    }
    v->addLayout(ov);
    connect(m_view, &QComboBox::currentIndexChanged, this, [this] {
        m_hoverRow = m_hoverCol = m_pinRow = m_pinCol = -1;
        refresh();
    });
    connect(m_scale, &QComboBox::currentIndexChanged, this, [this] { refresh(); });

    m_image = new VideoImage;
    m_image->onHover = [this](int x, int y) { hover(x, y); };
    m_image->onClick = [this](int x, int y) { click(x, y); };
    m_image->onDouble = [this](int x, int y) { doubleClick(x, y); };
    auto *area = new QScrollArea;
    area->setWidget(m_image);
    area->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    area->setMinimumWidth(256 * 2 + 36);
    v->addWidget(area, 1);

    m_hover = dbgui::dimLabel();
    m_hover->setMinimumHeight(m_hover->fontMetrics().lineSpacing() * 2 + 4);
    m_hover->setWordWrap(true);
    v->addWidget(m_hover);
    return w;
}

QWidget *VideoTab::buildCell()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_cellText = dbgui::monoView();
    v->addWidget(m_cellText, 1);
    v->addWidget(dbgui::dimLabel(QStringLiteral("Hover a cell of the picture for its row, column, value, palette and "
                                                "last write; click to pin it (click it again to let go); "
                                                "double-click to break on writes to it (again to remove).")));
    return w;
}

QWidget *VideoTab::buildPalettes()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(dbgui::titleLabel(QStringLiteral("The four palettes")));
    m_palTable = plainTable({ QStringLiteral("Palette"), QStringLiteral("Value 0"), QStringLiteral("Value 1"),
                              QStringLiteral("Value 2"), QStringLiteral("Value 3"), QStringLiteral("Rows") }, 4);
    m_palTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    v->addWidget(m_palTable);
    v->addWidget(dbgui::dimLabel(QStringLiteral("A row's palette is (column 126 & 2) | (column 125 >> 1), from the "
                                                "values in its own columns 125 and 126. Rows: how many of the 64 "
                                                "rows use it now.")));
    v->addWidget(dbgui::titleLabel(QStringLiteral("Each row's palette")));
    m_strip = new RowStrip;
    auto *area = new QScrollArea;
    area->setWidget(m_strip);
    area->setFrameShape(QFrame::NoFrame);
    v->addWidget(area, 1);
    return w;
}

QWidget *VideoTab::buildPorts()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(dbgui::titleLabel(QStringLiteral("The F8's four video ports")));
    m_ports = plainTable({ QStringLiteral("Port"), QStringLiteral("Latch"), QStringLiteral("Complement"),
                           QStringLiteral("Decoded"), QStringLiteral("CPU reads now") }, 4);
    m_ports->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    v->addWidget(m_ports);
    m_portNote = dbgui::dimLabel();
    v->addWidget(m_portNote);
    v->addStretch(1);
    return w;
}

QWidget *VideoTab::buildLog()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *f = new QGridLayout;
    auto spin = [](int lo, int hi, int val) {
        auto *s = new QSpinBox;
        s->setRange(lo, hi);
        s->setValue(val);
        return s;
    };
    m_row0 = spin(0, 63, 0);
    m_row1 = spin(0, 63, 63);
    m_col0 = spin(0, 127, 0);
    m_col1 = spin(0, 127, 127);
    f->addWidget(new QLabel(QStringLiteral("Rows")), 0, 0);
    f->addWidget(m_row0, 0, 1);
    f->addWidget(new QLabel(QStringLiteral("to")), 0, 2);
    f->addWidget(m_row1, 0, 3);
    f->addWidget(new QLabel(QStringLiteral("Columns")), 1, 0);
    f->addWidget(m_col0, 1, 1);
    f->addWidget(new QLabel(QStringLiteral("to")), 1, 2);
    f->addWidget(m_col1, 1, 3);
    m_changesOnly = new QCheckBox(QStringLiteral("Changes only"));
    m_changesOnly->setToolTip(QStringLiteral("Only the writes that changed their cell"));
    m_thisFrame = new QCheckBox(QStringLiteral("This frame"));
    m_palCols = new QCheckBox(QStringLiteral("Palette columns"));
    m_palCols->setToolTip(QStringLiteral("Only writes to columns 125-127"));
    f->addWidget(m_changesOnly, 0, 4);
    f->addWidget(m_thisFrame, 1, 4);
    f->addWidget(m_palCols, 0, 5);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chfdebug_vram_writes_clear(m_dbg);
        refreshLog();
    });
    f->addWidget(clear, 1, 5);
    f->setColumnStretch(6, 1);
    v->addLayout(f);
    for (QSpinBox *s : { m_row0, m_row1, m_col0, m_col1 })
        connect(s, &QSpinBox::valueChanged, this, [this] { refreshLog(); });
    for (QCheckBox *c : { m_changesOnly, m_thisFrame, m_palCols })
        connect(c, &QCheckBox::toggled, this, [this] { refreshLog(); });
    m_logCounts = dbgui::dimLabel();
    v->addWidget(m_logCounts);
    m_logText = dbgui::monoView();
    v->addWidget(m_logText, 1);
    return w;
}

/* ---- refreshing --------------------------------------------------------------- */

int VideoTab::view() const { return m_view->currentIndex(); }
int VideoTab::scale() const { return m_scale->currentData().toInt(); }

unsigned VideoTab::overlays() const
{
    unsigned f = 0;
    for (int i = 0; i < 8; ++i)
        if (m_overlay[i]->isChecked()) f |= kOverlays[i].flag;
    return f;
}

void VideoTab::refresh()
{
    chfdebug_video_get(m_dbg, &m_v);
    refreshPicture();
    refreshCell();
    switch (m_sections->currentIndex()) {
    case 1: refreshPalettes(); break;
    case 2: refreshPorts(); break;
    case 3: refreshLog(); break;
    default: break;
    }
}

void VideoTab::refreshPicture()
{
    int w = 0, h = 0;
    if (!chfdebug_video_view(m_dbg, view(), overlays(), scale(), nullptr, 0, &w, &h) || w <= 0 || h <= 0) return;
    m_px.assign((size_t)w * (size_t)h, 0);
    int w2 = 0, h2 = 0;
    if (!chfdebug_video_view(m_dbg, view(), overlays(), scale(), m_px.data(), m_px.size(), &w2, &h2)) return;
    /* the pixels are 0x00RRGGBB: make them opaque, or a compositor shows through */
    for (uint32_t &px : m_px) px |= 0xFF000000u;
    m_image->setImage(QImage(reinterpret_cast<const uchar *>(m_px.data()), w2, h2, w2 * 4, QImage::Format_RGB32).copy());
    QRect hover, pin;
    if (m_hoverRow >= 0) hover = cellRect(m_hoverRow, m_hoverCol);
    if (m_pinRow >= 0) pin = cellRect(m_pinRow, m_pinCol);
    m_image->setMarks(hover, pin);
}

/* where a VRAM cell is in the view (null when it is not shown) */
QRect VideoTab::cellRect(int row, int col) const
{
    const int s = scale();
    switch (view()) {
    case CHFDEBUG_VIEW_FRAME:
    case CHFDEBUG_VIEW_LIVE:
    case CHFDEBUG_VIEW_BEAM:
        if (row < CHFDEBUG_VIS_ROW0 || row >= CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS || col < CHFDEBUG_VIS_COL0
            || col >= CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS)
            return QRect();
        return QRect((col - CHFDEBUG_VIS_COL0) * 2 * s, (row - CHFDEBUG_VIS_ROW0) * 4 * s, 2 * s, 4 * s);
    case CHFDEBUG_VIEW_VRAM:
    case CHFDEBUG_VIEW_VALUES: return QRect(col * 2 * s, row * 4 * s, 2 * s, 4 * s);
    default: return QRect();
    }
}

bool VideoTab::hitAt(int x, int y, int *row, int *col)
{
    if (x < 0 || y < 0) return false;
    return chfdebug_video_hit(m_dbg, view(), x, y, scale(), row, col) >= 0;
}

void VideoTab::hover(int x, int y)
{
    int row, col;
    if (hitAt(x, y, &row, &col)) {
        m_hoverRow = row;
        m_hoverCol = col;
    } else {
        m_hoverRow = m_hoverCol = -1;
    }
    refreshPicture();
    refreshCell();
}

void VideoTab::click(int x, int y)
{
    int row, col;
    if (!hitAt(x, y, &row, &col)) return;
    if (row == m_pinRow && col == m_pinCol) m_pinRow = m_pinCol = -1;
    else { m_pinRow = row; m_pinCol = col; }
    refreshPicture();
    refreshCell();
}

/* Double-click: break on writes to the cell, or take that breakpoint off. */
void VideoTab::doubleClick(int x, int y)
{
    int row, col;
    if (!hitAt(x, y, &row, &col)) return;
    const int cell = row * CHFDEBUG_VRAM_COLS + col;
    chfdebug_breakpoint bps[256];
    const int n = chfdebug_breakpoint_list(m_dbg, bps, 256);
    for (int i = 0; i < n; ++i)
        if (bps[i].type == CHFDEBUG_BP_VRAM && bps[i].start == cell && bps[i].end == cell) {
            chfdebug_breakpoint_remove(m_dbg, bps[i].id);
            if (onMessage) onMessage(QStringLiteral("Breakpoint on writes to row %1 column %2 removed").arg(row).arg(col));
            return;
        }
    if (chfdebug_breakpoint_add(m_dbg, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, (uint16_t)cell, (uint16_t)cell, "") >= 0 && onMessage)
        onMessage(QStringLiteral("Breaking on writes to row %1 column %2").arg(row).arg(col));
}

QString VideoTab::cellReadout(int row, int col)
{
    chfdebug_vram_cell c;
    if (chfdebug_vram_cell_get(m_dbg, row, col, &c) < 0) return QStringLiteral("(no such cell)");
    QString t = QStringLiteral("Row %1, column %2  (cell %3)\n").arg(c.row).arg(c.col).arg(c.row * 128 + c.col);
    t += c.visible ? QStringLiteral("Shown on the TV\n")
                   : QStringLiteral("Hidden: %1\n").arg(QString::fromUtf8(c.hidden_why ? c.hidden_why : "outside the picture"));
    t += QStringLiteral("Value       %1\n").arg(c.value);
    t += QStringLiteral("Palette     %1 (%2)\n").arg(c.palette).arg(QString::fromUtf8(chfdebug_palette_name(c.palette)));
    t += QStringLiteral("Colour      pen %1, %2, %3\n").arg(c.pen).arg(QString::fromUtf8(chfdebug_colour_name(c.pen)),
                                                                     dbgui::rgbText(c.rgb));
    t += QStringLiteral("Writes      %1 since the debugger attached\n").arg(c.writes);
    if (c.last_pc >= 0)
        t += QStringLiteral("Last write  PC $%1, frame %2, line %3, over value %4")
                 .arg(hx((unsigned)c.last_pc, 4)).arg(c.last_frame).arg(c.last_line).arg(c.last_old);
    else
        t += QStringLiteral("Last write  never");
    return t;
}

void VideoTab::refreshCell()
{
    QString text;
    if (m_pinRow >= 0)
        text += QStringLiteral("Pinned\n") + cellReadout(m_pinRow, m_pinCol) + QStringLiteral("\n\n");
    if (m_hoverRow >= 0)
        text += QStringLiteral("Under the pointer\n") + cellReadout(m_hoverRow, m_hoverCol);
    else if (m_pinRow < 0)
        text = QStringLiteral("(no cell under the pointer)");
    dbgui::setText(m_cellText, text);

    /* one line under the picture */
    auto brief = [&](int row, int col) {
        chfdebug_vram_cell c;
        if (chfdebug_vram_cell_get(m_dbg, row, col, &c) < 0) return QString();
        return QStringLiteral("row %1 col %2: value %3, palette %4, %5 %6%7").arg(c.row).arg(c.col).arg(c.value)
            .arg(c.palette).arg(QString::fromUtf8(chfdebug_colour_name(c.pen)), dbgui::rgbText(c.rgb),
                                 c.visible ? QString() : QStringLiteral(" (hidden: %1)").arg(QString::fromUtf8(c.hidden_why ? c.hidden_why : "?")));
    };
    QString line;
    if (m_hoverRow >= 0) line = brief(m_hoverRow, m_hoverCol);
    if (m_pinRow >= 0) line += (line.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("Pinned: ") + brief(m_pinRow, m_pinCol);
    if (line.isEmpty()) line = QStringLiteral("Hover a cell");
    m_hover->setText(line);
}

void VideoTab::refreshPalettes()
{
    for (int p = 0; p < 4; ++p) {
        setCell(m_palTable, p, 0, QStringLiteral("%1 %2").arg(p).arg(QString::fromUtf8(chfdebug_palette_name(p))));
        for (int v = 0; v < 4; ++v) {
            const int pen = chfdebug_palette_pen(p, v);
            const uint32_t rgb = chfdebug_colour_rgb(pen);
            setCell(m_palTable, p, 1 + v, QStringLiteral("%1 %2").arg(QString::fromUtf8(chfdebug_colour_name(pen)), dbgui::rgbText(rgb)));
            QTableWidgetItem *it = m_palTable->item(p, 1 + v);
            it->setBackground(fromXrgb(rgb));
            it->setForeground(inkOn(fromXrgb(rgb)));
        }
        setCell(m_palTable, p, 5, QString::number(m_v.palette_rows[p]));
    }
    m_strip->set(m_v.row_palette, m_v.row_background);
}

void VideoTab::refreshPorts()
{
    const struct { int port; uint8_t latch, in; } ports[4] = {
        { 0, m_v.port0, m_v.in0 }, { 1, m_v.port1, m_v.in1 }, { 4, m_v.port4, m_v.in4 }, { 5, m_v.port5, m_v.in5 } };
    const QString decoded[4] = {
        QStringLiteral("strobe %1, controllers %2, panel latch %3")
            .arg(m_v.strobe ? "up" : "down", m_v.controllers_enabled ? "read" : "off", hx(m_v.panel_latch, 1)),
        QStringLiteral("colour value %1").arg(m_v.value),
        QStringLiteral("column %1").arg(m_v.col),
        QStringLiteral("row %1, sound %2").arg(m_v.row).arg(QString::fromLatin1(kSoundModes[m_v.sound_mode & 3])),
    };
    for (int i = 0; i < 4; ++i) {
        setCell(m_ports, i, 0, QStringLiteral("%1").arg(ports[i].port));
        setCell(m_ports, i, 1, QStringLiteral("$%1 %2").arg(hx(ports[i].latch, 2), dbgui::bin8(ports[i].latch)));
        const unsigned comp = (~ports[i].latch) & 0xFF;
        setCell(m_ports, i, 2, QStringLiteral("$%1 %2").arg(hx(comp, 2), dbgui::bin8(comp)));
        setCell(m_ports, i, 3, decoded[i]);
        setCell(m_ports, i, 4, QStringLiteral("$%1 %2").arg(hx(ports[i].in, 2), dbgui::bin8(ports[i].in)));
    }
    m_portNote->setText(QStringLiteral("The CPU writes a cell by putting the colour on port 1 (bits 7-6, complemented), "
                                       "the column on port 4 (complemented) and the row on port 5 (bits 5-0, "
                                       "complemented; bits 7-6 are the sound), then strobing port 0 bit 5. Ports 1 and 4 "
                                       "read player 1's (right) and player 2's (left) controller, active low, while "
                                       "port 0 bit 6 is clear; port 0 reads the panel's buttons.\n\n"
                                       "Beam: line %1 of %2, cycle %3 of %4. Frame %5. "
                                       "VRAM writes: %6 so far this frame; the last whole frame had %7, %8 of them "
                                       "changing their cell.")
                            .arg(m_v.line).arg(m_v.lines).arg(m_v.cycle_in_line).arg(m_v.cycles_per_line).arg(m_v.frame)
                            .arg(m_v.strobes_frame).arg(m_v.strobes_last).arg(m_v.changes_last));
}

void VideoTab::refreshLog()
{
    static chfdebug_vram_write w[500];
    chfdebug_vram_filter f;
    f.row0 = m_row0->value();
    f.row1 = m_row1->value();
    f.col0 = m_col0->value();
    f.col1 = m_col1->value();
    f.changes_only = m_changesOnly->isChecked();
    f.this_frame = m_thisFrame->isChecked();
    f.palette_cols = m_palCols->isChecked();
    const int n = chfdebug_vram_writes(m_dbg, &f, w, 500);
    chfdebug_video v;
    chfdebug_video_get(m_dbg, &v);
    m_logCounts->setText(QStringLiteral("This frame so far: %1 writes. Last whole frame: %2 writes, %3 changing their "
                                        "cell. Showing the newest %4.")
                             .arg(v.strobes_frame).arg(v.strobes_last).arg(v.changes_last).arg(n));
    QString t = QStringLiteral("frame  line  cycle      PC     cell        old → new\n");
    for (int i = 0; i < n; ++i)
        t += QStringLiteral("%1 %2 %3  $%4  r%5 c%6  %7 → %8\n").arg(w[i].frame, 5).arg(w[i].line, 4)
                 .arg((qulonglong)w[i].cycle, 10).arg(hx(w[i].pc, 4)).arg(w[i].row, 2).arg(w[i].col, -3)
                 .arg(w[i].old).arg(w[i].value);
    if (n == 0) t += QStringLiteral("(no writes match)\n");
    dbgui::setText(m_logText, t, dbgui::atEnd(m_logText));
}
