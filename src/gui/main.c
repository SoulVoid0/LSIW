/* LSIW Control (lsiw-control): the GTK 4 settings app of LSIW.
 *
 * One window: a sidebar of pages (Locations, Drives, Engine, Programs,
 * Integration, Steam, Updates), a banner when a newer LSIW is released,
 * and a header bar menu with Check for Updates, About and Quit. The app watches
 * the user's configuration file, so a change made with `lsiw config` in a
 * terminal shows up here at once.
 *
 *   lsiw-control [--page=locations|drive|engine|programs|integration|steam|update]
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const struct {
    const char *name, *title;
    GtkWidget *(*create)(void);
} pages[] = {
    { "locations",   "Locations",   lsiw_page_locations_new },
    { "drive",       "Drives",      lsiw_page_drive_new },
    { "engine",      "Engine",      lsiw_page_engine_new },
    { "programs",    "Programs",    lsiw_page_programs_new },
    { "integration", "Integration", lsiw_page_integration_new },
    { "steam",       "Steam integration", lsiw_page_steam_new },
    { "update",      "Updates",     lsiw_page_update_new },
};

static const char app_css[] =
    ".lsiw-heading { font-weight: 700; font-size: 130%; }\n"
    ".lsiw-bold { font-weight: 700; }\n"
    ".lsiw-dim { opacity: 0.6; }\n"
    ".lsiw-playtime { opacity: 0.8; font-size: 0.92em; }\n"
    ".lsiw-small { font-size: 90%; }\n"
    ".lsiw-mono { font-family: monospace; }\n"
    ".lsiw-on { color: @success_color; }\n"
    ".lsiw-off { color: @warning_color; }\n"
    ".lsiw-banner { padding: 6px 12px; background: alpha(@accent_bg_color, 0.18); "
    "border-bottom: 1px solid alpha(@borders, 0.8); }\n";

/* Watching the configuration file */
static GFileMonitor *config_monitor;
static guint reload_id;

static gboolean is_page_name(const char *name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++)
        if (!strcmp(pages[i].name, name)) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Configuration changes made elsewhere                                 */
/* ------------------------------------------------------------------ */

static gboolean reload_config(gpointer data)
{
    GtkApplication *app = data;
    reload_id = 0;
    /* Also triggered by this app's own saves; reloading those is a no-op. */
    lsiw_config_load();
    for (GList *l = gtk_application_get_windows(app); l; l = l->next) {
        GtkWidget *stack = g_object_get_data(G_OBJECT(l->data), "lsiw-stack");
        if (stack) lsiw_gui_refresh_all(stack, LSIW_REFRESH_CONFIG);
    }
    return G_SOURCE_REMOVE;
}

static void config_file_changed(GFileMonitor *m, GFile *file, GFile *other,
                                GFileMonitorEvent event, gpointer data)
{
    (void)m; (void)file; (void)other;
    if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED ||
        event == G_FILE_MONITOR_EVENT_PRE_UNMOUNT || event == G_FILE_MONITOR_EVENT_UNMOUNTED)
        return;
    /* A save is several events (write, rename); reload once they settle. */
    if (reload_id) g_source_remove(reload_id);
    reload_id = g_timeout_add(300, reload_config, data);
}

static void watch_config(GtkApplication *app)
{
    GFile *f = g_file_new_for_path(lsiw_config_path());
    GError *err = NULL;
    config_monitor = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, NULL, &err);
    if (config_monitor)
        g_signal_connect(config_monitor, "changed", G_CALLBACK(config_file_changed), app);
    else {
        /* Not fatal: changes made elsewhere show after a restart. */
        g_debug("cannot watch %s: %s", lsiw_config_path(), err->message);
        g_error_free(err);
    }
    g_object_unref(f);
}

/* ------------------------------------------------------------------ */
/* Window                                                               */
/* ------------------------------------------------------------------ */

static void visible_page_cb(GObject *stack, GParamSpec *pspec, gpointer data)
{
    (void)pspec; (void)data;
    GtkWidget *child = gtk_stack_get_visible_child(GTK_STACK(stack));
    if (child) lsiw_page_refresh(child, LSIW_REFRESH_SHOWN);
}

/* ------------------------------------------------------------------ */
/* "A new version is available"                                         */
/* ------------------------------------------------------------------ */

static void banner_update_cb(GtkButton *b, gpointer data)
{
    (void)data;
    GtkWidget *win = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(b)));
    GtkWidget *stack = win ? g_object_get_data(G_OBJECT(win), "lsiw-stack") : NULL;
    if (stack) gtk_stack_set_visible_child_name(GTK_STACK(stack), "update");
}

static void banner_close_cb(GtkButton *b, gpointer data)
{
    (void)b;
    gtk_revealer_set_reveal_child(GTK_REVEALER(data), FALSE);
}

void lsiw_gui_update_banner(GtkWidget *any)
{
    GtkWidget *win = any ? GTK_WIDGET(gtk_widget_get_root(any)) : NULL;
    GtkWidget *rev = win ? g_object_get_data(G_OBJECT(win), "lsiw-banner") : NULL;
    GtkWidget *label = win ? g_object_get_data(G_OBJECT(win), "lsiw-banner-label") : NULL;
    if (!rev || !label) return;
    char *latest = NULL;
    lsiw_update_cached(&latest, NULL);
    gboolean avail = latest && lsiw_version_newer(latest, LSIW_VERSION);
    if (avail) {
        char *m = g_markup_printf_escaped("<b>LSIW %s is available.</b> You have %s.", latest, LSIW_VERSION);
        gtk_label_set_markup(GTK_LABEL(label), m);
        g_free(m);
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), avail);
    g_free(latest);
}

static GtkWidget *create_banner(GtkWidget *win)
{
    GtkWidget *rev = gtk_revealer_new();
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(bar, "lsiw-banner");
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(bar), l);
    GtkWidget *b = gtk_button_new_with_mnemonic("_Update…");
    gtk_widget_add_css_class(b, "suggested-action");
    g_signal_connect(b, "clicked", G_CALLBACK(banner_update_cb), NULL);
    gtk_box_append(GTK_BOX(bar), b);
    GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(x, "flat");
    gtk_widget_set_tooltip_text(x, "Hide until LSIW Control starts again");
    g_signal_connect(x, "clicked", G_CALLBACK(banner_close_cb), rev);
    gtk_box_append(GTK_BOX(bar), x);
    gtk_revealer_set_child(GTK_REVEALER(rev), bar);
    g_object_set_data(G_OBJECT(win), "lsiw-banner", rev);
    g_object_set_data(G_OBJECT(win), "lsiw-banner-label", l);
    return rev;
}

static void check_done_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *win = data;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    if (gtk_widget_get_root(win)) {
        lsiw_gui_update_banner(win);
        /* The Updates page shows the new result too. */
        GtkWidget *stack = g_object_get_data(G_OBJECT(win), "lsiw-stack");
        GtkWidget *sw = stack ? gtk_stack_get_child_by_name(GTK_STACK(stack), "update") : NULL;
        GtkWidget *page = sw ? g_object_get_data(G_OBJECT(sw), "lsiw-page-widget") : NULL;
        if (page) lsiw_page_refresh(page, LSIW_REFRESH_CONFIG);
    }
    g_object_unref(win);
}

/* The automatic check: at most once a day, in the background. */
static void auto_check(GtkWidget *win)
{
    lsiw_gui_update_banner(win);
    if (!lsiw_update_auto()) return;
    long long checked = 0;
    lsiw_update_cached(NULL, &checked);
    long long now = (long long)time(NULL);
    if (checked && checked <= now && now - checked < 24 * 60 * 60) return;
    const char *cli = lsiw_gui_cli();
    if (!cli) return;
    GSubprocess *sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                       NULL, cli, "update", "check", "--porcelain", NULL);
    if (!sp) return;
    g_subprocess_wait_async(sp, NULL, check_done_cb, g_object_ref(win));
    g_object_unref(sp);
}

static void act_check_updates(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    GtkWindow *win = gtk_application_get_active_window(data);
    GtkWidget *stack = win ? g_object_get_data(G_OBJECT(win), "lsiw-stack") : NULL;
    if (!stack) return;
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "update");
    GtkWidget *sw = gtk_stack_get_child_by_name(GTK_STACK(stack), "update");
    GtkWidget *page = sw ? g_object_get_data(G_OBJECT(sw), "lsiw-page-widget") : NULL;
    if (page) gtk_widget_activate_action(page, "update.check", NULL);
}

static GtkWidget *create_window(GtkApplication *app)
{
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "LSIW Control");
    gtk_window_set_default_size(GTK_WINDOW(win), 1000, 700);

    GtkWidget *header = gtk_header_bar_new();
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Check for _Updates", "app.check-updates");
    g_menu_append(menu, "_About LSIW Control", "app.about");
    g_menu_append(menu, "_Quit", "app.quit");
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(mb), TRUE);
    gtk_widget_set_tooltip_text(mb, "Main menu");
    g_object_unref(menu);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), mb);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++) {
        /* Each page scrolls, so no page sets how small the window can be:
         * text wraps to the width, and what cannot get narrower scrolls. */
        GtkWidget *page = pages[i].create();
        GtkWidget *sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), page);
        g_object_set_data(G_OBJECT(sw), "lsiw-page-widget", page);
        gtk_stack_add_titled(GTK_STACK(stack), sw, pages[i].name, pages[i].title);
    }
    g_object_set_data(G_OBJECT(win), "lsiw-stack", stack);
    g_signal_connect(stack, "notify::visible-child", G_CALLBACK(visible_page_cb), NULL);

    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_widget_set_size_request(sidebar, 150, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(box), sidebar);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(box), stack);
    gtk_widget_set_vexpand(box, TRUE);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(outer), create_banner(win));
    gtk_box_append(GTK_BOX(outer), box);
    gtk_widget_set_size_request(outer, 480, 320);
    gtk_window_set_child(GTK_WINDOW(win), outer);
    auto_check(win);
    return win;
}

static void show_page(GtkApplication *app, const char *name)
{
    GtkWindow *win = gtk_application_get_active_window(app);
    GtkWidget *stack = win ? g_object_get_data(G_OBJECT(win), "lsiw-stack") : NULL;
    if (stack && name) gtk_stack_set_visible_child_name(GTK_STACK(stack), name);
}

/* ------------------------------------------------------------------ */
/* Application                                                          */
/* ------------------------------------------------------------------ */

static void about_cb(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    GtkApplication *app = data;
    static const char *const authors[] = { "The LSIW contributors", NULL };
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_show_about_dialog(gtk_application_get_active_window(app),
        "program-name", "LSIW",
        "version", LSIW_VERSION,
        "comments", "Linux Subsystem for Improved Wine: runs Windows x86-64 programs on Linux "
                    "with its own engine, using Wine only as a fallback. LSIW Control chooses "
                    "where Windows folders live and how programs run.",
        "website", "https://github.com/SoulVoid0/LSIW",
        "website-label", "Project website",
        "license-type", GTK_LICENSE_AGPL_3_0,
        "copyright", "© 2026 The LSIW contributors",
        "authors", authors,
        "logo-icon-name", gtk_icon_theme_has_icon(theme, "lsiw") ? "lsiw" : "application-x-executable",
        NULL);
}

static void quit_cb(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    GtkApplication *app = data;
    /* Closing the windows lets each page finish cleanly. */
    GList *wins = g_list_copy(gtk_application_get_windows(app));
    for (GList *l = wins; l; l = l->next) gtk_window_destroy(GTK_WINDOW(l->data));
    g_list_free(wins);
}

static const GActionEntry app_actions[] = {
    { .name = "about", .activate = about_cb },
    { .name = "check-updates", .activate = act_check_updates },
    { .name = "quit", .activate = quit_cb },
};

static void startup_cb(GApplication *gapp, gpointer data)
{
    (void)data;
    GtkApplication *app = GTK_APPLICATION(gapp);
    lsiw_config_load();
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), app);
    static const char *const quit_accels[] = { "<Control>q", NULL };
    gtk_application_set_accels_for_action(app, "app.quit", quit_accels);
    gtk_window_set_default_icon_name("lsiw");

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, app_css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    watch_config(app);
}

static void shutdown_cb(GApplication *app, gpointer data)
{
    (void)app; (void)data;
    if (reload_id) g_source_remove(reload_id);
    reload_id = 0;
    g_clear_object(&config_monitor);
}

static void activate_cb(GApplication *gapp, gpointer data)
{
    (void)data;
    GtkApplication *app = GTK_APPLICATION(gapp);
    GtkWindow *win = gtk_application_get_active_window(app);
    if (!win) win = GTK_WINDOW(create_window(app));
    gtk_window_present(win);
}

static int command_line_cb(GApplication *gapp, GApplicationCommandLine *cmd, gpointer data)
{
    (void)data;
    GVariantDict *opts = g_application_command_line_get_options_dict(cmd);
    const char *page = NULL;
    g_variant_dict_lookup(opts, "page", "&s", &page);
    if (page && !is_page_name(page)) {
        g_application_command_line_printerr(cmd, "lsiw-control: unknown page '%s' (use locations, "
                                                 "drive, engine, programs, integration, steam or update)\n", page);
        return 2;
    }
    g_application_activate(gapp);
    show_page(GTK_APPLICATION(gapp), page);
    return 0;
}

static int local_options_cb(GApplication *app, GVariantDict *opts, gpointer data)
{
    (void)app; (void)data;
    if (g_variant_dict_contains(opts, "version")) {
        printf("lsiw-control %s\n", LSIW_VERSION);
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new(LSIW_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option(G_APPLICATION(app), "page", 'p', G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_STRING,
                                  "Page to show: locations, drive, engine, programs, integration, steam or update",
                                  "PAGE");
    g_application_add_main_option(G_APPLICATION(app), "version", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Show the version", NULL);
    g_signal_connect(app, "handle-local-options", G_CALLBACK(local_options_cb), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(startup_cb), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_cb), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate_cb), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(command_line_cb), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
