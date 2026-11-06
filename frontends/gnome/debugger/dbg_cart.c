/*
 * The debugger's Cart tab: the cartridge in the slot.
 *
 *   left   its state. A Videocart: the type, MAME's software list name and
 *          description, the ROM's size and checksum, its RAM and the bank
 *          latch. The FujiNet cartridge: the link, the live image and the one
 *          staged for the next swap (size, checksum, app or game), the APPBANK
 *          pages and the one showing at $4800, the boot state, and the status
 *          page at $FC00-$FC0F decoded (the 60 Hz tick included).
 *   right  the mailbox page the console reads ($F800-$FFFF, side-effect free) in
 *          hex, and the cartridge's event log: register writes, bytes sent, page
 *          selects and swaps, each with the PC that made it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <string.h>

#define EVENTS_MAX 300
#define MBOX_BASE 0xF800
#define MBOX_SIZE 0x800

struct DbgCart {
    chfsession *session;
    chfdebug *dbg;
    GtkTextView *state_text, *mbox_text, *events_text;
    int last_events;
    uint64_t last_sig;
};

static const char *const k_ev_kind[5] = { "?", "REG ", "TX  ", "BANK", "SWAP" };

static void refresh_events(DbgCart *c)
{
    static chfdebug_cart_event ev[EVENTS_MAX];
    const int n = chfdebug_cart_events(c->dbg, ev, EVENTS_MAX);
    uint64_t sig = n ? ev[n - 1].cycle ^ ((uint64_t)n << 48) : 0;
    GString *s;
    int i;

    if (n == c->last_events && sig == c->last_sig)
        return;
    c->last_events = n;
    c->last_sig = sig;
    s = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        const chfdebug_cart_event *e = &ev[i];
        g_string_append_printf(s, "frame %-6u pc $%04X  %s", e->frame, e->pc,
                               k_ev_kind[e->kind <= 4 ? e->kind : 0]);
        switch (e->kind) {
        case 1: g_string_append_printf(s, " reg %02X <- $%02X", e->reg, e->value); break;
        case 2: g_string_append_printf(s, " byte $%02X", e->value); break;
        case 3: g_string_append_printf(s, " page %d at $4800", e->value); break;
        case 4: g_string_append(s, " serve the staged image"); break;
        default: break;
        }
        g_string_append_c(s, '\n');
    }
    if (n == 0)
        g_string_append(s, "(no cartridge events since the debugger attached)\n");
    dbg_log_note_reader(c->events_text);
    dbg_set_text_follow(c->events_text, s->str, dbg_log_following(c->events_text));
    g_string_free(s, TRUE);
}

void dbg_cart_refresh(DbgCart *c, gboolean live)
{
    static chfdebug_cart d;
    static char text[5000], mbox[MBOX_SIZE / 16 * 80 + 200];
    char info[256];
    int len = 0, i;
    (void)live;

    chfdebug_cart_get(c->dbg, &d);
    chfdebug_cart_info(c->dbg, info, sizeof info);
    if (!d.present) {
        dbg_set_text(c->state_text, info[0] ? info : "(no cartridge)");
        dbg_set_text(c->mbox_text, "(no cartridge)");
        refresh_events(c);
        return;
    }
    len += g_snprintf(text + len, sizeof text - len, "%s\n\nType          %s\n", info,
                      d.type_name ? d.type_name : "?");
    if (d.softlist_name)
        len += g_snprintf(text + len, sizeof text - len, "Software list %s\n", d.softlist_name);
    if (d.description)
        len += g_snprintf(text + len, sizeof text - len, "Description   %s\n", d.description);
    if (d.rom_size)
        len += g_snprintf(text + len, sizeof text - len,
                          "ROM           %u bytes   CRC-32 %08X\n", d.rom_size, d.rom_crc);
    if (d.ram_size)
        len += g_snprintf(text + len, sizeof text - len,
                          d.ram_size == 1024 ? "RAM           %u bits (a 2102)\n"
                                             : "RAM           %u bytes\n", d.ram_size);
    if (d.bank || d.half)
        len += g_snprintf(text + len, sizeof text - len, "Bank          %d   half %d\n", d.bank, d.half);

    if (d.type_name && strstr(d.type_name, "FujiNet")) {
        const uint8_t *p = d.status_page;
        char page_note[48] = "";
        if (d.page >= 0)
            g_snprintf(page_note, sizeof page_note, ", page %d is at $4800", d.page);
        len += g_snprintf(text + len, sizeof text - len,
            "\nLink          %s%s\n"
            "Mailbox       %s\n"
            "Live image    %u bytes   CRC-32 %08X   %s\n"
            "Staged        %u bytes   CRC-32 %08X   %s%s%s\n"
            "Booted game   %s   swaps %d\n"
            "Pages         %d APPBANK page%s%s\n"
            "Boot          state %d   %d%%   error %d\n"
            "Queue         %u events waiting\n",
            d.link_up ? "up" : "down", d.link_off ? "   (switched off)" : "",
            d.mailbox ? "open (a socket and fujimail)" : "closed (a game runs alone)",
            d.live_size, d.live_crc, d.app_live ? "a FujiNet app" : "a game or CONFIG alone",
            d.staged_size, d.staged_crc, d.staged_app ? "an app" : "not an app",
            d.staged_mailbox ? ", claims the mailbox" : "", d.armed ? ", armed" : "",
            d.booted_game ? "yes" : "no", d.swaps,
            d.npages, d.npages == 1 ? "" : "s", page_note,
            d.boot_state, d.boot_pct, d.boot_err, d.queue_depth);
        len += g_snprintf(text + len, sizeof text - len,
            "\nStatus page $FC00-$FC0F, as the console reads it\n"
            "  $FC00  ACKSEQ      $%02X\n"
            "  $FC01  STATUS      $%02X   %s%s\n"
            "  $FC02  ERR         $%02X\n"
            "  $FC03  REPLY_CMD   $%02X   %s\n"
            "  $FC04  RXLEN       $%04X (%u)\n"
            "  $FC06  BOOT_STATE  $%02X\n"
            "  $FC07  BOOT_PCT    $%02X (%u%%)\n"
            "  $FC08  BOOT_ERR    $%02X\n"
            "  $FC09  MAGIC       '%c%c'\n"
            "  $FC0B  PROTO_VER   %u\n"
            "  $FC0C  SLICE_ECHO  $%02X\n"
            "  $FC0E  TICK        $%04X (%u, the 60 Hz count)\n",
            p[0], p[1], p[1] & 1 ? "link up" : "link down", p[1] & 2 ? ", busy" : "",
            p[2], p[3], p[3] == 0x06 ? "ACK" : p[3] == 0x15 ? "NAK" : "",
            p[4] | (p[5] << 8), p[4] | (p[5] << 8), p[6], p[7], p[7], p[8],
            p[9] >= 32 && p[9] < 127 ? p[9] : '.', p[10] >= 32 && p[10] < 127 ? p[10] : '.',
            p[11], p[12], p[14] | (p[15] << 8), p[14] | (p[15] << 8));
    }
    dbg_set_text(c->state_text, text);

    len = 0;
    len += g_snprintf(mbox, sizeof mbox,
                      "       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (i = 0; i < MBOX_SIZE; i++) {
        if (i % 16 == 0)
            len += g_snprintf(mbox + len, sizeof mbox - len, "$%04X: ", MBOX_BASE + i);
        len += g_snprintf(mbox + len, sizeof mbox - len, "%02X ", d.mailbox_page[i]);
        if (i % 16 == 15)
            len += g_snprintf(mbox + len, sizeof mbox - len, "\n");
    }
    dbg_set_text(c->mbox_text, mbox);
    refresh_events(c);
}

DbgCart *dbg_cart_new(chfsession *session, chfdebug *dbg, GtkWidget **root)
{
    DbgCart *c = g_new0(DbgCart, 1);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *right = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *state = dbg_mono_view(&c->state_text, FALSE);
    GtkWidget *mbox = dbg_mono_view(&c->mbox_text, FALSE);
    GtkWidget *events = dbg_mono_view(&c->events_text, FALSE);

    c->session = session;
    c->dbg = dbg;
    c->last_events = -1;
    gtk_text_view_set_wrap_mode(c->state_text, GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(state, 620, -1);
    gtk_widget_set_hexpand(state, FALSE);
    dbg_log_make(c->events_text);

    gtk_box_append(GTK_BOX(top), dbg_heading("Mailbox page ($F800-$FFFF) as the console reads it"));
    gtk_box_append(GTK_BOX(top), mbox);
    gtk_box_append(GTK_BOX(bottom), dbg_heading("Cartridge events"));
    gtk_box_append(GTK_BOX(bottom), events);
    gtk_paned_set_start_child(GTK_PANED(right), top);
    gtk_paned_set_end_child(GTK_PANED(right), bottom);
    gtk_paned_set_shrink_start_child(GTK_PANED(right), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(right), FALSE);
    gtk_paned_set_position(GTK_PANED(right), 340);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_size_request(mbox, 460, 160);
    gtk_widget_set_size_request(events, 460, 150);

    gtk_box_append(GTK_BOX(outer), state);
    gtk_box_append(GTK_BOX(outer), right);
    *root = dbg_padded(outer);
    return c;
}

void dbg_cart_free(DbgCart *c)
{
    g_free(c);
}
