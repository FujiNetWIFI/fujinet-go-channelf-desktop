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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolTip>
#include <QVBoxLayout>
#include <cstring>
#include <functional>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;

namespace {

constexpr int kZoom = 2;   /* the picture is shown at 2x, nearest neighbour */
constexpr int kCellW = 16;  /* one character cell in the Screen section: 8 pixels wide at 2x, */
constexpr int kCellH8 = 16; /* 8 lines tall at 2x (a 16-line cell is drawn twice that) */

QString colourText(int c)
{
    return QStringLiteral("%1 %2").arg(c).arg(QString::fromUtf8(chfdebug_uvi_colour_name(c)));
}

QString shapeText(const uint8_t shape[8], int rows)
{
    QStringList b;
    for (int i = 0; i < rows; ++i) b << hx(shape[i], 2);
    return b.join(QLatin1Char(' '));
}

QColor fromXrgb(uint32_t v) { return QColor((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

/* Sets a table cell's text (creating the item), touching it only when it
 * changed. */
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

QTableWidget *objectTable(const QStringList &headers, int rows, int rowHeight)
{
    auto *t = new QTableWidget(rows, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(rowHeight);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setFocusPolicy(Qt::NoFocus);
    t->setFont(dbgui::monoFont(t->font()));
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setIconSize(QSize(rowHeight - 4, rowHeight - 4));
    /* tall enough for every row, so the section scrolls, not the table */
    t->setFixedHeight(t->horizontalHeader()->sizeHint().height() + rows * rowHeight + 4);
    return t;
}


const char *const kObjectBoxes[4] = { "red", "green", "blue", "orange" };

} // namespace

/* ---- the picture ---------------------------------------------------------------- */

/* The chip's picture at 2x, nearest neighbour, with a box round the object or
 * cell under the pointer. */
class UviImage : public QWidget {
public:
    std::function<void(int, int)> onHover;   /* frame pixel, or -1 -1 when the pointer leaves */

    UviImage()
    {
        setMouseTracking(true);
        setFixedSize(CHFSESSION_FB_WIDTH * kZoom, CHFSESSION_FB_HEIGHT * kZoom);
        setCursor(Qt::CrossCursor);
    }
    void setImage(const QImage &img)
    {
        m_img = img;
        update();
    }
    void setHighlight(const QRect &r)
    {
        if (r == m_hl) return;
        m_hl = r;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (m_img.isNull()) return;
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(rect(), m_img);
        if (!m_hl.isNull()) {
            const QRect r(m_hl.x() * kZoom, m_hl.y() * kZoom, m_hl.width() * kZoom, m_hl.height() * kZoom);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 3));
            p.drawRect(r.adjusted(-1, -1, 1, 1));
            p.setPen(QPen(Qt::white, 1));
            p.drawRect(r.adjusted(-1, -1, 1, 1));
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (onHover) onHover(qBound(0, (int)e->position().x() / kZoom, CHFSESSION_FB_WIDTH - 1),
                             qBound(0, (int)e->position().y() / kZoom, CHFSESSION_FB_HEIGHT - 1));
    }
    void leaveEvent(QEvent *) override
    {
        if (onHover) onHover(-1, -1);
    }

private:
    QImage m_img;
    QRect m_hl;
};

/* ---- rows of labelled bits ------------------------------------------------------------ */

/* Rows of bit cells under a header: the shift register (24 cells) and the
 * collision matrix. Bit j of a row's value is the cell at column j, or, with
 * msbFirst, at column n-1-j. */
class BitsWidget : public QWidget {
public:
    BitsWidget(const QStringList &columns, const QStringList &rows, int cellW, bool msbFirst = false)
        : m_cols(columns), m_rows(rows), m_cw(cellW), m_msbFirst(msbFirst), m_values(rows.size(), 0)
    {
        QFontMetrics fm(font());
        for (const QString &r : rows) m_labelW = qMax(m_labelW, fm.horizontalAdvance(r) + 10);
        m_ch = fm.height() + 8;
        setFixedSize(m_labelW + columns.size() * m_cw + 2, (rows.size() + 1) * (m_ch + 2) + 2);
    }
    void set(int row, uint32_t value)
    {
        if (m_values[(size_t)row] == value) return;
        m_values[(size_t)row] = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const int n = m_cols.size();
        p.setPen(palette().color(QPalette::Text));
        for (int j = 0; j < n; ++j)
            p.drawText(QRect(m_labelW + j * m_cw, 0, m_cw, m_ch), Qt::AlignCenter, m_cols[j]);
        for (int r = 0; r < m_rows.size(); ++r) {
            const int y = (r + 1) * (m_ch + 2);
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRect(0, y, m_labelW - 6, m_ch), Qt::AlignRight | Qt::AlignVCenter, m_rows[r]);
            for (int j = 0; j < n; ++j) {
                const int bit = m_msbFirst ? n - 1 - j : j;
                const bool on = (m_values[(size_t)r] >> bit) & 1u;
                const QRect c(m_labelW + j * m_cw + 1, y, m_cw - 2, m_ch);
                p.setPen(palette().color(QPalette::Mid));
                p.setBrush(on ? chfAccentColor() : palette().color(QPalette::Base));
                p.drawRoundedRect(c, 3, 3);
                p.setPen(on ? QColor(0x1B, 0x14, 0x08) : palette().color(QPalette::Text));
                p.drawText(c, Qt::AlignCenter, on ? QStringLiteral("1") : QStringLiteral("0"));
            }
        }
    }

private:
    QStringList m_cols, m_rows;
    int m_cw, m_ch = 0, m_labelW = 0;
    bool m_msbFirst;
    std::vector<uint32_t> m_values;
};

/* ---- the character screen ------------------------------------------------------------ */

/* The 16 x 13 (or 26) character screen as the chip draws it: every cell's
 * glyph in its own colours. Hover a cell for its code. */
class CharScreen : public QWidget {
public:
    CharScreen() { setMouseTracking(true); setFixedSize(CHFDEBUG_UVI_COLS * kCellW + 2, CHFDEBUG_UVI_MAX_ROWS * kCellH8 + 2); }
    void set(const chfdebug_uvi &u, const chfdebug_uvi_cell *cells, int rows, const QColor *pal)
    {
        std::memcpy(m_chars, u.chars, sizeof m_chars);
        std::memcpy(m_cells, cells, sizeof(chfdebug_uvi_cell) * (size_t)rows * CHFDEBUG_UVI_COLS);
        for (int i = 0; i < 8; ++i) m_pal[i] = pal[i];
        m_rows = rows;
        m_cellH = u.cell_height >= 16 ? 2 * kCellH8 : kCellH8;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        const qreal px = kCellW / 8.0, py = m_cellH / 8.0;
        for (int r = 0; r < m_rows; ++r)
            for (int c = 0; c < CHFDEBUG_UVI_COLS; ++c) {
                const chfdebug_uvi_cell &cell = m_cells[r * CHFDEBUG_UVI_COLS + c];
                const QRectF box(1 + c * kCellW, 1 + r * m_cellH, kCellW, m_cellH);
                p.fillRect(box, m_pal[cell.bg & 7]);
                p.setPen(Qt::NoPen);
                p.setBrush(m_pal[cell.fg & 7]);
                if (cell.graphics) {
                    /* 2 x 3 blocks, a bit each, low bit top left */
                    for (int b = 0; b < 6; ++b)
                        if ((cell.code >> b) & 1)
                            p.drawRect(QRectF(box.x() + (b & 1) * kCellW / 2.0, box.y() + (b >> 1) * m_cellH / 3.0,
                                              kCellW / 2.0, m_cellH / 3.0));
                } else {
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            if ((m_chars[cell.glyph & 0x3F][y] >> (7 - x)) & 1)
                                p.drawRect(QRectF(box.x() + x * px, box.y() + y * py, px, py));
                }
            }
        p.setPen(QColor(128, 128, 128, 90));
        for (int c = 0; c <= CHFDEBUG_UVI_COLS; ++c) p.drawLine(1 + c * kCellW, 1, 1 + c * kCellW, 1 + m_rows * m_cellH);
        for (int r = 0; r <= m_rows; ++r) p.drawLine(1, 1 + r * m_cellH, 1 + CHFDEBUG_UVI_COLS * kCellW, 1 + r * m_cellH);
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int c = (he->pos().x() - 1) / kCellW, r = (he->pos().y() - 1) / m_cellH;
            if (c < 0 || c >= CHFDEBUG_UVI_COLS || r < 0 || r >= m_rows) {
                QToolTip::hideText();
                return true;
            }
            const chfdebug_uvi_cell &cell = m_cells[r * CHFDEBUG_UVI_COLS + c];
            QToolTip::showText(he->globalPos(),
                               QStringLiteral("row %1 column %2: $%3 %4, foreground %5, background %6")
                                   .arg(r).arg(c).arg(hx(cell.code, 2),
                                       cell.graphics ? QStringLiteral("block graphics")
                                                     : QStringLiteral("glyph $%1").arg(hx(cell.glyph, 2)),
                                       colourText(cell.fg), colourText(cell.bg)),
                               this);
            return true;
        }
        return QWidget::event(e);
    }

private:
    chfdebug_uvi_cell m_cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS] = {};
    uint8_t m_chars[0x40][8] = {};
    QColor m_pal[8];
    int m_rows = 13;
    int m_cellH = 2 * kCellH8;
};

/* ---- the tab --------------------------------------------------------------------------- */

VideoTab::VideoTab(chfdebug *debugger, QWidget *parent)
    : QWidget(parent), m_dbg(debugger), m_px(CHFDEBUG_UVI_VIEW_MAX_PIXELS)
{
    memset(&m_uvi, 0, sizeof m_uvi);
    loadPalette();

    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QScrollArea;
    left->setWidget(buildLeft());
    left->setFrameShape(QFrame::NoFrame);
    split->addWidget(left);
    m_sections = new QTabWidget;
    m_sections->addTab(buildObjects(), QStringLiteral("Objects"));
    m_sections->addTab(buildScreen(), QStringLiteral("Screen"));
    m_sections->addTab(buildRegisters(), QStringLiteral("Registers"));
    m_sections->addTab(buildControl(), QStringLiteral("Control/Collisions"));
    m_sections->addTab(buildSheets(), QStringLiteral("Sheets"));
    connect(m_sections, &QTabWidget::currentChanged, this, [this] { refresh(); });
    split->addWidget(m_sections);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    left->setMinimumWidth(CHFSESSION_FB_WIDTH * kZoom + 36);
    m_sections->setMinimumWidth(460);

    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(split);
    hover(-1, -1);
}

/* The 8 colours as the chip draws them, sampled from the palette strip (one
 * cell each in a row), for the swatches and the character screen. */
void VideoTab::loadPalette()
{
    for (int i = 0; i < 8; ++i) m_pal[i] = 0x808080;
    int w = 0, h = 0;
    if (!chfdebug_uvi_view(m_dbg, CHFDEBUG_UVI_PALETTE, CHFDEBUG_UVI_L_ALL, 0, 1, m_px.data(), m_px.size(), &w, &h)
        || w < 8 || h < 1)
        return;
    const int cell = w / 8;
    if (cell < 1) return;
    for (int col = 0; col < 8; ++col)
        m_pal[col] = m_px[(size_t)(h / 2) * w + col * cell + cell / 2];
}

/* ---- building ------------------------------------------------------------------------------ */

QWidget *VideoTab::buildLeft()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setSizeConstraint(QLayout::SetFixedSize);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("View")));
    m_view = new QComboBox;
    m_view->addItems({ QStringLiteral("Frame (what the display shows)"),
                       QStringLiteral("Rendered from the registers"),
                       QStringLiteral("Frame in progress, up to the beam") });
    m_view->setCurrentIndex(CHFDEBUG_UVI_RENDER);
    m_view->setToolTip(QStringLiteral("Rendered re-draws the chip's registers as they are now with the layers "
                                      "and overlays below, whatever the beam is doing; Frame is the last "
                                      "finished frame; Frame in progress shows what the beam has drawn so far"));
    connect(m_view, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    row->addWidget(m_view, 1);
    v->addLayout(row);

    static const char *const layerNames[2] = { "Character screen", "Objects" };
    auto *lg = new QGridLayout;
    lg->addWidget(dbgui::titleLabel(QStringLiteral("Layers")), 0, 0, 1, 2);
    for (int i = 0; i < 2; ++i) {
        m_layer[i] = new QCheckBox(QString::fromLatin1(layerNames[i]));
        m_layer[i]->setChecked(true);
        m_layer[i]->setFocusPolicy(Qt::NoFocus);
        connect(m_layer[i], &QCheckBox::toggled, this, [this] { refresh(); });
        lg->addWidget(m_layer[i], 1, i);
    }
    v->addLayout(lg);

    static const struct { const char *name, *tip; bool on; } overlays[4] = {
        { "Object boxes", "Outline every visible object (red, green, blue, orange for 0-3)", true },
        { "Collisions", "Outline the objects that hit something in magenta", false },
        { "Character grid", "Draw the character cells' borders", false },
        { "Beam cross-hair", "A cross-hair where the beam is", true },
    };
    auto *og = new QGridLayout;
    og->addWidget(dbgui::titleLabel(QStringLiteral("Overlays")), 0, 0, 1, 2);
    for (int i = 0; i < 4; ++i) {
        m_overlay[i] = new QCheckBox(QString::fromLatin1(overlays[i].name));
        m_overlay[i]->setToolTip(QString::fromLatin1(overlays[i].tip));
        m_overlay[i]->setChecked(overlays[i].on);
        m_overlay[i]->setFocusPolicy(Qt::NoFocus);
        connect(m_overlay[i], &QCheckBox::toggled, this, [this] { refresh(); });
        og->addWidget(m_overlay[i], 1 + i / 2, i % 2);
    }
    v->addLayout(og);

    m_image = new UviImage;
    m_image->onHover = [this](int x, int y) { hover(x, y); };
    v->addWidget(m_image);
    m_hover = new QLabel;
    m_hover->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_hover->setFixedWidth(CHFSESSION_FB_WIDTH * kZoom);
    m_hover->setWordWrap(true);
    m_hover->setMinimumHeight(m_hover->fontMetrics().lineSpacing() * 3 + 4);
    m_hover->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(m_hover);
    m_beam = dbgui::dimLabel();
    m_beam->setFixedWidth(CHFSESSION_FB_WIDTH * kZoom);
    v->addWidget(m_beam);
    v->addStretch();
    return w;
}

QWidget *VideoTab::buildObjects()
{
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Objects: four 8-wide shapes (8 or 16 lines), held in the UDCs $38-$3B")));
    m_objects = objectTable({ QStringLiteral("#"), QStringLiteral("Bitmap"), QStringLiteral("X"), QStringLiteral("Y"),
                              QStringLiteral("Raw X,Y"), QStringLiteral("Colour"), QStringLiteral("Size"),
                              QStringLiteral("UDC"), QStringLiteral("Hits"), QStringLiteral("State"),
                              QStringLiteral("Shape rows") }, 4, 40);
    v->addWidget(m_objects);
    v->addWidget(dbgui::dimLabel(QStringLiteral("X and Y are screen pixels of the 128-pixel playfield; (0,0) is its "
                                                "top-left and the object may sit past an edge. Hits: the "
                                                "characters (C) and the other objects it overlaps. Shape rows "
                                                "are the UDC bytes, bit 7 leftmost.")));
    v->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

QWidget *VideoTab::buildScreen()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;
    left->addWidget(dbgui::titleLabel(QStringLiteral("Character screen: 16 columns x 13 rows (26 with two screens)")));
    m_screen = new CharScreen;
    left->addWidget(m_screen);
    left->addWidget(dbgui::dimLabel(QStringLiteral("Each cell in its own colours; hover for its code. The lower "
                                                   "screen ($1A00) shows when $19F8 bit 6 is set.")));
    left->addStretch();
    h->addLayout(left);
    m_screenText = dbgui::monoView();
    m_screenText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    h->addWidget(m_screenText, 1);
    return w;
}

QWidget *VideoTab::buildRegisters()
{
    constexpr int kRegs = 0x300;
    m_regs = new QTableWidget(kRegs, 4);
    m_regs->setHorizontalHeaderLabels({ QStringLiteral("Addr"), QStringLiteral("Name"), QStringLiteral("Value"),
                                        QStringLiteral("Decoded") });
    m_regs->verticalHeader()->setVisible(false);
    m_regs->verticalHeader()->setDefaultSectionSize(m_regs->fontMetrics().height() + 6);
    m_regs->setFont(dbgui::monoFont(m_regs->font()));
    m_regs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_regs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_regs->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_regs->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_regs->horizontalHeader()->setStretchLastSection(true);
    m_regsFilling = true;
    for (int off = 0; off < kRegs; ++off) {
        for (int col = 0; col < 4; ++col) {
            auto *it = new QTableWidgetItem;
            if (col != 2) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            m_regs->setItem(off, col, it);
        }
        m_regs->item(off, 0)->setText(QStringLiteral("$") + hx(0x1800 + off, 4));
    }
    m_regsFilling = false;
    /* a store into the chip's registers, with its write effects */
    connect(m_regs, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_regsFilling || it->column() != 2) return;
        bool ok = false;
        QString t = it->text().trimmed();
        if (t.startsWith(QLatin1Char('$'))) t = t.mid(1);
        const unsigned v = t.toUInt(&ok, 16);
        if (ok && v <= 0xFF)
            chfdebug_uvi_write(m_dbg, (unsigned)it->row(), (uint8_t)v);
        refresh();
    });
    return m_regs;
}

QWidget *VideoTab::buildControl()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_ctlText = dbgui::monoView();
    m_ctlText->setMaximumHeight(380);
    v->addWidget(m_ctlText, 1);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Collisions: objects against characters ($19FC)")));
    m_collChars = new BitsWidget({ QStringLiteral("obj 0"), QStringLiteral("obj 1"), QStringLiteral("obj 2"),
                                   QStringLiteral("obj 3") },
                                 { QStringLiteral("hit") }, 56);
    v->addWidget(m_collChars);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Collisions: object pairs ($19FD)")));
    m_collObjects = new BitsWidget({ QStringLiteral("0-1"), QStringLiteral("0-2"), QStringLiteral("0-3"),
                                     QStringLiteral("1-2"), QStringLiteral("1-3"), QStringLiteral("2-3") },
                                   { QStringLiteral("hit") }, 56);
    v->addWidget(m_collObjects);
    v->addWidget(dbgui::dimLabel(QStringLiteral("A hit reads as 0 in the register; lit here means a hit. The "
                                                "registers clear when the CPU reads them.")));
    v->addStretch();
    return w;
}

QWidget *VideoTab::buildSheets()
{
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    auto section = [&](const QString &title, QLabel *&label, const QString &note) {
        v->addWidget(dbgui::titleLabel(title));
        v->addWidget(dbgui::dimLabel(note));
        label = new QLabel;
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        v->addWidget(label);
    };
    section(QStringLiteral("Glyphs: 64 characters"), m_fontSheet,
            QStringLiteral("The built-in set ($00-$37) and the eight user-defined characters ($38-$3F, the last "
                           "row; the first four are the objects' images), in the character colour."));
    section(QStringLiteral("Objects"), m_shapeSheet, QStringLiteral("The four objects' images, in their colours."));
    section(QStringLiteral("Palette"), m_paletteSheet, QStringLiteral("The chip's eight colours."));
    v->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

/* ---- refreshing --------------------------------------------------------------------------- */

unsigned VideoTab::layers() const
{
    unsigned m = 0;
    if (m_layer[0]->isChecked()) m |= CHFDEBUG_UVI_L_CHARS;
    if (m_layer[1]->isChecked()) m |= CHFDEBUG_UVI_L_OBJECTS;
    return m;
}

unsigned VideoTab::overlays() const
{
    static const unsigned flags[4] = { CHFDEBUG_UVI_F_BOXES, CHFDEBUG_UVI_F_COLLIDE, CHFDEBUG_UVI_F_GRID,
                                       CHFDEBUG_UVI_F_BEAM };
    unsigned f = 0;
    for (int i = 0; i < 4; ++i)
        if (m_overlay[i]->isChecked()) f |= flags[i];
    return f;
}

void VideoTab::refresh()
{
    chfdebug_uvi_get(m_dbg, &m_uvi);
    refreshPicture();
    switch (m_sections->currentIndex()) {
    case 0: refreshObjects(); break;
    case 1: refreshScreen(); break;
    case 2: refreshRegisters(); break;
    case 3: refreshControl(); break;
    case 4: refreshSheets(); break;
    default: break;
    }
}

void VideoTab::refreshPicture()
{
    int w = 0, h = 0;
    if (chfdebug_uvi_view(m_dbg, m_view->currentIndex(), layers(), overlays(), 1, m_px.data(), m_px.size(), &w, &h)
        && w > 0 && h > 0) {
        const QImage img(reinterpret_cast<const uchar *>(m_px.data()), w, h, w * 4, QImage::Format_RGB32);
        m_image->setImage(img.copy());   /* the buffer is reused by the next call */
    }
    m_beam->setText(QStringLiteral("Beam at line %1 of %2 (%3), character row %4; %5.")
                        .arg(m_uvi.line).arg(m_uvi.lines)
                        .arg(m_uvi.vblank ? QStringLiteral("vertical blank") : QStringLiteral("drawing"))
                        .arg(m_uvi.charline)
                        .arg(m_uvi.lines == 312 ? QStringLiteral("PAL, 50 Hz") : QStringLiteral("NTSC, 60 Hz")));
}

/* An object or glyph as the tables draw it, as a pixmap of `scale` per pixel. */
QPixmap VideoTab::thingPixmap(int kind, int index, int scale)
{
    int w = 0, h = 0;
    if (!chfdebug_uvi_thing(m_dbg, kind, index, scale, 0xFF202020u, m_px.data(), m_px.size(), &w, &h) || w <= 0 || h <= 0)
        return QPixmap();
    const QImage img(reinterpret_cast<const uchar *>(m_px.data()), w, h, w * 4, QImage::Format_RGB32);
    /* copied: the image only wraps the buffer the next call overwrites */
    return QPixmap::fromImage(img.copy());
}

/* A sheet view scaled up by a whole number to fit maxWidth. */
QPixmap VideoTab::viewPixmap(int view, int maxWidth)
{
    int w = 0, h = 0;
    if (!chfdebug_uvi_view(m_dbg, view, CHFDEBUG_UVI_L_ALL, 0, 1, m_px.data(), m_px.size(), &w, &h) || w <= 0 || h <= 0)
        return QPixmap();
    const QImage img(reinterpret_cast<const uchar *>(m_px.data()), w, h, w * 4, QImage::Format_RGB32);
    const int scale = qBound(1, maxWidth / w, 8);
    return QPixmap::fromImage(img.scaled(w * scale, h * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

/* The colour cell: a swatch and the number and name. */
static void setColourCell(QTableWidget *t, int row, int col, int colour, const uint32_t *pal)
{
    setCell(t, row, col, colourText(colour));
    QPixmap sw(10, 10);
    sw.fill(fromXrgb(pal[colour & 7]));
    t->item(row, col)->setData(Qt::DecorationRole, sw);
}

/* A table as wide as its columns, so the section scrolls sideways when the
 * window is narrow instead of cutting the last columns off. */
static void fitWidth(QTableWidget *t)
{
    t->setMinimumWidth(t->horizontalHeader()->length() + 2 * t->frameWidth() + 2);
}

void VideoTab::refreshObjects()
{
    for (int i = 0; i < 4; ++i) {
        const chfdebug_uvi_object &o = m_uvi.obj[i];
        setCell(m_objects, i, 0, QString::number(i));
        setCell(m_objects, i, 1, QString());
        m_objects->item(i, 1)->setData(Qt::DecorationRole, thingPixmap(CHFDEBUG_UVI_OBJECT, i, 2));
        setCell(m_objects, i, 2, QString::number(o.x));
        setCell(m_objects, i, 3, QString::number(o.y));
        setCell(m_objects, i, 4, QStringLiteral("$%1,$%2").arg(hx(o.reg_x, 2), hx(o.reg_y, 2)));
        setColourCell(m_objects, i, 5, o.colour, m_pal);
        setCell(m_objects, i, 6, o.double_height ? QStringLiteral("8 x 16") : QStringLiteral("8 x 8"));
        setCell(m_objects, i, 7, QStringLiteral("$") + hx(o.udc, 2));
        QStringList hits;
        if (o.hits_chars) hits << QStringLiteral("C");
        for (int k = 0; k < 4; ++k)
            if ((o.hits_objects >> k) & 1) hits << QString::number(k);
        setCell(m_objects, i, 8, hits.isEmpty() ? QStringLiteral("-") : hits.join(QLatin1Char(' ')));
        setCell(m_objects, i, 9, o.visible ? QStringLiteral("visible") : QStringLiteral("off screen"));
        setCell(m_objects, i, 10, shapeText(o.shape, 8));
    }
    fitWidth(m_objects);
}

void VideoTab::refreshScreen()
{
    static chfdebug_uvi_cell cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS];
    const int rows = chfdebug_uvi_cells(m_dbg, cells);
    QColor pal[8];
    for (int i = 0; i < 8; ++i) pal[i] = fromXrgb(m_pal[i]);
    if (rows > 0) m_screen->set(m_uvi, cells, rows, pal);
    const chfdebug_uvi &u = m_uvi;
    QString t;
    t += QStringLiteral("Rows            %1 of %2 lines\n").arg(u.rows).arg(u.cell_height);
    t += QStringLiteral("Two screens     %1 ($19F8 bit 6)\n").arg(u.two_screens ? "yes" : "no");
    t += QStringLiteral("First line      %1 ($18FC)\n").arg(u.first_line);
    t += QStringLiteral("Fine scroll     %1 pixels ($18FE bits 5-7)\n").arg(u.shift);
    t += QStringLiteral("Block graphics  %1 from the top ($19F8 bit 7)\n").arg(u.block_graphics ? "on" : "off");
    t += QStringLiteral("Multicolor      %1 ($18FD bit 7: colour bits per character)\n").arg(u.multicolor ? "on" : "off");
    t += QStringLiteral("\nColours\n");
    t += QStringLiteral("  screen ($19F9)        %1\n  characters            %2\n").arg(colourText(u.screen_colour), colourText(u.char_colour));
    t += QStringLiteral("  lower screen ($19F8)  %1\n  lower characters      %2\n").arg(colourText(u.alt_screen_colour), colourText(u.alt_char_colour));
    dbgui::setText(m_screenText, t);
}

void VideoTab::refreshRegisters()
{
    m_regsFilling = true;
    for (int off = 0; off < 0x300; ++off) {
        char name[64] = "", text[200] = "";
        chfdebug_uvi_describe(m_dbg, (unsigned)off, name, sizeof name, text, sizeof text);
        setCell(m_regs, off, 1, QString::fromUtf8(name));
        const QString value = QStringLiteral("$") + hx(m_uvi.reg[off], 2);
        QTableWidgetItem *v = m_regs->item(off, 2);
        /* leave a value being typed alone */
        if (!(dbgui::editing(m_regs) && m_regs->currentRow() == off)) {
            /* a register that changed since the last look is shown in the accent */
            const char hot = !v->text().isEmpty() && v->text() != value;
            if (v->text() != value) v->setText(value);
            if (hot != m_regHot[(size_t)off]) {
                m_regHot[(size_t)off] = hot;
                v->setForeground(hot ? QBrush(chfAccentTextColor(this)) : QBrush());
            }
        }
        const QString decoded = QString::fromUtf8(text);
        setCell(m_regs, off, 3, decoded);
        if (m_regs->item(off, 3)->toolTip() != decoded) m_regs->item(off, 3)->setToolTip(decoded);
    }
    m_regsFilling = false;
}

void VideoTab::refreshControl()
{
    const chfdebug_uvi &u = m_uvi;
    auto flag = [](int v) { return v ? QStringLiteral("1") : QStringLiteral("0"); };
    QString t;
    t += QStringLiteral("Beam            line %1 of %2, character row %3, SENSE (vblank) %4\n")
             .arg(u.line).arg(u.lines).arg(u.charline).arg(flag(u.vblank));
    t += QStringLiteral("\nScreen geometry\n");
    t += QStringLiteral("  first line      %1 ($18FC)\n").arg(u.first_line);
    t += QStringLiteral("  fine scroll     %1 ($18FE bits 5-7)\n").arg(u.shift);
    t += QStringLiteral("  character rows  %1, %2 lines each\n").arg(u.rows).arg(u.cell_height);
    t += QStringLiteral("  two screens     %1 ($19F8 bit 6)\n").arg(flag(u.two_screens));
    t += QStringLiteral("  block graphics  %1 ($19F8 bit 7)\n").arg(flag(u.block_graphics));
    t += QStringLiteral("  multicolor      %1 ($18FD bit 7)\n").arg(flag(u.multicolor));
    t += QStringLiteral("\nColours ($19F8, $19F9)\n");
    t += QStringLiteral("  screen          %1\n  characters      %2\n").arg(colourText(u.screen_colour), colourText(u.char_colour));
    t += QStringLiteral("  lower screen    %1\n  lower chars     %2\n").arg(colourText(u.alt_screen_colour), colourText(u.alt_char_colour));
    t += QStringLiteral("\nSound ($18FD, $18FE)\n");
    t += QStringLiteral("  pitch divider %1, volume %2, tone %3, noise %4\n")
             .arg(u.sound_pitch).arg(u.sound_volume).arg(flag(u.sound_tone), flag(u.sound_noise));
    t += QStringLiteral("\nPots ($19FF / $19FE)  controller 1 $%1, controller 2 $%2; axis read: %3 ($19F9 bit 6)\n")
             .arg(hx(u.pot[0], 2), hx(u.pot[1], 2), u.pot_y ? "Y" : "X");
    t += QStringLiteral("Panel $1908       $%1\n").arg(hx(u.panel, 2));
    t += QStringLiteral("Collisions        characters $%1, objects $%2\n").arg(hx(u.coll_chars, 2), hx(u.coll_objects, 2));
    dbgui::setText(m_ctlText, t);
    /* a 0 bit is a hit */
    m_collChars->set(0, ~(uint32_t)u.coll_chars & 0x0F);
    m_collObjects->set(0, ~(uint32_t)u.coll_objects & 0x3F);
}

void VideoTab::refreshSheets()
{
    m_fontSheet->setPixmap(viewPixmap(CHFDEBUG_UVI_FONT, 560));
    m_shapeSheet->setPixmap(viewPixmap(CHFDEBUG_UVI_OBJECTS, 280));
    m_paletteSheet->setPixmap(viewPixmap(CHFDEBUG_UVI_PALETTE, 280));
}

/* ---- under the pointer ------------------------------------------------------------------------- */

void VideoTab::hover(int x, int y)
{
    if (x < 0) {
        m_hover->setText(QStringLiteral("Move the pointer over the picture to see what draws under it."));
        m_hover->setStyleSheet(QStringLiteral("color: gray;"));
        m_image->setHighlight(QRect());
        return;
    }
    m_hover->setStyleSheet(QString());
    chfdebug_hit hits[8];
    const int n = chfdebug_uvi_hit_test(m_dbg, x, y, hits, 8);
    QString text = QStringLiteral("x=%1 y=%2: ").arg(x).arg(y);
    QRect box;
    if (n <= 0) {
        text += QStringLiteral("nothing drawn (the border or the screen colour)");
    } else {
        const chfdebug_hit &h = hits[0];
        if (h.kind == CHFDEBUG_UVI_HIT_OBJECT && h.index >= 0 && h.index < 4) {
            const chfdebug_uvi_object &o = m_uvi.obj[h.index];
            box = QRect(o.px, o.py, o.width, o.height);
            text += QStringLiteral("Object %1 (%2 box): x=%3 y=%4 colour=%5, %6")
                        .arg(h.index).arg(QString::fromLatin1(kObjectBoxes[h.index])).arg(o.x).arg(o.y)
                        .arg(colourText(o.colour), o.double_height ? QStringLiteral("8 x 16") : QStringLiteral("8 x 8"));
        } else if (h.kind == CHFDEBUG_UVI_HIT_CELL) {
            const int row = h.index / CHFDEBUG_UVI_COLS, col = h.index % CHFDEBUG_UVI_COLS;
            box = QRect(32 + col * 8, m_uvi.first_line + row * m_uvi.cell_height, 8, m_uvi.cell_height);
            static chfdebug_uvi_cell cells[CHFDEBUG_UVI_COLS * CHFDEBUG_UVI_MAX_ROWS];
            const int rows = chfdebug_uvi_cells(m_dbg, cells);
            if (row < rows) {
                const chfdebug_uvi_cell &c = cells[h.index];
                text += QStringLiteral("Character row %1 column %2: code $%3, %4, foreground %5, background %6")
                            .arg(row).arg(col).arg(hx(c.code, 2),
                                c.graphics ? QStringLiteral("block graphics") : QStringLiteral("glyph $%1").arg(hx(c.glyph, 2)),
                                colourText(c.fg), colourText(c.bg));
            }
        } else {
            text += QStringLiteral("kind %1 index %2").arg(h.kind).arg(h.index);
        }
        if (n > 1) text += QStringLiteral(" (+%1 under it)").arg(n - 1);
    }
    m_hover->setText(text);
    m_image->setHighlight(box);
}
