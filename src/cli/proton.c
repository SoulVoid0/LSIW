/* Proton and Steam integration, and the Wine/Proton versions LSIW manages.
 *
 * Proton is Valve's Wine with DXVK, VKD3D-Proton and Steam's glue; GE-Proton
 * is a community build of it. LSIW uses them in two ways:
 *
 *   lsiw run --engine=proton[:VERSION] PROGRAM
 *       runs PROGRAM with that Proton in LSIW's shared prefix
 *       (lsiw_shared_dir(), the same prefix Wine uses), or in the prefix of
 *       its own a program of the library has ($LSIW_PREFIX).
 *
 *   "LSIW Proton <version>" in Steam
 *       `lsiw steam add` puts one compatibility tool per installed Proton
 *       (official and GE) into Steam's compatibilitytools.d. Steam starts
 *       the tool's lsiw-proton script, which runs `lsiw steam-run`: the
 *       game runs in LSIW's shared prefix instead of its own
 *       steamapps/compatdata/<appid> ([steam] shared_prefix, on by default;
 *       `lsiw steam own APPID` gives one game its own prefix again), and the
 *       real Proton is started with the command line Steam gave, inside the
 *       Steam Linux Runtime container that Proton asks for. The first time a
 *       game that already had a prefix of its own runs this way, the files
 *       of that prefix's user folders (saves, settings) are copied into
 *       LSIW's folders where missing; the old prefix itself is left as it
 *       was, as a backup. `lsiw steam default` makes LSIW Proton the tool
 *       of every game Steam has and adds later.
 *
 * In the shared prefix, drive_c is the C: drive and C:\users\steamuser is
 * the user's profile, so nothing needs linking. A prefix of its own is
 * "prepared" instead, which is what makes LSIW Proton different from plain
 * Proton there: the user folders of the prefix (Documents, Saved Games, AppData, Desktop,
 * Downloads, Music, Pictures, Videos ...) become links to the folders
 * chosen in LSIW Control, so every game and program, under any engine,
 * shares them. C:\ also shows the whole Linux filesystem, as it does for
 * LSIW's engine ([steam] full_c_drive). Proton itself needs its Z: drive
 * and recreates it on every start, so it stays.
 *
 * Linked folders are recorded in <compatdata>/lsiw-links, and the links are
 * removed before Proton deletes a prefix (verb destroyprefix), so Proton
 * never deletes anything inside the user's folders.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include "wine.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define TOOL_CONF   "lsiw-tool.conf"
#define TOOL_SCRIPT "lsiw-proton"
#define LINKS_FILE  "lsiw-links"
#define GE_REPO     "GloriousEggroll/proton-ge-custom"
#define WINE_REPO   "Kron4ek/Wine-Builds"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static int same_dir(const char *a, const char *b);

static int truthy(const char *v)
{
    return v && (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "on"));
}

static char *self_exe(void)
{
    char *p = lsiw_cli_self_path();
    return p ? p : lsiw_cli_strdup("lsiw");
}

/* Remove a directory tree that LSIW made. Links are removed, never followed. */
static int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *c = lsiw_cli_printf("%s/%s", path, de->d_name);
        if (remove_tree(c) < 0) rc = -1;
        free(c);
    }
    closedir(d);
    if (rmdir(path) < 0) rc = -1;
    return rc;
}

static int dir_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *de;
    int empty = 1;
    while ((de = readdir(d)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { empty = 0; break; }
    closedir(d);
    return empty;
}

/* key=value lines */
static char *conf_get(const char *text, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=')
            return strndup(p + kl + 1, len - kl - 1);
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Preparing a Proton prefix                                            */
/* ------------------------------------------------------------------ */

/* Proton's user is "steamuser"; these folders of its profile become the
 * folders LSIW Control chose. */
static const struct { const char *rel, *var; } user_links[] = {
    { "users/steamuser/Desktop",          "CSIDL_DESKTOPDIRECTORY" },
    { "users/steamuser/Documents",        "CSIDL_PERSONAL" },
    { "users/steamuser/Downloads",        "CSIDL_DOWNLOADS" },
    { "users/steamuser/Music",            "CSIDL_MYMUSIC" },
    { "users/steamuser/Pictures",         "CSIDL_MYPICTURES" },
    { "users/steamuser/Videos",           "CSIDL_MYVIDEO" },
    { "users/steamuser/Favorites",        "CSIDL_FAVORITES" },
    { "users/steamuser/Contacts",         "CSIDL_CONTACTS" },
    { "users/steamuser/Saved Games",      "CSIDL_SAVED_GAMES" },
    { "users/steamuser/AppData/Roaming",  "CSIDL_APPDATA" },
    { "users/steamuser/AppData/Local",    "CSIDL_LOCAL_APPDATA" },
    { "users/steamuser/AppData/LocalLow", "CSIDL_APPDATA_LOCALLOW" },
    { "users/Public/Desktop",             "CSIDL_COMMON_DESKTOPDIRECTORY" },
    { "users/Public/Documents",           "CSIDL_COMMON_DOCUMENTS" },
    { "users/Public/Music",               "CSIDL_COMMON_MUSIC" },
    { "users/Public/Pictures",            "CSIDL_COMMON_PICTURES" },
    { "users/Public/Videos",              "CSIDL_COMMON_VIDEO" },
};

/* Top-level Linux folders not shown on C: in a Proton prefix: kernel
 * interfaces a game has no business walking into. */
static int skip_top(const char *name)
{
    static const char *const skip[] = { "proc", "sys", "dev", "lost+found", NULL };
    for (int i = 0; skip[i]; i++) if (!strcmp(name, skip[i])) return 1;
    return name[0] == '.';
}

static int exists_nocase(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d)))
        if (!strcasecmp(de->d_name, name)) { found = 1; break; }
    closedir(d);
    return found;
}

struct links { char **v; size_t n; };

static void links_add(struct links *l, const char *rel)
{
    for (size_t i = 0; i < l->n; i++) if (!strcmp(l->v[i], rel)) return;
    l->v = realloc(l->v, (l->n + 1) * sizeof *l->v);
    l->v[l->n++] = lsiw_cli_strdup(rel);
}

static void links_free(struct links *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
}

/* The folder at `link` becomes a link to `target`. What a game already
 * saved there is moved into the target; a name that exists on both sides
 * stays where it was, in "<folder> (before LSIW)". */
static int link_folder(const char *link, const char *target, int verbose)
{
    struct stat st;
    if (lsiw_mkdirs(target, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", target, strerror(errno));
        return -1;
    }
    if (same_dir(link, target)) return 0;      /* already that folder, by any path */
    if (lstat(link, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            char *cur = lsiw_cli_readlink(link);
            int same = cur && !strcmp(cur, target);
            free(cur);
            if (same) return 0;
            unlink(link);
        } else if (S_ISDIR(st.st_mode)) {
            DIR *d = opendir(link);
            struct dirent *de;
            while (d && (de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char *from = lsiw_cli_printf("%s/%s", link, de->d_name);
                char *to = lsiw_cli_printf("%s/%s", target, de->d_name);
                if (lstat(to, &st) < 0 && rename(from, to) == 0 && verbose)
                    lsiw_log("moved %s to %s\n", from, to);
                free(from);
                free(to);
            }
            if (d) closedir(d);
            if (dir_empty(link)) rmdir(link);
            else {
                char *keep = lsiw_cli_printf("%s (before LSIW)", link);
                if (rename(link, keep) < 0) {
                    lsiw_log("cannot move %s aside: %s\n", link, strerror(errno));
                    free(keep);
                    return -1;
                }
                lsiw_log("%s and %s both had files of the same name; the prefix's copies are in %s\n",
                         link, target, keep);
                free(keep);
            }
        } else {
            lsiw_log("%s is not a folder; leaving it alone\n", link);
            return -1;
        }
    }
    char *parent = lsiw_cli_strdup(link);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; lsiw_mkdirs(parent, 0755); }
    free(parent);
    if (symlink(target, link) < 0) {
        lsiw_log("cannot create %s: %s\n", link, strerror(errno));
        return -1;
    }
    if (verbose) lsiw_log("%s -> %s\n", link, target);
    return 0;
}

static void write_links(const char *compat, struct links *l)
{
    size_t len = 0;
    for (size_t i = 0; i < l->n; i++) len += strlen(l->v[i]) + 1;
    char *buf = malloc(len + 1), *o = buf;
    for (size_t i = 0; i < l->n; i++) o += sprintf(o, "%s\n", l->v[i]);
    *o = 0;
    char *path = lsiw_cli_printf("%s/" LINKS_FILE, compat);
    if (lsiw_cli_write_file(path, buf, len, 0644) < 0)
        lsiw_log("cannot write %s: %s\n", path, strerror(errno));
    free(path);
    free(buf);
}

/* Link the prefix in <compat>/pfx to LSIW's folders. The prefix need not
 * exist yet: Proton fills a new prefix around existing links (it copies
 * only files that are missing). Returns 0, or -1 if a folder could not be
 * linked (the game still starts). */
/* Folders Steam's runtime container must be told about (STEAM_COMPAT_MOUNTS)
 * because they may lie outside the home folder: the C: drive and the
 * shared prefix, and the anchored Windows folders. */
static void add_mount(char **m, size_t *len, size_t *cap, const char *path)
{
    if (!path || path[0] != '/') return;
    size_t tl = strlen(path);
    if (*len + tl + 2 > *cap) { *cap = (*len + tl + 2) * 2; *m = realloc(*m, *cap); }
    if (*len) (*m)[(*len)++] = ':';
    memcpy(*m + *len, path, tl + 1);
    *len += tl;
}

/* The folders of the extra drives (D:, E:, ...): Steam's runtime
 * container shows only folders it is told about. */
static void add_drive_mounts(char **m, size_t *len, size_t *cap)
{
    for (int l = 'A'; l <= 'Z'; l++) {
        const char *p = l == 'C' ? NULL : lsiw_drive_path(l);
        if (p) add_mount(m, len, cap, p);
    }
}

static char *shared_mounts(void)
{
    size_t len = 0, cap = 256;
    char *m = calloc(1, cap);
    add_mount(&m, &len, &cap, lsiw_root_dir());
    add_mount(&m, &len, &cap, lsiw_shared_dir());
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++)
        if (t[i].anchor) add_mount(&m, &len, &cap, lsiw_var_linux(t[i].name));
    static const char *const disks[] = { "/mnt", "/media", "/run/media", "/srv", NULL };
    for (int i = 0; disks[i]; i++) if (is_dir(disks[i])) add_mount(&m, &len, &cap, disks[i]);
    add_drive_mounts(&m, &len, &cap);
    return m;
}

/* Proton keeps a list of the files it put into the prefix in
 * <compat>/tracked_files, and it reads that list whenever the prefix
 * already counts as made (pfx/creation_sync_guard, which Proton also
 * writes when it upgrades a prefix from before Proton 10). A prefix that
 * has the guard but not the list makes every Proton stop with
 * "FileNotFoundError: ... tracked_files" in update_builtin_libs, before the
 * program starts: a prefix Wine made and an older Proton upgraded, a
 * version file from another Proton, or a list deleted by hand. Repair it:
 *   - guard but no registry: the prefix was never made; the guard goes, so
 *     Proton makes the prefix (filling in only missing files);
 *   - otherwise an empty list: Proton adds the files it updates to it.
 * Nothing in the prefix is deleted. Returns 1 when something was repaired. */
int lsiw_proton_repair_prefix(const char *compat, int verbose)
{
    char *tracked = lsiw_cli_printf("%s/tracked_files", compat);
    char *guard = lsiw_cli_printf("%s/pfx/creation_sync_guard", compat);
    char *sysreg = lsiw_cli_printf("%s/pfx/system.reg", compat);
    struct stat st;
    int rc = 0;
    if (lstat(tracked, &st) == 0) {
        if (!S_ISREG(st.st_mode) && !is_file(tracked))
            lsiw_log("warning: %s is not a file; Proton cannot use this prefix\n", tracked);
        goto out;
    }
    int has_guard = lstat(guard, &st) == 0;
    int has_reg = is_file(sysreg);
    if (has_guard && !has_reg) {
        if (unlink(guard) == 0) {
            rc = 1;
            lsiw_log("the Proton prefix %s was never finished; Proton sets it up again\n", compat);
        }
    } else if (has_guard || has_reg) {
        int fd = open(tracked, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0) {
            close(fd);
            rc = 1;
            lsiw_log("repaired the Proton prefix %s (its tracked_files list was missing)\n", compat);
        } else if (errno != EEXIST)
            lsiw_log("cannot create %s: %s\n", tracked, strerror(errno));
    }
    if (verbose && !rc) lsiw_log("Proton prefix %s: %s\n", compat, has_reg ? "set up" : "new");
out:
    free(sysreg);
    free(guard);
    free(tracked);
    return rc;
}

int lsiw_proton_prepare(const char *compat, int verbose, char **mounts)
{
    lsiw_proton_repair_prefix(compat, verbose);
    if (lsiw_is_shared_compat(compat)) {
        /* The shared prefix: C: is the C: drive, which has everything. */
        lsiw_prefix_migrate(verbose);
        if (lsiw_sync_root(0) < 0) {}
        int rc = lsiw_prefix_link_drive(compat, verbose);
        char *m = shared_mounts();
        if (mounts) *mounts = m; else free(m);
        return rc;
    }
    char *drive_c = lsiw_cli_printf("%s/pfx/drive_c", compat);
    if (lsiw_mkdirs(drive_c, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", drive_c, strerror(errno));
        free(drive_c);
        return -1;
    }
    if (lsiw_sync_root(0) < 0) {}   /* creates every configured folder */
    struct links l = { NULL, 0 };
    int rc = 0;
    size_t mlen = 0, mcap = 256;
    char *m = calloc(1, mcap);
    for (size_t i = 0; i < sizeof user_links / sizeof user_links[0]; i++) {
        const char *target = lsiw_var_linux(user_links[i].var);
        if (!target || target[0] != '/') continue;
        char *link = lsiw_cli_printf("%s/%s", drive_c, user_links[i].rel);
        if (link_folder(link, target, verbose) < 0) rc = -1;
        else {
            links_add(&l, user_links[i].rel);
            size_t tl = strlen(target);
            if (mlen + tl + 2 > mcap) { mcap = (mlen + tl + 2) * 2; m = realloc(m, mcap); }
            if (mlen) m[mlen++] = ':';
            memcpy(m + mlen, target, tl + 1);
            mlen += tl;
        }
        free(link);
    }

    /* C:\ shows the whole Linux filesystem, as with LSIW's engine. */
    if (truthy(lsiw_setting_get("steam", "full_c_drive", "true"))) {
        DIR *d = opendir("/");
        struct dirent *de;
        while (d && (de = readdir(d))) {
            const char *n = de->d_name;
            if (skip_top(n)) continue;
            char *link = lsiw_cli_printf("%s/%s", drive_c, n);
            char *target = lsiw_cli_printf("/%s", n);
            struct stat st;
            char *cur = lsiw_cli_readlink(link);
            if (cur) {
                if (strcmp(cur, target)) { unlink(link); if (symlink(target, link) == 0) links_add(&l, n); }
                else links_add(&l, n);
            } else if (lstat(link, &st) < 0 && !exists_nocase(drive_c, n)) {
                if (symlink(target, link) == 0) links_add(&l, n);
            }
            free(cur);
            free(link);
            free(target);
        }
        if (d) closedir(d);
        /* Disks are usually mounted here; the runtime container shows
         * them only when listed. */
        static const char *const disks[] = { "/mnt", "/media", "/run/media", "/srv", NULL };
        for (int i = 0; disks[i]; i++) {
            if (!is_dir(disks[i])) continue;
            size_t tl = strlen(disks[i]);
            if (mlen + tl + 2 > mcap) { mcap = (mlen + tl + 2) * 2; m = realloc(m, mcap); }
            if (mlen) m[mlen++] = ':';
            memcpy(m + mlen, disks[i], tl + 1);
            mlen += tl;
        }
    }
    write_links(compat, &l);
    links_free(&l);
    free(drive_c);
    if (lsiw_prefix_link_drives(compat, verbose) < 0) rc = -1;
    add_drive_mounts(&m, &mlen, &mcap);
    if (mounts) *mounts = m; else free(m);
    return rc;
}

/* Remove the links lsiw_proton_prepare made (before Proton deletes a
 * prefix). */
int lsiw_proton_unlink(const char *compat, int verbose)
{
    char *path = lsiw_cli_printf("%s/" LINKS_FILE, compat);
    char *text = lsiw_cli_read_file(path, NULL);
    if (!text) { free(path); return 0; }
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *link = lsiw_cli_printf("%s/pfx/drive_c/%s", compat, line);
        struct stat st;
        if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode)) {
            unlink(link);
            if (verbose) lsiw_log("removed %s\n", link);
        }
        free(link);
    }
    free(text);
    unlink(path);
    free(path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Starting Proton                                                      */
/* ------------------------------------------------------------------ */

struct argvec { char **v; int n, cap; };

static void av_push(struct argvec *a, const char *s)
{
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = realloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    a->v[a->n++] = lsiw_cli_strdup(s);
    a->v[a->n] = NULL;
}

/* A tool's toolmanifest.vdf "commandline" ("/proton %verb%"), split on
 * spaces; a leading "/" means the tool's own folder. */
static void push_tool_command(struct argvec *a, const char *dir, const char *verb, const char *fallback)
{
    char *mf = lsiw_cli_printf("%s/toolmanifest.vdf", dir);
    char *text = lsiw_read_text(mf);
    free(mf);
    char *cmd = lsiw_vdf_value(text, "commandline");
    free(text);
    const char *c = cmd ? cmd : fallback;
    char *copy = lsiw_cli_strdup(c), *save = NULL;
    int first = 1;
    for (char *t = strtok_r(copy, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        char *tok;
        char *v = strstr(t, "%verb%");
        if (v) tok = lsiw_cli_printf("%.*s%s%s", (int)(v - t), t, verb, v + 6);
        else tok = lsiw_cli_strdup(t);
        if (first && tok[0] == '/') {
            char *full = lsiw_cli_printf("%s%s", dir, tok);
            av_push(a, full);
            free(full);
        } else av_push(a, tok);
        free(tok);
        first = 0;
    }
    free(copy);
    free(cmd);
}

static char *manifest_value(const char *dir, const char *key)
{
    char *mf = lsiw_cli_printf("%s/toolmanifest.vdf", dir);
    char *text = lsiw_read_text(mf);
    free(mf);
    char *v = lsiw_vdf_value(text, key);
    free(text);
    return v;
}

static void append_env_list(const char *name, const char *add)
{
    if (!add || !*add) return;
    const char *cur = getenv(name);
    char *v = cur && *cur ? lsiw_cli_printf("%s:%s", cur, add) : lsiw_cli_strdup(add);
    setenv(name, v, 1);
    free(v);
}

static int verb_uses_prefix(const char *verb)
{
    static const char *const v[] = { "run", "waitforexitandrun", "runinprefix", "getcompatpath",
                                     "getnativepath", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(verb, v[i])) return 1;
    return 0;
}

/* Run Proton (the folder `proton_dir`) with VERB ARGS..., the way Steam
 * would: inside the Steam Linux Runtime it asks for, when installed. Only
 * returns on failure. */
static int proton_exec(const char *proton_dir, const char *verb, int argc, char **argv, int verbose)
{
    const char *compat = getenv("STEAM_COMPAT_DATA_PATH");
    if (!compat || !*compat) {
        lsiw_log("STEAM_COMPAT_DATA_PATH is not set; Proton needs a prefix folder\n");
        return -1;
    }
    if (lsiw_mkdirs(compat, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", compat, strerror(errno));
        return -1;
    }
    if (!strcmp(verb, "destroyprefix")) lsiw_proton_unlink(compat, verbose);
    else if (verb_uses_prefix(verb)) {
        char *mounts = NULL;
        if (lsiw_proton_prepare(compat, verbose, &mounts) < 0)
            lsiw_log("warning: some folders of the prefix could not be linked to LSIW's folders\n");
        /* The runtime container only sees the home folder and Steam's
         * folders unless told about others. */
        append_env_list("STEAM_COMPAT_MOUNTS", mounts);
        free(mounts);
    }

    struct argvec a = { NULL, 0, 0 };
    char *appid = manifest_value(proton_dir, "require_tool_appid");
    char *runtime = appid ? lsiw_steam_app_dir(appid) : NULL;
    /* GE-Proton and similar builds run fine on the host; Valve's Proton
     * is built for the runtime, so it is worth a warning there. */
    if (appid && !runtime && (verbose || !getenv("UMU_ID")))
        lsiw_log("%s: %s asks for the Steam Linux Runtime (app %s), which is not installed; "
                 "starting it without the runtime\n", getenv("UMU_ID") ? "note" : "warning", proton_dir, appid);
    if (runtime) push_tool_command(&a, runtime, verb, "/_v2-entry-point --verb=%verb% --");
    push_tool_command(&a, proton_dir, verb, "/proton %verb%");
    for (int i = 0; i < argc; i++) av_push(&a, argv[i]);

    /* The tools in the chain, as Steam lists them for pressure-vessel. */
    char *tools = runtime ? lsiw_cli_printf("%s:%s", proton_dir, runtime) : lsiw_cli_strdup(proton_dir);
    setenv("STEAM_COMPAT_TOOL_PATHS", tools, 1);
    free(tools);
    free(runtime);
    free(appid);

    if (verbose) {
        lsiw_log("running:");
        for (int i = 0; i < a.n; i++) fprintf(stderr, " %s", a.v[i]);
        fputc('\n', stderr);
    }
    fflush(NULL);
    execv(a.v[0], a.v);
    lsiw_log("cannot start %s: %s\n", a.v[0], strerror(errno));
    return -1;
}

const char *lsiw_proton_setting(void)
{
    const char *v = lsiw_setting_get("engine", "proton", NULL);
    return v && *v ? v : NULL;
}

int lsiw_proton_available(void)
{
    struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_PROTON, lsiw_proton_setting());
    int ok = r != NULL;
    lsiw_runner_free(r);
    return ok;
}

static int ask_steam_to_install(const char *appid, const char *what);

/* lsiw run --engine=proton PROGRAM ARGS: LSIW's own Proton prefix. */
int lsiw_proton_run(const char *linux_path, int argc, char **argv, int verbose)
{
    const char *setting = lsiw_proton_setting();
    struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_PROTON, setting);
    char *appid, *display;
    if (!r && lsiw_proton_official(setting, &appid, &display)) {
        /* LSIW's twin of a Valve Proton Steam has not downloaded yet. */
        char *what = lsiw_cli_printf("LSIW %s", display);
        ask_steam_to_install(appid, what);
        free(what); free(appid); free(display);
        errno = ENOENT;
        return -1;
    }
    if (!r) {
        if (setting) lsiw_log("the configured Proton '%s' was not found ('lsiw runners' lists what is installed)\n", setting);
        else lsiw_log("no Proton is installed: install one in Steam, or run 'lsiw runners install ge'\n");
        errno = ENOENT;
        return -1;
    }
    /* The shared prefix (or the program's own), never a leftover
     * STEAM_COMPAT_DATA_PATH of whatever started lsiw. */
    setenv("STEAM_COMPAT_DATA_PATH", lsiw_compat_dir(), 1);
    /* Proton copies Steam's glue (legacycompat/steamclient.dll ...) from
     * the Steam client's folder; without Steam an empty folder will do. */
    const char *cip = getenv("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    char *client = lsiw_steam_client_dir();
    if (!cip || !*cip || !is_dir(cip)) {
        char *c = client ? lsiw_cli_strdup(client) : lsiw_cli_printf("%s/steam-client", lsiw_data_dir());
        lsiw_mkdirs(c, 0755);
        setenv("STEAM_COMPAT_CLIENT_INSTALL_PATH", c, 1);
        free(c);
    }
    setenv("SteamAppId", "0", 0);
    setenv("SteamGameId", "0", 0);
    /* GE-Proton and the other builds made for umu-launcher start programs
     * without Steam in umu mode. Valve's Proton starts them through its
     * steam.exe, which works outside Steam too, but programs that use
     * Steam's API need the Steam client. */
    char *script = lsiw_cli_printf("%s/proton", r->path);
    char *text = lsiw_read_text(script);
    const char *umu_env = getenv("LSIW_PROTON_UMU");      /* 0: never umu mode (tests) */
    int umu = text && strstr(text, "UMU_ID") && !(umu_env && !strcmp(umu_env, "0"));
    free(text);
    free(script);
    if (umu) setenv("UMU_ID", "umu-0", 0);
    else if (!client && verbose)
        lsiw_log("note: Steam is not installed; %s starts programs, but those that need Steam "
                 "(its overlay, achievements, DRM) cannot reach it\n", r->name);
    free(client);
    if (!verbose) setenv("WINEDEBUG", "-all", 0);
    if (verbose) lsiw_log("running with %s (%s)\n", r->name, r->path);

    /* Proton maps / to Z:, and C:\ shows the Linux folders too: hand
     * Windows paths over, since Windows programs read '/' as an option. */
    int shared = lsiw_is_shared_compat(lsiw_compat_dir());
    int full_c = shared || truthy(lsiw_setting_get("steam", "full_c_drive", "true"));
    int translate = strcasecmp(lsiw_setting_get("runtime", "translate_args", "true"), "false") != 0;
    char **args = calloc((size_t)argc + 2, sizeof *args);
    args[0] = lsiw_cli_strdup(linux_path);
    for (int i = 1; i < argc; i++) {
        struct stat st;
        const char *s = argv[i];
        if (translate && s[0] == '/' && s[1] && stat(s, &st) == 0) {
            char *w;
            if (shared) w = lsiw_linux_to_win(s);
            else {
                w = lsiw_cli_printf("%c:%s", full_c ? 'C' : 'Z', s);
                for (char *c = w; *c; c++) if (*c == '/') *c = '\\';
            }
            args[i] = w;
        } else args[i] = lsiw_cli_strdup(s);
    }
    proton_exec(r->path, "waitforexitandrun", argc, args, verbose);
    lsiw_runner_free(r);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Valve's Proton versions, installed on demand                         */
/* ------------------------------------------------------------------ */

/* 1 when Steam has fully installed app `appid` ("StateFlags" "4") and its
 * folder holds `marker` (the proton launcher, or the runtime's manifest).
 * *dir receives the folder (caller frees). */
static int app_installed(const char *appid, const char *marker, char **dir)
{
    *dir = NULL;
    char **libs = lsiw_steam_libraries();
    int ok = 0;
    for (char **l = libs; l && *l && !ok; l++) {
        char *acf = lsiw_cli_printf("%s/steamapps/appmanifest_%s.acf", *l, appid);
        char *text = lsiw_read_text(acf);
        char *inst = lsiw_vdf_value(text, "installdir");
        char *state = lsiw_vdf_value(text, "StateFlags");
        if (inst && (!state || !strcmp(state, "4"))) {
            char *d = lsiw_cli_printf("%s/steamapps/common/%s", *l, inst);
            char *m = lsiw_cli_printf("%s/%s", d, marker);
            if (is_file(m)) { ok = 1; *dir = d; } else free(d);
            free(m);
        }
        free(state); free(inst); free(text); free(acf);
    }
    lsiw_strv_free(libs);
    return ok;
}

/* Tell the person something while Steam shows no terminal. */
static void notify(const char *title, const char *body)
{
    lsiw_log("%s: %s\n", title, body);
    char *nt = lsiw_cli_which("notify-send");
    if (nt) {
        char *argv[] = { nt, "--app-name=LSIW", (char *)title, (char *)body, NULL };
        lsiw_cli_spawn(argv, NULL, 1);
        free(nt);
    }
}

/* Ask Steam to install app `appid`, the way choosing a Proton it has not
 * downloaded yet does. Steam may not download while a game is running, so
 * the launch stops here and the game is started again afterwards. */
static int ask_steam_to_install(const char *appid, const char *what)
{
    char *url = lsiw_cli_printf("steam://install/%s", appid);
    char *steam = lsiw_cli_which("steam");
    char *argv[] = { steam ? steam : "xdg-open", url, NULL };
    int rc = lsiw_cli_spawn(argv, NULL, 1);
    char *msg = rc == 0
        ? lsiw_cli_printf("%s is not installed yet. Steam is now installing it; start the game again "
                          "when the download has finished.", what)
        : lsiw_cli_printf("%s is not installed yet, and Steam could not be asked to install it. "
                          "Pick %s for any game once (or open %s), then try again.", what, what, url);
    notify("LSIW Proton", msg);
    free(msg);
    free(steam);
    free(url);
    return 127;
}

/* ------------------------------------------------------------------ */
/* lsiw steam-run                                                       */
/* ------------------------------------------------------------------ */

/* The app ID of the game Steam is starting: $SteamAppId, else the name of
 * its compatdata folder. */
static char *game_appid(const char *compat)
{
    const char *id = getenv("SteamAppId");
    if (id && *id && strcmp(id, "0")) return lsiw_cli_strdup(id);
    const char *b = compat ? strrchr(compat, '/') : NULL;
    return lsiw_cli_strdup(b ? b + 1 : "");
}

static int in_list(const char *list, const char *item)
{
    if (!list || !*item) return 0;
    char *copy = lsiw_cli_strdup(list), *save = NULL;
    int found = 0;
    for (char *t = strtok_r(copy, ", ", &save); t && !found; t = strtok_r(NULL, ", ", &save))
        found = !strcmp(t, item);
    free(copy);
    return found;
}

/* 1 when the game with this app ID runs in the shared prefix. */
static int game_shared(const char *appid)
{
    if (!truthy(lsiw_setting_get("steam", "shared_prefix", "true"))) return 0;
    return !in_list(lsiw_setting_get("steam", "own_prefix", NULL), appid);
}

#define IMPORTED_FILE "lsiw-imported"

/* A game that had a prefix of its own before: copy what it saved in the
 * user folders of that prefix into LSIW's folders (missing files only),
 * once. The old prefix stays as it is. Returns the number of files
 * copied, -1 on an error. */
static long import_game_prefix(const char *compat, int verbose)
{
    char *mark = lsiw_cli_printf("%s/" IMPORTED_FILE, compat);
    char *users = lsiw_cli_printf("%s/pfx/drive_c/users", compat);
    long total = 0;
    struct stat st;
    if (stat(mark, &st) == 0 || lstat(users, &st) < 0 || !S_ISDIR(st.st_mode)) goto out;
    int errors = 0;
    char *log = lsiw_cli_strdup("");
    for (size_t i = 0; i < sizeof user_links / sizeof user_links[0]; i++) {
        char *src = lsiw_cli_printf("%s/pfx/drive_c/%s", compat, user_links[i].rel);
        const char *dst = lsiw_var_linux(user_links[i].var);
        if (dst && lstat(src, &st) == 0 && S_ISDIR(st.st_mode)) {
            long n = lsiw_copy_missing(src, dst, verbose);
            if (n < 0) errors++;
            else if (n > 0) {
                total += n;
                char *nl = lsiw_cli_printf("%s%s -> %s (%ld)\n", log, src, dst, n);
                free(log);
                log = nl;
            }
        }
        free(src);
    }
    char *text = lsiw_cli_printf("# Copied by LSIW into its shared folders; this prefix is kept as it was.\n%s",
                                 log);
    if (!errors) lsiw_cli_write_file(mark, text, strlen(text), 0644);
    if (total) lsiw_log("copied %ld files and folders from the game's own prefix %s into LSIW's folders\n",
                        total, compat);
    free(text);
    free(log);
    if (errors) total = -1;
out:
    free(users);
    free(mark);
    return total;
}

/* Before Proton starts a Steam game: the shared prefix, unless this game
 * has its own. */
static void steam_game_prefix(int verbose)
{
    const char *compat = getenv("STEAM_COMPAT_DATA_PATH");
    if (!compat || !*compat || lsiw_is_shared_compat(compat)) return;
    char *appid = game_appid(compat);
    if (game_shared(appid)) {
        import_game_prefix(compat, verbose);
        lsiw_prefix_migrate(verbose);
        if (verbose) lsiw_log("game %s runs in the shared prefix %s\n", appid, lsiw_shared_dir());
        setenv("STEAM_COMPAT_DATA_PATH", lsiw_shared_dir(), 1);
    }
    free(appid);
}

/* lsiw steam-run TOOLDIR VERB ARGS...: what an LSIW Proton tool runs. */
int lsiw_cmd_steam_run(int argc, char **argv)
{
    if (argc < 3) {
        lsiw_log("usage: lsiw steam-run TOOL-FOLDER VERB [ARGS...] (started by Steam's LSIW Proton tools)\n");
        return 2;
    }
    const char *tool = argv[1];
    char *conf = lsiw_cli_printf("%s/" TOOL_CONF, tool);
    char *text = lsiw_cli_read_file(conf, NULL);
    free(conf);
    char *proton = text ? conf_get(text, "proton") : NULL;
    char *appid = text ? conf_get(text, "appid") : NULL;
    char *alias = text ? conf_get(text, "alias") : NULL;
    char *what = text ? conf_get(text, "proton_name") : NULL;
    free(text);
    int verbose = lsiw_verbose || truthy(getenv("LSIW_VERBOSE"));

    if (alias && *alias) {
        /* "Proton Stable": whichever version Steam calls stable today. */
        struct lsiw_steam_tool *v;
        size_t n = lsiw_steam_official_tools(&v);
        const struct lsiw_steam_tool *t = lsiw_steam_tool_by_alias(v, n, alias);
        if (t) {
            free(appid);
            appid = lsiw_cli_strdup(t->appid);
            free(what);
            what = lsiw_cli_strdup(t->display);
        }
        lsiw_steam_tools_free(v, n);
    }
    if (appid && *appid) {
        /* One of Valve's Protons: wherever Steam installed it. */
        free(proton);
        proton = NULL;
        char *dir;
        if (!app_installed(appid, "proton", &dir)) {
            int rc = ask_steam_to_install(appid, what && *what ? what : "This Proton");
            free(appid); free(alias); free(what);
            return rc;
        }
        proton = dir;
    }
    char *launcher = proton ? lsiw_cli_printf("%s/proton", proton) : NULL;
    if (!launcher || !is_file(launcher)) {
        char *m = lsiw_cli_printf("the Proton this tool runs (%s) is gone; run 'lsiw steam add' and "
                                  "restart Steam", proton ? proton : "?");
        notify("LSIW Proton", m);
        free(m);
        free(launcher); free(proton); free(appid); free(alias); free(what);
        return 127;
    }
    free(launcher);

    /* The Steam Linux Runtime this Proton runs in. */
    char *rt = manifest_value(proton, "require_tool_appid");
    char *rtdir = NULL;
    if (rt && !app_installed(rt, "toolmanifest.vdf", &rtdir)) {
        int rc = ask_steam_to_install(rt, "The Steam Linux Runtime this Proton needs");
        free(rt); free(proton); free(appid); free(alias); free(what);
        return rc;
    }
    free(rtdir);
    free(rt);
    steam_game_prefix(verbose);
    proton_exec(proton, argv[2], argc - 3, argv + 3, verbose);
    free(proton); free(appid); free(alias); free(what);
    return 127;
}

/* ------------------------------------------------------------------ */
/* Steam compatibility tools                                            */
/* ------------------------------------------------------------------ */

static char *slug(const char *s)
{
    char *r = lsiw_cli_strdup(s);
    for (char *c = r; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-' && *c != '_') *c = '_';
    return r;
}

static char *vdf_quote(const char *s)
{
    size_t n = strlen(s);
    char *r = malloc(2 * n + 1), *o = r;
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') *o++ = '\\';
        *o++ = *s;
    }
    *o = 0;
    return r;
}

static int is_lsiw_tool(const char *dir)
{
    char *c = lsiw_cli_printf("%s/" TOOL_CONF, dir);
    int r = is_file(c);
    free(c);
    return r;
}

static int remove_tool(const char *dir)
{
    static const char *const files[] = { "compatibilitytool.vdf", "toolmanifest.vdf", TOOL_SCRIPT, TOOL_CONF, NULL };
    for (int i = 0; files[i]; i++) {
        char *f = lsiw_cli_printf("%s/%s", dir, files[i]);
        unlink(f);
        free(f);
    }
    return rmdir(dir);
}

/* One LSIW tool to put into Steam. */
struct tool_spec {
    char *dir;       /* "LSIW-proton_11"                                  */
    char *display;   /* "LSIW Proton 11.0-2"                              */
    char *conf;      /* lsiw-tool.conf                                    */
    char *proton;    /* the Proton folder, NULL until Steam installs it   */
    char *note;      /* printed after the name                            */
};

struct specs { struct tool_spec *v; size_t n; };

static void spec_add(struct specs *s, char *dir, char *display, char *conf, const char *proton, char *note)
{
    s->v = realloc(s->v, (s->n + 1) * sizeof *s->v);
    s->v[s->n++] = (struct tool_spec){ dir, display, conf, proton ? lsiw_cli_strdup(proton) : NULL, note };
}

static void specs_free(struct specs *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].dir); free(s->v[i].display); free(s->v[i].conf);
        free(s->v[i].proton); free(s->v[i].note);
    }
    free(s->v);
}

static int same_dir(const char *a, const char *b)
{
    char *ra = realpath(a, NULL), *rb = realpath(b, NULL);
    int same = ra && rb && !strcmp(ra, rb);
    free(ra);
    free(rb);
    return same;
}

/* Every LSIW tool Steam should list: a twin of each of Valve's Proton
 * versions (installed or not) and of "Proton Stable", then one per other
 * Proton installed (GE-Proton, custom builds, LSIW's downloads). */
static void collect_specs(struct specs *s)
{
    struct lsiw_steam_tool *off;
    size_t noff = lsiw_steam_official_tools(&off);
    int legacy = truthy(lsiw_setting_get("steam", "legacy_versions", "true"));
    char **official_dirs = calloc(noff + 1, sizeof *official_dirs);

    const struct lsiw_steam_tool *stable = lsiw_steam_tool_by_alias(off, noff, "proton-stable");
    if (stable) {
        char *dir = NULL;
        app_installed(stable->appid, "proton", &dir);
        spec_add(s, lsiw_cli_strdup("LSIW-proton-stable"), lsiw_cli_strdup("LSIW Proton Stable"),
                 lsiw_cli_printf("alias=proton-stable\nname=LSIW Proton Stable\nproton_name=Proton Stable\n"),
                 dir, lsiw_cli_printf("now %s%s", stable->display, dir ? "" : ", installed by Steam on first use"));
        free(dir);
    }
    for (size_t i = 0; i < noff; i++) {
        app_installed(off[i].appid, "proton", &official_dirs[i]);
        if (off[i].legacy && !legacy && !official_dirs[i]) continue;
        char *k = slug(off[i].key);
        char *display = lsiw_cli_printf("LSIW %s", off[i].display);
        spec_add(s, lsiw_cli_printf("LSIW-%s", k), display,
                 lsiw_cli_printf("appid=%s\nofficial=%s\nname=%s\nproton_name=%s\n", off[i].appid, off[i].key,
                                 display, off[i].display),
                 official_dirs[i],
                 official_dirs[i] ? lsiw_cli_strdup(official_dirs[i])
                                  : lsiw_cli_strdup("installed by Steam on first use"));
        free(k);
    }

    struct lsiw_runner *v;
    size_t n = lsiw_runners(LSIW_RUNNER_PROTON, &v);
    for (size_t i = 0; i < n; i++) {
        if (v[i].foreign) continue;               /* built for another processor */
        int twin = 0;
        for (size_t j = 0; j < noff && !twin; j++)
            twin = official_dirs[j] && same_dir(official_dirs[j], v[i].path);
        if (twin) continue;                       /* already covered above */
        char *k = slug(v[i].id);
        char *display = lsiw_steam_tool_display_name(&v[i]);
        spec_add(s, lsiw_cli_printf("LSIW-%s", k), display,
                 lsiw_cli_printf("proton=%s\nrunner=%s\nname=%s\nproton_name=%s\n", v[i].path, v[i].id,
                                 display, v[i].name),
                 v[i].path, lsiw_cli_strdup(v[i].path));
        free(k);
    }
    lsiw_runners_free(v, n);
    for (size_t i = 0; i < noff; i++) free(official_dirs[i]);
    free(official_dirs);
    lsiw_steam_tools_free(off, noff);
}

static int write_tool(const char *tools, const struct tool_spec *sp, const char *lsiw)
{
    char *dir = lsiw_cli_printf("%s/%s", tools, sp->dir);
    int rc = -1;
    if (lsiw_mkdirs(dir, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", dir, strerror(errno));
        free(dir);
        return -1;
    }
    char *qd = vdf_quote(sp->display), *qi = vdf_quote(sp->dir);
    char *ct = lsiw_cli_printf(
        "// Written by LSIW ('lsiw steam add'): Proton with the folders chosen in LSIW Control.\n"
        "\"compatibilitytools\"\n{\n  \"compat_tools\"\n  {\n    \"%s\"\n    {\n"
        "      \"install_path\" \".\"\n      \"display_name\" \"%s\"\n"
        "      \"from_oslist\" \"windows\"\n      \"to_oslist\" \"linux\"\n    }\n  }\n}\n",
        qi, qd);
    /* Proton's own manifest, minus the runtime: lsiw steam-run starts the
     * Steam Linux Runtime itself, after preparing the prefix on the host. */
    char *layer = sp->proton ? manifest_value(sp->proton, "compatmanager_layer_name") : NULL;
    char *sessions = sp->proton ? manifest_value(sp->proton, "use_sessions") : NULL;
    char *ql = vdf_quote(layer ? layer : "proton");
    char *tm = lsiw_cli_printf(
        "// Written by LSIW ('lsiw steam add').\n"
        "\"manifest\"\n{\n  \"version\" \"2\"\n  \"commandline\" \"/" TOOL_SCRIPT " %%verb%%\"\n"
        "  \"use_sessions\" \"%s\"\n  \"compatmanager_layer_name\" \"%s\"\n"
        "  \"use_tool_subprocess_reaper\" \"1\"\n}\n",
        sessions ? sessions : "1", ql);
    char *script = lsiw_cli_printf(
        "#!/bin/sh\n"
        "# LSIW Proton, written by 'lsiw steam add': runs the Proton named in\n"
        "# " TOOL_CONF " with the Windows folders chosen in LSIW Control.\n"
        "here=$(dirname -- \"$(readlink -f -- \"$0\")\")\n"
        "lsiw='%s'\n"
        "[ -x \"$lsiw\" ] || lsiw=$(command -v lsiw) || {\n"
        "    echo 'LSIW Proton: the lsiw command is not installed' >&2\n"
        "    exit 127\n"
        "}\n"
        "exec \"$lsiw\" steam-run \"$here\" \"$@\"\n", lsiw);
    struct { const char *name, *data; int mode; } files[] = {
        { "compatibilitytool.vdf", ct, 0644 }, { "toolmanifest.vdf", tm, 0644 },
        { TOOL_SCRIPT, script, 0755 }, { TOOL_CONF, sp->conf, 0644 },
    };
    rc = 0;
    for (size_t i = 0; i < 4; i++) {
        char *f = lsiw_cli_printf("%s/%s", dir, files[i].name);
        if (lsiw_cli_write_file(f, files[i].data, strlen(files[i].data), files[i].mode) < 0) {
            lsiw_log("cannot write %s: %s\n", f, strerror(errno));
            rc = -1;
        }
        free(f);
    }
    free(ct); free(tm); free(script);
    free(qd); free(qi); free(ql); free(layer); free(sessions);
    free(dir);
    return rc;
}

static int wanted_tool(const struct specs *s, const char *dirname)
{
    for (size_t i = 0; i < s->n; i++)
        if (!strcmp(s->v[i].dir, dirname)) return 1;
    return 0;
}

static void sweep_tools(const char *tools, const struct specs *s, int verbose)
{
    DIR *d = opendir(tools);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *dir = lsiw_cli_printf("%s/%s", tools, de->d_name);
        if (is_lsiw_tool(dir) && !wanted_tool(s, de->d_name)) {
            if (remove_tool(dir) == 0 && verbose) printf("  removed %s\n", de->d_name);
        }
        free(dir);
    }
    if (d) closedir(d);
}

int lsiw_steam_integrate(int add, int verbose)
{
    char **roots = lsiw_steam_roots();
    if (!roots) {
        lsiw_log("Steam was not found (looked in ~/.steam, ~/.local/share/Steam and the Flatpak "
                 "and Snap folders; set [steam] root in %s if it is elsewhere)\n", lsiw_config_path());
        return -1;
    }
    struct specs s = { NULL, 0 };
    if (add) collect_specs(&s);
    char *lsiw = self_exe();
    int rc = 0, done = 0;
    for (char **r = roots; *r; r++) {
        if (lsiw_steam_root_sandboxed(*r)) {
            lsiw_log("skipping %s: the Flatpak and Snap builds of Steam cannot start programs installed "
                     "outside their sandbox; use Steam from your distribution's packages\n", *r);
            continue;
        }
        char *tools = lsiw_cli_printf("%s/compatibilitytools.d", *r);
        if (add && lsiw_mkdirs(tools, 0755) < 0) {
            lsiw_log("cannot create %s: %s\n", tools, strerror(errno));
            free(tools);
            rc = -1;
            continue;
        }
        if (verbose) printf("%s %s:\n", add ? "Steam integration in" : "Removing the LSIW tools from", *r);
        sweep_tools(tools, &s, verbose);
        for (size_t i = 0; i < s.n; i++) {
            if (write_tool(tools, &s.v[i], lsiw) < 0) { rc = -1; continue; }
            if (verbose) printf("  %-28s %s\n", s.v[i].display, s.v[i].note);
        }
        if (add && !s.n && verbose)
            printf("  no Proton found: start Steam once, or run 'lsiw runners install ge'\n");
        done++;
        free(tools);
    }
    free(lsiw);
    specs_free(&s);
    lsiw_strv_free(roots);
    if (!done) return -1;
    if (truthy(lsiw_setting_get("steam", "integration", NULL)) != add) {
        lsiw_setting_set("steam", "integration", add ? "true" : "false");
        if (lsiw_config_save() < 0) {
            lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
            rc = -1;
        }
    }
    if (verbose)
        printf("%s\n", add ? "Restart Steam, then pick an \"LSIW Proton\" tool under a game's Properties > "
                             "Compatibility (or in Settings > Compatibility for every game). Valve's Proton "
                             "versions that are not installed yet are installed by Steam the first time "
                             "their LSIW twin is used."
                           : "Restart Steam so it forgets the LSIW tools. Games set to use one fall back to "
                             "Steam's default.");
    return rc;
}

/* After Proton versions change: refresh the tools if integration is on. */
static void steam_refresh_if_enabled(void)
{
    if (truthy(lsiw_setting_get("steam", "integration", NULL))) lsiw_steam_integrate(1, 0);
}

static int steam_status(void)
{
    char **roots = lsiw_steam_roots();
    int on = truthy(lsiw_setting_get("steam", "integration", NULL));
    printf("Steam integration: %s\n", on ? "on" : "off");
    if (!roots) {
        printf("Steam:             not found\n");
        return 1;
    }
    for (char **r = roots; *r; r++) {
        printf("Steam:             %s%s\n", *r, lsiw_steam_root_sandboxed(*r) ? " (sandboxed; not supported)" : "");
        char *tools = lsiw_cli_printf("%s/compatibilitytools.d", *r);
        struct dirent **ents = NULL;
        int ne = scandir(tools, &ents, NULL, alphasort);
        for (int ei = 0; ei < ne; ei++) {
            struct dirent *de = ents[ei];
            if (de->d_name[0] == '.') continue;
            char *dir = lsiw_cli_printf("%s/%s", tools, de->d_name);
            if (is_lsiw_tool(dir)) {
                char *c = lsiw_cli_printf("%s/" TOOL_CONF, dir);
                char *text = lsiw_cli_read_file(c, NULL);
                char *name = text ? conf_get(text, "name") : NULL;
                char *pp = text ? conf_get(text, "proton") : NULL;
                char *appid = text ? conf_get(text, "appid") : NULL;
                char *alias = text ? conf_get(text, "alias") : NULL;
                char *where = NULL;
                if (alias) where = lsiw_cli_printf("follows Steam's %s", alias);
                else if (appid) {
                    char *d2 = NULL;
                    where = app_installed(appid, "proton", &d2) ? lsiw_cli_strdup(d2)
                          : lsiw_cli_printf("app %s, installed by Steam on first use", appid);
                    free(d2);
                } else {
                    char *launcher = pp ? lsiw_cli_printf("%s/proton", pp) : NULL;
                    where = lsiw_cli_printf("%s%s", pp ? pp : "?",
                                            launcher && is_file(launcher) ? "" : "  (missing: run 'lsiw steam add')");
                    free(launcher);
                }
                printf("  %-28s %s\n", name ? name : de->d_name, where);
                free(where); free(alias); free(appid); free(pp); free(name); free(text); free(c);
            }
            free(dir);
        }
        for (int ei = 0; ei < ne; ei++) free(ents[ei]);
        free(ents);
        free(tools);
    }
    printf("Whole filesystem on C: in Proton prefixes: %s\n",
           truthy(lsiw_setting_get("steam", "full_c_drive", "true")) ? "yes" : "no");
    lsiw_strv_free(roots);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Steam's compatibility tool choices (config/config.vdf)               */
/* ------------------------------------------------------------------ */

/* Steam keeps the tool of every game in config/config.vdf:
 *   "CompatToolMapping" { "0" { "name" "proton_experimental" ... }
 *                         "<appid>" { "name" "GE-Proton9-20" ... } }
 * "0" is the default for every game without its own choice, new games
 * included. Steam rewrites the file when it exits, so it is edited only
 * while Steam is not running, and a copy is kept first. */

struct vtok { char type; size_t start, end; char *s; };   /* type: 's' string, '{', '}' */

static size_t vdf_lex(const char *t, struct vtok **out)
{
    size_t n = 0, cap = 256;
    struct vtok *v = malloc(cap * sizeof *v);
    for (size_t i = 0; t[i]; ) {
        char c = t[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && t[i + 1] == '/') { while (t[i] && t[i] != '\n') i++; continue; }
        if (n + 1 >= cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
        if (c == '{' || c == '}') { v[n++] = (struct vtok){ c, i, i + 1, NULL }; i++; continue; }
        size_t st = i, len = 0;
        char *s = malloc(strlen(t + i) + 1);
        if (c == '"') {
            i++;
            while (t[i] && t[i] != '"') {
                if (t[i] == '\\' && t[i + 1]) i++;
                s[len++] = t[i++];
            }
            if (t[i]) i++;
        } else {
            while (t[i] && !isspace((unsigned char)t[i]) && t[i] != '{' && t[i] != '}' && t[i] != '"') s[len++] = t[i++];
        }
        s[len] = 0;
        v[n++] = (struct vtok){ 's', st, i, s };
    }
    *out = v;
    return n;
}

static void vdf_free(struct vtok *v, size_t n)
{
    for (size_t i = 0; i < n; i++) free(v[i].s);
    free(v);
}

struct vedit { size_t start, end; char *text; };
struct vedits { struct vedit *v; size_t n; };

static void vedit_add(struct vedits *e, size_t start, size_t end, char *text)
{
    e->v = realloc(e->v, (e->n + 1) * sizeof *e->v);
    e->v[e->n++] = (struct vedit){ start, end, text };
}

static int vedit_cmp(const void *a, const void *b)
{
    const struct vedit *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static char *vedit_apply(const char *t, struct vedits *e)
{
    qsort(e->v, e->n, sizeof *e->v, vedit_cmp);
    size_t len = strlen(t), cap = len + 1;
    for (size_t i = 0; i < e->n; i++) cap += strlen(e->v[i].text);
    char *out = malloc(cap), *o = out;
    size_t pos = 0;
    for (size_t i = 0; i < e->n; i++) {
        memcpy(o, t + pos, e->v[i].start - pos);
        o += e->v[i].start - pos;
        o = stpcpy(o, e->v[i].text);
        pos = e->v[i].end;
        free(e->v[i].text);
    }
    strcpy(o, t + pos);
    free(e->v);
    e->v = NULL;
    e->n = 0;
    return out;
}

/* The LSIW twin (a folder name in compatibilitytools.d) of the tool Steam
 * calls `name`, or NULL. */
static char *twin_of(const char *tools, const char *name)
{
    if (!strncmp(name, "LSIW-", 5)) return NULL;
    char *k = slug(name);
    char *dir = lsiw_cli_printf("%s/LSIW-%s", tools, k);
    free(k);
    char *res = NULL;
    if (is_lsiw_tool(dir)) res = lsiw_cli_strdup(strrchr(dir, '/') + 1);
    free(dir);
    return res;
}

static int steam_is_running(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int running = 0;
    while (d && (de = readdir(d)) && !running) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char *f = lsiw_cli_printf("/proc/%s/comm", de->d_name);
        char *comm = lsiw_read_text(f);
        if (comm) {
            comm[strcspn(comm, "\n")] = 0;
            running = !strcmp(comm, "steam") || !strcmp(comm, "steamwebhelper");
        }
        free(comm);
        free(f);
    }
    if (d) closedir(d);
    return running;
}

/* Set the default tool of one Steam installation to `tool` and, with
 * existing, move every game that names one of Valve's Protons or another
 * tool with an LSIW twin to that twin. Returns 0, or -1. */
static int steam_set_default_in(const char *root, const char *tool, int existing, int dry)
{
    char *path = lsiw_cli_printf("%s/config/config.vdf", root);
    char *tools = lsiw_cli_printf("%s/compatibilitytools.d", root);
    char *text = lsiw_read_text(path);
    int rc = -1;
    if (!text) {
        lsiw_log("%s: no config/config.vdf yet (start Steam once)\n", root);
        goto out;
    }
    struct vtok *v;
    size_t n = vdf_lex(text, &v);
    struct vedits e = { NULL, 0 };
    /* Walk the tree: names[] is the path of block names. */
    const char *names[64];
    int depth = 0;
    long map_close = -1, steam_close = -1;
    int map_depth = -1, have_default = 0, moved = 0, kept = 0;
    const char *app = NULL;
    for (size_t i = 0; i < n; i++) {
        if (v[i].type == 's' && i + 1 < n && v[i + 1].type == '{') {
            if (depth < 64) names[depth] = v[i].s;
            depth++;
            if (!strcasecmp(v[i].s, "CompatToolMapping") && map_depth < 0) map_depth = depth;
            if (map_depth > 0 && depth == map_depth + 1) app = v[i].s;
            i++;
            continue;
        }
        if (v[i].type == 's' && i + 1 < n && v[i + 1].type == 's') {
            if (app && depth == map_depth + 1 && !strcasecmp(v[i].s, "name")) {
                const struct vtok *val = &v[i + 1];
                int is_default = !strcmp(app, "0");
                char *want = NULL;
                if (is_default) {
                    have_default = 1;
                    if (strcmp(val->s, tool)) want = lsiw_cli_strdup(tool);
                } else if (existing && *val->s) {
                    want = twin_of(tools, val->s);
                    if (!want && strncmp(val->s, "LSIW-", 5)) {
                        printf("  game %-10s keeps %s (LSIW has no twin of it; pick an LSIW tool for it in Steam)\n",
                               app, val->s);
                        kept++;
                    }
                }
                if (want) {
                    if (!is_default) {
                        printf("  game %-10s %s -> %s\n", app, val->s, want);
                        moved++;
                    }
                    char *q = vdf_quote(want);
                    vedit_add(&e, val->start, val->end, lsiw_cli_printf("\"%s\"", q));
                    free(q);
                    free(want);
                }
            }
            i++;
            continue;
        }
        if (v[i].type == '}') {
            if (depth == map_depth) { map_close = (long)v[i].start; map_depth = -2; }
            if (depth >= 2 && depth <= 64 && !strcasecmp(names[depth - 1], "Steam") &&
                !strcasecmp(names[depth - 2], "Valve") && steam_close < 0)
                steam_close = (long)v[i].start;
            if (map_depth > 0 && depth == map_depth + 1) app = NULL;
            depth--;
        }
    }
    char *q = vdf_quote(tool);
    char *entry = lsiw_cli_printf("\t\"0\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"name\"\t\t\"%s\"\n\t\t\t\t\t\t\"config\"\t\t\"\"\n"
                                  "\t\t\t\t\t\t\"priority\"\t\t\"75\"\n\t\t\t\t\t}\n\t\t\t\t", q);
    free(q);
    if (!have_default) {
        if (map_close >= 0) vedit_add(&e, (size_t)map_close, (size_t)map_close, entry);
        else if (steam_close >= 0) {
            vedit_add(&e, (size_t)steam_close, (size_t)steam_close,
                      lsiw_cli_printf("\t\"CompatToolMapping\"\n\t\t\t\t{\n\t\t\t\t%s}\n\t\t\t", entry));
            free(entry);
        } else {
            lsiw_log("%s: Steam's settings block was not found; set the default tool in Steam instead "
                     "(Settings > Compatibility)\n", path);
            free(entry);
            vdf_free(v, n);
            goto out;
        }
    } else free(entry);
    vdf_free(v, n);
    printf("  default for every game: %s%s\n", tool, have_default ? "" : " (new setting)");
    if (existing) printf("  %d games moved to their LSIW twin, %d kept\n", moved, kept);
    if (!e.n || dry) {
        for (size_t i = 0; i < e.n; i++) free(e.v[i].text);
        free(e.v);
        rc = 0;
        goto out;
    }
    char *updated = vedit_apply(text, &e);
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    char *backup = lsiw_cli_printf("%s.lsiw-backup-%s", path, stamp);
    if (lsiw_cli_write_file(backup, text, strlen(text), 0600) < 0) {
        lsiw_log("cannot write %s: %s; nothing was changed\n", backup, strerror(errno));
    } else if (lsiw_cli_write_file(path, updated, strlen(updated), 0644) < 0) {
        lsiw_log("cannot write %s: %s\n", path, strerror(errno));
    } else {
        printf("  saved %s (the previous file is %s)\n", path, backup);
        rc = 0;
    }
    free(backup);
    free(updated);
out:
    free(text);
    free(tools);
    free(path);
    return rc;
}

static int steam_default(int argc, char **argv)
{
    const char *tool = "LSIW-proton-stable";
    int existing = 0, dry = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--all") || !strcmp(argv[i], "--existing")) existing = 1;
        else if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-n")) dry = 1;
        else if (argv[i][0] == '-') { lsiw_log("unknown option '%s' (try 'lsiw steam --help')\n", argv[i]); return 2; }
        else tool = argv[i];
    }
    if (!dry && steam_is_running()) {
        lsiw_log("Steam is running and would overwrite the change when it exits: quit Steam "
                 "(Steam > Exit), then run this again\n");
        return 1;
    }
    /* The tools have to exist for Steam to accept the choice. */
    if (lsiw_steam_integrate(1, 0) < 0) return 1;
    char **roots = lsiw_steam_roots();
    int rc = 0;
    for (char **r = roots; r && *r; r++) {
        if (lsiw_steam_root_sandboxed(*r)) continue;
        char *dir = lsiw_cli_printf("%s/compatibilitytools.d/%s", *r, tool);
        printf("%s:\n", *r);
        if (!is_lsiw_tool(dir) && strncmp(tool, "proton", 6))
            lsiw_log("warning: %s is not one of the LSIW tools in %s\n", tool, *r);
        free(dir);
        if (steam_set_default_in(*r, tool, existing, dry) < 0) rc = 1;
    }
    lsiw_strv_free(roots);
    if (!rc && !dry) {
        lsiw_setting_set("steam", "default_tool", tool);
        lsiw_config_save();
        printf("Start Steam again. Every game without a tool of its own%s now runs with %s, in LSIW's "
               "shared prefix.\n", existing ? ", and every game that had Valve's Proton or a tool with an "
               "LSIW twin," : "", tool);
    }
    return rc;
}

/* Every compatdata folder of every Steam library, and where its game runs. */
static int steam_games(void)
{
    char **libs = lsiw_steam_libraries();
    int shared_on = truthy(lsiw_setting_get("steam", "shared_prefix", "true"));
    printf("Shared prefix for Steam games: %s (%s)\n", shared_on ? "on" : "off", lsiw_shared_dir());
    int any = 0;
    for (char **l = libs; l && *l; l++) {
        char *cd = lsiw_cli_printf("%s/steamapps/compatdata", *l);
        struct dirent **ents = NULL;
        int ne = scandir(cd, &ents, NULL, alphasort);
        for (int i = 0; i < ne; i++) {
            const char *id = ents[i]->d_name;
            if (!isdigit((unsigned char)id[0]) || !strcmp(id, "0")) continue;
            char *acf = lsiw_cli_printf("%s/steamapps/appmanifest_%s.acf", *l, id);
            char *t = lsiw_read_text(acf);
            char *name = lsiw_vdf_value(t, "name");
            char *mark = lsiw_cli_printf("%s/%s/" IMPORTED_FILE, cd, id);
            char *pfx = lsiw_cli_printf("%s/%s/pfx", cd, id);
            struct stat st;
            int has_pfx = lstat(pfx, &st) == 0;
            printf("  %-10s %-34.34s %s%s\n", id, name ? name : "?",
                   game_shared(id) ? "shared prefix" : "own prefix",
                   game_shared(id) && has_pfx ? (stat(mark, &st) == 0 ? "; old prefix kept, its saves copied"
                                                                       : "; old prefix kept, saves copied on next start")
                                              : "");
            any = 1;
            free(pfx); free(mark); free(name); free(t); free(acf);
        }
        for (int i = 0; i < ne; i++) free(ents[i]);
        free(ents);
        free(cd);
    }
    if (!any) printf("  no Steam game has run with Proton yet\n");
    lsiw_strv_free(libs);
    return 0;
}

/* lsiw steam own|shared APPID...: a game's own prefix, or the shared one. */
static int steam_game_mode(int argc, char **argv, int own)
{
    if (argc < 3) { lsiw_log("usage: lsiw steam %s APPID...\n", own ? "own" : "shared"); return 2; }
    const char *cur = lsiw_setting_get("steam", "own_prefix", "");
    char *list = lsiw_cli_strdup(cur ? cur : "");
    for (int i = 2; i < argc; i++) {
        const char *id = argv[i];
        int has = in_list(list, id);
        char *nl = NULL;
        if (own && !has) nl = *list ? lsiw_cli_printf("%s,%s", list, id) : lsiw_cli_strdup(id);
        else if (!own && has) {
            nl = lsiw_cli_strdup("");
            char *copy = lsiw_cli_strdup(list), *save = NULL;
            for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
                if (!strcmp(t, id)) continue;
                char *x = *nl ? lsiw_cli_printf("%s,%s", nl, t) : lsiw_cli_strdup(t);
                free(nl);
                nl = x;
            }
            free(copy);
        }
        if (nl) { free(list); list = nl; }
        printf("game %s: %s\n", id, own ? "its own prefix (steamapps/compatdata/<appid>)" : "the shared prefix");
    }
    lsiw_setting_set("steam", "own_prefix", *list ? list : NULL);
    free(list);
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        return 1;
    }
    return 0;
}

const char lsiw_help_steam[] =
    "Usage: lsiw steam [status]\n"
    "       lsiw steam add | remove | refresh\n"
    "       lsiw steam default [TOOL] [--all] [--dry-run]\n"
    "       lsiw steam games\n"
    "       lsiw steam own APPID... | shared APPID...\n"
    "       lsiw steam shared-prefix [on|off]\n"
    "       lsiw steam full-c-drive [on|off]\n"
    "\n"
    "Steam integration. 'add' gives every Proton an \"LSIW\" twin in Steam's list\n"
    "of compatibility tools: each of Valve's versions Steam offers (LSIW Proton\n"
    "Stable, LSIW Proton Experimental, LSIW Proton 11.0-2, ...; Steam installs\n"
    "one the first time its twin is used), GE-Proton and other tools in\n"
    "compatibilitytools.d, and builds downloaded with 'lsiw runners install'.\n"
    "Pick one under a game's Properties > Compatibility. It runs that Proton,\n"
    "with the game's Documents, Saved Games, AppData, Desktop, Downloads,\n"
    "Music, Pictures and Videos folders being the folders chosen in LSIW\n"
    "Control (shared by every game and every engine), and C:\\ showing the\n"
    "whole Linux filesystem.\n"
    "\n"
    "  status        show the Steam installations and the LSIW tools in them\n"
    "  add           add or update the tools (restart Steam afterwards)\n"
    "  remove        remove them again\n"
    "  refresh       update the tools if the integration is on\n"
    "  default       make an LSIW tool (LSIW Proton Stable unless TOOL names\n"
    "                another, e.g. LSIW-proton_experimental) Steam's default for\n"
    "                every game, including games added later; --all also moves\n"
    "                every game set to one of Valve's Protons, or to a tool with\n"
    "                an LSIW twin, to that twin. Steam must not be running; its\n"
    "                config.vdf is backed up first\n"
    "  games         list Steam's games that have run with Proton, and their prefix\n"
    "  own, shared   give games their own prefix again, or the shared one\n"
    "  shared-prefix show or set whether games run in LSIW's shared prefix\n"
    "                (default: on). A game that had a prefix of its own keeps it\n"
    "                untouched as a backup; its saves are copied into LSIW's\n"
    "                folders on its first start\n"
    "  full-c-drive  show or set whether C:\\ in a game's own Proton prefix shows\n"
    "                the whole Linux filesystem (default: on; the shared prefix\n"
    "                always has the C: drive)\n"
    "\n"
    "Every Proton Steam lists gets a twin, Proton 3.7 to Experimental, Hotfix\n"
    "and Next included. With [steam] legacy_versions = false, Proton 6.3 and\n"
    "earlier and Hotfix get one only when installed.\n";

int lsiw_cmd_steam(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "help")) { fputs(lsiw_help_steam, stdout); return 0; }
    if (!strcmp(sub, "status")) return steam_status();
    if (!strcmp(sub, "add")) return lsiw_steam_integrate(1, 1) < 0 ? 1 : 0;
    if (!strcmp(sub, "remove")) return lsiw_steam_integrate(0, 1) < 0 ? 1 : 0;
    if (!strcmp(sub, "refresh")) {
        if (!truthy(lsiw_setting_get("steam", "integration", NULL))) {
            printf("Steam integration is off ('lsiw steam add' turns it on).\n");
            return 0;
        }
        return lsiw_steam_integrate(1, 1) < 0 ? 1 : 0;
    }
    if (!strcmp(sub, "default")) return steam_default(argc, argv);
    if (!strcmp(sub, "games")) return steam_games();
    if (!strcmp(sub, "own")) return steam_game_mode(argc, argv, 1);
    if (!strcmp(sub, "shared")) return steam_game_mode(argc, argv, 0);
    if (!strcmp(sub, "shared-prefix")) {
        if (argc < 3) {
            printf("%s\n", truthy(lsiw_setting_get("steam", "shared_prefix", "true")) ? "on" : "off");
            return 0;
        }
        int on = truthy(argv[2]);
        lsiw_setting_set("steam", "shared_prefix", on ? "true" : "false");
        if (lsiw_config_save() < 0) {
            lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
            return 1;
        }
        printf("Steam games run in %s (from their next start).\n",
               on ? "LSIW's shared prefix" : "their own prefixes");
        return 0;
    }
    if (!strcmp(sub, "full-c-drive")) {
        if (argc < 3) {
            printf("%s\n", truthy(lsiw_setting_get("steam", "full_c_drive", "true")) ? "on" : "off");
            return 0;
        }
        int on = truthy(argv[2]);
        if (!on && strcasecmp(argv[2], "off") && strcasecmp(argv[2], "false") && strcmp(argv[2], "0")) {
            lsiw_log("usage: lsiw steam full-c-drive [on|off]\n");
            return 2;
        }
        lsiw_setting_set("steam", "full_c_drive", on ? "true" : "false");
        if (lsiw_config_save() < 0) {
            lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
            return 1;
        }
        printf("C:\\ in Proton prefixes %s the whole Linux filesystem (from the next start of a game).\n",
               on ? "shows" : "does not show");
        return 0;
    }
    lsiw_log("unknown subcommand 'steam %s' (try 'lsiw steam --help')\n", sub);
    return 2;
}

/* ------------------------------------------------------------------ */
/* Runners: list, install, remove                                       */
/* ------------------------------------------------------------------ */

static void print_runners(int kind, const char *selected_setting)
{
    struct lsiw_runner *v;
    size_t n = lsiw_runners(kind, &v);
    struct lsiw_runner *sel = lsiw_runner_pick(kind, selected_setting);
    printf("%s:\n", kind == LSIW_RUNNER_WINE ? "Wine" : "Proton");
    if (!n) printf("  (none installed)\n");
    for (size_t i = 0; i < n; i++) {
        int is_sel = sel && !strcmp(sel->path, v[i].path);
        printf("  %s %-30s %-26s %s%s\n", is_sel ? "*" : " ", v[i].id, v[i].source, v[i].path,
               v[i].foreign ? "  (built for another processor: cannot run here)" : "");
    }
    lsiw_runner_free(sel);
    lsiw_runners_free(v, n);
}

/* The URL of a release asset: the first "browser_download_url" ending in
 * `suffix` (and starting with `prefix` after the last '/', if given). */
static char *asset_url(const char *json, const char *prefix, const char *suffix, const char *exclude)
{
    const char *key = "\"browser_download_url\"";
    for (const char *p = strstr(json, key); p; p = strstr(p + 1, key)) {
        const char *q = strchr(p + strlen(key), '"');
        if (!q) break;
        const char *e = strchr(q + 1, '"');
        if (!e) break;
        char *url = strndup(q + 1, (size_t)(e - q - 1));
        const char *base = strrchr(url, '/');
        base = base ? base + 1 : url;
        size_t ul = strlen(base), sl = strlen(suffix);
        int ok = ul >= sl && !strcmp(base + ul - sl, suffix) &&
                 (!prefix || !strncmp(base, prefix, strlen(prefix))) &&
                 (!exclude || !strstr(base, exclude));
        if (ok) return url;
        free(url);
    }
    return NULL;
}

/* "x86_64" or "aarch64": the processor GE-Proton archives are named for. */
static const char *host_arch(void)
{
    struct utsname u;
    return uname(&u) == 0 && (!strcmp(u.machine, "aarch64") || !strcmp(u.machine, "arm64"))
        ? "aarch64" : "x86_64";
}

/* An archive name says which processor it is for, or none (older GE). */
static int archive_for_host(const char *base)
{
    int arm = strstr(base, "aarch64") || strstr(base, "arm64");
    return !strcmp(host_arch(), "aarch64") ? arm : !arm;
}

/* The GE-Proton archive of a release for this processor: the one named
 * for it ("GE-Proton11-11-x86_64.tar.gz"), else one named for none. */
static char *ge_asset_url(const char *json)
{
    const char *key = "\"browser_download_url\"";
    char *plain = NULL;
    for (const char *p = strstr(json, key); p; p = strstr(p + 1, key)) {
        const char *q = strchr(p + strlen(key), '"');
        if (!q) break;
        const char *e = strchr(q + 1, '"');
        if (!e) break;
        char *url = strndup(q + 1, (size_t)(e - q - 1));
        const char *base = strrchr(url, '/');
        base = base ? base + 1 : url;
        size_t ul = strlen(base);
        if (ul > 7 && !strcmp(base + ul - 7, ".tar.gz") && archive_for_host(base)) {
            if (strstr(base, host_arch())) { free(plain); return url; }
            if (!plain) { plain = url; continue; }
        }
        free(url);
    }
    return plain;
}

/* "<dir>/X.tar.gz" -> "<dir>/X.sha512sum" */
static char *sum_url_for(const char *url)
{
    size_t n = strlen(url);
    if (n < 7 || strcmp(url + n - 7, ".tar.gz")) return NULL;
    return lsiw_cli_printf("%.*s.sha512sum", (int)(n - 7), url);
}

static char *json_string(const char *json, const char *key)
{
    char *k = lsiw_cli_printf("\"%s\"", key);
    const char *p = strstr(json, k);
    free(k);
    if (!p) return NULL;
    p = strchr(p + strlen(key) + 2, '"');
    if (!p) return NULL;
    const char *e = strchr(p + 1, '"');
    return e ? strndup(p + 1, (size_t)(e - p - 1)) : NULL;
}

static char *fetch(const char *url)
{
    char *argv[] = { "curl", "-fsSL", "--proto", "=https", "-H", "Accept: application/vnd.github+json",
                     (char *)url, NULL };
    int st = -1;
    char *out = lsiw_cli_capture(argv, NULL, &st);
    if (st != 0) { free(out); return NULL; }
    return out;
}

static int download(const char *url, const char *file)
{
    printf("Downloading %s\n", url);
    fflush(stdout);
    char *argv[] = { "curl", "-fL", "--proto", "=https", "--progress-bar", "-o", (char *)file, (char *)url, NULL };
    return lsiw_cli_spawn(argv, NULL, 0) == 0 ? 0 : -1;
}

/* GE-Proton publishes "<file>.sha512sum" next to each archive. */
static int verify_sha512(const char *sum_url, const char *file)
{
    char *sums = fetch(sum_url);
    if (!sums) { lsiw_log("cannot download the checksum %s\n", sum_url); return -1; }
    char *sp = strpbrk(sums, " \t");
    if (sp) *sp = 0;
    char *argv[] = { "sha512sum", (char *)file, NULL };
    int st = -1;
    char *out = lsiw_cli_capture(argv, NULL, &st);
    int ok = st == 0 && out && !strncasecmp(out, sums, strlen(sums)) && strlen(sums) == 128;
    if (!ok) lsiw_log("checksum mismatch for %s: the download is damaged or was altered\n", file);
    else printf("Checksum verified (SHA-512).\n");
    free(out);
    free(sums);
    return ok ? 0 : -1;
}

static int url_exists(const char *url)
{
    char *argv[] = { "curl", "-fsL", "--proto", "=https", "-r", "0-0", "-o", "/dev/null", (char *)url, NULL };
    return lsiw_cli_spawn(argv, NULL, 1) == 0;
}

/* The archive URL of a release, found by trying the names these projects
 * use; NULL if the release has none for this processor. */
static char *release_url(int kind, const char *tag, int staging)
{
    if (kind == LSIW_RUNNER_PROTON) {
        /* GE-Proton11-11-x86_64.tar.gz since 2026; GE-Proton10-x.tar.gz before */
        char *u = lsiw_cli_printf("https://github.com/" GE_REPO "/releases/download/%s/%s-%s.tar.gz",
                                  tag, tag, host_arch());
        if (url_exists(u)) return u;
        free(u);
        if (!strcmp(host_arch(), "aarch64")) return NULL;
        u = lsiw_cli_printf("https://github.com/" GE_REPO "/releases/download/%s/%s.tar.gz", tag, tag);
        if (url_exists(u)) return u;
        free(u);
        return NULL;
    }
    /* Kron4ek's WoW64 build ("-amd64-wow64") needs no 32-bit libraries on
     * the computer and still runs 32-bit programs; the plain "-amd64" build
     * needs the 32-bit (i686) system libraries, and without them wineboot
     * fails ("could not load kernel32.dll"). Older releases have only that. */
    char *u = lsiw_cli_printf("https://github.com/" WINE_REPO "/releases/download/%s/wine-%s%s-amd64-wow64.tar.xz",
                              tag, tag, staging ? "-staging" : "");
    if (url_exists(u)) return u;
    free(u);
    return lsiw_cli_printf("https://github.com/" WINE_REPO "/releases/download/%s/wine-%s%s-amd64.tar.xz",
                           tag, tag, staging ? "-staging" : "");
}

static int cmp_ver_desc(const void *a, const void *b)
{
    return strverscmp(*(char *const *)b, *(char *const *)a);
}

/* Wine releases that cannot start on an ordinary desktop. Wine 11.20 asks
 * Linux for 32 GB of memory at start (an mmap without MAP_NORESERVE) and
 * crashes in wine-preloader when it is refused, which Linux's default
 * overcommit rule does whenever RAM plus swap is below 32 GB. */
static const char *const broken_wine[] = { "11.20", NULL };

static int wine_release_broken(const char *ver)
{
    for (int i = 0; broken_wine[i]; i++)
        if (!strcmp(ver, broken_wine[i])) return 1;
    return 0;
}

/* The newest tag, from `git ls-remote`, whose archive is published:
 * GE-ProtonX-Y for GE, a plain version number (9.21) for Wine; with
 * `below`, the newest Wine release older than that one. Wine releases
 * known not to start are skipped. A tag can exist before its release
 * does, so up to 20 tags are tried. */
static char *latest_tag_below(int kind, int staging, const char *below)
{
    char *url = lsiw_cli_printf("https://github.com/%s", kind == LSIW_RUNNER_PROTON ? GE_REPO : WINE_REPO);
    char *argv[] = { "git", "ls-remote", "--tags", "--refs", url, NULL };
    int st = -1;
    char *out = lsiw_cli_capture(argv, NULL, &st);
    free(url);
    if ((st != 0 || !out) && kind == LSIW_RUNNER_WINE && below) {
        /* No git: Wine releases are numbered X.Y, so try the ones just
         * below `below` by their download address. */
        free(out);
        int major = 0, minor = 0;
        if (sscanf(below, "%d.%d", &major, &minor) != 2) return NULL;
        for (int k = 0; k < 20 && major > 0; k++) {
            if (--minor < 0) { major--; minor = 30; }
            char *t = lsiw_cli_printf("%d.%d", major, minor);
            char *u = wine_release_broken(t) ? NULL : release_url(kind, t, staging);
            if (u && url_exists(u)) { free(u); return t; }
            free(u);
            free(t);
        }
        return NULL;
    }
    if (st != 0 || !out) { free(out); return NULL; }
    char **tags = NULL;
    size_t n = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = strstr(line, "refs/tags/");
        if (!t) continue;
        t += 10;
        int ok;
        if (kind == LSIW_RUNNER_PROTON) ok = !strncmp(t, "GE-Proton", 9) && isdigit((unsigned char)t[9]);
        else {
            ok = isdigit((unsigned char)t[0]);
            for (const char *c = t; *c && ok; c++) ok = isdigit((unsigned char)*c) || *c == '.';
        }
        if (ok && kind == LSIW_RUNNER_WINE && (wine_release_broken(t) || (below && strverscmp(t, below) >= 0)))
            ok = 0;
        if (!ok) continue;
        tags = realloc(tags, (n + 1) * sizeof *tags);
        tags[n++] = lsiw_cli_strdup(t);
    }
    free(out);
    qsort(tags, n, sizeof *tags, cmp_ver_desc);
    char *best = NULL;
    for (size_t i = 0; i < n && i < 20 && !best; i++) {
        char *u = release_url(kind, tags[i], staging);
        if (u && (kind == LSIW_RUNNER_PROTON || url_exists(u))) best = lsiw_cli_strdup(tags[i]);
        free(u);
    }
    for (size_t i = 0; i < n; i++) free(tags[i]);
    free(tags);
    return best;
}

static char *latest_tag(int kind, int staging) { return latest_tag_below(kind, staging, NULL); }

/* A Wine build that runs 32-bit programs with the computer's 32-bit
 * libraries (it has an i386-unix part), as opposed to a WoW64 build. */
static int wine_build_needs_32bit(const char *dir)
{
    char *p = lsiw_cli_printf("%s/lib/wine/i386-unix", dir);
    int r = is_dir(p);
    free(p);
    return r;
}

static int install_runner(int kind, const char *tag, int staging)
{
    if (kind == LSIW_RUNNER_WINE && strcmp(host_arch(), "x86_64")) {
        lsiw_log("the Wine builds LSIW downloads are for x86-64 only; install Wine from your distribution\n");
        return 1;
    }
    const char *repo = kind == LSIW_RUNNER_PROTON ? GE_REPO : WINE_REPO;
    char *api = tag ? lsiw_cli_printf("https://api.github.com/repos/%s/releases/tags/%s", repo, tag)
                    : lsiw_cli_printf("https://api.github.com/repos/%s/releases/latest", repo);
    printf("Looking up %s release of %s...\n", tag ? tag : "the latest", repo);
    fflush(stdout);
    char *json = fetch(api);
    free(api);
    char *rtag = NULL, *url = NULL, *sum_url = NULL;
    if (json && !tag && kind == LSIW_RUNNER_WINE) {
        char *t = json_string(json, "tag_name");
        if (t && wine_release_broken(t)) {
            printf("Wine %s does not start on most computers; using the release before it.\n", t);
            free(json);
            json = NULL;
        }
        free(t);
    }
    if (json) {
        rtag = json_string(json, "tag_name");
        if (kind == LSIW_RUNNER_PROTON) {
            url = ge_asset_url(json);
            sum_url = url ? sum_url_for(url) : NULL;
        } else {
            /* Kron4ek: wine-9.21-amd64.tar.xz, wine-9.21-staging-amd64.tar.xz, ... */
            char *want = staging ? lsiw_cli_printf("wine-%s-staging-amd64-wow64.tar.xz", rtag ? rtag : "")
                                 : lsiw_cli_printf("wine-%s-amd64-wow64.tar.xz", rtag ? rtag : "");
            url = asset_url(json, want, ".tar.xz", NULL);
            free(want);
            if (!url) {
                want = staging ? lsiw_cli_printf("wine-%s-staging-amd64.tar.xz", rtag ? rtag : "")
                               : lsiw_cli_printf("wine-%s-amd64.tar.xz", rtag ? rtag : "");
                url = asset_url(json, want, ".tar.xz", NULL);
                free(want);
            }
        }
    } else {
        /* The GitHub API refused (it allows 60 requests an hour without an
         * account): find the tag with git and use the archive names these
         * projects always publish. */
        rtag = tag ? lsiw_cli_strdup(tag) : latest_tag(kind, staging);
        if (!rtag) {
            lsiw_log("cannot read the release list from GitHub (are curl and git installed and the network up?)\n");
            return 1;
        }
        if (!tag) printf("Using release %s.\n", rtag);
        if (kind == LSIW_RUNNER_WINE && wine_release_broken(rtag))
            lsiw_log("warning: Wine %s crashes at start unless RAM plus swap is 32 GB or more\n", rtag);
        url = release_url(kind, rtag, staging);
        if (kind == LSIW_RUNNER_PROTON && url) sum_url = sum_url_for(url);
    }
    int rc = 1;
    if (!url) {
        lsiw_log("release %s has no %s archive\n", rtag ? rtag : "?",
                 kind == LSIW_RUNNER_PROTON ? "GE-Proton" : staging ? "Wine Staging x86-64" : "Wine x86-64");
        goto out;
    }
    const char *base = strrchr(url, '/') + 1;
    /* GE-Proton is installed under its release name ("GE-Proton11-11"),
     * whatever processor its archive is named for. */
    char *name = kind == LSIW_RUNNER_PROTON && rtag ? lsiw_cli_strdup(rtag) : lsiw_cli_strdup(base);
    char *dot = strstr(name, ".tar.");
    if (dot) *dot = 0;
    /* Kron4ek names x86-64 builds "amd64" (Debian's name for x86-64; they
     * run on Intel and AMD processors alike): install them as x86_64. */
    int wow64 = 0;
    if (kind == LSIW_RUNNER_WINE) {
        /* The WoW64 build is the x86-64 one LSIW installs: wine-11.20-x86_64. */
        size_t nl = strlen(name);
        wow64 = nl > 6 && !strcmp(name + nl - 6, "-wow64");
        if (wow64) name[nl - 6] = 0;
        char *x = lsiw_runner_wine_dir_name(name);
        free(name);
        name = x;
    }
    char *parent = lsiw_cli_printf("%s/%s", lsiw_runners_dir(), kind == LSIW_RUNNER_PROTON ? "proton" : "wine");
    char *dest = lsiw_cli_printf("%s/%s", parent, name);
    char *old = NULL;
    if (is_dir(dest)) {
        if (!wow64 || !wine_build_needs_32bit(dest)) {
            printf("%s is already installed in %s\n", name, dest);
            rc = 0;
            goto out2;
        }
        /* LSIW 1.4.0 and older downloaded the build that needs 32-bit
         * libraries: replace it with the WoW64 build of the same version. */
        printf("Replacing %s with the build that needs no 32-bit libraries...\n", name);
        old = lsiw_cli_printf("%s/.old-%s", parent, name);
        remove_tree(old);
    }
    if (lsiw_mkdirs(parent, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", parent, strerror(errno));
        goto out2;
    }
    char *file = lsiw_cli_printf("%s/.download-%s", parent, base);
    char *tmp = lsiw_cli_printf("%s/.unpack-%s", parent, name);
    remove_tree(tmp);
    if (download(url, file) < 0) {
        lsiw_log("the download failed\n");
        goto out3;
    }
    if (kind == LSIW_RUNNER_PROTON) {
        int bad = sum_url ? verify_sha512(sum_url, file) : 0;
        if (!sum_url) printf("No checksum published for this release; not verified.\n");
        if (bad < 0) goto out3;
    }
    printf("Unpacking...\n");
    fflush(stdout);
    if (lsiw_mkdirs(tmp, 0755) < 0) goto out3;
    char *targv[] = { "tar", "-xf", file, "-C", tmp, NULL };
    if (lsiw_cli_spawn(targv, NULL, 0) != 0) {
        lsiw_log("unpacking %s failed\n", file);
        goto out3;
    }
    /* The archive holds one folder named like the release. */
    char *inner = lsiw_cli_printf("%s/%s", tmp, name);
    if (!is_dir(inner)) {
        DIR *d = opendir(tmp);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            free(inner);
            inner = lsiw_cli_printf("%s/%s", tmp, de->d_name);
            break;
        }
        if (d) closedir(d);
    }
    if (old && rename(dest, old) < 0) {
        lsiw_log("cannot move %s aside: %s\n", dest, strerror(errno));
        free(inner);
        goto out3;
    }
    if (rename(inner, dest) < 0) {
        lsiw_log("cannot move %s to %s: %s\n", inner, dest, strerror(errno));
        if (old) rename(old, dest);
        free(inner);
        goto out3;
    }
    free(inner);
    if (old) remove_tree(old);
    printf("Installed %s in %s\n", name, dest);
    rc = 0;
    if (kind == LSIW_RUNNER_PROTON) steam_refresh_if_enabled();
out3:
    unlink(file);
    remove_tree(tmp);
    free(file);
    free(tmp);
out2:
    free(old);
    free(dest);
    free(parent);
    free(name);
out:
    free(url);
    free(sum_url);
    free(rtag);
    free(json);
    return rc;
}

int lsiw_wine_build_upgrade(const char *wine)
{
    const char *runners = lsiw_runners_dir();
    size_t rl = strlen(runners);
    if (strncmp(wine, runners, rl) || strncmp(wine + rl, "/wine/wine-", 11)) return 0;
    const char *name = wine + rl + 6;                   /* wine-11.20-x86_64/bin/wine */
    const char *slash = strchr(name, '/');
    if (!slash) return 0;
    char *dir = lsiw_cli_printf("%.*s", (int)(slash - wine), wine);
    char *nm = lsiw_cli_printf("%.*s", (int)(slash - name), name);
    int rc = 0;
    size_t nl = strlen(nm);
    if (wine_build_needs_32bit(dir) && nl > 12 && !strcmp(nm + nl - 7, "-x86_64")) {
        nm[nl - 7] = 0;
        int staging = nl - 7 > 13 && !strcmp(nm + nl - 15, "-staging");
        if (staging) nm[nl - 15] = 0;
        const char *ver = nm + 5;
        lsiw_log("%s needs 32-bit system libraries that are missing on many computers "
                 "(wineboot then fails with \"could not load kernel32.dll\"); downloading the WoW64 "
                 "build of Wine %s, which needs none and still runs 32-bit programs\n",
                 dir + strlen(runners) + 6, ver);
        char *u = release_url(LSIW_RUNNER_WINE, ver, staging);
        if (u && strstr(u, "-wow64.")) rc = install_runner(LSIW_RUNNER_WINE, ver, staging) ? -1 : 1;
        else lsiw_log("Wine %s has no WoW64 build; install a newer version with 'lsiw runners install wine'\n", ver);
        free(u);
    }
    free(nm);
    free(dir);
    return rc;
}

/* "…/runners/wine/wine-11.20-x86_64/bin/wine" -> version "11.20" and
 * whether it is Staging, for a Kron4ek build LSIW downloaded. */
static char *lsiw_wine_version_of(const char *wine, int *staging)
{
    const char *runners = lsiw_runners_dir();
    size_t rl = strlen(runners);
    if (strncmp(wine, runners, rl) || strncmp(wine + rl, "/wine/wine-", 11)) return NULL;
    const char *name = wine + rl + 11;
    const char *slash = strchr(name, '/');
    if (!slash) return NULL;
    char *v = lsiw_cli_printf("%.*s", (int)(slash - name), name);
    size_t n = strlen(v);
    if (n > 7 && !strcmp(v + n - 7, "-x86_64")) v[n -= 7] = 0;
    *staging = n > 8 && !strcmp(v + n - 8, "-staging");
    if (*staging) v[n - 8] = 0;
    return v;
}

int lsiw_wine_build_fallback(const char *wine)
{
    int staging;
    char *ver = lsiw_wine_version_of(wine, &staging);
    if (!ver) return 0;
    char *older = latest_tag_below(LSIW_RUNNER_WINE, staging, ver);
    int rc = 0;
    if (!older) lsiw_log("no older Wine %sbuild to try\n", staging ? "Staging " : "");
    else {
        lsiw_log("trying Wine %s instead of Wine %s\n", older, ver);
        if (install_runner(LSIW_RUNNER_WINE, older, staging) == 0) {
            char *id = lsiw_cli_printf("wine-%s%s-x86_64", older, staging ? "-staging" : "");
            lsiw_setting_set("engine", "wine", id);
            lsiw_setting_set("runtime", "wine", NULL);
            if (lsiw_config_save() < 0)
                lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
            else lsiw_log("Wine version set to %s (in %s)\n", id, lsiw_config_path());
            free(id);
            rc = 1;
        }
    }
    free(older);
    free(ver);
    return rc;
}

static int remove_runner(const char *id)
{
    for (int kind = 0; kind < 2; kind++) {
        struct lsiw_runner *v;
        size_t n = lsiw_runners(kind, &v);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(v[i].id, id)) continue;
            if (strcmp(v[i].source, "LSIW")) {
                lsiw_log("%s was not installed by LSIW (%s); remove it with the program that installed it\n",
                         id, v[i].source);
                lsiw_runners_free(v, n);
                return 1;
            }
            char *dir = lsiw_cli_strdup(v[i].path);
            if (kind == LSIW_RUNNER_WINE) {          /* .../<name>/bin/wine -> .../<name> */
                for (int k = 0; k < 2; k++) { char *s = strrchr(dir, '/'); if (s) *s = 0; }
            }
            int rc = remove_tree(dir);
            printf(rc == 0 ? "Removed %s\n" : "Could not remove all of %s\n", dir);
            free(dir);
            lsiw_runners_free(v, n);
            if (kind == LSIW_RUNNER_PROTON) steam_refresh_if_enabled();
            return rc == 0 ? 0 : 1;
        }
        lsiw_runners_free(v, n);
    }
    lsiw_log("no Wine or Proton with the id '%s' ('lsiw runners' lists them)\n", id);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Other Proton builds (lsiw_proton_builds)                             */
/* ------------------------------------------------------------------ */

/* `tmpl` with every "$tag" replaced by `tag`. */
static char *fill_tag(const char *tmpl, const char *tag)
{
    size_t cap = strlen(tmpl) + 4 * strlen(tag) + 1, len = 0;
    char *out = malloc(cap);
    for (const char *p = tmpl; *p; ) {
        if (!strncmp(p, "$tag", 4)) {
            size_t tl = strlen(tag);
            if (len + tl + 1 > cap) { cap = (len + tl + 1) * 2; out = realloc(out, cap); }
            memcpy(out + len, tag, tl);
            len += tl;
            p += 4;
        } else {
            if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[len++] = *p++;
        }
    }
    out[len] = 0;
    return out;
}

/* Whether `s` contains one of the '|'-separated words of `list`. */
static int contains_any(const char *s, const char *list)
{
    char *words = lsiw_cli_strdup(list), *save = NULL;
    int found = 0;
    for (char *w = strtok_r(words, "|", &save); w && !found; w = strtok_r(NULL, "|", &save))
        found = strstr(s, w) != NULL;
    free(words);
    return found;
}

static int build_tag_ok(const struct lsiw_proton_build *b, const char *tag)
{
    if (b->skip && contains_any(tag, b->skip)) return 0;
    if (b->tag_suffix) {
        size_t tl = strlen(tag), sl = strlen(b->tag_suffix);
        if (tl < sl || strcmp(tag + tl - sl, b->tag_suffix)) return 0;
    }
    char *prefixes = lsiw_cli_strdup(b->tag_prefix), *save = NULL;
    int ok = 0;
    for (char *p = strtok_r(prefixes, "|", &save); p && !ok; p = strtok_r(NULL, "|", &save))
        ok = !strncmp(tag, p, strlen(p)) && isdigit((unsigned char)tag[strlen(p)]);
    free(prefixes);
    return ok;
}

/* The release tags of a build, newest first. */
static char **build_tags(const struct lsiw_proton_build *b, size_t *count)
{
    char *url = lsiw_cli_printf("https://github.com/%s", b->repo);
    char *argv[] = { "git", "ls-remote", "--tags", "--refs", url, NULL };
    int st = -1;
    char *out = lsiw_cli_capture(argv, NULL, &st);
    free(url);
    char **tags = NULL;
    size_t n = 0;
    char *save = NULL;
    for (char *line = st == 0 && out ? strtok_r(out, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        char *t = strstr(line, "refs/tags/");
        if (!t || !build_tag_ok(b, t + 10)) continue;
        tags = realloc(tags, (n + 1) * sizeof *tags);
        tags[n++] = lsiw_cli_strdup(t + 10);
    }
    free(out);
    if (n) qsort(tags, n, sizeof *tags, cmp_ver_desc);
    *count = n;
    return tags;
}

/* Unpack `file` (a .tar.* archive holding one folder) as parent/name. */
static int unpack_build(const char *file, const char *parent, const char *name, const char *dest)
{
    char *tmp = lsiw_cli_printf("%s/.unpack-%s", parent, name);
    remove_tree(tmp);
    int rc = -1;
    printf("Unpacking...\n");
    fflush(stdout);
    char *targv[] = { "tar", "-xf", (char *)file, "-C", tmp, NULL };
    if (lsiw_mkdirs(tmp, 0755) < 0 || lsiw_cli_spawn(targv, NULL, 0) != 0) {
        lsiw_log("unpacking %s failed\n", file);
        goto out;
    }
    /* The one folder in the archive (whatever it is called). */
    char *inner = NULL;
    DIR *d = opendir(tmp);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        free(inner);
        inner = lsiw_cli_printf("%s/%s", tmp, de->d_name);
    }
    if (d) closedir(d);
    char *launcher = inner ? lsiw_cli_printf("%s/proton", inner) : NULL;
    if (!launcher || !is_file(launcher)) {
        lsiw_log("%s does not hold a Proton folder\n", file);
    } else if (rename(inner, dest) < 0) {
        lsiw_log("cannot move %s to %s: %s\n", inner, dest, strerror(errno));
    } else {
        rc = 0;
    }
    free(launcher);
    free(inner);
out:
    remove_tree(tmp);
    free(tmp);
    return rc;
}

/* Proton-Tkg: the newest automatic build, a zip holding a .tar archive. */
static int install_nightly(const struct lsiw_proton_build *b, const char *parent)
{
    char *zip = lsiw_cli_printf("%s/.download-%s.zip", parent, b->id);
    char *tmp = lsiw_cli_printf("%s/.unzip-%s", parent, b->id);
    int rc = 1;
    remove_tree(tmp);
    printf("Looking up the latest %s build...\n", b->title);
    if (download(b->nightly, zip) < 0) {
        lsiw_log("the download failed (the %s builds come from GitHub Actions through nightly.link)\n", b->title);
        goto out;
    }
    char *uargv[] = { "unzip", "-q", "-o", zip, "-d", tmp, NULL };
    if (lsiw_mkdirs(tmp, 0755) < 0 || lsiw_cli_spawn(uargv, NULL, 0) != 0) {
        lsiw_log("cannot unpack %s (is unzip installed? sudo dnf install unzip)\n", zip);
        goto out;
    }
    char *inner = NULL;
    DIR *d = opendir(tmp);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (strstr(de->d_name, ".tar")) { free(inner); inner = lsiw_cli_strdup(de->d_name); }
    }
    if (d) closedir(d);
    if (!inner) { lsiw_log("the %s build holds no archive\n", b->title); goto out; }
    char *name = lsiw_cli_strdup(inner);
    char *dot = strstr(name, ".tar");
    if (dot) *dot = 0;
    char *dest = lsiw_cli_printf("%s/%s", parent, name);
    char *file = lsiw_cli_printf("%s/%s", tmp, inner);
    if (is_dir(dest)) {
        printf("%s is already installed in %s\n", name, dest);
        rc = 0;
    } else if (unpack_build(file, parent, name, dest) == 0) {
        printf("Installed %s in %s\n", name, dest);
        rc = 0;
        steam_refresh_if_enabled();
    }
    free(file);
    free(dest);
    free(name);
    free(inner);
out:
    unlink(zip);
    remove_tree(tmp);
    free(zip);
    free(tmp);
    return rc;
}

static int install_build(const struct lsiw_proton_build *b, const char *tag)
{
    if (strcmp(host_arch(), "x86_64")) {
        lsiw_log("%s is built for x86-64 PCs only\n", b->title);
        return 1;
    }
    char *parent = lsiw_cli_printf("%s/proton", lsiw_runners_dir());
    if (lsiw_mkdirs(parent, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", parent, strerror(errno));
        free(parent);
        return 1;
    }
    if (b->nightly) {
        if (tag) lsiw_log("%s has no releases to choose from; installing its latest build\n", b->title);
        int rc = install_nightly(b, parent);
        free(parent);
        return rc;
    }

    /* The release: TAG, or the newest tag whose archive is published (a
     * tag can exist before its archive, or be a build without one). */
    printf("Looking up %s release of %s...\n", tag ? tag : "the latest", b->repo);
    fflush(stdout);
    char *rtag = NULL, *url = NULL;
    size_t n = 0;
    char **tags = NULL;
    if (tag) {
        tags = malloc(sizeof *tags);
        tags[n++] = lsiw_cli_strdup(tag);
    } else {
        tags = build_tags(b, &n);
        if (!n) lsiw_log("cannot read the release list of %s (are git and the network working?)\n", b->repo);
    }
    for (size_t i = 0; i < n && i < 15 && !url; i++) {
        char *asset = fill_tag(b->asset, tags[i]);
        char *u = lsiw_cli_printf("https://github.com/%s/releases/download/%s/%s", b->repo, tags[i], asset);
        free(asset);
        if (url_exists(u)) { url = u; rtag = lsiw_cli_strdup(tags[i]); }
        else free(u);
    }
    for (size_t i = 0; i < n; i++) free(tags[i]);
    free(tags);
    int rc = 1;
    if (!url) {
        if (n) lsiw_log("no %s archive found%s%s\n", b->title, tag ? " for " : "", tag ? tag : "");
        goto out;
    }

    /* Installed as the archive's name without the processor:
     * proton-cachyos-11.0-20261015-slr, dwproton-11.0-15, ... */
    const char *base = strrchr(url, '/') + 1;
    char *name = lsiw_cli_strdup(base);
    char *dot = strstr(name, ".tar.");
    if (dot) *dot = 0;
    size_t nl = strlen(name);
    if (nl > 7 && !strcmp(name + nl - 7, "-x86_64")) name[nl - 7] = 0;
    char *dest = lsiw_cli_printf("%s/%s", parent, name);
    char *file = lsiw_cli_printf("%s/.download-%s", parent, base);
    if (is_dir(dest)) {
        printf("%s is already installed in %s\n", name, dest);
        rc = 0;
    } else if (download(url, file) < 0) {
        lsiw_log("the download failed\n");
    } else {
        char *sum = b->sum ? fill_tag(b->sum, rtag) : NULL;
        char *sum_url = sum ? lsiw_cli_printf("https://github.com/%s/releases/download/%s/%s", b->repo, rtag, sum) : NULL;
        int bad = 0;
        if (sum_url && url_exists(sum_url)) bad = verify_sha512(sum_url, file);
        else printf("No checksum published for this release; not verified.\n");
        if (!bad && unpack_build(file, parent, name, dest) == 0) {
            printf("Installed %s in %s\n", name, dest);
            rc = 0;
            steam_refresh_if_enabled();
        }
        free(sum_url);
        free(sum);
    }
    unlink(file);
    free(file);
    free(dest);
    free(name);
out:
    free(url);
    free(rtag);
    free(parent);
    return rc;
}

const char lsiw_help_runners[] =
    "Usage: lsiw runners [list]\n"
    "       lsiw runners install ge|cachyos|dwproton|rtsp|tkg|em|wineland [TAG]\n"
    "       lsiw runners install wine [TAG] [--staging]\n"
    "       lsiw runners remove ID\n"
    "\n"
    "The Wine and Proton versions LSIW can use. 'list' shows every installed\n"
    "one with its ID, where it comes from and where it is; '*' marks the one\n"
    "'lsiw run --engine=wine' / '--engine=proton' uses (choose with\n"
    "'lsiw config wine ID' and 'lsiw config proton ID', or for one run with\n"
    "'--engine=wine:ID' / '--engine=proton:ID').\n"
    "\n"
    "  install ge    download GE-Proton (the latest, or release TAG such as\n"
    "                GE-Proton9-20) from GitHub, checksum verified\n"
    "  install cachyos | dwproton | rtsp | tkg | em | wineland\n"
    "                download Proton-CachyOS, DW-Proton, Proton-GE RTSP,\n"
    "                Proton-Tkg, Proton-EM or Proton-Wineland the same way\n"
    "                (checksum verified where the project publishes one)\n"
    "  install wine  download a Wine build (Kron4ek/Wine-Builds; TAG such as\n"
    "                9.21; --staging for Wine Staging)\n"
    "  remove ID     delete a version LSIW downloaded\n"
    "\n"
    "Downloads go to " "~/.local/share/lsiw/runners. Wine from $PATH, WineHQ (/opt/wine-*),\n"
    "Lutris and Bottles runners, Steam's Proton versions and Steam's\n"
    "compatibilitytools.d are found automatically; add others with\n"
    "[wine] custom = PATH;... and [proton] custom = FOLDER;... in the\n"
    "configuration file.\n";

int lsiw_cmd_runners(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (!strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "help")) { fputs(lsiw_help_runners, stdout); return 0; }
    if (!strcmp(sub, "list")) {
        const char *w = lsiw_setting_get("engine", "wine", NULL);
        if (!w || !*w) w = lsiw_setting_get("runtime", "wine", NULL);
        print_runners(LSIW_RUNNER_WINE, w);
        print_runners(LSIW_RUNNER_PROTON, lsiw_proton_setting());
        return 0;
    }
    if (!strcmp(sub, "install")) {
        if (argc < 3) { fputs(lsiw_help_runners, stderr); return 2; }
        int kind;
        const struct lsiw_proton_build *b = lsiw_proton_build_find(argv[2]);
        if (b && strcmp(b->id, "ge")) return install_build(b, argc > 3 ? argv[3] : NULL);
        if (b) kind = LSIW_RUNNER_PROTON;
        else if (!strcasecmp(argv[2], "wine")) kind = LSIW_RUNNER_WINE;
        else {
            lsiw_log("install what? wine, ge, cachyos, dwproton, rtsp, tkg, em or wineland\n");
            return 2;
        }
        const char *tag = NULL;
        int staging = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--staging")) staging = 1;
            else tag = argv[i];
        }
        return install_runner(kind, tag, staging);
    }
    if (!strcmp(sub, "remove")) {
        if (argc != 3) { fputs(lsiw_help_runners, stderr); return 2; }
        return remove_runner(argv[2]);
    }
    lsiw_log("unknown subcommand 'runners %s' (try 'lsiw runners --help')\n", sub);
    return 2;
}
