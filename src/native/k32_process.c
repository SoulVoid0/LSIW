/* kernel32: processes, threads, synchronisation, environment, modules,
 * system information, time and locale.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include "pe.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

extern char **environ;
void lsiw_disable_thread_calls(HMODULE h);

/* ================================================================== */
/* Command line                                                         */
/* ================================================================== */

static char  *g_cmdline_a;
static WCHAR *g_cmdline_w;

static void append_quoted(char **buf, size_t *len, size_t *cap, const char *arg)
{
    int need = !*arg || strpbrk(arg, " \t\"") != NULL;
    size_t al = strlen(arg);
    while (*len + al * 2 + 4 > *cap) { *cap *= 2; *buf = realloc(*buf, *cap); }
    char *o = *buf + *len;
    if (*len) *o++ = ' ';
    if (!need) { memcpy(o, arg, al); o += al; *len = (size_t)(o - *buf); return; }
    *o++ = '"';
    for (const char *p = arg; ; p++) {
        int bs = 0;
        while (*p == '\\') { p++; bs++; }
        if (!*p) { for (int i = 0; i < bs * 2; i++) *o++ = '\\'; break; }
        if (*p == '"') { for (int i = 0; i < bs * 2 + 1; i++) *o++ = '\\'; *o++ = '"'; }
        else { for (int i = 0; i < bs; i++) *o++ = '\\'; *o++ = *p; }
    }
    *o++ = '"';
    *len = (size_t)(o - *buf);
}

void lsiw_set_command_line(int argc, char **argv, const char *exe_win)
{
    const char *verbatim = getenv("LSIW_CMDLINE");
    if (verbatim && *verbatim) {
        g_cmdline_a = strdup(verbatim);
        unsetenv("LSIW_CMDLINE");
    } else {
        size_t cap = 256, len = 0;
        char *buf = malloc(cap);
        buf[0] = 0;
        append_quoted(&buf, &len, &cap, exe_win);
        int translate = strcasecmp(lsiw_setting_get("runtime", "translate_args", "true"), "false") != 0;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            char *conv = NULL;
            struct stat st;
            /* An absolute Linux path that exists is handed over as C:\... */
            if (translate && a[0] == '/' && a[1] && stat(a, &st) == 0) conv = lsiw_linux_to_win(a);
            append_quoted(&buf, &len, &cap, conv ? conv : a);
            free(conv);
        }
        buf[len] = 0;
        g_cmdline_a = buf;
    }
    g_cmdline_w = lsiw_u2w(g_cmdline_a);
}

const char  *lsiw_cmdline_a(void) { return g_cmdline_a ? g_cmdline_a : ""; }
const WCHAR *lsiw_cmdline_w(void) { static WCHAR empty[1]; return g_cmdline_w ? g_cmdline_w : empty; }

API char  *GetCommandLineA(void) { return g_cmdline_a; }
API WCHAR *GetCommandLineW(void) { return g_cmdline_w; }

/* ================================================================== */
/* Environment                                                          */
/* ================================================================== */

static int g_exit_fd = -1;        /* see lsiw_exit_report() */
static char **g_env;
static size_t g_env_n, g_env_cap;
static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;

static long env_find(const char *name, size_t nl)
{
    for (size_t i = 0; i < g_env_n; i++)
        if (!strncasecmp(g_env[i], name, nl) && g_env[i][nl] == '=') return (long)i;
    return -1;
}

int lsiw_env_set(const char *name, const char *value)
{
    size_t nl = strlen(name);
    if (!nl || strchr(name + 1, '=')) return -1;
    pthread_mutex_lock(&g_env_lock);
    long i = env_find(name, nl);
    if (!value) {
        if (i >= 0) { free(g_env[i]); g_env[i] = g_env[--g_env_n]; g_env[g_env_n] = NULL; }
    } else {
        /* Keep the original capitalisation of an existing name. */
        char *e = lsiw_strdup_printf("%.*s=%s", (int)nl, i >= 0 ? g_env[i] : name, value);
        if (i >= 0) { free(g_env[i]); g_env[i] = e; }
        else {
            if (g_env_n + 2 > g_env_cap) { g_env_cap = g_env_cap ? g_env_cap * 2 : 64; g_env = realloc(g_env, g_env_cap * sizeof(char *)); }
            g_env[g_env_n++] = e;
            g_env[g_env_n] = NULL;
        }
    }
    pthread_mutex_unlock(&g_env_lock);
    return 0;
}

const char *lsiw_env_get(const char *name)
{
    long i = env_find(name, strlen(name));
    return i >= 0 ? strchr(g_env[i], '=') + 1 : NULL;
}

char **lsiw_env_block(void) { return g_env; }

static char *linux_pathlist_to_win(const char *p)
{
    size_t cap = strlen(p) * 2 + 64, len = 0;
    char *out = malloc(cap);
    out[0] = 0;
    char *copy = strdup(p), *save = NULL;
    for (char *e = strtok_r(copy, ":", &save); e; e = strtok_r(NULL, ":", &save)) {
        if (!*e) continue;
        char *w = lsiw_linux_to_win(e);
        size_t wl = strlen(w);
        while (len + wl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        if (len) out[len++] = ';';
        memcpy(out + len, w, wl); len += wl; out[len] = 0;
        free(w);
    }
    free(copy);
    return out;
}

/* Variables LSIW uses to talk to its own child processes; never shown to
 * Windows programs. */
static int internal_env(const char *name)
{
    return !strcmp(name, "LSIW_CMDLINE") || !strcmp(name, "LSIW_EXIT_FD") || !strcmp(name, "LSIW_FROM_WINDOWS") ||
           !strcmp(name, "LSIW_CWD") || !strncmp(name, "LSIW_WIN_", 9);
}

static char *win_pathlist_to_linux(const char *p)
{
    size_t cap = strlen(p) * 2 + 64, len = 0;
    char *out = malloc(cap);
    out[0] = 0;
    char *copy = strdup(p), *save = NULL;
    for (char *e = strtok_r(copy, ";", &save); e; e = strtok_r(NULL, ";", &save)) {
        if (!*e) continue;
        char *l = (e[0] == '/') ? strdup(e) : lsiw_win_to_linux(e);
        size_t ll = strlen(l);
        while (len + ll + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        if (len) out[len++] = ':';
        memcpy(out + len, l, ll); len += ll; out[len] = 0;
        free(l);
    }
    free(copy);
    return out;
}

void lsiw_env_init(void)
{
    if (g_env) return;
    const char *xfd = getenv("LSIW_EXIT_FD");
    if (xfd) {
        g_exit_fd = atoi(xfd);
        if (g_exit_fd > 2 && fcntl(g_exit_fd, F_SETFD, FD_CLOEXEC) == 0) {} else g_exit_fd = -1;
    }
    /* A parent LSIW process hands over its Windows environment as it is,
     * except HOME and PATH, which stay Linux values for the Linux side and
     * travel as LSIW_WIN_HOME / LSIW_WIN_PATH. */
    int from_windows = getenv("LSIW_FROM_WINDOWS") != NULL;
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(eq - *e), *e);
        if (internal_env(name) || !strcmp(name, "PWD") || !strcmp(name, "OLDPWD") || !strcmp(name, "_"))
            continue;
        if (from_windows && (!strcmp(name, "HOME") || !strcmp(name, "PATH"))) continue;
        lsiw_env_set(name, eq + 1);
    }
    if (from_windows) {
        for (char **e = environ; *e; e++)
            if (!strncmp(*e, "LSIW_WIN_", 9)) {
                const char *eq = strchr(*e, '=');
                if (!eq) continue;
                char name[256];
                snprintf(name, sizeof name, "%.*s", (int)(eq - *e - 9), *e + 9);
                lsiw_env_set(name, eq + 1);
            }
        static const char *const internal[] = { "LSIW_EXIT_FD", "LSIW_FROM_WINDOWS", "LSIW_WIN_HOME", "LSIW_WIN_PATH" };
        for (size_t i = 0; i < sizeof internal / sizeof internal[0]; i++) unsetenv(internal[i]);
        return;
    }
    unsetenv("LSIW_EXIT_FD");
    /* PATH: Linux list -> Windows list, with the Windows folders first.
     * A PATH that is already Windows-style (set by a parent Windows program
     * through CreateProcess) is kept as it is. */
    const char *path = lsiw_env_get("PATH");
    const char *sys = lsiw_var_windows("SYSTEM32"), *win = lsiw_var_windows("WINDIR");
    if (!path || !strchr(path, ';') || path[1] != ':') {
        char *wp = linux_pathlist_to_win(path ? path : "/usr/local/bin:/usr/bin:/bin");
        char *full = lsiw_strdup_printf("%s;%s;%s", sys ? sys : "C:\\windows\\system32", win ? win : "C:\\windows", wp);
        lsiw_env_set("PATH", full);
        free(wp); free(full);
    }
    const char *home = lsiw_env_get("HOME");
    if (home && home[0] == '/') { char *w = lsiw_linux_to_win(home); lsiw_env_set("HOME", w); free(w); }

    static const struct { const char *env, *var; } map[] = {
        { "ALLUSERSPROFILE", "ALLUSERSPROFILE" }, { "APPDATA", "APPDATA" },
        { "CommonProgramFiles", "COMMONPROGRAMFILES" }, { "CommonProgramFiles(x86)", "COMMONPROGRAMFILES(X86)" },
        { "CommonProgramW6432", "COMMONPROGRAMW6432" }, { "COMPUTERNAME", "COMPUTERNAME" },
        { "ComSpec", "COMSPEC" }, { "HOMEDRIVE", "HOMEDRIVE" }, { "HOMEPATH", "HOMEPATH" },
        { "LOCALAPPDATA", "LOCALAPPDATA" }, { "ProgramData", "PROGRAMDATA" },
        { "ProgramFiles", "PROGRAMFILES" }, { "ProgramFiles(x86)", "PROGRAMFILES(X86)" },
        { "ProgramW6432", "PROGRAMW6432" }, { "PUBLIC", "PUBLIC" }, { "SystemDrive", "SYSTEMDRIVE" },
        { "SystemRoot", "SYSTEMROOT" }, { "TEMP", "TEMP" }, { "TMP", "TMP" },
        { "USERNAME", "USERNAME" }, { "USERPROFILE", "USERPROFILE" }, { "windir", "WINDIR" },
        { "USERDOMAIN", "COMPUTERNAME" }, { "LOGONSERVER", NULL },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (!map[i].var) continue;
        const char *v = lsiw_var_windows(map[i].var);
        if (v) { lsiw_env_set(map[i].env, NULL); lsiw_env_set(map[i].env, v); }
    }
    char np[16];
    snprintf(np, sizeof np, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    lsiw_env_set("NUMBER_OF_PROCESSORS", np);
    lsiw_env_set("OS", "Windows_NT");
    lsiw_env_set("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    lsiw_env_set("PROCESSOR_ARCHITECTURE", "AMD64");
    lsiw_env_set("PROCESSOR_IDENTIFIER", "Intel64 Family 6 Model 0 Stepping 0, GenuineIntel");
    lsiw_env_set("PROCESSOR_LEVEL", "6");
    lsiw_env_set("LSIW", LSIW_VERSION);
}

API DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD size)
{
    const char *v = name ? lsiw_env_get(name) : NULL;
    if (!v) { lsiw_set_last_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    lsiw_set_last_error(0);
    return lsiw_ret_str_a(v, buf, size);
}

API DWORD GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD size)
{
    char *n = lsiw_w2u(name);
    const char *v = n ? lsiw_env_get(n) : NULL;
    free(n);
    if (!v) { lsiw_set_last_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
    lsiw_set_last_error(0);
    return lsiw_ret_str_w(v, buf, size);
}

API BOOL SetEnvironmentVariableA(const char *name, const char *value)
{
    if (!name || lsiw_env_set(name, value) < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

API BOOL SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value)
{
    char *n = lsiw_w2u(name), *v = lsiw_w2u(value);
    int r = n ? lsiw_env_set(n, v) : -1;
    free(n); free(v);
    if (r < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

API char *GetEnvironmentStringsA(void)
{
    pthread_mutex_lock(&g_env_lock);
    size_t total = 2;
    for (size_t i = 0; i < g_env_n; i++) total += strlen(g_env[i]) + 1;
    char *blk = malloc(total), *o = blk;
    for (size_t i = 0; i < g_env_n; i++) { size_t l = strlen(g_env[i]) + 1; memcpy(o, g_env[i], l); o += l; }
    *o++ = 0; *o = 0;
    pthread_mutex_unlock(&g_env_lock);
    return blk;
}

API WCHAR *GetEnvironmentStringsW(void)
{
    pthread_mutex_lock(&g_env_lock);
    size_t total = 2;
    for (size_t i = 0; i < g_env_n; i++) total += (size_t)lsiw_utf8_to_utf16(g_env[i], -1, NULL, 0);
    WCHAR *blk = malloc(total * sizeof(WCHAR)), *o = blk;
    for (size_t i = 0; i < g_env_n; i++) o += lsiw_utf8_to_utf16(g_env[i], -1, o, (int)(total - (size_t)(o - blk)));
    *o++ = 0; *o = 0;
    pthread_mutex_unlock(&g_env_lock);
    return blk;
}

API BOOL FreeEnvironmentStringsA(char *p) { free(p); return TRUE; }
API BOOL FreeEnvironmentStringsW(WCHAR *p) { free(p); return TRUE; }

static char *expand_env(const char *s)
{
    size_t cap = strlen(s) + 64, len = 0;
    char *out = malloc(cap);
    for (const char *p = s; *p; ) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            if (e && e > p + 1) {
                char name[256];
                snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
                const char *v = lsiw_env_get(name);
                if (!v) v = lsiw_var_windows(name);   /* USMT names such as %CSIDL_FONTS% */
                if (v) {
                    size_t vl = strlen(v);
                    while (len + vl + 1 > cap) { cap *= 2; out = realloc(out, cap); }
                    memcpy(out + len, v, vl); len += vl; p = e + 1;
                    continue;
                }
            }
        }
        if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        out[len++] = *p++;
    }
    out[len] = 0;
    return out;
}

API DWORD ExpandEnvironmentStringsA(const char *src, char *dst, DWORD size)
{
    char *r = expand_env(src ? src : "");
    DWORD n = (DWORD)strlen(r) + 1;
    if (dst && size >= n) memcpy(dst, r, n);
    free(r);
    return n;
}

API DWORD ExpandEnvironmentStringsW(const WCHAR *src, WCHAR *dst, DWORD size)
{
    char *s = lsiw_w2u(src);
    char *r = expand_env(s ? s : "");
    DWORD n = (DWORD)lsiw_utf8_to_utf16(r, -1, NULL, 0);
    if (dst && size >= n) lsiw_utf8_to_utf16(r, -1, dst, (int)size);
    free(s); free(r);
    return n;
}

/* ================================================================== */
/* Errors and process exit                                              */
/* ================================================================== */

API DWORD GetLastError(void) { return lsiw_get_last_error(); }
API void  SetLastError(DWORD e) { lsiw_set_last_error(e); }
API void  RestoreLastError(DWORD e) { lsiw_set_last_error(e); }

/* A parent LSIW process passes a pipe in LSIW_EXIT_FD; the full 32-bit exit
 * code goes through it, since a Linux exit status keeps only 8 bits. */
void lsiw_exit_report(uint32_t code)
{
    int fd = __atomic_exchange_n(&g_exit_fd, -1, __ATOMIC_SEQ_CST);
    if (fd < 0) return;
    ssize_t r = write(fd, &code, sizeof code);
    (void)r;
    close(fd);
}

void lsiw_exit_process(uint32_t code)
{
    static int exiting;
    if (!__atomic_exchange_n(&exiting, 1, __ATOMIC_SEQ_CST)) {
        lsiw_run_exit_handlers();      /* msvcrt runs atexit() functions on ExitProcess too */
        lsiw_process_detach();
        lsiw_crt_flush_all();
    }
    fflush(NULL);
    lsiw_exit_report(code);
    _exit((int)code);
}

API void ExitProcess(UINT code) { lsiw_exit_process(code); }

API BOOL TerminateProcess(HANDLE h, UINT code)
{
    if (h == (HANDLE)(intptr_t)-1) { fflush(NULL); lsiw_exit_report(code); _exit((int)code); }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_PROCESS);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (o->reaped) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (!o->term_set) { o->term_set = 1; o->term_code = code; }
    if (kill(o->pid, SIGKILL) < 0) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

API void FatalAppExitA(UINT a, const char *msg) { (void)a; fprintf(stderr, "%s\n", msg ? msg : ""); lsiw_exit_report(0x40000015); _exit(255); }
API void FatalAppExitW(UINT a, const WCHAR *msg) { (void)a; char *m = lsiw_w2u(msg); fprintf(stderr, "%s\n", m ? m : ""); lsiw_exit_report(0x40000015); _exit(255); }

/* The Windows exit code of a reaped child: what it reported through its exit
 * pipe, else the TerminateProcess code, else its Linux status. */
static uint32_t process_exit_code(struct lsiw_handle_obj *o, int st)
{
    if (o->fd >= 0) {
        uint32_t code;
        ssize_t r = read(o->fd, &code, sizeof code);
        close(o->fd);
        o->fd = -1;
        if (r == (ssize_t)sizeof code) return code;
    }
    if (o->term_set) return o->term_code;
    if (WIFEXITED(st)) return (uint32_t)WEXITSTATUS(st);
    switch (WTERMSIG(st)) {
    case SIGSEGV: case SIGBUS: return 0xC0000005;     /* STATUS_ACCESS_VIOLATION */
    case SIGILL:  return 0xC000001D;                  /* STATUS_ILLEGAL_INSTRUCTION */
    case SIGFPE:  return 0xC0000094;                  /* STATUS_INTEGER_DIVIDE_BY_ZERO */
    case SIGTRAP: return 0x80000003;                  /* STATUS_BREAKPOINT */
    case SIGABRT: return 3;                           /* abort() */
    case SIGKILL: case SIGTERM: return 1;
    default:      return 0xC000013A;                  /* STATUS_CONTROL_C_EXIT */
    }
}

/* ================================================================== */
/* Handles, waits, synchronisation objects                              */
/* ================================================================== */

API BOOL CloseHandle(HANDLE h)
{
    if (h == (HANDLE)(intptr_t)-1 || h == (HANDLE)(intptr_t)-2) return TRUE;
    if (!lsiw_handle_close(h)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

API BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD opts)
{
    (void)sp; (void)tp; (void)access; (void)inherit;
    HANDLE n;
    if (src == (HANDLE)(intptr_t)-1 || src == (HANDLE)(intptr_t)-2) n = src;
    else n = lsiw_handle_dup(src);
    if (!n) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (opts & 1 /* DUPLICATE_CLOSE_SOURCE */) lsiw_handle_close(src);
    if (dst) *dst = n;
    return TRUE;
}

API BOOL GetHandleInformation(HANDLE h, DWORD *flags) { if (flags) *flags = 0; return lsiw_handle_get(h, -1) != NULL; }
API BOOL SetHandleInformation(HANDLE h, DWORD m, DWORD f) { (void)m; (void)f; return lsiw_handle_get(h, -1) != NULL; }

/* Named objects live in a per-process table so OpenEvent & co. work. */
struct named { char *name; HANDLE h; int type; struct named *next; };
static struct named *g_named;

static HANDLE named_lookup(const char *name, int type)
{
    for (struct named *n = g_named; n; n = n->next)
        if (!strcmp(n->name, name) && n->type == type && lsiw_handle_get(n->h, type)) return n->h;
    return NULL;
}

static HANDLE create_object(int type, const char *name, int manual, int initial, long max)
{
    if (name && *name) {
        HANDLE e = named_lookup(name, type);
        if (e) { lsiw_set_last_error(ERROR_ALREADY_EXISTS); return lsiw_handle_dup(e); }
    }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(type, &o);
    if (!h) return NULL;
    o->manual_reset = manual;
    if (type == H_EVENT) o->signaled = initial;
    if (type == H_MUTEX && initial) { o->owner_tid = lsiw_thread()->tid; o->recursion = 1; }
    if (type == H_SEMAPHORE) { o->count = initial; o->max_count = max; }
    if (name && *name) {
        struct named *n = calloc(1, sizeof *n);
        n->name = strdup(name); n->h = h; n->type = type; n->next = g_named; g_named = n;
    }
    lsiw_set_last_error(0);
    return h;
}

static HANDLE open_object(int type, const char *name)
{
    HANDLE e = name ? named_lookup(name, type) : NULL;
    if (!e) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
    return lsiw_handle_dup(e);
}

#define WNAME(w) char *_n = lsiw_w2u(w)
API HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) { (void)sa; return create_object(H_EVENT, name, manual, initial, 0); }
API HANDLE CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name) { (void)sa; WNAME(name); HANDLE h = create_object(H_EVENT, _n, manual, initial, 0); free(_n); return h; }
API HANDLE CreateEventExA(void *sa, const char *name, DWORD flags, DWORD access) { (void)sa; (void)access; return create_object(H_EVENT, name, flags & 1, (flags & 2) != 0, 0); }
API HANDLE CreateEventExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) { (void)sa; (void)access; WNAME(name); HANDLE h = create_object(H_EVENT, _n, flags & 1, (flags & 2) != 0, 0); free(_n); return h; }
API HANDLE OpenEventA(DWORD a, BOOL i, const char *name) { (void)a; (void)i; return open_object(H_EVENT, name); }
API HANDLE OpenEventW(DWORD a, BOOL i, const WCHAR *name) { (void)a; (void)i; WNAME(name); HANDLE h = open_object(H_EVENT, _n); free(_n); return h; }
API HANDLE CreateMutexA(void *sa, BOOL owner, const char *name) { (void)sa; return create_object(H_MUTEX, name, 0, owner, 0); }
API HANDLE CreateMutexW(void *sa, BOOL owner, const WCHAR *name) { (void)sa; WNAME(name); HANDLE h = create_object(H_MUTEX, _n, 0, owner, 0); free(_n); return h; }
API HANDLE CreateMutexExA(void *sa, const char *name, DWORD flags, DWORD access) { (void)sa; (void)access; return create_object(H_MUTEX, name, 0, flags & 1, 0); }
API HANDLE CreateMutexExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) { (void)sa; (void)access; WNAME(name); HANDLE h = create_object(H_MUTEX, _n, 0, flags & 1, 0); free(_n); return h; }
API HANDLE OpenMutexA(DWORD a, BOOL i, const char *name) { (void)a; (void)i; return open_object(H_MUTEX, name); }
API HANDLE OpenMutexW(DWORD a, BOOL i, const WCHAR *name) { (void)a; (void)i; WNAME(name); HANDLE h = open_object(H_MUTEX, _n); free(_n); return h; }
API HANDLE CreateSemaphoreA(void *sa, LONG init, LONG max, const char *name) { (void)sa; return create_object(H_SEMAPHORE, name, 0, init, max); }
API HANDLE CreateSemaphoreW(void *sa, LONG init, LONG max, const WCHAR *name) { (void)sa; WNAME(name); HANDLE h = create_object(H_SEMAPHORE, _n, 0, init, max); free(_n); return h; }
API HANDLE CreateSemaphoreExW(void *sa, LONG init, LONG max, const WCHAR *name, DWORD f, DWORD a) { (void)sa; (void)f; (void)a; WNAME(name); HANDLE h = create_object(H_SEMAPHORE, _n, 0, init, max); free(_n); return h; }
API HANDLE OpenSemaphoreW(DWORD a, BOOL i, const WCHAR *name) { (void)a; (void)i; WNAME(name); HANDLE h = open_object(H_SEMAPHORE, _n); free(_n); return h; }

API BOOL SetEvent(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_EVENT);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_wait_lock(); o->signaled = 1; lsiw_wait_broadcast(); lsiw_wait_unlock();
    return TRUE;
}

API BOOL ResetEvent(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_EVENT);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_wait_lock(); o->signaled = 0; lsiw_wait_unlock();
    return TRUE;
}

API BOOL PulseEvent(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_EVENT);
    if (!o) return FALSE;
    lsiw_wait_lock(); o->signaled = 1; lsiw_wait_broadcast(); lsiw_wait_unlock();
    sched_yield();
    lsiw_wait_lock(); o->signaled = 0; lsiw_wait_unlock();
    return TRUE;
}

API BOOL ReleaseMutex(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_MUTEX);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_wait_lock();
    if (o->owner_tid != lsiw_thread()->tid) { lsiw_wait_unlock(); lsiw_set_last_error(ERROR_NOT_OWNER); return FALSE; }
    if (--o->recursion <= 0) { o->recursion = 0; o->owner_tid = 0; lsiw_wait_broadcast(); }
    lsiw_wait_unlock();
    return TRUE;
}

API BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_SEMAPHORE);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_wait_lock();
    if (o->count + n > o->max_count) { lsiw_wait_unlock(); lsiw_set_last_error(298 /* ERROR_TOO_MANY_POSTS */); return FALSE; }
    if (prev) *prev = (LONG)o->count;
    o->count += n;
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
    return TRUE;
}

/* Waitable timers */
static uint64_t now_100ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ULL;
}

API HANDLE CreateWaitableTimerW(void *sa, BOOL manual, const WCHAR *name) { (void)sa; WNAME(name); HANDLE h = create_object(H_TIMER, _n, manual, 0, 0); free(_n); return h; }
API HANDLE CreateWaitableTimerA(void *sa, BOOL manual, const char *name) { (void)sa; return create_object(H_TIMER, name, manual, 0, 0); }
API HANDLE CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD a) { (void)sa; (void)a; WNAME(name); HANDLE h = create_object(H_TIMER, _n, flags & 1, 0, 0); free(_n); return h; }

API BOOL SetWaitableTimer(HANDLE h, const int64_t *due, LONG period, void *cb, void *arg, BOOL resume)
{
    (void)cb; (void)arg; (void)resume;
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_TIMER);
    if (!o || !due) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_wait_lock();
    o->map_size = *due < 0 ? now_100ns() + (uint64_t)(-*due) : (uint64_t)*due;   /* due time (absolute) */
    o->count = period;                                                       /* period in ms */
    o->signaled = 0;
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
    return TRUE;
}
API BOOL SetWaitableTimerEx(HANDLE h, const int64_t *due, LONG period, void *cb, void *arg, void *ctx, ULONG delay)
{ (void)ctx; (void)delay; return SetWaitableTimer(h, due, period, cb, arg, FALSE); }

API BOOL CancelWaitableTimer(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_TIMER);
    if (!o) return FALSE;
    lsiw_wait_lock(); o->map_size = 0; lsiw_wait_unlock();
    return TRUE;
}

/* Is the object signalled? If `take`, consume it (auto-reset, mutex ownership, semaphore count). */
static int obj_ready(struct lsiw_handle_obj *o, uint32_t tid, int take, uint64_t *next_timer)
{
    switch (o->type) {
    case H_EVENT:
        if (!o->signaled) return 0;
        if (take && !o->manual_reset) o->signaled = 0;
        return 1;
    case H_MUTEX:
        if (o->owner_tid && o->owner_tid != tid) {
            /* The owner thread ended without releasing it: abandoned. */
            if (syscall(SYS_tgkill, getpid(), (pid_t)o->owner_tid, 0) == 0 || errno != ESRCH) return 0;
            if (take) { o->owner_tid = tid; o->recursion = 1; }
            return 2;
        }
        if (take) { o->owner_tid = tid; o->recursion++; }
        return 1;
    case H_SEMAPHORE:
        if (o->count <= 0) return 0;
        if (take) o->count--;
        return 1;
    case H_THREAD:
        return o->signaled;
    case H_PROCESS:
        if (!o->reaped) {
            int st;
            pid_t r = waitpid(o->pid, &st, WNOHANG);
            if (r == o->pid) {
                o->reaped = 1; o->signaled = 1;
                o->exit_code = process_exit_code(o, st);
            } else if (r < 0 && errno == ECHILD) { o->reaped = 1; o->signaled = 1; }
        }
        if (!o->signaled && next_timer) *next_timer = 1;  /* poll again soon */
        return o->signaled;
    case H_TIMER:
        if (o->signaled) { if (take && !o->manual_reset) o->signaled = 0; return 1; }
        if (o->map_size && now_100ns() >= o->map_size) {
            if (o->count > 0) o->map_size += (uint64_t)o->count * 10000;
            else o->map_size = 0;
            if (o->manual_reset || !take) o->signaled = 1;
            return 1;
        }
        if (o->map_size && next_timer && (!*next_timer || o->map_size < *next_timer)) *next_timer = o->map_size;
        return 0;
    case H_FILE: {
        struct pollfd p = { o->fd, POLLIN, 0 };
        if (poll(&p, 1, 0) > 0) return 1;
        if (next_timer) *next_timer = 1;
        return 0;
    }
    default:
        return 1;
    }
}

int lsiw_apc_pending(void);       /* k32_extra.c */
int lsiw_apc_run_pending(void);
const int lsiw_k32_alertable_apcs = 1;   /* alertable waits run queued APCs */
#define WAIT_IO_COMPLETION 0xC0
#define WAIT_ABANDONED_0   0x80

static DWORD wait_objects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable)
{
    if (alertable && lsiw_apc_run_pending()) return WAIT_IO_COMPLETION;
    if (n == 0 && alertable) { /* SleepEx */ }
    else if (n == 0 || n > 64) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    struct lsiw_handle_obj *objs[64];
    for (DWORD i = 0; i < n; i++) {
        if (hs[i] == (HANDLE)(intptr_t)-1 || hs[i] == (HANDLE)(intptr_t)-2) { objs[i] = NULL; continue; }
        objs[i] = lsiw_handle_get(hs[i], -1);
        if (!objs[i]) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    }
    uint32_t tid = lsiw_thread()->tid;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (ms != INFINITE) {
        deadline.tv_sec += ms / 1000;
        deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }
    lsiw_wait_lock();
    for (;;) {
        uint64_t next = 0;
        if (n && all) {
            int ok = 1;
            for (DWORD i = 0; i < n; i++) if (!objs[i] || !obj_ready(objs[i], tid, 0, &next)) ok = 0;
            if (ok) {
                int abandoned = 0;
                for (DWORD i = 0; i < n; i++) if (obj_ready(objs[i], tid, 1, NULL) == 2) abandoned = 1;
                lsiw_wait_unlock();
                return abandoned ? WAIT_ABANDONED_0 : WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < n; i++) {
                int r = objs[i] ? obj_ready(objs[i], tid, 1, &next) : 0;
                if (r) { lsiw_wait_unlock(); return (r == 2 ? WAIT_ABANDONED_0 : WAIT_OBJECT_0) + i; }
            }
        }
        if (alertable && lsiw_apc_pending()) {
            lsiw_wait_unlock();
            lsiw_apc_run_pending();
            return WAIT_IO_COMPLETION;
        }
        if (ms == 0) { lsiw_wait_unlock(); return WAIT_TIMEOUT; }
        struct timespec now, until;
        clock_gettime(CLOCK_REALTIME, &now);
        if (ms != INFINITE && (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))) {
            lsiw_wait_unlock(); return WAIT_TIMEOUT;
        }
        until = ms == INFINITE ? (struct timespec){ now.tv_sec + 3600, now.tv_nsec } : deadline;
        if (next) {
            /* Something must be polled (child process, timer, stdin). */
            struct timespec p = now;
            if (next == 1) p.tv_nsec += 10000000L;
            else {
                uint64_t cur = now_100ns();
                uint64_t d = next > cur ? next - cur : 0;
                p.tv_sec += (time_t)(d / 10000000ULL);
                p.tv_nsec += (long)(d % 10000000ULL) * 100;
            }
            if (p.tv_nsec >= 1000000000L) { p.tv_sec++; p.tv_nsec -= 1000000000L; }
            if (p.tv_sec < until.tv_sec || (p.tv_sec == until.tv_sec && p.tv_nsec < until.tv_nsec)) until = p;
        }
        lsiw_wait_cond(&until);
    }
}

API DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return wait_objects(1, &h, FALSE, ms, FALSE); }
API DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL a) { return wait_objects(1, &h, FALSE, ms, a); }
API DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) { return wait_objects(n, hs, all, ms, FALSE); }
API DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL a) { return wait_objects(n, hs, all, ms, a); }
API DWORD SignalObjectAndWait(HANDLE sig, HANDLE w, DWORD ms, BOOL a)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(sig, -1);
    if (o && o->type == H_EVENT) SetEvent(sig);
    else if (o && o->type == H_MUTEX) ReleaseMutex(sig);
    else if (o && o->type == H_SEMAPHORE) ReleaseSemaphore(sig, 1, NULL);
    return wait_objects(1, &w, FALSE, ms, a);
}

/* ---- Critical sections (RTL_CRITICAL_SECTION, 40 bytes) ---- */
struct cs { void *debug; LONG lock_count; LONG recursion; HANDLE owner; HANDLE sem; ULONG_PTR spin; };
static pthread_mutex_t g_cs_init = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_mutex(struct cs *c)
{
    pthread_mutex_t *m = __atomic_load_n((pthread_mutex_t **)&c->sem, __ATOMIC_ACQUIRE);
    if (m) return m;
    pthread_mutex_lock(&g_cs_init);
    m = (pthread_mutex_t *)c->sem;
    if (!m) {
        m = malloc(sizeof *m);
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a);
        pthread_mutexattr_destroy(&a);
        __atomic_store_n((pthread_mutex_t **)&c->sem, m, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cs_init);
    return m;
}

API void InitializeCriticalSection(struct cs *c) { memset(c, 0, sizeof *c); c->lock_count = -1; cs_mutex(c); }
API BOOL InitializeCriticalSectionAndSpinCount(struct cs *c, DWORD spin) { InitializeCriticalSection(c); c->spin = spin; return TRUE; }
API BOOL InitializeCriticalSectionEx(struct cs *c, DWORD spin, DWORD flags) { (void)flags; return InitializeCriticalSectionAndSpinCount(c, spin); }
API DWORD SetCriticalSectionSpinCount(struct cs *c, DWORD spin) { DWORD o = (DWORD)c->spin; c->spin = spin; return o; }
API void DeleteCriticalSection(struct cs *c)
{
    pthread_mutex_t *m = (pthread_mutex_t *)c->sem;
    if (m) { pthread_mutex_destroy(m); free(m); }
    c->sem = NULL;
}
API void EnterCriticalSection(struct cs *c)
{
    pthread_mutex_lock(cs_mutex(c));
    c->owner = (HANDLE)(uintptr_t)lsiw_thread()->tid;
    c->recursion++;
    c->lock_count++;
}
API BOOL TryEnterCriticalSection(struct cs *c)
{
    if (pthread_mutex_trylock(cs_mutex(c))) return FALSE;
    c->owner = (HANDLE)(uintptr_t)lsiw_thread()->tid;
    c->recursion++;
    c->lock_count++;
    return TRUE;
}
API void LeaveCriticalSection(struct cs *c)
{
    c->lock_count--;
    if (--c->recursion <= 0) { c->recursion = 0; c->owner = NULL; }
    pthread_mutex_unlock(cs_mutex(c));
}

/* ---- Slim reader/writer locks, condition variables, init-once ---- */
static pthread_mutex_t g_lazy = PTHREAD_MUTEX_INITIALIZER;

static pthread_rwlock_t *srw(void **slot)
{
    pthread_rwlock_t *l = __atomic_load_n((pthread_rwlock_t **)slot, __ATOMIC_ACQUIRE);
    if (l) return l;
    pthread_mutex_lock(&g_lazy);
    if (!(l = *slot)) {
        l = malloc(sizeof *l);
        pthread_rwlock_init(l, NULL);
        __atomic_store_n((pthread_rwlock_t **)slot, l, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_lazy);
    return l;
}

API void InitializeSRWLock(void **l) { *l = NULL; }
API void AcquireSRWLockExclusive(void **l) { pthread_rwlock_wrlock(srw(l)); }
API void AcquireSRWLockShared(void **l) { pthread_rwlock_rdlock(srw(l)); }
API void ReleaseSRWLockExclusive(void **l) { pthread_rwlock_unlock(srw(l)); }
API void ReleaseSRWLockShared(void **l) { pthread_rwlock_unlock(srw(l)); }
API BOOL TryAcquireSRWLockExclusive(void **l) { return pthread_rwlock_trywrlock(srw(l)) == 0; }
API BOOL TryAcquireSRWLockShared(void **l) { return pthread_rwlock_tryrdlock(srw(l)) == 0; }

/* Condition variables use a generation counter on the global wait lock;
 * this works with both critical sections and SRW locks. */
API void InitializeConditionVariable(void **cv) { *cv = NULL; }

static BOOL cv_sleep(void **cv, DWORD ms, void (*unlock)(void *), void (*relock)(void *), void *lk)
{
    lsiw_wait_lock();
    uintptr_t gen = (uintptr_t)*cv;
    unlock(lk);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (ms != INFINITE) {
        dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    int timed_out = 0;
    while ((uintptr_t)*cv == gen) {
        if (lsiw_wait_cond(ms == INFINITE ? NULL : &dl) == ETIMEDOUT) { timed_out = 1; break; }
    }
    lsiw_wait_unlock();
    relock(lk);
    if (timed_out) { lsiw_set_last_error(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}

static void cs_unlock_fn(void *c) { LeaveCriticalSection(c); }
static void cs_lock_fn(void *c) { EnterCriticalSection(c); }
static void srw_x_unlock(void *l) { pthread_rwlock_unlock(srw(l)); }
static void srw_x_lock(void *l) { pthread_rwlock_wrlock(srw(l)); }
static void srw_s_lock(void *l) { pthread_rwlock_rdlock(srw(l)); }

API BOOL SleepConditionVariableCS(void **cv, struct cs *c, DWORD ms) { return cv_sleep(cv, ms, cs_unlock_fn, cs_lock_fn, c); }
API BOOL SleepConditionVariableSRW(void **cv, void **l, DWORD ms, ULONG flags)
{ return cv_sleep(cv, ms, srw_x_unlock, (flags & 1) ? srw_s_lock : srw_x_lock, l); }
API void WakeConditionVariable(void **cv) { lsiw_wait_lock(); *cv = (void *)((uintptr_t)*cv + 1); lsiw_wait_broadcast(); lsiw_wait_unlock(); }
API void WakeAllConditionVariable(void **cv) { WakeConditionVariable(cv); }

API BOOL WaitOnAddress(volatile void *addr, void *cmp, SIZE_T size, DWORD ms)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (ms != INFINITE) {
        dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    lsiw_wait_lock();
    while (!memcmp((const void *)addr, cmp, size)) {
        if (lsiw_wait_cond(ms == INFINITE ? NULL : &dl) == ETIMEDOUT) {
            lsiw_wait_unlock(); lsiw_set_last_error(ERROR_TIMEOUT); return FALSE;
        }
    }
    lsiw_wait_unlock();
    return TRUE;
}
API void WakeByAddressSingle(void *a) { (void)a; lsiw_wait_lock(); lsiw_wait_broadcast(); lsiw_wait_unlock(); }
API void WakeByAddressAll(void *a) { (void)a; lsiw_wait_lock(); lsiw_wait_broadcast(); lsiw_wait_unlock(); }

typedef BOOL (WINAPI *init_once_fn)(void *once, void *param, void **ctx);
API void InitOnceInitialize(void **once) { *once = NULL; }
API BOOL InitOnceExecuteOnce(void **once, init_once_fn fn, void *param, void **ctx)
{
    /* State in the pointer: 0 = new, 1 = running, 2 = done (low bits). */
    for (;;) {
        uintptr_t v = __atomic_load_n((uintptr_t *)once, __ATOMIC_ACQUIRE);
        if ((v & 3) == 2) { if (ctx) *ctx = (void *)(v & ~(uintptr_t)3); return TRUE; }
        uintptr_t z = 0;
        if (v == 0 && __atomic_compare_exchange_n((uintptr_t *)once, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            void *c = NULL;
            BOOL ok = fn(once, param, &c);
            __atomic_store_n((uintptr_t *)once, ok ? (((uintptr_t)c & ~(uintptr_t)3) | 2) : 0, __ATOMIC_RELEASE);
            if (ok && ctx) *ctx = c;
            return ok;
        }
        sched_yield();
    }
}
API BOOL InitOnceBeginInitialize(void **once, DWORD flags, BOOL *pending, void **ctx)
{
    (void)flags;
    for (;;) {
        uintptr_t v = __atomic_load_n((uintptr_t *)once, __ATOMIC_ACQUIRE);
        if ((v & 3) == 2) { *pending = FALSE; if (ctx) *ctx = (void *)(v & ~(uintptr_t)3); return TRUE; }
        uintptr_t z = 0;
        if (v == 0 && __atomic_compare_exchange_n((uintptr_t *)once, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *pending = TRUE; return TRUE;
        }
        if (flags & 2 /* INIT_ONCE_CHECK_ONLY */) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        sched_yield();
    }
}
API BOOL InitOnceComplete(void **once, DWORD flags, void *ctx)
{
    if (flags & 4 /* INIT_ONCE_INIT_FAILED */) __atomic_store_n((uintptr_t *)once, 0, __ATOMIC_RELEASE);
    else __atomic_store_n((uintptr_t *)once, ((uintptr_t)ctx & ~(uintptr_t)3) | 2, __ATOMIC_RELEASE);
    return TRUE;
}

/* ================================================================== */
/* Threads                                                              */
/* ================================================================== */

typedef DWORD (WINAPI *thread_fn)(void *);
struct thread_start { thread_fn fn; void *param; struct lsiw_handle_obj *obj; HANDLE h; };

static void thread_finish(struct lsiw_handle_obj *o, uint32_t code)
{
    lsiw_dll_notify_threads(3 /* DLL_THREAD_DETACH */);
    lsiw_teb_free_thread_data();
    lsiw_wait_lock();
    o->exit_code = code;
    o->signaled = 1;
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
}

static void *thread_main(void *arg)
{
    struct thread_start s = *(struct thread_start *)arg;
    free(arg);
    if (!lsiw_teb_create()) return NULL;
    struct lsiw_thread_priv *priv = lsiw_thread();
    priv->self = s.obj;
    lsiw_wait_lock();
    s.obj->tid = priv->tid;
    lsiw_wait_broadcast();
    while (s.obj->suspended) lsiw_wait_cond(NULL);
    lsiw_wait_unlock();
    lsiw_dll_notify_threads(2 /* DLL_THREAD_ATTACH */);
    jmp_buf jb;
    priv->jmp = &jb;
    uint32_t code;
    if (!setjmp(jb)) code = s.fn(s.param);
    else code = s.obj->exit_code;
    thread_finish(s.obj, code);
    return NULL;
}

API HANDLE CreateThread(void *sa, SIZE_T stack, thread_fn fn, void *param, DWORD flags, DWORD *tid)
{
    (void)sa;
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_THREAD, &o);
    if (!h) return NULL;
    o->suspended = (flags & 4) != 0;          /* CREATE_SUSPENDED */
    o->refs++;                                 /* the running thread holds a reference */
    struct thread_start *s = malloc(sizeof *s);
    *s = (struct thread_start){ fn, param, o, h };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t sz = stack ? stack : 0x100000 * 4;  /* Windows default is 1 MiB; be generous */
    if (sz < 0x40000) sz = 0x40000;
    pthread_attr_setstacksize(&attr, sz);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    o->pthread = malloc(sizeof(pthread_t));
    int r = pthread_create((pthread_t *)o->pthread, &attr, thread_main, s);
    pthread_attr_destroy(&attr);
    if (r) { free(s); lsiw_handle_close(h); lsiw_set_errno_error(r); return NULL; }
    lsiw_wait_lock();
    while (!o->tid) lsiw_wait_cond(NULL);
    lsiw_wait_unlock();
    if (tid) *tid = o->tid;
    return h;
}

API HANDLE CreateRemoteThread(HANDLE proc, void *sa, SIZE_T stack, thread_fn fn, void *param, DWORD flags, DWORD *tid)
{
    if (proc != (HANDLE)(intptr_t)-1) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    return CreateThread(sa, stack, fn, param, flags, tid);
}

API HANDLE CreateRemoteThreadEx(HANDLE proc, void *sa, SIZE_T stack, thread_fn fn, void *param, DWORD flags, void *attrs, DWORD *tid)
{ (void)attrs; return CreateRemoteThread(proc, sa, stack, fn, param, flags, tid); }

API void ExitThread(DWORD code)
{
    struct lsiw_thread_priv *priv = lsiw_thread();
    if (!priv->self || !priv->jmp) lsiw_exit_process(code);   /* main thread */
    priv->self->exit_code = code;
    longjmp(*(jmp_buf *)priv->jmp, 1);
}

API DWORD ResumeThread(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
    if (!o) return (DWORD)-1;
    lsiw_wait_lock();
    DWORD prev = (DWORD)o->suspended;
    if (o->suspended) o->suspended--;
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
    return prev;
}

API DWORD SuspendThread(HANDLE h)
{
    (void)h;
    /* Suspending a running thread at an arbitrary point is not supported. */
    lsiw_set_last_error(ERROR_NOT_SUPPORTED);
    return (DWORD)-1;
}

API BOOL TerminateThread(HANDLE h, DWORD code)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
    if (o && o->tid == lsiw_thread()->tid) ExitThread(code);
    lsiw_set_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

API BOOL GetExitCodeThread(HANDLE h, DWORD *code)
{
    if (h == (HANDLE)(intptr_t)-2) { *code = 259; return TRUE; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *code = o->signaled ? o->exit_code : 259 /* STILL_ACTIVE */;
    return TRUE;
}

API HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
API HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
API DWORD  GetCurrentThreadId(void) { return lsiw_thread()->tid; }
API DWORD  GetCurrentProcessId(void) { return (DWORD)getpid(); }
API DWORD  GetThreadId(HANDLE h)
{
    if (h == (HANDLE)(intptr_t)-2) return lsiw_thread()->tid;
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
    return o ? o->tid : 0;
}
API DWORD GetProcessId(HANDLE h)
{
    if (h == (HANDLE)(intptr_t)-1) return (DWORD)getpid();
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_PROCESS);
    return o ? (DWORD)o->pid : 0;
}
API DWORD GetProcessIdOfThread(HANDLE h) { (void)h; return (DWORD)getpid(); }
/* Thread priorities are remembered as set; the Linux nice value follows on a
 * best-effort basis (raising priority needs CAP_SYS_NICE, so it may not). */
static int priority_nice(int p)
{
    if (p <= -15) return 19;
    if (p >= 15) return -20;
    static const int map[] = { 10, 5, 0, -5, -10 };   /* -2 .. 2 */
    return p < -2 ? 10 : p > 2 ? -10 : map[p + 2];
}
API BOOL SetThreadPriority(HANDLE h, int p)
{
    if (!(p >= -2 && p <= 2) && p != -15 && p != 15) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uint32_t tid;
    if (h == (HANDLE)(intptr_t)-2) { lsiw_thread()->priority = p; tid = lsiw_thread()->tid; }
    else {
        struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
        if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
        o->priority = p;
        tid = o->tid;
        if (tid == lsiw_thread()->tid) lsiw_thread()->priority = p;
    }
    if (tid) setpriority(PRIO_PROCESS, (id_t)tid, priority_nice(p));
    return TRUE;
}
API int GetThreadPriority(HANDLE h)
{
    if (h == (HANDLE)(intptr_t)-2) return lsiw_thread()->priority;
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0x7fffffff; /* THREAD_PRIORITY_ERROR_RETURN */ }
    return o->tid == lsiw_thread()->tid ? lsiw_thread()->priority : o->priority;
}
API BOOL  SetThreadPriorityBoost(HANDLE h, BOOL d) { (void)h; (void)d; return TRUE; }
API BOOL  SetPriorityClass(HANDLE h, DWORD c) { (void)h; (void)c; return TRUE; }
API DWORD GetPriorityClass(HANDLE h) { (void)h; return 0x20; /* NORMAL_PRIORITY_CLASS */ }
API ULONG_PTR SetThreadAffinityMask(HANDLE h, ULONG_PTR m) { (void)h; (void)m; return 1; }
API BOOL  SetProcessAffinityMask(HANDLE h, ULONG_PTR m) { (void)h; (void)m; return TRUE; }
API BOOL  GetProcessAffinityMask(HANDLE h, ULONG_PTR *p, ULONG_PTR *s)
{
    (void)h;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    ULONG_PTR m = n >= 64 ? ~(ULONG_PTR)0 : (((ULONG_PTR)1 << n) - 1);
    if (p) *p = m;
    if (s) *s = m;
    return TRUE;
}
API DWORD SetThreadIdealProcessor(HANDLE h, DWORD p) { (void)h; (void)p; return 0; }
API BOOL  SwitchToThread(void) { sched_yield(); return TRUE; }
API LONG  SetThreadDescription(HANDLE h, const WCHAR *d)
{
    if (h == (HANDLE)(intptr_t)-2 && d) {
        char *n = lsiw_w2u(d);
        char s[16];
        snprintf(s, sizeof s, "%s", n);
        pthread_setname_np(pthread_self(), s);
        free(n);
    }
    return 0;
}
API LONG GetThreadDescription(HANDLE h, WCHAR **d) { (void)h; *d = calloc(1, sizeof(WCHAR)); return 0; }

static void filetime_from_timeval(struct timeval tv, uint64_t *ft) { *ft = (uint64_t)tv.tv_sec * 10000000ULL + (uint64_t)tv.tv_usec * 10; }

API BOOL GetProcessTimes(HANDLE h, uint64_t *create, uint64_t *exit_t, uint64_t *kernel, uint64_t *user)
{
    (void)h;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    static uint64_t start;
    if (!start) start = now_100ns();
    if (create) *create = start;
    if (exit_t) *exit_t = 0;
    if (kernel) filetime_from_timeval(ru.ru_stime, kernel);
    if (user) filetime_from_timeval(ru.ru_utime, user);
    return TRUE;
}

API BOOL GetThreadTimes(HANDLE h, uint64_t *create, uint64_t *exit_t, uint64_t *kernel, uint64_t *user)
{
    (void)h;
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    if (create) *create = now_100ns();
    if (exit_t) *exit_t = 0;
    if (kernel) filetime_from_timeval(ru.ru_stime, kernel);
    if (user) filetime_from_timeval(ru.ru_utime, user);
    return TRUE;
}

API BOOL QueryThreadCycleTime(HANDLE h, uint64_t *c) { (void)h; struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); *c = (uint64_t)ts.tv_sec * 3000000000ULL + (uint64_t)ts.tv_nsec * 3; return TRUE; }
API BOOL QueryProcessCycleTime(HANDLE h, uint64_t *c) { (void)h; struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); *c = (uint64_t)ts.tv_sec * 3000000000ULL + (uint64_t)ts.tv_nsec * 3; return TRUE; }

API void Sleep(DWORD ms)
{
    if (ms == INFINITE) for (;;) pause();
    if (!ms) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) ;
}
API DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (!alertable) { Sleep(ms); return 0; }
    DWORD r = wait_objects(0, NULL, FALSE, ms, TRUE);
    return r == WAIT_IO_COMPLETION ? r : 0;
}

/* ---- TLS / FLS ---- */
/* Slot 0 is taken, as it is on Windows by the time a program runs (system
 * DLLs allocate first); some runtimes treat index 0 as "no key". */
static uint8_t g_tls_used[TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS] = { 1 };
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;

API DWORD TlsAlloc(void)
{
    pthread_mutex_lock(&g_tls_lock);
    for (DWORD i = 0; i < sizeof g_tls_used; i++)
        if (!g_tls_used[i]) { g_tls_used[i] = 1; pthread_mutex_unlock(&g_tls_lock); return i; }
    pthread_mutex_unlock(&g_tls_lock);
    lsiw_set_last_error(ERROR_NO_MORE_ITEMS);
    return 0xFFFFFFFF;
}

static void **tls_slot(DWORD i, int create)
{
    uint8_t *teb = lsiw_teb_current();
    if (!teb) return NULL;
    if (i < TLS_MINIMUM_AVAILABLE) return (void **)(teb + TEB_TLS_SLOTS) + i;
    if (i >= TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS) return NULL;
    void ***exp = (void ***)(teb + TEB_TLS_EXP);
    if (!*exp) { if (!create) return NULL; *exp = calloc(TLS_EXPANSION_SLOTS, sizeof(void *)); }
    return *exp + (i - TLS_MINIMUM_AVAILABLE);
}

API BOOL TlsFree(DWORD i)
{
    if (i >= sizeof g_tls_used || !g_tls_used[i]) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    g_tls_used[i] = 0;
    void **s = tls_slot(i, 0);
    if (s) *s = NULL;
    return TRUE;
}

API void *TlsGetValue(DWORD i)
{
    void **s = tls_slot(i, 0);
    lsiw_set_last_error(ERROR_SUCCESS);
    return s ? *s : NULL;
}

API BOOL TlsSetValue(DWORD i, void *v)
{
    void **s = tls_slot(i, 1);
    if (!s) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *s = v;
    return TRUE;
}

typedef void (WINAPI *fls_cb)(void *);
static fls_cb g_fls_cb[128];
static uint8_t g_fls_used[128];

API DWORD FlsAlloc(fls_cb cb)
{
    pthread_mutex_lock(&g_tls_lock);
    for (DWORD i = 1; i < 128; i++)
        if (!g_fls_used[i]) { g_fls_used[i] = 1; g_fls_cb[i] = cb; pthread_mutex_unlock(&g_tls_lock); return i; }
    pthread_mutex_unlock(&g_tls_lock);
    lsiw_set_last_error(ERROR_NO_MORE_ITEMS);
    return 0xFFFFFFFF;
}
API BOOL FlsFree(DWORD i)
{
    if (i >= 128 || !g_fls_used[i]) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    void *v = lsiw_thread()->fls[i];
    if (v && g_fls_cb[i]) g_fls_cb[i](v);
    lsiw_thread()->fls[i] = NULL;
    g_fls_used[i] = 0;
    return TRUE;
}
API void *FlsGetValue(DWORD i) { if (i >= 128) return NULL; lsiw_set_last_error(0); return lsiw_thread()->fls[i]; }
API BOOL  FlsSetValue(DWORD i, void *v) { if (i >= 128) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; } lsiw_thread()->fls[i] = v; return TRUE; }
API BOOL  IsThreadAFiber(void) { uint8_t *t = lsiw_teb_current(); return t && *(void **)(t + 0x20) != NULL; }

void lsiw_teb_free_thread_data(void)
{
    struct lsiw_thread_priv *p = lsiw_thread();
    for (int i = 1; i < 128; i++) {
        void *v = p->fls[i];
        if (v && g_fls_used[i] && g_fls_cb[i]) { p->fls[i] = NULL; g_fls_cb[i](v); }
    }
}

/* ================================================================== */
/* Child processes                                                      */
/* ================================================================== */

struct startupinfo {
    DWORD cb; void *reserved; void *desktop; void *title;
    DWORD x, y, xs, ys, xcc, ycc, fill, flags; WORD show, r2; void *r3;
    HANDLE in, out, err;
};
struct procinfo { HANDLE process, thread; DWORD pid, tid; };

/* Split a Windows command line the way CommandLineToArgvW does. */
static char **split_cmdline(const char *cmd, int *argc_out)
{
    int cap = 16, argc = 0;
    char **argv = malloc((size_t)cap * sizeof(char *));
    const char *p = cmd;
    size_t bl = strlen(cmd) + 1;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *arg = malloc(bl), *o = arg;
        int inq = 0;
        while (*p && (inq || (*p != ' ' && *p != '\t'))) {
            int bs = 0;
            while (*p == '\\') { p++; bs++; }
            if (*p == '"') {
                for (int i = 0; i < bs / 2; i++) *o++ = '\\';
                if (bs % 2) { *o++ = '"'; p++; }
                else { if (inq && p[1] == '"') { *o++ = '"'; p++; } else inq = !inq; p++; }
            } else {
                for (int i = 0; i < bs; i++) *o++ = '\\';
                if (*p && (inq || (*p != ' ' && *p != '\t'))) *o++ = *p++;
            }
        }
        *o = 0;
        if (argc + 2 > cap) { cap *= 2; argv = realloc(argv, (size_t)cap * sizeof(char *)); }
        argv[argc++] = arg;
    }
    argv[argc] = NULL;
    *argc_out = argc;
    return argv;
}

static char *find_program(const char *name)
{
    char *lin = lsiw_win_to_linux(name);
    struct stat st;
    if (stat(lin, &st) == 0 && S_ISREG(st.st_mode)) return lin;
    char *withexe = lsiw_strdup_printf("%s.exe", lin);
    free(lin);
    if (stat(withexe, &st) == 0 && S_ISREG(st.st_mode)) return withexe;
    free(withexe);
    if (strchr(name, '\\') || strchr(name, '/')) return NULL;
    /* Search the program's folder, then PATH. */
    const char *dirs[2] = { NULL, lsiw_env_get("PATH") };
    char *exe_dir = NULL;
    if (lsiw_main_module && lsiw_main_module->linux_path) {
        exe_dir = strdup(lsiw_main_module->linux_path);
        char *s = strrchr(exe_dir, '/'); if (s) *s = 0;
        dirs[0] = exe_dir;
    }
    char *found = NULL;
    for (int d = 0; d < 2 && !found; d++) {
        if (!dirs[d]) continue;
        char *copy = strdup(dirs[d]), *save = NULL;
        const char *sep = d == 0 ? "\n" : ";";
        for (char *e = strtok_r(copy, sep, &save); e && !found; e = strtok_r(NULL, sep, &save)) {
            char *dir = d == 0 ? strdup(e) : lsiw_win_to_linux(e);
            const char *exts[] = { "", ".exe", ".com", NULL };
            for (int k = 0; exts[k] && !found; k++) {
                char *c = lsiw_strdup_printf("%s/%s%s", dir, name, exts[k]);
                if (stat(c, &st) == 0 && S_ISREG(st.st_mode)) found = c; else free(c);
            }
            free(dir);
        }
        free(copy);
    }
    free(exe_dir);
    return found;
}

static int is_pe_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char mz[2] = {0};
    ssize_t r = read(fd, mz, 2);
    close(fd);
    return r == 2 && mz[0] == 'M' && mz[1] == 'Z';
}

static BOOL create_process(const char *app, const char *cmdline, char **envp, const char *cwd_win,
                           const struct startupinfo *si, struct procinfo *pi)
{
    int argc = 0;
    char **wargv = split_cmdline(cmdline ? cmdline : (app ? app : ""), &argc);
    const char *prog = app ? app : (argc ? wargv[0] : NULL);
    if (!prog) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *path = find_program(prog);
    if (!path) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }

    char **argv;
    char *self = realpath("/proc/self/exe", NULL);
    int pe = is_pe_file(path);
    if (pe) {
        /* Another Windows program: run it through LSIW with the exact command line. */
        argv = calloc(4, sizeof(char *));
        argv[0] = self ? self : strdup("lsiw");
        argv[1] = strdup("run");
        argv[2] = path;
    } else {
        /* A Linux program reached through its C:\ path: pass the arguments on. */
        argv = calloc((size_t)argc + 2, sizeof(char *));
        argv[0] = path;
        for (int i = 1; i < argc; i++) argv[i] = wargv[i];
        free(self);
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (si && (si->flags & 0x100 /* STARTF_USESTDHANDLES */)) {
        HANDLE hs[3] = { si->in, si->out, si->err };
        for (int i = 0; i < 3; i++) {
            int fd = lsiw_fd_from_handle(hs[i]);
            if (fd >= 0) posix_spawn_file_actions_adddup2(&fa, fd, i);   /* fd == i: clears close-on-exec */
        }
    }
    if (cwd_win) {
        char *cwd = lsiw_win_to_linux(cwd_win);
        posix_spawn_file_actions_addchdir_np(&fa, cwd);
        free(cwd);
    }

    /* The child inherits the Windows environment (the caller's block, or
     * ours) through its Linux environment. HOME and PATH are given Linux
     * values there; a Windows child gets the Windows ones on the side. */
    char **e = envp ? envp : lsiw_env_block();
    size_t n = 0; while (e && e[n]) n++;
    char **env = calloc(n + 10, sizeof(char *));
    char **owned = calloc(n + 10, sizeof(char *));
    size_t k = 0, no = 0;
    const char *win_home = NULL, *win_path = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!strncasecmp(e[i], "HOME=", 5)) { win_home = e[i] + 5; continue; }
        if (!strncasecmp(e[i], "PATH=", 5)) { win_path = e[i] + 5; continue; }
        env[k++] = e[i];
    }
    const char *home = lsiw_home_dir();
    env[k++] = owned[no++] = lsiw_strdup_printf("HOME=%s", home);
    if (win_path) { char *lp = win_pathlist_to_linux(win_path); env[k++] = owned[no++] = lsiw_strdup_printf("PATH=%s", lp); free(lp); }
    int xp[2] = { -1, -1 };
    if (pe) {
        env[k++] = "LSIW_FROM_WINDOWS=1";
        if (win_home) env[k++] = owned[no++] = lsiw_strdup_printf("LSIW_WIN_HOME=%s", win_home);
        if (win_path) env[k++] = owned[no++] = lsiw_strdup_printf("LSIW_WIN_PATH=%s", win_path);
        if (cmdline || prog) env[k++] = owned[no++] = lsiw_strdup_printf("LSIW_CMDLINE=%s", cmdline ? cmdline : prog);
        if (cwd_win) env[k++] = owned[no++] = lsiw_strdup_printf("LSIW_CWD=%s", cwd_win);
        if (pipe2(xp, O_CLOEXEC) == 0) {
            posix_spawn_file_actions_adddup2(&fa, xp[1], xp[1]);   /* same fd: clears close-on-exec in the child */
            env[k++] = owned[no++] = lsiw_strdup_printf("LSIW_EXIT_FD=%d", xp[1]);
        }
    }
    env[k] = NULL;
    pid_t pid;
    int r = posix_spawn(&pid, argv[0], &fa, NULL, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    unsetenv("LSIW_CMDLINE");
    for (size_t i = 0; i < no; i++) free(owned[i]);
    free(owned); free(env);
    if (xp[1] >= 0) close(xp[1]);
    if (r) { if (xp[0] >= 0) close(xp[0]); lsiw_set_errno_error(r); return FALSE; }

    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_PROCESS, &o);
    o->pid = pid;
    o->fd = xp[0];
    HANDLE th = lsiw_handle_dup(h);
    if (pi) { pi->process = h; pi->thread = th; pi->pid = (DWORD)pid; pi->tid = (DWORD)pid; }
    return TRUE;
}

static char **env_from_block_a(const char *blk)
{
    if (!blk) return NULL;
    size_t n = 0;
    for (const char *p = blk; *p; p += strlen(p) + 1) n++;
    char **env = calloc(n + 2, sizeof(char *));
    size_t i = 0;
    for (const char *p = blk; *p; p += strlen(p) + 1) env[i++] = strdup(p);
    return env;
}

static char **env_from_block_w(const WCHAR *blk)
{
    if (!blk) return NULL;
    size_t n = 0;
    for (const WCHAR *p = blk; *p; p += lsiw_wcslen(p) + 1) n++;
    char **env = calloc(n + 2, sizeof(char *));
    size_t i = 0;
    for (const WCHAR *p = blk; *p; p += lsiw_wcslen(p) + 1) env[i++] = lsiw_w2u(p);
    return env;
}

API BOOL CreateProcessA(const char *app, char *cmd, void *psa, void *tsa, BOOL inherit, DWORD flags,
                        void *env, const char *cwd, struct startupinfo *si, struct procinfo *pi)
{
    (void)psa; (void)tsa; (void)inherit;
    char **e = (flags & 0x400) ? env_from_block_w(env) : env_from_block_a(env);
    return create_process(app, cmd, e, cwd, si, pi);
}

API BOOL CreateProcessW(const WCHAR *app, WCHAR *cmd, void *psa, void *tsa, BOOL inherit, DWORD flags,
                        void *env, const WCHAR *cwd, struct startupinfo *si, struct procinfo *pi)
{
    (void)psa; (void)tsa; (void)inherit;
    char *a = lsiw_w2u(app), *c = lsiw_w2u(cmd), *d = lsiw_w2u(cwd);
    char **e = (flags & 0x400) ? env_from_block_w(env) : env_from_block_a(env);
    BOOL r = create_process(a, c, e, d, si, pi);
    free(a); free(c); free(d);
    return r;
}

API BOOL GetExitCodeProcess(HANDLE h, DWORD *code)
{
    if (h == (HANDLE)(intptr_t)-1) { *code = 259; return TRUE; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_PROCESS);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_wait_lock();
    obj_ready(o, 0, 0, NULL);
    *code = o->signaled ? o->exit_code : 259;
    lsiw_wait_unlock();
    return TRUE;
}

API HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    (void)access; (void)inherit;
    if (pid == (DWORD)getpid()) return (HANDLE)(intptr_t)-1;
    if (kill((pid_t)pid, 0) < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_PROCESS, &o);
    if (h) { o->pid = (int)pid; o->reaped = 0; }
    return h;
}

API BOOL CreatePipe(HANDLE *r, HANDLE *w, void *sa, DWORD size)
{
    (void)sa; (void)size;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) { lsiw_set_errno_error(errno); return FALSE; }
    *r = lsiw_handle_from_fd(fds[0], 0);
    *w = lsiw_handle_from_fd(fds[1], 0);
    return TRUE;
}

API UINT WinExec(const char *cmd, UINT show)
{
    (void)show;
    struct procinfo pi;
    if (!create_process(NULL, cmd, NULL, NULL, NULL, &pi)) return 2;
    CloseHandle(pi.process); CloseHandle(pi.thread);
    return 33;
}

API void GetStartupInfoA(struct startupinfo *si)
{
    memset(si, 0, sizeof *si);
    si->cb = sizeof *si;
    si->show = 1; /* SW_SHOWNORMAL */
}
API void GetStartupInfoW(struct startupinfo *si) { GetStartupInfoA(si); }

/* ================================================================== */
/* Modules and resources                                                */
/* ================================================================== */

static HMODULE module_handle(const char *name)
{
    if (!name) return lsiw_main_module ? (HMODULE)lsiw_main_module->base : NULL;
    struct lsiw_module *m = lsiw_find_module(name);
    if (m) return m->base;
    const struct lsiw_builtin_dll *b = lsiw_builtin_find(name);
    if (!b) {
        char buf[300];
        snprintf(buf, sizeof buf, "%s.dll", name);
        b = strchr(name, '.') ? NULL : lsiw_builtin_find(buf);
    }
    if (b) { m = lsiw_load_library(name, NULL); if (m) return m->base; }
    lsiw_set_last_error(ERROR_MOD_NOT_FOUND);
    return NULL;
}

API HMODULE GetModuleHandleA(const char *name) { return module_handle(name); }
API HMODULE GetModuleHandleW(const WCHAR *name) { char *n = lsiw_w2u(name); HMODULE h = module_handle(n); free(n); return h; }

API BOOL GetModuleHandleExA(DWORD flags, const char *name, HMODULE *out)
{
    if (flags & 4 /* FROM_ADDRESS */) {
        struct lsiw_module *m = lsiw_module_from_address(name);
        *out = m ? m->base : NULL;
    } else *out = module_handle(name);
    if (!*out) { lsiw_set_last_error(ERROR_MOD_NOT_FOUND); return FALSE; }
    return TRUE;
}
API BOOL GetModuleHandleExW(DWORD flags, const WCHAR *name, HMODULE *out)
{
    if (flags & 4) return GetModuleHandleExA(flags, (const char *)name, out);
    char *n = lsiw_w2u(name);
    BOOL r = GetModuleHandleExA(flags, n, out);
    free(n);
    return r;
}

static const char *module_path(HMODULE h)
{
    struct lsiw_module *m = lsiw_module_from_handle(h);
    if (!m) {
        for (struct lsiw_module *x = lsiw_modules; x; x = x->next)
            if (x->base == h) { m = x; break; }
    }
    return m ? m->win_path : NULL;
}

API DWORD GetModuleFileNameA(HMODULE h, char *buf, DWORD size)
{
    const char *p = module_path(h);
    if (!p) { lsiw_set_last_error(ERROR_MOD_NOT_FOUND); return 0; }
    size_t l = strlen(p);
    if (!size) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (l >= size) { memcpy(buf, p, size - 1); buf[size - 1] = 0; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return size; }
    memcpy(buf, p, l + 1);
    lsiw_set_last_error(0);
    return (DWORD)l;
}

API DWORD GetModuleFileNameW(HMODULE h, WCHAR *buf, DWORD size)
{
    const char *p = module_path(h);
    if (!p) { lsiw_set_last_error(ERROR_MOD_NOT_FOUND); return 0; }
    WCHAR *w = lsiw_u2w(p);
    size_t l = lsiw_wcslen(w);
    if (!size) { free(w); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (l >= size) { memcpy(buf, w, (size - 1) * 2); buf[size - 1] = 0; free(w); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return size; }
    memcpy(buf, w, (l + 1) * 2);
    free(w);
    lsiw_set_last_error(0);
    return (DWORD)l;
}

API DWORD K32GetModuleFileNameExA(HANDLE p, HMODULE h, char *buf, DWORD size) { (void)p; return GetModuleFileNameA(h, buf, size); }
API DWORD K32GetModuleFileNameExW(HANDLE p, HMODULE h, WCHAR *buf, DWORD size) { (void)p; return GetModuleFileNameW(h, buf, size); }
API DWORD K32GetModuleBaseNameW(HANDLE p, HMODULE h, WCHAR *buf, DWORD size)
{
    (void)p;
    struct lsiw_module *m = lsiw_module_from_handle(h);
    if (!m) return 0;
    return lsiw_ret_str_w(m->name, buf, size);
}
struct modinfo { void *base; DWORD size; void *entry; };
API BOOL K32GetModuleInformation(HANDLE p, HMODULE h, struct modinfo *mi, DWORD cb)
{
    (void)p; (void)cb;
    struct lsiw_module *m = lsiw_module_from_handle(h);
    if (!m) return FALSE;
    mi->base = m->base; mi->size = (DWORD)m->size; mi->entry = m->entry;
    return TRUE;
}
API BOOL K32EnumProcessModules(HANDLE p, HMODULE *mods, DWORD cb, DWORD *needed)
{
    (void)p;
    DWORD n = 0;
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
        if ((n + 1) * sizeof(HMODULE) <= cb) mods[n] = m->base;
        n++;
    }
    *needed = n * (DWORD)sizeof(HMODULE);
    return TRUE;
}

static HMODULE load_library(const char *name)
{
    if (!name || !*name) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    struct lsiw_module *m = lsiw_load_library(name, NULL);
    if (!m) { LSIW_TRACE("LoadLibrary(%s) failed\n", name); lsiw_set_last_error(ERROR_MOD_NOT_FOUND); return NULL; }
    return m->base;
}

API HMODULE LoadLibraryA(const char *name) { return load_library(name); }
API HMODULE LoadLibraryW(const WCHAR *name) { char *n = lsiw_w2u(name); HMODULE h = load_library(n); free(n); return h; }
API HMODULE LoadLibraryExA(const char *name, HANDLE f, DWORD flags) { (void)f; (void)flags; return load_library(name); }
API HMODULE LoadLibraryExW(const WCHAR *name, HANDLE f, DWORD flags) { (void)f; (void)flags; return LoadLibraryW(name); }
API BOOL FreeLibrary(HMODULE h) { (void)h; return TRUE; }
API void FreeLibraryAndExitThread(HMODULE h, DWORD code) { (void)h; ExitThread(code); }
API BOOL DisableThreadLibraryCalls(HMODULE h) { lsiw_disable_thread_calls(h); return TRUE; }
API BOOL SetDllDirectoryA(const char *d) { (void)d; return TRUE; }
API BOOL SetDllDirectoryW(const WCHAR *d) { (void)d; return TRUE; }
API void *AddDllDirectory(const WCHAR *d) { (void)d; return (void *)1; }
API BOOL RemoveDllDirectory(void *c) { (void)c; return TRUE; }
API BOOL SetDefaultDllDirectories(DWORD f) { (void)f; return TRUE; }

API void *GetProcAddress(HMODULE h, const char *name)
{
    struct lsiw_module *m = NULL;
    for (struct lsiw_module *x = lsiw_modules; x; x = x->next) if (x->base == h) { m = x; break; }
    if (!m && !h) m = lsiw_main_module;
    if (!m) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    void *p;
    if ((uintptr_t)name < 0x10000) {
        if (m->is_builtin) {                      /* builtin tables list ordinals as "#n" */
            char ord[16];
            snprintf(ord, sizeof ord, "#%u", (unsigned)(uintptr_t)name);
            p = lsiw_get_export(m, ord, 0);
        } else p = lsiw_get_export(m, NULL, (int)(uintptr_t)name);
    } else {
        p = lsiw_get_export(m, name, 0);
    }
    if (m->is_builtin) p = lsiw_trace_wrap(m->name, (uintptr_t)name < 0x10000 ? "#ordinal" : name, p);
    if (!p) { LSIW_TRACE("GetProcAddress(%s, %s) -> not found\n", m->name, (uintptr_t)name < 0x10000 ? "#ordinal" : name); lsiw_set_last_error(ERROR_PROC_NOT_FOUND); }
    return p;
}

/* Resources: walk the three-level .rsrc tree (type / name / language). */
static IMAGE_RESOURCE_DIRECTORY_ENTRY *rsrc_find(uint8_t *root, IMAGE_RESOURCE_DIRECTORY *dir, const void *id, int is_wide)
{
    IMAGE_RESOURCE_DIRECTORY_ENTRY *e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    int n = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    if (!id) return n ? &e[0] : NULL;                          /* any (language) */
    if ((uintptr_t)id < 0x10000) {
        for (int i = 0; i < n; i++)
            if (!(e[i].Name & 0x80000000) && e[i].Name == (uint32_t)(uintptr_t)id) return &e[i];
        return NULL;
    }
    char want[256];
    if (is_wide) { char *u = lsiw_w2u(id); snprintf(want, sizeof want, "%s", u); free(u); }
    else snprintf(want, sizeof want, "%s", (const char *)id);
    if (want[0] == '#') {
        return rsrc_find(root, dir, (const void *)(uintptr_t)atoi(want + 1), 0);
    }
    for (int i = 0; i < n; i++) {
        if (!(e[i].Name & 0x80000000)) continue;
        uint16_t *s = (uint16_t *)(root + (e[i].Name & 0x7fffffff));
        char *u = lsiw_w2u_n(s + 1, s[0]);
        int eq = !strcasecmp(u, want);
        free(u);
        if (eq) return &e[i];
    }
    return NULL;
}

static void *find_resource(HMODULE h, const void *name, const void *type, int wide)
{
    struct lsiw_module *m = h ? NULL : lsiw_main_module;
    for (struct lsiw_module *x = lsiw_modules; x && !m; x = x->next) if (x->base == h) m = x;
    if (!m || m->is_builtin) { lsiw_set_last_error(1813 /* ERROR_RESOURCE_TYPE_NOT_FOUND */); return NULL; }
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(m->base + ((IMAGE_DOS_HEADER *)m->base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (!dd.VirtualAddress) { lsiw_set_last_error(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */); return NULL; }
    uint8_t *root = m->base + dd.VirtualAddress;
    IMAGE_RESOURCE_DIRECTORY_ENTRY *e = rsrc_find(root, (IMAGE_RESOURCE_DIRECTORY *)root, type, wide);
    if (!e || !(e->OffsetToData & 0x80000000)) { lsiw_set_last_error(1813); return NULL; }
    e = rsrc_find(root, (IMAGE_RESOURCE_DIRECTORY *)(root + (e->OffsetToData & 0x7fffffff)), name, wide);
    if (!e || !(e->OffsetToData & 0x80000000)) { lsiw_set_last_error(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return NULL; }
    e = rsrc_find(root, (IMAGE_RESOURCE_DIRECTORY *)(root + (e->OffsetToData & 0x7fffffff)), NULL, 0);
    if (!e || (e->OffsetToData & 0x80000000)) { lsiw_set_last_error(1815); return NULL; }
    return root + e->OffsetToData;   /* IMAGE_RESOURCE_DATA_ENTRY* is the HRSRC */
}

API void *FindResourceA(HMODULE h, const char *name, const char *type) { return find_resource(h, name, type, 0); }
API void *FindResourceW(HMODULE h, const WCHAR *name, const WCHAR *type) { return find_resource(h, name, type, 1); }
API void *FindResourceExA(HMODULE h, const char *type, const char *name, WORD lang) { (void)lang; return find_resource(h, name, type, 0); }
API void *FindResourceExW(HMODULE h, const WCHAR *type, const WCHAR *name, WORD lang) { (void)lang; return find_resource(h, name, type, 1); }
API void *LoadResource(HMODULE h, void *res)
{
    if (!res) return NULL;
    struct lsiw_module *m = h ? NULL : lsiw_main_module;
    for (struct lsiw_module *x = lsiw_modules; x && !m; x = x->next) if (x->base == h) m = x;
    if (!m) m = lsiw_module_from_address(res);
    if (!m) return NULL;
    return m->base + ((IMAGE_RESOURCE_DATA_ENTRY *)res)->OffsetToData;
}
API void *LockResource(void *p) { return p; }
API DWORD SizeofResource(HMODULE h, void *res) { (void)h; return res ? ((IMAGE_RESOURCE_DATA_ENTRY *)res)->Size : 0; }
API BOOL FreeResource(void *p) { (void)p; return FALSE; }

/* LoadString lives in user32 but needs the resource walker. */
int lsiw_load_string(HMODULE h, UINT id, WCHAR **out)
{
    void *res = find_resource(h, (const void *)(uintptr_t)((id >> 4) + 1), (const void *)(uintptr_t)6 /* RT_STRING */, 1);
    if (!res) return -1;
    WCHAR *p = LoadResource(h, res);
    if (!p) return -1;
    for (UINT i = 0; i < (id & 15); i++) p += *p + 1;
    *out = p;
    return 0;
}

/* ================================================================== */
/* System information                                                   */
/* ================================================================== */

struct sysinfo_w {
    WORD arch; WORD reserved; DWORD page_size; void *min_addr; void *max_addr;
    ULONG_PTR active_mask; DWORD nprocs; DWORD proc_type; DWORD alloc_gran;
    WORD level; WORD revision;
};

API void GetSystemInfo(struct sysinfo_w *si)
{
    memset(si, 0, sizeof *si);
    si->arch = 9;                     /* PROCESSOR_ARCHITECTURE_AMD64 */
    si->page_size = 4096;
    si->min_addr = (void *)0x10000;
    si->max_addr = (void *)0x7FFFFFFEFFFFULL;
    si->nprocs = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
    si->active_mask = si->nprocs >= 64 ? ~(ULONG_PTR)0 : (((ULONG_PTR)1 << si->nprocs) - 1);
    si->proc_type = 8664;
    si->alloc_gran = 65536;
    si->level = 6;
    si->revision = 0;
}
API void GetNativeSystemInfo(struct sysinfo_w *si) { GetSystemInfo(si); }
API BOOL IsWow64Process(HANDLE h, BOOL *w) { (void)h; *w = FALSE; return TRUE; }
API BOOL IsWow64Process2(HANDLE h, WORD *pm, WORD *nm) { (void)h; if (pm) *pm = 0; if (nm) *nm = 0x8664; return TRUE; }
API DWORD GetActiveProcessorCount(WORD g) { (void)g; return (DWORD)sysconf(_SC_NPROCESSORS_ONLN); }
API DWORD GetMaximumProcessorCount(WORD g) { (void)g; return (DWORD)sysconf(_SC_NPROCESSORS_CONF); }
API WORD GetActiveProcessorGroupCount(void) { return 1; }
API DWORD GetCurrentProcessorNumber(void) { return (DWORD)sched_getcpu(); }

API DWORD GetVersion(void) { return (19045u << 16) | (0u << 8) | 10u; }

struct osver_a { DWORD size, major, minor, build, platform; char csd[128]; WORD spmaj, spmin, suite; BYTE type, res; };
struct osver_w { DWORD size, major, minor, build, platform; WCHAR csd[128]; WORD spmaj, spmin, suite; BYTE type, res; };

API BOOL GetVersionExA(struct osver_a *v)
{
    DWORD sz = v->size;
    memset((char *)v + 4, 0, sz - 4);
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;
    if (sz >= sizeof *v) v->type = 1;   /* VER_NT_WORKSTATION */
    return TRUE;
}
API BOOL GetVersionExW(struct osver_w *v)
{
    DWORD sz = v->size;
    memset((char *)v + 4, 0, sz - 4);
    v->major = 10; v->minor = 0; v->build = 19045; v->platform = 2;
    if (sz >= sizeof *v) v->type = 1;
    return TRUE;
}
API LONG RtlGetVersion(struct osver_w *v) { GetVersionExW(v); return 0; }
/* VerifyVersionInfo: compare the running version (Windows 10 build 19045,
 * workstation) with the caller's under the conditions packed 3 bits per
 * field in cond. Major, minor and the service-pack numbers compare as one
 * version: a later field only decides when the earlier ones are equal. */
static int ver_cmp(DWORD cur, DWORD want, int c)
{
    switch (c) {
    case 1: return cur == want;
    case 2: return cur > want;
    case 3: return cur >= want;
    case 4: return cur < want;
    case 5: return cur <= want;
    case 6: return (cur & want) == want;   /* VER_AND */
    case 7: return (cur & want) != 0;      /* VER_OR */
    default: return 0;
    }
}

static LONG verify_version(DWORD major, DWORD minor, DWORD build, DWORD platform, WORD spmaj, WORD spmin,
                           WORD suite, BYTE type, DWORD mask, ULONGLONG cond)
{
    const LONG mismatch = (LONG)0xC0000059;           /* STATUS_REVISION_MISMATCH */
    if (!mask || !cond) return (LONG)0xC000000D;       /* STATUS_INVALID_PARAMETER */
#define COND(bit) ((int)((cond >> ((bit) * 3)) & 7))
    if ((mask & 0x80) && !ver_cmp(1, type, COND(7))) return mismatch;            /* VER_PRODUCT_TYPE */
    if ((mask & 0x40) && !ver_cmp(0x100, suite, COND(6) ? COND(6) : 6)) return mismatch;   /* VER_SUITENAME */
    if ((mask & 0x08) && !ver_cmp(2, platform, COND(3))) return mismatch;        /* VER_PLATFORMID */
    if ((mask & 0x04) && !ver_cmp(19045, build, COND(2))) return mismatch;       /* VER_BUILDNUMBER */
    if (mask & 0x33) {
        const struct { DWORD bit, idx, cur, want; } f[4] = {
            { 0x02, 1, 10, major }, { 0x01, 0, 0, minor }, { 0x20, 5, 0, spmaj }, { 0x10, 4, 0, spmin } };
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            if (!(mask & f[i].bit)) continue;
            int c = COND(f[i].idx);
            ok = ver_cmp(f[i].cur, f[i].want, c);
            if (f[i].cur != f[i].want || c < 1 || c > 5) break;   /* decided here */
        }
        if (!ok) return mismatch;
    }
#undef COND
    return 0;
}

API LONG RtlVerifyVersionInfo(struct osver_w *v, DWORD mask, ULONGLONG cond)
{
    return verify_version(v->major, v->minor, v->build, v->platform, v->spmaj, v->spmin, v->suite, v->type, mask, cond);
}
API BOOL VerifyVersionInfoW(struct osver_w *v, DWORD mask, ULONGLONG cond)
{
    LONG st = RtlVerifyVersionInfo(v, mask, cond);
    if (st) { lsiw_set_last_error(st == (LONG)0xC0000059 ? 1150 /* ERROR_OLD_WIN_VERSION */ : ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
API BOOL VerifyVersionInfoA(struct osver_a *v, DWORD mask, ULONGLONG cond)
{
    LONG st = verify_version(v->major, v->minor, v->build, v->platform, v->spmaj, v->spmin, v->suite, v->type, mask, cond);
    if (st) { lsiw_set_last_error(st == (LONG)0xC0000059 ? 1150 : ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
API ULONGLONG VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE cond)
{
    if (!type || !cond) return mask;
    for (int i = 0; i < 8; i++)
        if (type & (1u << i)) mask |= (ULONGLONG)(cond & 7) << (i * 3);
    return mask;
}
API BOOL GetProductInfo(DWORD a, DWORD b, DWORD c, DWORD d, DWORD *t) { (void)a; (void)b; (void)c; (void)d; *t = 0x30; return TRUE; }

static const char *computer_name(void)
{
    const char *c = lsiw_var_windows("COMPUTERNAME");
    return c ? c : "LSIW";
}
API BOOL GetComputerNameA(char *buf, DWORD *size)
{
    const char *n = computer_name();
    DWORD l = (DWORD)strlen(n);
    if (*size <= l) { *size = l + 1; lsiw_set_last_error(111 /* ERROR_BUFFER_OVERFLOW */); return FALSE; }
    memcpy(buf, n, l + 1); *size = l;
    return TRUE;
}
API BOOL GetComputerNameW(WCHAR *buf, DWORD *size)
{
    const char *n = computer_name();
    DWORD need = (DWORD)lsiw_utf8_to_utf16(n, -1, NULL, 0);
    if (*size < need) { *size = need; lsiw_set_last_error(111); return FALSE; }
    lsiw_utf8_to_utf16(n, -1, buf, (int)*size);
    *size = need - 1;
    return TRUE;
}
API BOOL GetComputerNameExA(int fmt, char *buf, DWORD *size) { (void)fmt; return GetComputerNameA(buf, size); }
API BOOL GetComputerNameExW(int fmt, WCHAR *buf, DWORD *size) { (void)fmt; return GetComputerNameW(buf, size); }

static DWORD dir_a(const char *var, const char *def, char *buf, UINT size)
{
    const char *v = lsiw_var_windows(var);
    return lsiw_ret_str_a(v ? v : def, buf, size);
}
static DWORD dir_w(const char *var, const char *def, WCHAR *buf, UINT size)
{
    const char *v = lsiw_var_windows(var);
    return lsiw_ret_str_w(v ? v : def, buf, size);
}
API UINT GetSystemDirectoryA(char *b, UINT s) { return dir_a("SYSTEM32", "C:\\windows\\system32", b, s); }
API UINT GetSystemDirectoryW(WCHAR *b, UINT s) { return dir_w("SYSTEM32", "C:\\windows\\system32", b, s); }
API UINT GetWindowsDirectoryA(char *b, UINT s) { return dir_a("WINDIR", "C:\\windows", b, s); }
API UINT GetWindowsDirectoryW(WCHAR *b, UINT s) { return dir_w("WINDIR", "C:\\windows", b, s); }
API UINT GetSystemWindowsDirectoryA(char *b, UINT s) { return dir_a("WINDIR", "C:\\windows", b, s); }
API UINT GetSystemWindowsDirectoryW(WCHAR *b, UINT s) { return dir_w("WINDIR", "C:\\windows", b, s); }
API UINT GetSystemWow64DirectoryA(char *b, UINT s) { (void)b; (void)s; lsiw_set_last_error(ERROR_CALL_NOT_IMPLEMENTED); return 0; }
API UINT GetSystemWow64DirectoryW(WCHAR *b, UINT s) { (void)b; (void)s; lsiw_set_last_error(ERROR_CALL_NOT_IMPLEMENTED); return 0; }

struct memstatusex { DWORD len, load; ULONGLONG total_phys, avail_phys, total_page, avail_page, total_virt, avail_virt, avail_ext; };
API BOOL GlobalMemoryStatusEx(struct memstatusex *m)
{
    struct sysinfo si;
    sysinfo(&si);
    uint64_t unit = si.mem_unit ? si.mem_unit : 1;
    m->total_phys = (uint64_t)si.totalram * unit;
    m->avail_phys = ((uint64_t)si.freeram + (uint64_t)si.bufferram) * unit;
    m->total_page = m->total_phys + (uint64_t)si.totalswap * unit;
    m->avail_page = m->avail_phys + (uint64_t)si.freeswap * unit;
    m->total_virt = 0x7FFFFFFE0000ULL;
    m->avail_virt = 0x7FF000000000ULL;
    m->avail_ext = 0;
    m->load = m->total_phys ? (DWORD)(100 - m->avail_phys * 100 / m->total_phys) : 0;
    return TRUE;
}
struct memstatus { DWORD len, load; SIZE_T total_phys, avail_phys, total_page, avail_page, total_virt, avail_virt; };
API void GlobalMemoryStatus(struct memstatus *m)
{
    struct memstatusex e = { sizeof e, 0, 0, 0, 0, 0, 0, 0, 0 };
    GlobalMemoryStatusEx(&e);
    m->load = e.load; m->total_phys = e.total_phys; m->avail_phys = e.avail_phys;
    m->total_page = e.total_page; m->avail_page = e.avail_page;
    m->total_virt = e.total_virt; m->avail_virt = e.avail_virt;
}
API BOOL GetPhysicallyInstalledSystemMemory(ULONGLONG *kb)
{
    struct sysinfo si; sysinfo(&si);
    *kb = (ULONGLONG)si.totalram * (si.mem_unit ? si.mem_unit : 1) / 1024;
    return TRUE;
}

API BOOL IsProcessorFeaturePresent(DWORD f)
{
    __builtin_cpu_init();
    switch (f) {
    case 6:  return __builtin_cpu_supports("sse");       /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
    case 10: return __builtin_cpu_supports("sse2");      /* PF_XMMI64_INSTRUCTIONS_AVAILABLE */
    case 13: return __builtin_cpu_supports("sse3");      /* PF_SSE3_INSTRUCTIONS_AVAILABLE */
    case 36: return __builtin_cpu_supports("ssse3");
    case 37: return __builtin_cpu_supports("sse4.1");
    case 38: return __builtin_cpu_supports("sse4.2");
    case 39: return __builtin_cpu_supports("avx");
    case 40: return __builtin_cpu_supports("avx2");
    case 41: return __builtin_cpu_supports("avx512f");
    case 3: case 7: case 8: case 12: case 17: case 20: case 23: return TRUE; /* MMX, 3DNow?, RDTSC, NX, FASTFAIL ... */
    default: return FALSE;
    }
}

struct slpi { ULONG_PTR mask; int rel; BYTE data[16]; };
API BOOL GetLogicalProcessorInformation(struct slpi *buf, DWORD *len)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    DWORD need = (DWORD)(n * (long)sizeof(struct slpi));
    if (!buf || *len < need) { *len = need; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memset(buf, 0, need);
    for (long i = 0; i < n; i++) { buf[i].mask = (ULONG_PTR)1 << (i & 63); buf[i].rel = 0; /* RelationProcessorCore */ }
    *len = need;
    return TRUE;
}

/* ================================================================== */
/* Time                                                                 */
/* ================================================================== */

typedef struct { WORD year, month, dow, day, hour, minute, second, ms; } SYSTEMTIME;
#define EPOCH_DIFF 116444736000000000ULL

API void GetSystemTimeAsFileTime(uint64_t *ft) { *ft = now_100ns(); }
API void GetSystemTimePreciseAsFileTime(uint64_t *ft) { *ft = now_100ns(); }

static void ft_to_st(uint64_t ft, SYSTEMTIME *st)
{
    time_t t = (time_t)((ft - EPOCH_DIFF) / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    st->year = (WORD)(tm.tm_year + 1900); st->month = (WORD)(tm.tm_mon + 1); st->dow = (WORD)tm.tm_wday;
    st->day = (WORD)tm.tm_mday; st->hour = (WORD)tm.tm_hour; st->minute = (WORD)tm.tm_min;
    st->second = (WORD)tm.tm_sec; st->ms = (WORD)((ft / 10000) % 1000);
}

static uint64_t st_to_ft(const SYSTEMTIME *st)
{
    struct tm tm = {0};
    tm.tm_year = st->year - 1900; tm.tm_mon = st->month - 1; tm.tm_mday = st->day;
    tm.tm_hour = st->hour; tm.tm_min = st->minute; tm.tm_sec = st->second;
    time_t t = timegm(&tm);
    return (uint64_t)t * 10000000ULL + EPOCH_DIFF + (uint64_t)st->ms * 10000;
}

static int64_t tz_bias_100ns(time_t at)
{
    struct tm tm;
    localtime_r(&at, &tm);
    return (int64_t)tm.tm_gmtoff * 10000000LL;
}

API void GetSystemTime(SYSTEMTIME *st) { ft_to_st(now_100ns(), st); }
API void GetLocalTime(SYSTEMTIME *st) { uint64_t ft = now_100ns(); ft_to_st(ft + (uint64_t)tz_bias_100ns(time(NULL)), st); }
API BOOL SetLocalTime(const SYSTEMTIME *st) { (void)st; lsiw_set_last_error(1314 /* ERROR_PRIVILEGE_NOT_HELD */); return FALSE; }
API BOOL SetSystemTime(const SYSTEMTIME *st) { (void)st; lsiw_set_last_error(1314); return FALSE; }
API BOOL FileTimeToSystemTime(const uint64_t *ft, SYSTEMTIME *st) { ft_to_st(*ft, st); return TRUE; }
API BOOL SystemTimeToFileTime(const SYSTEMTIME *st, uint64_t *ft) { *ft = st_to_ft(st); return TRUE; }
API BOOL FileTimeToLocalFileTime(const uint64_t *ft, uint64_t *lft)
{ *lft = *ft + (uint64_t)tz_bias_100ns((time_t)((*ft - EPOCH_DIFF) / 10000000ULL)); return TRUE; }
API BOOL LocalFileTimeToFileTime(const uint64_t *lft, uint64_t *ft)
{ *ft = *lft - (uint64_t)tz_bias_100ns((time_t)((*lft - EPOCH_DIFF) / 10000000ULL)); return TRUE; }
API LONG CompareFileTime(const uint64_t *a, const uint64_t *b) { return *a < *b ? -1 : *a > *b ? 1 : 0; }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
API DWORD GetTickCount(void) { return (DWORD)(mono_ns() / 1000000ULL); }
API ULONGLONG GetTickCount64(void) { return mono_ns() / 1000000ULL; }
API BOOL QueryPerformanceCounter(int64_t *c) { *c = (int64_t)(mono_ns() / 100); return TRUE; }
API BOOL QueryPerformanceFrequency(int64_t *f) { *f = 10000000; return TRUE; }
API BOOL QueryUnbiasedInterruptTime(ULONGLONG *t) { *t = mono_ns() / 100; return TRUE; }
API void QueryInterruptTime(ULONGLONG *t) { *t = mono_ns() / 100; }
API BOOL GetSystemTimes(uint64_t *idle, uint64_t *kernel, uint64_t *user)
{
    if (idle) *idle = 0;
    if (kernel) *kernel = 0;
    if (user) *user = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u, n, s, i;
        if (fscanf(f, "cpu %llu %llu %llu %llu", &u, &n, &s, &i) == 4) {
            long hz = sysconf(_SC_CLK_TCK);
            if (idle) *idle = i * 10000000ULL / (unsigned long long)hz;
            if (kernel) *kernel = (s + i) * 10000000ULL / (unsigned long long)hz;
            if (user) *user = (u + n) * 10000000ULL / (unsigned long long)hz;
        }
        fclose(f);
    }
    return TRUE;
}

static int fmt_date(const SYSTEMTIME *st, DWORD flags, const char *fmt, char *out, size_t n, int is_time)
{
    SYSTEMTIME now;
    if (!st) { GetLocalTime(&now); st = &now; }
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };
    if (!fmt) fmt = is_time ? "h:mm:ss tt" : ((flags & 2) ? "dddd, MMMM d, yyyy" : "M/d/yyyy");
    size_t o = 0;
    for (const char *p = fmt; *p && o + 32 < n; ) {
        char c = *p; int cnt = 0;
        if (c == '\'') { p++; while (*p && *p != '\'') out[o++] = *p++; if (*p) p++; continue; }
        while (p[cnt] == c) cnt++;
        char tmp[32] = "";
        switch (c) {
        case 'd':
            if (is_time) { snprintf(tmp, sizeof tmp, "%.*s", cnt, p); break; }
            if (cnt == 1) snprintf(tmp, sizeof tmp, "%d", st->day);
            else if (cnt == 2) snprintf(tmp, sizeof tmp, "%02d", st->day);
            else if (cnt == 3) snprintf(tmp, sizeof tmp, "%.3s", days[st->dow % 7]);
            else snprintf(tmp, sizeof tmp, "%s", days[st->dow % 7]);
            break;
        case 'M':
            if (cnt == 1) snprintf(tmp, sizeof tmp, "%d", st->month);
            else if (cnt == 2) snprintf(tmp, sizeof tmp, "%02d", st->month);
            else if (cnt == 3) snprintf(tmp, sizeof tmp, "%.3s", months[(st->month + 11) % 12]);
            else snprintf(tmp, sizeof tmp, "%s", months[(st->month + 11) % 12]);
            break;
        case 'y':
            if (cnt <= 2) snprintf(tmp, sizeof tmp, cnt == 1 ? "%d" : "%02d", st->year % 100);
            else snprintf(tmp, sizeof tmp, "%d", st->year);
            break;
        case 'h': { int h = st->hour % 12; if (!h) h = 12; snprintf(tmp, sizeof tmp, cnt == 1 ? "%d" : "%02d", h); break; }
        case 'H': snprintf(tmp, sizeof tmp, cnt == 1 ? "%d" : "%02d", st->hour); break;
        case 'm': snprintf(tmp, sizeof tmp, cnt == 1 ? "%d" : "%02d", st->minute); break;
        case 's': snprintf(tmp, sizeof tmp, cnt == 1 ? "%d" : "%02d", st->second); break;
        case 't': snprintf(tmp, sizeof tmp, "%.*s", cnt == 1 ? 1 : 2, st->hour < 12 ? "AM" : "PM"); break;
        default: snprintf(tmp, sizeof tmp, "%.*s", cnt, p); break;
        }
        size_t tl = strlen(tmp);
        memcpy(out + o, tmp, tl); o += tl;
        p += cnt;
    }
    out[o] = 0;
    return (int)o + 1;
}

API int GetDateFormatA(DWORD lcid, DWORD flags, const SYSTEMTIME *st, const char *fmt, char *buf, int n)
{
    (void)lcid;
    char tmp[256]; int l = fmt_date(st, flags, fmt, tmp, sizeof tmp, 0);
    if (!n) return l;
    if (l > n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, tmp, (size_t)l); return l;
}
API int GetTimeFormatA(DWORD lcid, DWORD flags, const SYSTEMTIME *st, const char *fmt, char *buf, int n)
{
    (void)lcid;
    char tmp[256]; int l = fmt_date(st, flags, fmt, tmp, sizeof tmp, 1);
    if (!n) return l;
    if (l > n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, tmp, (size_t)l); return l;
}
static int fmt_w(const SYSTEMTIME *st, DWORD flags, const WCHAR *fmt, WCHAR *buf, int n, int is_time)
{
    char *f = lsiw_w2u(fmt), tmp[256];
    fmt_date(st, flags, f, tmp, sizeof tmp, is_time);
    free(f);
    int need = lsiw_utf8_to_utf16(tmp, -1, NULL, 0);
    if (!n) return need;
    if (need > n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return lsiw_utf8_to_utf16(tmp, -1, buf, n);
}
API int GetDateFormatW(DWORD lcid, DWORD flags, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) { (void)lcid; return fmt_w(st, flags, fmt, buf, n, 0); }
API int GetTimeFormatW(DWORD lcid, DWORD flags, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) { (void)lcid; return fmt_w(st, flags, fmt, buf, n, 1); }
API int GetDateFormatEx(const WCHAR *loc, DWORD flags, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n, const WCHAR *cal) { (void)loc; (void)cal; return fmt_w(st, flags, fmt, buf, n, 0); }
API int GetTimeFormatEx(const WCHAR *loc, DWORD flags, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) { (void)loc; return fmt_w(st, flags, fmt, buf, n, 1); }

/* ================================================================== */
/* Code pages and locale (LSIW uses UTF-8 as the ANSI code page)        */
/* ================================================================== */

#define CP_UTF8 65001
API UINT GetACP(void) { return CP_UTF8; }
API UINT GetOEMCP(void) { return CP_UTF8; }
API BOOL IsValidCodePage(UINT cp) { return cp == CP_UTF8 || cp == 1252 || cp == 437 || cp == 20127 || cp == 28591 || cp == 1200; }

struct cpinfo { UINT max_char; BYTE def_char[2]; BYTE lead[12]; };
API BOOL GetCPInfo(UINT cp, struct cpinfo *ci)
{
    memset(ci, 0, sizeof *ci);
    ci->max_char = (cp == CP_UTF8 || cp == 0 || cp == 1 || cp == 3) ? 4 : 1;
    ci->def_char[0] = '?';
    return TRUE;
}
struct cpinfoex_a { UINT max_char; BYTE def_char[2]; BYTE lead[12]; WCHAR uni_def; UINT cp; char name[260]; };
struct cpinfoex_w { UINT max_char; BYTE def_char[2]; BYTE lead[12]; WCHAR uni_def; UINT cp; WCHAR name[260]; };
API BOOL GetCPInfoExA(UINT cp, DWORD f, struct cpinfoex_a *ci) { (void)f; GetCPInfo(cp, (struct cpinfo *)ci); ci->uni_def = '?'; ci->cp = cp ? cp : CP_UTF8; strcpy(ci->name, "65001 (UTF-8)"); return TRUE; }
API BOOL GetCPInfoExW(UINT cp, DWORD f, struct cpinfoex_w *ci) { (void)f; GetCPInfo(cp, (struct cpinfo *)ci); ci->uni_def = '?'; ci->cp = cp ? cp : CP_UTF8; lsiw_utf8_to_utf16("65001 (UTF-8)", -1, ci->name, 260); return TRUE; }

static int is_single_byte_cp(UINT cp) { return cp == 1252 || cp == 28591 || cp == 437 || cp == 20127 || cp == 850; }

API int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int slen, WCHAR *out, int outlen)
{
    (void)flags;
    if (!s || slen == 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (is_single_byte_cp(cp)) {
        int n = slen < 0 ? (int)strlen(s) + 1 : slen;
        if (!outlen) return n;
        if (outlen < n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        for (int i = 0; i < n; i++) out[i] = (unsigned char)s[i];
        return n;
    }
    int n = lsiw_utf8_to_utf16(s, slen, outlen ? out : NULL, outlen);
    if (n < 0) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return n;
}

API int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *s, int slen, char *out, int outlen,
                            const char *defchar, BOOL *used_def)
{
    (void)flags; (void)defchar;
    if (used_def) *used_def = FALSE;
    if (!s || slen == 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (is_single_byte_cp(cp)) {
        int n = slen < 0 ? (int)lsiw_wcslen(s) + 1 : slen;
        if (!outlen) return n;
        if (outlen < n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        for (int i = 0; i < n; i++) {
            if (s[i] > 0xFF) { out[i] = '?'; if (used_def) *used_def = TRUE; }
            else out[i] = (char)s[i];
        }
        return n;
    }
    int n = lsiw_utf16_to_utf8(s, slen, outlen ? out : NULL, outlen);
    if (n < 0) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return n;
}

API BOOL IsDBCSLeadByte(BYTE b) { (void)b; return FALSE; }
API BOOL IsDBCSLeadByteEx(UINT cp, BYTE b) { (void)cp; (void)b; return FALSE; }

#define LCID_EN_US 0x0409
API DWORD GetUserDefaultLCID(void) { return LCID_EN_US; }
API DWORD GetSystemDefaultLCID(void) { return LCID_EN_US; }
API DWORD GetThreadLocale(void) { return LCID_EN_US; }
API BOOL  SetThreadLocale(DWORD l) { (void)l; return TRUE; }
API WORD  GetUserDefaultLangID(void) { return LCID_EN_US; }
API WORD  GetSystemDefaultLangID(void) { return LCID_EN_US; }
API WORD  GetUserDefaultUILanguage(void) { return LCID_EN_US; }
API WORD  GetSystemDefaultUILanguage(void) { return LCID_EN_US; }
API int   GetUserDefaultLocaleName(WCHAR *b, int n) { return (int)lsiw_ret_str_w("en-US", b, (DWORD)n) + 1; }
API int   GetSystemDefaultLocaleName(WCHAR *b, int n) { return (int)lsiw_ret_str_w("en-US", b, (DWORD)n) + 1; }
API int   LCIDToLocaleName(DWORD l, WCHAR *b, int n, DWORD f) { (void)l; (void)f; if (!n) return 6; return (int)lsiw_ret_str_w("en-US", b, (DWORD)n) + 1; }
API DWORD LocaleNameToLCID(const WCHAR *n, DWORD f) { (void)n; (void)f; return LCID_EN_US; }
API BOOL  IsValidLocale(DWORD l, DWORD f) { (void)f; return l == LCID_EN_US || l == 0x400 || l == 0x800 || l == 0x7F; }
API BOOL  IsValidLocaleName(const WCHAR *n) { (void)n; return TRUE; }
API BOOL  GetUserPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *len)
{
    (void)f;
    *num = 1;
    if (!buf || *len < 7) { *len = 7; return buf ? (lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER), FALSE) : TRUE; }
    lsiw_utf8_to_utf16("en-US", -1, buf, 6); buf[6] = 0; *len = 7;
    return TRUE;
}
API BOOL GetThreadPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *len) { return GetUserPreferredUILanguages(f, num, buf, len); }
API BOOL GetProcessPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *len) { return GetUserPreferredUILanguages(f, num, buf, len); }
API BOOL SetThreadPreferredUILanguages(DWORD f, const WCHAR *l, ULONG *n) { (void)f; (void)l; if (n) *n = 1; return TRUE; }

static const char *locale_value(DWORD type)
{
    switch (type & 0x0FFFFFFF) {
    case 0x01: return "1033";            /* LOCALE_ILANGUAGE */
    case 0x02: return "English (United States)";  /* LOCALE_SLANGUAGE */
    case 0x03: return "ENU";
    case 0x04: return "English";
    case 0x05: return "1";               /* ICOUNTRY */
    case 0x06: return "United States";
    case 0x07: return "USA";
    case 0x0B: return "437";             /* IDEFAULTCODEPAGE */
    case 0x0C: return ",";               /* SLIST */
    case 0x0D: return "1";               /* IMEASURE: US */
    case 0x0E: return ".";               /* SDECIMAL */
    case 0x0F: return ",";               /* STHOUSAND */
    case 0x10: return "3;0";             /* SGROUPING */
    case 0x11: return "2";               /* IDIGITS */
    case 0x12: return "1";               /* ILZERO */
    case 0x14: return "$";               /* SCURRENCY */
    case 0x15: return "USD";
    case 0x16: return ".";
    case 0x17: return ",";
    case 0x18: return "3;0";
    case 0x19: return "2";
    case 0x1B: return "0";
    case 0x1C: return "0";
    case 0x1D: return "/";               /* SDATE */
    case 0x1E: return ":";               /* STIME */
    case 0x1F: return "M/d/yyyy";        /* SSHORTDATE */
    case 0x20: return "dddd, MMMM d, yyyy";
    case 0x21: return "0";               /* IDATE */
    case 0x23: return "0";               /* ITIME (12h) */
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x1004: return "65001";         /* IDEFAULTANSICODEPAGE */
    case 0x1003: return "1033";
    case 0x1001: return "English";
    case 0x1002: return "United States";
    case 0x1009: return "1";             /* IFIRSTDAYOFWEEK? (0=Mon..6=Sun) */
    case 0x100C: return "0";
    case 0x1005: return "0";
    case 0x1014: return "M/d/yyyy";
    case 0x0059: return "en";            /* SISO639LANGNAME */
    case 0x005A: return "US";            /* SISO3166CTRYNAME */
    case 0x005C: return "en-US";         /* SNAME */
    case 0x0067: return "eng";
    case 0x0068: return "USA";
    case 0x1003 + 0x10000: return "1033";
    case 0x51:   return "-";             /* SNEGATIVESIGN */
    case 0x50:   return "";              /* SPOSITIVESIGN */
    case 0x1000: return "h:mm:ss tt";    /* STIMEFORMAT */
    case 0x1006: return "0";
    case 0x100A: return "0";
    case 0x100D: return "0";
    case 0x1010: return "1";             /* IPAPERSIZE letter */
    case 0x6B:   return "English (United States)";
    case 0x72:   return "English (United States)";
    case 0x73:   return "en-US";
    default:     return NULL;
    }
}

API int GetLocaleInfoA(DWORD lcid, DWORD type, char *buf, int n)
{
    (void)lcid;
    if ((type & 0x0FFFFFFF) == 0 || (type & 0x20000000 /* RETURN_NUMBER */)) {
        if (type & 0x20000000) {
            const char *v = locale_value(type);
            if (!v) return 0;
            if (n < 2) return 2;
            *(DWORD *)buf = (DWORD)atoi(v);
            return 2;
        }
    }
    const char *v = locale_value(type);
    if (!v) { lsiw_set_last_error(1004 /* ERROR_INVALID_FLAGS */); return 0; }
    int l = (int)strlen(v) + 1;
    if (!n) return l;
    if (n < l) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, v, (size_t)l);
    return l;
}
API int GetLocaleInfoW(DWORD lcid, DWORD type, WCHAR *buf, int n)
{
    (void)lcid;
    const char *v = locale_value(type);
    if (!v) { lsiw_set_last_error(1004); return 0; }
    if (type & 0x20000000) {
        if (n < 2) return 2;
        *(DWORD *)buf = (DWORD)atoi(v);
        return 2;
    }
    int l = lsiw_utf8_to_utf16(v, -1, NULL, 0);
    if (!n) return l;
    if (n < l) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    return lsiw_utf8_to_utf16(v, -1, buf, n);
}
API int GetLocaleInfoEx(const WCHAR *loc, DWORD type, WCHAR *buf, int n) { (void)loc; return GetLocaleInfoW(LCID_EN_US, type, buf, n); }

API BOOL GetStringTypeW(DWORD type, const WCHAR *s, int n, WORD *out)
{
    if (n < 0) n = (int)lsiw_wcslen(s) + 1;
    for (int i = 0; i < n; i++) {
        wint_t c = s[i];
        WORD r = 0;
        if (type == 1 /* CT_CTYPE1 */) {
            if (iswupper(c)) r |= 0x001;
            if (iswlower(c)) r |= 0x002;
            if (iswdigit(c)) r |= 0x004;
            if (iswspace(c)) r |= 0x008;
            if (iswpunct(c)) r |= 0x010;
            if (iswcntrl(c)) r |= 0x020;
            if (c == ' ' || c == '\t') r |= 0x040;
            if (iswxdigit(c)) r |= 0x080;
            if (iswalpha(c)) r |= 0x100;
            if (c && !iswcntrl(c)) r |= 0x200;   /* C1_DEFINED */
        } else if (type == 2) r = iswdigit(c) ? 3 : (iswalpha(c) ? 1 : 0);
        out[i] = r;
    }
    return TRUE;
}
API BOOL GetStringTypeExW(DWORD lcid, DWORD type, const WCHAR *s, int n, WORD *out) { (void)lcid; return GetStringTypeW(type, s, n, out); }
API BOOL GetStringTypeA(DWORD lcid, DWORD type, const char *s, int n, WORD *out)
{
    (void)lcid;
    if (n < 0) n = (int)strlen(s) + 1;
    WCHAR *w = malloc((size_t)n * sizeof(WCHAR));
    for (int i = 0; i < n; i++) w[i] = (unsigned char)s[i];
    GetStringTypeW(type, w, n, out);
    free(w);
    return TRUE;
}
API BOOL GetStringTypeExA(DWORD lcid, DWORD type, const char *s, int n, WORD *out) { return GetStringTypeA(lcid, type, s, n, out); }

API int LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *s, int n, WCHAR *d, int dn)
{
    (void)lcid;
    if (n < 0) n = (int)lsiw_wcslen(s) + 1;
    if (flags & 0x400 /* LCMAP_SORTKEY */) {
        if (!dn) return n * 2 + 1;
        int k = 0;
        for (int i = 0; i < n && k + 2 < dn; i++) { WCHAR c = (WCHAR)towupper(s[i]); ((BYTE *)d)[k++] = (BYTE)(c >> 8); ((BYTE *)d)[k++] = (BYTE)c; }
        ((BYTE *)d)[k++] = 0;
        return k;
    }
    if (!dn) return n;
    if (dn < n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (flags & 0x100) c = (WCHAR)towlower(c);       /* LCMAP_LOWERCASE */
        else if (flags & 0x200) c = (WCHAR)towupper(c);  /* LCMAP_UPPERCASE */
        d[i] = c;
    }
    return n;
}
API int LCMapStringEx(const WCHAR *loc, DWORD flags, const WCHAR *s, int n, WCHAR *d, int dn, void *a, void *b, intptr_t c)
{ (void)loc; (void)a; (void)b; (void)c; return LCMapStringW(LCID_EN_US, flags, s, n, d, dn); }
API int LCMapStringA(DWORD lcid, DWORD flags, const char *s, int n, char *d, int dn)
{
    (void)lcid;
    if (n < 0) n = (int)strlen(s) + 1;
    if (!dn) return n;
    if (dn < n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (flags & 0x100) c = (char)tolower((unsigned char)c);
        else if (flags & 0x200) c = (char)toupper((unsigned char)c);
        d[i] = c;
    }
    return n;
}

static int cmp_w(DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb)
{
    if (na < 0) na = (int)lsiw_wcslen(a);
    if (nb < 0) nb = (int)lsiw_wcslen(b);
    int ic = flags & 0x1;   /* NORM_IGNORECASE */
    for (int i = 0; ; i++) {
        if (i >= na || i >= nb) return na == nb ? 2 : (na < nb ? 1 : 3);
        wint_t x = a[i], y = b[i];
        if (ic) { x = towlower(x); y = towlower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
}
API int CompareStringW(DWORD lcid, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb) { (void)lcid; return cmp_w(flags, a, na, b, nb); }
API int CompareStringEx(const WCHAR *loc, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb, void *v, void *r, intptr_t p)
{ (void)loc; (void)v; (void)r; (void)p; return cmp_w(flags, a, na, b, nb); }
API int CompareStringOrdinal(const WCHAR *a, int na, const WCHAR *b, int nb, BOOL ic) { return cmp_w(ic ? 1 : 0, a, na, b, nb); }
API int CompareStringA(DWORD lcid, DWORD flags, const char *a, int na, const char *b, int nb)
{
    (void)lcid;
    WCHAR *wa = lsiw_u2w(a), *wb = lsiw_u2w(b);
    (void)na; (void)nb;
    int r = cmp_w(flags, wa, -1, wb, -1);
    free(wa); free(wb);
    return r;
}
API int FoldStringW(DWORD f, const WCHAR *s, int n, WCHAR *d, int dn)
{
    (void)f;
    if (n < 0) n = (int)lsiw_wcslen(s) + 1;
    if (!dn) return n;
    if (dn < n) return 0;
    memcpy(d, s, (size_t)n * 2);
    return n;
}

API int lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }
API int lstrlenW(const WCHAR *s) { return s ? (int)lsiw_wcslen(s) : 0; }
API int lstrcmpA(const char *a, const char *b) { int r = strcmp(a ? a : "", b ? b : ""); return r < 0 ? -1 : r > 0; }
API int lstrcmpiA(const char *a, const char *b) { int r = strcasecmp(a ? a : "", b ? b : ""); return r < 0 ? -1 : r > 0; }
API int lstrcmpW(const WCHAR *a, const WCHAR *b) { static WCHAR e[1]; return cmp_w(0, a ? a : e, -1, b ? b : e, -1) - 2; }
API int lstrcmpiW(const WCHAR *a, const WCHAR *b) { static WCHAR e[1]; return cmp_w(1, a ? a : e, -1, b ? b : e, -1) - 2; }
API char *lstrcpyA(char *d, const char *s) { return strcpy(d, s); }
API WCHAR *lstrcpyW(WCHAR *d, const WCHAR *s) { memcpy(d, s, (lsiw_wcslen(s) + 1) * 2); return d; }
API char *lstrcpynA(char *d, const char *s, int n) { if (n <= 0) return d; int i; for (i = 0; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; return d; }
API WCHAR *lstrcpynW(WCHAR *d, const WCHAR *s, int n) { if (n <= 0) return d; int i; for (i = 0; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; return d; }
API char *lstrcatA(char *d, const char *s) { return strcat(d, s); }
API WCHAR *lstrcatW(WCHAR *d, const WCHAR *s) { lstrcpyW(d + lsiw_wcslen(d), s); return d; }

/* ================================================================== */
/* Debugging, error mode, formatting                                    */
/* ================================================================== */

API void OutputDebugStringA(const char *s) { if (lsiw_verbose && s) fprintf(stderr, "lsiw: debug: %s", s); }
API void OutputDebugStringW(const WCHAR *s) { if (lsiw_verbose && s) { char *u = lsiw_w2u(s); fprintf(stderr, "lsiw: debug: %s", u); free(u); } }
API BOOL IsDebuggerPresent(void) { return FALSE; }
API BOOL CheckRemoteDebuggerPresent(HANDLE h, BOOL *p) { (void)h; *p = FALSE; return TRUE; }
API void DebugBreak(void) { __asm__ volatile ("int3"); }
static UINT g_error_mode;
API UINT SetErrorMode(UINT m) { UINT o = g_error_mode; g_error_mode = m; return o; }
API UINT GetErrorMode(void) { return g_error_mode; }
API BOOL SetThreadErrorMode(DWORD m, DWORD *o) { if (o) *o = g_error_mode; g_error_mode = m; return TRUE; }
API DWORD GetThreadErrorMode(void) { return g_error_mode; }
API BOOL Beep(DWORD f, DWORD d) { (void)f; (void)d; fputc('\a', stderr); return TRUE; }
API void *EncodePointer(void *p) { return (void *)((uintptr_t)p ^ 0x5A5A5A5A5A5A5A5AULL); }
API void *DecodePointer(void *p) { return (void *)((uintptr_t)p ^ 0x5A5A5A5A5A5A5A5AULL); }
API void *EncodeSystemPointer(void *p) { return p; }
API void *DecodeSystemPointer(void *p) { return p; }

static const char *message_text(DWORD id)
{
    switch (id) {
    case 0:   return "The operation completed successfully.";
    case 1:   return "Incorrect function.";
    case 2:   return "The system cannot find the file specified.";
    case 3:   return "The system cannot find the path specified.";
    case 4:   return "The system cannot open the file.";
    case 5:   return "Access is denied.";
    case 6:   return "The handle is invalid.";
    case 8:   return "Not enough memory resources are available to process this command.";
    case 14:  return "Not enough memory resources are available to complete this operation.";
    case 15:  return "The system cannot find the drive specified.";
    case 17:  return "The system cannot move the file to a different disk drive.";
    case 18:  return "There are no more files.";
    case 19:  return "The media is write protected.";
    case 32:  return "The process cannot access the file because it is being used by another process.";
    case 38:  return "Reached the end of the file.";
    case 50:  return "The request is not supported.";
    case 80:  return "The file exists.";
    case 87:  return "The parameter is incorrect.";
    case 109: return "The pipe has been ended.";
    case 112: return "There is not enough space on the disk.";
    case 120: return "This function is not supported on this system.";
    case 122: return "The data area passed to a system call is too small.";
    case 123: return "The filename, directory name, or volume label syntax is incorrect.";
    case 126: return "The specified module could not be found.";
    case 127: return "The specified procedure could not be found.";
    case 145: return "The directory is not empty.";
    case 183: return "Cannot create a file when that file already exists.";
    case 203: return "The system could not find the environment option that was entered.";
    case 206: return "The filename or extension is too long.";
    case 234: return "More data is available.";
    case 259: return "No more data is available.";
    case 267: return "The directory name is invalid.";
    case 1460: return "This operation returned because the timeout period expired.";
    default:  return NULL;
    }
}

static char *format_message(DWORD flags, const void *src, DWORD id, int wide_src, ms_va_list *args)
{
    (void)args;
    char *fmt;
    if (flags & 0x1000 /* FROM_SYSTEM */) {
        const char *t = message_text(id);
        if (!t) {
            if (flags & 0x400 /* FROM_STRING */) t = NULL;
            else return NULL;
        }
        fmt = lsiw_strdup_printf("%s\r\n", t);
    } else if (flags & 0x400 /* FROM_STRING */) {
        fmt = wide_src ? lsiw_w2u(src) : strdup(src ? src : "");
    } else return NULL;
    if (flags & 0x200 /* IGNORE_INSERTS */) return fmt;
    /* Inserts %1..%99 with optional !fmt! are replaced from the argument array. */
    size_t cap = strlen(fmt) + 256, len = 0;
    char *out = malloc(cap);
    void **argv = (flags & 0x2000 /* ARGUMENT_ARRAY */) && args ? *(void ***)args : NULL;
    for (const char *p = fmt; *p; ) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '9') {
            int n = 0; p++;
            while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            char spec[16] = "s";
            if (*p == '!') { const char *e = strchr(p + 1, '!'); if (e) { snprintf(spec, sizeof spec, "%.*s", (int)(e - p - 1), p + 1); p = e + 1; } }
            char tmp[512] = "";
            void *a = argv ? argv[n - 1] : NULL;
            if (!argv && args) {
                /* va_list: walk to the n-th argument (all are pointer-sized). */
                ms_va_list ap;
                __builtin_ms_va_copy(ap, *args);
                for (int i = 0; i < n; i++) a = ms_va_arg(ap, void *);
                ms_va_end(ap);
            }
            if (strchr(spec, 's')) {
                if (wide_src || strchr(spec, 'S') || (flags & 0x10000000)) { char *u = lsiw_w2u(a); snprintf(tmp, sizeof tmp, "%s", u ? u : ""); free(u); }
                else snprintf(tmp, sizeof tmp, "%s", a ? (const char *)a : "");
            } else {
                char f2[20]; snprintf(f2, sizeof f2, "%%%s", spec);
                snprintf(tmp, sizeof tmp, f2, (long long)(intptr_t)a);
            }
            size_t tl = strlen(tmp);
            while (len + tl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + len, tmp, tl); len += tl;
            continue;
        }
        if (*p == '%' && p[1] == 'n') { p += 2; if (len + 3 > cap) { cap *= 2; out = realloc(out, cap); } out[len++] = '\r'; out[len++] = '\n'; continue; }
        if (*p == '%' && p[1]) { p++; }
        if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        out[len++] = *p++;
    }
    out[len] = 0;
    free(fmt);
    return out;
}

API DWORD FormatMessageA(DWORD flags, const void *src, DWORD id, DWORD lang, char *buf, DWORD size, ms_va_list *args)
{
    (void)lang;
    char *m = format_message(flags, src, id, 0, args);
    if (!m) { lsiw_set_last_error(317 /* ERROR_MR_MID_NOT_FOUND */); return 0; }
    DWORD l = (DWORD)strlen(m);
    if (flags & 0x100 /* ALLOCATE_BUFFER */) {
        char *p = lsiw_heap_alloc(l + 1, 0);       /* the caller frees it with LocalFree */
        memcpy(p, m, l + 1);
        *(char **)buf = p;
    } else {
        if (l + 1 > size) { free(m); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(buf, m, l + 1);
    }
    free(m);
    return l;
}

API DWORD FormatMessageW(DWORD flags, const void *src, DWORD id, DWORD lang, WCHAR *buf, DWORD size, ms_va_list *args)
{
    (void)lang;
    char *m = format_message(flags, src, id, 1, args);
    if (!m) { lsiw_set_last_error(317); return 0; }
    WCHAR *w = lsiw_u2w(m);
    free(m);
    DWORD l = (DWORD)lsiw_wcslen(w);
    if (flags & 0x100) {
        WCHAR *p = lsiw_heap_alloc((l + 1) * 2, 0);
        memcpy(p, w, (l + 1) * 2);
        free(w);
        *(WCHAR **)buf = p;
    } else {
        if (l + 1 > size) { free(w); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(buf, w, (l + 1) * 2);
        free(w);
    }
    return l;
}

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_k32proc_exports[] = {
    E(GetCommandLineA), E(GetCommandLineW),
    E(GetEnvironmentVariableA), E(GetEnvironmentVariableW), E(SetEnvironmentVariableA), E(SetEnvironmentVariableW),
    E(GetEnvironmentStringsA), E(GetEnvironmentStringsW), EX("GetEnvironmentStrings", GetEnvironmentStringsA),
    E(FreeEnvironmentStringsA), E(FreeEnvironmentStringsW), E(ExpandEnvironmentStringsA), E(ExpandEnvironmentStringsW),
    E(GetLastError), E(SetLastError), E(RestoreLastError), E(ExitProcess), E(TerminateProcess), E(FatalAppExitA), E(FatalAppExitW),
    E(CloseHandle), E(DuplicateHandle), E(GetHandleInformation), E(SetHandleInformation),
    E(CreateEventA), E(CreateEventW), E(CreateEventExA), E(CreateEventExW), E(OpenEventA), E(OpenEventW),
    E(CreateMutexA), E(CreateMutexW), E(CreateMutexExA), E(CreateMutexExW), E(OpenMutexA), E(OpenMutexW),
    E(CreateSemaphoreA), E(CreateSemaphoreW), E(CreateSemaphoreExW), E(OpenSemaphoreW),
    E(SetEvent), E(ResetEvent), E(PulseEvent), E(ReleaseMutex), E(ReleaseSemaphore),
    E(CreateWaitableTimerA), E(CreateWaitableTimerW), E(CreateWaitableTimerExW), E(SetWaitableTimer), E(SetWaitableTimerEx), E(CancelWaitableTimer),
    E(WaitForSingleObject), E(WaitForSingleObjectEx), E(WaitForMultipleObjects), E(WaitForMultipleObjectsEx), E(SignalObjectAndWait),
    E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(InitializeCriticalSectionEx),
    E(SetCriticalSectionSpinCount), E(DeleteCriticalSection), E(EnterCriticalSection), E(TryEnterCriticalSection), E(LeaveCriticalSection),
    E(InitializeSRWLock), E(AcquireSRWLockExclusive), E(AcquireSRWLockShared), E(ReleaseSRWLockExclusive), E(ReleaseSRWLockShared),
    E(TryAcquireSRWLockExclusive), E(TryAcquireSRWLockShared),
    E(InitializeConditionVariable), E(SleepConditionVariableCS), E(SleepConditionVariableSRW), E(WakeConditionVariable), E(WakeAllConditionVariable),
    E(WaitOnAddress), E(WakeByAddressSingle), E(WakeByAddressAll),
    E(InitOnceInitialize), E(InitOnceExecuteOnce), E(InitOnceBeginInitialize), E(InitOnceComplete),
    EX("RtlEnterCriticalSection", EnterCriticalSection), EX("RtlLeaveCriticalSection", LeaveCriticalSection),
    EX("RtlInitializeCriticalSection", InitializeCriticalSection), EX("RtlDeleteCriticalSection", DeleteCriticalSection),
    EX("RtlTryEnterCriticalSection", TryEnterCriticalSection),
    EX("RtlInitializeSRWLock", InitializeSRWLock), EX("RtlAcquireSRWLockExclusive", AcquireSRWLockExclusive),
    EX("RtlAcquireSRWLockShared", AcquireSRWLockShared), EX("RtlReleaseSRWLockExclusive", ReleaseSRWLockExclusive),
    EX("RtlReleaseSRWLockShared", ReleaseSRWLockShared),
    E(CreateThread), E(CreateRemoteThread), E(CreateRemoteThreadEx), E(ExitThread), E(ResumeThread), E(SuspendThread), E(TerminateThread),
    E(GetExitCodeThread), E(GetCurrentThread), E(GetCurrentProcess), E(GetCurrentThreadId), E(GetCurrentProcessId),
    E(GetThreadId), E(GetProcessId), E(GetProcessIdOfThread), E(SetThreadPriority), E(GetThreadPriority), E(SetThreadPriorityBoost),
    E(SetPriorityClass), E(GetPriorityClass), E(SetThreadAffinityMask), E(SetProcessAffinityMask), E(GetProcessAffinityMask),
    E(SetThreadIdealProcessor), E(SwitchToThread), E(SetThreadDescription), E(GetThreadDescription),
    E(GetProcessTimes), E(GetThreadTimes), E(QueryThreadCycleTime), E(QueryProcessCycleTime), E(Sleep), E(SleepEx),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(FlsAlloc), E(FlsFree), E(FlsGetValue), E(FlsSetValue), E(IsThreadAFiber),
    E(CreateProcessA), E(CreateProcessW), E(GetExitCodeProcess), E(OpenProcess), E(CreatePipe), E(WinExec),
    E(GetStartupInfoA), E(GetStartupInfoW),
    E(GetModuleHandleA), E(GetModuleHandleW), E(GetModuleHandleExA), E(GetModuleHandleExW),
    E(GetModuleFileNameA), E(GetModuleFileNameW), E(K32GetModuleFileNameExA), E(K32GetModuleFileNameExW),
    EX("GetModuleFileNameExA", K32GetModuleFileNameExA), EX("GetModuleFileNameExW", K32GetModuleFileNameExW),
    E(K32GetModuleBaseNameW), EX("GetModuleBaseNameW", K32GetModuleBaseNameW),
    E(K32GetModuleInformation), EX("GetModuleInformation", K32GetModuleInformation),
    E(K32EnumProcessModules), EX("EnumProcessModules", K32EnumProcessModules),
    E(LoadLibraryA), E(LoadLibraryW), E(LoadLibraryExA), E(LoadLibraryExW), E(FreeLibrary), E(FreeLibraryAndExitThread),
    E(DisableThreadLibraryCalls), E(SetDllDirectoryA), E(SetDllDirectoryW), E(AddDllDirectory), E(RemoveDllDirectory),
    E(SetDefaultDllDirectories), E(GetProcAddress),
    E(FindResourceA), E(FindResourceW), E(FindResourceExA), E(FindResourceExW), E(LoadResource), E(LockResource),
    E(SizeofResource), E(FreeResource),
    E(GetSystemInfo), E(GetNativeSystemInfo), E(IsWow64Process), E(IsWow64Process2), E(GetActiveProcessorCount),
    E(GetMaximumProcessorCount), E(GetActiveProcessorGroupCount), E(GetCurrentProcessorNumber),
    E(GetVersion), E(GetVersionExA), E(GetVersionExW), E(RtlGetVersion), E(VerifyVersionInfoA), E(VerifyVersionInfoW), E(RtlVerifyVersionInfo),
    E(VerSetConditionMask), E(GetProductInfo),
    E(GetComputerNameA), E(GetComputerNameW), E(GetComputerNameExA), E(GetComputerNameExW),
    E(GetSystemDirectoryA), E(GetSystemDirectoryW), E(GetWindowsDirectoryA), E(GetWindowsDirectoryW),
    E(GetSystemWindowsDirectoryA), E(GetSystemWindowsDirectoryW), E(GetSystemWow64DirectoryA), E(GetSystemWow64DirectoryW),
    E(GlobalMemoryStatusEx), E(GlobalMemoryStatus), E(GetPhysicallyInstalledSystemMemory), E(IsProcessorFeaturePresent),
    E(GetLogicalProcessorInformation),
    E(GetSystemTimeAsFileTime), E(GetSystemTimePreciseAsFileTime), E(GetSystemTime), E(GetLocalTime), E(SetLocalTime), E(SetSystemTime),
    E(FileTimeToSystemTime), E(SystemTimeToFileTime), E(FileTimeToLocalFileTime), E(LocalFileTimeToFileTime),
    E(CompareFileTime), E(GetTickCount), E(GetTickCount64),
    E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(QueryUnbiasedInterruptTime), E(QueryInterruptTime), E(GetSystemTimes),
    E(GetDateFormatA), E(GetDateFormatW), E(GetTimeFormatA), E(GetTimeFormatW), E(GetDateFormatEx), E(GetTimeFormatEx),
    E(GetACP), E(GetOEMCP), E(IsValidCodePage), E(GetCPInfo), E(GetCPInfoExA), E(GetCPInfoExW),
    E(MultiByteToWideChar), E(WideCharToMultiByte), E(IsDBCSLeadByte), E(IsDBCSLeadByteEx),
    E(GetUserDefaultLCID), E(GetSystemDefaultLCID), E(GetThreadLocale), E(SetThreadLocale), E(GetUserDefaultLangID),
    E(GetSystemDefaultLangID), E(GetUserDefaultUILanguage), E(GetSystemDefaultUILanguage), E(GetUserDefaultLocaleName),
    E(GetSystemDefaultLocaleName), E(LCIDToLocaleName), E(LocaleNameToLCID), E(IsValidLocale), E(IsValidLocaleName),
    E(GetUserPreferredUILanguages), E(GetThreadPreferredUILanguages), E(GetProcessPreferredUILanguages), E(SetThreadPreferredUILanguages),
    E(GetLocaleInfoA), E(GetLocaleInfoW), E(GetLocaleInfoEx),
    E(GetStringTypeW), E(GetStringTypeExW), E(GetStringTypeA), E(GetStringTypeExA),
    E(LCMapStringW), E(LCMapStringEx), E(LCMapStringA), E(CompareStringW), E(CompareStringEx), E(CompareStringOrdinal), E(CompareStringA),
    E(FoldStringW),
    E(lstrlenA), E(lstrlenW), E(lstrcmpA), E(lstrcmpiA), E(lstrcmpW), E(lstrcmpiW), E(lstrcpyA), E(lstrcpyW),
    E(lstrcpynA), E(lstrcpynW), E(lstrcatA), E(lstrcatW),
    EX("lstrlen", lstrlenA), EX("lstrcmp", lstrcmpA), EX("lstrcmpi", lstrcmpiA), EX("lstrcpy", lstrcpyA), EX("lstrcpyn", lstrcpynA), EX("lstrcat", lstrcatA),
    E(OutputDebugStringA), E(OutputDebugStringW), E(IsDebuggerPresent), E(CheckRemoteDebuggerPresent), E(DebugBreak),
    E(SetErrorMode), E(GetErrorMode), E(SetThreadErrorMode), E(GetThreadErrorMode), E(Beep),
    E(EncodePointer), E(DecodePointer), E(EncodeSystemPointer), E(DecodeSystemPointer),
    EX("RtlEncodePointer", EncodePointer), EX("RtlDecodePointer", DecodePointer),
    E(FormatMessageA), E(FormatMessageW),
    { NULL, NULL }
};
