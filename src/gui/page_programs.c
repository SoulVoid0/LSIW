/* LSIW Control: the Programs page.
 *
 * The library: Windows programs and games the user added (or that `lsiw
 * programs scan` found in the Start menu of the shared prefix), each with
 * its own settings ([program:ID] in the config): engine or compatibility
 * tool (native, a Wine version, a Proton version, the custom chain), prefix
 * (shared by default, or its own), DXVK and VKD3D, launch arguments,
 * environment variables, MangoHud, a debug log, an icon and shortcuts.
 * Programs of the library are started with `lsiw programs run ID`, which
 * records their play time. The library is shown by libview.c as a Grid,
 * a List or Details (the layout the user picked).
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
#include <signal.h>
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
    GtkWidget *path_entry, *args_entry, *engine_dd, *status;
    LsiwLibView *lib;
    GtkTextView *output;
    LsiwTask *runner;
    char *last_dir;
    /* Stop: a button while one program runs, a menu of them while several
     * do (started here, or elsewhere: a .lwlnk, the applications menu). */
    GtkWidget *stop_button, *stop_menu;
    GMenu *stop_items;
    char *stop_sig;               /* what the menu shows, to rebuild it on change */
    struct lsiw_live_session *ext;  /* library programs started elsewhere */
    size_t n_ext;
    GHashTable *ext_stops;        /* pid -> how often Stop was asked */
    GHashTable *ext_names;        /* pid -> the name of a program being stopped */
    guint poll_id;
};

static void update_stop(struct prog_page *p);

static void update_actions(struct prog_page *p)
{
    const char *path = gtk_editable_get_text(GTK_EDITABLE(p->path_entry));
    gboolean have = *path != 0;
    /* Programs run side by side: starting one never waits for another. */
    lsiw_gui_action_enable(p->actions, "run", have);
    lsiw_gui_action_enable(p->actions, "check", have);
    update_stop(p);
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
    char *label = g_path_get_basename(path);
    lsiw_task_start_full(p->runner, (const char *const *)argv->pdata, cwd, label, MODE_RUN);
    g_free(label);
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
    char *base = g_path_get_basename(path);
    char *label = g_strdup_printf("Check of %s", base);
    lsiw_task_start_full(p->runner, argv, NULL, label, MODE_CHECK);
    g_free(label);
    g_free(base);
}

/* ---- stopping ---- */

static char *program_name(const char *id)
{
    struct lsiw_program *pr = lsiw_program_get(id);
    char *n = g_strdup(pr && pr->name && *pr->name ? pr->name : id);
    lsiw_program_free(pr);
    return n;
}

/* A library program started elsewhere: its watcher leads a process group
 * with everything it started (src/cli/library.c), so the group is
 * stopped, as Stop does for programs started here. SIGTERM first, SIGKILL
 * when asked again. */
static void stop_external(struct prog_page *p, long pid)
{
    gpointer key = GINT_TO_POINTER((int)pid);
    int asked = GPOINTER_TO_INT(g_hash_table_lookup(p->ext_stops, key));
    g_hash_table_insert(p->ext_stops, key, GINT_TO_POINTER(asked + 1));
    int sig = asked ? SIGKILL : SIGTERM;
    if (getpgid((pid_t)pid) != (pid_t)pid || kill(-(pid_t)pid, sig) < 0) kill((pid_t)pid, sig);
    for (size_t i = 0; i < p->n_ext; i++) {
        if (p->ext[i].pid != pid) continue;
        char *n = program_name(p->ext[i].id);
        char *m = g_strdup_printf(asked ? "Forcing %s to stop…" : "Stopping %s…", n);
        gtk_label_set_text(GTK_LABEL(p->status), m);
        g_free(m);
        g_hash_table_insert(p->ext_names, key, n);
    }
}

static void act_stop(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    if (lsiw_task_running(p->runner)) lsiw_task_stop(p->runner);
    else if (p->n_ext == 1) stop_external(p, p->ext[0].pid);
}

/* "run:ID" (started here) or "pid:PID" (started elsewhere). */
static void act_stop_one(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a;
    struct prog_page *p = data;
    const char *t = g_variant_get_string(param, NULL);
    if (g_str_has_prefix(t, "run:")) lsiw_task_stop_run(p->runner, (guint)g_ascii_strtoull(t + 4, NULL, 10));
    else if (g_str_has_prefix(t, "pid:")) stop_external(p, (long)g_ascii_strtoll(t + 4, NULL, 10));
}

static void act_stop_all(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    lsiw_task_stop(p->runner);
    for (size_t i = 0; i < p->n_ext; i++) stop_external(p, p->ext[i].pid);
}

static gboolean pid_is_ours(struct prog_page *p, long pid)
{
    for (guint i = 0; i < lsiw_task_count(p->runner); i++)
        if (lsiw_task_run_pid(p->runner, i) == pid) return TRUE;
    return FALSE;
}

/* Re-read the library programs running right now. */
static void refresh_external(struct prog_page *p)
{
    lsiw_playtime_live_free(p->ext, p->n_ext);
    struct lsiw_live_session *v;
    size_t n = lsiw_playtime_live(&v), k = 0;
    for (size_t i = 0; i < n; i++) {
        if (pid_is_ours(p, v[i].pid)) { free(v[i].id); continue; }
        v[k++] = v[i];
    }
    p->ext = v;
    p->n_ext = k;
    /* A program started elsewhere that Stop ended. */
    GHashTableIter it;
    gpointer key, name;
    g_hash_table_iter_init(&it, p->ext_names);
    while (g_hash_table_iter_next(&it, &key, &name)) {
        gboolean alive = FALSE;
        for (size_t i = 0; i < k; i++) if (v[i].pid == GPOINTER_TO_INT(key)) alive = TRUE;
        if (alive) continue;
        char *m = g_strdup_printf("%s was stopped.", (char *)name);
        gtk_label_set_text(GTK_LABEL(p->status), m);
        g_free(m);
        g_hash_table_remove(p->ext_stops, key);
        g_hash_table_iter_remove(&it);
    }
}

static void update_stop(struct prog_page *p)
{
    if (!p->stop_button) return;
    guint ours = lsiw_task_count(p->runner);
    size_t total = ours + p->n_ext;
    lsiw_gui_action_enable(p->actions, "stop", total > 0);
    gtk_widget_set_visible(p->stop_button, total <= 1);
    gtk_widget_set_visible(p->stop_menu, total > 1);
    if (total == 1 && p->n_ext == 1) {
        char *n = program_name(p->ext[0].id);
        char *tip = g_strdup_printf("Stop %s (started outside LSIW Control)", n);
        gtk_widget_set_tooltip_text(p->stop_button, tip);
        g_free(tip);
        g_free(n);
    } else {
        gtk_widget_set_tooltip_text(p->stop_button, NULL);
    }

    /* The menu: one entry for each running program, then Stop all. */
    GString *sig = g_string_new(NULL);
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < ours; i++) {
        g_ptr_array_add(labels, g_strdup_printf("Stop %s", lsiw_task_run_label(p->runner, i)));
        g_ptr_array_add(targets, g_strdup_printf("run:%u", lsiw_task_run_id(p->runner, i)));
    }
    for (size_t i = 0; i < p->n_ext; i++) {
        char *n = program_name(p->ext[i].id);
        char *when = lsiw_playtime_when(p->ext[i].start);
        g_ptr_array_add(labels, g_strdup_printf("Stop %s (started %s, outside LSIW Control)", n, when));
        g_ptr_array_add(targets, g_strdup_printf("pid:%ld", p->ext[i].pid));
        free(when);
        g_free(n);
    }
    /* Two runs of the same program: tell them apart by their order. */
    char **orig = g_new0(char *, labels->len + 1);
    for (guint i = 0; i < labels->len; i++) orig[i] = g_strdup(labels->pdata[i]);
    for (guint i = 0; i < labels->len; i++) {
        int same = 0, nth = 0;
        for (guint j = 0; j < labels->len; j++) {
            if (strcmp(orig[i], orig[j])) continue;
            same++;
            if (j <= i) nth = same;
        }
        if (same > 1) {
            g_free(labels->pdata[i]);
            labels->pdata[i] = g_strdup_printf("%s #%d", orig[i], nth);
        }
    }
    g_strfreev(orig);
    for (guint i = 0; i < labels->len; i++)
        g_string_append_printf(sig, "%s\n%s\n", (char *)labels->pdata[i], (char *)targets->pdata[i]);
    if (g_strcmp0(sig->str, p->stop_sig)) {
        g_free(p->stop_sig);
        p->stop_sig = g_strdup(sig->str);
        g_menu_remove_all(p->stop_items);
        GMenu *each = g_menu_new();
        for (guint i = 0; i < labels->len; i++) {
            GMenuItem *it = g_menu_item_new(labels->pdata[i], NULL);
            g_menu_item_set_action_and_target_value(it, "prog.stop-one",
                                                    g_variant_new_string(targets->pdata[i]));
            g_menu_append_item(each, it);
            g_object_unref(it);
        }
        g_menu_append_section(p->stop_items, NULL, G_MENU_MODEL(each));
        g_object_unref(each);
        GMenu *all = g_menu_new();
        g_menu_append(all, "Stop _all", "prog.stop-all");
        g_menu_append_section(p->stop_items, NULL, G_MENU_MODEL(all));
        g_object_unref(all);
    }
    g_string_free(sig, TRUE);
    g_ptr_array_unref(labels);
    g_ptr_array_unref(targets);
}

static gboolean poll_cb(gpointer data)
{
    struct prog_page *p = data;
    refresh_external(p);
    update_stop(p);
    return G_SOURCE_CONTINUE;
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
    { .name = "stop-one", .activate = act_stop_one, .parameter_type = "s" },
    { .name = "stop-all", .activate = act_stop_all },
    { .name = "clear", .activate = act_clear },
};

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    (void)r;
    refresh_external(data);
    update_actions(data);
    update_lib_actions(data);
}

/* Explain the exit codes `lsiw` documents. */
static void runner_done_cb(LsiwTask *r, gpointer data)
{
    struct prog_page *p = data;
    int code;
    int mode = lsiw_task_last_tag(r);
    if (!lsiw_task_exit_code(r, &code)) return;
    char *m = NULL;
    if (mode == MODE_CHECK) {
        m = g_strdup(code == 0 ? "The native engine can run this program entirely."
                   : code == 1 ? "The native engine cannot run all of it (see the report); "
                                 "with the automatic engine it runs with Wine."
                   : "This is not a Windows program, or it was not found.");
    } else if (mode == MODE_RUN) {
        const char *n = lsiw_task_last_label(r);
        if (code == 126) m = g_strdup_printf("%s could not be run (exit code 126); the output says why.", n);
        else if (code == 127) m = g_strdup_printf("%s, or Wine when it was needed, was not found (exit code 127).", n);
        else if (code == 128 + SIGTERM || code == 128 + SIGKILL || code == 128 + SIGINT) m = g_strdup_printf("%s was stopped.", n);
        else m = g_strdup_printf("%s exited with code %d%s.", n, code, code == 0 ? " (success)" : "");
    }
    else if (mode == MODE_LWLNK)
        m = g_strdup(code == 0 ? "The shortcut is on the desktop: double-click it to start the program "
                                 "with these settings (the path is in the output below)."
                               : "The shortcut could not be made; the output says why.");
    else if (mode == MODE_SCAN && code == 0)
        m = g_strdup("Every installed program found in the Start menu is in the library now.");
    if (m && lsiw_task_running(r)) {
        /* Others still run: say so too. */
        GString *s = g_string_new(m);
        g_string_append(s, " Still running: ");
        for (guint i = 0; i < lsiw_task_count(r); i++)
            g_string_append_printf(s, "%s%s", i ? ", " : "", lsiw_task_run_label(r, i));
        g_string_append(s, ".");
        g_free(m);
        m = g_string_free(s, FALSE);
    }
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
    if (p->poll_id) g_source_remove(p->poll_id);
    lsiw_task_free(p->runner);
    lsiw_playtime_live_free(p->ext, p->n_ext);
    g_hash_table_unref(p->ext_stops);
    g_hash_table_unref(p->ext_names);
    g_clear_object(&p->stop_items);
    g_free(p->stop_sig);
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
    return p->lib ? lsiw_libview_selected(p->lib) : NULL;
}

static void update_lib_actions(struct prog_page *p)
{
    gboolean sel = selected_id(p) != NULL;
    lsiw_gui_action_enable(p->actions, "lib-edit", sel);
    lsiw_gui_action_enable(p->actions, "lib-remove", sel);
    lsiw_gui_action_enable(p->actions, "lib-run", sel);
    lsiw_gui_action_enable(p->actions, "lib-shortcut", sel);
    lsiw_gui_action_enable(p->actions, "lib-lwlnk", sel);
    lsiw_gui_action_enable(p->actions, "lib-scan", TRUE);
}

static void lwlnk_for(struct prog_page *p, const char *id);
static void open_editor(struct prog_page *p, const char *id);

static void lib_changed_cb(gpointer data)
{
    update_lib_actions(data);
}

static void lib_activated_cb(const char *id, gpointer data)
{
    open_editor(data, id);
}

/* The desktop-shortcut button of a row of the List layout. */
static void lib_shortcut_cb(const char *id, gpointer data)
{
    lwlnk_for(data, id);
}

static void fill_library(struct prog_page *p, const char *select)
{
    if (p->lib) lsiw_libview_reload(p->lib, select);
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
            lsiw_task_start_full(p->runner, argv, NULL, NULL, MODE_NONE);
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

static void lib_cli(struct prog_page *p, const char *sub, const char *id, const char *opt, int mode,
                    const char *label)
{
    const char *cli = need_cli(p);
    if (!cli) return;
    const char *argv[] = { cli, "programs", sub, id, opt, NULL };
    lsiw_task_start_full(p->runner, argv, NULL, label, mode);
}

static void act_lib_run(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (!id) return;
    char *keep = g_strdup(id), *name = program_name(id);
    lib_cli(p, "run", keep, NULL, MODE_RUN, name);
    g_free(name);
    g_free(keep);
}

static void act_lib_shortcut(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (id) lib_cli(p, "shortcut", id, NULL, MODE_NONE, NULL);
}

/* An LSIW shortcut (.lwlnk) on the desktop, with the program's icon. */
static void lwlnk_for(struct prog_page *p, const char *id)
{
    const char *cli = need_cli(p);
    if (!cli) return;
    char *keep = g_strdup(id);       /* the row (and its id) is rebuilt when the task ends */
    const char *argv[] = { cli, "lwlnk", "create", keep, NULL };
    lsiw_task_start_full(p->runner, argv, NULL, NULL, MODE_LWLNK);
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
    lib_cli(data, "scan", "--add", NULL, MODE_SCAN, NULL);
}

static void act_lib_remove(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct prog_page *p = data;
    const char *id = selected_id(p);
    if (!id) return;
    /* `lsiw programs remove` also removes its shortcuts. */
    char *keep = g_strdup(id);
    lib_cli(p, "remove", keep, NULL, MODE_NONE, NULL);
    g_free(keep);
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
        "environment variables, MangoHud, a debug log and an icon. Show them as a <b>Grid</b>, a "
        "<b>List</b> or <b>Details</b> with sortable columns. LSIW keeps track of how long each one "
        "runs when it is started from here, from its shortcuts or with <tt>lsiw programs run</tt>: "
        "play time, launches and when it was last played. Double-click one to change its "
        "settings. <b>Desktop shortcut</b> puts an LSIW shortcut (.lwlnk) with the program's icon on "
        "the desktop that starts it with these settings. <b>Find installed</b> adds what installers "
        "put in the Start menu."));
    p->lib = lsiw_libview_new(lib_changed_cb, lib_activated_cb, lib_shortcut_cb, p);
    gtk_box_append(GTK_BOX(page), lsiw_libview_widget(p->lib));
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
    p->stop_button = lsiw_gui_action_button("_Stop", "prog.stop");
    lsiw_gui_wrap_row_append(row, p->stop_button);
    p->stop_items = g_menu_new();
    p->stop_menu = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(p->stop_menu), "_Stop");
    gtk_menu_button_set_use_underline(GTK_MENU_BUTTON(p->stop_menu), TRUE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(p->stop_menu), G_MENU_MODEL(p->stop_items));
    gtk_widget_set_tooltip_text(p->stop_menu, "Several programs are running: choose the one to stop");
    gtk_widget_set_visible(p->stop_menu, FALSE);
    lsiw_gui_wrap_row_append(row, p->stop_menu);
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
    lsiw_task_set_multi(p->runner, TRUE);
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);
    p->ext_stops = g_hash_table_new(NULL, NULL);
    p->ext_names = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    refresh_external(p);
    p->poll_id = g_timeout_add_seconds(3, poll_cb, p);

    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(drop_cb), p);
    gtk_widget_add_controller(page, GTK_EVENT_CONTROLLER(drop));

    update_actions(p);
    fill_library(p, NULL);
    return page;
}
