/* The single C: drive.
 *
 * C:\ is one folder the user chooses, lsiw_root_dir() ([drive] c_drive,
 * default $HOME/.cdrive), for every engine. lsiw_sync_root() puts a link to
 * every top-level Linux folder in it, so "C:\home\me\file.txt" is still
 * "/home/me/file.txt" and "C:\mnt\usb" and "C:\run\media\me\USB" are the
 * mounted drives, exactly as Linux sees them. Anything else a program
 * creates under C:\ is a real folder inside the C: drive. Other drive
 * letters do not exist; a path that names one (D:\x) is folded onto C:.
 *
 * Windows itself also expects a few folders right under C:\ (windows,
 * Program Files, ProgramData, users). Those are "anchored" variables: the
 * user decides where they live on Linux, and they appear at their usual
 * Windows names under C:\ (as links, or as real folders when they are
 * inside the C: drive). C:\users is a real folder of the C: drive by
 * default, with C:\users\<you> and C:\users\steamuser (Proton's user)
 * both leading to your profile, $HOME.
 *
 * Windows paths are case-insensitive, so lookups fall back to a
 * case-insensitive match on each component.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct lsiw_var *anchor_named(const char *name)
{
    size_t n; const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++)
        if (t[i].anchor && !strcasecmp(t[i].anchor, name)) return &t[i];
    return NULL;
}

static void set_path(char *path, const char *src)
{
    size_t l = strlen(src);
    if (l >= PATH_MAX) l = PATH_MAX - 1;
    memcpy(path, src, l);
    path[l] = 0;
}

/* Append `comp` to `path` (a buffer of PATH_MAX), matching an existing
 * entry case-insensitively when the exact name does not exist. */
static void append_component(char *path, const char *comp)
{
    size_t pl = strlen(path);
    char cand[PATH_MAX + NAME_MAX + 2];
    snprintf(cand, sizeof cand, "%s%s%s", path, (pl && path[pl - 1] == '/') ? "" : "/", comp);
    struct stat st;
    if (lstat(cand, &st) == 0) { set_path(path, cand); return; }
    DIR *d = opendir(pl ? path : "/");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcasecmp(de->d_name, comp)) {
                snprintf(cand, sizeof cand, "%s%s%s", path,
                         (pl && path[pl - 1] == '/') ? "" : "/", de->d_name);
                break;
            }
        }
        closedir(d);
    }
    set_path(path, cand);
}

static void pop_component(char *path)
{
    char *s = strrchr(path, '/');
    if (!s) return;
    if (s == path) path[1] = 0; else *s = 0;
}

char *lsiw_win_to_linux(const char *winpath)
{
    char *exp = lsiw_expand(winpath);
    char *p = exp;

    /* Convert every separator first. */
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';

    /* Device / NT namespace prefixes: //?/  //./  /??/ */
    if (!strncmp(p, "//?/", 4) || !strncmp(p, "//./", 4) || !strncmp(p, "/?\?/", 4)) {
        p += 4;
        if (!strncasecmp(p, "UNC/", 4)) { p += 2; p[0] = '/'; p[1] = '/'; }
    }

    char out[PATH_MAX];
    int absolute = 0;

    if (p[0] == '/' && p[1] == '/') {
        /* UNC: //server/share/rest -> the GVFS mount of that share. */
        char server[256] = "", share[256] = "";
        char *s = p + 2, *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        snprintf(server, sizeof server, "%.*s", (int)l, s);
        s = e ? e + 1 : s + l;
        e = strchr(s, '/');
        l = e ? (size_t)(e - s) : strlen(s);
        snprintf(share, sizeof share, "%.*s", (int)l, s);
        p = e ? e : s + l;
        for (char *c = server; *c; c++) *c = tolower((unsigned char)*c);
        for (char *c = share; *c; c++) *c = tolower((unsigned char)*c);
        const char *net = lsiw_var_linux("CSIDL_NETWORK");
        snprintf(out, sizeof out, "%s/smb-share:server=%s,share=%s",
                 net ? net : "/run/user", server, share);
        absolute = 2;
    } else {
        if (isalpha((unsigned char)p[0]) && p[1] == ':') {          /* any drive -> C: */
            p += 2;
            /* A bare "C:" is the drive itself, as people mean it (Windows
             * would read it as the current folder of that drive). */
            if (!*p) { p[-1] = '/'; p--; }
        }
        if (*p == '/') {
            /* C:\ is the C: drive folder; before it exists, the Linux root. */
            struct stat st;
            absolute = 1;
            if (stat(lsiw_root_dir(), &st) == 0 && S_ISDIR(st.st_mode)) set_path(out, lsiw_root_dir());
            else strcpy(out, "/");
        }
        else if (!getcwd(out, sizeof out)) strcpy(out, "/");
    }

    /* Resolve "." and ".." lexically first, like GetFullPathName does. */
    char *comps[512];
    int nc = 0;
    char *save = NULL;
    for (char *comp = strtok_r(p, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            if (nc > 0 && strcmp(comps[nc - 1], "..")) nc--;
            else if (!absolute && nc < 512) comps[nc++] = comp;
            continue;
        }
        /* Windows silently ignores trailing dots and spaces in names. */
        size_t cl = strlen(comp);
        while (cl > 1 && (comp[cl - 1] == '.' || comp[cl - 1] == ' ')) comp[--cl] = 0;
        if (nc < 512) comps[nc++] = comp;
    }
    for (int i = 0; i < nc; i++) {
        if (i == 0 && absolute == 1) {
            const struct lsiw_var *a = anchor_named(comps[0]);
            const char *loc = a ? lsiw_var_linux(a->name) : NULL;
            if (loc) { snprintf(out, sizeof out, "%s", loc); continue; }
            /* C:\home, C:\mnt, ...: the link to a top-level Linux folder
             * is followed here, so programs see the same paths as Linux. */
            if (strcmp(out, "/")) {
                append_component(out, comps[0]);
                char target[PATH_MAX];
                ssize_t tl = readlink(out, target, sizeof target - 1);
                if (tl > 1) {
                    target[tl] = 0;
                    const char *name = strrchr(out, '/') + 1;
                    if (target[0] == '/' && !strcmp(target + 1, name)) set_path(out, target);
                }
                continue;
            }
        }
        if (!strcmp(comps[i], "..")) { pop_component(out); continue; }
        append_component(out, comps[i]);
    }
    free(exp);

    return strdup(out);
}

/* ---- the C:\ view used by the Wine engine ---- */

static int ensure_symlink(const char *link, const char *target, int verbose)
{
    char cur[PATH_MAX];
    ssize_t n = readlink(link, cur, sizeof cur - 1);
    if (n >= 0) {
        cur[n] = 0;
        if (!strcmp(cur, target)) return 0;
        unlink(link);
    } else {
        struct stat st;
        if (lstat(link, &st) == 0) {
            /* A real file or directory is in the way; never delete user data. */
            if (verbose) lsiw_log("leaving existing %s in place\n", link);
            return -1;
        }
    }
    if (symlink(target, link) < 0) {
        if (verbose) lsiw_log("symlink %s -> %s: %s\n", link, target, strerror(errno));
        return -1;
    }
    if (verbose) lsiw_log("C:\\ %s -> %s\n", strrchr(link, '/') + 1, target);
    return 0;
}

/* Until 1.3.0 the Windows folders lived under ~/.local/share/lsiw by
 * default; now they default to $HOME/Windows, $HOME/Program Files, ... and
 * the profile is $HOME itself. A folder still at its old default place is
 * moved to the new one once, when nothing is there yet, so installed
 * programs and their data come along. */
static void move_old_default(const char *var, const char *old, int verbose)
{
    if (lsiw_var_override(var)) return;            /* the user chose a place */
    const char *loc = lsiw_var_linux(var);
    struct stat st;
    if (!loc || lstat(old, &st) < 0 || !S_ISDIR(st.st_mode) || lstat(loc, &st) == 0) return;
    char *parent = strdup(loc);
    char *sl = strrchr(parent, '/');
    if (sl && sl != parent) { *sl = 0; lsiw_mkdirs(parent, 0755); }
    free(parent);
    if (rename(old, loc) == 0) {
        if (verbose) lsiw_log("moved %s to %s (the new default location)\n", old, loc);
    } else if (verbose) {
        lsiw_log("%s stays in %s (cannot move it to %s: %s); set %s to keep using it\n",
                 var, old, loc, strerror(errno), var);
    }
}

static void migrate_old_defaults(int verbose)
{
    const char *data = lsiw_data_dir();
    static const struct { const char *var, *old; } moves[] = {
        { "CSIDL_PROGRAM_FILES",    "Program Files" },
        { "CSIDL_PROGRAM_FILESX86", "Program Files (x86)" },
        { "CSIDL_COMMON_APPDATA",   "ProgramData" },
        { "WINDIR",                 "prefix/drive_c/windows" },
    };
    for (size_t i = 0; i < sizeof moves / sizeof moves[0]; i++) {
        char *old = lsiw_strdup_printf("%s/%s", data, moves[i].old);
        move_old_default(moves[i].var, old, verbose);
        free(old);
    }
    /* The old profile's own folders (AppData, Saved Games, ...) go into
     * $HOME; links in it (to Documents, Music, ...) are left behind. */
    if (lsiw_var_override("CSIDL_PROFILE")) return;
    const char *home = lsiw_var_linux("CSIDL_PROFILE");
    char *oldp = lsiw_strdup_printf("%s/users/%s", data, lsiw_user_name());
    DIR *d = home ? opendir(oldp) : NULL;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *from = lsiw_strdup_printf("%s/%s", oldp, de->d_name);
        char *to = lsiw_strdup_printf("%s/%s", home, de->d_name);
        struct stat st;
        if (lstat(from, &st) == 0 && S_ISDIR(st.st_mode) && lstat(to, &st) < 0) {
            if (rename(from, to) == 0 && verbose)
                lsiw_log("moved %s to %s (the new default location)\n", from, to);
        }
        free(from);
        free(to);
    }
    if (d) closedir(d);
    free(oldp);
}

static void migrate_old_root(int verbose);

int lsiw_sync_root(int verbose)
{
    migrate_old_defaults(verbose);
    lsiw_cdrive_migrate(verbose);

    const char *root = lsiw_root_dir();
    if (lsiw_mkdirs(root, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", root, strerror(errno));
        return -1;
    }
    migrate_old_root(verbose);

    size_t n; const struct lsiw_var *t = lsiw_vars(&n);

    /* Create every configured folder that does not exist yet. */
    for (size_t i = 0; i < n; i++) {
        if (t[i].kind != LSIW_VAR_FOLDER) continue;
        const char *loc = lsiw_var_linux(t[i].name);
        if (!loc || !*loc) continue;
        struct stat st;
        if (stat(loc, &st) == 0) continue;
        /* The per-user profile inside PROFILESFOLDER is linked below, not created. */
        if (lsiw_mkdirs(loc, 0755) < 0 && verbose)
            lsiw_log("cannot create %s: %s\n", loc, strerror(errno));
    }

    /* Anchored Windows folders: a link, unless the folder is that place in
     * the C: drive itself (C:\users by default). */
    for (size_t i = 0; i < n; i++) {
        if (!t[i].anchor) continue;
        const char *loc = lsiw_var_linux(t[i].name);
        if (!loc) continue;
        char *link = lsiw_strdup_printf("%s/%s", root, t[i].anchor);
        if (strcmp(link, loc)) ensure_symlink(link, loc, verbose);
        else lsiw_mkdirs(loc, 0755);
        free(link);
    }

    /* One entry per top-level Linux directory. */
    DIR *d = opendir("/");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
                continue;
            if (anchor_named(de->d_name)) continue;
            char *link = lsiw_strdup_printf("%s/%s", root, de->d_name);
            char *target = lsiw_strdup_printf("/%s", de->d_name);
            ensure_symlink(link, target, verbose);
            free(link); free(target);
        }
        closedir(d);
    }

    /* Remove links for top-level directories that are gone. */
    d = opendir(root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char *link = lsiw_strdup_printf("%s/%s", root, de->d_name);
            struct stat st;
            if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode) && !anchor_named(de->d_name)) {
                /* Only LSIW's own links (to "/<name>"): other links in the
                 * C: drive are the user's. */
                char *top = lsiw_strdup_printf("/%s", de->d_name);
                char *cur = NULL;
                char buf[PATH_MAX];
                ssize_t l = readlink(link, buf, sizeof buf - 1);
                if (l >= 0) { buf[l] = 0; cur = buf; }
                if (cur && !strcmp(cur, top) && lstat(top, &st) < 0) unlink(link);
                free(top);
            }
            free(link);
        }
        closedir(d);
    }

    /* Inside the profiles folder, the standard names must lead to the
     * configured profile locations (C:\users\<me>, Public, Default). */
    const char *profiles = lsiw_var_linux("PROFILESFOLDER");
    if (profiles) {
        static const struct { const char *var; const char *name; } prof[] = {
            { "CSIDL_PROFILE", NULL }, { "PUBLIC", "Public" }, { "DEFAULTUSERPROFILE", "Default" },
        };
        for (size_t i = 0; i < sizeof prof / sizeof prof[0]; i++) {
            const char *loc = lsiw_var_linux(prof[i].var);
            if (!loc) continue;
            char *std = lsiw_strdup_printf("%s/%s", profiles, prof[i].name ? prof[i].name : lsiw_user_name());
            if (strcmp(std, loc)) ensure_symlink(std, loc, verbose);
            free(std);
        }
        /* Proton runs every program as "steamuser"; in LSIW's own users
         * folder that is you as well, so games under Proton and programs
         * under Wine share one profile. */
        size_t rl = strlen(root);
        const char *me = lsiw_var_linux("CSIDL_PROFILE");
        if (me && !strncmp(profiles, root, rl) && profiles[rl] == '/' && strcmp(lsiw_user_name(), "steamuser")) {
            char *su = lsiw_strdup_printf("%s/steamuser", profiles);
            ensure_symlink(su, me, verbose);
            free(su);
        }
    }
    return 0;
}

/* Until 1.5 the C:\ view was <data>/root, made only of links. It becomes a
 * link to the C: drive; anything else found in it moves into the drive. */
static void migrate_old_root(int verbose)
{
    char *old = lsiw_strdup_printf("%s/root", lsiw_data_dir());
    const char *root = lsiw_root_dir();
    struct stat st;
    if (strcmp(old, root) && lstat(old, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(old);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char *from = lsiw_strdup_printf("%s/%s", old, de->d_name);
            char *to = lsiw_strdup_printf("%s/%s", root, de->d_name);
            if (lstat(from, &st) == 0) {
                if (S_ISLNK(st.st_mode)) unlink(from);          /* rebuilt in the new drive */
                else if (lstat(to, &st) < 0 && rename(from, to) == 0 && verbose)
                    lsiw_log("moved %s to %s\n", from, to);
            }
            free(from);
            free(to);
        }
        if (d) closedir(d);
        if (rmdir(old) == 0 && symlink(root, old) == 0 && verbose)
            lsiw_log("the old C:\\ view %s now leads to %s\n", old, root);
        else if (verbose && lstat(old, &st) == 0 && S_ISDIR(st.st_mode))
            lsiw_log("%s still holds files; it is left as it is\n", old);
    }
    free(old);
}
