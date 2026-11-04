/* LSIW command-line program: each program shows on the desktop as itself.
 *
 * Without this, every program LSIW starts looks like one application to the
 * desktop:
 *  - Proton names every window "steam_app_$SteamAppId", and LSIW ran every
 *    program as app 0, so a taskbar grouped all of them into one entry with
 *    one icon (steam_app_0);
 *  - they ran in the cgroup of whatever started lsiw (LSIW Control's
 *    app-...scope), so System Monitor's Applications list counted them as
 *    LSIW Control and did not list them at all.
 *
 * Before a program starts, lsiw_cli_identity() gives it:
 *  - an app ID of its own: SteamAppId/SteamGameId for Proton (a number with
 *    the high bit set, the way Steam numbers its non-Steam shortcuts, so it
 *    never meets a real Steam app and its game-specific fixes), so each
 *    program's windows have their own class, steam_app_N;
 *  - a desktop entry: the program's applications-menu shortcut
 *    (lsiw-ID.desktop) when it has one, else a hidden one (lsiw.ID.desktop,
 *    NoDisplay) with its name and icon, both with StartupWMClass=steam_app_N
 *    (how KWin, Plasma's task manager and GNOME Shell match windows to
 *    applications) and BAMF_DESKTOP_FILE_HINT in the environment (the
 *    task manager's fallback for windows whose class says nothing, such as
 *    Wine's "program.exe");
 *  - a systemd user scope of its own, app-lsiw-<desktop ID>-<random>.scope
 *    in app.slice (the name the XDG/systemd application spec gives
 *    launched applications), which System Monitor lists under the desktop
 *    entry's name and icon. The scope is made over D-Bus with libsystemd,
 *    loaded at run time: without systemd, a user manager or the library,
 *    programs run as before.
 * Wine's shared processes (wineserver, services.exe, explorer.exe ...) are
 * started by whichever program comes first; lsiw_cli_scope_tidy() moves
 * them into a scope of their own, so a program's entry ends with the
 * program, not with the last program of the prefix.
 *
 * LSIW_NO_IDENTITY=1 turns all of this off.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "lsiw.h"
#include "wine.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DESKTOP_VAR "LSIW_DESKTOP_ID"
#define APPID_VAR   "LSIW_STEAM_APPID"

static int off(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") && strcasecmp(v, "false") && strcasecmp(v, "no");
}

/* ------------------------------------------------------------------ */
/* App ID and window class                                              */
/* ------------------------------------------------------------------ */

static uint32_t crc32_str(const char *s)
{
    uint32_t c = 0xffffffffu;
    for (; *s; s++) {
        c ^= (unsigned char)*s;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

unsigned long lsiw_cli_app_number(const char *id)
{
    char *key = lsiw_cli_printf("lsiw:%s", id);
    unsigned long n = (unsigned long)(crc32_str(key) | 0x80000000u);
    free(key);
    return n;
}

char *lsiw_cli_window_class(const char *id)
{
    return lsiw_cli_printf("steam_app_%lu", lsiw_cli_app_number(id));
}

/* ------------------------------------------------------------------ */
/* Desktop entries                                                      */
/* ------------------------------------------------------------------ */

static char *applications_dir(void)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') return lsiw_cli_printf("%s/applications", xdg);
    return lsiw_cli_printf("%s/.local/share/applications", lsiw_home_dir());
}

/* An Exec= argument: quoted, with the characters the spec reserves. */
static char *exec_quote(const char *s)
{
    char *r = malloc(strlen(s) * 3 + 3), *o = r;
    *o++ = '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '`' || *s == '$' || *s == '\\') { *o++ = '\\'; *o++ = '\\'; }
        if (*s == '%') *o++ = '%';
        *o++ = *s;
    }
    *o++ = '"';
    *o = 0;
    return r;
}

static char *one_line(const char *s)
{
    char *r = lsiw_cli_strdup(s);
    for (char *c = r; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    return r;
}

/* Write `text` to `path` unless it already holds exactly that. */
static int write_if_changed(const char *path, const char *text)
{
    size_t len = 0;
    char *old = lsiw_cli_read_file(path, &len);
    int same = old && len == strlen(text) && !memcmp(old, text, len);
    free(old);
    if (same) return 0;
    char *dir = lsiw_cli_strdup(path);
    *strrchr(dir, '/') = 0;
    lsiw_mkdirs(dir, 0755);
    free(dir);
    return lsiw_cli_write_file(path, text, strlen(text), 0644);
}

/* The program's own menu shortcut gets StartupWMClass (shortcuts made
 * before 1.10.1 have none). */
static void ensure_wmclass(const char *file, const char *wmclass)
{
    size_t len = 0;
    char *text = lsiw_cli_read_file(file, &len);
    if (!text) return;
    char *want = lsiw_cli_printf("StartupWMClass=%s\n", wmclass);
    char *line = strstr(text, "\nStartupWMClass=");
    char *out = NULL;
    if (line) {
        line++;
        char *end = strchr(line, '\n');
        end = end ? end + 1 : line + strlen(line);
        if ((size_t)(end - line) != strlen(want) || memcmp(line, want, strlen(want)))
            out = lsiw_cli_printf("%.*s%s%s", (int)(line - text), text, want, end);
    } else {
        out = lsiw_cli_printf("%s%s%s", text, len && text[len - 1] != '\n' ? "\n" : "", want);
    }
    if (out) {
        struct stat st;
        int mode = stat(file, &st) == 0 ? (int)(st.st_mode & 0777) : 0755;
        if (lsiw_cli_write_file(file, out, strlen(out), mode) < 0)
            LSIW_TRACE("cannot update %s: %s\n", file, strerror(errno));
    }
    free(out);
    free(want);
    free(text);
}

/* setenv, placed after APPDIR in the environment: Plasma's task manager
 * reads the first of BAMF_DESKTOP_FILE_HINT and APPDIR (AppImages) it
 * finds, and lsiw from the AppImage has APPDIR set. */
static void set_hint(const char *path)
{
    const char *a = getenv("APPDIR");
    char *appdir = a ? lsiw_cli_strdup(a) : NULL;
    if (appdir) unsetenv("APPDIR");
    unsetenv("BAMF_DESKTOP_FILE_HINT");
    setenv("BAMF_DESKTOP_FILE_HINT", path, 1);
    if (appdir) setenv("APPDIR", appdir, 1);
    free(appdir);
}

/* ------------------------------------------------------------------ */
/* systemd scopes (libsystemd, loaded when needed)                      */
/* ------------------------------------------------------------------ */

typedef struct { const char *name, *message; int need_free; } sdbus_error;
static struct {
    int tried;
    void *bus;
    pid_t owner;      /* sd-bus connections cannot be used after fork() */
    int (*open_user)(void **);
    int (*new_method_call)(void *, void **, const char *, const char *, const char *, const char *);
    int (*append)(void *, const char *, ...);
    int (*append_array)(void *, char, const void *, size_t);
    int (*open_container)(void *, char, const char *);
    int (*close_container)(void *);
    int (*call)(void *, void *, uint64_t, sdbus_error *, void **);
    void *(*message_unref)(void *);
    void (*error_free)(sdbus_error *);
} sd;

static int open_bus(void)
{
    int r = sd.open_user(&sd.bus);
    if (r < 0) {
        sd.bus = NULL;
        LSIW_TRACE("no systemd user manager (%s): programs share the cgroup of what started lsiw\n",
                   strerror(-r));
        return 0;
    }
    sd.owner = getpid();
    return 1;
}

static int sd_ready(void)
{
    if (sd.tried) {
        if (sd.bus && sd.owner != getpid()) return open_bus();   /* a child: a connection of its own */
        return sd.bus != NULL;
    }
    sd.tried = 1;
    void *h = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) { LSIW_TRACE("no libsystemd: programs share the cgroup of what started lsiw\n"); return 0; }
    *(void **)&sd.open_user = dlsym(h, "sd_bus_open_user");
    *(void **)&sd.new_method_call = dlsym(h, "sd_bus_message_new_method_call");
    *(void **)&sd.append = dlsym(h, "sd_bus_message_append");
    *(void **)&sd.append_array = dlsym(h, "sd_bus_message_append_array");
    *(void **)&sd.open_container = dlsym(h, "sd_bus_message_open_container");
    *(void **)&sd.close_container = dlsym(h, "sd_bus_message_close_container");
    *(void **)&sd.call = dlsym(h, "sd_bus_call");
    *(void **)&sd.message_unref = dlsym(h, "sd_bus_message_unref");
    *(void **)&sd.error_free = dlsym(h, "sd_bus_error_free");
    if (!sd.open_user || !sd.new_method_call || !sd.append || !sd.append_array || !sd.open_container ||
        !sd.close_container || !sd.call || !sd.message_unref || !sd.error_free) return 0;
    return open_bus();
}

/* StartTransientUnit(unit, "fail", properties, no aux units). */
static int call_start(const char *unit, const char *desc, const uint32_t *pids, int n, int collect,
                      sdbus_error *err)
{
    void *m = NULL, *reply = NULL;
    int r = sd.new_method_call(sd.bus, &m, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                               "org.freedesktop.systemd1.Manager", "StartTransientUnit");
    if (r >= 0) r = sd.append(m, "ss", unit, "fail");
    if (r >= 0) r = sd.open_container(m, 'a', "(sv)");
    if (r >= 0) r = sd.append(m, "(sv)", "Slice", "s", "app.slice");
    if (r >= 0) r = sd.append(m, "(sv)", "Description", "s", desc);
    /* CollectMode (systemd 236) lets a failed scope go away too. */
    if (r >= 0 && collect) r = sd.append(m, "(sv)", "CollectMode", "s", "inactive-or-failed");
    if (r >= 0) r = sd.open_container(m, 'r', "sv");
    if (r >= 0) r = sd.append(m, "s", "PIDs");
    if (r >= 0) r = sd.open_container(m, 'v', "au");
    if (r >= 0) r = sd.append_array(m, 'u', pids, (size_t)n * sizeof *pids);
    if (r >= 0) r = sd.close_container(m);
    if (r >= 0) r = sd.close_container(m);
    if (r >= 0) r = sd.close_container(m);
    if (r >= 0) r = sd.append(m, "a(sa(sv))", 0);
    if (r >= 0) r = sd.call(sd.bus, m, 0, err, &reply);
    if (reply) sd.message_unref(reply);
    if (m) sd.message_unref(m);
    return r;
}

/* Start scope `unit` (in app.slice) holding `pids`. 0 when done. */
static int start_scope(const char *unit, const char *desc, const pid_t *pids, int n)
{
    if (n < 1 || !sd_ready()) return -1;
    uint32_t *p = calloc((size_t)n, sizeof *p);
    for (int i = 0; i < n; i++) p[i] = (uint32_t)pids[i];
    int rc = -1;
    for (int collect = 1; collect >= 0; collect--) {
        sdbus_error err = { NULL, NULL, 0 };
        int r = call_start(unit, desc, p, n, collect, &err);
        if (r >= 0) { sd.error_free(&err); rc = 0; break; }
        LSIW_TRACE("cannot start %s: %s\n", unit, err.message ? err.message : strerror(-r));
        int retry = err.name && strstr(err.name, "InvalidArgs");
        sd.error_free(&err);
        if (!retry) break;
    }
    free(p);
    return rc;
}

/* A unit name part: what systemd-escape makes of it. */
static char *unit_escape(const char *s, size_t max)
{
    char *r = malloc(strlen(s) * 4 + 1), *o = r;
    for (const char *c = s; *c && (size_t)(o - r) < max; c++) {
        unsigned char u = (unsigned char)*c;
        if (isalnum(u) || u == '_' || u == ':' || (u == '.' && c != s)) *o++ = (char)u;
        else o += sprintf(o, "\\x%02x", u);
    }
    *o = 0;
    return r;
}

/* The cgroup of process `pid` (0: this one), "/user.slice/...". */
static char *cgroup_of(pid_t pid)
{
    char path[64];
    if (pid) snprintf(path, sizeof path, "/proc/%ld/cgroup", (long)pid);
    else snprintf(path, sizeof path, "/proc/self/cgroup");
    char *text = lsiw_cli_read_file(path, NULL);
    if (!text) return NULL;
    char *res = NULL;
    for (char *l = text; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, "0::", 3)) { res = lsiw_cli_strdup(l + 3); break; }
        l = nl ? nl + 1 : NULL;
    }
    free(text);
    return res;
}

static int ends_with_unit(const char *cg, const char *unit)
{
    const char *b = cg ? strrchr(cg, '/') : NULL;
    return b && !strcmp(b + 1, unit);
}

static char *random_part(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return lsiw_cli_printf("%lx%lx", (unsigned long)getpid(), (unsigned long)ts.tv_nsec);
}

static char *g_scope;   /* the cgroup this program runs in, once moved */

static void enter_scope(const char *desktop_id, const char *name)
{
    char *esc = unit_escape(desktop_id, 180);
    char *cg = cgroup_of(0);
    /* Started from the desktop entry: the desktop made the scope already. */
    char *mine = lsiw_cli_printf("-%s-", esc);
    if (cg && strstr(cg, "/app-") && strstr(strrchr(cg, '/'), mine)) {
        LSIW_TRACE("already in %s\n", cg);
        g_scope = cg;
        cg = NULL;
        goto out;
    }
    char *rnd = random_part();
    char *unit = lsiw_cli_printf("app-lsiw-%s-%s.scope", esc, rnd);
    char *desc = lsiw_cli_printf("%s (LSIW)", name);
    pid_t self = getpid();
    if (start_scope(unit, desc, &self, 1) == 0) {
        /* The move is done by the job the call queued: wait for it, so the
         * processes started next are created in the scope. */
        for (int i = 0; i < 200; i++) {
            free(cg);
            cg = cgroup_of(0);
            if (ends_with_unit(cg, unit)) break;
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (ends_with_unit(cg, unit)) {
            LSIW_TRACE("running in %s\n", unit);
            g_scope = cg;
            cg = NULL;
        }
    }
    free(desc);
    free(unit);
    free(rnd);
out:
    free(mine);
    free(cg);
    free(esc);
}

int lsiw_cli_scope_procs(pid_t **out)
{
    *out = NULL;
    if (!g_scope) return 0;
    /* cgroup v2, or its "unified" part on hybrid systems. */
    char *path = lsiw_cli_printf("/sys/fs/cgroup%s/cgroup.procs", g_scope);
    char *text = lsiw_cli_read_file(path, NULL);
    free(path);
    if (!text) {
        path = lsiw_cli_printf("/sys/fs/cgroup/unified%s/cgroup.procs", g_scope);
        text = lsiw_cli_read_file(path, NULL);
        free(path);
    }
    if (!text) return 0;
    int n = 0, cap = 0;
    pid_t *v = NULL;
    for (char *s = text, *e; *s; s = e) {
        long pid = strtol(s, &e, 10);
        if (e == s) break;
        while (*e == '\n') e++;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, (size_t)cap * sizeof *v); }
        v[n++] = (pid_t)pid;
    }
    free(text);
    *out = v;
    return n;
}

int lsiw_cli_scope_release(const pid_t *pids, int n)
{
    if (!g_scope || n < 1) return 0;
    char *rnd = random_part();
    char *unit = lsiw_cli_printf("lsiw-wine-%s.scope", rnd);
    int r = start_scope(unit, "Wine services shared by LSIW programs", pids, n);
    if (r == 0) LSIW_TRACE("Wine's shared processes moved to %s\n", unit);
    free(unit);
    free(rnd);
    return r;
}

/* ------------------------------------------------------------------ */
/* The identity of the program about to start                           */
/* ------------------------------------------------------------------ */

void lsiw_cli_identity(const struct lsiw_program *p, const char *exe)
{
    if (getenv(DESKTOP_VAR) || off("LSIW_NO_IDENTITY")) return;
    if (!p && (!exe || !*exe)) return;

    /* A library program by its ID; anything else by its file. */
    struct lsiw_program *lib = p && *p->id ? lsiw_program_get(p->id) : NULL;
    struct lsiw_program *tmp = NULL;
    char *id;
    if (lib) id = lsiw_cli_strdup(lib->id);
    else {
        const char *file = p && *p->exe ? p->exe : exe;
        char *real = realpath(file, NULL);
        tmp = lsiw_program_new(real ? real : file, p && *p->name ? p->name : NULL);
        if (p && *p->icon) { free(tmp->icon); tmp->icon = lsiw_cli_strdup(p->icon); }
        /* Readable and unique: run-<name>-<crc of the path>. */
        char *slug = lsiw_cli_strdup(tmp->id);
        char *dash = strrchr(slug, '-');
        if (dash && dash[1] && strspn(dash + 1, "0123456789") == strlen(dash + 1)) *dash = 0;   /* new_id's -2 */
        id = lsiw_cli_printf("run-%s-%08lx", slug, (unsigned long)crc32_str(tmp->exe));
        free(tmp->id);
        tmp->id = lsiw_cli_strdup(id);
        free(slug);
        free(real);
    }
    const struct lsiw_program *q = lib ? lib : tmp;

    char *wmclass = lsiw_cli_window_class(id);
    char *num = lsiw_cli_printf("%lu", lsiw_cli_app_number(id));
    setenv(APPID_VAR, num, 1);

    char *apps = applications_dir();
    char *hidden = lsiw_cli_printf("%s/lsiw.%s.desktop", apps, id);
    char *menu = lib ? lsiw_program_desktop_file(lib) : NULL;
    struct stat st;
    char *desktop_id, *file;
    if (menu && stat(menu, &st) == 0) {
        ensure_wmclass(menu, wmclass);
        unlink(hidden);
        desktop_id = lsiw_cli_printf("lsiw-%s", id);
        file = menu;
        menu = NULL;
    } else {
        char *self = lsiw_cli_self_path();
        char *qs = exec_quote(self ? self : "lsiw");
        char *qx = exec_quote(q->exe);
        char *exec = lib ? lsiw_cli_printf("%s programs run %s", qs, id)
                         : lsiw_cli_printf("%s run -- %s", qs, qx);
        char *icon = lsiw_program_icon(q, 0);
        char *name = one_line(q->name);
        char *text = lsiw_cli_printf(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=%s\n"
            "Comment=Windows program, run by LSIW\n"
            "Exec=%s\n"
            "Icon=%s\n"
            "Terminal=false\n"
            "NoDisplay=true\n"
            "StartupWMClass=%s\n"
            "X-LSIW-Program=%s\n",
            name, exec, icon ? icon : "lsiw", wmclass, id);
        if (write_if_changed(hidden, text) < 0)
            LSIW_TRACE("cannot write %s: %s\n", hidden, strerror(errno));
        free(text); free(name); free(icon); free(exec); free(qx); free(qs); free(self);
        desktop_id = lsiw_cli_printf("lsiw.%s", id);
        file = hidden;
        hidden = NULL;
    }
    setenv(DESKTOP_VAR, desktop_id, 1);
    set_hint(file);
    LSIW_TRACE("desktop identity: %s (%s, window class %s)\n", desktop_id, file, wmclass);
    if (!off("LSIW_NO_SCOPE")) enter_scope(desktop_id, q->name);

    free(file); free(hidden); free(menu); free(apps);
    free(desktop_id); free(num); free(wmclass); free(id);
    if (lib) lsiw_program_free(lib);
    if (tmp) lsiw_program_free(tmp);
}
