/* LSIW Control: the Locations page.
 *
 * Lists every Windows location variable LSIW knows (the USMT/CSIDL list:
 * %APPDATA%, CSIDL_PERSONAL, %PROGRAMFILES%, WINDIR, ...) with the Windows
 * path programs see and the Linux folder behind it, and lets the user move
 * folders, edit plain values and reset either to the default.
 *
 * Model: GListStore of LsiwVarItem -> GtkFilterListModel (GtkCustomFilter:
 * search text, USMT only, scope) -> GtkSingleSelection -> GtkColumnView.
 * The buttons are bound to the page's "loc.*" actions, whose enabled state
 * follows the selected variable's kind:
 *   FOLDER   relocatable: Change location, Reset, Open folder
 *   ALIAS    acts on its target (APPDATA moves CSIDL_APPDATA)
 *   VALUE    Edit value, Reset
 *   DERIVED  follows its parent folder; read-only
 *   VIRTUAL  shell object (Recycle Bin, This PC); read-only
 *
 * Every change is written to the user's configuration file at once, the
 * C:\ view is rebuilt (lsiw_sync_root) and all rows are re-read, because
 * moving a folder also moves the folders defined inside it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct loc_page {
    LsiwPage base;
    GListStore *store;
    GtkFilter *filter;
    GtkFilterListModel *filtered;
    GtkSingleSelection *selection;
    GSimpleActionGroup *actions;
    GtkWidget *search, *scope, *usmt, *count;
    GtkWidget *detail_title, *detail_hint;
    GtkWidget *value_revealer, *value_entry;
    GtkWidget *status;
    char **needles;          /* case-folded search words */
    char *editing;           /* variable the value editor is open for */
};

enum { SCOPE_ALL, SCOPE_USER, SCOPE_SYSTEM };

/* ------------------------------------------------------------------ */
/* Filtering                                                            */
/* ------------------------------------------------------------------ */

static gboolean filter_match(gpointer obj, gpointer data)
{
    struct loc_page *p = data;
    LsiwVarItem *it = LSIW_VAR_ITEM(obj);
    const struct lsiw_var *v = lsiw_var_item_get_var(it);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(p->usmt)) && !v->usmt) return FALSE;
    guint scope = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->scope));
    if (scope == SCOPE_USER && v->scope != LSIW_SCOPE_USER) return FALSE;
    if (scope == SCOPE_SYSTEM && v->scope != LSIW_SCOPE_SYSTEM) return FALSE;
    if (p->needles) {
        const char *key = lsiw_var_item_get_search_key(it);
        for (char **n = p->needles; *n; n++)
            if (**n && !strstr(key, *n)) return FALSE;
    }
    return TRUE;
}

static void update_count(struct loc_page *p)
{
    guint shown = g_list_model_get_n_items(G_LIST_MODEL(p->filtered));
    guint total = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    char *s = shown == total ? g_strdup_printf("%u variables", total)
                             : g_strdup_printf("%u of %u variables", shown, total);
    gtk_label_set_text(GTK_LABEL(p->count), s);
    g_free(s);
}

static void filter_changed(struct loc_page *p)
{
    gtk_filter_changed(p->filter, GTK_FILTER_CHANGE_DIFFERENT);
    update_count(p);
}

static void search_changed_cb(GtkSearchEntry *entry, gpointer data)
{
    struct loc_page *p = data;
    g_strfreev(p->needles);
    char *folded = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);
    p->needles = g_strsplit_set(g_strstrip(folded), " \t", -1);
    g_free(folded);
    filter_changed(p);
}

static void option_changed_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    filter_changed(data);
}

static void usmt_toggled_cb(GtkCheckButton *b, gpointer data)
{
    (void)b;
    filter_changed(data);
}

/* ------------------------------------------------------------------ */
/* Selection and details                                                */
/* ------------------------------------------------------------------ */

static LsiwVarItem *selected_item(struct loc_page *p)
{
    gpointer it = gtk_single_selection_get_selected_item(p->selection);
    return it ? LSIW_VAR_ITEM(it) : NULL;
}

/* SYSTEMDRIVE and HOMEDRIVE are "C:" by LSIW's design: the whole Linux
 * filesystem is the only drive. */
static gboolean is_fixed_value(const struct lsiw_var *v)
{
    v = lsiw_var_resolve(v);
    return v && v->kind == LSIW_VAR_VALUE && v->def && !strcmp(v->def, "C:");
}

static void hide_value_editor(struct loc_page *p)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->value_revealer), FALSE);
    g_clear_pointer(&p->editing, g_free);
}

static void update_details(struct loc_page *p)
{
    LsiwVarItem *it = selected_item(p);
    gboolean can_change = FALSE, can_reset = FALSE, can_open = FALSE, can_edit = FALSE;

    if (p->editing && (!it || strcmp(lsiw_var_item_get_var(it)->name, p->editing)))
        hide_value_editor(p);

    if (!it) {
        gtk_label_set_markup(GTK_LABEL(p->detail_title), "<b>No variable selected</b>");
        gtk_label_set_text(GTK_LABEL(p->detail_hint),
                           "Select a variable to move its folder, edit its value or reset it.");
    } else {
        const struct lsiw_var *v = lsiw_var_item_get_var(it);
        const struct lsiw_var *r = lsiw_var_resolve(v);
        int kind = lsiw_var_item_effective_kind(it);
        const char *win = lsiw_var_item_get_windows_path(it);
        const char *lin = lsiw_var_item_get_linux_path(it);
        gboolean custom = lsiw_var_item_is_custom(it);

        char *title = g_markup_printf_escaped("<b>%s</b> — %s", v->name, v->desc);
        gtk_label_set_markup(GTK_LABEL(p->detail_title), title);
        g_free(title);

        GString *h = g_string_new(NULL);
        if (v->kind == LSIW_VAR_ALIAS && r)
            g_string_append_printf(h, "%s is another name for %s: changing one changes both. ",
                                   v->name, r->name);
        char *parent = lsiw_var_parent_name(v);
        switch (kind) {
        case LSIW_VAR_FOLDER:
            g_string_append_printf(h, "Windows programs see this folder as %s; its files are in %s%s. ",
                                   win ? win : "?", lin ? lin : "?",
                                   custom ? " (your choice)" : " (the default)");
            if (r && r->anchor)
                g_string_append_printf(h, "Wherever it lives, it appears as C:\\%s. ", r->anchor);
            if (!custom && parent)
                g_string_append_printf(h, "By default it is inside %s and moves with it. ", parent);
            g_string_append(h, "Changing the location does not move files that are already there.");
            can_change = TRUE;
            can_reset = custom;
            can_open = lin != NULL;
            break;
        case LSIW_VAR_VALUE:
            if (is_fixed_value(v)) {
                g_string_append_printf(h, "Always %s: in LSIW the whole Linux filesystem is drive C:, "
                                          "and there are no other drive letters.", win ? win : "C:");
                can_reset = custom;
            } else {
                g_string_append_printf(h, "A plain value, not a folder. Windows programs see “%s”%s.",
                                       win ? win : "", custom ? " (your choice)" : "");
                can_edit = TRUE;
                can_reset = custom;
            }
            break;
        case LSIW_VAR_DERIVED:
            g_string_append_printf(h, "Always at %s, inside %s: move %s to move it.",
                                   win ? win : "?", parent ? parent : "its parent folder",
                                   parent ? parent : "that folder");
            can_open = lin != NULL;
            break;
        case LSIW_VAR_VIRTUAL:
            if (lin)
                g_string_append_printf(h, "A Windows shell object, shown from %s. It cannot be moved.", lin);
            else
                g_string_append(h, "A Windows shell object with no folder behind it.");
            can_open = lin != NULL;
            break;
        }
        g_free(parent);
        gtk_label_set_text(GTK_LABEL(p->detail_hint), h->str);
        g_string_free(h, TRUE);

        /* Only folders are created on demand; other locations must exist. */
        if (can_open && kind != LSIW_VAR_FOLDER && !g_file_test(lin, G_FILE_TEST_IS_DIR))
            can_open = FALSE;
    }
    lsiw_gui_action_enable(p->actions, "change", can_change);
    lsiw_gui_action_enable(p->actions, "reset", can_reset);
    lsiw_gui_action_enable(p->actions, "open", can_open);
    lsiw_gui_action_enable(p->actions, "edit-value", can_edit);
}

static void selection_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    update_details(data);
}

/* ------------------------------------------------------------------ */
/* Changing a variable                                                  */
/* ------------------------------------------------------------------ */

static void set_status(struct loc_page *p, const char *text)
{
    gtk_label_set_text(GTK_LABEL(p->status), text ? text : "");
}

/* Set (value != NULL) or reset a variable, save, rebuild C:\ and refresh. */
static void apply_change(struct loc_page *p, const char *name, const char *value)
{
    GtkWidget *w = p->base.widget;
    const struct lsiw_var *v = lsiw_var_find(name);
    const struct lsiw_var *r = lsiw_var_resolve(v);
    int rc;

    if (value && r && r->kind == LSIW_VAR_FOLDER && value[0] == '/' &&
        (strchr(value, '\\') || strchr(value, '%'))) {
        /* lsiw_var_set() reads a value containing '\' or '%' as a Windows
         * path (C:\x, %APPDATA%\x). A folder chosen in the file dialog is
         * a Linux path, so it is stored verbatim instead, after
         * lsiw_var_set(NULL) has removed overrides under any alias name. */
        rc = lsiw_var_set(name, NULL);
        if (rc == 0) lsiw_setting_set("variables", r->name, value);
    } else {
        rc = lsiw_var_set(name, value);
    }

    if (rc == -1) {
        char *d = g_strdup_printf("LSIW has no variable called %s.", name);
        lsiw_gui_alert(w, "Unknown variable", d);
        g_free(d);
        return;
    }
    if (rc == -2) {
        char *parent = lsiw_var_parent_name(v);
        char *m = g_strdup_printf("%s cannot be moved on its own", name);
        char *d = g_strdup_printf("It is always inside %s. Move %s instead and it follows.",
                                  parent ? parent : "another folder",
                                  parent ? parent : "that folder");
        lsiw_gui_alert(w, m, d);
        g_free(m);
        g_free(d);
        g_free(parent);
        return;
    }
    if (!lsiw_gui_save_config(w)) {
        lsiw_gui_refresh_all(w, LSIW_REFRESH_CONFIG);
        return;
    }

    /* Create the folder, and point C:\<anchor> at it for anchored ones. */
    int sync = lsiw_sync_root(0);

    char *msg;
    const char *now_win = lsiw_var_windows(name);
    const char *now_lin = lsiw_var_linux(name);
    if (r && r->kind == LSIW_VAR_VALUE)
        msg = value ? g_strdup_printf("%s is now “%s”.", name, now_win ? now_win : value)
                    : g_strdup_printf("%s is back to its default, “%s”.", name, now_win ? now_win : "");
    else
        msg = g_strdup_printf("%s %s %s (%s in Windows).", name,
                              value ? "now lives in" : "is back at its default location,",
                              now_lin ? now_lin : "?", now_win ? now_win : "?");
    char *full = g_strdup_printf("%s Saved in %s.%s", msg, lsiw_config_path(),
                                 sync ? " The C: drive view could not be rebuilt; see the C: drive page." : "");
    set_status(p, full);
    g_free(full);
    g_free(msg);

    lsiw_gui_refresh_all(w, LSIW_REFRESH_CONFIG);
}

struct pick {
    GtkWidget *page;     /* reference held */
    char *name;
};

static void folder_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    struct pick *pk = data;
    GError *err = NULL;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    struct loc_page *p = g_object_get_data(G_OBJECT(pk->page), "lsiw-page");
    if (!f) {
        if (!g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
            !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
            gtk_widget_get_root(pk->page))
            lsiw_gui_alert(pk->page, "No folder was chosen", err->message);
        g_clear_error(&err);
    } else {
        char *path = g_file_get_path(f);
        if (!path)
            lsiw_gui_alert(pk->page, "This folder cannot be used",
                           "Choose a folder on this computer; network locations without a "
                           "local path cannot hold Windows folders.");
        else if (p && gtk_widget_get_root(pk->page))
            apply_change(p, pk->name, path);
        g_free(path);
        g_object_unref(f);
    }
    g_object_unref(pk->page);
    g_free(pk->name);
    g_free(pk);
}

/* The folder itself if it exists, else its nearest existing parent. */
static GFile *existing_folder(const char *path)
{
    if (!path) return g_file_new_for_path(g_get_home_dir());
    char *cur = g_strdup(path);
    while (!g_file_test(cur, G_FILE_TEST_IS_DIR) && strcmp(cur, "/")) {
        char *up = g_path_get_dirname(cur);
        g_free(cur);
        cur = up;
    }
    GFile *f = g_file_new_for_path(cur);
    g_free(cur);
    return f;
}

static void act_change(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct loc_page *p = data;
    LsiwVarItem *it = selected_item(p);
    if (!it) return;
    const struct lsiw_var *v = lsiw_var_item_get_var(it);

    GtkFileDialog *d = gtk_file_dialog_new();
    char *title = g_strdup_printf("Choose the Linux folder for %s", v->name);
    gtk_file_dialog_set_title(d, title);
    g_free(title);
    gtk_file_dialog_set_modal(d, TRUE);
    gtk_file_dialog_set_accept_label(d, "_Select");
    GFile *start = existing_folder(lsiw_var_item_get_linux_path(it));
    gtk_file_dialog_set_initial_folder(d, start);
    g_object_unref(start);

    struct pick *pk = g_new0(struct pick, 1);
    pk->page = g_object_ref(p->base.widget);
    pk->name = g_strdup(v->name);
    gtk_file_dialog_select_folder(d, lsiw_gui_window(p->base.widget), NULL, folder_chosen_cb, pk);
    g_object_unref(d);
}

static void act_reset(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct loc_page *p = data;
    LsiwVarItem *it = selected_item(p);
    if (it) apply_change(p, lsiw_var_item_get_var(it)->name, NULL);
}

static void act_open(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct loc_page *p = data;
    LsiwVarItem *it = selected_item(p);
    const char *lin = it ? lsiw_var_item_get_linux_path(it) : NULL;
    if (!lin) return;
    /* LSIW creates configured folders when it first needs them; opening
     * one does the same, so a new folder can be filled right away. */
    if (!g_file_test(lin, G_FILE_TEST_IS_DIR) &&
        lsiw_var_item_effective_kind(it) == LSIW_VAR_FOLDER &&
        g_mkdir_with_parents(lin, 0755) < 0) {
        char *d = g_strdup_printf("%s: %s", lin, g_strerror(errno));
        lsiw_gui_alert(p->base.widget, "The folder could not be created", d);
        g_free(d);
        return;
    }
    lsiw_gui_open_folder(p->base.widget, lin);
}

static void act_edit_value(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct loc_page *p = data;
    LsiwVarItem *it = selected_item(p);
    if (!it) return;
    const struct lsiw_var *v = lsiw_var_item_get_var(it);
    g_free(p->editing);
    p->editing = g_strdup(v->name);
    const char *cur = lsiw_var_item_get_windows_path(it);
    gtk_editable_set_text(GTK_EDITABLE(p->value_entry), cur ? cur : "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->value_revealer), TRUE);
    gtk_widget_grab_focus(p->value_entry);
    gtk_editable_select_region(GTK_EDITABLE(p->value_entry), 0, -1);
}

static void act_value_save(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct loc_page *p = data;
    if (!p->editing) return;
    char *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->value_entry))));
    if (!*text) {
        lsiw_gui_alert(p->base.widget, "The value cannot be empty",
                       "Type a value, or use Reset to default to go back to the default.");
    } else {
        char *name = g_strdup(p->editing);
        hide_value_editor(p);
        apply_change(p, name, text);
        g_free(name);
    }
    g_free(text);
}

static void act_value_cancel(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    hide_value_editor(data);
}

/* loc.set(name, value): the operation behind every button; an empty value
 * resets. Also usable from other code and tests. */
static void act_set(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a;
    const char *name, *value;
    g_variant_get(param, "(&s&s)", &name, &value);
    apply_change(data, name, *value ? value : NULL);
}

static void act_focus_search(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct loc_page *p = data;
    gtk_widget_grab_focus(p->search);
}

static const GActionEntry loc_actions[] = {
    { .name = "change", .activate = act_change },
    { .name = "reset", .activate = act_reset },
    { .name = "open", .activate = act_open },
    { .name = "edit-value", .activate = act_edit_value },
    { .name = "value-save", .activate = act_value_save },
    { .name = "value-cancel", .activate = act_value_cancel },
    { .name = "set", .activate = act_set, .parameter_type = "(ss)" },
    { .name = "focus-search", .activate = act_focus_search },
};

static void row_activated_cb(GtkColumnView *view, guint pos, gpointer data)
{
    (void)view; (void)pos;
    struct loc_page *p = data;
    /* Double-click or Enter: the main action of the row's kind. */
    GAction *change = g_action_map_lookup_action(G_ACTION_MAP(p->actions), "change");
    GAction *edit = g_action_map_lookup_action(G_ACTION_MAP(p->actions), "edit-value");
    if (g_action_get_enabled(change)) g_action_activate(change, NULL);
    else if (g_action_get_enabled(edit)) g_action_activate(edit, NULL);
}

static void value_activate_cb(GtkEntry *entry, gpointer data)
{
    (void)entry;
    act_value_save(NULL, NULL, data);
}

static gboolean value_key_cb(GtkEventControllerKey *c, guint keyval, guint keycode,
                             GdkModifierType state, gpointer data)
{
    (void)c; (void)keycode; (void)state;
    if (keyval != GDK_KEY_Escape) return FALSE;
    hide_value_editor(data);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Columns                                                              */
/* ------------------------------------------------------------------ */

struct col_spec {
    const char *title;
    const char *prop;         /* LsiwVarItem property shown on the first line */
    const char *sub_prop;     /* optional second line, dimmed                 */
    gboolean markup;
    gboolean expand;
    int width;
    GtkInscriptionOverflow overflow;
};

static const struct col_spec columns[] = {
    { "Variable", "title-markup", "description", TRUE, FALSE, 330, GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_END },
    { "Kind", "kind", "scope", FALSE, FALSE, 100, GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_END },
    { "Windows path", "windows-path", NULL, FALSE, TRUE, 220, GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_MIDDLE },
    { "Linux location", "linux-path", NULL, FALSE, TRUE, 220, GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_MIDDLE },
    { "Status", "status-markup", NULL, TRUE, FALSE, 80, GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_END },
};

/* Bind widget property wprop to the cell's item property iprop; the
 * expression follows both the cell's item and the item's notifications. */
static void bind_item_prop(GtkWidget *w, const char *wprop, const char *iprop, GtkColumnViewCell *cell)
{
    GtkExpression *e = gtk_property_expression_new(GTK_TYPE_COLUMN_VIEW_CELL, NULL, "item");
    e = gtk_property_expression_new(LSIW_TYPE_VAR_ITEM, e, iprop);
    gtk_expression_bind(e, w, wprop, cell);
}

static GtkWidget *cell_line(const struct col_spec *spec, const char *prop, gboolean markup,
                            GtkColumnViewCell *cell)
{
    GtkWidget *ins = gtk_inscription_new(NULL);
    gtk_inscription_set_text_overflow(GTK_INSCRIPTION(ins), spec->overflow);
    gtk_inscription_set_min_chars(GTK_INSCRIPTION(ins), 4);
    /* Never draw into the next column, whatever Pango makes of a long path. */
    gtk_widget_set_overflow(ins, GTK_OVERFLOW_HIDDEN);
    bind_item_prop(ins, markup ? "markup" : "text", prop, cell);
    bind_item_prop(ins, "tooltip-text", "tooltip", cell);
    return ins;
}

static void cell_setup_cb(GtkSignalListItemFactory *f, GObject *obj, gpointer data)
{
    (void)f;
    const struct col_spec *spec = data;
    GtkColumnViewCell *cell = GTK_COLUMN_VIEW_CELL(obj);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_box_append(GTK_BOX(box), cell_line(spec, spec->prop, spec->markup, cell));
    if (spec->sub_prop) {
        GtkWidget *sub = cell_line(spec, spec->sub_prop, FALSE, cell);
        gtk_widget_add_css_class(sub, "lsiw-dim");
        gtk_widget_add_css_class(sub, "lsiw-small");
        gtk_box_append(GTK_BOX(box), sub);
    }
    gtk_column_view_cell_set_child(cell, box);
}

static GtkWidget *build_view(struct loc_page *p)
{
    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(p->selection)));
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view), TRUE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(view), FALSE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Windows location variables", -1);
    for (size_t i = 0; i < G_N_ELEMENTS(columns); i++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(cell_setup_cb), (gpointer)&columns[i]);
        GtkColumnViewColumn *c = gtk_column_view_column_new(columns[i].title, f);
        gtk_column_view_column_set_resizable(c, TRUE);
        gtk_column_view_column_set_expand(c, columns[i].expand);
        gtk_column_view_column_set_fixed_width(c, columns[i].width);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view), c);
        g_object_unref(c);
    }
    g_signal_connect(view, "activate", G_CALLBACK(row_activated_cb), p);
    return view;
}

/* ------------------------------------------------------------------ */
/* Page                                                                 */
/* ------------------------------------------------------------------ */

static void loc_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    (void)reason;
    struct loc_page *p = (struct loc_page *)base;
    gboolean changed = FALSE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = 0; i < n; i++) {
        LsiwVarItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
        changed |= lsiw_var_item_refresh(it);
        g_object_unref(it);
    }
    /* Paths are part of what the search matches. */
    if (changed) filter_changed(p);
    update_details(p);
}

static void loc_free(gpointer data)
{
    struct loc_page *p = data;
    g_object_unref(p->selection);
    g_object_unref(p->store);
    g_strfreev(p->needles);
    g_free(p->editing);
    g_free(p);
}

static void filtered_items_cb(GListModel *m, guint pos, guint removed, guint added, gpointer data)
{
    (void)m; (void)pos; (void)removed; (void)added;
    update_count(data);
}

GtkWidget *lsiw_page_locations_new(void)
{
    struct loc_page *p = g_new0(struct loc_page, 1);
    p->base.refresh = loc_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, loc_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), loc_actions,
                                    G_N_ELEMENTS(loc_actions), p);
    gtk_widget_insert_action_group(page, "loc", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);    /* the page holds it */

    GtkEventController *sc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>f"),
                         gtk_named_action_new("loc.focus-search")));
    gtk_widget_add_controller(page, sc);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Windows folder locations"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "Every folder a Windows program asks for (Documents, AppData, Program Files, the "
        "Windows folder, Temp…) is a variable with a Linux location you choose. Programs see "
        "it at the Windows path on the single C: drive."));

    /* Search and filters */
    GtkWidget *bar = lsiw_gui_wrap_row(12);
    p->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(p->search),
                                          "Search names, descriptions and paths");
    gtk_widget_set_hexpand(p->search, TRUE);
    g_signal_connect(p->search, "search-changed", G_CALLBACK(search_changed_cb), p);
    lsiw_gui_wrap_row_append(bar, p->search);

    static const char *const scopes[] = { "All scopes", "User", "System", NULL };
    p->scope = gtk_drop_down_new_from_strings(scopes);
    gtk_widget_set_tooltip_text(p->scope, "User variables belong to your Windows profile; "
                                          "system variables are shared by all users");
    g_signal_connect(p->scope, "notify::selected", G_CALLBACK(option_changed_cb), p);
    lsiw_gui_wrap_row_append(bar, p->scope);

    p->usmt = gtk_check_button_new_with_mnemonic("Show only _USMT variables");
    gtk_widget_set_tooltip_text(p->usmt, "Only the variables Microsoft's User State Migration "
                                         "Tool documentation lists");
    g_signal_connect(p->usmt, "toggled", G_CALLBACK(usmt_toggled_cb), p);
    lsiw_gui_wrap_row_append(bar, p->usmt);
    gtk_box_append(GTK_BOX(page), bar);

    /* The list */
    p->store = g_list_store_new(LSIW_TYPE_VAR_ITEM);
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++) {
        LsiwVarItem *it = lsiw_var_item_new(&t[i]);
        g_list_store_append(p->store, it);
        g_object_unref(it);
    }
    p->filter = GTK_FILTER(gtk_custom_filter_new(filter_match, p, NULL));
    p->filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(p->store)), p->filter);
    p->selection = gtk_single_selection_new(G_LIST_MODEL(p->filtered));
    gtk_single_selection_set_autoselect(p->selection, FALSE);
    gtk_single_selection_set_can_unselect(p->selection, TRUE);
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->selection));
    g_signal_connect(p->selection, "notify::selected-item", G_CALLBACK(selection_cb), p);
    g_signal_connect(p->filtered, "items-changed", G_CALLBACK(filtered_items_cb), p);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), build_view(p));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), sw);
    gtk_box_append(GTK_BOX(page), frame);

    p->count = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->count), 0);
    gtk_widget_add_css_class(p->count, "lsiw-dim");
    gtk_box_append(GTK_BOX(page), p->count);

    /* Details of the selected variable and its actions */
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    p->detail_title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->detail_title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->detail_title), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(details), p->detail_title);
    p->detail_hint = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->detail_hint), 0);
    gtk_label_set_wrap(GTK_LABEL(p->detail_hint), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(p->detail_hint), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(p->detail_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(p->detail_hint), TRUE);
    gtk_box_append(GTK_BOX(details), p->detail_hint);

    GtkWidget *buttons = lsiw_gui_wrap_row(6);
    GtkWidget *change = lsiw_gui_action_button("_Change location…", "loc.change");
    gtk_widget_add_css_class(change, "suggested-action");
    lsiw_gui_wrap_row_append(buttons, change);
    lsiw_gui_wrap_row_append(buttons, lsiw_gui_action_button("_Edit value…", "loc.edit-value"));
    lsiw_gui_wrap_row_append(buttons, lsiw_gui_action_button("_Reset to default", "loc.reset"));
    lsiw_gui_wrap_row_append(buttons, lsiw_gui_action_button("_Open folder", "loc.open"));
    gtk_box_append(GTK_BOX(details), buttons);

    /* Inline editor for VALUE variables (USERNAME, COMPUTERNAME, USERSID) */
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *elabel = gtk_label_new_with_mnemonic("New _value:");
    gtk_box_append(GTK_BOX(edit), elabel);
    p->value_entry = gtk_entry_new();
    gtk_widget_set_hexpand(p->value_entry, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(elabel), p->value_entry);
    gtk_entry_set_activates_default(GTK_ENTRY(p->value_entry), FALSE);
    gtk_widget_set_tooltip_text(p->value_entry, "Enter saves, Escape cancels");
    g_signal_connect(p->value_entry, "activate", G_CALLBACK(value_activate_cb), p);
    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(value_key_cb), p);
    gtk_widget_add_controller(p->value_entry, kc);
    gtk_box_append(GTK_BOX(edit), p->value_entry);
    GtkWidget *save = lsiw_gui_action_button("_Save", "loc.value-save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_box_append(GTK_BOX(edit), save);
    gtk_box_append(GTK_BOX(edit), lsiw_gui_action_button("Cancel", "loc.value-cancel"));
    p->value_revealer = gtk_revealer_new();
    gtk_revealer_set_child(GTK_REVEALER(p->value_revealer), edit);
    gtk_box_append(GTK_BOX(details), p->value_revealer);

    GtkWidget *dframe = gtk_frame_new(NULL);
    gtk_widget_set_margin_top(details, 8);
    gtk_widget_set_margin_bottom(details, 8);
    gtk_widget_set_margin_start(details, 10);
    gtk_widget_set_margin_end(details, 10);
    gtk_frame_set_child(GTK_FRAME(dframe), details);
    gtk_box_append(GTK_BOX(page), dframe);

    p->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->status), 0);
    gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(p->status), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(p->status), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(p->status), TRUE);
    gtk_box_append(GTK_BOX(page), p->status);

    gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(p->search), page);
    update_count(p);
    update_details(p);
    return page;
}
