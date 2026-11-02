/* C runtime core: program startup, arguments, environment, exit and atexit,
 * errno, memory allocation, signals, sorting, random numbers, threads,
 * child processes and setjmp/longjmp.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "crt.h"
#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void *k32(const char *name) { return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), name); }

/* ================================================================== */
/* errno                                                                */
/* ================================================================== */

int *crt_errno(void) { return &lsiw_thread()->crt_errno; }
void crt_set_errno(int e) { *crt_errno() = e; }

int crt_errno_from_host(int e)
{
    if (e >= 1 && e <= 34) return e;
    switch (e) {
    case EDEADLK:      return MS_EDEADLK;
    case ENAMETOOLONG: return MS_ENAMETOOLONG;
    case ENOLCK:       return MS_ENOLCK;
    case ENOSYS:       return MS_ENOSYS;
    case ENOTEMPTY:    return MS_ENOTEMPTY;
    case EILSEQ:       return MS_EILSEQ;
    case ELOOP:        return MS_ENOENT;
    case EOVERFLOW:    return MS_EFBIG;
    case EOPNOTSUPP:   return MS_ENOSYS;
    case ETIMEDOUT:    return MS_EAGAIN;
    default:           return MS_EINVAL;
    }
}

void crt_set_errno_host(int e)
{
    crt_set_errno(crt_errno_from_host(e));
    unsigned long w = lsiw_errno_to_win(e);
    lsiw_thread()->doserrno = w;
    lsiw_set_last_error(w);       /* the OS call the CRT wraps would have set it */
}

API int *_errno(void) { return crt_errno(); }
API unsigned long *__doserrno(void) { return &lsiw_thread()->doserrno; }
API int _get_errno(int *v) { if (!v) return MS_EINVAL; *v = *crt_errno(); return 0; }
API int _set_errno(int v) { *crt_errno() = v; return 0; }
API int _get_doserrno(unsigned long *v) { if (!v) return MS_EINVAL; *v = lsiw_thread()->doserrno; return 0; }
API int _set_doserrno(unsigned long v) { lsiw_thread()->doserrno = v; return 0; }

/* ================================================================== */
/* Invalid-parameter handling, fatal errors                             */
/* ================================================================== */

typedef void (WINAPI *invalid_param_fn)(const WCHAR *, const WCHAR *, const WCHAR *, unsigned, uintptr_t);
static invalid_param_fn g_invalid_param;
static __thread invalid_param_fn t_invalid_param;

API invalid_param_fn _set_invalid_parameter_handler(invalid_param_fn f) { invalid_param_fn o = g_invalid_param; g_invalid_param = f; return o; }
API invalid_param_fn _get_invalid_parameter_handler(void) { return g_invalid_param; }
API invalid_param_fn _set_thread_local_invalid_parameter_handler(invalid_param_fn f) { invalid_param_fn o = t_invalid_param; t_invalid_param = f; return o; }
API invalid_param_fn _get_thread_local_invalid_parameter_handler(void) { return t_invalid_param; }

API void _invalid_parameter_noinfo_noreturn(void)
{
    fprintf(stderr, "lsiw: the program passed an invalid parameter to a C runtime function\n");
    lsiw_exit_process(0xC0000417);
}

void crt_invalid_parameter(void)
{
    invalid_param_fn f = t_invalid_param ? t_invalid_param : g_invalid_param;
    if (f) { f(NULL, NULL, NULL, 0, 0); return; }
    _invalid_parameter_noinfo_noreturn();
}

API void _invalid_parameter_noinfo(void) { crt_invalid_parameter(); }
API void _invalid_parameter(const WCHAR *e, const WCHAR *fn, const WCHAR *file, unsigned line, uintptr_t r)
{
    invalid_param_fn f = t_invalid_param ? t_invalid_param : g_invalid_param;
    if (f) f(e, fn, file, line, r);
    else _invalid_parameter_noinfo_noreturn();
}
API void _invoke_watson(const WCHAR *e, const WCHAR *fn, const WCHAR *file, unsigned line, uintptr_t r)
{ (void)e; (void)fn; (void)file; (void)line; (void)r; _invalid_parameter_noinfo_noreturn(); }

API void _amsg_exit(int n)
{
    fprintf(stderr, "runtime error R60%02d\n", n);
    lsiw_exit_process(255);
}

API void _purecall(void)
{
    fprintf(stderr, "lsiw: pure virtual function call\n");
    lsiw_exit_process(3);
}
typedef void (WINAPI *purecall_fn)(void);
static purecall_fn g_purecall;
API purecall_fn _set_purecall_handler(purecall_fn f) { purecall_fn o = g_purecall; g_purecall = f; return o; }
API purecall_fn _get_purecall_handler(void) { return g_purecall; }

API void __crtTerminateProcess(UINT code) { lsiw_exit_process(code); }
API void __fastfail(unsigned code) { fprintf(stderr, "lsiw: fast fail (%u)\n", code); lsiw_exit_process(0xC0000409); }

/* ================================================================== */
/* Command line -> argv                                                 */
/* ================================================================== */

int    crt_argc;
char **crt_argv;
WCHAR **crt_wargv;
char  *crt_acmdln;
WCHAR *crt_wcmdln;
char  *crt_pgmptr;
WCHAR *crt_wpgmptr;
static int g_args_done;

/* Split using the Microsoft C runtime rules (UCRT variant). Works on UTF-8;
 * the result shares one allocation. */
static char **split_cmdline(const char *cmd, int *argc_out)
{
    size_t len = strlen(cmd);
    char *buf = malloc(len + 1);
    char **argv = malloc(sizeof(char *) * (len / 2 + 2));
    int argc = 0;
    const char *p = cmd;
    char *o = buf;

    /* argv[0]: quotes delimit, backslashes are literal. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p) {
        argv[argc++] = o;
        if (*p == '"') { p++; while (*p && *p != '"') *o++ = *p++; if (*p == '"') p++; }
        else while (*p && *p != ' ' && *p != '\t') *o++ = *p++;
        *o++ = 0;
    }
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = o;
        int inq = 0;
        while (*p) {
            if (!inq && (*p == ' ' || *p == '\t')) break;
            if (*p == '\\') {
                size_t n = 0;
                while (p[n] == '\\') n++;
                if (p[n] == '"') {
                    for (size_t i = 0; i < n / 2; i++) *o++ = '\\';
                    p += n;
                    if (n % 2) { *o++ = '"'; p++; }
                    continue;
                }
                for (size_t i = 0; i < n; i++) *o++ = '\\';
                p += n;
                continue;
            }
            if (*p == '"') {
                if (inq && p[1] == '"') { *o++ = '"'; p += 2; continue; }
                inq = !inq;
                p++;
                continue;
            }
            *o++ = *p++;
        }
        *o++ = 0;
    }
    argv[argc] = NULL;
    *argc_out = argc;
    return argv;
}

static void init_args(void)
{
    if (g_args_done) return;
    g_args_done = 1;
    const char *cmd = lsiw_cmdline_a();
    crt_acmdln = strdup(cmd ? cmd : "");
    crt_wcmdln = lsiw_u2w(crt_acmdln);
    crt_argv = split_cmdline(crt_acmdln, &crt_argc);
    crt_wargv = malloc(sizeof(WCHAR *) * (crt_argc + 1));
    for (int i = 0; i < crt_argc; i++) crt_wargv[i] = lsiw_u2w(crt_argv[i]);
    crt_wargv[crt_argc] = NULL;
    const char *exe = lsiw_main_module ? lsiw_main_module->win_path : (crt_argc ? crt_argv[0] : "");
    crt_pgmptr = strdup(exe ? exe : "");
    crt_wpgmptr = lsiw_u2w(crt_pgmptr);
}

/* ================================================================== */
/* Environment                                                          */
/* ================================================================== */

char  **crt_environ;
WCHAR **crt_wenviron;
char  **crt_initenv;
WCHAR **crt_winitenv;

static void rebuild_wide_env(void)
{
    char **e = lsiw_env_block();
    size_t n = 0;
    while (e && e[n]) n++;
    WCHAR **w = malloc(sizeof(WCHAR *) * (n + 1));
    for (size_t i = 0; i < n; i++) w[i] = lsiw_u2w(e[i]);
    w[n] = NULL;
    /* The previous array is leaked on purpose: programs may still hold it. */
    crt_wenviron = w;
}

void crt_env_changed(void)
{
    crt_environ = lsiw_env_block();
    rebuild_wide_env();
}

static void init_env(void)
{
    if (crt_environ) return;
    crt_environ = lsiw_env_block();
    rebuild_wide_env();
    crt_initenv = crt_environ;
    crt_winitenv = crt_wenviron;
}

API char *ms_getenv(const char *name)
{
    if (!name) { crt_invalid_parameter(); return NULL; }
    return (char *)lsiw_env_get(name);
}

API WCHAR *_wgetenv(const WCHAR *name)
{
    if (!name) { crt_invalid_parameter(); return NULL; }
    init_env();
    size_t nl = lsiw_wcslen(name);
    for (WCHAR **e = crt_wenviron; *e; e++) {
        size_t i = 0;
        for (; i < nl; i++) {
            WCHAR a = (*e)[i], b = name[i];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (i == nl && (*e)[nl] == '=') return *e + nl + 1;
    }
    return NULL;
}

API int getenv_s(size_t *ret, char *buf, size_t n, const char *name)
{
    const char *v = ms_getenv(name);
    if (!v) { if (ret) *ret = 0; if (buf && n) buf[0] = 0; return 0; }
    size_t l = strlen(v) + 1;
    if (ret) *ret = l;
    if (!buf) return 0;
    if (n < l) { if (n) buf[0] = 0; return MS_ERANGE; }
    memcpy(buf, v, l);
    return 0;
}
API int _wgetenv_s(size_t *ret, WCHAR *buf, size_t n, const WCHAR *name)
{
    const WCHAR *v = _wgetenv(name);
    if (!v) { if (ret) *ret = 0; if (buf && n) buf[0] = 0; return 0; }
    size_t l = lsiw_wcslen(v) + 1;
    if (ret) *ret = l;
    if (!buf) return 0;
    if (n < l) { if (n) buf[0] = 0; return MS_ERANGE; }
    memcpy(buf, v, l * 2);
    return 0;
}
API int _dupenv_s(char **out, size_t *len, const char *name)
{
    const char *v = ms_getenv(name);
    *out = NULL;
    if (len) *len = 0;
    if (!v) return 0;
    size_t l = strlen(v) + 1;
    *out = lsiw_heap_alloc(l, 0);
    memcpy(*out, v, l);
    if (len) *len = l;
    return 0;
}
API int _wdupenv_s(WCHAR **out, size_t *len, const WCHAR *name)
{
    const WCHAR *v = _wgetenv(name);
    *out = NULL;
    if (len) *len = 0;
    if (!v) return 0;
    size_t l = lsiw_wcslen(v) + 1;
    *out = lsiw_heap_alloc(l * 2, 0);
    memcpy(*out, v, l * 2);
    if (len) *len = l;
    return 0;
}

static int put_env(const char *s)
{
    const char *eq = strchr(s, '=');
    if (!eq || eq == s) { crt_set_errno(MS_EINVAL); return -1; }
    char *name = strndup(s, (size_t)(eq - s));
    int r = lsiw_env_set(name, eq[1] ? eq + 1 : NULL);
    free(name);
    crt_env_changed();
    return r < 0 ? -1 : 0;
}
API int _putenv(const char *s) { return put_env(s); }
API int _wputenv(const WCHAR *s) { char *u = lsiw_w2u(s); int r = put_env(u); free(u); return r; }
API int _putenv_s(const char *n, const char *v) { int r = lsiw_env_set(n, *v ? v : NULL); crt_env_changed(); return r < 0 ? MS_EINVAL : 0; }
API int _wputenv_s(const WCHAR *n, const WCHAR *v)
{
    char *a = lsiw_w2u(n), *b = lsiw_w2u(v);
    int r = lsiw_env_set(a, *b ? b : NULL);
    free(a); free(b);
    crt_env_changed();
    return r < 0 ? MS_EINVAL : 0;
}

/* ================================================================== */
/* Startup                                                              */
/* ================================================================== */

int crt_fmode = O_MS_TEXT;
int crt_commode = 0;
static int g_app_type;

API void __set_app_type(int t) { g_app_type = t; }
API void _set_app_type(int t) { g_app_type = t; }
API int *__p__fmode(void) { return &crt_fmode; }
API int *__p__commode(void) { return &crt_commode; }
API int _set_fmode(int m) { crt_fmode = m; return 0; }
API int _get_fmode(int *m) { if (!m) return MS_EINVAL; *m = crt_fmode; return 0; }

API int __getmainargs(int *argc, char ***argv, char ***env, int glob, void *si)
{
    (void)glob; (void)si;
    init_args(); init_env();
    *argc = crt_argc; *argv = crt_argv;
    if (env) *env = crt_environ;
    return 0;
}
API int __wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env, int glob, void *si)
{
    (void)glob; (void)si;
    init_args(); init_env();
    *argc = crt_argc; *argv = crt_wargv;
    if (env) *env = crt_wenviron;
    return 0;
}
API int *__p___argc(void) { init_args(); return &crt_argc; }
API char ***__p___argv(void) { init_args(); return &crt_argv; }
API WCHAR ***__p___wargv(void) { init_args(); return &crt_wargv; }
API char **__p__acmdln(void) { init_args(); return &crt_acmdln; }
API WCHAR **__p__wcmdln(void) { init_args(); return &crt_wcmdln; }
API char ***__p__environ(void) { init_env(); return &crt_environ; }
API WCHAR ***__p__wenviron(void) { init_env(); return &crt_wenviron; }
API char ***__p___initenv(void) { init_env(); return &crt_initenv; }
API WCHAR ***__p___winitenv(void) { init_env(); return &crt_winitenv; }
API char **__p__pgmptr(void) { init_args(); return &crt_pgmptr; }
API WCHAR **__p__wpgmptr(void) { init_args(); return &crt_wpgmptr; }
API int _get_pgmptr(char **p) { init_args(); *p = crt_pgmptr; return 0; }
API int _get_wpgmptr(WCHAR **p) { init_args(); *p = crt_wpgmptr; return 0; }

/* ucrt startup */
API int _configure_narrow_argv(int mode) { (void)mode; init_args(); return 0; }
API int _configure_wide_argv(int mode) { (void)mode; init_args(); return 0; }
API int _initialize_narrow_environment(void) { init_env(); return 0; }
API int _initialize_wide_environment(void) { init_env(); return 0; }
API char **_get_initial_narrow_environment(void) { init_env(); return crt_initenv; }
API WCHAR **_get_initial_wide_environment(void) { init_env(); return crt_winitenv; }
API char *_get_narrow_winmain_command_line(void)
{
    init_args();
    const char *p = crt_acmdln;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return (char *)p;
}
API WCHAR *_get_wide_winmain_command_line(void)
{
    init_args();
    const WCHAR *p = crt_wcmdln;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return (WCHAR *)p;
}

API void _initterm(ms_vfn *start, ms_vfn *end)
{
    for (; start < end; start++) if (*start) (*start)();
}
API int _initterm_e(ms_ifn *start, ms_ifn *end)
{
    for (; start < end; start++) if (*start) { int r = (*start)(); if (r) return r; }
    return 0;
}

API int _seh_filter_exe(DWORD code, void *ptrs) { (void)code; (void)ptrs; return 0; /* EXCEPTION_CONTINUE_SEARCH */ }
API int _seh_filter_dll(DWORD code, void *ptrs) { (void)code; (void)ptrs; return 0; }
API int _XcptFilter(DWORD code, void *ptrs) { (void)code; (void)ptrs; return 0; }
API void __setusermatherr(void *f) { (void)f; }
API int _matherr(void *e) { (void)e; return 0; }
API int _set_new_mode(int m) { static int mode; int o = mode; mode = m; return o; }
API int _query_new_mode(void) { return 0; }
typedef int (WINAPI *new_handler_fn)(size_t);
static new_handler_fn g_new_handler;
API new_handler_fn _set_new_handler(new_handler_fn f) { new_handler_fn o = g_new_handler; g_new_handler = f; return o; }
API new_handler_fn _query_new_handler(void) { return g_new_handler; }
API int _callnewh(size_t n) { return g_new_handler ? g_new_handler(n) : 0; }
API int _configthreadlocale(int m) { (void)m; return 1; /* _ENABLE_PER_THREAD_LOCALE off */ }
API void __security_init_cookie(void) {}
API void __crt_debugger_hook(int r) { (void)r; }
API void _CrtDbgBreak(void) {}
API int _CrtDbgReport(int t, const char *f, int l, const char *m, const char *fmt, ...) { (void)t; (void)f; (void)l; (void)m; (void)fmt; return 0; }
API int _CrtSetReportMode(int t, int m) { (void)t; (void)m; return 0; }
API int _CrtSetDbgFlag(int f) { (void)f; return 0; }
API void *_CrtSetReportHook(void *f) { (void)f; return NULL; }
API int _set_error_mode(int m) { static int mode; int o = mode; if (m >= 0) mode = m; return o; }
API unsigned _set_abort_behavior(unsigned f, unsigned m) { (void)f; (void)m; return 1; }

/* ================================================================== */
/* exit, atexit, _onexit                                                */
/* ================================================================== */

typedef struct { ms_vfn *first, *last, *end; } onexit_table;

static ms_vfn *g_atexit;
static size_t g_natexit, g_capatexit;
static pthread_mutex_t g_exit_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_exit_handlers_ran;

static int add_atexit(ms_vfn f)
{
    pthread_mutex_lock(&g_exit_lock);
    if (g_natexit == g_capatexit) {
        g_capatexit = g_capatexit ? g_capatexit * 2 : 64;
        g_atexit = realloc(g_atexit, g_capatexit * sizeof *g_atexit);
    }
    g_atexit[g_natexit++] = f;
    pthread_mutex_unlock(&g_exit_lock);
    return 0;
}

void lsiw_run_exit_handlers(void)
{
    if (__atomic_exchange_n(&g_exit_handlers_ran, 1, __ATOMIC_SEQ_CST)) return;
    for (;;) {
        pthread_mutex_lock(&g_exit_lock);
        if (!g_natexit) { pthread_mutex_unlock(&g_exit_lock); break; }
        ms_vfn f = g_atexit[--g_natexit];
        pthread_mutex_unlock(&g_exit_lock);
        if (f) f();
    }
    lsiw_crt_flush_all();
}

API int ms_atexit(ms_vfn f) { return add_atexit(f); }
API ms_vfn _onexit(ms_vfn f) { add_atexit(f); return f; }
API int _crt_atexit(ms_vfn f) { return add_atexit(f); }
API int _crt_at_quick_exit(ms_vfn f) { (void)f; return 0; }
API int ms_at_quick_exit(ms_vfn f) { (void)f; return 0; }

API ms_vfn __dllonexit(ms_vfn f, ms_vfn **begin, ms_vfn **end)
{
    size_t n = (size_t)(*end - *begin);
    ms_vfn *t = realloc(*begin, (n + 1) * sizeof *t);
    if (!t) return NULL;
    t[n] = f;
    *begin = t;
    *end = t + n + 1;
    return f;
}

API int _initialize_onexit_table(onexit_table *t)
{
    if (!t) return -1;
    if (t->first == t->end) t->first = t->last = t->end = NULL;
    return 0;
}
API int _register_onexit_function(onexit_table *t, ms_vfn f)
{
    if (!t) return -1;
    pthread_mutex_lock(&g_exit_lock);
    if (t->last == t->end) {
        size_t n = (size_t)(t->end - t->first), cap = n ? n * 2 : 32;
        ms_vfn *nf = realloc(t->first, cap * sizeof *nf);
        if (!nf) { pthread_mutex_unlock(&g_exit_lock); return -1; }
        t->last = nf + (t->last - t->first);
        t->first = nf;
        t->end = nf + cap;
    }
    *t->last++ = f;
    pthread_mutex_unlock(&g_exit_lock);
    return 0;
}
API int _execute_onexit_table(onexit_table *t)
{
    if (!t) return -1;
    pthread_mutex_lock(&g_exit_lock);
    ms_vfn *first = t->first, *last = t->last;
    t->first = t->last = t->end = NULL;
    pthread_mutex_unlock(&g_exit_lock);
    while (last > first) { ms_vfn f = *--last; if (f) f(); }
    free(first);
    return 0;
}

typedef void (WINAPI *tls_atexit_fn)(void *, DWORD, void *);
static tls_atexit_fn g_tls_atexit;
API void _register_thread_local_exe_atexit_callback(tls_atexit_fn f) { g_tls_atexit = f; }

API void ms_exit(int code) { lsiw_run_exit_handlers(); lsiw_exit_process((uint32_t)code); }
API void ms__exit(int code) { lsiw_crt_flush_all(); fflush(NULL); _exit(code); }
API void ms__Exit(int code) { lsiw_crt_flush_all(); fflush(NULL); _exit(code); }
API void ms_quick_exit(int code) { lsiw_crt_flush_all(); fflush(NULL); _exit(code); }
API void _cexit(void) { lsiw_run_exit_handlers(); }
API void _c_exit(void) { lsiw_crt_flush_all(); }

/* ================================================================== */
/* Signals, abort                                                       */
/* ================================================================== */

#define MS_SIGINT   2
#define MS_SIGILL   4
#define MS_SIGFPE   8
#define MS_SIGSEGV  11
#define MS_SIGTERM  15
#define MS_SIGBREAK 21
#define MS_SIGABRT  22
#define MS_SIGABRT_COMPAT 6

typedef void (WINAPI *ms_sig_fn)(int);
#define MS_SIG_DFL ((ms_sig_fn)0)
#define MS_SIG_IGN ((ms_sig_fn)1)
#define MS_SIG_ERR ((ms_sig_fn)-1)

static ms_sig_fn g_sig[32];

static void host_signal(int hs)
{
    int ms = hs == SIGINT ? MS_SIGINT : hs == SIGTERM ? MS_SIGTERM : hs == SIGQUIT ? MS_SIGBREAK : 0;
    ms_sig_fn f = ms ? g_sig[ms] : NULL;
    if (f && f != MS_SIG_IGN && f != MS_SIG_DFL) {
        g_sig[ms] = MS_SIG_DFL;    /* one-shot, as on Windows */
        f(ms);
    }
}

API ms_sig_fn ms_signal(int sig, ms_sig_fn f)
{
    if (sig <= 0 || sig >= 32) { crt_set_errno(MS_EINVAL); return MS_SIG_ERR; }
    if (sig == MS_SIGABRT_COMPAT) sig = MS_SIGABRT;
    ms_sig_fn old = g_sig[sig];
    g_sig[sig] = f;
    int hs = sig == MS_SIGINT ? SIGINT : sig == MS_SIGTERM ? SIGTERM : sig == MS_SIGBREAK ? SIGQUIT : 0;
    if (hs) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        if (f == MS_SIG_IGN) sa.sa_handler = SIG_IGN;
        else if (f == MS_SIG_DFL) sa.sa_handler = SIG_DFL;
        else { sa.sa_handler = host_signal; sa.sa_flags = SA_RESTART; }
        sigaction(hs, &sa, NULL);
    }
    return old;
}

API int ms_raise(int sig)
{
    if (sig == MS_SIGABRT_COMPAT) sig = MS_SIGABRT;
    if (sig <= 0 || sig >= 32) { crt_set_errno(MS_EINVAL); return -1; }
    ms_sig_fn f = g_sig[sig];
    if (f == MS_SIG_IGN) return 0;
    if (f == MS_SIG_DFL) {
        if (sig == MS_SIGABRT) { lsiw_crt_flush_all(); _exit(3); }
        if (sig == MS_SIGINT) lsiw_exit_process(0xC000013A);
        lsiw_exit_process(3);
    }
    if (sig != MS_SIGFPE && sig != MS_SIGILL && sig != MS_SIGSEGV) g_sig[sig] = MS_SIG_DFL;
    f(sig);
    return 0;
}

API void ms_abort(void)
{
    if (g_sig[MS_SIGABRT] && g_sig[MS_SIGABRT] != MS_SIG_IGN) ms_raise(MS_SIGABRT);
    fprintf(stderr, "\nThis application has requested the Runtime to terminate it in an unusual way.\n");
    lsiw_crt_flush_all();
    _exit(3);
}

/* assert() failures: the message console programs print, then abort(). */
API void _assert(const char *expr, const char *file, unsigned line)
{
    lsiw_crt_flush_all();
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", expr ? expr : "?", file ? file : "?", line);
    ms_abort();
}
API void _wassert(const WCHAR *expr, const WCHAR *file, unsigned line)
{
    char *e = expr ? lsiw_w2u(expr) : NULL, *f = file ? lsiw_w2u(file) : NULL;
    lsiw_crt_flush_all();
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", e ? e : "?", f ? f : "?", line);
    free(e); free(f);
    ms_abort();
}

API void *__pxcptinfoptrs(void) { static __thread void *p; return &p; }
API int *__fpecode(void) { static __thread int c; return &c; }

/* ================================================================== */
/* Memory                                                               */
/* ================================================================== */

API void *ms_malloc(size_t n)
{
    void *p = lsiw_heap_alloc(n ? n : 1, 0);
    if (!p) crt_set_errno(MS_ENOMEM);
    return p;
}
API void *ms_calloc(size_t n, size_t s)
{
    if (s && n > SIZE_MAX / s) { crt_set_errno(MS_ENOMEM); return NULL; }
    size_t nb = n * s;
    void *p = lsiw_heap_alloc(nb ? nb : 1, 1);
    if (!p) crt_set_errno(MS_ENOMEM);
    return p;
}
API void ms_free(void *p) { if (p) lsiw_heap_free(p); }
API void *ms_realloc(void *p, size_t n)
{
    if (!p) return ms_malloc(n);
    if (!n) { lsiw_heap_free(p); return NULL; }
    void *r = lsiw_heap_realloc(p, n);
    if (!r) crt_set_errno(MS_ENOMEM);
    return r;
}
API size_t _msize(void *p) { if (!p) { crt_invalid_parameter(); return (size_t)-1; } return lsiw_heap_size(p); }
API void *_expand(void *p, size_t n) { return (p && n <= lsiw_heap_size(p)) ? p : NULL; }
API void *_recalloc(void *p, size_t n, size_t s)
{
    size_t old = p ? lsiw_heap_size(p) : 0, nb = n * s;
    void *r = ms_realloc(p, nb);
    if (r && nb > old) memset((char *)r + old, 0, nb - old);
    return r;
}
API void _free_base(void *p) { ms_free(p); }
API void *_malloc_base(size_t n) { return ms_malloc(n); }
API void *_calloc_base(size_t n, size_t s) { return ms_calloc(n, s); }
API void *_realloc_base(void *p, size_t n) { return ms_realloc(p, n); }
API size_t _msize_base(void *p) { return lsiw_heap_size(p); }
API void *_malloc_crt(size_t n) { return ms_malloc(n); }
API void *_calloc_crt(size_t n, size_t s) { return ms_calloc(n, s); }

/* Aligned blocks: [padding][original pointer][user data...] */
API void *_aligned_offset_malloc(size_t n, size_t align, size_t off)
{
    if (align & (align - 1)) { crt_set_errno(MS_EINVAL); return NULL; }
    if (align < sizeof(void *)) align = sizeof(void *);
    char *raw = lsiw_heap_alloc(n + align + sizeof(void *), 0);
    if (!raw) { crt_set_errno(MS_ENOMEM); return NULL; }
    uintptr_t u = ((uintptr_t)raw + sizeof(void *) + off + align - 1) & ~(uintptr_t)(align - 1);
    u -= off;
    ((void **)u)[-1] = raw;
    return (void *)u;
}
API void *_aligned_malloc(size_t n, size_t align) { return _aligned_offset_malloc(n, align, 0); }
API void _aligned_free(void *p) { if (p) lsiw_heap_free(((void **)p)[-1]); }
API size_t _aligned_msize(void *p, size_t align, size_t off)
{
    char *raw = ((void **)p)[-1];
    (void)align; (void)off;
    return lsiw_heap_size(raw) - (size_t)((char *)p - raw);
}
API void *_aligned_offset_realloc(void *p, size_t n, size_t align, size_t off)
{
    if (!p) return _aligned_offset_malloc(n, align, off);
    if (!n) { _aligned_free(p); return NULL; }
    size_t old = _aligned_msize(p, align, off);
    void *np = _aligned_offset_malloc(n, align, off);
    if (!np) return NULL;
    memcpy(np, p, old < n ? old : n);
    _aligned_free(p);
    return np;
}
API void *_aligned_realloc(void *p, size_t n, size_t align) { return _aligned_offset_realloc(p, n, align, 0); }
API void *_aligned_recalloc(void *p, size_t c, size_t s, size_t align)
{
    size_t old = p ? _aligned_msize(p, align, 0) : 0, n = c * s;
    void *r = _aligned_realloc(p, n, align);
    if (r && n > old) memset((char *)r + old, 0, n - old);
    return r;
}
API int _heapchk(void) { return -2; /* _HEAPOK */ }
API int _heapmin(void) { return 0; }
API int _heapset(unsigned v) { (void)v; return -2; }
API int _heapwalk(void *e) { (void)e; return -5; /* _HEAPEND */ }
API HANDLE _get_heap_handle(void) { return lsiw_process_heap(); }
API size_t _get_sbh_threshold(void) { return 0; }
API int _set_sbh_threshold(size_t n) { (void)n; return 0; }

/* ================================================================== */
/* Sorting and searching (ms_abi callbacks)                             */
/* ================================================================== */

static int qsort_tramp(const void *a, const void *b, void *ctx) { return ((ms_cmp_fn)ctx)(a, b); }
struct qs_ctx { ms_cmp_s_fn f; void *c; };
static int qsort_s_tramp(const void *a, const void *b, void *ctx) { struct qs_ctx *q = ctx; return q->f(q->c, a, b); }

API void ms_qsort(void *base, size_t n, size_t size, ms_cmp_fn cmp)
{
    if (n > 1) qsort_r(base, n, size, qsort_tramp, (void *)cmp);
}
API void qsort_s(void *base, size_t n, size_t size, ms_cmp_s_fn cmp, void *ctx)
{
    struct qs_ctx q = { cmp, ctx };
    if (n > 1) qsort_r(base, n, size, qsort_s_tramp, &q);
}
API void *ms_bsearch(const void *key, const void *base, size_t n, size_t size, ms_cmp_fn cmp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(key, p);
        if (!c) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
API void *bsearch_s(const void *key, const void *base, size_t n, size_t size, ms_cmp_s_fn cmp, void *ctx)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(ctx, key, p);
        if (!c) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
API void *_lfind(const void *key, const void *base, unsigned *n, unsigned size, ms_cmp_fn cmp)
{
    for (unsigned i = 0; i < *n; i++) { const char *p = (const char *)base + (size_t)i * size; if (!cmp(key, p)) return (void *)p; }
    return NULL;
}
API void *_lsearch(const void *key, void *base, unsigned *n, unsigned size, ms_cmp_fn cmp)
{
    void *r = _lfind(key, base, n, size, cmp);
    if (r) return r;
    char *p = (char *)base + (size_t)*n * size;
    memcpy(p, key, size);
    (*n)++;
    return p;
}

/* ================================================================== */
/* Random numbers, integer helpers                                      */
/* ================================================================== */

static __thread unsigned t_rand_seed = 1;
API void ms_srand(unsigned s) { t_rand_seed = s; }
API int ms_rand(void) { t_rand_seed = t_rand_seed * 214013u + 2531011u; return (int)((t_rand_seed >> 16) & 0x7fff); }
API int rand_s(unsigned *out)
{
    if (!out) { crt_set_errno(MS_EINVAL); return MS_EINVAL; }
    if (getrandom(out, sizeof *out, 0) != sizeof *out) *out = (unsigned)ms_rand() << 16 ^ (unsigned)ms_rand();
    return 0;
}

API int ms_abs(int x) { return x < 0 ? -x : x; }
API long ms_labs(long x) { return x < 0 ? -x : x; }
API long long ms_llabs(long long x) { return x < 0 ? -x : x; }
API long long _abs64(long long x) { return x < 0 ? -x : x; }
API long long ms_imaxabs(long long x) { return x < 0 ? -x : x; }
typedef struct { int quot, rem; } ms_div_t;
typedef struct { int32_t quot, rem; } ms_ldiv_t;
typedef struct { long long quot, rem; } ms_lldiv_t;
API ms_div_t ms_div(int a, int b) { return (ms_div_t){ a / b, a % b }; }
API int64_t ms_ldiv(int32_t a, int32_t b) { ms_ldiv_t r = { a / b, a % b }; int64_t v; memcpy(&v, &r, 8); return v; }
API ms_lldiv_t ms_lldiv(long long a, long long b) { return (ms_lldiv_t){ a / b, a % b }; }
API ms_lldiv_t ms_imaxdiv(long long a, long long b) { return (ms_lldiv_t){ a / b, a % b }; }
API unsigned _rotl(unsigned v, int s) { s &= 31; return (v << s) | (v >> ((32 - s) & 31)); }
API unsigned _rotr(unsigned v, int s) { s &= 31; return (v >> s) | (v << ((32 - s) & 31)); }
API unsigned long long _rotl64(unsigned long long v, int s) { s &= 63; return (v << s) | (v >> ((64 - s) & 63)); }
API unsigned long long _rotr64(unsigned long long v, int s) { s &= 63; return (v >> s) | (v << ((64 - s) & 63)); }
API uint32_t _lrotl(uint32_t v, int s) { return _rotl(v, s); }
API uint32_t _lrotr(uint32_t v, int s) { return _rotr(v, s); }
API unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
API uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
API unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }
API void _swab(char *src, char *dst, int n) { for (int i = 0; i + 1 < n; i += 2) { char a = src[i], b = src[i + 1]; dst[i] = b; dst[i + 1] = a; } }

/* ================================================================== */
/* Process and thread helpers                                           */
/* ================================================================== */

API int _getpid(void) { return getpid(); }
API void _sleep(unsigned long ms) { void (WINAPI *s)(DWORD) = k32("Sleep"); s((DWORD)ms); }
API unsigned long __threadid(void) { return lsiw_thread()->tid; }
API uintptr_t __threadhandle(void) { return (uintptr_t)-2; }

typedef unsigned (WINAPI *beginthreadex_fn)(void *);
typedef void (WINAPI *beginthread_fn)(void *);
struct bt_start { beginthread_fn fn; void *arg; HANDLE h; };

static DWORD WINAPI beginthread_tramp(void *p)
{
    struct bt_start s = *(struct bt_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

API uintptr_t _beginthreadex(void *sec, unsigned stack, beginthreadex_fn fn, void *arg, unsigned flags, unsigned *tid)
{
    HANDLE (WINAPI *ct)(void *, SIZE_T, void *, void *, DWORD, DWORD *) = k32("CreateThread");
    DWORD t = 0;
    HANDLE h = ct(sec, stack, (void *)fn, arg, flags, &t);
    if (!h) { crt_set_errno(MS_EAGAIN); return 0; }
    if (tid) *tid = t;
    return (uintptr_t)h;
}
API uintptr_t _beginthread(beginthread_fn fn, unsigned stack, void *arg)
{
    HANDLE (WINAPI *ct)(void *, SIZE_T, void *, void *, DWORD, DWORD *) = k32("CreateThread");
    struct bt_start *s = malloc(sizeof *s);
    s->fn = fn; s->arg = arg;
    HANDLE h = ct(NULL, stack, (void *)beginthread_tramp, s, 0, NULL);
    if (!h) { free(s); crt_set_errno(MS_EAGAIN); return (uintptr_t)-1; }
    return (uintptr_t)h;
}
API void _endthreadex(unsigned code) { void (WINAPI *et)(DWORD) = k32("ExitThread"); et(code); }
API void _endthread(void) { _endthreadex(0); }

/* ---- system(): a small cmd.exe for the common built-in commands ---- */

static int run_host_shell(const char *cmd)
{
    lsiw_crt_flush_all();
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int read_any_key(void)
{
    struct termios t, saved;
    int tty = isatty(0) && tcgetattr(0, &t) == 0;
    if (tty) { saved = t; t.c_lflag &= ~(tcflag_t)(ICANON | ECHO); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; tcsetattr(0, TCSANOW, &t); }
    char c;
    ssize_t r = read(0, &c, 1);
    if (tty) tcsetattr(0, TCSANOW, &saved);
    return r == 1 ? c : -1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
    return s;
}

static int cmd_builtin(char *line, int *handled)
{
    *handled = 1;
    char *cmd = trim(line);
    if (*cmd == '@') cmd = trim(cmd + 1);
    char word[32];
    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' && cmd[i] != '.' && i < sizeof word - 1) { word[i] = (char)tolower((unsigned char)cmd[i]); i++; }
    word[i] = 0;
    char *arg = trim(cmd + i);
    lsiw_crt_flush_all();

    if (!strcmp(word, "cls")) { if (isatty(1)) { ssize_t r = write(1, "\x1b[2J\x1b[3J\x1b[H", 11); (void)r; } return 0; }
    if (!strcmp(word, "pause")) {
        fputs("Press any key to continue . . . ", stdout); fflush(stdout);
        read_any_key();
        fputs("\n", stdout); fflush(stdout);
        return 0;
    }
    if (!strcmp(word, "echo")) {
        if (cmd[i] == '.') { puts(""); fflush(stdout); return 0; }
        if (!*arg || !strcasecmp(arg, "on") || !strcasecmp(arg, "off")) return 0;
        puts(arg); fflush(stdout);
        return 0;
    }
    if (!strcmp(word, "title")) {
        BOOL (WINAPI *st)(const char *) = k32("SetConsoleTitleA");
        st(arg);
        return 0;
    }
    if (!strcmp(word, "color") || !strcmp(word, "chcp") || !strcmp(word, "mode") || !strcmp(word, "rem")) return 0;
    if (!strcmp(word, "ver")) { puts("\nMicrosoft Windows [Version 10.0.19045] (LSIW)\n"); fflush(stdout); return 0; }
    if (!strcmp(word, "cd") || !strcmp(word, "chdir")) {
        if (!*arg) { char *c = getcwd(NULL, 0); char *w = lsiw_linux_to_win(c ? c : "/"); puts(w); free(w); free(c); fflush(stdout); return 0; }
        if (!strncasecmp(arg, "/d ", 3)) arg = trim(arg + 3);
        char *p = lsiw_win_to_linux(arg);
        int r = chdir(p);
        free(p);
        if (r) { fputs("The system cannot find the path specified.\n", stderr); return 1; }
        return 0;
    }
    if (!strcmp(word, "md") || !strcmp(word, "mkdir")) { char *p = lsiw_win_to_linux(arg); int r = lsiw_mkdirs(p, 0777); free(p); return r ? 1 : 0; }
    if (!strcmp(word, "rd") || !strcmp(word, "rmdir")) {
        int recursive = 0;
        while (*arg == '/') { if (tolower((unsigned char)arg[1]) == 's') recursive = 1; arg = trim(arg + 2); }
        char *p = lsiw_win_to_linux(arg);
        int r;
        if (recursive) { char *c = lsiw_strdup_printf("rm -rf -- '%s'", p); r = run_host_shell(c); free(c); }
        else r = rmdir(p);
        free(p);
        return r ? 1 : 0;
    }
    if (!strcmp(word, "del") || !strcmp(word, "erase")) {
        while (*arg == '/') arg = trim(arg + 2);
        char *p = lsiw_win_to_linux(arg);
        int r = unlink(p);
        free(p);
        if (r) { fprintf(stderr, "Could Not Find %s\n", arg); }
        return 0;
    }
    if (!strcmp(word, "type")) {
        char *p = lsiw_win_to_linux(arg);
        FILE *f = fopen(p, "rb");
        free(p);
        if (!f) { fputs("The system cannot find the file specified.\n", stderr); return 1; }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
        fclose(f); fflush(stdout);
        return 0;
    }
    if (!strcmp(word, "exit")) return atoi(arg);
    if (!strcmp(word, "timeout")) {
        int secs = 0;
        for (char *a = arg; *a; a++) if (*a == '/' && tolower((unsigned char)a[1]) == 't') secs = atoi(trim(a + 2));
        if (!secs) secs = atoi(arg);
        if (secs > 0) sleep((unsigned)secs);
        return 0;
    }
    *handled = 0;
    return 0;
}

/* Run "C:\path\prog.exe args" through LSIW itself; anything else through
 * /bin/sh. */
static int run_command(const char *cmd)
{
    char first[4096];
    const char *p = cmd;
    while (*p == ' ') p++;
    size_t i = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && i < sizeof first - 1) first[i++] = *p++; if (*p) p++; }
    else while (*p && *p != ' ' && i < sizeof first - 1) first[i++] = *p++;
    first[i] = 0;
    size_t fl = strlen(first);
    int is_exe = fl > 4 && (!strcasecmp(first + fl - 4, ".exe") || !strcasecmp(first + fl - 4, ".com"));
    char *lin = lsiw_win_to_linux(first);
    if (!is_exe && access(lin, F_OK) != 0) {
        /* "prog" without extension: try prog.exe */
        char *w = lsiw_strdup_printf("%s.exe", first);
        char *l2 = lsiw_win_to_linux(w);
        if (access(l2, F_OK) == 0) { free(lin); lin = l2; is_exe = 1; } else free(l2);
        free(w);
    }
    if (is_exe && access(lin, F_OK) == 0) {
        BOOL (WINAPI *cp)(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, void *, void *) = k32("CreateProcessA");
        DWORD (WINAPI *wait)(HANDLE, DWORD) = k32("WaitForSingleObject");
        BOOL (WINAPI *gec)(HANDLE, DWORD *) = k32("GetExitCodeProcess");
        BOOL (WINAPI *ch)(HANDLE) = k32("CloseHandle");
        struct { DWORD cb; char pad[100]; } si = { 104, { 0 } };
        struct { HANDLE hp, ht; DWORD pid, tid; } pi;
        char *line = strdup(cmd);
        lsiw_crt_flush_all();
        BOOL ok = cp(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
        free(line);
        free(lin);
        if (!ok) return -1;
        wait(pi.hp, INFINITE);
        DWORD code = 0;
        gec(pi.hp, &code);
        ch(pi.hp); ch(pi.ht);
        return (int)code;
    }
    free(lin);
    return run_host_shell(cmd);
}

/* cmd.exe /c quote handling: a command starting with '"' that holds more
 * than two quotes loses the first and the last one, so
 * ""C:\a b\x.exe" arg" runs "C:\a b\x.exe" arg. */
char *crt_cmd_strip_quotes(const char *cmd)
{
    const char *p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    int q = 0;
    for (const char *c = p; *c; c++) q += *c == '"';
    if (*p != '"' || q <= 2) return strdup(cmd);
    char *out = strdup(p + 1);
    char *last = strrchr(out, '"');
    if (last) memmove(last, last + 1, strlen(last));
    return out;
}

static int system_line(const char *cmd);

API int ms_system(const char *cmd)
{
    if (!cmd) return 1;
    char *stripped = crt_cmd_strip_quotes(cmd);
    int r = system_line(stripped);
    free(stripped);
    return r;
}

static int system_line(const char *cmd)
{
    char *copy = strdup(cmd);
    /* Handle simple "a & b" / "a && b" chains of built-ins. */
    int rc = 0;
    char *save = NULL;
    int all_builtin = 1;
    for (char *part = strtok_r(copy, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
        int handled;
        char *dup = strdup(part);
        rc = cmd_builtin(dup, &handled);
        free(dup);
        if (!handled) { all_builtin = 0; break; }
    }
    free(copy);
    if (all_builtin) return rc;
    return run_command(cmd);
}
API int _wsystem(const WCHAR *cmd) { if (!cmd) return 1; char *u = lsiw_w2u(cmd); int r = ms_system(u); free(u); return r; }

/* ---- _spawn* / _exec* ---- */

#define P_WAIT    0
#define P_NOWAIT  1
#define P_OVERLAY 2
#define P_NOWAITO 3
#define P_DETACH  4

static char *quote_arg(const char *a)
{
    if (*a && !strpbrk(a, " \t\"")) return strdup(a);
    size_t l = strlen(a);
    char *o = malloc(l * 2 + 3), *q = o;
    *q++ = '"';
    for (const char *p = a; *p; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < bs * 2; i++) *q++ = '\\'; break; }
        if (*p == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) *q++ = '\\'; *q++ = '"'; }
        else { for (size_t i = 0; i < bs; i++) *q++ = '\\'; *q++ = *p; }
    }
    *q++ = '"';
    *q = 0;
    return o;
}

static intptr_t spawn(int mode, const char *path, const char *const *argv, const char *const *envp, int search)
{
    (void)search;
    size_t cap = 256, len = 0;
    char *line = malloc(cap);
    line[0] = 0;
    for (int i = 0; argv && argv[i]; i++) {
        char *q = i == 0 ? strdup(argv[i]) : quote_arg(argv[i]);
        if (i == 0 && strpbrk(q, " \t")) { char *t = lsiw_strdup_printf("\"%s\"", q); free(q); q = t; }
        size_t ql = strlen(q);
        if (len + ql + 2 > cap) { cap = (len + ql + 2) * 2; line = realloc(line, cap); }
        if (len) line[len++] = ' ';
        memcpy(line + len, q, ql + 1);
        len += ql;
        free(q);
    }
    char *envblock = NULL;
    if (envp) {
        size_t el = 1;
        for (int i = 0; envp[i]; i++) el += strlen(envp[i]) + 1;
        envblock = malloc(el);
        char *e = envblock;
        for (int i = 0; envp[i]; i++) { size_t l = strlen(envp[i]) + 1; memcpy(e, envp[i], l); e += l; }
        *e = 0;
    }
    BOOL (WINAPI *cp)(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, void *, void *) = k32("CreateProcessA");
    DWORD (WINAPI *wait)(HANDLE, DWORD) = k32("WaitForSingleObject");
    BOOL (WINAPI *gec)(HANDLE, DWORD *) = k32("GetExitCodeProcess");
    BOOL (WINAPI *ch)(HANDLE) = k32("CloseHandle");
    DWORD (WINAPI *sp)(const char *, const char *, const char *, DWORD, char *, char **) = k32("SearchPathA");
    char found[4096];
    const char *app = path;
    if (!strpbrk(path, "\\/:") && sp(NULL, path, ".exe", sizeof found, found, NULL)) app = found;
    struct { DWORD cb; char pad[100]; } si = { 104, { 0 } };
    struct { HANDLE hp, ht; DWORD pid, tid; } pi;
    lsiw_crt_flush_all();
    BOOL ok = cp(app, line, NULL, NULL, TRUE, 0, envblock, NULL, &si, &pi);
    free(line); free(envblock);
    if (!ok) { crt_set_errno(MS_ENOENT); return -1; }
    ch(pi.ht);
    if (mode == P_WAIT || mode == P_OVERLAY) {
        wait(pi.hp, INFINITE);
        DWORD code = 0;
        gec(pi.hp, &code);
        ch(pi.hp);
        if (mode == P_OVERLAY) ms_exit((int)code);
        return (intptr_t)code;
    }
    if (mode == P_DETACH) { ch(pi.hp); return 0; }
    return (intptr_t)pi.hp;
}

static const char **collect_list(const char *a0, ms_va_list ap, const char *const **envp_out)
{
    size_t n = 1, cap = 16;
    const char **v = malloc(cap * sizeof *v);
    v[0] = a0;
    if (a0) {
        for (;;) {
            const char *a = ms_va_arg(ap, const char *);
            if (n + 1 >= cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
            v[n++] = a;
            if (!a) break;
        }
    } else v[n++] = NULL;
    if (envp_out) *envp_out = ms_va_arg(ap, const char *const *);
    return v;
}

API intptr_t _spawnv(int m, const char *p, const char *const *a) { return spawn(m, p, a, NULL, 0); }
API intptr_t _spawnve(int m, const char *p, const char *const *a, const char *const *e) { return spawn(m, p, a, e, 0); }
API intptr_t _spawnvp(int m, const char *p, const char *const *a) { return spawn(m, p, a, NULL, 1); }
API intptr_t _spawnvpe(int m, const char *p, const char *const *a, const char *const *e) { return spawn(m, p, a, e, 1); }
API intptr_t _spawnl(int m, const char *p, const char *a0, ...)
{ ms_va_list ap; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, NULL); ms_va_end(ap); intptr_t r = spawn(m, p, v, NULL, 0); free(v); return r; }
API intptr_t _spawnlp(int m, const char *p, const char *a0, ...)
{ ms_va_list ap; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, NULL); ms_va_end(ap); intptr_t r = spawn(m, p, v, NULL, 1); free(v); return r; }
API intptr_t _spawnle(int m, const char *p, const char *a0, ...)
{ ms_va_list ap; const char *const *e; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, &e); ms_va_end(ap); intptr_t r = spawn(m, p, v, e, 0); free(v); return r; }
API intptr_t _execv(const char *p, const char *const *a) { return spawn(P_OVERLAY, p, a, NULL, 0); }
API intptr_t _execve(const char *p, const char *const *a, const char *const *e) { return spawn(P_OVERLAY, p, a, e, 0); }
API intptr_t _execvp(const char *p, const char *const *a) { return spawn(P_OVERLAY, p, a, NULL, 1); }
API intptr_t _execvpe(const char *p, const char *const *a, const char *const *e) { return spawn(P_OVERLAY, p, a, e, 1); }
API intptr_t _execl(const char *p, const char *a0, ...)
{ ms_va_list ap; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, NULL); ms_va_end(ap); intptr_t r = spawn(P_OVERLAY, p, v, NULL, 0); free(v); return r; }
API intptr_t _execlp(const char *p, const char *a0, ...)
{ ms_va_list ap; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, NULL); ms_va_end(ap); intptr_t r = spawn(P_OVERLAY, p, v, NULL, 1); free(v); return r; }

static char **wide_list_to_utf8(const WCHAR *const *w)
{
    size_t n = 0;
    while (w && w[n]) n++;
    char **v = malloc((n + 1) * sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = lsiw_w2u(w[i]);
    v[n] = NULL;
    return v;
}
static void free_list(char **v) { if (!v) return; for (size_t i = 0; v[i]; i++) free(v[i]); free(v); }
API intptr_t _wspawnv(int m, const WCHAR *p, const WCHAR *const *a)
{ char *pp = lsiw_w2u(p); char **v = wide_list_to_utf8(a); intptr_t r = spawn(m, pp, (const char *const *)v, NULL, 0); free(pp); free_list(v); return r; }
API intptr_t _wspawnvp(int m, const WCHAR *p, const WCHAR *const *a)
{ char *pp = lsiw_w2u(p); char **v = wide_list_to_utf8(a); intptr_t r = spawn(m, pp, (const char *const *)v, NULL, 1); free(pp); free_list(v); return r; }
API intptr_t _wspawnve(int m, const WCHAR *p, const WCHAR *const *a, const WCHAR *const *e)
{ char *pp = lsiw_w2u(p); char **v = wide_list_to_utf8(a), **ev = e ? wide_list_to_utf8(e) : NULL; intptr_t r = spawn(m, pp, (const char *const *)v, (const char *const *)ev, 0); free(pp); free_list(v); free_list(ev); return r; }
API intptr_t _wexecv(const WCHAR *p, const WCHAR *const *a) { return _wspawnv(P_OVERLAY, p, a); }
API intptr_t _wexecvp(const WCHAR *p, const WCHAR *const *a) { return _wspawnvp(P_OVERLAY, p, a); }
API intptr_t _wspawnvpe(int m, const WCHAR *p, const WCHAR *const *a, const WCHAR *const *e)
{ char *pp = lsiw_w2u(p); char **v = wide_list_to_utf8(a), **ev = e ? wide_list_to_utf8(e) : NULL; intptr_t r = spawn(m, pp, (const char *const *)v, (const char *const *)ev, 1); free(pp); free_list(v); free_list(ev); return r; }
API intptr_t _wexecve(const WCHAR *p, const WCHAR *const *a, const WCHAR *const *e) { return _wspawnve(P_OVERLAY, p, a, e); }
API intptr_t _wexecvpe(const WCHAR *p, const WCHAR *const *a, const WCHAR *const *e) { return _wspawnvpe(P_OVERLAY, p, a, e); }
API intptr_t _spawnlpe(int m, const char *p, const char *a0, ...)
{ ms_va_list ap; const char *const *e; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, &e); ms_va_end(ap); intptr_t r = spawn(m, p, v, e, 1); free(v); return r; }
API intptr_t _execle(const char *p, const char *a0, ...)
{ ms_va_list ap; const char *const *e; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, &e); ms_va_end(ap); intptr_t r = spawn(P_OVERLAY, p, v, e, 0); free(v); return r; }
API intptr_t _execlpe(const char *p, const char *a0, ...)
{ ms_va_list ap; const char *const *e; ms_va_start(ap, a0); const char **v = collect_list(a0, ap, &e); ms_va_end(ap); intptr_t r = spawn(P_OVERLAY, p, v, e, 1); free(v); return r; }

/* The wide list forms: arguments up to a NULL, then (for the "e" forms)
 * the environment array. */
static const WCHAR **collect_wlist(const WCHAR *a0, ms_va_list ap, const WCHAR *const **envp_out)
{
    size_t n = 1, cap = 16;
    const WCHAR **v = malloc(cap * sizeof *v);
    v[0] = a0;
    if (a0) {
        for (;;) {
            const WCHAR *a = ms_va_arg(ap, const WCHAR *);
            if (n + 1 >= cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
            v[n++] = a;
            if (!a) break;
        }
    } else v[n++] = NULL;
    if (envp_out) *envp_out = ms_va_arg(ap, const WCHAR *const *);
    return v;
}
#define WIDE_LIST_SPAWN(name, mode_expr, has_env, search) \
API intptr_t name(WIDE_LIST_PARAMS_##mode_expr const WCHAR *p, const WCHAR *a0, ...) \
{ \
    ms_va_list ap; const WCHAR *const *e = NULL; \
    ms_va_start(ap, a0); const WCHAR **v = collect_wlist(a0, ap, has_env ? &e : NULL); ms_va_end(ap); \
    char *pp = lsiw_w2u(p); char **uv = wide_list_to_utf8(v), **ue = e ? wide_list_to_utf8(e) : NULL; \
    intptr_t r = spawn(WIDE_LIST_MODE_##mode_expr, pp, (const char *const *)uv, (const char *const *)ue, search); \
    free(pp); free_list(uv); free_list(ue); free(v); return r; \
}
#define WIDE_LIST_PARAMS_spawn int m,
#define WIDE_LIST_PARAMS_exec
#define WIDE_LIST_MODE_spawn m
#define WIDE_LIST_MODE_exec P_OVERLAY
WIDE_LIST_SPAWN(_wspawnl, spawn, 0, 0)
WIDE_LIST_SPAWN(_wspawnlp, spawn, 0, 1)
WIDE_LIST_SPAWN(_wspawnle, spawn, 1, 0)
WIDE_LIST_SPAWN(_wspawnlpe, spawn, 1, 1)
WIDE_LIST_SPAWN(_wexecl, exec, 0, 0)
WIDE_LIST_SPAWN(_wexeclp, exec, 0, 1)
WIDE_LIST_SPAWN(_wexecle, exec, 1, 0)
WIDE_LIST_SPAWN(_wexeclpe, exec, 1, 1)

/* FLT_ROUNDS for the current rounding mode: 0 toward zero, 1 to nearest,
 * 2 upward, 3 downward (the x87/SSE RC field order differs). */
API int __fpe_flt_rounds(void)
{
    switch (fegetround()) {
    case FE_TOWARDZERO: return 0;
    case FE_UPWARD:     return 2;
    case FE_DOWNWARD:   return 3;
    default:            return 1;
    }
}

API intptr_t _cwait(int *status, intptr_t h, int action)
{
    (void)action;
    DWORD (WINAPI *wait)(HANDLE, DWORD) = k32("WaitForSingleObject");
    BOOL (WINAPI *gec)(HANDLE, DWORD *) = k32("GetExitCodeProcess");
    if (wait((HANDLE)h, INFINITE) != 0) { crt_set_errno(MS_ECHILD); return -1; }
    DWORD code = 0;
    gec((HANDLE)h, &code);
    if (status) *status = (int)code;
    return h;
}

/* ================================================================== */
/* setjmp / longjmp (Microsoft x64 _JUMP_BUFFER layout)                 */
/* ================================================================== */

__asm__(
    ".text\n"
    ".globl lsiw__setjmp\n"
    "lsiw__setjmp:\n"
    "  movq %rdx, 0(%rcx)\n"          /* Frame */
    "  movq %rbx, 8(%rcx)\n"
    "  leaq 8(%rsp), %rax\n"
    "  movq %rax, 16(%rcx)\n"         /* Rsp after return */
    "  movq %rbp, 24(%rcx)\n"
    "  movq %rsi, 32(%rcx)\n"
    "  movq %rdi, 40(%rcx)\n"
    "  movq %r12, 48(%rcx)\n"
    "  movq %r13, 56(%rcx)\n"
    "  movq %r14, 64(%rcx)\n"
    "  movq %r15, 72(%rcx)\n"
    "  movq (%rsp), %rax\n"
    "  movq %rax, 80(%rcx)\n"         /* Rip */
    "  stmxcsr 88(%rcx)\n"
    "  fnstcw 92(%rcx)\n"
    "  movdqu %xmm6, 96(%rcx)\n"
    "  movdqu %xmm7, 112(%rcx)\n"
    "  movdqu %xmm8, 128(%rcx)\n"
    "  movdqu %xmm9, 144(%rcx)\n"
    "  movdqu %xmm10, 160(%rcx)\n"
    "  movdqu %xmm11, 176(%rcx)\n"
    "  movdqu %xmm12, 192(%rcx)\n"
    "  movdqu %xmm13, 208(%rcx)\n"
    "  movdqu %xmm14, 224(%rcx)\n"
    "  movdqu %xmm15, 240(%rcx)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"

    ".globl lsiw_longjmp\n"
    "lsiw_longjmp:\n"
    "  movl %edx, %eax\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  movl $1, %eax\n"
    "1:\n"
    "  movq 8(%rcx), %rbx\n"
    "  movq 24(%rcx), %rbp\n"
    "  movq 32(%rcx), %rsi\n"
    "  movq 40(%rcx), %rdi\n"
    "  movq 48(%rcx), %r12\n"
    "  movq 56(%rcx), %r13\n"
    "  movq 64(%rcx), %r14\n"
    "  movq 72(%rcx), %r15\n"
    "  ldmxcsr 88(%rcx)\n"
    "  fldcw 92(%rcx)\n"
    "  movdqu 96(%rcx), %xmm6\n"
    "  movdqu 112(%rcx), %xmm7\n"
    "  movdqu 128(%rcx), %xmm8\n"
    "  movdqu 144(%rcx), %xmm9\n"
    "  movdqu 160(%rcx), %xmm10\n"
    "  movdqu 176(%rcx), %xmm11\n"
    "  movdqu 192(%rcx), %xmm12\n"
    "  movdqu 208(%rcx), %xmm13\n"
    "  movdqu 224(%rcx), %xmm14\n"
    "  movdqu 240(%rcx), %xmm15\n"
    "  movq 16(%rcx), %rsp\n"
    "  jmpq *80(%rcx)\n"
);
extern int WINAPI lsiw__setjmp(void *buf, void *frame);
extern void WINAPI lsiw_longjmp(void *buf, int v);

/* ================================================================== */
/* Locks (_lock / _unlock), misc                                        */
/* ================================================================== */

static pthread_mutex_t g_locks[64];
static pthread_once_t g_locks_once = PTHREAD_ONCE_INIT;
static void init_locks(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < 64; i++) pthread_mutex_init(&g_locks[i], &a);
}
API void _lock(int n) { pthread_once(&g_locks_once, init_locks); pthread_mutex_lock(&g_locks[n & 63]); }
API void _unlock(int n) { pthread_once(&g_locks_once, init_locks); pthread_mutex_unlock(&g_locks[n & 63]); }
API void _lock_locales(void) {}
API void _unlock_locales(void) {}
API void __acrt_lock(int n) { _lock(n); }
API void __acrt_unlock(int n) { _unlock(n); }

/* The loader calls this once the main module is known, so _pgmptr and
 * _wpgmptr hold the absolute image path even when argv[0] is relative. */
void lsiw_crt_set_program(const char *win_path)
{
    if (!win_path || !*win_path) return;
    init_args();
    crt_pgmptr = strdup(win_path);
    crt_wpgmptr = lsiw_u2w(crt_pgmptr);
}

/* Called by main.c before entering the program. */
void lsiw_crt_init(void)
{
    init_args();
    init_env();
    pthread_once(&g_locks_once, init_locks);
}

/* Microsoft version variables (data exports) */
unsigned int crt_osver = 19045, crt_winver = 0x0a00, crt_winmajor = 10, crt_winminor = 0;
API unsigned *__p__osver(void) { return &crt_osver; }
API unsigned *__p__winver(void) { return &crt_winver; }
API unsigned *__p__winmajor(void) { return &crt_winmajor; }
API unsigned *__p__winminor(void) { return &crt_winminor; }
API int _get_osver(unsigned *v) { *v = crt_osver; return 0; }
API int _get_winver(unsigned *v) { *v = crt_winver; return 0; }
API int _get_winmajor(unsigned *v) { *v = crt_winmajor; return 0; }
API int _get_winminor(unsigned *v) { *v = crt_winminor; return 0; }


const struct lsiw_export lsiw_crt_core_exports[] = {
    E(_errno), E(__doserrno), E(_get_errno), E(_set_errno), E(_get_doserrno), E(_set_doserrno),
    E(_set_invalid_parameter_handler), E(_get_invalid_parameter_handler),
    E(_set_thread_local_invalid_parameter_handler), E(_get_thread_local_invalid_parameter_handler),
    E(_invalid_parameter_noinfo), E(_invalid_parameter_noinfo_noreturn), E(_invalid_parameter), E(_invoke_watson),
    E(_amsg_exit), E(_purecall), E(_set_purecall_handler), E(_get_purecall_handler), E(__crtTerminateProcess), E(__fastfail),
    EM(getenv), E(_wgetenv), E(getenv_s), E(_wgetenv_s), E(_dupenv_s), E(_wdupenv_s), E(_putenv), E(_wputenv), E(_putenv_s), E(_wputenv_s),
    E(__set_app_type), E(_set_app_type), E(__p__fmode), E(__p__commode), E(_set_fmode), E(_get_fmode),
    E(__getmainargs), E(__wgetmainargs), E(__p___argc), E(__p___argv), E(__p___wargv), E(__p__acmdln), E(__p__wcmdln),
    E(__p__environ), E(__p__wenviron), E(__p___initenv), E(__p___winitenv), E(__p__pgmptr), E(__p__wpgmptr),
    E(_get_pgmptr), E(_get_wpgmptr),
    E(_configure_narrow_argv), E(_configure_wide_argv), E(_initialize_narrow_environment), E(_initialize_wide_environment),
    E(_get_initial_narrow_environment), E(_get_initial_wide_environment),
    E(_get_narrow_winmain_command_line), E(_get_wide_winmain_command_line),
    E(_initterm), E(_initterm_e), E(_seh_filter_exe), E(_seh_filter_dll), E(_XcptFilter), E(__setusermatherr), E(_matherr),
    E(_set_new_mode), E(_query_new_mode), E(_set_new_handler), E(_query_new_handler), E(_callnewh), E(_configthreadlocale),
    E(__security_init_cookie), E(__crt_debugger_hook), E(_CrtDbgBreak), E(_CrtDbgReport), E(_CrtSetReportMode),
    E(_CrtSetDbgFlag), E(_CrtSetReportHook), E(_set_error_mode), E(_set_abort_behavior), E(_assert), E(_wassert),
    EM(atexit), E(_onexit), E(_crt_atexit), E(_crt_at_quick_exit), EM(at_quick_exit), E(__dllonexit),
    E(_initialize_onexit_table), E(_register_onexit_function), E(_execute_onexit_table), E(_register_thread_local_exe_atexit_callback),
    EM(exit), EM(_exit), EM(_Exit), EM(quick_exit), E(_cexit), E(_c_exit),
    EM(signal), EM(raise), EM(abort), E(__pxcptinfoptrs), E(__fpecode),
    EM(malloc), EM(calloc), EM(free), EM(realloc), E(_msize), E(_expand), E(_recalloc),
    E(_free_base), E(_malloc_base), E(_calloc_base), E(_realloc_base), E(_msize_base), E(_malloc_crt), E(_calloc_crt),
    E(_aligned_offset_malloc), E(_aligned_malloc), E(_aligned_free), E(_aligned_msize), E(_aligned_offset_realloc),
    E(_aligned_realloc), E(_aligned_recalloc), E(_heapchk), E(_heapmin), E(_heapset), E(_heapwalk), E(_get_heap_handle),
    E(_get_sbh_threshold), E(_set_sbh_threshold),
    EM(qsort), E(qsort_s), EM(bsearch), E(bsearch_s), E(_lfind), E(_lsearch),
    EM(srand), EM(rand), E(rand_s), EM(abs), EM(labs), EM(llabs), E(_abs64), EM(imaxabs), EM(div), EM(ldiv), EM(lldiv), EM(imaxdiv),
    E(_rotl), E(_rotr), E(_rotl64), E(_rotr64), E(_lrotl), E(_lrotr), E(_byteswap_ushort), E(_byteswap_ulong), E(_byteswap_uint64), E(_swab),
    E(_getpid), E(_sleep), E(__threadid), E(__threadhandle), E(_beginthreadex), E(_beginthread), E(_endthreadex), E(_endthread),
    EM(system), E(_wsystem),
    E(_spawnv), E(_spawnve), E(_spawnvp), E(_spawnvpe), E(_spawnl), E(_spawnlp), E(_spawnle),
    E(_execv), E(_execve), E(_execvp), E(_execvpe), E(_execl), E(_execlp),
    E(_wspawnv), E(_wspawnvp), E(_wspawnve), E(_wexecv), E(_wexecvp), E(_cwait),
    E(_wspawnvpe), E(_wexecve), E(_wexecvpe), E(_spawnlpe), E(_execle), E(_execlpe),
    E(_wspawnl), E(_wspawnlp), E(_wspawnle), E(_wspawnlpe), E(_wexecl), E(_wexeclp), E(_wexecle), E(_wexeclpe),
    E(__fpe_flt_rounds),
    EX("_setjmp", lsiw__setjmp), EX("_setjmpex", lsiw__setjmp), EX("setjmp", lsiw__setjmp),
    EX("__intrinsic_setjmp", lsiw__setjmp), EX("__intrinsic_setjmpex", lsiw__setjmp), EX("longjmp", lsiw_longjmp),
    E(_lock), E(_unlock), E(_lock_locales), E(_unlock_locales), E(__acrt_lock), E(__acrt_unlock),
    E(__p__osver), E(__p__winver), E(__p__winmajor), E(__p__winminor), E(_get_osver), E(_get_winver), E(_get_winmajor), E(_get_winminor),
    /* data */
    EX("_environ", &crt_environ), EX("_wenviron", &crt_wenviron), EX("__initenv", &crt_initenv), EX("__winitenv", &crt_winitenv),
    EX("__argc", &crt_argc), EX("__argv", &crt_argv), EX("__wargv", &crt_wargv), EX("_acmdln", &crt_acmdln), EX("_wcmdln", &crt_wcmdln),
    EX("_pgmptr", &crt_pgmptr), EX("_wpgmptr", &crt_wpgmptr), EX("_fmode", &crt_fmode), EX("_commode", &crt_commode),
    EX("_osver", &crt_osver), EX("_winver", &crt_winver), EX("_winmajor", &crt_winmajor), EX("_winminor", &crt_winminor),
    { NULL, NULL }
};
