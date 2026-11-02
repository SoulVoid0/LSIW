/*
 * LSIW - kernel32: older and less common file functions (_lopen family,
 * OpenFile, LZ expansion of "SZDD" files), ReOpenFile, CopyFile2, process
 * snapshots (Pss*), geographical information and WER flags.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef int HFILE;
typedef int INT;
typedef int64_t LPARAM;
#define ERROR_BAD_LENGTH 24
#define HFILE_ERROR (-1)

#define GENERIC_READ_   0x80000000u
#define GENERIC_WRITE_  0x40000000u
#define CREATE_ALWAYS_  2
#define OPEN_EXISTING_  3
#define ERROR_NOT_SUPPORTED_ 50
#define ERROR_INVALID_FLAGS_ 1004

static HFILE handle_to_hfile(HANDLE h) { return h ? (HFILE)(intptr_t)h : HFILE_ERROR; }
static HANDLE hfile_to_handle(HFILE f) { return (HANDLE)(intptr_t)f; }

static int fd_of(HFILE f)
{
    int fd = lsiw_fd_from_handle(hfile_to_handle(f));
    if (fd < 0) lsiw_set_last_error(ERROR_INVALID_HANDLE);
    return fd;
}

typedef BOOL (WINAPI *close_handle_fn)(HANDLE);
static BOOL close_handle(HANDLE h)
{
    static close_handle_fn fn;
    if (!fn) fn = (close_handle_fn)lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "CloseHandle");
    return fn ? fn(h) : FALSE;
}

/* ================================================================== */
/* _lopen, _lcreat, _lread, _lwrite, _llseek, _lclose                   */
/* ================================================================== */

/* OF_READ / OF_WRITE / OF_READWRITE in the low bits; share modes in 0x70. */
static DWORD of_access(UINT style)
{
    switch (style & 3) {
    case 1:  return GENERIC_WRITE_;
    case 2:  return GENERIC_READ_ | GENERIC_WRITE_;
    default: return GENERIC_READ_;
    }
}

static DWORD of_share(UINT style)
{
    switch (style & 0x70) {
    case 0x10: return 0;            /* OF_SHARE_EXCLUSIVE */
    case 0x20: return 1;            /* OF_SHARE_DENY_WRITE */
    case 0x30: return 2;            /* OF_SHARE_DENY_READ */
    default:   return 3;            /* OF_SHARE_COMPAT / DENY_NONE */
    }
}

static HANDLE open_win_path(const char *win, DWORD access, DWORD share, DWORD disp)
{
    char *lin = lsiw_win_to_linux(win);
    if (!lin) { lsiw_set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return lsiw_create_file_linux(lin, access, share, disp, 0x80 /* FILE_ATTRIBUTE_NORMAL */);
}

API HFILE _lopen(const char *path, int style)
{
    HANDLE h = open_win_path(path, of_access((UINT)style), of_share((UINT)style), OPEN_EXISTING_);
    return h == INVALID_HANDLE_VALUE ? HFILE_ERROR : handle_to_hfile(h);
}

API HFILE _lcreat(const char *path, int attr)
{
    (void)attr;
    HANDLE h = open_win_path(path, GENERIC_READ_ | GENERIC_WRITE_, 3, CREATE_ALWAYS_);
    return h == INVALID_HANDLE_VALUE ? HFILE_ERROR : handle_to_hfile(h);
}

API long _hread(HFILE f, void *buf, long n)
{
    int fd = fd_of(f);
    if (fd < 0) return HFILE_ERROR;
    long total = 0;
    while (total < n) {
        ssize_t r = read(fd, (char *)buf + total, (size_t)(n - total));
        if (r < 0) { if (errno == EINTR) continue; lsiw_set_errno_error(errno); return HFILE_ERROR; }
        if (!r) break;
        total += r;
    }
    return total;
}
API UINT _lread(HFILE f, void *buf, UINT n) { return (UINT)_hread(f, buf, (long)n); }

API long _hwrite(HFILE f, const char *buf, long n)
{
    int fd = fd_of(f);
    if (fd < 0) return HFILE_ERROR;
    /* A zero-length write truncates the file at the current position. */
    if (n == 0) {
        off_t pos = lseek(fd, 0, SEEK_CUR);
        if (pos < 0 || ftruncate(fd, pos) < 0) { lsiw_set_errno_error(errno); return HFILE_ERROR; }
        return 0;
    }
    long total = 0;
    while (total < n) {
        ssize_t w = write(fd, buf + total, (size_t)(n - total));
        if (w < 0) { if (errno == EINTR) continue; lsiw_set_errno_error(errno); return HFILE_ERROR; }
        total += w;
    }
    return total;
}
API UINT _lwrite(HFILE f, const char *buf, UINT n) { return (UINT)_hwrite(f, buf, (long)n); }

API LONG _llseek(HFILE f, LONG off, int origin)
{
    int fd = fd_of(f);
    if (fd < 0) return HFILE_ERROR;
    off_t r = lseek(fd, off, origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET);
    if (r < 0) { lsiw_set_errno_error(errno); return HFILE_ERROR; }
    return (LONG)r;
}

API HFILE _lclose(HFILE f) { return close_handle(hfile_to_handle(f)) ? 0 : HFILE_ERROR; }

/* ================================================================== */
/* OpenFile                                                             */
/* ================================================================== */

/* OFSTRUCT: cBytes, fFixedDisk, nErrCode, Reserved1, Reserved2,
 * szPathName[128]. */
typedef struct { BYTE bytes, fixed_disk; WORD err; WORD r1, r2; char path[128]; } OFSTRUCT;

#define OF_DELETE  0x0200
#define OF_EXIST   0x4000
#define OF_PARSE   0x0100
#define OF_CREATE  0x1000
#define OF_REOPEN  0x8000

static void full_win_path(const char *name, char out[128])
{
    if ((name[0] && name[1] == ':') || name[0] == '\\' || name[0] == '/') {
        snprintf(out, 128, "%s", name);
        return;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) { snprintf(out, 128, "%s", name); return; }
    char *w = lsiw_linux_to_win(cwd);
    size_t n = strlen(w);
    snprintf(out, 128, "%s%s%s", w, n && w[n - 1] == '\\' ? "" : "\\", name);
    free(w);
}

API HFILE OpenFile(const char *name, OFSTRUCT *of, UINT style)
{
    if (!of) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return HFILE_ERROR; }
    const char *path = (style & OF_REOPEN) ? of->path : name;
    if (!path) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return HFILE_ERROR; }
    char full[128];
    full_win_path(path, full);
    of->bytes = (BYTE)sizeof *of;
    of->fixed_disk = 1;
    of->err = 0;
    snprintf(of->path, sizeof of->path, "%s", full);
    if (style & OF_PARSE) return 0;

    if (style & OF_DELETE) {
        char *lin = lsiw_win_to_linux(full);
        int r = lin ? unlink(lin) : -1;
        int e = errno;
        free(lin);
        if (r < 0) { lsiw_set_errno_error(e); of->err = (WORD)lsiw_get_last_error(); return HFILE_ERROR; }
        return 1;
    }
    DWORD disp = (style & OF_CREATE) ? CREATE_ALWAYS_ : OPEN_EXISTING_;
    DWORD access = (style & OF_CREATE) ? GENERIC_READ_ | GENERIC_WRITE_ : of_access(style);
    HANDLE h = open_win_path(full, access, of_share(style), disp);
    if (h == INVALID_HANDLE_VALUE) { of->err = (WORD)lsiw_get_last_error(); return HFILE_ERROR; }
    if (style & OF_EXIST) { close_handle(h); return 1; }
    return handle_to_hfile(h);
}

/* ================================================================== */
/* LZ expansion (lz32): files compressed by COMPRESS.EXE ("SZDD")       */
/* ================================================================== */

/* Header: "SZDD" 88 F0 27 33, method 'A', the replaced last character of
 * the file name, then the expanded size (little-endian DWORD). The body is
 * LZSS: a 4096-byte window filled with spaces, writing from 4096-16; each
 * flag byte (LSB first) says literal (1) or a 2-byte back reference (0):
 * position = b0 | (b1 & 0xF0) << 4, length = (b1 & 0x0F) + 3. */

#define LZ_MIN_HANDLE 0x400
#define LZ_MAX_HANDLES 16
#define LZERROR_BADINHANDLE  (-1)
#define LZERROR_BADOUTHANDLE (-2)
#define LZERROR_READ         (-3)
#define LZERROR_WRITE        (-4)
#define LZERROR_GLOBALLOC    (-5)
#define LZERROR_BADVALUE     (-7)
#define LZERROR_UNKNOWNALG   (-8)

static const uint8_t g_szdd[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };

struct lz_state { int used; HFILE file; uint8_t *data; size_t size, pos; };
static struct lz_state g_lz[LZ_MAX_HANDLES];
static pthread_mutex_t g_lz_lock = PTHREAD_MUTEX_INITIALIZER;

static struct lz_state *lz_get(HFILE h)
{
    if (h < LZ_MIN_HANDLE || h >= LZ_MIN_HANDLE + LZ_MAX_HANDLES) return NULL;
    struct lz_state *s = &g_lz[h - LZ_MIN_HANDLE];
    return s->used ? s : NULL;
}

static uint8_t *read_all(int fd, size_t *len)
{
    size_t cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; buf = realloc(buf, cap); }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; free(buf); return NULL; }
        if (!r) break;
        n += (size_t)r;
    }
    *len = n;
    return buf;
}

static uint8_t *lz_expand(const uint8_t *in, size_t in_len, size_t out_len)
{
    uint8_t window[4096];
    memset(window, ' ', sizeof window);
    unsigned wpos = 4096 - 16;
    uint8_t *out = malloc(out_len ? out_len : 1);
    size_t o = 0, i = 0;
    while (i < in_len && o < out_len) {
        uint8_t flags = in[i++];
        for (int bit = 0; bit < 8 && i < in_len && o < out_len; bit++) {
            if (flags & (1u << bit)) {
                uint8_t c = in[i++];
                out[o++] = c;
                window[wpos] = c;
                wpos = (wpos + 1) & 0xfff;
            } else {
                if (i + 1 >= in_len) { i = in_len; break; }
                unsigned pos = in[i] | ((unsigned)(in[i + 1] & 0xF0) << 4);
                unsigned len = (in[i + 1] & 0x0F) + 3u;
                i += 2;
                for (unsigned k = 0; k < len && o < out_len; k++) {
                    uint8_t c = window[(pos + k) & 0xfff];
                    out[o++] = c;
                    window[wpos] = c;
                    wpos = (wpos + 1) & 0xfff;
                }
            }
        }
    }
    if (o < out_len) memset(out + o, 0, out_len - o);
    return out;
}

/* LZInit: a compressed file gets an LZ handle; any other file is returned
 * as it is, so the LZ functions work on both. */
API INT LZInit(HFILE src)
{
    int fd = fd_of(src);
    if (fd < 0) return LZERROR_BADINHANDLE;
    off_t start = lseek(fd, 0, SEEK_CUR);
    uint8_t hdr[14];
    ssize_t r = pread(fd, hdr, sizeof hdr, start < 0 ? 0 : start);
    if (r < (ssize_t)sizeof hdr || memcmp(hdr, g_szdd, 8)) return src;
    if (hdr[8] != 'A') return LZERROR_UNKNOWNALG;
    size_t out_len = (size_t)hdr[10] | (size_t)hdr[11] << 8 | (size_t)hdr[12] << 16 | (size_t)hdr[13] << 24;
    if (lseek(fd, (start < 0 ? 0 : start) + 14, SEEK_SET) < 0) return LZERROR_READ;
    size_t in_len;
    uint8_t *in = read_all(fd, &in_len);
    if (!in) return LZERROR_READ;
    uint8_t *data = lz_expand(in, in_len, out_len);
    free(in);
    pthread_mutex_lock(&g_lz_lock);
    for (int k = 0; k < LZ_MAX_HANDLES; k++)
        if (!g_lz[k].used) {
            g_lz[k] = (struct lz_state){ 1, src, data, out_len, 0 };
            pthread_mutex_unlock(&g_lz_lock);
            return LZ_MIN_HANDLE + k;
        }
    pthread_mutex_unlock(&g_lz_lock);
    free(data);
    return LZERROR_GLOBALLOC;
}

API INT LZRead(INT h, char *buf, INT n)
{
    struct lz_state *s = lz_get(h);
    if (!s) {
        long r = _hread(h, buf, n);
        return r < 0 ? LZERROR_READ : (INT)r;
    }
    if (n < 0) return LZERROR_BADVALUE;
    size_t avail = s->size - s->pos, take = (size_t)n < avail ? (size_t)n : avail;
    memcpy(buf, s->data + s->pos, take);
    s->pos += take;
    return (INT)take;
}

API LONG LZSeek(INT h, LONG off, INT mode)
{
    struct lz_state *s = lz_get(h);
    if (!s) { LONG r = _llseek(h, off, mode); return r < 0 ? LZERROR_BADINHANDLE : r; }
    long base = mode == 1 ? (long)s->pos : mode == 2 ? (long)s->size : 0;
    long np = base + off;
    if (mode < 0 || mode > 2 || np < 0 || (size_t)np > s->size) return LZERROR_BADVALUE;
    s->pos = (size_t)np;
    return (LONG)np;
}

API void LZClose(INT h)
{
    struct lz_state *s = lz_get(h);
    if (!s) { _lclose(h); return; }
    HFILE f = s->file;
    pthread_mutex_lock(&g_lz_lock);
    free(s->data);
    memset(s, 0, sizeof *s);
    pthread_mutex_unlock(&g_lz_lock);
    _lclose(f);
}

API LONG LZCopy(INT src, INT dst)
{
    int dfd = fd_of(dst);
    if (dfd < 0) return LZERROR_BADOUTHANDLE;
    INT h = src;
    int opened = 0;
    if (!lz_get(src)) {
        h = LZInit(src);
        if (h < 0) return h;
        opened = h != src;
    }
    char buf[8192];
    LONG total = 0;
    for (;;) {
        INT r = LZRead(h, buf, sizeof buf);
        if (r < 0) return LZERROR_READ;
        if (!r) break;
        if (_hwrite(dst, buf, r) != r) return LZERROR_WRITE;
        total += r;
    }
    if (opened) {
        /* LZInit took the source over; give the caller's handle back. */
        struct lz_state *s = lz_get(h);
        pthread_mutex_lock(&g_lz_lock);
        free(s->data);
        memset(s, 0, sizeof *s);
        pthread_mutex_unlock(&g_lz_lock);
    }
    return total;
}
API LONG CopyLZFile(INT src, INT dst) { return LZCopy(src, dst); }
API INT LZStart(void) { return 1; }
API void LZDone(void) { }

/* The name a compressed file expands to: COMPRESS.EXE replaced the last
 * character of the extension with '_' and stored the original in the
 * header. */
static int expanded_name(const char *in, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s", in);
    char *lin = lsiw_win_to_linux(in);
    if (!lin) return 1;
    int fd = open(lin, O_RDONLY | O_CLOEXEC);
    free(lin);
    if (fd < 0) return 1;
    uint8_t hdr[14];
    ssize_t r = read(fd, hdr, sizeof hdr);
    close(fd);
    if (r < (ssize_t)sizeof hdr || memcmp(hdr, g_szdd, 8)) return 1;
    size_t n = strlen(out);
    if (n && out[n - 1] == '_' && hdr[9]) {
        char c = (char)hdr[9];
        /* Match the case of the rest of the name. */
        int lower = 0;
        for (size_t i = 0; i < n; i++) if (out[i] >= 'a' && out[i] <= 'z') { lower = 1; break; }
        if (lower && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n - 1] = c;
    }
    return 1;
}

API INT GetExpandedNameA(const char *in, char *out)
{
    if (!in || !out) return LZERROR_BADVALUE;
    char tmp[4096];
    expanded_name(in, tmp, sizeof tmp);
    strcpy(out, tmp);
    return 1;
}
API INT GetExpandedNameW(const WCHAR *in, WCHAR *out)
{
    if (!in || !out) return LZERROR_BADVALUE;
    char *a = lsiw_w2u(in), tmp[4096];
    expanded_name(a, tmp, sizeof tmp);
    free(a);
    WCHAR *w = lsiw_u2w(tmp);
    memcpy(out, w, (lsiw_wcslen(w) + 1) * sizeof(WCHAR));
    free(w);
    return 1;
}

API INT LZOpenFileA(const char *name, OFSTRUCT *of, WORD style)
{
    HFILE f = OpenFile(name, of, style);
    if (f == HFILE_ERROR && !(style & (OF_CREATE | OF_DELETE))) {
        /* Try the compressed name: last character replaced by '_'. */
        size_t n = strlen(name);
        if (n) {
            char *alt = strdup(name);
            alt[n - 1] = '_';
            f = OpenFile(alt, of, style);
            free(alt);
        }
    }
    if (f == HFILE_ERROR) return LZERROR_BADINHANDLE;
    if ((style & 3) == 0 && !(style & (OF_EXIST | OF_DELETE | OF_PARSE))) return LZInit(f);
    return f;
}
API INT LZOpenFileW(const WCHAR *name, OFSTRUCT *of, WORD style)
{
    char *a = lsiw_w2u(name);
    INT r = LZOpenFileA(a, of, style);
    free(a);
    return r;
}

/* ================================================================== */
/* ReOpenFile, CopyFile2                                                */
/* ================================================================== */

API HANDLE ReOpenFile(HANDLE h, DWORD access, DWORD share, DWORD flags)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    if (!o || o->fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }
    char *path;
    if (o->path) path = strdup(o->path);
    else {
        char proc[64], buf[4096];
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", o->fd);
        ssize_t n = readlink(proc, buf, sizeof buf - 1);
        if (n <= 0 || buf[0] != '/') { lsiw_set_last_error(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }
        buf[n] = 0;
        path = strdup(buf);
    }
    return lsiw_create_file_linux(path, access, share, OPEN_EXISTING_, flags);
}

typedef BOOL (WINAPI *copy_file_w_fn)(const WCHAR *, const WCHAR *, BOOL);

/* COPYFILE2_EXTENDED_PARAMETERS: dwSize, dwCopyFlags, pfCancel,
 * pProgressRoutine, pvCallbackContext. Returns an HRESULT. */
API LONG CopyFile2(const WCHAR *src, const WCHAR *dst, const DWORD *params)
{
    static copy_file_w_fn copy;
    if (!copy) copy = (copy_file_w_fn)lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "CopyFileW");
    if (!src || !dst || !copy) return (LONG)0x80070057;                   /* E_INVALIDARG */
    BOOL fail_if_exists = params && params[0] >= 8 && (params[1] & 1);    /* COPY_FILE_FAIL_IF_EXISTS */
    if (params && params[0] >= 16) {
        const BOOL *cancel = *(BOOL *const *)(params + 2);
        if (cancel && *cancel) return (LONG)0x800704C7;                   /* ERROR_CANCELLED */
    }
    if (copy(src, dst, fail_if_exists)) return 0;
    DWORD e = lsiw_get_last_error();
    return e ? (LONG)(0x80070000u | (e & 0xffff)) : (LONG)0x80004005;
}

/* ================================================================== */
/* Process snapshots (PssCaptureSnapshot)                               */
/* ================================================================== */

/* Only the process information part is captured (that is what callers
 * such as Python's os.getppid() query); the thread, handle and memory
 * parts are reported as not captured. */

#define PSS_MAGIC 0x50535331u   /* "PSS1" */

struct pss_snapshot {
    uint32_t magic;
    uint8_t info[704];           /* PSS_PROCESS_INFORMATION */
};

static uint64_t boot_time_100ns(void)
{
    static uint64_t cached;
    if (cached) return cached;
    FILE *f = fopen("/proc/stat", "re");
    if (!f) return 0;
    char line[256];
    unsigned long long btime = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "btime %llu", &btime) == 1) break;
    fclose(f);
    cached = (btime + 11644473600ULL) * 10000000ULL;
    return cached;
}

static int capture_process(int pid, uint8_t *pi)
{
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *r = strrchr(buf, ')');
    if (!r) return 0;
    /* Fields after ")": state ppid pgrp session tty tpgid flags minflt
     * cminflt majflt cmajflt utime stime cutime cstime priority nice
     * num_threads itrealvalue starttime vsize rss */
    char state;
    long ppid, d;
    unsigned long minflt, majflt, utime, stime, vsize;
    unsigned long long starttime;
    long rss, nice;
    if (sscanf(r + 2, "%c %ld %ld %ld %ld %ld %ld %lu %ld %lu %ld %lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %ld",
               &state, &ppid, &d, &d, &d, &d, &d, &minflt, &d, &majflt, &d, &utime, &stime, &d, &d, &d, &nice,
               &d, &d, &starttime, &vsize, &rss) < 22) return 0;
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    memset(pi, 0, 704);
    *(DWORD *)(pi + 0) = state == 'Z' ? 0 : 259;                 /* ExitStatus (STILL_ACTIVE) */
    if (pid == getpid()) *(void **)(pi + 8) = lsiw_peb();
    *(ULONG_PTR *)(pi + 16) = (ULONG_PTR)((1ull << sysconf(_SC_NPROCESSORS_ONLN)) - 1);
    *(LONG *)(pi + 24) = 8;                                      /* BasePriority */
    *(DWORD *)(pi + 28) = (DWORD)pid;
    *(DWORD *)(pi + 32) = (DWORD)ppid;
    *(uint64_t *)(pi + 40) = boot_time_100ns() + starttime * 10000000ULL / (uint64_t)hz;   /* CreateTime */
    *(uint64_t *)(pi + 56) = (uint64_t)stime * 10000000ULL / (uint64_t)hz;                 /* KernelTime */
    *(uint64_t *)(pi + 64) = (uint64_t)utime * 10000000ULL / (uint64_t)hz;                 /* UserTime */
    *(DWORD *)(pi + 72) = nice < 0 ? 0x80 : nice > 0 ? 0x4000 : 0x20;                     /* PriorityClass */
    *(SIZE_T *)(pi + 80) = vsize;
    *(SIZE_T *)(pi + 88) = vsize;
    *(DWORD *)(pi + 96) = (DWORD)(minflt + majflt);
    *(SIZE_T *)(pi + 104) = (SIZE_T)rss * (SIZE_T)page;
    *(SIZE_T *)(pi + 112) = (SIZE_T)rss * (SIZE_T)page;
    *(SIZE_T *)(pi + 168) = (SIZE_T)rss * (SIZE_T)page;          /* PrivateUsage */
    /* ImageFileName: the Windows path of the executable. */
    char exe[64], target[4096];
    snprintf(exe, sizeof exe, "/proc/%d/exe", pid);
    ssize_t len = readlink(exe, target, sizeof target - 1);
    if (pid == getpid() && lsiw_main_module && lsiw_main_module->win_path)
        lsiw_utf8_to_utf16(lsiw_main_module->win_path, -1, (WCHAR *)(pi + 180), 260);
    else if (len > 0) {
        target[len] = 0;
        char *w = lsiw_linux_to_win(target);
        lsiw_utf8_to_utf16(w, -1, (WCHAR *)(pi + 180), 260);
        free(w);
    }
    return 1;
}

static int process_pid(HANDLE p)
{
    if (p == (HANDLE)(intptr_t)-1) return getpid();
    struct lsiw_handle_obj *o = lsiw_handle_get(p, H_PROCESS);
    return o ? o->pid : -1;
}

API DWORD PssCaptureSnapshot(HANDLE process, DWORD flags, DWORD ctx_flags, void **snapshot)
{
    (void)flags; (void)ctx_flags;
    if (!snapshot) return ERROR_INVALID_PARAMETER;
    int pid = process_pid(process);
    if (pid <= 0) return ERROR_INVALID_HANDLE;
    struct pss_snapshot *s = calloc(1, sizeof *s);
    s->magic = PSS_MAGIC;
    if (!capture_process(pid, s->info)) { free(s); return ERROR_INVALID_PARAMETER; }
    *snapshot = s;
    return 0;
}

API DWORD PssQuerySnapshot(void *snapshot, int cls, void *buf, DWORD len)
{
    struct pss_snapshot *s = snapshot;
    if (!s || s->magic != PSS_MAGIC || !buf) return ERROR_INVALID_PARAMETER;
    switch (cls) {
    case 0:   /* PSS_QUERY_PROCESS_INFORMATION */
        if (len < sizeof s->info) return ERROR_BAD_LENGTH;
        memcpy(buf, s->info, sizeof s->info);
        return 0;
    case 5:   /* PSS_QUERY_THREAD_INFORMATION: ThreadsCaptured, ContextLength */
        if (len < 8) return ERROR_BAD_LENGTH;
        memset(buf, 0, 8);
        return 0;
    case 4:   /* PSS_QUERY_HANDLE_INFORMATION: HandlesCaptured */
        if (len < 4) return ERROR_BAD_LENGTH;
        memset(buf, 0, 4);
        return 0;
    default:
        return ERROR_NOT_SUPPORTED_;
    }
}

API DWORD PssFreeSnapshot(HANDLE process, void *snapshot)
{
    (void)process;
    struct pss_snapshot *s = snapshot;
    if (!s || s->magic != PSS_MAGIC) return ERROR_INVALID_PARAMETER;
    s->magic = 0;
    free(s);
    return 0;
}

API DWORD PssDuplicateSnapshot(HANDLE src_proc, void *snapshot, HANDLE dst_proc, void **out, DWORD flags)
{
    (void)src_proc; (void)dst_proc; (void)flags;
    struct pss_snapshot *s = snapshot;
    if (!s || s->magic != PSS_MAGIC || !out) return ERROR_INVALID_PARAMETER;
    struct pss_snapshot *c = malloc(sizeof *c);
    memcpy(c, s, sizeof *c);
    *out = c;
    return 0;
}

/* Walk markers: nothing beyond the process information is captured, so
 * every walk is empty. */
API DWORD PssWalkMarkerCreate(const void *alloc, void **marker)
{ (void)alloc; if (!marker) return ERROR_INVALID_PARAMETER; *marker = calloc(1, 8); return 0; }
API DWORD PssWalkMarkerFree(void *marker) { free(marker); return 0; }
API DWORD PssWalkMarkerGetPosition(void *marker, ULONG_PTR *pos) { (void)marker; if (pos) *pos = 0; return 0; }
API DWORD PssWalkMarkerSetPosition(void *marker, ULONG_PTR pos) { (void)marker; (void)pos; return 0; }
API DWORD PssWalkMarkerSeekToBeginning(void *marker) { (void)marker; return 0; }
API DWORD PssWalkSnapshot(void *snapshot, int cls, void *marker, void *buf, DWORD len)
{ (void)snapshot; (void)cls; (void)marker; (void)buf; (void)len; return ERROR_NO_MORE_ITEMS; }

/* ================================================================== */
/* Geographical information                                             */
/* ================================================================== */

struct geo { int id; const char *iso2, *iso3, *num, *name, *official, *lang, *rfc1766, *lcid, *dial, *cur, *sym, *lat, *lon, *parent; };
static const struct geo g_geos[] = {
    { 244, "US", "USA", "840", "United States", "United States of America", "en", "en-US", "00000409", "1", "USD", "$", "39.76", "-98.5", "19" },
    { 242, "GB", "GBR", "826", "United Kingdom", "United Kingdom of Great Britain and Northern Ireland", "en", "en-GB", "00000809", "44", "GBP", "\xc2\xa3", "54.6", "-2", "10039882" },
    { 39,  "CA", "CAN", "124", "Canada", "Canada", "en;fr", "en-CA", "00001009", "1", "CAD", "$", "60", "-95", "19" },
    { 12,  "AU", "AUS", "036", "Australia", "Commonwealth of Australia", "en", "en-AU", "00000c09", "61", "AUD", "$", "-25", "135", "10039880" },
    { 94,  "DE", "DEU", "276", "Germany", "Federal Republic of Germany", "de", "de-DE", "00000407", "49", "EUR", "\xe2\x82\xac", "51", "9", "10210824" },
    { 84,  "FR", "FRA", "250", "France", "French Republic", "fr", "fr-FR", "0000040c", "33", "EUR", "\xe2\x82\xac", "46", "2", "10210824" },
    { 217, "ES", "ESP", "724", "Spain", "Kingdom of Spain", "es", "es-ES", "00000c0a", "34", "EUR", "\xe2\x82\xac", "40", "-4", "47610" },
    { 118, "IT", "ITA", "380", "Italy", "Italian Republic", "it", "it-IT", "00000410", "39", "EUR", "\xe2\x82\xac", "42.83", "12.83", "47610" },
    { 176, "NL", "NLD", "528", "Netherlands", "Kingdom of the Netherlands", "nl", "nl-NL", "00000413", "31", "EUR", "\xe2\x82\xac", "52.5", "5.75", "10210824" },
    { 122, "JP", "JPN", "392", "Japan", "Japan", "ja", "ja-JP", "00000411", "81", "JPY", "\xc2\xa5", "36", "138", "47600" },
    { 134, "KR", "KOR", "410", "Korea", "Republic of Korea", "ko", "ko-KR", "00000412", "82", "KRW", "\xe2\x82\xa9", "37", "127.5", "47600" },
    { 45,  "CN", "CHN", "156", "China", "People's Republic of China", "zh", "zh-CN", "00000804", "86", "CNY", "\xc2\xa5", "35", "105", "47600" },
    { 113, "IN", "IND", "356", "India", "Republic of India", "hi;en", "hi-IN", "00000439", "91", "INR", "\xe2\x82\xb9", "21", "78", "47614" },
    { 32,  "BR", "BRA", "076", "Brazil", "Federative Republic of Brazil", "pt", "pt-BR", "00000416", "55", "BRL", "R$", "-10", "-55", "31396" },
    { 166, "MX", "MEX", "484", "Mexico", "United Mexican States", "es", "es-MX", "0000080a", "52", "MXN", "$", "23", "-102", "27082" },
    { 203, "RU", "RUS", "643", "Russia", "Russian Federation", "ru", "ru-RU", "00000419", "7", "RUB", "\xe2\x82\xbd", "60", "100", "47609" },
    { 221, "SE", "SWE", "752", "Sweden", "Kingdom of Sweden", "sv", "sv-SE", "0000041d", "46", "SEK", "kr", "62", "15", "10039882" },
    { 223, "CH", "CHE", "756", "Switzerland", "Swiss Confederation", "de;fr;it", "de-CH", "00000807", "41", "CHF", "CHF", "47", "8", "10210824" },
};

static const struct geo *geo_by_id(int id)
{
    for (size_t i = 0; i < sizeof g_geos / sizeof g_geos[0]; i++) if (g_geos[i].id == id) return &g_geos[i];
    return NULL;
}
static const struct geo *geo_by_name(const char *n)
{
    for (size_t i = 0; i < sizeof g_geos / sizeof g_geos[0]; i++)
        if (!strcasecmp(g_geos[i].iso2, n) || !strcasecmp(g_geos[i].iso3, n) || !strcmp(g_geos[i].num, n)) return &g_geos[i];
    return NULL;
}

/* GEOTYPE values: 1 NATION, 2 LATITUDE, 3 LONGITUDE, 4 ISO2, 5 ISO3,
 * 6 RFC1766, 7 LCID, 8 FRIENDLYNAME, 9 OFFICIALNAME, 10 TIMEZONES,
 * 11 OFFICIALLANGUAGES, 12 ISO_UN_NUMBER, 13 PARENT, 14 DIALINGCODE,
 * 15 CURRENCYCODE, 16 CURRENCYSYMBOL, 17 NAME, 18 ID. */
static int geo_value(const struct geo *g, int type, char *out, size_t n)
{
    const char *v = NULL;
    char tmp[32];
    switch (type) {
    case 1: case 18: snprintf(tmp, sizeof tmp, "%d", g->id); v = tmp; break;
    case 2: v = g->lat; break;
    case 3: v = g->lon; break;
    case 4: case 17: v = g->iso2; break;
    case 5: v = g->iso3; break;
    case 6: v = g->rfc1766; break;
    case 7: v = g->lcid; break;
    case 8: v = g->name; break;
    case 9: v = g->official; break;
    case 11: v = g->lang; break;
    case 12: v = g->num; break;
    case 13: v = g->parent; break;
    case 14: v = g->dial; break;
    case 15: v = g->cur; break;
    case 16: v = g->sym; break;
    default: return 0;
    }
    snprintf(out, n, "%s", v);
    return 1;
}

static int geo_return_w(const char *val, WCHAR *data, int len)
{
    WCHAR *w = lsiw_u2w(val);
    int need = (int)lsiw_wcslen(w) + 1;
    if (!data || !len) { free(w); return need; }
    if (len < need) { free(w); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(data, w, (size_t)need * sizeof(WCHAR));
    free(w);
    return need;
}

API int GetGeoInfoW(int location, int type, WCHAR *data, int len, WORD lang)
{
    (void)lang;
    const struct geo *g = geo_by_id(location);
    char val[128];
    if (!g) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!geo_value(g, type, val, sizeof val)) { lsiw_set_last_error(ERROR_INVALID_FLAGS_); return 0; }
    return geo_return_w(val, data, len);
}

API int GetGeoInfoA(int location, int type, char *data, int len, WORD lang)
{
    (void)lang;
    const struct geo *g = geo_by_id(location);
    char val[128];
    if (!g) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!geo_value(g, type, val, sizeof val)) { lsiw_set_last_error(ERROR_INVALID_FLAGS_); return 0; }
    int need = (int)strlen(val) + 1;
    if (!data || !len) return need;
    if (len < need) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(data, val, (size_t)need);
    return need;
}

API int GetGeoInfoEx(const WCHAR *location, int type, WCHAR *data, int len)
{
    char *n = location ? lsiw_w2u(location) : NULL;
    const struct geo *g = n ? geo_by_name(n) : NULL;
    free(n);
    char val[128];
    if (!g) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    /* GetGeoInfoEx does not support the LCID and nation id types. */
    if (type == 1 || type == 7 || !geo_value(g, type, val, sizeof val)) { lsiw_set_last_error(ERROR_INVALID_FLAGS_); return 0; }
    return geo_return_w(val, data, len);
}

typedef BOOL (WINAPI *geo_enum_proc)(int);
typedef BOOL (WINAPI *geo_name_proc)(WCHAR *, LPARAM);

API BOOL EnumSystemGeoID(DWORD geoclass, int parent, geo_enum_proc proc)
{
    (void)parent;
    if (!proc || geoclass != 16 /* GEOCLASS_NATION */) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (size_t i = 0; i < sizeof g_geos / sizeof g_geos[0]; i++) if (!proc(g_geos[i].id)) break;
    return TRUE;
}

API BOOL EnumSystemGeoNames(DWORD geoclass, geo_name_proc proc, LPARAM lp)
{
    if (!proc || (geoclass != 16 && geoclass != 0)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (size_t i = 0; i < sizeof g_geos / sizeof g_geos[0]; i++) {
        WCHAR name[4];
        lsiw_utf8_to_utf16(g_geos[i].iso2, -1, name, 4);
        if (!proc(name, lp)) break;
    }
    return TRUE;
}

/* ================================================================== */
/* Windows Error Reporting flags                                        */
/* ================================================================== */

/* There is no error reporting service; the flags are only remembered. */
static DWORD g_wer_flags;
API LONG WerSetFlags(DWORD flags) { g_wer_flags = flags; return 0; }
API LONG WerGetFlags(HANDLE process, DWORD *flags)
{
    (void)process;
    if (!flags) return (LONG)0x80070057;
    *flags = g_wer_flags;
    return 0;
}

const struct lsiw_export lsiw_k32compat_exports[] = {
    E(_lopen), E(_lcreat), E(_lread), E(_hread), E(_lwrite), E(_hwrite), E(_llseek), E(_lclose), E(OpenFile),
    E(LZInit), E(LZRead), E(LZSeek), E(LZClose), E(LZCopy), E(CopyLZFile), E(LZStart), E(LZDone),
    E(GetExpandedNameA), E(GetExpandedNameW), E(LZOpenFileA), E(LZOpenFileW),
    E(ReOpenFile), E(CopyFile2),
    E(PssCaptureSnapshot), E(PssQuerySnapshot), E(PssFreeSnapshot), E(PssDuplicateSnapshot),
    E(PssWalkMarkerCreate), E(PssWalkMarkerFree), E(PssWalkMarkerGetPosition), E(PssWalkMarkerSetPosition),
    E(PssWalkMarkerSeekToBeginning), E(PssWalkSnapshot),
    E(GetGeoInfoW), E(GetGeoInfoA), E(GetGeoInfoEx), E(EnumSystemGeoID), E(EnumSystemGeoNames),
    E(WerSetFlags), E(WerGetFlags),
    { NULL, NULL }
};
