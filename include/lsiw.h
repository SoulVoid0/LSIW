/* LSIW - Linux Subsystem for Improved Wine
 * Shared library interface: configuration, variable table, path mapping.
 * Used by the runtime (lsiw), the CLI and the GTK control app.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_H
#define LSIW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSIW_VERSION "1.7.1"

/* ------------------------------------------------------------------ */
/* Variable table                                                       */
/* ------------------------------------------------------------------ */

enum lsiw_var_kind {
    LSIW_VAR_FOLDER,    /* a real folder; the user picks its Linux location   */
    LSIW_VAR_VIRTUAL,   /* a shell namespace object (Recycle Bin, This PC...)  */
    LSIW_VAR_VALUE,     /* a plain value, not a folder (SYSTEMDRIVE, USERSID)  */
    LSIW_VAR_ALIAS,     /* same thing as another variable (APPDATA=CSIDL_APPDATA) */
    LSIW_VAR_DERIVED    /* follows its parent, cannot be relocated by itself   */
};

enum lsiw_var_scope { LSIW_SCOPE_USER, LSIW_SCOPE_SYSTEM };

struct lsiw_var {
    const char *name;        /* USMT name, e.g. "CSIDL_APPDATA"                 */
    int         kind;        /* enum lsiw_var_kind                              */
    int         scope;       /* enum lsiw_var_scope                             */
    const char *def;         /* default: Linux path template, see vars.c        */
    const char *alias_of;    /* for ALIAS                                       */
    const char *anchor;      /* fixed name directly under C:\ (e.g. "windows")  */
    int         csidl;       /* CSIDL number, -1 if none                        */
    const char *shell_value; /* Wine/Windows "Shell Folders" registry value     */
    int         usmt;        /* 1 = listed by Microsoft's USMT documentation    */
    const char *desc;        /* one-line description                            */
};

const struct lsiw_var *lsiw_vars(size_t *count);
const struct lsiw_var *lsiw_var_find(const char *name);   /* case-insensitive */
const struct lsiw_var *lsiw_var_by_csidl(int csidl);
/* Follow ALIAS links to the variable that actually stores the value. */
const struct lsiw_var *lsiw_var_resolve(const struct lsiw_var *v);

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

/* Load /etc/lsiw/lsiw.conf, then ~/.config/lsiw/lsiw.conf (or $LSIW_CONFIG).
 * Safe to call more than once; later calls reload. */
void lsiw_config_load(void);
/* Write the user's config file. Returns 0 on success. */
int  lsiw_config_save(void);
const char *lsiw_config_path(void);

/* Generic [section] key = value settings. */
const char *lsiw_setting_get(const char *section, const char *key, const char *def);
void        lsiw_setting_set(const char *section, const char *key, const char *value);
/* Every section whose name starts with `prefix` ("program:"), and every
 * key of a section, in file order. NULL-terminated lists (NULL if none),
 * freed with lsiw_strv_free. */
char      **lsiw_setting_sections(const char *prefix);
char      **lsiw_setting_keys(const char *section);
/* Remove a whole section. */
void        lsiw_setting_remove_section(const char *section);

/* Variable overrides. The value stored is a Linux path for folders (Windows
 * paths such as C:\foo are accepted and converted). NULL value = reset.
 * Returns 0 on success, -1 for an unknown name, -2 if the variable cannot be
 * relocated on its own (DERIVED). Setting an ALIAS sets its target. */
int         lsiw_var_set(const char *name, const char *value);
const char *lsiw_var_override(const char *name);    /* NULL if default */

/* Where the variable physically lives on Linux (absolute path). For VALUE
 * variables this is the value itself. Returns a pointer into an internal
 * cache that stays valid until the next config change. NULL if unset. */
const char *lsiw_var_linux(const char *name);
/* The path a Windows program sees (C:\...). Same lifetime rules. */
const char *lsiw_var_windows(const char *name);

/* ------------------------------------------------------------------ */
/* Directories                                                          */
/* ------------------------------------------------------------------ */

const char *lsiw_data_dir(void);    /* ~/.local/share/lsiw                  */
/* The C: drive: [drive] c_drive, default $HOME/.cdrive. Every engine sees
 * this folder as C:\ (the native engine's path converter, the drive_c and
 * dosdevices/c: of the shared Wine/Proton prefix). The string stays valid
 * until the setting changes. */
const char *lsiw_root_dir(void);
/* The prefix in use: <compat>/pfx, where <compat> is lsiw_compat_dir(). */
const char *lsiw_prefix_dir(void);
const char *lsiw_home_dir(void);
const char *lsiw_user_name(void);

/* ------------------------------------------------------------------ */
/* The C: drive                                                         */
/* ------------------------------------------------------------------ */

/* The configured C: location as written in the config (may hold ${HOME}
 * and the other tokens), or NULL for the default. */
const char *lsiw_cdrive_setting(void);
/* Move the C: drive to `path` (a Linux folder that does not exist or is
 * empty). What is in the old folder that is not one of LSIW's own links is
 * moved along (copied, then the old folder is kept as "<old>.old" when the
 * two are on different disks), and the old place becomes a link to the new
 * one. With folders, Program Files, Program Files (x86), ProgramData and
 * the Windows folder move into the new C: as well. Saves the setting and
 * rebuilds the drive. Returns 0, or -1 with a message printed. */
int  lsiw_cdrive_move(const char *path, int folders, int verbose);
/* With the default location: move the pre-1.7.1 default ~/c_drive to
 * ~/.cdrive and leave a link at ~/c_drive. Never deletes anything; does
 * nothing when [drive] c_drive is set, ~/c_drive is already a link, or
 * ~/.cdrive already has files. Called by lsiw_sync_root() and
 * lsiw_cdrive_move(). Returns 0, or -1 when the move failed. */
int  lsiw_cdrive_migrate(int verbose);

/* ------------------------------------------------------------------ */
/* Prefixes (prefix.c)                                                  */
/* ------------------------------------------------------------------ */

/* Every Windows program, under Wine or Proton, uses one shared prefix by
 * default: the folder lsiw_shared_dir() ([prefix] shared, default
 * <data>/shared), laid out like a Steam compatdata folder, with the Wine
 * prefix in pfx/ and pfx/drive_c a link to the C: drive. */
const char *lsiw_shared_dir(void);
/* The prefix folder this process uses: $LSIW_PREFIX when set (a program
 * with its own prefix), else lsiw_shared_dir(). */
const char *lsiw_compat_dir(void);
/* 1 when `compat` is the shared prefix folder. */
int  lsiw_is_shared_compat(const char *compat);
/* Move the prefixes of LSIW 1.5 and earlier (<data>/prefix, the Wine
 * prefix) into the shared folder, once; the old place becomes a link.
 * Returns 0, -1 on error (message printed). */
int  lsiw_prefix_migrate(int verbose);
/* Make <compat>/pfx/drive_c a link to the C: drive. A real drive_c already
 * there is kept as drive_c.lsiw-backup (nothing is deleted). Returns 0 or
 * -1. */
int  lsiw_prefix_link_drive(const char *compat, int verbose);
/* Copy every file and folder of `src` that `dst` does not have yet (an
 * existing name is never overwritten). Returns the number of entries
 * copied, -1 if `src` cannot be read. */
long lsiw_copy_missing(const char *src, const char *dst, int verbose);

/* ------------------------------------------------------------------ */
/* The program library (programs.c)                                     */
/* ------------------------------------------------------------------ */

/* A Windows program or game with its own settings, stored in the config as
 * [program:ID]. Strings are never NULL ("" when unset). */
struct lsiw_program {
    char *id;        /* "notepad-plus-plus"                                  */
    char *name;      /* shown to people                                      */
    char *exe;       /* Linux path of the program (or a .lnk/.msi/.bat)      */
    char *args;      /* arguments, shell quoting                             */
    char *workdir;   /* "" = the program's own folder                        */
    char *engine;    /* "" (the default engine), auto, native, wine,         */
                     /* wine:VERSION, proton, proton:VERSION                 */
    char *prefix;    /* "" or "shared": the shared prefix; "own": a prefix    */
                     /* of its own (<data>/prefixes/ID); or a folder          */
    char *env;       /* "NAME=value" lines                                   */
    char *icon;      /* an icon file, or ""                                  */
    int   dxvk;      /* -1 default, 0 off (WineD3D), 1 on                    */
    int   vkd3d;     /* -1 default, 0 off, 1 on                              */
    int   mangohud;  /* 1: run with the MangoHud overlay                     */
    int   debug;     /* 1: verbose LSIW, Wine and Proton logs                */
};
/* Every program of the library, sorted by name. Free with
 * lsiw_programs_free. */
size_t lsiw_programs(struct lsiw_program **out);
void   lsiw_programs_free(struct lsiw_program *v, size_t n);
/* One program by id; NULL if there is none. Free with lsiw_program_free. */
struct lsiw_program *lsiw_program_get(const char *id);
void   lsiw_program_free(struct lsiw_program *p);
/* A new entry for `exe` (name from the file name when NULL), with a fresh
 * id; not stored until lsiw_program_store. */
struct lsiw_program *lsiw_program_new(const char *exe, const char *name);
/* Write the entry into the in-memory config (lsiw_config_save saves it). */
void   lsiw_program_store(const struct lsiw_program *p);
void   lsiw_program_remove(const char *id);
/* The prefix folder (compatdata layout) the program runs in. Caller frees. */
char  *lsiw_program_compat_dir(const struct lsiw_program *p);
/* Its desktop shortcut file (~/.local/share/applications/lsiw-ID.desktop).
 * Caller frees. */
char  *lsiw_program_desktop_file(const struct lsiw_program *p);
/* The program's icon as a file a desktop can show: its icon setting (a
 * .png as is; an .ico, .exe or .dll converted), else the icon inside the
 * program (or a .lnk's icon or target), converted to
 * <data>/icons/ID.png. An icon setting in another image format (.svg,
 * .jpg) is returned as it is. With refresh, the PNG is made again even if
 * it looks current. NULL if there is none. Caller frees. */
char  *lsiw_program_icon(const struct lsiw_program *p, int refresh);
/* 1 if `path` is a PNG file (by its signature). */
int    lsiw_is_png_file(const char *path);

/* ------------------------------------------------------------------ */
/* Icons (icon.c)                                                       */
/* ------------------------------------------------------------------ */

/* The icon of a Windows program (.exe/.dll: the first icon group, as
 * Explorer shows it), of an .ico file, or a .png, as PNG bytes: the
 * largest image up to 256x256. Returns 0, or -1 with errno set. Free
 * *png with free(). */
int   lsiw_icon_png(const char *path, unsigned char **png, size_t *len);
int   lsiw_icon_png_from_data(const unsigned char *d, size_t n, unsigned char **png, size_t *len);
/* RGBA pixels (0xAARRGGBB, rows top to bottom) -> PNG bytes. */
int   lsiw_png_encode(const uint32_t *rgba, int w, int h, unsigned char **png, size_t *len);
char *lsiw_base64_encode(const unsigned char *d, size_t n);
unsigned char *lsiw_base64_decode(const char *s, size_t *len);

/* ------------------------------------------------------------------ */
/* LSIW shortcuts (.lwlnk, lwlnk.c)                                     */
/* ------------------------------------------------------------------ */

/* A .lwlnk file names a program of the library (program->id) and keeps a
 * copy of its settings (program, used when the library has no such
 * program any more) and its icon: `icon` (a file, "" if none) and the PNG
 * itself (icon_png, NULL if none). */
struct lsiw_lwlnk {
    struct lsiw_program *program;
    char *icon;
    unsigned char *icon_png;
    size_t icon_png_len;
};
/* "[LSIW Shortcut]": the first line of every .lwlnk file. */
extern const char lsiw_lwlnk_magic[];
#define LSIW_LWLNK_MIME "application/x-lsiw-shortcut"
int  lsiw_lwlnk_parse(const char *text, struct lsiw_lwlnk *l);
int  lsiw_lwlnk_read(const char *path, struct lsiw_lwlnk *l);   /* -1 (errno) if not one */
/* Write a shortcut to `p` (atomically). icon and png may be NULL. */
int  lsiw_lwlnk_write(const char *path, const struct lsiw_program *p, const char *icon,
                      const unsigned char *png, size_t png_len);
void lsiw_lwlnk_free(struct lsiw_lwlnk *l);

/* ------------------------------------------------------------------ */
/* Engine fallbacks                                                     */
/* ------------------------------------------------------------------ */

/* [engine] default = custom: [engine] chain lists the engines to try, in
 * order, separated by commas: native, auto, wine, wine:VERSION, proton,
 * proton:VERSION. The first is the default, the rest (at most
 * LSIW_CHAIN_MAX - 1) are fallbacks used when the one before cannot run
 * the program. Returns the number of entries; free with lsiw_strv_free. */
#define LSIW_CHAIN_MAX 4
size_t lsiw_engine_chain(char ***out);

/* Rebuild the C:\ view: one entry per top-level Linux directory plus the
 * anchored Windows folders (windows, Program Files, ProgramData, users...).
 * Also creates every configured folder that does not exist yet.
 * Returns 0 on success. */
int  lsiw_sync_root(int verbose);

/* Windows path -> real Linux path. Handles C:\, any other drive letter
 * (folded onto C:), forward or back slashes, %VARIABLES%, and
 * case-insensitive matching against existing files. Caller frees. */
char *lsiw_win_to_linux(const char *winpath);
/* Linux path -> Windows path on C:. Caller frees. */
char *lsiw_linux_to_win(const char *linuxpath);
/* Expand the ${HOME}, ${DATA}, ${XDG:NAME}, ${VAR:NAME}, ... tokens of a
 * config value (see lsiw.conf). Caller frees. */
char *lsiw_expand_template(const char *tpl);
/* Expand %VAR% references (Windows-style values). Caller frees. */
char *lsiw_expand(const char *s);

/* Windows shortcuts (.lnk, src/common/lnk.c). Strings are UTF-8; target
 * and workdir are Windows paths ("C:\Program Files\x\x.exe", possibly
 * with %VARIABLES%). Any field may be NULL. */
struct lsiw_lnk {
    char *target, *args, *workdir, *description, *relative_path, *icon;
    int icon_index;
    int show;        /* SW_SHOWNORMAL (1), SW_SHOWMAXIMIZED (3), SW_SHOWMINNOACTIVE (7) */
};
int  lsiw_lnk_parse(const unsigned char *data, size_t len, struct lsiw_lnk *l);
int  lsiw_lnk_read(const char *path, struct lsiw_lnk *l);   /* -1 (errno) if not a shortcut */
int  lsiw_lnk_write(const char *path, const struct lsiw_lnk *l);
void lsiw_lnk_free(struct lsiw_lnk *l);

/* ------------------------------------------------------------------ */
/* Engines                                                              */
/* ------------------------------------------------------------------ */

/* Native engine (src/native): load and run a Windows program in this
 * process. argv[0] is the program as the user named it; argv[1..] are its
 * arguments. Returns the program's exit code (most programs end the process
 * through ExitProcess and never return). Returns -1 with *why set if the
 * file cannot be loaded at all. */
int lsiw_native_run(const char *linux_path, int argc, char **argv, char **why);
/* Scan a program and every DLL it imports without running it. Returns 0 if
 * every import is implemented natively, 1 if something is missing (listed
 * in *report, which the caller frees), -1 if the file is not a usable PE. */
int lsiw_native_check(const char *linux_path, char **report);

/* ------------------------------------------------------------------ */
/* Wine and Proton installations (runners.c)                            */
/* ------------------------------------------------------------------ */

enum lsiw_runner_kind { LSIW_RUNNER_WINE, LSIW_RUNNER_PROTON };

struct lsiw_runner {
    int   kind;      /* enum lsiw_runner_kind                                  */
    char *id;        /* what [engine] wine / [engine] proton store             */
    char *name;      /* shown to people ("GE-Proton9-20", "wine-staging (WineHQ)") */
    char *path;      /* Wine: the wine program; Proton: its folder             */
    char *source;    /* "system", "WineHQ", "LSIW", "Lutris", "Bottles", "Steam", ... */
    char *version;   /* may be NULL                                            */
    int   ge;        /* a Proton-GE build                                      */
    int   foreign;   /* built for another processor (an aarch64 build on x86-64) */
};

/* Every installed runner of a kind. Free with lsiw_runners_free. */
size_t lsiw_runners(int kind, struct lsiw_runner **out);
void   lsiw_runners_free(struct lsiw_runner *v, size_t n);
/* The runner a setting names (an id, a name or a path); NULL, "" or "auto"
 * picks the default (Wine from $PATH; the newest official Proton). NULL if
 * none. Free with lsiw_runner_free. */
struct lsiw_runner *lsiw_runner_pick(int kind, const char *setting);
void   lsiw_runner_free(struct lsiw_runner *r);
/* A Proton setting "steam:KEY" names one of Valve's Protons by its Steam
 * name or alias ("steam:proton_11", "steam:proton-stable"): the LSIW twin
 * of that version, run in LSIW's folders wherever Steam installs it.
 * Returns 1 and fills *appid and *display (caller frees) when `setting`
 * is such a name Steam knows; 0 otherwise. */
int    lsiw_proton_official(const char *setting, char **appid, char **display);
/* <data>/runners: Wine and Proton builds LSIW downloaded (wine/, proton/). */
const char *lsiw_runners_dir(void);

/* Steam: installations (folders with steamapps/), every library folder
 * (from libraryfolders.vdf), and the install folder of an app or tool by
 * its app ID. Lists are NULL-terminated (NULL if empty), freed with
 * lsiw_strv_free; the folder is freed by the caller. */
char **lsiw_steam_roots(void);
char **lsiw_steam_libraries(void);
char  *lsiw_steam_app_dir(const char *appid);
/* 1 for the Flatpak and Snap builds of Steam, whose sandbox cannot start
 * programs installed on the host. */
int    lsiw_steam_root_sandboxed(const char *root);
char  *lsiw_steam_tool_display_name(const struct lsiw_runner *r);
void   lsiw_strv_free(char **v);
/* The compatibility tools Valve publishes through Steam (Proton
 * Experimental, Proton 11.0-2, ...), from Steam's app cache
 * (appcache/appinfo.vdf, app 891390 and the other Steam Play manifests),
 * or a built-in list when Steam has none. Only tools that run Windows
 * programs on Linux and are built for this processor are listed; none of
 * them needs to be installed. */
struct lsiw_steam_tool {
    char *key;       /* Steam's internal name: "proton_11"                    */
    char *appid;     /* "4628710"                                             */
    char *display;   /* "Proton 11.0-2"                                       */
    char *aliases;   /* "proton-stable,proton-11.0-2pin,..."                  */
    int   legacy;    /* shown only with "Show all compatibility tools"         */
};
size_t lsiw_steam_official_tools(struct lsiw_steam_tool **out);
void   lsiw_steam_tools_free(struct lsiw_steam_tool *v, size_t n);
/* The tool that has `alias` ("proton-stable"), or NULL; points into v. */
const struct lsiw_steam_tool *lsiw_steam_tool_by_alias(const struct lsiw_steam_tool *v, size_t n,
                                                        const char *alias);
/* 1 if a Proton/Wine build at `path` (a folder or the wine program) is for
 * another processor than this one. */
int    lsiw_runner_foreign(const char *path);
/* The folder name LSIW gives a downloaded Wine build: "wine-11.20-amd64"
 * becomes "wine-11.20-x86_64" (the same processor; Kron4ek's builds use
 * Debian's name for it). Newly allocated. */
char  *lsiw_runner_wine_dir_name(const char *name);

/* Proton builds LSIW can download besides Valve's own ("lsiw runners
 * install ID"): GE-Proton and the other community builds. */
struct lsiw_proton_build {
    const char *id;          /* "ge", "cachyos", ...                         */
    const char *title;       /* "Proton-CachyOS"                             */
    const char *about;       /* one line for LSIW Control                    */
    const char *repo;        /* GitHub owner/name                            */
    const char *tag_prefix;  /* release tags: "cachyos-" ('|' separates)     */
    const char *tag_suffix;  /* and their end ("-slr"), or NULL              */
    const char *skip;        /* tags with one of these words ('|') are not   */
                             /* releases: "alpha|beta|rc"                  */
    const char *asset;       /* archive name, $tag for the tag               */
    const char *sum;         /* SHA-512 file name, or NULL                   */
    const char *nightly;     /* a nightly.link zip instead of releases       */
};
const struct lsiw_proton_build *lsiw_proton_builds(size_t *count);
const struct lsiw_proton_build *lsiw_proton_build_find(const char *id);

/* Steam KeyValues (.vdf, .acf) text: every value of `key` at any depth, or
 * the first one. */
char **lsiw_vdf_values(const char *text, const char *key);
char  *lsiw_vdf_value(const char *text, const char *key);
/* A whole small text file, NUL-terminated; NULL on error. */
char  *lsiw_read_text(const char *path);

/* ------------------------------------------------------------------ */
/* Misc helpers                                                         */
/* ------------------------------------------------------------------ */

int   lsiw_mkdirs(const char *path, int mode);
char *lsiw_strdup_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int lsiw_verbose;
void  lsiw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LSIW_TRACE(...) do { if (lsiw_verbose) lsiw_log(__VA_ARGS__); } while (0)

#ifdef __cplusplus
}
#endif
#endif /* LSIW_H */
