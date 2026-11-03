/* The Wine fallback engine, and process/file helpers for the lsiw command.
 *
 * LSIW runs Windows programs with its own native engine. Wine is used only
 * when the native engine cannot run a program: 32-bit programs, programs
 * that import functions LSIW does not implement yet, .msi installers and
 * batch files. Programs started through Wine still see the same Windows
 * LSIW shows natively, because LSIW's Wine prefix (lsiw_prefix_dir()) is
 * arranged to match it:
 *
 *   - dosdevices/c: points at the C:\ view (lsiw_root_dir()): every
 *     top-level Linux directory plus the anchored Windows folders. Every
 *     other drive letter Wine creates (z: -> / in particular) is removed,
 *     because LSIW has a single C: drive.
 *   - The "User Shell Folders" / "Shell Folders" registry values, the
 *     Program Files locations and HKCU\Environment TEMP/TMP point at the
 *     folders configured in LSIW.
 *   - Wine is told not to take over Linux file associations: opening a
 *     document should not start a Windows program behind the user's back.
 *
 * A stamp file (prefix/.lsiw-stamp) holds a hash of the variable table's
 * Linux and Windows paths; the prefix is refreshed whenever it changes.
 *
 * The prefix is the shared one (lsiw_shared_dir()/pfx), which Proton and
 * Steam games use as well; its drive_c is a link to the C: drive. A program
 * of the library with a prefix of its own ($LSIW_PREFIX) gets a plain Wine
 * prefix there instead, with its user folders linked to LSIW's folders
 * (lsiw_proton_prepare), and LSIW's setup is not applied to it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* pipe2, mkostemp, vasprintf */
#endif
#include "lsiw.h"
#include "wine.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STAMP_FILE   ".lsiw-stamp"
#define REG_FILE     "lsiw-folders.reg"
#define LOCK_FILE    ".wine-setup.lock"
/* Bump when the prefix layout or the registry content written below
 * changes, so existing prefixes are refreshed once.
 * 2: prefixes made by layout 1 have no Windows services registered (see
 *    ensure_windows_dirs); refreshing runs "wineboot -u", which repairs them. */
#define PREFIX_LAYOUT "lsiw-prefix-2"

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static void out_of_memory(void)
{
    fputs("lsiw: out of memory\n", stderr);
    exit(1);
}

char *lsiw_cli_strdup(const char *s)
{
    char *r = strdup(s ? s : "");
    if (!r) out_of_memory();
    return r;
}

char *lsiw_cli_printf(const char *fmt, ...)
{
    va_list ap;
    char *s = NULL;
    va_start(ap, fmt);
    int n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0) out_of_memory();
    return s;
}

int lsiw_cli_has_ext(const char *path, const char *ext)
{
    size_t l = strlen(path), e = strlen(ext);
    return l > e && strcasecmp(path + l - e, ext) == 0 && path[l - e - 1] != '/';
}

static int is_exec_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

char *lsiw_cli_which(const char *name)
{
    if (!name || !*name) return NULL;
    if (strchr(name, '/')) return is_exec_file(name) ? lsiw_cli_strdup(name) : NULL;
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (const char *p = path; ; ) {
        const char *e = strchr(p, ':');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        /* Empty entries mean "current directory"; never look there. */
        if (l > 0) {
            char *cand = lsiw_cli_printf("%.*s/%s", (int)l, p, name);
            if (is_exec_file(cand)) return cand;
            free(cand);
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

char *lsiw_cli_self_path(void)
{
    char *self = lsiw_cli_readlink("/proc/self/exe");
    const char *image = getenv("APPIMAGE"), *dir = getenv("APPDIR");
    size_t n = dir ? strlen(dir) : 0;
    if (self && image && *image == '/' && n > 1 && !strncmp(self, dir, n) && self[n] == '/' &&
        access(image, X_OK) == 0) {
        free(self);
        return lsiw_cli_strdup(image);
    }
    return self;
}

char *lsiw_cli_readlink(const char *path)
{
    for (size_t cap = 256; cap <= (1u << 20); cap *= 2) {
        char *buf = malloc(cap);
        if (!buf) out_of_memory();
        ssize_t n = readlink(path, buf, cap);
        if (n < 0) {
            int e = errno;
            free(buf);
            errno = e;
            return NULL;
        }
        if ((size_t)n < cap) {
            buf[n] = 0;
            return buf;
        }
        free(buf);
    }
    errno = ENAMETOOLONG;
    return NULL;
}

char *lsiw_cli_read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) out_of_memory();
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= (64u << 20)) { free(buf); close(fd); errno = EFBIG; return NULL; }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) out_of_memory();
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(buf);
            close(fd);
            errno = e;
            return NULL;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

int lsiw_cli_write_file(const char *path, const void *data, size_t len, int mode)
{
    char *tmp = lsiw_cli_printf("%s.XXXXXX", path);
    int fd = mkostemp(tmp, O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        free(tmp);
        errno = e;
        return -1;
    }
    const char *p = data;
    size_t left = len;
    int e = 0;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            e = errno;
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (!e && fchmod(fd, (mode_t)mode) < 0) e = errno;
    if (!e && fsync(fd) < 0 && errno != EINVAL && errno != EROFS) e = errno;
    if (close(fd) < 0 && !e) e = errno;
    if (!e && rename(tmp, path) < 0) e = errno;
    if (e) unlink(tmp);
    free(tmp);
    errno = e;
    return e ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Child processes                                                      */
/* ------------------------------------------------------------------ */

/* fork + exec with an error pipe, so "could not start" is told apart from
 * "started and exited with 127". stdout_fd / stderr_fd >= 0 replace the
 * child's stdout / stderr; -1 keeps ours. */
static int start_child(char *const argv[], char *const env[], int stdout_fd,
                       int stderr_fd, pid_t *pid_out)
{
    int ep[2];
    if (pipe2(ep, O_CLOEXEC) < 0) return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(ep[0]);
        close(ep[1]);
        errno = e;
        return -1;
    }
    if (pid == 0) {
        close(ep[0]);
        int ok = 1;
        if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0) ok = 0;
        if (ok && stderr_fd >= 0 && dup2(stderr_fd, STDERR_FILENO) < 0) ok = 0;
        for (char *const *e = env; ok && e && *e; e++) {
            const char *eq = strchr(*e, '=');
            if (!eq) continue;
            char *name = strndup(*e, (size_t)(eq - *e));
            if (!name || setenv(name, eq + 1, 1) < 0) ok = 0;
            free(name);
        }
        if (ok) execvp(argv[0], argv);
        int err = errno;
        ssize_t w = write(ep[1], &err, sizeof err);
        (void)w;
        _exit(127);
    }
    close(ep[1]);
    int err = 0;
    ssize_t n;
    do n = read(ep[0], &err, sizeof err); while (n < 0 && errno == EINTR);
    close(ep[0]);
    if (n == (ssize_t)sizeof err) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) ;
        errno = err;
        return -1;
    }
    *pid_out = pid;
    return 0;
}

static int wait_child(pid_t pid)
{
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

int lsiw_cli_spawn(char *const argv[], char *const env[], int quiet)
{
    int nul = -1;
    if (quiet && (nul = open("/dev/null", O_WRONLY | O_CLOEXEC)) < 0) return -1;
    pid_t pid;
    int rc = start_child(argv, env, nul, -1, &pid);
    int e = errno;
    if (nul >= 0) close(nul);
    if (rc < 0) {
        errno = e;
        return -1;
    }
    return wait_child(pid);
}

char *lsiw_cli_capture(char *const argv[], char *const env[], int *status)
{
    int fds[2];
    int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nul < 0) return NULL;
    if (pipe2(fds, O_CLOEXEC) < 0) {
        close(nul);
        return NULL;
    }
    pid_t pid;
    if (start_child(argv, env, fds[1], nul, &pid) < 0) {
        int e = errno;
        close(fds[0]);
        close(fds[1]);
        close(nul);
        errno = e;
        return NULL;
    }
    close(fds[1]);
    close(nul);
    size_t cap = 1024, n = 0;
    char *buf = malloc(cap);
    if (!buf) out_of_memory();
    for (;;) {
        if (n + 1 >= cap) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb) out_of_memory();
            buf = nb;
        }
        ssize_t r = read(fds[0], buf + n, cap - n - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fds[0]);
    buf[n] = 0;
    int st = wait_child(pid);
    if (status) *status = st;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Finding Wine                                                         */
/* ------------------------------------------------------------------ */

static const char *configured_wine(void)
{
    const char *v = lsiw_setting_get("engine", "wine", NULL);
    if (!v || !*v) v = lsiw_setting_get("runtime", "wine", NULL);   /* older key */
    return (v && *v) ? v : NULL;
}

const char *lsiw_wine_binary(void)
{
    static char *cached, *cached_key;
    static int valid;
    const char *cfg = configured_wine();
    const char *key = cfg ? cfg : "";
    if (valid && !strcmp(cached_key, key)) return cached;

    free(cached);
    free(cached_key);
    cached = NULL;
    cached_key = lsiw_cli_strdup(key);
    valid = 1;

    if (cfg) {
        /* A version from 'lsiw runners' (its id or name), or a path. */
        struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_WINE, cfg);
        if (r) {
            cached = lsiw_cli_strdup(r->path);
            lsiw_runner_free(r);
            return cached;
        }
        char *p = (cfg[0] == '~' && cfg[1] == '/')
            ? lsiw_cli_printf("%s%s", lsiw_home_dir(), cfg + 1) : lsiw_cli_strdup(cfg);
        cached = lsiw_cli_which(p);
        free(p);
        if (cached) return cached;
        lsiw_log("the configured Wine binary '%s' was not found; looking for Wine in $PATH\n", cfg);
    }
    cached = lsiw_cli_which("wine");
    if (!cached) cached = lsiw_cli_which("wine64");
    return cached;
}

char *lsiw_wine_version(void)
{
    const char *wine = lsiw_wine_binary();
    if (!wine) return NULL;
    char *argv[] = { (char *)wine, "--version", NULL };
    char *env[] = { "WINEDEBUG=-all", NULL };
    char *out = lsiw_cli_capture(argv, env, NULL);
    if (!out) return NULL;
    /* Some distribution wrappers print hints before the version; keep the
     * line Wine itself prints ("wine-9.0 ..."). */
    char *res = NULL, *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, "wine-", 5)) { res = lsiw_cli_strdup(line); break; }
    free(out);
    return res;
}

/* ------------------------------------------------------------------ */
/* Prefix state                                                         */
/* ------------------------------------------------------------------ */

static uint64_t hash_str(uint64_t h, const char *s)
{
    /* FNV-1a; a 0xff separator keeps ("ab","c") apart from ("a","bc"). */
    for (; s && *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    h ^= 0xff;
    h *= 1099511628211ULL;
    return h;
}

static char *compute_stamp(void)
{
    uint64_t h = 1469598103934665603ULL;
    h = hash_str(h, PREFIX_LAYOUT);
    h = hash_str(h, LSIW_VERSION);
    h = hash_str(h, lsiw_root_dir());
    /* Another Wine version may need its own wineboot of the shared prefix. */
    h = hash_str(h, lsiw_wine_binary());
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++) {
        h = hash_str(h, t[i].name);
        h = hash_str(h, lsiw_var_linux(t[i].name));
        h = hash_str(h, lsiw_var_windows(t[i].name));
    }
    return lsiw_cli_printf("%016llx\n", (unsigned long long)h);
}

/* Drive letter entries in dosdevices: "x:" (the drive) and "x::" (the raw
 * device behind it). */
static int drive_entry(const char *name)
{
    if (name[0] < 'a' || name[0] > 'z' || name[1] != ':') return 0;
    return name[2] == 0 || (name[2] == ':' && name[3] == 0);
}

/* 1 when C: is the C:\ view and no other drive letter exists. */
static int drives_ok(const char *prefix)
{
    char *c = lsiw_cli_printf("%s/dosdevices/c:", prefix);
    char *target = lsiw_cli_readlink(c);
    int ok = target && !strcmp(target, lsiw_root_dir());
    free(target);
    free(c);
    if (!ok) return 0;
    char *dd = lsiw_cli_printf("%s/dosdevices", prefix);
    DIR *d = opendir(dd);
    free(dd);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (!drive_entry(n) || n[0] == 'c') continue;
        /* Only the extra drives the user chose, each leading to its folder. */
        const char *want = lsiw_drive_path(n[0]);
        if (!want || n[2]) { ok = 0; break; }
        char *link = lsiw_cli_printf("%s/dosdevices/%s", prefix, n);
        char *cur = lsiw_cli_readlink(link);
        free(link);
        int same = cur && !strcmp(cur, want);
        free(cur);
        if (!same) { ok = 0; break; }
    }
    closedir(d);
    /* And every extra drive is there. */
    for (int l = 'a'; ok && l <= 'z'; l++) {
        if (l == 'c' || !lsiw_drive_path(l)) continue;
        char *link = lsiw_cli_printf("%s/dosdevices/%c:", prefix, l);
        struct stat st;
        if (lstat(link, &st) < 0) ok = 0;
        free(link);
    }
    return ok;
}

/* MISSING, STALE (the stamp differs: folders moved, LSIW updated) or
 * READY; *bad_drives tells whether only the drive links need fixing. Wine's
 * mount manager adds letters for removable media at run time, and that
 * alone must not cost a full wineboot. */
static int prefix_check(int *bad_drives)
{
    const char *prefix = lsiw_prefix_dir();
    *bad_drives = 0;
    char *path = lsiw_cli_printf("%s/" STAMP_FILE, prefix);
    char *have = lsiw_cli_read_file(path, NULL);
    free(path);
    if (!have) {
        /* No stamp: never set up by LSIW. The wineserver writes system.reg
         * only when it exits, so a prefix LSIW just made is recognised by
         * its stamp, not by the registry files. */
        struct stat st;
        char *sysreg = lsiw_cli_printf("%s/system.reg", prefix);
        int exists = stat(sysreg, &st) == 0;
        free(sysreg);
        return exists ? LSIW_PREFIX_STALE : LSIW_PREFIX_MISSING;
    }
    char *want = compute_stamp();
    int same = have && !strcmp(have, want);
    free(have);
    free(want);
    *bad_drives = !drives_ok(prefix);
    return same ? LSIW_PREFIX_READY : LSIW_PREFIX_STALE;
}

int lsiw_wine_prefix_state(void)
{
    int bad_drives;
    int state = prefix_check(&bad_drives);
    return state == LSIW_PREFIX_READY && bad_drives ? LSIW_PREFIX_STALE : state;
}

int lsiw_wine_invalidate(void)
{
    char *path = lsiw_cli_printf("%s/" STAMP_FILE, lsiw_prefix_dir());
    int rc = unlink(path);
    int e = errno;
    free(path);
    if (rc < 0 && e != ENOENT) {
        errno = e;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Drives                                                               */
/* ------------------------------------------------------------------ */

/* Point dosdevices/c: at the C:\ view, replacing the link atomically. */
static int set_c_drive(const char *prefix, int verbose)
{
    const char *root = lsiw_root_dir();
    char *dd = lsiw_cli_printf("%s/dosdevices", prefix);
    char *c = lsiw_cli_printf("%s/c:", dd);
    char *tmp = lsiw_cli_printf("%s/.c-lsiw.tmp", dd);
    int rc = -1;

    if (mkdir(dd, 0755) < 0 && errno != EEXIST) {
        lsiw_log("cannot create %s: %s\n", dd, strerror(errno));
        goto out;
    }
    char *cur = lsiw_cli_readlink(c);
    if (cur && !strcmp(cur, root)) {
        free(cur);
        rc = 0;
        goto out;
    }
    struct stat st;
    if (!cur && lstat(c, &st) == 0) {
        /* Something that is not a symlink: never delete what we did not make. */
        lsiw_log("%s is not a symbolic link; leaving it alone, so C: is not the LSIW C:\\ view\n", c);
        goto out;
    }
    free(cur);
    if (unlink(tmp) < 0 && errno != ENOENT) {
        lsiw_log("cannot remove %s: %s\n", tmp, strerror(errno));
        goto out;
    }
    if (symlink(root, tmp) < 0) {
        lsiw_log("cannot create %s: %s\n", tmp, strerror(errno));
        goto out;
    }
    if (rename(tmp, c) < 0) {
        lsiw_log("cannot replace %s: %s\n", c, strerror(errno));
        unlink(tmp);
        goto out;
    }
    if (verbose) lsiw_log("drive C: -> %s\n", root);
    rc = 0;
out:
    free(tmp);
    free(c);
    free(dd);
    return rc;
}

/* Besides C:, only the extra drives the user chose (D: = a folder):
 * remove every other drive letter (Wine's z: -> / and any letters it
 * added for CD-ROMs or mounts), then link the extra drives. The "c::"
 * raw-device entry is kept: Wine reads the volume label and serial number
 * through it. */
static int remove_other_drives(const char *prefix, int verbose)
{
    char *dd = lsiw_cli_printf("%s/dosdevices", prefix);
    DIR *d = opendir(dd);
    if (!d) {
        lsiw_log("cannot open %s: %s\n", dd, strerror(errno));
        free(dd);
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (!drive_entry(n) || n[0] == 'c') continue;
        if (!n[2] && lsiw_drive_path(n[0])) continue;     /* an extra drive: linked below */
        struct stat st;
        if (fstatat(dirfd(d), n, &st, AT_SYMLINK_NOFOLLOW) < 0) continue;
        if (!S_ISLNK(st.st_mode)) {
            lsiw_log("%s/%s is not a symbolic link; leaving it alone\n", dd, n);
            rc = -1;
            continue;
        }
        if (unlinkat(dirfd(d), n, 0) < 0) {
            lsiw_log("cannot remove %s/%s: %s\n", dd, n, strerror(errno));
            rc = -1;
            continue;
        }
        if (verbose && !n[2]) lsiw_log("removed drive %c: (not one of LSIW's drives)\n", toupper((unsigned char)n[0]));
    }
    closedir(d);
    free(dd);
    /* prefix is <compat>/pfx */
    char *compat = lsiw_cli_strdup(prefix);
    char *sl = strrchr(compat, '/');
    if (sl && !strcmp(sl, "/pfx")) {
        *sl = 0;
        if (lsiw_prefix_link_drives(compat, verbose) < 0) rc = -1;
    }
    free(compat);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Registry                                                             */
/* ------------------------------------------------------------------ */

struct sbuf { char *p; size_t len, cap; };

static void sb_add(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > cap) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) out_of_memory();
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_puts(struct sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

/* A .reg string: quotes around it, backslash and quote escaped. */
static void sb_regstr(struct sbuf *b, const char *s)
{
    sb_add(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '\\' || *s == '"') sb_add(b, "\\", 1);
        sb_add(b, s, 1);
    }
    sb_add(b, "\"", 1);
}

static int reg_safe(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s < 0x20) return 0;
    return 1;
}

static void sb_regval(struct sbuf *b, const char *name, const char *data)
{
    if (!data || !reg_safe(data)) return;
    sb_regstr(b, name);
    sb_add(b, "=", 1);
    sb_regstr(b, data);
    sb_add(b, "\n", 1);
}

#define CURVER "Software\\Microsoft\\Windows\\CurrentVersion"

/* Values whose place is not the Explorer shell-folder keys. The Program
 * Files locations live in CurrentVersion, once for 64-bit programs and once
 * in the Wow6432Node view 32-bit programs read (where "ProgramFilesDir" is
 * the x86 folder). TEMP/TMP live in HKCU\Environment. */
static const struct { const char *key, *value, *var; } fixed_values[] = {
    { "HKEY_LOCAL_MACHINE\\" CURVER, "ProgramFilesDir",       "CSIDL_PROGRAM_FILES" },
    { "HKEY_LOCAL_MACHINE\\" CURVER, "ProgramFilesDir (x86)", "CSIDL_PROGRAM_FILESX86" },
    { "HKEY_LOCAL_MACHINE\\" CURVER, "ProgramW6432Dir",       "CSIDL_PROGRAM_FILES" },
    { "HKEY_LOCAL_MACHINE\\" CURVER, "CommonFilesDir",        "CSIDL_PROGRAM_FILES_COMMON" },
    { "HKEY_LOCAL_MACHINE\\" CURVER, "CommonFilesDir (x86)",  "CSIDL_PROGRAM_FILES_COMMONX86" },
    { "HKEY_LOCAL_MACHINE\\" CURVER, "CommonW6432Dir",        "CSIDL_PROGRAM_FILES_COMMON" },
    { "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion", "ProgramFilesDir",       "CSIDL_PROGRAM_FILESX86" },
    { "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion", "ProgramFilesDir (x86)", "CSIDL_PROGRAM_FILESX86" },
    { "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion", "ProgramW6432Dir",       "CSIDL_PROGRAM_FILES" },
    { "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion", "CommonFilesDir",        "CSIDL_PROGRAM_FILES_COMMONX86" },
    { "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion", "CommonFilesDir (x86)",  "CSIDL_PROGRAM_FILES_COMMONX86" },
    { "HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion", "CommonW6432Dir",        "CSIDL_PROGRAM_FILES_COMMON" },
    { "HKEY_CURRENT_USER\\Environment", "TEMP", "TEMP" },
    { "HKEY_CURRENT_USER\\Environment", "TMP",  "TMP" },
};

static int handled_by_fixed(const char *shell_value)
{
    return !strncmp(shell_value, "ProgramFilesDir", 15) || !strncmp(shell_value, "CommonFilesDir", 14);
}

/* Build the .reg text (UTF-8, '\n' line ends). Per-user folders go under
 * HKCU like on Windows; the all-users ones ("Common AppData", "Common
 * Desktop", ...) under HKLM, which is where Windows and Wine read them. */
static char *build_reg(void)
{
    struct sbuf b = {0};
    sb_puts(&b, "Windows Registry Editor Version 5.00\n\n");
    sb_puts(&b, "; Written by LSIW (lsiw wine-setup): the Wine fallback sees the same\n"
                "; folders as LSIW's native engine. Regenerated when folders move.\n\n");

    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    static const char *const roots[] = { "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE" };
    static const char *const keys[] = { "User Shell Folders", "Shell Folders" };
    for (int r = 0; r < 2; r++) {
        int scope = r == 0 ? LSIW_SCOPE_USER : LSIW_SCOPE_SYSTEM;
        for (int k = 0; k < 2; k++) {
            sb_puts(&b, "[");
            sb_puts(&b, roots[r]);
            sb_puts(&b, "\\" CURVER "\\Explorer\\");
            sb_puts(&b, keys[k]);
            sb_puts(&b, "]\n");
            for (size_t i = 0; i < n; i++) {
                if (!t[i].shell_value || t[i].scope != scope || handled_by_fixed(t[i].shell_value))
                    continue;
                sb_regval(&b, t[i].shell_value, lsiw_var_windows(t[i].name));
            }
            sb_puts(&b, "\n");
        }
    }

    const char *last = NULL;
    for (size_t i = 0; i < sizeof fixed_values / sizeof fixed_values[0]; i++) {
        if (!last || strcmp(last, fixed_values[i].key)) {
            if (last) sb_puts(&b, "\n");
            sb_puts(&b, "[");
            sb_puts(&b, fixed_values[i].key);
            sb_puts(&b, "]\n");
            last = fixed_values[i].key;
        }
        sb_regval(&b, fixed_values[i].value, lsiw_var_windows(fixed_values[i].var));
    }

    /* winemenubuilder would otherwise register Windows programs as Linux
     * handlers for document types (.txt, .jpg ...). */
    sb_puts(&b, "\n[HKEY_CURRENT_USER\\Software\\Wine\\FileOpenAssociations]\n");
    sb_regval(&b, "Enable", "N");

    /* Wine's powershell.exe is a stub that runs nothing and exits with 0, so
     * every "if (...) { exit 0 } else { exit 1 }" check an installer makes
     * comes out true. electron-builder installers (Bitwarden, ...) then
     * believe the app is running, cannot "close" it and quit with exit code
     * 2. With the stub disabled, starting powershell.exe fails and those
     * installers use their tasklist/taskkill fallback, which Wine has. */
    sb_puts(&b, "\n[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]\n");
    sb_regval(&b, "powershell.exe", "");
    return b.p;
}

/* UTF-8 -> UTF-16LE with a byte order mark and CRLF line ends: Wine's
 * regedit reads a "Version 5.00" file as Unicode only when it has the BOM,
 * and folder names are not always ASCII. Invalid UTF-8 becomes U+FFFD. */
static char *to_utf16le(const char *s, size_t *outlen)
{
    size_t len = strlen(s);
    char *out = malloc(2 + len * 4 + 4);
    if (!out) out_of_memory();
    size_t o = 0;
    out[o++] = (char)0xff;
    out[o++] = (char)0xfe;
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    while (p < end) {
        uint32_t c = *p++;
        int extra = 0;
        if (c < 0x80) { /* ASCII */ }
        else if (c < 0xc0) c = 0xfffd;
        else if (c < 0xe0) { c &= 0x1f; extra = 1; }
        else if (c < 0xf0) { c &= 0x0f; extra = 2; }
        else if (c < 0xf8) { c &= 0x07; extra = 3; }
        else c = 0xfffd;
        for (int i = 0; i < extra; i++) {
            if (p >= end || (*p & 0xc0) != 0x80) { c = 0xfffd; extra = 0; break; }
            c = (c << 6) | (*p++ & 0x3f);
        }
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
        if (c == '\n') {
            out[o++] = '\r';
            out[o++] = 0;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            uint16_t hi = (uint16_t)(0xd800 | (c >> 10)), lo = (uint16_t)(0xdc00 | (c & 0x3ff));
            out[o++] = (char)(hi & 0xff); out[o++] = (char)(hi >> 8);
            out[o++] = (char)(lo & 0xff); out[o++] = (char)(lo >> 8);
        } else {
            out[o++] = (char)(c & 0xff);
            out[o++] = (char)(c >> 8);
        }
    }
    *outlen = o;
    return out;
}

/* ------------------------------------------------------------------ */
/* Setup                                                                */
/* ------------------------------------------------------------------ */

static void set_wine_pwd(void);

/* ---- Wine Mono ------------------------------------------------------
 * Many installers need the .NET Framework: Squirrel installers (Discord,
 * GitHub Desktop's MSI has a "NETFRAMEWORK45" launch condition), .NET
 * Framework WPF installers (Docker Desktop), VSTO ... A plain Wine prefix
 * gets Wine Mono at its first wineboot; LSIW used to disable that
 * (WINEDLLOVERRIDES=mscoree=) to avoid the download prompt, so its prefix
 * had no .NET at all. LSIW now gets the exact MSI this Wine build expects
 * (from the distribution's wine-mono package, ~/.cache/wine, or a download
 * checked against the SHA-256 in Wine's appwiz.cpl) and installs it with
 * msiexec once wineboot has finished. Letting wineboot install it does not
 * work in a new LSIW prefix: Mono's INSTALLFAKEDLLS action runs a 32-bit
 * helper before syswow64 is complete ("SETUPAPI.dll not found", 1627). */

/* 1: Wine finds an unpacked Mono of a distribution package by itself, so
 * wineboot may install it (mscoree is not disabled). */
static int g_mono_by_wineboot;

/* appwiz.cpl names the Mono MSI it wants ("wine-mono-11.3.0-x86.msi", in
 * UTF-16) and holds its SHA-256 (in ASCII). */
static char *find_appwiz(const char *wine)
{
    char *real = realpath(wine, NULL);
    if (!real) return NULL;
    char *slash = strrchr(real, '/');
    if (slash) *slash = 0;                         /* .../bin */
    static const char *const rel[] = {
        "../lib/wine/i386-windows/appwiz.cpl", "../lib/wine/x86_64-windows/appwiz.cpl",
        "../lib64/wine/i386-windows/appwiz.cpl", "../lib64/wine/x86_64-windows/appwiz.cpl",
        "../lib/i386-linux-gnu/wine/i386-windows/appwiz.cpl",
        "../lib/x86_64-linux-gnu/wine/x86_64-windows/appwiz.cpl",
    };
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        char *p = lsiw_cli_printf("%s/%s", real, rel[i]);
        if (access(p, R_OK) == 0) { free(real); return p; }
        free(p);
    }
    free(real);
    return NULL;
}

static char *utf16_find(const char *buf, size_t len, const char *prefix, const char *suffix)
{
    size_t pl = strlen(prefix);
    for (size_t i = 0; i + pl * 2 < len; i++) {
        size_t j = 0;
        while (j < pl && buf[i + 2 * j] == prefix[j] && buf[i + 2 * j + 1] == 0) j++;
        if (j < pl) continue;
        char name[128];
        size_t n = 0;
        for (size_t o = i; o + 1 < len && n + 1 < sizeof name && buf[o] && !buf[o + 1]; o += 2)
            name[n++] = buf[o];
        name[n] = 0;
        if (n > strlen(suffix) && !strcmp(name + n - strlen(suffix), suffix)) return lsiw_cli_strdup(name);
    }
    return NULL;
}

static char *file_sha256(const char *path)
{
    char *argv[] = { "sha256sum", (char *)path, NULL };
    int st = -1;
    char *out = lsiw_cli_capture(argv, NULL, &st);
    if (st != 0 || !out || strlen(out) < 64) { free(out); return NULL; }
    out[64] = 0;
    return out;
}

static int mono_file_ok(const char *path, const char *appwiz_data, size_t appwiz_len)
{
    if (access(path, R_OK) != 0) return 0;
    char *sum = file_sha256(path);
    int ok = sum && memmem(appwiz_data, appwiz_len, sum, 64) != NULL;
    free(sum);
    return ok;
}

/* A failed Mono download is retried by 'lsiw wine-setup' at any time, and
 * by 'lsiw run' at most once a day, so a computer that is offline does not
 * wait for a download before every program. */
#define MONO_FAILED_FILE ".lsiw-mono-failed"

static void mono_failed_mark(int failed)
{
    char *path = lsiw_cli_printf("%s/" MONO_FAILED_FILE, lsiw_prefix_dir());
    if (failed) lsiw_cli_write_file(path, "", 0, 0644);
    else unlink(path);
    free(path);
}

static int mono_failed_recently(void)
{
    char *path = lsiw_cli_printf("%s/" MONO_FAILED_FILE, lsiw_prefix_dir());
    struct stat st;
    int recent = stat(path, &st) == 0 && time(NULL) - st.st_mtime < 24 * 3600;
    free(path);
    return recent;
}

/* The Mono MSI this Wine wants, downloaded to ~/.cache/wine when needed and
 * allowed ([wine] mono = off disables it); NULL when there is none or when
 * an unpacked copy makes wineboot install it (g_mono_by_wineboot). */
static char *prepare_mono(const char *wine, int verbose, int allow_unpacked, int allow_download)
{
    g_mono_by_wineboot = 0;
    if (!strcasecmp(lsiw_setting_get("wine", "mono", "auto"), "off")) return NULL;
    char *msi = NULL;
    char *appwiz = find_appwiz(wine);
    size_t len = 0;
    char *data = appwiz ? lsiw_cli_read_file(appwiz, &len) : NULL;
    char *name = data ? utf16_find(data, len, "wine-mono-", ".msi") : NULL;
    int ok = 0;
    if (!name) {
        LSIW_TRACE("cannot tell which Wine Mono %s needs; .NET programs will not run under Wine\n", wine);
        goto out;
    }
    const char *xdg = getenv("XDG_CACHE_HOME");
    char *cache = (xdg && xdg[0] == '/') ? lsiw_cli_printf("%s/wine", xdg)
                                         : lsiw_cli_printf("%s/.cache/wine", lsiw_home_dir());
    char *real = realpath(wine, NULL);
    char *bindir = real ? real : lsiw_cli_strdup(wine);
    char *s = strrchr(bindir, '/');
    if (s) *s = 0;
    char *ver = lsiw_cli_strdup(name + strlen("wine-mono-"));
    char *dash = strrchr(ver, '-');                 /* "-x86.msi" */
    if (dash) *dash = 0;
    char *cands[] = {
        lsiw_cli_printf("%s/%s", cache, name),
        lsiw_cli_printf("%s/../share/wine/mono/%s", bindir, name),
        lsiw_cli_printf("/usr/share/wine/mono/%s", name),
        lsiw_cli_printf("/usr/share/wine/mono/wine-mono-%s", ver),   /* unpacked (distribution package) */
        lsiw_cli_printf("%s/../share/wine/mono/wine-mono-%s", bindir, ver),
    };
    for (size_t i = 0; i < sizeof cands / sizeof cands[0] && !ok; i++) {
        struct stat st;
        if (i >= 3) ok = g_mono_by_wineboot = allow_unpacked && stat(cands[i], &st) == 0 && S_ISDIR(st.st_mode);
        else if ((ok = mono_file_ok(cands[i], data, len))) msi = lsiw_cli_strdup(cands[i]);
    }
    if (!ok && allow_download && lsiw_mkdirs(cache, 0755) == 0) {
        static const char *const bases[] = {
            "https://dl.winehq.org/wine/wine-mono/%s/%s",
            "https://github.com/wine-mono/wine-mono/releases/download/wine-mono-%s/%s",
        };
        char *tmp = lsiw_cli_printf("%s/%s.part", cache, name);
        for (size_t i = 0; i < sizeof bases / sizeof bases[0] && !ok; i++) {
            char *url = lsiw_cli_printf(bases[i], ver, name);
            lsiw_log("downloading Wine Mono (.NET Framework for Wine), needed by many installers: %s\n", url);
            char *argv[] = { "curl", "-fsSL", "--proto", "=https", "-o", tmp, url, NULL };
            if (lsiw_cli_spawn(argv, NULL, !verbose) == 0 && mono_file_ok(tmp, data, len)) {
                if ((ok = rename(tmp, cands[0]) == 0)) msi = lsiw_cli_strdup(cands[0]);
            } else {
                unlink(tmp);
            }
            free(url);
        }
        free(tmp);
        if (!ok) mono_failed_mark(1);
        if (!ok) lsiw_log("warning: Wine Mono could not be downloaded; .NET installers (Discord, GitHub Desktop, "
                          "Docker Desktop ...) will fail. Put %s in %s and run 'lsiw wine-setup'\n", name, cache);
    }
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) free(cands[i]);
    free(ver);
    free(bindir);
    free(cache);
out:
    free(name);
    free(data);
    free(appwiz);
    return msi;
}

/* Environment for Wine's own maintenance programs: LSIW's prefix, no
 * Gecko download prompt, no Mono download prompt (Mono is installed by
 * install_mono below), no debug noise unless verbose. */
static char **setup_env(int verbose)
{
    char **env = calloc(4, sizeof *env);
    if (!env) out_of_memory();
    int k = 0;
    env[k++] = lsiw_cli_printf("WINEPREFIX=%s", lsiw_prefix_dir());
    env[k++] = lsiw_cli_strdup(g_mono_by_wineboot ? "WINEDLLOVERRIDES=mshtml=" : "WINEDLLOVERRIDES=mscoree,mshtml=");
    if (!verbose) env[k++] = lsiw_cli_strdup("WINEDEBUG=-all");
    return env;
}

static void free_env(char **env)
{
    for (char **e = env; *e; e++) free(*e);
    free(env);
}

/* Run one of Wine's maintenance commands. Verbose: its output is shown.
 * Otherwise it goes to `log` (prefix/lsiw-setup.log): a fresh prefix makes
 * Wine print harmless warnings, and only a failure is worth showing. */
static int run_wine_tool(char *const argv[], int verbose, int log)
{
    char **env = setup_env(verbose);
    pid_t pid;
    int st = -1;
    if (start_child(argv, env, verbose ? -1 : log, verbose ? -1 : log, &pid) == 0) st = wait_child(pid);
    else lsiw_log("cannot start %s: %s\n", argv[0], strerror(errno));
    free_env(env);
    return st;
}

/* After a failure, show the end of the setup log. */
static void show_log_tail(const char *path)
{
    char *text = lsiw_cli_read_file(path, NULL);
    if (!text) return;
    size_t len = strlen(text);
    int lines = 0;
    char *p = text + len;
    while (p > text && lines <= 10) if (*--p == '\n') lines++;
    if (p > text) p++;
    lsiw_log("last lines of %s:\n%s", path, p);
    if (len && text[len - 1] != '\n') fputc('\n', stderr);
    free(text);
}

static int import_registry(const char *wine, const char *prefix, int verbose, int log)
{
    char *text = build_reg();
    size_t len;
    char *data = to_utf16le(text, &len);
    free(text);
    char *path = lsiw_cli_printf("%s/" REG_FILE, prefix);
    int rc = -1;
    if (lsiw_cli_write_file(path, data, len, 0644) < 0) {
        lsiw_log("cannot write %s: %s\n", path, strerror(errno));
        goto out;
    }
    /* regedit is a Windows program: give it the file's C:\ path. */
    char *win = lsiw_linux_to_win(path);
    char *argv[] = { (char *)wine, "regedit", "/S", win, NULL };
    int st = run_wine_tool(argv, verbose, log);
    if (st != 0) {
        if (st > 0) lsiw_log("importing %s into the Wine registry failed (regedit exit status %d)\n", win, st);
    } else {
        if (verbose) lsiw_log("folder locations written to the Wine registry (%s)\n", path);
        rc = 0;
    }
    free(win);
out:
    free(path);
    free(data);
    return rc;
}

/* Set when wineboot was killed by a signal: Wine itself crashed. */
static int wine_crashed;

static long meminfo_kb(const char *text, const char *key)
{
    const char *p = text ? strstr(text, key) : NULL;
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static char *read_small(const char *path)
{
    char *t = lsiw_cli_read_file(path, NULL);
    if (t) t[strcspn(t, "\n")] = 0;
    return t;
}

/* Wine crashed before it could say why: print what usually causes it. */
static void explain_wine_crash(const char *wine, int st)
{
    lsiw_log("Wine crashed while starting (%s, signal %d): %s\n",
             st == 128 + 11 ? "segmentation fault" : "killed", st - 128, wine);
    char *mem = lsiw_cli_read_file("/proc/meminfo", NULL);
    long ram = meminfo_kb(mem, "MemTotal:"), swap = meminfo_kb(mem, "SwapTotal:");
    free(mem);
    char *oc = read_small("/proc/sys/vm/overcommit_memory");
    char *mmin = read_small("/proc/sys/vm/mmap_min_addr");
    char *se = read_small("/sys/fs/selinux/enforce");
    if (ram >= 0)
        lsiw_log("  memory: %.1f GB RAM + %.1f GB swap; vm.overcommit_memory = %s; vm.mmap_min_addr = %s; "
                 "SELinux %s\n", ram / 1048576.0, swap > 0 ? swap / 1048576.0 : 0.0, oc ? oc : "?",
                 mmin ? mmin : "?", !se ? "off" : !strcmp(se, "1") ? "enforcing" : "permissive");
    if (oc && !strcmp(oc, "2"))
        lsiw_log("  vm.overcommit_memory = 2 (strict) refuses the large address-space reservations Wine "
                 "makes at start; Wine needs 0 or 1\n");
    if (strstr(wine, "/wine-11.20"))
        lsiw_log("  Wine 11.20 asks for 32 GB of memory at start and crashes when Linux refuses, which "
                 "it does when RAM plus swap is below 32 GB\n");
    free(oc);
    free(mmin);
    free(se);
}

/* wineboot starts services.exe with C:\windows\system32 as its working
 * directory before it installs anything (programs/wineboot/wineboot.c,
 * start_services_process, runs before update_wineprefix). In a plain prefix
 * that folder exists by then; C:\windows of the C:\ view is LSIW's own
 * folder (~/Windows), which lsiw_sync_root creates empty. Then services.exe fails to start ("Couldn't start
 * services.exe: error 267"), every service wine.inf adds (RpcSs, MSIServer,
 * Eventlog, Winmgmt, Schedule, MountMgr, PlugPlay ...) is lost, and the
 * prefix stays without services for good. */
static int ensure_windows_dirs(int verbose)
{
    const char *sys32 = lsiw_var_linux("SYSTEM32");
    const char *windir = lsiw_var_linux("WINDIR");
    int rc = 0;
    if (sys32 && lsiw_mkdirs(sys32, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", sys32, strerror(errno));
        rc = -1;
    }
    if (windir) {
        char *wow = lsiw_cli_printf("%s/syswow64", windir);
        if (lsiw_mkdirs(wow, 0755) < 0 && verbose) lsiw_log("cannot create %s: %s\n", wow, strerror(errno));
        free(wow);
    }
    return rc;
}

/* "wine reg query ...": 1 when the key/value exists. Output is discarded. */
static int reg_exists(const char *wine, const char *key, const char *value)
{
    char *argv[] = { (char *)wine, "reg", "query", (char *)key, "/v", (char *)value, NULL };
    int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nul < 0) return 0;
    int st = run_wine_tool(argv, 0, nul);
    close(nul);
    return st == 0;
}

/* 1 when the prefix has Wine's services (RpcSs stands for all of them). */
static int services_ok(const char *wine)
{
    return reg_exists(wine, "HKLM\\System\\CurrentControlSet\\Services\\RpcSs", "ImagePath");
}

/* Install Wine Mono with msiexec unless this prefix has it (Wine Mono
 * registers the .NET Framework 4 keys installers look for). */
#define NDP4_KEY "HKLM\\Software\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full"

static void install_mono(const char *wine, const char *msi, int verbose, int log)
{
    if (reg_exists(wine, NDP4_KEY, "Release")) { mono_failed_mark(0); return; }
    char *win = lsiw_linux_to_win(msi);
    if (!win) return;
    lsiw_log("installing Wine Mono (.NET Framework for Wine programs and installers)\n");
    char *argv[] = { (char *)wine, "msiexec", "/i", win, "/qn", NULL };
    int st = run_wine_tool(argv, verbose, log);
    if (st != 0) {
        lsiw_log("warning: installing Wine Mono failed (msiexec exit status %d); .NET installers will fail\n", st);
        mono_failed_mark(1);
    } else {
        mono_failed_mark(0);
    }
    free(win);
}

/* Wine writes the registry to system.reg when the wineserver exits; LSIW
 * reads that file to check a ready prefix without starting Wine. A key
 * written in the last few seconds may be missing from it, which only costs
 * a "wine reg query" to confirm. */
static int sysreg_has(const char *text, const char *key)
{
    return text && strstr(text, key) != NULL;
}

/* 1 when the prefix lacks something LSIW installs (Wine's services, or
 * Wine Mono when it is wanted). *mono_only: only Mono is missing. */
static int prefix_incomplete(int *mono_only)
{
    char *path = lsiw_cli_printf("%s/system.reg", lsiw_prefix_dir());
    char *text = lsiw_cli_read_file(path, NULL);
    free(path);
    *mono_only = 0;
    if (!text) return 0;
    int services = sysreg_has(text, "[System\\\\ControlSet001\\\\Services\\\\RpcSs]") ||
                   sysreg_has(text, "[System\\\\CurrentControlSet\\\\Services\\\\RpcSs]");
    int mono = sysreg_has(text, "[Software\\\\Microsoft\\\\NET Framework Setup\\\\NDP\\\\v4\\\\Full]");
    free(text);
    if (!strcasecmp(lsiw_setting_get("wine", "mono", "auto"), "off")) mono = 1;
    if (!services) return 1;
    if (!mono) { *mono_only = 1; return 1; }
    return 0;
}

/* Install Wine Mono into a prefix that is otherwise ready. Without
 * download, only an MSI already on this computer is used. */
static int repair_mono(const char *wine, int verbose, int download)
{
    char *msi = download ? NULL : prepare_mono(wine, verbose, 0, 0);
    if (!download && !msi) return -1;
    if (reg_exists(wine, NDP4_KEY, "Release")) { mono_failed_mark(0); free(msi); return 0; }
    lsiw_log("the Wine prefix has no .NET Framework (Wine Mono); installing it\n");
    if (!msi) msi = prepare_mono(wine, verbose, 0, 1);
    if (!msi) return -1;
    int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
    install_mono(wine, msi, verbose, nul);
    if (nul >= 0) close(nul);
    free(msi);
    return 0;
}

static int do_setup_steps(const char *wine, const char *prefix, int state, int verbose,
                          const char *logpath, int *log)
{
    /* The C:\ view must exist before C: can point at it. */
    if (lsiw_sync_root(verbose) < 0) return -1;
    if (ensure_windows_dirs(verbose) < 0) return -1;
    if (lsiw_wine_build_upgrade(wine) < 0) return -1;
    if (lsiw_mkdirs(prefix, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", prefix, strerror(errno));
        return -1;
    }
    /* Make C: the C:\ view before wineboot: Wine only creates its default
     * drive_c and z: links when dosdevices does not exist yet, so wineboot
     * then installs its files straight into the folders LSIW configured
     * (C:\windows, C:\Program Files, the user's profile ...). */
    if (set_c_drive(prefix, verbose) < 0) return -1;
    if (!verbose && (*log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
        lsiw_log("cannot create %s: %s\n", logpath, strerror(errno));
        return -1;
    }
    /* Wine names its start directory after $PWD (see set_wine_pwd). */
    set_wine_pwd();

    if (state == LSIW_PREFIX_MISSING)
        lsiw_log("creating the Wine prefix in %s (first use of the Wine fallback; this can take a minute)\n", prefix);
    else
        lsiw_log("updating the Wine prefix in %s (folder locations or drives changed)\n", prefix);

    char *mono_msi = prepare_mono(wine, verbose, 1, 1);

    /* "-i" alone does not reinstall wine.inf in a prefix whose
     * .update-timestamp is current, so a refresh must force it with "-u":
     * that is what repairs prefixes left without services. */
    const char *mode = state == LSIW_PREFIX_MISSING ? "-i" : "-u";
    char *argv[] = { (char *)wine, "wineboot", (char *)mode, NULL };
    if (verbose) lsiw_log("running: WINEPREFIX=%s %s wineboot %s\n", prefix, wine, mode);
    int st = run_wine_tool(argv, verbose, *log);
    if (st == 0 && !services_ok(wine)) {
        lsiw_log("the Wine prefix has no Windows services; running wineboot -u to install them\n");
        char *again[] = { (char *)wine, "wineboot", "-u", NULL };
        st = run_wine_tool(again, verbose, *log);
        if (st == 0 && !services_ok(wine))
            lsiw_log("warning: Wine's services (RpcSs, MSIServer ...) are still missing; installers may fail\n");
    }
    if (st != 0) {
        if (st > 0) lsiw_log("wineboot failed (exit status %d)\n", st);
        if (st > 128) {
            wine_crashed = 1;
            explain_wine_crash(wine, st);
        }
        free(mono_msi);
        return -1;
    }
    /* An unpacked distribution Mono is installed by wineboot only into a
     * new prefix; anything else gets the MSI. */
    if (!mono_msi && g_mono_by_wineboot && !reg_exists(wine, NDP4_KEY, "Release"))
        mono_msi = prepare_mono(wine, verbose, 0, 1);
    if (mono_msi) {
        install_mono(wine, mono_msi, verbose, *log);
        free(mono_msi);
    }

    /* wineboot (or Wine's first start) may have added drives of its own. */
    if (set_c_drive(prefix, verbose) < 0) return -1;
    int drives = remove_other_drives(prefix, verbose);
    if (import_registry(wine, prefix, verbose, *log) < 0) return -1;
    if (drives < 0) return -1;

    char *stamp = compute_stamp();
    char *path = lsiw_cli_printf("%s/" STAMP_FILE, prefix);
    int rc = lsiw_cli_write_file(path, stamp, strlen(stamp), 0644);
    if (rc < 0) lsiw_log("cannot write %s: %s\n", path, strerror(errno));
    free(path);
    free(stamp);
    if (rc < 0) return -1;
    if (verbose) lsiw_log("Wine prefix ready: %s\n", prefix);
    return 1;
}

static int do_setup(const char *wine, const char *prefix, int state, int verbose)
{
    char *logpath = lsiw_cli_printf("%s/lsiw-setup.log", prefix);
    int log = -1;
    int rc = do_setup_steps(wine, prefix, state, verbose, logpath, &log);
    if (log >= 0) {
        close(log);
        /* Only this run's log: an older one would mislead. */
        if (rc < 0) show_log_tail(logpath);
    }
    free(logpath);
    return rc;
}

int lsiw_wine_setup(int verbose)
{
    const char *wine = lsiw_wine_binary();
    if (!wine) {
        lsiw_log("Wine is not installed (on Fedora: sudo dnf install wine)\n");
        errno = ENOENT;
        return -1;
    }
    if (lsiw_mkdirs(lsiw_data_dir(), 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", lsiw_data_dir(), strerror(errno));
        return -1;
    }

    /* Two programs started at once must not both run wineboot. */
    char *lockpath = lsiw_cli_printf("%s/" LOCK_FILE, lsiw_data_dir());
    int lock = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0) {
        lsiw_log("cannot open %s: %s\n", lockpath, strerror(errno));
        free(lockpath);
        return -1;
    }
    free(lockpath);
    while (flock(lock, LOCK_EX) < 0) {
        if (errno != EINTR) {
            lsiw_log("cannot lock the Wine prefix: %s\n", strerror(errno));
            close(lock);
            return -1;
        }
    }

    /* The shared prefix: the Wine prefix of LSIW 1.5 moves in once, and
     * its drive_c is the C: drive. */
    lsiw_prefix_migrate(verbose);
    const char *prefix = lsiw_prefix_dir();
    if (lsiw_is_shared_compat(lsiw_compat_dir()) || !getenv("LSIW_PREFIX"))
        lsiw_prefix_link_drive(lsiw_compat_dir(), verbose);

    int bad_drives;
    int state = prefix_check(&bad_drives);
    int rc = 0;
    if (state != LSIW_PREFIX_READY) {
        wine_crashed = 0;
        rc = do_setup(wine, prefix, state, verbose);
        /* A Wine build LSIW downloaded that crashes here (a broken
         * release): switch to an older one and try once more. */
        if (rc < 0 && wine_crashed && lsiw_wine_build_fallback(wine) > 0 && (wine = lsiw_wine_binary())) {
            lsiw_log("running wineboot again with %s\n", wine);
            rc = do_setup(wine, prefix, state, verbose);
        }
    } else if (bad_drives) {
        rc = set_c_drive(prefix, verbose) < 0 || remove_other_drives(prefix, verbose) < 0 ? -1 : 1;
    }
    int mono_only;
    if (rc >= 0 && state == LSIW_PREFIX_READY && prefix_incomplete(&mono_only)) {
        /* Ready by its stamp, but a part is missing: a Mono download that
         * failed, or a prefix damaged since. */
        if (!mono_only) {
            if (!services_ok(wine)) {
                lsiw_log("the Wine prefix has no Windows services; repairing it\n");
                wine_crashed = 0;
                rc = do_setup(wine, prefix, LSIW_PREFIX_STALE, verbose);
            }
        } else {
            repair_mono(wine, verbose, !mono_failed_recently());
        }
    } else if (state == LSIW_PREFIX_READY && !bad_drives && verbose) {
        lsiw_log("Wine prefix is up to date: %s\n", prefix);
    }
    close(lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Running a program                                                    */
/* ------------------------------------------------------------------ */

static char *current_dir(void)
{
    for (size_t cap = 256; cap <= (1u << 20); cap *= 2) {
        char *buf = malloc(cap);
        if (!buf) out_of_memory();
        if (getcwd(buf, cap)) return buf;
        free(buf);
        if (errno != ERANGE) return NULL;
    }
    return NULL;
}

/* Wine names the starting directory after $PWD when $PWD is the same
 * directory as the real working directory (so symlinked paths are kept).
 * With C: being the C:\ view, the real directory (/home/me) is outside
 * every Wine drive and the program would start in C:\windows. Pointing $PWD
 * at the same directory seen through the view (<root>/home/me) makes Wine
 * start the program in C:\home\me, as the native engine does. */
static void set_wine_pwd(void)
{
    char *cwd = current_dir();
    if (!cwd) return;
    const char *root = lsiw_root_dir();
    char *win = lsiw_linux_to_win(cwd);
    if (!win || strncmp(win, "C:\\", 3)) {
        free(win);
        free(cwd);
        return;
    }
    char *view = lsiw_cli_printf("%s/%s", root, win + 3);
    for (char *p = view + strlen(root); *p; p++) if (*p == '\\') *p = '/';
    size_t vl = strlen(view);
    while (vl > 1 && view[vl - 1] == '/') view[--vl] = 0;

    struct stat a, b;
    if (!win[3]) {
        /* "/" itself is C:\; only the view directory can stand for it. */
        if (chdir(root) == 0) setenv("PWD", root, 1);
    } else if (stat(view, &a) == 0 && stat(cwd, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
        setenv("PWD", view, 1);
    } else {
        LSIW_TRACE("%s is not reachable through the C:\\ view; Wine may start in C:\\windows\n", cwd);
    }
    free(view);
    free(win);
    free(cwd);
}

static int is_msi(const char *path)
{
    return lsiw_cli_has_ext(path, ".msi") || lsiw_cli_has_ext(path, ".msp");
}

/* 1/0 from an environment variable LSIW sets for a program of the
 * library (LSIW_DXVK, LSIW_VKD3D); -1 when unset. */
static int env_flag(const char *name)
{
    const char *v = getenv(name);
    if (!v || !*v) return -1;
    return strcmp(v, "0") != 0;
}

/* The DLLs of DXVK (or VKD3D-Proton) that an installed Proton ships, for
 * 64-bit (bits 64) or 32-bit programs. Caller frees; NULL if none. */
static char *proton_d3d_dir(const char *what, int bits)
{
    static const char *const layouts64[] = { "files/lib/wine/%s/x86_64-windows", "files/lib64/wine/%s", NULL };
    static const char *const layouts32[] = { "files/lib/wine/%s/i386-windows", "files/lib/wine/%s", NULL };
    struct lsiw_runner *v;
    size_t n = lsiw_runners(LSIW_RUNNER_PROTON, &v);
    char *found = NULL;
    for (size_t i = 0; i < n && !found; i++) {
        if (v[i].foreign) continue;
        const char *const *l = bits == 64 ? layouts64 : layouts32;
        for (int k = 0; l[k] && !found; k++) {
            char *sub = lsiw_cli_printf(l[k], what);
            char *dir = lsiw_cli_printf("%s/%s", v[i].path, sub);
            char *probe = lsiw_cli_printf("%s/%s", dir, strcmp(what, "dxvk") ? "d3d12.dll" : "d3d11.dll");
            struct stat st;
            if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) found = dir; else free(dir);
            free(probe);
            free(sub);
        }
    }
    lsiw_runners_free(v, n);
    return found;
}

/* Copy every DLL of `from` into `to` where it is missing or differs in
 * size. Returns the DLL names as "a,b,c" (caller frees), NULL if none. */
static char *install_dlls(const char *from, const char *to)
{
    DIR *d = opendir(from);
    if (!d) return NULL;
    lsiw_mkdirs(to, 0755);
    char *names = NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!lsiw_cli_has_ext(de->d_name, ".dll")) continue;
        char *src = lsiw_cli_printf("%s/%s", from, de->d_name);
        char *dst = lsiw_cli_printf("%s/%s", to, de->d_name);
        struct stat a, b;
        if (stat(src, &a) == 0 && (stat(dst, &b) < 0 || a.st_size != b.st_size)) {
            size_t len;
            char *data = lsiw_cli_read_file(src, &len);
            if (data) lsiw_cli_write_file(dst, data, len, 0644);
            free(data);
        }
        char *base = lsiw_cli_strdup(de->d_name);
        base[strlen(base) - 4] = 0;
        char *nn = names ? lsiw_cli_printf("%s,%s", names, base) : lsiw_cli_strdup(base);
        free(names);
        free(base);
        names = nn;
        free(src);
        free(dst);
    }
    closedir(d);
    return names;
}

static void add_overrides(const char *dlls, const char *mode)
{
    if (!dlls || !*dlls) return;
    const char *cur = getenv("WINEDLLOVERRIDES");
    char *v = cur && *cur ? lsiw_cli_printf("%s;%s=%s", cur, dlls, mode) : lsiw_cli_printf("%s=%s", dlls, mode);
    setenv("WINEDLLOVERRIDES", v, 1);
    free(v);
}

/* DXVK and VKD3D-Proton under Wine, for programs of the library that turn
 * them on: the DLLs come from an installed Proton and are put into the
 * prefix's system32/syswow64; WINEDLLOVERRIDES decides per run whether
 * they are used, so programs that leave them off keep Wine's own
 * Direct3D (WineD3D) in the same shared prefix. */
static void wine_d3d(const char *prefix, int verbose)
{
    static const char *const what[] = { "dxvk", "vkd3d-proton" };
    static const char *const flag[] = { "LSIW_DXVK", "LSIW_VKD3D" };
    int shared = !getenv("LSIW_PREFIX") || lsiw_is_shared_compat(lsiw_compat_dir());
    const char *sys = shared ? lsiw_var_linux("SYSTEM32") : NULL;
    const char *win = shared ? lsiw_var_linux("WINDIR") : NULL;
    char *sys64 = sys ? lsiw_cli_strdup(sys) : lsiw_cli_printf("%s/drive_c/windows/system32", prefix);
    char *sys32 = win ? lsiw_cli_printf("%s/syswow64", win) : lsiw_cli_printf("%s/drive_c/windows/syswow64", prefix);
    for (int k = 0; k < 2; k++) {
        int on = env_flag(flag[k]);
        char *marker = lsiw_cli_printf("%s/.lsiw-%s", prefix, what[k]);
        char *dlls = NULL;
        if (on == 1) {
            char *d64 = proton_d3d_dir(what[k], 64), *d32 = proton_d3d_dir(what[k], 32);
            if (!d64) lsiw_log("warning: %s was asked for, but no installed Proton has it; using WineD3D\n", what[k]);
            else {
                dlls = install_dlls(d64, sys64);
                if (d32 && strcmp(d32, d64)) free(install_dlls(d32, sys32));
                if (dlls) lsiw_cli_write_file(marker, dlls, strlen(dlls), 0644);
                if (verbose) lsiw_log("%s from %s: %s\n", what[k], d64, dlls ? dlls : "(none)");
                add_overrides(dlls, "n,b");
            }
            free(d64);
            free(d32);
        } else {
            /* Installed for another program: Wine's own DLLs for this one. */
            dlls = lsiw_cli_read_file(marker, NULL);
            add_overrides(dlls, "b");
        }
        free(dlls);
        free(marker);
    }
    free(sys64);
    free(sys32);
}

int lsiw_wine_run(const char *linux_path, int argc, char **argv, int verbose)
{
    const char *wine = lsiw_wine_binary();
    if (!wine) {
        lsiw_log("Wine is not installed, so the Wine fallback is not available (on Fedora: sudo dnf install wine)\n");
        errno = ENOENT;
        return -1;
    }
    const char *own = getenv("LSIW_PREFIX");
    if (own && *own && !lsiw_is_shared_compat(lsiw_compat_dir())) {
        /* A program with a prefix of its own: plain Wine, LSIW's folders. */
        if (lsiw_proton_prepare(lsiw_compat_dir(), verbose, NULL) < 0)
            lsiw_log("warning: some folders of the prefix could not be linked to LSIW's folders\n");
    } else if (lsiw_wine_setup(verbose) < 0)
        lsiw_log("warning: the Wine prefix could not be prepared; trying to run the program anyway\n");
    wine = lsiw_wine_binary();      /* setup may have switched to another Wine build */
    wine_d3d(lsiw_prefix_dir(), verbose);

    char *win = lsiw_linux_to_win(linux_path);
    if (!win) out_of_memory();
    char **wargv = calloc((size_t)(argc > 0 ? argc : 1) + 5, sizeof *wargv);
    if (!wargv) out_of_memory();
    int k = 0;
    wargv[k++] = (char *)wine;
    /* Wine starts .exe files itself; installers and batch files need the
     * Windows program that opens them, as Explorer would use. */
    if (is_msi(linux_path)) {
        wargv[k++] = "msiexec";
        wargv[k++] = lsiw_cli_has_ext(linux_path, ".msp") ? "/p" : "/i";
    } else if (lsiw_cli_has_ext(linux_path, ".bat") || lsiw_cli_has_ext(linux_path, ".cmd")) {
        wargv[k++] = "cmd";
        wargv[k++] = "/c";
    }
    wargv[k++] = win;
    /* Like the native engine ([runtime] translate_args): an argument that is
     * an existing absolute Linux path is handed over as its C:\ path, since
     * Windows programs read a leading '/' as an option. */
    int translate = strcasecmp(lsiw_setting_get("runtime", "translate_args", "true"), "false") != 0;
    int first_arg = k;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        const char *a = argv[i];
        char *conv = NULL;
        if (translate && a[0] == '/' && a[1] && stat(a, &st) == 0) conv = lsiw_linux_to_win(a);
        wargv[k++] = conv ? conv : lsiw_cli_strdup(a);
    }
    wargv[k] = NULL;

    /* Wine's debug channels stay quiet unless -v; a WINEDEBUG the user set
     * explicitly is respected. */
    if (setenv("WINEPREFIX", lsiw_prefix_dir(), 1) < 0 ||
        (!verbose && setenv("WINEDEBUG", "-all", 0) < 0)) {
        lsiw_log("cannot set up Wine's environment: %s\n", strerror(errno));
        for (int i = first_arg; wargv[i]; i++) free(wargv[i]);
        free(wargv);
        free(win);
        return -1;
    }
    set_wine_pwd();
    if (verbose) {
        lsiw_log("running with Wine: WINEPREFIX=%s", lsiw_prefix_dir());
        for (int i = 0; wargv[i]; i++) fprintf(stderr, " %s", wargv[i]);
        fputc('\n', stderr);
    }
    fflush(NULL);
    /* exec, not fork: the program's exit code becomes ours. */
    execv(wine, wargv);
    int e = errno;
    lsiw_log("cannot start %s: %s\n", wine, strerror(e));
    for (int i = first_arg; wargv[i]; i++) free(wargv[i]);
    free(wargv);
    free(win);
    errno = e;
    return -1;
}
