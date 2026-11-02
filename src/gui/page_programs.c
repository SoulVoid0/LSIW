/* LSIW Control: the Programs page.
 *
 * The library: Windows programs and games the user added (or that `lsiw
 * programs scan` found in the Start menu of the shared prefix), each with
 * its own settings ([program:ID] in the config): engine or compatibility
 * tool (native, a Wine version, a Proton version, the custom chain), prefix
 * (shared by default, or its own), DXVK and VKD3D, launch arguments,
 * environment variables, MangoHud, a debug log, an icon and shortcuts.
 * Programs of the library are started with `lsiw programs run ID`.
 * *Desktop shortcut* puts an LSIW shortcut (.lwlnk, `lsiw lwlnk create
 * ID`) on the desktop: double-clicking it starts the program with these
 * settings, and file managers show the program's icon for it.
 *
 * Below it, a quick run of any program: runs a Windows program with `lsiw run --engine=X -- PROGRAM ARGS...` and
 * shows its console output as it arrives, with the exit code at the end;
 * or inspects it with `lsiw check PROGRAM`, which reports every imported
 * Windows function the native engine lacks and the engine `lsiw run`
 * would choose. Programs start in their own folder, as Explorer starts
 * them, with no input (standard input is /dev/null).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Dropdown entries; the first one follows the Engine page. */
static const char *const engine_titles[] = {
    "Default (set on the Engine page)",
    "Automatic",
    "Native engine",
    "Wine (the version chosen on the Engine page)",
    "Proton (the version chosen on the Engine page)",
    NULL
};
static const char *const engine_ids[] = { NULL, "auto", "native", "wine", "proton" };

enum { MODE_NONE, MODE_RUN, MODE_CHECK, MODE_SCAN, MODE_LWLNK };

struct prog_page {
    LsiwPage base;
    GSimpleActionGroup *actions;
    GtkWidget *path_entry, *args_entry, *engine_dd, *status, *lib_list;
    GtkTextView *output;
    LsiwTask *runner;
    char *last_dir;
    int mode;
};

static void update_actions(struct prog_page *p)
{
    gboolean busy = lsiw_task_running(p->runner);
    const char *path = gtk_editable_get_text(GTK_EDITABLE(p->path_entry));
    gboolean have = *path != 0;
    lsiw_gui_action_enable(p->actions, "run", have && !busy);
    lsiw_gui_action_enable(p->actions, "check", have && !busy);
    lsiw_gui_action_enable(p->actions, "stop", busy);
}

static void path_changed_cb(GtkEditable *e, gpointer data)
{
    (void)e;
    update_actions(data);
}

static void set_program(struct prog_page *p, const char *path)
{
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), path);
    g_free(p->last_dir);
    p->last_dir = g_path_get_dirname(path);
}

/* ------------------------------------------------------------------ */
/* Choosing a program                                                   */
/* ------------------------------------------------------------------ */

static void program_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = data;
    struct prog_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GError *err = NULL;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
    if (f && p && gtk_widget_get_root(page)) {
        char *path = g_file_get_path(f);
        if (path) set_program(p, path);
        else lsiw_gui_alert(page, "This file cannot be run",
                            "Choose a program on this computer; LSIW runs local files only.");
        g_free(path);
    } else if (err && !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
               !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
               gtk_widget_get_root(page)) {
        lsiw_gui_alert(page, "No program was chosen", err->message);
    }
    g_clear_error(&err);
    g_clear_object(&f);
    g_object_unref(page);
}

static void act_choose(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose a Windows program");
    gtk_file_dialog_set_modal(d, TRUE);

    /* Suffix matches are case-insensitive: SETUP.EXE matches too. */
    GtkFileFilter *progs = gtk_file_filter_new();
    gtk_file_filter_set_name(progs, "Windows programs (*.exe, *.com)");
    gtk_file_filter_add_suffix(progs, "exe");
    gtk_file_filter_add_suffix(progs, "com");
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, progs);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(d, progs);
    g_object_unref(filters);
    g_object_unref(progs);
    g_object_unref(all);

    const char *dir = p->last_dir;
    char *dl = NULL;
    if (!dir) {
        const char *x = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        dir = dl = g_strdup(x && g_file_test(x, G_FILE_TEST_IS_DIR) ? x : g_get_home_dir());
    }
    GFile *start = g_file_new_for_path(dir);
    gtk_file_dialog_set_initial_folder(d, start);
    g_object_unref(start);
    g_free(dl);

    gtk_file_dialog_open(d, lsiw_gui_window(p->base.widget), NULL, program_chosen_cb,
                         g_object_ref(p->base.widget));
    g_object_unref(d);
}

static gboolean drop_cb(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data)
{
    (void)t; (void)x; (void)y;
    struct prog_page *p = data;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    char *path = g_file_get_path(g_value_get_object(value));
    if (!path) return FALSE;
    set_program(p, path);
    g_free(path);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Running                                                              */
/* ------------------------------------------------------------------ */

static const char *need_cli(struct prog_page *p)
{
    const char *cli = lsiw_gui_cli();
    if (!cli)
        lsiw_gui_alert(p->base.widget, "The lsiw command was not found",
                       "LSIW Control looks for it next to itself and in $PATH. Install LSIW, or "
                       "start LSIW Control from the folder it was built in.");
    return cli;
}

/* The folder a program starts in: its own, like a double-click in
 * Explorer. NULL (inherit) for Windows-style or relative paths. */
static char *program_dir(const char *path)
{
    if (path[0] != '/') return NULL;
    char *dir = g_path_get_dirname(path);
    if (g_file_test(dir, G_FILE_TEST_IS_DIR)) return dir;
    g_free(dir);
    return NULL;
}

static void act_run(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *path = gtk_editable_get_text(GTK_EDITABLE(p->path_entry));
    const char *args = gtk_editable_get_text(GTK_EDITABLE(p->args_entry));
    if (!*path) return;

    /* Arguments follow shell quoting rules, so "a b" stays one argument. */
    char **argv_user = NULL;
    GError *err = NULL;
    char *trimmed = g_strstrip(g_strdup(args));
    if (*trimmed && !g_shell_parse_argv(trimmed, NULL, &argv_user, &err)) {
        lsiw_gui_alert(p->base.widget, "The arguments cannot be read", err->message);
        g_error_free(err);
        g_free(trimmed);
        return;
    }
    g_free(trimmed);

    const char *cli = need_cli(p);
    if (!cli) {
        g_strfreev(argv_user);
        return;
    }
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->engine_dd));
    const char *engine = (sel < G_N_ELEMENTS(engine_ids) && engine_ids[sel])
        ? engine_ids[sel] : lsiw_gui_default_engine();
    char *opt = g_strdup_printf("--engine=%s", engine);

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer)cli);
    g_ptr_array_add(argv, "run");
    g_ptr_array_add(argv, opt);
    g_ptr_array_add(argv, "--");
    g_ptr_array_add(argv, (gpointer)path);
    for (char **u = argv_user; u && *u; u++) g_ptr_array_add(argv, *u);
    g_ptr_array_add(argv, NULL);

    char *cwd = program_dir(path);
    p->mode = MODE_RUN;
    lsiw_task_start(p->runner, (const char *const *)argv->pdata, cwd);
    g_free(cwd);
    g_ptr_array_unref(argv);
    g_free(opt);
    g_strfreev(argv_user);
}

static void act_check(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *path = gtk_editable_get_text(GTK_EDITABLE(p->path_entry));
    const char *cli = *path ? need_cli(p) : NULL;
    if (!cli) return;
    const char *argv[] = { cli, "check", path, NULL };
    p->mode = MODE_CHECK;
    lsiw_task_start(p->runner, argv, NULL);
}

static void act_stop(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    lsiw_task_stop(p->runner);
}

static void act_clear(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    lsiw_task_clear(p->runner);
}

static void act_lib_add(GSimpleAction *a, GVariant *param, gpointer data);
static void act_lib_edit(GSimpleAction *a, GVariant *param, gpointer data);
static void act_lib_run(GSimpleAction *a, GVariant *param, gpointer data);
static void act_lib_shortcut(GSimpleAction *a, GVariant *param, gpointer data);
static void act_lib_lwlnk(GSimpleAction *a, GVariant *param, gpointer data);
static void act_lib_scan(GSimpleAction *a, GVariant *param, gpointer data);
static void act_lib_remove(GSimpleAction *a, GVariant *param, gpointer data);
static void update_lib_actions(struct prog_page *p);
static void fill_library(struct prog_page *p, const char *select);

static const GActionEntry prog_actions[] = {
    { .name = "lib-add", .activate = act_lib_add },
    { .name = "lib-edit", .activate = act_lib_edit },
    { .name = "lib-run", .activate = act_lib_run },
    { .name = "lib-shortcut", .activate = act_lib_shortcut },
    { .name = "lib-lwlnk", .activate = act_lib_lwlnk },
    { .name = "lib-scan", .activate = act_lib_scan },
    { .name = "lib-remove", .activate = act_lib_remove },
    { .name = "choose", .activate = act_choose },
    { .name = "run", .activate = act_run },
    { .name = "check", .activate = act_check },
    { .name = "stop", .activate = act_stop },
    { .name = "clear", .activate = act_clear },
};

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    (void)r;
    update_actions(data);
    update_lib_actions(data);
}

/* Explain the exit codes `lsiw` documents. */
static void runner_done_cb(LsiwTask *r, gpointer data)
{
    struct prog_page *p = data;
    int code;
    if (!lsiw_task_exit_code(r, &code)) return;
    char *m = NULL;
    if (p->mode == MODE_CHECK) {
        m = g_strdup(code == 0 ? "The native engine can run this program entirely."
                   : code == 1 ? "The native engine cannot run all of it (see the report); "
                                 "with the automatic engine it runs with Wine."
                   : "This is not a Windows program, or it was not found.");
    } else if (p->mode == MODE_RUN) {
        if (code == 126) m = g_strdup("The program could not be run (exit code 126); the output says why.");
        else if (code == 127) m = g_strdup("The program, or Wine when it was needed, was not found (exit code 127).");
        else m = g_strdup_printf("The program exited with code %d%s.", code,
                                 code == 0 ? " (success)" : "");
    }
    else if (p->mode == MODE_LWLNK)
        m = g_strdup(code == 0 ? "The shortcut is on the desktop: double-click it to start the program "
                                 "with these settings (the path is in the output below)."
                               : "The shortcut could not be made; the output says why.");
    else if (p->mode == MODE_SCAN && code == 0)
        m = g_strdup("Every installed program found in the Start menu is in the library now.");
    if (m) gtk_label_set_text(GTK_LABEL(p->status), m);
    g_free(m);
    /* `lsiw programs ...` may have changed the library. */
    lsiw_config_load();
    fill_library(p, NULL);
}

/* ------------------------------------------------------------------ */
/* Page                                                                 */
/* ------------------------------------------------------------------ */

static void prog_free(gpointer data)
{
    struct prog_page *p = data;
    lsiw_task_free(p->runner);
    g_free(p->last_dir);
    g_free(p);
}

static void prog_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    (void)reason;
    update_actions((struct prog_page *)base);
    fill_library((struct prog_page *)base, NULL);
}

static GtkWidget *form_label(const char *mnemonic, GtkWidget *target)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
}

/* ------------------------------------------------------------------ */
/* The library                                                          */
/* ------------------------------------------------------------------ */

/* Choices of the engine dropdown of a program; values[i] is what
 * [program:ID] engine stores. */
struct choices { GPtrArray *titles, *values; };

static void choice_add(struct choices *c, const char *title, const char *value)
{
    g_ptr_array_add(c->titles, g_strdup(title));
    g_ptr_array_add(c->values, g_strdup(value));
}

static struct choices engine_choices(void)
{
    struct choices c = { g_ptr_array_new_with_free_func(g_free), g_ptr_array_new_with_free_func(g_free) };
    choice_add(&c, "Default (set on the Engine page)", "");
    choice_add(&c, "Automatic (native engine, Wine when needed)", "auto");
    choice_add(&c, "Native engine", "native");
    choice_add(&c, "Wine (the version chosen on the Engine page)", "wine");
    struct lsiw_runner *v;
    size_t n = lsiw_runners(LSIW_RUNNER_WINE, &v);
    for (size_t i = 0; i < n; i++) {
        if (v[i].foreign) continue;
        char *t = g_strdup_printf("Wine: %s", v[i].name), *val = g_strdup_printf("wine:%s", v[i].id);
        choice_add(&c, t, val);
        g_free(t);
        g_free(val);
    }
    lsiw_runners_free(v, n);
    choice_add(&c, "Proton (the version chosen on the Engine page)", "proton");
    n = lsiw_runners(LSIW_RUNNER_PROTON, &v);
    for (size_t i = 0; i < n; i++) {
        if (v[i].foreign) continue;
        char *t = g_strdup_printf("Proton: %s", v[i].name), *val = g_strdup_printf("proton:%s", v[i].id);
        choice_add(&c, t, val);
        g_free(t);
        g_free(val);
    }
    lsiw_runners_free(v, n);
    choice_add(&c, "Custom configuration (default engine and fallbacks)", "custom");
    return c;
}

static guint choice_index(struct choices *c, const char *value)
{
    for (guint i = 0; i < c->values->len; i++)
        if (!g_ascii_strcasecmp(g_ptr_array_index(c->values, i), value ? value : "")) return i;
    char *t = g_strdup_printf("%s (not installed)", value);
    choice_add(c, t, value);
    g_free(t);
    return c->values->len - 1;
}

static void choices_free(struct choices *c)
{
    g_ptr_array_unref(c->titles);
    g_ptr_array_unref(c->values);
}

static const char *const tri_titles[] = { "Default", "On", "Off", NULL };
static guint tri_index(int v) { return v < 0 ? 0 : v ? 1 : 2; }
static int tri_value(guint i) { return i == 1 ? 1 : i == 2 ? 0 : -1; }

static const char *selected_id(struct prog_page *p)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->lib_list));
    return row ? g_object_get_data(G_OBJECT(row), "lsiw-id") : NULL;
}

static void update_lib_actions(struct prog_page *p)
{
    gboolean busy = lsiw_task_running(p->runner);
    gboolean sel = selected_id(p) != NULL;
    lsiw_gui_action_enable(p->actions, "lib-edit", sel);
    lsiw_gui_action_enable(p->actions, "lib-remove", sel && !busy);
    lsiw_gui_action_enable(p->actions, "lib-run", sel && !busy);
    lsiw_gui_action_enable(p->actions, "lib-shortcut", sel && !busy);
    lsiw_gui_action_enable(p->actions, "lib-lwlnk", sel && !busy);
    lsiw_gui_action_enable(p->actions, "lib-scan", !busy);
}

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

static void lwlnk_for(struct prog_page *p, const char *id);

/* The desktop-shortcut button of one row. */
static void row_lwlnk_clicked(GtkButton *b, gpointer data)
{
    struct prog_page *p = data;
    const char *id = g_object_get_data(G_OBJECT(b), "lsiw-id");
    GtkWidget *row = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
    if (row) gtk_list_box_select_row(GTK_LIST_BOX(p->lib_list), GTK_LIST_BOX_ROW(row));
    if (id && !lsiw_task_running(p->runner)) lwlnk_for(p, id);
}

static void fill_library(struct prog_page *p, const char *select)
{
    char *keep = g_strdup(select ? select : selected_id(p));
    gtk_list_box_remove_all(GTK_LIST_BOX(p->lib_list));
    struct lsiw_program *v;
    size_t n = lsiw_programs(&v);
    for (size_t i = 0; i < n; i++) {
        GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(outer, 4);
        gtk_widget_set_margin_bottom(outer, 4);
        gtk_widget_set_margin_start(outer, 8);
        gtk_widget_set_margin_end(outer, 8);
        /* The program's icon (cached as a PNG; made once from the .exe). */
        char *icon = lsiw_program_icon(&v[i], 0);
        GtkWidget *img = icon ? gtk_image_new_from_file(icon) : gtk_image_new_from_icon_name("application-x-executable");
        free(icon);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
        gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(outer), img);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(box, TRUE);
        gtk_box_append(GTK_BOX(outer), box);
        GtkWidget *name = gtk_label_new(v[i].name);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_add_css_class(name, "lsiw-bold");
        gtk_box_append(GTK_BOX(box), name);
        char *sum = engine_summary(&v[i]);
        char *line = g_strdup_printf("%s\n%s", sum, v[i].exe);
        GtkWidget *d = gtk_label_new(line);
        gtk_label_set_xalign(GTK_LABEL(d), 0);
        gtk_label_set_ellipsize(GTK_LABEL(d), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_add_css_class(d, "lsiw-dim");
        gtk_box_append(GTK_BOX(box), d);
        g_free(line);
        g_free(sum);
        GtkWidget *mk = gtk_button_new_with_label("Desktop shortcut");
        gtk_widget_set_tooltip_text(mk, "Desktop shortcut: put an LSIW shortcut (.lwlnk) with this program's "
                                        "icon on the desktop");
        gtk_widget_set_valign(mk, GTK_ALIGN_CENTER);
        g_object_set_data_full(G_OBJECT(mk), "lsiw-id", g_strdup(v[i].id), g_free);
        g_signal_connect(mk, "clicked", G_CALLBACK(row_lwlnk_clicked), p);
        gtk_box_append(GTK_BOX(outer), mk);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
        g_object_set_data_full(G_OBJECT(row), "lsiw-id", g_strdup(v[i].id), g_free);
        gtk_list_box_append(GTK_LIST_BOX(p->lib_list), row);
        if (keep && !strcmp(keep, v[i].id)) gtk_list_box_select_row(GTK_LIST_BOX(p->lib_list), GTK_LIST_BOX_ROW(row));
    }
    lsiw_programs_free(v, n);
    g_free(keep);
    update_lib_actions(p);
}

/* ---- the settings window of one program ---- */

struct editor {
    struct prog_page *page;
    char *id;
    GtkWidget *win, *name, *exe, *args, *workdir, *engine, *prefix, *dxvk, *vkd3d, *icon, *icon_preview;
    GtkWidget *mangohud, *debug, *shortcut;
    GtkTextView *env;
    struct choices engines;
};

static void editor_free(gpointer data)
{
    struct editor *e = data;
    choices_free(&e->engines);
    g_free(e->id);
    g_free(e);
}

static void editor_browse_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    struct editor *e = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        char *path = g_file_get_path(f);
        if (path) gtk_editable_set_text(GTK_EDITABLE(e->exe), path);
        g_free(path);
        g_object_unref(f);
    }
    g_object_unref(e->win);
}

static void editor_browse(GtkButton *b, gpointer data)
{
    (void)b;
    struct editor *e = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose the program");
    const char *cur = gtk_editable_get_text(GTK_EDITABLE(e->exe));
    if (*cur == '/') {
        char *dir = g_path_get_dirname(cur);
        GFile *start = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(d, start);
        g_object_unref(start);
        g_free(dir);
    }
    gtk_file_dialog_open(d, GTK_WINDOW(e->win), NULL, editor_browse_cb, g_object_ref(e->win));
    g_object_unref(d);
}

/* The preview next to the icon field: the chosen icon, else the program's. */
static void editor_icon_preview(struct editor *e)
{
    struct lsiw_program *pr = lsiw_program_get(e->id);
    if (!pr) return;
    free(pr->icon);
    pr->icon = strdup(gtk_editable_get_text(GTK_EDITABLE(e->icon)));
    free(pr->exe);
    pr->exe = strdup(gtk_editable_get_text(GTK_EDITABLE(e->exe)));
    /* A preview id, so the cached icon of the saved settings stays. */
    free(pr->id);
    pr->id = strdup("preview");
    char *icon = lsiw_program_icon(pr, 1);
    if (icon) gtk_image_set_from_file(GTK_IMAGE(e->icon_preview), icon);
    else gtk_image_set_from_icon_name(GTK_IMAGE(e->icon_preview), "application-x-executable");
    char *cached = g_build_filename(lsiw_data_dir(), "icons", "preview.png", NULL);
    unlink(cached);   /* the image keeps its pixels */
    g_free(cached);
    free(icon);
    lsiw_program_free(pr);
}

static void editor_icon_changed(GtkEditable *ed, gpointer data)
{
    (void)ed;
    editor_icon_preview(data);
}

static gboolean has_suffix_ci(const char *path, const char *ext)
{
    size_t l = strlen(path), n = strlen(ext);
    return l > n && !g_ascii_strcasecmp(path + l - n, ext);
}

/* Formats LSIW reads itself (and embeds in .lwlnk files) are used as they
 * are; any other image GTK can load (.svg, .jpg, .bmp, .webp, ...) is
 * converted to a PNG of at most 256x256 in LSIW's icon folder. */
static char *icon_for_lsiw(struct editor *e, const char *path, GError **err)
{
    if (has_suffix_ci(path, ".png") || has_suffix_ci(path, ".ico") || has_suffix_ci(path, ".exe") ||
        has_suffix_ci(path, ".dll"))
        return g_strdup(path);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 256, 256, TRUE, err);
    if (!pb) return NULL;
    char *dir = g_build_filename(lsiw_data_dir(), "icons", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *base = g_strdup_printf("%s-chosen.png", e->id);
    char *out = g_build_filename(dir, base, NULL);
    gboolean ok = gdk_pixbuf_save(pb, out, "png", err, NULL);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(pb);
    g_free(base);
    g_free(dir);
    if (!ok) { g_free(out); return NULL; }
    return out;
}

static void editor_icon_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *win = data;
    struct editor *e = g_object_get_data(G_OBJECT(win), "lsiw-editor");
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f && e) {
        char *path = g_file_get_path(f);
        GError *err = NULL;
        char *icon = path ? icon_for_lsiw(e, path, &err) : NULL;
        if (icon) gtk_editable_set_text(GTK_EDITABLE(e->icon), icon);
        else if (path) lsiw_gui_alert(win, "This image cannot be used as an icon", err ? err->message : path);
        g_clear_error(&err);
        g_free(icon);
        g_free(path);
    }
    g_clear_object(&f);
    g_object_unref(win);
}

static void editor_icon_browse(GtkButton *b, gpointer data)
{
    (void)b;
    struct editor *e = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose an icon");
    GtkFileFilter *img = gtk_file_filter_new();
    gtk_file_filter_set_name(img, "Images, icons and programs");
    gtk_file_filter_add_pixbuf_formats(img);
    gtk_file_filter_add_suffix(img, "ico");
    gtk_file_filter_add_suffix(img, "exe");
    gtk_file_filter_add_suffix(img, "dll");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, img);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(img);
    gtk_file_dialog_open(d, GTK_WINDOW(e->win), NULL, editor_icon_chosen_cb, g_object_ref(e->win));
    g_object_unref(d);
}

static void editor_icon_reset(GtkButton *b, gpointer data)
{
    (void)b;
    struct editor *e = data;
    gtk_editable_set_text(GTK_EDITABLE(e->icon), "");
}

static void editor_save(GtkButton *b, gpointer data)
{
    (void)b;
    struct editor *e = data;
    struct prog_page *p = e->page;
    struct lsiw_program *pr = lsiw_program_get(e->id);
    if (!pr) { gtk_window_destroy(GTK_WINDOW(e->win)); return; }
#define SETS(field, w) do { free(pr->field); pr->field = strdup(gtk_editable_get_text(GTK_EDITABLE(w))); } while (0)
    SETS(name, e->name);
    SETS(exe, e->exe);
    SETS(args, e->args);
    SETS(workdir, e->workdir);
    SETS(icon, e->icon);
#undef SETS
    guint ei = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->engine));
    free(pr->engine);
    pr->engine = strdup(ei < e->engines.values->len ? (const char *)g_ptr_array_index(e->engines.values, ei) : "");
    free(pr->prefix);
    pr->prefix = strdup(gtk_drop_down_get_selected(GTK_DROP_DOWN(e->prefix)) == 1 ? "own" : "shared");
    pr->dxvk = tri_value(gtk_drop_down_get_selected(GTK_DROP_DOWN(e->dxvk)));
    pr->vkd3d = tri_value(gtk_drop_down_get_selected(GTK_DROP_DOWN(e->vkd3d)));
    pr->mangohud = gtk_check_button_get_active(GTK_CHECK_BUTTON(e->mangohud));
    pr->debug = gtk_check_button_get_active(GTK_CHECK_BUTTON(e->debug));
    GtkTextBuffer *buf = gtk_text_view_get_buffer(e->env);
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(buf, &a, &z);
    free(pr->env);
    char *env = gtk_text_buffer_get_text(buf, &a, &z, FALSE);
    pr->env = strdup(env);
    g_free(env);
    lsiw_program_store(pr);
    free(lsiw_program_icon(pr, 1));      /* the icon may have changed: renew the cached PNG */
    gboolean want_shortcut = gtk_check_button_get_active(GTK_CHECK_BUTTON(e->shortcut));
    char *df = lsiw_program_desktop_file(pr);
    gboolean have_shortcut = g_file_test(df, G_FILE_TEST_EXISTS);
    g_free(df);
    if (lsiw_gui_save_config(p->base.widget)) {
        char *m = g_strdup_printf("Saved the settings of %s.", pr->name);
        gtk_label_set_text(GTK_LABEL(p->status), m);
        g_free(m);
        const char *cli = lsiw_gui_cli();
        if (cli && want_shortcut != have_shortcut) {
            const char *argv[] = { cli, "programs", "shortcut", pr->id, want_shortcut ? "--desktop" : "--remove", NULL };
            p->mode = MODE_NONE;
            lsiw_task_start(p->runner, argv, NULL);
        }
    }
    fill_library(p, pr->id);
    lsiw_program_free(pr);
    gtk_window_destroy(GTK_WINDOW(e->win));
}

static void editor_cancel(GtkButton *b, gpointer data)
{
    (void)b;
    struct editor *e = data;
    gtk_window_destroy(GTK_WINDOW(e->win));
}

static GtkWidget *dd_from(GPtrArray *titles)
{
    GtkStringList *m = gtk_string_list_new(NULL);
    for (guint i = 0; i < titles->len; i++) gtk_string_list_append(m, g_ptr_array_index(titles, i));
    GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(m), NULL);
    lsiw_gui_dropdown_ellipsize(GTK_DROP_DOWN(dd));
    return dd;
}

static void open_editor(struct prog_page *p, const char *id)
{
    struct lsiw_program *pr = lsiw_program_get(id);
    if (!pr) return;
    struct editor *e = g_new0(struct editor, 1);
    e->page = p;
    e->id = g_strdup(id);
    e->engines = engine_choices();
    guint esel = choice_index(&e->engines, pr->engine);

    e->win = gtk_window_new();
    char *title = g_strdup_printf("%s: settings", pr->name);
    gtk_window_set_title(GTK_WINDOW(e->win), title);
    g_free(title);
    gtk_window_set_modal(GTK_WINDOW(e->win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(e->win), lsiw_gui_window(p->base.widget));
    gtk_window_set_default_size(GTK_WINDOW(e->win), 620, -1);
    g_object_set_data_full(G_OBJECT(e->win), "lsiw-editor", e, editor_free);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);
    int r = 0;
#define ROW(label, w) do { gtk_grid_attach(GTK_GRID(grid), form_label(label, w), 0, r, 1, 1); \
                           gtk_grid_attach(GTK_GRID(grid), w, 1, r, 1, 1); r++; } while (0)
    e->name = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e->name), pr->name);
    gtk_widget_set_hexpand(e->name, TRUE);
    ROW("_Name", e->name);

    GtkWidget *exe_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    e->exe = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e->exe), pr->exe);
    gtk_widget_set_hexpand(e->exe, TRUE);
    gtk_box_append(GTK_BOX(exe_row), e->exe);
    GtkWidget *browse = gtk_button_new_with_mnemonic("_Browse…");
    g_signal_connect(browse, "clicked", G_CALLBACK(editor_browse), e);
    gtk_box_append(GTK_BOX(exe_row), browse);
    gtk_grid_attach(GTK_GRID(grid), form_label("_Program", e->exe), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), exe_row, 1, r++, 1, 1);

    e->args = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e->args), pr->args);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e->args), "Launch arguments, e.g. -windowed -dx11");
    ROW("_Arguments", e->args);
    e->workdir = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e->workdir), pr->workdir);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e->workdir), "Empty: the program's own folder");
    ROW("_Starts in", e->workdir);

    GtkWidget *icon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    e->icon_preview = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(e->icon_preview), 32);
    gtk_box_append(GTK_BOX(icon_row), e->icon_preview);
    e->icon = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e->icon), pr->icon);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e->icon), "Empty: the icon inside the program");
    gtk_widget_set_tooltip_text(e->icon, "The icon of the program's shortcuts: an image (.png, .svg, .jpg, "
                                         ".ico, ...) or an .exe or .dll to take the icon from");
    gtk_widget_set_hexpand(e->icon, TRUE);
    gtk_box_append(GTK_BOX(icon_row), e->icon);
    GtkWidget *icon_browse = gtk_button_new_with_mnemonic("Ch_oose…");
    g_signal_connect(icon_browse, "clicked", G_CALLBACK(editor_icon_browse), e);
    gtk_box_append(GTK_BOX(icon_row), icon_browse);
    GtkWidget *icon_reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_set_tooltip_text(icon_reset, "Use the icon inside the program");
    g_signal_connect(icon_reset, "clicked", G_CALLBACK(editor_icon_reset), e);
    gtk_box_append(GTK_BOX(icon_row), icon_reset);
    gtk_grid_attach(GTK_GRID(grid), form_label("_Icon", e->icon), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), icon_row, 1, r++, 1, 1);

    e->engine = dd_from(e->engines.titles);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(e->engine), esel);
    gtk_widget_set_tooltip_text(e->engine, "The engine or compatibility tool that runs this program: "
                                           "LSIW's native engine, a Wine version or a Proton version");
    ROW("_Engine", e->engine);

    static const char *const prefix_titles[] = { "Shared prefix (every program and game)", "A prefix of its own", NULL };
    e->prefix = gtk_drop_down_new_from_strings(prefix_titles);
    gboolean own = *pr->prefix && g_ascii_strcasecmp(pr->prefix, "shared");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(e->prefix), own ? 1 : 0);
    gtk_widget_set_tooltip_text(e->prefix, "A prefix of its own (~/.local/share/lsiw/prefixes/ID) keeps "
                                           "this program's registry and Windows folder apart; its user "
                                           "folders are still LSIW's folders");
    ROW("P_refix", e->prefix);

    e->dxvk = gtk_drop_down_new_from_strings(tri_titles);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(e->dxvk), tri_index(pr->dxvk));
    gtk_widget_set_tooltip_text(e->dxvk, "Direct3D 9, 10 and 11 through Vulkan. Default: on with Proton, "
                                         "off with Wine (on uses the DXVK of an installed Proton)");
    ROW("_DXVK", e->dxvk);
    e->vkd3d = gtk_drop_down_new_from_strings(tri_titles);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(e->vkd3d), tri_index(pr->vkd3d));
    gtk_widget_set_tooltip_text(e->vkd3d, "Direct3D 12 through Vulkan (VKD3D-Proton)");
    ROW("_VKD3D", e->vkd3d);

    GtkWidget *env_sw = gtk_scrolled_window_new();
    GtkWidget *env = gtk_text_view_new();
    e->env = GTK_TEXT_VIEW(env);
    gtk_text_view_set_monospace(e->env, TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(e->env), pr->env, -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(env_sw), env);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(env_sw), 70);
    gtk_widget_set_tooltip_text(env_sw, "One NAME=value per line, e.g. DXVK_HUD=fps");
    gtk_grid_attach(GTK_GRID(grid), form_label("E_nvironment", env), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), env_sw, 1, r++, 1, 1);

    e->mangohud = gtk_check_button_new_with_mnemonic("Show the _MangoHud overlay");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(e->mangohud), pr->mangohud);
    gtk_grid_attach(GTK_GRID(grid), e->mangohud, 1, r++, 1, 1);
    e->debug = gtk_check_button_new_with_mnemonic("Write a _debug log (LSIW, Wine and Proton)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(e->debug), pr->debug);
    gtk_grid_attach(GTK_GRID(grid), e->debug, 1, r++, 1, 1);
    e->shortcut = gtk_check_button_new_with_mnemonic("Desktop and applications menu s_hortcut");
    char *df = lsiw_program_desktop_file(pr);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(e->shortcut), g_file_test(df, G_FILE_TEST_EXISTS));
    g_free(df);
    gtk_grid_attach(GTK_GRID(grid), e->shortcut, 1, r++, 1, 1);
#undef ROW

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(editor_cancel), e);
    GtkWidget *ok = gtk_button_new_with_mnemonic("_Save");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(ok, "clicked", G_CALLBACK(editor_save), e);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), ok);
    gtk_grid_attach(GTK_GRID(grid), buttons, 0, r, 2, 1);

    g_signal_connect(e->icon, "changed", G_CALLBACK(editor_icon_changed), e);
    editor_icon_preview(e);
    gtk_window_set_child(GTK_WINDOW(e->win), grid);
    gtk_window_set_default_widget(GTK_WINDOW(e->win), ok);
    gtk_window_present(GTK_WINDOW(e->win));
    lsiw_program_free(pr);
}

/* ---- library actions ---- */

static void lib_added_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = data;
    struct prog_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f && p && gtk_widget_get_root(page)) {
        char *path = g_file_get_path(f);
        if (path) {
            struct lsiw_program *pr = lsiw_program_new(path, NULL);
            lsiw_program_store(pr);
            if (lsiw_gui_save_config(page)) {
                fill_library(p, pr->id);
                open_editor(p, pr->id);
            }
            lsiw_program_free(pr);
        }
        g_free(path);
    }
    g_clear_object(&f);
    g_object_unref(page);
}

static void act_lib_add(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Add a Windows program or game to the library");
    GtkFileFilter *progs = gtk_file_filter_new();
    gtk_file_filter_set_name(progs, "Windows programs and shortcuts (*.exe, *.lnk, *.msi, *.bat)");
    gtk_file_filter_add_suffix(progs, "exe");
    gtk_file_filter_add_suffix(progs, "lnk");
    gtk_file_filter_add_suffix(progs, "msi");
    gtk_file_filter_add_suffix(progs, "bat");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, progs);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(progs);
    const char *start_dir = lsiw_root_dir();
    if (g_file_test(start_dir, G_FILE_TEST_IS_DIR)) {
        GFile *start = g_file_new_for_path(start_dir);
        gtk_file_dialog_set_initial_folder(d, start);
        g_object_unref(start);
    }
    gtk_file_dialog_open(d, lsiw_gui_window(p->base.widget), NULL, lib_added_cb, g_object_ref(p->base.widget));
    g_object_unref(d);
}

static void act_lib_edit(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (id) open_editor(p, id);
}

static void lib_cli(struct prog_page *p, const char *sub, const char *id, const char *opt, int mode)
{
    const char *cli = need_cli(p);
    if (!cli) return;
    const char *argv[] = { cli, "programs", sub, id, opt, NULL };
    p->mode = mode;
    lsiw_task_start(p->runner, argv, NULL);
}

static void act_lib_run(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (id) lib_cli(p, "run", id, NULL, MODE_RUN);
}

static void act_lib_shortcut(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (id) lib_cli(p, "shortcut", id, NULL, MODE_NONE);
}

/* An LSIW shortcut (.lwlnk) on the desktop, with the program's icon. */
static void lwlnk_for(struct prog_page *p, const char *id)
{
    const char *cli = need_cli(p);
    if (!cli) return;
    char *keep = g_strdup(id);       /* the row (and its id) is rebuilt when the task ends */
    const char *argv[] = { cli, "lwlnk", "create", keep, NULL };
    p->mode = MODE_LWLNK;
    lsiw_task_start(p->runner, argv, NULL);
    g_free(keep);
}

static void act_lib_lwlnk(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (id) lwlnk_for(p, id);
}

static void act_lib_scan(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    lib_cli(data, "scan", "--add", NULL, MODE_SCAN);
}

static void act_lib_remove(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (!id) return;
    /* `lsiw programs remove` also removes its shortcuts. */
    lib_cli(p, "remove", id, NULL, MODE_NONE);
}

static void lib_selected_cb(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box; (void)row;
    update_lib_actions(data);
}

static void lib_activated_cb(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    const char *id = g_object_get_data(G_OBJECT(row), "lsiw-id");
    if (id) open_editor(data, id);
}

static void entry_activate_cb(GtkEntry *e, gpointer data)
{
    (void)e;
    struct prog_page *p = data;
    GAction *run = g_action_map_lookup_action(G_ACTION_MAP(p->actions), "run");
    if (g_action_get_enabled(run)) g_action_activate(run, NULL);
}

GtkWidget *lsiw_page_programs_new(void)
{
    struct prog_page *p = g_new0(struct prog_page, 1);
    p->base.refresh = prog_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, prog_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), prog_actions,
                                    G_N_ELEMENTS(prog_actions), p);
    gtk_widget_insert_action_group(page, "prog", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Library"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "Your Windows programs and games, each with its own settings: the engine (LSIW's native "
        "engine, a Wine version or a Proton version), DXVK and VKD3D, the prefix (the <b>shared "
        "prefix</b> every program and game uses, or one of its own), launch arguments, "
        "environment variables, MangoHud, a debug log and an icon. Double-click one to change its "
        "settings. <b>Desktop shortcut</b> puts an LSIW shortcut (.lwlnk) with the program's icon on "
        "the desktop that starts it with these settings. <b>Find installed</b> adds what installers "
        "put in the Start menu."));
    p->lib_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->lib_list), GTK_SELECTION_SINGLE);
    GtkWidget *lib_empty = gtk_label_new("No programs yet: Add program… or Find installed.");
    gtk_widget_add_css_class(lib_empty, "lsiw-dim");
    gtk_widget_set_margin_top(lib_empty, 12);
    gtk_widget_set_margin_bottom(lib_empty, 12);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(p->lib_list), lib_empty);
    g_signal_connect(p->lib_list, "row-selected", G_CALLBACK(lib_selected_cb), p);
    g_signal_connect(p->lib_list, "row-activated", G_CALLBACK(lib_activated_cb), p);
    GtkWidget *lib_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lib_sw), p->lib_list);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(lib_sw), 150);
    GtkWidget *lib_frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(lib_frame), lib_sw);
    gtk_box_append(GTK_BOX(page), lib_frame);
    GtkWidget *lrow = lsiw_gui_wrap_row(8);
    GtkWidget *lrun = lsiw_gui_action_button("_Launch", "prog.lib-run");
    gtk_widget_add_css_class(lrun, "suggested-action");
    lsiw_gui_wrap_row_append(lrow, lrun);
    lsiw_gui_wrap_row_append(lrow, lsiw_gui_action_button("_Settings…", "prog.lib-edit"));
    lsiw_gui_wrap_row_append(lrow, lsiw_gui_action_button("_Add program…", "prog.lib-add"));
    lsiw_gui_wrap_row_append(lrow, lsiw_gui_action_button("_Find installed", "prog.lib-scan"));
    GtkWidget *lwlnk = lsiw_gui_action_button("_Desktop shortcut", "prog.lib-lwlnk");
    gtk_widget_set_tooltip_text(lwlnk, "Put an LSIW shortcut (.lwlnk) with the program's icon on the desktop: "
                                       "double-clicking it starts the program with these settings");
    lsiw_gui_wrap_row_append(lrow, lwlnk);
    GtkWidget *menu = lsiw_gui_action_button("Add to applications me_nu", "prog.lib-shortcut");
    gtk_widget_set_tooltip_text(menu, "A launcher in the applications menu (lsiw-ID.desktop)");
    lsiw_gui_wrap_row_append(lrow, menu);
    lsiw_gui_wrap_row_append(lrow, lsiw_gui_action_button("Re_move", "prog.lib-remove"));
    gtk_box_append(GTK_BOX(page), lrow);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Run a Windows program"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "Run a 64-bit Windows program and watch its console output here, or check first "
        "whether LSIW's native engine implements everything it uses. You can also drop a "
        "program file on this page. Windowed programs open their own windows."));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    p->path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->path_entry),
                                   "/home/me/Downloads/setup.exe or C:\\home\\me\\tool.exe");
    gtk_widget_set_hexpand(p->path_entry, TRUE);
    g_signal_connect(p->path_entry, "changed", G_CALLBACK(path_changed_cb), p);
    g_signal_connect(p->path_entry, "activate", G_CALLBACK(entry_activate_cb), p);
    gtk_grid_attach(GTK_GRID(grid), form_label("_Program", p->path_entry), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->path_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lsiw_gui_action_button("_Choose program…", "prog.choose"), 2, 0, 1, 1);

    p->args_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->args_entry), "Optional, e.g. /S \"C:\\my folder\\file.txt\"");
    gtk_widget_set_tooltip_text(p->args_entry, "Separated by spaces; quote an argument that contains "
                                               "spaces, as in a shell");
    g_signal_connect(p->args_entry, "activate", G_CALLBACK(entry_activate_cb), p);
    gtk_grid_attach(GTK_GRID(grid), form_label("_Arguments", p->args_entry), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->args_entry, 1, 1, 2, 1);

    p->engine_dd = gtk_drop_down_new_from_strings(engine_titles);
    lsiw_gui_dropdown_ellipsize(GTK_DROP_DOWN(p->engine_dd));
    gtk_widget_set_halign(p->engine_dd, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), form_label("_Engine", p->engine_dd), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->engine_dd, 1, 2, 2, 1);
    gtk_box_append(GTK_BOX(page), grid);

    GtkWidget *row = lsiw_gui_wrap_row(8);
    GtkWidget *run = lsiw_gui_action_button("_Run", "prog.run");
    gtk_widget_add_css_class(run, "suggested-action");
    lsiw_gui_wrap_row_append(row, run);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("Check _compatibility", "prog.check"));
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_Stop", "prog.stop"));
    p->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->status), 0);
    gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(p->status), 20);
    gtk_label_set_selectable(GTK_LABEL(p->status), TRUE);
    gtk_widget_set_hexpand(p->status, TRUE);
    lsiw_gui_wrap_row_append(row, p->status);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("C_lear output", "prog.clear"));
    gtk_box_append(GTK_BOX(page), row);

    gtk_box_append(GTK_BOX(page), lsiw_gui_output_view(&p->output));
    p->runner = lsiw_task_new(p->output, GTK_LABEL(p->status));
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);

    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(drop_cb), p);
    gtk_widget_add_controller(page, GTK_EVENT_CONTROLLER(drop));

    update_actions(p);
    fill_library(p, NULL);
    return page;
}
