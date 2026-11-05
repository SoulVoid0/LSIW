/* LSIW Control: the Engine page.
 *
 * LSIW runs Windows programs with its own native engine (its PE loader and
 * Win32 implementation). Wine is only the fallback for what the native
 * engine cannot run yet; Proton can be chosen instead. This page sets the
 * default engine of `lsiw run` ([engine] default), which Wine version
 * ([engine] wine) and which Proton version ([engine] proton) are used,
 * downloads Wine builds and GE-Proton (`lsiw runners install`), and sets
 * up the shared prefix (`lsiw wine-setup`). Every Wine and Proton version
 * shares that one prefix by default, so every engine sees the same C:
 * drive and the folders chosen on the Locations page.
 *
 * "Custom configuration" ([engine] default = custom) is a default engine
 * plus up to three fallbacks ([engine] chain), tried in order when the one
 * before cannot run a program.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <string.h>
#include <strings.h>

static const char *const engine_ids[] = { "auto", "native", "wine", "proton", "custom" };
static const char *const engine_titles[] = {
    "Automatic: native engine, Wine when needed",
    "Native engine only",
    "Wine only",
    "Proton only",
    "Custom configuration: a default engine and fallbacks",
    NULL
};
static const char *const engine_hints[] = {
    "Each program runs with LSIW's own engine when every Windows function it uses is "
    "implemented, and with Wine otherwise (also 32-bit programs, .msi installers and batch files).",
    "Programs always run with LSIW's own engine. A program that calls a function LSIW does "
    "not implement yet stops with a message naming it.",
    "Programs always run with Wine, in LSIW's own Wine prefix with the same C: drive and folders.",
    "Programs always run with Proton (Valve's Proton or GE-Proton, with DXVK and VKD3D-Proton for "
    "games), in the same shared prefix as Wine, with the same C: drive and folders.",
    "Choose the engine to try first and up to three fallbacks below. When an engine cannot run a "
    "program (it is not installed, the program uses functions it lacks, it is the wrong kind of "
    "program, or it cannot start), the next one is tried.",
};

#define CHAIN_SLOTS 4      /* LSIW_CHAIN_MAX: the default and three fallbacks */

/* A dropdown of installed versions; values[i] is the setting for row i
 * ("" for automatic). */
struct versions {
    GtkWidget *dd;
    GtkStringList *model;
    GPtrArray *values;
    int kind;
};

struct engine_page {
    LsiwPage base;
    GSimpleActionGroup *actions;
    GtkWidget *engine_dd, *engine_hint, *engine_status;
    GtkWidget *wine_status, *proton_status, *prefix_label, *run_status;
    struct versions wine, proton;
    GtkTextView *output;
    LsiwTask *runner;
    gboolean syncing;        /* a dropdown is being set from the config */
    GtkWidget *chain_box, *chain_dd[CHAIN_SLOTS];
    GtkStringList *chain_model;
    GPtrArray *chain_values;  /* setting of each chain_model row ("" = none)  */
    char *wine_checked;      /* the setting Wine was last detected for */
    guint detect_serial;
};

/* ------------------------------------------------------------------ */
/* Default engine                                                       */
/* ------------------------------------------------------------------ */

static guint engine_index(const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(engine_ids); i++)
        if (!strcmp(engine_ids[i], id)) return i;
    return 0;
}

static void engine_selected_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    struct engine_page *p = data;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->engine_dd));
    if (i >= G_N_ELEMENTS(engine_ids)) return;
    gtk_label_set_text(GTK_LABEL(p->engine_hint), engine_hints[i]);
    gtk_widget_set_visible(p->chain_box, !strcmp(engine_ids[i], "custom"));
    if (p->syncing || !strcmp(lsiw_gui_default_engine(), engine_ids[i])) return;

    lsiw_setting_set("engine", "default", engine_ids[i]);
    lsiw_setting_set("runtime", "backend", NULL);      /* the older key, as `lsiw config engine` */
    if (lsiw_gui_save_config(p->base.widget)) {
        char *m = g_strdup_printf("Default engine: %s. Saved in %s.", engine_ids[i], lsiw_config_path());
        gtk_label_set_text(GTK_LABEL(p->engine_status), m);
        g_free(m);
    }
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_CONFIG);
}

/* ------------------------------------------------------------------ */
/* Custom configuration: default engine and fallbacks                   */
/* ------------------------------------------------------------------ */

static void chain_add(struct engine_page *p, const char *title, const char *value)
{
    gtk_string_list_append(p->chain_model, title);
    g_ptr_array_add(p->chain_values, g_strdup(value));
}

static guint chain_index(struct engine_page *p, const char *value)
{
    for (guint i = 0; i < p->chain_values->len; i++)
        if (!g_ascii_strcasecmp(g_ptr_array_index(p->chain_values, i), value)) return i;
    char *t = g_strdup_printf("%s (not installed)", value);
    chain_add(p, t, value);
    g_free(t);
    return p->chain_values->len - 1;
}

/* Every engine and every installed version, in each of the four dropdowns. */
static void fill_chain(struct engine_page *p)
{
    p->syncing = TRUE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->chain_model));
    gtk_string_list_splice(p->chain_model, 0, n, NULL);
    g_ptr_array_set_size(p->chain_values, 0);
    chain_add(p, "None", "");
    chain_add(p, "Native engine", "native");
    chain_add(p, "Automatic (native, Wine when needed)", "auto");
    chain_add(p, "Wine (the version chosen above)", "wine");
    struct lsiw_runner *v;
    size_t nr = lsiw_runners(LSIW_RUNNER_WINE, &v);
    for (size_t i = 0; i < nr; i++) {
        if (v[i].foreign) continue;
        char *t = g_strdup_printf("Wine: %s", v[i].name);
        char *val = g_strdup_printf("wine:%s", v[i].id);
        chain_add(p, t, val);
        g_free(t);
        g_free(val);
    }
    lsiw_runners_free(v, nr);
    chain_add(p, "Proton (the version chosen above)", "proton");
    nr = lsiw_runners(LSIW_RUNNER_PROTON, &v);
    for (size_t i = 0; i < nr; i++) {
        if (v[i].foreign) continue;
        char *t = g_strdup_printf("Proton: %s", v[i].name);
        char *val = g_strdup_printf("proton:%s", v[i].id);
        chain_add(p, t, val);
        g_free(t);
        g_free(val);
    }
    lsiw_runners_free(v, nr);

    char **chain;
    size_t nc = lsiw_engine_chain(&chain);
    for (int k = 0; k < CHAIN_SLOTS; k++)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(p->chain_dd[k]), (size_t)k < nc ? chain_index(p, chain[k]) : 0);
    lsiw_strv_free(chain);
    p->syncing = FALSE;
}

static void chain_selected_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    struct engine_page *p = data;
    if (p->syncing) return;
    GString *s = g_string_new(NULL);
    for (int k = 0; k < CHAIN_SLOTS; k++) {
        guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->chain_dd[k]));
        const char *v = i < p->chain_values->len ? g_ptr_array_index(p->chain_values, i) : "";
        if (!*v) continue;
        if (s->len) g_string_append_c(s, ',');
        g_string_append(s, v);
    }
    if (!s->len) g_string_append(s, "none");
    lsiw_setting_set("engine", "chain", s->str);
    if (lsiw_gui_save_config(p->base.widget)) {
        char *m = g_strdup_printf("Engines, in order: %s. Saved in %s.", s->str, lsiw_config_path());
        gtk_label_set_text(GTK_LABEL(p->engine_status), m);
        g_free(m);
    }
    g_string_free(s, TRUE);
}

/* ------------------------------------------------------------------ */
/* Wine detection                                                       */
/* ------------------------------------------------------------------ */

/* Runs in a worker thread: `wine --version`, which can take a moment the
 * first time. Returns Wine's own version line ("wine-9.0 (Staging)"). */
static void version_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    (void)src; (void)cancel;
    const char *wine = data;
    char *argv[] = { (char *)wine, "--version", NULL };
    char **env = g_environ_setenv(g_get_environ(), "WINEDEBUG", "-all", TRUE);
    char *out = NULL;
    int status = 0;
    GError *err = NULL;
    if (!g_spawn_sync(NULL, argv, env, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      &out, NULL, &status, &err)) {
        g_strfreev(env);
        g_task_return_error(task, err);
        return;
    }
    g_strfreev(env);
    /* Distribution wrappers may print hints first; keep Wine's line. */
    char *res = NULL;
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (char **l = lines; *l && !res; l++)
        if (g_str_has_prefix(*l, "wine-")) res = g_strdup(g_strstrip(*l));
    for (char **l = lines; *l && !res; l++)
        if (*g_strstrip(*l)) res = g_strdup(*l);
    g_strfreev(lines);
    g_free(out);
    if (!res) res = g_strdup("version unknown");
    g_task_return_pointer(task, res, g_free);
}

static void version_done_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = GTK_WIDGET(src);
    guint serial = GPOINTER_TO_UINT(data);
    struct engine_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GError *err = NULL;
    char *ver = g_task_propagate_pointer(G_TASK(res), &err);
    if (p && serial == p->detect_serial && gtk_widget_get_root(page)) {
        const char *wine = g_task_get_task_data(G_TASK(res));
        char *m = ver ? g_markup_printf_escaped("<b>Wine found:</b> %s (%s)", wine, ver)
                      : g_markup_printf_escaped("<b>Wine found:</b> %s, but it did not answer: %s",
                                                wine, err->message);
        gtk_label_set_markup(GTK_LABEL(p->wine_status), m);
        g_free(m);
    }
    g_free(ver);
    g_clear_error(&err);
}

static void detect_wine(struct engine_page *p)
{
    const char *cfg = lsiw_gui_wine_setting();
    g_free(p->wine_checked);
    p->wine_checked = g_strdup(cfg);
    p->detect_serial++;

    char *wine = lsiw_gui_find_wine();
    if (!wine) {
        gtk_label_set_markup(GTK_LABEL(p->wine_status),
            *cfg ? "<b>Wine not found</b>: neither the configured command nor wine/wine64 in $PATH. "
                   "On Fedora: <tt>sudo dnf install wine</tt>"
                 : "<b>Wine is not installed.</b> Programs the native engine cannot run will not "
                   "start. On Fedora: <tt>sudo dnf install wine</tt>");
        return;
    }
    char *m = g_markup_printf_escaped("<b>Wine found:</b> %s (checking the version…)", wine);
    gtk_label_set_markup(GTK_LABEL(p->wine_status), m);
    g_free(m);

    GTask *task = g_task_new(p->base.widget, NULL, version_done_cb, GUINT_TO_POINTER(p->detect_serial));
    g_task_set_task_data(task, wine, g_free);
    g_task_run_in_thread(task, version_thread);
    g_object_unref(task);
}

/* ------------------------------------------------------------------ */
/* Wine and Proton versions                                             */
/* ------------------------------------------------------------------ */

static const char *proton_setting(void)
{
    const char *v = lsiw_setting_get("engine", "proton", NULL);
    return v ? v : "";
}

/* Refill a version dropdown from what is installed and select `current`. */
static void fill_versions(struct engine_page *p, struct versions *v, const char *current)
{
    p->syncing = TRUE;
    guint n_old = g_list_model_get_n_items(G_LIST_MODEL(v->model));
    g_ptr_array_set_size(v->values, 0);
    GPtrArray *titles = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(titles, g_strdup(v->kind == LSIW_RUNNER_WINE
                                     ? "Automatic: wine from $PATH"
                                     : "Automatic: the newest official Proton in Steam"));
    g_ptr_array_add(v->values, g_strdup(""));
    struct lsiw_runner *r;
    size_t n = lsiw_runners(v->kind, &r);
    struct lsiw_runner *sel = *current ? lsiw_runner_pick(v->kind, current) : NULL;
    guint pick = 0;
    for (size_t i = 0; i < n; i++) {
        g_ptr_array_add(titles, g_strdup_printf("%s  (%s)%s", r[i].name, r[i].source,
                                                r[i].foreign ? "  – built for another processor" : ""));
        g_ptr_array_add(v->values, g_strdup(r[i].id));
        if (sel && !strcmp(sel->path, r[i].path) && (!strcmp(current, r[i].id) || !strcmp(current, r[i].name)
                                                     || !strcmp(current, r[i].path)))
            pick = v->values->len - 1;
    }
    if (*current && !pick) {
        /* A path that is not one of the versions found. */
        g_ptr_array_add(titles, g_strdup_printf("Other: %s%s", current, sel ? "" : " (not found)"));
        g_ptr_array_add(v->values, g_strdup(current));
        pick = v->values->len - 1;
    }
    g_ptr_array_add(titles, NULL);
    gtk_string_list_splice(v->model, 0, n_old, (const char *const *)titles->pdata);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v->dd), pick);
    g_ptr_array_free(titles, TRUE);
    lsiw_runner_free(sel);
    lsiw_runners_free(r, n);
    p->syncing = FALSE;
}

static void add_row(struct versions *v, GPtrArray *titles, char *title, const char *value)
{
    g_ptr_array_add(titles, title);
    g_ptr_array_add(v->values, g_strdup(value));
}

/* The Proton dropdown: LSIW's twin of every Proton Valve publishes in
 * Steam (installed or not: Steam downloads it when it is first used),
 * then LSIW's twin of every other Proton found. Every one of them runs
 * in LSIW's Proton prefix with LSIW's folders. */
static void fill_proton_versions(struct engine_page *p, struct versions *v, const char *current)
{
    p->syncing = TRUE;
    guint n_old = g_list_model_get_n_items(G_LIST_MODEL(v->model));
    g_ptr_array_set_size(v->values, 0);
    GPtrArray *titles = g_ptr_array_new_with_free_func(g_free);
    add_row(v, titles, g_strdup("Automatic: the newest installed LSIW Proton"), "");
    struct lsiw_runner *sel = *current ? lsiw_runner_pick(LSIW_RUNNER_PROTON, current) : NULL;
    guint pick = 0;

    struct lsiw_steam_tool *off;
    size_t noff = lsiw_steam_official_tools(&off);
    int legacy = strcasecmp(lsiw_setting_get("steam", "legacy_versions", "true"), "false") != 0;
    char **dirs = g_new0(char *, noff + 1);
    const struct lsiw_steam_tool *stable = lsiw_steam_tool_by_alias(off, noff, "proton-stable");
    if (stable) {
        add_row(v, titles, g_strdup_printf("LSIW Proton Stable  (now %s)", stable->display), "steam:proton-stable");
        if (!strcmp(current, "steam:proton-stable")) pick = v->values->len - 1;
    }
    for (size_t i = 0; i < noff; i++) {
        char *dir = lsiw_steam_app_dir(off[i].appid);
        char *launcher = dir ? g_build_filename(dir, "proton", NULL) : NULL;
        if (launcher && g_file_test(launcher, G_FILE_TEST_IS_REGULAR)) dirs[i] = dir;
        else g_free(dir);
        g_free(launcher);
        if (off[i].legacy && !legacy && !dirs[i]) continue;
        char *value = g_strdup_printf("steam:%s", off[i].key);
        add_row(v, titles, g_strdup_printf("LSIW %s%s", off[i].display,
                                           dirs[i] ? "" : "  (Steam downloads it when first used)"), value);
        /* Also an older setting naming the folder of this version. */
        if (!strcmp(current, value) || (sel && dirs[i] && strncmp(current, "steam:", 6) &&
                                        !strcmp(sel->path, dirs[i])))
            pick = v->values->len - 1;
        g_free(value);
    }

    struct lsiw_runner *r;
    size_t n = lsiw_runners(LSIW_RUNNER_PROTON, &r);
    for (size_t i = 0; i < n; i++) {
        int twin = 0;
        for (size_t j = 0; j < noff && !twin; j++) twin = dirs[j] && !strcmp(dirs[j], r[i].path);
        if (twin) continue;                 /* listed above as the official version */
        add_row(v, titles, g_strdup_printf("LSIW %s  (%s)%s", r[i].name, r[i].source,
                                           r[i].foreign ? "  – built for another processor" : ""), r[i].id);
        if (sel && strncmp(current, "steam:", 6) && !strcmp(sel->path, r[i].path))
            pick = v->values->len - 1;
    }
    if (*current && !pick) {
        add_row(v, titles, g_strdup_printf("Other: %s%s", current, sel ? "" : " (not found)"), current);
        pick = v->values->len - 1;
    }
    g_ptr_array_add(titles, NULL);
    gtk_string_list_splice(v->model, 0, n_old, (const char *const *)titles->pdata);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(v->dd), pick);
    g_ptr_array_free(titles, TRUE);
    for (size_t i = 0; i < noff; i++) g_free(dirs[i]);
    g_free(dirs);
    lsiw_steam_tools_free(off, noff);
    lsiw_runner_free(sel);
    lsiw_runners_free(r, n);
    p->syncing = FALSE;
}

static void store_version(struct engine_page *p, int kind, const char *value, const char *title)
{
    if (kind == LSIW_RUNNER_WINE) {
        lsiw_setting_set("engine", "wine", *value ? value : NULL);
        lsiw_setting_set("runtime", "wine", NULL);      /* the older key */
    } else lsiw_setting_set("engine", "proton", *value ? value : NULL);
    if (lsiw_gui_save_config(p->base.widget)) {
        char *m = g_strdup_printf("%s: %s. Saved in %s.", kind == LSIW_RUNNER_WINE ? "Wine" : "Proton",
                                  title ? title : *value ? value : "automatic", lsiw_config_path());
        gtk_label_set_text(GTK_LABEL(p->engine_status), m);
        g_free(m);
    }
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_CONFIG);
}

static void version_selected_cb(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    struct engine_page *p = data;
    if (p->syncing) return;
    struct versions *v = obj == G_OBJECT(p->wine.dd) ? &p->wine : &p->proton;
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(v->dd));
    if (i >= v->values->len) return;
    const char *value = g_ptr_array_index(v->values, i);
    const char *cur = v->kind == LSIW_RUNNER_WINE ? lsiw_gui_wine_setting() : proton_setting();
    if (!strcmp(value, cur)) return;
    store_version(p, v->kind, value, gtk_string_list_get_string(v->model, i));
}

static void update_proton_status(struct engine_page *p)
{
    struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_PROTON, *proton_setting() ? proton_setting() : NULL);
    char *appid = NULL, *display = NULL;
    char *m = r ? g_markup_printf_escaped("<b>Proton found:</b> LSIW %s (<tt>%s</tt>), run in LSIW's folders",
                                          r->name, r->path)
        : lsiw_proton_official(proton_setting(), &appid, &display)
            ? g_markup_printf_escaped("<b>LSIW %s</b> is not downloaded yet; Steam downloads it the first "
                                      "time a program runs with it.", display)
            : g_strdup("<b>No Proton found.</b> Install Proton in Steam, or use Download Proton.");
    free(appid);
    free(display);
    gtk_label_set_markup(GTK_LABEL(p->proton_status), m);
    g_free(m);
    lsiw_runner_free(r);
}

static void wine_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = data;
    struct engine_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f && p && gtk_widget_get_root(page)) {
        char *path = g_file_get_path(f);
        if (path && !g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            lsiw_gui_alert(page, "This is not a program", "Choose Wine's 'wine' program, for example "
                                                          "/opt/wine-staging/bin/wine.");
        else if (path) store_version(p, LSIW_RUNNER_WINE, path, NULL);
        g_free(path);
    }
    g_clear_object(&f);
    g_object_unref(page);
}

static void act_browse_wine(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct engine_page *p = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose the Wine program");
    gtk_file_dialog_set_modal(d, TRUE);
    char *cur = lsiw_gui_find_wine();
    char *dir = cur ? g_path_get_dirname(cur) : g_strdup("/usr/bin");
    GFile *start = g_file_new_for_path(dir);
    gtk_file_dialog_set_initial_folder(d, start);
    g_object_unref(start);
    g_free(dir);
    g_free(cur);
    gtk_file_dialog_open(d, lsiw_gui_window(p->base.widget), NULL, wine_chosen_cb,
                         g_object_ref(p->base.widget));
    g_object_unref(d);
}

static void proton_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = data;
    struct engine_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f && p && gtk_widget_get_root(page)) {
        char *path = g_file_get_path(f);
        char *script = path ? g_build_filename(path, "proton", NULL) : NULL;
        if (script && !g_file_test(script, G_FILE_TEST_IS_REGULAR))
            lsiw_gui_alert(page, "This is not a Proton folder",
                           "A Proton folder holds a file named 'proton', as in "
                           "~/.steam/steam/steamapps/common/Proton 9.0 or a GE-Proton folder.");
        else if (path) store_version(p, LSIW_RUNNER_PROTON, path, NULL);
        g_free(script);
        g_free(path);
    }
    g_clear_object(&f);
    g_object_unref(page);
}

static void act_browse_proton(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct engine_page *p = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose a Proton folder");
    gtk_file_dialog_set_modal(d, TRUE);
    gtk_file_dialog_select_folder(d, lsiw_gui_window(p->base.widget), NULL, proton_chosen_cb,
                                  g_object_ref(p->base.widget));
    g_object_unref(d);
}

static void run_cli(struct engine_page *p, const char *const *args)
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

static void act_get_wine(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "runners", "install", "wine", NULL };
    run_cli(data, args);
}

static void act_get_ge(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a;
    /* The build chosen in the Download Proton menu: ge, cachyos, ... */
    const char *args[] = { "runners", "install", g_variant_get_string(param, NULL), NULL };
    run_cli(data, args);
}

static void act_detect(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct engine_page *p = data;
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_SHOWN);
}

/* ------------------------------------------------------------------ */
/* Wine prefix                                                          */
/* ------------------------------------------------------------------ */

static void update_prefix(struct engine_page *p)
{
    const char *dir = lsiw_prefix_dir();
    char *reg = g_build_filename(dir, "system.reg", NULL);
    gboolean ready = g_file_test(reg, G_FILE_TEST_EXISTS);
    g_free(reg);
    char *m = g_markup_printf_escaped("<tt>%s</tt>  <span alpha=\"60%%\">%s; every program and game uses it "
                                      "unless it has its own (Programs page)</span>", dir,
                                      ready ? "set up"
                                            : "not set up yet; LSIW does it the first time a program needs it");
    gtk_label_set_markup(GTK_LABEL(p->prefix_label), m);
    g_free(m);
}

static void act_setup(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "wine-setup", NULL };
    run_cli(data, args);
}

static void act_running(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "prefix", "running", NULL };
    run_cli(data, args);
}

static void act_stop_idle(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    const char *args[] = { "prefix", "stop", NULL };
    run_cli(data, args);
}

static void act_stop(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct engine_page *p = data;
    lsiw_task_stop(p->runner);
}

static const GActionEntry engine_actions[] = {
    { .name = "browse-wine", .activate = act_browse_wine },
    { .name = "browse-proton", .activate = act_browse_proton },
    { .name = "get-wine", .activate = act_get_wine },
    { .name = "get-ge", .activate = act_get_ge, .parameter_type = "s" },
    { .name = "detect", .activate = act_detect },
    { .name = "setup", .activate = act_setup },
    { .name = "running", .activate = act_running },
    { .name = "stop-idle", .activate = act_stop_idle },
    { .name = "stop", .activate = act_stop },
};

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    struct engine_page *p = data;
    gboolean busy = lsiw_task_running(r);
    lsiw_gui_action_enable(p->actions, "setup", !busy);
    lsiw_gui_action_enable(p->actions, "running", !busy);
    lsiw_gui_action_enable(p->actions, "stop-idle", !busy);
    lsiw_gui_action_enable(p->actions, "get-wine", !busy);
    lsiw_gui_action_enable(p->actions, "get-ge", !busy);
    lsiw_gui_action_enable(p->actions, "stop", busy);
}

static void runner_done_cb(LsiwTask *r, gpointer data)
{
    struct engine_page *p = data;
    int code;
    if (lsiw_task_exit_code(r, &code)) {
        if (code == 127) gtk_label_set_text(GTK_LABEL(p->run_status),
                                            "Wine is not installed, so there is no prefix to set up.");
    }
    update_prefix(p);
    /* A download adds a version; Steam's tools were refreshed by lsiw. */
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_SHOWN);
}

/* ------------------------------------------------------------------ */
/* Page                                                                 */
/* ------------------------------------------------------------------ */

static void engine_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    struct engine_page *p = (struct engine_page *)base;
    p->syncing = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(p->engine_dd), engine_index(lsiw_gui_default_engine()));
    p->syncing = FALSE;
    gtk_widget_set_visible(p->chain_box, !strcmp(lsiw_gui_default_engine(), "custom"));
    fill_chain(p);

    const char *cfg = lsiw_gui_wine_setting();
    fill_versions(p, &p->wine, cfg);
    fill_proton_versions(p, &p->proton, proton_setting());
    update_proton_status(p);
    if (g_strcmp0(cfg, p->wine_checked) || reason == LSIW_REFRESH_SHOWN) detect_wine(p);
    update_prefix(p);
}

static void engine_free(gpointer data)
{
    struct engine_page *p = data;
    lsiw_task_free(p->runner);
    g_ptr_array_free(p->wine.values, TRUE);
    g_ptr_array_free(p->proton.values, TRUE);
    g_ptr_array_free(p->chain_values, TRUE);
    g_free(p->wine_checked);
    g_free(p);
}

static GtkWidget *form_label(const char *mnemonic, GtkWidget *target)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_valign(l, GTK_ALIGN_BASELINE_CENTER);
    if (target) gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
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

GtkWidget *lsiw_page_engine_new(void)
{
    struct engine_page *p = g_new0(struct engine_page, 1);
    p->base.refresh = engine_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, engine_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), engine_actions,
                                    G_N_ELEMENTS(engine_actions), p);
    gtk_widget_insert_action_group(page, "engine", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Engine"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "LSIW runs Windows programs with <b>its own native engine</b>: its own loader for "
        "Windows executables and its own implementation of the Windows system libraries, inside "
        "an ordinary Linux process. <b>Wine is only a fallback</b>, used when the native engine "
        "cannot run a program: 32-bit programs, programs that use Windows functions LSIW does "
        "not implement yet, .msi installers and batch files. <tt>lsiw check</tt> (on the Programs "
        "page) shows which engine a program gets."));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    p->engine_dd = gtk_drop_down_new_from_strings(engine_titles);
    lsiw_gui_dropdown_ellipsize(GTK_DROP_DOWN(p->engine_dd));
    gtk_grid_attach(GTK_GRID(grid), form_label("Default _engine", p->engine_dd), 0, 0, 1, 1);
    gtk_widget_set_halign(p->engine_dd, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), p->engine_dd, 1, 0, 3, 1);
    p->engine_hint = wrap_label();
    gtk_widget_add_css_class(p->engine_hint, "lsiw-dim");
    gtk_grid_attach(GTK_GRID(grid), p->engine_hint, 1, 1, 3, 1);

    /* Custom configuration: shown only when chosen. */
    p->chain_model = gtk_string_list_new(NULL);
    p->chain_values = g_ptr_array_new_with_free_func(g_free);
    GtkWidget *cgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cgrid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(cgrid), 12);
    static const char *const slot_labels[CHAIN_SLOTS] = {
        "_1. Default engine", "_2. First fallback", "_3. Second fallback", "_4. Third fallback"
    };
    for (int k = 0; k < CHAIN_SLOTS; k++) {
        p->chain_dd[k] = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(p->chain_model)), NULL);
        lsiw_gui_dropdown_ellipsize(GTK_DROP_DOWN(p->chain_dd[k]));
        gtk_widget_set_hexpand(p->chain_dd[k], TRUE);
        gtk_grid_attach(GTK_GRID(cgrid), form_label(slot_labels[k], p->chain_dd[k]), 0, k, 1, 1);
        gtk_grid_attach(GTK_GRID(cgrid), p->chain_dd[k], 1, k, 1, 1);
    }
    g_object_unref(p->chain_model);
    p->chain_box = cgrid;
    gtk_grid_attach(GTK_GRID(grid), cgrid, 1, 7, 3, 1);

    for (int k = 0; k < 2; k++) {
        struct versions *v = k ? &p->proton : &p->wine;
        v->kind = k ? LSIW_RUNNER_PROTON : LSIW_RUNNER_WINE;
        v->model = gtk_string_list_new(NULL);
        v->values = g_ptr_array_new_with_free_func(g_free);
        v->dd = gtk_drop_down_new(G_LIST_MODEL(v->model), NULL);
        lsiw_gui_dropdown_ellipsize(GTK_DROP_DOWN(v->dd));
        gtk_widget_set_hexpand(v->dd, TRUE);
        g_signal_connect(v->dd, "notify::selected", G_CALLBACK(version_selected_cb), p);
    }
    gtk_widget_set_tooltip_text(p->wine.dd, "Every Wine found: from $PATH, WineHQ (/opt/wine-*), Lutris, "
                                            "Bottles, and builds downloaded here. All of them use LSIW's "
                                            "one Wine prefix.");
    gtk_widget_set_tooltip_text(p->proton.dd, "LSIW's twin of every Proton: each of Valve's Proton versions "
                                              "in Steam, GE-Proton and other tools in Steam's "
                                              "compatibilitytools.d, and builds downloaded here. All of "
                                              "them run in LSIW's Proton prefix with LSIW's folders.");
    gtk_grid_attach(GTK_GRID(grid), form_label("_Wine version", p->wine.dd), 0, 2, 1, 1);
    GtkWidget *wbar = lsiw_gui_wrap_row(8);
    lsiw_gui_wrap_row_append(wbar, p->wine.dd);
    lsiw_gui_wrap_row_append(wbar, lsiw_gui_action_button("_Other…", "engine.browse-wine"));
    lsiw_gui_wrap_row_append(wbar, lsiw_gui_action_button("_Download Wine", "engine.get-wine"));
    gtk_grid_attach(GTK_GRID(grid), wbar, 1, 2, 3, 1);

    GtkWidget *wrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->wine_status = wrap_label();
    gtk_box_append(GTK_BOX(wrow), p->wine_status);
    GtkWidget *again = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(again, "Look for Wine and Proton again");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(again), "engine.detect");
    gtk_widget_set_valign(again, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(wrow), again);
    gtk_grid_attach(GTK_GRID(grid), wrow, 1, 3, 3, 1);

    gtk_grid_attach(GTK_GRID(grid), form_label("_Proton version", p->proton.dd), 0, 5, 1, 1);
    GtkWidget *pbar = lsiw_gui_wrap_row(8);
    lsiw_gui_wrap_row_append(pbar, p->proton.dd);
    lsiw_gui_wrap_row_append(pbar, lsiw_gui_action_button("O_ther…", "engine.browse-proton"));
    lsiw_gui_wrap_row_append(pbar, lsiw_gui_proton_download_button("engine.get-ge"));
    gtk_grid_attach(GTK_GRID(grid), pbar, 1, 5, 3, 1);
    p->proton_status = wrap_label();
    gtk_grid_attach(GTK_GRID(grid), p->proton_status, 1, 6, 3, 1);

    p->prefix_label = wrap_label();
    gtk_grid_attach(GTK_GRID(grid), form_label("Shared prefix", NULL), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->prefix_label, 1, 4, 3, 1);
    gtk_box_append(GTK_BOX(page), grid);

    p->engine_status = wrap_label();
    gtk_box_append(GTK_BOX(page), p->engine_status);

    GtkWidget *row = lsiw_gui_wrap_row(8);
    GtkWidget *setup = lsiw_gui_action_button("Set _up shared prefix", "engine.setup");
    gtk_widget_set_tooltip_text(setup, "Run 'lsiw wine-setup': create or refresh the shared prefix, make C: "
                                       "the C: drive with no other drive letters, and point Windows' "
                                       "folders at LSIW's locations");
    lsiw_gui_wrap_row_append(row, setup);
    GtkWidget *running = lsiw_gui_action_button("Show _running Wine", "engine.running");
    gtk_widget_set_tooltip_text(running, "Run 'lsiw prefix running': which Wine and Proton versions run now, "
                                         "for which prefix, and how much memory they take");
    lsiw_gui_wrap_row_append(row, running);
    GtkWidget *idle = lsiw_gui_action_button("Stop _idle Wine", "engine.stop-idle");
    gtk_widget_set_tooltip_text(idle, "Run 'lsiw prefix stop': end Wine and Proton left running with no "
                                      "program (LSIW also does this on its own after a program ends)");
    lsiw_gui_wrap_row_append(row, idle);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("Stop", "engine.stop"));
    p->run_status = wrap_label();
    lsiw_gui_wrap_row_append(row, p->run_status);
    gtk_box_append(GTK_BOX(page), row);

    gtk_box_append(GTK_BOX(page), lsiw_gui_output_view(&p->output));
    p->runner = lsiw_task_new(p->output, GTK_LABEL(p->run_status));
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);
    runner_state_cb(p->runner, p);

    g_signal_connect(p->engine_dd, "notify::selected", G_CALLBACK(engine_selected_cb), p);
    engine_refresh(&p->base, LSIW_REFRESH_CONFIG);
    for (int k = 0; k < CHAIN_SLOTS; k++)
        g_signal_connect(p->chain_dd[k], "notify::selected", G_CALLBACK(chain_selected_cb), p);
    gtk_label_set_text(GTK_LABEL(p->engine_hint),
                       engine_hints[gtk_drop_down_get_selected(GTK_DROP_DOWN(p->engine_dd))]);
    return page;
}
