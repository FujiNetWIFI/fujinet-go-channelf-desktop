/*
 * vidview.c -- the debugger's side of the Channel F's picture and ports: the
 * decoded port latches, the six video views (the frame the display shows, the
 * live VRAM, the CRT's beam, the whole VRAM, its raw values and the palettes)
 * with their overlays, the hit test and the cell readout, the VRAM write log,
 * the console's inputs and tone, and the prompt's text views of all that.
 * Pure reading: every call takes the host's run lock, and the machine is
 * never disturbed (the one write is to the debugger-only bookkeeping, to
 * bring the beam shadow up to the beam).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debugger_internal.h"

/* ---- text out ------------------------------------------------------------- */

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->len >= o->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->dst + o->len, (size_t)(o->cap - o->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += n < o->cap - o->len ? n : o->cap - o->len - 1;
}

/* ---- colours and palettes ---------------------------------------------------- */

uint32_t chfdebug_colour_rgb(int pen)
{
    return video_pen_rgb((unsigned)pen & 7);
}

const char *chfdebug_colour_name(int pen)
{
    static const char *const names[8] = { "black", "white", "red", "green", "blue", "light grey",
                                          "light green", "light blue" };
    return names[pen & 7];
}

int chfdebug_palette_pen(int palette, int value)
{
    return (int)video_palette_pen((unsigned)palette & 3, (unsigned)value & 3);
}

const char *chfdebug_palette_name(int palette)
{
    static const char *const names[4] = { "black & white", "light blue", "light grey", "light green" };
    return names[palette & 3];
}

static uint32_t cell_rgb(unsigned palette, unsigned value)
{
    return video_pen_rgb(video_palette_pen(palette, value));
}

/* ---- the ports ----------------------------------------------------------------- */

void chfdebug_video_get(chfdebug *d, chfdebug_video *o)
{
    chf_machine_t *m;

    (void)d;
    memset(o, 0, sizeof *o);
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const cf_video_t *v = &m->vid;

        o->port0 = v->latch[0];
        o->port1 = v->latch[1];
        o->port4 = v->latch[2];
        o->port5 = v->latch[3];
        o->value = (~o->port1 >> 6) & 3;
        o->col = ~o->port4 & 0x7f;
        o->row = ~o->port5 & 0x3f;
        o->sound_mode = o->port5 >> 6;
        o->strobe = (o->port0 & 0x20) != 0;
        o->controllers_enabled = (o->port0 & 0x40) == 0;
        o->panel_latch = o->port0 & 0x0f;
        o->in0 = chf_machine_port_read(m, 0);
        o->in1 = chf_machine_port_read(m, 1);
        o->in4 = chf_machine_port_read(m, 4);
        o->in5 = chf_machine_port_read(m, 5);
        for (int r = 0; r < CHFDEBUG_VRAM_ROWS; r++)
        {
            const unsigned p = video_row_palette(v, (unsigned)r);
            o->row_palette[r] = (uint8_t)p;
            o->row_background[r] = cell_rgb(p, 0);
            o->palette_rows[p]++;
        }
        chf_machine_beam(m, &o->line, &o->cycle_in_line);
        o->lines = m->lines;
        o->cycles_per_line = (int)(m->scan_as / m->cycle_as);
        o->frame = (uint32_t)m->frames_done;
        o->is_pal = m->is_pal;
        if (m->track)
        {
            o->strobes_frame = m->track->strobes_frame;
            o->strobes_last = m->track->strobes_last;
            o->changes_last = m->track->changes_last;
        }
    }
    chf_host_unlock();
}

/* ---- the VRAM log -------------------------------------------------------------- */

void chfdebug_vram_log_fix_pc(chf_machine_t *m, unsigned offs, uint16_t pc)
{
    chf_vtrack_t *t = m->track;

    if (!t || !t->total || offs >= CF_VRAM_SIZE)
        return;
    {
        chf_vwrite_t *w = &t->ring[(t->total - 1) & (CHF_VWRITE_RING - 1)];
        if ((unsigned)w->row * CF_VRAM_COLS + w->col == offs)
            w->pc = pc;
    }
    if (t->cell[offs].pc >= 0)
        t->cell[offs].pc = pc;
}

int chfdebug_vram_writes(chfdebug *d, const chfdebug_vram_filter *f, chfdebug_vram_write *out_,
                         int max)
{
    chf_machine_t *m;
    int n = 0;

    (void)d;
    if (!out_ || max <= 0)
        return 0;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && m->track)
    {
        const chf_vtrack_t *t = m->track;
        const uint64_t have = t->total < CHF_VWRITE_RING ? t->total : CHF_VWRITE_RING;
        const uint32_t now = (uint32_t)m->frames_done;

        /* newest to oldest, until max match; then the order is flipped */
        for (uint64_t i = 0; i < have && n < max; i++)
        {
            const chf_vwrite_t *w = &t->ring[(t->total - 1 - i) & (CHF_VWRITE_RING - 1)];
            if (f)
            {
                if (w->row < f->row0 || w->row > f->row1 || w->col < f->col0 || w->col > f->col1)
                    continue;
                if (f->changes_only && w->value == w->old)
                    continue;
                if (f->this_frame && w->frame != now)
                    continue;
                if (f->palette_cols && w->col < CHFDEBUG_PAL_COL_A)
                    continue;
            }
            out_[n].cycle = w->cycle;
            out_[n].frame = w->frame;
            out_[n].line = w->line;
            out_[n].pc = w->pc;
            out_[n].row = w->row;
            out_[n].col = w->col;
            out_[n].value = w->value;
            out_[n].old = w->old;
            n++;
        }
        for (int i = 0; i < n / 2; i++)
        {
            chfdebug_vram_write tmp = out_[i];
            out_[i] = out_[n - 1 - i];
            out_[n - 1 - i] = tmp;
        }
    }
    chf_host_unlock();
    return n;
}

void chfdebug_vram_writes_clear(chfdebug *d)
{
    chf_machine_t *m;

    (void)d;
    chf_host_lock();
    m = chfdebug_machine();
    if (m && m->track)
        m->track->total = 0;
    chf_host_unlock();
}

int chfdebug_vram_cell_get(chfdebug *d, int row, int col, chfdebug_vram_cell *o)
{
    chf_machine_t *m;
    int rc = -1;

    (void)d;
    memset(o, 0, sizeof *o);
    o->last_pc = -1;
    if (row < 0 || row >= CHFDEBUG_VRAM_ROWS || col < 0 || col >= CHFDEBUG_VRAM_COLS)
        return -1;
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const unsigned offs = (unsigned)row * CF_VRAM_COLS + (unsigned)col;

        o->row = row;
        o->col = col;
        o->visible = col >= CHFDEBUG_VIS_COL0 && col < CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS &&
                     row >= CHFDEBUG_VIS_ROW0 && row < CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS;
        if (col == CHFDEBUG_PAL_COL_A || col == CHFDEBUG_PAL_COL_B)
            o->hidden_why = "palette column";
        else if (col < CHFDEBUG_VIS_COL0)
            o->hidden_why = "left border";
        else if (col >= CHFDEBUG_VIS_COL0 + CHFDEBUG_VIS_COLS)
            o->hidden_why = "right border";
        else if (row < CHFDEBUG_VIS_ROW0)
            o->hidden_why = "top border";
        else if (row >= CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS)
            o->hidden_why = "bottom border";
        o->value = m->vid.vram[offs] & 3;
        o->palette = (int)video_row_palette(&m->vid, (unsigned)row);
        o->pen = chfdebug_palette_pen(o->palette, o->value);
        o->rgb = chfdebug_colour_rgb(o->pen);
        if (m->track)
        {
            const chf_vtrack_t *t = m->track;
            o->writes = t->cell[offs].writes;
            o->last_pc = t->cell[offs].pc;
            o->last_frame = t->cell[offs].frame;
            o->last_line = t->cell[offs].line;
            o->last_cycle = t->cell[offs].cycle;
            o->last_old = t->cell[offs].old;
        }
        rc = 0;
    }
    chf_host_unlock();
    return rc;
}

/* ---- the views --------------------------------------------------------------------- */

#define SW_W 64                 /* the palette swatches */
#define SW_H 32

static uint32_t g_base[CHFDEBUG_VIEW_MAX_PIXELS];

static void base_size(int view, int *w, int *h)
{
    switch (view)
    {
    case CHFDEBUG_VIEW_VRAM:
    case CHFDEBUG_VIEW_VALUES:
        *w = CHFDEBUG_VRAM_COLS * CF_CELL_W;
        *h = CHFDEBUG_VRAM_ROWS * CF_CELL_H;
        break;
    case CHFDEBUG_VIEW_PALETTES:
        *w = 4 * SW_W;
        *h = 4 * SW_H;
        break;
    default:
        *w = CF_FRAME_W;
        *h = CF_FRAME_H;
        break;
    }
}

static int is_picture(int view)
{
    return view == CHFDEBUG_VIEW_FRAME || view == CHFDEBUG_VIEW_LIVE || view == CHFDEBUG_VIEW_BEAM;
}

static int is_whole(int view)
{
    return view == CHFDEBUG_VIEW_VRAM || view == CHFDEBUG_VIEW_VALUES;
}

/* The base-pixel rectangle of a VRAM cell in a view; 0 if the view does not show it. */
static int cell_rect(int view, int row, int col, int *x, int *y)
{
    if (is_whole(view))
    {
        *x = col * CF_CELL_W;
        *y = row * CF_CELL_H;
        return 1;
    }
    if (is_picture(view))
    {
        if (col < CF_VIS_COL0 || col >= CF_VIS_COL0 + CF_VIS_COLS || row < CF_VIS_ROW0 ||
            row >= CF_VIS_ROW0 + CF_VIS_ROWS)
            return 0;
        *x = (col - CF_VIS_COL0) * CF_CELL_W;
        *y = (row - CF_VIS_ROW0) * CF_CELL_H;
        return 1;
    }
    return 0;
}

static uint32_t blend(uint32_t base, uint32_t over, unsigned alpha)   /* alpha 0-255 */
{
    uint32_t r = 0;

    for (int sh = 0; sh <= 16; sh += 8)
    {
        const unsigned b = (base >> sh) & 0xff, o = (over >> sh) & 0xff;
        r |= (uint32_t)((b * (255 - alpha) + o * alpha) / 255) << sh;
    }
    return r;
}

static uint32_t dim(uint32_t c)
{
    return blend(c, 0x000000, 150);
}

/* Bring the CRT shadow up to the beam: the rows it has drawn since the last
 * blank are copied from the VRAM as it is now (the same as machine.c does at a
 * store, which copies them before it changes the cell). Returns the rows the
 * beam has passed. */
static int beam_sync(chf_machine_t *m)
{
    chf_vtrack_t *t = m->track;
    int line, rows;

    chf_machine_beam(m, &line, NULL);
    rows = line >= m->vbstart ? 0 : line / m->y_rpt + 1;
    if (rows > CF_VRAM_ROWS)
        rows = CF_VRAM_ROWS;
    if (t && rows > t->beam_rows)
    {
        memcpy(t->beam + (size_t)t->beam_rows * CF_VRAM_COLS,
               m->vid.vram + (size_t)t->beam_rows * CF_VRAM_COLS,
               (size_t)(rows - t->beam_rows) * CF_VRAM_COLS);
        t->beam_rows = rows;
    }
    return t ? t->beam_rows : rows;
}

/* the whole VRAM, 2 x 4 pixels a cell; values false-coloured, or by palette */
static void draw_whole(const uint8_t *vram, int values, uint32_t *dst)
{
    static const uint32_t vcol[4] = { 0x303030, 0x3a6df0, 0xe03030, 0x30c050 };
    const int w = CHFDEBUG_VRAM_COLS * CF_CELL_W;

    for (int r = 0; r < CF_VRAM_ROWS; r++)
    {
        unsigned pal = (vram[r * CF_VRAM_COLS + CF_PAL_COL_B] & 2) | ((vram[r * CF_VRAM_COLS + CF_PAL_COL_A] & 3) >> 1);
        for (int c = 0; c < CF_VRAM_COLS; c++)
        {
            const unsigned v = vram[r * CF_VRAM_COLS + c] & 3;
            const uint32_t rgb = values ? vcol[v] : cell_rgb(pal, v);
            const int hidden = c < CF_VIS_COL0 || c >= CF_VIS_COL0 + CF_VIS_COLS || r < CF_VIS_ROW0 ||
                               r >= CF_VIS_ROW0 + CF_VIS_ROWS;

            for (int y = 0; y < CF_CELL_H; y++)
                for (int x = 0; x < CF_CELL_W; x++)
                {
                    const int px = c * CF_CELL_W + x, py = r * CF_CELL_H + y;
                    uint32_t p = rgb;
                    if (hidden)
                        p = blend(rgb, 0x000000, ((px + py) & 3) < 2 ? 140 : 40);
                    dst[py * w + px] = p;
                }
        }
    }
}

static void draw_palettes(uint32_t *dst)
{
    const int w = 4 * SW_W;

    for (int p = 0; p < 4; p++)
        for (int v = 0; v < 4; v++)
            for (int y = 0; y < SW_H; y++)
                for (int x = 0; x < SW_W; x++)
                    dst[(p * SW_H + y) * w + v * SW_W + x] =
                        (x < SW_W - 2 && y < SW_H - 2) ? cell_rgb((unsigned)p, (unsigned)v) : 0x202020;
}

static void tint_cell(uint32_t *base, int bw, int view, int row, int col, uint32_t rgb, unsigned alpha)
{
    int x, y;

    if (!cell_rect(view, row, col, &x, &y))
        return;
    for (int j = 0; j < CF_CELL_H; j++)
        for (int i = 0; i < CF_CELL_W; i++)
            base[(y + j) * bw + x + i] = blend(base[(y + j) * bw + x + i], rgb, alpha);
}

static unsigned row_pal(const uint8_t *vram, int r)
{
    return (unsigned)((vram[r * CF_VRAM_COLS + CF_PAL_COL_B] & 2) | ((vram[r * CF_VRAM_COLS + CF_PAL_COL_A] & 3) >> 1));
}

/* a one-pixel line of the final image */
static void put(uint32_t *dst, int w, int h, int x, int y, uint32_t rgb)
{
    if (x >= 0 && y >= 0 && x < w && y < h)
        dst[y * w + x] = rgb;
}

static void hline(uint32_t *dst, int w, int h, int x0, int x1, int y, uint32_t rgb)
{
    for (int x = x0; x <= x1; x++)
        put(dst, w, h, x, y, rgb);
}

static void vline(uint32_t *dst, int w, int h, int x, int y0, int y1, uint32_t rgb)
{
    for (int y = y0; y <= y1; y++)
        put(dst, w, h, x, y, rgb);
}

static void rect(uint32_t *dst, int w, int h, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    hline(dst, w, h, x0, x1, y0, rgb);
    hline(dst, w, h, x0, x1, y1, rgb);
    vline(dst, w, h, x0, y0, y1, rgb);
    vline(dst, w, h, x1, y0, y1, rgb);
}

int chfdebug_video_view(chfdebug *d, int view, unsigned flags, int scale, uint32_t *dst, size_t cap,
                        int *w, int *h)
{
    chf_machine_t *m;
    int bw, bh, ow, oh, ok = 0;

    (void)d;
    if (view < CHFDEBUG_VIEW_FRAME || view > CHFDEBUG_VIEW_PALETTES)
        return 0;
    if (scale < 1)
        scale = 1;
    base_size(view, &bw, &bh);
    ow = bw * scale;
    oh = bh * scale;
    if (w)
        *w = ow;
    if (h)
        *h = oh;
    if (!dst)
        return 1;
    if (cap < (size_t)ow * (size_t)oh)
        return 0;

    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const chf_vtrack_t *t = m->track;
        const uint8_t *shown = m->vid.vram;      /* what the view is of */
        const uint8_t *ref = t ? t->frame_vram : NULL;
        uint32_t *base = g_base;
        int rows_beam = 0;

        switch (view)
        {
        case CHFDEBUG_VIEW_FRAME:
            memcpy(base, m->fb, (size_t)bw * (size_t)bh * sizeof *base);
            shown = t ? t->frame_vram : m->vid.vram;
            ref = t ? t->prev_vram : NULL;
            break;
        case CHFDEBUG_VIEW_LIVE:
            video_render(&m->vid, base);
            break;
        case CHFDEBUG_VIEW_BEAM:
            if (t)
            {
                cf_video_t tmp;

                rows_beam = beam_sync(m);
                memcpy(tmp.vram, t->beam, sizeof tmp.vram);
                video_render(&tmp, base);
                /* what the beam has not drawn yet is last frame's, dimmed */
                for (int r = 0; r < CF_VIS_ROWS; r++)
                    if (CF_VIS_ROW0 + r >= rows_beam)
                        for (int y = 0; y < CF_CELL_H; y++)
                            for (int x = 0; x < bw; x++)
                                base[(r * CF_CELL_H + y) * bw + x] = dim(base[(r * CF_CELL_H + y) * bw + x]);
            }
            else
                video_render(&m->vid, base);
            break;
        case CHFDEBUG_VIEW_VRAM:
            draw_whole(m->vid.vram, 0, base);
            break;
        case CHFDEBUG_VIEW_VALUES:
            draw_whole(m->vid.vram, 1, base);
            break;
        default:
            draw_palettes(base);
            break;
        }

        if (is_picture(view) || is_whole(view))
        {
            /* cell tints */
            if ((flags & CHFDEBUG_F_HEAT) && t)
            {
                const uint8_t *heat = view == CHFDEBUG_VIEW_FRAME ? t->heat_last : t->heat;
                for (int r = 0; r < CF_VRAM_ROWS; r++)
                    for (int c = 0; c < CF_VRAM_COLS; c++)
                    {
                        const unsigned n = heat[r * CF_VRAM_COLS + c];
                        if (n)
                            tint_cell(base, bw, view, r, c, 0xff6a00, 110 + (n > 4 ? 4 : n) * 36);
                    }
            }
            if ((flags & CHFDEBUG_F_DIRTY) && ref)
            {
                for (int r = 0; r < CF_VRAM_ROWS; r++)
                    for (int c = 0; c < CF_VRAM_COLS; c++)
                        if (shown[r * CF_VRAM_COLS + c] != ref[r * CF_VRAM_COLS + c])
                            tint_cell(base, bw, view, r, c, 0xff00ff, 150);
            }
        }

        /* scale up */
        for (int y = 0; y < oh; y++)
        {
            const uint32_t *src = base + (y / scale) * bw;
            uint32_t *line = dst + (size_t)y * (size_t)ow;
            if (scale == 1)
                memcpy(line, src, (size_t)bw * sizeof *line);
            else
                for (int x = 0; x < ow; x++)
                    line[x] = src[x / scale];
        }

        if (is_picture(view) || is_whole(view))
        {
            const uint32_t accent = CHFSESSION_ACCENT_RGB;
            const int whole = is_whole(view);
            const int y0 = whole ? 0 : -CF_VIS_ROW0 * CF_CELL_H;

            if ((flags & CHFDEBUG_F_GRID) && scale >= 2)
            {
                for (int x = 0; x < ow; x += CF_CELL_W * scale)
                    for (int y = 0; y < oh; y++)
                        dst[(size_t)y * (size_t)ow + x] = blend(dst[(size_t)y * (size_t)ow + x], 0x808080, 110);
                for (int y = 0; y < oh; y += CF_CELL_H * scale)
                    for (int x = 0; x < ow; x++)
                        dst[(size_t)y * (size_t)ow + x] = blend(dst[(size_t)y * (size_t)ow + x], 0x808080, 110);
            }
            if ((flags & CHFDEBUG_F_VISIBLE) && whole)
                rect(dst, ow, oh, (CF_VIS_COL0 * CF_CELL_W) * scale - 1, (CF_VIS_ROW0 * CF_CELL_H) * scale - 1,
                     ((CF_VIS_COL0 + CF_VIS_COLS) * CF_CELL_W) * scale,
                     ((CF_VIS_ROW0 + CF_VIS_ROWS) * CF_CELL_H) * scale, accent);
            if ((flags & CHFDEBUG_F_PALCOLS) && whole)
            {
                vline(dst, ow, oh, (CF_PAL_COL_A * CF_CELL_W) * scale - 1, 0, oh - 1, 0x00e5ff);
                vline(dst, ow, oh, ((CF_PAL_COL_B + 1) * CF_CELL_W) * scale - 1, 0, oh - 1, 0x00e5ff);
            }
            if (flags & CHFDEBUG_F_PALROWS)
            {
                for (int r = 0; r < CF_VRAM_ROWS; r++)
                {
                    int x, y;
                    if (ref && row_pal(shown, r) != row_pal(ref, r) && cell_rect(view, r, whole ? 0 : CF_VIS_COL0, &x, &y))
                        for (int j = 0; j < CF_CELL_H * scale; j++)
                            for (int i = 0; i < 3 * scale && i < ow; i++)
                                put(dst, ow, oh, i, y * scale + j, accent);
                }
            }
            if (flags & CHFDEBUG_F_BEAM)
            {
                int line;
                chf_machine_beam(m, &line, NULL);
                if (line < m->vbstart)
                {
                    const int by = (line * CF_CELL_H / m->y_rpt + y0) * scale;
                    if (by >= 0 && by < oh)
                        hline(dst, ow, oh, 0, ow - 1, by, 0xff2020);
                }
            }
            if (flags & CHFDEBUG_F_CURSOR)
            {
                int cx, cy;
                if (cell_rect(view, m->vid.row_reg, m->vid.col_reg, &cx, &cy))
                    rect(dst, ow, oh, cx * scale, cy * scale, (cx + CF_CELL_W) * scale - 1,
                         (cy + CF_CELL_H) * scale - 1, 0x00e5ff);
            }
        }
        ok = 1;
    }
    chf_host_unlock();
    return ok;
}

int chfdebug_video_hit(chfdebug *d, int view, int x, int y, int scale, int *row, int *col)
{
    int r, c;

    (void)d;
    if (scale < 1)
        scale = 1;
    if (x < 0 || y < 0)
        return -1;
    x /= scale;
    y /= scale;
    if (is_whole(view))
    {
        c = x / CF_CELL_W;
        r = y / CF_CELL_H;
        if (c >= CF_VRAM_COLS || r >= CF_VRAM_ROWS)
            return -1;
    }
    else if (is_picture(view))
    {
        if (x >= CF_FRAME_W || y >= CF_FRAME_H)
            return -1;
        c = CF_VIS_COL0 + x / CF_CELL_W;
        r = CF_VIS_ROW0 + y / CF_CELL_H;
    }
    else
        return -1;
    if (row)
        *row = r;
    if (col)
        *col = c;
    return r * CF_VRAM_COLS + c;
}

/* ---- inputs and the tone ---------------------------------------------------------------- */

void chfdebug_io_get(chfdebug *d, chfdebug_io *o)
{
    chf_machine_t *m;

    (void)d;
    memset(o, 0, sizeof *o);
    chf_host_lock();
    m = chfdebug_machine();
    if (m)
    {
        const chf_cart_t *c = m->cart;

        o->machine = m->machine;
        o->is_pal = m->is_pal;
        o->controller[0] = m->ctl[0];
        o->controller[1] = m->ctl[1];
        o->panel = (uint8_t)m->panel;
        o->reset_held = m->reset_held;
        o->sound_mode = m->snd.mode;
        o->sound_level = m->snd.envelope;
        o->sound_forced = m->snd.forced_ontime > 0;
        o->sound_changes = m->snd.changes;
        o->audio_samples = m->audio_samples;
        o->cart_type = m->fujinet ? CHF_CART_FUJINET : c ? c->type : 0;
        if (c && !m->fujinet && (c->type == CHF_CART_MAZE || c->type == CHF_CART_HANGMAN))
        {
            o->has_2102 = 1;
            o->ram_port = c->type == CHF_CART_MAZE ? 0x24 : 0x20;
            o->ram_addr = c->addr_latch & 0x3ff;
            o->ram_bit = c->ram[c->addr_latch & 0x3ff] & 1;
        }
    }
    chf_host_unlock();
}

/* ---- the prompt's text views ---------------------------------------------------------------- */

static const char *sound_name(int mode)
{
    static const char *const names[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };
    return names[mode & 3];
}

int chfdebug_text_ports(char *dst, int dstsz)
{
    chfdebug_video v;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chfdebug_video_get(NULL, &v);
    out(&o, "port 0 PANEL     out %02X  in %02X   strobe %d  controllers %s  panel latch %X\n",
        v.port0, v.in0, v.strobe, v.controllers_enabled ? "read" : "off", v.panel_latch);
    out(&o, "port 1 RIGHT_C   out %02X  in %02X   colour value %d (~port1 >> 6)\n", v.port1, v.in1, v.value);
    out(&o, "port 4 LEFT_C    out %02X  in %02X   column %d (~port4 & $7F)\n", v.port4, v.in4, v.col);
    out(&o, "port 5 ROW_SOUND out %02X  in %02X   row %d (~port5 & $3F)  sound %s\n", v.port5, v.in5,
        v.row, sound_name(v.sound_mode));
    out(&o, "next strobe writes value %d at row %d column %d\n", v.value, v.row, v.col);
    return o.len;
}

int chfdebug_text_video(char *dst, int dstsz)
{
    chfdebug_video v;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chfdebug_video_get(NULL, &v);
    out(&o, "beam: line %d of %d, cycle %d of %d  frame %u  (%s)\n", v.line, v.lines, v.cycle_in_line,
        v.cycles_per_line, v.frame, v.is_pal ? "PAL" : "NTSC");
    out(&o, "VRAM strobes: this frame %u, last frame %u (%u changed a cell)\n", v.strobes_frame,
        v.strobes_last, v.changes_last);
    out(&o, "next strobe: value %d at row %d column %d%s\n", v.value, v.row, v.col,
        v.strobe ? "  (strobe up now)" : "");
    out(&o, "rows by palette: %d black & white, %d light blue, %d light grey, %d light green\n",
        v.palette_rows[0], v.palette_rows[1], v.palette_rows[2], v.palette_rows[3]);
    out(&o, "visible: columns 4-105, rows 4-61 of 128 x 64; palette columns 125 and 126\n");
    return o.len;
}

int chfdebug_text_pal(char *dst, int dstsz)
{
    chfdebug_video v;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chfdebug_video_get(NULL, &v);
    out(&o, "palette = (v[126] & 2) | (v[125] >> 1)\n");
    for (int p = 0; p < 4; p++)
    {
        out(&o, "%d %-14s rows %2d :", p, chfdebug_palette_name(p), v.palette_rows[p]);
        for (int val = 0; val < 4; val++)
        {
            const int pen = chfdebug_palette_pen(p, val);
            out(&o, "  %d=%s #%06X", val, chfdebug_colour_name(pen), chfdebug_colour_rgb(pen));
        }
        out(&o, "\n");
    }
    return o.len;
}

int chfdebug_text_vram(int row, char *dst, int dstsz)
{
    chf_machine_t *m;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chf_host_lock();
    m = chfdebug_machine();
    if (!m)
        out(&o, "the machine is not running\n");
    else if (row >= 0)
    {
        if (row >= CF_VRAM_ROWS)
            out(&o, "row 0-63\n");
        else
        {
            const unsigned pal = video_row_palette(&m->vid, (unsigned)row);
            out(&o, "row %d: palette %u (%s)  c125=%d c126=%d\n", row, pal, chfdebug_palette_name((int)pal),
                m->vid.vram[row * CF_VRAM_COLS + CF_PAL_COL_A], m->vid.vram[row * CF_VRAM_COLS + CF_PAL_COL_B]);
            for (int c = 0; c < CF_VRAM_COLS; c++)
                out(&o, "%s%d", c && c % 8 == 0 ? " " : "", m->vid.vram[row * CF_VRAM_COLS + c]);
            out(&o, "\n");
        }
    }
    else
    {
        out(&o, "visible picture, columns 4-105 (. = 0)\n");
        for (int r = CF_VIS_ROW0; r < CF_VIS_ROW0 + CF_VIS_ROWS; r++)
        {
            out(&o, "%2d ", r);
            for (int c = CF_VIS_COL0; c < CF_VIS_COL0 + CF_VIS_COLS; c++)
            {
                const int v = m->vid.vram[r * CF_VRAM_COLS + c];
                out(&o, "%c", v ? '0' + v : '.');
            }
            out(&o, "  p%u\n", video_row_palette(&m->vid, (unsigned)r));
        }
    }
    chf_host_unlock();
    return o.len;
}

int chfdebug_text_scratch(char *dst, int dstsz)
{
    chfdebug_cpu c;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chfdebug_cpu_get(NULL, &c);
    out(&o, "        +0   +1   +2   +3   +4   +5   +6   +7\n");
    for (int r = 0; r < 64; r += 8)
    {
        out(&o, "r%-2d o%02o ", r, r);
        for (int i = 0; i < 8; i++)
            out(&o, r + i == c.isar ? "[%02X] " : " %02X  ", c.r[r + i]);
        out(&o, "\n");
    }
    out(&o, "J = r9   H = r10:r11   K = r12:r13   Q = r14:r15   ISAR o%02o = r%d\n", c.isar, c.isar);
    return o.len;
}

int chfdebug_text_kstack(char *dst, int dstsz)
{
    chfdebug_cpu c;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chfdebug_cpu_get(NULL, &c);
    if (c.k_depth < 0)
    {
        out(&o, "r59 = %d is not a pointer into the K stack (an even value from 40 to 58)\n", c.r[59]);
        return o.len;
    }
    out(&o, "K stack: depth %d  (r59 = %d; two bytes an entry from r40 up)\n", c.k_depth, c.r[59]);
    for (int i = c.k_depth - 1; i >= 0; i--)
        out(&o, "  [%d] r%d:r%d = %04X\n", i, 40 + 2 * i, 41 + 2 * i, c.k_stack[i]);
    if (!c.k_depth)
        out(&o, "  (empty)\n");
    out(&o, "K = %04X  Q = %04X  PC1 = %04X\n", c.j_h_k_q[2], c.j_h_k_q[3], c.pc1);
    return o.len;
}

int chfdebug_text_sound(chfdebug *d, char *dst, int dstsz)
{
    chfdebug_io io;
    chfdebug_sound_event ev[8];
    outbuf o = { dst, dstsz, 0 };
    int n;

    dst[0] = '\0';
    chfdebug_io_get(d, &io);
    out(&o, "tone: %s%s  envelope %d/32767  changes %u  samples %llu\n", sound_name(io.sound_mode),
        io.sound_forced ? " (forced on)" : "", io.sound_level, io.sound_changes,
        (unsigned long long)io.audio_samples);
    n = chfdebug_sound_events(d, ev, 8);
    for (int i = 0; i < n; i++)
        out(&o, "  frame %u  pc %04X  %s -> %s\n", ev[i].frame, ev[i].pc, sound_name(ev[i].old_mode),
            sound_name(ev[i].mode));
    return o.len;
}

int chfdebug_text_input(char *dst, int dstsz)
{
    static const char *const act[8] = { "right", "left", "back", "forward", "ccw", "cw", "pull", "push" };
    static const char *const btn[4] = { "1 TIME", "2 MODE", "3 HOLD", "4 START" };
    chfdebug_io io;
    outbuf o = { dst, dstsz, 0 };

    dst[0] = '\0';
    chfdebug_io_get(NULL, &io);
    for (int p = 0; p < 2; p++)
    {
        out(&o, "%s controller (port %d) %02X:", p ? "left (player 2)" : "right (player 1)", p ? 4 : 1,
            io.controller[p]);
        for (int b = 0; b < 8; b++)
            if (io.controller[p] & (1 << b))
                out(&o, " %s", act[b]);
        out(&o, "%s\n", io.controller[p] ? "" : " (none)");
    }
    out(&o, "console buttons %X:", io.panel);
    for (int b = 0; b < 4; b++)
        if (io.panel & (1 << b))
            out(&o, " %s", btn[b]);
    out(&o, "%s   RESET %s\n", io.panel ? "" : " (none)", io.reset_held ? "held" : "up");
    if (io.has_2102)
        out(&o, "2102 RAM on ports $%02X/$%02X: address %03X, bit %d\n", io.ram_port, io.ram_port + 1,
            io.ram_addr, io.ram_bit);
    return o.len;
}

int chfdebug_text_bios(char *dst, int dstsz)
{
    chf_machine_t *m;
    outbuf o = { dst, dstsz, 0 };
    char why[128];

    dst[0] = '\0';
    chf_host_lock();
    m = chfdebug_machine();
    if (!m)
        out(&o, "the machine is not running\n");
    else
    {
        out(&o, "%s\n", chf_machine_name(m->machine));
        if (m->have_bios)
            out(&o, "BIOS: imported, %s first half (CRC %08X) + second half (CRC %08X)\n",
                m->bios_id == CHF_ROM_SL90025 ? "sl90025" : "sl31253",
                chf_crc32(m->rom, CF_BIOS_HALF), chf_crc32(m->rom + CF_BIOS_HALF, CF_BIOS_HALF));
        else
            out(&o, "BIOS: the app's stand-in (RESET, CLRSCRN, PUSHK and POPK only)\n");
        if (chf_machine_missing(m, why, sizeof why))
            out(&o, "parked: %s\n", why);
    }
    chf_host_unlock();
    return o.len;
}
