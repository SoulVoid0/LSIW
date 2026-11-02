/* ntdll: the NT native API layer.
 *
 * Many msvcrt/ucrt and kernel32 paths bottom out in Nt and Rtl calls, and some
 * programs call them directly. These are thin wrappers over the kernel32/
 * memory implementations plus the Rtl string and helper routines. Heap,
 * SList and the SEH functions live in k32_mem.c and seh.c; they are re-
 * exported from ntdll through the builtin table list in builtins.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <sys/random.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define STATUS_SUCCESS            0x00000000
#define STATUS_UNSUCCESSFUL       0xC0000001
#define STATUS_NOT_IMPLEMENTED    0xC0000002
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004
#define STATUS_INVALID_PARAMETER  0xC000000D
#define STATUS_NO_MEMORY          0xC0000017
#define STATUS_BUFFER_TOO_SMALL   0xC0000023
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034
#define STATUS_NOT_FOUND          0xC0000225
#define STATUS_NO_MORE_ENTRIES    0x8000001A

/* ---- UNICODE_STRING / ANSI_STRING ---- */
typedef struct { WORD Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { WORD Length, MaximumLength; char *Buffer; } STRING;

API void RtlInitUnicodeString(UNICODE_STRING *d, const WCHAR *s)
{
    d->Buffer = (WCHAR *)s;
    d->Length = s ? (WORD)(lsiw_wcslen(s) * 2) : 0;
    d->MaximumLength = s ? (WORD)(d->Length + 2) : 0;
}
API NTSTATUS RtlInitUnicodeStringEx(UNICODE_STRING *d, const WCHAR *s) { RtlInitUnicodeString(d, s); return 0; }
API void RtlInitString(STRING *d, const char *s)
{
    d->Buffer = (char *)s;
    d->Length = s ? (WORD)strlen(s) : 0;
    d->MaximumLength = s ? (WORD)(d->Length + 1) : 0;
}
API void RtlInitAnsiString(STRING *d, const char *s) { RtlInitString(d, s); }

API NTSTATUS RtlAnsiStringToUnicodeString(UNICODE_STRING *d, const STRING *s, BOOL alloc)
{
    int need = lsiw_utf8_to_utf16(s->Buffer, s->Length, NULL, 0);
    if (alloc) { d->Buffer = malloc((need + 1) * 2); d->MaximumLength = (WORD)((need + 1) * 2); }
    else if (d->MaximumLength < (need + 1) * 2) return STATUS_BUFFER_TOO_SMALL;
    int n = lsiw_utf8_to_utf16(s->Buffer, s->Length, d->Buffer, need + 1);
    d->Buffer[n] = 0;
    d->Length = (WORD)(n * 2);
    return STATUS_SUCCESS;
}
API NTSTATUS RtlUnicodeStringToAnsiString(STRING *d, const UNICODE_STRING *s, BOOL alloc)
{
    int need = lsiw_utf16_to_utf8(s->Buffer, s->Length / 2, NULL, 0);
    if (alloc) { d->Buffer = malloc(need + 1); d->MaximumLength = (WORD)(need + 1); }
    else if (d->MaximumLength < need + 1) return STATUS_BUFFER_TOO_SMALL;
    int n = lsiw_utf16_to_utf8(s->Buffer, s->Length / 2, d->Buffer, need + 1);
    d->Buffer[n] = 0;
    d->Length = (WORD)n;
    return STATUS_SUCCESS;
}
API void RtlFreeUnicodeString(UNICODE_STRING *s) { if (s) { if (lsiw_heap_owns(s->Buffer)) lsiw_heap_free(s->Buffer); else free(s->Buffer); s->Buffer = NULL; s->Length = s->MaximumLength = 0; } }
API void RtlFreeAnsiString(STRING *s) { if (s) { free(s->Buffer); s->Buffer = NULL; s->Length = s->MaximumLength = 0; } }
API NTSTATUS RtlMultiByteToUnicodeN(WCHAR *dst, ULONG dstlen, ULONG *written, const char *src, ULONG srclen)
{
    int n = lsiw_utf8_to_utf16(src, (int)srclen, dst, (int)(dstlen / 2));
    if (n < 0) n = (int)(dstlen / 2);
    if (written) *written = (ULONG)n * 2;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlUnicodeToMultiByteN(char *dst, ULONG dstlen, ULONG *written, const WCHAR *src, ULONG srclen)
{
    int n = lsiw_utf16_to_utf8(src, (int)(srclen / 2), dst, (int)dstlen);
    if (n < 0) n = (int)dstlen;
    if (written) *written = (ULONG)n;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlMultiByteToUnicodeSize(ULONG *size, const char *src, ULONG srclen)
{ *size = (ULONG)lsiw_utf8_to_utf16(src, (int)srclen, NULL, 0) * 2; return 0; }
API NTSTATUS RtlUnicodeToMultiByteSize(ULONG *size, const WCHAR *src, ULONG srclen)
{ *size = (ULONG)lsiw_utf16_to_utf8(src, (int)(srclen / 2), NULL, 0); return 0; }
API LONG RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOL ci)
{
    size_t n = (a->Length < b->Length ? a->Length : b->Length) / 2;
    for (size_t i = 0; i < n; i++) {
        WCHAR x = a->Buffer[i], y = b->Buffer[i];
        if (ci) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x != y) return x - y;
    }
    return (LONG)a->Length - (LONG)b->Length;
}
API BOOL RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOL ci)
{ return a->Length == b->Length && RtlCompareUnicodeString(a, b, ci) == 0; }
API void RtlCopyUnicodeString(UNICODE_STRING *d, const UNICODE_STRING *s)
{
    WORD n = s->Length < d->MaximumLength ? s->Length : d->MaximumLength;
    if (s->Buffer && d->Buffer) memcpy(d->Buffer, s->Buffer, n);
    d->Length = n;
    if (n + 2 <= d->MaximumLength) d->Buffer[n / 2] = 0;
}

/* ---- status <-> errno ---- */
#define STATUS_PENDING_                 0x00000103u
#define STATUS_END_OF_FILE_             0xC0000011u
#define STATUS_INVALID_HANDLE_          0xC0000008u
#define STATUS_ACCESS_DENIED_           0xC0000022u
#define STATUS_OBJECT_NAME_INVALID_     0xC0000033u
#define STATUS_OBJECT_NAME_COLLISION_   0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND_   0xC000003Au
#define STATUS_SHARING_VIOLATION_       0xC0000043u
#define STATUS_DELETE_PENDING_          0xC0000056u
#define STATUS_DISK_FULL_               0xC000007Fu
#define STATUS_MEDIA_WRITE_PROTECTED_   0xC00000A2u
#define STATUS_FILE_IS_A_DIRECTORY_     0xC00000BAu
#define STATUS_NOT_SUPPORTED_           0xC00000BBu
#define STATUS_PIPE_CLOSING_            0xC00000B1u
#define STATUS_NOT_SAME_DEVICE_         0xC00000D4u
#define STATUS_DIRECTORY_NOT_EMPTY_     0xC0000101u
#define STATUS_NOT_A_DIRECTORY_         0xC0000103u
#define STATUS_NAME_TOO_LONG_           0xC0000106u
#define STATUS_TOO_MANY_OPENED_FILES_   0xC000011Fu
#define STATUS_CANNOT_DELETE_           0xC0000121u
#define STATUS_PIPE_BROKEN_             0xC000014Bu
#define STATUS_INVALID_DEVICE_REQUEST_  0xC0000010u
#define STATUS_TIMEOUT_                 0x00000102u
#define STATUS_STOPPED_ON_SYMLINK_      0x8000002Du
#define STATUS_BUFFER_OVERFLOW_         0x80000005u
#define STATUS_NO_MORE_FILES_           0x80000006u
#define STATUS_UNSUCCESSFUL_            0xC0000001u
#define STATUS_IO_DEVICE_ERROR_         0xC0000185u
#define STATUS_NOT_SAME_OBJECT_         0xC00001ACu

uint32_t lsiw_errno_to_ntstatus(int err)
{
    switch (err) {
    case 0:            return STATUS_SUCCESS;
    case ENOENT:       return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR:      return STATUS_OBJECT_PATH_NOT_FOUND_;
    case EACCES: case EPERM: return STATUS_ACCESS_DENIED_;
    case EEXIST:       return STATUS_OBJECT_NAME_COLLISION_;
    case EISDIR:       return STATUS_FILE_IS_A_DIRECTORY_;
    case ENOTEMPTY:    return STATUS_DIRECTORY_NOT_EMPTY_;
    case ENOSPC: case EDQUOT: return STATUS_DISK_FULL_;
    case EBADF:        return STATUS_INVALID_HANDLE_;
    case EINVAL:       return STATUS_INVALID_PARAMETER;
    case EPIPE:        return STATUS_PIPE_CLOSING_;
    case ELOOP:        return STATUS_STOPPED_ON_SYMLINK_;
    case ENAMETOOLONG: return STATUS_NAME_TOO_LONG_;
    case EROFS:        return STATUS_MEDIA_WRITE_PROTECTED_;
    case EMFILE: case ENFILE: return STATUS_TOO_MANY_OPENED_FILES_;
    case ENOMEM:       return STATUS_NO_MEMORY;
    case EBUSY: case ETXTBSY: return STATUS_SHARING_VIOLATION_;
    case EXDEV:        return STATUS_NOT_SAME_DEVICE_;
    case ENOSYS: case EOPNOTSUPP: return STATUS_NOT_SUPPORTED_;
    case ENOTTY: case ENODEV: case ENXIO: return STATUS_INVALID_DEVICE_REQUEST_;
    case EIO:          return STATUS_IO_DEVICE_ERROR_;
    case ETIMEDOUT:    return STATUS_TIMEOUT_;
    default:           return STATUS_UNSUCCESSFUL_;
    }
}

API ULONG RtlNtStatusToDosError(NTSTATUS st)
{
    switch ((unsigned)st) {
    case STATUS_SUCCESS:              return 0;
    case STATUS_PENDING_:             return 997;   /* ERROR_IO_PENDING */
    case STATUS_TIMEOUT_:             return 1460;  /* ERROR_TIMEOUT */
    case STATUS_NO_MEMORY:            return ERROR_NOT_ENOUGH_MEMORY;
    case STATUS_INVALID_PARAMETER:    return ERROR_INVALID_PARAMETER;
    case STATUS_OBJECT_NAME_NOT_FOUND:return ERROR_FILE_NOT_FOUND;
    case STATUS_OBJECT_PATH_NOT_FOUND_: return ERROR_PATH_NOT_FOUND;
    case STATUS_OBJECT_NAME_INVALID_: return 123;   /* ERROR_INVALID_NAME */
    case STATUS_OBJECT_NAME_COLLISION_: return ERROR_ALREADY_EXISTS;
    case STATUS_BUFFER_TOO_SMALL:     return ERROR_INSUFFICIENT_BUFFER;
    case STATUS_INFO_LENGTH_MISMATCH: return 24;    /* ERROR_BAD_LENGTH */
    case STATUS_BUFFER_OVERFLOW_:     return ERROR_MORE_DATA;
    case STATUS_NOT_IMPLEMENTED:      return ERROR_CALL_NOT_IMPLEMENTED;
    case STATUS_NO_MORE_ENTRIES:      return ERROR_NO_MORE_ITEMS;
    case STATUS_NO_MORE_FILES_:       return ERROR_NO_MORE_FILES;
    case STATUS_INVALID_HANDLE_:      return ERROR_INVALID_HANDLE;
    case STATUS_ACCESS_DENIED_:       return ERROR_ACCESS_DENIED;
    case STATUS_FILE_IS_A_DIRECTORY_: return ERROR_ACCESS_DENIED;
    case STATUS_CANNOT_DELETE_:       return ERROR_ACCESS_DENIED;
    case STATUS_DELETE_PENDING_:      return ERROR_ACCESS_DENIED;
    case STATUS_SHARING_VIOLATION_:   return 32;    /* ERROR_SHARING_VIOLATION */
    case STATUS_END_OF_FILE_:         return ERROR_HANDLE_EOF;
    case STATUS_PIPE_BROKEN_:         return 109;   /* ERROR_BROKEN_PIPE */
    case STATUS_PIPE_CLOSING_:        return 232;   /* ERROR_NO_DATA */
    case STATUS_DISK_FULL_:           return 112;   /* ERROR_DISK_FULL */
    case STATUS_MEDIA_WRITE_PROTECTED_: return 19;  /* ERROR_WRITE_PROTECT */
    case STATUS_NOT_SUPPORTED_:       return ERROR_NOT_SUPPORTED;
    case STATUS_NOT_SAME_DEVICE_:     return 17;    /* ERROR_NOT_SAME_DEVICE */
    case STATUS_DIRECTORY_NOT_EMPTY_: return ERROR_DIR_NOT_EMPTY;
    case STATUS_NOT_A_DIRECTORY_:     return ERROR_DIRECTORY;
    case STATUS_NAME_TOO_LONG_:       return 206;   /* ERROR_FILENAME_EXCED_RANGE */
    case STATUS_TOO_MANY_OPENED_FILES_: return ERROR_TOO_MANY_OPEN_FILES;
    case STATUS_STOPPED_ON_SYMLINK_:  return 681;   /* ERROR_STOPPED_ON_SYMLINK */
    case STATUS_INVALID_DEVICE_REQUEST_: return ERROR_INVALID_FUNCTION;
    case STATUS_IO_DEVICE_ERROR_:     return 1117;  /* ERROR_IO_DEVICE */
    case STATUS_UNSUCCESSFUL_:        return 31;    /* ERROR_GEN_FAILURE */
    case STATUS_NOT_FOUND:            return 1168;  /* ERROR_NOT_FOUND */
    default:
        /* Win32 errors wrapped as NTSTATUS by FACILITY_WIN32 (0xC007xxxx). */
        if (((unsigned)st & 0xFFFF0000u) == 0xC0070000u) return (unsigned)st & 0xFFFF;
        return ((unsigned)st & 0xC0000000u) == 0xC0000000u ? 317 /* ERROR_MR_MID_NOT_FOUND */ : 0;
    }
}
API ULONG RtlNtStatusToDosErrorNoTeb(NTSTATUS st) { return RtlNtStatusToDosError(st); }
API NTSTATUS RtlGetLastNtStatus(void) { return 0; }
API ULONG RtlGetLastWin32Error(void) { return lsiw_get_last_error(); }
API void RtlSetLastWin32Error(ULONG e) { lsiw_set_last_error(e); }
API void RtlSetLastWin32ErrorAndNtStatusFromNtStatus(NTSTATUS st) { lsiw_set_last_error(RtlNtStatusToDosError(st)); }
API void RtlRestoreLastWin32Error(ULONG e) { lsiw_set_last_error(e); }

/* ---- TEB/PEB accessors ---- */
API void *NtCurrentTeb(void) { return lsiw_teb_current(); }
API void *RtlGetCurrentPeb(void) { return lsiw_peb(); }
API BOOL RtlGetVersion(void *info)
{
    DWORD *v = info;
    v[0] = 284;        /* dwOSVersionInfoSize (RTL_OSVERSIONINFOEXW) */
    v[1] = 10; v[2] = 0; v[3] = 19045; v[4] = 2;
    memset(v + 5, 0, 128 * 2);   /* szCSDVersion empty */
    return 0;
}
API void RtlGetNtVersionNumbers(DWORD *major, DWORD *minor, DWORD *build)
{ if (major) *major = 10; if (minor) *minor = 0; if (build) *build = 19045 | 0xF0000000; }
API BOOL RtlIsProcessorFeaturePresent(DWORD f) { (void)f; return TRUE; }

/* ---- Rtl helpers ---- */
API DWORD RtlComputeCrc32(DWORD crc, const void *buf, int len)
{
    crc = ~crc;
    const uint8_t *p = buf;
    for (int i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return ~crc;
}
API void RtlAssert(void *expr, void *file, ULONG line, char *msg)
{ fprintf(stderr, "lsiw: RtlAssert at line %u: %s\n", line, msg ? msg : ""); (void)expr; (void)file; }

static uint32_t g_seed;
API ULONG RtlRandomEx(ULONG *seed)
{
    ULONG s = seed ? *seed : (g_seed ^ (ULONG)time(NULL));
    s = s * 1103515245u + 12345u;
    ULONG r = (s >> 16) & 0x7fff;
    s = s * 1103515245u + 12345u;
    r = (r << 15) | ((s >> 16) & 0x7fff);
    if (seed) *seed = s; else g_seed = s;
    return r & 0x7fffffff;
}
API ULONG RtlRandom(ULONG *seed) { return RtlRandomEx(seed); }
API ULONG RtlUniform(ULONG *seed) { return RtlRandomEx(seed); }

API NTSTATUS RtlGenRandom(void *buf, ULONG len)
{
    if (getrandom(buf, len, 0) == (ssize_t)len) return 0;
    uint8_t *b = buf;
    for (ULONG i = 0; i < len; i++) b[i] = (uint8_t)RtlRandomEx(NULL);
    return 0;
}
API BOOL SystemFunction036(void *buf, ULONG len) { RtlGenRandom(buf, len); return TRUE; }   /* RtlGenRandom's export name */

/* ---- Nt virtual memory (forwarded to the kernel32 VM layer) ---- */

API NTSTATUS NtAllocateVirtualMemory(HANDLE proc, void **base, ULONG_PTR zero, SIZE_T *size, ULONG type, ULONG protect)
{
    (void)proc; (void)zero;
    void *(WINAPI *va)(void *, SIZE_T, DWORD, DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "VirtualAlloc");
    void *p = va(*base, *size, type, protect);
    if (!p) return STATUS_NO_MEMORY;
    *base = p;
    return STATUS_SUCCESS;
}
API NTSTATUS NtFreeVirtualMemory(HANDLE proc, void **base, SIZE_T *size, ULONG type)
{
    (void)proc;
    BOOL (WINAPI *vf)(void *, SIZE_T, DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "VirtualFree");
    return vf(*base, type & 0x8000 ? 0 : *size, type) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
API NTSTATUS NtProtectVirtualMemory(HANDLE proc, void **base, SIZE_T *size, ULONG protect, ULONG *old)
{
    (void)proc;
    BOOL (WINAPI *vp)(void *, SIZE_T, DWORD, DWORD *) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "VirtualProtect");
    return vp(*base, *size, protect, old) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
API NTSTATUS NtQueryVirtualMemory(HANDLE proc, void *addr, int cls, void *info, SIZE_T len, SIZE_T *ret)
{
    (void)proc; (void)cls;
    SIZE_T (WINAPI *vq)(const void *, void *, SIZE_T) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "VirtualQuery");
    SIZE_T r = vq(addr, info, len);
    if (ret) *ret = r;
    return r ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
API NTSTATUS NtReadVirtualMemory(HANDLE p, const void *src, void *dst, SIZE_T n, SIZE_T *done)
{ (void)p; memcpy(dst, src, n); if (done) *done = n; return 0; }
API NTSTATUS NtWriteVirtualMemory(HANDLE p, void *dst, const void *src, SIZE_T n, SIZE_T *done)
{ (void)p; memcpy(dst, src, n); if (done) *done = n; return 0; }

API NTSTATUS NtClose(HANDLE h) { return lsiw_handle_close(h) ? STATUS_SUCCESS : 0xC0000008; }

/* ---- timing ---- */
API NTSTATUS NtQueryPerformanceCounter(int64_t *counter, int64_t *freq)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (counter) *counter = (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100;
    if (freq) *freq = 10000000;
    return STATUS_SUCCESS;
}
API NTSTATUS NtDelayExecution(BOOL alertable, int64_t *interval)
{
    (void)alertable;
    if (!interval) return 0;
    int64_t v = *interval;
    struct timespec ts;
    if (v < 0) { v = -v; ts.tv_sec = v / 10000000; ts.tv_nsec = (v % 10000000) * 100; }
    else return 0;      /* absolute times are rare here; treat as no-op */
    nanosleep(&ts, NULL);
    return STATUS_SUCCESS;
}
API NTSTATUS NtYieldExecution(void) { sched_yield(); return 0; }

API NTSTATUS NtQuerySystemTime(int64_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *t = (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100 + 116444736000000000LL;
    return 0;
}
API NTSTATUS RtlQueryPerformanceCounter(int64_t *c) { return NtQueryPerformanceCounter(c, NULL); }
API NTSTATUS RtlQueryPerformanceFrequency(int64_t *f) { int64_t c; return NtQueryPerformanceCounter(&c, f); }

/* ---- system / process information (minimal) ---- */
API NTSTATUS NtQuerySystemInformation(int cls, void *info, ULONG len, ULONG *ret)
{
    switch (cls) {
    case 0: /* SystemBasicInformation */
        if (len < 44) { if (ret) *ret = 44; return STATUS_INFO_LENGTH_MISMATCH; }
        memset(info, 0, 44);
        ((DWORD *)info)[2] = 4096;                    /* page size */
        ((DWORD *)info)[7] = 0x10000;                 /* allocation granularity */
        ((BYTE *)info)[40] = (BYTE)sysconf(_SC_NPROCESSORS_ONLN);  /* NumberOfProcessors */
        if (ret) *ret = 44;
        return STATUS_SUCCESS;
    case 0x25: /* SystemRecommendedSharedDataAlignment */
        if (len >= 4) *(DWORD *)info = 64;
        if (ret) *ret = 4;
        return STATUS_SUCCESS;
    default:
        if (ret) *ret = 0;
        return STATUS_NOT_IMPLEMENTED;
    }
}
API NTSTATUS NtQueryInformationProcess(HANDLE p, int cls, void *info, ULONG len, ULONG *ret)
{
    (void)p;
    memset(info, 0, len);
    if (ret) *ret = len;
    if (cls == 0 && len >= 48) {   /* ProcessBasicInformation */
        ((void **)info)[1] = lsiw_peb();              /* PebBaseAddress */
        ((ULONG_PTR *)info)[4] = (ULONG_PTR)getpid();
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}
API NTSTATUS NtQueryInformationThread(HANDLE t, int cls, void *info, ULONG len, ULONG *ret)
{ (void)t; (void)cls; memset(info, 0, len); if (ret) *ret = len; return STATUS_SUCCESS; }
API NTSTATUS NtSetInformationThread(HANDLE t, int cls, void *info, ULONG len)
{ (void)t; (void)cls; (void)info; (void)len; return STATUS_SUCCESS; }
API NTSTATUS NtSetInformationProcess(HANDLE p, int cls, void *info, ULONG len)
{ (void)p; (void)cls; (void)info; (void)len; return STATUS_SUCCESS; }

/* ---- loader (Ldr*) ---- */
API NTSTATUS LdrGetDllHandle(const WCHAR *path, void *flags, UNICODE_STRING *name, HMODULE *out)
{
    (void)path; (void)flags;
    char *n = lsiw_w2u_n(name->Buffer, name->Length / 2);
    struct lsiw_module *m = lsiw_find_module(n);
    free(n);
    if (!m) return STATUS_OBJECT_NAME_NOT_FOUND;
    *out = (HMODULE)m->base;
    return STATUS_SUCCESS;
}
API NTSTATUS LdrGetProcedureAddress(HMODULE h, const STRING *name, ULONG ord, void **out)
{
    struct lsiw_module *m = lsiw_module_from_handle(h);
    if (!m) { const struct lsiw_builtin_dll *d = lsiw_builtin_from_handle(h); if (d && name) { *out = lsiw_builtin_lookup(d, name->Buffer); return *out ? 0 : STATUS_NOT_FOUND; } return 0xC0000008; }
    *out = lsiw_get_export(m, name ? name->Buffer : NULL, (int)ord);
    return *out ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}
API NTSTATUS LdrLoadDll(const WCHAR *path, void *flags, UNICODE_STRING *name, HMODULE *out)
{
    (void)path; (void)flags;
    char *n = lsiw_w2u_n(name->Buffer, name->Length / 2);
    struct lsiw_module *m = lsiw_load_library(n, NULL);
    free(n);
    if (!m) return STATUS_OBJECT_NAME_NOT_FOUND;
    *out = (HMODULE)m->base;
    return STATUS_SUCCESS;
}
API NTSTATUS LdrUnloadDll(HMODULE h) { (void)h; return STATUS_SUCCESS; }
API NTSTATUS LdrFindResource_U(HMODULE h, void *info, ULONG level, void **entry)
{ (void)h; (void)info; (void)level; (void)entry; return STATUS_NOT_FOUND; }
API BOOL RtlDllShutdownInProgress(void) { return FALSE; }
API NTSTATUS LdrDisableThreadCalloutsForDll(HMODULE h) { (void)h; return 0; }
API void LdrInitializeThunk(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; }

/* ---- environment / process params ---- */
API NTSTATUS RtlExpandEnvironmentStrings_U(void *env, const UNICODE_STRING *src, UNICODE_STRING *dst, ULONG *ret)
{
    (void)env;
    DWORD (WINAPI *ex)(const WCHAR *, WCHAR *, DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "ExpandEnvironmentStringsW");
    WCHAR tmp[4096];
    DWORD n = ex(src->Buffer, tmp, 4096);
    if (ret) *ret = n * 2;
    if (dst->MaximumLength < n * 2) return STATUS_BUFFER_TOO_SMALL;
    memcpy(dst->Buffer, tmp, n * 2);
    dst->Length = (WORD)((n - 1) * 2);
    return STATUS_SUCCESS;
}

/* ---- exception helper used by the CRT ---- */
API void RtlRaiseStatus(NTSTATUS st)
{
    void (WINAPI *re)(DWORD, DWORD, DWORD, const ULONG_PTR *) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "RaiseException");
    re((DWORD)st, 1, 0, NULL);
}

/* ---- bitmaps (used by some CRTs/allocators) ---- */
typedef struct { ULONG SizeOfBitMap; ULONG *Buffer; } RTL_BITMAP;
API void RtlInitializeBitMap(RTL_BITMAP *b, ULONG *buf, ULONG size) { b->SizeOfBitMap = size; b->Buffer = buf; }
API void RtlSetBit(RTL_BITMAP *b, ULONG i) { b->Buffer[i / 32] |= 1u << (i % 32); }
API void RtlClearBit(RTL_BITMAP *b, ULONG i) { b->Buffer[i / 32] &= ~(1u << (i % 32)); }
API BOOL RtlTestBit(RTL_BITMAP *b, ULONG i) { return (b->Buffer[i / 32] >> (i % 32)) & 1; }
API void RtlClearAllBits(RTL_BITMAP *b) { memset(b->Buffer, 0, (b->SizeOfBitMap + 7) / 8); }
API void RtlSetAllBits(RTL_BITMAP *b) { memset(b->Buffer, 0xff, (b->SizeOfBitMap + 7) / 8); }
API ULONG RtlFindClearBitsAndSet(RTL_BITMAP *b, ULONG n, ULONG hint)
{
    for (ULONG i = hint; i + n <= b->SizeOfBitMap; i++) {
        ULONG k = 0;
        while (k < n && !((b->Buffer[(i + k) / 32] >> ((i + k) % 32)) & 1)) k++;
        if (k == n) { for (ULONG j = 0; j < n; j++) RtlSetBit(b, i + j); return i; }
    }
    return 0xFFFFFFFF;
}

/* ---- misc no-ops frequently imported ---- */
API NTSTATUS NtQueryObject(HANDLE h, int cls, void *info, ULONG len, ULONG *ret)
{ (void)h; (void)cls; if (info && len) memset(info, 0, len); if (ret) *ret = len; return STATUS_SUCCESS; }
API void RtlAcquirePebLock(void) {}
API void RtlReleasePebLock(void) {}
API NTSTATUS NtSetEvent(HANDLE h, LONG *prev)
{
    BOOL (WINAPI *se)(HANDLE) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "SetEvent");
    if (prev) *prev = 0;
    return se(h) ? 0 : STATUS_UNSUCCESSFUL;
}
API NTSTATUS NtWaitForSingleObject(HANDLE h, BOOL alert, int64_t *timeout)
{
    (void)alert;
    DWORD (WINAPI *w)(HANDLE, DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "WaitForSingleObject");
    DWORD ms = INFINITE;
    if (timeout) { int64_t v = *timeout; if (v < 0) ms = (DWORD)((-v) / 10000); else ms = 0; }
    DWORD r = w(h, ms);
    if (r == WAIT_TIMEOUT) return 0x102;
    if (r == WAIT_FAILED) return (NTSTATUS)STATUS_UNSUCCESSFUL;
    return (NTSTATUS)r;
}


/* ---- NT file I/O: NtCreateFile, NtOpenFile, NtReadFile, NtWriteFile ---- */
#ifndef CREATE_NEW
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5
#endif
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes;
                 void *SecurityDescriptor; void *SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { union { NTSTATUS Status; void *Pointer; }; ULONG_PTR Information; } IO_STATUS_BLOCK;

static NTSTATUS win32_to_ntstatus(DWORD e)
{
    switch (e) {
    case 0:                         return STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:      return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND:      return (NTSTATUS)STATUS_OBJECT_PATH_NOT_FOUND_;
    case ERROR_ACCESS_DENIED:       return (NTSTATUS)STATUS_ACCESS_DENIED_;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:      return (NTSTATUS)STATUS_OBJECT_NAME_COLLISION_;
    case 32:                        return (NTSTATUS)STATUS_SHARING_VIOLATION_;
    case ERROR_TOO_MANY_OPEN_FILES: return (NTSTATUS)STATUS_TOO_MANY_OPENED_FILES_;
    case ERROR_DIRECTORY:           return (NTSTATUS)STATUS_NOT_A_DIRECTORY_;
    case ERROR_INVALID_PARAMETER:   return STATUS_INVALID_PARAMETER;
    case 112:                       return (NTSTATUS)STATUS_DISK_FULL_;
    case 19:                        return (NTSTATUS)STATUS_MEDIA_WRITE_PROTECTED_;
    case 206:                       return (NTSTATUS)STATUS_NAME_TOO_LONG_;
    case ERROR_NOT_ENOUGH_MEMORY:   return STATUS_NO_MEMORY;
    default:                        return (NTSTATUS)(0xC0070000u | (e & 0xFFFF));
    }
}

/* NT object name (optionally relative to a directory handle) -> Linux path. */
static char *nt_name_to_linux(const OBJECT_ATTRIBUTES *oa, NTSTATUS *st)
{
    *st = STATUS_SUCCESS;
    char *name = (oa->ObjectName && oa->ObjectName->Buffer)
        ? lsiw_w2u_n(oa->ObjectName->Buffer, oa->ObjectName->Length / 2) : strdup("");
    if (oa->RootDirectory) {
        struct lsiw_handle_obj *root = lsiw_handle_get(oa->RootDirectory, H_FILE);
        if (!root || !root->path) { free(name); *st = (NTSTATUS)STATUS_INVALID_HANDLE_; return NULL; }
        if (name[0] == '\\') { free(name); *st = (NTSTATUS)STATUS_OBJECT_NAME_INVALID_; return NULL; }
        for (char *c = name; *c; c++) if (*c == '\\') *c = '/';
        char *p = *name ? lsiw_strdup_printf("%s/%s", root->path, name) : strdup(root->path);
        free(name);
        return p;
    }
    const char *w = name;
    if (!strncmp(w, "\\??\\", 4) || !strncmp(w, "\\\\?\\", 4)) w += 4;
    else if (!strncasecmp(w, "\\DosDevices\\", 12)) w += 12;
    else if (!strncasecmp(w, "\\GLOBAL??\\", 10)) w += 10;
    if (!strncasecmp(w, "UNC\\", 4) || !(w[0] && w[1] == ':')) {
        free(name);
        *st = (NTSTATUS)STATUS_OBJECT_PATH_NOT_FOUND_;
        return NULL;
    }
    char *p = lsiw_win_to_linux(w);
    free(name);
    return p;
}

API NTSTATUS NtCreateFile(HANDLE *out, DWORD access, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb,
                          int64_t *alloc, ULONG attrs, ULONG share, ULONG disp, ULONG options, void *ea, ULONG ealen)
{
    (void)alloc; (void)ea; (void)ealen;
    if (!out || !oa || !iosb) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    NTSTATUS st;
    char *path = nt_name_to_linux(oa, &st);
    if (!path) { iosb->Status = st; iosb->Information = 0; return st; }
    enum { SUPERSEDE, OPEN, CREATE, OPEN_IF, OVERWRITE, OVERWRITE_IF };
    enum { O_DIR = 0x1, O_NONDIR = 0x40, O_DELCLOSE = 0x1000, O_REPARSE = 0x200000 };
    struct stat sb;
    int exists = ((options & O_REPARSE) ? lstat(path, &sb) : stat(path, &sb)) == 0;
    ULONG_PTR info;
    if (exists && (disp == CREATE)) { free(path); st = (NTSTATUS)STATUS_OBJECT_NAME_COLLISION_; goto done_info4; }
    if (exists && (options & O_DIR) && !S_ISDIR(sb.st_mode) && !S_ISLNK(sb.st_mode)) { free(path); st = (NTSTATUS)STATUS_NOT_A_DIRECTORY_; goto done_info0; }
    if (exists && (options & O_NONDIR) && S_ISDIR(sb.st_mode)) { free(path); st = (NTSTATUS)STATUS_FILE_IS_A_DIRECTORY_; goto done_info0; }
    if (!exists && (options & O_DIR) && (disp == CREATE || disp == OPEN_IF)) {
        if (mkdir(path, 0777) < 0) { st = (NTSTATUS)lsiw_errno_to_ntstatus(errno); free(path); goto done_info0; }
        exists = 0;
        disp = OPEN;
        info = 2;  /* FILE_CREATED */
    } else {
        info = !exists ? 2 : (disp == SUPERSEDE) ? 0 : (disp == OVERWRITE || disp == OVERWRITE_IF) ? 3 : 1;
    }
    DWORD w32disp;
    switch (disp) {
    case SUPERSEDE: case OVERWRITE_IF: w32disp = CREATE_ALWAYS; break;
    case OPEN:      w32disp = OPEN_EXISTING; break;
    case CREATE:    w32disp = CREATE_NEW; break;
    case OPEN_IF:   w32disp = OPEN_ALWAYS; break;
    case OVERWRITE: w32disp = TRUNCATE_EXISTING; break;
    default: free(path); st = STATUS_INVALID_PARAMETER; goto done_info0;
    }
    if (S_ISDIR(sb.st_mode) && exists && w32disp != OPEN_EXISTING && w32disp != OPEN_ALWAYS) w32disp = OPEN_EXISTING;
    DWORD flags = 0x02000000 /* FILE_FLAG_BACKUP_SEMANTICS */ | (attrs & 0xFFFF);
    if (options & O_REPARSE) flags |= 0x00200000;     /* FILE_FLAG_OPEN_REPARSE_POINT */
    if (options & O_DELCLOSE) flags |= 0x04000000;    /* FILE_FLAG_DELETE_ON_CLOSE */
    HANDLE h = lsiw_create_file_linux(path, access, share, w32disp, flags);
    if (h == INVALID_HANDLE_VALUE) { st = win32_to_ntstatus(lsiw_get_last_error()); goto done_info0; }
    *out = h;
    st = STATUS_SUCCESS;
    iosb->Status = st; iosb->Information = info;
    return st;
done_info4:
    iosb->Status = st; iosb->Information = 4;   /* FILE_EXISTS */
    return st;
done_info0:
    iosb->Status = st; iosb->Information = 0;
    return st;
}

API NTSTATUS NtOpenFile(HANDLE *out, DWORD access, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb, ULONG share, ULONG options)
{ return NtCreateFile(out, access, oa, iosb, NULL, 0, share, 1 /* FILE_OPEN */, options, NULL, 0); }

static void nt_signal(HANDLE ev)
{
    if (!ev) return;
    BOOL (WINAPI *set)(HANDLE) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "SetEvent");
    if (set) set(ev);
}

/* Synchronous: completes before returning, so STATUS_PENDING never occurs. */
API NTSTATUS NtReadFile(HANDLE h, HANDLE ev, void *apc, void *apc_ctx, IO_STATUS_BLOCK *iosb,
                        void *buf, ULONG len, int64_t *off, ULONG *key)
{
    (void)apc; (void)apc_ctx; (void)key;
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) return (NTSTATUS)STATUS_INVALID_HANDLE_;
    ssize_t r;
    int64_t pos = off ? *off : -2;
    if (pos >= 0) {
        do r = pread(fd, buf, len, (off_t)pos); while (r < 0 && errno == EINTR);
        if (r >= 0) lseek(fd, (off_t)(pos + r), SEEK_SET);   /* synchronous handles move, as on Windows */
    } else {
        do r = read(fd, buf, len); while (r < 0 && errno == EINTR);
    }
    NTSTATUS st;
    if (r < 0) { st = (NTSTATUS)lsiw_errno_to_ntstatus(errno); r = 0; }
    else if (r == 0 && len) {
        struct stat sb;
        st = (NTSTATUS)((fstat(fd, &sb) == 0 && (S_ISFIFO(sb.st_mode) || S_ISSOCK(sb.st_mode)))
                        ? STATUS_PIPE_BROKEN_ : STATUS_END_OF_FILE_);
    } else st = STATUS_SUCCESS;
    if (iosb) { iosb->Status = st; iosb->Information = (ULONG_PTR)r; }
    nt_signal(ev);
    return st;
}

API NTSTATUS NtWriteFile(HANDLE h, HANDLE ev, void *apc, void *apc_ctx, IO_STATUS_BLOCK *iosb,
                         const void *buf, ULONG len, int64_t *off, ULONG *key)
{
    (void)apc; (void)apc_ctx; (void)key;
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) return (NTSTATUS)STATUS_INVALID_HANDLE_;
    ssize_t r;
    int64_t pos = off ? *off : -2;
    if (pos == -1) {                                 /* FILE_WRITE_TO_END_OF_FILE */
        lseek(fd, 0, SEEK_END);
        do r = write(fd, buf, len); while (r < 0 && errno == EINTR);
    } else if (pos >= 0) {
        do r = pwrite(fd, buf, len, (off_t)pos); while (r < 0 && errno == EINTR);
        if (r >= 0) lseek(fd, (off_t)(pos + r), SEEK_SET);
    } else {
        do r = write(fd, buf, len); while (r < 0 && errno == EINTR);
    }
    NTSTATUS st = STATUS_SUCCESS;
    if (r < 0) { st = (NTSTATUS)lsiw_errno_to_ntstatus(errno); r = 0; }
    if (iosb) { iosb->Status = st; iosb->Information = (ULONG_PTR)r; }
    nt_signal(ev);
    return st;
}

API NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb)
{
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) return (NTSTATUS)STATUS_INVALID_HANDLE_;
    NTSTATUS st = (fsync(fd) < 0 && errno != EINVAL && errno != EROFS) ? (NTSTATUS)lsiw_errno_to_ntstatus(errno) : STATUS_SUCCESS;
    if (iosb) { iosb->Status = st; iosb->Information = 0; }
    return st;
}

const struct lsiw_export lsiw_ntdll_exports[] = {
    E(RtlInitUnicodeString), E(RtlInitUnicodeStringEx), E(RtlInitString), E(RtlInitAnsiString),
    E(RtlAnsiStringToUnicodeString), E(RtlUnicodeStringToAnsiString), E(RtlFreeUnicodeString), E(RtlFreeAnsiString),
    E(RtlMultiByteToUnicodeN), E(RtlUnicodeToMultiByteN), E(RtlMultiByteToUnicodeSize), E(RtlUnicodeToMultiByteSize),
    E(RtlCompareUnicodeString), E(RtlEqualUnicodeString), E(RtlCopyUnicodeString),
    E(NtCreateFile), E(NtOpenFile), E(NtReadFile), E(NtWriteFile), E(NtFlushBuffersFile),
    EX("ZwCreateFile", NtCreateFile), EX("ZwOpenFile", NtOpenFile), EX("ZwReadFile", NtReadFile), EX("ZwWriteFile", NtWriteFile),
    E(RtlNtStatusToDosError), E(RtlNtStatusToDosErrorNoTeb), E(RtlGetLastNtStatus), E(RtlGetLastWin32Error),
    E(RtlSetLastWin32Error), E(RtlSetLastWin32ErrorAndNtStatusFromNtStatus), E(RtlRestoreLastWin32Error),
    E(NtCurrentTeb), E(RtlGetCurrentPeb), E(RtlGetVersion), E(RtlGetNtVersionNumbers), E(RtlIsProcessorFeaturePresent),
    E(RtlComputeCrc32), E(RtlAssert), E(RtlRandomEx), E(RtlRandom), E(RtlUniform), E(RtlGenRandom), E(SystemFunction036),
    E(NtAllocateVirtualMemory), E(NtFreeVirtualMemory), E(NtProtectVirtualMemory), E(NtQueryVirtualMemory),
    E(NtReadVirtualMemory), E(NtWriteVirtualMemory), E(NtClose),
    E(NtQueryPerformanceCounter), E(NtDelayExecution), E(NtYieldExecution), E(NtQuerySystemTime),
    E(RtlQueryPerformanceCounter), E(RtlQueryPerformanceFrequency),
    E(NtQuerySystemInformation), E(NtQueryInformationProcess), E(NtQueryInformationThread),
    E(NtSetInformationThread), E(NtSetInformationProcess),
    E(LdrGetDllHandle), E(LdrGetProcedureAddress), E(LdrLoadDll), E(LdrUnloadDll), E(LdrFindResource_U),
    E(RtlDllShutdownInProgress), E(LdrDisableThreadCalloutsForDll), E(LdrInitializeThunk),
    E(RtlExpandEnvironmentStrings_U), E(RtlRaiseStatus),
    E(RtlInitializeBitMap), E(RtlSetBit), E(RtlClearBit), E(RtlTestBit), E(RtlClearAllBits), E(RtlSetAllBits), E(RtlFindClearBitsAndSet),
    E(NtQueryObject), E(RtlAcquirePebLock), E(RtlReleasePebLock),
    E(NtSetEvent), E(NtWaitForSingleObject),
    { NULL, NULL }
};
