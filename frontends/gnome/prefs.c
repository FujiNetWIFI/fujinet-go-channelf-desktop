/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * Pages: Machine, Display & Audio, Input, FujiNet and ROMs, named as Help's
 * contexts ("prefs/Machine", ...). The right stick, the volume, the aspect and
 * smooth scaling apply LIVE. Everything else is read when the machine powers
 * up: which console it is, which BIOS, and the host options (FujiNet, audio,
 * gamepads) that run beside it. Those mark the dialog dirty, and closing it
 * power-cycles the session once (chfsession_restart, which restarts FujiNet
 * and the devices only if one of their switches changed) -- an opened
 * cartridge boots again.
 *
 * The ROMs page lists the console's three BIOS ROMs and imports them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "controllers/controllers_window.h"
#include "fujilog.h"
#include "help/help_window.h"
#include "window.h"

#include <string.h>

#define MAX_PAD_ROWS 8

typedef struct {
    ChfWindow *window;
    chfsession *session;
    void (*restart)(ChfWindow *window);
    gboolean dirty;
    gboolean syncing;           /* a row is being set back: not a change */
    AdwDialog *dialog;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[MAX_PAD_ROWS + 1];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;
    GtkWidget *rom_status[CHF_ROM_COUNT];   /* "Imported" / "Not imported" */
    GtkWidget *bios_now;                    /* the Machine page's BIOS in use */
} PrefsState;

/* What a row does when it changes. */
enum {
    ROW_RESTART = -1,   /* read at power-up: power-cycle on close */
    ROW_STICK = -2,     /* live */
    ROW_ASPECT = -3,    /* live */
    ROW_SMOOTH = -4,    /* live */
    ROW_MACHINE = -5,   /* read at power-up */
};

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    int kind;
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def, int kind)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->kind = kind;
    return b;
}

static const char *aspect_name(int i)
{
    static const char *const names[] = { "Television (4:3)", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static const char *machine_name(int i)
{
    return i >= 0 && i < CHF_MACHINE_COUNT ? chf_machine_name(i) : NULL;
}

static const char *bios_name(int i)
{
    return i >= 0 && i < CHF_BIOS_CHOICE_COUNT ? chf_bios_choice_name(i) : NULL;
}

/* The machines' names are long: the popup's items show them whole. */
static void name_item_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    GtkWidget *l = gtk_label_new(NULL);
    (void)f; (void)ud;
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_margin_top(l, 6);
    gtk_widget_set_margin_bottom(l, 6);
    gtk_widget_set_margin_start(l, 6);
    gtk_widget_set_margin_end(l, 18);
    gtk_list_item_set_child(li, l);
}

static void name_item_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer ud)
{
    GtkStringObject *o = gtk_list_item_get_item(li);
    (void)f; (void)ud;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)),
                       o ? gtk_string_object_get_string(o) : "");
}

static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* ---- generic rows ------------------------------------------------------------ */

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (b->state->syncing)
        return;
    if (b->kind == ROW_ASPECT) {
        chfsession_set_int(b->state->session, b->key, sel);
        chf_window_apply_aspect(b->state->window, sel);
        return;
    }
    if (chfsession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    chfsession_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
}

static void stick_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)row; (void)pspec;
    chfsession_set_right_stick(state->session,
        chfsession_get_int(state->session, "right_stick", 1));
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (chfsession_get_int(b->state->session, b->key, b->def) == on)
        return;
    chfsession_set_int(b->state->session, b->key, on);
    if (b->kind == ROW_STICK)           /* the right stick: live */
        stick_changed(row, NULL, b->state);
    else if (b->kind == ROW_SMOOTH)
        chf_window_apply_smooth(b->state->window, on != 0);
    else
        b->state->dirty = TRUE;
}

static void volume_changed(GtkRange *range, gpointer user_data)
{
    PrefsState *state = user_data;
    chfsession_set_volume(state->session, (int)gtk_range_get_value(range));
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *(*name_fn)(int), int kind)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(NULL);
    int i;

    for (i = 0; name_fn(i); i++)
        gtk_string_list_append(model, name_fn(i));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
        (guint)chfsession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new(state, key, def, kind), binding_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int kind)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              chfsession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def, kind), binding_free, 0);
    return row;
}

#define EXTERNAL_PLACEHOLDER "127.0.0.1:11526"

/* The address field: valid text (or none, meaning 127.0.0.1:11526) is saved
 * and marks the machine dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (chfsession_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(chfsession_get_str(state->session, "fujinet_external_addr", ""),
                  text) == 0)
        return;
    chfsession_set_str(state->session, "fujinet_external_addr", text);
    state->dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(user_data),
                             adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

/* ---- gamepads: one row per pad, with the stick it drives ------------------ */

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    chfsession_gamepad_assign(state->session, idx, sel - 1);
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Player 1", "Player 2", NULL };
    int i, n = chfsession_gamepad_count(state->session);
    char name[128], sub[64];

    for (i = 0; i < state->pad_row_count; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group), state->pad_rows[i]);
    state->pad_row_count = 0;

    if (n == 0) {
        GtkWidget *row = info_row("No gamepads connected",
                                  "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (i = 0; i < n && i < MAX_PAD_ROWS; i++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        int eff = chfsession_gamepad_effective_port(state->session, i);
        chfsession_gamepad_name(state->session, i, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        if (eff >= 0)
            g_snprintf(sub, sizeof sub, "Driving player %d", eff + 1);
        else
            g_snprintf(sub, sizeof sub, "Driving nothing");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
            (guint)(chfsession_gamepad_assignment(state->session, i) + 1));
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(i));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

static void bios_now_sync(PrefsState *state);

static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    unsigned gen = chfsession_gamepad_generation(state->session);
    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    }
    bios_now_sync(state);       /* the machine may have started since */
    return G_SOURCE_CONTINUE;
}

/* ---- FujiNet ------------------------------------------------------------------ */

static void on_open_webui(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)b;
    if (!chfsession_fujinet_running(state->session)) {
        adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                         adw_toast_new(chfsession_fujinet_external(state->session)
                                                       ? "FujiNet is external: use its own web UI"
                                                       : "FujiNet is not running"));
        return;
    }
    chf_fujiconfig_show(GTK_WINDOW(state->window), state->session);
}

/* ---- the ROMs page ----------------------------------------------------------- */

static void rom_status_sync(PrefsState *state)
{
    int i;
    for (i = 0; i < CHF_ROM_COUNT && i < chfsession_rom_item_count(); i++) {
        const gboolean have = chfsession_rom_item_available(state->session, i) != 0;
        GtkWidget *l = state->rom_status[i];
        gtk_label_set_text(GTK_LABEL(l), have ? "Imported" : "Not imported");
        if (have) {
            gtk_widget_add_css_class(l, "chf-accent");
            gtk_widget_remove_css_class(l, "dim-label");
        } else {
            gtk_widget_remove_css_class(l, "chf-accent");
            gtk_widget_add_css_class(l, "dim-label");
        }
    }
}

static void bios_now_sync(PrefsState *state)
{
    static const char *const files[] = { "sl31253", "sl90025" };
    const int in_use = chfsession_bios_in_use(state->session);
    char text[160];

    if (in_use >= 0 && in_use < 2)
        g_snprintf(text, sizeof text, "%s and sl31254, imported", files[in_use]);
    else
        g_snprintf(text, sizeof text, "The app's stand-in (CONFIG and FujiNet apps only)");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->bios_now), text);
}

static void on_rom_imported(int imported, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)imported;
    rom_status_sync(state);
    bios_now_sync(state);
}

static void on_rom_import(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    const gboolean folder = g_object_get_data(G_OBJECT(b), "chf-folder") != NULL;
    chf_window_import_bios(state->window, GTK_WIDGET(state->dialog), folder, TRUE, NULL,
                           on_rom_imported, state);
}

static void build_roms_page(PrefsState *state, AdwPreferencesPage *page)
{
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *imp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *row = adw_action_row_new();
    GtkWidget *where = adw_action_row_new();
    GtkWidget *files = gtk_button_new_with_label("Import\xe2\x80\xa6");
    GtkWidget *dir = gtk_button_new_with_label("Import a Folder\xe2\x80\xa6");
    int i;

    adw_preferences_group_set_title(group, "Console");
    adw_preferences_group_set_description(group,
        "The console's BIOS is copyrighted, so none comes with the app. CONFIG and the "
        "FujiNet apps run on the app's own stand-in; Videocarts need these.");
    adw_preferences_group_set_header_suffix(group, chf_help_button("prefs/ROMs"));
    for (i = 0; i < chfsession_rom_item_count() && i < CHF_ROM_COUNT; i++) {
        const chf_rom_item *it = chfsession_rom_item(i);
        GtkWidget *r = adw_action_row_new();
        GtkWidget *st = gtk_label_new("");
        char sub[200];

        g_snprintf(sub, sizeof sub, "%s \xc2\xb7 %u bytes \xc2\xb7 CRC-32 %08X", it->file,
                   it->size, it->crc);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), it->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
        gtk_widget_add_css_class(st, "chf-pill");
        gtk_widget_set_valign(st, GTK_ALIGN_CENTER);
        gtk_widget_set_size_request(st, 104, -1);
        gtk_label_set_xalign(GTK_LABEL(st), 0.5f);
        adw_action_row_add_suffix(ADW_ACTION_ROW(r), st);
        adw_preferences_group_add(group, r);
        state->rom_status[i] = st;
    }
    adw_preferences_page_add(page, group);

    adw_preferences_group_set_title(imp, "Import");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Import a BIOS");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
        "A file, a zip (MAME's channelf.zip has all three) or a folder, recognised by "
        "size and checksum. Takes effect at the next power cycle.");
    g_object_set_data(G_OBJECT(dir), "chf-folder", GINT_TO_POINTER(1));
    g_signal_connect(files, "clicked", G_CALLBACK(on_rom_import), state);
    g_signal_connect(dir, "clicked", G_CALLBACK(on_rom_import), state);
    gtk_widget_set_valign(files, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(dir, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), files);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), dir);
    adw_preferences_group_add(imp, row);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(where), "Kept in");
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(where), FALSE);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(where), chfsession_roms_path(state->session));
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(where), TRUE);
    adw_preferences_group_add(imp, where);
    adw_preferences_page_add(page, imp);
    rom_status_sync(state);
}

/* ---- the dialog -------------------------------------------------------------- */

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    g_object_set_data(G_OBJECT(dialog), "prefs-state", NULL);
    if (state->pad_timer) g_source_remove(state->pad_timer);
    state->pad_timer = 0;
    chfsession_settings_flush(state->session);
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

/* name is Help's context suffix ("prefs/<name>") and the page's name. */
static AdwPreferencesPage *new_page(const char *title, const char *name, const char *icon)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, title);
    adw_preferences_page_set_name(page, name);
    adw_preferences_page_set_icon_name(page, icon);
    return page;
}

/* A group; the first on a page carries the page's Help button. */
static AdwPreferencesGroup *new_group(AdwPreferencesPage *page, const char *title,
                                      const char *description)
{
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, title);
    if (description)
        adw_preferences_group_set_description(g, description);
    if (!g_object_get_data(G_OBJECT(page), "chf-help-done")) {
        char ctx[64];
        g_snprintf(ctx, sizeof ctx, "prefs/%s", adw_preferences_page_get_name(page));
        adw_preferences_group_set_header_suffix(g, chf_help_button(ctx));
        g_object_set_data(G_OBJECT(page), "chf-help-done", GINT_TO_POINTER(1));
    }
    adw_preferences_page_add(page, g);
    return g;
}

static void on_open_controllers(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)b;
    chf_controllers_show(GTK_WINDOW(state->window), state->session);
}

static void on_reset_controls(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)b;
    chfsession_bindings_reset(state->session);
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                     adw_toast_new("Controls back to their defaults"));
}

void chf_prefs_show(ChfWindow *parent, chfsession *session,
                    void (*restart)(ChfWindow *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *machine = new_page("Machine", "Machine", "applications-games-symbolic");
    AdwPreferencesPage *video = new_page("Display & Audio", "Display", "video-display-symbolic");
    AdwPreferencesPage *input = new_page("Input", "Input", "input-gaming-symbolic");
    AdwPreferencesPage *net = new_page("FujiNet", "FujiNet", "network-wireless-symbolic");
    AdwPreferencesPage *roms = new_page("ROMs", "ROMs", "media-flash-symbolic");
    AdwPreferencesGroup *group, *external_group;
    GtkWidget *external, *addr, *row;

    state->window = parent;
    state->session = session;
    state->restart = restart;
    state->dialog = ADW_DIALOG(dialog);
    g_object_set_data(G_OBJECT(dialog), "prefs-state", state);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 640);

    /* ---- the console: applied by a power cycle when the dialog closes */
    group = new_group(machine, "Console",
        "Applied when Preferences closes, with a power cycle: an opened "
        "cartridge boots again. The NTSC machines run at 60.5 frames a second, "
        "the PAL ones at 50. The BIOS choice only matters once the real BIOS is "
        "imported (the ROMs page); without it the app's stand-in runs.");
    {
        GtkWidget *mrow = combo_row(state, "Machine", NULL,
                  "machine", CHF_MACHINE_CHANNELF, machine_name, ROW_MACHINE);
        /* the machines' names are longer than the row's suffix */
        guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(mrow));
        state->syncing = TRUE;
        adw_combo_row_set_use_subtitle(ADW_COMBO_ROW(mrow), TRUE);
        GtkListItemFactory *items = gtk_signal_list_item_factory_new();
        g_signal_connect(items, "setup", G_CALLBACK(name_item_setup), NULL);
        g_signal_connect(items, "bind", G_CALLBACK(name_item_bind), NULL);
        adw_combo_row_set_factory(ADW_COMBO_ROW(mrow), items);
        g_object_unref(items);
        /* the subtitle follows a change of selection: make one and undo it */
        adw_combo_row_set_selected(ADW_COMBO_ROW(mrow), (sel + 1) % CHF_MACHINE_COUNT);
        adw_combo_row_set_selected(ADW_COMBO_ROW(mrow), sel);
        state->syncing = FALSE;
        adw_preferences_group_add(group, mrow);
    }
    adw_preferences_group_add(group,
        combo_row(state, "BIOS", "Automatic takes the machine's own first half, else the other",
                  "bios", CHF_BIOS_AUTO, bios_name, ROW_RESTART));
    state->bios_now = info_row("BIOS running now", NULL);
    adw_preferences_group_add(group, state->bios_now);
    bios_now_sync(state);
    adw_preferences_dialog_add(dialog, machine);

    /* ---- the picture: live */
    group = new_group(video, "Display", NULL);
    adw_preferences_group_add(group,
        combo_row(state, "Aspect", "The 204 x 232 picture's shape (the television's, or "
                  "square pixels)", "aspect", 0, aspect_name, ROW_ASPECT));
    adw_preferences_group_add(group,
        switch_row(state, "Smooth scaling", "Blend the pixels when the picture is "
                   "scaled, instead of keeping them sharp", "smooth", 0, ROW_SMOOTH));

    /* ---- audio */
    group = new_group(video, "Audio", NULL);
    adw_preferences_group_add(group,
        switch_row(state, "Enable audio", "Open the system audio device "
                   "(applied when Preferences closes)", "enable_audio", 1, ROW_RESTART));
    row = adw_action_row_new();
    {
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Volume");
        gtk_range_set_value(GTK_RANGE(scale), chfsession_get_int(session, "volume", 100));
        gtk_widget_set_size_request(scale, 200, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
        adw_preferences_group_add(group, row);
    }
    adw_preferences_dialog_add(dialog, video);

    /* ---- input */
    group = new_group(input, "Input", NULL);
    adw_preferences_group_add(group,
        switch_row(state, "Enable gamepads", "Poll USB/Bluetooth gamepads "
                   "(applied when Preferences closes)", "enable_gamepad", 1, ROW_RESTART));
    adw_preferences_group_add(group,
        switch_row(state, "Right stick works the knob",
                   "The right analog stick twists (left and right) and pulls or "
                   "pushes (up and down); the left stick and the D-pad always tilt it",
                   "right_stick", 1, ROW_STICK));

    group = new_group(input, "Gamepads",
        "Assigned to the console's two controllers in connection order unless chosen here.");
    state->pad_group = GTK_WIDGET(group);
    rebuild_pad_rows(state);
    state->pad_generation = chfsession_gamepad_generation(session);

    group = new_group(input, "Controls",
        "Keys and gamepad buttons are mapped in the Controllers window (F9): press Map, "
        "choose a control, then press the key or gamepad button to give it.");
    {
        GtkWidget *ctl = info_row("Controllers window", "Map any of the 23 controls");
        GtkWidget *rst = info_row("Reset Controls", "Put every key and gamepad button back to its default");
        GtkWidget *open = gtk_button_new_with_label("Open");
        GtkWidget *reset = gtk_button_new_with_label("Reset");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_controllers), state);
        g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_controls), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(ctl), open);
        adw_action_row_add_suffix(ADW_ACTION_ROW(rst), reset);
        adw_preferences_group_add(group, ctl);
        adw_preferences_group_add(group, rst);
    }
    adw_preferences_dialog_add(dialog, input);

    /* ---- FujiNet */
    external_group = new_group(net, "FujiNet", NULL);
    adw_preferences_group_add(external_group,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. Off "
                   "means no network and a link-down CONFIG (applied when "
                   "Preferences closes).",
                   "enable_fujinet", 1, ROW_RESTART));
    external = switch_row(state, "Use external FujiNet",
                          "Skip the built-in FujiNet and dial another instance "
                          "at the address below (host:port, e.g. 192.168.1.20:11526; "
                          "empty is " EXTERNAL_PLACEHOLDER "). Set that instance's BoIP "
                          "listener to match (applied when Preferences closes).",
                          "fujinet_external", 0, ROW_RESTART);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr), "Address");
    gtk_widget_set_tooltip_text(addr, "host:port, or just a port; empty is " EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          chfsession_get_str(session, "fujinet_external_addr", ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled), addr);
    adw_preferences_group_add(external_group, external);
    adw_preferences_group_add(external_group, addr);
    {
        GtkWidget *web = info_row("Web UI",
                                  chfsession_fujinet_external(session)
                                      ? "FujiNet is external"
                                      : chfsession_fujinet_webui_url(session));
        GtkWidget *open = gtk_button_new_with_label("Open");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_webui), state);
        adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(web), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(web), open);
        adw_preferences_group_add(external_group, web);
    }
    adw_preferences_dialog_add(dialog, net);

    /* ---- the ROMs */
    build_roms_page(state, roms);
    adw_preferences_dialog_add(dialog, roms);

    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
