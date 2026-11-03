/* The C: drive location and the shared prefix.
 *
 * C:\ is one folder the user chooses ([drive] c_drive, default
 * $HOME/.cdrive; $HOME/c_drive until 1.7.0, see lsiw_cdrive_migrate()).
 * lsiw_sync_root() fills it with a link to every top-level
 * Linux folder (C:\home, C:\mnt, ...) and to the anchored Windows folders
 * (C:\windows, C:\Program Files, ...); anything a program creates directly
 * under C:\ is a real folder in it. The native engine's path converter, the
 * Wine engine's dosdevices/c: and the shared prefix's drive_c all lead to
 * this one folder.
 *
 * Every Windows program and game, under Wine or under Proton (Steam games
 * included), runs in one shared prefix by default: lsiw_shared_dir(),
 * which has the layout of a Steam compatdata folder (pfx/ is the Wine
 * prefix, Proton keeps its version and tracked_files next to it). A program
 * of the library can have a prefix of its own instead ($LSIW_PREFIX).
 *
 * Nothing here deletes user data: a folder that is in the way is renamed
 * (".old", ".lsiw-backup"), never removed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CDRIVE "${HOME}/.cdrive"
#define OLD_DEFAULT_CDRIVE "${HOME}/c_drive"   /* the default until 1.7.0 */
#define DEFAULT_SHARED "${DATA}/shared"

/* ------------------------------------------------------------------ */
/* Locations                                                            */
/* ------------------------------------------------------------------ */

/* A configured folder: tokens expanded, "~/" accepted, no trailing slash.
 * NULL when the value is not an absolute path. */
static char *folder_value(const char *v)
{
    char *s;
    if (v[0] == '~' && (v[1] == '/' || !v[1])) s = lsiw_strdup_printf("%s%s", lsiw_home_dir(), v + 1);
    else s = lsiw_expand_template(v);
    if (!s || s[0] != '/') { free(s); return NULL; }
    size_t l = strlen(s);
    while (l > 1 && s[l - 1] == '/') s[--l] = 0;
    return s;
}

/* A cached folder setting: recomputed when the configured value changes.
 * Old strings are kept (they are small and callers may still hold them). */
struct cached { char *src, *val; };

static const char *cached_folder(struct cached *c, const char *section, const char *key, const char *def)
{
    const char *v = lsiw_setting_get(section, key, NULL);
    if (!v || !*v || strstr(v, "${ROOT}")) v = def;      /* ${ROOT} would refer to itself */
    if (c->val && c->src && !strcmp(c->src, v)) return c->val;
    char *val = folder_value(v);
    if (!val) val = folder_value(def);
    c->src = strdup(v);
    c->val = val;
    return val;
}

const char *lsiw_cdrive_setting(void)
{
    const char *v = lsiw_setting_get("drive", "c_drive", NULL);
    return v && *v ? v : NULL;
}

const char *lsiw_root_dir(void)
{
    static struct cached c;
    return cached_folder(&c, "drive", "c_drive", DEFAULT_CDRIVE);
}

const char *lsiw_shared_dir(void)
{
    static struct cached c;
    return cached_folder(&c, "prefix", "shared", DEFAULT_SHARED);
}

const char *lsiw_compat_dir(void)
{
    const char *ov = getenv("LSIW_PREFIX");
    if (ov && ov[0] == '/') return ov;
    return lsiw_shared_dir();
}

const char *lsiw_prefix_dir(void)
{
    static char *src, *dir;
    const char *compat = lsiw_compat_dir();
    if (!dir || strcmp(src, compat)) {
        src = strdup(compat);
        dir = lsiw_strdup_printf("%s/pfx", compat);
    }
    return dir;
}

static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) < 0 || stat(b, &sb) < 0) return !strcmp(a, b);
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int lsiw_is_shared_compat(const char *compat)
{
    return compat && same_file(compat, lsiw_shared_dir());
}

/* ------------------------------------------------------------------ */
/* Copying                                                              */
/* ------------------------------------------------------------------ */

static int copy_file(const char *src, const char *dst, const struct stat *st)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st->st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < n; ) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += w;
        }
        if (rc < 0) break;
    }
    if (n < 0) rc = -1;
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    futimens(out, ts);
    if (close(out) < 0) rc = -1;
    close(in);
    if (rc < 0) unlink(dst);       /* only the partial copy we just made */
    return rc;
}

static long copy_tree(const char *src, const char *dst, int verbose, int *errors)
{
    DIR *d = opendir(src);
    if (!d) { (*errors)++; return 0; }
    long n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *from = lsiw_strdup_printf("%s/%s", src, de->d_name);
        char *to = lsiw_strdup_printf("%s/%s", dst, de->d_name);
        struct stat st, dt;
        int have = lstat(to, &dt) == 0;
        if (lstat(from, &st) < 0) (*errors)++;
        else if (S_ISDIR(st.st_mode)) {
            if (!have) {
                if (mkdir(to, st.st_mode & 07777) < 0) { (*errors)++; goto next; }
                n++;
                have = 1;
                lstat(to, &dt);
            }
            if (S_ISDIR(dt.st_mode)) n += copy_tree(from, to, verbose, errors);
        } else if (have) {
            /* An existing name is never overwritten. */
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t l = readlink(from, target, sizeof target - 1);
            if (l < 0) (*errors)++;
            else {
                target[l] = 0;
                if (symlink(target, to) < 0) (*errors)++; else n++;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file(from, to, &st) < 0) {
                lsiw_log("cannot copy %s to %s: %s\n", from, to, strerror(errno));
                (*errors)++;
            } else {
                n++;
                if (verbose) lsiw_log("copied %s\n", to);
            }
        }
    next:
        free(from);
        free(to);
    }
    closedir(d);
    return n;
}

long lsiw_copy_missing(const char *src, const char *dst, int verbose)
{
    struct stat st;
    if (stat(src, &st) < 0 || !S_ISDIR(st.st_mode)) return -1;
    if (lsiw_mkdirs(dst, 0755) < 0) return -1;
    int errors = 0;
    long n = copy_tree(src, dst, verbose, &errors);
    return errors ? -1 : n;
}

/* ------------------------------------------------------------------ */
/* Moving folders safely                                                */
/* ------------------------------------------------------------------ */

static int dir_is_empty(const char *path)
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

/* A name next to `path` that does not exist yet: "<path><suffix>",
 * "<path><suffix>-2", ... */
static char *free_name(const char *path, const char *suffix)
{
    struct stat st;
    char *n = lsiw_strdup_printf("%s%s", path, suffix);
    for (int i = 2; lstat(n, &st) == 0 && i < 1000; i++) {
        free(n);
        n = lsiw_strdup_printf("%s%s-%d", path, suffix, i);
    }
    return n;
}

static void make_parent(const char *path)
{
    char *parent = strdup(path);
    char *sl = strrchr(parent, '/');
    if (sl && sl != parent) { *sl = 0; lsiw_mkdirs(parent, 0755); }
    free(parent);
}

/* Move folder `from` to `to` (which must not exist or be empty). Across
 * disks the files are copied and `from` is kept as "<from>.old". Returns
 * 0, or -1 with a message (nothing was changed then, apart from a partial
 * copy in `to`). */
static int move_folder(const char *from, const char *to, int verbose)
{
    struct stat st;
    if (lstat(to, &st) == 0) {
        if (!S_ISDIR(st.st_mode) || !dir_is_empty(to)) {
            lsiw_log("%s already exists and is not an empty folder\n", to);
            return -1;
        }
        rmdir(to);
    }
    make_parent(to);
    if (rename(from, to) == 0) {
        if (verbose) lsiw_log("moved %s to %s\n", from, to);
        return 0;
    }
    if (errno != EXDEV) {
        lsiw_log("cannot move %s to %s: %s\n", from, to, strerror(errno));
        return -1;
    }
    /* Another disk: copy, then keep the original under another name. */
    if (lsiw_copy_missing(from, to, verbose) < 0) {
        lsiw_log("copying %s to %s failed; the original is unchanged\n", from, to);
        return -1;
    }
    char *old = free_name(from, ".old");
    if (rename(from, old) == 0)
        lsiw_log("copied %s to %s; the original is kept as %s (delete it when everything works)\n",
                 from, to, old);
    else
        lsiw_log("copied %s to %s; the original stays where it was\n", from, to);
    free(old);
    return 0;
}

/* Replace `path` (which must not exist) with a link to `target`. */
static void relink(const char *path, const char *target, int verbose)
{
    struct stat st;
    if (lstat(path, &st) == 0) return;
    if (symlink(target, path) == 0 && verbose) lsiw_log("%s -> %s\n", path, target);
}

/* ------------------------------------------------------------------ */
/* The C: drive                                                         */
/* ------------------------------------------------------------------ */

/* 1 if `name` in the C: folder is a link LSIW makes there and rebuilds
 * by itself (a top-level Linux folder or an anchored Windows folder). */
static int own_link(const char *root, const char *name)
{
    char *path = lsiw_strdup_printf("%s/%s", root, name);
    char target[PATH_MAX];
    ssize_t l = readlink(path, target, sizeof target - 1);
    free(path);
    if (l < 0) return 0;
    target[l] = 0;
    if (target[0] == '/' && !strcmp(target + 1, name)) return 1;
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++)
        if (t[i].anchor && !strcasecmp(t[i].anchor, name)) return 1;
    return 0;
}

/* Remove LSIW's own links from a C: folder that is about to move: they
 * would point the same way from anywhere, and are recreated. */
static void drop_own_links(const char *root)
{
    DIR *d = opendir(root);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.' && (!de->d_name[1] || !strcmp(de->d_name, ".."))) continue;
        if (own_link(root, de->d_name)) {
            char *p = lsiw_strdup_printf("%s/%s", root, de->d_name);
            unlink(p);
            free(p);
        }
    }
    if (d) closedir(d);
}

/* Until 1.7.0 the default C: drive was ~/c_drive; it is ~/.cdrive now.
 * With the default location (no [drive] c_drive setting), an old
 * ~/c_drive folder moves to ~/.cdrive and ~/c_drive becomes a link to it,
 * so Wine prefixes, Proton prefixes, the program library and .lwlnk
 * shortcuts that name ~/c_drive/... keep working. Nothing is deleted: when
 * ~/.cdrive already has files, ~/c_drive is left as it is. */
int lsiw_cdrive_migrate(int verbose)
{
    if (lsiw_cdrive_setting()) return 0;          /* a location the user chose */
    char *old = folder_value(OLD_DEFAULT_CDRIVE);
    char *to = folder_value(DEFAULT_CDRIVE);
    struct stat st;
    int rc = 0;
    if (!old || !to || lstat(old, &st) < 0 || !S_ISDIR(st.st_mode)) goto out;   /* none, or already a link */
    if (lstat(to, &st) == 0 && !(S_ISDIR(st.st_mode) && dir_is_empty(to))) {
        static int told;
        if (!told++)
            lsiw_log("both %s and %s exist; C: is %s and %s is left as it is (move what you need "
                     "from it, then delete it or replace it with a link to %s)\n", old, to, to, old, to);
        goto out;
    }
    drop_own_links(old);
    if (move_folder(old, to, verbose) < 0) { rc = -1; goto out; }
    relink(old, to, verbose);
    lsiw_log("the C: drive is now %s (%s leads there)\n", to, old);
out:
    free(old);
    free(to);
    return rc;
}

static const char *const cdrive_folders[] = {
    "WINDIR", "CSIDL_PROGRAM_FILES", "CSIDL_PROGRAM_FILESX86", "CSIDL_COMMON_APPDATA", NULL
};

int lsiw_cdrive_move(const char *path, int folders, int verbose)
{
    char *to = folder_value(path);
    if (!to) {
        lsiw_log("'%s' is not an absolute Linux path\n", path);
        return -1;
    }
    lsiw_cdrive_migrate(verbose);
    char *from = strdup(lsiw_root_dir());
    char *real_from = realpath(from, NULL);
    size_t fl = strlen(from), tl = strlen(to);
    int rc = -1;
    char *old_loc[8] = { NULL };

    if (strcmp(from, to) && ((!strncmp(to, from, fl) && to[fl] == '/') ||
                             (!strncmp(from, to, tl) && from[tl] == '/'))) {
        lsiw_log("the new C: drive %s cannot be inside the old one (%s) or the other way round\n", to, from);
        goto out;
    }
    if (folders)
        for (int i = 0; cdrive_folders[i]; i++) {
            const char *l = lsiw_var_linux(cdrive_folders[i]);
            old_loc[i] = l ? strdup(l) : NULL;
        }

    struct stat st;
    if (strcmp(from, to)) {
        if (lstat(to, &st) == 0 && !(S_ISDIR(st.st_mode) && dir_is_empty(to)) &&
            !(real_from && same_file(to, real_from))) {
            lsiw_log("%s already exists and is not empty; choose a new or empty folder for C:\n", to);
            goto out;
        }
        /* Going back to a place that is now only a link to the drive (the
         * link an earlier move left there): the link makes way. */
        if (lstat(to, &st) == 0 && S_ISLNK(st.st_mode) && real_from && same_file(to, real_from))
            unlink(to);
        if (lstat(from, &st) == 0 && S_ISLNK(st.st_mode)) {
            /* The old place is only a link (to an earlier location). */
            if (real_from && !same_file(real_from, to)) {
                drop_own_links(real_from);
                if (move_folder(real_from, to, verbose) < 0) goto out;
            }
            unlink(from);
        } else if (lstat(from, &st) == 0 && S_ISDIR(st.st_mode)) {
            drop_own_links(from);
            if (move_folder(from, to, verbose) < 0) goto out;
        } else if (lsiw_mkdirs(to, 0755) < 0) {
            lsiw_log("cannot create %s: %s\n", to, strerror(errno));
            goto out;
        }
        /* Paths that still name the old place keep working. */
        relink(from, to, verbose);
    }

    char *def = folder_value(DEFAULT_CDRIVE);
    lsiw_setting_set("drive", "c_drive", strcmp(def, to) ? path : NULL);
    free(def);

    if (folders) {
        for (int i = 0; cdrive_folders[i]; i++) {
            const struct lsiw_var *v = lsiw_var_find(cdrive_folders[i]);
            char *want = lsiw_strdup_printf("%s/%s", to, v->anchor);
            if (old_loc[i] && strcmp(old_loc[i], want) && lstat(old_loc[i], &st) == 0 &&
                S_ISDIR(st.st_mode)) {
                if (move_folder(old_loc[i], want, verbose) < 0) {
                    lsiw_log("%s stays in %s\n", v->name, old_loc[i]);
                    free(want);
                    continue;
                }
                relink(old_loc[i], want, verbose);
            }
            char *tok = lsiw_strdup_printf("${ROOT}/%s", v->anchor);
            lsiw_var_set(v->name, tok);
            free(tok);
            free(want);
        }
    }
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        goto out;
    }
    if (lsiw_sync_root(verbose) < 0) goto out;
    lsiw_prefix_link_drive(lsiw_shared_dir(), verbose);
    rc = 0;
out:
    for (int i = 0; i < 8; i++) free(old_loc[i]);
    free(real_from);
    free(from);
    free(to);
    return rc;
}

/* ------------------------------------------------------------------ */
/* The shared prefix                                                    */
/* ------------------------------------------------------------------ */

int lsiw_prefix_migrate(int verbose)
{
    const char *shared = lsiw_shared_dir();
    char *pfx = lsiw_strdup_printf("%s/pfx", shared);
    char *old = lsiw_strdup_printf("%s/prefix", lsiw_data_dir());
    struct stat st;
    int rc = 0;
    if (lstat(old, &st) == 0 && S_ISDIR(st.st_mode) && lstat(pfx, &st) < 0) {
        lsiw_mkdirs(shared, 0755);
        if (rename(old, pfx) == 0) {
            lsiw_log("the Wine prefix %s is now the shared prefix %s\n", old, pfx);
            relink(old, pfx, verbose);
        } else {
            lsiw_log("cannot move the Wine prefix %s to %s: %s; a new shared prefix is created and the "
                     "old one is left as it is\n", old, pfx, strerror(errno));
            rc = -1;
        }
    }
    free(old);
    free(pfx);
    return rc;
}

int lsiw_prefix_link_drive(const char *compat, int verbose)
{
    const char *root = lsiw_root_dir();
    char *pfx = lsiw_strdup_printf("%s/pfx", compat);
    char *drive = lsiw_strdup_printf("%s/drive_c", pfx);
    int rc = -1;
    if (lsiw_mkdirs(pfx, 0755) < 0 || lsiw_mkdirs(root, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", pfx, strerror(errno));
        goto out;
    }
    struct stat st;
    char cur[PATH_MAX];
    ssize_t l = readlink(drive, cur, sizeof cur - 1);
    if (l >= 0) {
        cur[l] = 0;
        if (!strcmp(cur, root)) { rc = 0; goto out; }
        unlink(drive);
    } else if (lstat(drive, &st) == 0) {
        if (S_ISDIR(st.st_mode) && dir_is_empty(drive)) rmdir(drive);
        else {
            /* What Wine or Proton put there before: kept, not deleted. */
            char *keep = free_name(drive, ".lsiw-backup");
            if (rename(drive, keep) < 0) {
                lsiw_log("cannot move %s aside: %s\n", drive, strerror(errno));
                free(keep);
                goto out;
            }
            lsiw_log("the prefix's own drive_c is kept as %s; C:\\ is now %s\n", keep, root);
            free(keep);
        }
    }
    if (symlink(root, drive) < 0) {
        lsiw_log("cannot create %s: %s\n", drive, strerror(errno));
        goto out;
    }
    if (verbose) lsiw_log("%s -> %s\n", drive, root);
    rc = 0;
out:
    free(drive);
    free(pfx);
    if (rc == 0) lsiw_prefix_link_drives(compat, verbose);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Extra drives (D:, E:, ...)                                           */
/* ------------------------------------------------------------------ */

/* [drives] d = /home/me/Games makes that folder D:\ for every engine. The
 * key is the lower-case letter; any letter but C can be used. */
static int drive_index(int letter)
{
    if (letter >= 'a' && letter <= 'z') letter -= 'a' - 'A';
    if (letter < 'A' || letter > 'Z' || letter == 'C') return -1;
    return letter - 'A';
}

const char *lsiw_drive_setting(int letter)
{
    int i = drive_index(letter);
    if (i < 0) return NULL;
    char key[2] = { (char)('a' + i), 0 };
    const char *v = lsiw_setting_get("drives", key, NULL);
    return v && *v ? v : NULL;
}

const char *lsiw_drive_path(int letter)
{
    if (letter == 'C' || letter == 'c') return lsiw_root_dir();
    int i = drive_index(letter);
    if (i < 0) return NULL;
    /* Old strings are kept, like cached_folder(): the native engine's
     * threads may still be using them. */
    static struct cached c[26];
    const char *v = lsiw_drive_setting(letter);
    if (!v || strstr(v, "${ROOT}")) return NULL;
    if (c[i].val && c[i].src && !strcmp(c[i].src, v)) return c[i].val;
    char *val = folder_value(v);
    if (!val || !strcmp(val, "/")) { free(val); return NULL; }
    c[i].src = strdup(v);
    c[i].val = val;
    return val;
}

uint32_t lsiw_drive_mask(void)
{
    uint32_t m = 1u << 2;
    for (int l = 'A'; l <= 'Z'; l++)
        if (l != 'C' && lsiw_drive_path(l)) m |= 1u << (l - 'A');
    return m;
}

int lsiw_drive_set(int letter, const char *path)
{
    int i = drive_index(letter);
    if (i < 0) return -1;
    char key[2] = { (char)('a' + i), 0 };
    if (!path || !*path) { lsiw_setting_set("drives", key, NULL); return 0; }
    char *val = folder_value(path);
    if (!val || !strcmp(val, "/")) { free(val); return -2; }
    free(val);
    /* "~/Games" is kept as "${HOME}/Games", which also works for the
     * system-wide configuration. */
    char *store = path[0] == '~' ? lsiw_strdup_printf("${HOME}%s", path + 1) : strdup(path);
    size_t l = strlen(store);
    while (l > 1 && store[l - 1] == '/') store[--l] = 0;
    lsiw_setting_set("drives", key, store);
    free(store);
    return 0;
}

int lsiw_drive_for_linux(const char *abs, size_t *len)
{
    int best = 0;
    size_t bl = 0;
    for (int l = 'A'; l <= 'Z'; l++) {
        if (l == 'C') continue;
        const char *p = lsiw_drive_path(l);
        if (!p) continue;
        size_t pl = strlen(p);
        if (!strncmp(abs, p, pl) && (abs[pl] == 0 || abs[pl] == '/') && pl > bl) { best = l; bl = pl; }
    }
    if (len) *len = bl;
    return best;
}

#define DRIVES_FILE ".lsiw-drives"

int lsiw_prefix_link_drives(const char *compat, int verbose)
{
    char *dd = lsiw_strdup_printf("%s/pfx/dosdevices", compat);
    char *mark = lsiw_strdup_printf("%s/" DRIVES_FILE, compat);
    int rc = 0;
    if (lsiw_mkdirs(dd, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", dd, strerror(errno));
        free(dd); free(mark);
        return -1;
    }
    /* Letters LSIW linked before that are no longer drives. */
    char *old = lsiw_read_text(mark);
    for (char *o = old; o && *o; o++) {
        if (*o < 'a' || *o > 'z' || lsiw_drive_path(*o)) continue;
        char *link = lsiw_strdup_printf("%s/%c:", dd, *o);
        struct stat st;
        if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode) && unlink(link) == 0 && verbose)
            lsiw_log("removed drive %c:\n", *o - 'a' + 'A');
        free(link);
    }
    free(old);
    char made[27];
    size_t n = 0;
    for (int l = 'a'; l <= 'z'; l++) {
        if (l == 'c') continue;
        const char *target = lsiw_drive_path(l);
        if (!target) continue;
        lsiw_mkdirs(target, 0755);
        char *link = lsiw_strdup_printf("%s/%c:", dd, l);
        char cur[PATH_MAX];
        ssize_t cl = readlink(link, cur, sizeof cur - 1);
        struct stat st;
        if (cl >= 0) {
            cur[cl] = 0;
            if (strcmp(cur, target)) unlink(link);
        } else if (lstat(link, &st) == 0) {
            lsiw_log("%s is not a symbolic link; leaving it alone, so %c: is not %s\n", link, l - 'a' + 'A', target);
            rc = -1;
            free(link);
            continue;
        }
        if ((cl < 0 || strcmp(cur, target)) && symlink(target, link) < 0) {
            lsiw_log("cannot create %s: %s\n", link, strerror(errno));
            rc = -1;
        } else {
            made[n++] = (char)l;
            if (verbose && (cl < 0 || strcmp(cur, target))) lsiw_log("drive %c: -> %s\n", l - 'a' + 'A', target);
        }
        /* A raw device entry (d:: -> /dev/sr0) would make Wine treat the
         * folder as that disc. */
        char *raw = lsiw_strdup_printf("%s::", link);
        if (lstat(raw, &st) == 0 && S_ISLNK(st.st_mode)) unlink(raw);
        free(raw);
        free(link);
    }
    made[n] = 0;
    if (n) {
        FILE *f = fopen(mark, "w");
        if (f) { fprintf(f, "%s\n", made); fclose(f); }
    } else unlink(mark);
    free(dd);
    free(mark);
    return rc;
}
