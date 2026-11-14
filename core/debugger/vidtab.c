/*
 * vidtab.c -- the Video tab's controller (core/include/chfvidtab.h), over the
 * engine's public video API: the picture is chfdebug_video_view() rebuilt
 * when something it shows changed, the tables are the machine's VRAM, its
 * palette columns, its port latches, its write log and its port log.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "chfvidtab.h"

#define CELL_W 2                /* a VRAM cell in the pictures, base pixels */
#define CELL_H 4
#define VRAM_BYTES (CHFDEBUG_VRAM_COLS * CHFDEBUG_VRAM_ROWS)
#define MAXPIX 17000000L        /* the largest picture we will build */
#define MAX_REGS 8
#define MAX_TIMING 12
#define LOG_N 16384             /* the machine's whole VRAM write ring */
#define IO_N 4096
#define LINE_ROWS CHFDEBUG_VRAM_ROWS
#define PAL_N 4

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

struct chfvid_tab {
    chfdebug *dbg;
    int view, zoom, vieww, viewh;
    unsigned ovl;
    int env_section;

    /* the machine as last read */
    int have, stopped;
    chfdebug_video vid;
    uint8_t vram[VRAM_BYTES];
    int y_rpt, vbstart, vbend;
    uint64_t sig, table_sig;
    int dirty;

    /* the picture */
    uint32_t *pic;
    size_t pic_cap;
    int picw, pich, effzoom;
    char why[200];

    /* tables */
    chfvid_reg regs[MAX_REGS];
    char prev_val[MAX_REGS][24];
    int nregs, regs_primed;
    chfvid_swatch swat[PAL_N * 4];
    chfvid_line_row rows[LINE_ROWS];
    chfvid_write_filter wf;
    chfdebug_vram_write *wlist;
    int nwrites;
    chfdebug_vram_write *wlog;      /* scratch: the log for the Lines table */
    chfdebug_io_event *ioev;        /* scratch: the port log */
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;

    /* cells */
    int hovr, hovc, pinr, pinc;     /* -1: none */
    chfvid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const chfvid_overlay_info k_overlays[] = {
    { CHFVID_OV_VISIBLE, "visible", "Visible area outline" },
    { CHFVID_OV_GRID,    "grid",    "Cell grid" },
    { CHFVID_OV_PALCOLS, "palcols", "Palette columns" },
    { CHFVID_OV_BEAM,    "beam",    "Beam line" },
    { CHFVID_OV_CURSOR,  "cursor",  "Next strobe's cell" },
    { CHFVID_OV_HEAT,    "heat",    "Written this frame" },
    { CHFVID_OV_DIRTY,   "dirty",   "Changed since last frame" },
    { CHFVID_OV_PALROWS, "palrows", "Palette changes" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const chfvid_overlay_info *chfvid_overlays(int *count)
{
    if (count)
        *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values", "Live" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values", "live" };
static const int k_view_kind[] = { CHFDEBUG_VIEW_FRAME, CHFDEBUG_VIEW_VRAM, CHFDEBUG_VIEW_BEAM,
                                   CHFDEBUG_VIEW_VALUES, CHFDEBUG_VIEW_LIVE };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Writes", "Timing" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "writes", "timing" };

const char *chfvid_view_name(int view) { return view >= 0 && view < CHFVID_V_COUNT ? k_view_names[view] : ""; }
const char *chfvid_view_key(int view) { return view >= 0 && view < CHFVID_V_COUNT ? k_view_keys[view] : ""; }
const char *chfvid_section_name(int sec) { return sec >= 0 && sec < CHFVID_S_COUNT ? k_sec_names[sec] : ""; }
const char *chfvid_section_key(int sec) { return sec >= 0 && sec < CHFVID_S_COUNT ? k_sec_keys[sec] : ""; }

int chfvid_section_from_name(const char *name)
{
    if (!name || !*name)
        return -1;
    for (int sec = 0; sec < CHFVID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name)))
            return sec;
    return -1;
}

const int *chfvid_zooms(int *count)
{
    if (count)
        *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *src = data;

    while (len--)
    {
        hash ^= *src++;
        hash *= 1099511628211ull;
    }
    return hash;
}

static void why_set(chfvid_tab *tab, const char *text)
{
    snprintf(tab->why, sizeof tab->why, "%s", text);
}

static int whole_view(int view) { return view == CHFVID_V_DECODED || view == CHFVID_V_VALUES; }

/* ---- life --------------------------------------------------------------------------------- */

chfvid_tab *chfvid_tab_new(chfdebug *dbg)
{
    chfvid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;

    if (!tab)
        return NULL;
    tab->dbg = dbg;
    tab->view = CHFVID_V_DISPLAY;
    tab->ovl = CHFVID_OV_VISIBLE | CHFVID_OV_BEAM;
    tab->zoom = 0;
    tab->vieww = 204 * 2;
    tab->viewh = 232 * 2;
    tab->hovr = tab->hovc = tab->pinr = tab->pinc = -1;
    tab->dirty = 1;

    if ((envv = getenv("CHF_DEBUGGER_VIDEO_VIEW")) && *envv)
    {
        for (int idx = 0; idx < CHFVID_V_COUNT; idx++)
            if (!strncasecmp(envv, k_view_keys[idx], strlen(envv)))
            {
                tab->view = idx;
                break;
            }
    }
    if ((envv = getenv("CHF_DEBUGGER_VIDEO_OVERLAYS")))
    {
        unsigned ovl = 0;
        const char *cur = envv;

        while (*cur)
        {
            char word[32];
            size_t len = 0;

            while (*cur == ',' || *cur == ' ')
                cur++;
            while (*cur && *cur != ',' && *cur != ' ' && len < sizeof word - 1)
                word[len++] = *cur++;
            word[len] = '\0';
            if (!len)
                continue;
            if (!strcasecmp(word, "all"))
                ovl |= CHFVID_OV_ALL;
            else if (!strcasecmp(word, "none"))
                ovl = 0;
            else
                for (int idx = 0; idx < N_OVERLAYS; idx++)
                    if (!strcasecmp(word, k_overlays[idx].name))
                        ovl |= k_overlays[idx].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("CHF_DEBUGGER_VIDEO_ZOOM")) && *envv)
    {
        if (!strcasecmp(envv, "fit"))
            tab->zoom = 0;
        else
        {
            const int want = atoi(envv);

            for (size_t idx = 1; idx < sizeof k_zooms / sizeof *k_zooms; idx++)
                if (k_zooms[idx] == want)
                    tab->zoom = want;
        }
    }
    tab->env_section = CHFVID_S_PIXEL;
    if ((envv = getenv("CHF_DEBUGGER_VIDEO_SECTION")) && *envv)
    {
        const int sec = chfvid_section_from_name(envv);

        if (sec >= 0)
            tab->env_section = sec;
    }
    return tab;
}

void chfvid_tab_free(chfvid_tab *tab)
{
    if (!tab)
        return;
    free(tab->pic);
    free(tab->wlist);
    free(tab->wlog);
    free(tab->ioev);
    free(tab);
}

void chfvid_tab_set_view(chfvid_tab *tab, int view)
{
    if (tab && view >= 0 && view < CHFVID_V_COUNT && view != tab->view)
    {
        tab->view = view;
        tab->dirty = 1;
    }
}
int chfvid_tab_view(const chfvid_tab *tab) { return tab ? tab->view : 0; }
void chfvid_tab_set_overlays(chfvid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & CHFVID_OV_ALL) != tab->ovl)
    {
        tab->ovl = ovl & CHFVID_OV_ALL;
        tab->dirty = 1;
    }
}
unsigned chfvid_tab_overlays(const chfvid_tab *tab) { return tab ? tab->ovl : 0; }
void chfvid_tab_set_zoom(chfvid_tab *tab, int zoom)
{
    if (tab && zoom >= 0 && zoom <= 16 && zoom != tab->zoom)
    {
        tab->zoom = zoom;
        tab->dirty = 1;
    }
}
int chfvid_tab_zoom(const chfvid_tab *tab) { return tab ? tab->zoom : 0; }
void chfvid_tab_set_viewport(chfvid_tab *tab, int wide, int high)
{
    if (!tab)
        return;
    if (wide < 1)
        wide = 1;
    if (high < 1)
        high = 1;
    if (wide != tab->vieww || high != tab->viewh)
    {
        tab->vieww = wide;
        tab->viewh = high;
        if (tab->zoom == 0)
            tab->dirty = 1;
    }
}
int chfvid_tab_env_section(const chfvid_tab *tab) { return tab ? tab->env_section : 0; }

void chfvid_tab_set_write_filter(chfvid_tab *tab, const chfvid_write_filter *filter)
{
    if (tab && filter)
        tab->wf = *filter;
}
void chfvid_tab_write_filter(const chfvid_tab *tab, chfvid_write_filter *out)
{
    if (out)
        memset(out, 0, sizeof *out);
    if (tab && out)
        *out = tab->wf;
}

void chfvid_tab_clear_writes(chfvid_tab *tab)
{
    if (tab)
        chfdebug_vram_writes_clear(tab->dbg);
}

/* ---- the cell inspector ------------------------------------------------------------------ */

static void locate(chfvid_tab *tab, int row, int col, chfvid_pixel *outp)
{
    chfdebug_vram_cell cell;
    int len;

    memset(outp, 0, sizeof *outp);
    outp->last_pc = -1;
    if (chfdebug_vram_cell_get(tab->dbg, row, col, &cell) != 0)
        return;
    outp->valid = 1;
    outp->row = row;
    outp->col = col;
    outp->visible = cell.visible;
    outp->hidden_why = cell.hidden_why;
    outp->value = cell.value;
    outp->palette = cell.palette;
    outp->palette_name = chfdebug_palette_name(cell.palette);
    outp->pen = cell.pen;
    outp->colour = chfdebug_colour_name(cell.pen);
    outp->rgb = cell.rgb | 0xff000000u;
    outp->scanline = row * (tab->y_rpt ? tab->y_rpt : 4);
    outp->writes = cell.writes;
    outp->last_pc = cell.last_pc;
    outp->last_frame = cell.last_frame;
    outp->last_line = cell.last_line;
    outp->last_cycle = cell.last_cycle;
    outp->last_old = cell.last_old;

    len = snprintf(outp->text, sizeof outp->text, "row %d, column %d  (%s)\n", row, col,
                   cell.visible ? "visible" : cell.hidden_why ? cell.hidden_why : "hidden");
    len += snprintf(outp->text + len, sizeof outp->text - (size_t)len,
                    "value %d  palette %d (%s)  shows %s #%06X\n", cell.value, cell.palette,
                    outp->palette_name, outp->colour, outp->rgb & 0xffffff);
    len += snprintf(outp->text + len, sizeof outp->text - (size_t)len, "row starts at beam line %d\n",
                    outp->scanline);
    if (cell.writes && cell.last_pc >= 0)
        snprintf(outp->text + len, sizeof outp->text - (size_t)len,
                 "written %u time%s since the debugger attached; last by PC $%04X on frame %u line %d "
                 "(was %d)",
                 cell.writes, cell.writes == 1 ? "" : "s", cell.last_pc, cell.last_frame, cell.last_line,
                 cell.last_old);
    else
        snprintf(outp->text + len, sizeof outp->text - (size_t)len, "not written since the debugger attached");
}

/* the picture-space rectangle of a cell in the current view, or 0 */
static int cell_mark(const chfvid_tab *tab, int row, int col, int rect[4])
{
    const int zoom = tab->effzoom ? tab->effzoom : 1;
    int org_col = 0, org_row = 0;

    if (row < 0 || col < 0)
        return 0;
    if (!whole_view(tab->view))
    {
        org_col = CHFDEBUG_VIS_COL0;
        org_row = CHFDEBUG_VIS_ROW0;
        if (col < org_col || col >= org_col + CHFDEBUG_VIS_COLS || row < org_row ||
            row >= org_row + CHFDEBUG_VIS_ROWS)
            return 0;
    }
    rect[0] = (col - org_col) * CELL_W * zoom;
    rect[1] = (row - org_row) * CELL_H * zoom;
    rect[2] = CELL_W * zoom;
    rect[3] = CELL_H * zoom;
    return 1;
}

static int hit_cell(const chfvid_tab *tab, int posx, int posy, int *row, int *col)
{
    const int zoom = tab->effzoom ? tab->effzoom : 1;

    if (posx < 0 || posy < 0 || !tab->picw)
        return 0;
    if (posx >= tab->picw || posy >= tab->pich)
        return 0;
    return chfdebug_video_hit(tab->dbg, k_view_kind[tab->view], posx, posy, zoom, row, col) >= 0;
}

int chfvid_tab_hover(chfvid_tab *tab, int posx, int posy)
{
    char before[sizeof tab->hover.text];
    int row, col;

    if (!tab)
        return 0;
    snprintf(before, sizeof before, "%s", tab->hover.valid ? tab->hover.text : "");
    if (hit_cell(tab, posx, posy, &row, &col))
    {
        tab->hovr = row;
        tab->hovc = col;
        locate(tab, row, col, &tab->hover);
    }
    else
    {
        tab->hovr = tab->hovc = -1;
        memset(&tab->hover, 0, sizeof tab->hover);
    }
    return strcmp(before, tab->hover.valid ? tab->hover.text : "") != 0;
}

void chfvid_tab_pin_cell(chfvid_tab *tab, int row, int col)
{
    if (!tab)
        return;
    if (row < 0 || row >= CHFDEBUG_VRAM_ROWS || col < 0 || col >= CHFDEBUG_VRAM_COLS)
        return;
    tab->pinr = row;
    tab->pinc = col;
    locate(tab, row, col, &tab->pin);
    if (!tab->pin.valid)
        chfvid_tab_unpin(tab);
}

void chfvid_tab_pin(chfvid_tab *tab, int posx, int posy)
{
    int row, col;

    if (!tab)
        return;
    if (!hit_cell(tab, posx, posy, &row, &col))
    {
        chfvid_tab_unpin(tab);
        return;
    }
    if (tab->pinr == row && tab->pinc == col)
    {
        chfvid_tab_unpin(tab);
        return;
    }
    chfvid_tab_pin_cell(tab, row, col);
}

void chfvid_tab_unpin(chfvid_tab *tab)
{
    if (!tab)
        return;
    tab->pinr = tab->pinc = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const chfvid_pixel *chfvid_tab_pixel(const chfvid_tab *tab)
{
    if (!tab)
        return NULL;
    if (tab->pin.valid)
        return &tab->pin;
    if (tab->hover.valid)
        return &tab->hover;
    return NULL;
}

int chfvid_tab_marks(const chfvid_tab *tab, int hover[4], int pin[4])
{
    int ret = 0;

    if (!tab)
        return 0;
    if (tab->hover.valid && cell_mark(tab, tab->hovr, tab->hovc, hover))
        ret |= 1;
    if (tab->pin.valid && cell_mark(tab, tab->pinr, tab->pinc, pin))
        ret |= 2;
    return ret;
}

int chfvid_tab_can_break(const chfvid_tab *tab)
{
    const chfvid_pixel *pixp = chfvid_tab_pixel(tab);

    return tab && tab->dbg && pixp && pixp->valid;
}

int chfvid_tab_break_on_pixel(chfvid_tab *tab)
{
    const chfvid_pixel *pixp = chfvid_tab_pixel(tab);
    chfdebug_breakpoint list[256];
    int count, cell, id;

    if (!chfvid_tab_can_break(tab))
        return -1;
    cell = pixp->row * CHFDEBUG_VRAM_COLS + pixp->col;
    count = chfdebug_breakpoint_list(tab->dbg, list, 256);
    for (int idx = 0; idx < count; idx++)
        if (list[idx].type == CHFDEBUG_BP_VRAM && list[idx].start == cell && list[idx].end == cell)
        {
            if (!list[idx].enabled)
                chfdebug_breakpoint_enable(tab->dbg, list[idx].id, 1);
            return list[idx].id;
        }
    id = chfdebug_breakpoint_add(tab->dbg, CHFDEBUG_BP_VRAM, CHF_SPACE_VRAM, (uint16_t)cell, (uint16_t)cell, "");
    return id > 0 ? id : -1;
}

/* ---- reading the machine ------------------------------------------------------------------ */

static int alloc_scratch(chfvid_tab *tab)
{
    if (!tab->wlist)
        tab->wlist = calloc(CHFVID_WRITES_MAX, sizeof *tab->wlist);
    if (!tab->wlog)
        tab->wlog = calloc(LOG_N, sizeof *tab->wlog);
    if (!tab->ioev)
        tab->ioev = calloc(IO_N, sizeof *tab->ioev);
    return tab->wlist && tab->wlog && tab->ioev;
}

static const char *sound_name(int mode)
{
    static const char *const names[4] = { "off", "1 kHz", "500 Hz", "120 Hz" };

    return names[mode & 3];
}

static void reg_set(chfvid_tab *tab, int *num, const char *name, int port, const char *value, const char *meaning)
{
    chfvid_reg *regp;

    if (*num >= MAX_REGS)
        return;
    regp = &tab->regs[*num];
    memset(regp, 0, sizeof *regp);
    regp->name = name;
    regp->port = port;
    snprintf(regp->value, sizeof regp->value, "%s", value);
    snprintf(regp->meaning, sizeof regp->meaning, "%s", meaning);
    snprintf(regp->last, sizeof regp->last, "%s", "never");
    (*num)++;
}

static void build_regs(chfvid_tab *tab)
{
    const chfdebug_video *vid = &tab->vid;
    static const int ports[4] = { 0, 1, 4, 5 };
    static const char *const names[4] = { "PORT 0", "PORT 1", "PORT 4", "PORT 5" };
    const uint8_t raw[4] = { vid->port0, vid->port1, vid->port4, vid->port5 };
    int ages[MAX_REGS], num = 0, got;
    char value[24], meaning[128];

    for (int idx = 0; idx < MAX_REGS; idx++)
        ages[idx] = tab->regs[idx].age;
    for (int idx = 0; idx < 4; idx++)
    {
        snprintf(value, sizeof value, "$%02X", raw[idx]);
        switch (idx)
        {
        case 0:
            snprintf(meaning, sizeof meaning, "PANEL: ~$%02X  strobe %s  controllers %s  panel latch %X",
                     (uint8_t)~raw[idx], vid->strobe ? "up" : "down",
                     vid->controllers_enabled ? "read" : "off", vid->panel_latch);
            break;
        case 1:
            snprintf(meaning, sizeof meaning, "RIGHT_C: ~$%02X  colour value %d (~port1 >> 6)",
                     (uint8_t)~raw[idx], vid->value);
            break;
        case 2:
            snprintf(meaning, sizeof meaning, "LEFT_C: ~$%02X  column %d (~port4 & $7F)", (uint8_t)~raw[idx],
                     vid->col);
            break;
        default:
            snprintf(meaning, sizeof meaning, "ROW_SOUND: ~$%02X  row %d (~port5 & $3F)  sound %s",
                     (uint8_t)~raw[idx], vid->row, sound_name(vid->sound_mode));
            break;
        }
        reg_set(tab, &num, names[idx], ports[idx], value, meaning);
        tab->regs[num - 1].raw = raw[idx];
        tab->regs[num - 1].comp = (uint8_t)~raw[idx];
    }
    {
        const unsigned pal = vid->row_palette[vid->row & 0x3f];

        snprintf(value, sizeof value, "r%d c%d = %d", vid->row, vid->col, vid->value);
        snprintf(meaning, sizeof meaning, "the next strobe writes value %d at row %d column %d, shown as %s "
                 "(palette %u, %s)", vid->value, vid->row, vid->col,
                 chfdebug_colour_name(chfdebug_palette_pen((int)pal, vid->value)), pal,
                 chfdebug_palette_name((int)pal));
        reg_set(tab, &num, "CELL", -1, value, meaning);
    }
    snprintf(meaning, sizeof meaning, "port 5 bits 7-6 = %d", vid->sound_mode);
    reg_set(tab, &num, "SOUND", -1, sound_name(vid->sound_mode), meaning);

    /* ages: refreshes since the value last changed */
    for (int idx = 0; idx < num; idx++)
    {
        if (!tab->regs_primed)
            tab->regs[idx].age = CHFVID_REG_FADE;
        else if (strcmp(tab->prev_val[idx], tab->regs[idx].value) != 0)
            tab->regs[idx].age = 0;
        else
            tab->regs[idx].age = ages[idx] < CHFVID_REG_FADE ? ages[idx] + 1 : CHFVID_REG_FADE;
        snprintf(tab->prev_val[idx], sizeof tab->prev_val[idx], "%s", tab->regs[idx].value);
    }
    tab->nregs = num;
    tab->regs_primed = 1;

    /* the newest OUT to each port from the port log */
    got = tab->ioev ? chfdebug_io_events(tab->dbg, tab->ioev, IO_N) : 0;
    for (int idx = 0; idx < 4; idx++)
        for (int pos = got - 1; pos >= 0; pos--)
            if (tab->ioev[pos].is_write && tab->ioev[pos].port == ports[idx])
            {
                snprintf(tab->regs[idx].last, sizeof tab->regs[idx].last, "frame %u pc $%04X",
                         tab->ioev[pos].frame, tab->ioev[pos].pc);
                break;
            }
}

static void build_colours(chfvid_tab *tab)
{
    long uses[PAL_N][4];

    memset(uses, 0, sizeof uses);
    for (int row = 0; row < CHFDEBUG_VRAM_ROWS; row++)
    {
        const int pal = tab->vid.row_palette[row] & 3;

        for (int col = 0; col < CHFDEBUG_VRAM_COLS; col++)
            uses[pal][tab->vram[row * CHFDEBUG_VRAM_COLS + col] & 3]++;
    }
    for (int pal = 0; pal < PAL_N; pal++)
        for (int val = 0; val < 4; val++)
        {
            chfvid_swatch *swat = &tab->swat[pal * 4 + val];

            memset(swat, 0, sizeof *swat);
            swat->palette = pal;
            swat->palette_name = chfdebug_palette_name(pal);
            swat->value = val;
            swat->pen = chfdebug_palette_pen(pal, val);
            swat->name = chfdebug_colour_name(swat->pen);
            swat->rgb = chfdebug_colour_rgb(swat->pen) | 0xff000000u;
            swat->rows = tab->vid.palette_rows[pal];
            swat->cells = uses[pal][val];
        }
}

static void build_lines(chfvid_tab *tab, int nlog)
{
    const int lines_per_row = tab->y_rpt ? tab->y_rpt : 4;
    int beam_row = -1;

    if (tab->vid.line < tab->vbstart)
        beam_row = tab->vid.line / lines_per_row;
    for (int row = 0; row < LINE_ROWS; row++)
    {
        chfvid_line_row *rowp = &tab->rows[row];

        memset(rowp, 0, sizeof *rowp);
        rowp->row = row;
        rowp->scanline = row * lines_per_row;
        rowp->rows_lines = lines_per_row;
        rowp->visible = row >= CHFDEBUG_VIS_ROW0 && row < CHFDEBUG_VIS_ROW0 + CHFDEBUG_VIS_ROWS;
        rowp->palette = tab->vid.row_palette[row];
        rowp->palette_name = chfdebug_palette_name(rowp->palette);
        rowp->background = tab->vid.row_background[row] | 0xff000000u;
        rowp->last_pc = -1;
        rowp->beam = row == beam_row;
    }
    for (int pos = nlog - 1; pos >= 0; pos--)
    {
        const chfdebug_vram_write *wp = &tab->wlog[pos];
        chfvid_line_row *rowp;

        if (wp->row >= LINE_ROWS)
            continue;
        rowp = &tab->rows[wp->row];
        if (rowp->last_pc < 0)
        {
            rowp->last_pc = wp->pc;
            rowp->last_col = wp->col;
            rowp->last_frame = wp->frame;
        }
        if (wp->frame == tab->vid.frame)
        {
            rowp->writes++;
            if (wp->col >= CHFDEBUG_PAL_COL_A)
                rowp->pal_writes++;
        }
    }
}

static void build_writes(chfvid_tab *tab)
{
    chfdebug_vram_filter filt;
    const chfvid_pixel *pixp = chfvid_tab_pixel(tab);

    memset(&filt, 0, sizeof filt);
    filt.row1 = CHFDEBUG_VRAM_ROWS - 1;
    filt.col1 = CHFDEBUG_VRAM_COLS - 1;
    filt.changes_only = tab->wf.changes_only;
    filt.this_frame = tab->wf.this_frame;
    filt.palette_cols = tab->wf.palette_cols;
    if (pixp && pixp->valid && (tab->wf.inspected_cell || tab->wf.inspected_row))
    {
        filt.row0 = filt.row1 = pixp->row;
        if (tab->wf.inspected_cell)
            filt.col0 = filt.col1 = pixp->col;
    }
    tab->nwrites = chfdebug_vram_writes(tab->dbg, &filt, tab->wlist, CHFVID_WRITES_MAX);
}

int chfvid_write_text(const chfdebug_vram_write *write, char *dst, int dstsz)
{
    int len;

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!write)
        return 0;
    len = snprintf(dst, (size_t)dstsz, "frame %u line %d pc $%04X  row %d col %d = %d (was %d)", write->frame,
                   write->line, write->pc, write->row, write->col, write->value, write->old);
    return len < dstsz ? len : dstsz - 1;
}

static void timing_line(chfvid_tab *tab, const char *label, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void timing_line(chfvid_tab *tab, const char *label, const char *fmt, ...)
{
    char value[96];
    va_list ap;

    if (tab->ntiming >= MAX_TIMING)
        return;
    va_start(ap, fmt);
    vsnprintf(value, sizeof value, fmt, ap);
    va_end(ap);
    snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], "%s\t%s", label, value);
    tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming];
    tab->ntiming++;
}

static void build_timing(chfvid_tab *tab)
{
    const chfdebug_video *vid = &tab->vid;
    const int lines_per_row = tab->y_rpt ? tab->y_rpt : 4;

    tab->ntiming = 0;
    timing_line(tab, "Standard", "%s, %d lines a frame at %d Hz", vid->is_pal ? "PAL" : "NTSC", vid->lines,
                vid->is_pal ? 50 : 60);
    timing_line(tab, "Beam", "line %d of %d, CPU cycle %d of %d", vid->line, vid->lines, vid->cycle_in_line,
                vid->cycles_per_line);
    timing_line(tab, "Frame", "%u", vid->frame);
    timing_line(tab, "VRAM row", "%d lines (row = line / %d)", lines_per_row, lines_per_row);
    timing_line(tab, "Drawn lines", "%d to %d, vertical blank from %d", tab->vbend, tab->vbstart - 1,
                tab->vbstart);
    timing_line(tab, "CPU cycles", "%d a line, %d a frame", vid->cycles_per_line,
                vid->cycles_per_line * vid->lines);
    timing_line(tab, "Strobes", "%u this frame so far, %u last frame (%u changed a cell)", vid->strobes_frame,
                vid->strobes_last, vid->changes_last);
    timing_line(tab, "Palettes", "rows: %d black & white, %d light blue, %d light grey, %d light green",
                vid->palette_rows[0], vid->palette_rows[1], vid->palette_rows[2], vid->palette_rows[3]);
    timing_line(tab, "Machine", "%s", tab->stopped ? "stopped" : "running");
}

static void build_status(chfvid_tab *tab)
{
    const chfdebug_video *vid = &tab->vid;
    const chfvid_pixel *pixp = chfvid_tab_pixel(tab);
    const int lines_per_row = tab->y_rpt ? tab->y_rpt : 4;
    int len;

    len = snprintf(tab->status, sizeof tab->status, "%s | frame %u | beam line %d of %d (row %d) | %s\n",
                   chfvid_view_name(tab->view), vid->frame, vid->line, vid->lines, vid->line / lines_per_row,
                   tab->stopped ? "stopped" : "running");
    if (pixp && pixp->valid)
        snprintf(tab->status + len, sizeof tab->status - (size_t)len,
                 "row %d column %d = %d (%s)%s%s", pixp->row, pixp->col, pixp->value, pixp->colour,
                 pixp->visible ? "" : ", hidden: ", pixp->visible || !pixp->hidden_why ? "" : pixp->hidden_why);
    else
        snprintf(tab->status + len, sizeof tab->status - (size_t)len,
                 "strobes %u this frame, %u last frame (%u changed a cell)", vid->strobes_frame,
                 vid->strobes_last, vid->changes_last);
}

/* Read the machine and rebuild the tables; 0 when there is none. */
static int gather(chfvid_tab *tab)
{
    int nlog;

    if (!alloc_scratch(tab))
        return 0;
    chfdebug_video_get(tab->dbg, &tab->vid);
    if (tab->vid.lines <= 0 || chfdebug_read(tab->dbg, CHF_SPACE_VRAM, 0, tab->vram, VRAM_BYTES) != VRAM_BYTES ||
        !chfdebug_video_geometry(tab->dbg, &tab->y_rpt, &tab->vbstart, &tab->vbend))
    {
        tab->have = 0;
        tab->nregs = tab->ntiming = tab->nwrites = 0;
        memset(tab->rows, 0, sizeof tab->rows);
        memset(tab->swat, 0, sizeof tab->swat);
        snprintf(tab->status, sizeof tab->status, "The machine is not running.");
        return 0;
    }
    tab->have = 1;
    tab->stopped = tab->dbg ? chfdebug_is_stopped(tab->dbg) : 0;

    if (tab->hovr >= 0)
        locate(tab, tab->hovr, tab->hovc, &tab->hover);
    if (tab->pinr >= 0)
        locate(tab, tab->pinr, tab->pinc, &tab->pin);

    nlog = chfdebug_vram_writes(tab->dbg, NULL, tab->wlog, LOG_N);
    build_regs(tab);
    build_colours(tab);
    build_lines(tab, nlog);
    build_writes(tab);
    build_timing(tab);
    build_status(tab);
    return 1;
}

static uint64_t tables_signature(const chfvid_tab *tab)
{
    uint64_t hash = 14695981039346656037ull;
    const chfvid_pixel *pixp = chfvid_tab_pixel(tab);

    hash = fnv(hash, tab->regs, sizeof tab->regs);
    hash = fnv(hash, tab->swat, sizeof tab->swat);
    hash = fnv(hash, tab->rows, sizeof tab->rows);
    hash = fnv(hash, tab->wlist, (size_t)tab->nwrites * sizeof *tab->wlist);
    hash = fnv(hash, tab->status, strlen(tab->status));
    for (int idx = 0; idx < tab->ntiming; idx++)
        hash = fnv(hash, tab->timing_buf[idx], strlen(tab->timing_buf[idx]));
    if (pixp)
        hash = fnv(hash, pixp->text, strlen(pixp->text));
    return hash;
}

/* ---- the picture --------------------------------------------------------------------------- */

static uint64_t picture_signature(const chfvid_tab *tab)
{
    uint64_t hash = 14695981039346656037ull;
    const chfdebug_video *vid = &tab->vid;
    const uint32_t parts[8] = { vid->frame, vid->strobes_frame, vid->strobes_last, vid->changes_last,
                                (uint32_t)vid->port0 | (uint32_t)vid->port1 << 8 | (uint32_t)vid->port4 << 16 |
                                    (uint32_t)vid->port5 << 24,
                                0, 0, 0 };
    uint32_t words[8];

    memcpy(words, parts, sizeof words);
    if (tab->view == CHFVID_V_BEAM || (tab->ovl & CHFVID_OV_BEAM))
        words[5] = (uint32_t)vid->line + 1;
    words[6] = (uint32_t)tab->stopped;
    hash = fnv(hash, words, sizeof words);
    return fnv(hash, tab->vram, sizeof tab->vram);
}

static int pick_zoom(const chfvid_tab *tab, int basew, int baseh)
{
    int zoom = tab->zoom;

    if (zoom == 0)
    {
        zoom = 1;
        for (size_t idx = 1; idx < sizeof k_zooms / sizeof *k_zooms; idx++)
            if (basew * k_zooms[idx] <= tab->vieww && baseh * k_zooms[idx] <= tab->viewh)
                zoom = k_zooms[idx];
    }
    while (zoom > 1 && (long)basew * zoom * (long)baseh * zoom > MAXPIX)
    {
        int lower = 1;

        for (size_t idx = 1; idx < sizeof k_zooms / sizeof *k_zooms; idx++)
            if (k_zooms[idx] < zoom)
                lower = k_zooms[idx];
        zoom = lower;
    }
    return zoom;
}

static void build_picture(chfvid_tab *tab)
{
    const int kind = k_view_kind[tab->view];
    int basew = 0, baseh = 0, zoom, wide = 0, high = 0;
    size_t need;

    tab->picw = tab->pich = 0;
    if (!tab->have)
    {
        why_set(tab, "The machine is not running.");
        return;
    }
    chfdebug_video_view(tab->dbg, kind, 0, 1, NULL, 0, &basew, &baseh);
    zoom = pick_zoom(tab, basew, baseh);
    need = (size_t)basew * (size_t)zoom * (size_t)baseh * (size_t)zoom;
    if (need > tab->pic_cap)
    {
        uint32_t *grown = realloc(tab->pic, need * sizeof *grown);

        if (!grown)
        {
            why_set(tab, "Out of memory for the picture.");
            return;
        }
        tab->pic = grown;
        tab->pic_cap = need;
    }
    if (!chfdebug_video_view(tab->dbg, kind, tab->ovl & CHFVID_OV_ALL, zoom, tab->pic, tab->pic_cap, &wide, &high))
    {
        why_set(tab, "The machine is not running.");
        return;
    }
    for (size_t idx = 0; idx < (size_t)wide * (size_t)high; idx++)
        tab->pic[idx] |= 0xff000000u;
    tab->picw = wide;
    tab->pich = high;
    tab->effzoom = zoom;
    tab->why[0] = '\0';
}

unsigned chfvid_tab_refresh(chfvid_tab *tab, int force)
{
    unsigned ret = 0;
    uint64_t sig, tsig;

    if (!tab)
        return 0;
    if (!gather(tab))
    {
        why_set(tab, "The machine is not running.");
        tab->picw = tab->pich = 0;
        tab->sig = 0;
        tab->dirty = 1;
        return CHFVID_TAB_OFF | CHFVID_TAB_PICTURE | CHFVID_TAB_TABLES;
    }
    sig = picture_signature(tab);
    if (force || tab->dirty || sig != tab->sig)
    {
        tab->sig = sig;
        tab->dirty = 0;
        build_picture(tab);
        ret |= CHFVID_TAB_PICTURE;
    }
    tsig = tables_signature(tab);
    if (force || tsig != tab->table_sig)
    {
        tab->table_sig = tsig;
        ret |= CHFVID_TAB_TABLES;
    }
    return ret;
}

const uint32_t *chfvid_tab_picture(chfvid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw)
        return NULL;
    if (wide)
        *wide = tab->picw;
    if (high)
        *high = tab->pich;
    return tab->pic;
}
const char *chfvid_tab_why(const chfvid_tab *tab) { return tab ? tab->why : ""; }
int chfvid_tab_effective_zoom(const chfvid_tab *tab) { return tab ? tab->effzoom : 1; }
const char *chfvid_tab_status(const chfvid_tab *tab) { return tab ? tab->status : ""; }

int chfvid_tab_regs(const chfvid_tab *tab, const chfvid_reg **out)
{
    if (out)
        *out = tab->regs;
    return tab->nregs;
}

int chfvid_tab_swatches(const chfvid_tab *tab, const chfvid_swatch **out)
{
    /* the inspected cell moves between refreshes */
    const chfvid_pixel *pixp = chfvid_tab_pixel(tab);
    chfvid_swatch *swat = ((chfvid_tab *)tab)->swat;
    const int count = tab->have ? PAL_N * 4 : 0;

    for (int idx = 0; idx < count; idx++)
        swat[idx].inspected = pixp && pixp->valid && pixp->palette == swat[idx].palette &&
                              pixp->value == swat[idx].value;
    if (out)
        *out = tab->swat;
    return count;
}

int chfvid_tab_lines(const chfvid_tab *tab, const chfvid_line_row **out)
{
    if (out)
        *out = tab->rows;
    return tab->have ? LINE_ROWS : 0;
}

int chfvid_tab_writes(const chfvid_tab *tab, const chfdebug_vram_write **out)
{
    if (out)
        *out = tab->wlist;
    return tab->nwrites;
}

int chfvid_tab_timing(const chfvid_tab *tab, const char *const **lines)
{
    if (lines)
        *lines = (const char *const *)tab->timing_ptr;
    return tab->ntiming;
}

int chfvid_tab_beam_permille(const chfvid_tab *tab)
{
    return tab && tab->have && tab->vid.lines > 0 ? tab->vid.line * 1000 / tab->vid.lines : 0;
}

int chfvid_tab_text(chfvid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;

#define PUT(...) do { if (num < dstsz) num += snprintf(dst + num, (size_t)(dstsz - num), __VA_ARGS__); } while (0)
    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!tab || !gather(tab))
    {
        PUT("The machine is not running.\n");
        return num < dstsz ? num : dstsz - 1;
    }
    switch (section)
    {
    case CHFVID_S_REGISTERS:
        for (int idx = 0; idx < tab->nregs; idx++)
            PUT("%-7s %-10s %s%s%s\n", tab->regs[idx].name, tab->regs[idx].value, tab->regs[idx].meaning,
                tab->regs[idx].port >= 0 ? "  last OUT: " : "", tab->regs[idx].port >= 0 ? tab->regs[idx].last : "");
        break;
    case CHFVID_S_COLOURS:
        for (int idx = 0; idx < PAL_N * 4; idx++)
            PUT("palette %d (%s, %d rows) value %d  %-11s #%06X  %ld cells\n", tab->swat[idx].palette,
                tab->swat[idx].palette_name, tab->swat[idx].rows, tab->swat[idx].value, tab->swat[idx].name,
                tab->swat[idx].rgb & 0xffffff, tab->swat[idx].cells);
        break;
    case CHFVID_S_LINES:
        for (int idx = 0; idx < LINE_ROWS; idx++)
        {
            const chfvid_line_row *rowp = &tab->rows[idx];

            PUT("row %2d  line %3d  palette %d %-14s %3d write%s", rowp->row, rowp->scanline, rowp->palette,
                rowp->palette_name, rowp->writes, rowp->writes == 1 ? " " : "s");
            if (rowp->last_pc >= 0)
                PUT("  last pc $%04X col %d frame %u", rowp->last_pc, rowp->last_col, rowp->last_frame);
            PUT("%s%s%s\n", rowp->pal_writes ? "  palette written" : "", rowp->visible ? "" : "  hidden",
                rowp->beam ? "  <- beam" : "");
        }
        break;
    case CHFVID_S_WRITES:
        for (int idx = 0; idx < tab->nwrites; idx++)
        {
            char line[128];

            chfvid_write_text(&tab->wlist[idx], line, sizeof line);
            PUT("%s\n", line);
        }
        if (!tab->nwrites)
            PUT("no matching writes\n");
        break;
    case CHFVID_S_TIMING:
        for (int idx = 0; idx < tab->ntiming; idx++)
            PUT("%s\n", tab->timing_buf[idx]);
        break;
    default:
        PUT("%s\n", tab->status);
        if (chfvid_tab_pixel(tab))
            PUT("%s\n", chfvid_tab_pixel(tab)->text);
        else
        {
            chfvid_pixel next;

            locate(tab, tab->vid.row, tab->vid.col, &next);
            if (next.valid)
                PUT("next strobe's cell:\n%s\n", next.text);
        }
        break;
    }
#undef PUT
    return num < dstsz ? num : dstsz - 1;
}
