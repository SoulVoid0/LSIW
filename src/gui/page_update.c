/* LSIW Control: the Updates page.
 *
 * Shows the installed version and the newest release LSIW knows of (the
 * last check, <data>/update-check), turns the automatic daily check on or
 * off ([update] check), and runs `lsiw update check` and `lsiw update
 * install`, which downloads the new release for the way LSIW is installed
 * (rpm, deb or AppImage), verifies it against the release's SHA256SUMS and
 * installs it (asking for the administrator password through pkexec).
 * The window shows a banner when a newer release is known (main.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct update_page {
    LsiwPage base;
    GSimpleActionGroup *actions;
    GtkWidget *current, *latest, *auto_check, *install_btn, *status;
    GtkTextView *output;
    LsiwTask *runner;
    gboolean syncing;
};

static void update_labels(struct update_page *p)
{
    char *latest = NULL;
    long long checked = 0;
    lsiw_update_cached(&latest, &checked);
    char *cur = g_strdup_printf("Installed: <b>LSIW %s</b>", LSIW_VERSION);
    gtk_label_set_markup(GTK_LABEL(p->current), cur);
    g_free(cur);
    char when[64] = "";
    if (checked) {
        time_t t = (time_t)checked;
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&t));
    }
    gboolean avail = latest && lsiw_version_newer(latest, LSIW_VERSION);
    char *m;
    if (!latest)
        m = g_strdup(checked ? "Newest release: not known yet (the last check could not reach GitHub)."
                             : "Newest release: not checked yet.");
    else if (avail)
        m = g_markup_printf_escaped("Newest release: <b>LSIW %s</b>, a new version (checked %s).", latest, when);
    else
        m = g_markup_printf_escaped("Newest release: LSIW %s. You have the newest version (checked %s).",
                                    latest, when);
    gtk_label_set_markup(GTK_LABEL(p->latest), m);
    g_free(m);
    char *b = avail ? g_strdup_printf("_Update to %s", latest) : g_strdup("_Reinstall or update…");
    gtk_button_set_label(GTK_BUTTON(p->install_btn), b);
    gtk_button_set_use_underline(GTK_BUTTON(p->install_btn), TRUE);
    g_free(b);
    if (avail) gtk_widget_add_css_class(p->install_btn, "suggested-action");
    else gtk_widget_remove_css_class(p->install_btn, "suggested-action");
    gtk_widget_set_visible(p->install_btn, avail);
    g_free(latest);
}

static void run_cli(struct update_page *p, const char *const *args)
{
    const char *cli = lsiw_gui_cli();
    if (!cli) {
        lsiw_gui_alert(p->base.widget, "The lsiw command was not found",
                       "Checking for and installing updates is done by 'lsiw update'. Install LSIW, "
                       "or start LSIW Control from the folder it was built in.");
        return;
    }
    const char *argv[8];
    int n = 0;
    argv[n++] = cli;
    for (int i = 0; args[i] && n < 7; i++) argv[n++] = args[i];
    argv[n] = NULL;
    lsiw_task_clear(p->runner);
    lsiw_task_start(p->runner, argv, NULL);
}

static void act_check(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    static const char *const args[] = { "update", "check", NULL };
    run_cli(data, args);
}

static void act_install(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    static const char *const args[] = { "update", "install", "--yes", NULL };
    run_cli(data, args);
}

static void act_notes(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct update_page *p = data;
    GtkUriLauncher *l = gtk_uri_launcher_new("https://github.com/" LSIW_REPO "/releases/latest");
    gtk_uri_launcher_launch(l, lsiw_gui_window(p->base.widget), NULL, NULL, NULL);
    g_object_unref(l);
}

static const GActionEntry update_actions[] = {
    { .name = "check", .activate = act_check },
    { .name = "install", .activate = act_install },
    { .name = "notes", .activate = act_notes },
};

static void auto_toggled_cb(GtkCheckButton *b, gpointer data)
{
    struct update_page *p = data;
    if (p->syncing) return;
    lsiw_setting_set("update", "check", gtk_check_button_get_active(b) ? NULL : "false");
    lsiw_gui_save_config(p->base.widget);
}

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    struct update_page *p = data;
    gboolean busy = lsiw_task_running(r);
    lsiw_gui_action_enable(p->actions, "check", !busy);
    lsiw_gui_action_enable(p->actions, "install", !busy);
}

static void runner_done_cb(LsiwTask *r, gpointer data)
{
    (void)r;
    struct update_page *p = data;
    update_labels(p);
    lsiw_gui_update_banner(p->base.widget);
}

static void update_free(gpointer data)
{
    struct update_page *p = data;
    lsiw_task_free(p->runner);
    g_free(p);
}

static void update_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    (void)reason;
    struct update_page *p = (struct update_page *)base;
    p->syncing = TRUE;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(p->auto_check), lsiw_update_auto());
    p->syncing = FALSE;
    update_labels(p);
}

static GtkWidget *plain_label(void)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), 20);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    return l;
}

GtkWidget *lsiw_page_update_new(void)
{
    struct update_page *p = g_new0(struct update_page, 1);
    p->base.refresh = update_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, update_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), update_actions,
                                    G_N_ELEMENTS(update_actions), p);
    gtk_widget_insert_action_group(page, "update", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Updates"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "New versions of LSIW are published on GitHub. LSIW Control can install one for you: "
        "it downloads the packages for the way LSIW is installed here (rpm, deb or AppImage), "
        "checks every file against the release's SHA256SUMS, and installs them, asking for "
        "your password. Your settings, programs and prefixes are kept."));

    p->current = plain_label();
    gtk_box_append(GTK_BOX(page), p->current);
    p->latest = plain_label();
    gtk_box_append(GTK_BOX(page), p->latest);

    GtkWidget *row = lsiw_gui_wrap_row(8);
    p->install_btn = lsiw_gui_action_button("_Update", "update.install");
    lsiw_gui_wrap_row_append(row, p->install_btn);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_Check now", "update.check"));
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_Release notes", "update.notes"));
    gtk_box_append(GTK_BOX(page), row);

    p->auto_check = gtk_check_button_new_with_mnemonic(
        "Check for new versions _automatically (once a day; nothing is installed without asking)");
    g_signal_connect(p->auto_check, "toggled", G_CALLBACK(auto_toggled_cb), p);
    gtk_box_append(GTK_BOX(page), p->auto_check);

    p->status = plain_label();
    gtk_box_append(GTK_BOX(page), p->status);
    GtkWidget *out = lsiw_gui_output_view(&p->output);
    gtk_widget_set_vexpand(out, TRUE);
    gtk_widget_set_size_request(out, -1, 160);
    gtk_box_append(GTK_BOX(page), out);
    p->runner = lsiw_task_new(p->output, GTK_LABEL(p->status));
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);
    runner_state_cb(p->runner, p);

    update_refresh(&p->base, LSIW_REFRESH_SHOWN);
    return page;
}
