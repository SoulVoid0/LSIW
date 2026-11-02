/* Internal definitions for LSIW's native engine: the PE loader and the
 * Win32 API implementations it links Windows programs against.
 *
 * Every function a Windows program can call is compiled with the Microsoft
 * x64 calling convention (WINAPI below), so PE code calls straight into it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_NATIVE_H
#define LSIW_NATIVE_H

#include "lsiw.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#if !defined(__x86_64__)
#error "The LSIW native engine targets x86-64 hosts."
#endif

#define WINAPI __attribute__((ms_abi))
typedef __builtin_ms_va_list ms_va_list;
#define ms_va_start __builtin_ms_va_start
#define ms_va_end   __builtin_ms_va_end
#define ms_va_arg   __builtin_va_arg

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int64_t  LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t SIZE_T;
typedef int64_t  SSIZE_T;
typedef uint64_t ULONG_PTR;
typedef int      BOOL;
typedef uint16_t WCHAR;
typedef void    *HANDLE;
typedef void    *HMODULE;
typedef void    *LPVOID;
typedef int32_t  NTSTATUS;
typedef uint32_t UINT;

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260

/* Error codes (winerror.h subset). */
#define ERROR_SUCCESS               0
#define ERROR_INVALID_FUNCTION      1
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_PATH_NOT_FOUND        3
#define ERROR_TOO_MANY_OPEN_FILES   4
#define ERROR_ACCESS_DENIED         5
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_INVALID_DATA          13
#define ERROR_OUTOFMEMORY           14
#define ERROR_INVALID_DRIVE         15
#define ERROR_NOT_SAME_DEVICE       17
#define ERROR_NO_MORE_FILES         18
#define ERROR_WRITE_PROTECT         19
#define ERROR_SHARING_VIOLATION     32
#define ERROR_HANDLE_EOF            38
#define ERROR_NOT_SUPPORTED         50
#define ERROR_FILE_EXISTS           80
#define ERROR_INVALID_PARAMETER     87
#define ERROR_BROKEN_PIPE           109
#define ERROR_DISK_FULL             112
#define ERROR_CALL_NOT_IMPLEMENTED  120
#define ERROR_INSUFFICIENT_BUFFER   122
#define ERROR_INVALID_NAME          123
#define ERROR_MOD_NOT_FOUND         126
#define ERROR_PROC_NOT_FOUND        127
#define ERROR_NEGATIVE_SEEK         131
#define ERROR_DIR_NOT_EMPTY         145
#define ERROR_BAD_PATHNAME          161
#define ERROR_ALREADY_EXISTS        183
#define ERROR_ENVVAR_NOT_FOUND      203
#define ERROR_FILENAME_EXCED_RANGE  206
#define ERROR_MORE_DATA             234
#define ERROR_NO_MORE_ITEMS         259
#define ERROR_DIRECTORY             267
#define ERROR_NOT_OWNER             288
#define ERROR_TIMEOUT               1460
#define WAIT_TIMEOUT                258

#define WAIT_OBJECT_0   0
#define WAIT_ABANDONED  0x80
#define WAIT_FAILED     0xFFFFFFFFu
#define INFINITE        0xFFFFFFFFu

/* ---- TEB / threads (teb.c) ---- */
#define TEB_SIZE        0x3000
#define TEB_TLS_SLOTS   0x1480   /* TlsSlots[64]               */
#define TEB_TLS_EXP     0x1780   /* TlsExpansionSlots pointer  */
#define TEB_PRIV        0x2000   /* LSIW private per-thread data */
#define TLS_MINIMUM_AVAILABLE 64
#define TLS_EXPANSION_SLOTS   1024
#define FLS_MAXIMUM_AVAILABLE 4080

struct lsiw_thread_priv {
    void     *fls[128];            /* fiber-local storage values           */
    uint32_t  tid;
    int       is_main;
    void     *jmp;                 /* jmp_buf* used by ExitThread          */
    struct lsiw_handle_obj *self;  /* thread object, NULL for main thread  */
    int       crt_errno;           /* msvcrt per-thread errno              */
    unsigned long doserrno;
    char     *strtok_ctx;
    WCHAR    *wcstok_ctx;
    int       priority;            /* SetThreadPriority value              */
};

void   *lsiw_teb_create(void);                /* allocate + install on this thread */
void   *lsiw_teb_current(void);
struct lsiw_thread_priv *lsiw_thread(void);
uint32_t lsiw_get_last_error(void);
void     lsiw_set_last_error(uint32_t);
void     lsiw_set_errno_error(int err);       /* errno -> Win32 last error */
uint32_t lsiw_errno_to_win(int err);
void    *lsiw_peb(void);
struct lsiw_module;
int      lsiw_kuser_init(void);                         /* KUSER_SHARED_DATA at 0x7FFE0000 (kuser.c) */
void     lsiw_peb_init_process(struct lsiw_module *exe);   /* ProcessParameters + Ldr */
void     lsiw_peb_add_module(struct lsiw_module *m);       /* append to PEB.Ldr lists */
void     lsiw_teb_free_thread_data(void);     /* run FLS callbacks */

/* ---- modules (loader.c) ---- */
struct lsiw_module {
    char      name[260];          /* lower-case file name ("foo.dll")      */
    char     *linux_path;         /* where it was loaded from              */
    char     *win_path;           /* C:\... path                           */
    uint8_t  *base;
    size_t    size;
    void     *entry;              /* entry point or DllMain                */
    int       is_exe;
    int       is_builtin;         /* implemented by LSIW itself            */
    const struct lsiw_builtin_dll *builtin;
    /* implicit TLS (.tls section) */
    int       tls_index;          /* -1 if none                            */
    void     *tls_template;
    size_t    tls_template_size;
    size_t    tls_zero_fill;
    uint64_t *tls_callbacks;
    struct lsiw_module *next;
};

struct lsiw_export { const char *name; void *addr; };
#define LSIW_MAX_TABLES 16
struct lsiw_builtin_dll { const char *name; const struct lsiw_export *tables[LSIW_MAX_TABLES]; };

extern struct lsiw_module *lsiw_modules;
extern struct lsiw_module *lsiw_main_module;

/* Load an executable or DLL from a Linux path. On failure returns NULL and,
 * if `why` is non-NULL, describes the reason. */
struct lsiw_module *lsiw_load_pe(const char *linux_path, int is_exe, char **why);
struct lsiw_module *lsiw_load_library(const char *name, const char *search_from);
struct lsiw_module *lsiw_module_from_handle(HMODULE h);
struct lsiw_module *lsiw_module_from_address(const void *addr);
struct lsiw_module *lsiw_find_module(const char *name);
void  *lsiw_get_export(struct lsiw_module *m, const char *name, int ordinal);
int    lsiw_run_exe(struct lsiw_module *exe);
void   lsiw_process_detach(void);
/* Scan a PE file without running it: returns 0 if every import can be
 * satisfied natively, otherwise 1 and a description of what is missing. */
int    lsiw_native_check(const char *linux_path, char **report);
/* Per-thread TLS setup for every module that has a .tls section. */
void **lsiw_tls_build_array(void);
void   lsiw_call_tls_callbacks(uint32_t reason);
void   lsiw_dll_notify_threads(uint32_t reason);

/* ---- builtin DLL registry (builtins.c) ---- */
const struct lsiw_builtin_dll *lsiw_builtin_find(const char *dllname);
void *lsiw_builtin_lookup(const struct lsiw_builtin_dll *dll, const char *sym);
HMODULE lsiw_builtin_handle(const struct lsiw_builtin_dll *dll);
const struct lsiw_builtin_dll *lsiw_builtin_from_handle(HMODULE h);

extern const struct lsiw_export lsiw_k32proc_exports[];
extern const struct lsiw_export lsiw_k32file_exports[];
extern const struct lsiw_export lsiw_k32mem_exports[];
extern const struct lsiw_export lsiw_k32extra_exports[];  /* thread pool, IOCP, named pipes, INI files, atoms... */
extern const struct lsiw_export lsiw_k32sys_exports[];    /* Toolhelp32, job objects, attribute lists, time zones */
extern const struct lsiw_export lsiw_k32compat_exports[]; /* _lopen family, LZ, Pss, geo info, CopyFile2 */
extern const struct lsiw_export lsiw_ntdll_exports[];
extern const struct lsiw_export lsiw_ntfile_exports[];   /* ntdll_file.c: NT file information, directories, volumes, FSCTLs */
extern const struct lsiw_export lsiw_ntobj_exports[];    /* ntdll_obj.c: objects, tokens, security, registry, strings */
extern const struct lsiw_export lsiw_k32more_exports[];  /* ntdll_obj.c: resources, serial, tape, debug, IDN, crypt providers, GetBestRoute2 */
extern const struct lsiw_export lsiw_msvcrt_exports[];  /* msvcrt.dll-only names and data */
extern const struct lsiw_export lsiw_ucrt_exports[];    /* ucrtbase-only names            */
extern const struct lsiw_export lsiw_crt_core_exports[];   /* startup, exit, errno, memory, env, process */
extern const struct lsiw_export lsiw_crt_string_exports[]; /* string.h, wchar.h, ctype.h, stdlib conversions */
extern const struct lsiw_export lsiw_crt_stdio_exports[];  /* stdio.h, io.h, direct.h, sys/stat.h */
extern const struct lsiw_export lsiw_crt_math_exports[];   /* math.h, float.h, fenv.h */
extern const struct lsiw_export lsiw_crt_time_exports[];   /* time.h, locale.h */
extern const struct lsiw_export lsiw_vcruntime_exports[];
extern const struct lsiw_export lsiw_user32_exports[];
extern const struct lsiw_export lsiw_shell32_exports[];
extern const struct lsiw_export lsiw_advapi32_exports[];
extern const struct lsiw_export lsiw_misc_exports[];   /* winmm, version, userenv, secur32, psapi, dbghelp, netapi32... */
extern const struct lsiw_export lsiw_ole_exports[];    /* ole32, oleaut32, combase, rpcrt4, crypt32, bcrypt */
extern const struct lsiw_export lsiw_ws2_exports[];    /* ws2_32, wsock32, iphlpapi */
extern const struct lsiw_export lsiw_wsock32_exports[];   /* wsock32-only: Winsock 1.1 option numbers, extension ordinals */

/* ---- handles (handles.c) ---- */
enum lsiw_htype {
    H_FREE, H_FILE, H_FIND, H_THREAD, H_PROCESS, H_EVENT, H_MUTEX,
    H_SEMAPHORE, H_MAPPING, H_TIMER,
    H_SNAPSHOT,                  /* Toolhelp32 snapshot                     */
    H_JOB,                       /* job object                              */
    H_GENERIC                    /* module-private object: see data below   */
};

struct lsiw_handle_obj {
    int      type;
    int      refs;
    int      fd;                 /* H_FILE, H_MAPPING                       */
    char    *path;               /* Linux path, if any                      */
    int      is_console;
    int      append;
    /* waitable state */
    int      signaled;
    int      manual_reset;
    uint32_t owner_tid;          /* mutex owner                             */
    int      recursion;
    long     count, max_count;   /* semaphore                               */
    uint32_t exit_code;
    int      pid;                /* H_PROCESS                               */
    int      reaped;
    int      term_set;           /* TerminateProcess code pending (term_code) */
    uint32_t term_code;
    int      priority;           /* H_THREAD: SetThreadPriority value        */
    /* threads */
    uint32_t tid;
    int      suspended;
    void    *pthread;            /* pthread_t*                              */
    /* find */
    void    *dir;                /* DIR*                                    */
    char    *pattern;
    char    *dir_path;
    /* mapping */
    uint64_t map_size;
    uint32_t map_protect;
    /* module-private payload, released with free_data(data) on last close */
    void    *data;
    void   (*free_data)(void *data);
};

HANDLE lsiw_handle_new(int type, struct lsiw_handle_obj **out);
/* Report the full 32-bit exit code to a parent LSIW process (k32_process.c). */
void   lsiw_exit_report(uint32_t code);
struct lsiw_handle_obj *lsiw_handle_get(HANDLE h, int type);   /* type -1 = any */
int    lsiw_handle_close(HANDLE h);
HANDLE lsiw_handle_dup(HANDLE h);
HANDLE lsiw_std_handle(int which);           /* 0,1,2 */
void   lsiw_std_handle_set(int which, HANDLE h);
HANDLE lsiw_handle_from_fd(int fd, int console);
int    lsiw_fd_from_handle(HANDLE h);
/* Open a Linux path with CreateFile semantics (k32_file.c). Takes ownership
 * of `path` (malloc'd). Sets the last error; INVALID_HANDLE_VALUE on failure. */
HANDLE lsiw_create_file_linux(char *path, DWORD access, DWORD share, DWORD disp, DWORD flags);
/* errno -> NTSTATUS (ntdll.c) */
uint32_t lsiw_errno_to_ntstatus(int err);
void   lsiw_handles_init(void);
/* Global lock + condition used by every wait operation. */
void   lsiw_wait_lock(void);
void   lsiw_wait_unlock(void);
void   lsiw_wait_broadcast(void);            /* call with the lock held */
int    lsiw_wait_cond(const struct timespec *deadline); /* 0 or ETIMEDOUT */

/* ---- strings (strings.c) ---- */
size_t lsiw_wcslen(const WCHAR *s);
char  *lsiw_w2u(const WCHAR *w);               /* UTF-16 -> UTF-8, malloc   */
char  *lsiw_w2u_n(const WCHAR *w, int n);      /* n = -1 for NUL-terminated */
WCHAR *lsiw_u2w(const char *s);                /* UTF-8 -> UTF-16, malloc   */
int    lsiw_u2w_buf(const char *s, WCHAR *out, int outlen); /* returns chars written incl. NUL, or needed */
int    lsiw_utf8_to_utf16(const char *s, int slen, WCHAR *out, int outlen);
int    lsiw_utf16_to_utf8(const WCHAR *s, int slen, char *out, int outlen);
/* Copy a string into a caller buffer the Windows way: returns length
 * without NUL on success, or the needed size including NUL if too small. */
DWORD  lsiw_ret_str_a(const char *s, char *buf, DWORD size);
DWORD  lsiw_ret_str_w(const char *s, WCHAR *buf, DWORD size);

/* ---- process-wide state (kernel32.c) ---- */
void   lsiw_set_command_line(int argc, char **argv, const char *exe_win);
const char  *lsiw_cmdline_a(void);
const WCHAR *lsiw_cmdline_w(void);
void   lsiw_env_init(void);
const char *lsiw_env_get(const char *name);
int    lsiw_env_set(const char *name, const char *value);   /* value NULL = delete */
char **lsiw_env_block(void);                 /* NULL-terminated "NAME=value" */
void   lsiw_crt_init(void);
void   lsiw_crt_set_program(const char *win_path);
void  *lsiw_trace_wrap(const char *dll, const char *sym, void *fn);   /* LSIW_APITRACE (loader.c) */
void   lsiw_crt_flush_all(void);
void   lsiw_run_exit_handlers(void);
__attribute__((noreturn)) void lsiw_exit_process(uint32_t code);
int    lsiw_fault_init(void);

/* Path helpers usable by API code. Caller frees. */
char *lsiw_path_a(const char *win);            /* Windows ANSI path -> Linux */
char *lsiw_path_w(const WCHAR *win);           /* Windows wide path -> Linux */

/* printf/scanf engines over an MS-ABI va_list (printf.c) */
int lsiw_ms_vsnprintf(char *buf, size_t n, const char *fmt, ms_va_list ap, int wide_fmt);
int lsiw_ms_vsnwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, ms_va_list ap);
typedef int (*lsiw_getc_fn)(void *ctx);
typedef void (*lsiw_ungetc_fn)(int c, void *ctx);
int lsiw_ms_vscanf(lsiw_getc_fn get, lsiw_ungetc_fn unget, void *ctx, const char *fmt, ms_va_list ap);
#define LSIW_SCAN_SECURE 1   /* scanf_s: a buffer size follows each %c/%s/%[ pointer */
#define LSIW_SCAN_WIDE   2   /* wscanf: bare %c/%s/%[ store wide characters */
int lsiw_ms_vscanf_ex(lsiw_getc_fn get, lsiw_ungetc_fn unget, void *ctx, const char *fmt, ms_va_list ap, int flags);

/* Convenience for API implementations. */
#define API static WINAPI
#define E(n) { #n, (void *)n }
#define EX(name, fn) { name, (void *)fn }

/* Heap shared by HeapAlloc and the C runtime (k32_mem.c). */
void  *lsiw_process_heap(void);
void  *lsiw_heap_alloc(size_t n, int zero);
void   lsiw_heap_free(void *p);
void  *lsiw_heap_realloc(void *p, size_t n);
size_t lsiw_heap_size(const void *p);
int    lsiw_heap_owns(const void *p);
struct map_info { uintptr_t start, end; int prot; int mapped; int file; };
int    lsiw_query_maps(uintptr_t addr, struct map_info *out);

/* Exceptions (seh.c) */
int  lsiw_seh_init(void);
void lsiw_seh_thread_init(void);             /* per-thread signal stack */
void lsiw_raise(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);
extern const struct lsiw_export lsiw_seh_exports[];

/* Tracing of unimplemented functions. */
void lsiw_unimplemented(const char *what);

#endif
