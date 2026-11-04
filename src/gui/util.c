/* LSIW Control: helpers shared by the pages, and LsiwTask, which runs a
 * command (`lsiw run`, `lsiw check`, `lsiw wine-setup`, ...) without
 * blocking the window and streams its output into a text view.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Pages                                                                */
/* ------------------------------------------------------------------ */

void lsiw_page_attach(LsiwPage *page, GtkWidget *widget, GDestroyNotify free_page)
{
    page->widget = widget;
    g_object_set_data_full(G_OBJECT(widget), "lsiw-page", page, free_page);
}

void lsiw_page_refresh(GtkWidget *page_widget, enum lsiw_refresh_reason reason)
{
    GtkWidget *inner = g_object_get_data(G_OBJECT(page_widget), "lsiw-page-widget");
    if (inner) page_widget = inner;         /* the scrolled window around a page */
    LsiwPage *p = g_object_get_data(G_OBJECT(page_widget), "lsiw-page");
    if (p && p->refresh) p->refresh(p, reason);
}

void lsiw_gui_refresh_all(GtkWidget *any, enum lsiw_refresh_reason reason)
{
    GtkRoot *root = gtk_widget_get_root(any);
    GtkWidget *stack = root ? g_object_get_data(G_OBJECT(root), "lsiw-stack") : NULL;
    if (!stack) {
        /* Not inside the main window: walk up to the page itself. */
        for (GtkWidget *w = any; w; w = gtk_widget_get_parent(w))
            if (g_object_get_data(G_OBJECT(w), "lsiw-page")) {
                lsiw_page_refresh(w, reason);
                break;
            }
        return;
    }
    for (GtkWidget *c = gtk_widget_get_first_child(stack); c; c = gtk_widget_get_next_sibling(c))
        lsiw_page_refresh(c, reason);
}

/* ------------------------------------------------------------------ */
/* Programs and settings                                                */
/* ------------------------------------------------------------------ */

const char *lsiw_gui_cli(void)
{
    static char *path;
    static gboolean searched;
    if (searched) return path;
    searched = TRUE;
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *cand = g_build_filename(dir, "lsiw", NULL);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE) && !g_file_test(cand, G_FILE_TEST_IS_DIR))
            path = cand;
        else
            g_free(cand);
        g_free(dir);
        g_free(self);
    }
    if (!path) path = g_find_program_in_path("lsiw");
    return path;
}

const char *lsiw_gui_wine_setting(void)
{
    const char *cfg = lsiw_setting_get("engine", "wine", NULL);
    if (!cfg || !*cfg) cfg = lsiw_setting_get("runtime", "wine", NULL);
    return cfg ? cfg : "";
}

char *lsiw_gui_find_wine(void)
{
    /* The same search as the CLI's Wine engine: the configured command
     * (a name looked up in $PATH, or a path, ~ allowed), then wine and
     * wine64 from $PATH. */
    const char *cfg = lsiw_gui_wine_setting();
    if (*cfg) {
        struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_WINE, cfg);   /* a version's ID */
        if (r) {
            char *path = g_strdup(r->path);
            lsiw_runner_free(r);
            return path;
        }
        char *p = (cfg[0] == '~' && cfg[1] == '/')
            ? g_build_filename(g_get_home_dir(), cfg + 2, NULL) : g_strdup(cfg);
        char *found = g_find_program_in_path(p);
        g_free(p);
        if (found) return found;
    }
    char *found = g_find_program_in_path("wine");
    if (!found) found = g_find_program_in_path("wine64");
    return found;
}

const char *lsiw_gui_default_engine(void)
{
    static const char *const names[] = { "auto", "native", "wine", "proton", "custom" };
    const char *v = lsiw_setting_get("engine", "default", NULL);
    if (!v || !*v) v = lsiw_setting_get("runtime", "backend", NULL);
    if (v)
        for (int i = 0; i < 5; i++)
            if (!g_ascii_strcasecmp(v, names[i])) return names[i];
    return "auto";      /* lsiw itself treats unknown values as auto */
}

/* ------------------------------------------------------------------ */
/* Dialogs and launching                                                */
/* ------------------------------------------------------------------ */

GtkWindow *lsiw_gui_window(GtkWidget *w)
{
    GtkRoot *root = w ? gtk_widget_get_root(w) : NULL;
    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
}

void lsiw_gui_alert(GtkWidget *w, const char *message, const char *detail)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message);
    if (detail) gtk_alert_dialog_set_detail(d, detail);
    gtk_alert_dialog_set_modal(d, TRUE);
    gtk_alert_dialog_show(d, lsiw_gui_window(w));
    g_object_unref(d);
}

gboolean lsiw_gui_save_config(GtkWidget *w)
{
    if (lsiw_config_save() == 0) return TRUE;
    int err = errno;
    char *detail = g_strdup_printf("%s: %s\nThe change was not saved.",
                                   lsiw_config_path(), g_strerror(err));
    lsiw_gui_alert(w, "The configuration could not be saved", detail);
    g_free(detail);
    lsiw_config_load();
    return FALSE;
}

static void opened_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    GtkWidget *w = data;
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri_finish(res, &err)) {
        if (gtk_widget_get_root(w))
            lsiw_gui_alert(w, "The folder could not be opened", err->message);
        g_error_free(err);
    }
    g_object_unref(w);
}

void lsiw_gui_open_folder(GtkWidget *w, const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (!uri) {
        lsiw_gui_alert(w, "The folder could not be opened", err->message);
        g_error_free(err);
        return;
    }
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(w));
    g_app_info_launch_default_for_uri_async(uri, G_APP_LAUNCH_CONTEXT(ctx), NULL,
                                            opened_cb, g_object_ref(w));
    g_object_unref(ctx);
    g_free(uri);
}

void lsiw_gui_action_enable(GSimpleActionGroup *group, const char *name, gboolean enabled)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(group), name);
    if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), enabled);
}

/* ------------------------------------------------------------------ */
/* Widgets                                                              */
/* ------------------------------------------------------------------ */

GtkWidget *lsiw_gui_heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), 20);
    gtk_widget_add_css_class(l, "lsiw-heading");
    return l;
}

/* Wrapping labels get a minimum width in characters: without one, GTK
 * measures them one word per line, and the window opens far too tall. */
GtkWidget *lsiw_gui_paragraph(const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 100);
    return l;
}

GtkWidget *lsiw_gui_page_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    return box;
}

GtkWidget *lsiw_gui_action_button(const char *mnemonic_label, const char *action)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(mnemonic_label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}

/* A layout that places its children left to right at their natural width
 * and starts a new line when the next one does not fit. A child that
 * expands horizontally takes what is left of its line. */
#define LSIW_TYPE_WRAP_LAYOUT (lsiw_wrap_layout_get_type())
G_DECLARE_FINAL_TYPE(LsiwWrapLayout, lsiw_wrap_layout, LSIW, WRAP_LAYOUT, GtkLayoutManager)

struct _LsiwWrapLayout {
    GtkLayoutManager parent;
    int spacing;
    int line_spacing;
};

G_DEFINE_FINAL_TYPE(LsiwWrapLayout, lsiw_wrap_layout, GTK_TYPE_LAYOUT_MANAGER)

static int child_width(GtkWidget *c, int avail)
{
    int min, nat;
    gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
    if (nat > avail) nat = avail;
    return nat < min ? min : nat;
}

/* Lays the children out in `width`; with alloc, also allocates them.
 * Returns the height used. */
static int wrap_run(LsiwWrapLayout *self, GtkWidget *widget, int width, int alloc)
{
    int y = 0;
    GtkWidget *first = gtk_widget_get_first_child(widget);
    while (first) {
        /* Collect one line. */
        GtkWidget *end = first;
        int used = 0, n = 0;
        for (GtkWidget *c = first; c; c = gtk_widget_get_next_sibling(c)) {
            end = gtk_widget_get_next_sibling(c);
            if (!gtk_widget_should_layout(c)) continue;
            int w = child_width(c, width);
            if (n > 0 && used + self->spacing + w > width) { end = c; break; }
            used += (n ? self->spacing : 0) + w;
            n++;
        }
        int extra = width - used, x = 0, line_h = 0, extra_alloc = width - used;
        for (GtkWidget *c = first; c != end; c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c)) continue;
            int w = child_width(c, width);
            if (extra > 0 && gtk_widget_get_hexpand(c)) { w += extra; extra = 0; }
            int min, nat;
            gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, w, &min, &nat, NULL, NULL);
            if (nat > line_h) line_h = nat;
        }
        if (alloc) {
            for (GtkWidget *c = first; c != end; c = gtk_widget_get_next_sibling(c)) {
                if (!gtk_widget_should_layout(c)) continue;
                int w = child_width(c, width);
                if (extra_alloc > 0 && gtk_widget_get_hexpand(c)) { w += extra_alloc; extra_alloc = 0; }
                int hmin, h;      /* each child at its own height, centred in the line */
                gtk_widget_measure(c, GTK_ORIENTATION_VERTICAL, w, &hmin, &h, NULL, NULL);
                GtkAllocation a = { x, y + (line_h - h) / 2, w, h };
                gtk_widget_size_allocate(c, &a, -1);
                x += w + self->spacing;
            }
        }
        y += line_h;
        first = end;
        if (first) y += self->line_spacing;
    }
    return y;
}

static void wrap_measure(GtkLayoutManager *lm, GtkWidget *widget, GtkOrientation o, int for_size,
                         int *min, int *nat, int *min_base, int *nat_base)
{
    LsiwWrapLayout *self = LSIW_WRAP_LAYOUT(lm);
    *min = *nat = 0;
    *min_base = *nat_base = -1;
    if (o == GTK_ORIENTATION_HORIZONTAL) {
        int n = 0;
        for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
            if (!gtk_widget_should_layout(c)) continue;
            int cmin, cnat;
            gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &cmin, &cnat, NULL, NULL);
            if (cmin > *min) *min = cmin;
            *nat += (n++ ? self->spacing : 0) + cnat;
        }
        return;
    }
    if (for_size < 0) {
        int dummy;
        wrap_measure(lm, widget, GTK_ORIENTATION_HORIZONTAL, -1, &dummy, &for_size, &dummy, &dummy);
    }
    *min = *nat = wrap_run(self, widget, for_size, 0);
}

static void wrap_allocate(GtkLayoutManager *lm, GtkWidget *widget, int width, int height, int baseline)
{
    (void)height; (void)baseline;
    wrap_run(LSIW_WRAP_LAYOUT(lm), widget, width, 1);
}

static GtkSizeRequestMode wrap_request_mode(GtkLayoutManager *lm, GtkWidget *widget)
{
    (void)lm; (void)widget;
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void lsiw_wrap_layout_class_init(LsiwWrapLayoutClass *klass)
{
    GtkLayoutManagerClass *lm = GTK_LAYOUT_MANAGER_CLASS(klass);
    lm->measure = wrap_measure;
    lm->allocate = wrap_allocate;
    lm->get_request_mode = wrap_request_mode;
}

static void lsiw_wrap_layout_init(LsiwWrapLayout *self) { (void)self; }

GtkWidget *lsiw_gui_wrap_row(int spacing)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    LsiwWrapLayout *wl = g_object_new(LSIW_TYPE_WRAP_LAYOUT, NULL);
    wl->spacing = spacing;
    wl->line_spacing = 6;
    gtk_widget_set_layout_manager(box, GTK_LAYOUT_MANAGER(wl));
    return box;
}

void lsiw_gui_wrap_row_append(GtkWidget *row, GtkWidget *child)
{
    if (GTK_IS_LABEL(child)) gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), child);
}

GtkWidget *lsiw_gui_proton_download_button(const char *action)
{
    GMenu *menu = g_menu_new();
    size_t n;
    const struct lsiw_proton_build *b = lsiw_proton_builds(&n);
    for (size_t i = 0; i < n; i++) {
        GMenuItem *it = g_menu_item_new(b[i].title, NULL);
        g_menu_item_set_action_and_target_value(it, action, g_variant_new_string(b[i].id));
        g_menu_append_item(menu, it);
        g_object_unref(it);
    }
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "Download Proton");
    gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(mb), TRUE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    GString *tip = g_string_new("Download the latest build (checksum verified where published); with the "
                                "Steam integration added, it appears in Steam as LSIW …\n");
    for (size_t i = 0; i < n; i++) g_string_append_printf(tip, "\n%s: %s", b[i].title, b[i].about);
    gtk_widget_set_tooltip_text(mb, tip->str);
    g_string_free(tip, TRUE);
    g_object_unref(menu);
    return mb;
}

static void dd_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer ellipsize)
{
    (void)f;
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (ellipsize) {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(l), 12);
    }
    gtk_list_item_set_child(item, l);
}

static void dd_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    GtkStringObject *s = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    GtkWidget *l = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(l), s ? gtk_string_object_get_string(s) : "");
    gtk_widget_set_tooltip_text(l, s ? gtk_string_object_get_string(s) : NULL);
}

void lsiw_gui_dropdown_ellipsize(GtkDropDown *dd)
{
    for (int popup = 0; popup < 2; popup++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(dd_setup_cb), GINT_TO_POINTER(!popup));
        g_signal_connect(f, "bind", G_CALLBACK(dd_bind_cb), NULL);
        if (popup) gtk_drop_down_set_list_factory(dd, f);
        else gtk_drop_down_set_factory(dd, f);
        g_object_unref(f);
    }
}

GtkWidget *lsiw_gui_output_view(GtkTextView **view)
{
    GtkWidget *tv = gtk_text_view_new();
    GtkTextView *t = GTK_TEXT_VIEW(tv);
    gtk_text_view_set_editable(t, FALSE);
    gtk_text_view_set_cursor_visible(t, FALSE);
    gtk_text_view_set_monospace(t, TRUE);
    gtk_text_view_set_wrap_mode(t, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(t, 6);
    gtk_text_view_set_bottom_margin(t, 6);
    gtk_text_view_set_left_margin(t, 8);
    gtk_text_view_set_right_margin(t, 8);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 120);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_add_css_class(sw, "lsiw-output");
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), sw);
    gtk_widget_set_vexpand(frame, TRUE);
    *view = t;
    return frame;
}

/* ------------------------------------------------------------------ */
/* LsiwTask                                                           */
/* ------------------------------------------------------------------ */

/* Output kept in a view; older text is dropped beyond this. */
#define OUTPUT_LIMIT (512 * 1024)
/* How long to keep reading after the process exited. A daemon it started
 * (wineserver) can inherit the output pipe and hold it open. */
#define DRAIN_MS 1500

struct run;

struct LsiwTask {
    gboolean alive;
    GtkTextView *view;
    GtkTextBuffer *buffer;     /* reference held */
    GtkTextMark *end_mark;
    GtkLabel *status;
    LsiwTaskFunc state_changed, finished;
    gpointer data;
    GPtrArray *runs;           /* the active runs (each a reference) */
    struct run *writer;        /* the run whose output came last (weak) */
    gboolean multi;            /* several runs at once */
    guint next_id;
    gboolean have_code;
    int code;
    int last_tag;
    char *last_label;
};

struct run {
    LsiwTask *runner;        /* reference held */
    guint id;
    int tag;
    GSubprocess *proc;
    GInputStream *out;
    GCancellable *cancel;
    GByteArray *pending;       /* an incomplete UTF-8 sequence */
    gboolean eof, exited, done;
    guint drain_id;
    int stops;
    char *what;                /* the name for the status line and the output */
};

static void runner_release(LsiwTask *r)
{
    g_object_unref(r->buffer);
    g_ptr_array_unref(r->runs);
    g_free(r->last_label);
}

static struct run *run_ref(struct run *run) { return g_rc_box_acquire(run); }

static void run_clear(struct run *run)
{
    if (run->drain_id) g_source_remove(run->drain_id);
    g_clear_object(&run->proc);
    g_clear_object(&run->out);
    g_clear_object(&run->cancel);
    g_byte_array_unref(run->pending);
    g_free(run->what);
    g_rc_box_release_full(run->runner, (GDestroyNotify)runner_release);
}

static void run_unref(gpointer run) { g_rc_box_release_full(run, (GDestroyNotify)run_clear); }

static void runner_insert(LsiwTask *r, const char *text, gssize len)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(r->buffer, &end);
    gtk_text_buffer_insert(r->buffer, &end, text, (int)len);
    if (gtk_text_buffer_get_char_count(r->buffer) > OUTPUT_LIMIT) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(r->buffer, &a);
        gtk_text_buffer_get_iter_at_offset(r->buffer, &b, OUTPUT_LIMIT / 4);
        gtk_text_iter_forward_line(&b);
        gtk_text_buffer_delete(r->buffer, &a, &b);
    }
    gtk_text_view_scroll_mark_onscreen(r->view, r->end_mark);
}

static gboolean at_line_start(LsiwTask *r)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(r->buffer, &end);
    if (gtk_text_iter_is_start(&end)) return TRUE;
    gtk_text_iter_backward_char(&end);
    return gtk_text_iter_get_char(&end) == '\n';
}

/* With several runs, output of a run that did not write last starts
 * under a line naming it. */
static void run_insert(struct run *run, const char *text, gssize len)
{
    LsiwTask *r = run->runner;
    if (r->multi && r->writer != run && (r->writer || r->runs->len > 1)) {
        char *h = g_strdup_printf("%s[%s]\n", at_line_start(r) ? "" : "\n", run->what);
        runner_insert(r, h, -1);
        g_free(h);
    }
    r->writer = run;
    runner_insert(r, text, len);
}

void lsiw_task_append(LsiwTask *r, const char *text)
{
    r->writer = NULL;
    runner_insert(r, text, -1);
}

/* Append program output: valid UTF-8 as is, an incomplete sequence at the
 * end kept for the next chunk, invalid bytes as U+FFFD. */
static void run_output(struct run *run, const guint8 *data, gsize len, gboolean final)
{
    g_byte_array_append(run->pending, data, (guint)len);
    const char *p = (const char *)run->pending->data;
    gsize left = run->pending->len;
    while (left) {
        const char *end;
        g_utf8_validate_len(p, left, &end);
        if (end > p) run_insert(run, p, end - p);
        left -= (gsize)(end - p);
        p = end;
        if (!left) break;
        gunichar c = g_utf8_get_char_validated(p, (gssize)left);
        if (c == (gunichar)-2 && !final) break;          /* wait for the rest */
        run_insert(run, "\xef\xbf\xbd", 3);
        p++;
        left--;
    }
    g_byte_array_remove_range(run->pending, 0, run->pending->len - (guint)left);
}

/* "Running VotV…", "Running 2 programs: VotV, Arknights…" */
static void status_running(LsiwTask *r, const char *before)
{
    GString *s = g_string_new(before);
    if (before && *before) g_string_append_c(s, ' ');
    if (r->runs->len == 1) {
        struct run *run = g_ptr_array_index(r->runs, 0);
        g_string_append_printf(s, "%s %s…", before && *before ? "Still running" : "Running", run->what);
    } else {
        g_string_append_printf(s, "%s %u programs: ", before && *before ? "Still running" : "Running",
                               r->runs->len);
        for (guint i = 0; i < r->runs->len; i++) {
            struct run *run = g_ptr_array_index(r->runs, i);
            g_string_append_printf(s, "%s%s", i ? ", " : "", run->what);
        }
        g_string_append(s, "…");
    }
    gtk_label_set_text(r->status, s->str);
    g_string_free(s, TRUE);
}

static void run_finish(struct run *run)
{
    LsiwTask *r = run->runner;
    if (run->done || !run->eof || !run->exited) return;
    run->done = TRUE;
    if (run->drain_id) {
        g_source_remove(run->drain_id);
        run->drain_id = 0;
    }
    g_cancellable_cancel(run->cancel);     /* a read still waiting on a held pipe */
    run_output(run, NULL, 0, TRUE);

    char *msg;
    r->have_code = FALSE;
    if (g_subprocess_get_if_exited(run->proc)) {
        r->have_code = TRUE;
        r->code = g_subprocess_get_exit_status(run->proc);
        msg = g_strdup_printf("%s finished with exit code %d.", run->what, r->code);
    } else if (g_subprocess_get_if_signaled(run->proc)) {
        int sig = g_subprocess_get_term_sig(run->proc);
        msg = g_strdup_printf("%s was stopped by signal %d (%s).", run->what, sig, g_strsignal(sig));
    } else {
        msg = g_strdup_printf("%s ended.", run->what);
    }
    char *line = g_strdup_printf("%s[%s]\n", at_line_start(r) ? "\n" : "\n\n", msg);
    runner_insert(r, line, -1);
    g_free(line);
    r->writer = NULL;
    r->last_tag = run->tag;
    g_free(r->last_label);
    r->last_label = g_strdup(run->what);

    /* The runner's reference is now this function's. */
    gboolean held = g_ptr_array_remove(r->runs, run);
    if (r->runs->len) status_running(r, msg);
    else gtk_label_set_text(r->status, msg);
    g_free(msg);

    if (r->finished) r->finished(r, r->data);
    if (r->state_changed) r->state_changed(r, r->data);
    if (held) run_unref(run);
}

static void read_next(struct run *run);

static void read_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    struct run *run = data;
    GError *err = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
    if (run->runner->alive && !run->done) {
        if (!bytes || g_bytes_get_size(bytes) == 0) {
            if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                char *m = g_strdup_printf("\n[reading the output failed: %s]\n", err->message);
                run_insert(run, m, -1);
                g_free(m);
            }
            run->eof = TRUE;
            run_finish(run);
        } else {
            gsize n;
            const guint8 *d = g_bytes_get_data(bytes, &n);
            run_output(run, d, n, FALSE);
            read_next(run);
        }
    }
    if (bytes) g_bytes_unref(bytes);
    g_clear_error(&err);
    run_unref(run);
}

static void read_next(struct run *run)
{
    g_input_stream_read_bytes_async(run->out, 16384, G_PRIORITY_DEFAULT, run->cancel,
                                    read_cb, run_ref(run));
}

static gboolean drain_timeout(gpointer data)
{
    struct run *run = data;
    run->drain_id = 0;
    if (run->runner->alive && !run->done) {
        run->eof = TRUE;
        run_finish(run);
    }
    return G_SOURCE_REMOVE;
}

static void wait_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    struct run *run = data;
    GError *err = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (run->runner->alive && !run->done && !err) {
        run->exited = TRUE;
        if (!run->eof && !run->drain_id)
            run->drain_id = g_timeout_add_full(G_PRIORITY_DEFAULT, DRAIN_MS, drain_timeout,
                                               run_ref(run), run_unref);
        run_finish(run);
    }
    g_clear_error(&err);
    run_unref(run);
}

LsiwTask *lsiw_task_new(GtkTextView *view, GtkLabel *status)
{
    LsiwTask *r = g_rc_box_new0(LsiwTask);
    r->alive = TRUE;
    r->view = view;
    r->buffer = g_object_ref(gtk_text_view_get_buffer(view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(r->buffer, &end);
    r->end_mark = gtk_text_buffer_create_mark(r->buffer, "lsiw-end", &end, FALSE);
    r->status = status;
    r->runs = g_ptr_array_new();
    return r;
}

void lsiw_task_set_callbacks(LsiwTask *r, LsiwTaskFunc state_changed,
                               LsiwTaskFunc finished, gpointer data)
{
    r->state_changed = state_changed;
    r->finished = finished;
    r->data = data;
}

void lsiw_task_set_multi(LsiwTask *r, gboolean multi)
{
    r->multi = multi;
}

/* Runs in the child between fork and exec: a process group of its own,
 * so Stop reaches everything it started (Wine starts several processes). */
static void child_setup(gpointer data)
{
    (void)data;
    setpgid(0, 0);
}

guint lsiw_task_start_full(LsiwTask *r, const char *const *argv, const char *cwd,
                           const char *label, int tag)
{
    if (r->runs->len && !r->multi) return 0;

    GString *cmd = g_string_new("$");
    for (int i = 0; argv[i]; i++) {
        char *q = g_shell_quote(argv[i]);
        g_string_append_c(cmd, ' ');
        g_string_append(cmd, q);
        g_free(q);
    }
    g_string_append_c(cmd, '\n');
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(r->buffer, &end);
    if (!gtk_text_iter_is_start(&end)) runner_insert(r, at_line_start(r) ? "\n" : "\n\n", -1);
    runner_insert(r, cmd->str, (gssize)cmd->len);
    g_string_free(cmd, TRUE);

    GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                       G_SUBPROCESS_FLAGS_STDERR_MERGE);
    /* No terminal to read from: a program asking for input sees end of file. */
    g_subprocess_launcher_set_stdin_file_path(l, "/dev/null");
    if (cwd) g_subprocess_launcher_set_cwd(l, cwd);
    g_subprocess_launcher_set_child_setup(l, child_setup, NULL, NULL);
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_launcher_spawnv(l, argv, &err);
    g_object_unref(l);

    char *base = g_path_get_basename(argv[0]);
    if (!proc) {
        r->writer = NULL;
        char *m = g_strdup_printf("[%s could not be started: %s]\n", base, err->message);
        runner_insert(r, m, -1);
        g_free(m);
        m = g_strdup_printf("%s could not be started: %s", base, err->message);
        gtk_label_set_text(r->status, m);
        g_free(m);
        g_free(base);
        g_error_free(err);
        r->have_code = FALSE;
        return 0;
    }

    struct run *run = g_rc_box_new0(struct run);
    run->runner = g_rc_box_acquire(r);
    run->id = ++r->next_id;
    run->tag = tag;
    run->proc = proc;
    run->out = g_object_ref(g_subprocess_get_stdout_pipe(proc));
    run->cancel = g_cancellable_new();
    run->pending = g_byte_array_new();
    /* "lsiw run", "pkexec", ... for the status line */
    if (label && *label)
        run->what = g_strdup(label);
    else if (!strcmp(base, "lsiw") && argv[1])
        run->what = g_strdup_printf("lsiw %s", argv[1]);
    else
        run->what = g_strdup(base);
    g_free(base);
    g_ptr_array_add(r->runs, run);   /* the runner's reference */
    r->writer = run;                 /* its output follows its command line */

    status_running(r, NULL);

    read_next(run);
    g_subprocess_wait_async(proc, run->cancel, wait_cb, run_ref(run));
    if (r->state_changed) r->state_changed(r, r->data);
    return run->id;
}

gboolean lsiw_task_start(LsiwTask *r, const char *const *argv, const char *cwd)
{
    return lsiw_task_start_full(r, argv, cwd, NULL, 0) != 0;
}

gboolean lsiw_task_running(LsiwTask *r)
{
    return r->runs->len != 0;
}

guint lsiw_task_count(LsiwTask *r)
{
    return r->runs->len;
}

guint lsiw_task_run_id(LsiwTask *r, guint i)
{
    return i < r->runs->len ? ((struct run *)g_ptr_array_index(r->runs, i))->id : 0;
}

const char *lsiw_task_run_label(LsiwTask *r, guint i)
{
    return i < r->runs->len ? ((struct run *)g_ptr_array_index(r->runs, i))->what : NULL;
}

int lsiw_task_run_pid(LsiwTask *r, guint i)
{
    if (i >= r->runs->len) return 0;
    const char *id = g_subprocess_get_identifier(((struct run *)g_ptr_array_index(r->runs, i))->proc);
    return id ? atoi(id) : 0;
}

gboolean lsiw_task_exit_code(LsiwTask *r, int *code)
{
    if (r->have_code && code) *code = r->code;
    return r->have_code;
}

int lsiw_task_last_tag(LsiwTask *r)
{
    return r->last_tag;
}

const char *lsiw_task_last_label(LsiwTask *r)
{
    return r->last_label;
}

static void run_stop(struct run *run)
{
    LsiwTask *r = run->runner;
    if (run->exited) return;
    const char *id = g_subprocess_get_identifier(run->proc);
    pid_t pid = id ? (pid_t)atoi(id) : 0;
    /* lsiw stops the program it watches on SIGTERM and forces it on a
     * second one, so with several programs SIGKILL comes only third. */
    int sig = run->stops++ >= (r->multi ? 2 : 1) ? SIGKILL : SIGTERM;
    /* The group first; a child that is set-user-ID (pkexec) refuses it. */
    if (pid <= 0 || kill(-pid, sig) < 0) {
        if (sig == SIGKILL) g_subprocess_force_exit(run->proc);
        else g_subprocess_send_signal(run->proc, sig);
    }
    char *m = r->multi ? g_strdup_printf(run->stops > 1 ? "Forcing %s to stop…" : "Stopping %s…", run->what)
                       : g_strdup(sig == SIGKILL ? "Forcing it to stop…" : "Stopping…");
    gtk_label_set_text(r->status, m);
    g_free(m);
}

void lsiw_task_stop_run(LsiwTask *r, guint id)
{
    for (guint i = 0; i < r->runs->len; i++) {
        struct run *run = g_ptr_array_index(r->runs, i);
        if (run->id == id) { run_stop(run); return; }
    }
}

void lsiw_task_stop(LsiwTask *r)
{
    for (guint i = 0; i < r->runs->len; i++) run_stop(g_ptr_array_index(r->runs, i));
    if (r->multi && r->runs->len > 1)
        gtk_label_set_text(r->status, "Stopping every program…");
}

void lsiw_task_clear(LsiwTask *r)
{
    r->writer = NULL;
    gtk_text_buffer_set_text(r->buffer, "", 0);
}

void lsiw_task_free(LsiwTask *r)
{
    if (!r) return;
    r->alive = FALSE;
    while (r->runs->len) {
        struct run *run = g_ptr_array_steal_index(r->runs, r->runs->len - 1);
        g_cancellable_cancel(run->cancel);
        run_unref(run);
    }
    g_rc_box_release_full(r, (GDestroyNotify)runner_release);
}
