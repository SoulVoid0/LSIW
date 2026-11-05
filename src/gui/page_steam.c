/* LSIW Control: the Steam integration page.
 *
 * Adding the integration (`lsiw steam add`) puts one compatibility tool per
 * installed Proton (Steam's own versions, GE-Proton, builds downloaded with
 * `lsiw runners install ge`) into Steam's compatibilitytools.d. Steam lists
 * them as "LSIW Proton 9.0", "LSIW GE-Proton9-20", ... next to its own
 * Proton versions under a game's Properties > Compatibility. They run that
 * Proton in LSIW's shared prefix ([steam] shared_prefix), whose C: is the
 * C: drive and whose folders are the ones chosen on the Locations page.
 * "Use for every game" (`lsiw steam default --all`) makes LSIW Proton
 * Steam's default and moves existing games to their LSIW twin. Removing the
 * integration (`lsiw steam remove`) deletes the tools.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <string.h>

struct steam_page {
    LsiwPage base;
    GSimpleActionGroup *actions;
    GtkWidget *state, *detail, *tools, *full_c, *shared, *status;
    GtkTextView *output;
    LsiwTask *runner;
    gboolean syncing;
};

static gboolean truthy(const char *v)
{
    return v && (!g_ascii_strcasecmp(v, "true") || !g_ascii_strcasecmp(v, "yes") ||
                 !strcmp(v, "1") || !g_ascii_strcasecmp(v, "on"));
}

static void set_state(GtkWidget *label, gboolean on, const char *text)
{
    char *m = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), m);
    g_free(m);
    gtk_widget_remove_css_class(label, on ? "lsiw-off" : "lsiw-on");
    gtk_widget_add_css_class(label, on ? "lsiw-on" : "lsiw-off");
}

/* The LSIW tools present in a Steam installation, one line each. */
static void list_tools(GString *out, const char *root)
{
    char *dir = g_build_filename(root, "compatibilitytools.d", NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while (d && (n = g_dir_read_name(d))) g_ptr_array_add(names, g_strdup(n));
    if (d) g_dir_close(d);
    g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        char *conf = g_build_filename(dir, name, "lsiw-tool.conf", NULL);
        char *text = NULL;
        if (g_file_get_contents(conf, &text, NULL, NULL)) {
            char *title = NULL, *proton = NULL, *appid = NULL, *alias = NULL;
            char **lines = g_strsplit(text, "\n", -1);
            for (char **l = lines; *l; l++) {
                if (g_str_has_prefix(*l, "name=")) title = g_strdup(*l + 5);
                else if (g_str_has_prefix(*l, "proton=")) proton = g_strdup(*l + 7);
                else if (g_str_has_prefix(*l, "appid=")) appid = g_strdup(*l + 6);
                else if (g_str_has_prefix(*l, "alias=")) alias = g_strdup(*l + 6);
            }
            g_strfreev(lines);
            char *where;
            if (alias) where = g_strdup("follows the version Steam calls stable");
            else if (appid) {
                char *dir = lsiw_steam_app_dir(appid);
                where = dir ? g_strdup(dir) : g_strdup("Valve's; Steam installs it the first time it is used");
                free(dir);
            } else {
                char *script = proton ? g_build_filename(proton, "proton", NULL) : NULL;
                gboolean ok = script && g_file_test(script, G_FILE_TEST_IS_REGULAR);
                where = g_strdup_printf("%s%s", proton ? proton : "",
                                        ok ? "" : "  (its Proton is gone: update the integration)");
                g_free(script);
            }
            char *line = g_markup_printf_escaped("• <b>%s</b>  <span alpha=\"60%%\">%s</span>\n",
                                                 title ? title : name, where);
            g_string_append(out, line);
            g_free(line);
            g_free(where);
            g_free(title);
            g_free(proton);
            g_free(appid);
            g_free(alias);
        }
        g_free(text);
        g_free(conf);
    }
    g_ptr_array_free(names, TRUE);
    g_free(dir);
}

static void update_state(struct steam_page *p)
{
    gboolean on = truthy(lsiw_setting_get("steam", "integration", NULL));
    char **roots = lsiw_steam_roots();
    GString *d = g_string_new(NULL), *tools = g_string_new(NULL);
    gboolean usable = FALSE;
    if (!roots) {
        g_string_append(d, "Steam was not found (looked in ~/.steam, ~/.local/share/Steam and the Flatpak "
                           "and Snap folders). Install Steam, start it once, then look again.");
    } else {
        for (char **r = roots; *r; r++) {
            gboolean sandboxed = lsiw_steam_root_sandboxed(*r);
            if (!sandboxed) usable = TRUE;
            char *line = g_markup_printf_escaped("Steam: <tt>%s</tt>%s\n", *r,
                                                 sandboxed ? " (Flatpak or Snap: not supported, its "
                                                             "sandbox cannot start LSIW)" : "");
            g_string_append(d, line);
            g_free(line);
            list_tools(tools, *r);
        }
    }
    lsiw_strv_free(roots);

    struct lsiw_runner *v;
    size_t nall = lsiw_runners(LSIW_RUNNER_PROTON, &v), n = 0;
    for (size_t i = 0; i < nall; i++) n += !v[i].foreign && strcmp(v[i].source, "Steam");
    struct lsiw_steam_tool *off;
    size_t noff = lsiw_steam_official_tools(&off);
    lsiw_steam_tools_free(off, noff);
    char *count = g_strdup_printf("Steam offers %zu of Valve's Proton versions; %zu other Proton "
                                  "version%s installed (GE-Proton, custom, downloaded).\n",
                                  noff, n, n == 1 ? " is" : "s are");
    g_string_append(d, count);
    g_free(count);
    lsiw_runners_free(v, nall);

    set_state(p->state, on, on ? "Added to Steam" : usable ? "Not added" : "Steam not found");
    gtk_label_set_markup(GTK_LABEL(p->detail), g_strchomp(d->str));
    if (!tools->len) g_string_append(tools, "<span alpha=\"60%\">No LSIW tools in Steam.</span>");
    gtk_label_set_markup(GTK_LABEL(p->tools), g_strchomp(tools->str));
    g_string_free(d, TRUE);
    g_string_free(tools, TRUE);
    lsiw_gui_action_enable(p->actions, "add", usable && !lsiw_task_running(p->runner));
    lsiw_gui_action_enable(p->actions, "remove", !lsiw_task_running(p->runner));

    p->syncing = TRUE;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(p->full_c),
                                truthy(lsiw_setting_get("steam", "full_c_drive", "true")));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(p->shared),
                                truthy(lsiw_setting_get("steam", "shared_prefix", "true")));
    p->syncing = FALSE;
}

static void run_cli(struct steam_page *p, const char *const *args)
{
    const char *cli = lsiw_gui_cli();
    if (!cli) {
        lsiw_gui_alert(p->base.widget, "The lsiw command was not found",
                       "LSIW Control looks for it next to itself and in $PATH. Install LSIW, or "
                       "start LSIW Control from the folder it was built in.");
        return;
    }
    const char *argv[8] = { cli };
    for (int i = 0; args[i] && i < 6; i++) argv[i + 1] = args[i];
    lsiw_task_start(p->runner, argv, NULL);
}

static void act_add(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "steam", "add", NULL };
    run_cli(data, args);
}

static void act_remove(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "steam", "remove", NULL };
    run_cli(data, args);
}

static void act_get_ge(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a;
    /* The build chosen in the Download Proton menu: ge, cachyos, ... */
    const char *args[] = { "runners", "install", g_variant_get_string(param, NULL), NULL };
    run_cli(data, args);
}

static void act_default(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "steam", "default", "--all", NULL };
    run_cli(data, args);
}

static void act_games(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "steam", "games", NULL };
    run_cli(data, args);
}

static void act_refresh(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    update_state(data);
}

static const GActionEntry steam_actions[] = {
    { .name = "add", .activate = act_add },
    { .name = "remove", .activate = act_remove },
    { .name = "get-ge", .activate = act_get_ge, .parameter_type = "s" },
    { .name = "refresh", .activate = act_refresh },
    { .name = "default", .activate = act_default },
    { .name = "games", .activate = act_games },
};

static void shared_toggled_cb(GtkCheckButton *b, gpointer data)
{
    struct steam_page *p = data;
    if (p->syncing) return;
    gboolean on = gtk_check_button_get_active(b);
    lsiw_setting_set("steam", "shared_prefix", on ? "true" : "false");
    if (lsiw_gui_save_config(p->base.widget))
        gtk_label_set_text(GTK_LABEL(p->status), on ? "Games run in LSIW's shared prefix from their next start. "
                                                      "A game's own old prefix is kept as it is."
                                                    : "Games run in their own prefixes from their next start.");
}

static void full_c_toggled_cb(GtkCheckButton *b, gpointer data)
{
    struct steam_page *p = data;
    if (p->syncing) return;
    gboolean on = gtk_check_button_get_active(b);
    lsiw_setting_set("steam", "full_c_drive", on ? "true" : "false");
    if (lsiw_gui_save_config(p->base.widget))
        gtk_label_set_text(GTK_LABEL(p->status), on ? "C:\\ in Proton prefixes shows the whole Linux "
                                                      "filesystem from the next start of a game."
                                                    : "C:\\ in Proton prefixes shows only Windows' own "
                                                      "folders from the next start of a game.");
}

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    struct steam_page *p = data;
    gboolean busy = lsiw_task_running(r);
    lsiw_gui_action_enable(p->actions, "add", !busy);
    lsiw_gui_action_enable(p->actions, "remove", !busy);
    lsiw_gui_action_enable(p->actions, "get-ge", !busy);
    lsiw_gui_action_enable(p->actions, "default", !busy);
    lsiw_gui_action_enable(p->actions, "games", !busy);
}

static void runner_done_cb(LsiwTask *r, gpointer data)
{
    struct steam_page *p = data;
    int code;
    if (lsiw_task_exit_code(r, &code) && code == 0)
        gtk_label_set_text(GTK_LABEL(p->status), "Done. Restart Steam so it sees the change.");
    /* lsiw saved [steam] integration; the config watcher reloads it, and
     * a download changes the Engine page's lists too. */
    lsiw_config_load();
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_SHOWN);
}

static void steam_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    (void)reason;
    update_state((struct steam_page *)base);
}

static void steam_free(gpointer data)
{
    struct steam_page *p = data;
    lsiw_task_free(p->runner);
    g_free(p);
}

static GtkWidget *wrap_label(void)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    return l;
}

GtkWidget *lsiw_page_steam_new(void)
{
    struct steam_page *p = g_new0(struct steam_page, 1);
    p->base.refresh = steam_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, steam_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), steam_actions,
                                    G_N_ELEMENTS(steam_actions), p);
    gtk_widget_insert_action_group(page, "steam", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Steam integration"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "Adds <b>LSIW Proton</b> to Steam: every Proton gets an LSIW twin next to it in Steam's "
        "list: <b>LSIW Proton Stable</b>, <b>LSIW Proton Experimental</b>, <b>LSIW Proton 11.0-2</b> "
        "and every other version of Valve's, Proton 3.7 to Hotfix and Next (Steam installs one the "
        "first time its twin is used), and an LSIW twin of every other installed build: GE-Proton, "
        "Proton-CachyOS, DW-Proton, Proton-GE RTSP, Proton-Tkg, Proton-EM, Proton-Wineland (use "
        "<b>Download Proton</b>), under a game's "
        "<i>Properties › Compatibility › Force the use of a specific Steam Play compatibility "
        "tool</i>. It runs the same Proton, but in <b>LSIW's shared prefix</b>, the one Wine and "
        "every other program use: C:\\ is your C: drive, and Documents, Saved Games, AppData, "
        "Desktop, Downloads, Music, Pictures and Videos are the folders chosen on the Locations "
        "page. <b>Use for every game</b> makes LSIW Proton Experimental Steam's default, as Proton Experimental is Steam's own (games you add later "
        "included) and moves the games already set to a Proton to its LSIW twin; quit Steam "
        "first, its settings file is backed up. A game that already had a prefix of its own keeps "
        "it untouched as a backup; its saves are copied into LSIW's folders the first time it "
        "starts. Restart Steam after adding or removing the integration."));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *l = gtk_label_new("Status");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
    p->state = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->state), 0);
    gtk_grid_attach(GTK_GRID(grid), p->state, 1, 0, 1, 1);
    GtkWidget *again = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(again, "Look again");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(again), "steam.refresh");
    gtk_widget_set_halign(again, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), again, 2, 0, 1, 1);
    p->detail = wrap_label();
    gtk_widget_add_css_class(p->detail, "lsiw-dim");
    gtk_grid_attach(GTK_GRID(grid), p->detail, 1, 1, 2, 1);
    l = gtk_label_new("Tools in Steam");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_valign(l, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), l, 0, 2, 1, 1);
    p->tools = wrap_label();
    gtk_grid_attach(GTK_GRID(grid), p->tools, 1, 2, 2, 1);
    gtk_box_append(GTK_BOX(page), grid);

    GtkWidget *row = lsiw_gui_wrap_row(8);
    GtkWidget *add = lsiw_gui_action_button("_Add or update integration", "steam.add");
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_tooltip_text(add, "Run 'lsiw steam add': an LSIW twin of every Proton Steam lists");
    lsiw_gui_wrap_row_append(row, add);
    GtkWidget *rm = lsiw_gui_action_button("_Remove integration", "steam.remove");
    gtk_widget_set_tooltip_text(rm, "Run 'lsiw steam remove': delete the LSIW tools from Steam. Games set "
                                    "to use one go back to Steam's default.");
    lsiw_gui_wrap_row_append(row, rm);
    GtkWidget *ge = lsiw_gui_proton_download_button("steam.get-ge");
    lsiw_gui_wrap_row_append(row, ge);
    GtkWidget *def = lsiw_gui_action_button("_Use for every game", "steam.default");
    gtk_widget_set_tooltip_text(def, "Run 'lsiw steam default --all' (quit Steam first): LSIW Proton Experimental "
                                     "becomes the default for every game, new ones included, and games "
                                     "set to one of Valve's Protons or another Proton with an LSIW twin "
                                     "move to that twin. Steam's config.vdf is backed up.");
    lsiw_gui_wrap_row_append(row, def);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_List games", "steam.games"));
    gtk_box_append(GTK_BOX(page), row);

    p->shared = gtk_check_button_new_with_mnemonic("Run every game in LSIW's _shared prefix");
    gtk_widget_set_tooltip_text(p->shared, "Off: each game runs in its own steamapps/compatdata prefix, "
                                           "with its user folders linked to LSIW's folders. "
                                           "'lsiw steam own APPID' does this for one game.");
    g_signal_connect(p->shared, "toggled", G_CALLBACK(shared_toggled_cb), p);
    gtk_box_append(GTK_BOX(page), p->shared);

    GtkWidget *full_c_label = gtk_label_new_with_mnemonic(
        "_C:\\ in a game's own prefix shows the whole Linux filesystem, as with LSIW's engine");
    gtk_label_set_wrap(GTK_LABEL(full_c_label), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(full_c_label), 20);
    gtk_label_set_xalign(GTK_LABEL(full_c_label), 0);
    p->full_c = gtk_check_button_new();
    gtk_check_button_set_child(GTK_CHECK_BUTTON(p->full_c), full_c_label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(full_c_label), p->full_c);
    gtk_widget_set_tooltip_text(p->full_c, "Proton keeps its own Z: drive (it needs it); this adds the "
                                           "Linux folders (home, mnt, media, ...) to C:\\ as well.");
    g_signal_connect(p->full_c, "toggled", G_CALLBACK(full_c_toggled_cb), p);
    gtk_box_append(GTK_BOX(page), p->full_c);

    p->status = wrap_label();
    gtk_box_append(GTK_BOX(page), p->status);
    gtk_box_append(GTK_BOX(page), lsiw_gui_output_view(&p->output));
    p->runner = lsiw_task_new(p->output, GTK_LABEL(p->status));
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);
    runner_state_cb(p->runner, p);
    update_state(p);
    return page;
}
