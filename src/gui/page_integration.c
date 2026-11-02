/* LSIW Control: the Integration page.
 *
 * Two ways Linux can start Windows programs with LSIW:
 *  - the kernel's binfmt_misc: a handler named LSIW hands every file that
 *    starts with "MZ" to lsiw-binfmt, so ./setup.exe runs from a shell.
 *    Its state is /proc/sys/fs/binfmt_misc/LSIW ("enabled"/"disabled" on
 *    the first line). Registering it needs root: `pkexec lsiw register
 *    --system`.
 *  - the desktop: lsiw.desktop as the default application of the .exe MIME
 *    type, so file managers open .exe files with LSIW (`lsiw register
 *    --user`), checked with `xdg-mime query default`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <string.h>

#define BINFMT_DIR   "/proc/sys/fs/binfmt_misc"
#define BINFMT_ENTRY BINFMT_DIR "/LSIW"
#define EXE_MIME     "application/x-ms-dos-executable"
#define LSIW_DESKTOP "lsiw.desktop"

struct integ_page {
    LsiwPage base;
    GSimpleActionGroup *actions;
    GtkWidget *binfmt_state, *binfmt_detail, *mime_state, *mime_detail, *status;
    GtkTextView *output;
    LsiwTask *runner;
    guint query_serial;
    gboolean as_root;        /* the last command ran through pkexec */
};

/* ------------------------------------------------------------------ */
/* binfmt_misc                                                          */
/* ------------------------------------------------------------------ */

static void set_state(GtkWidget *label, gboolean on, const char *text)
{
    char *m = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), m);
    g_free(m);
    gtk_widget_remove_css_class(label, on ? "lsiw-off" : "lsiw-on");
    gtk_widget_add_css_class(label, on ? "lsiw-on" : "lsiw-off");
}

static void update_binfmt(struct integ_page *p)
{
    GString *d = g_string_new(NULL);
    char *contents = NULL;
    if (!g_file_test(BINFMT_DIR "/register", G_FILE_TEST_EXISTS)) {
        set_state(p->binfmt_state, FALSE, "Not available");
        g_string_append(d, "binfmt_misc is not mounted on " BINFMT_DIR ". Registering system-wide "
                           "mounts it and adds the handler.");
    } else if (!g_file_get_contents(BINFMT_ENTRY, &contents, NULL, NULL)) {
        set_state(p->binfmt_state, FALSE, "Not registered");
        g_string_append(d, "Windows programs do not run directly from a shell; use 'lsiw run' or "
                           "register LSIW system-wide.");
    } else {
        /* First line: "enabled" or "disabled"; then "interpreter PATH". */
        char **lines = g_strsplit(contents, "\n", -1);
        gboolean on = lines[0] && !strcmp(g_strstrip(lines[0]), "enabled");
        set_state(p->binfmt_state, on, on ? "Enabled" : "Disabled");
        g_string_append_printf(d, "%s: %s", BINFMT_ENTRY, lines[0] ? lines[0] : "");
        for (char **l = lines; *l; l++)
            if (g_str_has_prefix(*l, "interpreter "))
                g_string_append_printf(d, ", %s", *l);
        g_strfreev(lines);
        char *global = NULL;
        if (g_file_get_contents(BINFMT_DIR "/status", &global, NULL, NULL) &&
            g_str_has_prefix(global, "disabled"))
            g_string_append(d, " (binfmt_misc itself is disabled)");
        g_free(global);
    }
    g_free(contents);

    static const char *const boot[] = {
        "/etc/binfmt.d/zz-lsiw.conf", "/usr/lib/binfmt.d/zz-lsiw.conf",
        "/usr/local/lib/binfmt.d/zz-lsiw.conf", NULL
    };
    const char *found = NULL;
    for (int i = 0; boot[i] && !found; i++)
        if (g_file_test(boot[i], G_FILE_TEST_EXISTS)) found = boot[i];
    if (found) g_string_append_printf(d, "\nRegistered at every boot by %s.", found);
    else g_string_append(d, "\nNot registered at boot.");
    gtk_label_set_text(GTK_LABEL(p->binfmt_detail), d->str);
    g_string_free(d, TRUE);
}

/* ------------------------------------------------------------------ */
/* Desktop default application                                          */
/* ------------------------------------------------------------------ */

static void show_mime_default(struct integ_page *p, const char *app, const char *how)
{
    gboolean ours = app && !strcmp(app, LSIW_DESKTOP);
    if (!app || !*app) set_state(p->mime_state, FALSE, "No default application");
    else if (ours) set_state(p->mime_state, TRUE, "LSIW opens .exe files");
    else {
        char *t = g_strdup_printf("Another application: %s", app);
        set_state(p->mime_state, FALSE, t);
        g_free(t);
    }
    char *user_desktop = g_build_filename(g_get_user_data_dir(), "applications", LSIW_DESKTOP, NULL);
    char *d = g_strdup_printf("Default for %s (%s): %s\n%s %s.", EXE_MIME, how,
                              app && *app ? app : "none", user_desktop,
                              g_file_test(user_desktop, G_FILE_TEST_EXISTS)
                                  ? "is installed" : "is not installed (packages install it system-wide)");
    gtk_label_set_text(GTK_LABEL(p->mime_detail), d);
    g_free(d);
    g_free(user_desktop);
}

struct query {
    GtkWidget *page;    /* reference held */
    guint serial;
};

static void query_done_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    struct query *q = data;
    struct integ_page *p = g_object_get_data(G_OBJECT(q->page), "lsiw-page");
    char *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
    if (p && q->serial == p->query_serial && gtk_widget_get_root(q->page)) {
        if (ok) show_mime_default(p, out ? g_strstrip(out) : NULL, "xdg-mime");
        else gtk_label_set_text(GTK_LABEL(p->mime_detail), err->message);
    }
    g_free(out);
    g_clear_error(&err);
    g_object_unref(q->page);
    g_free(q);
}

static void update_mime(struct integ_page *p)
{
    p->query_serial++;
    char *xdg = g_find_program_in_path("xdg-mime");
    if (!xdg) {
        /* Without xdg-utils, ask GIO, which reads the same mimeapps.list. */
        GAppInfo *app = g_app_info_get_default_for_type(EXE_MIME, FALSE);
        show_mime_default(p, app ? g_app_info_get_id(app) : NULL, "GIO; xdg-mime is not installed");
        g_clear_object(&app);
        return;
    }
    gtk_label_set_text(GTK_LABEL(p->mime_detail), "Checking…");
    const char *argv[] = { xdg, "query", "default", EXE_MIME, NULL };
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                          G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    g_free(xdg);
    if (!proc) {
        gtk_label_set_text(GTK_LABEL(p->mime_detail), err->message);
        g_error_free(err);
        return;
    }
    struct query *q = g_new0(struct query, 1);
    q->page = g_object_ref(p->base.widget);
    q->serial = p->query_serial;
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, query_done_cb, q);
    g_object_unref(proc);
}

/* ------------------------------------------------------------------ */
/* Register / unregister                                                */
/* ------------------------------------------------------------------ */

static void run_lsiw(struct integ_page *p, const char *cmd, const char *scope, gboolean as_root)
{
    const char *cli = lsiw_gui_cli();
    if (!cli) {
        lsiw_gui_alert(p->base.widget, "The lsiw command was not found",
                       "LSIW Control looks for it next to itself and in $PATH.");
        return;
    }
    if (as_root) {
        /* binfmt_misc and /etc/binfmt.d belong to root: ask polkit. */
        char *pkexec = g_find_program_in_path("pkexec");
        if (!pkexec) {
            char *d = g_strdup_printf("pkexec (polkit) is needed to ask for the administrator "
                                      "password. Run this in a terminal instead:\n\nsudo %s %s %s",
                                      cli, cmd, scope);
            lsiw_gui_alert(p->base.widget, "Administrator rights are needed", d);
            g_free(d);
            return;
        }
        const char *argv[] = { pkexec, cli, cmd, scope, NULL };
        p->as_root = TRUE;
        lsiw_task_start(p->runner, argv, NULL);
        g_free(pkexec);
    } else {
        const char *argv[] = { cli, cmd, scope, NULL };
        p->as_root = FALSE;
        lsiw_task_start(p->runner, argv, NULL);
    }
}

static void act_register_user(GSimpleAction *a, GVariant *v, gpointer data)
{
    (void)a; (void)v;
    run_lsiw(data, "register", "--user", FALSE);
}

static void act_unregister_user(GSimpleAction *a, GVariant *v, gpointer data)
{
    (void)a; (void)v;
    run_lsiw(data, "unregister", "--user", FALSE);
}

static void act_register_system(GSimpleAction *a, GVariant *v, gpointer data)
{
    (void)a; (void)v;
    run_lsiw(data, "register", "--system", TRUE);
}

static void act_unregister_system(GSimpleAction *a, GVariant *v, gpointer data)
{
    (void)a; (void)v;
    run_lsiw(data, "unregister", "--system", TRUE);
}

static void integ_refresh(LsiwPage *base, enum lsiw_refresh_reason reason);

static void act_refresh(GSimpleAction *a, GVariant *v, gpointer data)
{
    (void)a; (void)v;
    struct integ_page *p = data;
    integ_refresh(&p->base, LSIW_REFRESH_SHOWN);
}

static const GActionEntry integ_actions[] = {
    { .name = "register-user", .activate = act_register_user },
    { .name = "unregister-user", .activate = act_unregister_user },
    { .name = "register-system", .activate = act_register_system },
    { .name = "unregister-system", .activate = act_unregister_system },
    { .name = "refresh", .activate = act_refresh },
};

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    struct integ_page *p = data;
    gboolean busy = lsiw_task_running(r);
    for (size_t i = 0; i < G_N_ELEMENTS(integ_actions) - 1; i++)
        lsiw_gui_action_enable(p->actions, integ_actions[i].name, !busy);
}

static void runner_done_cb(LsiwTask *r, gpointer data)
{
    struct integ_page *p = data;
    int code;
    /* pkexec: 126 = the user dismissed the password dialog, 127 = not
     * authorized. */
    if (p->as_root && lsiw_task_exit_code(r, &code) && (code == 126 || code == 127))
        gtk_label_set_text(GTK_LABEL(p->status), code == 126
                           ? "Cancelled: the administrator password was not given."
                           : "Not authorized to make system-wide changes.");
    integ_refresh(&p->base, LSIW_REFRESH_SHOWN);
}

/* ------------------------------------------------------------------ */
/* Page                                                                 */
/* ------------------------------------------------------------------ */

static void integ_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    /* Nothing here depends on the configuration file. */
    if (reason == LSIW_REFRESH_CONFIG) return;
    struct integ_page *p = (struct integ_page *)base;
    update_binfmt(p);
    update_mime(p);
}

static void integ_free(gpointer data)
{
    struct integ_page *p = data;
    lsiw_task_free(p->runner);
    g_free(p);
}

static GtkWidget *section(const char *title, const char *text, GtkWidget **state, GtkWidget **detail,
                          GtkWidget *buttons)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    GtkWidget *head = lsiw_gui_wrap_row(12);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "lsiw-bold");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    lsiw_gui_wrap_row_append(head, t);
    *state = gtk_label_new(NULL);
    lsiw_gui_wrap_row_append(head, *state);
    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), lsiw_gui_paragraph(text));
    *detail = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(*detail), 0);
    gtk_label_set_wrap(GTK_LABEL(*detail), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(*detail), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(*detail), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(*detail), TRUE);
    gtk_widget_add_css_class(*detail, "lsiw-dim");
    gtk_box_append(GTK_BOX(box), *detail);
    gtk_box_append(GTK_BOX(box), buttons);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

static GtkWidget *button_row(const char *l1, const char *a1, const char *l2, const char *a2)
{
    GtkWidget *row = lsiw_gui_wrap_row(8);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button(l1, a1));
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button(l2, a2));
    return row;
}

GtkWidget *lsiw_page_integration_new(void)
{
    struct integ_page *p = g_new0(struct integ_page, 1);
    p->base.refresh = integ_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, integ_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), integ_actions,
                                    G_N_ELEMENTS(integ_actions), p);
    gtk_widget_insert_action_group(page, "integ", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *h = lsiw_gui_heading("Desktop and system integration");
    gtk_widget_set_hexpand(h, TRUE);
    gtk_box_append(GTK_BOX(head), h);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Check again");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(refresh), "integ.refresh");
    gtk_box_append(GTK_BOX(head), refresh);
    gtk_box_append(GTK_BOX(page), head);

    gtk_box_append(GTK_BOX(page), section(
        "Open .exe files from the file manager",
        "Makes LSIW the default application for Windows programs (.exe, .msi) for your "
        "account, so a double-click runs them with LSIW.",
        &p->mime_state, &p->mime_detail,
        button_row("Register for this _user", "integ.register-user",
                   "Unregister for this user", "integ.unregister-user")));

    gtk_box_append(GTK_BOX(page), section(
        "Run .exe files directly (binfmt_misc)",
        "Lets the Linux kernel start Windows programs itself, so <tt>./setup.exe</tt> works in a "
        "terminal like any Linux program (it needs execute permission). This is a system-wide "
        "setting and asks for the administrator password.",
        &p->binfmt_state, &p->binfmt_detail,
        button_row("Register _system-wide", "integ.register-system",
                   "Unregister system-wide", "integ.unregister-system")));

    p->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->status), 0);
    gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(p->status), 20);
    gtk_label_set_selectable(GTK_LABEL(p->status), TRUE);
    gtk_box_append(GTK_BOX(page), p->status);

    gtk_box_append(GTK_BOX(page), lsiw_gui_output_view(&p->output));
    p->runner = lsiw_task_new(p->output, GTK_LABEL(p->status));
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);

    integ_refresh(&p->base, LSIW_REFRESH_SHOWN);
    return page;
}
