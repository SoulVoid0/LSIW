/* LSIW Control: the C: drive page.
 *
 * C:\ is one folder the user chooses here
 * ([drive] c_drive, default ~/.cdrive), the same for the native engine,
 * Wine and Proton. lsiw_sync_root() fills it with links to every top-level
 * Linux folder, so C:\home\me\file.txt is /home/me/file.txt and other disks
 * are the folders Linux mounts them on (C:\run\media\me\USB). Besides C:,
 * any folder can be made a drive of its own (D:, E:, ...: [drives] in the
 * config, the "Other drives" list here). The Windows folders a program expects directly under C:\
 * (windows, Program Files, ProgramData, users) are anchored there wherever
 * they live on Linux. This page lists the drive, moves it (`lsiw cdrive
 * set`, which moves its contents along and leaves a link behind), and
 * converts paths both ways with the same functions the runtime uses.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "common.h"
#include <stdlib.h>
#include <string.h>

struct drive_page {
    LsiwPage base;
    GSimpleActionGroup *actions;
    GtkWidget *root_label, *list, *summary;
    GtkWidget *win_entry, *win_result, *lin_entry, *lin_result;
    GtkWidget *status, *with_folders;
    GtkWidget *drives_list, *letter_dd;
    GtkStringList *letters;
    int pending_letter;          /* the drive a folder dialog is choosing for */
    GtkTextView *output;
    LsiwTask *runner;
};

/* ------------------------------------------------------------------ */
/* The C:\ view                                                         */
/* ------------------------------------------------------------------ */

static const struct lsiw_var *anchored(const char *name)
{
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++)
        if (t[i].anchor && !g_ascii_strcasecmp(t[i].anchor, name)) return &t[i];
    return NULL;
}

static int compare_names(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static GtkWidget *entry_row(const char *root, const char *name)
{
    char *full = g_build_filename(root, name, NULL);
    char *target = g_file_read_link(full, NULL);
    const struct lsiw_var *a = anchored(name);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);

    char *win = g_strdup_printf("C:\\%s", name);
    GtkWidget *l = gtk_label_new(win);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_width_chars(GTK_LABEL(l), 12);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "lsiw-bold");
    gtk_box_append(GTK_BOX(row), l);
    g_free(win);

    gtk_box_append(GTK_BOX(row), gtk_label_new("→"));

    const char *shown = target ? target
        : g_file_test(full, G_FILE_TEST_IS_DIR) ? "(a real folder, not a link)" : "(a file, not a link)";
    GtkWidget *t = gtk_label_new(shown);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(t), TRUE);
    gtk_widget_set_hexpand(t, TRUE);
    gtk_widget_set_tooltip_text(t, shown);
    gtk_box_append(GTK_BOX(row), t);

    const char *note = NULL;
    char *owned = NULL;
    if (target && !g_file_test(full, G_FILE_TEST_EXISTS)) note = "target missing";
    else if (a) note = owned = g_strdup_printf("Windows folder, %s", a->name);
    if (note) {
        GtkWidget *nl = gtk_label_new(note);
        gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(nl), 8);
        gtk_widget_set_tooltip_text(nl, note);
        gtk_widget_add_css_class(nl, "lsiw-dim");
        gtk_box_append(GTK_BOX(row), nl);
    }
    g_free(owned);
    g_free(target);
    g_free(full);
    return row;
}

static void fill_list(struct drive_page *p)
{
    const char *root = lsiw_root_dir();
    gtk_label_set_text(GTK_LABEL(p->root_label), root);
    gtk_list_box_remove_all(GTK_LIST_BOX(p->list));

    GDir *d = g_dir_open(root, 0, NULL);
    if (!d) {
        gtk_label_set_text(GTK_LABEL(p->summary),
                           "The C: drive has not been built yet. LSIW builds it the first time a "
                           "program runs, or now with Rebuild C: drive.");
        return;
    }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while ((n = g_dir_read_name(d))) g_ptr_array_add(names, g_strdup(n));
    g_dir_close(d);
    g_ptr_array_sort(names, compare_names);
    guint links = 0, windows = 0, own = 0;
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        gtk_list_box_append(GTK_LIST_BOX(p->list), entry_row(root, name));
        char *full = g_build_filename(root, name, NULL);
        char *target = g_file_read_link(full, NULL);
        if (anchored(name)) windows++;
        else if (target) links++;
        else own++;
        g_free(target);
        g_free(full);
    }
    char *s = g_strdup_printf("%u entries: %u top-level Linux folders, %u Windows folders and %u "
                              "files and folders of the C: drive itself.", names->len, links, windows, own);
    gtk_label_set_text(GTK_LABEL(p->summary), s);
    g_free(s);
    g_ptr_array_unref(names);
}

/* ------------------------------------------------------------------ */
/* Converter                                                            */
/* ------------------------------------------------------------------ */

static void show_result(GtkWidget *label, const char *path, const char *note)
{
    char *m = note ? g_markup_printf_escaped("%s  <span alpha=\"60%%\">%s</span>", path, note)
                   : g_markup_printf_escaped("%s", path);
    gtk_label_set_markup(GTK_LABEL(label), m);
    g_free(m);
}

static void convert_windows(struct drive_page *p)
{
    const char *in = gtk_editable_get_text(GTK_EDITABLE(p->win_entry));
    if (!*in) {
        gtk_label_set_text(GTK_LABEL(p->win_result), "");
        return;
    }
    char *lin = lsiw_win_to_linux(in);
    gboolean exists = lin && g_file_test(lin, G_FILE_TEST_EXISTS);
    show_result(p->win_result, lin ? lin : "?", exists ? "exists" : "does not exist yet");
    free(lin);
}

static void convert_linux(struct drive_page *p)
{
    const char *in = gtk_editable_get_text(GTK_EDITABLE(p->lin_entry));
    if (!*in) {
        gtk_label_set_text(GTK_LABEL(p->lin_result), "");
        return;
    }
    /* The shell would expand ~; do the same here. */
    char *path = (in[0] == '~' && (in[1] == '/' || !in[1]))
        ? g_strconcat(g_get_home_dir(), in + 1, NULL) : g_strdup(in);
    char *win = lsiw_linux_to_win(path);
    show_result(p->lin_result, win ? win : "?",
                path[0] == '/' ? NULL : "relative to the folder LSIW Control was started in");
    free(win);
    g_free(path);
}

static void win_changed_cb(GtkEditable *e, gpointer data)
{
    (void)e;
    convert_windows(data);
}

static void lin_changed_cb(GtkEditable *e, gpointer data)
{
    (void)e;
    convert_linux(data);
}

/* ------------------------------------------------------------------ */
/* Other drives (D:, E:, ...)                                           */
/* ------------------------------------------------------------------ */

static void drives_changed(struct drive_page *p)
{
    if (!lsiw_gui_save_config(p->base.widget)) return;
    /* The shared prefix's drive letters now; prefixes of programs follow
     * when they next start. */
    lsiw_prefix_link_drives(lsiw_shared_dir(), 1);
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_CONFIG);
}

static void drive_folder_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = data;
    struct drive_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GError *err = NULL;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (f && p && gtk_widget_get_root(page) && p->pending_letter) {
        char *path = g_file_get_path(f);
        if (path && !strcmp(path, "/")) {
            lsiw_gui_alert(page, "The whole Linux filesystem cannot be a drive",
                           "It is already on C: (C:\\home, C:\\mnt, ...). Choose a folder.");
        } else if (path) {
            int l = p->pending_letter;
            if (lsiw_drive_set(l, path) == 0) {
                lsiw_mkdirs(path, 0755);
                drives_changed(p);
                char *m = g_strdup_printf("%c:\\ is now %s.", l, path);
                gtk_label_set_text(GTK_LABEL(p->status), m);
                g_free(m);
            }
        }
        g_free(path);
    }
    g_clear_error(&err);
    g_clear_object(&f);
    g_object_unref(page);
}

static void choose_drive_folder(struct drive_page *p, int letter)
{
    p->pending_letter = letter;
    GtkFileDialog *d = gtk_file_dialog_new();
    char *title = g_strdup_printf("Which folder should be %c:?", letter);
    gtk_file_dialog_set_title(d, title);
    g_free(title);
    gtk_file_dialog_set_accept_label(d, "_Use as drive");
    gtk_file_dialog_set_modal(d, TRUE);
    const char *cur = lsiw_drive_path(letter);
    GFile *start = g_file_new_for_path(cur && g_file_test(cur, G_FILE_TEST_IS_DIR) ? cur : g_get_home_dir());
    gtk_file_dialog_set_initial_folder(d, start);
    g_object_unref(start);
    gtk_file_dialog_select_folder(d, lsiw_gui_window(p->base.widget), NULL, drive_folder_chosen_cb,
                                  g_object_ref(p->base.widget));
    g_object_unref(d);
}

static void drive_change_cb(GtkButton *b, gpointer data)
{
    int l = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lsiw-letter"));
    choose_drive_folder(data, l);
}

static void drive_open_cb(GtkButton *b, gpointer data)
{
    struct drive_page *p = data;
    int l = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lsiw-letter"));
    const char *path = lsiw_drive_path(l);
    if (path) {
        lsiw_mkdirs(path, 0755);
        lsiw_gui_open_folder(p->base.widget, path);
    }
}

static void drive_remove_cb(GtkButton *b, gpointer data)
{
    struct drive_page *p = data;
    int l = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "lsiw-letter"));
    lsiw_drive_set(l, NULL);
    drives_changed(p);
    char *m = g_strdup_printf("%c: removed; its folder and files are kept.", l);
    gtk_label_set_text(GTK_LABEL(p->status), m);
    g_free(m);
}

static GtkWidget *drive_button(struct drive_page *p, const char *label, const char *tip, int letter, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(b, tip);
    g_object_set_data(G_OBJECT(b), "lsiw-letter", GINT_TO_POINTER(letter));
    g_signal_connect(b, "clicked", cb, p);
    return b;
}

static void fill_drives(struct drive_page *p)
{
    gtk_list_box_remove_all(GTK_LIST_BOX(p->drives_list));
    /* The free letters, for "Add drive". */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->letters));
    const char *chosen = NULL;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->letter_dd));
    char keep[3] = "";
    if (sel != GTK_INVALID_LIST_POSITION && sel < n) {
        chosen = gtk_string_list_get_string(p->letters, sel);
        if (chosen) g_strlcpy(keep, chosen, sizeof keep);
    }
    const char **free_letters = g_new0(const char *, 27);
    static char names[26][3];
    int nf = 0;
    guint want = 0;
    for (int l = 'D'; l <= 'Z' + 2; l++) {
        int letter = l <= 'Z' ? l : l - 'Z' - 1 + 'A';     /* D..Z, then A, B */
        if (lsiw_drive_path(letter)) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(row, 4);
            gtk_widget_set_margin_bottom(row, 4);
            gtk_widget_set_margin_start(row, 8);
            gtk_widget_set_margin_end(row, 8);
            char lab[4] = { (char)letter, ':', '\\', 0 };
            GtkWidget *ll = gtk_label_new(lab);
            gtk_label_set_width_chars(GTK_LABEL(ll), 4);
            gtk_label_set_xalign(GTK_LABEL(ll), 0);
            gtk_widget_add_css_class(ll, "lsiw-bold");
            gtk_box_append(GTK_BOX(row), ll);
            gtk_box_append(GTK_BOX(row), gtk_label_new("→"));
            const char *path = lsiw_drive_path(letter);
            GtkWidget *t = gtk_label_new(path);
            gtk_label_set_xalign(GTK_LABEL(t), 0);
            gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
            gtk_label_set_selectable(GTK_LABEL(t), TRUE);
            gtk_widget_set_hexpand(t, TRUE);
            gtk_widget_set_tooltip_text(t, path);
            gtk_widget_add_css_class(t, "lsiw-mono");
            if (!g_file_test(path, G_FILE_TEST_IS_DIR)) gtk_widget_add_css_class(t, "lsiw-off");
            gtk_box_append(GTK_BOX(row), t);
            gtk_box_append(GTK_BOX(row), drive_button(p, "Open", "Open the folder in the file manager",
                                                      letter, G_CALLBACK(drive_open_cb)));
            gtk_box_append(GTK_BOX(row), drive_button(p, "Change…", "Choose another folder for this drive",
                                                      letter, G_CALLBACK(drive_change_cb)));
            gtk_box_append(GTK_BOX(row), drive_button(p, "Remove", "This letter is no drive any more; "
                                                      "the folder and its files are kept",
                                                      letter, G_CALLBACK(drive_remove_cb)));
            gtk_list_box_append(GTK_LIST_BOX(p->drives_list), row);
        } else {
            names[nf][0] = (char)letter;
            names[nf][1] = ':';
            names[nf][2] = 0;
            if (keep[0] == letter) want = (guint)nf;
            free_letters[nf] = names[nf];
            nf++;
        }
    }
    gtk_string_list_splice(p->letters, 0, n, free_letters);
    g_free(free_letters);
    if (nf) gtk_drop_down_set_selected(GTK_DROP_DOWN(p->letter_dd), want);
    lsiw_gui_action_enable(p->actions, "add-drive", nf > 0);
}

static void act_add_drive(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct drive_page *p = data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(p->letter_dd));
    const char *l = sel != GTK_INVALID_LIST_POSITION ? gtk_string_list_get_string(p->letters, sel) : NULL;
    if (l && *l) choose_drive_folder(p, *l);
}

/* ------------------------------------------------------------------ */
/* Actions                                                              */
/* ------------------------------------------------------------------ */

static void act_rebuild(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct drive_page *p = data;
    /* Verbose: what changed is also logged on standard error. */
    if (lsiw_sync_root(1) < 0) {
        char *d = g_strdup_printf("%s could not be created. Check that %s is writable.",
                                  lsiw_root_dir(), lsiw_data_dir());
        lsiw_gui_alert(p->base.widget, "The C: drive could not be rebuilt", d);
        g_free(d);
        gtk_label_set_text(GTK_LABEL(p->status), "Rebuilding C:\\ failed.");
    } else {
        gtk_label_set_text(GTK_LABEL(p->status),
                           "C:\\ rebuilt: links for every top-level Linux folder and the Windows "
                           "folders, and every configured folder created.");
    }
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_CONFIG);
}

static void act_open(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct drive_page *p = data;
    if (!g_file_test(lsiw_root_dir(), G_FILE_TEST_IS_DIR)) {
        lsiw_sync_root(0);
        fill_list(p);
    }
    lsiw_gui_open_folder(p->base.widget, lsiw_root_dir());
}

static void move_to(struct drive_page *p, const char *path)
{
    const char *cli = lsiw_gui_cli();
    if (!cli) {
        lsiw_gui_alert(p->base.widget, "The lsiw command was not found",
                       "Moving the C: drive is done by 'lsiw cdrive set'. Install LSIW, or start "
                       "LSIW Control from the folder it was built in.");
        return;
    }
    gboolean folders = gtk_check_button_get_active(GTK_CHECK_BUTTON(p->with_folders));
    const char *argv[] = { cli, "cdrive", path ? "set" : "reset", path ? path : "--with-folders",
                           folders && path ? "--with-folders" : NULL, NULL };
    if (!path && !folders) argv[3] = NULL;
    lsiw_task_start(p->runner, argv, NULL);
}

static void folder_chosen_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *page = data;
    struct drive_page *p = g_object_get_data(G_OBJECT(page), "lsiw-page");
    GError *err = NULL;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (f && p && gtk_widget_get_root(page)) {
        char *path = g_file_get_path(f);
        if (path) {
            /* A folder that already has files gets a c_drive folder inside. */
            GDir *d = g_dir_open(path, 0, NULL);
            gboolean empty = !d || !g_dir_read_name(d);
            if (d) g_dir_close(d);
            char *target = empty || !g_strcmp0(path, lsiw_root_dir()) ? g_strdup(path)
                                                                     : g_build_filename(path, "c_drive", NULL);
            move_to(p, target);
            g_free(target);
        }
        g_free(path);
    }
    g_clear_error(&err);
    g_clear_object(&f);
    g_object_unref(page);
}

static void act_choose(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    struct drive_page *p = data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Where should the C: drive be?");
    gtk_file_dialog_set_accept_label(d, "_Move C: here");
    gtk_file_dialog_set_modal(d, TRUE);
    GFile *start = g_file_new_for_path(g_get_home_dir());
    gtk_file_dialog_set_initial_folder(d, start);
    g_object_unref(start);
    gtk_file_dialog_select_folder(d, lsiw_gui_window(p->base.widget), NULL, folder_chosen_cb,
                                  g_object_ref(p->base.widget));
    g_object_unref(d);
}

static void act_reset(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a; (void)param;
    move_to(data, NULL);
}

static const GActionEntry drive_actions[] = {
    { .name = "rebuild", .activate = act_rebuild },
    { .name = "open", .activate = act_open },
    { .name = "choose", .activate = act_choose },
    { .name = "reset", .activate = act_reset },
    { .name = "add-drive", .activate = act_add_drive },
};

static void runner_state_cb(LsiwTask *r, gpointer data)
{
    struct drive_page *p = data;
    gboolean busy = lsiw_task_running(r);
    lsiw_gui_action_enable(p->actions, "choose", !busy);
    lsiw_gui_action_enable(p->actions, "reset", !busy && lsiw_cdrive_setting());
    lsiw_gui_action_enable(p->actions, "rebuild", !busy);
}

static void runner_done_cb(LsiwTask *r, gpointer data)
{
    struct drive_page *p = data;
    (void)r;
    /* lsiw saved the new location: read it back, then every page follows. */
    lsiw_config_load();
    lsiw_gui_refresh_all(p->base.widget, LSIW_REFRESH_CONFIG);
}

static void drive_free(gpointer data)
{
    struct drive_page *p = data;
    lsiw_task_free(p->runner);
    g_free(p);
}

/* ------------------------------------------------------------------ */
/* Page                                                                 */
/* ------------------------------------------------------------------ */

static void drive_refresh(LsiwPage *base, enum lsiw_refresh_reason reason)
{
    (void)reason;
    struct drive_page *p = (struct drive_page *)base;
    fill_list(p);
    fill_drives(p);
    if (p->runner) runner_state_cb(p->runner, p);
    /* Folder locations decide where anchored Windows paths lead. */
    convert_windows(p);
    convert_linux(p);
}

static GtkWidget *grid_label(const char *mnemonic, GtkWidget *target)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (target) gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
}

static GtkWidget *result_label(void)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(l), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_CHAR);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_widget_add_css_class(l, "lsiw-mono");
    return l;
}

GtkWidget *lsiw_page_drive_new(void)
{
    struct drive_page *p = g_new0(struct drive_page, 1);
    p->base.refresh = drive_refresh;
    GtkWidget *page = lsiw_gui_page_box();
    lsiw_page_attach(&p->base, page, drive_free);

    p->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(p->actions), drive_actions,
                                    G_N_ELEMENTS(drive_actions), p);
    gtk_widget_insert_action_group(page, "drive", G_ACTION_GROUP(p->actions));
    g_object_unref(p->actions);

    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("The C: drive"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "<b>C:\\ is one folder you choose</b>, the same for LSIW's own engine, Wine, Proton "
        "and every Steam game (it is the shared prefix's <tt>drive_c</tt>). It also shows "
        "your whole Linux filesystem: <tt>C:\\home\\me\\notes.txt</tt> is "
        "<tt>/home/me/notes.txt</tt>, and other disks are the folders Linux mounts them on "
        "(<tt>C:\\run\\media\\me\\USB</tt>, <tt>C:\\mnt\\data</tt>). The folders Windows "
        "expects right under C:\\ (windows, Program Files, ProgramData, users) appear there "
        "wherever you keep them; choose their locations on the Locations page. Any other "
        "folder can also be a drive of its own (D:, E:, …): see <b>Other drives</b> below."));

    GtkWidget *row = lsiw_gui_wrap_row(8);
    GtkWidget *l = gtk_label_new("C: drive:");
    lsiw_gui_wrap_row_append(row, l);
    p->root_label = gtk_label_new(NULL);
    gtk_label_set_selectable(GTK_LABEL(p->root_label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(p->root_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(p->root_label), 0);
    gtk_widget_set_hexpand(p->root_label, TRUE);
    gtk_widget_add_css_class(p->root_label, "lsiw-mono");
    lsiw_gui_wrap_row_append(row, p->root_label);
    GtkWidget *change = lsiw_gui_action_button("_Change location…", "drive.choose");
    gtk_widget_set_tooltip_text(change, "Choose a new or empty folder (a folder that has files gets a "
                                        "c_drive folder inside). What C: holds moves along, and the old "
                                        "place becomes a link to the new one.");
    lsiw_gui_wrap_row_append(row, change);
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_Default (~/.cdrive)", "drive.reset"));
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_Open in file manager", "drive.open"));
    lsiw_gui_wrap_row_append(row, lsiw_gui_action_button("_Rebuild C: drive", "drive.rebuild"));
    gtk_box_append(GTK_BOX(page), row);
    p->with_folders = gtk_check_button_new_with_mnemonic(
        "When moving, also move the _Windows folder, Program Files and ProgramData into the new C:");
    gtk_box_append(GTK_BOX(page), p->with_folders);

    p->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_NONE);
    GtkWidget *empty = gtk_label_new("Nothing here yet: use Rebuild C: drive.");
    gtk_widget_add_css_class(empty, "lsiw-dim");
    gtk_widget_set_margin_top(empty, 12);
    gtk_widget_set_margin_bottom(empty, 12);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(p->list), empty);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), p->list);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 160);
    gtk_widget_set_vexpand(sw, TRUE);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), sw);
    gtk_box_append(GTK_BOX(page), frame);

    p->summary = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->summary), 0);
    gtk_label_set_wrap(GTK_LABEL(p->summary), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(p->summary), 20);
    gtk_widget_add_css_class(p->summary, "lsiw-dim");
    gtk_box_append(GTK_BOX(page), p->summary);

    /* Other drives */
    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Other drives"));
    gtk_box_append(GTK_BOX(page), lsiw_gui_paragraph(
        "Make any Linux folder a drive: with <tt>~/Games</tt> as D:, Windows programs see "
        "<tt>D:\\</tt> as that folder, under LSIW's engine, Wine and Proton (Steam games "
        "too). Good for game libraries on another disk, or programs that insist on a "
        "drive letter. A path naming a letter that is no drive is read as C:. "
        "Command line: <tt>lsiw drives</tt>."));
    p->drives_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->drives_list), GTK_SELECTION_NONE);
    GtkWidget *none = gtk_label_new("No other drives: only C:.");
    gtk_widget_add_css_class(none, "lsiw-dim");
    gtk_widget_set_margin_top(none, 8);
    gtk_widget_set_margin_bottom(none, 8);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(p->drives_list), none);
    GtkWidget *dframe = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(dframe), p->drives_list);
    gtk_box_append(GTK_BOX(page), dframe);
    GtkWidget *addrow = lsiw_gui_wrap_row(8);
    p->letters = gtk_string_list_new(NULL);
    p->letter_dd = gtk_drop_down_new(G_LIST_MODEL(p->letters), NULL);
    gtk_widget_set_tooltip_text(p->letter_dd, "The drive letter");
    lsiw_gui_wrap_row_append(addrow, gtk_label_new("New drive:"));
    lsiw_gui_wrap_row_append(addrow, p->letter_dd);
    GtkWidget *addb = lsiw_gui_action_button("_Add drive…", "drive.add-drive");
    gtk_widget_set_tooltip_text(addb, "Choose the folder this letter stands for");
    lsiw_gui_wrap_row_append(addrow, addb);
    gtk_box_append(GTK_BOX(page), addrow);

    /* Path converter */
    gtk_box_append(GTK_BOX(page), lsiw_gui_heading("Path converter"));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    p->win_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->win_entry), "C:\\users\\me\\Documents or %APPDATA%\\App");
    gtk_widget_set_hexpand(p->win_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), grid_label("_Windows path", p->win_entry), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->win_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("is on Linux", NULL), 0, 1, 1, 1);
    p->win_result = result_label();
    gtk_grid_attach(GTK_GRID(grid), p->win_result, 1, 1, 1, 1);

    p->lin_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->lin_entry), "/home/me/notes.txt");
    gtk_widget_set_hexpand(p->lin_entry, TRUE);
    gtk_widget_set_margin_top(p->lin_entry, 6);
    gtk_grid_attach(GTK_GRID(grid), grid_label("_Linux path", p->lin_entry), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p->lin_entry, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("is in Windows", NULL), 0, 3, 1, 1);
    p->lin_result = result_label();
    gtk_grid_attach(GTK_GRID(grid), p->lin_result, 1, 3, 1, 1);
    gtk_box_append(GTK_BOX(page), grid);

    p->status = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(p->status), 0);
    gtk_label_set_wrap(GTK_LABEL(p->status), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(p->status), 20);
    gtk_box_append(GTK_BOX(page), p->status);

    /* Output of `lsiw cdrive set` (moving can take a while). */
    GtkWidget *out = lsiw_gui_output_view(&p->output);
    gtk_widget_set_vexpand(out, FALSE);
    gtk_widget_set_size_request(out, -1, 90);
    gtk_box_append(GTK_BOX(page), out);
    p->runner = lsiw_task_new(p->output, GTK_LABEL(p->status));
    lsiw_task_set_callbacks(p->runner, runner_state_cb, runner_done_cb, p);
    runner_state_cb(p->runner, p);

    /* Examples, so the converter shows what it does from the start. */
    gtk_editable_set_text(GTK_EDITABLE(p->win_entry), "%APPDATA%\\Example");
    gtk_editable_set_text(GTK_EDITABLE(p->lin_entry), g_get_home_dir());
    g_signal_connect(p->win_entry, "changed", G_CALLBACK(win_changed_cb), p);
    g_signal_connect(p->lin_entry, "changed", G_CALLBACK(lin_changed_cb), p);
    drive_refresh(&p->base, LSIW_REFRESH_SHOWN);
    return page;
}
