/* LSIW Control (lsiw-control): declarations shared by the files of the
 * GTK 4 settings app.
 *
 * The app edits the same configuration as `lsiw config` through the shared
 * API in lsiw.h, and runs the `lsiw` command for anything that executes
 * Windows code (running and checking programs, the Wine prefix, desktop
 * registration). It never loads Windows code itself.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_GUI_COMMON_H
#define LSIW_GUI_COMMON_H

#include <gtk/gtk.h>
#include "lsiw.h"

#define LSIW_APP_ID "io.github.lsiw.Control"

/* ------------------------------------------------------------------ */
/* Pages                                                                */
/* ------------------------------------------------------------------ */

/* Why a page is asked to re-read its state. */
enum lsiw_refresh_reason {
    LSIW_REFRESH_CONFIG,    /* the configuration changed (here or on disk) */
    LSIW_REFRESH_SHOWN      /* the page was shown or the user asked for it  */
};

/* Every page keeps its state in a struct that starts with LsiwPage. The
 * struct is attached to the page widget (lsiw_page_attach) and freed with
 * it. */
typedef struct LsiwPage LsiwPage;
struct LsiwPage {
    GtkWidget *widget;
    void (*refresh)(LsiwPage *page, enum lsiw_refresh_reason reason);
};

GtkWidget *lsiw_page_locations_new(void);
GtkWidget *lsiw_page_drive_new(void);
GtkWidget *lsiw_page_engine_new(void);
GtkWidget *lsiw_page_programs_new(void);
GtkWidget *lsiw_page_integration_new(void);
GtkWidget *lsiw_page_steam_new(void);
GtkWidget *lsiw_page_update_new(void);
/* The library view of the Programs page (libview.c): the programs in a
 * Grid, List or Details layout with their play time. `changed` runs when
 * the selection changes, `activated` on a double-click (or Enter) and
 * `shortcut` from a row's Desktop shortcut button. The view is freed with
 * its widget. */
typedef struct LsiwLibView LsiwLibView;
typedef void (*LsiwLibViewChanged)(gpointer data);
typedef void (*LsiwLibViewActivated)(const char *id, gpointer data);
LsiwLibView *lsiw_libview_new(LsiwLibViewChanged changed, LsiwLibViewActivated activated,
                              LsiwLibViewActivated shortcut, gpointer data);
GtkWidget   *lsiw_libview_widget(LsiwLibView *v);
/* Read the library and play time again; select `select` (or keep the
 * selection when NULL). */
void         lsiw_libview_reload(LsiwLibView *v, const char *select);
/* The id of the selected program, or NULL. */
const char  *lsiw_libview_selected(LsiwLibView *v);
/* Show or hide the window's "a new version is available" banner after
 * the last update check (main.c); `any` is any widget of the window. */
void lsiw_gui_update_banner(GtkWidget *any);

void lsiw_page_attach(LsiwPage *page, GtkWidget *widget, GDestroyNotify free_page);
void lsiw_page_refresh(GtkWidget *page_widget, enum lsiw_refresh_reason reason);
/* Refresh every page of the window `any` belongs to (the window stores
 * its GtkStack as "lsiw-stack"); only `any`'s own page without one. */
void lsiw_gui_refresh_all(GtkWidget *any, enum lsiw_refresh_reason reason);

/* ------------------------------------------------------------------ */
/* Helpers (util.c)                                                     */
/* ------------------------------------------------------------------ */

/* The `lsiw` command: next to this program first (development builds put
 * both binaries in the same directory), then $PATH. NULL if not found. */
const char *lsiw_gui_cli(void);
/* The Wine binary the fallback engine would use: [engine] wine (or the
 * older [runtime] wine) when it names an executable, else wine, then
 * wine64, from $PATH. Caller frees; NULL if none. */
char *lsiw_gui_find_wine(void);
/* The configured Wine setting ("" when automatic). */
const char *lsiw_gui_wine_setting(void);
/* The default engine of `lsiw run`: "auto", "native", "wine" or "proton". */
const char *lsiw_gui_default_engine(void);

GtkWindow *lsiw_gui_window(GtkWidget *w);
void lsiw_gui_alert(GtkWidget *w, const char *message, const char *detail);
/* lsiw_config_save() with an alert on failure. On failure the in-memory
 * configuration is reloaded from disk, so what the app shows stays what
 * is actually saved. */
gboolean lsiw_gui_save_config(GtkWidget *w);
/* Open a Linux folder in the default file manager. */
void lsiw_gui_open_folder(GtkWidget *w, const char *path);
void lsiw_gui_action_enable(GSimpleActionGroup *group, const char *name, gboolean enabled);

GtkWidget *lsiw_gui_heading(const char *text);
GtkWidget *lsiw_gui_paragraph(const char *text);
GtkWidget *lsiw_gui_page_box(void);
GtkWidget *lsiw_gui_action_button(const char *mnemonic_label, const char *action);
/* A row of buttons and labels that continues on a new line when the
 * window is too narrow for all of it. */
GtkWidget *lsiw_gui_wrap_row(int spacing);
void lsiw_gui_wrap_row_append(GtkWidget *row, GtkWidget *child);
/* "Download Proton" with a menu of the builds LSIW can download; each item
 * activates `action` (taking a string) with the build's id. */
GtkWidget *lsiw_gui_proton_download_button(const char *action);
/* Shorten the chosen item of a dropdown of strings with "…" when space is
 * short, instead of widening the window to fit the longest one. */
void lsiw_gui_dropdown_ellipsize(GtkDropDown *dd);
/* A scrolled, read-only monospace text view for command output. */
GtkWidget *lsiw_gui_output_view(GtkTextView **view);

/* ------------------------------------------------------------------ */
/* Running `lsiw ...` with its output streamed into a text view         */
/* ------------------------------------------------------------------ */

typedef struct LsiwTask LsiwTask;
typedef void (*LsiwTaskFunc)(LsiwTask *runner, gpointer data);

LsiwTask *lsiw_task_new(GtkTextView *view, GtkLabel *status);
/* state_changed: called when a run starts or ends (update sensitivity).
 * finished: called when a run ends, after the status label is set. */
void lsiw_task_set_callbacks(LsiwTask *r, LsiwTaskFunc state_changed,
                               LsiwTaskFunc finished, gpointer data);
/* Start argv (argv[0] is a path or a name looked up in $PATH) in cwd
 * (NULL: inherit). Returns FALSE (and reports why in the view) if it
 * could not be started or another run is active. */
gboolean lsiw_task_start(LsiwTask *r, const char *const *argv, const char *cwd);
/* Like lsiw_task_start; with lsiw_task_set_multi, while other runs are
 * active too. label names the run in the status line and the output
 * (NULL: the command); tag comes back from lsiw_task_last_tag when it
 * ends. Returns the run's id, 0 when it could not be started. */
guint lsiw_task_start_full(LsiwTask *r, const char *const *argv, const char *cwd,
                           const char *label, int tag);
/* Allow several runs at once; their output is told apart by labels. */
void lsiw_task_set_multi(LsiwTask *r, gboolean multi);
gboolean lsiw_task_running(LsiwTask *r);
/* The active runs: how many, and the id, label and pid of the i-th. */
guint lsiw_task_count(LsiwTask *r);
guint lsiw_task_run_id(LsiwTask *r, guint i);
const char *lsiw_task_run_label(LsiwTask *r, guint i);
int lsiw_task_run_pid(LsiwTask *r, guint i);
/* Of the last finished run: its tag and label. */
int lsiw_task_last_tag(LsiwTask *r);
const char *lsiw_task_last_label(LsiwTask *r);
/* Stop one run (as lsiw_task_stop does all). */
void lsiw_task_stop_run(LsiwTask *r, guint id);
/* Of the last finished run: TRUE and the exit code if it exited normally. */
gboolean lsiw_task_exit_code(LsiwTask *r, int *code);
/* Every active run. First call: SIGTERM to the process group; later
 * calls: SIGKILL. */
void lsiw_task_stop(LsiwTask *r);
void lsiw_task_clear(LsiwTask *r);
void lsiw_task_append(LsiwTask *r, const char *text);
/* Called when the owning page goes away; a running process is left
 * running, its output is no longer read. */
void lsiw_task_free(LsiwTask *r);

/* ------------------------------------------------------------------ */
/* LsiwVarItem: one variable of the table, as a list model item         */
/* ------------------------------------------------------------------ */

#define LSIW_TYPE_VAR_ITEM (lsiw_var_item_get_type())
G_DECLARE_FINAL_TYPE(LsiwVarItem, lsiw_var_item, LSIW, VAR_ITEM, GObject)

LsiwVarItem *lsiw_var_item_new(const struct lsiw_var *var);
const struct lsiw_var *lsiw_var_item_get_var(LsiwVarItem *item);
/* The kind that matters for editing: an ALIAS answers for its target. */
int lsiw_var_item_effective_kind(LsiwVarItem *item);
const char *lsiw_var_item_get_windows_path(LsiwVarItem *item);  /* NULL if none */
const char *lsiw_var_item_get_linux_path(LsiwVarItem *item);    /* NULL if none */
gboolean lsiw_var_item_is_custom(LsiwVarItem *item);
/* Case-folded text the search entry matches against. */
const char *lsiw_var_item_get_search_key(LsiwVarItem *item);
/* Re-read paths and status from the configuration; TRUE if any changed. */
gboolean lsiw_var_item_refresh(LsiwVarItem *item);

const char *lsiw_var_kind_label(int kind);
/* The variable another one is defined inside ("${VAR:X}/..." defaults),
 * or NULL. Caller frees. */
char *lsiw_var_parent_name(const struct lsiw_var *v);

#endif /* LSIW_GUI_COMMON_H */
