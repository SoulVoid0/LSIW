/* kernel32 / kernelbase extras for the native engine.
 *
 * The long tail of kernel32 that language runtimes and frameworks (MSVC STL,
 * MinGW libstdc++/winpthreads, Rust std, Go, Python, Node/libuv, .NET)
 * import on top of the core file, process and memory APIs:
 *
 *   - the Vista thread pool (work, timers, waits, I/O, cleanup groups) and
 *     the older QueueUserWorkItem / RegisterWaitForSingleObject / timer
 *     queue APIs, all running callbacks on real Windows threads made by
 *     kernel32's own CreateThread;
 *   - I/O completion ports, user APCs, ReadFileEx / WriteFileEx;
 *   - named pipes (Unix domain sockets under $XDG_RUNTIME_DIR/lsiw-pipes),
 *     PeekNamedPipe on anonymous pipes, ReadDirectoryChangesW (inotify);
 *   - CPU topology, NUMA, processor groups, XState contexts, firmware
 *     (SMBIOS) tables, power status, process and thread information;
 *   - number / currency formatting and other NLS functions, atoms, INI
 *     files, activation contexts and the AppModel/WER/restart functions
 *     that must answer the way an unpackaged desktop program expects.
 *
 * Hooks other engine files may call (see the module report):
 *   lsiw_iocp_io_done()      - an overlapped read/write finished synchronously;
 *                              queues the completion packet / thread-pool I/O
 *                              callback for handles bound to a port.
 *   lsiw_named_pipe_open()   - CreateFile on \\.\pipe\name.
 *   lsiw_apc_run_pending() / lsiw_apc_pending()
 *                            - alertable waits (SleepEx, WaitFor...Ex) run
 *                              the calling thread's queued user APCs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include "seh.h"
#include "pe.h"
#include <cpuid.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ================================================================== */
/* Windows types, constants and the hooks this file provides            */
/* ================================================================== */

typedef uint8_t  BOOLEAN;
typedef uint16_t USHORT;
typedef int16_t  SHORT;
typedef uint8_t  UCHAR;
typedef int32_t  HRESULT;
typedef uint16_t LANGID;
typedef uint32_t LCID;
typedef uint16_t ATOM;
typedef uint64_t KAFFINITY;
typedef uint64_t DWORD64;
typedef uint32_t GEOID;

#define S_OK            ((HRESULT)0)
#define E_FAIL          ((HRESULT)0x80004005)
#define E_INVALIDARG    ((HRESULT)0x80070057)
#define HRESULT_FROM_WIN32(x) ((x) ? (HRESULT)(0x80070000u | ((DWORD)(x) & 0xFFFF)) : S_OK)

#define ERROR_BAD_LENGTH             24
#define ERROR_GEN_FAILURE            31
#define ERROR_BAD_NETPATH            53
#define ERROR_SEM_TIMEOUT            121
#define ERROR_BAD_EXE_FORMAT         193
#define ERROR_BAD_PIPE               230
#define ERROR_PIPE_BUSY              231
#define ERROR_NO_DATA                232
#define ERROR_PIPE_NOT_CONNECTED     233
#define ERROR_PIPE_CONNECTED         535
#define ERROR_PIPE_LISTENING         536
#define ERROR_ABANDONED_WAIT_0       735
#define ERROR_NOTIFY_CLEANUP         745
#define ERROR_OPERATION_ABORTED      995
#define ERROR_IO_PENDING             997
#define ERROR_NOACCESS               998
#define ERROR_INVALID_FLAGS          1004
#define ERROR_NOTIFY_ENUM_DIR        1022
#define ERROR_NOT_FOUND              1168
#define ERROR_PRIVILEGE_NOT_HELD     1314
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813
#define ERROR_SXS_KEY_NOT_FOUND      14007
#define APPMODEL_ERROR_NO_PACKAGE    15700
#define APPMODEL_ERROR_NO_APPLICATION 15703
#define WAIT_IO_COMPLETION           0xC0

#define STATUS_SUCCESS          0x00000000u
#define STATUS_PENDING          0x00000103u
#define STATUS_NOTIFY_CLEANUP   0x0000010Bu
#define STATUS_NOTIFY_ENUM_DIR  0x0000010Cu
#define STATUS_BUFFER_OVERFLOW  0x80000005u
#define STATUS_END_OF_FILE      0xC0000011u
#define STATUS_DELETE_PENDING   0xC0000056u
#define STATUS_PIPE_DISCONNECTED 0xC00000B0u
#define STATUS_CANCELLED        0xC0000120u
#define STATUS_PIPE_BROKEN      0xC000014Bu

typedef struct { DWORD lo, hi; } FILETIME;
typedef struct { ULONG_PTR Internal, InternalHigh; DWORD Offset, OffsetHigh; HANDLE hEvent; } OVERLAPPED;
typedef struct { ULONG_PTR lpCompletionKey; OVERLAPPED *lpOverlapped; ULONG_PTR Internal; DWORD dwNumberOfBytesTransferred; } OVERLAPPED_ENTRY;
typedef struct { SHORT X, Y; } COORD;
typedef struct { SHORT Left, Top, Right, Bottom; } SMALL_RECT;
_Static_assert(sizeof(OVERLAPPED) == 32, "OVERLAPPED");
_Static_assert(sizeof(OVERLAPPED_ENTRY) == 32, "OVERLAPPED_ENTRY");

typedef DWORD (WINAPI *THREAD_START_FN)(void *);
typedef void  (WINAPI *APC_FN)(ULONG_PTR);
typedef void  (WINAPI *IO_COMPLETION_FN)(DWORD err, DWORD bytes, OVERLAPPED *ov);

/* Hooks other engine files call. */
void lsiw_iocp_io_done(HANDLE file, void *overlapped, DWORD bytes, uint32_t ntstatus);
int  lsiw_named_pipe_open(const char *win, DWORD access, DWORD flags, HANDLE *out);
int  lsiw_apc_pending(void);
int  lsiw_apc_run_pending(void);

/* k32_process.c defines this (non-zero) once its alertable waits call
 * lsiw_apc_run_pending(). Until then ReadFileEx / WriteFileEx run their
 * completion routine right after the (synchronous) I/O, and asynchronous
 * completion routines run on a thread-pool thread, instead of waiting for
 * an alertable wait that would never deliver them. */
extern const int lsiw_k32_alertable_apcs __attribute__((weak));

static int alertable_waits_deliver_apcs(void)
{
    return &lsiw_k32_alertable_apcs != NULL && lsiw_k32_alertable_apcs;
}

/* ================================================================== */
/* Small helpers                                                        */
/* ================================================================== */

#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define PSEUDO_THREAD  ((HANDLE)(intptr_t)-2)

static void *xmalloc0(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) abort();
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) abort();
    return p;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Current time as a FILETIME value (100 ns units since 1601). */
static uint64_t ft_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
}

static uint64_t ft_from_timespec(struct timespec ts)
{
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
}

/* A thread-pool due time: negative = relative (100 ns), positive = absolute
 * FILETIME, zero = now. Returns a CLOCK_MONOTONIC deadline in ns. */
static uint64_t deadline_from_filetime(const FILETIME *ft)
{
    int64_t v = (int64_t)(((uint64_t)ft->hi << 32) | ft->lo);
    uint64_t now = mono_ns();
    if (v < 0) return now + (uint64_t)(-v) * 100;
    if (v == 0) return now;
    uint64_t cur = ft_now();
    return (uint64_t)v > cur ? now + ((uint64_t)v - cur) * 100 : now;
}

static void cond_init_mono(pthread_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}

/* Wait on a CLOCK_MONOTONIC condition variable until `deadline` (0 = none). */
static int cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, uint64_t deadline)
{
    if (!deadline) return pthread_cond_wait(c, m);
    struct timespec ts = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
    return pthread_cond_timedwait(c, m, &ts);
}

static size_t wlen(const WCHAR *s) { return s ? lsiw_wcslen(s) : 0; }

static WCHAR wupper(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 32);
    /* Latin-1 and the simple even/odd pairs of Latin Extended-A. */
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        int odd_lower = !((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E));
        if (odd_lower ? (c & 1) : !(c & 1)) return (WCHAR)(c - 1);
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (WCHAR)(c - 32);   /* Greek */
    if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 32);                  /* Cyrillic */
    if (c >= 0x450 && c <= 0x45F) return (WCHAR)(c - 80);
    return c;
}

static int wicmp(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = wupper(*a), y = wupper(*b);
        if (x != y || !x) return (int)x - (int)y;
    }
}

static int wnicmp(const WCHAR *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        WCHAR x = wupper(a[i]), y = wupper((WCHAR)(unsigned char)b[i]);
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
    return 0;
}

/* Read a small text file (sysfs, procfs) into buf; returns length or -1. */
static ssize_t read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return n;
}

static long read_long(const char *path, long def)
{
    char b[64];
    if (read_text(path, b, sizeof b) <= 0) return def;
    return strtol(b, NULL, 0);
}

/* Read a whole file into a malloc'd buffer (NUL-terminated for convenience). */
static uint8_t *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap + 1);
    for (;;) {
        if (n == cap) { cap *= 2; uint8_t *nb = realloc(buf, cap + 1); if (!nb) abort(); buf = nb; }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r < 0) { free(buf); buf = NULL; } break; }
        n += (size_t)r;
    }
    int e = errno;
    close(fd);
    errno = e;
    if (buf) { buf[n] = 0; *len = n; }
    return buf;
}

/* "Value from /proc/meminfo-style text": the number after `key`, in kB. */
static uint64_t meminfo_kb(const char *text, const char *key)
{
    const char *p = text ? strstr(text, key) : NULL;
    return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

/* Copy a UTF-8 string into a wide caller buffer: the "size in characters
 * including NUL" convention used by many NLS functions (0 = query). */
static int ret_w_count(const char *s, WCHAR *buf, int size)
{
    int need = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    if (size == 0) return need;
    if (!buf || size < need) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    lsiw_utf8_to_utf16(s, -1, buf, size);
    return need;
}

static int ret_wstr_count(const WCHAR *s, size_t n, WCHAR *buf, int size)
{
    int need = (int)n + 1;
    if (size == 0) return need;
    if (!buf || size < need) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, s, n * sizeof(WCHAR));
    buf[n] = 0;
    return need;
}

/* The ANSI side of the A functions goes through the process code page. */
static char *w2a(const WCHAR *w, int n);
static WCHAR *a2w(const char *s, int n);

/* ---- kernel32 entry points this file calls back into ---- */
typedef HANDLE (WINAPI *fn_CreateThread)(void *, SIZE_T, THREAD_START_FN, void *, DWORD, DWORD *);
typedef BOOL   (WINAPI *fn_CloseHandle)(HANDLE);
typedef DWORD  (WINAPI *fn_WaitForMultipleObjects)(DWORD, const HANDLE *, BOOL, DWORD);
typedef HANDLE (WINAPI *fn_CreateEventW)(void *, BOOL, BOOL, const WCHAR *);
typedef BOOL   (WINAPI *fn_SetEvent)(HANDLE);
typedef BOOL   (WINAPI *fn_ReleaseMutex)(HANDLE);
typedef BOOL   (WINAPI *fn_ReleaseSemaphore)(HANDLE, LONG, LONG *);
typedef void   (WINAPI *fn_LeaveCriticalSection)(void *);
typedef BOOL   (WINAPI *fn_FreeLibrary)(HMODULE);
typedef void   (WINAPI *fn_RtlCaptureContext)(CONTEXT *);
typedef ULONG  (WINAPI *fn_RtlNtStatusToDosError)(NTSTATUS);
typedef int    (WINAPI *fn_GetLocaleInfoEx)(const WCHAR *, DWORD, WCHAR *, int);
typedef int    (WINAPI *fn_GetLocaleInfoW)(LCID, DWORD, WCHAR *, int);
typedef int    (WINAPI *fn_LCIDToLocaleName)(LCID, WCHAR *, int, DWORD);
typedef LCID   (WINAPI *fn_LocaleNameToLCID)(const WCHAR *, DWORD);
typedef BOOL   (WINAPI *fn_IsValidLocaleName)(const WCHAR *);
typedef int    (WINAPI *fn_GetUserDefaultLocaleName)(WCHAR *, int);
typedef LANGID (WINAPI *fn_GetUserDefaultUILanguage)(void);
typedef int    (WINAPI *fn_LCMapStringEx)(const WCHAR *, DWORD, const WCHAR *, int, WCHAR *, int, void *, void *, intptr_t);
typedef UINT   (WINAPI *fn_GetWindowsDirectoryA)(char *, UINT);
typedef DWORD  (WINAPI *fn_QueryDosDeviceW)(const WCHAR *, WCHAR *, DWORD);
typedef DWORD  (WINAPI *fn_K32GetModuleBaseNameW)(HANDLE, HMODULE, WCHAR *, DWORD);
typedef BOOL   (WINAPI *fn_K32EnumProcessModules)(HANDLE, HMODULE *, DWORD, DWORD *);
typedef int    (WINAPI *fn_MultiByteToWideChar)(UINT, DWORD, const char *, int, WCHAR *, int);
typedef int    (WINAPI *fn_WideCharToMultiByte)(UINT, DWORD, const WCHAR *, int, char *, int, const char *, BOOL *);
typedef BOOL   (WINAPI *fn_GetPreferredUILanguages)(DWORD, ULONG *, WCHAR *, ULONG *);
typedef int    (WINAPI *fn_GetGeoInfoEx)(const WCHAR *, DWORD, WCHAR *, int);

static struct {
    fn_CreateThread CreateThread;
    fn_CloseHandle CloseHandle;
    fn_WaitForMultipleObjects WaitForMultipleObjects;
    fn_CreateEventW CreateEventW;
    fn_SetEvent SetEvent;
    fn_ReleaseMutex ReleaseMutex;
    fn_ReleaseSemaphore ReleaseSemaphore;
    fn_LeaveCriticalSection LeaveCriticalSection;
    fn_FreeLibrary FreeLibrary;
    fn_RtlCaptureContext RtlCaptureContext;
    fn_RtlNtStatusToDosError RtlNtStatusToDosError;
    fn_GetLocaleInfoEx GetLocaleInfoEx;
    fn_GetLocaleInfoW GetLocaleInfoW;
    fn_LCIDToLocaleName LCIDToLocaleName;
    fn_LocaleNameToLCID LocaleNameToLCID;
    fn_IsValidLocaleName IsValidLocaleName;
    fn_GetUserDefaultLocaleName GetUserDefaultLocaleName;
    fn_GetUserDefaultUILanguage GetUserDefaultUILanguage;
    fn_LCMapStringEx LCMapStringEx;
    fn_GetWindowsDirectoryA GetWindowsDirectoryA;
    fn_QueryDosDeviceW QueryDosDeviceW;
    fn_K32GetModuleBaseNameW K32GetModuleBaseNameW;
    fn_K32EnumProcessModules K32EnumProcessModules;
    fn_MultiByteToWideChar MultiByteToWideChar;
    fn_WideCharToMultiByte WideCharToMultiByte;
    fn_GetPreferredUILanguages GetUserPreferredUILanguages;
    fn_GetGeoInfoEx GetGeoInfoEx;
} k32;

static pthread_once_t g_k32_once = PTHREAD_ONCE_INIT;

static void *k32_sym(const char *name)
{
    const struct lsiw_builtin_dll *dll = lsiw_builtin_find("kernel32.dll");
    return dll ? lsiw_builtin_lookup(dll, name) : NULL;
}

static void k32_init(void)
{
    k32.CreateThread = (fn_CreateThread)k32_sym("CreateThread");
    k32.CloseHandle = (fn_CloseHandle)k32_sym("CloseHandle");
    k32.WaitForMultipleObjects = (fn_WaitForMultipleObjects)k32_sym("WaitForMultipleObjects");
    k32.CreateEventW = (fn_CreateEventW)k32_sym("CreateEventW");
    k32.SetEvent = (fn_SetEvent)k32_sym("SetEvent");
    k32.ReleaseMutex = (fn_ReleaseMutex)k32_sym("ReleaseMutex");
    k32.ReleaseSemaphore = (fn_ReleaseSemaphore)k32_sym("ReleaseSemaphore");
    k32.LeaveCriticalSection = (fn_LeaveCriticalSection)k32_sym("LeaveCriticalSection");
    k32.FreeLibrary = (fn_FreeLibrary)k32_sym("FreeLibrary");
    k32.RtlCaptureContext = (fn_RtlCaptureContext)k32_sym("RtlCaptureContext");
    k32.RtlNtStatusToDosError = (fn_RtlNtStatusToDosError)k32_sym("RtlNtStatusToDosError");
    k32.GetLocaleInfoEx = (fn_GetLocaleInfoEx)k32_sym("GetLocaleInfoEx");
    k32.GetLocaleInfoW = (fn_GetLocaleInfoW)k32_sym("GetLocaleInfoW");
    k32.LCIDToLocaleName = (fn_LCIDToLocaleName)k32_sym("LCIDToLocaleName");
    k32.LocaleNameToLCID = (fn_LocaleNameToLCID)k32_sym("LocaleNameToLCID");
    k32.IsValidLocaleName = (fn_IsValidLocaleName)k32_sym("IsValidLocaleName");
    k32.GetUserDefaultLocaleName = (fn_GetUserDefaultLocaleName)k32_sym("GetUserDefaultLocaleName");
    k32.GetUserDefaultUILanguage = (fn_GetUserDefaultUILanguage)k32_sym("GetUserDefaultUILanguage");
    k32.LCMapStringEx = (fn_LCMapStringEx)k32_sym("LCMapStringEx");
    k32.GetWindowsDirectoryA = (fn_GetWindowsDirectoryA)k32_sym("GetWindowsDirectoryA");
    k32.QueryDosDeviceW = (fn_QueryDosDeviceW)k32_sym("QueryDosDeviceW");
    k32.K32GetModuleBaseNameW = (fn_K32GetModuleBaseNameW)k32_sym("K32GetModuleBaseNameW");
    k32.K32EnumProcessModules = (fn_K32EnumProcessModules)k32_sym("K32EnumProcessModules");
    k32.MultiByteToWideChar = (fn_MultiByteToWideChar)k32_sym("MultiByteToWideChar");
    k32.WideCharToMultiByte = (fn_WideCharToMultiByte)k32_sym("WideCharToMultiByte");
    k32.GetUserPreferredUILanguages = (fn_GetPreferredUILanguages)k32_sym("GetUserPreferredUILanguages");
    k32.GetGeoInfoEx = (fn_GetGeoInfoEx)k32_sym("GetGeoInfoEx");
}

static void k32_ready(void) { pthread_once(&g_k32_once, k32_init); }

#define CP_ACP 0

static char *w2a(const WCHAR *w, int n)
{
    if (!w) return NULL;
    k32_ready();
    if (!k32.WideCharToMultiByte) return lsiw_w2u_n(w, n);
    int len = n < 0 ? (int)wlen(w) : n;
    int need = k32.WideCharToMultiByte(CP_ACP, 0, w, len, NULL, 0, NULL, NULL);
    char *s = xmalloc0((size_t)need + 1);
    k32.WideCharToMultiByte(CP_ACP, 0, w, len, s, need, NULL, NULL);
    return s;
}

static WCHAR *a2w(const char *s, int n)
{
    if (!s) return NULL;
    k32_ready();
    int len = n < 0 ? (int)strlen(s) : n;
    if (!k32.MultiByteToWideChar) {
        char *t = strndup(s, (size_t)len);
        WCHAR *w = lsiw_u2w(t);
        free(t);
        return w;
    }
    int need = k32.MultiByteToWideChar(CP_ACP, 0, s, len, NULL, 0);
    WCHAR *w = xmalloc0(((size_t)need + 1) * sizeof(WCHAR));
    k32.MultiByteToWideChar(CP_ACP, 0, s, len, w, need);
    return w;
}

/* NTSTATUS stored in OVERLAPPED.Internal -> Win32 error code. */
static DWORD status_to_win32(uint32_t st)
{
    switch (st) {
    case STATUS_SUCCESS:          return ERROR_SUCCESS;
    case STATUS_NOTIFY_CLEANUP:   return ERROR_NOTIFY_CLEANUP;
    case STATUS_NOTIFY_ENUM_DIR:  return ERROR_NOTIFY_ENUM_DIR;
    case STATUS_BUFFER_OVERFLOW:  return ERROR_MORE_DATA;
    case STATUS_END_OF_FILE:      return ERROR_HANDLE_EOF;
    case STATUS_CANCELLED:        return ERROR_OPERATION_ABORTED;
    case STATUS_PIPE_BROKEN:      return ERROR_BROKEN_PIPE;
    case STATUS_PIPE_DISCONNECTED: return ERROR_PIPE_NOT_CONNECTED;
    case STATUS_DELETE_PENDING:   return ERROR_ACCESS_DENIED;
    case STATUS_PENDING:          return ERROR_IO_PENDING;
    }
    k32_ready();
    if (k32.RtlNtStatusToDosError) return (DWORD)k32.RtlNtStatusToDosError((NTSTATUS)st);
    return ERROR_GEN_FAILURE;
}

/* Signal an event handle from any thread (also host threads without a TEB). */
static void signal_event(HANDLE ev)
{
    if (!ev) return;
    struct lsiw_handle_obj *e = lsiw_handle_get(ev, H_EVENT);
    if (!e) return;
    lsiw_wait_lock();
    e->signaled = 1;
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
}

/* Start a Windows thread (TEB, TLS, DLL_THREAD_ATTACH) through kernel32's
 * own CreateThread and drop the handle; the thread keeps running. */
static int start_windows_thread(THREAD_START_FN fn, void *arg, SIZE_T stack)
{
    k32_ready();
    if (!k32.CreateThread) return 0;
    HANDLE h = k32.CreateThread(NULL, stack, fn, arg, 0, NULL);
    if (!h) return 0;
    k32.CloseHandle(h);
    return 1;
}

/* A detached plain pthread for host-side helpers that never run Windows code. */
static int start_host_thread(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    int r = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r == 0;
}

/* Thread id behind a thread handle (or the pseudo handle). 0 if invalid or
 * the thread has already exited. */
static uint32_t thread_tid(HANDLE h)
{
    if (h == PSEUDO_THREAD) return lsiw_thread()->tid;
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_THREAD);
    if (!o) return 0;
    lsiw_wait_lock();
    uint32_t tid = o->signaled ? 0 : o->tid;
    lsiw_wait_unlock();
    return tid;
}

/* Linux pid behind a process handle; -1 if the handle is not a process. */
static pid_t process_pid(HANDLE h)
{
    if (h == PSEUDO_PROCESS) return getpid();
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_PROCESS);
    return o ? (pid_t)o->pid : -1;
}

/* ================================================================== */
/* Handle-table objects owned by this file                              */
/* ================================================================== */

/* Kernel objects that exist only here (completion ports, power requests,
 * volume searches) are H_GENERIC handles whose payload sits in obj->data;
 * obj->free_data both identifies the kind and releases the payload when
 * the last handle is closed. Waits treat them as signalled. */
static HANDLE gobj_create(void *data, void (*release)(void *))
{
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_GENERIC, &o);
    if (!h) return NULL;
    o->data = data;
    o->free_data = release;
    return h;
}

static void *gobj_get(HANDLE h, void (*release)(void *))
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_GENERIC);
    return o && o->free_data == release ? o->data : NULL;
}

/* ---- Per-file state (completion port binding, pipe end, directory watch) ----
 * Attached to the H_FILE object through obj->data / obj->free_data, which
 * k32_file.c leaves unused, so the record is released with the handle. If
 * another module already uses obj->data (sockets may), the record is kept
 * on the side list only and matched by handle, object and descriptor. */
struct iocp;
struct tp_object;
struct pipe_inst;
struct dirwatch;

struct fext {
    struct fext *next;
    HANDLE h;
    struct lsiw_handle_obj *obj;
    int fd;
    int tagged;                      /* lives in obj->data                  */
    struct iocp *port;               /* CreateIoCompletionPort binding      */
    ULONG_PTR key;
    struct tp_object *tpio;          /* CreateThreadpoolIo binding          */
    IO_COMPLETION_FN bind_cb;        /* BindIoCompletionCallback binding    */
    UCHAR notify_modes;              /* SetFileCompletionNotificationModes  */
    struct pipe_inst *pipe;          /* named pipe end                      */
    int readmode_msg, nowait;        /* SetNamedPipeHandleState on any pipe */
    struct dirwatch *dw;             /* ReadDirectoryChangesW state         */
};
static struct fext *g_fexts;
static pthread_mutex_t g_fext_lock = PTHREAD_MUTEX_INITIALIZER;   /* outermost lock */

static void fext_release(void *p);
static void fext_free(struct fext *f);

static int fext_untagged_alive(const struct fext *f)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(f->h, H_FILE);
    return o == f->obj && o->fd == f->fd && o->free_data != fext_release;
}

/* Find (or create) the record for a file handle. Call with g_fext_lock held. */
static struct fext *fext_find(HANDLE h, int create)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    if (!o) return NULL;
    if (o->free_data == fext_release) return o->data;
    struct fext *found = NULL, *dead = NULL;
    for (struct fext **pp = &g_fexts; *pp;) {
        struct fext *f = *pp;
        if (!f->tagged && !fext_untagged_alive(f)) {   /* handle closed: drop */
            *pp = f->next;
            f->next = dead;
            dead = f;
            continue;
        }
        if (!f->tagged && f->h == h && f->obj == o) found = f;
        pp = &f->next;
    }
    while (dead) { struct fext *f = dead; dead = f->next; fext_free(f); }
    if (found || !create) return found;
    struct fext *f = xmalloc0(sizeof *f);
    f->h = h;
    f->obj = o;
    f->fd = o->fd;
    if (!o->data && !o->free_data) {
        o->data = f;
        o->free_data = fext_release;
        f->tagged = 1;
    }
    f->next = g_fexts;
    g_fexts = f;
    return f;
}

/* obj->free_data of a tagged record: the file handle is being destroyed. */
static void fext_release(void *p)
{
    struct fext *f = p;
    pthread_mutex_lock(&g_fext_lock);
    for (struct fext **pp = &g_fexts; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    pthread_mutex_unlock(&g_fext_lock);
    fext_free(f);
}

/* ================================================================== */
/* User APCs                                                            */
/* ================================================================== */

/* Windows delivers a user APC only when its target thread enters an
 * alertable wait (SleepEx, WaitFor...Ex, GetQueuedCompletionStatusEx,
 * MsgWaitForMultipleObjectsEx with alertable = TRUE). APCs are queued per
 * thread here and run by lsiw_apc_run_pending(), which those waits call. */
enum { APC_USER, APC_IO };
struct apc {
    struct apc *next;
    uint32_t tid;
    int kind;
    void *fn;
    ULONG_PTR arg;
    DWORD err, bytes;
    OVERLAPPED *ov;
};
static struct apc *g_apc_head, *g_apc_tail;
static pthread_mutex_t g_apc_lock = PTHREAD_MUTEX_INITIALIZER;
static void iocp_wake_all(void);

static void apc_queue(uint32_t tid, int kind, void *fn, ULONG_PTR arg, DWORD err, DWORD bytes, OVERLAPPED *ov)
{
    struct apc *a = xmalloc0(sizeof *a);
    a->tid = tid; a->kind = kind; a->fn = fn; a->arg = arg;
    a->err = err; a->bytes = bytes; a->ov = ov;
    pthread_mutex_lock(&g_apc_lock);
    if (g_apc_tail) g_apc_tail->next = a; else g_apc_head = a;
    g_apc_tail = a;
    pthread_mutex_unlock(&g_apc_lock);
    /* Wake alertable waiters: kernel32 waits share the global wait
     * condition, completion-port waits have their own. */
    lsiw_wait_lock();
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
    iocp_wake_all();
}

int lsiw_apc_pending(void)
{
    uint32_t tid = lsiw_thread()->tid;
    int found = 0;
    pthread_mutex_lock(&g_apc_lock);
    for (struct apc *a = g_apc_head; a && !found; a = a->next) found = a->tid == tid;
    pthread_mutex_unlock(&g_apc_lock);
    return found;
}

/* Run every APC queued to the calling thread, oldest first, including any
 * queued by the APCs themselves. Returns how many ran. */
int lsiw_apc_run_pending(void)
{
    uint32_t tid = lsiw_thread()->tid;
    int ran = 0;
    for (;;) {
        struct apc *a = NULL, *prev = NULL;
        pthread_mutex_lock(&g_apc_lock);
        for (struct apc *p = g_apc_head; p; prev = p, p = p->next) {
            if (p->tid != tid) continue;
            a = p;
            if (prev) prev->next = p->next; else g_apc_head = p->next;
            if (g_apc_tail == p) g_apc_tail = prev;
            break;
        }
        pthread_mutex_unlock(&g_apc_lock);
        if (!a) return ran;
        if (a->kind == APC_USER) ((APC_FN)a->fn)(a->arg);
        else ((IO_COMPLETION_FN)a->fn)(a->err, a->bytes, a->ov);
        free(a);
        ran++;
    }
}

API DWORD QueueUserAPC(APC_FN fn, HANDLE thread, ULONG_PTR data)
{
    if (!fn) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (thread != PSEUDO_THREAD && !lsiw_handle_get(thread, H_THREAD)) {
        lsiw_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    uint32_t tid = thread_tid(thread);
    /* NtQueueApcThread on a terminated thread fails with STATUS_UNSUCCESSFUL. */
    if (!tid) { lsiw_set_last_error(ERROR_GEN_FAILURE); return 0; }
    apc_queue(tid, APC_USER, (void *)fn, data, 0, 0, NULL);
    return 1;
}

#define QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC 0x1
#define QUEUE_USER_APC_CALLBACK_DATA_CONTEXT  0x10000

API BOOL QueueUserAPC2(APC_FN fn, HANDLE thread, ULONG_PTR data, DWORD flags)
{
    if (flags & ~(DWORD)(QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC | QUEUE_USER_APC_CALLBACK_DATA_CONTEXT)) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* The callback-data form hands the APC the interrupted thread's CONTEXT,
     * which needs a real asynchronous interruption of the target thread. */
    if (flags & QUEUE_USER_APC_CALLBACK_DATA_CONTEXT) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
    /* A special user APC runs without the target waiting alertably. For the
     * calling thread "as soon as possible" is now; other threads get it at
     * their next alertable wait (they cannot be interrupted safely). */
    if ((flags & QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC) &&
        (thread == PSEUDO_THREAD || thread_tid(thread) == lsiw_thread()->tid)) {
        if (!fn) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        fn(data);
        return TRUE;
    }
    return QueueUserAPC(fn, thread, data) != 0;
}

/* ================================================================== */
/* I/O completion ports                                                 */
/* ================================================================== */

struct iocp_packet {
    struct iocp_packet *next;
    ULONG_PTR key;
    OVERLAPPED *ov;
    uint32_t status;
    DWORD bytes;
};

struct iocp {
    struct iocp *next;               /* g_ports */
    int refs;                        /* handle + bindings + waiters        */
    int closed;                      /* every handle to it was closed      */
    DWORD concurrency;
    struct iocp_packet *head, *tail;
    pthread_cond_t cond;
};
static struct iocp *g_ports;
static pthread_mutex_t g_iocp_lock = PTHREAD_MUTEX_INITIALIZER;

static void iocp_wake_all(void)
{
    pthread_mutex_lock(&g_iocp_lock);
    for (struct iocp *p = g_ports; p; p = p->next) pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&g_iocp_lock);
}

static void iocp_unref_locked(struct iocp *p)
{
    if (--p->refs > 0) return;
    for (struct iocp **pp = &g_ports; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    while (p->head) { struct iocp_packet *k = p->head; p->head = k->next; free(k); }
    pthread_cond_destroy(&p->cond);
    free(p);
}

static void iocp_unref(struct iocp *p)
{
    pthread_mutex_lock(&g_iocp_lock);
    iocp_unref_locked(p);
    pthread_mutex_unlock(&g_iocp_lock);
}

/* free_data of the port handle: the last handle to the port was closed.
 * Threads blocked in GetQueuedCompletionStatus return ERROR_ABANDONED_WAIT_0. */
static void iocp_release_handle(void *data)
{
    struct iocp *p = data;
    pthread_mutex_lock(&g_iocp_lock);
    p->closed = 1;
    while (p->head) { struct iocp_packet *k = p->head; p->head = k->next; free(k); }
    p->tail = NULL;
    pthread_cond_broadcast(&p->cond);
    iocp_unref_locked(p);
    pthread_mutex_unlock(&g_iocp_lock);
}

static int iocp_post(struct iocp *p, ULONG_PTR key, OVERLAPPED *ov, uint32_t status, DWORD bytes)
{
    struct iocp_packet *k = xmalloc0(sizeof *k);
    k->key = key; k->ov = ov; k->status = status; k->bytes = bytes;
    pthread_mutex_lock(&g_iocp_lock);
    if (p->closed) { pthread_mutex_unlock(&g_iocp_lock); free(k); return 0; }
    if (p->tail) p->tail->next = k; else p->head = k;
    p->tail = k;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&g_iocp_lock);
    return 1;
}

static struct iocp *iocp_from_handle(HANDLE h)
{
    struct iocp *p = gobj_get(h, iocp_release_handle);
    if (p) {
        pthread_mutex_lock(&g_iocp_lock);
        if (p->closed) p = NULL; else p->refs++;
        pthread_mutex_unlock(&g_iocp_lock);
    }
    return p;
}

static HANDLE iocp_create(DWORD concurrency)
{
    struct iocp *p = xmalloc0(sizeof *p);
    p->refs = 1;
    p->concurrency = concurrency ? concurrency : (DWORD)sysconf(_SC_NPROCESSORS_ONLN);
    cond_init_mono(&p->cond);
    pthread_mutex_lock(&g_iocp_lock);
    p->next = g_ports;
    g_ports = p;
    pthread_mutex_unlock(&g_iocp_lock);
    HANDLE h = gobj_create(p, iocp_release_handle);
    if (!h) iocp_unref(p);
    return h;
}

/* Bind a file to a port: done once per file, as on Windows. */
static BOOL iocp_bind(HANDLE file, struct iocp *port, ULONG_PTR key)
{
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(file, 1);
    if (!f) { pthread_mutex_unlock(&g_fext_lock); lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (f->port || f->tpio || f->bind_cb) {
        pthread_mutex_unlock(&g_fext_lock);
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_iocp_lock);
    port->refs++;
    pthread_mutex_unlock(&g_iocp_lock);
    f->port = port;
    f->key = key;
    pthread_mutex_unlock(&g_fext_lock);
    return TRUE;
}

API HANDLE CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD concurrency)
{
    if (file == INVALID_HANDLE_VALUE) {
        if (existing) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
        HANDLE h = iocp_create(concurrency);
        if (h) lsiw_set_last_error(0);
        return h;
    }
    if (!lsiw_handle_get(file, H_FILE)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    HANDLE ph = existing;
    if (!ph && !(ph = iocp_create(concurrency))) return NULL;
    struct iocp *p = iocp_from_handle(ph);
    if (!p) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    BOOL ok = iocp_bind(file, p, key);
    iocp_unref(p);
    if (!ok) {
        if (!existing) { DWORD e = lsiw_get_last_error(); lsiw_handle_close(ph); lsiw_set_last_error(e); }
        return NULL;
    }
    return ph;
}

API BOOL PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, OVERLAPPED *ov)
{
    struct iocp *p = iocp_from_handle(port);
    if (!p) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    int ok = iocp_post(p, key, ov, STATUS_SUCCESS, bytes);
    iocp_unref(p);
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

/* Dequeue up to `max` packets. Returns 0 (ok), WAIT_TIMEOUT,
 * WAIT_IO_COMPLETION or ERROR_ABANDONED_WAIT_0. */
static DWORD iocp_wait(struct iocp *p, struct iocp_packet **out, ULONG max, ULONG *got, DWORD ms, int alertable)
{
    uint64_t deadline = ms == INFINITE ? 0 : mono_ns() + (uint64_t)ms * 1000000ull;
    *got = 0;
    pthread_mutex_lock(&g_iocp_lock);
    for (;;) {
        if (p->closed) { pthread_mutex_unlock(&g_iocp_lock); return ERROR_ABANDONED_WAIT_0; }
        if (p->head) {
            while (p->head && *got < max) {
                struct iocp_packet *k = p->head;
                p->head = k->next;
                if (!p->head) p->tail = NULL;
                out[(*got)++] = k;
            }
            pthread_mutex_unlock(&g_iocp_lock);
            return 0;
        }
        if (alertable && lsiw_apc_pending()) {
            pthread_mutex_unlock(&g_iocp_lock);
            lsiw_apc_run_pending();
            return WAIT_IO_COMPLETION;
        }
        if (ms == 0 || (deadline && mono_ns() >= deadline)) { pthread_mutex_unlock(&g_iocp_lock); return WAIT_TIMEOUT; }
        cond_wait_until(&p->cond, &g_iocp_lock, deadline);
    }
}

API BOOL GetQueuedCompletionStatus(HANDLE port, DWORD *bytes, ULONG_PTR *key, OVERLAPPED **ov, DWORD ms)
{
    if (ov) *ov = NULL;
    struct iocp *p = iocp_from_handle(port);
    if (!p) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct iocp_packet *k;
    ULONG got;
    DWORD r = iocp_wait(p, &k, 1, &got, ms, 0);
    iocp_unref(p);
    if (r) { lsiw_set_last_error(r); return FALSE; }
    if (bytes) *bytes = k->bytes;
    if (key) *key = k->key;
    if (ov) *ov = k->ov;
    uint32_t st = k->status;
    free(k);
    /* A packet for a failed operation: FALSE with *ov set and the error. */
    if ((int32_t)st < 0) { lsiw_set_last_error(status_to_win32(st)); return FALSE; }
    return TRUE;
}

API BOOL GetQueuedCompletionStatusEx(HANDLE port, OVERLAPPED_ENTRY *entries, ULONG count, ULONG *removed,
                                     DWORD ms, BOOL alertable)
{
    if (removed) *removed = 0;
    if (!entries || !count || !removed) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct iocp *p = iocp_from_handle(port);
    if (!p) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    ULONG max = count > 1024 ? 1024 : count, got;
    struct iocp_packet **ks = malloc(max * sizeof *ks);
    DWORD r = iocp_wait(p, ks, max, &got, ms, alertable);
    iocp_unref(p);
    if (r) { free(ks); lsiw_set_last_error(r); return FALSE; }
    for (ULONG i = 0; i < got; i++) {
        entries[i].lpCompletionKey = ks[i]->key;
        entries[i].lpOverlapped = ks[i]->ov;
        entries[i].Internal = ks[i]->status;
        entries[i].dwNumberOfBytesTransferred = ks[i]->bytes;
        free(ks[i]);
    }
    free(ks);
    *removed = got;
    return TRUE;
}

#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define FILE_SKIP_SET_EVENT_ON_HANDLE        0x2

API BOOL SetFileCompletionNotificationModes(HANDLE file, UCHAR flags)
{
    if (flags & ~(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE)) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(file, 1);
    if (f) f->notify_modes = flags;
    pthread_mutex_unlock(&g_fext_lock);
    if (!f) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

/* Locking user buffers for faster overlapped I/O is a pure optimisation. */
API BOOL SetFileIoOverlappedRange(HANDLE file, UCHAR *start, ULONG len)
{
    (void)start; (void)len;
    if (!lsiw_handle_get(file, H_FILE)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

/* ---- delivering completions ---- */

static void tpio_complete(struct tp_object *io, OVERLAPPED *ov, uint32_t status, DWORD bytes);
static void tp_submit_io_routine(IO_COMPLETION_FN fn, DWORD err, DWORD bytes, OVERLAPPED *ov);
static void tp_obj_hold(struct tp_object *o, int delta);

/* Where the completion of an operation goes, captured when an asynchronous
 * operation starts so it is still delivered after the file handle closes
 * (a pending ReadDirectoryChangesW completes with STATUS_NOTIFY_CLEANUP). */
struct io_target {
    struct iocp *port;
    ULONG_PTR key;
    struct tp_object *tpio;
    IO_COMPLETION_FN bind_cb;
    IO_COMPLETION_FN routine;        /* ReadFileEx-style completion routine */
    uint32_t tid;                    /* thread that started the operation   */
};

static void io_target_capture(HANDLE file, struct io_target *t, IO_COMPLETION_FN routine)
{
    memset(t, 0, sizeof *t);
    t->routine = routine;
    t->tid = lsiw_thread()->tid;
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(file, 0);
    if (f && f->port) {
        pthread_mutex_lock(&g_iocp_lock);
        f->port->refs++;
        pthread_mutex_unlock(&g_iocp_lock);
        t->port = f->port;
        t->key = f->key;
    } else if (f && f->tpio) {
        t->tpio = f->tpio;
        tp_obj_hold(t->tpio, 1);
    } else if (f) t->bind_cb = f->bind_cb;
    pthread_mutex_unlock(&g_fext_lock);
}

/* Queue a completion routine: an APC for the issuing thread when alertable
 * waits deliver APCs, otherwise a thread-pool callback (see above). */
static void queue_completion_routine(IO_COMPLETION_FN fn, uint32_t tid, DWORD err, DWORD bytes, OVERLAPPED *ov)
{
    if (alertable_waits_deliver_apcs() && tid) apc_queue(tid, APC_IO, (void *)fn, 0, err, bytes, ov);
    else tp_submit_io_routine(fn, err, bytes, ov);
}

/* Drop the references of a target whose operation never started. */
static void io_target_release(struct io_target *t)
{
    if (t->port) iocp_unref(t->port);
    if (t->tpio) tp_obj_hold(t->tpio, -1);
    memset(t, 0, sizeof *t);
}

/* Complete an asynchronous operation: fill the OVERLAPPED, signal its event,
 * then queue the completion packet / callback / routine. */
static void io_target_complete(struct io_target *t, OVERLAPPED *ov, uint32_t status, DWORD bytes)
{
    if (ov) {
        __atomic_store_n(&ov->InternalHigh, bytes, __ATOMIC_RELAXED);
        __atomic_store_n(&ov->Internal, status, __ATOMIC_RELEASE);
        if (ov->hEvent && !t->routine) signal_event((HANDLE)((uintptr_t)ov->hEvent & ~(uintptr_t)1));
    }
    /* An event handle with the low bit set asks for no completion packet. */
    int quiet = ov && ((uintptr_t)ov->hEvent & 1);
    if (t->routine) queue_completion_routine(t->routine, t->tid, status_to_win32(status), bytes, ov);
    else if (t->port && !quiet) iocp_post(t->port, t->key, ov, status, bytes);
    else if (t->tpio && !quiet) tpio_complete(t->tpio, ov, status, bytes);
    else if (t->bind_cb && !quiet) tp_submit_io_routine(t->bind_cb, status_to_win32(status), bytes, ov);
    io_target_release(t);
}

/* Deliver the completion of an overlapped operation that finished inside
 * the call that started it (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS then
 * suppresses the packet for a successful one). */
static void io_deliver_sync(HANDLE file, OVERLAPPED *ov, DWORD bytes, uint32_t status)
{
    if (!ov || ((uintptr_t)ov->hEvent & 1)) return;
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(file, 0);
    if (!f || ((int32_t)status >= 0 && (f->notify_modes & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS))) {
        pthread_mutex_unlock(&g_fext_lock);
        return;
    }
    if (f->port) iocp_post(f->port, f->key, ov, status, bytes);
    else if (f->tpio) tpio_complete(f->tpio, ov, status, bytes);
    else if (f->bind_cb) tp_submit_io_routine(f->bind_cb, status_to_win32(status), bytes, ov);
    pthread_mutex_unlock(&g_fext_lock);
}

/* Hook for ReadFile/WriteFile (k32_file.c) and socket I/O: an overlapped
 * operation on `file` completed synchronously. */
void lsiw_iocp_io_done(HANDLE file, void *overlapped, DWORD bytes, uint32_t ntstatus)
{
    io_deliver_sync(file, overlapped, bytes, ntstatus);
}

/* Fill an OVERLAPPED for an operation done synchronously and report it. */
static void complete_sync(HANDLE file, OVERLAPPED *ov, uint32_t status, DWORD bytes)
{
    if (!ov) return;
    ov->InternalHigh = bytes;
    ov->Internal = status;
    if (ov->hEvent) signal_event((HANDLE)((uintptr_t)ov->hEvent & ~(uintptr_t)1));
    io_deliver_sync(file, ov, bytes, status);
}

/* ================================================================== */
/* Thread pool                                                          */
/* ================================================================== */

/* Every callback runs on a worker that is a full Windows thread (TEB, TLS,
 * DLL_THREAD_ATTACH), created through kernel32's CreateThread. One timer
 * thread drives thread-pool timers and timer queues; wait threads (one per
 * 63 waits, like Windows) block in WaitForMultipleObjects. All pool state is
 * protected by g_tp_lock; g_tp_done is broadcast whenever a callback
 * finishes, which is what the WaitFor...Callbacks functions sleep on. */

typedef struct {
    DWORD Version;
    void *Pool;
    void *CleanupGroup;
    void *CleanupGroupCancelCallback;
    void *RaceDll;
    void *ActivationContext;
    void *FinalizationCallback;
    DWORD Flags;                   /* bit 0 LongFunction, bit 1 Persistent */
    DWORD CallbackPriority;        /* version 3 only */
    DWORD Size;                    /* version 3 only */
} TP_CALLBACK_ENVIRON_V3;
_Static_assert(sizeof(TP_CALLBACK_ENVIRON_V3) == 72, "TP_CALLBACK_ENVIRON_V3");
_Static_assert(offsetof(TP_CALLBACK_ENVIRON_V3, Flags) == 56, "TP_CALLBACK_ENVIRON_V3.u");

typedef struct { SIZE_T StackReserve, StackCommit; } TP_POOL_STACK_INFORMATION;

typedef void (WINAPI *TP_SIMPLE_CB)(void *inst, void *ctx);
typedef void (WINAPI *TP_WORK_CB)(void *inst, void *ctx, void *obj);
typedef void (WINAPI *TP_WAIT_CB)(void *inst, void *ctx, void *wait, DWORD result);
typedef void (WINAPI *TP_IO_CB)(void *inst, void *ctx, void *ov, ULONG result, ULONG_PTR bytes, void *io);
typedef void (WINAPI *TP_CANCEL_CB)(void *obj_ctx, void *cleanup_ctx);
typedef void (WINAPI *WAITORTIMER_CB)(void *param, BOOLEAN timer_or_wait_fired);

#define WT_EXECUTEINWAITTHREAD       0x04
#define WT_EXECUTEONLYONCE           0x08
#define WT_EXECUTELONGFUNCTION       0x10
#define WT_EXECUTEINTIMERTHREAD      0x20

enum { TPO_SIMPLE, TPO_WORK, TPO_TIMER, TPO_WAIT, TPO_IO };
enum { CB_NATIVE, CB_WAITORTIMER, CB_THREAD_START, CB_IO_ROUTINE };

struct tp_task {
    struct tp_task *next;
    struct tp_object *obj;
    DWORD wait_result;
    OVERLAPPED *ov;
    ULONG io_result;
    ULONG_PTR io_bytes;
};

struct tp_pool {
    struct tp_pool *next;            /* g_pools */
    int refs;                        /* CreateThreadpool + bound objects */
    int max_threads, min_threads;
    int threads, idle, long_running;
    int queued;
    unsigned long progress, progress_seen;
    struct tp_task *head[3], *tail[3];
    pthread_cond_t cond;
    SIZE_T stack_reserve, stack_commit;
};

struct tp_group { struct tp_object *members; };

struct countdown { int left; HANDLE event; };

struct timer_queue { struct tp_object *timers; };

struct wait_thread {
    struct wait_thread *next;
    HANDLE update;                   /* auto-reset event: the wait set changed */
    struct tp_object *objs[63];
    int count;
};

struct tp_object {
    int kind, cbtype;
    struct tp_pool *pool;
    void *callback, *context;
    struct tp_group *group;
    struct tp_object *gnext, *gprev;
    TP_CANCEL_CB cancel_cb;
    TP_SIMPLE_CB final_cb;
    int prio, long_fn;
    int pending, running;            /* queued / executing callbacks           */
    int hold;                        /* internal references that pin the object */
    int closed;                      /* released by the program               */
    int run_cancel;
    DWORD wt_flags;                  /* WT_* for the legacy APIs              */
    HANDLE done_event;               /* signalled when the object is gone     */
    struct countdown *done_cd;
    /* timers */
    int timer_set, timer_armed, fired_once;
    uint64_t due, period;            /* CLOCK_MONOTONIC ns                    */
    struct tp_object *tnext;
    struct timer_queue *tq;
    struct tp_object *qnext;
    /* waits */
    HANDLE wait_h;
    uint64_t wait_deadline;          /* 0 = no timeout                        */
    DWORD wait_ms;                   /* legacy waits re-arm with this         */
    int wait_active, wait_gen, wt_busy;
    struct wait_thread *wt;
    /* I/O */
    int io_started;
};

struct tp_instance {
    struct tp_object *obj;
    int disassociated, may_run_long;
    void *cs;
    HANDLE mutex, sem, event;
    LONG sem_count;
    HMODULE dll;
};

static pthread_mutex_t g_tp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tp_done, g_timer_cond;
static pthread_once_t g_tp_once = PTHREAD_ONCE_INIT;
static struct tp_pool *g_pools, *g_default_pool;
static struct tp_object *g_timers;          /* armed timers by due time */
static int g_timer_thread_started;
static struct wait_thread *g_wait_threads;
static int g_ncpu;
static __thread int t_in_wait_thread;

static void tp_init(void)
{
    cond_init_mono(&g_tp_done);
    cond_init_mono(&g_timer_cond);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    g_ncpu = n > 0 ? (int)n : 1;
}

static void tp_lock(void) { pthread_once(&g_tp_once, tp_init); pthread_mutex_lock(&g_tp_lock); }
static void tp_unlock(void) { pthread_mutex_unlock(&g_tp_lock); }

static struct tp_pool *pool_new_locked(void)
{
    struct tp_pool *p = xmalloc0(sizeof *p);
    p->refs = 1;
    p->max_threads = 512;
    cond_init_mono(&p->cond);
    p->next = g_pools;
    g_pools = p;
    return p;
}

static struct tp_pool *pool_default_locked(void)
{
    if (!g_default_pool) g_default_pool = pool_new_locked();
    return g_default_pool;
}

static void pool_free_locked(struct tp_pool *p)
{
    for (struct tp_pool **pp = &g_pools; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    pthread_cond_destroy(&p->cond);
    free(p);
}

static void pool_unref_locked(struct tp_pool *p)
{
    if (--p->refs > 0) return;
    pthread_cond_broadcast(&p->cond);       /* idle workers exit */
    if (!p->threads) pool_free_locked(p);
}

static DWORD WINAPI tp_worker_main(void *arg);
static void timer_thread_ensure_locked(void);

static int pool_spawn_locked(struct tp_pool *p)
{
    k32_ready();
    p->threads++;
    /* kernel32's CreateThread only waits for the new thread to publish its
     * id, never for g_tp_lock, so calling it with the lock held is safe. */
    if (!start_windows_thread(tp_worker_main, p, p->stack_reserve)) { p->threads--; return 0; }
    return 1;
}

static int tp_soft_limit(void) { return g_ncpu + 2 < 4 ? 4 : g_ncpu + 2; }

/* Make sure someone will run the queue: wake an idle worker, start a new
 * one while the pool is below its working-thread target, otherwise let the
 * timer thread's starvation check add threads if the queue stops moving. */
static void pool_kick_locked(struct tp_pool *p)
{
    if (p->idle > 0) pthread_cond_signal(&p->cond);
    if (p->queued <= p->idle) return;
    int active = p->threads - p->idle - p->long_running;
    if (p->threads < p->max_threads && (active < tp_soft_limit() || p->threads < p->min_threads))
        pool_spawn_locked(p);
    else
        timer_thread_ensure_locked();
}

static struct tp_task *pool_dequeue_locked(struct tp_pool *p)
{
    for (int i = 0; i < 3; i++) {
        struct tp_task *t = p->head[i];
        if (!t) continue;
        p->head[i] = t->next;
        if (!p->head[i]) p->tail[i] = NULL;
        p->queued--;
        return t;
    }
    return NULL;
}

static void tp_enqueue_locked(struct tp_object *o, DWORD wait_result, OVERLAPPED *ov, ULONG io_result, ULONG_PTR io_bytes)
{
    struct tp_task *t = xmalloc0(sizeof *t);
    t->obj = o;
    t->wait_result = wait_result;
    t->ov = ov;
    t->io_result = io_result;
    t->io_bytes = io_bytes;
    o->pending++;
    struct tp_pool *p = o->pool;
    int pr = o->prio < 0 || o->prio > 2 ? 1 : o->prio;
    if (p->tail[pr]) p->tail[pr]->next = t; else p->head[pr] = t;
    p->tail[pr] = t;
    p->queued++;
    pool_kick_locked(p);
}

/* Drop queued (not yet started) callbacks of an object. */
static int tp_cancel_pending_locked(struct tp_object *o)
{
    struct tp_pool *p = o->pool;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        struct tp_task *prev = NULL;
        for (struct tp_task *t = p->head[i]; t;) {
            struct tp_task *next = t->next;
            if (t->obj == o) {
                if (prev) prev->next = next; else p->head[i] = next;
                if (p->tail[i] == t) p->tail[i] = prev;
                free(t);
                n++;
                p->queued--;
            } else prev = t;
            t = next;
        }
    }
    o->pending -= n;
    return n;
}

static void group_remove_locked(struct tp_object *o)
{
    if (!o->group) return;
    if (o->gprev) o->gprev->gnext = o->gnext; else o->group->members = o->gnext;
    if (o->gnext) o->gnext->gprev = o->gprev;
    o->group = NULL;
    o->gnext = o->gprev = NULL;
}

static void timer_unlink_locked(struct tp_object *o)
{
    if (!o->timer_armed) return;
    for (struct tp_object **pp = &g_timers; *pp; pp = &(*pp)->tnext)
        if (*pp == o) { *pp = o->tnext; break; }
    o->timer_armed = 0;
    o->tnext = NULL;
}

static void timer_insert_locked(struct tp_object *o)
{
    struct tp_object **pp = &g_timers;
    while (*pp && (*pp)->due <= o->due) pp = &(*pp)->tnext;
    o->tnext = *pp;
    *pp = o;
    o->timer_armed = 1;
    timer_thread_ensure_locked();
    if (g_timers == o) pthread_cond_signal(&g_timer_cond);
}

static void tq_remove_locked(struct tp_object *o)
{
    if (!o->tq) return;
    for (struct tp_object **pp = &o->tq->timers; *pp; pp = &(*pp)->qnext)
        if (*pp == o) { *pp = o->qnext; break; }
    o->tq = NULL;
    o->qnext = NULL;
}

static void wait_detach_locked(struct tp_object *o)
{
    if (!o->wait_active) return;
    struct wait_thread *wt = o->wt;
    for (int i = 0; i < wt->count; i++)
        if (wt->objs[i] == o) { wt->objs[i] = wt->objs[--wt->count]; break; }
    o->wait_active = 0;
    o->wt = NULL;
    o->wait_gen++;
    signal_event(wt->update);
}

/* Free a released object once nothing refers to it any more. */
static void tp_obj_maybe_free_locked(struct tp_object *o)
{
    if (!o->closed || o->pending || o->running || o->hold || o->timer_armed || o->wait_active || o->wt_busy) return;
    group_remove_locked(o);
    tq_remove_locked(o);
    HANDLE ev = o->done_event;
    struct countdown *cd = o->done_cd;
    pool_unref_locked(o->pool);
    free(o);
    if (ev && ev != INVALID_HANDLE_VALUE) signal_event(ev);
    if (cd && --cd->left == 0) {
        if (cd->event && cd->event != INVALID_HANDLE_VALUE) signal_event(cd->event);
        free(cd);
    }
    pthread_cond_broadcast(&g_tp_done);
}

/* Wait until an object has no queued or running callbacks (and, unless we
 * are its wait thread, no wait thread is still blocked on its handle). */
static void tp_wait_idle_locked(struct tp_object *o)
{
    while (o->pending || o->running || (o->wt_busy && !t_in_wait_thread))
        pthread_cond_wait(&g_tp_done, &g_tp_lock);
}

static struct tp_object *tp_obj_new(int kind, int cbtype, void *cb, void *ctx, const TP_CALLBACK_ENVIRON_V3 *env)
{
    struct tp_object *o = xmalloc0(sizeof *o);
    o->kind = kind;
    o->cbtype = cbtype;
    o->callback = cb;
    o->context = ctx;
    o->prio = 1;
    tp_lock();
    struct tp_pool *p = env && env->Pool ? (struct tp_pool *)env->Pool : pool_default_locked();
    p->refs++;
    o->pool = p;
    if (env) {
        if (env->CleanupGroup) {
            struct tp_group *g = env->CleanupGroup;
            o->group = g;
            o->cancel_cb = (TP_CANCEL_CB)env->CleanupGroupCancelCallback;
            o->gnext = g->members;
            if (g->members) g->members->gprev = o;
            g->members = o;
        }
        o->final_cb = (TP_SIMPLE_CB)env->FinalizationCallback;
        o->long_fn = env->Flags & 1;
        if (env->Version >= 3 && env->CallbackPriority <= 2) o->prio = (int)env->CallbackPriority;
    }
    tp_unlock();
    return o;
}

static void tp_invoke(struct tp_object *o, struct tp_instance *inst, const struct tp_task *t)
{
    switch (o->cbtype) {
    case CB_NATIVE:
        switch (o->kind) {
        case TPO_SIMPLE: ((TP_SIMPLE_CB)o->callback)(inst, o->context); break;
        case TPO_WORK:
        case TPO_TIMER:  ((TP_WORK_CB)o->callback)(inst, o->context, o); break;
        case TPO_WAIT:   ((TP_WAIT_CB)o->callback)(inst, o->context, o, t->wait_result); break;
        case TPO_IO:     ((TP_IO_CB)o->callback)(inst, o->context, t->ov, t->io_result, t->io_bytes, o); break;
        }
        break;
    case CB_WAITORTIMER:
        ((WAITORTIMER_CB)o->callback)(o->context, (BOOLEAN)(o->kind == TPO_TIMER || t->wait_result == WAIT_TIMEOUT));
        break;
    case CB_THREAD_START:
        ((THREAD_START_FN)o->callback)(o->context);
        break;
    case CB_IO_ROUTINE:
        ((IO_COMPLETION_FN)o->callback)(t->io_result, (DWORD)t->io_bytes, t->ov);
        break;
    }
    if (o->final_cb) o->final_cb(inst, o->context);
}

/* The ...WhenCallbackReturns actions, in the order Windows performs them;
 * FreeLibrary comes last because it may unload the callback's own code. */
static void tp_instance_finish(struct tp_instance *inst)
{
    if (!inst->cs && !inst->mutex && !inst->sem && !inst->event && !inst->dll) return;
    k32_ready();
    if (inst->cs) k32.LeaveCriticalSection(inst->cs);
    if (inst->mutex) k32.ReleaseMutex(inst->mutex);
    if (inst->sem) k32.ReleaseSemaphore(inst->sem, inst->sem_count, NULL);
    if (inst->event) k32.SetEvent(inst->event);
    if (inst->dll) k32.FreeLibrary(inst->dll);
}

/* Run one queued callback. Called and returns with g_tp_lock held. */
static void tp_run_task_locked(struct tp_pool *p, struct tp_task *t)
{
    struct tp_object *o = t->obj;
    o->pending--;
    o->running++;
    o->hold++;
    p->progress++;
    struct tp_instance inst = { .obj = o, .may_run_long = o->long_fn };
    if (inst.may_run_long) p->long_running++;
    tp_unlock();
    tp_invoke(o, &inst, t);
    tp_instance_finish(&inst);
    tp_lock();
    if (!inst.disassociated) o->running--;
    if (inst.may_run_long) p->long_running--;
    o->hold--;
    pthread_cond_broadcast(&g_tp_done);
    tp_obj_maybe_free_locked(o);
    free(t);
}

static DWORD WINAPI tp_worker_main(void *arg)
{
    struct tp_pool *p = arg;
    tp_lock();
    for (;;) {
        struct tp_task *t = pool_dequeue_locked(p);
        if (t) { tp_run_task_locked(p, t); continue; }
        if (p->refs <= 0) break;
        p->idle++;
        int r = cond_wait_until(&p->cond, &g_tp_lock, mono_ns() + 20000000000ull);
        p->idle--;
        /* Idle workers above the pool minimum retire after 20 s. */
        if (r == ETIMEDOUT && !p->queued && p->threads > p->min_threads) break;
    }
    p->threads--;
    if (p->refs <= 0 && !p->threads) pool_free_locked(p);
    tp_unlock();
    return 0;
}

/* ---- timer thread ---- */

static DWORD WINAPI timer_thread_main(void *arg);

static void timer_thread_ensure_locked(void)
{
    if (g_timer_thread_started) { pthread_cond_signal(&g_timer_cond); return; }
    g_timer_thread_started = 1;
    if (!start_windows_thread(timer_thread_main, NULL, 0)) g_timer_thread_started = 0;
}

/* Starvation check: a pool whose queue made no progress since the last
 * look while every worker is busy gets one more thread. Returns non-zero
 * while some pool still has work waiting for a thread. */
static int pools_check_locked(void)
{
    int waiting = 0;
    for (struct tp_pool *p = g_pools; p; p = p->next) {
        if (p->queued <= p->idle) { p->progress_seen = p->progress; continue; }
        waiting = 1;
        if (p->progress == p->progress_seen && p->threads < p->max_threads) pool_spawn_locked(p);
        p->progress_seen = p->progress;
    }
    return waiting;
}

static void tp_run_inline_locked(struct tp_object *o, DWORD result)
{
    struct tp_task t = { .obj = o, .wait_result = result };
    struct tp_instance inst = { .obj = o };
    o->pending--;
    o->running++;
    o->hold++;
    tp_unlock();
    tp_invoke(o, &inst, &t);
    tp_instance_finish(&inst);
    tp_lock();
    if (!inst.disassociated) o->running--;
    o->hold--;
    pthread_cond_broadcast(&g_tp_done);
    tp_obj_maybe_free_locked(o);
}

static DWORD WINAPI timer_thread_main(void *arg)
{
    (void)arg;
    tp_lock();
    for (;;) {
        uint64_t now = mono_ns();
        struct tp_object *inl[32];
        int ninl = 0;
        while (g_timers && g_timers->due <= now && ninl < 32) {
            struct tp_object *o = g_timers;
            g_timers = o->tnext;
            o->tnext = NULL;
            o->timer_armed = 0;
            o->fired_once = 1;
            if (o->period) {
                o->due += o->period;
                if (o->due <= now) o->due = now + o->period;
                timer_insert_locked(o);
            } else o->timer_set = 0;
            if (o->wt_flags & WT_EXECUTEINTIMERTHREAD) { o->pending++; inl[ninl++] = o; }
            else tp_enqueue_locked(o, 0, NULL, 0, 0);
        }
        for (int i = 0; i < ninl; i++) tp_run_inline_locked(inl[i], 0);
        if (ninl) continue;
        uint64_t wake = g_timers ? g_timers->due : 0;
        if (pools_check_locked()) {
            uint64_t s = mono_ns() + 500000000ull;
            if (!wake || s < wake) wake = s;
        }
        cond_wait_until(&g_timer_cond, &g_tp_lock, wake);
    }
    return 0;
}

/* ---- wait threads ---- */

static int handle_usable(HANDLE h)
{
    return h == PSEUDO_PROCESS || h == PSEUDO_THREAD || lsiw_handle_get(h, -1) != NULL;
}

/* A wait was satisfied or timed out. Returns 1 if it left the wait set. */
static int wait_fire_locked(struct tp_object *o, DWORD result, struct tp_object **inl, DWORD *inl_res, int *ninl)
{
    int removed = 0;
    if (o->cbtype == CB_WAITORTIMER && !(o->wt_flags & WT_EXECUTEONLYONCE)) {
        /* RegisterWaitForSingleObject without WT_EXECUTEONLYONCE keeps waiting. */
        o->wait_deadline = o->wait_ms == INFINITE ? 0 : mono_ns() + (uint64_t)o->wait_ms * 1000000ull;
    } else {
        wait_detach_locked(o);
        removed = 1;
    }
    if ((o->wt_flags & WT_EXECUTEINWAITTHREAD) && *ninl < 64) {
        o->pending++;
        inl[*ninl] = o;
        inl_res[(*ninl)++] = result;
    } else tp_enqueue_locked(o, result, NULL, 0, 0);
    return removed;
}

static DWORD WINAPI wait_thread_main(void *arg)
{
    struct wait_thread *wt = arg;
    t_in_wait_thread = 1;
    k32_ready();
    tp_lock();
    for (;;) {
        struct tp_object *inl[64];
        DWORD inl_res[64];
        int ninl = 0;
        uint64_t now = mono_ns(), next = 0;
        for (int i = 0; i < wt->count;) {
            struct tp_object *o = wt->objs[i];
            if (o->wait_deadline && o->wait_deadline <= now && wait_fire_locked(o, WAIT_TIMEOUT, inl, inl_res, &ninl)) continue;
            i++;
        }
        HANDLE hs[64];
        struct tp_object *snap[63];
        int gen[63], n = 0;
        hs[0] = wt->update;
        for (int i = 0; i < wt->count; i++) {
            struct tp_object *o = wt->objs[i];
            hs[n + 1] = o->wait_h;
            snap[n] = o;
            gen[n] = o->wait_gen;
            o->wt_busy++;
            n++;
            if (o->wait_deadline && (!next || o->wait_deadline < next)) next = o->wait_deadline;
        }
        for (int i = 0; i < ninl; i++) tp_run_inline_locked(inl[i], inl_res[i]);
        tp_unlock();
        DWORD ms = INFINITE;
        if (next) {
            uint64_t cur = mono_ns();
            ms = next > cur ? (DWORD)((next - cur + 999999) / 1000000) : 0;
        }
        DWORD r = k32.WaitForMultipleObjects((DWORD)n + 1, hs, FALSE, ms);
        tp_lock();
        ninl = 0;
        if (r >= WAIT_OBJECT_0 + 1 && r <= WAIT_OBJECT_0 + (DWORD)n) {
            struct tp_object *o = snap[r - 1];
            if (o->wait_active && o->wt == wt && o->wait_gen == gen[r - 1])
                wait_fire_locked(o, WAIT_OBJECT_0, inl, inl_res, &ninl);
        } else if (r == WAIT_FAILED) {
            /* A handle was closed while we waited on it: drop those waits. */
            int dropped = 0;
            for (int i = 0; i < n; i++)
                if (snap[i]->wait_active && snap[i]->wt == wt && !handle_usable(snap[i]->wait_h)) {
                    wait_detach_locked(snap[i]);
                    dropped = 1;
                }
            if (!dropped) { tp_unlock(); usleep(10000); tp_lock(); }
        }
        for (int i = 0; i < n; i++) snap[i]->wt_busy--;
        for (int i = 0; i < ninl; i++) tp_run_inline_locked(inl[i], inl_res[i]);
        for (int i = 0; i < n; i++) tp_obj_maybe_free_locked(snap[i]);
        pthread_cond_broadcast(&g_tp_done);
    }
    return 0;
}

static int wait_attach_locked(struct tp_object *o)
{
    struct wait_thread *wt;
    for (wt = g_wait_threads; wt; wt = wt->next)
        if (wt->count < 63) break;
    if (!wt) {
        k32_ready();
        wt = xmalloc0(sizeof *wt);
        wt->update = k32.CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!wt->update) { free(wt); return 0; }
        if (!start_windows_thread(wait_thread_main, wt, 256 * 1024)) {
            k32.CloseHandle(wt->update);
            free(wt);
            return 0;
        }
        wt->next = g_wait_threads;
        g_wait_threads = wt;
    }
    wt->objs[wt->count++] = o;
    o->wt = wt;
    o->wait_active = 1;
    o->wait_gen++;
    signal_event(wt->update);
    return 1;
}

/* ================================================================== */
/* Thread pool API (Vista)                                              */
/* ================================================================== */

API void *CreateThreadpool(void *reserved)
{
    (void)reserved;
    tp_lock();
    struct tp_pool *p = pool_new_locked();
    tp_unlock();
    return p;
}

API void CloseThreadpool(struct tp_pool *p)
{
    if (!p) return;
    tp_lock();
    pool_unref_locked(p);
    tp_unlock();
}

API void SetThreadpoolThreadMaximum(struct tp_pool *p, DWORD max)
{
    tp_lock();
    p->max_threads = max ? (int)(max > 4096 ? 4096 : max) : 1;
    if (p->min_threads > p->max_threads) p->min_threads = p->max_threads;
    tp_unlock();
}

API BOOL SetThreadpoolThreadMinimum(struct tp_pool *p, DWORD min)
{
    tp_lock();
    p->min_threads = (int)(min > 4096 ? 4096 : min);
    if (p->max_threads < p->min_threads) p->max_threads = p->min_threads;
    BOOL ok = TRUE;
    while (p->threads < p->min_threads)
        if (!pool_spawn_locked(p)) { ok = FALSE; break; }
    tp_unlock();
    if (!ok) lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return ok;
}

API BOOL SetThreadpoolStackInformation(struct tp_pool *p, const TP_POOL_STACK_INFORMATION *si)
{
    if (!p || !si) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    tp_lock();
    p->stack_reserve = si->StackReserve;
    p->stack_commit = si->StackCommit;
    tp_unlock();
    return TRUE;
}

API BOOL QueryThreadpoolStackInformation(struct tp_pool *p, TP_POOL_STACK_INFORMATION *si)
{
    if (!p || !si) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    tp_lock();
    si->StackReserve = p->stack_reserve ? p->stack_reserve : 0x400000;
    si->StackCommit = p->stack_commit ? p->stack_commit : 0x1000;
    tp_unlock();
    return TRUE;
}

/* ---- work ---- */

API void *CreateThreadpoolWork(TP_WORK_CB cb, void *ctx, TP_CALLBACK_ENVIRON_V3 *env)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return tp_obj_new(TPO_WORK, CB_NATIVE, (void *)cb, ctx, env);
}

API void SubmitThreadpoolWork(struct tp_object *o)
{
    tp_lock();
    tp_enqueue_locked(o, 0, NULL, 0, 0);
    tp_unlock();
}

API void WaitForThreadpoolWorkCallbacks(struct tp_object *o, BOOL cancel)
{
    tp_lock();
    if (cancel) tp_cancel_pending_locked(o);
    tp_wait_idle_locked(o);
    tp_unlock();
}

static void tp_close(struct tp_object *o)
{
    if (!o) return;
    tp_lock();
    o->closed = 1;
    tp_obj_maybe_free_locked(o);
    tp_unlock();
}

API void CloseThreadpoolWork(struct tp_object *o) { tp_close(o); }

API BOOL TrySubmitThreadpoolCallback(TP_SIMPLE_CB cb, void *ctx, TP_CALLBACK_ENVIRON_V3 *env)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct tp_object *o = tp_obj_new(TPO_SIMPLE, CB_NATIVE, (void *)cb, ctx, env);
    tp_lock();
    o->closed = 1;                       /* freed after it ran */
    tp_enqueue_locked(o, 0, NULL, 0, 0);
    tp_unlock();
    return TRUE;
}

/* ---- timers ---- */

API void *CreateThreadpoolTimer(TP_WORK_CB cb, void *ctx, TP_CALLBACK_ENVIRON_V3 *env)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return tp_obj_new(TPO_TIMER, CB_NATIVE, (void *)cb, ctx, env);
}

/* Returns TRUE if the timer had an expiration pending before this call. */
API BOOL SetThreadpoolTimerEx(struct tp_object *o, const FILETIME *due, DWORD period_ms, DWORD window_ms)
{
    (void)window_ms;               /* coalescing window: we fire on time */
    tp_lock();
    BOOL was = o->timer_armed;
    timer_unlink_locked(o);
    if (!due) {
        o->timer_set = 0;          /* stop queueing; queued callbacks still run */
    } else {
        o->timer_set = 1;
        o->due = deadline_from_filetime(due);
        o->period = (uint64_t)period_ms * 1000000ull;
        timer_insert_locked(o);
    }
    tp_unlock();
    return was;
}

API void SetThreadpoolTimer(struct tp_object *o, const FILETIME *due, DWORD period_ms, DWORD window_ms)
{
    SetThreadpoolTimerEx(o, due, period_ms, window_ms);
}

API BOOL IsThreadpoolTimerSet(struct tp_object *o)
{
    tp_lock();
    BOOL r = o->timer_set;
    tp_unlock();
    return r;
}

API void WaitForThreadpoolTimerCallbacks(struct tp_object *o, BOOL cancel)
{
    tp_lock();
    if (cancel) tp_cancel_pending_locked(o);
    tp_wait_idle_locked(o);
    tp_unlock();
}

API void CloseThreadpoolTimer(struct tp_object *o)
{
    if (!o) return;
    tp_lock();
    timer_unlink_locked(o);
    o->timer_set = 0;
    o->closed = 1;
    tp_obj_maybe_free_locked(o);
    tp_unlock();
}

/* ---- waits ---- */

API void *CreateThreadpoolWait(TP_WAIT_CB cb, void *ctx, TP_CALLBACK_ENVIRON_V3 *env)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return tp_obj_new(TPO_WAIT, CB_NATIVE, (void *)cb, ctx, env);
}

/* Returns TRUE if a previous wait was still active (and is now replaced). */
API BOOL SetThreadpoolWaitEx(struct tp_object *o, HANDLE h, const FILETIME *timeout, void *reserved)
{
    (void)reserved;
    tp_lock();
    BOOL was = o->wait_active;
    wait_detach_locked(o);
    if (h) {
        o->wait_h = h;
        o->wait_deadline = timeout ? deadline_from_filetime(timeout) : 0;
        wait_attach_locked(o);
    }
    tp_unlock();
    return was;
}

API void SetThreadpoolWait(struct tp_object *o, HANDLE h, const FILETIME *timeout)
{
    SetThreadpoolWaitEx(o, h, timeout, NULL);
}

API void WaitForThreadpoolWaitCallbacks(struct tp_object *o, BOOL cancel)
{
    tp_lock();
    if (cancel) tp_cancel_pending_locked(o);
    tp_wait_idle_locked(o);
    tp_unlock();
}

API void CloseThreadpoolWait(struct tp_object *o)
{
    if (!o) return;
    tp_lock();
    wait_detach_locked(o);
    o->closed = 1;
    tp_obj_maybe_free_locked(o);
    tp_unlock();
}

/* ---- I/O ---- */

API void *CreateThreadpoolIo(HANDLE file, TP_IO_CB cb, void *ctx, TP_CALLBACK_ENVIRON_V3 *env)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(file, 1);
    if (!f) { pthread_mutex_unlock(&g_fext_lock); lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    if (f->port || f->tpio || f->bind_cb) {
        pthread_mutex_unlock(&g_fext_lock);
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    struct tp_object *o = tp_obj_new(TPO_IO, CB_NATIVE, (void *)cb, ctx, env);
    f->tpio = o;
    pthread_mutex_unlock(&g_fext_lock);
    return o;
}

API void StartThreadpoolIo(struct tp_object *o) { tp_lock(); o->io_started++; tp_unlock(); }

API void CancelThreadpoolIo(struct tp_object *o)
{
    tp_lock();
    if (o->io_started > 0) o->io_started--;
    tp_unlock();
}

static void tpio_detach(struct tp_object *o)
{
    pthread_mutex_lock(&g_fext_lock);
    for (struct fext *f = g_fexts; f; f = f->next)
        if (f->tpio == o) f->tpio = NULL;
    pthread_mutex_unlock(&g_fext_lock);
}

API void CloseThreadpoolIo(struct tp_object *o)
{
    if (!o) return;
    tpio_detach(o);
    tp_close(o);
}

API void WaitForThreadpoolIoCallbacks(struct tp_object *o, BOOL cancel)
{
    tp_lock();
    if (cancel) tp_cancel_pending_locked(o);
    tp_wait_idle_locked(o);
    tp_unlock();
}

/* Called with g_fext_lock held when I/O on a bound file completes. A
 * completion with no matching StartThreadpoolIo is dropped. */
static void tpio_complete(struct tp_object *io, OVERLAPPED *ov, uint32_t status, DWORD bytes)
{
    DWORD err = status_to_win32(status);
    tp_lock();
    if (!io->closed && io->io_started > 0) {
        io->io_started--;
        tp_enqueue_locked(io, 0, ov, err, bytes);
    }
    tp_unlock();
}

static void tp_submit_io_routine(IO_COMPLETION_FN fn, DWORD err, DWORD bytes, OVERLAPPED *ov)
{
    struct tp_object *o = tp_obj_new(TPO_SIMPLE, CB_IO_ROUTINE, (void *)fn, NULL, NULL);
    tp_lock();
    o->closed = 1;
    tp_enqueue_locked(o, 0, ov, err, bytes);
    tp_unlock();
}

API BOOL BindIoCompletionCallback(HANDLE file, IO_COMPLETION_FN fn, ULONG flags)
{
    (void)flags;
    if (!fn) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(file, 1);
    BOOL ok = FALSE;
    if (!f) lsiw_set_last_error(ERROR_INVALID_HANDLE);
    else if (f->port || f->tpio || f->bind_cb) lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    else { f->bind_cb = fn; ok = TRUE; }
    pthread_mutex_unlock(&g_fext_lock);
    return ok;
}

/* ---- cleanup groups ---- */

API void *CreateThreadpoolCleanupGroup(void) { return xmalloc0(sizeof(struct tp_group)); }

API void CloseThreadpoolCleanupGroupMembers(struct tp_group *g, BOOL cancel, void *cleanup_ctx)
{
    if (!g) return;
    /* Unbind I/O objects first (the file lock ranks above the pool lock). */
    pthread_mutex_lock(&g_fext_lock);
    tp_lock();
    for (struct fext *f = g_fexts; f; f = f->next)
        if (f->tpio && f->tpio->group == g) f->tpio = NULL;
    struct tp_object *list = g->members, *o;
    g->members = NULL;
    for (o = list; o; o = o->gnext) {
        o->group = NULL;
        o->hold++;
        if (o->kind == TPO_TIMER) { timer_unlink_locked(o); o->timer_set = 0; }
        if (o->kind == TPO_WAIT) wait_detach_locked(o);
        o->run_cancel = cancel && tp_cancel_pending_locked(o) > 0 && o->cancel_cb;
    }
    tp_unlock();
    pthread_mutex_unlock(&g_fext_lock);
    /* The cleanup callback lets the program free the context of each
     * object whose pending callbacks were cancelled. */
    for (o = list; o; o = o->gnext)
        if (o->run_cancel) o->cancel_cb(o->context, cleanup_ctx);
    tp_lock();
    for (o = list; o; o = o->gnext) tp_wait_idle_locked(o);
    while (list) {
        o = list;
        list = o->gnext;
        o->gnext = o->gprev = NULL;
        o->hold--;
        o->closed = 1;
        tp_obj_maybe_free_locked(o);
    }
    tp_unlock();
}

API void CloseThreadpoolCleanupGroup(struct tp_group *g)
{
    if (!g) return;
    tp_lock();
    while (g->members) group_remove_locked(g->members);
    tp_unlock();
    free(g);
}

/* ---- callback instance ---- */

API BOOL CallbackMayRunLong(struct tp_instance *inst)
{
    if (!inst) return FALSE;
    tp_lock();
    struct tp_pool *p = inst->obj->pool;
    if (!inst->may_run_long) { inst->may_run_long = 1; p->long_running++; }
    BOOL more = p->idle > 0 || p->threads < p->max_threads;
    if (p->queued > p->idle && p->threads < p->max_threads) pool_spawn_locked(p);
    tp_unlock();
    return more;
}

API void DisassociateCurrentThreadFromCallback(struct tp_instance *inst)
{
    if (!inst) return;
    tp_lock();
    if (!inst->disassociated) {
        inst->disassociated = 1;
        inst->obj->running--;
        pthread_cond_broadcast(&g_tp_done);
    }
    tp_unlock();
}

API void FreeLibraryWhenCallbackReturns(struct tp_instance *inst, HMODULE m) { if (inst) inst->dll = m; }
API void LeaveCriticalSectionWhenCallbackReturns(struct tp_instance *inst, void *cs) { if (inst) inst->cs = cs; }
API void ReleaseMutexWhenCallbackReturns(struct tp_instance *inst, HANDLE m) { if (inst) inst->mutex = m; }
API void SetEventWhenCallbackReturns(struct tp_instance *inst, HANDLE e) { if (inst) inst->event = e; }
API void ReleaseSemaphoreWhenCallbackReturns(struct tp_instance *inst, HANDLE s, DWORD n)
{
    if (inst) { inst->sem = s; inst->sem_count = (LONG)n; }
}

/* ================================================================== */
/* Legacy thread pool: work items, registered waits, timer queues       */
/* ================================================================== */

API BOOL QueueUserWorkItem(THREAD_START_FN fn, void *ctx, ULONG flags)
{
    if (!fn) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct tp_object *o = tp_obj_new(TPO_SIMPLE, CB_THREAD_START, (void *)fn, ctx, NULL);
    tp_lock();
    o->long_fn = (flags & WT_EXECUTELONGFUNCTION) != 0;
    o->closed = 1;
    tp_enqueue_locked(o, 0, NULL, 0, 0);
    tp_unlock();
    return TRUE;
}

static struct tp_object *register_wait(HANDLE h, WAITORTIMER_CB cb, void *ctx, ULONG ms, ULONG flags)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (!handle_usable(h)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    struct tp_object *o = tp_obj_new(TPO_WAIT, CB_WAITORTIMER, (void *)cb, ctx, NULL);
    tp_lock();
    o->wt_flags = flags;
    o->long_fn = (flags & WT_EXECUTELONGFUNCTION) != 0;
    o->wait_h = h;
    o->wait_ms = ms;
    o->wait_deadline = ms == INFINITE ? 0 : mono_ns() + (uint64_t)ms * 1000000ull;
    if (!wait_attach_locked(o)) {
        o->closed = 1;
        tp_obj_maybe_free_locked(o);
        tp_unlock();
        lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    tp_unlock();
    return o;
}

API BOOL RegisterWaitForSingleObject(HANDLE *out, HANDLE h, WAITORTIMER_CB cb, void *ctx, ULONG ms, ULONG flags)
{
    if (!out) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct tp_object *o = register_wait(h, cb, ctx, ms, flags);
    *out = o;
    return o != NULL;
}

API HANDLE RegisterWaitForSingleObjectEx(HANDLE h, WAITORTIMER_CB cb, void *ctx, ULONG ms, ULONG flags)
{
    return register_wait(h, cb, ctx, ms, flags);
}

/* Shared by UnregisterWaitEx and DeleteTimerQueueTimer. `ev`:
 * INVALID_HANDLE_VALUE = block until callbacks are done; NULL = return at
 * once (FALSE + ERROR_IO_PENDING if callbacks are still running); an event
 * = signal it when the last callback has finished. Called locked. */
static BOOL legacy_release_locked(struct tp_object *o, HANDLE ev, struct countdown *cd)
{
    if (o->kind == TPO_WAIT) wait_detach_locked(o);
    else { timer_unlink_locked(o); o->timer_set = 0; }
    if (ev == INVALID_HANDLE_VALUE) {
        o->hold++;
        tp_wait_idle_locked(o);
        o->hold--;
        o->closed = 1;
        tp_obj_maybe_free_locked(o);
        return TRUE;
    }
    int busy = o->pending || o->running;
    if (cd) { o->done_cd = cd; cd->left++; }
    else if (ev) o->done_event = ev;
    o->closed = 1;
    tp_obj_maybe_free_locked(o);
    if (busy && !ev && !cd) { lsiw_set_last_error(ERROR_IO_PENDING); return FALSE; }
    return TRUE;
}

API BOOL UnregisterWaitEx(struct tp_object *o, HANDLE ev)
{
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    tp_lock();
    BOOL r = legacy_release_locked(o, ev, NULL);
    tp_unlock();
    return r;
}

API BOOL UnregisterWait(struct tp_object *o) { return UnregisterWaitEx(o, NULL); }

API HANDLE CreateTimerQueue(void) { return xmalloc0(sizeof(struct timer_queue)); }

API BOOL CreateTimerQueueTimer(HANDLE *out, struct timer_queue *q, WAITORTIMER_CB cb, void *param,
                               DWORD due, DWORD period, ULONG flags)
{
    if (!out || !cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct tp_object *o = tp_obj_new(TPO_TIMER, CB_WAITORTIMER, (void *)cb, param, NULL);
    tp_lock();
    o->wt_flags = flags;
    o->long_fn = (flags & WT_EXECUTELONGFUNCTION) != 0;
    if (q) { o->tq = q; o->qnext = q->timers; q->timers = o; }
    o->timer_set = 1;
    o->due = mono_ns() + (uint64_t)due * 1000000ull;
    o->period = (flags & WT_EXECUTEONLYONCE) ? 0 : (uint64_t)period * 1000000ull;
    timer_insert_locked(o);
    tp_unlock();
    *out = o;
    return TRUE;
}

API BOOL ChangeTimerQueueTimer(struct timer_queue *q, struct tp_object *o, ULONG due, ULONG period)
{
    (void)q;
    if (!o) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    tp_lock();
    /* A one-shot timer that has already expired is not re-armed. */
    if (o->closed || (!o->period && o->fired_once && !o->timer_armed)) { tp_unlock(); return TRUE; }
    timer_unlink_locked(o);
    o->timer_set = 1;
    o->due = mono_ns() + (uint64_t)due * 1000000ull;
    o->period = (o->wt_flags & WT_EXECUTEONLYONCE) ? 0 : (uint64_t)period * 1000000ull;
    timer_insert_locked(o);
    tp_unlock();
    return TRUE;
}

API BOOL DeleteTimerQueueTimer(struct timer_queue *q, struct tp_object *o, HANDLE ev)
{
    (void)q;
    if (!o) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    tp_lock();
    BOOL r = legacy_release_locked(o, ev, NULL);
    tp_unlock();
    return r;
}

API BOOL DeleteTimerQueueEx(struct timer_queue *q, HANDLE ev)
{
    if (!q) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    tp_lock();
    BOOL ok = TRUE;
    struct countdown *cd = NULL;
    if (ev && ev != INVALID_HANDLE_VALUE) { cd = xmalloc0(sizeof *cd); cd->event = ev; cd->left = 1; }
    while (q->timers) {
        struct tp_object *o = q->timers;
        tq_remove_locked(o);
        if (!legacy_release_locked(o, ev, cd)) ok = FALSE;
    }
    if (cd && --cd->left == 0) { signal_event(cd->event); free(cd); }
    tp_unlock();
    free(q);
    if (!ok) lsiw_set_last_error(ERROR_IO_PENDING);
    return ok;
}

API BOOL DeleteTimerQueue(struct timer_queue *q) { return DeleteTimerQueueEx(q, NULL); }

/* Pin / unpin a thread-pool object from outside the pool (a pending
 * asynchronous I/O keeps its TP_IO alive until it completes). */
static void tp_obj_hold(struct tp_object *o, int delta)
{
    tp_lock();
    o->hold += delta;
    tp_obj_maybe_free_locked(o);
    tp_unlock();
}

/* ================================================================== */
/* ReadFileEx / WriteFileEx                                             */
/* ================================================================== */

static int fd_seekable(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 && (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode));
}

/* The I/O itself is done synchronously; the completion routine is then
 * queued the way Windows does it (an APC for an alertable wait), or run
 * right away when alertable waits cannot deliver APCs. */
static void ex_completion(IO_COMPLETION_FN routine, DWORD bytes, OVERLAPPED *ov)
{
    ov->Internal = STATUS_SUCCESS;
    ov->InternalHigh = bytes;
    if (alertable_waits_deliver_apcs()) apc_queue(lsiw_thread()->tid, APC_IO, (void *)routine, 0, 0, bytes, ov);
    else routine(0, bytes, ov);
}

API BOOL ReadFileEx(HANDLE h, void *buf, DWORD n, OVERLAPPED *ov, IO_COMPLETION_FN routine)
{
    if (!ov || !routine) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    int seek = fd_seekable(fd);
    ssize_t r;
    off_t off = (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset);
    do r = seek ? pread(fd, buf, n, off) : read(fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) { lsiw_set_errno_error(errno); return FALSE; }
    /* End of file is reported synchronously, and nothing is queued. */
    if (r == 0 && n > 0) {
        lsiw_set_last_error(seek ? ERROR_HANDLE_EOF : ERROR_BROKEN_PIPE);
        return FALSE;
    }
    ex_completion(routine, (DWORD)r, ov);
    lsiw_set_last_error(0);
    return TRUE;
}

API BOOL WriteFileEx(HANDLE h, const void *buf, DWORD n, OVERLAPPED *ov, IO_COMPLETION_FN routine)
{
    if (!ov || !routine) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    int seek = fd_seekable(fd);
    int append = ov->Offset == 0xFFFFFFFF && ov->OffsetHigh == 0xFFFFFFFF;
    off_t off = (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset);
    size_t done = 0;
    while (done < n) {
        ssize_t r = seek && !append ? pwrite(fd, (const char *)buf + done, n - done, off + (off_t)done)
                                    : write(fd, (const char *)buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            lsiw_set_last_error(errno == EPIPE ? ERROR_NO_DATA : lsiw_errno_to_win(errno));
            return FALSE;
        }
        done += (size_t)r;
    }
    ex_completion(routine, (DWORD)done, ov);
    lsiw_set_last_error(0);
    return TRUE;
}

/* Synchronous I/O of another thread cannot be interrupted here; Windows
 * reports ERROR_NOT_FOUND when there is nothing it could cancel. */
API BOOL CancelSynchronousIo(HANDLE thread)
{
    if (thread != PSEUDO_THREAD && !lsiw_handle_get(thread, H_THREAD)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_set_last_error(ERROR_NOT_FOUND);
    return FALSE;
}

/* ================================================================== */
/* Named pipes                                                          */
/* ================================================================== */

/* A Windows named pipe \\.\pipe\NAME is a Unix domain socket under a
 * per-user directory ($XDG_RUNTIME_DIR/lsiw-pipes, else
 * /tmp/lsiw-pipes-<uid>, mode 0700), so pipes work between LSIW processes.
 * Message-type pipes use SOCK_SEQPACKET, which keeps message boundaries.
 *
 * Every server instance of one name shares the listening socket; an
 * instance handle is an H_FILE whose descriptor number stays fixed: an
 * unconnected socket until ConnectNamedPipe accepts a client, at which
 * point the accepted connection is dup3()'d onto it, so ReadFile /
 * WriteFile / CloseHandle in k32_file.c work on it unchanged. */

#define PIPE_ACCESS_DUPLEX             3
#define FILE_FLAG_FIRST_PIPE_INSTANCE  0x00080000
#define FILE_FLAG_OVERLAPPED           0x40000000
#define PIPE_NOWAIT                    0x1
#define PIPE_READMODE_MESSAGE          0x2
#define PIPE_TYPE_MESSAGE              0x4
#define PIPE_UNLIMITED_INSTANCES       255
#define PIPE_SERVER_END                1
#define NMPWAIT_WAIT_FOREVER           0xFFFFFFFFu
#define NMPWAIT_USE_DEFAULT_WAIT       0

enum { PIPE_LISTENING, PIPE_CONNECTED };

struct pipe_name {
    struct pipe_name *next;
    char *path;
    int listen_fd, type, message;
    int instances, max_instances, refs;
    int listening;                   /* instances waiting for a client */
    DWORD out_size, in_size, timeout;
};

struct pipe_inst {
    struct pipe_name *name;          /* server end; NULL for a client end */
    int refs, dead, state, accepting, gen, overlapped, message;
    HANDLE h;
    struct lsiw_handle_obj *obj;
    int fd;
};

static struct pipe_name *g_pipes;
static pthread_mutex_t g_pipe_lock = PTHREAD_MUTEX_INITIALIZER;

static char g_pipe_dir[96];

static void pipe_dir_init(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt && strlen(rt) < 60) snprintf(g_pipe_dir, sizeof g_pipe_dir, "%s/lsiw-pipes", rt);
    else snprintf(g_pipe_dir, sizeof g_pipe_dir, "/tmp/lsiw-pipes-%u", (unsigned)getuid());
    mkdir(g_pipe_dir, 0700);
}

static const char *pipe_dir(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, pipe_dir_init);
    return g_pipe_dir;
}

/* Split a Windows pipe path. Returns the pipe name, NULL if `win` is not a
 * pipe path; *remote is set for \\server\pipe\... on another machine. */
static const char *pipe_parse(const char *win, int *remote)
{
    *remote = 0;
    if (!win || !(win[0] == '\\' || win[0] == '/') || !(win[1] == '\\' || win[1] == '/')) return NULL;
    const char *srv = win + 2, *e = srv;
    while (*e && *e != '\\' && *e != '/') e++;
    if (!*e || strncasecmp(e + 1, "pipe", 4) || (e[5] != '\\' && e[5] != '/')) return NULL;
    size_t sl = (size_t)(e - srv);
    if (!((sl == 1 && *srv == '.') || (sl == 9 && !strncasecmp(srv, "localhost", 9)))) *remote = 1;
    return e + 6;
}

/* Socket path for a pipe name: case-insensitive like Windows, readable
 * prefix plus a hash so any name fits sockaddr_un. */
static char *pipe_sock_path(const char *name)
{
    const char *dir = pipe_dir();
    uint64_t hsh = 1469598103934665603ull;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)tolower((unsigned char)*p);
        if (c == '/') c = '\\';
        hsh = (hsh ^ c) * 1099511628211ull;
    }
    size_t room = sizeof(((struct sockaddr_un *)0)->sun_path) - strlen(dir) - 20;
    if (room > 40) room = 40;
    char pre[48];
    size_t i = 0;
    for (const char *p = name; *p && i < room; p++)
        pre[i++] = isalnum((unsigned char)*p) || *p == '.' || *p == '-' ? (char)tolower((unsigned char)*p) : '_';
    pre[i] = 0;
    char *out;
    if (asprintf(&out, "%s/%s-%016llx", dir, pre, (unsigned long long)hsh) < 0) abort();
    return out;
}

static int sock_addr(const char *path, struct sockaddr_un *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa->sun_path) return -1;
    strcpy(sa->sun_path, path);
    return 0;
}

/* Is some process listening on this socket path? (/proc/net/unix lists
 * bound sockets; flag 0x10000 marks a listening one.) Avoids a probing
 * connect that another server would see as a client. */
static int unix_listening(const char *path)
{
    size_t len;
    char *t = (char *)read_file("/proc/net/unix", &len);
    if (!t) return 0;
    int found = 0;
    for (char *line = strtok(t, "\n"); line && !found; line = strtok(NULL, "\n")) {
        unsigned long flags;
        char p[256] = "";
        if (sscanf(line, "%*s %*s %*s %lx %*s %*s %*s %255s", &flags, p) == 2 && !strcmp(p, path) && (flags & 0x10000))
            found = 1;
    }
    free(t);
    return found;
}

static struct pipe_name *pipe_name_find_locked(const char *path)
{
    for (struct pipe_name *n = g_pipes; n; n = n->next)
        if (!strcmp(n->path, path)) return n;
    return NULL;
}

static void pipe_name_unref_locked(struct pipe_name *n)
{
    if (--n->refs > 0) return;
    for (struct pipe_name **pp = &g_pipes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    close(n->listen_fd);
    unlink(n->path);
    free(n->path);
    free(n);
}

static void pipe_inst_unref_locked(struct pipe_inst *pi)
{
    if (--pi->refs > 0) return;
    if (pi->name) pipe_name_unref_locked(pi->name);
    free(pi);
}

/* The handle of a pipe end was closed (called from fext_free). */
static void pipe_inst_detach(struct pipe_inst *pi)
{
    pthread_mutex_lock(&g_pipe_lock);
    pi->dead = 1;
    if (pi->name) {
        pi->name->instances--;
        if (pi->state == PIPE_LISTENING) pi->name->listening--;
    }
    pipe_inst_unref_locked(pi);
    pthread_mutex_unlock(&g_pipe_lock);
}

static void set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
}

/* Pipe record of a handle: pipe instance plus the per-handle modes. */
struct pipe_view { struct pipe_inst *pi; int fd, readmode_msg, nowait, is_fifo, is_sock; };

static int pipe_view(HANDLE h, struct pipe_view *v)
{
    memset(v, 0, sizeof *v);
    v->fd = lsiw_fd_from_handle(h);
    if (v->fd < 0) return 0;
    struct stat st;
    if (fstat(v->fd, &st) != 0) return 0;
    v->is_fifo = S_ISFIFO(st.st_mode);
    v->is_sock = S_ISSOCK(st.st_mode);
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(h, 0);
    if (f) { v->pi = f->pipe; v->readmode_msg = f->readmode_msg; v->nowait = f->nowait; }
    pthread_mutex_unlock(&g_fext_lock);
    return v->is_fifo || v->is_sock;
}

static HANDLE pipe_attach(int fd, struct pipe_inst *pi, int readmode_msg, int nowait)
{
    HANDLE h = lsiw_handle_from_fd(fd, 0);
    if (!h) return NULL;
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(h, 1);
    f->pipe = pi;
    f->readmode_msg = readmode_msg;
    f->nowait = nowait;
    pthread_mutex_unlock(&g_fext_lock);
    pthread_mutex_lock(&g_pipe_lock);
    pi->h = h;
    pi->obj = lsiw_handle_get(h, H_FILE);
    pi->fd = fd;
    pthread_mutex_unlock(&g_pipe_lock);
    return h;
}

static HANDLE pipe_create(const char *win, DWORD open_mode, DWORD pipe_mode, DWORD max_inst,
                          DWORD out_size, DWORD in_size, DWORD timeout)
{
    int remote;
    const char *name = pipe_parse(win, &remote);
    if (!name || !*name || remote) { lsiw_set_last_error(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    if (!(open_mode & PIPE_ACCESS_DUPLEX) || !max_inst || max_inst > PIPE_UNLIMITED_INSTANCES ||
        ((pipe_mode & PIPE_READMODE_MESSAGE) && !(pipe_mode & PIPE_TYPE_MESSAGE))) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    int message = (pipe_mode & PIPE_TYPE_MESSAGE) != 0;
    int type = message ? SOCK_SEQPACKET : SOCK_STREAM;
    char *path = pipe_sock_path(name);
    pthread_mutex_lock(&g_pipe_lock);
    struct pipe_name *pn = pipe_name_find_locked(path);
    DWORD err = 0;
    if (pn) {
        if ((open_mode & FILE_FLAG_FIRST_PIPE_INSTANCE) || pn->message != message) err = ERROR_ACCESS_DENIED;
        else if (pn->max_instances != PIPE_UNLIMITED_INSTANCES && pn->instances >= pn->max_instances) err = ERROR_PIPE_BUSY;
        free(path);
    } else {
        struct sockaddr_un sa;
        int lfd = -1;
        if (sock_addr(path, &sa)) err = ERROR_FILENAME_EXCED_RANGE;
        /* Instances served by another process cannot share our listener. */
        else if (unix_listening(path)) err = ERROR_ACCESS_DENIED;
        else {
            unlink(path);
            lfd = socket(AF_UNIX, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof sa) ||
                listen(lfd, max_inst == PIPE_UNLIMITED_INSTANCES ? 128 : (int)max_inst)) {
                err = lsiw_errno_to_win(errno);
                if (lfd >= 0) close(lfd);
            }
        }
        if (err) free(path);
        else {
            pn = xmalloc0(sizeof *pn);
            pn->path = path;
            pn->listen_fd = lfd;
            pn->type = type;
            pn->message = message;
            pn->max_instances = (int)max_inst;
            pn->out_size = out_size;
            pn->in_size = in_size;
            pn->timeout = timeout ? timeout : 50;
            pn->next = g_pipes;
            g_pipes = pn;
        }
    }
    int fd = err ? -1 : socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (!err && fd < 0) err = lsiw_errno_to_win(errno);
    struct pipe_inst *pi = NULL;
    if (!err) {
        pn->instances++;
        pn->listening++;
        pn->refs++;
        pi = xmalloc0(sizeof *pi);
        pi->name = pn;
        pi->refs = 1;
        pi->state = PIPE_LISTENING;
        pi->overlapped = (open_mode & FILE_FLAG_OVERLAPPED) != 0;
        pi->message = message;
    }
    pthread_mutex_unlock(&g_pipe_lock);
    if (err) { lsiw_set_last_error(err); return INVALID_HANDLE_VALUE; }
    HANDLE h = pipe_attach(fd, pi, (pipe_mode & PIPE_READMODE_MESSAGE) != 0, (pipe_mode & PIPE_NOWAIT) != 0);
    if (!h) {
        close(fd);
        pthread_mutex_lock(&g_pipe_lock);
        pn->instances--;
        pn->listening--;
        pipe_inst_unref_locked(pi);
        pthread_mutex_unlock(&g_pipe_lock);
        return INVALID_HANDLE_VALUE;
    }
    lsiw_set_last_error(0);
    return h;
}

API HANDLE CreateNamedPipeW(const WCHAR *name, DWORD open_mode, DWORD pipe_mode, DWORD max_inst,
                            DWORD out_size, DWORD in_size, DWORD timeout, void *sa)
{
    (void)sa;
    char *u = lsiw_w2u(name);
    HANDLE h = pipe_create(u, open_mode, pipe_mode, max_inst, out_size, in_size, timeout);
    free(u);
    return h;
}

API HANDLE CreateNamedPipeA(const char *name, DWORD open_mode, DWORD pipe_mode, DWORD max_inst,
                            DWORD out_size, DWORD in_size, DWORD timeout, void *sa)
{
    WCHAR *w = a2w(name, -1);
    HANDLE h = CreateNamedPipeW(w, open_mode, pipe_mode, max_inst, out_size, in_size, timeout, sa);
    free(w);
    return h;
}

/* Accept one pending client onto the instance. Called with g_pipe_lock
 * held; returns 1 connected, 0 nobody waiting, -1 error (errno). */
static int pipe_try_accept_locked(struct pipe_inst *pi)
{
    int c = accept4(pi->name->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    /* The handle must still be ours: CloseHandle removes it from the table
     * before the descriptor is closed. */
    if (pi->dead || lsiw_handle_get(pi->h, H_FILE) != pi->obj) { close(c); return 0; }
    dup3(c, pi->fd, O_CLOEXEC);
    close(c);
    pi->state = PIPE_CONNECTED;
    pi->name->listening--;
    return 1;
}

struct accept_job { struct pipe_inst *pi; OVERLAPPED *ov; struct io_target t; int gen; };

static void *pipe_accept_thread(void *arg)
{
    struct accept_job *j = arg;
    struct pipe_inst *pi = j->pi;
    uint32_t st;
    pthread_mutex_lock(&g_pipe_lock);
    for (;;) {
        if (pi->dead) { st = STATUS_CANCELLED; break; }
        if (pi->gen != j->gen) { st = STATUS_PIPE_DISCONNECTED; break; }
        int r = pipe_try_accept_locked(pi);
        if (r > 0) { st = STATUS_SUCCESS; break; }
        if (r < 0) { st = lsiw_errno_to_ntstatus(errno); break; }
        struct pollfd p = { pi->name->listen_fd, POLLIN, 0 };
        pthread_mutex_unlock(&g_pipe_lock);
        poll(&p, 1, 200);
        pthread_mutex_lock(&g_pipe_lock);
    }
    pi->accepting = 0;
    pipe_inst_unref_locked(pi);
    pthread_mutex_unlock(&g_pipe_lock);
    io_target_complete(&j->t, j->ov, st, 0);
    free(j);
    return NULL;
}

static int peer_closed(int fd)
{
    struct pollfd p = { fd, POLLRDHUP, 0 };
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLRDHUP));
}

API BOOL ConnectNamedPipe(HANDLE h, OVERLAPPED *ov)
{
    struct pipe_view v;
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!v.pi || !v.pi->name) { lsiw_set_last_error(ERROR_INVALID_FUNCTION); return FALSE; }
    struct pipe_inst *pi = v.pi;
    pthread_mutex_lock(&g_pipe_lock);
    DWORD err = 0;
    int r = 0;
    if (pi->state == PIPE_CONNECTED)
        err = peer_closed(pi->fd) ? ERROR_NO_DATA : ERROR_PIPE_CONNECTED;
    else if (pi->accepting) err = ERROR_PIPE_LISTENING;
    else if ((r = pipe_try_accept_locked(pi)) > 0) {
        /* A client connected before the call: FALSE, ERROR_PIPE_CONNECTED,
         * and the connection is good. */
        if (v.nowait) set_nonblock(pi->fd, 1);
        err = ERROR_PIPE_CONNECTED;
    } else if (r < 0) err = lsiw_errno_to_win(errno);
    if (err) { pthread_mutex_unlock(&g_pipe_lock); lsiw_set_last_error(err); return FALSE; }
    if (v.nowait) { pthread_mutex_unlock(&g_pipe_lock); lsiw_set_last_error(ERROR_PIPE_LISTENING); return FALSE; }
    if (ov && pi->overlapped) {
        struct accept_job *j = xmalloc0(sizeof *j);
        j->pi = pi;
        j->ov = ov;
        j->gen = pi->gen;
        pi->refs++;
        pi->accepting = 1;
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        pthread_mutex_unlock(&g_pipe_lock);
        io_target_capture(h, &j->t, NULL);
        if (!start_host_thread(pipe_accept_thread, j)) {
            pthread_mutex_lock(&g_pipe_lock);
            pi->accepting = 0;
            pipe_inst_unref_locked(pi);
            pthread_mutex_unlock(&g_pipe_lock);
            io_target_release(&j->t);
            free(j);
            lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        lsiw_set_last_error(ERROR_IO_PENDING);
        return FALSE;
    }
    /* Synchronous: block until a client arrives. */
    int gen = pi->gen;
    for (;;) {
        if (pi->dead) { err = ERROR_OPERATION_ABORTED; break; }
        if (pi->gen != gen) { err = ERROR_PIPE_NOT_CONNECTED; break; }
        r = pipe_try_accept_locked(pi);
        if (r > 0) break;
        if (r < 0) { err = lsiw_errno_to_win(errno); break; }
        struct pollfd p = { pi->name->listen_fd, POLLIN, 0 };
        pthread_mutex_unlock(&g_pipe_lock);
        poll(&p, 1, 200);
        pthread_mutex_lock(&g_pipe_lock);
    }
    pthread_mutex_unlock(&g_pipe_lock);
    if (err) { lsiw_set_last_error(err); return FALSE; }
    if (ov) complete_sync(h, ov, STATUS_SUCCESS, 0);
    return TRUE;
}

API BOOL DisconnectNamedPipe(HANDLE h)
{
    struct pipe_view v;
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!v.pi || !v.pi->name) { lsiw_set_last_error(ERROR_INVALID_FUNCTION); return FALSE; }
    struct pipe_inst *pi = v.pi;
    pthread_mutex_lock(&g_pipe_lock);
    int ns = socket(AF_UNIX, pi->name->type | SOCK_CLOEXEC, 0);
    if (ns < 0) { pthread_mutex_unlock(&g_pipe_lock); lsiw_set_errno_error(errno); return FALSE; }
    /* Unread data is discarded; the client's next read sees the pipe end. */
    dup3(ns, pi->fd, O_CLOEXEC);
    close(ns);
    if (pi->state != PIPE_LISTENING) pi->name->listening++;
    pi->state = PIPE_LISTENING;
    pi->gen++;
    pthread_mutex_unlock(&g_pipe_lock);
    return TRUE;
}

/* Connect a client end. Returns the descriptor or -1 with the error set. */
static int pipe_connect(const char *win, int *message)
{
    int remote;
    const char *name = pipe_parse(win, &remote);
    if (!name) { lsiw_set_last_error(ERROR_INVALID_NAME); return -1; }
    if (remote) { lsiw_set_last_error(ERROR_BAD_NETPATH); return -1; }
    char *path = pipe_sock_path(name);
    struct sockaddr_un sa;
    if (sock_addr(path, &sa)) { free(path); lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return -1; }
    /* Every instance served here is busy: Windows says ERROR_PIPE_BUSY. */
    pthread_mutex_lock(&g_pipe_lock);
    struct pipe_name *pn = pipe_name_find_locked(path);
    int busy = pn && pn->listening <= 0;
    pthread_mutex_unlock(&g_pipe_lock);
    free(path);
    if (busy) { lsiw_set_last_error(ERROR_PIPE_BUSY); return -1; }
    int types[2] = { SOCK_STREAM, SOCK_SEQPACKET };
    for (int i = 0; i < 2; i++) {
        int fd = socket(AF_UNIX, types[i] | SOCK_CLOEXEC, 0);
        if (fd < 0) { lsiw_set_errno_error(errno); return -1; }
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) { *message = i == 1; return fd; }
        int e = errno;
        close(fd);
        if (e == EPROTOTYPE) continue;
        lsiw_set_last_error(e == ENOENT || e == ECONNREFUSED ? ERROR_FILE_NOT_FOUND :
                            e == EAGAIN ? ERROR_PIPE_BUSY : lsiw_errno_to_win(e));
        return -1;
    }
    lsiw_set_last_error(ERROR_FILE_NOT_FOUND);
    return -1;
}

/* Hook for CreateFileA/W: returns 0 if `win` is not a pipe path, otherwise
 * 1 with *out = the client handle (or INVALID_HANDLE_VALUE, error set). */
int lsiw_named_pipe_open(const char *win, DWORD access, DWORD flags, HANDLE *out)
{
    (void)access;
    int remote;
    if (!pipe_parse(win, &remote)) return 0;
    *out = INVALID_HANDLE_VALUE;
    int message = 0;
    int fd = pipe_connect(win, &message);
    if (fd < 0) return 1;
    struct pipe_inst *pi = xmalloc0(sizeof *pi);
    pi->refs = 1;
    pi->state = PIPE_CONNECTED;
    pi->message = message;
    pi->overlapped = (flags & FILE_FLAG_OVERLAPPED) != 0;
    HANDLE h = pipe_attach(fd, pi, 0, 0);
    if (!h) { close(fd); free(pi); return 1; }
    lsiw_set_last_error(0);
    *out = h;
    return 1;
}

static int pipe_wait(const char *win, DWORD timeout)
{
    int remote;
    const char *name = pipe_parse(win, &remote);
    if (!name || !*name) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
    if (remote) { lsiw_set_last_error(ERROR_BAD_NETPATH); return 0; }
    char *path = pipe_sock_path(name);
    uint64_t start = mono_ns();
    int ok = 0;
    for (;;) {
        pthread_mutex_lock(&g_pipe_lock);
        struct pipe_name *pn = pipe_name_find_locked(path);
        int exists = pn != NULL, free_inst = pn && pn->listening > 0;
        DWORD def = pn ? pn->timeout : 50;
        pthread_mutex_unlock(&g_pipe_lock);
        if (!exists && unix_listening(path)) exists = free_inst = 1;   /* served by another process */
        if (free_inst) { ok = 1; break; }
        /* No instance at all: fail at once, whatever the time-out. */
        if (!exists) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); break; }
        DWORD ms = timeout == NMPWAIT_USE_DEFAULT_WAIT ? def : timeout;
        if (ms != NMPWAIT_WAIT_FOREVER && mono_ns() - start >= (uint64_t)ms * 1000000ull) {
            lsiw_set_last_error(ERROR_SEM_TIMEOUT);
            break;
        }
        usleep(5000);
    }
    free(path);
    return ok;
}

API BOOL WaitNamedPipeW(const WCHAR *name, DWORD timeout)
{
    char *u = lsiw_w2u(name);
    int r = pipe_wait(u, timeout);
    free(u);
    return r;
}

API BOOL WaitNamedPipeA(const char *name, DWORD timeout)
{
    WCHAR *w = a2w(name, -1);
    BOOL r = WaitNamedPipeW(w, timeout);
    free(w);
    return r;
}

static int write_all(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = send(fd, (const char *)buf + done, n - done, MSG_NOSIGNAL);
        if (r < 0 && errno == ENOTSOCK) r = write(fd, (const char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)r;
    }
    return 0;
}

/* Read one message (or one chunk of a byte stream). Returns the bytes
 * stored; *more is set when a message did not fit (ERROR_MORE_DATA). */
static ssize_t read_message(int fd, int message, void *buf, size_t size, int *more)
{
    *more = 0;
    ssize_t r;
    do r = message ? recv(fd, buf, size, MSG_TRUNC) : read(fd, buf, size); while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (message && (size_t)r > size) { *more = 1; return (ssize_t)size; }
    return r;
}

static DWORD pipe_io_error(int e)
{
    return e == EPIPE || e == ECONNRESET ? ERROR_BROKEN_PIPE : e == ENOTCONN ? ERROR_PIPE_NOT_CONNECTED : lsiw_errno_to_win(e);
}

API BOOL TransactNamedPipe(HANDLE h, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *nread, OVERLAPPED *ov)
{
    struct pipe_view v;
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!v.pi || !v.readmode_msg) { lsiw_set_last_error(ERROR_BAD_PIPE); return FALSE; }
    if (nread) *nread = 0;
    if (write_all(v.fd, in, in_size) < 0) { lsiw_set_last_error(pipe_io_error(errno)); return FALSE; }
    int more;
    ssize_t r = read_message(v.fd, v.pi->message, out, out_size, &more);
    if (r < 0) { lsiw_set_last_error(pipe_io_error(errno)); return FALSE; }
    if (r == 0 && out_size) { lsiw_set_last_error(ERROR_BROKEN_PIPE); return FALSE; }
    if (nread) *nread = (DWORD)r;
    complete_sync(h, ov, more ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, (DWORD)r);
    if (more) { lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
    return TRUE;
}

static BOOL call_pipe(const char *win, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *nread, DWORD timeout)
{
    if (nread) *nread = 0;
    if (!pipe_wait(win, timeout)) return FALSE;
    int message = 0;
    int fd = pipe_connect(win, &message);
    if (fd < 0) return FALSE;
    BOOL ok = FALSE;
    int more = 0;
    if (write_all(fd, in, in_size) < 0) lsiw_set_last_error(pipe_io_error(errno));
    else {
        ssize_t r = read_message(fd, message, out, out_size, &more);
        if (r < 0) lsiw_set_last_error(pipe_io_error(errno));
        else {
            if (nread) *nread = (DWORD)r;
            if (more) lsiw_set_last_error(ERROR_MORE_DATA); else ok = TRUE;
        }
    }
    close(fd);
    return ok;
}

API BOOL CallNamedPipeW(const WCHAR *name, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *nread, DWORD timeout)
{
    char *u = lsiw_w2u(name);
    BOOL r = call_pipe(u, in, in_size, out, out_size, nread, timeout);
    free(u);
    return r;
}

API BOOL CallNamedPipeA(const char *name, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *nread, DWORD timeout)
{
    WCHAR *w = a2w(name, -1);
    BOOL r = CallNamedPipeW(w, in, in_size, out, out_size, nread, timeout);
    free(w);
    return r;
}

/* PeekNamedPipe works on named pipes and on CreatePipe's anonymous pipes
 * (Linux pipes: tee() copies the queued bytes without consuming them). */
API BOOL PeekNamedPipe(HANDLE h, void *buf, DWORD size, DWORD *nread, DWORD *avail, DWORD *left)
{
    struct pipe_view v;
    if (lsiw_fd_from_handle(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_FUNCTION); return FALSE; }
    /* The write end of an anonymous pipe has no read access: Windows fails
     * the peek, and callers (gnulib's poll) use that to tell the ends apart. */
    if (v.is_fifo && (fcntl(v.fd, F_GETFL) & O_ACCMODE) == O_WRONLY) {
        lsiw_set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    int queued = 0;
    if (ioctl(v.fd, FIONREAD, &queued) < 0) queued = 0;
    DWORD copied = 0, msg_left = 0;
    if (queued == 0) {
        struct pollfd p = { v.fd, POLLIN | POLLRDHUP, 0 };
        int eof = 0;
        if (poll(&p, 1, 0) > 0) {
            if (v.is_fifo) eof = (p.revents & POLLHUP) != 0;
            else if (p.revents & (POLLIN | POLLHUP | POLLRDHUP)) {
                char c;
                eof = recv(v.fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
            }
        }
        if (eof) { lsiw_set_last_error(ERROR_BROKEN_PIPE); return FALSE; }
        if (v.pi && v.pi->name && v.pi->state != PIPE_CONNECTED) { lsiw_set_last_error(ERROR_PIPE_LISTENING); return FALSE; }
    } else if (buf && size) {
        size_t want = (size_t)queued < size ? (size_t)queued : size;
        if (v.is_fifo) {
            int tp[2];
            if (pipe2(tp, O_NONBLOCK | O_CLOEXEC) == 0) {
                ssize_t t = tee(v.fd, tp[1], want, SPLICE_F_NONBLOCK);
                if (t > 0) { ssize_t r = read(tp[0], buf, (size_t)t); if (r > 0) copied = (DWORD)r; }
                close(tp[0]);
                close(tp[1]);
            }
        } else {
            int message = v.pi && v.pi->message;
            ssize_t r = recv(v.fd, buf, want, MSG_PEEK | MSG_DONTWAIT | (message ? MSG_TRUNC : 0));
            if (r > 0) {
                copied = (DWORD)((size_t)r < want ? (size_t)r : want);
                if (message && v.readmode_msg) msg_left = (DWORD)r - copied;
            }
        }
    } else if (v.pi && v.pi->message && v.readmode_msg) {
        char c;
        ssize_t r = recv(v.fd, &c, 1, MSG_PEEK | MSG_DONTWAIT | MSG_TRUNC);
        if (r > 0) msg_left = (DWORD)r;
    }
    if (nread) *nread = copied;
    if (avail) *avail = (DWORD)queued;
    if (left) *left = msg_left;
    return TRUE;
}

static BOOL pipe_handle_state(HANDLE h, DWORD *state, DWORD *cur, DWORD *maxcoll, DWORD *timeout, char **user)
{
    struct pipe_view v;
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (state) *state = (v.nowait ? PIPE_NOWAIT : 0) | (v.readmode_msg ? PIPE_READMODE_MESSAGE : 0);
    if (cur) {
        pthread_mutex_lock(&g_pipe_lock);
        *cur = v.pi && v.pi->name ? (DWORD)v.pi->name->instances : 1;
        pthread_mutex_unlock(&g_pipe_lock);
    }
    if (maxcoll) *maxcoll = 0;
    if (timeout) *timeout = 0;
    if (user) *user = xstrdup(lsiw_user_name());
    return TRUE;
}

API BOOL GetNamedPipeHandleStateW(HANDLE h, DWORD *state, DWORD *cur, DWORD *maxcoll, DWORD *timeout, WCHAR *user, DWORD usersize)
{
    char *u = NULL;
    if (!pipe_handle_state(h, state, cur, maxcoll, timeout, user ? &u : NULL)) return FALSE;
    if (user) {
        int need = lsiw_utf8_to_utf16(u, -1, NULL, 0);
        if ((int)usersize < need) { free(u); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        lsiw_utf8_to_utf16(u, -1, user, (int)usersize);
        free(u);
    }
    return TRUE;
}

API BOOL GetNamedPipeHandleStateA(HANDLE h, DWORD *state, DWORD *cur, DWORD *maxcoll, DWORD *timeout, char *user, DWORD usersize)
{
    char *u = NULL;
    if (!pipe_handle_state(h, state, cur, maxcoll, timeout, user ? &u : NULL)) return FALSE;
    if (user) {
        if (strlen(u) + 1 > usersize) { free(u); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        strcpy(user, u);
        free(u);
    }
    return TRUE;
}

API BOOL SetNamedPipeHandleState(HANDLE h, DWORD *mode, DWORD *maxcoll, DWORD *timeout)
{
    (void)maxcoll; (void)timeout;
    struct pipe_view v;
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!mode) return TRUE;
    if (*mode & ~(DWORD)(PIPE_NOWAIT | PIPE_READMODE_MESSAGE)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    /* Message read mode needs a message-type pipe. */
    if ((*mode & PIPE_READMODE_MESSAGE) && !(v.pi && v.pi->message)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(h, 1);
    if (f) { f->readmode_msg = (*mode & PIPE_READMODE_MESSAGE) != 0; f->nowait = (*mode & PIPE_NOWAIT) != 0; }
    pthread_mutex_unlock(&g_fext_lock);
    set_nonblock(v.fd, (*mode & PIPE_NOWAIT) != 0);
    return TRUE;
}

API BOOL GetNamedPipeInfo(HANDLE h, DWORD *flags, DWORD *out_size, DWORD *in_size, DWORD *max_inst)
{
    struct pipe_view v;
    if (!pipe_view(h, &v)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    DWORD fl = 0, os = 4096, is = 4096, mi = 1;
    if (v.is_fifo) {
        /* Anonymous pipes: the read end is the server end. */
        if ((fcntl(v.fd, F_GETFL) & O_ACCMODE) == O_RDONLY) fl = PIPE_SERVER_END;
        int sz = fcntl(v.fd, F_GETPIPE_SZ);
        if (sz > 0) os = is = (DWORD)sz;
    } else if (v.pi) {
        pthread_mutex_lock(&g_pipe_lock);
        if (v.pi->name) {
            fl = PIPE_SERVER_END;
            os = v.pi->name->out_size;
            is = v.pi->name->in_size;
            mi = (DWORD)v.pi->name->max_instances;
        }
        if (v.pi->message) fl |= PIPE_TYPE_MESSAGE;
        pthread_mutex_unlock(&g_pipe_lock);
    }
    if (flags) *flags = fl;
    if (out_size) *out_size = os;
    if (in_size) *in_size = is;
    if (max_inst) *max_inst = mi;
    return TRUE;
}

static BOOL pipe_peer_pid(HANDLE h, int want_server_end, ULONG *pid)
{
    struct pipe_view v;
    if (!pipe_view(h, &v) || !v.is_sock || !v.pi) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if ((v.pi->name != NULL) != want_server_end) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct ucred cr;
    socklen_t l = sizeof cr;
    if (getsockopt(v.fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) || cr.pid <= 0) {
        lsiw_set_last_error(ERROR_PIPE_NOT_CONNECTED);
        return FALSE;
    }
    if (pid) *pid = (ULONG)cr.pid;
    return TRUE;
}

API BOOL GetNamedPipeClientProcessId(HANDLE h, ULONG *pid) { return pipe_peer_pid(h, 1, pid); }
API BOOL GetNamedPipeServerProcessId(HANDLE h, ULONG *pid) { return pipe_peer_pid(h, 0, pid); }
API BOOL GetNamedPipeClientSessionId(HANDLE h, ULONG *s) { ULONG p; if (!pipe_peer_pid(h, 1, &p)) return FALSE; if (s) *s = 1; return TRUE; }
API BOOL GetNamedPipeServerSessionId(HANDLE h, ULONG *s) { ULONG p; if (!pipe_peer_pid(h, 0, &p)) return FALSE; if (s) *s = 1; return TRUE; }

/* ================================================================== */
/* ReadDirectoryChangesW (inotify)                                      */
/* ================================================================== */

#define FILE_NOTIFY_CHANGE_FILE_NAME   0x001
#define FILE_NOTIFY_CHANGE_DIR_NAME    0x002
#define FILE_NOTIFY_CHANGE_ATTRIBUTES  0x004
#define FILE_NOTIFY_CHANGE_SIZE        0x008
#define FILE_NOTIFY_CHANGE_LAST_WRITE  0x010
#define FILE_NOTIFY_CHANGE_LAST_ACCESS 0x020
#define FILE_NOTIFY_CHANGE_CREATION    0x040
#define FILE_NOTIFY_CHANGE_SECURITY    0x100
#define FILE_ACTION_ADDED              1
#define FILE_ACTION_REMOVED            2
#define FILE_ACTION_MODIFIED           3
#define FILE_ACTION_RENAMED_OLD_NAME   4
#define FILE_ACTION_RENAMED_NEW_NAME   5

struct dw_rec { struct dw_rec *next; DWORD action; char *rel; };
struct dw_watch { int wd; char *rel; };

struct dirwatch {
    int refs, ifd, subtree, closing, pending, overflow, gone, root_wd;
    DWORD filter;
    char *root;
    struct dw_watch *w;
    int nw, cap;
    struct dw_rec *head, *tail;
};
static pthread_mutex_t g_dw_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t dw_mask(DWORD filter, int subtree)
{
    uint32_t m = IN_ONLYDIR;
    if (filter & (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME)) m |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    if (filter & (FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION)) m |= IN_ATTRIB;
    if (filter & (FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE)) m |= IN_MODIFY;
    if (filter & FILE_NOTIFY_CHANGE_LAST_ACCESS) m |= IN_ACCESS;
    if (subtree) m |= IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM;
    return m;
}

static const char *dw_rel_of(struct dirwatch *dw, int wd)
{
    for (int i = 0; i < dw->nw; i++) if (dw->w[i].wd == wd) return dw->w[i].rel;
    return NULL;
}

/* Watch `rel` (relative to the root, "" = root) and, for a subtree watch,
 * every folder below it. */
static void dw_add_tree(struct dirwatch *dw, const char *rel, int depth)
{
    if (dw->nw >= 8192 || depth > 64) return;
    char *path;
    if (asprintf(&path, "%s%s%s", dw->root, *rel ? "/" : "", rel) < 0) abort();
    int wd = inotify_add_watch(dw->ifd, path, dw_mask(dw->filter, dw->subtree));
    if (wd >= 0 && !dw_rel_of(dw, wd)) {
        if (dw->nw == dw->cap) {
            dw->cap = dw->cap ? dw->cap * 2 : 16;
            dw->w = realloc(dw->w, (size_t)dw->cap * sizeof *dw->w);
        }
        dw->w[dw->nw].wd = wd;
        dw->w[dw->nw++].rel = xstrdup(rel);
        if (!*rel) dw->root_wd = wd;
    }
    if (dw->subtree && wd >= 0) {
        DIR *d = opendir(path);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            int isdir = de->d_type == DT_DIR;
            if (de->d_type == DT_UNKNOWN) {
                struct stat st;
                char *p;
                if (asprintf(&p, "%s/%s", path, de->d_name) < 0) abort();
                isdir = lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
                free(p);
            }
            if (!isdir) continue;
            char *sub;
            if (asprintf(&sub, "%s%s%s", rel, *rel ? "/" : "", de->d_name) < 0) abort();
            dw_add_tree(dw, sub, depth + 1);
            free(sub);
        }
        if (d) closedir(d);
    }
    free(path);
}

static void dw_push(struct dirwatch *dw, DWORD action, const char *rel)
{
    /* Coalesce repeated "modified" reports for the same file. */
    if (action == FILE_ACTION_MODIFIED && dw->tail && dw->tail->action == action && !strcmp(dw->tail->rel, rel)) return;
    struct dw_rec *r = xmalloc0(sizeof *r);
    r->action = action;
    r->rel = xstrdup(rel);
    if (dw->tail) dw->tail->next = r; else dw->head = r;
    dw->tail = r;
}

static void dw_rename_prefix(struct dirwatch *dw, const char *from, const char *to)
{
    size_t fl = strlen(from);
    for (int i = 0; i < dw->nw; i++) {
        char *r = dw->w[i].rel;
        if (!strncmp(r, from, fl) && (r[fl] == 0 || r[fl] == '/')) {
            char *n;
            if (asprintf(&n, "%s%s", to, r + fl) < 0) abort();
            free(r);
            dw->w[i].rel = n;
        }
    }
}

/* Translate queued inotify events into Windows actions. Call with g_dw_lock. */
static void dw_pump(struct dirwatch *dw)
{
    char buf[16384] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(dw->ifd, buf, sizeof buf);
        if (n <= 0) return;
        for (ssize_t off = 0; off < n;) {
            struct inotify_event *ev = (struct inotify_event *)(buf + off);
            off += (ssize_t)(sizeof *ev + ev->len);
            if (ev->mask & IN_Q_OVERFLOW) { dw->overflow = 1; continue; }
            if (ev->mask & IN_IGNORED) {
                if (ev->wd == dw->root_wd) dw->gone = 1;
                for (int i = 0; i < dw->nw; i++)
                    if (dw->w[i].wd == ev->wd) { free(dw->w[i].rel); dw->w[i] = dw->w[--dw->nw]; break; }
                continue;
            }
            if (!ev->len || !ev->name[0]) continue;
            const char *dir = dw_rel_of(dw, ev->wd);
            if (!dir) continue;
            char *rel;
            if (asprintf(&rel, "%s%s%s", dir, *dir ? "/" : "", ev->name) < 0) abort();
            int isdir = (ev->mask & IN_ISDIR) != 0;
            int named = (dw->filter & (isdir ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME)) != 0;
            if (ev->mask & IN_CREATE) {
                if (dw->subtree && isdir) dw_add_tree(dw, rel, 1);
                if (named) dw_push(dw, FILE_ACTION_ADDED, rel);
            } else if (ev->mask & IN_DELETE) {
                if (named) dw_push(dw, FILE_ACTION_REMOVED, rel);
            } else if (ev->mask & IN_MOVED_FROM) {
                struct inotify_event *to = NULL;
                for (ssize_t o2 = off; o2 < n;) {
                    struct inotify_event *e2 = (struct inotify_event *)(buf + o2);
                    o2 += (ssize_t)(sizeof *e2 + e2->len);
                    if ((e2->mask & IN_MOVED_TO) && e2->cookie == ev->cookie) { to = e2; break; }
                }
                const char *tdir = to ? dw_rel_of(dw, to->wd) : NULL;
                if (to && tdir) {
                    char *nrel;
                    if (asprintf(&nrel, "%s%s%s", tdir, *tdir ? "/" : "", to->name) < 0) abort();
                    if (isdir) dw_rename_prefix(dw, rel, nrel);
                    if (named) { dw_push(dw, FILE_ACTION_RENAMED_OLD_NAME, rel); dw_push(dw, FILE_ACTION_RENAMED_NEW_NAME, nrel); }
                    free(nrel);
                    to->mask = 0;            /* consumed */
                } else if (named) dw_push(dw, FILE_ACTION_REMOVED, rel);   /* moved out of the tree */
            } else if (ev->mask & IN_MOVED_TO) {
                if (dw->subtree && isdir) dw_add_tree(dw, rel, 1);
                if (named) dw_push(dw, FILE_ACTION_ADDED, rel);       /* moved in from outside */
            } else if (ev->mask & IN_MODIFY) {
                if (dw->filter & (FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE)) dw_push(dw, FILE_ACTION_MODIFIED, rel);
            } else if (ev->mask & IN_ATTRIB) {
                if (dw->filter & (FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION))
                    dw_push(dw, FILE_ACTION_MODIFIED, rel);
            } else if (ev->mask & IN_ACCESS) {
                if (dw->filter & FILE_NOTIFY_CHANGE_LAST_ACCESS) dw_push(dw, FILE_ACTION_MODIFIED, rel);
            }
            free(rel);
        }
    }
}

static void dw_clear(struct dirwatch *dw)
{
    while (dw->head) { struct dw_rec *r = dw->head; dw->head = r->next; free(r->rel); free(r); }
    dw->tail = NULL;
}

static void dw_unref_locked(struct dirwatch *dw)
{
    if (--dw->refs > 0) return;
    dw_clear(dw);
    for (int i = 0; i < dw->nw; i++) free(dw->w[i].rel);
    free(dw->w);
    close(dw->ifd);
    free(dw->root);
    free(dw);
}

/* The directory handle was closed (called from fext_free). */
static void dirwatch_release(struct dirwatch *dw)
{
    pthread_mutex_lock(&g_dw_lock);
    dw->closing = 1;
    dw_unref_locked(dw);
    pthread_mutex_unlock(&g_dw_lock);
}

static void fext_free(struct fext *f)
{
    if (f->port) iocp_unref(f->port);
    if (f->pipe) pipe_inst_detach(f->pipe);
    if (f->dw) dirwatch_release(f->dw);
    free(f);
}

/* FILE_NOTIFY_EXTENDED_INFORMATION, x64 layout. */
#define FNEI_NAME_OFF 84

static void fill_extended(struct dirwatch *dw, const char *rel, uint8_t *rec)
{
    char *path;
    if (asprintf(&path, "%s/%s", dw->root, rel) < 0) abort();
    struct stat st, pst;
    if (lstat(path, &st) == 0) {
        uint64_t mt = ft_from_timespec(st.st_mtim), ct = ft_from_timespec(st.st_ctim), at = ft_from_timespec(st.st_atim);
        memcpy(rec + 8, &mt, 8);                     /* CreationTime (no birth time here) */
        memcpy(rec + 16, &mt, 8);                    /* LastModificationTime */
        memcpy(rec + 24, &ct, 8);                    /* LastChangeTime */
        memcpy(rec + 32, &at, 8);                    /* LastAccessTime */
        uint64_t alloc = (uint64_t)st.st_blocks * 512, size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        memcpy(rec + 40, &alloc, 8);
        memcpy(rec + 48, &size, 8);
        DWORD attr = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        DWORD tag = 0;
        if (S_ISLNK(st.st_mode)) { attr |= 0x400; tag = 0xA000000Cu; }
        if (!(st.st_mode & S_IWUSR)) attr |= 0x1;
        memcpy(rec + 56, &attr, 4);
        memcpy(rec + 60, &tag, 4);
        uint64_t ino = st.st_ino;
        memcpy(rec + 64, &ino, 8);
    }
    char *slash = strrchr(path, '/');
    *slash = 0;
    if (stat(path, &pst) == 0) { uint64_t pino = pst.st_ino; memcpy(rec + 72, &pino, 8); }
    free(path);
}

/* Move queued records into a FILE_NOTIFY_INFORMATION (or extended) buffer.
 * A buffer too small for the first record gets nothing (bytes = 0), which
 * Windows programs read as "overflow: rescan". Call with g_dw_lock. */
static DWORD dw_fill(struct dirwatch *dw, uint8_t *buf, DWORD len, int extended)
{
    DWORD used = 0, prev = (DWORD)-1;
    size_t hdr = extended ? FNEI_NAME_OFF : 12, align = extended ? 8 : 4;
    if (dw->overflow) { dw->overflow = 0; dw_clear(dw); return 0; }
    while (dw->head) {
        struct dw_rec *r = dw->head;
        WCHAR *w = lsiw_u2w(r->rel);
        size_t nl = wlen(w);
        for (size_t i = 0; i < nl; i++) if (w[i] == '/') w[i] = '\\';
        size_t sz = (hdr + nl * 2 + align - 1) & ~(align - 1);
        if (used + sz > len) {
            free(w);
            if (!used) dw_clear(dw);
            break;
        }
        uint8_t *rec = buf + used;
        memset(rec, 0, sz);
        DWORD action = r->action, nbytes = (DWORD)(nl * 2);
        memcpy(rec + 4, &action, 4);
        if (extended) {
            fill_extended(dw, r->rel, rec);
            memcpy(rec + 80, &nbytes, 4);
        } else memcpy(rec + 8, &nbytes, 4);
        memcpy(rec + hdr, w, nl * 2);
        free(w);
        if (prev != (DWORD)-1) { DWORD next = used - prev; memcpy(buf + prev, &next, 4); }
        prev = used;
        used += (DWORD)sz;
        dw->head = r->next;
        if (!dw->head) dw->tail = NULL;
        free(r->rel);
        free(r);
    }
    return used;
}

/* Wait for changes. Returns the bytes stored, or (DWORD)-1 if the watch was
 * closed (*why = STATUS_NOTIFY_CLEANUP) or the folder vanished. */
static DWORD dw_wait(struct dirwatch *dw, uint8_t *buf, DWORD len, int extended, uint32_t *why)
{
    pthread_mutex_lock(&g_dw_lock);
    for (;;) {
        if (dw->closing) { *why = STATUS_NOTIFY_CLEANUP; break; }
        dw_pump(dw);
        if (dw->head || dw->overflow) {
            int over = dw->overflow;
            DWORD n = dw_fill(dw, buf, len, extended);
            pthread_mutex_unlock(&g_dw_lock);
            *why = !n && over ? STATUS_NOTIFY_ENUM_DIR : STATUS_SUCCESS;
            return n;
        }
        if (dw->gone) { *why = STATUS_DELETE_PENDING; break; }
        struct pollfd p = { dw->ifd, POLLIN, 0 };
        pthread_mutex_unlock(&g_dw_lock);
        poll(&p, 1, 200);
        pthread_mutex_lock(&g_dw_lock);
    }
    pthread_mutex_unlock(&g_dw_lock);
    return (DWORD)-1;
}

struct dw_job { struct dirwatch *dw; uint8_t *buf; DWORD len; int extended; OVERLAPPED *ov; struct io_target t; };

static void *dw_thread(void *arg)
{
    struct dw_job *j = arg;
    uint32_t why;
    DWORD n = dw_wait(j->dw, j->buf, j->len, j->extended, &why);
    if (n == (DWORD)-1) n = 0;
    pthread_mutex_lock(&g_dw_lock);
    j->dw->pending = 0;
    dw_unref_locked(j->dw);
    pthread_mutex_unlock(&g_dw_lock);
    io_target_complete(&j->t, j->ov, why, n);
    free(j);
    return NULL;
}

static BOOL read_dir_changes(HANDLE h, void *buf, DWORD len, BOOL subtree, DWORD filter, DWORD *returned,
                             OVERLAPPED *ov, IO_COMPLETION_FN routine, int extended)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct stat st;
    if (!o->path || stat(o->path, &st) || !S_ISDIR(st.st_mode)) { lsiw_set_last_error(ERROR_INVALID_FUNCTION); return FALSE; }
    if (!filter || (filter & ~0x17Fu)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (((uintptr_t)buf & 3) || (extended && ((uintptr_t)buf & 7))) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    if (routine && !ov) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *root = xstrdup(o->path);
    pthread_mutex_lock(&g_fext_lock);
    struct fext *f = fext_find(h, 1);
    pthread_mutex_lock(&g_dw_lock);
    struct dirwatch *dw = f->dw;
    if (dw && dw->pending) {
        pthread_mutex_unlock(&g_dw_lock);
        pthread_mutex_unlock(&g_fext_lock);
        free(root);
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!dw || dw->subtree != !!subtree || dw->filter != filter) {
        if (dw) { dw->closing = 1; dw_unref_locked(dw); }
        dw = xmalloc0(sizeof *dw);
        dw->refs = 1;
        dw->root_wd = -1;
        dw->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        dw->subtree = !!subtree;
        dw->filter = filter;
        dw->root = root;
        root = NULL;
        if (dw->ifd >= 0) dw_add_tree(dw, "", 0);
        f->dw = dw;
    }
    free(root);
    if (dw->ifd < 0 || dw->root_wd < 0) {
        f->dw = NULL;
        dw->closing = 1;
        dw_unref_locked(dw);
        pthread_mutex_unlock(&g_dw_lock);
        pthread_mutex_unlock(&g_fext_lock);
        lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    dw->refs++;
    if (ov) dw->pending = 1;
    pthread_mutex_unlock(&g_dw_lock);
    pthread_mutex_unlock(&g_fext_lock);
    if (ov) {
        struct dw_job *j = xmalloc0(sizeof *j);
        j->dw = dw;
        j->buf = buf;
        j->len = len;
        j->extended = extended;
        j->ov = ov;
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        io_target_capture(h, &j->t, routine);
        if (!start_host_thread(dw_thread, j)) {
            io_target_release(&j->t);
            free(j);
            pthread_mutex_lock(&g_dw_lock);
            dw->pending = 0;
            dw_unref_locked(dw);
            pthread_mutex_unlock(&g_dw_lock);
            lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        return TRUE;
    }
    uint32_t why;
    DWORD n = dw_wait(dw, buf, len, extended, &why);
    pthread_mutex_lock(&g_dw_lock);
    dw_unref_locked(dw);
    pthread_mutex_unlock(&g_dw_lock);
    if (n == (DWORD)-1) { lsiw_set_last_error(status_to_win32(why)); return FALSE; }
    if (returned) *returned = n;
    return TRUE;
}

API BOOL ReadDirectoryChangesW(HANDLE h, void *buf, DWORD len, BOOL subtree, DWORD filter, DWORD *returned,
                               OVERLAPPED *ov, IO_COMPLETION_FN routine)
{
    return read_dir_changes(h, buf, len, subtree, filter, returned, ov, routine, 0);
}

API BOOL ReadDirectoryChangesExW(HANDLE h, void *buf, DWORD len, BOOL subtree, DWORD filter, DWORD *returned,
                                 OVERLAPPED *ov, IO_COMPLETION_FN routine, int cls)
{
    /* ReadDirectoryNotifyInformation = 1, ReadDirectoryNotifyExtendedInformation = 2 */
    if (cls != 1 && cls != 2) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return read_dir_changes(h, buf, len, subtree, filter, returned, ov, routine, cls == 2);
}

/* ================================================================== */
/* Files and volumes                                                    */
/* ================================================================== */

/* Alternate data streams do not exist on Linux file systems; every file has
 * exactly its unnamed "::$DATA" stream, which is what Windows reports for
 * an ordinary file. Folders have no stream (ERROR_HANDLE_EOF). */
typedef struct { int64_t StreamSize; WCHAR cStreamName[MAX_PATH + 36]; } WIN32_FIND_STREAM_DATA;

API HANDLE FindFirstStreamW(const WCHAR *name, int level, WIN32_FIND_STREAM_DATA *data, DWORD flags)
{
    if (level != 0 || flags || !data) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    char *p = lsiw_path_w(name);
    struct stat st;
    int r = p ? stat(p, &st) : -1;
    int e = errno;
    free(p);
    if (r) { lsiw_set_errno_error(e); return INVALID_HANDLE_VALUE; }
    if (S_ISDIR(st.st_mode)) { lsiw_set_last_error(ERROR_HANDLE_EOF); return INVALID_HANDLE_VALUE; }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_FIND, &o);     /* closed with FindClose */
    if (!h) return INVALID_HANDLE_VALUE;
    memset(data, 0, sizeof *data);
    data->StreamSize = st.st_size;
    static const char dflt[] = "::$DATA";
    for (int i = 0; dflt[i]; i++) data->cStreamName[i] = (WCHAR)dflt[i];
    return h;
}

API BOOL FindNextStreamW(HANDLE h, void *data)
{
    (void)data;
    if (!lsiw_handle_get(h, H_FIND)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_set_last_error(ERROR_HANDLE_EOF);
    return FALSE;
}

/* Kernel transactions (TxF) do not exist here; the transacted variants fail
 * the way they do on a volume without transaction support. */
API HANDLE FindFirstStreamTransactedW(const WCHAR *n, int l, void *d, DWORD f, HANDLE t)
{ (void)n; (void)l; (void)d; (void)f; (void)t; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE; }
API BOOL GetFileAttributesTransactedW(const WCHAR *n, int l, void *i, HANDLE t)
{ (void)n; (void)l; (void)i; (void)t; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL GetFileAttributesTransactedA(const char *n, int l, void *i, HANDLE t)
{ (void)n; (void)l; (void)i; (void)t; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API DWORD GetLongPathNameTransactedW(const WCHAR *s, WCHAR *l, DWORD n, HANDLE t)
{ (void)s; (void)l; (void)n; (void)t; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
API DWORD GetLongPathNameTransactedA(const char *s, char *l, DWORD n, HANDLE t)
{ (void)s; (void)l; (void)n; (void)t; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return 0; }

/* Windows 11 24H2 API; callers (CPython) fall back to the older path calls
 * when it reports ERROR_NOT_SUPPORTED. */
API BOOL GetFileInformationByName(const WCHAR *n, int cls, void *buf, ULONG len)
{ (void)n; (void)cls; (void)buf; (void)len; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }

/* Size on disk: the allocated size for sparse or compressed files (those
 * occupy fewer blocks than their length), otherwise the file size. */
static DWORD compressed_size(char *p, DWORD *high)
{
    struct stat st;
    int r = p ? stat(p, &st) : -1;
    int e = errno;
    free(p);
    if (r) { lsiw_set_errno_error(e); if (high) *high = 0; return 0xFFFFFFFFu; }
    uint64_t alloc = (uint64_t)st.st_blocks * 512, sz = (uint64_t)st.st_size;
    uint64_t v = alloc < sz ? alloc : sz;
    if (high) *high = (DWORD)(v >> 32);
    lsiw_set_last_error(0);
    return (DWORD)v;
}

API DWORD GetCompressedFileSizeW(const WCHAR *name, DWORD *high) { return compressed_size(lsiw_path_w(name), high); }
API DWORD GetCompressedFileSizeA(const char *name, DWORD *high) { return compressed_size(lsiw_path_a(name), high); }

#define SCS_32BIT_BINARY 0
#define SCS_DOS_BINARY   1
#define SCS_WOW_BINARY   2
#define SCS_PIF_BINARY   3
#define SCS_64BIT_BINARY 6

static BOOL binary_type(char *p, DWORD *type)
{
    if (!p) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    const char *dot = strrchr(p, '.');
    int is_com = dot && !strcasecmp(dot, ".com"), is_pif = dot && !strcasecmp(dot, ".pif");
    free(p);
    if (fd < 0) { lsiw_set_errno_error(errno); return FALSE; }
    IMAGE_DOS_HEADER dos;
    BOOL ok = FALSE;
    DWORD t = 0;
    ssize_t n = pread(fd, &dos, sizeof dos, 0);
    if (n >= 2 && dos.e_magic == IMAGE_DOS_SIGNATURE) {
        uint8_t nt[24 + 2] = { 0 };
        if (n == (ssize_t)sizeof dos && dos.e_lfanew > 0 && pread(fd, nt, sizeof nt, dos.e_lfanew) == (ssize_t)sizeof nt) {
            uint32_t sig;
            memcpy(&sig, nt, 4);
            if (sig == IMAGE_NT_SIGNATURE) {
                IMAGE_FILE_HEADER fh;
                uint16_t magic;
                memcpy(&fh, nt + 4, sizeof fh);
                memcpy(&magic, nt + 24, 2);
                if (fh.Characteristics & IMAGE_FILE_DLL) { close(fd); lsiw_set_last_error(ERROR_BAD_EXE_FORMAT); return FALSE; }
                t = magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? SCS_64BIT_BINARY : SCS_32BIT_BINARY;
                ok = TRUE;
            } else if (nt[0] == 'N' && nt[1] == 'E') { t = SCS_WOW_BINARY; ok = TRUE; }
        }
        if (!ok) { t = SCS_DOS_BINARY; ok = TRUE; }       /* plain MZ: a DOS program */
    } else if (is_com) { t = SCS_DOS_BINARY; ok = TRUE; }
    else if (is_pif) { t = SCS_PIF_BINARY; ok = TRUE; }
    close(fd);
    if (!ok) { lsiw_set_last_error(ERROR_BAD_EXE_FORMAT); return FALSE; }
    if (type) *type = t;
    return TRUE;
}

API BOOL GetBinaryTypeW(const WCHAR *name, DWORD *type) { return binary_type(lsiw_path_w(name), type); }
API BOOL GetBinaryTypeA(const char *name, DWORD *type) { return binary_type(lsiw_path_a(name), type); }

/* Is `name` a valid MS-DOS 8.3 name? (base 1-8, extension 0-3, one dot, no
 * characters that FAT short names forbid). */
static BOOL legal_83(const WCHAR *name, char *oem, DWORD oem_size, BOOL *spaces, BOOL *legal)
{
    if (!name || !legal || (oem && !oem_size)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t n = wlen(name), base = 0, ext = 0;
    int dots = 0, ok = n > 0 && n <= 12, sp = 0;
    if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) ok = 1;
    else for (size_t i = 0; i < n && ok; i++) {
        WCHAR c = name[i];
        if (c == '.') { if (dots++ || i == 0) ok = 0; continue; }
        if (c < 0x20 || c > 0x7E || strchr("\"*+,/:;<=>?[\\]|", (char)c)) ok = 0;
        if (c == ' ') sp = 1;
        if (dots) ext++; else base++;
    }
    if (base > 8 || ext > 3) ok = 0;
    if (oem) {
        char *a = w2a(name, -1);
        if (strlen(a) + 1 > oem_size) { free(a); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        strcpy(oem, a);
        free(a);
    }
    if (spaces) *spaces = sp;
    *legal = ok;
    return TRUE;
}

API BOOL CheckNameLegalDOS8Dot3W(const WCHAR *name, char *oem, DWORD oem_size, BOOL *spaces, BOOL *legal)
{ return legal_83(name, oem, oem_size, spaces, legal); }

API BOOL CheckNameLegalDOS8Dot3A(const char *name, char *oem, DWORD oem_size, BOOL *spaces, BOOL *legal)
{
    WCHAR *w = a2w(name, -1);
    BOOL r = legal_83(w, oem, oem_size, spaces, legal);
    free(w);
    return r;
}

/* The whole Linux file system is the single volume C:; its volume GUID path
 * is the one GetVolumeNameForVolumeMountPointW (k32_file.c) reports. */
static const char g_volume_name[] = "\\\\?\\Volume{4c534957-0000-0000-0000-000000000001}\\";

static void volume_find_free(void *p) { free(p); }

static HANDLE find_first_volume(char *out_a, WCHAR *out_w, DWORD len)
{
    size_t need = sizeof g_volume_name;           /* includes NUL */
    if (len < need) { lsiw_set_last_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
    HANDLE h = gobj_create(xmalloc0(1), volume_find_free);
    if (!h) return INVALID_HANDLE_VALUE;
    if (out_a) memcpy(out_a, g_volume_name, need);
    else lsiw_utf8_to_utf16(g_volume_name, -1, out_w, (int)len);
    return h;
}

API HANDLE FindFirstVolumeW(WCHAR *name, DWORD len) { return find_first_volume(NULL, name, len); }
API HANDLE FindFirstVolumeA(char *name, DWORD len) { return find_first_volume(name, NULL, len); }

static BOOL find_next_volume(HANDLE h)
{
    if (!gobj_get(h, volume_find_free)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_set_last_error(ERROR_NO_MORE_FILES);
    return FALSE;
}

API BOOL FindNextVolumeW(HANDLE h, WCHAR *name, DWORD len) { (void)name; (void)len; return find_next_volume(h); }
API BOOL FindNextVolumeA(HANDLE h, char *name, DWORD len) { (void)name; (void)len; return find_next_volume(h); }

API BOOL FindVolumeClose(HANDLE h)
{
    if (!gobj_get(h, volume_find_free)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_handle_close(h);
    return TRUE;
}

/* Mount points inside the volume: none are reported. */
API HANDLE FindFirstVolumeMountPointW(const WCHAR *root, WCHAR *buf, DWORD len)
{ (void)root; (void)buf; (void)len; lsiw_set_last_error(ERROR_NO_MORE_FILES); return INVALID_HANDLE_VALUE; }

static int volume_name_matches(const char *v)
{
    return v && strlen(v) >= sizeof g_volume_name - 2 && !strncasecmp(v, g_volume_name, sizeof g_volume_name - 2);
}

API BOOL GetVolumePathNamesForVolumeNameW(const WCHAR *vol, WCHAR *buf, DWORD len, DWORD *ret)
{
    char *v = lsiw_w2u(vol);
    int ok = volume_name_matches(v);
    free(v);
    if (!ok) { lsiw_set_last_error(vol && wnicmp(vol, "\\\\?\\Volume{", 11) == 0 ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_NAME); return FALSE; }
    static const WCHAR paths[] = { 'C', ':', '\\', 0, 0 };
    if (ret) *ret = 5;
    if (!buf || len < 5) { lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
    memcpy(buf, paths, sizeof paths);
    return TRUE;
}

API BOOL GetVolumePathNamesForVolumeNameA(const char *vol, char *buf, DWORD len, DWORD *ret)
{
    if (!volume_name_matches(vol)) { lsiw_set_last_error(vol && !strncasecmp(vol, "\\\\?\\Volume{", 11) ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_NAME); return FALSE; }
    if (ret) *ret = 5;
    if (!buf || len < 5) { lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
    memcpy(buf, "C:\\\0", 5);
    return TRUE;
}

API DWORD QueryDosDeviceA(const char *dev, char *buf, DWORD max)
{
    k32_ready();
    if (!k32.QueryDosDeviceW) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    WCHAR *wd = dev ? a2w(dev, -1) : NULL;
    DWORD cap = max ? max : 1;
    WCHAR *wb = xmalloc0((size_t)cap * sizeof(WCHAR));
    DWORD n = k32.QueryDosDeviceW(wd, wb, cap);
    free(wd);
    if (!n) { free(wb); return 0; }
    /* Convert the multi-string list as one block (NULs included). */
    char *a = w2a(wb, (int)n);
    size_t al = 0;
    while (al < (size_t)n * 4 && !(a[al] == 0 && (al + 1 >= (size_t)n * 4 || a[al + 1] == 0))) al++;
    al += 2;
    free(wb);
    if (al > max) { free(a); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, a, al);
    free(a);
    return (DWORD)al;
}

/* ================================================================== */
/* Processes and threads                                                */
/* ================================================================== */

/* Thread objects have no id index, so OpenThread looks through the handle
 * table for a live handle to the thread and shares its object. A thread of
 * this process that has no open handle any more (the main thread, or one
 * whose handle was closed) gets a new object that names it; waiting on such
 * a handle does not see the thread exit. */
API HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid)
{
    (void)access; (void)inherit;
    for (uintptr_t v = 4; v <= 65536u * 4; v += 4) {
        struct lsiw_handle_obj *o = lsiw_handle_get((HANDLE)v, H_THREAD);
        if (o && o->tid == tid && !o->signaled) {
            HANDLE h = lsiw_handle_dup((HANDLE)v);
            if (!h) lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return h;
        }
    }
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%u", (unsigned)tid);
    struct stat st;
    if (!tid || stat(p, &st)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_THREAD, &o);
    if (h) o->tid = tid;
    return h;
}

/* Windows path of a process image. */
static char *process_image_win(HANDLE h)
{
    pid_t pid = process_pid(h);
    if (pid < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    if (pid == getpid() && lsiw_main_module && lsiw_main_module->win_path) return xstrdup(lsiw_main_module->win_path);
    char link[64], target[PATH_MAX];
    snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n < 0) { lsiw_set_last_error(errno == ENOENT ? ERROR_INVALID_HANDLE : ERROR_ACCESS_DENIED); return NULL; }
    target[n] = 0;
    char *w = lsiw_linux_to_win(target);
    return w ? w : xstrdup(target);
}

/* "\Device\HarddiskVolume1\..." form of a C:\ path (PROCESS_NAME_NATIVE). */
static char *native_device_path(const char *win)
{
    char *out;
    const char *rest = (isalpha((unsigned char)win[0]) && win[1] == ':') ? win + 2 : win;
    if (asprintf(&out, "\\Device\\HarddiskVolume1%s", rest) < 0) abort();
    return out;
}

#define PROCESS_NAME_NATIVE 1

API BOOL QueryFullProcessImageNameW(HANDLE h, DWORD flags, WCHAR *buf, DWORD *size)
{
    if (!size || !buf || (flags & ~(DWORD)PROCESS_NAME_NATIVE)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *w = process_image_win(h);
    if (!w) return FALSE;
    if (flags & PROCESS_NAME_NATIVE) { char *n = native_device_path(w); free(w); w = n; }
    int need = lsiw_utf8_to_utf16(w, -1, NULL, 0);
    if ((DWORD)need > *size) { free(w); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    lsiw_utf8_to_utf16(w, -1, buf, (int)*size);
    free(w);
    *size = (DWORD)need - 1;
    return TRUE;
}

API BOOL QueryFullProcessImageNameA(HANDLE h, DWORD flags, char *buf, DWORD *size)
{
    if (!size || !buf) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD wsz = 32768;
    WCHAR *wb = xmalloc0(wsz * sizeof(WCHAR));
    if (!QueryFullProcessImageNameW(h, flags, wb, &wsz)) { free(wb); return FALSE; }
    char *a = w2a(wb, -1);
    free(wb);
    size_t n = strlen(a);
    if (n + 1 > *size) { free(a); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, a, n + 1);
    free(a);
    *size = (DWORD)n;
    return TRUE;
}

API BOOL GetProcessHandleCount(HANDLE h, DWORD *count)
{
    pid_t pid = process_pid(h);
    if (pid < 0 || !count) { lsiw_set_last_error(pid < 0 ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD n = 0;
    if (pid == getpid()) {
        for (uintptr_t v = 4; v <= 65536u * 4; v += 4) if (lsiw_handle_get((HANDLE)v, -1)) n++;
    } else {
        char p[64];
        snprintf(p, sizeof p, "/proc/%d/fd", (int)pid);
        DIR *d = opendir(p);
        if (!d) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
        struct dirent *de;
        while ((de = readdir(d))) if (de->d_name[0] != '.') n++;
        closedir(d);
    }
    *count = n;
    return TRUE;
}

typedef struct { ULONGLONG ReadOperationCount, WriteOperationCount, OtherOperationCount,
                 ReadTransferCount, WriteTransferCount, OtherTransferCount; } IO_COUNTERS;

API BOOL GetProcessIoCounters(HANDLE h, IO_COUNTERS *io)
{
    pid_t pid = process_pid(h);
    if (pid < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!io) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/io", (int)pid);
    size_t len;
    char *t = (char *)read_file(p, &len);
    if (!t) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    memset(io, 0, sizeof *io);
    io->ReadOperationCount = meminfo_kb(t, "syscr:");
    io->WriteOperationCount = meminfo_kb(t, "syscw:");
    io->ReadTransferCount = meminfo_kb(t, "rchar:");
    io->WriteTransferCount = meminfo_kb(t, "wchar:");
    free(t);
    return TRUE;
}

/* ---- process / thread information classes ---- */

#define MEMORY_PRIORITY_NORMAL 5

static BOOL info_size(DWORD have, DWORD want)
{
    if (have != want) { lsiw_set_last_error(ERROR_BAD_LENGTH); return FALSE; }
    return TRUE;
}

static DWORD g_proc_memprio = MEMORY_PRIORITY_NORMAL;
static DWORD g_proc_throttle[2];
static DWORD g_proc_leap;

API BOOL GetProcessInformation(HANDLE h, int cls, void *info, DWORD size)
{
    if (process_pid(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!info) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    switch (cls) {
    case 0:   /* ProcessMemoryPriority */
        if (!info_size(size, 4)) return FALSE;
        *(DWORD *)info = g_proc_memprio;
        return TRUE;
    case 2: { /* ProcessAppMemoryInfo: APP_MEMORY_INFORMATION */
        if (!info_size(size, 32)) return FALSE;
        size_t len;
        char *mi = (char *)read_file("/proc/meminfo", &len), *st = (char *)read_file("/proc/self/status", &len);
        uint64_t *o = info;
        o[0] = meminfo_kb(mi, "MemAvailable:") * 1024;          /* AvailableCommit */
        o[1] = meminfo_kb(st, "VmData:") * 1024;                /* PrivateCommitUsage */
        o[2] = meminfo_kb(st, "VmPeak:") * 1024;                /* PeakPrivateCommitUsage */
        o[3] = meminfo_kb(st, "VmSize:") * 1024;                /* TotalCommitUsage */
        free(mi); free(st);
        return TRUE;
    }
    case 4:   /* ProcessPowerThrottling: { Version, ControlMask, StateMask } */
        if (!info_size(size, 12)) return FALSE;
        ((DWORD *)info)[0] = 1;
        ((DWORD *)info)[1] = g_proc_throttle[0];
        ((DWORD *)info)[2] = g_proc_throttle[1];
        return TRUE;
    case 7:   /* ProcessProtectionLevelInfo: PROTECTION_LEVEL_NONE */
        if (!info_size(size, 4)) return FALSE;
        *(DWORD *)info = 0xFFFFFFFEu;
        return TRUE;
    case 8:   /* ProcessLeapSecondInfo: { Flags, Reserved } */
        if (!info_size(size, 8)) return FALSE;
        ((DWORD *)info)[0] = g_proc_leap;
        ((DWORD *)info)[1] = 0;
        return TRUE;
    case 9:   /* ProcessMachineTypeInfo: { USHORT Machine; USHORT Res; DWORD Attributes } */
        if (!info_size(size, 8)) return FALSE;
        ((USHORT *)info)[0] = IMAGE_FILE_MACHINE_AMD64;
        ((USHORT *)info)[1] = 0;
        ((DWORD *)info)[1] = 3;       /* UserEnabled | KernelEnabled */
        return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

API BOOL SetProcessInformation(HANDLE h, int cls, void *info, DWORD size)
{
    if (process_pid(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!info) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    switch (cls) {
    case 0:
        if (!info_size(size, 4)) return FALSE;
        if (*(DWORD *)info < 1 || *(DWORD *)info > 5) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        g_proc_memprio = *(DWORD *)info;
        return TRUE;
    case 1:   /* ProcessMemoryExhaustionInfo: accepted, nothing to enforce */
        if (!info_size(size, 16)) return FALSE;
        return TRUE;
    case 4:
        if (!info_size(size, 12)) return FALSE;
        if (((DWORD *)info)[0] != 1) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        g_proc_throttle[0] = ((DWORD *)info)[1];
        g_proc_throttle[1] = ((DWORD *)info)[2];
        return TRUE;
    case 8:
        if (!info_size(size, 8)) return FALSE;
        g_proc_leap = ((DWORD *)info)[0];
        return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static int thread_handle_ok(HANDLE h) { return h == PSEUDO_THREAD || lsiw_handle_get(h, H_THREAD) != NULL; }

API BOOL GetThreadInformation(HANDLE h, int cls, void *info, DWORD size)
{
    if (!thread_handle_ok(h)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!info) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    switch (cls) {
    case 0: if (!info_size(size, 4)) return FALSE; *(DWORD *)info = MEMORY_PRIORITY_NORMAL; return TRUE;  /* ThreadMemoryPriority */
    case 1: if (!info_size(size, 4)) return FALSE; *(LONG *)info = 8; return TRUE;                        /* ThreadAbsoluteCpuPriority */
    case 2: if (!info_size(size, 4)) return FALSE; *(DWORD *)info = 0; return TRUE;                       /* ThreadDynamicCodePolicy */
    case 3: if (!info_size(size, 12)) return FALSE;                                                        /* ThreadPowerThrottling */
        ((DWORD *)info)[0] = 1; ((DWORD *)info)[1] = 0; ((DWORD *)info)[2] = 0; return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

API BOOL SetThreadInformation(HANDLE h, int cls, void *info, DWORD size)
{
    if (!thread_handle_ok(h)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!info) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    switch (cls) {
    case 0: case 2: return info_size(size, 4);
    case 3:
        if (!info_size(size, 12)) return FALSE;
        if (((DWORD *)info)[0] != 1) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/* Mitigation policies: nothing is enforced, but a policy a program sets is
 * remembered so that reading it back gives the same flags. DEP is always
 * on for 64-bit code and cannot be changed. */
#define MAX_MITIGATION_POLICY 19
static DWORD g_mitigation[MAX_MITIGATION_POLICY];

static DWORD mitigation_size(int policy)
{
    if (policy == 0) return 8;        /* PROCESS_MITIGATION_DEP_POLICY { DWORD Flags; BOOLEAN Permanent; } */
    if (policy == 5) return 16;       /* ProcessMitigationOptionsMask: ULONG64[2] (also accepts 8) */
    return 4;
}

API BOOL GetProcessMitigationPolicy(HANDLE h, int policy, void *buf, SIZE_T len)
{
    if (process_pid(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (policy < 0 || policy >= MAX_MITIGATION_POLICY || !buf ||
        (len != mitigation_size(policy) && !(policy == 5 && len == 8))) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(buf, 0, len);
    if (policy == 0) { ((DWORD *)buf)[0] = 1; ((BOOLEAN *)buf)[4] = 1; }   /* Enable, Permanent */
    else if (policy != 5) *(DWORD *)buf = g_mitigation[policy];
    return TRUE;
}

API BOOL SetProcessMitigationPolicy(int policy, void *buf, SIZE_T len)
{
    if (policy < 0 || policy >= MAX_MITIGATION_POLICY || policy == 5 || !buf || len != mitigation_size(policy)) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (policy == 0) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
    g_mitigation[policy] |= *(DWORD *)buf;     /* policies can only be tightened */
    return TRUE;
}

#define PROCESS_DEP_ENABLE 1

API BOOL GetProcessDEPPolicy(HANDLE h, DWORD *flags, BOOL *permanent)
{
    if (process_pid(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (flags) *flags = PROCESS_DEP_ENABLE;
    if (permanent) *permanent = TRUE;
    return TRUE;
}

/* Only 32-bit processes can change their DEP policy. */
API BOOL SetProcessDEPPolicy(DWORD flags) { (void)flags; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }

API int GetSystemDEPPolicy(void) { return 2; /* OptIn, the Windows client default */ }

API BOOL IsProcessCritical(HANDLE h, BOOL *critical)
{
    if (process_pid(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (critical) *critical = FALSE;
    return TRUE;
}

/* Everything runs in the one interactive session, number 1. */
API BOOL ProcessIdToSessionId(DWORD pid, DWORD *session)
{
    char p[32];
    snprintf(p, sizeof p, "/proc/%u", (unsigned)pid);
    struct stat st;
    if (!session || stat(p, &st)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *session = 1;
    return TRUE;
}


static DWORD g_shutdown_level = 0x280, g_shutdown_flags;

API BOOL SetProcessShutdownParameters(DWORD level, DWORD flags)
{
    if (level > 0x4FF) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    g_shutdown_level = level;
    g_shutdown_flags = flags;
    return TRUE;
}

API BOOL GetProcessShutdownParameters(DWORD *level, DWORD *flags)
{
    if (level) *level = g_shutdown_level;
    if (flags) *flags = g_shutdown_flags;
    return TRUE;
}

/* Subsystem version of the process image (MajorSubsystemVersion << 16 | Minor). */
API DWORD GetProcessVersion(DWORD pid)
{
    if (pid && pid != (DWORD)getpid()) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!lsiw_main_module || !lsiw_main_module->base) return 0x000A0000;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)lsiw_main_module->base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(lsiw_main_module->base + dos->e_lfanew);
    return ((DWORD)nt->OptionalHeader.MajorSubsystemVersion << 16) | nt->OptionalHeader.MinorSubsystemVersion;
}

API BOOL GetThreadIOPendingFlag(HANDLE h, BOOL *pending)
{
    if (!thread_handle_ok(h) || !pending) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *pending = FALSE;
    return TRUE;
}

API BOOL QueryProcessAffinityUpdateMode(HANDLE h, DWORD *flags)
{
    if (process_pid(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (flags) *flags = 0;
    return TRUE;
}

API BOOL SetProcessAffinityUpdateMode(HANDLE h, DWORD flags)
{
    if (h != PSEUDO_PROCESS || flags > 1) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

/* ---- stacks ---- */

/* TEB fields: StackBase 0x08, StackLimit 0x10, DeallocationStack 0x1478,
 * GuaranteedStackBytes 0x1748. */
API void GetCurrentThreadStackLimits(ULONG_PTR *low, ULONG_PTR *high)
{
    uint8_t *teb = lsiw_teb_current();
    ULONG_PTR lo = 0, hi = 0;
    if (teb) {
        hi = *(ULONG_PTR *)(teb + 0x08);
        lo = *(ULONG_PTR *)(teb + 0x1478);
        if (!lo) lo = *(ULONG_PTR *)(teb + 0x10);
    }
    if (!hi) {
        pthread_attr_t a;
        void *base;
        size_t sz;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            if (pthread_attr_getstack(&a, &base, &sz) == 0) { lo = (ULONG_PTR)base; hi = lo + sz; }
            pthread_attr_destroy(&a);
        }
    }
    if (low) *low = lo;
    if (high) *high = hi;
}

/* Recorded in the TEB like Windows; the overflow handling of seh.c runs on
 * its own signal stack, so the reserve itself is not needed. */
API BOOL SetThreadStackGuarantee(ULONG *size)
{
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uint8_t *teb = lsiw_teb_current();
    static __thread ULONG fallback;
    ULONG *cur = teb ? (ULONG *)(teb + 0x1748) : &fallback;
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    if (hi && *size >= hi - lo) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG prev = *cur;
    if (*size > prev) *cur = (*size + 0xFFF) & ~0xFFFu;
    *size = prev;
    return TRUE;
}

/* ================================================================== */
/* CPU topology, NUMA, processor groups                                 */
/* ================================================================== */

/* Logical processor n of Linux is processor n % 64 of group n / 64, the
 * numbering GetCurrentProcessorNumber also uses. Topology comes from
 * /sys/devices/system/cpu and /sys/devices/system/node; without sysfs each
 * CPU is its own core in one package and one NUMA node. */
#define MAXCPU   1024
#define MAXGROUP (MAXCPU / 64)
#define MAXNODE  64
#define MAXCACHE 256

typedef struct { uint64_t m[MAXGROUP]; } cpumask_t;

struct cache_info { int level, type, line, ways; uint64_t size; cpumask_t cpus; };

static struct {
    int ncpu;                        /* highest online CPU + 1 */
    int nconf;                       /* configured CPUs */
    cpumask_t online;
    int core[MAXCPU];                /* lowest CPU of the core */
    int pkg[MAXCPU], die[MAXCPU], node[MAXCPU], eff[MAXCPU];
    int max_node;
    cpumask_t node_cpus[MAXNODE];
    uint8_t node_present[MAXNODE];
    struct cache_info caches[MAXCACHE];
    int ncache;
} T;
static pthread_once_t g_topo_once = PTHREAD_ONCE_INIT;

static void mask_set(cpumask_t *m, int c) { if (c >= 0 && c < MAXCPU) m->m[c / 64] |= 1ull << (c % 64); }
static int mask_has(const cpumask_t *m, int c) { return c >= 0 && c < MAXCPU && (m->m[c / 64] >> (c % 64)) & 1; }
static int mask_eq(const cpumask_t *a, const cpumask_t *b) { return !memcmp(a, b, sizeof *a); }
static int mask_first(const cpumask_t *m) { for (int c = 0; c < MAXCPU; c++) if (mask_has(m, c)) return c; return -1; }

/* "0-3,8,10-11" */
static void parse_cpulist(const char *s, cpumask_t *m)
{
    memset(m, 0, sizeof *m);
    while (s && *s) {
        char *e;
        long a = strtol(s, &e, 10), b = a;
        if (e == s) break;
        if (*e == '-') b = strtol(e + 1, &e, 10);
        for (long c = a; c <= b && c < MAXCPU; c++) mask_set(m, (int)c);
        s = *e == ',' ? e + 1 : NULL;
    }
}

static int read_cpulist(const char *path, cpumask_t *m)
{
    char b[4096];
    if (read_text(path, b, sizeof b) < 0) return 0;
    parse_cpulist(b, m);
    return 1;
}

static void topo_init(void)
{
    char p[256], b[256];
    if (!read_cpulist("/sys/devices/system/cpu/online", &T.online)) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        for (long c = 0; c < (n > 0 ? n : 1) && c < MAXCPU; c++) mask_set(&T.online, (int)c);
    }
    long conf = sysconf(_SC_NPROCESSORS_CONF);
    for (int c = 0; c < MAXCPU; c++) if (mask_has(&T.online, c)) T.ncpu = c + 1;
    T.nconf = conf > T.ncpu ? (int)conf : T.ncpu;
    long caps[MAXCPU];
    for (int c = 0; c < T.ncpu; c++) {
        T.core[c] = c;
        if (!mask_has(&T.online, c)) continue;
        cpumask_t sib;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_cpus_list", c);
        if (!read_cpulist(p, &sib)) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
            if (!read_cpulist(p, &sib)) { memset(&sib, 0, sizeof sib); mask_set(&sib, c); }
        }
        T.core[c] = mask_first(&sib) >= 0 ? mask_first(&sib) : c;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", c);
        T.pkg[c] = (int)read_long(p, 0);
        if (T.pkg[c] < 0) T.pkg[c] = 0;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/die_id", c);
        T.die[c] = T.pkg[c] * 1024 + (int)read_long(p, 0);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        caps[c] = read_long(p, 1024);
        /* caches */
        for (int i = 0; i < 16; i++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", c, i);
            long level = read_long(p, -1);
            if (level < 0) break;
            struct cache_info ci = { .level = (int)level };
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/type", c, i);
            if (read_text(p, b, sizeof b) > 0)
                ci.type = !strcmp(b, "Instruction") ? 1 : !strcmp(b, "Data") ? 2 : 0;   /* PROCESSOR_CACHE_TYPE */
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", c, i);
            if (read_text(p, b, sizeof b) > 0) {
                char *e;
                ci.size = strtoull(b, &e, 10);
                if (*e == 'K') ci.size <<= 10; else if (*e == 'M') ci.size <<= 20;
            }
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/coherency_line_size", c, i);
            ci.line = (int)read_long(p, 64);
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/ways_of_associativity", c, i);
            ci.ways = (int)read_long(p, 0);
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", c, i);
            if (!read_cpulist(p, &ci.cpus)) { memset(&ci.cpus, 0, sizeof ci.cpus); mask_set(&ci.cpus, c); }
            int dup = 0;
            for (int k = 0; k < T.ncache && !dup; k++)
                dup = T.caches[k].level == ci.level && T.caches[k].type == ci.type && mask_eq(&T.caches[k].cpus, &ci.cpus);
            if (!dup && T.ncache < MAXCACHE) T.caches[T.ncache++] = ci;
        }
    }
    /* Efficiency class: rank of the CPU's capacity among distinct values
     * (hybrid CPUs); 0 everywhere on uniform machines. */
    for (int c = 0; c < T.ncpu; c++) {
        if (!mask_has(&T.online, c)) continue;
        int rank = 0;
        long seen[64];
        int ns = 0;
        for (int d = 0; d < T.ncpu; d++) {
            if (!mask_has(&T.online, d) || caps[d] >= caps[c]) continue;
            int dup = 0;
            for (int k = 0; k < ns; k++) dup |= seen[k] == caps[d];
            if (!dup && ns < 64) { seen[ns++] = caps[d]; rank++; }
        }
        T.eff[c] = rank;
    }
    cpumask_t nodes;
    int have_nodes = read_cpulist("/sys/devices/system/node/online", &nodes);
    for (int n = 0; n < MAXNODE && have_nodes; n++) {
        if (!mask_has(&nodes, n)) continue;
        snprintf(p, sizeof p, "/sys/devices/system/node/node%d/cpulist", n);
        read_cpulist(p, &T.node_cpus[n]);
        T.node_present[n] = 1;
        T.max_node = n;
        for (int c = 0; c < T.ncpu; c++) if (mask_has(&T.node_cpus[n], c)) T.node[c] = n;
    }
    if (!have_nodes) {
        T.node_present[0] = 1;
        T.node_cpus[0] = T.online;
    }
}

static void topo(void) { pthread_once(&g_topo_once, topo_init); }
static int ngroups_active(void) { topo(); return (T.ncpu + 63) / 64; }

typedef struct { KAFFINITY Mask; USHORT Group; USHORT Reserved[3]; } GROUP_AFFINITY;
typedef struct { USHORT Group; BYTE Number; BYTE Reserved; } PROCESSOR_NUMBER;
_Static_assert(sizeof(GROUP_AFFINITY) == 16, "GROUP_AFFINITY");

/* Record builder for SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: computes the
 * total size and writes only while the caller's buffer has room. */
struct rb { uint8_t *buf; size_t cap, off; };

static uint8_t *rb_rec(struct rb *b, DWORD rel, size_t size)
{
    uint8_t *p = NULL;
    if (b->buf && b->off + size <= b->cap) {
        p = b->buf + b->off;
        memset(p, 0, size);
        DWORD s = (DWORD)size;
        memcpy(p, &rel, 4);
        memcpy(p + 4, &s, 4);
    }
    b->off += size;
    return p;
}

static int mask_groups(const cpumask_t *m, USHORT *groups)
{
    int n = 0;
    for (int g = 0; g < MAXGROUP; g++) if (m->m[g]) groups[n++] = (USHORT)g;
    return n;
}

static void put_affinity(uint8_t *p, const cpumask_t *m, USHORT g)
{
    GROUP_AFFINITY a = { m->m[g], g, { 0, 0, 0 } };
    memcpy(p, &a, sizeof a);
}

/* PROCESSOR_RELATIONSHIP: Flags, EfficiencyClass, Reserved[20], GroupCount, GroupMask[] at 24. */
static void emit_processor(struct rb *b, DWORD rel, const cpumask_t *m, BYTE flags, BYTE eff)
{
    USHORT gs[MAXGROUP];
    int gc = mask_groups(m, gs);
    uint8_t *p = rb_rec(b, rel, 8 + 24 + 16 * (size_t)gc);
    if (!p) return;
    p[8] = flags;
    p[9] = eff;
    USHORT c = (USHORT)gc;
    memcpy(p + 8 + 22, &c, 2);
    for (int i = 0; i < gc; i++) put_affinity(p + 8 + 24 + 16 * i, m, gs[i]);
}

#define LTP_PC_SMT 1

static BOOL logical_info_ex(int rel, uint8_t *buf, DWORD *len)
{
    topo();
    if (!len) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    if (!(rel >= 0 && rel <= 7) && rel != 0xFFFF) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct rb b = { buf, *len, 0 };
    int all = rel == 0xFFFF;
    if (all || rel == 0)      /* RelationProcessorCore */
        for (int c = 0; c < T.ncpu; c++) {
            if (!mask_has(&T.online, c) || T.core[c] != c) continue;
            cpumask_t m = { { 0 } };
            int n = 0;
            for (int d = 0; d < T.ncpu; d++) if (mask_has(&T.online, d) && T.core[d] == c) { mask_set(&m, d); n++; }
            emit_processor(&b, 0, &m, n > 1 ? LTP_PC_SMT : 0, (BYTE)T.eff[c]);
        }
    if (all || rel == 1 || rel == 6)    /* RelationNumaNode / RelationNumaNodeEx */
        for (int n = 0; n <= T.max_node; n++) {
            if (!T.node_present[n]) continue;
            cpumask_t m = T.node_cpus[n];
            for (int g = 0; g < MAXGROUP; g++) m.m[g] &= T.online.m[g];
            USHORT gs[MAXGROUP];
            int gc = mask_groups(&m, gs);
            if (rel != 6 && gc > 1) gc = 1;
            int slots = gc ? gc : 1;
            uint8_t *p = rb_rec(&b, (DWORD)(rel == 6 ? 6 : 1), 8 + 24 + 16 * (size_t)slots);
            if (!p) continue;
            DWORD nn = (DWORD)n;
            USHORT c = (USHORT)slots;
            memcpy(p + 8, &nn, 4);
            memcpy(p + 8 + 22, &c, 2);
            for (int i = 0; i < gc; i++) put_affinity(p + 8 + 24 + 16 * i, &m, gs[i]);
        }
    if (all || rel == 2)      /* RelationCache */
        for (int k = 0; k < T.ncache; k++) {
            struct cache_info *ci = &T.caches[k];
            USHORT gs[MAXGROUP];
            int gc = mask_groups(&ci->cpus, gs);
            if (!gc) continue;
            uint8_t *p = rb_rec(&b, 2, 8 + 32 + 16 * (size_t)gc);
            if (!p) continue;
            p[8] = (BYTE)ci->level;
            p[9] = (BYTE)(ci->ways > 254 ? 0xFF : ci->ways);
            USHORT line = (USHORT)ci->line, c = (USHORT)gc;
            DWORD size = (DWORD)ci->size, type = (DWORD)ci->type;
            memcpy(p + 10, &line, 2);
            memcpy(p + 12, &size, 4);
            memcpy(p + 16, &type, 4);
            memcpy(p + 8 + 30, &c, 2);
            for (int i = 0; i < gc; i++) put_affinity(p + 8 + 32 + 16 * i, &ci->cpus, gs[i]);
        }
    if (all || rel == 3 || rel == 5) {  /* RelationProcessorPackage / RelationProcessorDie */
        int *key = rel == 5 ? T.die : T.pkg;
        int loops = all ? 2 : 1;
        for (int pass = 0; pass < loops; pass++) {
            if (pass) key = T.die;
            DWORD r = (rel == 5 || pass) ? 5 : 3;
            if (all && r == 5) break;          /* RelationAll reports packages, not dies */
            for (int c = 0; c < T.ncpu; c++) {
                if (!mask_has(&T.online, c)) continue;
                int first = 1;
                for (int d = 0; d < c; d++) if (mask_has(&T.online, d) && key[d] == key[c]) first = 0;
                if (!first) continue;
                cpumask_t m = { { 0 } };
                for (int d = 0; d < T.ncpu; d++) if (mask_has(&T.online, d) && key[d] == key[c]) mask_set(&m, d);
                emit_processor(&b, r, &m, 0, 0);
            }
        }
    }
    if (all || rel == 4) {    /* RelationGroup */
        int ng = ngroups_active();
        uint8_t *p = rb_rec(&b, 4, 8 + 24 + 48 * (size_t)ng);
        if (p) {
            USHORT g16 = (USHORT)ng;
            memcpy(p + 8, &g16, 2);           /* MaximumGroupCount */
            memcpy(p + 10, &g16, 2);          /* ActiveGroupCount */
            for (int g = 0; g < ng; g++) {
                uint8_t *gi = p + 8 + 24 + 48 * g;
                int maxc = T.nconf - g * 64, act = __builtin_popcountll(T.online.m[g]);
                gi[0] = (BYTE)(maxc > 64 ? 64 : maxc);
                gi[1] = (BYTE)act;
                memcpy(gi + 40, &T.online.m[g], 8);
            }
        }
    }
    DWORD need = (DWORD)b.off;
    if (!buf || need > *len) { *len = need; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *len = need;
    return TRUE;
}

API BOOL GetLogicalProcessorInformationEx(int rel, void *buf, DWORD *len) { return logical_info_ex(rel, buf, len); }

/* SYSTEM_CPU_SET_INFORMATION: one 32-byte record per logical processor,
 * id 0x100 + processor index. */
API BOOL GetSystemCpuSetInformation(void *info, ULONG len, ULONG *ret, HANDLE process, ULONG flags)
{
    topo();
    if (flags || (process && process_pid(process) < 0)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    ULONG n = 0;
    for (int c = 0; c < T.ncpu; c++) if (mask_has(&T.online, c)) n++;
    ULONG need = n * 32;
    if (ret) *ret = need;
    if (!info || len < need) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    uint8_t *p = info;
    for (int c = 0; c < T.ncpu; c++) {
        if (!mask_has(&T.online, c)) continue;
        memset(p, 0, 32);
        DWORD size = 32, type = 0, id = 0x100 + (DWORD)c;
        USHORT g = (USHORT)(c / 64);
        int llc = c;
        for (int k = 0, best = -1; k < T.ncache; k++)
            if (mask_has(&T.caches[k].cpus, c) && T.caches[k].level > best) { best = T.caches[k].level; llc = mask_first(&T.caches[k].cpus); }
        memcpy(p, &size, 4);
        memcpy(p + 4, &type, 4);
        memcpy(p + 8, &id, 4);
        memcpy(p + 12, &g, 2);
        p[14] = (BYTE)(c % 64);
        p[15] = (BYTE)(T.core[c] % 64);
        p[16] = (BYTE)(llc % 64);
        p[17] = (BYTE)T.node[c];
        p[18] = (BYTE)T.eff[c];
        p += 32;
    }
    return TRUE;
}

static pid_t thread_sched_tid(HANDLE h, int *ok)
{
    *ok = 1;
    if (h == PSEUDO_THREAD) return 0;
    uint32_t tid = thread_tid(h);
    if (!tid) *ok = 0;
    return (pid_t)tid == (pid_t)lsiw_thread()->tid ? 0 : (pid_t)tid;
}

static void affinity_mask(pid_t who, cpumask_t *m)
{
    cpu_set_t cs;
    memset(m, 0, sizeof *m);
    if (sched_getaffinity(who, sizeof cs, &cs) == 0) {
        for (int c = 0; c < MAXCPU && c < CPU_SETSIZE; c++) if (CPU_ISSET(c, &cs)) mask_set(m, c);
    } else m->m[0] = T.online.m[0];
}

static int set_affinity(pid_t who, const cpumask_t *m)
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    for (int c = 0; c < MAXCPU && c < CPU_SETSIZE; c++) if (mask_has(m, c)) CPU_SET(c, &cs);
    return sched_setaffinity(who, sizeof cs, &cs);
}

/* A Windows thread belongs to exactly one group: the first one its
 * affinity touches. */
API BOOL GetThreadGroupAffinity(HANDLE h, GROUP_AFFINITY *aff)
{
    topo();
    int ok;
    pid_t who = thread_sched_tid(h, &ok);
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!aff) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    cpumask_t m;
    affinity_mask(who, &m);
    int g = 0;
    while (g < MAXGROUP - 1 && !m.m[g]) g++;
    memset(aff, 0, sizeof *aff);
    aff->Mask = m.m[g];
    aff->Group = (USHORT)g;
    return TRUE;
}

API BOOL SetThreadGroupAffinity(HANDLE h, const GROUP_AFFINITY *aff, GROUP_AFFINITY *prev)
{
    topo();
    int ok;
    pid_t who = thread_sched_tid(h, &ok);
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!aff || aff->Group >= ngroups_active() || aff->Reserved[0] || aff->Reserved[1] || aff->Reserved[2] ||
        !aff->Mask || (aff->Mask & ~T.online.m[aff->Group])) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (prev && !GetThreadGroupAffinity(h, prev)) return FALSE;
    cpumask_t m = { { 0 } };
    m.m[aff->Group] = aff->Mask;
    if (set_affinity(who, &m)) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

API BOOL GetProcessGroupAffinity(HANDLE h, USHORT *count, USHORT *groups)
{
    topo();
    pid_t pid = process_pid(h);
    if (pid < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!count) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    cpumask_t m;
    affinity_mask(pid == getpid() ? 0 : pid, &m);
    USHORT gs[MAXGROUP];
    int n = mask_groups(&m, gs);
    if (!n) { gs[0] = 0; n = 1; }
    if (*count < n || !groups) { *count = (USHORT)n; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(groups, gs, (size_t)n * sizeof(USHORT));
    *count = (USHORT)n;
    return TRUE;
}

API void GetCurrentProcessorNumberEx(PROCESSOR_NUMBER *pn)
{
    int c = sched_getcpu();
    if (c < 0) c = 0;
    pn->Group = (USHORT)(c / 64);
    pn->Number = (BYTE)(c % 64);
    pn->Reserved = 0;
}

API USHORT GetMaximumProcessorGroupCount(void) { topo(); return (USHORT)((T.nconf + 63) / 64); }

/* The ideal processor is a scheduling hint; the thread's current CPU is
 * reported and a new value is only validated. */
API BOOL GetThreadIdealProcessorEx(HANDLE h, PROCESSOR_NUMBER *pn)
{
    if (!thread_handle_ok(h) || !pn) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    GetCurrentProcessorNumberEx(pn);
    return TRUE;
}

API BOOL SetThreadIdealProcessorEx(HANDLE h, PROCESSOR_NUMBER *ideal, PROCESSOR_NUMBER *prev)
{
    topo();
    if (!thread_handle_ok(h) || !ideal || ideal->Number >= 64 || !mask_has(&T.online, ideal->Group * 64 + ideal->Number)) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (prev) GetCurrentProcessorNumberEx(prev);
    return TRUE;
}

/* ---- CPU sets: ids are 0x100 + Linux CPU number ---- */

static BOOL cpu_ids_to_mask(const ULONG *ids, ULONG n, cpumask_t *m)
{
    topo();
    memset(m, 0, sizeof *m);
    if (!n) { *m = T.online; return TRUE; }
    for (ULONG i = 0; i < n; i++) {
        if (ids[i] < 0x100 || !mask_has(&T.online, (int)(ids[i] - 0x100))) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        mask_set(m, (int)(ids[i] - 0x100));
    }
    return TRUE;
}

API BOOL SetThreadSelectedCpuSets(HANDLE h, const ULONG *ids, ULONG n)
{
    int ok;
    pid_t who = thread_sched_tid(h, &ok);
    cpumask_t m;
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!cpu_ids_to_mask(ids, n, &m)) return FALSE;
    if (set_affinity(who, &m)) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

API BOOL SetProcessDefaultCpuSets(HANDLE h, const ULONG *ids, ULONG n)
{
    cpumask_t m;
    if (process_pid(h) != getpid()) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (!cpu_ids_to_mask(ids, n, &m)) return FALSE;
    if (set_affinity(0, &m)) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

/* No explicit CPU-set selection is recorded: report the empty set ("all"). */
API BOOL GetThreadSelectedCpuSets(HANDLE h, ULONG *ids, ULONG n, ULONG *req)
{
    (void)ids; (void)n;
    if (!thread_handle_ok(h) || !req) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *req = 0;
    return TRUE;
}

API BOOL GetProcessDefaultCpuSets(HANDLE h, ULONG *ids, ULONG n, ULONG *req)
{
    (void)ids; (void)n;
    if (process_pid(h) < 0 || !req) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *req = 0;
    return TRUE;
}

/* ---- NUMA ---- */

API BOOL GetNumaHighestNodeNumber(ULONG *n)
{
    topo();
    if (!n) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *n = (ULONG)T.max_node;
    return TRUE;
}

static int node_ok(int n) { topo(); return n >= 0 && n < MAXNODE && T.node_present[n]; }

static cpumask_t node_online(int n)
{
    cpumask_t m = T.node_cpus[n];
    for (int g = 0; g < MAXGROUP; g++) m.m[g] &= T.online.m[g];
    return m;
}

API BOOL GetNumaNodeProcessorMaskEx(USHORT node, GROUP_AFFINITY *aff)
{
    if (!node_ok(node) || !aff) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    cpumask_t m = node_online(node);
    int g = 0;
    while (g < MAXGROUP - 1 && !m.m[g]) g++;
    memset(aff, 0, sizeof *aff);
    aff->Mask = m.m[g];
    aff->Group = (USHORT)g;
    return TRUE;
}

API BOOL GetNumaNodeProcessorMask2(USHORT node, GROUP_AFFINITY *arr, USHORT count, USHORT *req)
{
    if (!node_ok(node) || !req) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    cpumask_t m = node_online(node);
    USHORT gs[MAXGROUP];
    int n = mask_groups(&m, gs);
    *req = (USHORT)n;
    if (count < n || (n && !arr)) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (int i = 0; i < n; i++) { memset(&arr[i], 0, sizeof arr[i]); arr[i].Mask = m.m[gs[i]]; arr[i].Group = gs[i]; }
    return TRUE;
}

API BOOL GetNumaNodeProcessorMask(UCHAR node, ULONGLONG *mask)
{
    if (!node_ok(node) || !mask) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *mask = node_online(node).m[0];
    return TRUE;
}

API BOOL GetNumaProcessorNode(UCHAR proc, UCHAR *node)
{
    topo();
    if (!node) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!mask_has(&T.online, proc)) { *node = 0xFF; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *node = (UCHAR)T.node[proc];
    return TRUE;
}

API BOOL GetNumaProcessorNodeEx(PROCESSOR_NUMBER *pn, USHORT *node)
{
    topo();
    if (!pn || !node) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int c = pn->Group * 64 + pn->Number;
    if (pn->Number >= 64 || !mask_has(&T.online, c)) { *node = 0xFFFF; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *node = (USHORT)T.node[c];
    return TRUE;
}

static BOOL numa_free(int node, ULONGLONG *bytes)
{
    if (!node_ok(node) || !bytes) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char p[64];
    size_t len;
    snprintf(p, sizeof p, "/sys/devices/system/node/node%d/meminfo", node);
    char *t = (char *)read_file(p, &len);
    char key[32];
    snprintf(key, sizeof key, "Node %d MemFree:", node);
    if (t && strstr(t, key)) *bytes = meminfo_kb(t, key) * 1024;
    else { struct sysinfo si; sysinfo(&si); *bytes = (ULONGLONG)si.freeram * si.mem_unit; }
    free(t);
    return TRUE;
}

API BOOL GetNumaAvailableMemoryNode(UCHAR node, ULONGLONG *bytes) { return numa_free(node, bytes); }
API BOOL GetNumaAvailableMemoryNodeEx(USHORT node, ULONGLONG *bytes) { return numa_free(node, bytes); }

API BOOL GetNumaNodeNumberFromHandle(HANDLE h, USHORT *node)
{
    if (!lsiw_handle_get(h, -1) || !node) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *node = 0;
    return TRUE;
}

/* Linux exposes ACPI proximity domains as node numbers. */
API BOOL GetNumaProximityNodeEx(ULONG prox, USHORT *node)
{
    if (!node_ok((int)prox) || !node) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *node = (USHORT)prox;
    return TRUE;
}

API BOOL GetNumaProximityNode(ULONG prox, UCHAR *node)
{
    USHORT n;
    if (!GetNumaProximityNodeEx(prox, &n)) return FALSE;
    if (node) *node = (UCHAR)n;
    return TRUE;
}

/* ================================================================== */
/* Extended processor state (XState) and thread contexts                */
/* ================================================================== */

/* Contexts with CONTEXT_XSTATE carry a CONTEXT_EX right after the CONTEXT
 * and an XSAVE area (standard, non-compacted layout: the XSAVE header
 * followed by each feature at its CPUID leaf 0xD offset minus the 512-byte
 * legacy region, which lives in CONTEXT.FltSave). */
#define CONTEXT_AMD64           0x00100000u
#define CONTEXT_CONTROL         (CONTEXT_AMD64 | 0x01)
#define CONTEXT_INTEGER         (CONTEXT_AMD64 | 0x02)
#define CONTEXT_SEGMENTS        (CONTEXT_AMD64 | 0x04)
#define CONTEXT_FLOATING_POINT  (CONTEXT_AMD64 | 0x08)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x10)
#define CONTEXT_XSTATE          (CONTEXT_AMD64 | 0x40)
#define XSTATE_MASK_LEGACY      3ull
#define CONTEXT_EX_SIZE         32

typedef struct { LONG Offset; DWORD Length; } CONTEXT_CHUNK;
typedef struct { CONTEXT_CHUNK All, Legacy, XState, KernelCet; } CONTEXT_EX;
_Static_assert(sizeof(CONTEXT_EX) == CONTEXT_EX_SIZE, "CONTEXT_EX");

static uint64_t xcr0(void)
{
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))) return XSTATE_MASK_LEGACY;   /* no OSXSAVE */
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

/* Features Windows enables for user code: x87, SSE, AVX and AVX-512. */
static uint64_t enabled_features(void) { return (xcr0() & 0xE7ull) | XSTATE_MASK_LEGACY; }

static void xfeature(int id, uint32_t *off, uint32_t *size)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(0xD, id, a, b, c, d);
    *size = a;
    *off = b;
}

/* Bytes of the XState chunk: header + every enabled feature above SSE. */
static DWORD xstate_area_size(void)
{
    uint64_t f = enabled_features();
    uint32_t end = 512 + 64;
    for (int id = 2; id < 64; id++) {
        if (!(f >> id & 1)) continue;
        uint32_t off, size;
        xfeature(id, &off, &size);
        if (off + size > end) end = off + size;
    }
    return end - 512;
}

API DWORD64 GetEnabledXStateFeatures(void) { return enabled_features(); }

static CONTEXT_EX *ctx_ex(CONTEXT *c)
{
    return (c->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE ? (CONTEXT_EX *)((uint8_t *)c + sizeof(CONTEXT)) : NULL;
}

static uint8_t *ctx_xsave(CONTEXT *c, DWORD *len)
{
    CONTEXT_EX *ex = ctx_ex(c);
    if (!ex || !ex->XState.Length) return NULL;
    if (len) *len = ex->XState.Length;
    return (uint8_t *)ex + ex->XState.Offset;
}

API BOOL InitializeContext2(void *buf, DWORD flags, CONTEXT **ctx, DWORD *len, DWORD64 compaction)
{
    (void)compaction;     /* the standard layout is always used */
    if (!len || !(flags & CONTEXT_AMD64)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int xs = (flags & CONTEXT_XSTATE) == CONTEXT_XSTATE;
    if (xs && !(enabled_features() & ~XSTATE_MASK_LEGACY)) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
    DWORD xsize = xs ? xstate_area_size() : 0;
    DWORD need = 15 + sizeof(CONTEXT) + CONTEXT_EX_SIZE + (xs ? 63 + xsize : 0);
    if (!buf || *len < need) { *len = need; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    CONTEXT *c = (CONTEXT *)(((uintptr_t)buf + 15) & ~(uintptr_t)15);
    memset(c, 0, sizeof *c + CONTEXT_EX_SIZE);
    c->ContextFlags = flags;
    CONTEXT_EX *ex = (CONTEXT_EX *)((uint8_t *)c + sizeof *c);
    uint8_t *x = xs ? (uint8_t *)(((uintptr_t)ex + CONTEXT_EX_SIZE + 63) & ~(uintptr_t)63) : (uint8_t *)ex + CONTEXT_EX_SIZE;
    if (xs) memset(x, 0, xsize);
    ex->Legacy.Offset = -(LONG)sizeof(CONTEXT);
    ex->Legacy.Length = sizeof(CONTEXT);
    ex->XState.Offset = (LONG)(x - (uint8_t *)ex);
    ex->XState.Length = xsize;
    ex->All.Offset = -(LONG)sizeof(CONTEXT);
    ex->All.Length = (DWORD)(x + xsize - (uint8_t *)c);
    if (ctx) *ctx = c;
    *len = need;
    return TRUE;
}

API BOOL InitializeContext(void *buf, DWORD flags, CONTEXT **ctx, DWORD *len) { return InitializeContext2(buf, flags, ctx, len, 0); }

API void *LocateXStateFeature(CONTEXT *c, DWORD id, DWORD *len)
{
    if (!c || !(c->ContextFlags & CONTEXT_AMD64)) return NULL;
    if (id == 0) { if (len) *len = 160; return c->FltSave; }          /* x87 state */
    if (id == 1) { if (len) *len = 256; return c->FltSave + 160; }    /* XMM0-15 */
    DWORD area;
    uint8_t *x = ctx_xsave(c, &area);
    if (!x || id >= 64 || !(enabled_features() >> id & 1)) return NULL;
    uint32_t off, size;
    xfeature((int)id, &off, &size);
    if (off < 512 || off - 512 + size > area) return NULL;
    if (len) *len = size;
    return x + (off - 512);
}

API BOOL GetXStateFeaturesMask(CONTEXT *c, DWORD64 *mask)
{
    uint8_t *x = c ? ctx_xsave(c, NULL) : NULL;
    if (!x || !mask) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uint64_t m;
    memcpy(&m, x, 8);
    *mask = (m & ~XSTATE_MASK_LEGACY) | ((c->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT ? XSTATE_MASK_LEGACY : 0);
    return TRUE;
}

API BOOL SetXStateFeaturesMask(CONTEXT *c, DWORD64 mask)
{
    uint8_t *x = c ? ctx_xsave(c, NULL) : NULL;
    if (!x || (mask & ~enabled_features())) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (mask & XSTATE_MASK_LEGACY) c->ContextFlags |= CONTEXT_FLOATING_POINT;
    else c->ContextFlags &= ~(CONTEXT_FLOATING_POINT & ~CONTEXT_AMD64);
    uint64_t m = mask & ~XSTATE_MASK_LEGACY;
    memcpy(x, &m, 8);
    return TRUE;
}

/* Copy the parts named by `flags` that the source holds. */
static BOOL copy_context(CONTEXT *d, DWORD flags, CONTEXT *s)
{
    DWORD parts = flags & s->ContextFlags & 0x5F;
    if (parts & 0x01) { d->SegCs = s->SegCs; d->SegSs = s->SegSs; d->EFlags = s->EFlags; d->Rsp = s->Rsp; d->Rip = s->Rip; }
    if (parts & 0x02) {
        d->Rax = s->Rax; d->Rcx = s->Rcx; d->Rdx = s->Rdx; d->Rbx = s->Rbx; d->Rbp = s->Rbp; d->Rsi = s->Rsi; d->Rdi = s->Rdi;
        d->R8 = s->R8; d->R9 = s->R9; d->R10 = s->R10; d->R11 = s->R11;
        d->R12 = s->R12; d->R13 = s->R13; d->R14 = s->R14; d->R15 = s->R15;
    }
    if (parts & 0x04) { d->SegDs = s->SegDs; d->SegEs = s->SegEs; d->SegFs = s->SegFs; d->SegGs = s->SegGs; }
    if (parts & 0x08) { d->MxCsr = s->MxCsr; memcpy(d->FltSave, s->FltSave, sizeof d->FltSave); }
    if (parts & 0x10) { d->Dr0 = s->Dr0; d->Dr1 = s->Dr1; d->Dr2 = s->Dr2; d->Dr3 = s->Dr3; d->Dr6 = s->Dr6; d->Dr7 = s->Dr7; }
    if (parts & 0x40) {
        DWORD sl, dl;
        uint8_t *sx = ctx_xsave(s, &sl), *dx = ctx_xsave(d, &dl);
        if (!sx || !dx) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
        uint64_t m;
        memcpy(&m, sx, 8);
        for (int id = 2; id < 64; id++) {
            if (!(m >> id & 1)) continue;
            uint32_t off, size;
            xfeature(id, &off, &size);
            if (off - 512 + size > dl || off - 512 + size > sl) { lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
            memcpy(dx + off - 512, sx + off - 512, size);
        }
        memcpy(dx, &m, 8);
    }
    d->ContextFlags |= CONTEXT_AMD64 | parts;
    return TRUE;
}

API BOOL CopyContext(CONTEXT *d, DWORD flags, CONTEXT *s)
{
    if (!d || !s || !(s->ContextFlags & CONTEXT_AMD64)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return copy_context(d, flags, s);
}

/* Save the calling thread's extended state into a context's XSAVE area. */
static void capture_xstate(CONTEXT *c)
{
    DWORD area;
    uint8_t *x = ctx_xsave(c, &area);
    if (!x) return;
    uint64_t f = enabled_features() & ~XSTATE_MASK_LEGACY;
    size_t full = 512 + area;
    uint8_t *tmp = aligned_alloc(64, (full + 63) & ~(size_t)63);
    if (!tmp) return;
    memset(tmp, 0, full);
    __asm__ volatile("xsave %0" : "+m"(*(uint8_t(*)[full])tmp) : "a"((uint32_t)f), "d"((uint32_t)(f >> 32)) : "memory");
    memcpy(x, tmp + 512, area);
    uint64_t bv;
    memcpy(&bv, x, 8);
    bv &= f;
    memcpy(x, &bv, 8);
    memset(x + 8, 0, 8);                 /* standard format: no compaction */
    free(tmp);
}

/* Contexts of the calling thread are captured (as RtlCaptureContext does);
 * other threads cannot be stopped and inspected from here. */
API BOOL GetThreadContext(HANDLE h, CONTEXT *ctx)
{
    if (!ctx || !(ctx->ContextFlags & CONTEXT_AMD64)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (h != PSEUDO_THREAD && !lsiw_handle_get(h, H_THREAD)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (h != PSEUDO_THREAD && thread_tid(h) != lsiw_thread()->tid) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    k32_ready();
    if (!k32.RtlCaptureContext) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
    CONTEXT cur;
    memset(&cur, 0, sizeof cur);
    k32.RtlCaptureContext(&cur);
    cur.ContextFlags |= CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT | CONTEXT_DEBUG_REGISTERS;
    DWORD want = ctx->ContextFlags;
    if (!copy_context(ctx, want & ~(CONTEXT_XSTATE & ~CONTEXT_AMD64), &cur)) return FALSE;
    if ((want & CONTEXT_XSTATE) == CONTEXT_XSTATE) { ctx->ContextFlags = want; capture_xstate(ctx); }
    return TRUE;
}

API BOOL SetThreadContext(HANDLE h, const CONTEXT *ctx)
{
    if (!ctx || !(ctx->ContextFlags & CONTEXT_AMD64)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (h != PSEUDO_THREAD && !lsiw_handle_get(h, H_THREAD)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (h != PSEUDO_THREAD && thread_tid(h) != lsiw_thread()->tid) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    /* Clearing the debug registers is a no-op (none are ever set); moving
     * the calling thread to another context or arming hardware breakpoints
     * is not possible from here. */
    if ((ctx->ContextFlags & ~CONTEXT_AMD64) == 0x10 && !ctx->Dr0 && !ctx->Dr1 && !ctx->Dr2 && !ctx->Dr3 && !(ctx->Dr7 & 0xFF))
        return TRUE;
    lsiw_set_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* No thread is a WOW64 (32-bit) thread. */
API BOOL Wow64GetThreadContext(HANDLE h, void *ctx) { (void)h; (void)ctx; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
API BOOL Wow64SetThreadContext(HANDLE h, const void *ctx) { (void)h; (void)ctx; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }

/* ================================================================== */
/* Firmware tables                                                      */
/* ================================================================== */

#define FW_RSMB 0x52534D42u          /* 'RSMB' */
#define FW_ACPI 0x41435049u          /* 'ACPI' */
#define FW_FIRM 0x4649524Du          /* 'FIRM' */

/* Raw SMBIOS: the kernel's copy when readable (root only on most systems),
 * otherwise BIOS / system / baseboard / chassis structures built from the
 * world-readable /sys/class/dmi/id values. */
struct sbuf { uint8_t *p; size_t n, cap; };

static void sb_put(struct sbuf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) abort(); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static const char *dmi_id(const char *key, char *buf, size_t size)
{
    char p[96];
    snprintf(p, sizeof p, "/sys/class/dmi/id/%s", key);
    return read_text(p, buf, size) > 0 && *buf ? buf : NULL;
}

/* Append one structure: formatted area, then its string set. */
static void smbios_struct(struct sbuf *b, uint8_t *fmt, size_t flen, const char **strs, int nstr)
{
    sb_put(b, fmt, flen);
    int any = 0;
    for (int i = 0; i < nstr; i++)
        if (strs[i] && *strs[i]) { sb_put(b, strs[i], strlen(strs[i]) + 1); any = 1; }
    if (!any) sb_put(b, "", 1);
    sb_put(b, "", 1);
}

static uint8_t *smbios_synth(size_t *len, uint8_t *major, uint8_t *minor)
{
    char v[8][128];
    const char *bios_vendor = dmi_id("bios_vendor", v[0], 128), *bios_ver = dmi_id("bios_version", v[1], 128),
               *bios_date = dmi_id("bios_date", v[2], 128), *sys_vendor = dmi_id("sys_vendor", v[3], 128),
               *product = dmi_id("product_name", v[4], 128), *pver = dmi_id("product_version", v[5], 128),
               *board_vendor = dmi_id("board_vendor", v[6], 128), *board = dmi_id("board_name", v[7], 128);
    char ct[16], fam[128], sku[128];
    const char *family = dmi_id("product_family", fam, sizeof fam), *skus = dmi_id("product_sku", sku, sizeof sku);
    if (!bios_vendor && !sys_vendor && !product && !board_vendor) return NULL;
    struct sbuf b = { 0 };
    uint8_t idx;
#define STR(field, s) (fmt[field] = (s) ? idx++ : 0)
    {   /* type 0: BIOS information */
        uint8_t fmt[0x18] = { 0, 0x18, 0, 0 };
        idx = 1;
        STR(4, bios_vendor); STR(5, bios_ver);
        fmt[6] = 0x00; fmt[7] = 0xE8;                 /* starting segment E800 */
        STR(8, bios_date);
        fmt[0x0A] = 0x08;                             /* characteristics not supported */
        fmt[0x14] = fmt[0x15] = fmt[0x16] = fmt[0x17] = 0xFF;
        const char *s[] = { bios_vendor, bios_ver, bios_date };
        smbios_struct(&b, fmt, sizeof fmt, s, 3);
    }
    {   /* type 1: system information (UUID is root-only: left zero) */
        uint8_t fmt[0x1B] = { 1, 0x1B, 1, 0 };
        idx = 1;
        STR(4, sys_vendor); STR(5, product); STR(6, pver);
        fmt[0x18] = 6;                                /* wake-up: power switch */
        STR(0x19, skus); STR(0x1A, family);
        const char *s[] = { sys_vendor, product, pver, skus, family };
        smbios_struct(&b, fmt, sizeof fmt, s, 5);
    }
    {   /* type 2: baseboard */
        uint8_t fmt[0x0F] = { 2, 0x0F, 2, 0 };
        idx = 1;
        STR(4, board_vendor); STR(5, board);
        fmt[9] = 1;                                   /* hosting board */
        fmt[0x0B] = 3;                                /* chassis handle */
        fmt[0x0D] = 0x0A;                             /* motherboard */
        const char *s[] = { board_vendor, board };
        smbios_struct(&b, fmt, sizeof fmt, s, 2);
    }
    {   /* type 3: chassis */
        uint8_t fmt[0x0D] = { 3, 0x0D, 3, 0 };
        idx = 1;
        STR(4, sys_vendor);
        fmt[5] = (uint8_t)(dmi_id("chassis_type", ct, sizeof ct) ? atoi(ct) : 2);
        fmt[9] = fmt[0x0A] = fmt[0x0B] = fmt[0x0C] = 3;   /* safe */
        const char *s[] = { sys_vendor };
        smbios_struct(&b, fmt, sizeof fmt, s, 1);
    }
#undef STR
    uint8_t end[4] = { 127, 4, 4, 0 };
    sb_put(&b, end, 4);
    sb_put(&b, "\0", 2);
    *major = 2; *minor = 8;
    *len = b.n;
    return b.p;
}

static uint8_t *smbios_table(size_t *len, uint8_t *major, uint8_t *minor)
{
    size_t el;
    uint8_t *ep = read_file("/sys/firmware/dmi/tables/smbios_entry_point", &el);
    uint8_t *t = ep ? read_file("/sys/firmware/dmi/tables/DMI", len) : NULL;
    if (t) {
        if (el >= 8 && !memcmp(ep, "_SM3_", 5)) { *major = ep[7]; *minor = ep[8]; }
        else if (el >= 8 && !memcmp(ep, "_SM_", 4)) { *major = ep[6]; *minor = ep[7]; }
        else { *major = 2; *minor = 8; }
        free(ep);
        return t;
    }
    free(ep);
    return smbios_synth(len, major, minor);
}

static const char *acpi_name(DWORD id, char out[5])
{
    memcpy(out, &id, 4);         /* table id = signature bytes in memory order */
    out[4] = 0;
    return out;
}

API UINT EnumSystemFirmwareTables(DWORD provider, void *buf, DWORD size)
{
    struct sbuf b = { 0 };
    if (provider == FW_RSMB) {
        DWORD id = 0;
        sb_put(&b, &id, 4);
    } else if (provider == FW_ACPI) {
        DIR *d = opendir("/sys/firmware/acpi/tables");
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (strlen(de->d_name) != 4) continue;
            DWORD id;
            memcpy(&id, de->d_name, 4);
            sb_put(&b, &id, 4);
        }
        if (d) closedir(d);
    } else if (provider != FW_FIRM) {
        lsiw_set_last_error(ERROR_INVALID_FUNCTION);
        return 0;
    }
    UINT need = (UINT)b.n;
    if (buf && size >= need) memcpy(buf, b.p, need);
    else if (need) lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER);
    free(b.p);
    return need;
}

API UINT GetSystemFirmwareTable(DWORD provider, DWORD id, void *buf, DWORD size)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (provider == FW_RSMB) {
        uint8_t major, minor;
        uint8_t *t = smbios_table(&len, &major, &minor);
        if (!t) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
        /* RawSMBIOSData { Used20CallingMethod, Major, Minor, DmiRevision, DWORD Length, data } */
        data = xmalloc0(8 + len);
        data[1] = major;
        data[2] = minor;
        DWORD l = (DWORD)len;
        memcpy(data + 4, &l, 4);
        memcpy(data + 8, t, len);
        free(t);
        len += 8;
    } else if (provider == FW_ACPI) {
        char n[5], p[64];
        snprintf(p, sizeof p, "/sys/firmware/acpi/tables/%s", acpi_name(id, n));
        data = read_file(p, &len);
        if (!data) { lsiw_set_last_error(errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_NOT_FOUND); return 0; }
    } else if (provider == FW_FIRM) {
        lsiw_set_last_error(ERROR_NOT_SUPPORTED);
        return 0;
    } else {
        lsiw_set_last_error(ERROR_INVALID_FUNCTION);
        return 0;
    }
    UINT need = (UINT)len;
    if (buf && size >= need) memcpy(buf, data, len);
    else lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER);
    free(data);
    return need;
}

/* FIRMWARE_TYPE: FirmwareTypeBios 1, FirmwareTypeUefi 2. */
API BOOL GetFirmwareType(int *type)
{
    if (!type) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct stat st;
    *type = stat("/sys/firmware/efi", &st) == 0 ? 2 : 1;
    return TRUE;
}

/* UEFI variables need SE_SYSTEM_ENVIRONMENT_NAME; on BIOS machines the call
 * fails with ERROR_INVALID_FUNCTION (programs use that to detect UEFI). */
static DWORD firmware_env_error(void)
{
    struct stat st;
    DWORD e = stat("/sys/firmware/efi", &st) == 0 ? ERROR_PRIVILEGE_NOT_HELD : ERROR_INVALID_FUNCTION;
    lsiw_set_last_error(e);
    return 0;
}

API DWORD GetFirmwareEnvironmentVariableW(const WCHAR *n, const WCHAR *g, void *b, DWORD s) { (void)n; (void)g; (void)b; (void)s; return firmware_env_error(); }
API DWORD GetFirmwareEnvironmentVariableA(const char *n, const char *g, void *b, DWORD s) { (void)n; (void)g; (void)b; (void)s; return firmware_env_error(); }
API BOOL SetFirmwareEnvironmentVariableW(const WCHAR *n, const WCHAR *g, void *b, DWORD s) { (void)n; (void)g; (void)b; (void)s; return firmware_env_error(); }
API BOOL SetFirmwareEnvironmentVariableA(const char *n, const char *g, void *b, DWORD s) { (void)n; (void)g; (void)b; (void)s; return firmware_env_error(); }

/* ================================================================== */
/* Power                                                                */
/* ================================================================== */

typedef struct {
    BYTE ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag;
    DWORD BatteryLifeTime, BatteryFullLifeTime;
} SYSTEM_POWER_STATUS;

API BOOL GetSystemPowerStatus(SYSTEM_POWER_STATUS *ps)
{
    if (!ps) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int mains = -1, batteries = 0, charging = 0;
    long cap_sum = 0;
    double now_sum = 0, full_sum = 0, rate_sum = 0;
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[512], b[64];
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", de->d_name);
        if (read_text(p, b, sizeof b) <= 0) continue;
        if (!strcmp(b, "Mains")) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", de->d_name);
            if (read_long(p, 0)) mains = 1; else if (mains < 0) mains = 0;
        } else if (!strcmp(b, "Battery")) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", de->d_name);
            if (read_text(p, b, sizeof b) > 0 && !strcmp(b, "Device")) continue;   /* mouse, headset... */
            batteries++;
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", de->d_name);
            cap_sum += read_long(p, 0);
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", de->d_name);
            if (read_text(p, b, sizeof b) > 0 && !strcmp(b, "Charging")) charging = 1;
            const char *now_k[] = { "energy_now", "charge_now" }, *full_k[] = { "energy_full", "charge_full" },
                       *rate_k[] = { "power_now", "current_now" };
            for (int k = 0; k < 2; k++) {
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/%s", de->d_name, now_k[k]);
                long nv = read_long(p, -1);
                if (nv < 0) continue;
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/%s", de->d_name, full_k[k]);
                full_sum += (double)read_long(p, 0);
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/%s", de->d_name, rate_k[k]);
                rate_sum += (double)labs(read_long(p, 0));
                now_sum += (double)nv;
                break;
            }
        }
    }
    if (d) closedir(d);
    memset(ps, 0, sizeof *ps);
    ps->BatteryLifeTime = ps->BatteryFullLifeTime = 0xFFFFFFFFu;
    if (!batteries) {
        ps->ACLineStatus = mains == 0 ? 0 : 1;        /* desktops: on AC */
        ps->BatteryFlag = 128;                        /* no system battery */
        ps->BatteryLifePercent = 255;
        return TRUE;
    }
    int pct = (int)(cap_sum / batteries);
    ps->ACLineStatus = mains < 0 ? (charging ? 1 : 255) : (BYTE)mains;
    ps->BatteryLifePercent = (BYTE)(pct > 100 ? 100 : pct);
    ps->BatteryFlag = (BYTE)((pct > 66 ? 1 : pct < 33 ? 2 : 0) | (pct < 5 ? 4 : 0) | (charging ? 8 : 0));
    if (!charging && ps->ACLineStatus != 1 && rate_sum > 0) {
        ps->BatteryLifeTime = (DWORD)(now_sum / rate_sum * 3600.0);
        if (full_sum > 0) ps->BatteryFullLifeTime = (DWORD)(full_sum / rate_sum * 3600.0);
    }
    return TRUE;
}

/* Execution-state and power requests are recorded but do not inhibit the
 * Linux desktop's sleep or screen saver. */
#define ES_SYSTEM_REQUIRED   0x00000001u
#define ES_DISPLAY_REQUIRED  0x00000002u
#define ES_USER_PRESENT      0x00000004u
#define ES_AWAYMODE_REQUIRED 0x00000040u
#define ES_CONTINUOUS        0x80000000u

API DWORD SetThreadExecutionState(DWORD flags)
{
    static __thread DWORD state = ES_CONTINUOUS;
    if (flags & ~(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED | ES_USER_PRESENT | ES_AWAYMODE_REQUIRED | ES_CONTINUOUS)) return 0;
    if ((flags & ES_AWAYMODE_REQUIRED) && !(flags & (ES_CONTINUOUS | ES_SYSTEM_REQUIRED))) return 0;
    DWORD prev = state;
    if (flags & ES_CONTINUOUS) state = flags;
    return prev;
}

struct power_req { int counts[4]; };
static void power_req_free(void *p) { free(p); }

typedef struct { ULONG Version; DWORD Flags; void *Reason[3]; } REASON_CONTEXT;

API HANDLE PowerCreateRequest(REASON_CONTEXT *rc)
{
    if (!rc || rc->Version != 0 || !(rc->Flags & 0x3)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    HANDLE h = gobj_create(xmalloc0(sizeof(struct power_req)), power_req_free);
    return h ? h : INVALID_HANDLE_VALUE;
}

static BOOL power_req(HANDLE h, int type, int delta)
{
    struct power_req *r = gobj_get(h, power_req_free);
    if (!r) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (type < 0 || type > 3) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (delta < 0 && r->counts[type] == 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    r->counts[type] += delta;
    return TRUE;
}

API BOOL PowerSetRequest(HANDLE h, int type) { return power_req(h, type, 1); }
API BOOL PowerClearRequest(HANDLE h, int type) { return power_req(h, type, -1); }

/* ---- memory resource notifications ---- */

/* The handles are manual-reset events kept in step with the available
 * memory by a monitor thread, so they work in WaitForMultipleObjects (the
 * .NET finalizer thread waits on the low-memory one). Thresholds as on
 * Windows: low = 32 MB per 4 GB of RAM, at most 64 MB; high = 3 x low. */
struct memnote { struct memnote *next; struct lsiw_handle_obj *obj; int high; };
static struct memnote *g_memnotes;
static pthread_mutex_t g_memnote_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_memnote_thread;

static int mem_state(int high)
{
    size_t len;
    char *mi = (char *)read_file("/proc/meminfo", &len);
    uint64_t total = meminfo_kb(mi, "MemTotal:") * 1024, avail = meminfo_kb(mi, "MemAvailable:") * 1024;
    free(mi);
    uint64_t low = total / 4096 * 32;
    if (low > 64ull << 20) low = 64ull << 20;
    return high ? avail >= 3 * low : avail < low;
}

static void memnote_update_locked(void)
{
    int st[2] = { mem_state(0), mem_state(1) };
    lsiw_wait_lock();
    for (struct memnote *n = g_memnotes; n; n = n->next) n->obj->signaled = st[n->high];
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
}

static void *memnote_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&g_memnote_lock);
        if (!g_memnotes) { g_memnote_thread = 0; pthread_mutex_unlock(&g_memnote_lock); return NULL; }
        memnote_update_locked();
        pthread_mutex_unlock(&g_memnote_lock);
    }
}

static void memnote_free(void *p)
{
    pthread_mutex_lock(&g_memnote_lock);
    for (struct memnote **pp = &g_memnotes; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = ((struct memnote *)p)->next; break; }
    pthread_mutex_unlock(&g_memnote_lock);
    free(p);
}

API HANDLE CreateMemoryResourceNotification(int type)
{
    if (type != 0 && type != 1) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_EVENT, &o);
    if (!h) return NULL;
    struct memnote *n = xmalloc0(sizeof *n);
    n->obj = o;
    n->high = type;
    o->manual_reset = 1;
    o->data = n;
    o->free_data = memnote_free;
    pthread_mutex_lock(&g_memnote_lock);
    n->next = g_memnotes;
    g_memnotes = n;
    memnote_update_locked();
    if (!g_memnote_thread) g_memnote_thread = start_host_thread(memnote_thread, NULL);
    pthread_mutex_unlock(&g_memnote_lock);
    return h;
}

API BOOL QueryMemoryResourceNotification(HANDLE h, BOOL *state)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_EVENT);
    if (!o || o->free_data != memnote_free) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!state) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *state = mem_state(((struct memnote *)o->data)->high);
    return TRUE;
}

/* ================================================================== */
/* AppModel: an unpackaged desktop program                              */
/* ================================================================== */

API LONG GetCurrentPackageId(UINT *len, BYTE *buf) { (void)buf; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetCurrentPackageFullName(UINT *len, WCHAR *buf) { (void)buf; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetCurrentPackageFamilyName(UINT *len, WCHAR *buf) { (void)buf; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetCurrentPackagePath(UINT *len, WCHAR *buf) { (void)buf; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetCurrentPackageInfo(UINT flags, UINT *len, BYTE *buf, UINT *count)
{ (void)flags; (void)buf; (void)count; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetCurrentApplicationUserModelId(UINT *len, WCHAR *buf) { (void)buf; return len ? APPMODEL_ERROR_NO_APPLICATION : ERROR_INVALID_PARAMETER; }

API LONG GetPackageFullName(HANDLE proc, UINT *len, WCHAR *buf)
{ (void)buf; if (process_pid(proc) < 0) return ERROR_INVALID_HANDLE; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetPackageFamilyName(HANDLE proc, UINT *len, WCHAR *buf)
{ (void)buf; if (process_pid(proc) < 0) return ERROR_INVALID_HANDLE; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetPackageId(HANDLE proc, UINT *len, BYTE *buf)
{ (void)buf; if (process_pid(proc) < 0) return ERROR_INVALID_HANDLE; return len ? APPMODEL_ERROR_NO_PACKAGE : ERROR_INVALID_PARAMETER; }
API LONG GetApplicationUserModelId(HANDLE proc, UINT *len, WCHAR *buf)
{ (void)buf; if (process_pid(proc) < 0) return ERROR_INVALID_HANDLE; return len ? APPMODEL_ERROR_NO_APPLICATION : ERROR_INVALID_PARAMETER; }

/* No package is installed, so no full name resolves to a path. */
API LONG GetPackagePathByFullName(const WCHAR *name, UINT *len, WCHAR *buf)
{ (void)buf; return name && len ? ERROR_NOT_FOUND : ERROR_INVALID_PARAMETER; }

API LONG GetPackagesByPackageFamily(const WCHAR *family, UINT *count, WCHAR **names, UINT *buflen, WCHAR *buf)
{
    (void)names; (void)buf;
    if (!family || !count || !buflen) return ERROR_INVALID_PARAMETER;
    *count = 0;
    *buflen = 0;
    return ERROR_SUCCESS;
}

API HMODULE LoadPackagedLibrary(const WCHAR *name, DWORD reserved)
{
    (void)name;
    lsiw_set_last_error(reserved ? ERROR_INVALID_PARAMETER : APPMODEL_ERROR_NO_PACKAGE);
    return NULL;
}

/* AppPolicy* (imported by the UCRT and the C++ runtime through
 * api-ms-win-appmodel-runtime): answers for a classic desktop process. */
API LONG AppPolicyGetProcessTerminationMethod(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 0; /* ExitProcess */ return ERROR_SUCCESS; }
API LONG AppPolicyGetThreadInitializationType(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 0; /* None */ return ERROR_SUCCESS; }
API LONG AppPolicyGetShowDeveloperDiagnostic(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 1; /* ShowUI */ return ERROR_SUCCESS; }
API LONG AppPolicyGetWindowingModel(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 2; /* ClassicDesktop */ return ERROR_SUCCESS; }
API LONG AppPolicyGetMediaFoundationCodecLoading(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 0; /* All */ return ERROR_SUCCESS; }
API LONG AppPolicyGetClrCompat(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 1; /* ClassicDesktop */ return ERROR_SUCCESS; }
API LONG AppPolicyGetCreateFileAccess(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 0; /* Full */ return ERROR_SUCCESS; }
API LONG AppPolicyGetLifecycleManagement(HANDLE t, int *v) { (void)t; if (!v) return ERROR_INVALID_PARAMETER; *v = 0; /* Unmanaged */ return ERROR_SUCCESS; }

/* ================================================================== */
/* Windows Error Reporting, recovery and restart                        */
/* ================================================================== */

/* There is no WER service; registrations are kept so that unregistering
 * reports "not found" for things never registered, as Windows does. */
struct reg_item { struct reg_item *next; int kind; const void *key; WCHAR *name; };
static struct reg_item *g_regs;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static HRESULT reg_add(int kind, const void *key, const WCHAR *name)
{
    struct reg_item *r = xmalloc0(sizeof *r);
    r->kind = kind;
    r->key = key;
    if (name) { size_t n = wlen(name); r->name = xmalloc0((n + 1) * sizeof(WCHAR)); memcpy(r->name, name, n * sizeof(WCHAR)); }
    pthread_mutex_lock(&g_reg_lock);
    r->next = g_regs;
    g_regs = r;
    pthread_mutex_unlock(&g_reg_lock);
    return S_OK;
}

static HRESULT reg_del(int kind, const void *key, const WCHAR *name)
{
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    pthread_mutex_lock(&g_reg_lock);
    for (struct reg_item **pp = &g_regs; *pp; pp = &(*pp)->next) {
        struct reg_item *r = *pp;
        if (r->kind != kind || (name ? !r->name || wicmp(r->name, name) : r->key != key)) continue;
        *pp = r->next;
        free(r->name);
        free(r);
        hr = S_OK;
        break;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return hr;
}

enum { REG_MEMBLOCK, REG_FILE, REG_RTMODULE, REG_EXCLUDED, REG_METADATA, REG_DUMP };

API HRESULT WerRegisterMemoryBlock(void *p, DWORD size) { return p && size ? reg_add(REG_MEMBLOCK, p, NULL) : E_INVALIDARG; }
API HRESULT WerUnregisterMemoryBlock(void *p) { return reg_del(REG_MEMBLOCK, p, NULL); }
API HRESULT WerRegisterExcludedMemoryBlock(const void *p, DWORD size) { return p && size ? reg_add(REG_EXCLUDED, p, NULL) : E_INVALIDARG; }
API HRESULT WerUnregisterExcludedMemoryBlock(const void *p) { return reg_del(REG_EXCLUDED, p, NULL); }
API HRESULT WerRegisterFile(const WCHAR *path, int type, DWORD flags) { (void)type; (void)flags; return path ? reg_add(REG_FILE, NULL, path) : E_INVALIDARG; }
API HRESULT WerUnregisterFile(const WCHAR *path) { return path ? reg_del(REG_FILE, NULL, path) : E_INVALIDARG; }
API HRESULT WerRegisterRuntimeExceptionModule(const WCHAR *dll, void *ctx) { (void)ctx; return dll ? reg_add(REG_RTMODULE, NULL, dll) : E_INVALIDARG; }
API HRESULT WerUnregisterRuntimeExceptionModule(const WCHAR *dll, void *ctx) { (void)ctx; return dll ? reg_del(REG_RTMODULE, NULL, dll) : E_INVALIDARG; }
API HRESULT WerRegisterCustomMetadata(const WCHAR *key, const WCHAR *value) { return key && value ? reg_add(REG_METADATA, NULL, key) : E_INVALIDARG; }
API HRESULT WerUnregisterCustomMetadata(const WCHAR *key) { return key ? reg_del(REG_METADATA, NULL, key) : E_INVALIDARG; }
API HRESULT WerRegisterAppLocalDump(const WCHAR *dir) { return dir ? reg_add(REG_DUMP, NULL, dir) : E_INVALIDARG; }
API HRESULT WerUnregisterAppLocalDump(void) { return reg_del(REG_DUMP, NULL, NULL) == S_OK ? S_OK : S_OK; }

static struct { void *cb, *param; DWORD ping, flags; int set; } g_recovery;
static struct { WCHAR *cmd; DWORD flags; int set; } g_restart;

API HRESULT RegisterApplicationRecoveryCallback(void *cb, void *param, DWORD ping, DWORD flags)
{
    if (!cb || ping > 300000) return E_INVALIDARG;
    g_recovery.cb = cb; g_recovery.param = param; g_recovery.ping = ping ? ping : 5000; g_recovery.flags = flags; g_recovery.set = 1;
    return S_OK;
}

API HRESULT UnregisterApplicationRecoveryCallback(void) { g_recovery.set = 0; return S_OK; }

API HRESULT GetApplicationRecoveryCallback(HANDLE proc, void **cb, void **param, DWORD *ping, DWORD *flags)
{
    if (process_pid(proc) != getpid()) return E_INVALIDARG;
    if (!g_recovery.set) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (cb) *cb = g_recovery.cb;
    if (param) *param = g_recovery.param;
    if (ping) *ping = g_recovery.ping;
    if (flags) *flags = g_recovery.flags;
    return S_OK;
}

/* Only meaningful inside a recovery callback, which is never invoked. */
API HRESULT ApplicationRecoveryInProgress(BOOL *cancelled) { if (!cancelled) return E_INVALIDARG; *cancelled = FALSE; return S_OK; }
API void ApplicationRecoveryFinished(BOOL success) { (void)success; }

#define RESTART_MAX_CMD_LINE 1024

API HRESULT RegisterApplicationRestart(const WCHAR *cmd, DWORD flags)
{
    if ((cmd && wlen(cmd) >= RESTART_MAX_CMD_LINE) || (flags & ~0xFu)) return E_INVALIDARG;
    free(g_restart.cmd);
    g_restart.cmd = NULL;
    if (cmd) { size_t n = wlen(cmd); g_restart.cmd = xmalloc0((n + 1) * sizeof(WCHAR)); memcpy(g_restart.cmd, cmd, n * sizeof(WCHAR)); }
    g_restart.flags = flags;
    g_restart.set = 1;
    return S_OK;
}

API HRESULT UnregisterApplicationRestart(void) { g_restart.set = 0; return S_OK; }

API HRESULT GetApplicationRestartSettings(HANDLE proc, WCHAR *buf, DWORD *size, DWORD *flags)
{
    if (process_pid(proc) != getpid() || !size) return E_INVALIDARG;
    if (!g_restart.set) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    DWORD need = (DWORD)wlen(g_restart.cmd) + 1;
    if (!buf || *size < need) { *size = need; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); }
    if (g_restart.cmd) memcpy(buf, g_restart.cmd, need * sizeof(WCHAR)); else buf[0] = 0;
    *size = need;
    if (flags) *flags = g_restart.flags;
    return S_OK;
}

/* ================================================================== */
/* Miscellaneous                                                        */
/* ================================================================== */

/* Fail-fast: report and terminate immediately, without exception handlers,
 * unwinding or exit routines. */
API void RaiseFailFastException(EXCEPTION_RECORD *rec, CONTEXT *ctx, DWORD flags)
{
    (void)flags;
    DWORD code = rec ? rec->ExceptionCode : STATUS_STACK_BUFFER_OVERRUN;
    void *addr = rec ? rec->ExceptionAddress : ctx ? (void *)ctx->Rip : __builtin_return_address(0);
    fprintf(stderr, "lsiw: fail-fast exception 0x%08x at %p; terminating\n", (unsigned)code, addr);
    fflush(stderr);
    _exit(255);
}

/* (a * b) / c in 64 bits, rounded half away from zero; -1 on overflow or
 * division by zero. */
API int MulDiv(int a, int b, int c)
{
    if (c == 0) return -1;
    int64_t p = (int64_t)a * b;
    int neg = (p < 0) != (c < 0);
    uint64_t ap = p < 0 ? (uint64_t)(-p) : (uint64_t)p, ac = c < 0 ? (uint64_t)(-(int64_t)c) : (uint64_t)c;
    uint64_t q = (ap + ac / 2) / ac;
    if (q > (uint64_t)INT_MAX + (neg ? 1 : 0)) return -1;
    return neg ? (int)-(int64_t)q : (int)q;
}

/* There is no 32-bit file-system view to redirect. */
API BOOL Wow64DisableWow64FsRedirection(void **old) { if (old) *old = NULL; return TRUE; }
API BOOL Wow64RevertWow64FsRedirection(void *old) { (void)old; return TRUE; }
API BOOLEAN Wow64EnableWow64FsRedirection(BOOLEAN enable) { (void)enable; return TRUE; }

#define BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE  0x00000001
#define BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE 0x00010000
#define BASE_SEARCH_PATH_PERMANENT               0x00008000

static DWORD g_search_mode;

/* Recorded only: SearchPath (k32_file.c) always searches the documented
 * safe order. */
API BOOL SetSearchPathMode(DWORD flags)
{
    DWORD mode = flags & ~(DWORD)BASE_SEARCH_PATH_PERMANENT;
    if (mode != BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE && mode != BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (g_search_mode & BASE_SEARCH_PATH_PERMANENT) {
        if ((g_search_mode & ~(DWORD)BASE_SEARCH_PATH_PERMANENT) == mode) return TRUE;
        lsiw_set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    g_search_mode = flags;
    return TRUE;
}

/* Should CreateProcess / SearchPath look in the current directory for this
 * executable name? Not when the name has a path, or when the variable
 * NoDefaultCurrentDirectoryInExePath exists. */
static BOOL need_cwd(const char *exe)
{
    if (exe && (strchr(exe, '\\') || strchr(exe, '/'))) return TRUE;
    return lsiw_env_get("NoDefaultCurrentDirectoryInExePath") == NULL;
}

API BOOL NeedCurrentDirectoryForExePathW(const WCHAR *exe) { char *u = lsiw_w2u(exe); BOOL r = need_cwd(u); free(u); return r; }
API BOOL NeedCurrentDirectoryForExePathA(const char *exe) { return need_cwd(exe); }

/* The ANSI/OEM switch is recorded; both code pages map the same way here. */
static int g_file_apis_oem;
API BOOL AreFileApisANSI(void) { return !g_file_apis_oem; }
API void SetFileApisToANSI(void) { g_file_apis_oem = 0; }
API void SetFileApisToOEM(void) { g_file_apis_oem = 1; }

/* SetDllDirectory (k32_process.c) does not keep its argument, so there is
 * never a DLL directory to report: 0 and an empty string, as when none is set. */
API DWORD GetDllDirectoryW(DWORD len, WCHAR *buf) { if (buf && len) buf[0] = 0; lsiw_set_last_error(0); return 0; }
API DWORD GetDllDirectoryA(DWORD len, char *buf) { if (buf && len) buf[0] = 0; lsiw_set_last_error(0); return 0; }

/* Clock interrupt period of Windows (15.625 ms in 100 ns units); time is
 * disciplined by the host, so adjustment is reported as disabled. */
API BOOL GetSystemTimeAdjustment(DWORD *adj, DWORD *incr, BOOL *disabled)
{
    if (adj) *adj = 156250;
    if (incr) *incr = 156250;
    if (disabled) *disabled = TRUE;
    return TRUE;
}

API BOOL GetSystemTimeAdjustmentPrecise(DWORD64 *adj, DWORD64 *incr, BOOL *disabled)
{
    if (adj) *adj = 156250;
    if (incr) *incr = 156250;
    if (disabled) *disabled = TRUE;
    return TRUE;
}

API BOOL SetSystemTimeAdjustment(DWORD adj, BOOL disabled) { (void)adj; (void)disabled; lsiw_set_last_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
API BOOL SetSystemTimeAdjustmentPrecise(DWORD64 adj, BOOL disabled) { (void)adj; (void)disabled; lsiw_set_last_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }

/* MACHINE_ATTRIBUTES: UserEnabled 1, KernelEnabled 2, Wow64Container 4.
 * Only x86-64 code runs natively; there is no 32-bit (WOW64) support. */
API HRESULT GetMachineTypeAttributes(USHORT machine, int *attrs)
{
    if (!attrs) return E_INVALIDARG;
    *attrs = machine == IMAGE_FILE_MACHINE_AMD64 ? 3 : 0;
    return S_OK;
}

API HRESULT IsWow64GuestMachineSupported(USHORT machine, BOOL *supported)
{
    (void)machine;
    if (!supported) return E_INVALIDARG;
    *supported = FALSE;
    return S_OK;
}

API BOOL GetOsSafeBootMode(DWORD *mode) { if (!mode) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; } *mode = 0; return TRUE; }
API BOOL GetOsManufacturingMode(BOOL *on) { if (!on) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; } *on = FALSE; return TRUE; }

/* Replace the whole environment with a "NAME=value\0...\0\0" block. */
static BOOL set_env_block(char **entries, int n)
{
    for (int i = 0; i < n; i++) {
        const char *eq = strchr(entries[i] + 1, '=');      /* "=C:=C:\x" names start with '=' */
        if (!eq) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    }
    char **cur = lsiw_env_block();
    int nc = 0;
    while (cur && cur[nc]) nc++;
    char **names = xmalloc0(((size_t)nc + 1) * sizeof *names);
    for (int i = 0; i < nc; i++) {
        const char *eq = strchr(cur[i] + 1, '=');
        names[i] = eq ? strndup(cur[i], (size_t)(eq - cur[i])) : xstrdup(cur[i]);
    }
    for (int i = 0; i < nc; i++) { lsiw_env_set(names[i], NULL); free(names[i]); }
    free(names);
    for (int i = 0; i < n; i++) {
        char *eq = strchr(entries[i] + 1, '=');
        *eq = 0;
        lsiw_env_set(entries[i], eq + 1);
        *eq = '=';
    }
    return TRUE;
}

API BOOL SetEnvironmentStringsW(WCHAR *block)
{
    if (!block) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int n = 0;
    for (WCHAR *p = block; *p; p += wlen(p) + 1) n++;
    char **e = xmalloc0(((size_t)n + 1) * sizeof *e);
    int i = 0;
    for (WCHAR *p = block; *p; p += wlen(p) + 1) e[i++] = lsiw_w2u(p);
    BOOL r = set_env_block(e, n);
    for (i = 0; i < n; i++) free(e[i]);
    free(e);
    return r;
}

API BOOL SetEnvironmentStringsA(char *block)
{
    if (!block) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int n = 0;
    for (char *p = block; *p; p += strlen(p) + 1) n++;
    char **e = xmalloc0(((size_t)n + 1) * sizeof *e);
    int i = 0;
    for (char *p = block; *p; p += strlen(p) + 1) { WCHAR *w = a2w(p, -1); e[i++] = lsiw_w2u(w); free(w); }
    BOOL r = set_env_block(e, n);
    for (i = 0; i < n; i++) free(e[i]);
    free(e);
    return r;
}

/* ---- console odds and ends (a terminal has no selection or fonts) ---- */

typedef struct { DWORD dwFlags; COORD dwSelectionAnchor; SMALL_RECT srSelection; } CONSOLE_SELECTION_INFO;
typedef struct { DWORD nFont; COORD dwFontSize; } CONSOLE_FONT_INFO;

API BOOL GetConsoleSelectionInfo(CONSOLE_SELECTION_INFO *si)
{
    if (!si) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(si, 0, sizeof *si);           /* CONSOLE_NO_SELECTION */
    return TRUE;
}

API BOOL GetCurrentConsoleFont(HANDLE h, BOOL max, CONSOLE_FONT_INFO *fi)
{
    (void)max;
    if (lsiw_fd_from_handle(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!fi) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    fi->nFont = 0;
    fi->dwFontSize.X = 8;
    fi->dwFontSize.Y = 16;
    return TRUE;
}

API DWORD GetConsoleFontSize(HANDLE h, DWORD font)
{
    (void)font;
    if (lsiw_fd_from_handle(h) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    return 8 | (16u << 16);              /* COORD { 8, 16 } */
}

API BOOL GetNumberOfConsoleMouseButtons(DWORD *n)
{
    if (!n) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *n = 3;
    return TRUE;
}

/* ---- geographic location of the user ---- */

#define GEOCLASS_NATION 16
#define GEOCLASS_REGION 14
#define GEOCLASS_ALL    0
#define GEO_ID          18
#define GEOID_NOT_AVAILABLE ((GEOID)-1)

static int g_geo_override;
static WCHAR g_geo_name[8];

/* Region of the user's locale ("en-US" -> "US"), or the stored override. */
static void user_region(WCHAR out[8])
{
    if (g_geo_override) { memcpy(out, g_geo_name, sizeof g_geo_name); return; }
    k32_ready();
    WCHAR loc[85] = { 0 };
    out[0] = 0;
    if (k32.GetUserDefaultLocaleName) k32.GetUserDefaultLocaleName(loc, 85);
    WCHAR *dash = NULL;
    for (WCHAR *p = loc; *p; p++) if (*p == '-') dash = p;
    if (!dash || wlen(dash + 1) > 3 || wlen(dash + 1) < 2) return;
    size_t n = wlen(dash + 1);
    memcpy(out, dash + 1, (n + 1) * sizeof(WCHAR));
}

static GEOID region_geoid(const WCHAR *region)
{
    k32_ready();
    WCHAR num[16];
    if (!region[0] || !k32.GetGeoInfoEx || k32.GetGeoInfoEx(region, GEO_ID, num, 16) <= 0) return GEOID_NOT_AVAILABLE;
    char *u = lsiw_w2u(num);
    GEOID id = (GEOID)strtoul(u, NULL, 10);
    free(u);
    return id ? id : GEOID_NOT_AVAILABLE;
}

API GEOID GetUserGeoID(int cls)
{
    if (cls != GEOCLASS_NATION && cls != GEOCLASS_ALL) return GEOID_NOT_AVAILABLE;
    WCHAR r[8];
    user_region(r);
    return region_geoid(r);
}

API BOOL SetUserGeoID(GEOID id) { (void)id; return TRUE; }

API int GetUserDefaultGeoName(WCHAR *buf, int size)
{
    WCHAR r[8];
    user_region(r);
    if (!r[0]) { r[0] = '0'; r[1] = '0'; r[2] = '1'; r[3] = 0; }   /* "001": the world */
    if (size < 0 || (size && !buf)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return ret_wstr_count(r, wlen(r), buf, size);
}

API BOOL SetUserGeoName(const WCHAR *name)
{
    if (!name || !name[0] || wlen(name) > 3) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    memcpy(g_geo_name, name, (wlen(name) + 1) * sizeof(WCHAR));
    g_geo_override = 1;
    return TRUE;
}

/* ---- UI languages ---- */

static __thread LANGID t_ui_lang;

API LANGID GetThreadUILanguage(void)
{
    if (t_ui_lang) return t_ui_lang;
    k32_ready();
    return k32.GetUserDefaultUILanguage ? k32.GetUserDefaultUILanguage() : 0x0409;
}

API LANGID SetThreadUILanguage(LANGID lang)
{
    t_ui_lang = lang;
    return GetThreadUILanguage();
}

API BOOL GetSystemPreferredUILanguages(DWORD flags, ULONG *num, WCHAR *buf, ULONG *size)
{
    k32_ready();
    if (!k32.GetUserPreferredUILanguages) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
    return k32.GetUserPreferredUILanguages(flags, num, buf, size);
}

API BOOL SetProcessPreferredUILanguages(DWORD flags, const WCHAR *langs, ULONG *num)
{
    (void)flags;
    ULONG n = 0;
    for (const WCHAR *p = langs; p && *p; p += wlen(p) + 1) n++;
    if (num) *num = n;
    return TRUE;
}

/* No MUI resource files exist. */
API BOOL GetFileMUIPath(DWORD flags, const WCHAR *path, WCHAR *lang, ULONG *langlen, WCHAR *mui, ULONG *muilen, ULONGLONG *e)
{
    (void)flags; (void)path; (void)lang; (void)langlen; (void)mui; (void)muilen; (void)e;
    lsiw_set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
}

API BOOL GetFileMUIInfo(DWORD flags, const WCHAR *path, void *info, DWORD *size)
{
    (void)flags; (void)info; (void)size;
    char *p = path ? lsiw_path_w(path) : NULL;
    struct stat st;
    int ok = p && stat(p, &st) == 0;
    free(p);
    lsiw_set_last_error(ok ? ERROR_RESOURCE_TYPE_NOT_FOUND : ERROR_FILE_NOT_FOUND);
    return FALSE;
}

/* ================================================================== */
/* NLS: number and currency formatting                                  */
/* ================================================================== */

#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_SDECIMAL       0x0E
#define LOCALE_STHOUSAND      0x0F
#define LOCALE_SGROUPING      0x10
#define LOCALE_IDIGITS        0x11
#define LOCALE_ILZERO         0x12
#define LOCALE_SCURRENCY      0x14
#define LOCALE_SMONDECIMALSEP 0x16
#define LOCALE_SMONTHOUSANDSEP 0x17
#define LOCALE_SMONGROUPING   0x18
#define LOCALE_ICURRDIGITS    0x19
#define LOCALE_ICURRENCY      0x1B
#define LOCALE_INEGCURR       0x1C
#define LOCALE_INEGNUMBER     0x1010

/* A locale given by name (the ...Ex functions) or by LCID. */
struct loc { const WCHAR *name; LCID lcid; int by_name; };

static int loc_info(const struct loc *l, DWORD type, WCHAR *buf, int n)
{
    k32_ready();
    if (l->by_name) return k32.GetLocaleInfoEx ? k32.GetLocaleInfoEx(l->name, type, buf, n) : 0;
    return k32.GetLocaleInfoW ? k32.GetLocaleInfoW(l->lcid, type, buf, n) : 0;
}

static void loc_str(const struct loc *l, DWORD type, WCHAR *out, int n, const char *def)
{
    if (loc_info(l, type, out, n) <= 0) lsiw_utf8_to_utf16(def, -1, out, n);
}

static UINT loc_uint(const struct loc *l, DWORD type, UINT def)
{
    WCHAR b[16];
    if (loc_info(l, type, b, 16) <= 0) return def;
    UINT v = 0;
    for (WCHAR *p = b; *p >= '0' && *p <= '9'; p++) v = v * 10 + (UINT)(*p - '0');
    return v;
}

/* "3;0" -> 3, "3;2;0" -> 32, "3" -> 30 (NUMBERFMT.Grouping encoding). */
static UINT grouping_from_string(const WCHAR *s)
{
    UINT v = 0;
    int last0 = 0;
    for (const WCHAR *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') { last0 = *p == '0' && (p == s || p[-1] == ';'); if (!last0) v = v * 10 + (UINT)(*p - '0'); }
    }
    return last0 ? v : v * 10;
}

struct numfmt { UINT digits, lzero, grouping, neg, pos; WCHAR dec[8], thou[8], sym[16]; };

static void numfmt_locale(const struct loc *l, int currency, struct numfmt *f)
{
    WCHAR g[16];
    memset(f, 0, sizeof *f);
    if (currency) {
        f->digits = loc_uint(l, LOCALE_ICURRDIGITS, 2);
        loc_str(l, LOCALE_SMONGROUPING, g, 16, "3;0");
        loc_str(l, LOCALE_SMONDECIMALSEP, f->dec, 8, ".");
        loc_str(l, LOCALE_SMONTHOUSANDSEP, f->thou, 8, ",");
        f->neg = loc_uint(l, LOCALE_INEGCURR, 0);
        f->pos = loc_uint(l, LOCALE_ICURRENCY, 0);
        loc_str(l, LOCALE_SCURRENCY, f->sym, 16, "$");
    } else {
        f->digits = loc_uint(l, LOCALE_IDIGITS, 2);
        loc_str(l, LOCALE_SGROUPING, g, 16, "3;0");
        loc_str(l, LOCALE_SDECIMAL, f->dec, 8, ".");
        loc_str(l, LOCALE_STHOUSAND, f->thou, 8, ",");
        f->neg = loc_uint(l, LOCALE_INEGNUMBER, 1);
    }
    f->lzero = loc_uint(l, LOCALE_ILZERO, 1);
    f->grouping = grouping_from_string(g);
}

struct wbuf { WCHAR *p; size_t n, cap; };

static void wb_put(struct wbuf *b, const WCHAR *s, size_t n)
{
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap * sizeof(WCHAR)); if (!b->p) abort(); }
    memcpy(b->p + b->n, s, n * sizeof(WCHAR));
    b->n += n;
    b->p[b->n] = 0;
}

static void wb_c(struct wbuf *b, WCHAR c) { wb_put(b, &c, 1); }
static void wb_s(struct wbuf *b, const WCHAR *s) { wb_put(b, s, wlen(s)); }

/* Format the digits of `value` ("-123.456") with f. Returns the number
 * part (no sign) in *out and the sign in *neg; 0 if `value` is invalid. */
static int format_digits(const WCHAR *value, const struct numfmt *f, struct wbuf *out, int *neg)
{
    const WCHAR *p = value;
    *neg = 0;
    if (*p == '-') { *neg = 1; p++; }
    char ip[400], fp[400];
    size_t ni = 0, nf = 0;
    while (*p >= '0' && *p <= '9') { if (ni < sizeof ip - 2) ip[ni++] = (char)*p; p++; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { if (nf < sizeof fp - 1) fp[nf++] = (char)*p; p++; } }
    if (*p || (!ni && !nf)) return 0;
    /* Round half away from zero at f->digits. */
    if (nf > f->digits) {
        int up = fp[f->digits] >= '5';
        nf = f->digits;
        for (size_t k = nf; up && k-- > 0;) { if (fp[k] == '9') fp[k] = '0'; else { fp[k]++; up = 0; } }
        for (size_t k = ni; up && k-- > 0;) { if (ip[k] == '9') ip[k] = '0'; else { ip[k]++; up = 0; } }
        if (up) { memmove(ip + 1, ip, ni); ip[0] = '1'; ni++; }
    }
    while (nf < f->digits) fp[nf++] = '0';
    size_t lead = 0;
    while (lead + 1 < ni && ip[lead] == '0') lead++;          /* strip leading zeros */
    memmove(ip, ip + lead, ni - lead);
    ni -= lead;
    int zero = 1;
    for (size_t k = 0; k < ni; k++) zero &= ip[k] == '0';
    for (size_t k = 0; k < nf; k++) zero &= fp[k] == '0';
    if (zero) *neg = 0;
    /* Group sizes: digits of the Grouping value, last one repeating unless
     * the value ends in 0. */
    int gs[10], ng = 0, repeat = 1;
    if (f->grouping) {
        char g[12];
        snprintf(g, sizeof g, "%u", f->grouping);
        size_t gl = strlen(g);
        if (gl > 1 && g[gl - 1] == '0') { repeat = 0; gl--; }
        for (size_t k = 0; k < gl && ng < 10; k++) gs[ng++] = g[k] - '0';
    }
    int is_zero_int = ni == 0 || (ni == 1 && ip[0] == '0');
    if (!(is_zero_int && !f->lzero && f->digits)) {
        if (ni == 0) { ip[0] = '0'; ni = 1; }
        /* mark positions where separators go, counting from the right */
        char sep[400] = { 0 };
        size_t pos = ni;
        for (int gi = 0; ng && pos > 0;) {
            int size = gs[gi < ng ? gi : ng - 1];
            if (!size || (gi >= ng && !repeat)) break;
            if (pos <= (size_t)size) break;
            pos -= (size_t)size;
            sep[pos] = 1;
            gi++;
        }
        for (size_t k = 0; k < ni; k++) {
            if (sep[k]) wb_s(out, f->thou);
            wb_c(out, (WCHAR)ip[k]);
        }
    }
    if (f->digits) {
        wb_s(out, f->dec);
        for (size_t k = 0; k < nf; k++) wb_c(out, (WCHAR)fp[k]);
    }
    if (!out->p) wb_put(out, NULL, 0);
    return 1;
}

/* Copy a result with the usual "count including NUL; 0 = query" rules. */
static int ret_wbuf(struct wbuf *b, WCHAR *out, int cch)
{
    static const WCHAR empty[1];
    int r = ret_wstr_count(b->p ? b->p : empty, b->n, out, cch);
    free(b->p);
    return r;
}

static int number_format(const WCHAR *value, const struct numfmt *f, WCHAR *out, int cch)
{
    if (!value || cch < 0 || (cch && !out)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    struct wbuf n = { 0 }, r = { 0 };
    int neg;
    if (!format_digits(value, f, &n, &neg)) { free(n.p); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!neg) wb_s(&r, n.p);
    else switch (f->neg) {
        case 0: wb_c(&r, '('); wb_s(&r, n.p); wb_c(&r, ')'); break;
        case 2: wb_c(&r, '-'); wb_c(&r, ' '); wb_s(&r, n.p); break;
        case 3: wb_s(&r, n.p); wb_c(&r, '-'); break;
        case 4: wb_s(&r, n.p); wb_c(&r, ' '); wb_c(&r, '-'); break;
        default: wb_c(&r, '-'); wb_s(&r, n.p); break;
    }
    free(n.p);
    return ret_wbuf(&r, out, cch);
}

static int currency_format(const WCHAR *value, const struct numfmt *f, WCHAR *out, int cch)
{
    if (!value || cch < 0 || (cch && !out)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    struct wbuf n = { 0 }, r = { 0 };
    int neg;
    if (!format_digits(value, f, &n, &neg)) { free(n.p); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    /* Patterns: N = number, $ = symbol, ' ' = space, '-' = minus, () parens. */
    static const char *const pos_pat[4] = { "$N", "N$", "$ N", "N $" };
    static const char *const neg_pat[16] = { "($N)", "-$N", "$-N", "$N-", "(N$)", "-N$", "N-$", "N$-",
                                             "-N $", "-$ N", "N $-", "$ N-", "$ -N", "N- $", "($ N)", "(N $)" };
    const char *pat = neg ? neg_pat[f->neg & 15] : pos_pat[f->pos & 3];
    for (const char *c = pat; *c; c++) {
        if (*c == 'N') wb_s(&r, n.p);
        else if (*c == '$') wb_s(&r, f->sym);
        else wb_c(&r, (WCHAR)*c);
    }
    free(n.p);
    return ret_wbuf(&r, out, cch);
}

typedef struct { UINT NumDigits, LeadingZero, Grouping; WCHAR *lpDecimalSep, *lpThousandSep; UINT NegativeOrder; } NUMBERFMTW;
typedef struct { UINT NumDigits, LeadingZero, Grouping; char *lpDecimalSep, *lpThousandSep; UINT NegativeOrder; } NUMBERFMTA;
typedef struct { UINT NumDigits, LeadingZero, Grouping; WCHAR *lpDecimalSep, *lpThousandSep; UINT NegativeOrder, PositiveOrder; WCHAR *lpCurrencySymbol; } CURRENCYFMTW;
typedef struct { UINT NumDigits, LeadingZero, Grouping; char *lpDecimalSep, *lpThousandSep; UINT NegativeOrder, PositiveOrder; char *lpCurrencySymbol; } CURRENCYFMTA;
_Static_assert(sizeof(NUMBERFMTW) == 40 && sizeof(CURRENCYFMTW) == 48, "NUMBERFMT/CURRENCYFMT");

static void wcopy_n(WCHAR *d, const WCHAR *s, size_t n) { size_t l = wlen(s); if (l >= n) l = n - 1; memcpy(d, s, l * sizeof(WCHAR)); d[l] = 0; }

static int numfmt_from_user(struct numfmt *f, UINT digits, UINT lzero, UINT grouping, const WCHAR *dec, const WCHAR *thou,
                            UINT neg, UINT negmax)
{
    if (!dec || !thou || digits > 9 || lzero > 1 || neg > negmax) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    memset(f, 0, sizeof *f);
    f->digits = digits; f->lzero = lzero; f->grouping = grouping; f->neg = neg;
    wcopy_n(f->dec, dec, 8);
    wcopy_n(f->thou, thou, 8);
    return 1;
}

/* Common driver: locale defaults or a caller format, then format. */
static int number_api(const struct loc *l, DWORD flags, const WCHAR *value, const NUMBERFMTW *fmt, WCHAR *out, int cch)
{
    struct numfmt f;
    if (fmt) {
        if (flags) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return 0; }
        if (!numfmt_from_user(&f, fmt->NumDigits, fmt->LeadingZero, fmt->Grouping, fmt->lpDecimalSep, fmt->lpThousandSep, fmt->NegativeOrder, 4)) return 0;
    } else {
        if (flags & ~LOCALE_NOUSEROVERRIDE) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return 0; }
        numfmt_locale(l, 0, &f);
    }
    return number_format(value, &f, out, cch);
}

static int currency_api(const struct loc *l, DWORD flags, const WCHAR *value, const CURRENCYFMTW *fmt, WCHAR *out, int cch)
{
    struct numfmt f;
    if (fmt) {
        if (flags) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return 0; }
        if (!fmt->lpCurrencySymbol || fmt->PositiveOrder > 3 ||
            !numfmt_from_user(&f, fmt->NumDigits, fmt->LeadingZero, fmt->Grouping, fmt->lpDecimalSep, fmt->lpThousandSep, fmt->NegativeOrder, 15)) {
            lsiw_set_last_error(ERROR_INVALID_PARAMETER);
            return 0;
        }
        f.pos = fmt->PositiveOrder;
        wcopy_n(f.sym, fmt->lpCurrencySymbol, 16);
    } else {
        if (flags & ~LOCALE_NOUSEROVERRIDE) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return 0; }
        numfmt_locale(l, 1, &f);
    }
    return currency_format(value, &f, out, cch);
}

static int loc_name_ok(const WCHAR *name)
{
    k32_ready();
    if (!name || !name[0] || !k32.IsValidLocaleName) return 1;
    if (!wnicmp(name, "!x-sys-default-locale", 22)) return 1;
    return k32.IsValidLocaleName(name);
}

API int GetNumberFormatEx(const WCHAR *locale, DWORD flags, const WCHAR *value, const NUMBERFMTW *fmt, WCHAR *out, int cch)
{
    if (!loc_name_ok(locale)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    struct loc l = { locale, 0, 1 };
    return number_api(&l, flags, value, fmt, out, cch);
}

API int GetNumberFormatW(LCID lcid, DWORD flags, const WCHAR *value, const NUMBERFMTW *fmt, WCHAR *out, int cch)
{
    struct loc l = { NULL, lcid, 0 };
    return number_api(&l, flags, value, fmt, out, cch);
}

API int GetCurrencyFormatEx(const WCHAR *locale, DWORD flags, const WCHAR *value, const CURRENCYFMTW *fmt, WCHAR *out, int cch)
{
    if (!loc_name_ok(locale)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    struct loc l = { locale, 0, 1 };
    return currency_api(&l, flags, value, fmt, out, cch);
}

API int GetCurrencyFormatW(LCID lcid, DWORD flags, const WCHAR *value, const CURRENCYFMTW *fmt, WCHAR *out, int cch)
{
    struct loc l = { NULL, lcid, 0 };
    return currency_api(&l, flags, value, fmt, out, cch);
}

/* The A forms: convert in, format wide, convert the result back. */
static int a_result(int n, WCHAR *w, char *out, int cch)
{
    if (!n) { free(w); return 0; }
    char *a = w2a(w, -1);
    free(w);
    int need = (int)strlen(a) + 1;
    if (cch == 0) { free(a); return need; }
    if (!out || cch < need) { free(a); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, a, (size_t)need);
    free(a);
    return need;
}

API int GetNumberFormatA(LCID lcid, DWORD flags, const char *value, const NUMBERFMTA *fmt, char *out, int cch)
{
    if (!value || cch < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    WCHAR *wv = a2w(value, -1), *wd = NULL, *wt = NULL;
    NUMBERFMTW wf, *pf = NULL;
    if (fmt) {
        wd = a2w(fmt->lpDecimalSep, -1);
        wt = a2w(fmt->lpThousandSep, -1);
        wf = (NUMBERFMTW){ fmt->NumDigits, fmt->LeadingZero, fmt->Grouping, wd, wt, fmt->NegativeOrder };
        pf = &wf;
    }
    WCHAR *w = xmalloc0(512 * sizeof(WCHAR));
    int n = GetNumberFormatW(lcid, flags, wv, pf, w, 512);
    free(wv); free(wd); free(wt);
    return a_result(n, w, out, cch);
}

API int GetCurrencyFormatA(LCID lcid, DWORD flags, const char *value, const CURRENCYFMTA *fmt, char *out, int cch)
{
    if (!value || cch < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    WCHAR *wv = a2w(value, -1), *wd = NULL, *wt = NULL, *ws = NULL;
    CURRENCYFMTW wf, *pf = NULL;
    if (fmt) {
        wd = a2w(fmt->lpDecimalSep, -1);
        wt = a2w(fmt->lpThousandSep, -1);
        ws = a2w(fmt->lpCurrencySymbol, -1);
        wf = (CURRENCYFMTW){ fmt->NumDigits, fmt->LeadingZero, fmt->Grouping, wd, wt, fmt->NegativeOrder, fmt->PositiveOrder, ws };
        pf = &wf;
    }
    WCHAR *w = xmalloc0(512 * sizeof(WCHAR));
    int n = GetCurrencyFormatW(lcid, flags, wv, pf, w, 512);
    free(wv); free(wd); free(wt); free(ws);
    return a_result(n, w, out, cch);
}

/* ================================================================== */
/* NLS: calendars, versions, locale names                               */
/* ================================================================== */

#define CAL_GREGORIAN      1
#define CAL_RETURN_NUMBER  0x20000000u
#define CAL_USE_CP_ACP     0x40000000u
#define CAL_NOUSEROVERRIDE 0x80000000u

/* Gregorian calendar data: fixed values, names from the locale. */
static int calendar_info(const struct loc *l, DWORD cal, DWORD type, WCHAR *buf, int cch, DWORD *value)
{
    if (cal != CAL_GREGORIAN && cal != 2 /* CAL_GREGORIAN_US */) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD base = type & 0xFFFF;
    int number = (type & CAL_RETURN_NUMBER) != 0;
    const char *fixed = NULL;
    DWORD num = 0, lctype = 0;
    int is_num = 0;
    switch (base) {
    case 1:  is_num = 1; num = cal; break;                   /* CAL_ICALINTVALUE */
    case 2:  fixed = "Gregorian Calendar"; break;            /* CAL_SCALNAME */
    case 3:  is_num = 1; num = 0; break;                     /* CAL_IYEAROFFSETRANGE */
    case 4:  fixed = "A.D."; break;                          /* CAL_SERASTRING */
    case 5:  lctype = 0x1F; break;                           /* CAL_SSHORTDATE */
    case 6:  lctype = 0x20; break;                           /* CAL_SLONGDATE */
    case 33: case 46: fixed = ""; break;                     /* 13th month names */
    case 47: lctype = 0x1006; break;                         /* CAL_SYEARMONTH */
    case 48: is_num = 1; num = 2049; break;                  /* CAL_ITWODIGITYEARMAX */
    case 56: lctype = 0x78; break;                           /* CAL_SMONTHDAY */
    case 57: fixed = "AD"; break;                            /* CAL_SABBREVERASTRING */
    default:
        if (base >= 7 && base <= 13) lctype = 0x2A + base - 7;          /* day names */
        else if (base >= 14 && base <= 20) lctype = 0x31 + base - 14;  /* abbreviated day names */
        else if (base >= 21 && base <= 32) lctype = 0x38 + base - 21;  /* month names */
        else if (base >= 34 && base <= 45) lctype = 0x44 + base - 34;  /* abbreviated month names */
        else if (base >= 49 && base <= 55) lctype = 0x60 + base - 49;  /* shortest day names */
        else { lsiw_set_last_error(ERROR_INVALID_FLAGS); return 0; }
    }
    if (number) {
        if (!is_num) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return 0; }
        if (!value) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        *value = num;
        return 2;                     /* sizeof(DWORD) / sizeof(WCHAR) */
    }
    char tmp[16];
    if (is_num) { snprintf(tmp, sizeof tmp, "%u", (unsigned)num); fixed = tmp; }
    if (fixed) return ret_w_count(fixed, buf, cch);
    WCHAR v[128];
    int got = loc_info(l, lctype, v, 128) > 0;
    if (got && (base == 47 || base == 56)) {          /* a format must contain a month */
        got = 0;
        for (WCHAR *p = v; *p; p++) got |= *p == 'M';
    }
    if (got) return ret_wstr_count(v, wlen(v), buf, cch);
    /* The locale has no such name: use the invariant (English) one. */
    static const char *const days[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
    static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
    char e[16];
    if (base >= 7 && base <= 13) return ret_w_count(days[base - 7], buf, cch);
    if (base >= 14 && base <= 20) { snprintf(e, sizeof e, "%.3s", days[base - 14]); return ret_w_count(e, buf, cch); }
    if (base >= 49 && base <= 55) { snprintf(e, sizeof e, "%.2s", days[base - 49]); return ret_w_count(e, buf, cch); }
    if (base >= 21 && base <= 32) return ret_w_count(months[base - 21], buf, cch);
    if (base >= 34 && base <= 45) { snprintf(e, sizeof e, "%.3s", months[base - 34]); return ret_w_count(e, buf, cch); }
    if (base == 5) return ret_w_count("M/d/yyyy", buf, cch);
    if (base == 6) return ret_w_count("dddd, MMMM d, yyyy", buf, cch);
    if (base == 47) return ret_w_count("MMMM yyyy", buf, cch);
    if (base == 56) return ret_w_count("MMMM d", buf, cch);
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
}

API int GetCalendarInfoEx(const WCHAR *locale, DWORD cal, const WCHAR *res, DWORD type, WCHAR *buf, int cch, DWORD *value)
{
    (void)res;
    if (!loc_name_ok(locale)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    struct loc l = { locale, 0, 1 };
    return calendar_info(&l, cal, type, buf, cch, value);
}

API int GetCalendarInfoW(LCID lcid, DWORD cal, DWORD type, WCHAR *buf, int cch, DWORD *value)
{
    struct loc l = { NULL, lcid, 0 };
    return calendar_info(&l, cal, type, buf, cch, value);
}

API int GetCalendarInfoA(LCID lcid, DWORD cal, DWORD type, char *buf, int cch, DWORD *value)
{
    if (type & CAL_RETURN_NUMBER) return GetCalendarInfoW(lcid, cal, type, NULL, 0, value);
    WCHAR *w = xmalloc0(256 * sizeof(WCHAR));
    int n = GetCalendarInfoW(lcid, cal, type, w, 256, NULL);
    return a_result(n, w, buf, cch);
}

/* Sorting / NLS version of the Windows 10 tables. */
#define LSIW_NLS_VERSION     0x0006020Fu
#define LSIW_NLS_DEFINED     0x00060200u

typedef struct { DWORD dwNLSVersionInfoSize, dwNLSVersion, dwDefinedVersion, dwEffectiveId; BYTE guidCustomVersion[16]; } NLSVERSIONINFOEX;

static BOOL nls_version(DWORD function, LCID lcid, NLSVERSIONINFOEX *v)
{
    if (function != 1 /* COMPARE_STRING */ || !v) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (v->dwNLSVersionInfoSize != 12 && v->dwNLSVersionInfoSize != sizeof *v) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    v->dwNLSVersion = LSIW_NLS_VERSION;
    v->dwDefinedVersion = LSIW_NLS_DEFINED;
    if (v->dwNLSVersionInfoSize == sizeof *v) {
        v->dwEffectiveId = lcid;
        memset(v->guidCustomVersion, 0, 16);
    }
    return TRUE;
}

API BOOL GetNLSVersionEx(DWORD function, const WCHAR *locale, NLSVERSIONINFOEX *v)
{
    if (!loc_name_ok(locale)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    k32_ready();
    LCID lcid = k32.LocaleNameToLCID ? k32.LocaleNameToLCID(locale, 0) : 0x0409;
    return nls_version(function, lcid, v);
}

API BOOL GetNLSVersion(DWORD function, LCID lcid, NLSVERSIONINFOEX *v) { return nls_version(function, lcid, v); }

API DWORD IsValidNLSVersion(DWORD function, const WCHAR *locale, NLSVERSIONINFOEX *v)
{
    (void)locale;
    if (function != 1 || !v) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return v->dwNLSVersion == LSIW_NLS_VERSION || v->dwDefinedVersion == LSIW_NLS_DEFINED;
}

/* Every assigned BMP and supplementary code point counts as defined;
 * unpaired surrogates and the noncharacters U+FFFE/U+FFFF do not. */
API BOOL IsNLSDefinedString(DWORD function, DWORD flags, void *v, const WCHAR *s, int n)
{
    (void)flags; (void)v;
    if (function != 1 || !s || n == 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t len = n < 0 ? wlen(s) : (size_t)n;
    for (size_t i = 0; i < len; i++) {
        WCHAR c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) { if (i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) { i++; continue; } return FALSE; }
        if ((c >= 0xDC00 && c <= 0xDFFF) || c == 0xFFFE || c == 0xFFFF) return FALSE;
    }
    return TRUE;
}

/* Neutral language -> its usual specific locale. */
static const char *const g_neutral[][2] = {
    { "en", "en-US" }, { "de", "de-DE" }, { "fr", "fr-FR" }, { "es", "es-ES" }, { "it", "it-IT" },
    { "ja", "ja-JP" }, { "zh", "zh-CN" }, { "pt", "pt-BR" }, { "ru", "ru-RU" }, { "ko", "ko-KR" },
    { "nl", "nl-NL" }, { "sv", "sv-SE" }, { "pl", "pl-PL" }, { "tr", "tr-TR" }, { "da", "da-DK" },
    { "fi", "fi-FI" }, { "nb", "nb-NO" }, { "no", "nb-NO" }, { "cs", "cs-CZ" }, { "hu", "hu-HU" },
    { "el", "el-GR" }, { "he", "he-IL" }, { "ar", "ar-SA" }, { "uk", "uk-UA" }, { "ro", "ro-RO" },
    { "sk", "sk-SK" }, { "hr", "hr-HR" }, { "sl", "sl-SI" }, { "bg", "bg-BG" }, { "th", "th-TH" },
    { "vi", "vi-VN" }, { "id", "id-ID" }, { "hi", "hi-IN" }, { "ca", "ca-ES" }, { "et", "et-EE" },
    { "lv", "lv-LV" }, { "lt", "lt-LT" },
};

API int ResolveLocaleName(const WCHAR *name, WCHAR *buf, int cch)
{
    k32_ready();
    WCHAR def[85] = { 0 };
    if (!name || !name[0]) {
        if (!name && k32.GetUserDefaultLocaleName) k32.GetUserDefaultLocaleName(def, 85);
        return ret_wstr_count(def, wlen(def), buf, cch);
    }
    int has_dash = 0;
    for (const WCHAR *p = name; *p; p++) has_dash |= *p == '-' || *p == '_';
    if (!has_dash) {
        for (size_t i = 0; i < sizeof g_neutral / sizeof g_neutral[0]; i++)
            if (!wnicmp(name, g_neutral[i][0], 3)) return ret_w_count(g_neutral[i][1], buf, cch);
    }
    if (k32.IsValidLocaleName && k32.IsValidLocaleName(name)) return ret_wstr_count(name, wlen(name), buf, cch);
    /* Unknown locale: no match, which Windows reports as an empty name. */
    return ret_wstr_count(def, 0, buf, cch);
}

/* ================================================================== */
/* NLS: searching                                                       */
/* ================================================================== */

#define FIND_STARTSWITH 0x00100000u
#define FIND_ENDSWITH   0x00200000u
#define FIND_FROMSTART  0x00400000u
#define FIND_FROMEND    0x00800000u
#define FIND_MASK       0x00F00000u

static int find_in(DWORD mode, const WCHAR *src, int ns, const WCHAR *val, int nv, int fold)
{
    int lo = 0, hi = ns - nv;
    if (nv > ns) return -1;
    if (mode == FIND_STARTSWITH) hi = 0;
    if (mode == FIND_ENDSWITH) lo = hi;
    int step = mode == FIND_FROMEND || mode == FIND_ENDSWITH ? -1 : 1;
    for (int i = step > 0 ? lo : hi; step > 0 ? i <= hi : i >= lo; i += step) {
        int k = 0;
        while (k < nv && (fold ? wupper(src[i + k]) == wupper(val[k]) : src[i + k] == val[k])) k++;
        if (k == nv) return i;
    }
    return -1;
}

static int find_mode(DWORD flags, DWORD *mode)
{
    *mode = flags & FIND_MASK;
    if (!*mode) *mode = FIND_FROMSTART;
    return *mode == FIND_STARTSWITH || *mode == FIND_ENDSWITH || *mode == FIND_FROMSTART || *mode == FIND_FROMEND;
}

API int FindStringOrdinal(DWORD flags, const WCHAR *src, int ns, const WCHAR *val, int nv, BOOL ignore_case)
{
    DWORD mode;
    if ((flags & ~FIND_MASK) || !(flags & FIND_MASK) || !find_mode(flags, &mode)) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return -1; }
    if (!src || !val) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return -1; }
    if (ns < 0) ns = (int)wlen(src);
    if (nv < 0) nv = (int)wlen(val);
    lsiw_set_last_error(ERROR_SUCCESS);
    return find_in(mode, src, ns, val, nv, ignore_case);
}

/* Compare flags accepted alongside FIND_*; case-insensitivity is applied,
 * the linguistic ignore flags compare as ordinal. */
#define NLS_COMPARE_FLAGS 0x1830F03Fu

static int find_nls(DWORD flags, const WCHAR *src, int ns, const WCHAR *val, int nv, int *found)
{
    DWORD mode;
    if ((flags & ~(FIND_MASK | NLS_COMPARE_FLAGS | 0x08000000u)) || !find_mode(flags, &mode)) { lsiw_set_last_error(ERROR_INVALID_FLAGS); return -1; }
    if (!src || !val || !ns || ns < -1 || nv < -1) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return -1; }
    if (ns < 0) ns = (int)wlen(src);
    if (nv < 0) nv = (int)wlen(val);
    int r = find_in(mode, src, ns, val, nv, (flags & (0x1 | 0x10)) != 0);   /* NORM_ / LINGUISTIC_IGNORECASE */
    if (r >= 0 && found) *found = nv;
    lsiw_set_last_error(ERROR_SUCCESS);
    return r;
}

API int FindNLSStringEx(const WCHAR *locale, DWORD flags, const WCHAR *src, int ns, const WCHAR *val, int nv,
                        int *found, void *ver, void *res, intptr_t sort)
{
    (void)ver; (void)res; (void)sort;
    if (!loc_name_ok(locale)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return -1; }
    return find_nls(flags, src, ns, val, nv, found);
}

API int FindNLSString(LCID lcid, DWORD flags, const WCHAR *src, int ns, const WCHAR *val, int nv, int *found)
{
    (void)lcid;
    return find_nls(flags, src, ns, val, nv, found);
}

/* Script detection would need the Unicode script property tables. */
API int GetStringScripts(DWORD flags, const WCHAR *s, int n, WCHAR *buf, int cch)
{ (void)flags; (void)s; (void)n; (void)buf; (void)cch; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return 0; }

API BOOL VerifyScripts(DWORD flags, const WCHAR *loc, int nl, const WCHAR *test, int nt)
{ (void)flags; (void)loc; (void)nl; (void)test; (void)nt; return TRUE; }

/* ================================================================== */
/* NLS: Unicode normalization                                           */
/* ================================================================== */

/* Canonical decompositions of Latin-1 Supplement and Latin Extended-A
 * (base letter + one combining mark), Hangul syllables (algorithmic),
 * three canonical singletons, and for the compatibility forms the
 * full-width ASCII block, U+3000, U+00A0 and the f-ligatures. Other
 * characters pass through unchanged. */
static const struct { WCHAR cp, base, mark; } g_decomp[] = {
    {0xC0,'A',0x300},{0xC1,'A',0x301},{0xC2,'A',0x302},{0xC3,'A',0x303},{0xC4,'A',0x308},{0xC5,'A',0x30A},
    {0xC7,'C',0x327},{0xC8,'E',0x300},{0xC9,'E',0x301},{0xCA,'E',0x302},{0xCB,'E',0x308},{0xCC,'I',0x300},
    {0xCD,'I',0x301},{0xCE,'I',0x302},{0xCF,'I',0x308},{0xD1,'N',0x303},{0xD2,'O',0x300},{0xD3,'O',0x301},
    {0xD4,'O',0x302},{0xD5,'O',0x303},{0xD6,'O',0x308},{0xD9,'U',0x300},{0xDA,'U',0x301},{0xDB,'U',0x302},
    {0xDC,'U',0x308},{0xDD,'Y',0x301},
    {0xE0,'a',0x300},{0xE1,'a',0x301},{0xE2,'a',0x302},{0xE3,'a',0x303},{0xE4,'a',0x308},{0xE5,'a',0x30A},
    {0xE7,'c',0x327},{0xE8,'e',0x300},{0xE9,'e',0x301},{0xEA,'e',0x302},{0xEB,'e',0x308},{0xEC,'i',0x300},
    {0xED,'i',0x301},{0xEE,'i',0x302},{0xEF,'i',0x308},{0xF1,'n',0x303},{0xF2,'o',0x300},{0xF3,'o',0x301},
    {0xF4,'o',0x302},{0xF5,'o',0x303},{0xF6,'o',0x308},{0xF9,'u',0x300},{0xFA,'u',0x301},{0xFB,'u',0x302},
    {0xFC,'u',0x308},{0xFD,'y',0x301},{0xFF,'y',0x308},
    {0x100,'A',0x304},{0x101,'a',0x304},{0x102,'A',0x306},{0x103,'a',0x306},{0x104,'A',0x328},{0x105,'a',0x328},
    {0x106,'C',0x301},{0x107,'c',0x301},{0x108,'C',0x302},{0x109,'c',0x302},{0x10A,'C',0x307},{0x10B,'c',0x307},
    {0x10C,'C',0x30C},{0x10D,'c',0x30C},{0x10E,'D',0x30C},{0x10F,'d',0x30C},
    {0x112,'E',0x304},{0x113,'e',0x304},{0x114,'E',0x306},{0x115,'e',0x306},{0x116,'E',0x307},{0x117,'e',0x307},
    {0x118,'E',0x328},{0x119,'e',0x328},{0x11A,'E',0x30C},{0x11B,'e',0x30C},{0x11C,'G',0x302},{0x11D,'g',0x302},
    {0x11E,'G',0x306},{0x11F,'g',0x306},{0x120,'G',0x307},{0x121,'g',0x307},{0x122,'G',0x327},{0x123,'g',0x327},
    {0x124,'H',0x302},{0x125,'h',0x302},{0x128,'I',0x303},{0x129,'i',0x303},{0x12A,'I',0x304},{0x12B,'i',0x304},
    {0x12C,'I',0x306},{0x12D,'i',0x306},{0x12E,'I',0x328},{0x12F,'i',0x328},{0x130,'I',0x307},
    {0x134,'J',0x302},{0x135,'j',0x302},{0x136,'K',0x327},{0x137,'k',0x327},{0x139,'L',0x301},{0x13A,'l',0x301},
    {0x13B,'L',0x327},{0x13C,'l',0x327},{0x13D,'L',0x30C},{0x13E,'l',0x30C},{0x143,'N',0x301},{0x144,'n',0x301},
    {0x145,'N',0x327},{0x146,'n',0x327},{0x147,'N',0x30C},{0x148,'n',0x30C},{0x14C,'O',0x304},{0x14D,'o',0x304},
    {0x14E,'O',0x306},{0x14F,'o',0x306},{0x150,'O',0x30B},{0x151,'o',0x30B},{0x154,'R',0x301},{0x155,'r',0x301},
    {0x156,'R',0x327},{0x157,'r',0x327},{0x158,'R',0x30C},{0x159,'r',0x30C},{0x15A,'S',0x301},{0x15B,'s',0x301},
    {0x15C,'S',0x302},{0x15D,'s',0x302},{0x15E,'S',0x327},{0x15F,'s',0x327},{0x160,'S',0x30C},{0x161,'s',0x30C},
    {0x162,'T',0x327},{0x163,'t',0x327},{0x164,'T',0x30C},{0x165,'t',0x30C},{0x168,'U',0x303},{0x169,'u',0x303},
    {0x16A,'U',0x304},{0x16B,'u',0x304},{0x16C,'U',0x306},{0x16D,'u',0x306},{0x16E,'U',0x30A},{0x16F,'u',0x30A},
    {0x170,'U',0x30B},{0x171,'u',0x30B},{0x172,'U',0x328},{0x173,'u',0x328},{0x174,'W',0x302},{0x175,'w',0x302},
    {0x176,'Y',0x302},{0x177,'y',0x302},{0x178,'Y',0x308},{0x179,'Z',0x301},{0x17A,'z',0x301},{0x17B,'Z',0x307},
    {0x17C,'z',0x307},{0x17D,'Z',0x30C},{0x17E,'z',0x30C},
};

/* Canonical combining class of the combining diacritical marks block. */
static int ccc(WCHAR c)
{
    if (c < 0x300 || c > 0x36F) return 0;
    if (c <= 0x314) return 230;
    if (c == 0x315 || c == 0x31A) return 232;
    if (c <= 0x319) return 220;
    if (c == 0x31B) return 216;
    if (c <= 0x320) return 220;
    if (c <= 0x322) return 202;
    if (c <= 0x326) return 220;
    if (c <= 0x328) return 202;
    if (c <= 0x333) return 220;
    if (c <= 0x338) return 1;
    if (c <= 0x33C) return 220;
    if (c <= 0x344) return 230;
    if (c == 0x345) return 240;
    if (c == 0x346) return 230;
    if (c <= 0x349) return 220;
    if (c <= 0x34C) return 230;
    if (c <= 0x34E) return 220;
    if (c == 0x34F) return 0;
    if (c <= 0x352) return 230;
    if (c <= 0x356) return 220;
    if (c == 0x357) return 230;
    if (c == 0x358) return 232;
    if (c <= 0x35A) return 220;
    if (c == 0x35B) return 230;
    if (c == 0x35C || c == 0x35F || c == 0x362) return 233;
    if (c <= 0x361) return 234;
    return 230;
}

#define HANGUL_S 0xAC00
#define HANGUL_L 0x1100
#define HANGUL_V 0x1161
#define HANGUL_T 0x11A7
#define HANGUL_TCOUNT 28
#define HANGUL_NCOUNT 588
#define HANGUL_SCOUNT 11172

static void decompose(WCHAR c, int compat, struct wbuf *out)
{
    if (c >= HANGUL_S && c < HANGUL_S + HANGUL_SCOUNT) {
        int s = c - HANGUL_S;
        wb_c(out, (WCHAR)(HANGUL_L + s / HANGUL_NCOUNT));
        wb_c(out, (WCHAR)(HANGUL_V + (s % HANGUL_NCOUNT) / HANGUL_TCOUNT));
        if (s % HANGUL_TCOUNT) wb_c(out, (WCHAR)(HANGUL_T + s % HANGUL_TCOUNT));
        return;
    }
    if (c == 0x2126) { wb_c(out, 0x3A9); return; }              /* OHM SIGN */
    if (c == 0x212A) { wb_c(out, 'K'); return; }                /* KELVIN SIGN */
    if (c == 0x212B) { decompose(0xC5, compat, out); return; }  /* ANGSTROM SIGN */
    if (compat) {
        if (c >= 0xFF01 && c <= 0xFF5E) { wb_c(out, (WCHAR)(c - 0xFEE0)); return; }
        if (c == 0x3000 || c == 0xA0) { wb_c(out, ' '); return; }
        static const char *const lig[] = { "ff", "fi", "fl", "ffi", "ffl" };
        if (c >= 0xFB00 && c <= 0xFB04) { for (const char *p = lig[c - 0xFB00]; *p; p++) wb_c(out, (WCHAR)*p); return; }
    }
    if (c >= 0xC0 && c <= 0x17F)
        for (size_t i = 0; i < sizeof g_decomp / sizeof g_decomp[0]; i++)
            if (g_decomp[i].cp == c) { wb_c(out, g_decomp[i].base); wb_c(out, g_decomp[i].mark); return; }
    wb_c(out, c);
}

static WCHAR compose_pair(WCHAR a, WCHAR b)
{
    if (a >= HANGUL_L && a < HANGUL_L + 19 && b >= HANGUL_V && b < HANGUL_V + 21)
        return (WCHAR)(HANGUL_S + ((a - HANGUL_L) * 21 + (b - HANGUL_V)) * HANGUL_TCOUNT);
    if (a >= HANGUL_S && a < HANGUL_S + HANGUL_SCOUNT && (a - HANGUL_S) % HANGUL_TCOUNT == 0 && b > HANGUL_T && b < HANGUL_T + HANGUL_TCOUNT)
        return (WCHAR)(a + (b - HANGUL_T));
    for (size_t i = 0; i < sizeof g_decomp / sizeof g_decomp[0]; i++)
        if (g_decomp[i].base == a && g_decomp[i].mark == b) return g_decomp[i].cp;
    return 0;
}

/* Normalize; returns 0 and the index of the bad unit for invalid UTF-16. */
static int normalize(int form, const WCHAR *s, size_t n, struct wbuf *out, size_t *bad)
{
    int compat = form == 5 || form == 6, comp = form == 1 || form == 5;
    struct wbuf d = { 0 };
    for (size_t i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= n || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) { free(d.p); *bad = i; return 0; }
            wb_c(&d, c); wb_c(&d, s[++i]);
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) { free(d.p); *bad = i; return 0; }
        decompose(c, compat, &d);
    }
    /* Canonical ordering of combining marks (stable insertion sort). */
    for (size_t i = 1; i < d.n; i++) {
        int cc = ccc(d.p[i]);
        if (!cc) continue;
        for (size_t j = i; j > 0 && ccc(d.p[j - 1]) > cc; j--) { WCHAR t = d.p[j]; d.p[j] = d.p[j - 1]; d.p[j - 1] = t; }
    }
    if (!comp) { *out = d; if (!out->p) wb_put(out, NULL, 0); return 1; }
    /* Canonical composition. */
    size_t starter = (size_t)-1, w = 0;
    int last_cc = -1;
    for (size_t i = 0; i < d.n; i++) {
        WCHAR c = d.p[i];
        int cc = ccc(c);
        if (starter != (size_t)-1 && (last_cc < cc || (last_cc == -1 && cc == 0) || (last_cc == 0 && w == starter + 1))) {
            WCHAR p = (last_cc < cc || w == starter + 1) ? compose_pair(d.p[starter], c) : 0;
            if (p) { d.p[starter] = p; continue; }
        }
        if (cc == 0) { starter = w; last_cc = -1; } else last_cc = cc;
        if (cc == 0) last_cc = 0;
        d.p[w++] = c;
        if (cc == 0) last_cc = -1;
    }
    d.n = w;
    if (d.p) d.p[w] = 0;
    *out = d;
    if (!out->p) wb_put(out, NULL, 0);
    return 1;
}

#define ERROR_NO_UNICODE_TRANSLATION 1113

API int NormalizeString(int form, const WCHAR *src, int ns, WCHAR *dst, int nd)
{
    if ((form != 1 && form != 2 && form != 5 && form != 6) || !src || ns == 0 || ns < -1 || nd < 0) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t n = ns < 0 ? wlen(src) + 1 : (size_t)ns;      /* -1: the NUL is part of the result */
    struct wbuf out = { 0 };
    size_t bad;
    if (!normalize(form, src, n, &out, &bad)) { lsiw_set_last_error(ERROR_NO_UNICODE_TRANSLATION); return -(int)bad - 1; }
    int need = (int)out.n;
    if (nd == 0) { free(out.p); lsiw_set_last_error(ERROR_SUCCESS); return need; }
    if (!dst || nd < need) { free(out.p); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return -need; }
    memcpy(dst, out.p, (size_t)need * sizeof(WCHAR));
    free(out.p);
    lsiw_set_last_error(ERROR_SUCCESS);
    return need;
}

API BOOL IsNormalizedString(int form, const WCHAR *s, int n)
{
    if ((form != 1 && form != 2 && form != 5 && form != 6) || !s || n < -1) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t len = n < 0 ? wlen(s) : (size_t)n;
    struct wbuf out = { 0 };
    size_t bad;
    if (!normalize(form, s, len, &out, &bad)) { lsiw_set_last_error(ERROR_NO_UNICODE_TRANSLATION); return FALSE; }
    BOOL same = out.n == len && !memcmp(out.p, s, len * sizeof(WCHAR));
    free(out.p);
    lsiw_set_last_error(ERROR_SUCCESS);
    return same;
}

/* ================================================================== */
/* Atom tables                                                          */
/* ================================================================== */

/* String atoms are 0xC000..0xFFFF, integer atoms 1..0xBFFF ("#n" or
 * MAKEINTATOM). Names compare case-insensitively, are at most 255
 * characters, and carry a reference count. The "global" table is a
 * second, process-wide table: there is no cross-process atom server. */
#define MAXINTATOM     0xC000
#define ATOM_MAX_NAME  255
#define ERROR_INVALID_ATOM_NAME 0x2002
#ifndef ERROR_WRITE_FAULT
#define ERROR_WRITE_FAULT 29
#endif

struct atom_ent { WCHAR *name; unsigned refs; };
struct atom_table { struct atom_ent *e; size_t n; pthread_mutex_t lock; };
static struct atom_table g_local_atoms = { NULL, 0, PTHREAD_MUTEX_INITIALIZER };
static struct atom_table g_global_atoms = { NULL, 0, PTHREAD_MUTEX_INITIALIZER };

static int is_int_atom_ptr(const void *p) { return (uintptr_t)p < 0x10000; }

/* "#123" -> 123; returns 1 if the name is an integer atom spec. */
static int parse_int_atom(const WCHAR *s, unsigned *v)
{
    if (s[0] != '#') return 0;
    unsigned x = 0;
    const WCHAR *p = s + 1;
    if (!*p) return 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        x = x * 10 + (unsigned)(*p - '0');
        if (x > 0xFFFF) return 0;
    }
    *v = x;
    return 1;
}

/* Returns the atom, or 0 with last error set. */
static ATOM atom_add(struct atom_table *t, const WCHAR *name, int find_only)
{
    if (is_int_atom_ptr(name)) {
        uintptr_t v = (uintptr_t)name;
        if (!v || v >= MAXINTATOM) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        return (ATOM)v;
    }
    unsigned iv;
    if (parse_int_atom(name, &iv)) {
        if (!iv || iv >= MAXINTATOM) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        return (ATOM)iv;
    }
    size_t len = wlen(name);
    if (!len || len > ATOM_MAX_NAME) { lsiw_set_last_error(len ? ERROR_INVALID_PARAMETER : ERROR_INVALID_NAME); return 0; }
    pthread_mutex_lock(&t->lock);
    size_t free_slot = (size_t)-1;
    for (size_t i = 0; i < t->n; i++) {
        if (!t->e[i].name) { if (free_slot == (size_t)-1) free_slot = i; continue; }
        if (!wicmp(t->e[i].name, name)) {
            if (!find_only) t->e[i].refs++;
            pthread_mutex_unlock(&t->lock);
            return (ATOM)(MAXINTATOM + i);
        }
    }
    if (find_only) { pthread_mutex_unlock(&t->lock); lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    if (free_slot == (size_t)-1) {
        if (t->n >= 0x4000) { pthread_mutex_unlock(&t->lock); lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        struct atom_ent *ne = realloc(t->e, (t->n + 1) * sizeof *ne);
        if (!ne) abort();
        t->e = ne;
        free_slot = t->n++;
    }
    WCHAR *copy = xmalloc0((len + 1) * sizeof(WCHAR));
    memcpy(copy, name, len * sizeof(WCHAR));
    t->e[free_slot].name = copy;
    t->e[free_slot].refs = 1;
    pthread_mutex_unlock(&t->lock);
    return (ATOM)(MAXINTATOM + free_slot);
}

/* Returns 0 on success, the atom on failure (DeleteAtom convention). */
static ATOM atom_delete(struct atom_table *t, ATOM a)
{
    if (a < MAXINTATOM) return 0;
    size_t i = (size_t)(a - MAXINTATOM);
    pthread_mutex_lock(&t->lock);
    if (i >= t->n || !t->e[i].name) { pthread_mutex_unlock(&t->lock); lsiw_set_last_error(ERROR_INVALID_HANDLE); return a; }
    if (--t->e[i].refs == 0) { free(t->e[i].name); t->e[i].name = NULL; }
    pthread_mutex_unlock(&t->lock);
    return 0;
}

/* Name into a wide buffer of `size` characters; returns length copied. */
static UINT atom_name(struct atom_table *t, ATOM a, WCHAR *buf, int size)
{
    if (!buf || size <= 0) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    WCHAR tmp[ATOM_MAX_NAME + 1];
    size_t len;
    if (!a) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (a < MAXINTATOM) {
        char s[8];
        snprintf(s, sizeof s, "#%u", (unsigned)a);
        len = strlen(s);
        for (size_t k = 0; k <= len; k++) tmp[k] = (WCHAR)s[k];
    } else {
        size_t i = (size_t)(a - MAXINTATOM);
        pthread_mutex_lock(&t->lock);
        if (i >= t->n || !t->e[i].name) { pthread_mutex_unlock(&t->lock); lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }
        len = wlen(t->e[i].name);
        memcpy(tmp, t->e[i].name, (len + 1) * sizeof(WCHAR));
        pthread_mutex_unlock(&t->lock);
    }
    if (len >= (size_t)size) { len = (size_t)size - 1; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); }
    memcpy(buf, tmp, len * sizeof(WCHAR));
    buf[len] = 0;
    return (UINT)len;
}

static ATOM atom_add_a(struct atom_table *t, const char *name, int find_only)
{
    if (is_int_atom_ptr(name)) return atom_add(t, (const WCHAR *)name, find_only);
    WCHAR *w = a2w(name, -1);
    ATOM r = atom_add(t, w, find_only);
    free(w);
    return r;
}

static UINT atom_name_a(struct atom_table *t, ATOM a, char *buf, int size)
{
    if (!buf || size <= 0) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    WCHAR w[ATOM_MAX_NAME + 1];
    if (!atom_name(t, a, w, ATOM_MAX_NAME + 1)) return 0;
    char *s = w2a(w, -1);
    size_t len = strlen(s);
    if (len >= (size_t)size) { len = (size_t)size - 1; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); }
    memcpy(buf, s, len);
    buf[len] = 0;
    free(s);
    return (UINT)len;
}

API ATOM AddAtomW(const WCHAR *s) { return atom_add(&g_local_atoms, s, 0); }
API ATOM AddAtomA(const char *s) { return atom_add_a(&g_local_atoms, s, 0); }
API ATOM FindAtomW(const WCHAR *s) { return atom_add(&g_local_atoms, s, 1); }
API ATOM FindAtomA(const char *s) { return atom_add_a(&g_local_atoms, s, 1); }
API ATOM DeleteAtom(ATOM a) { return atom_delete(&g_local_atoms, a); }
API UINT GetAtomNameW(ATOM a, WCHAR *buf, int size) { return atom_name(&g_local_atoms, a, buf, size); }
API UINT GetAtomNameA(ATOM a, char *buf, int size) { return atom_name_a(&g_local_atoms, a, buf, size); }
API BOOL InitAtomTable(DWORD size) { (void)size; return TRUE; }

API ATOM GlobalAddAtomW(const WCHAR *s) { return atom_add(&g_global_atoms, s, 0); }
API ATOM GlobalAddAtomA(const char *s) { return atom_add_a(&g_global_atoms, s, 0); }
API ATOM GlobalAddAtomExW(const WCHAR *s, DWORD flags) { (void)flags; return atom_add(&g_global_atoms, s, 0); }
API ATOM GlobalAddAtomExA(const char *s, DWORD flags) { (void)flags; return atom_add_a(&g_global_atoms, s, 0); }
API ATOM GlobalFindAtomW(const WCHAR *s) { return atom_add(&g_global_atoms, s, 1); }
API ATOM GlobalFindAtomA(const char *s) { return atom_add_a(&g_global_atoms, s, 1); }
API ATOM GlobalDeleteAtom(ATOM a) { return atom_delete(&g_global_atoms, a); }
API UINT GlobalGetAtomNameW(ATOM a, WCHAR *buf, int size) { return atom_name(&g_global_atoms, a, buf, size); }
API UINT GlobalGetAtomNameA(ATOM a, char *buf, int size) { return atom_name_a(&g_global_atoms, a, buf, size); }

/* ================================================================== */
/* INI files (the Profile APIs)                                         */
/* ================================================================== */

/* Each call reads the file, works on its lines and, for writes, saves it
 * back in its original encoding (ANSI, UTF-8 with BOM, UTF-16LE with BOM).
 * A name without a directory lives in the Windows directory. */
enum { INI_ANSI, INI_UTF8, INI_UTF16 };

struct ini { WCHAR **line; size_t n, cap; int enc; int exists; char *path; };

static pthread_mutex_t g_ini_lock = PTHREAD_MUTEX_INITIALIZER;

static char *ini_path(const WCHAR *file)
{
    static const WCHAR winini[] = { 'w', 'i', 'n', '.', 'i', 'n', 'i', 0 };
    if (!file || !file[0]) file = winini;
    int has_dir = 0;
    for (const WCHAR *p = file; *p; p++) has_dir |= *p == '\\' || *p == '/' || *p == ':';
    if (has_dir) return lsiw_path_w(file);
    k32_ready();
    char dir[512] = "C:\\windows";
    if (k32.GetWindowsDirectoryA) k32.GetWindowsDirectoryA(dir, sizeof dir);
    char *u = lsiw_w2u(file);
    size_t n = strlen(dir) + strlen(u) + 2;
    char *full = xmalloc0(n);
    snprintf(full, n, "%s\\%s", dir, u);
    free(u);
    char *r = lsiw_path_a(full);
    free(full);
    return r;
}

static void ini_push(struct ini *f, WCHAR *l)
{
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 32; f->line = realloc(f->line, f->cap * sizeof *f->line); if (!f->line) abort(); }
    f->line[f->n++] = l;
}

static void ini_insert(struct ini *f, size_t at, WCHAR *l)
{
    ini_push(f, l);
    memmove(f->line + at + 1, f->line + at, (f->n - 1 - at) * sizeof *f->line);
    f->line[at] = l;
}

static void ini_remove(struct ini *f, size_t at)
{
    free(f->line[at]);
    memmove(f->line + at, f->line + at + 1, (f->n - at - 1) * sizeof *f->line);
    f->n--;
}

static WCHAR *wdup_n(const WCHAR *s, size_t n)
{
    WCHAR *d = xmalloc0((n + 1) * sizeof(WCHAR));
    memcpy(d, s, n * sizeof(WCHAR));
    return d;
}

static void ini_load(struct ini *f, const WCHAR *file)
{
    memset(f, 0, sizeof *f);
    f->path = ini_path(file);
    size_t len = 0;
    uint8_t *raw = f->path ? read_file(f->path, &len) : NULL;
    if (!raw) return;
    f->exists = 1;
    WCHAR *text;
    size_t tn;
    if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        f->enc = INI_UTF16;
        tn = (len - 2) / 2;
        text = xmalloc0((tn + 1) * sizeof(WCHAR));
        memcpy(text, raw + 2, tn * 2);
    } else if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        f->enc = INI_UTF8;
        text = lsiw_u2w((const char *)raw + 3);
        tn = wlen(text);
    } else {
        f->enc = INI_ANSI;
        text = a2w((const char *)raw, (int)len);
        tn = wlen(text);
    }
    free(raw);
    size_t start = 0;
    for (size_t i = 0; i <= tn; i++) {
        if (i == tn || text[i] == '\n' || text[i] == '\r' || text[i] == 0) {
            if (i == tn && i == start) break;
            ini_push(f, wdup_n(text + start, i - start));
            if (i < tn && text[i] == '\r' && i + 1 < tn && text[i + 1] == '\n') i++;
            start = i + 1;
            if (i < tn && text[i] == 0) break;
        }
    }
    free(text);
}

static void ini_free(struct ini *f)
{
    for (size_t i = 0; i < f->n; i++) free(f->line[i]);
    free(f->line);
    free(f->path);
}

static BOOL ini_save(struct ini *f)
{
    if (!f->path) { lsiw_set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    struct wbuf b = { 0 };
    static const WCHAR crlf[2] = { '\r', '\n' };
    for (size_t i = 0; i < f->n; i++) { wb_s(&b, f->line[i]); wb_put(&b, crlf, 2); }
    if (!b.p) wb_put(&b, NULL, 0);
    uint8_t *out;
    size_t on;
    if (f->enc == INI_UTF16) {
        on = 2 + b.n * 2;
        out = xmalloc0(on);
        out[0] = 0xFF; out[1] = 0xFE;
        memcpy(out + 2, b.p, b.n * 2);
    } else {
        char *s = f->enc == INI_UTF8 ? lsiw_w2u(b.p) : w2a(b.p, (int)b.n);
        size_t sl = strlen(s), bom = f->enc == INI_UTF8 ? 3 : 0;
        on = bom + sl;
        out = xmalloc0(on + 1);
        if (bom) { out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF; }
        memcpy(out + bom, s, sl);
        free(s);
    }
    free(b.p);
    int fd = open(f->path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        free(out);
        lsiw_set_last_error(errno == ENOENT ? ERROR_PATH_NOT_FOUND : ERROR_ACCESS_DENIED);
        return FALSE;
    }
    size_t done = 0;
    while (done < on) {
        ssize_t w = write(fd, out + done, on - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
    close(fd);
    free(out);
    if (done < on) { lsiw_set_last_error(ERROR_WRITE_FAULT); return FALSE; }
    f->exists = 1;
    return TRUE;
}

static int wspace(WCHAR c) { return c == ' ' || c == '\t'; }

/* Trimmed view [*b, *e) of s. */
static void trim(const WCHAR *s, size_t n, const WCHAR **b, const WCHAR **e)
{
    const WCHAR *x = s, *y = s + n;
    while (x < y && wspace(*x)) x++;
    while (y > x && wspace(y[-1])) y--;
    *b = x;
    *e = y;
}

/* If the line is "[name]", return the trimmed name span. */
static int ini_section_name(const WCHAR *l, const WCHAR **b, const WCHAR **e)
{
    const WCHAR *x, *y;
    trim(l, wlen(l), &x, &y);
    if (x == y || *x != '[') return 0;
    const WCHAR *close = x + 1;
    while (close < y && *close != ']') close++;
    trim(x + 1, (size_t)(close - x - 1), b, e);
    return 1;
}

static int span_ieq(const WCHAR *b, const WCHAR *e, const WCHAR *s)
{
    size_t n = (size_t)(e - b);
    if (wlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++) if (wupper(b[i]) != wupper(s[i])) return 0;
    return 1;
}

/* Trim of a lookup name, as Windows matches "  key " as "key". */
static WCHAR *trimmed_copy(const WCHAR *s)
{
    const WCHAR *b, *e;
    trim(s, wlen(s), &b, &e);
    return wdup_n(b, (size_t)(e - b));
}

/* Index of the section header line, or -1. */
static long ini_find_section(struct ini *f, const WCHAR *name)
{
    WCHAR *t = trimmed_copy(name);
    long r = -1;
    for (size_t i = 0; i < f->n && r < 0; i++) {
        const WCHAR *b, *e;
        if (ini_section_name(f->line[i], &b, &e) && span_ieq(b, e, t)) r = (long)i;
    }
    free(t);
    return r;
}

/* End (exclusive) of the section starting at header `s`. */
static size_t ini_section_end(struct ini *f, size_t s)
{
    size_t i = s + 1;
    const WCHAR *b, *e;
    while (i < f->n && !ini_section_name(f->line[i], &b, &e)) i++;
    return i;
}

/* Key span of line l ("key=value"), or 0 for a line without '=', a blank
 * line or a comment. */
static int ini_key(const WCHAR *l, const WCHAR **kb, const WCHAR **ke, const WCHAR **eq)
{
    const WCHAR *x, *y;
    trim(l, wlen(l), &x, &y);
    if (x == y || *x == ';') return 0;
    const WCHAR *q = x;
    while (q < y && *q != '=') q++;
    trim(x, (size_t)(q - x), kb, ke);
    *eq = q < y ? q : NULL;
    return 1;
}

static long ini_find_key(struct ini *f, size_t sec, const WCHAR *key)
{
    WCHAR *t = trimmed_copy(key);
    size_t end = ini_section_end(f, sec);
    long r = -1;
    for (size_t i = sec + 1; i < end && r < 0; i++) {
        const WCHAR *kb, *ke, *eq;
        if (ini_key(f->line[i], &kb, &ke, &eq) && span_ieq(kb, ke, t)) r = (long)i;
    }
    free(t);
    return r;
}

/* Value of a line: trimmed text after '=', one pair of surrounding quotes
 * removed. */
static WCHAR *ini_value(const WCHAR *l)
{
    const WCHAR *kb, *ke, *eq;
    if (!ini_key(l, &kb, &ke, &eq) || !eq) return wdup_n(l, 0);
    const WCHAR *b, *e;
    trim(eq + 1, wlen(eq + 1), &b, &e);
    if (e - b >= 2 && (*b == '"' || *b == '\'') && e[-1] == *b) { b++; e--; }
    return wdup_n(b, (size_t)(e - b));
}

/* Copy a single string into buf (size characters); returns characters
 * copied without NUL, size-1 when truncated (ERROR_MORE_DATA). */
static DWORD ini_ret_str(const WCHAR *s, size_t n, WCHAR *buf, DWORD size)
{
    if (!buf || !size) return 0;
    if (n >= size) { n = size - 1; lsiw_set_last_error(ERROR_MORE_DATA); }
    memcpy(buf, s, n * sizeof(WCHAR));
    buf[n] = 0;
    return (DWORD)n;
}

/* Copy a NUL-separated, double-NUL-terminated list; returns characters
 * copied without the final NUL, size-2 when truncated. */
static DWORD ini_ret_list(const WCHAR *s, size_t n, WCHAR *buf, DWORD size)
{
    if (!buf || !size) return 0;
    if (size == 1) { buf[0] = 0; return 0; }
    if (n + 1 > size) {
        memcpy(buf, s, (size - 2) * sizeof(WCHAR));
        buf[size - 2] = 0;
        buf[size - 1] = 0;
        lsiw_set_last_error(ERROR_MORE_DATA);
        return size - 2;
    }
    memcpy(buf, s, n * sizeof(WCHAR));
    buf[n] = 0;
    if (n == 0 && size > 1) buf[1] = 0;
    return (DWORD)n;
}

/* Section names, or keys of one section, as a list. */
static DWORD ini_list(struct ini *f, const WCHAR *sec, WCHAR *buf, DWORD size)
{
    struct wbuf b = { 0 };
    if (!sec) {
        for (size_t i = 0; i < f->n; i++) {
            const WCHAR *x, *y;
            if (ini_section_name(f->line[i], &x, &y)) { wb_put(&b, x, (size_t)(y - x)); wb_c(&b, 0); }
        }
    } else {
        long s = ini_find_section(f, sec);
        if (s >= 0) {
            size_t end = ini_section_end(f, (size_t)s);
            for (size_t i = (size_t)s + 1; i < end; i++) {
                const WCHAR *kb, *ke, *eq;
                if (ini_key(f->line[i], &kb, &ke, &eq)) { wb_put(&b, kb, (size_t)(ke - kb)); wb_c(&b, 0); }
            }
        }
    }
    wb_c(&b, 0);
    DWORD r = ini_ret_list(b.p, b.n - 1, buf, size);
    free(b.p);
    return r;
}

API DWORD GetPrivateProfileStringW(const WCHAR *sec, const WCHAR *key, const WCHAR *def, WCHAR *buf, DWORD size, const WCHAR *file)
{
    if (!buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_ini_lock);
    struct ini f;
    ini_load(&f, file);
    lsiw_set_last_error(f.exists ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
    DWORD r;
    if (!sec || !key) {
        r = ini_list(&f, sec, buf, size);
    } else {
        long s = ini_find_section(&f, sec), k = s >= 0 ? ini_find_key(&f, (size_t)s, key) : -1;
        if (k >= 0) {
            WCHAR *v = ini_value(f.line[k]);
            r = ini_ret_str(v, wlen(v), buf, size);
            free(v);
        } else {
            const WCHAR *d = def ? def : buf + size;   /* empty when NULL */
            size_t n = def ? wlen(def) : 0;
            while (n && d[n - 1] == ' ') n--;            /* trailing blanks of the default go */
            WCHAR *tmp = wdup_n(d, n);
            r = ini_ret_str(tmp, n, buf, size);
            free(tmp);
            if (f.exists) lsiw_set_last_error(ERROR_FILE_NOT_FOUND);
        }
    }
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return r;
}

/* Delete (key NULL: the section; value NULL: the key) or set a value. */
static BOOL ini_write(const WCHAR *sec, const WCHAR *key, const WCHAR *val, const WCHAR *file)
{
    if (!sec) return TRUE;               /* (NULL, NULL, NULL): flush the cache, there is none */
    pthread_mutex_lock(&g_ini_lock);
    struct ini f;
    ini_load(&f, file);
    long s = ini_find_section(&f, sec);
    BOOL ok = TRUE, dirty = FALSE;
    if (!key) {
        if (s >= 0) {
            size_t end = ini_section_end(&f, (size_t)s);
            while (end-- > (size_t)s) ini_remove(&f, (size_t)s);
            dirty = TRUE;
        }
    } else if (!val) {
        long k = s >= 0 ? ini_find_key(&f, (size_t)s, key) : -1;
        if (k >= 0) { ini_remove(&f, (size_t)k); dirty = TRUE; }
    } else {
        WCHAR *tk = trimmed_copy(key);
        struct wbuf l = { 0 };
        wb_s(&l, tk);
        wb_c(&l, '=');
        wb_s(&l, val);
        free(tk);
        if (s < 0) {
            WCHAR *ts = trimmed_copy(sec);
            struct wbuf h = { 0 };
            wb_c(&h, '['); wb_s(&h, ts); wb_c(&h, ']');
            free(ts);
            ini_push(&f, h.p);
            ini_push(&f, l.p);
        } else {
            long k = ini_find_key(&f, (size_t)s, key);
            if (k >= 0) { free(f.line[k]); f.line[k] = l.p; }
            else {
                size_t end = ini_section_end(&f, (size_t)s);
                while (end > (size_t)s + 1 && !f.line[end - 1][0]) end--;   /* before trailing blank lines */
                ini_insert(&f, end, l.p);
            }
        }
        dirty = TRUE;
    }
    if (dirty) ok = ini_save(&f);
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return ok;
}

API BOOL WritePrivateProfileStringW(const WCHAR *sec, const WCHAR *key, const WCHAR *val, const WCHAR *file)
{
    if (!sec && (key || val)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return ini_write(sec, key, val, file);
}

static int parse_int_w(const WCHAR *s, int *out)
{
    while (wspace(*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (*s < '0' || *s > '9') return 0;
    long long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 0xFFFFFFFFLL) v = 0xFFFFFFFFLL; s++; }
    *out = (int)(neg ? -v : v);
    return 1;
}

API UINT GetPrivateProfileIntW(const WCHAR *sec, const WCHAR *key, int def, const WCHAR *file)
{
    if (!sec || !key) return (UINT)def;
    WCHAR buf[64];
    static const WCHAR none[1];
    DWORD n = GetPrivateProfileStringW(sec, key, none, buf, 64, file);
    int v;
    if (!n) return (UINT)def;
    return parse_int_w(buf, &v) ? (UINT)v : 0;
}

API DWORD GetPrivateProfileSectionW(const WCHAR *sec, WCHAR *buf, DWORD size, const WCHAR *file)
{
    if (!sec || !buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_ini_lock);
    struct ini f;
    ini_load(&f, file);
    struct wbuf b = { 0 };
    long s = ini_find_section(&f, sec);
    if (s >= 0) {
        size_t end = ini_section_end(&f, (size_t)s);
        for (size_t i = (size_t)s + 1; i < end; i++) {
            const WCHAR *x, *y;
            trim(f.line[i], wlen(f.line[i]), &x, &y);
            if (x == y || *x == ';') continue;
            wb_put(&b, x, (size_t)(y - x));
            wb_c(&b, 0);
        }
    }
    wb_c(&b, 0);
    DWORD r = ini_ret_list(b.p, b.n - 1, buf, size);
    free(b.p);
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return r;
}

API BOOL WritePrivateProfileSectionW(const WCHAR *sec, const WCHAR *data, const WCHAR *file)
{
    if (!sec) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!data) return ini_write(sec, NULL, NULL, file);
    pthread_mutex_lock(&g_ini_lock);
    struct ini f;
    ini_load(&f, file);
    long s = ini_find_section(&f, sec);
    size_t at;
    if (s >= 0) {
        size_t end = ini_section_end(&f, (size_t)s);
        while (end-- > (size_t)s + 1) ini_remove(&f, (size_t)s + 1);
        at = (size_t)s + 1;
    } else {
        WCHAR *ts = trimmed_copy(sec);
        struct wbuf h = { 0 };
        wb_c(&h, '['); wb_s(&h, ts); wb_c(&h, ']');
        free(ts);
        ini_push(&f, h.p);
        at = f.n;
    }
    for (const WCHAR *p = data; *p; p += wlen(p) + 1) ini_insert(&f, at++, wdup_n(p, wlen(p)));
    BOOL ok = ini_save(&f);
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return ok;
}

API DWORD GetPrivateProfileSectionNamesW(WCHAR *buf, DWORD size, const WCHAR *file)
{
    if (!buf || !size) return 0;
    pthread_mutex_lock(&g_ini_lock);
    struct ini f;
    ini_load(&f, file);
    DWORD r = ini_list(&f, NULL, buf, size);
    ini_free(&f);
    pthread_mutex_unlock(&g_ini_lock);
    return r;
}

/* Structs are stored as hex bytes followed by a one-byte checksum
 * (sum of the bytes, modulo 256). */
API BOOL GetPrivateProfileStructW(const WCHAR *sec, const WCHAR *key, void *data, UINT size, const WCHAR *file)
{
    if (!sec || !key || !data) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t need = ((size_t)size + 1) * 2;
    WCHAR *buf = xmalloc0((need + 2) * sizeof(WCHAR));
    static const WCHAR none[1];
    DWORD n = GetPrivateProfileStringW(sec, key, none, buf, (DWORD)need + 2, file);
    BOOL ok = FALSE;
    if (n == need) {
        uint8_t *out = data, sum = 0;
        ok = TRUE;
        for (size_t i = 0; i <= size && ok; i++) {
            unsigned v = 0;
            for (int k = 0; k < 2; k++) {
                WCHAR c = buf[i * 2 + (size_t)k];
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (d < 0) { ok = FALSE; break; }
                v = v * 16 + (unsigned)d;
            }
            if (!ok) break;
            if (i < size) { out[i] = (uint8_t)v; sum = (uint8_t)(sum + v); }
            else if (sum != (uint8_t)v) ok = FALSE;
        }
        if (!ok) lsiw_set_last_error(ERROR_INVALID_DATA);
    } else {
        lsiw_set_last_error(n ? ERROR_BAD_LENGTH : ERROR_FILE_NOT_FOUND);
    }
    free(buf);
    return ok;
}

API BOOL WritePrivateProfileStructW(const WCHAR *sec, const WCHAR *key, const void *data, UINT size, const WCHAR *file)
{
    if (!sec) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!key || !data) return WritePrivateProfileStringW(sec, key, NULL, file);
    static const char hex[] = "0123456789ABCDEF";
    WCHAR *s = xmalloc0(((size_t)size * 2 + 3) * sizeof(WCHAR));
    const uint8_t *p = data;
    uint8_t sum = 0;
    for (UINT i = 0; i < size; i++) { s[i * 2] = (WCHAR)hex[p[i] >> 4]; s[i * 2 + 1] = (WCHAR)hex[p[i] & 15]; sum = (uint8_t)(sum + p[i]); }
    s[size * 2] = (WCHAR)hex[sum >> 4];
    s[size * 2 + 1] = (WCHAR)hex[sum & 15];
    BOOL ok = WritePrivateProfileStringW(sec, key, s, file);
    free(s);
    return ok;
}

/* ---- ANSI forms ---- */

/* Convert a wide result (a string, or a list of wn units with its inner
 * NULs) back to ANSI in the caller's buffer, with the same return rules. */
static DWORD ini_list_to_a(const WCHAR *w, DWORD wn, char *buf, DWORD size, int is_list)
{
    if (!buf || !size) return 0;
    size_t an = 0;
    char *a;
    k32_ready();
    if (k32.WideCharToMultiByte) {
        an = wn ? (size_t)k32.WideCharToMultiByte(CP_ACP, 0, w, (int)wn, NULL, 0, NULL, NULL) : 0;
        a = xmalloc0(an + 1);
        if (an) k32.WideCharToMultiByte(CP_ACP, 0, w, (int)wn, a, (int)an, NULL, NULL);
    } else {
        a = xmalloc0((size_t)wn * 3 + 1);
        for (DWORD i = 0; i < wn; i++) a[an++] = w[i] < 0x80 ? (char)w[i] : '?';
    }
    DWORD r;
    if (is_list) {
        if (an + 1 > size) {
            if (size >= 2) { memcpy(buf, a, size - 2); buf[size - 2] = 0; buf[size - 1] = 0; r = size - 2; }
            else { buf[0] = 0; r = 0; }
        } else {
            memcpy(buf, a, an);
            buf[an] = 0;
            if (an == 0 && size > 1) buf[1] = 0;
            r = (DWORD)an;
        }
    } else {
        if (an >= size) an = size - 1;
        memcpy(buf, a, an);
        buf[an] = 0;
        r = (DWORD)an;
    }
    free(a);
    return r;
}

/* Wide buffer size for an ANSI request of `size` bytes. */
#define INI_WSIZE(size) ((size) ? (size) : 1)

API DWORD GetPrivateProfileStringA(const char *sec, const char *key, const char *def, char *buf, DWORD size, const char *file)
{
    if (!buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    WCHAR *ws = a2w(sec, -1), *wk = a2w(key, -1), *wd = a2w(def, -1), *wf = a2w(file, -1);
    WCHAR *w = xmalloc0((size_t)INI_WSIZE(size) * sizeof(WCHAR));
    DWORD n = GetPrivateProfileStringW(ws, wk, wd, w, size, wf);
    DWORD e = lsiw_get_last_error();
    DWORD r = ini_list_to_a(w, n, buf, size, !sec || !key);
    lsiw_set_last_error(e);
    free(ws); free(wk); free(wd); free(wf); free(w);
    return r;
}

API BOOL WritePrivateProfileStringA(const char *sec, const char *key, const char *val, const char *file)
{
    WCHAR *ws = a2w(sec, -1), *wk = a2w(key, -1), *wv = a2w(val, -1), *wf = a2w(file, -1);
    BOOL r = WritePrivateProfileStringW(ws, wk, wv, wf);
    free(ws); free(wk); free(wv); free(wf);
    return r;
}

API UINT GetPrivateProfileIntA(const char *sec, const char *key, int def, const char *file)
{
    WCHAR *ws = a2w(sec, -1), *wk = a2w(key, -1), *wf = a2w(file, -1);
    UINT r = GetPrivateProfileIntW(ws, wk, def, wf);
    free(ws); free(wk); free(wf);
    return r;
}

API DWORD GetPrivateProfileSectionA(const char *sec, char *buf, DWORD size, const char *file)
{
    if (!sec || !buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    WCHAR *ws = a2w(sec, -1), *wf = a2w(file, -1);
    WCHAR *w = xmalloc0((size_t)size * sizeof(WCHAR));
    DWORD n = GetPrivateProfileSectionW(ws, w, size, wf);
    DWORD r = ini_list_to_a(w, n, buf, size, 1);
    free(ws); free(wf); free(w);
    return r;
}

/* Length of a double-NUL-terminated ANSI list including the final NUL. */
static size_t alist_len(const char *s) { const char *p = s; while (*p) p += strlen(p) + 1; return (size_t)(p - s) + 1; }

API BOOL WritePrivateProfileSectionA(const char *sec, const char *data, const char *file)
{
    WCHAR *ws = a2w(sec, -1), *wf = a2w(file, -1), *wd = NULL;
    if (data) wd = a2w(data, (int)alist_len(data));
    BOOL r = WritePrivateProfileSectionW(ws, wd, wf);
    free(ws); free(wf); free(wd);
    return r;
}

API DWORD GetPrivateProfileSectionNamesA(char *buf, DWORD size, const char *file)
{
    if (!buf || !size) return 0;
    WCHAR *wf = a2w(file, -1);
    WCHAR *w = xmalloc0((size_t)size * sizeof(WCHAR));
    DWORD n = GetPrivateProfileSectionNamesW(w, size, wf);
    DWORD r = ini_list_to_a(w, n, buf, size, 1);
    free(wf); free(w);
    return r;
}

API BOOL GetPrivateProfileStructA(const char *sec, const char *key, void *data, UINT size, const char *file)
{
    WCHAR *ws = a2w(sec, -1), *wk = a2w(key, -1), *wf = a2w(file, -1);
    BOOL r = GetPrivateProfileStructW(ws, wk, data, size, wf);
    free(ws); free(wk); free(wf);
    return r;
}

API BOOL WritePrivateProfileStructA(const char *sec, const char *key, const void *data, UINT size, const char *file)
{
    WCHAR *ws = a2w(sec, -1), *wk = a2w(key, -1), *wf = a2w(file, -1);
    BOOL r = WritePrivateProfileStructW(ws, wk, data, size, wf);
    free(ws); free(wk); free(wf);
    return r;
}

/* ---- win.ini ---- */

API DWORD GetProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *d, WCHAR *b, DWORD n) { return GetPrivateProfileStringW(s, k, d, b, n, NULL); }
API DWORD GetProfileStringA(const char *s, const char *k, const char *d, char *b, DWORD n) { return GetPrivateProfileStringA(s, k, d, b, n, NULL); }
API BOOL WriteProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *v) { return WritePrivateProfileStringW(s, k, v, NULL); }
API BOOL WriteProfileStringA(const char *s, const char *k, const char *v) { return WritePrivateProfileStringA(s, k, v, NULL); }
API UINT GetProfileIntW(const WCHAR *s, const WCHAR *k, int d) { return GetPrivateProfileIntW(s, k, d, NULL); }
API UINT GetProfileIntA(const char *s, const char *k, int d) { return GetPrivateProfileIntA(s, k, d, NULL); }
API DWORD GetProfileSectionW(const WCHAR *s, WCHAR *b, DWORD n) { return GetPrivateProfileSectionW(s, b, n, NULL); }
API DWORD GetProfileSectionA(const char *s, char *b, DWORD n) { return GetPrivateProfileSectionA(s, b, n, NULL); }
API BOOL WriteProfileSectionW(const WCHAR *s, const WCHAR *d) { return WritePrivateProfileSectionW(s, d, NULL); }
API BOOL WriteProfileSectionA(const char *s, const char *d) { return WritePrivateProfileSectionA(s, d, NULL); }

/* ================================================================== */
/* Activation contexts                                                  */
/* ================================================================== */

/* Side-by-side assemblies do not exist here, so a context only records
 * where its manifest came from; activation is a per-thread stack and every
 * section lookup reports "not found", which makes callers fall back to the
 * normal DLL search. */
#define ACTCTX_FLAG_PROCESSOR_ARCHITECTURE_VALID 0x001
#define ACTCTX_FLAG_LANGID_VALID                 0x002
#define ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID     0x004
#define ACTCTX_FLAG_RESOURCE_NAME_VALID          0x008
#define ACTCTX_FLAG_SET_PROCESS_DEFAULT          0x010
#define ACTCTX_FLAG_APPLICATION_NAME_VALID       0x020
#define ACTCTX_FLAG_HMODULE_VALID                0x080
#define ERROR_SXS_KEY_NOT_FOUND                  14007
#define ERROR_SXS_PROCESS_DEFAULT_ALREADY_SET    14011
#define ERROR_SXS_INVALID_DEACTIVATION           14013
#define ERROR_SXS_CANT_GEN_ACTCTX                14001

typedef struct {
    ULONG cbSize; DWORD dwFlags; const WCHAR *lpSource; USHORT wProcessorArchitecture; LANGID wLangId;
    const WCHAR *lpAssemblyDirectory; const WCHAR *lpResourceName; const WCHAR *lpApplicationName; HMODULE hModule;
} ACTCTXW;
typedef struct {
    ULONG cbSize; DWORD dwFlags; const char *lpSource; USHORT wProcessorArchitecture; LANGID wLangId;
    const char *lpAssemblyDirectory; const char *lpResourceName; const char *lpApplicationName; HMODULE hModule;
} ACTCTXA;
_Static_assert(sizeof(ACTCTXW) == 56, "ACTCTXW");

#define ACTCTX_MAGIC 0x41435458u

struct actctx { uint32_t magic; int refs; WCHAR *manifest; WCHAR *appdir; struct actctx *next; };
static struct actctx *g_actctxs;
static struct actctx *g_process_actctx;
static pthread_mutex_t g_actctx_lock = PTHREAD_MUTEX_INITIALIZER;

struct actframe { struct actctx *ctx; ULONG_PTR cookie; };
static __thread struct actframe *t_act;
static __thread size_t t_nact, t_capact;
static ULONG_PTR g_cookie_seq = 0x1000;

/* Must hold g_actctx_lock. */
static struct actctx *actctx_lookup(HANDLE h)
{
    for (struct actctx *a = g_actctxs; a; a = a->next) if ((HANDLE)a == h && a->magic == ACTCTX_MAGIC) return a;
    return NULL;
}

static WCHAR *wdup(const WCHAR *s) { return s ? wdup_n(s, wlen(s)) : NULL; }

API HANDLE CreateActCtxW(const ACTCTXW *ac)
{
    if (!ac || ac->cbSize < offsetof(ACTCTXW, lpAssemblyDirectory) || ac->cbSize > sizeof *ac) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    DWORD fl = ac->dwFlags;
    const WCHAR *appdir = (fl & ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID) && ac->cbSize > offsetof(ACTCTXW, lpAssemblyDirectory) ? ac->lpAssemblyDirectory : NULL;
    HMODULE mod = (fl & ACTCTX_FLAG_HMODULE_VALID) && ac->cbSize > offsetof(ACTCTXW, hModule) ? ac->hModule : NULL;
    WCHAR *manifest = NULL;
    if (ac->lpSource) {
        char *p = lsiw_path_w(ac->lpSource);
        struct stat st;
        int ok = p && stat(p, &st) == 0;
        free(p);
        if (!ok) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
        if (S_ISDIR(st.st_mode)) { lsiw_set_last_error(ERROR_SXS_CANT_GEN_ACTCTX); return INVALID_HANDLE_VALUE; }
        manifest = wdup(ac->lpSource);
    } else if (!mod) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    struct actctx *a = xmalloc0(sizeof *a);
    a->magic = ACTCTX_MAGIC;
    a->refs = 1;
    a->manifest = manifest;
    a->appdir = wdup(appdir);
    pthread_mutex_lock(&g_actctx_lock);
    if (fl & ACTCTX_FLAG_SET_PROCESS_DEFAULT) {
        if (g_process_actctx) {
            pthread_mutex_unlock(&g_actctx_lock);
            free(a->manifest); free(a->appdir); free(a);
            lsiw_set_last_error(ERROR_SXS_PROCESS_DEFAULT_ALREADY_SET);
            return INVALID_HANDLE_VALUE;
        }
        g_process_actctx = a;
        a->refs++;
    }
    a->next = g_actctxs;
    g_actctxs = a;
    pthread_mutex_unlock(&g_actctx_lock);
    if (fl & ACTCTX_FLAG_SET_PROCESS_DEFAULT) return NULL;   /* as Windows: no handle returned */
    return (HANDLE)a;
}

API HANDLE CreateActCtxA(const ACTCTXA *ac)
{
    if (!ac || ac->cbSize < offsetof(ACTCTXA, lpAssemblyDirectory) || ac->cbSize > sizeof *ac) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    ACTCTXW w;
    memset(&w, 0, sizeof w);
    memcpy(&w, ac, ac->cbSize);         /* same layout; strings converted below */
    WCHAR *src = a2w(ac->lpSource, -1), *dir = NULL, *res = NULL, *app = NULL;
    w.lpSource = src;
    if (ac->cbSize > offsetof(ACTCTXA, lpAssemblyDirectory)) w.lpAssemblyDirectory = dir = a2w(ac->lpAssemblyDirectory, -1);
    if (ac->cbSize > offsetof(ACTCTXA, lpResourceName)) {
        if (is_int_atom_ptr(ac->lpResourceName)) w.lpResourceName = (const WCHAR *)ac->lpResourceName;
        else w.lpResourceName = res = a2w(ac->lpResourceName, -1);
    }
    if (ac->cbSize > offsetof(ACTCTXA, lpApplicationName)) w.lpApplicationName = app = a2w(ac->lpApplicationName, -1);
    HANDLE h = CreateActCtxW(&w);
    DWORD e = lsiw_get_last_error();
    free(src); free(dir); free(res); free(app);
    lsiw_set_last_error(e);
    return h;
}

static void actctx_unref_locked(struct actctx *a)
{
    if (--a->refs > 0) return;
    for (struct actctx **pp = &g_actctxs; *pp; pp = &(*pp)->next)
        if (*pp == a) { *pp = a->next; break; }
    a->magic = 0;
    free(a->manifest);
    free(a->appdir);
    free(a);
}

API void AddRefActCtx(HANDLE h)
{
    pthread_mutex_lock(&g_actctx_lock);
    struct actctx *a = actctx_lookup(h);
    if (a) a->refs++;
    pthread_mutex_unlock(&g_actctx_lock);
}

API void ReleaseActCtx(HANDLE h)
{
    pthread_mutex_lock(&g_actctx_lock);
    struct actctx *a = actctx_lookup(h);
    if (a) actctx_unref_locked(a);
    pthread_mutex_unlock(&g_actctx_lock);
}

API BOOL ZombifyActCtx(HANDLE h)
{
    pthread_mutex_lock(&g_actctx_lock);
    struct actctx *a = actctx_lookup(h);
    pthread_mutex_unlock(&g_actctx_lock);
    if (!a) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

API BOOL ActivateActCtx(HANDLE h, ULONG_PTR *cookie)
{
    if (!cookie) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_actctx_lock);
    struct actctx *a = h ? actctx_lookup(h) : NULL;
    if (h && h != INVALID_HANDLE_VALUE && !a) { pthread_mutex_unlock(&g_actctx_lock); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (a) a->refs++;
    ULONG_PTR c = ++g_cookie_seq;
    pthread_mutex_unlock(&g_actctx_lock);
    if (t_nact == t_capact) {
        t_capact = t_capact ? t_capact * 2 : 8;
        t_act = realloc(t_act, t_capact * sizeof *t_act);
        if (!t_act) abort();
    }
    t_act[t_nact].ctx = a;
    t_act[t_nact].cookie = c;
    t_nact++;
    *cookie = c;
    return TRUE;
}

#define DEACTIVATE_ACTCTX_FLAG_FORCE_EARLY_DEACTIVATION 1

API BOOL DeactivateActCtx(DWORD flags, ULONG_PTR cookie)
{
    size_t i = t_nact;
    while (i > 0 && t_act[i - 1].cookie != cookie) i--;
    if (!i) { lsiw_set_last_error(ERROR_SXS_INVALID_DEACTIVATION); return FALSE; }
    if (i != t_nact && !(flags & DEACTIVATE_ACTCTX_FLAG_FORCE_EARLY_DEACTIVATION)) {
        lsiw_set_last_error(ERROR_SXS_INVALID_DEACTIVATION);   /* not the innermost activation */
        return FALSE;
    }
    pthread_mutex_lock(&g_actctx_lock);
    while (t_nact >= i) {
        struct actctx *a = t_act[--t_nact].ctx;
        if (a && a->magic == ACTCTX_MAGIC) actctx_unref_locked(a);
        if (t_nact + 1 == i) break;
    }
    pthread_mutex_unlock(&g_actctx_lock);
    return TRUE;
}

API BOOL GetCurrentActCtx(HANDLE *h)
{
    if (!h) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_actctx_lock);
    struct actctx *a = t_nact ? t_act[t_nact - 1].ctx : NULL;
    if (a && a->magic == ACTCTX_MAGIC) a->refs++; else a = NULL;
    pthread_mutex_unlock(&g_actctx_lock);
    *h = (HANDLE)a;
    return TRUE;
}

#define QUERY_ACTCTX_FLAG_USE_ACTIVE_ACTCTX 0x4
#define QUERY_ACTCTX_FLAG_ACTCTX_IS_HMODULE 0x8
#define QUERY_ACTCTX_FLAG_ACTCTX_IS_ADDRESS 0x10
#define QUERY_ACTCTX_FLAG_NO_ADDREF         0x80000000u

typedef struct {
    DWORD dwFlags, ulFormatVersion, ulAssemblyCount, ulRootManifestPathType, ulRootManifestPathChars,
          ulRootConfigurationPathType, ulRootConfigurationPathChars, ulAppDirPathType, ulAppDirPathChars;
    const WCHAR *lpRootManifestPath, *lpRootConfigurationPath, *lpAppDirPath;
} ACTIVATION_CONTEXT_DETAILED_INFORMATION;
_Static_assert(sizeof(ACTIVATION_CONTEXT_DETAILED_INFORMATION) == 64, "ACTIVATION_CONTEXT_DETAILED_INFORMATION");

API BOOL QueryActCtxW(DWORD flags, HANDLE h, void *sub, ULONG cls, void *buf, SIZE_T cb, SIZE_T *written)
{
    (void)sub;
    pthread_mutex_lock(&g_actctx_lock);
    struct actctx *a = NULL;
    if (flags & QUERY_ACTCTX_FLAG_USE_ACTIVE_ACTCTX) a = t_nact ? t_act[t_nact - 1].ctx : NULL;
    else if (flags & (QUERY_ACTCTX_FLAG_ACTCTX_IS_HMODULE | QUERY_ACTCTX_FLAG_ACTCTX_IS_ADDRESS)) a = g_process_actctx;
    else if (h) {
        a = actctx_lookup(h);
        if (!a) { pthread_mutex_unlock(&g_actctx_lock); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    } else a = g_process_actctx;
    if (a && a->magic != ACTCTX_MAGIC) a = NULL;
    BOOL ok = TRUE;
    SIZE_T need = 0;
    switch (cls) {
    case 1: {                                          /* ActivationContextBasicInformation */
        need = 16;
        if (buf && cb >= need) {
            *(HANDLE *)buf = (HANDLE)a;
            ((DWORD *)buf)[2] = 0;
            ((DWORD *)buf)[3] = 0;
            if (a && !(flags & QUERY_ACTCTX_FLAG_NO_ADDREF)) a->refs++;
        }
        break;
    }
    case 2: {                                          /* ActivationContextDetailedInformation */
        size_t mn = a ? wlen(a->manifest) : 0, dn = a ? wlen(a->appdir) : 0;
        need = sizeof(ACTIVATION_CONTEXT_DETAILED_INFORMATION) + (mn ? (mn + 1) * 2 : 0) + (dn ? (dn + 1) * 2 : 0);
        if (buf && cb >= need) {
            ACTIVATION_CONTEXT_DETAILED_INFORMATION *d = buf;
            memset(d, 0, sizeof *d);
            d->ulFormatVersion = 1;
            d->ulAssemblyCount = a && a->manifest ? 1 : 0;
            WCHAR *p = (WCHAR *)(d + 1);
            if (mn) {
                d->ulRootManifestPathType = 1;      /* ACTIVATION_CONTEXT_PATH_TYPE_WIN32_FILE */
                d->ulRootManifestPathChars = (DWORD)mn;
                memcpy(p, a->manifest, (mn + 1) * 2);
                d->lpRootManifestPath = p;
                p += mn + 1;
            }
            if (dn) {
                d->ulAppDirPathType = 1;
                d->ulAppDirPathChars = (DWORD)dn;
                memcpy(p, a->appdir, (dn + 1) * 2);
                d->lpAppDirPath = p;
            }
        }
        break;
    }
    case 5:                                            /* CompatibilityInformationInActivationContext */
        need = 4;
        if (buf && cb >= need) *(DWORD *)buf = 0;
        break;
    case 6:                                            /* RunlevelInformationInActivationContext */
        need = 12;
        if (buf && cb >= need) { ((DWORD *)buf)[0] = 0; ((DWORD *)buf)[1] = 1; /* asInvoker */ ((DWORD *)buf)[2] = 0; }
        break;
    case 3: case 4:                                    /* assembly / file detail: no assemblies */
        ok = FALSE;
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        break;
    default:
        ok = FALSE;
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    }
    pthread_mutex_unlock(&g_actctx_lock);
    if (!ok) return FALSE;
    if (written) *written = need;
    if (!buf || cb < need) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    return TRUE;
}

/* Manifest application settings (dpiAware, heapType, ...) are not parsed. */
API BOOL QueryActCtxSettingsW(DWORD flags, HANDLE h, const WCHAR *ns, const WCHAR *name, WCHAR *buf, SIZE_T cb, SIZE_T *written)
{
    (void)h; (void)ns; (void)buf; (void)cb;
    if (flags || !name) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (written) *written = 0;
    lsiw_set_last_error(ERROR_SXS_KEY_NOT_FOUND);
    return FALSE;
}

API BOOL FindActCtxSectionStringW(DWORD flags, const void *ext, ULONG section, const WCHAR *str, void *data)
{
    (void)flags; (void)section;
    if (ext || !str || !data) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    lsiw_set_last_error(ERROR_SXS_KEY_NOT_FOUND);
    return FALSE;
}

API BOOL FindActCtxSectionStringA(DWORD flags, const void *ext, ULONG section, const char *str, void *data)
{
    (void)flags; (void)section;
    if (ext || !str || !data) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    lsiw_set_last_error(ERROR_SXS_KEY_NOT_FOUND);
    return FALSE;
}

API BOOL FindActCtxSectionGuid(DWORD flags, const void *ext, ULONG section, const void *guid, void *data)
{
    (void)flags; (void)section;
    if (ext || !guid || !data) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    lsiw_set_last_error(ERROR_SXS_KEY_NOT_FOUND);
    return FALSE;
}

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_k32extra_exports[] = {
    E(QueueUserAPC), E(QueueUserAPC2), E(CreateIoCompletionPort), E(PostQueuedCompletionStatus),
    E(GetQueuedCompletionStatus), E(GetQueuedCompletionStatusEx), E(SetFileCompletionNotificationModes),
    E(SetFileIoOverlappedRange), E(CreateThreadpool), E(CloseThreadpool), E(SetThreadpoolThreadMaximum),
    E(SetThreadpoolThreadMinimum), E(SetThreadpoolStackInformation), E(QueryThreadpoolStackInformation),
    E(CreateThreadpoolWork), E(SubmitThreadpoolWork), E(WaitForThreadpoolWorkCallbacks),
    E(CloseThreadpoolWork), E(TrySubmitThreadpoolCallback), E(CreateThreadpoolTimer), E(SetThreadpoolTimerEx),
    E(SetThreadpoolTimer), E(IsThreadpoolTimerSet), E(WaitForThreadpoolTimerCallbacks),
    E(CloseThreadpoolTimer), E(CreateThreadpoolWait), E(SetThreadpoolWaitEx), E(SetThreadpoolWait),
    E(WaitForThreadpoolWaitCallbacks), E(CloseThreadpoolWait), E(CreateThreadpoolIo), E(StartThreadpoolIo),
    E(CancelThreadpoolIo), E(CloseThreadpoolIo), E(WaitForThreadpoolIoCallbacks), E(BindIoCompletionCallback),
    E(CreateThreadpoolCleanupGroup), E(CloseThreadpoolCleanupGroupMembers), E(CloseThreadpoolCleanupGroup),
    E(CallbackMayRunLong), E(DisassociateCurrentThreadFromCallback), E(FreeLibraryWhenCallbackReturns),
    E(LeaveCriticalSectionWhenCallbackReturns), E(ReleaseMutexWhenCallbackReturns),
    E(SetEventWhenCallbackReturns), E(ReleaseSemaphoreWhenCallbackReturns), E(QueueUserWorkItem),
    E(RegisterWaitForSingleObject), E(RegisterWaitForSingleObjectEx), E(UnregisterWaitEx), E(UnregisterWait),
    E(CreateTimerQueue), E(CreateTimerQueueTimer), E(ChangeTimerQueueTimer), E(DeleteTimerQueueTimer),
    E(DeleteTimerQueueEx), E(DeleteTimerQueue), E(ReadFileEx), E(WriteFileEx), E(CancelSynchronousIo),
    E(CreateNamedPipeW), E(CreateNamedPipeA), E(ConnectNamedPipe), E(DisconnectNamedPipe), E(WaitNamedPipeW),
    E(WaitNamedPipeA), E(TransactNamedPipe), E(CallNamedPipeW), E(CallNamedPipeA), E(PeekNamedPipe),
    E(GetNamedPipeHandleStateW), E(GetNamedPipeHandleStateA), E(SetNamedPipeHandleState), E(GetNamedPipeInfo),
    E(GetNamedPipeClientProcessId), E(GetNamedPipeServerProcessId), E(GetNamedPipeClientSessionId),
    E(GetNamedPipeServerSessionId), E(ReadDirectoryChangesW), E(ReadDirectoryChangesExW), E(FindFirstStreamW),
    E(FindNextStreamW), E(FindFirstStreamTransactedW), E(GetFileAttributesTransactedW),
    E(GetFileAttributesTransactedA), E(GetLongPathNameTransactedW), E(GetLongPathNameTransactedA),
    E(GetFileInformationByName), E(GetCompressedFileSizeW), E(GetCompressedFileSizeA), E(GetBinaryTypeW),
    E(GetBinaryTypeA), E(CheckNameLegalDOS8Dot3W), E(CheckNameLegalDOS8Dot3A), E(FindFirstVolumeW),
    E(FindFirstVolumeA), E(FindNextVolumeW), E(FindNextVolumeA), E(FindVolumeClose),
    E(FindFirstVolumeMountPointW), E(GetVolumePathNamesForVolumeNameW), E(GetVolumePathNamesForVolumeNameA),
    E(QueryDosDeviceA), E(OpenThread), E(QueryFullProcessImageNameW), E(QueryFullProcessImageNameA),
    E(GetProcessHandleCount), E(GetProcessIoCounters), E(GetProcessInformation), E(SetProcessInformation),
    E(GetThreadInformation), E(SetThreadInformation), E(GetProcessMitigationPolicy),
    E(SetProcessMitigationPolicy), E(GetProcessDEPPolicy), E(SetProcessDEPPolicy), E(GetSystemDEPPolicy),
    E(IsProcessCritical), E(ProcessIdToSessionId), E(SetProcessShutdownParameters),
    E(GetProcessShutdownParameters), E(GetProcessVersion), E(GetThreadIOPendingFlag),
    E(QueryProcessAffinityUpdateMode), E(SetProcessAffinityUpdateMode), E(GetCurrentThreadStackLimits),
    E(SetThreadStackGuarantee), E(GetLogicalProcessorInformationEx), E(GetSystemCpuSetInformation),
    E(GetThreadGroupAffinity), E(SetThreadGroupAffinity), E(GetProcessGroupAffinity),
    E(GetCurrentProcessorNumberEx), E(GetMaximumProcessorGroupCount), E(GetThreadIdealProcessorEx),
    E(SetThreadIdealProcessorEx), E(SetThreadSelectedCpuSets), E(SetProcessDefaultCpuSets),
    E(GetThreadSelectedCpuSets), E(GetProcessDefaultCpuSets), E(GetNumaHighestNodeNumber),
    E(GetNumaNodeProcessorMaskEx), E(GetNumaNodeProcessorMask2), E(GetNumaNodeProcessorMask),
    E(GetNumaProcessorNode), E(GetNumaProcessorNodeEx), E(GetNumaAvailableMemoryNode),
    E(GetNumaAvailableMemoryNodeEx), E(GetNumaNodeNumberFromHandle), E(GetNumaProximityNodeEx),
    E(GetNumaProximityNode), E(GetEnabledXStateFeatures), E(InitializeContext2), E(InitializeContext),
    E(LocateXStateFeature), E(GetXStateFeaturesMask), E(SetXStateFeaturesMask), E(CopyContext),
    E(GetThreadContext), E(SetThreadContext), E(Wow64GetThreadContext), E(Wow64SetThreadContext),
    E(EnumSystemFirmwareTables), E(GetSystemFirmwareTable), E(GetFirmwareType),
    E(GetFirmwareEnvironmentVariableW), E(GetFirmwareEnvironmentVariableA),
    E(SetFirmwareEnvironmentVariableW), E(SetFirmwareEnvironmentVariableA), E(GetSystemPowerStatus),
    E(SetThreadExecutionState), E(PowerCreateRequest), E(PowerSetRequest), E(PowerClearRequest),
    E(CreateMemoryResourceNotification), E(QueryMemoryResourceNotification), E(GetCurrentPackageId),
    E(GetCurrentPackageFullName), E(GetCurrentPackageFamilyName), E(GetCurrentPackagePath),
    E(GetCurrentPackageInfo), E(GetCurrentApplicationUserModelId), E(GetPackageFullName),
    E(GetPackageFamilyName), E(GetPackageId), E(GetApplicationUserModelId), E(GetPackagePathByFullName),
    E(GetPackagesByPackageFamily), E(LoadPackagedLibrary), E(AppPolicyGetProcessTerminationMethod),
    E(AppPolicyGetThreadInitializationType), E(AppPolicyGetShowDeveloperDiagnostic),
    E(AppPolicyGetWindowingModel), E(AppPolicyGetMediaFoundationCodecLoading), E(AppPolicyGetClrCompat),
    E(AppPolicyGetCreateFileAccess), E(AppPolicyGetLifecycleManagement), E(WerRegisterMemoryBlock),
    E(WerUnregisterMemoryBlock), E(WerRegisterExcludedMemoryBlock), E(WerUnregisterExcludedMemoryBlock),
    E(WerRegisterFile), E(WerUnregisterFile), E(WerRegisterRuntimeExceptionModule),
    E(WerUnregisterRuntimeExceptionModule), E(WerRegisterCustomMetadata), E(WerUnregisterCustomMetadata),
    E(WerRegisterAppLocalDump), E(WerUnregisterAppLocalDump), E(RegisterApplicationRecoveryCallback),
    E(UnregisterApplicationRecoveryCallback), E(GetApplicationRecoveryCallback),
    E(ApplicationRecoveryInProgress), E(ApplicationRecoveryFinished), E(RegisterApplicationRestart),
    E(UnregisterApplicationRestart), E(GetApplicationRestartSettings), E(RaiseFailFastException), E(MulDiv),
    E(Wow64DisableWow64FsRedirection), E(Wow64RevertWow64FsRedirection), E(Wow64EnableWow64FsRedirection),
    E(SetSearchPathMode), E(NeedCurrentDirectoryForExePathW), E(NeedCurrentDirectoryForExePathA),
    E(AreFileApisANSI), E(SetFileApisToANSI), E(SetFileApisToOEM), E(GetDllDirectoryW), E(GetDllDirectoryA),
    E(GetSystemTimeAdjustment), E(GetSystemTimeAdjustmentPrecise), E(SetSystemTimeAdjustment),
    E(SetSystemTimeAdjustmentPrecise), E(GetMachineTypeAttributes), E(IsWow64GuestMachineSupported),
    E(GetOsSafeBootMode), E(GetOsManufacturingMode), E(SetEnvironmentStringsW), E(SetEnvironmentStringsA),
    E(GetConsoleSelectionInfo), E(GetCurrentConsoleFont), E(GetConsoleFontSize),
    E(GetNumberOfConsoleMouseButtons), E(GetUserGeoID), E(SetUserGeoID), E(GetUserDefaultGeoName),
    E(SetUserGeoName), E(GetThreadUILanguage), E(SetThreadUILanguage), E(GetSystemPreferredUILanguages),
    E(SetProcessPreferredUILanguages), E(GetFileMUIPath), E(GetFileMUIInfo), E(GetNumberFormatEx),
    E(GetNumberFormatW), E(GetCurrencyFormatEx), E(GetCurrencyFormatW), E(GetNumberFormatA),
    E(GetCurrencyFormatA), E(GetCalendarInfoEx), E(GetCalendarInfoW), E(GetCalendarInfoA), E(GetNLSVersionEx),
    E(GetNLSVersion), E(IsValidNLSVersion), E(IsNLSDefinedString), E(ResolveLocaleName), E(FindStringOrdinal),
    E(FindNLSStringEx), E(FindNLSString), E(GetStringScripts), E(VerifyScripts), E(NormalizeString),
    E(IsNormalizedString), E(AddAtomW), E(AddAtomA), E(FindAtomW), E(FindAtomA), E(DeleteAtom),
    E(GetAtomNameW), E(GetAtomNameA), E(InitAtomTable), E(GlobalAddAtomW), E(GlobalAddAtomA),
    E(GlobalAddAtomExW), E(GlobalAddAtomExA), E(GlobalFindAtomW), E(GlobalFindAtomA), E(GlobalDeleteAtom),
    E(GlobalGetAtomNameW), E(GlobalGetAtomNameA), E(GetPrivateProfileStringW), E(WritePrivateProfileStringW),
    E(GetPrivateProfileIntW), E(GetPrivateProfileSectionW), E(WritePrivateProfileSectionW),
    E(GetPrivateProfileSectionNamesW), E(GetPrivateProfileStructW), E(WritePrivateProfileStructW),
    E(GetPrivateProfileStringA), E(WritePrivateProfileStringA), E(GetPrivateProfileIntA),
    E(GetPrivateProfileSectionA), E(WritePrivateProfileSectionA), E(GetPrivateProfileSectionNamesA),
    E(GetPrivateProfileStructA), E(WritePrivateProfileStructA), E(GetProfileStringW), E(GetProfileStringA),
    E(WriteProfileStringW), E(WriteProfileStringA), E(GetProfileIntW), E(GetProfileIntA),
    E(GetProfileSectionW), E(GetProfileSectionA), E(WriteProfileSectionW), E(WriteProfileSectionA),
    E(CreateActCtxW), E(CreateActCtxA), E(AddRefActCtx), E(ReleaseActCtx), E(ZombifyActCtx),
    E(ActivateActCtx), E(DeactivateActCtx), E(GetCurrentActCtx), E(QueryActCtxW), E(QueryActCtxSettingsW),
    E(FindActCtxSectionStringW), E(FindActCtxSectionStringA), E(FindActCtxSectionGuid),
    { NULL, NULL }
};
