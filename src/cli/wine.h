/* LSIW command-line program: interfaces shared by main.c, wine.c and
 * register.c.
 *
 * The Wine fallback engine lives here. LSIW runs Windows programs with its
 * own native engine; Wine is used only when the native engine cannot run a
 * program (32-bit programs, programs that need functions LSIW does not
 * implement yet, .msi installers, batch files).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_CLI_WINE_H
#define LSIW_CLI_WINE_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Wine fallback engine (wine.c)                                        */
/* ------------------------------------------------------------------ */

/* The Wine binary to use: config [engine] wine (the older [runtime] wine
 * key is honoured too), else 'wine' then 'wine64' from $PATH. NULL when Wine
 * is not installed. The string stays valid until the configured value
 * changes. */
const char *lsiw_wine_binary(void);

/* Wine's version string, e.g. "wine-9.0 (Staging)". Caller frees; NULL if
 * Wine is missing or does not answer. */
char *lsiw_wine_version(void);

/* State of LSIW's Wine prefix (lsiw_prefix_dir()). */
enum lsiw_prefix_state {
    LSIW_PREFIX_MISSING,    /* never initialised                              */
    LSIW_PREFIX_STALE,      /* folder locations or drives changed since setup */
    LSIW_PREFIX_READY
};
int lsiw_wine_prefix_state(void);

/* Create the prefix, or refresh it when its stamp is stale: wineboot, the
 * single C: drive, the shell-folder registry values. Returns 0 if it was
 * already up to date, 1 if it was created or refreshed, -1 on error (a
 * message has been printed). With verbose, every step is printed. */
int lsiw_wine_setup(int verbose);

/* Drop the stamp so the next lsiw_wine_setup() redoes everything.
 * Returns 0 on success (also when there was no stamp). */
int lsiw_wine_invalidate(void);

/* Run a Windows program with Wine: prepares the prefix if needed, then execs
 * 'wine C:\...\PROGRAM.exe ARGS...' so the program's exit code becomes the
 * exit code of this process. argv[0] is the program as the user named it,
 * argv[1..argc-1] its arguments. Only returns on failure: -1 with a message
 * printed (errno is ENOENT when Wine is not installed). */
int lsiw_wine_run(const char *linux_path, int argc, char **argv, int verbose);

/* ------------------------------------------------------------------ */
/* Proton and Steam (proton.c)                                          */
/* ------------------------------------------------------------------ */

/* When `wine` is a Wine build LSIW downloaded that needs the computer's
 * 32-bit libraries, replace it in place with the WoW64 build of the same
 * version. Returns 1 if replaced, 0 if nothing to do, -1 on failure. */
int lsiw_wine_build_upgrade(const char *wine);
/* After `wine` (a Wine build LSIW downloaded) crashed while starting:
 * download the newest older release that is known to start, and make it
 * the configured Wine. Returns 1 if switched, 0 if there is none. */
int lsiw_wine_build_fallback(const char *wine);

/* [engine] proton, or NULL for the default (the newest official Proton). */
const char *lsiw_proton_setting(void);
/* 1 if the configured (or default) Proton is installed. */
int lsiw_proton_available(void);
/* Link the user folders of the Proton prefix <compat>/pfx to the folders
 * chosen in LSIW Control (and show the Linux folders on C:). *mounts, if
 * not NULL, receives the linked folders as a ':' list (caller frees). */
int lsiw_proton_prepare(const char *compat, int verbose, char **mounts);
int lsiw_proton_repair_prefix(const char *compat, int verbose);
/* Remove those links again. */
int lsiw_proton_unlink(const char *compat, int verbose);
/* Run a program with Proton in LSIW's Proton prefix; like lsiw_wine_run. */
int lsiw_proton_run(const char *linux_path, int argc, char **argv, int verbose);
/* The wineserver running for the Wine prefix PFX (another program that
 * uses it right now): the path of its executable, NULL when none runs.
 * Caller frees. */
char *lsiw_prefix_server(const char *pfx);
/* The installed runner a running wineserver belongs to (a Proton folder or
 * a Wine version), NULL when LSIW does not know it. Free with
 * lsiw_runner_free. */
struct lsiw_runner *lsiw_server_runner(const char *server_exe);
/* Run FILE ARGV as one instance: its processes, Windows programs included
 * (Wine starts each in a session of its own), carry LSIW_INSTANCE in their
 * environment. This process stays as the watcher: SIGTERM, SIGINT and
 * SIGHUP stop every process of the instance (but not the wineserver or
 * Wine's services, which other programs of the prefix share), and when
 * its programs have ended while Proton still waits for others in the
 * prefix, the instance ends too. Exits with the program's exit status;
 * returns only when it could not start it. */
int lsiw_exec_instance(const char *file, char **argv);
/* Add (add=1) or remove the "LSIW Proton" tools in every Steam found. */
int lsiw_steam_integrate(int add, int verbose);
int lsiw_cmd_steam(int argc, char **argv);
int lsiw_cmd_steam_run(int argc, char **argv);
int lsiw_cmd_runners(int argc, char **argv);
extern const char lsiw_help_steam[], lsiw_help_runners[];

/* ------------------------------------------------------------------ */
/* C: drive, prefixes, program library (library.c)                      */
/* ------------------------------------------------------------------ */

int lsiw_cmd_cdrive(int argc, char **argv);
int lsiw_cmd_drives(int argc, char **argv);
int lsiw_cmd_update(int argc, char **argv);
/* After an interactive command: a one-line note when a newer LSIW is
 * known, and a background check once a day (update.c). */
void lsiw_update_notice(void);
int lsiw_cmd_prefix(int argc, char **argv);
int lsiw_cmd_programs(int argc, char **argv);
extern const char lsiw_help_cdrive[], lsiw_help_prefix[], lsiw_help_programs[];
extern const char lsiw_help_drives[], lsiw_help_update[];
/* Run a program the way 'lsiw run --engine=ENGINE' does (ENGINE NULL, ""
 * or "default": the default engine). argv[0] is the program. Returns the
 * exit status for this process. */
int lsiw_cli_run(int argc, char **argv, const char *engine);
/* Start a program of the library (or one read from a .lwlnk) with its
 * settings; argv are added after its own arguments. */
int lsiw_cli_program_run(struct lsiw_program *p, int argc, char **argv);
/* The XDG desktop folder (XDG_DESKTOP_DIR, else ~/Desktop). Caller frees. */
char *lsiw_cli_desktop_dir(void);

/* ------------------------------------------------------------------ */
/* LSIW shortcuts (lwlnk.c)                                             */
/* ------------------------------------------------------------------ */

extern const char lsiw_help_lwlnk[];
int lsiw_cmd_lwlnk(int argc, char **argv);
int lsiw_cmd_launch(int argc, char **argv);
/* Start what a .lwlnk names; argv are added to its arguments. */
int lsiw_launch_lwlnk(const char *file, int argc, char **argv);

/* ------------------------------------------------------------------ */
/* Windows shortcuts (lnk.c)                                            */
/* ------------------------------------------------------------------ */

extern const char lsiw_help_lnk[];
int lsiw_cmd_lnk(int argc, char **argv);
/* What `lsiw run LINK.lnk ARGS...` starts: *out_argv (the Linux path of
 * the target, then ARGS) and *out_cmdline (the Windows command line: the
 * target, the shortcut's arguments, ARGS). Changes to the shortcut's
 * working directory. Returns the argument count, or -1 with a message. A
 * shortcut without a target asks for one first. */
int lsiw_lnk_command(const char *link, int argc, char **argv, char ***out_argv, char **out_cmdline);

/* ------------------------------------------------------------------ */
/* Helpers shared by the CLI (wine.c)                                   */
/* ------------------------------------------------------------------ */

/* Allocation helpers that exit with a message when memory runs out. */
char *lsiw_cli_strdup(const char *s);
char *lsiw_cli_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Search $PATH for an executable (a name containing '/' is checked as is).
 * Caller frees; NULL if not found. */
char *lsiw_cli_which(const char *name);

/* Run argv[0] (searched in $PATH) and wait for it. `env` is an optional
 * NULL-terminated list of "NAME=VALUE" strings set in the child only. With
 * quiet, the child's stdout goes to /dev/null. Returns the exit status,
 * 128+signal if it was killed, or -1 if it could not be started. */
int lsiw_cli_spawn(char *const argv[], char *const env[], int quiet);

/* Like lsiw_cli_spawn but returns the child's stdout (stderr is discarded).
 * Caller frees; NULL on failure. *status receives the exit status if not
 * NULL. */
char *lsiw_cli_capture(char *const argv[], char *const env[], int *status);

/* Write a whole file atomically (temporary file + rename). Returns 0 on
 * success, -1 with errno set. */
int lsiw_cli_write_file(const char *path, const void *data, size_t len, int mode);

/* Read a whole (small) file, NUL-terminated. Caller frees; NULL with errno
 * set on failure. *len receives the length if not NULL. */
char *lsiw_cli_read_file(const char *path, size_t *len);

/* readlink() into a malloc'd string. NULL with errno set on failure. */
char *lsiw_cli_readlink(const char *path);

/* The path that starts this lsiw again later, for desktop entries, Steam
 * tools and shortcuts: /proc/self/exe, except inside an AppImage, whose
 * mount point disappears when it exits: there it is the .AppImage file
 * itself ($APPIMAGE), whose AppRun hands arguments to lsiw. NULL with errno
 * set on failure; caller frees. */
char *lsiw_cli_self_path(void);

/* 1 if `path` ends with `ext` (".exe"), compared case-insensitively. */
int lsiw_cli_has_ext(const char *path, const char *ext);

/* ------------------------------------------------------------------ */
/* Desktop and kernel integration (register.c)                          */
/* ------------------------------------------------------------------ */

/* MIME types LSIW registers itself for, NULL-terminated. */
extern const char *const lsiw_mime_types[];

/* --user: lsiw.desktop + default application for Windows programs.
 * --system: binfmt_misc handler so .exe files run directly. Each returns 0
 * on success, -1 on failure (messages printed). */
int lsiw_register_user(void);
int lsiw_unregister_user(void);
int lsiw_register_system(void);
int lsiw_unregister_system(void);

/* Print the binfmt_misc and desktop integration lines of 'lsiw info'. Each
 * line is "<label padded to `width`> <text>". */
void lsiw_print_integration(int width);

#endif /* LSIW_CLI_WINE_H */
