/* LSIW Control: the library view of the Programs page.
 *
 * The programs of the library in one of three layouts the user picks
 * (remembered as [control] programs_layout = grid | list | details):
 *
 *   Grid     big icons with the name and the play time below
 *   List     one row per program: icon, name, engine, path, play time and
 *            a Desktop shortcut button (the layout of LSIW 1.8 and older)
 *   Details  a table with sortable, resizable columns: name, play time,
 *            last played, launches, last session, engine, program
 *
 * and an order ([control] programs_sort = name | playtime | recent |
 * launches). Play time comes from lsiw_playtime_get (src/common/
 * playtime.c); while LSIW Control is open, the view checks every few
 * seconds whether a record changed (a program started from a shortcut,
 * or one that is running) and updates the play time in place.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* One program, as the views show it                                   */
/* ------------------------------------------------------------------ */

#define LSIW_TYPE_PROG_ITEM (lsiw_prog_item_get_type())
G_DECLARE_FINAL_TYPE(LsiwProgItem, lsiw_prog_item, LSIW, PROG_ITEM, GObject)

struct _LsiwProgItem {
    GObject parent;
    char *id, *name, *exe, *engine, *icon;
    struct lsiw_playtime pt;
};

G_DEFINE_TYPE(LsiwProgItem, lsiw_prog_item, G_TYPE_OBJECT)

static void lsiw_prog_item_finalize(GObject *o)
{
    LsiwProgItem *it = LSIW_PROG_ITEM(o);
    g_free(it->id);
    g_free(it->name);
    g_free(it->exe);
    g_free(it->engine);
    g_free(it->icon);
    G_OBJECT_CLASS(lsiw_prog_item_parent_class)->finalize(o);
}

static void lsiw_prog_item_class_init(LsiwProgItemClass *c) { G_OBJECT_CLASS(c)->finalize = lsiw_prog_item_finalize; }
static void lsiw_prog_item_init(LsiwProgItem *it) { (void)it; }

static char *engine_summary(const struct lsiw_program *pr)
{
    const char *e = *pr->engine ? pr->engine : "default engine";
    const char *pf = !*pr->prefix || !g_ascii_strcasecmp(pr->prefix, "shared") ? "shared prefix"
                   : !g_ascii_strcasecmp(pr->prefix, "own") ? "own prefix" : pr->prefix;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "%s · %s", e, pf);
    if (pr->dxvk >= 0) g_string_append_printf(s, " · DXVK %s", pr->dxvk ? "on" : "off");
    if (pr->vkd3d >= 0) g_string_append_printf(s, " · VKD3D %s", pr->vkd3d ? "on" : "off");
    if (pr->mangohud) g_string_append(s, " · MangoHud");
    if (pr->debug) g_string_append(s, " · debug log");
    return g_string_free(s, FALSE);
}

static LsiwProgItem *item_new(const struct lsiw_program *pr)
{
    LsiwProgItem *it = g_object_new(LSIW_TYPE_PROG_ITEM, NULL);
    it->id = g_strdup(pr->id);
    it->name = g_strdup(pr->name);
    it->exe = g_strdup(pr->exe);
    it->engine = engine_summary(pr);
    char *icon = lsiw_program_icon(pr, 0);     /* cached as a PNG; made once from the .exe */
    it->icon = g_strdup(icon);
    free(icon);
    lsiw_playtime_get(pr->id, &it->pt);
    return it;
}

static GtkWidget *item_image(LsiwProgItem *it, int size)
{
    GtkWidget *img = it->icon ? gtk_image_new_from_file(it->icon) : gtk_image_new_from_icon_name("application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), size);
    return img;
}

static long long last_played(const struct lsiw_playtime *pt)
{
    return pt->last_end ? pt->last_end : pt->last_start;
}

/* "3 h 05 min", "Never played", "Running now · 3 h 05 min" */
static char *playtime_short(const struct lsiw_playtime *pt)
{
    if (!pt->launches) return g_strdup("Never played");
    char *t = lsiw_playtime_format(pt->total);
    char *s = pt->running ? g_strdup_printf("Running now · %s", t) : g_strdup(t);
    free(t);
    return s;
}

/* "Played 3 h 05 min · 12 launches · last played today 14:02" */
static char *playtime_line(const struct lsiw_playtime *pt)
{
    if (!pt->launches) return g_strdup("Never played");
    char *t = lsiw_playtime_format(pt->total), *w = lsiw_playtime_when(last_played(pt));
    char *s = g_strdup_printf("%sPlayed %s · %lld launch%s · last played %s",
                              pt->running ? "Running now · " : "", t, pt->launches,
                              pt->launches == 1 ? "" : "es", w);
    free(t);
    free(w);
    return s;
}

static char *playtime_tooltip(LsiwProgItem *it)
{
    const struct lsiw_playtime *pt = &it->pt;
    if (!pt->launches) return g_strdup_printf("%s\nNot started from LSIW yet", it->name);
    char *t = lsiw_playtime_format(pt->total), *w = lsiw_playtime_when(last_played(pt));
    char *f = lsiw_playtime_when(pt->first), *l = lsiw_playtime_format(pt->last_session);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "%s\nPlay time: %s in %lld session%s\nLast played: %s, for %s (%s)\nFirst played: %s",
                           it->name, t, pt->launches, pt->launches == 1 ? "" : "s", w, l,
                           *pt->last_result ? pt->last_result : "unknown", f);
    if (pt->running) g_string_append(s, "\nRunning now");
    if (pt->unclean)
        g_string_append_printf(s, "\n%lld session%s ended without LSIW seeing it (counted until the last save)",
                               pt->unclean, pt->unclean == 1 ? "" : "s");
    free(t); free(w); free(f); free(l);
    return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------ */
/* The view                                                             */
/* ------------------------------------------------------------------ */

enum { LAYOUT_GRID, LAYOUT_LIST, LAYOUT_DETAILS, N_LAYOUTS };
static const char *const layout_ids[] = { "grid", "list", "details" };
static const char *const layout_titles[] = { "Grid", "List", "Details" };
static const char *const layout_icons[] = { "view-grid-symbolic", "view-list-symbolic", "view-continuous-symbolic" };

enum { SORT_NAME, SORT_PLAYTIME, SORT_RECENT, SORT_LAUNCHES, N_SORTS };
static const char *const sort_ids[] = { "name", "playtime", "recent", "launches" };
static const char *const sort_titles[] = { "Name", "Most played", "Recently played", "Most launched", NULL };

struct LsiwLibView {
    GtkWidget *root, *stack, *summary, *sort_dd, *layout_btn[N_LAYOUTS];
    GtkWidget *list, *flow, *cv, *sw[N_LAYOUTS];
    GtkColumnViewColumn *col[8];
    GListStore *store;
    GtkSingleSelection *cv_sel;
    GHashTable *pt_labels;        /* id -> GtkLabel showing its play time (grid, list) */
    char *selected;
    int layout, sort, quiet;
    guint timer;
    long long stamp;
    LsiwLibViewChanged changed;
    LsiwLibViewActivated activated, shortcut;
    gpointer data;
};

static int index_of(const char *const *ids, int n, const char *v, int def)
{
    for (int i = 0; v && i < n; i++) if (!g_ascii_strcasecmp(ids[i], v)) return i;
    return def;
}

static void set_selected(LsiwLibView *v, const char *id)
{
    if (!g_strcmp0(v->selected, id)) return;
    g_free(v->selected);
    v->selected = g_strdup(id);
    if (!v->quiet && v->changed) v->changed(v->data);
}

const char *lsiw_libview_selected(LsiwLibView *v) { return v->selected; }

static int compare_items(gconstpointer a, gconstpointer b, gpointer data)
{
    const LsiwProgItem *x = a, *y = b;
    LsiwLibView *v = data;
    long long d = 0;
    if (v->sort == SORT_PLAYTIME) d = y->pt.total - x->pt.total;
    else if (v->sort == SORT_RECENT) d = last_played(&y->pt) - last_played(&x->pt);
    else if (v->sort == SORT_LAUNCHES) d = y->pt.launches - x->pt.launches;
    if (d) return d > 0 ? 1 : -1;
    int c = g_ascii_strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->id, y->id);
}

/* ---- list ---- */

static void row_shortcut_clicked(GtkButton *b, gpointer data)
{
    LsiwLibView *v = data;
    const char *id = g_object_get_data(G_OBJECT(b), "lsiw-id");
    GtkWidget *row = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
    if (row) gtk_list_box_select_row(GTK_LIST_BOX(v->list), GTK_LIST_BOX_ROW(row));
    if (id && v->shortcut) v->shortcut(id, v->data);
}

static GtkWidget *list_row(LsiwLibView *v, LsiwProgItem *it)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(outer, 4);
    gtk_widget_set_margin_bottom(outer, 4);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 8);
    GtkWidget *img = item_image(it, 32);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(outer), img);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_box_append(GTK_BOX(outer), box);
    GtkWidget *name = gtk_label_new(it->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(name, "lsiw-bold");
    gtk_box_append(GTK_BOX(box), name);
    char *line = g_strdup_printf("%s\n%s", it->engine, it->exe);
    GtkWidget *d = gtk_label_new(line);
    gtk_label_set_xalign(GTK_LABEL(d), 0);
    gtk_label_set_ellipsize(GTK_LABEL(d), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(d, "lsiw-dim");
    gtk_box_append(GTK_BOX(box), d);
    g_free(line);
    GtkWidget *pt = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(pt), 0);
    gtk_label_set_ellipsize(GTK_LABEL(pt), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(pt, "lsiw-playtime");
    gtk_box_append(GTK_BOX(box), pt);
    g_hash_table_insert(v->pt_labels, g_strdup(it->id), pt);
    GtkWidget *mk = gtk_button_new_with_label("Desktop shortcut");
    gtk_widget_set_tooltip_text(mk, "Desktop shortcut: put an LSIW shortcut (.lwlnk) with this program's "
                                    "icon on the desktop");
    gtk_widget_set_valign(mk, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(mk), "lsiw-id", g_strdup(it->id), g_free);
    g_signal_connect(mk, "clicked", G_CALLBACK(row_shortcut_clicked), v);
    gtk_box_append(GTK_BOX(outer), mk);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
    g_object_set_data_full(G_OBJECT(row), "lsiw-id", g_strdup(it->id), g_free);
    return row;
}

static void list_selected_cb(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    LsiwLibView *v = data;
    if (v->layout == LAYOUT_LIST) set_selected(v, row ? g_object_get_data(G_OBJECT(row), "lsiw-id") : NULL);
}

static void list_activated_cb(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    LsiwLibView *v = data;
    const char *id = g_object_get_data(G_OBJECT(row), "lsiw-id");
    if (id && v->activated) v->activated(id, v->data);
}

/* ---- grid ---- */

static GtkWidget *grid_tile(LsiwLibView *v, LsiwProgItem *it)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(box, 132, -1);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    GtkWidget *img = item_image(it, 64);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), img);
    GtkWidget *name = gtk_label_new(it->name);
    gtk_label_set_wrap(GTK_LABEL(name), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(name), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(name), 2);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_justify(GTK_LABEL(name), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 14);
    gtk_label_set_width_chars(GTK_LABEL(name), 14);
    gtk_widget_add_css_class(name, "lsiw-bold");
    gtk_box_append(GTK_BOX(box), name);
    GtkWidget *pt = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(pt), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(pt), 16);
    gtk_widget_add_css_class(pt, "lsiw-dim");
    gtk_box_append(GTK_BOX(box), pt);
    g_hash_table_insert(v->pt_labels, g_strdup(it->id), pt);
    GtkWidget *child = gtk_flow_box_child_new();
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), box);
    g_object_set_data_full(G_OBJECT(child), "lsiw-id", g_strdup(it->id), g_free);
    return child;
}

static void flow_selected_cb(GtkFlowBox *box, gpointer data)
{
    LsiwLibView *v = data;
    if (v->layout != LAYOUT_GRID) return;
    GList *l = gtk_flow_box_get_selected_children(box);
    set_selected(v, l ? g_object_get_data(G_OBJECT(l->data), "lsiw-id") : NULL);
    g_list_free(l);
}

static void flow_activated_cb(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer data)
{
    (void)box;
    LsiwLibView *v = data;
    const char *id = g_object_get_data(G_OBJECT(child), "lsiw-id");
    if (id && v->activated) v->activated(id, v->data);
}

/* ---- details ---- */

enum { COL_NAME, COL_PLAYTIME, COL_LAST, COL_LAUNCHES, COL_SESSION, COL_ENGINE, COL_PROGRAM, N_COLS };

static char *cell_text(LsiwProgItem *it, int col)
{
    const struct lsiw_playtime *pt = &it->pt;
    switch (col) {
    case COL_NAME: return g_strdup(it->name);
    case COL_PLAYTIME:
        if (!pt->launches) return g_strdup("—");
        {
            char *t = lsiw_playtime_format(pt->total);
            char *s = pt->running ? g_strdup_printf("%s (running)", t) : g_strdup(t);
            free(t);
            return s;
        }
    case COL_LAST: {
        if (pt->running) return g_strdup("Now");
        char *w = lsiw_playtime_when(last_played(pt)), *s = g_strdup(pt->launches ? w : "Never");
        free(w);
        return s;
    }
    case COL_LAUNCHES: return g_strdup_printf("%lld", pt->launches);
    case COL_SESSION: {
        if (!pt->launches) return g_strdup("—");
        char *l = lsiw_playtime_format(pt->last_session);
        char *s = *pt->last_result && strcmp(pt->last_result, "exit 0") && !pt->running
            ? g_strdup_printf("%s (%s)", l, pt->last_result) : g_strdup(l);
        free(l);
        return s;
    }
    case COL_ENGINE: return g_strdup(it->engine);
    default: return g_strdup(it->exe);
    }
}

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    (void)f;
    int col = GPOINTER_TO_INT(data);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), col == COL_LAUNCHES ? 1 : 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), col == COL_PROGRAM ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_END);
    if (col == COL_NAME) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *img = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(img), 24);
        gtk_box_append(GTK_BOX(box), img);
        gtk_box_append(GTK_BOX(box), label);
        g_object_set_data(G_OBJECT(box), "lsiw-image", img);
        g_object_set_data(G_OBJECT(box), "lsiw-label", label);
        gtk_list_item_set_child(li, box);
    } else {
        if (col == COL_ENGINE || col == COL_PROGRAM) gtk_widget_add_css_class(label, "lsiw-dim");
        g_object_set_data(G_OBJECT(label), "lsiw-label", label);
        gtk_list_item_set_child(li, label);
    }
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    (void)f;
    int col = GPOINTER_TO_INT(data);
    LsiwProgItem *it = gtk_list_item_get_item(li);
    GtkWidget *child = gtk_list_item_get_child(li);
    GtkWidget *label = g_object_get_data(G_OBJECT(child), "lsiw-label");
    char *t = cell_text(it, col);
    gtk_label_set_text(GTK_LABEL(label), t);
    g_free(t);
    if (col == COL_NAME) {
        GtkWidget *img = g_object_get_data(G_OBJECT(child), "lsiw-image");
        if (it->icon) gtk_image_set_from_file(GTK_IMAGE(img), it->icon);
        else gtk_image_set_from_icon_name(GTK_IMAGE(img), "application-x-executable");
        gtk_image_set_pixel_size(GTK_IMAGE(img), 24);
        char *tip = playtime_tooltip(it);
        gtk_widget_set_tooltip_text(child, tip);
        g_free(tip);
    } else if (col == COL_PROGRAM) gtk_widget_set_tooltip_text(child, it->exe);
}

static int cmp_col(gconstpointer a, gconstpointer b, gpointer data)
{
    const LsiwProgItem *x = a, *y = b;
    int col = GPOINTER_TO_INT(data);
    long long d = 0;
    switch (col) {
    case COL_PLAYTIME: d = x->pt.total - y->pt.total; break;
    case COL_LAST: d = last_played(&x->pt) - last_played(&y->pt); break;
    case COL_LAUNCHES: d = x->pt.launches - y->pt.launches; break;
    case COL_SESSION: d = x->pt.last_session - y->pt.last_session; break;
    case COL_ENGINE: d = g_ascii_strcasecmp(x->engine, y->engine); break;
    case COL_PROGRAM: d = strcmp(x->exe, y->exe); break;
    default: break;
    }
    if (d) return d > 0 ? 1 : -1;
    return g_ascii_strcasecmp(x->name, y->name);
}

static void cv_selected_cb(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    LsiwLibView *v = data;
    if (v->layout != LAYOUT_DETAILS) return;
    LsiwProgItem *it = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(o));
    set_selected(v, it ? it->id : NULL);
}

static void cv_activated_cb(GtkColumnView *cv, guint pos, gpointer data)
{
    (void)cv;
    LsiwLibView *v = data;
    LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->cv_sel), pos);
    if (it && v->activated) v->activated(it->id, v->data);
    g_clear_object(&it);
}

static GtkWidget *details_new(LsiwLibView *v)
{
    static const char *const titles[N_COLS] = {
        "Name", "Play time", "Last played", "Launches", "Last session", "Engine", "Program"
    };
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(v->store)), NULL);
    v->cv_sel = gtk_single_selection_new(G_LIST_MODEL(sorted));
    gtk_single_selection_set_autoselect(v->cv_sel, FALSE);
    gtk_single_selection_set_can_unselect(v->cv_sel, TRUE);
    gtk_single_selection_set_selected(v->cv_sel, GTK_INVALID_LIST_POSITION);
    g_signal_connect(v->cv_sel, "notify::selected-item", G_CALLBACK(cv_selected_cb), v);
    GtkWidget *cv = gtk_column_view_new(GTK_SELECTION_MODEL(v->cv_sel));
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(cv), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(cv), FALSE);
    gtk_widget_add_css_class(cv, "data-table");
    for (int c = 0; c < N_COLS; c++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(cell_setup), GINT_TO_POINTER(c));
        g_signal_connect(f, "bind", G_CALLBACK(cell_bind), GINT_TO_POINTER(c));
        GtkColumnViewColumn *col = gtk_column_view_column_new(titles[c], f);
        GtkSorter *s = GTK_SORTER(gtk_custom_sorter_new(cmp_col, GINT_TO_POINTER(c), NULL));
        gtk_column_view_column_set_sorter(col, s);
        g_object_unref(s);
        gtk_column_view_column_set_resizable(col, TRUE);
        if (c == COL_NAME) gtk_column_view_column_set_fixed_width(col, 220);
        if (c == COL_PROGRAM) gtk_column_view_column_set_expand(col, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(cv), col);
        v->col[c] = col;
        g_object_unref(col);
    }
    gtk_sort_list_model_set_sorter(sorted, gtk_column_view_get_sorter(GTK_COLUMN_VIEW(cv)));
    g_signal_connect(cv, "activate", G_CALLBACK(cv_activated_cb), v);
    return cv;
}

/* The column and direction of the chosen order. */
static void details_sort(LsiwLibView *v)
{
    static const int col_of[N_SORTS] = { COL_NAME, COL_PLAYTIME, COL_LAST, COL_LAUNCHES };
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(v->cv), NULL, GTK_SORT_ASCENDING);   /* one arrow only */
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(v->cv), v->col[col_of[v->sort]],
                                   v->sort == SORT_NAME ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
}

/* ---- filling ---- */

static void update_playtime_labels(LsiwLibView *v)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(v->store));
    long long total = 0;
    int running = 0;
    for (guint i = 0; i < n; i++) {
        LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->store), i);
        total += it->pt.total;
        running += it->pt.running > 0;
        GtkWidget *l = g_hash_table_lookup(v->pt_labels, it->id);
        if (l) {
            char *t = v->layout == LAYOUT_GRID ? playtime_short(&it->pt) : playtime_line(&it->pt);
            gtk_label_set_text(GTK_LABEL(l), t);
            g_free(t);
            GtkWidget *holder = v->layout == LAYOUT_GRID
                ? gtk_widget_get_ancestor(l, GTK_TYPE_FLOW_BOX_CHILD)
                : gtk_widget_get_ancestor(l, GTK_TYPE_LIST_BOX_ROW);
            char *tip = playtime_tooltip(it);
            if (holder) gtk_widget_set_tooltip_text(holder, tip);
            g_free(tip);
        }
        g_object_unref(it);
    }
    GString *g = g_string_new(NULL);
    if (n) {
        char *t = lsiw_playtime_format(total);
        g_string_append_printf(g, "%u program%s · %s played", n, n == 1 ? "" : "s", total ? t : "nothing");
        free(t);
    }
    for (guint i = 0, k = 0; running && i < n; i++) {
        LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->store), i);
        if (it->pt.running) g_string_append_printf(g, "%s%s", k++ ? ", " : " · running now: ", it->name);
        g_object_unref(it);
    }
    if (!lsiw_playtime_enabled()) g_string_append_printf(g, "%splay time tracking is off", g->len ? " · " : "");
    char *s = g_string_free(g, FALSE);
    gtk_label_set_text(GTK_LABEL(v->summary), s);
    g_free(s);
}

/* Rebuild the active layout's widgets from the store. */
static void build_view(LsiwLibView *v)
{
    v->quiet++;
    g_hash_table_remove_all(v->pt_labels);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(v->store));
    if (v->layout == LAYOUT_LIST) {
        gtk_list_box_remove_all(GTK_LIST_BOX(v->list));
        for (guint i = 0; i < n; i++) {
            LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->store), i);
            GtkWidget *row = list_row(v, it);
            gtk_list_box_append(GTK_LIST_BOX(v->list), row);
            if (v->selected && !strcmp(v->selected, it->id))
                gtk_list_box_select_row(GTK_LIST_BOX(v->list), GTK_LIST_BOX_ROW(row));
            g_object_unref(it);
        }
    } else if (v->layout == LAYOUT_GRID) {
        gtk_flow_box_remove_all(GTK_FLOW_BOX(v->flow));
        for (guint i = 0; i < n; i++) {
            LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->store), i);
            GtkWidget *child = grid_tile(v, it);
            gtk_flow_box_append(GTK_FLOW_BOX(v->flow), child);
            if (v->selected && !strcmp(v->selected, it->id))
                gtk_flow_box_select_child(GTK_FLOW_BOX(v->flow), GTK_FLOW_BOX_CHILD(child));
            g_object_unref(it);
        }
    } else {
        details_sort(v);
        guint m = g_list_model_get_n_items(G_LIST_MODEL(v->cv_sel)), pos = GTK_INVALID_LIST_POSITION;
        for (guint i = 0; v->selected && i < m; i++) {
            LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->cv_sel), i);
            if (!strcmp(it->id, v->selected)) pos = i;
            g_object_unref(it);
        }
        gtk_single_selection_set_selected(v->cv_sel, pos);
    }
    /* A page scrolls as a whole and gives the view its minimum height
     * only: room for a few rows (or two rows of tiles) before it scrolls. */
    if (v->layout != LAYOUT_GRID) {     /* the grid grows with its tiles */
        int h = v->layout == LAYOUT_LIST ? (int)n * 84 + 4 : (int)n * 37 + 40;
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(v->sw[v->layout]), CLAMP(h, 150, 430));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(v->stack), n ? layout_ids[v->layout] : "empty");
    update_playtime_labels(v);
    v->quiet--;
}

void lsiw_libview_reload(LsiwLibView *v, const char *select)
{
    char *keep = g_strdup(select ? select : v->selected);
    struct lsiw_program *pv;
    size_t n = lsiw_programs(&pv);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        g_ptr_array_add(items, item_new(&pv[i]));
        if (keep && !strcmp(keep, pv[i].id)) found = 1;
    }
    lsiw_programs_free(pv, n);
    g_list_store_splice(v->store, 0, g_list_model_get_n_items(G_LIST_MODEL(v->store)), items->pdata, items->len);
    g_ptr_array_unref(items);
    g_list_store_sort(v->store, compare_items, v);
    v->stamp = lsiw_playtime_stamp();
    v->quiet++;
    set_selected(v, found ? keep : NULL);
    v->quiet--;
    build_view(v);
    g_free(keep);
    if (v->changed) v->changed(v->data);
}

/* Play time only: new numbers in the same widgets. */
static void refresh_playtime(LsiwLibView *v)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(v->store));
    for (guint i = 0; i < n; i++) {
        LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->store), i);
        lsiw_playtime_get(it->id, &it->pt);
        g_object_unref(it);
    }
    v->stamp = lsiw_playtime_stamp();
    if (v->layout == LAYOUT_DETAILS && n) {
        /* The same items again, so the cells are bound anew. */
        gpointer *same = g_new(gpointer, n);
        for (guint i = 0; i < n; i++) same[i] = g_list_model_get_item(G_LIST_MODEL(v->store), i);
        v->quiet++;
        g_list_store_splice(v->store, 0, n, same, n);
        v->quiet--;
        for (guint i = 0; i < n; i++) g_object_unref(same[i]);
        g_free(same);
        char *keep = g_strdup(v->selected);
        guint m = g_list_model_get_n_items(G_LIST_MODEL(v->cv_sel)), pos = GTK_INVALID_LIST_POSITION;
        for (guint i = 0; keep && i < m; i++) {
            LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->cv_sel), i);
            if (!strcmp(it->id, keep)) pos = i;
            g_object_unref(it);
        }
        v->quiet++;
        gtk_single_selection_set_selected(v->cv_sel, pos);
        v->quiet--;
        g_free(v->selected);
        v->selected = keep;
    }
    update_playtime_labels(v);
}

static gboolean poll_cb(gpointer data)
{
    LsiwLibView *v = data;
    if (!gtk_widget_get_mapped(v->root)) return G_SOURCE_CONTINUE;
    long long st = lsiw_playtime_stamp();
    int running = 0;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(v->store));
    for (guint i = 0; i < n && !running; i++) {
        LsiwProgItem *it = g_list_model_get_item(G_LIST_MODEL(v->store), i);
        running = it->pt.running;
        g_object_unref(it);
    }
    /* A running program's session may have ended without a change of the
     * folder (a killed watcher): look again now and then while one runs. */
    if (st != v->stamp || running) refresh_playtime(v);
    return G_SOURCE_CONTINUE;
}

/* ---- the toolbar ---- */

static void save_choice(LsiwLibView *v, const char *key, const char *value)
{
    const char *cur = lsiw_setting_get("control", key, NULL);
    if (cur && !strcmp(cur, value)) return;
    lsiw_setting_set("control", key, value);
    lsiw_gui_save_config(v->root);
}

static void set_layout(LsiwLibView *v, int layout, gboolean save)
{
    if (layout < 0 || layout >= N_LAYOUTS) layout = LAYOUT_GRID;
    v->layout = layout;
    v->quiet++;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->layout_btn[layout]), TRUE);
    v->quiet--;
    if (save) save_choice(v, "programs_layout", layout_ids[layout]);
    /* Only the active layout has widgets; the others are emptied. */
    if (layout != LAYOUT_LIST) gtk_list_box_remove_all(GTK_LIST_BOX(v->list));
    if (layout != LAYOUT_GRID) gtk_flow_box_remove_all(GTK_FLOW_BOX(v->flow));
    build_view(v);
}

static void layout_toggled(GtkToggleButton *b, gpointer data)
{
    LsiwLibView *v = data;
    if (v->quiet || !gtk_toggle_button_get_active(b)) return;
    int l = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lsiw-layout"));
    if (l != v->layout) set_layout(v, l, TRUE);
}

static void sort_changed(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    LsiwLibView *v = data;
    guint s = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    if (s >= N_SORTS || (int)s == v->sort) return;
    v->sort = (int)s;
    save_choice(v, "programs_sort", sort_ids[s]);
    g_list_store_sort(v->store, compare_items, v);
    build_view(v);
}

static GtkWidget *layout_button(LsiwLibView *v, int l, GtkToggleButton *group)
{
    GtkWidget *b = gtk_toggle_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && gtk_icon_theme_has_icon(theme, layout_icons[l]))
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(layout_icons[l]));
    gtk_box_append(GTK_BOX(box), gtk_label_new(layout_titles[l]));
    gtk_button_set_child(GTK_BUTTON(b), box);
    char *tip = g_strdup_printf("Show the library as %s", l == LAYOUT_GRID ? "a grid of big icons"
                                : l == LAYOUT_LIST ? "a list with each program's settings"
                                : "a table with play time, launches and more (click a header to sort)");
    gtk_widget_set_tooltip_text(b, tip);
    g_free(tip);
    if (group) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), group);
    g_object_set_data(G_OBJECT(b), "lsiw-layout", GINT_TO_POINTER(l));
    g_signal_connect(b, "toggled", G_CALLBACK(layout_toggled), v);
    v->layout_btn[l] = b;
    return b;
}

static void libview_free(gpointer data)
{
    LsiwLibView *v = data;
    if (v->timer) g_source_remove(v->timer);
    g_hash_table_unref(v->pt_labels);
    g_object_unref(v->store);
    g_free(v->selected);
    g_free(v);
}

static GtkWidget *scrolled(GtkWidget *child, int min_height)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), min_height);
    return sw;
}

LsiwLibView *lsiw_libview_new(LsiwLibViewChanged changed, LsiwLibViewActivated activated,
                              LsiwLibViewActivated shortcut, gpointer data)
{
    LsiwLibView *v = g_new0(LsiwLibView, 1);
    v->changed = changed;
    v->activated = activated;
    v->shortcut = shortcut;
    v->data = data;
    v->store = g_list_store_new(LSIW_TYPE_PROG_ITEM);
    v->pt_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    v->layout = index_of(layout_ids, N_LAYOUTS, lsiw_setting_get("control", "programs_layout", NULL), LAYOUT_LIST);
    v->sort = index_of(sort_ids, N_SORTS, lsiw_setting_get("control", "programs_sort", NULL), SORT_NAME);

    v->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    g_object_set_data_full(G_OBJECT(v->root), "lsiw-libview", v, libview_free);

    GtkWidget *bar = lsiw_gui_wrap_row(8);
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "linked");
    GtkWidget *first = NULL;
    for (int l = 0; l < N_LAYOUTS; l++) {
        GtkWidget *b = layout_button(v, l, first ? GTK_TOGGLE_BUTTON(first) : NULL);
        if (!first) first = b;
        gtk_box_append(GTK_BOX(seg), b);
    }
    GtkWidget *lab = gtk_label_new_with_mnemonic("La_yout");
    gtk_label_set_mnemonic_widget(GTK_LABEL(lab), first);
    lsiw_gui_wrap_row_append(bar, lab);
    lsiw_gui_wrap_row_append(bar, seg);
    v->sort_dd = gtk_drop_down_new_from_strings(sort_titles);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v->sort_dd), (guint)v->sort);
    gtk_widget_set_tooltip_text(v->sort_dd, "The order of the programs");
    g_signal_connect(v->sort_dd, "notify::selected", G_CALLBACK(sort_changed), v);
    GtkWidget *sl = gtk_label_new_with_mnemonic("S_ort by");
    gtk_label_set_mnemonic_widget(GTK_LABEL(sl), v->sort_dd);
    lsiw_gui_wrap_row_append(bar, sl);
    lsiw_gui_wrap_row_append(bar, v->sort_dd);
    v->summary = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(v->summary), 0);
    gtk_label_set_wrap(GTK_LABEL(v->summary), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(v->summary), 16);
    gtk_widget_set_hexpand(v->summary, TRUE);
    gtk_widget_add_css_class(v->summary, "lsiw-dim");
    lsiw_gui_wrap_row_append(bar, v->summary);
    gtk_box_append(GTK_BOX(v->root), bar);

    v->stack = gtk_stack_new();
    gtk_stack_set_vhomogeneous(GTK_STACK(v->stack), FALSE);
    gtk_stack_set_hhomogeneous(GTK_STACK(v->stack), FALSE);

    v->flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(v->flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(v->flow), FALSE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(v->flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(v->flow), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(v->flow), 64);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(v->flow), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(v->flow), 4);
    gtk_widget_set_valign(v->flow, GTK_ALIGN_START);
    gtk_widget_set_margin_top(v->flow, 4);
    gtk_widget_set_margin_bottom(v->flow, 4);
    gtk_widget_set_margin_start(v->flow, 4);
    gtk_widget_set_margin_end(v->flow, 4);
    g_signal_connect(v->flow, "selected-children-changed", G_CALLBACK(flow_selected_cb), v);
    g_signal_connect(v->flow, "child-activated", G_CALLBACK(flow_activated_cb), v);
    v->sw[LAYOUT_GRID] = scrolled(v->flow, 150);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(v->sw[LAYOUT_GRID]), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_stack_add_named(GTK_STACK(v->stack), v->sw[LAYOUT_GRID], "grid");

    v->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(v->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(v->list), FALSE);
    g_signal_connect(v->list, "row-selected", G_CALLBACK(list_selected_cb), v);
    g_signal_connect(v->list, "row-activated", G_CALLBACK(list_activated_cb), v);
    v->sw[LAYOUT_LIST] = scrolled(v->list, 150);
    gtk_stack_add_named(GTK_STACK(v->stack), v->sw[LAYOUT_LIST], "list");

    v->cv = details_new(v);
    v->sw[LAYOUT_DETAILS] = scrolled(v->cv, 150);
    gtk_stack_add_named(GTK_STACK(v->stack), v->sw[LAYOUT_DETAILS], "details");

    GtkWidget *empty = gtk_label_new("No programs yet: Add program… or Find installed.");
    gtk_widget_add_css_class(empty, "lsiw-dim");
    gtk_widget_set_margin_top(empty, 24);
    gtk_widget_set_margin_bottom(empty, 24);
    gtk_stack_add_named(GTK_STACK(v->stack), empty, "empty");

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), v->stack);
    gtk_box_append(GTK_BOX(v->root), frame);

    v->quiet++;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(v->layout_btn[v->layout]), TRUE);
    v->quiet--;
    v->timer = g_timeout_add_seconds(5, poll_cb, v);
    return v;
}

GtkWidget *lsiw_libview_widget(LsiwLibView *v) { return v->root; }
