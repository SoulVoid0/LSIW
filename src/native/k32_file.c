/* kernel32: files, directories, the console, paths and volumes.
 *
 * Every Windows path a program passes in goes through lsiw_win_to_linux(),
 * so "C:\home\me\x.txt" opens /home/me/x.txt, any other drive letter is
 * folded onto C:, and names are matched case-insensitively.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define GENERIC_READ     0x80000000u
#define GENERIC_WRITE    0x40000000u
#define GENERIC_ALL      0x10000000u
#define FILE_WRITE_DATA  0x0002u
#define FILE_APPEND_DATA 0x0004u
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5
#define FILE_FLAG_DELETE_ON_CLOSE    0x04000000u
#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_HIDDEN    0x02
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE   0x20
#define FILE_ATTRIBUTE_NORMAL    0x80
#define INVALID_FILE_ATTRIBUTES  0xFFFFFFFFu
#define EPOCH_DIFF 116444736000000000ULL

typedef struct { DWORD lo, hi; } FILETIME;

static FILETIME ts_to_ft(struct timespec ts)
{
    uint64_t v = (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100 + EPOCH_DIFF;
    return (FILETIME){ (DWORD)v, (DWORD)(v >> 32) };
}

static struct timespec ft_to_ts(const FILETIME *ft)
{
    uint64_t v = ((uint64_t)ft->hi << 32) | ft->lo;
    if (v < EPOCH_DIFF) v = EPOCH_DIFF;
    v -= EPOCH_DIFF;
    return (struct timespec){ (time_t)(v / 10000000ULL), (long)(v % 10000000ULL) * 100 };
}

#define FILE_ATTRIBUTE_REPARSE_POINT_ 0x400
#define IO_REPARSE_TAG_SYMLINK_ 0xA000000Cu

static DWORD attrs_of(const char *path, const struct stat *st)
{
    DWORD a = 0;
    if (S_ISLNK(st->st_mode)) {
        /* A symbolic link is a reparse point; a link to a folder is also a
         * folder, as with Windows directory symlinks. */
        struct stat t;
        return FILE_ATTRIBUTE_REPARSE_POINT_ |
               ((stat(path, &t) == 0 && S_ISDIR(t.st_mode)) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
    }
    if (S_ISDIR(st->st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (base[0] == '.' && base[1] && strcmp(base, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    /* Read-only is the owner write bit (root can write anything, so access()
     * alone would never report it), or a file this user cannot write. */
    if (!S_ISDIR(st->st_mode) && (!(st->st_mode & S_IWUSR) || (access(path, W_OK) != 0 && errno == EACCES)))
        a |= FILE_ATTRIBUTE_READONLY;
    if (!a) a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    else if (!S_ISDIR(st->st_mode)) a |= FILE_ATTRIBUTE_ARCHIVE;
    return a;
}

/* errno for a missing file: distinguish "file" from "path" not found. */
static void set_not_found(const char *path)
{
    char *d = strdup(path);
    char *s = strrchr(d, '/');
    if (s && s != d) *s = 0;
    struct stat st;
    lsiw_set_last_error(stat(d, &st) == 0 ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
    free(d);
}

static void set_error_for(int err, const char *path)
{
    if (err == ENOENT && path) set_not_found(path);
    else lsiw_set_errno_error(err);
}

/* Reserved device names: NUL, CON, CONIN$, CONOUT$ (in any folder, any case). */
static const char *device_of(const char *win)
{
    const char *b = win;
    for (const char *p = win; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    char base[16];
    size_t i = 0;
    for (; b[i] && b[i] != '.' && i < sizeof base - 1; i++) base[i] = (char)toupper((unsigned char)b[i]);
    base[i] = 0;
    if (!strcmp(base, "NUL")) return "/dev/null";
    if (!strcmp(base, "CON") || !strcmp(base, "CONOUT$")) return "/dev/tty";
    if (!strcmp(base, "CONIN$")) return "/dev/tty";
    /* COMn (also \\.\COMn) is the n-th serial port, /dev/ttyS(n-1). */
    if (!strncmp(base, "COM", 3) && base[3] >= '1' && base[3] <= '9') {
        char *e;
        long n = strtol(base + 3, &e, 10);
        if (!*e && n >= 1 && n <= 256) {
            static __thread char com[24];
            snprintf(com, sizeof com, "/dev/ttyS%ld", n - 1);
            return com;
        }
    }
    return NULL;
}

/* ================================================================== */
/* CreateFile / ReadFile / WriteFile                                    */
/* ================================================================== */

/* k32_extra.c: named pipes and completion ports. */
int  lsiw_named_pipe_open(const char *win, DWORD access, DWORD flags, HANDLE *out);
void lsiw_iocp_io_done(HANDLE file, void *overlapped, DWORD bytes, uint32_t ntstatus);

static HANDLE create_file(const char *win, DWORD access, DWORD share, DWORD disp, DWORD flags)
{
    (void)share;
    if (!win || !*win) { lsiw_set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    HANDLE pipe_h;
    if (lsiw_named_pipe_open(win, access, flags, &pipe_h)) return pipe_h;   /* \\.\pipe\name */
    const char *dev = device_of(win);
    int want_write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    int want_read = (access & (GENERIC_READ | GENERIC_ALL | 0x1)) != 0 || !want_write;
    if (dev) {
        int fd;
        const char *b = win + strlen(win) - 7;
        if (strlen(win) >= 7 && !strcasecmp(b, "CONOUT$")) fd = fcntl(1, F_DUPFD_CLOEXEC, 0);
        else if (strlen(win) >= 6 && !strcasecmp(win + strlen(win) - 6, "CONIN$")) fd = fcntl(0, F_DUPFD_CLOEXEC, 0);
        else fd = open(dev, ((want_read && want_write) ? O_RDWR : want_write ? O_WRONLY : O_RDONLY) |
                            (!strncmp(dev, "/dev/ttyS", 9) ? O_NOCTTY : 0));
        if (fd < 0 && !strncmp(dev, "/dev/ttyS", 9)) {       /* no such serial port, or no permission */
            lsiw_set_last_error(errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        if (fd < 0) fd = open("/dev/null", O_RDWR);
        HANDLE h = lsiw_handle_from_fd(fd, isatty(fd));
        lsiw_set_last_error(0);
        return h ? h : INVALID_HANDLE_VALUE;
    }
    return lsiw_create_file_linux(lsiw_win_to_linux(win), access, share, disp, flags);
}

HANDLE lsiw_create_file_linux(char *path, DWORD access, DWORD share, DWORD disp, DWORD flags)
{
    (void)share;
    if (!path) { lsiw_set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    int want_write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    int want_read = (access & (GENERIC_READ | GENERIC_ALL | 0x1)) != 0 || !want_write;
    struct stat st;
    /* FILE_FLAG_OPEN_REPARSE_POINT: a symbolic link is opened itself, not
     * followed (needed to delete links without touching their targets). */
    if ((flags & 0x00200000) && lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        if (disp == CREATE_NEW) { free(path); lsiw_set_last_error(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE; }
        int lfd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (lfd < 0) { set_error_for(errno, path); free(path); return INVALID_HANDLE_VALUE; }
        struct lsiw_handle_obj *lo;
        HANDLE lh = lsiw_handle_new(H_FILE, &lo);
        if (!lh) { close(lfd); free(path); return INVALID_HANDLE_VALUE; }
        lo->fd = lfd;
        lo->path = path;
        lsiw_set_last_error(0);
        return lh;
    }
    int exists = stat(path, &st) == 0;
    int oflags = O_CLOEXEC;
    oflags |= (want_read && want_write) ? O_RDWR : want_write ? O_WRONLY : O_RDONLY;

    if (exists && S_ISDIR(st.st_mode)) {
        if (!(flags & 0x02000000 /* FILE_FLAG_BACKUP_SEMANTICS */) || disp == CREATE_NEW) {
            free(path);
            lsiw_set_last_error(disp == CREATE_NEW ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        oflags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    } else {
        /* Windows refuses write access to a read-only file, even for admins. */
        if (exists && S_ISREG(st.st_mode) && !(st.st_mode & S_IWUSR) && disp != CREATE_NEW &&
            (want_write || disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING)) {
            free(path);
            lsiw_set_last_error(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        switch (disp) {
        case CREATE_NEW:        oflags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS:     oflags |= O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING:     break;
        case OPEN_ALWAYS:       oflags |= O_CREAT; break;
        case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
        default: free(path); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
        }
        if ((oflags & O_TRUNC) && !want_write) oflags = (oflags & ~O_ACCMODE) | O_RDWR;
        if ((access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | FILE_WRITE_DATA | GENERIC_ALL))) oflags |= O_APPEND;
    }
    int mode = (flags & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    int fd = open(path, oflags, mode);
    if (fd < 0 && errno == EACCES && (oflags & O_ACCMODE) == O_RDWR && !(access & GENERIC_WRITE)) {
        oflags = (oflags & ~O_ACCMODE) | O_RDONLY;
        fd = open(path, oflags, mode);
    }
    /* Opening only to query attributes or delete (no data access requested)
     * works on Windows even without read permission. */
    if (fd < 0 && errno == EACCES && exists && !(oflags & (O_CREAT | O_TRUNC)) &&
        !(access & (GENERIC_READ | GENERIC_WRITE | GENERIC_ALL | 0x20000000 /* GENERIC_EXECUTE */ |
                    0x1 /* FILE_READ_DATA */ | FILE_WRITE_DATA | FILE_APPEND_DATA | 0x20 /* FILE_EXECUTE */)))
        fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == EEXIST) lsiw_set_last_error(ERROR_FILE_EXISTS);
        else set_error_for(e, path);
        LSIW_TRACE("CreateFile(%s) failed: %s\n", path, strerror(e));
        free(path);
        return INVALID_HANDLE_VALUE;
    }
    if (flags & FILE_FLAG_DELETE_ON_CLOSE) unlink(path);
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_FILE, &o);
    if (!h) { close(fd); free(path); return INVALID_HANDLE_VALUE; }
    o->fd = fd;
    o->path = path;
    o->is_console = isatty(fd);
    o->append = (oflags & O_APPEND) != 0;
    /* CREATE_ALWAYS / OPEN_ALWAYS report whether the file already existed. */
    lsiw_set_last_error(((disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) && exists) ? ERROR_ALREADY_EXISTS : 0);
    return h;
}

API HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl)
{ (void)sa; (void)tmpl; return create_file(name, access, share, disp, flags); }

API HANDLE CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    (void)sa; (void)tmpl;
    char *n = lsiw_w2u(name);
    HANDLE h = create_file(n, access, share, disp, flags);
    free(n);
    return h;
}

struct create_params2 { DWORD size, attrs, flags, sqos; void *sa; HANDLE tmpl; };
API HANDLE CreateFile2(const WCHAR *name, DWORD access, DWORD share, DWORD disp, struct create_params2 *p)
{
    char *n = lsiw_w2u(name);
    HANDLE h = create_file(n, access, share, disp, p ? (p->attrs | p->flags) : 0);
    free(n);
    return h;
}

typedef struct { ULONG_PTR internal, internal_high; DWORD offset, offset_high; HANDLE event; } OVERLAPPED;

static void complete_overlapped(OVERLAPPED *ov, DWORD n, DWORD status)
{
    ov->internal = status;
    ov->internal_high = n;
    if (ov->event) {
        struct lsiw_handle_obj *e = lsiw_handle_get(ov->event, H_EVENT);
        if (e) { lsiw_wait_lock(); e->signaled = 1; lsiw_wait_broadcast(); lsiw_wait_unlock(); }
    }
}

static int fd_of(HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE || !h) return -1;
    return lsiw_fd_from_handle(h);
}

/* Console input mode (ENABLE_LINE_INPUT, ENABLE_ECHO_INPUT, ...). */
static DWORD g_in_mode = 0x1 | 0x2 | 0x4;
static struct termios g_orig_tio;
static int g_tio_saved;

static void restore_tty(void) { if (g_tio_saved) tcsetattr(0, TCSANOW, &g_orig_tio); }

API BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *nread, OVERLAPPED *ov)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t r;
    if (ov && !isatty(fd)) {
        off_t off = (off_t)(((uint64_t)ov->offset_high << 32) | ov->offset);
        do r = pread(fd, buf, n, off); while (r < 0 && errno == EINTR);
        if (r < 0 && errno == ESPIPE) do r = read(fd, buf, n); while (r < 0 && errno == EINTR);
    } else {
        do r = read(fd, buf, n); while (r < 0 && errno == EINTR);
    }
    if (r < 0) {
        if (nread) *nread = 0;
        lsiw_set_errno_error(errno == EPIPE ? EPIPE : errno);
        return FALSE;
    }
    if (nread) *nread = (DWORD)r;
    if (r == 0 && n && !isatty(fd)) {
        struct stat st;
        /* End of a pipe or socket: the writer has gone. */
        if (fstat(fd, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
            if (ov) complete_overlapped(ov, 0, 0xC000014B /* STATUS_PIPE_BROKEN */);
            lsiw_set_last_error(109 /* ERROR_BROKEN_PIPE */);
            return FALSE;
        }
        if (ov) { complete_overlapped(ov, 0, 0xC0000011 /* STATUS_END_OF_FILE */); lsiw_set_last_error(ERROR_HANDLE_EOF); return FALSE; }
    }
    if (ov) { complete_overlapped(ov, (DWORD)r, 0); lsiw_iocp_io_done(h, ov, (DWORD)r, 0); }
    return TRUE;
}

API BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *nwritten, OVERLAPPED *ov)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (fd <= 2) lsiw_crt_flush_all();     /* keep CRT and raw console output in order */
    ssize_t r;
    size_t done = 0;
    while (done < n) {
        if (ov && !isatty(fd) && (ov->offset != 0xFFFFFFFF || ov->offset_high != 0xFFFFFFFF)) {
            off_t off = (off_t)(((uint64_t)ov->offset_high << 32) | ov->offset) + (off_t)done;
            r = pwrite(fd, (const char *)buf + done, n - done, off);
            if (r < 0 && errno == ESPIPE) r = write(fd, (const char *)buf + done, n - done);
        } else {
            r = write(fd, (const char *)buf + done, n - done);
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (nwritten) *nwritten = (DWORD)done;
            lsiw_set_last_error(errno == EPIPE ? 232 /* ERROR_NO_DATA */ : lsiw_errno_to_win(errno));
            return FALSE;
        }
        done += (size_t)r;
    }
    if (nwritten) *nwritten = (DWORD)done;
    if (ov) { complete_overlapped(ov, (DWORD)done, 0); lsiw_iocp_io_done(h, ov, (DWORD)done, 0); }
    return TRUE;
}

#define STATUS_PENDING_ 0x103
API BOOL GetOverlappedResultEx(HANDLE h, OVERLAPPED *ov, DWORD *n, DWORD ms, BOOL alertable)
{
    if (!ov) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (__atomic_load_n(&ov->internal, __ATOMIC_ACQUIRE) == STATUS_PENDING_) {
        if (!ms) { lsiw_set_last_error(996 /* ERROR_IO_INCOMPLETE */); return FALSE; }
        DWORD (WINAPI *wait_ex)(HANDLE, DWORD, BOOL) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "WaitForSingleObjectEx");
        DWORD w = wait_ex(ov->event ? ov->event : h, ms, alertable);
        if (w == 0xC0) { lsiw_set_last_error(0xC0); return FALSE; }        /* WAIT_IO_COMPLETION */
        if (w == WAIT_TIMEOUT) { lsiw_set_last_error(WAIT_TIMEOUT); return FALSE; }
        if (ov->internal == STATUS_PENDING_) { lsiw_set_last_error(996); return FALSE; }
    }
    if (n) *n = (DWORD)ov->internal_high;
    switch ((uint32_t)ov->internal) {
    case 0: return TRUE;
    case 0xC0000011: lsiw_set_last_error(ERROR_HANDLE_EOF); return FALSE;
    case 0xC000014B: lsiw_set_last_error(109); return FALSE;
    case 0xC0000120: lsiw_set_last_error(995 /* ERROR_OPERATION_ABORTED */); return FALSE;
    default: {
        ULONG (WINAPI *conv)(uint32_t) = lsiw_builtin_lookup(lsiw_builtin_find("ntdll.dll"), "RtlNtStatusToDosError");
        lsiw_set_last_error(conv ? conv((uint32_t)ov->internal) : 31 /* ERROR_GEN_FAILURE */);
        return FALSE; }
    }
}
API BOOL GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *n, BOOL wait)
{ return GetOverlappedResultEx(h, ov, n, wait ? INFINITE : 0, FALSE); }
/* Cancellation, locks and device controls are done by the NT layer
 * (ntdll_file.c); these wrappers convert NTSTATUS to Win32 errors. */
typedef struct { ULONG_PTR status; ULONG_PTR information; } NT_IOSB;
static void *ntfn(const char *name) { return lsiw_builtin_lookup(lsiw_builtin_find("ntdll.dll"), name); }
static BOOL nt_result(uint32_t st)
{
    if (st == 0) return TRUE;
    if (st == 0x103) { lsiw_set_last_error(997 /* ERROR_IO_PENDING */); return FALSE; }
    ULONG (WINAPI *conv)(uint32_t) = ntfn("RtlNtStatusToDosError");
    lsiw_set_last_error(conv ? conv(st) : 31);
    return FALSE;
}
API BOOL CancelIo(HANDLE h)
{
    uint32_t (WINAPI *f)(HANDLE, NT_IOSB *) = ntfn("NtCancelIoFile");
    NT_IOSB io = { 0, 0 };
    return f ? nt_result(f(h, &io)) : TRUE;
}
API BOOL CancelIoEx(HANDLE h, OVERLAPPED *ov)
{
    uint32_t (WINAPI *f)(HANDLE, void *, NT_IOSB *) = ntfn("NtCancelIoFileEx");
    NT_IOSB io = { 0, 0 };
    return f ? nt_result(f(h, ov, &io)) : TRUE;
}

API BOOL FlushFileBuffers(HANDLE h)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (fd <= 2) lsiw_crt_flush_all();
    if (fsync(fd) < 0 && errno != EINVAL && errno != EROFS) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

API DWORD GetFileType(HANDLE h)
{
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    lsiw_set_last_error(0);
    if (S_ISCHR(st.st_mode)) return 2;          /* FILE_TYPE_CHAR */
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return 3;  /* FILE_TYPE_PIPE */
    return 1;                                    /* FILE_TYPE_DISK */
}

API BOOL GetFileSizeEx(HANDLE h, int64_t *size)
{
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *size = st.st_size;
    return TRUE;
}

API DWORD GetFileSize(HANDLE h, DWORD *high)
{
    int64_t s;
    if (!GetFileSizeEx(h, &s)) return 0xFFFFFFFF;
    if (high) *high = (DWORD)((uint64_t)s >> 32);
    lsiw_set_last_error(0);
    return (DWORD)s;
}

API BOOL SetFilePointerEx(HANDLE h, int64_t dist, int64_t *newpos, DWORD method)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    int wh = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(fd, (off_t)dist, wh);
    if (r < 0) { lsiw_set_last_error(errno == EINVAL ? ERROR_NEGATIVE_SEEK : lsiw_errno_to_win(errno)); return FALSE; }
    if (newpos) *newpos = r;
    return TRUE;
}

API DWORD SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method)
{
    int64_t dist = hi ? (int64_t)(((uint64_t)(uint32_t)*hi << 32) | (uint32_t)lo) : (int64_t)lo;
    int64_t np;
    if (!SetFilePointerEx(h, dist, &np, method)) return 0xFFFFFFFF;
    if (hi) *hi = (LONG)(np >> 32);
    lsiw_set_last_error(0);
    return (DWORD)np;
}

API BOOL SetEndOfFile(HANDLE h)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0 || ftruncate(fd, pos) < 0) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

API BOOL SetFileValidData(HANDLE h, int64_t len) { int fd = fd_of(h); return fd >= 0 && ftruncate(fd, (off_t)len) == 0; }

API BOOL GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w)
{
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (c) *c = ts_to_ft(st.st_mtim.tv_sec < st.st_ctim.tv_sec ? st.st_mtim : st.st_ctim);
    if (a) *a = ts_to_ft(st.st_atim);
    if (w) *w = ts_to_ft(st.st_mtim);
    return TRUE;
}

API BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w)
{
    (void)c;
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
    if (a && (a->lo || a->hi)) ts[0] = ft_to_ts(a);
    if (w && (w->lo || w->hi)) ts[1] = ft_to_ts(w);
    if (futimens(fd, ts) < 0) { lsiw_set_errno_error(errno); return FALSE; }
    return TRUE;
}

struct by_handle_info {
    DWORD attrs; FILETIME create, access, write;
    DWORD serial, size_high, size_low, links, index_high, index_low;
};

API BOOL GetFileInformationByHandle(HANDLE h, struct by_handle_info *bi)
{
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    bi->attrs = attrs_of(o && o->path ? o->path : "x", &st);
    bi->create = ts_to_ft(st.st_ctim);
    bi->access = ts_to_ft(st.st_atim);
    bi->write = ts_to_ft(st.st_mtim);
    bi->serial = (DWORD)st.st_dev;
    bi->size_high = (DWORD)((uint64_t)st.st_size >> 32);
    bi->size_low = (DWORD)st.st_size;
    bi->links = (DWORD)st.st_nlink;
    bi->index_high = (DWORD)((uint64_t)st.st_ino >> 32);
    bi->index_low = (DWORD)st.st_ino;
    return TRUE;
}

/* GetFileInformationByHandleEx directory classes: enumerate the folder the
 * handle refers to, packing as many 8-byte aligned entries as fit. */
static BOOL dir_info_by_handle(struct lsiw_handle_obj *o, int cls, void *buf, DWORD size)
{
    if (!o || !o->path) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!o->dir) {
        int dfd = open(o->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0 || !(o->dir = fdopendir(dfd))) {
            if (dfd >= 0) close(dfd);
            lsiw_set_last_error(errno == ENOTDIR ? ERROR_INVALID_PARAMETER : lsiw_errno_to_win(errno));
            return FALSE;
        }
    }
    if (cls == 11 || cls == 15 || cls == 20) rewinddir(o->dir);
    DIR *d = o->dir;
    size_t name_off = (cls == 10 || cls == 11) ? 104 : (cls == 14 || cls == 15) ? 68 : 88;
    char *out = buf;
    size_t used = 0, prev = (size_t)-1;
    for (;;) {
        long pos = telldir(d);
        struct dirent *de = readdir(d);
        if (!de) break;
        char *full = lsiw_strdup_printf("%s/%s", o->path, de->d_name);
        struct stat st;
        if (lstat(full, &st) < 0) { free(full); continue; }
        WCHAR wname[260];
        int wl = (int)lsiw_utf8_to_utf16(de->d_name, -1, wname, 260);
        if (wl > 0 && !wname[wl - 1]) wl--;
        size_t need = name_off + (size_t)wl * 2;
        size_t start = (used + 7) & ~(size_t)7;
        if (start + need > size) {
            free(full);
            seekdir(d, pos);            /* hand this entry out next time */
            if (prev == (size_t)-1) { lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
            break;
        }
        char *e = out + start;
        memset(e, 0, name_off);
        FILETIME c = ts_to_ft(st.st_ctim), a = ts_to_ft(st.st_atim), w = ts_to_ft(st.st_mtim);
        int64_t *t = (int64_t *)(e + 8);
        t[0] = ((int64_t)c.hi << 32) | c.lo; t[1] = ((int64_t)a.hi << 32) | a.lo;
        t[2] = ((int64_t)w.hi << 32) | w.lo; t[3] = t[2];
        int nosize = S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode);
        t[4] = nosize ? 0 : st.st_size;
        t[5] = nosize ? 0 : (int64_t)st.st_blocks * 512;
        DWORD attrs = attrs_of(full, &st);
        free(full);
        *(DWORD *)(e + 56) = attrs;
        *(DWORD *)(e + 60) = (DWORD)wl * 2;
        DWORD tag = S_ISLNK(st.st_mode) ? IO_REPARSE_TAG_SYMLINK_ : 0;
        *(DWORD *)(e + 64) = tag;                       /* EaSize carries the tag for reparse points */
        if (cls == 10 || cls == 11) *(uint64_t *)(e + 96) = (uint64_t)st.st_ino;
        else if (cls == 19 || cls == 20) { *(DWORD *)(e + 68) = tag; *(uint64_t *)(e + 72) = (uint64_t)st.st_ino; }
        memcpy(e + name_off, wname, (size_t)wl * 2);
        if (prev != (size_t)-1) *(DWORD *)(out + prev) = (DWORD)(start - prev);
        prev = start;
        used = start + need;
    }
    if (prev == (size_t)-1) { lsiw_set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    *(DWORD *)(out + prev) = 0;
    return TRUE;
}

API BOOL GetFileInformationByHandleEx(HANDLE h, int cls, void *buf, DWORD size)
{
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    const char *path = o && o->path ? o->path : "x";
    switch (cls) {
    case 0: { /* FileBasicInfo */
        if (size < 40) break;
        int64_t *t = buf;
        FILETIME c = ts_to_ft(st.st_ctim), a = ts_to_ft(st.st_atim), w = ts_to_ft(st.st_mtim);
        t[0] = ((int64_t)c.hi << 32) | c.lo; t[1] = ((int64_t)a.hi << 32) | a.lo;
        t[2] = ((int64_t)w.hi << 32) | w.lo; t[3] = t[2];
        *(DWORD *)(t + 4) = attrs_of(path, &st);
        return TRUE; }
    case 1: { /* FileStandardInfo */
        if (size < 24) break;
        int64_t *t = buf;
        t[0] = (int64_t)st.st_blocks * 512; t[1] = st.st_size;
        *(DWORD *)(t + 2) = (DWORD)st.st_nlink;
        ((BYTE *)buf)[20] = 0; ((BYTE *)buf)[21] = S_ISDIR(st.st_mode);
        return TRUE; }
    case 2: { /* FileNameInfo: path without the drive */
        char *w = lsiw_linux_to_win(path);
        WCHAR *ws = lsiw_u2w(w + 2);
        DWORD bytes = (DWORD)lsiw_wcslen(ws) * 2;
        *(DWORD *)buf = bytes;
        if (size < 4 + bytes) { free(w); free(ws); lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
        memcpy((char *)buf + 4, ws, bytes);
        free(w); free(ws);
        return TRUE; }
    case 9: /* FileAttributeTagInfo */
        if (size < 8) break;
        ((DWORD *)buf)[0] = attrs_of(path, &st);
        ((DWORD *)buf)[1] = S_ISLNK(st.st_mode) ? IO_REPARSE_TAG_SYMLINK_ : 0;
        return TRUE;
    case 10: case 11:   /* FileIdBothDirectoryInfo (+Restart) */
    case 14: case 15:   /* FileFullDirectoryInfo (+Restart)   */
    case 19: case 20:   /* FileIdExtdDirectoryInfo (+Restart) */
        return dir_info_by_handle(o, cls, buf, size);
    case 18: /* FileIdInfo */
        if (size < 24) break;
        *(uint64_t *)buf = (uint64_t)st.st_dev;
        memset((char *)buf + 8, 0, 16);
        *(uint64_t *)((char *)buf + 8) = (uint64_t)st.st_ino;
        return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

API BOOL SetFileInformationByHandle(HANDLE h, int cls, void *buf, DWORD size)
{
    (void)size;
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    switch (cls) {
    case 0: { FILETIME *t = buf; return SetFileTime(h, &t[0], &t[1], &t[2]); }
    case 4: /* FileDispositionInfo: BOOLEAN DeleteFile */
    case 21: { /* FileDispositionInfoEx: DWORD Flags, bit 0 = delete */
        if (!(*(BYTE *)buf & 1)) return TRUE;
        if (!o->path) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
        /* Windows deletes when the last handle closes; deleting now is
         * equivalent for the program (the open handle stays usable). */
        struct stat ls;
        int r = lstat(o->path, &ls) == 0 && S_ISDIR(ls.st_mode) ? rmdir(o->path) : unlink(o->path);
        if (r < 0 && errno != ENOENT) {
            lsiw_set_last_error(errno == ENOTEMPTY || errno == EEXIST ? 145 /* ERROR_DIR_NOT_EMPTY */ : lsiw_errno_to_win(errno));
            return FALSE;
        }
        return TRUE; }
    case 6: /* FileEndOfFileInfo */
    case 5: /* FileAllocationInfo */
        if (ftruncate(o->fd, (off_t)*(int64_t *)buf) < 0) { lsiw_set_errno_error(errno); return FALSE; }
        return TRUE;
    case 3: { /* FileRenameInfo: BOOL replace; HANDLE root; DWORD len; WCHAR name[] */
        BYTE replace = *(BYTE *)buf;
        DWORD len = *(DWORD *)((char *)buf + 16);
        char *n = lsiw_w2u_n((WCHAR *)((char *)buf + 20), (int)(len / 2));
        char *dst = lsiw_win_to_linux(n);
        free(n);
        struct stat st;
        if (!replace && stat(dst, &st) == 0) { free(dst); lsiw_set_last_error(ERROR_ALREADY_EXISTS); return FALSE; }
        int r = o->path ? rename(o->path, dst) : -1;
        if (r == 0) { free(o->path); o->path = dst; return TRUE; }
        free(dst);
        lsiw_set_errno_error(errno);
        return FALSE; }
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

API BOOL LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE; }
API BOOL UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)h; (void)a; (void)b; (void)c; (void)d; return TRUE; }
API BOOL LockFileEx(HANDLE h, DWORD flags, DWORD r, DWORD lo, DWORD hi, OVERLAPPED *ov)
{
    (void)r;
    if (!ov) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uint32_t (WINAPI *f)(HANDLE, HANDLE, void *, void *, NT_IOSB *, int64_t *, int64_t *, ULONG, uint8_t, uint8_t) = ntfn("NtLockFile");
    if (!f) return TRUE;
    int64_t off = (int64_t)(((uint64_t)ov->offset_high << 32) | ov->offset);
    int64_t len = (int64_t)(((uint64_t)hi << 32) | lo);
    NT_IOSB *io = (NT_IOSB *)ov;
    return nt_result(f(h, ov->event, NULL, ov, io, &off, &len, 0,
                       (flags & 1 /* LOCKFILE_FAIL_IMMEDIATELY */) != 0, (flags & 2 /* LOCKFILE_EXCLUSIVE_LOCK */) != 0));
}
API BOOL UnlockFileEx(HANDLE h, DWORD r, DWORD lo, DWORD hi, OVERLAPPED *ov)
{
    (void)r;
    if (!ov) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uint32_t (WINAPI *f)(HANDLE, NT_IOSB *, int64_t *, int64_t *, ULONG) = ntfn("NtUnlockFile");
    if (!f) return TRUE;
    int64_t off = (int64_t)(((uint64_t)ov->offset_high << 32) | ov->offset);
    int64_t len = (int64_t)(((uint64_t)hi << 32) | lo);
    NT_IOSB io = { 0, 0 };
    return nt_result(f(h, &io, &off, &len, 0));
}
API BOOL DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD inl, void *out, DWORD outl, DWORD *ret, OVERLAPPED *ov)
{
    /* Device type 9 (FILE_DEVICE_FILE_SYSTEM) codes are FSCTLs. */
    uint32_t (WINAPI *f)(HANDLE, HANDLE, void *, void *, NT_IOSB *, ULONG, void *, ULONG, void *, ULONG) =
        ntfn((code >> 16) == 9 ? "NtFsControlFile" : "NtDeviceIoControlFile");
    if (ret) *ret = 0;
    if (!f) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
    NT_IOSB local = { 0, 0 }, *io = ov ? (NT_IOSB *)ov : &local;
    uint32_t st = f(h, ov ? ov->event : NULL, NULL, ov, io, code, in, inl, out, outl);
    if (st == 0x103 && !ov) {                 /* synchronous caller: wait for it */
        DWORD (WINAPI *w)(HANDLE, DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "WaitForSingleObject");
        while (__atomic_load_n(&local.status, __ATOMIC_ACQUIRE) == 0x103) w(h, 10);
        st = (uint32_t)local.status;
    }
    if (ret && st != 0x103) *ret = (DWORD)io->information;
    if (st == 0x80000005 /* STATUS_BUFFER_OVERFLOW */) { lsiw_set_last_error(234 /* ERROR_MORE_DATA */); return FALSE; }
    return nt_result(st);
}

API DWORD GetFinalPathNameByHandleA(HANDLE h, char *buf, DWORD n, DWORD flags)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    if (!o || !o->path) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    char *w = lsiw_linux_to_win(o->path);
    char *full = (flags & 0x4 /* VOLUME_NAME_NONE */) ? strdup(w + 2) : lsiw_strdup_printf("\\\\?\\%s", w);
    DWORD r = lsiw_ret_str_a(full, buf, n);
    free(w); free(full);
    return r;
}

API DWORD GetFinalPathNameByHandleW(HANDLE h, WCHAR *buf, DWORD n, DWORD flags)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    if (!o || !o->path) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    char *w = lsiw_linux_to_win(o->path);
    char *full = (flags & 0x4) ? strdup(w + 2) : lsiw_strdup_printf("\\\\?\\%s", w);
    DWORD r = lsiw_ret_str_w(full, buf, n);
    free(w); free(full);
    return r;
}

/* ================================================================== */
/* Attributes, directories, copy/move/delete                            */
/* ================================================================== */

static DWORD file_attributes(const char *win)
{
    if (device_of(win)) return FILE_ATTRIBUTE_ARCHIVE;
    char *p = lsiw_win_to_linux(win);
    struct stat st;
    if (stat(p, &st) < 0) { set_error_for(errno, p); free(p); return INVALID_FILE_ATTRIBUTES; }
    DWORD a = attrs_of(p, &st);
    free(p);
    return a;
}

API DWORD GetFileAttributesA(const char *n) { return n ? file_attributes(n) : INVALID_FILE_ATTRIBUTES; }
API DWORD GetFileAttributesW(const WCHAR *n) { char *s = lsiw_w2u(n); DWORD a = s ? file_attributes(s) : INVALID_FILE_ATTRIBUTES; free(s); return a; }

struct attr_data { DWORD attrs; FILETIME create, access, write; DWORD size_high, size_low; };

static BOOL file_attributes_ex(const char *win, struct attr_data *d)
{
    char *p = lsiw_win_to_linux(win);
    struct stat st;
    if (stat(p, &st) < 0) { set_error_for(errno, p); free(p); return FALSE; }
    d->attrs = attrs_of(p, &st);
    d->create = ts_to_ft(st.st_ctim);
    d->access = ts_to_ft(st.st_atim);
    d->write = ts_to_ft(st.st_mtim);
    d->size_high = S_ISDIR(st.st_mode) ? 0 : (DWORD)((uint64_t)st.st_size >> 32);
    d->size_low = S_ISDIR(st.st_mode) ? 0 : (DWORD)st.st_size;
    free(p);
    return TRUE;
}

API BOOL GetFileAttributesExA(const char *n, int lvl, struct attr_data *d) { (void)lvl; return file_attributes_ex(n, d); }
API BOOL GetFileAttributesExW(const WCHAR *n, int lvl, struct attr_data *d) { (void)lvl; char *s = lsiw_w2u(n); BOOL r = file_attributes_ex(s, d); free(s); return r; }

static BOOL set_attributes(const char *win, DWORD attrs)
{
    char *p = lsiw_win_to_linux(win);
    struct stat st;
    if (stat(p, &st) < 0) { set_error_for(errno, p); free(p); return FALSE; }
    mode_t m = st.st_mode & 07777;
    if (attrs & FILE_ATTRIBUTE_READONLY) m &= ~(mode_t)0222; else m |= S_IWUSR;
    chmod(p, m);
    free(p);
    return TRUE;
}
API BOOL SetFileAttributesA(const char *n, DWORD a) { return set_attributes(n, a); }
API BOOL SetFileAttributesW(const WCHAR *n, DWORD a) { char *s = lsiw_w2u(n); BOOL r = set_attributes(s, a); free(s); return r; }

static BOOL create_directory(const char *win)
{
    char *p = lsiw_win_to_linux(win);
    int r = mkdir(p, 0777);
    if (r < 0) {
        if (errno == EEXIST) lsiw_set_last_error(ERROR_ALREADY_EXISTS);
        else set_error_for(errno, p);
    }
    free(p);
    return r == 0;
}
API BOOL CreateDirectoryA(const char *n, void *sa) { (void)sa; return create_directory(n); }
API BOOL CreateDirectoryW(const WCHAR *n, void *sa) { (void)sa; char *s = lsiw_w2u(n); BOOL r = create_directory(s); free(s); return r; }
API BOOL CreateDirectoryExW(const WCHAR *t, const WCHAR *n, void *sa) { (void)t; return CreateDirectoryW(n, sa); }

static BOOL remove_directory(const char *win)
{
    char *p = lsiw_win_to_linux(win);
    int r = rmdir(p);
    if (r < 0) set_error_for(errno == ENOTDIR ? ENOTDIR : errno, p);
    free(p);
    return r == 0;
}
API BOOL RemoveDirectoryA(const char *n) { return remove_directory(n); }
API BOOL RemoveDirectoryW(const WCHAR *n) { char *s = lsiw_w2u(n); BOOL r = remove_directory(s); free(s); return r; }

static BOOL delete_file(const char *win)
{
    char *p = lsiw_win_to_linux(win);
    struct stat st;
    if (lstat(p, &st) == 0 && (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && !(st.st_mode & S_IWUSR)))) {
        free(p); lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE;   /* directories and read-only files */
    }
    int r = unlink(p);
    if (r < 0) set_error_for(errno, p);
    free(p);
    return r == 0;
}
API BOOL DeleteFileA(const char *n) { return delete_file(n); }
API BOOL DeleteFileW(const WCHAR *n) { char *s = lsiw_w2u(n); BOOL r = delete_file(s); free(s); return r; }

static int copy_fd(int in, int out)
{
    char buf[65536];
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t o = 0;
        while (o < r) { ssize_t w = write(out, buf + o, (size_t)(r - o)); if (w < 0) return -1; o += w; }
    }
    return r < 0 ? -1 : 0;
}

static BOOL copy_file(const char *src_w, const char *dst_w, BOOL fail_if_exists)
{
    char *src = lsiw_win_to_linux(src_w), *dst = lsiw_win_to_linux(dst_w);
    BOOL ok = FALSE;
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) { set_error_for(errno, src); goto out; }
    struct stat st;
    fstat(in, &st);
    int out = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) { lsiw_set_last_error(errno == EEXIST ? ERROR_FILE_EXISTS : lsiw_errno_to_win(errno)); close(in); goto out; }
    if (copy_fd(in, out) == 0) {
        struct timespec ts[2] = { st.st_atim, st.st_mtim };
        futimens(out, ts);
        ok = TRUE;
    } else lsiw_set_errno_error(errno);
    close(in); close(out);
out:
    free(src); free(dst);
    return ok;
}
API BOOL CopyFileA(const char *s, const char *d, BOOL f) { return copy_file(s, d, f); }
API BOOL CopyFileW(const WCHAR *s, const WCHAR *d, BOOL f) { char *a = lsiw_w2u(s), *b = lsiw_w2u(d); BOOL r = copy_file(a, b, f); free(a); free(b); return r; }
API BOOL CopyFileExW(const WCHAR *s, const WCHAR *d, void *cb, void *data, BOOL *cancel, DWORD flags)
{ (void)cb; (void)data; (void)cancel; return CopyFileW(s, d, (flags & 1) != 0); }
API BOOL CopyFileExA(const char *s, const char *d, void *cb, void *data, BOOL *cancel, DWORD flags)
{ (void)cb; (void)data; (void)cancel; return copy_file(s, d, (flags & 1) != 0); }

static BOOL move_file(const char *src_w, const char *dst_w, DWORD flags)
{
    char *src = lsiw_win_to_linux(src_w);
    if (!dst_w) {   /* MOVEFILE_DELAY_UNTIL_REBOOT with NULL target = delete */
        int r = unlink(src); free(src);
        return r == 0 || flags & 4;
    }
    char *dst = lsiw_win_to_linux(dst_w);
    struct stat st;
    BOOL ok = FALSE;
    if (!(flags & 1 /* REPLACE_EXISTING */) && lstat(dst, &st) == 0) { lsiw_set_last_error(ERROR_ALREADY_EXISTS); goto out; }
    if (rename(src, dst) == 0) ok = TRUE;
    else if (errno == EXDEV && (flags & 2 /* COPY_ALLOWED */ || 1)) {
        char *ws = lsiw_linux_to_win(src), *wd = lsiw_linux_to_win(dst);
        if (copy_file(ws, wd, !(flags & 1))) { unlink(src); ok = TRUE; }
        free(ws); free(wd);
    } else set_error_for(errno, src);
out:
    free(src); free(dst);
    return ok;
}
API BOOL MoveFileA(const char *s, const char *d) { return move_file(s, d, 2); }
API BOOL MoveFileW(const WCHAR *s, const WCHAR *d) { char *a = lsiw_w2u(s), *b = lsiw_w2u(d); BOOL r = move_file(a, b, 2); free(a); free(b); return r; }
API BOOL MoveFileExA(const char *s, const char *d, DWORD f) { return move_file(s, d, f); }
API BOOL MoveFileExW(const WCHAR *s, const WCHAR *d, DWORD f) { char *a = lsiw_w2u(s), *b = lsiw_w2u(d); BOOL r = move_file(a, b, f); free(a); free(b); return r; }
API BOOL MoveFileWithProgressW(const WCHAR *s, const WCHAR *d, void *cb, void *data, DWORD f) { (void)cb; (void)data; return MoveFileExW(s, d, f); }
API BOOL ReplaceFileW(const WCHAR *repl, const WCHAR *with, const WCHAR *backup, DWORD f, void *a, void *b)
{
    (void)f; (void)a; (void)b;
    if (backup) CopyFileW(repl, backup, FALSE);
    return MoveFileExW(with, repl, 1);
}

API BOOL CreateHardLinkA(const char *n, const char *e, void *sa)
{
    (void)sa;
    char *a = lsiw_win_to_linux(e), *b = lsiw_win_to_linux(n);
    int r = link(a, b);
    if (r < 0) lsiw_set_errno_error(errno);
    free(a); free(b);
    return r == 0;
}
API BOOL CreateHardLinkW(const WCHAR *n, const WCHAR *e, void *sa) { char *a = lsiw_w2u(n), *b = lsiw_w2u(e); BOOL r = CreateHardLinkA(a, b, sa); free(a); free(b); return r; }
API BOOL CreateSymbolicLinkA(const char *link_w, const char *target_w, DWORD flags)
{
    (void)flags;
    char *l = lsiw_win_to_linux(link_w);
    /* Relative targets stay relative; absolute ones are translated. */
    char *t = (target_w[0] && target_w[1] == ':') || target_w[0] == '\\' ? lsiw_win_to_linux(target_w) : strdup(target_w);
    for (char *c = t; *c; c++) if (*c == '\\') *c = '/';
    int r = symlink(t, l);
    if (r < 0) lsiw_set_errno_error(errno);
    free(l); free(t);
    return r == 0;
}
API BOOL CreateSymbolicLinkW(const WCHAR *l, const WCHAR *t, DWORD f) { char *a = lsiw_w2u(l), *b = lsiw_w2u(t); BOOL r = CreateSymbolicLinkA(a, b, f); free(a); free(b); return r; }

/* ================================================================== */
/* FindFirstFile / FindNextFile                                         */
/* ================================================================== */

#pragma pack(push, 4)
struct find_data_a { DWORD attrs; FILETIME create, access, write; DWORD size_high, size_low, r0, r1; char name[260]; char alt[14]; };
struct find_data_w { DWORD attrs; FILETIME create, access, write; DWORD size_high, size_low, r0, r1; WCHAR name[260]; WCHAR alt[14]; };
#pragma pack(pop)

/* Windows wildcard match, case-insensitive. "*.*" also matches names without a dot. */
static int wild_match(const char *pat, const char *name)
{
    if (!strcmp(pat, "*.*") || !strcmp(pat, "*")) return 1;
    return fnmatch(pat, name, FNM_CASEFOLD | FNM_PERIOD * 0) == 0;
}

static int find_next(struct lsiw_handle_obj *o, char *name_out, struct stat *st_out)
{
    struct dirent *de;
    while ((de = readdir(o->dir))) {
        if (!wild_match(o->pattern, de->d_name)) continue;
        char *full = lsiw_strdup_printf("%s/%s", o->dir_path, de->d_name);
        struct stat st;
        int ok = lstat(full, st_out ? st_out : &st) == 0;
        free(full);
        if (!ok) continue;
        snprintf(name_out, 260, "%s", de->d_name);
        return 1;
    }
    return 0;
}

static void fill_find(struct lsiw_handle_obj *o, const char *name, const struct stat *st,
                      DWORD *attrs, FILETIME *c, FILETIME *a, FILETIME *w, DWORD *sh, DWORD *sl, DWORD *r0)
{
    char *full = lsiw_strdup_printf("%s/%s", o->dir_path, name);
    *attrs = attrs_of(full, st);
    free(full);
    *c = ts_to_ft(st->st_ctim);
    *a = ts_to_ft(st->st_atim);
    *w = ts_to_ft(st->st_mtim);
    int nosize = S_ISDIR(st->st_mode) || S_ISLNK(st->st_mode);
    *sh = nosize ? 0 : (DWORD)((uint64_t)st->st_size >> 32);
    *sl = nosize ? 0 : (DWORD)st->st_size;
    *r0 = S_ISLNK(st->st_mode) ? IO_REPARSE_TAG_SYMLINK_ : 0;
}

static HANDLE find_first(const char *win, char *name, struct stat *st)
{
    char *spec = strdup(win);
    for (char *c = spec; *c; c++) if (*c == '/') *c = '\\';
    char *sep = strrchr(spec, '\\');
    char *pat, *dirw;
    if (sep) { *sep = 0; pat = strdup(sep + 1); dirw = lsiw_strdup_printf("%s\\", spec); }
    else if (spec[0] && spec[1] == ':') { pat = strdup(spec + 2); dirw = strdup("C:\\"); }
    else { pat = strdup(spec); dirw = strdup("."); }
    free(spec);
    if (!*pat) { free(pat); pat = strdup("*"); }
    char *dir = lsiw_win_to_linux(dirw);
    free(dirw);
    DIR *d = opendir(dir);
    if (!d) {
        lsiw_set_last_error(errno == ENOTDIR ? ERROR_DIRECTORY : ERROR_PATH_NOT_FOUND);
        free(dir); free(pat);
        return INVALID_HANDLE_VALUE;
    }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_FIND, &o);
    o->dir = d;
    o->dir_path = dir;
    o->pattern = pat;
    if (!find_next(o, name, st)) {
        lsiw_handle_close(h);
        lsiw_set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

API HANDLE FindFirstFileA(const char *spec, struct find_data_a *fd)
{
    char name[260]; struct stat st;
    HANDLE h = find_first(spec, name, &st);
    if (h == INVALID_HANDLE_VALUE) return h;
    memset(fd, 0, sizeof *fd);
    fill_find(lsiw_handle_get(h, H_FIND), name, &st, &fd->attrs, &fd->create, &fd->access, &fd->write, &fd->size_high, &fd->size_low, &fd->r0);
    snprintf(fd->name, sizeof fd->name, "%s", name);
    return h;
}

API HANDLE FindFirstFileW(const WCHAR *spec, struct find_data_w *fd)
{
    char *s = lsiw_w2u(spec);
    char name[260]; struct stat st;
    HANDLE h = find_first(s, name, &st);
    free(s);
    if (h == INVALID_HANDLE_VALUE) return h;
    memset(fd, 0, sizeof *fd);
    fill_find(lsiw_handle_get(h, H_FIND), name, &st, &fd->attrs, &fd->create, &fd->access, &fd->write, &fd->size_high, &fd->size_low, &fd->r0);
    lsiw_utf8_to_utf16(name, -1, fd->name, 260);
    return h;
}

API HANDLE FindFirstFileExA(const char *spec, int lvl, struct find_data_a *fd, int op, void *f, DWORD fl)
{ (void)lvl; (void)op; (void)f; (void)fl; return FindFirstFileA(spec, fd); }
API HANDLE FindFirstFileExW(const WCHAR *spec, int lvl, struct find_data_w *fd, int op, void *f, DWORD fl)
{ (void)lvl; (void)op; (void)f; (void)fl; return FindFirstFileW(spec, fd); }

API BOOL FindNextFileA(HANDLE h, struct find_data_a *fd)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FIND);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    char name[260]; struct stat st;
    if (!find_next(o, name, &st)) { lsiw_set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    memset(fd, 0, sizeof *fd);
    fill_find(o, name, &st, &fd->attrs, &fd->create, &fd->access, &fd->write, &fd->size_high, &fd->size_low, &fd->r0);
    snprintf(fd->name, sizeof fd->name, "%s", name);
    return TRUE;
}

API BOOL FindNextFileW(HANDLE h, struct find_data_w *fd)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FIND);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    char name[260]; struct stat st;
    if (!find_next(o, name, &st)) { lsiw_set_last_error(ERROR_NO_MORE_FILES); return FALSE; }
    memset(fd, 0, sizeof *fd);
    fill_find(o, name, &st, &fd->attrs, &fd->create, &fd->access, &fd->write, &fd->size_high, &fd->size_low, &fd->r0);
    lsiw_utf8_to_utf16(name, -1, fd->name, 260);
    return TRUE;
}

API BOOL FindClose(HANDLE h)
{
    if (!lsiw_handle_get(h, H_FIND)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_handle_close(h);
    return TRUE;
}

API HANDLE FindFirstChangeNotificationW(const WCHAR *p, BOOL sub, DWORD f) { (void)p; (void)sub; (void)f; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE; }
API BOOL FindNextChangeNotification(HANDLE h) { (void)h; return FALSE; }
API BOOL FindCloseChangeNotification(HANDLE h) { (void)h; return TRUE; }

/* ================================================================== */
/* Current directory and path names                                     */
/* ================================================================== */

/* The current directory as the program last named it, kept while the
 * process stays in that folder: Windows reports the path that was set, not
 * a reverse-mapped one (which could differ through links or relocations). */
static char *g_cwd_name;
static dev_t g_cwd_dev;
static ino_t g_cwd_ino;
static pthread_mutex_t g_cwd_lock = PTHREAD_MUTEX_INITIALIZER;

static void remember_cwd(const char *win)
{
    struct stat st;
    pthread_mutex_lock(&g_cwd_lock);
    free(g_cwd_name);
    g_cwd_name = NULL;
    if (win && stat(".", &st) == 0) {
        g_cwd_name = strdup(win);
        size_t l = strlen(g_cwd_name);
        while (l > 3 && g_cwd_name[l - 1] == '\\') g_cwd_name[--l] = 0;
        g_cwd_dev = st.st_dev; g_cwd_ino = st.st_ino;
    }
    pthread_mutex_unlock(&g_cwd_lock);
}

static char *cwd_win(void)
{
    static int init;
    if (!init) {
        init = 1;
        const char *given = getenv("LSIW_CWD");   /* lpCurrentDirectory from a parent LSIW process */
        if (given) { remember_cwd(given); unsetenv("LSIW_CWD"); }
    }
    struct stat st;
    char *r = NULL;
    pthread_mutex_lock(&g_cwd_lock);
    if (g_cwd_name && stat(".", &st) == 0 && st.st_dev == g_cwd_dev && st.st_ino == g_cwd_ino) r = strdup(g_cwd_name);
    pthread_mutex_unlock(&g_cwd_lock);
    if (r) return r;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    return lsiw_linux_to_win(cwd);
}

API DWORD GetCurrentDirectoryA(DWORD n, char *buf) { char *w = cwd_win(); DWORD r = lsiw_ret_str_a(w, buf, n); free(w); return r; }
API DWORD GetCurrentDirectoryW(DWORD n, WCHAR *buf) { char *w = cwd_win(); DWORD r = lsiw_ret_str_w(w, buf, n); free(w); return r; }

static char *full_path(const char *in);

static BOOL set_cwd(const char *win)
{
    char *f = full_path(win);       /* before chdir: relative to the old folder */
    char *p = lsiw_win_to_linux(win);
    int r = chdir(p);
    if (r < 0) set_error_for(errno, p);
    free(p);
    if (r == 0) remember_cwd(f);
    free(f);
    return r == 0;
}
API BOOL SetCurrentDirectoryA(const char *p) { return set_cwd(p); }
API BOOL SetCurrentDirectoryW(const WCHAR *p) { char *s = lsiw_w2u(p); BOOL r = set_cwd(s); free(s); return r; }

/* Lexical full-path computation, like the real GetFullPathName. */
static char *full_path(const char *in)
{
    char *base;
    char *s = strdup(in);
    for (char *c = s; *c; c++) if (*c == '/') *c = '\\';
    if (s[0] == '\\' && s[1] == '\\') return s;                        /* UNC: leave as is */
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        if (s[2] == '\\') base = lsiw_strdup_printf("C:%s", s + 2);
        else { char *c = cwd_win(); base = lsiw_strdup_printf("%s\\%s", c, s + 2); free(c); }
    } else if (s[0] == '\\') base = lsiw_strdup_printf("C:%s", s);
    else { char *c = cwd_win(); base = lsiw_strdup_printf("%s\\%s", c, s); free(c); }
    free(s);
    /* Normalise components. */
    char *comps[1024]; int n = 0;
    int trailing = strlen(base) > 3 && base[strlen(base) - 1] == '\\';
    char *save = NULL;
    for (char *c = strtok_r(base + 2, "\\", &save); c; c = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (n) n--; continue; }
        if (n < 1024) comps[n++] = c;
    }
    size_t cap = strlen(in) + 4096, len = 0;
    char *out = malloc(cap);
    out[len++] = 'C'; out[len++] = ':'; out[len++] = '\\';
    for (int i = 0; i < n; i++) {
        size_t cl = strlen(comps[i]);
        if (len + cl + 2 > cap) { cap = (len + cl) * 2; out = realloc(out, cap); }
        memcpy(out + len, comps[i], cl); len += cl;
        if (i + 1 < n || trailing) out[len++] = '\\';
    }
    out[len] = 0;
    free(base);
    return out;
}

API DWORD GetFullPathNameA(const char *in, DWORD n, char *buf, char **file_part)
{
    if (!in || !*in) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
    char *f = full_path(in);
    DWORD r = lsiw_ret_str_a(f, buf, n);
    if (r < n && file_part) {
        char *s = strrchr(buf, '\\');
        *file_part = (s && s[1]) ? s + 1 : NULL;
    }
    free(f);
    return r;
}

API DWORD GetFullPathNameW(const WCHAR *in, DWORD n, WCHAR *buf, WCHAR **file_part)
{
    if (!in || !*in) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
    char *s = lsiw_w2u(in);
    char *f = full_path(s);
    free(s);
    DWORD r = lsiw_ret_str_w(f, buf, n);
    if (r < n && file_part) {
        WCHAR *last = NULL;
        for (WCHAR *p = buf; *p; p++) if (*p == '\\') last = p;
        *file_part = (last && last[1]) ? last + 1 : NULL;
    }
    free(f);
    return r;
}

static DWORD long_path_a(const char *in, char *out, DWORD n)
{
    char *p = lsiw_win_to_linux(in);
    struct stat st;
    int ok = stat(p, &st) == 0;
    if (!ok) set_error_for(errno, p);
    free(p);
    if (!ok) return 0;
    return lsiw_ret_str_a(in, out, n);
}
API DWORD GetLongPathNameA(const char *in, char *out, DWORD n) { return long_path_a(in, out, n); }
API DWORD GetShortPathNameA(const char *in, char *out, DWORD n) { return long_path_a(in, out, n); }
API DWORD GetLongPathNameW(const WCHAR *in, WCHAR *out, DWORD n)
{
    char *s = lsiw_w2u(in);
    char *p = lsiw_win_to_linux(s);
    struct stat st;
    DWORD r = 0;
    if (stat(p, &st) == 0) r = lsiw_ret_str_w(s, out, n); else set_error_for(errno, p);
    free(s); free(p);
    return r;
}
API DWORD GetShortPathNameW(const WCHAR *in, WCHAR *out, DWORD n) { return GetLongPathNameW(in, out, n); }

static char *temp_dir_win(void)
{
    const char *t = lsiw_env_get("TEMP");
    if (!t) t = lsiw_var_windows("TEMP");
    if (!t) t = "C:\\tmp";
    char *lin = lsiw_win_to_linux(t);
    lsiw_mkdirs(lin, 0700);
    free(lin);
    size_t l = strlen(t);
    return lsiw_strdup_printf("%s%s", t, (l && t[l - 1] == '\\') ? "" : "\\");
}
API DWORD GetTempPathA(DWORD n, char *buf) { char *t = temp_dir_win(); DWORD r = lsiw_ret_str_a(t, buf, n); free(t); return r; }
API DWORD GetTempPathW(DWORD n, WCHAR *buf) { char *t = temp_dir_win(); DWORD r = lsiw_ret_str_w(t, buf, n); free(t); return r; }
API DWORD GetTempPath2A(DWORD n, char *buf) { return GetTempPathA(n, buf); }
API DWORD GetTempPath2W(DWORD n, WCHAR *buf) { return GetTempPathW(n, buf); }

static UINT temp_file_name(const char *dir, const char *prefix, UINT unique, char *out)
{
    static unsigned counter;
    UINT u = unique ? unique : (UINT)((getpid() << 4) ^ (unsigned)time(NULL) ^ ++counter) & 0xFFFF;
    for (int tries = 0; tries < 65536; tries++) {
        size_t dl = strlen(dir);
        snprintf(out, MAX_PATH, "%s%s%.3s%X.tmp", dir, (dl && dir[dl - 1] == '\\') ? "" : "\\", prefix ? prefix : "", u);
        if (unique) return u;
        char *p = lsiw_win_to_linux(out);
        int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600);
        free(p);
        if (fd >= 0) { close(fd); return u; }
        if (errno != EEXIST) { lsiw_set_errno_error(errno); return 0; }
        u = (u + 1) & 0xFFFF; if (!u) u = 1;
    }
    return 0;
}
API UINT GetTempFileNameA(const char *dir, const char *prefix, UINT unique, char *out) { return temp_file_name(dir, prefix, unique, out); }
API UINT GetTempFileNameW(const WCHAR *dir, const WCHAR *prefix, UINT unique, WCHAR *out)
{
    char *d = lsiw_w2u(dir), *p = lsiw_w2u(prefix), buf[MAX_PATH];
    UINT r = temp_file_name(d, p, unique, buf);
    if (r) lsiw_utf8_to_utf16(buf, -1, out, MAX_PATH);
    free(d); free(p);
    return r;
}

static DWORD search_path(const char *path, const char *file, const char *ext, char *out, DWORD n, char **fp)
{
    char *name = (ext && !strchr(file, '.')) ? lsiw_strdup_printf("%s%s", file, ext) : strdup(file);
    char *found = NULL;
    struct stat st;
    if (strchr(name, '\\') || strchr(name, '/')) {
        char *l = lsiw_win_to_linux(name);
        if (stat(l, &st) == 0) found = full_path(name);
        free(l);
    } else {
        char *list;
        if (path) list = strdup(path);
        else {
            char *c = cwd_win();
            const char *exe = lsiw_main_module ? lsiw_main_module->win_path : NULL;
            char *exedir = exe ? strdup(exe) : strdup("C:\\");
            char *s = strrchr(exedir, '\\'); if (s) *s = 0;
            const char *p = lsiw_env_get("PATH");
            list = lsiw_strdup_printf("%s;%s;%s;%s;%s", exedir, c, lsiw_var_windows("SYSTEM32"), lsiw_var_windows("WINDIR"), p ? p : "");
            free(c); free(exedir);
        }
        char *save = NULL;
        for (char *d = strtok_r(list, ";", &save); d && !found; d = strtok_r(NULL, ";", &save)) {
            if (!*d) continue;
            char *cand = lsiw_strdup_printf("%s\\%s", d, name);
            char *l = lsiw_win_to_linux(cand);
            if (stat(l, &st) == 0 && S_ISREG(st.st_mode)) found = full_path(cand);
            free(l); free(cand);
        }
        free(list);
    }
    free(name);
    if (!found) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    DWORD r = lsiw_ret_str_a(found, out, n);
    if (r < n && fp) { char *s = strrchr(out, '\\'); *fp = s ? s + 1 : out; }
    free(found);
    return r;
}
API DWORD SearchPathA(const char *path, const char *file, const char *ext, DWORD n, char *out, char **fp) { return search_path(path, file, ext, out, n, fp); }
API DWORD SearchPathW(const WCHAR *path, const WCHAR *file, const WCHAR *ext, DWORD n, WCHAR *out, WCHAR **fp)
{
    char *p = lsiw_w2u(path), *f = lsiw_w2u(file), *e = lsiw_w2u(ext), buf[4096];
    DWORD r = search_path(p, f, e, buf, sizeof buf, NULL);
    free(p); free(f); free(e);
    if (!r) return 0;
    r = lsiw_ret_str_w(buf, out, n);
    if (r < n && fp) { WCHAR *last = out; for (WCHAR *c = out; *c; c++) if (*c == '\\') last = c + 1; *fp = last; }
    return r;
}

/* ================================================================== */
/* Drives and volumes: there is exactly one drive, C:                   */
/* ================================================================== */

static int is_c_root(const char *root)
{
    if (!root) return 1;
    if ((root[0] == 'C' || root[0] == 'c') && root[1] == ':' && (!root[2] || ((root[2] == '\\' || root[2] == '/') && !root[3]))) return 1;
    return 0;
}

API UINT GetDriveTypeA(const char *root)
{
    if (is_c_root(root)) return 3;                  /* DRIVE_FIXED */
    if (root && root[0] == '\\' && root[1] == '\\') return 4;  /* DRIVE_REMOTE */
    return 1;                                       /* DRIVE_NO_ROOT_DIR */
}
API UINT GetDriveTypeW(const WCHAR *root) { char *s = lsiw_w2u(root); UINT r = GetDriveTypeA(s); free(s); return r; }
API DWORD GetLogicalDrives(void) { return 1u << 2; }
API DWORD GetLogicalDriveStringsA(DWORD n, char *buf) { if (n < 5) return 5; memcpy(buf, "C:\\\0\0", 5); return 4; }
API DWORD GetLogicalDriveStringsW(DWORD n, WCHAR *buf) { if (n < 5) return 5; buf[0] = 'C'; buf[1] = ':'; buf[2] = '\\'; buf[3] = 0; buf[4] = 0; return 4; }

static BOOL volume_info(const char *root, char *label, DWORD ln, DWORD *serial, DWORD *maxlen, DWORD *flags, char *fs, DWORD fsn)
{
    if (!is_c_root(root)) { lsiw_set_last_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    struct statvfs sv;
    statvfs("/", &sv);
    if (label && ln) snprintf(label, ln, "LSIW");
    if (serial) *serial = (DWORD)(sv.f_fsid ^ (sv.f_fsid >> 32));
    if (maxlen) *maxlen = 255;
    if (flags) *flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x00400000; /* case sensitive/preserved, unicode, ACLs, hard links */
    if (fs && fsn) snprintf(fs, fsn, "NTFS");
    return TRUE;
}
API BOOL GetVolumeInformationA(const char *root, char *label, DWORD ln, DWORD *serial, DWORD *maxlen, DWORD *flags, char *fs, DWORD fsn)
{ return volume_info(root, label, ln, serial, maxlen, flags, fs, fsn); }
/* The C: drive is the Linux filesystem: its label is not LSIW's to change. */
API BOOL SetVolumeLabelA(const char *root, const char *label) { (void)root; (void)label; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
API BOOL SetVolumeLabelW(const WCHAR *root, const WCHAR *label) { (void)root; (void)label; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
API BOOL GetVolumeInformationW(const WCHAR *root, WCHAR *label, DWORD ln, DWORD *serial, DWORD *maxlen, DWORD *flags, WCHAR *fs, DWORD fsn)
{
    char *r = lsiw_w2u(root), l[64], f[64];
    BOOL ok = volume_info(r, l, sizeof l, serial, maxlen, flags, f, sizeof f);
    free(r);
    if (ok && label && ln) lsiw_utf8_to_utf16(l, -1, label, (int)ln);
    if (ok && fs && fsn) lsiw_utf8_to_utf16(f, -1, fs, (int)fsn);
    return ok;
}
API BOOL GetVolumeInformationByHandleW(HANDLE h, WCHAR *label, DWORD ln, DWORD *serial, DWORD *maxlen, DWORD *flags, WCHAR *fs, DWORD fsn)
{ (void)h; return GetVolumeInformationW(NULL, label, ln, serial, maxlen, flags, fs, fsn); }
API BOOL GetVolumePathNameA(const char *p, char *out, DWORD n) { (void)p; if (n < 4) return FALSE; strcpy(out, "C:\\"); return TRUE; }
API BOOL GetVolumePathNameW(const WCHAR *p, WCHAR *out, DWORD n) { (void)p; if (n < 4) return FALSE; out[0] = 'C'; out[1] = ':'; out[2] = '\\'; out[3] = 0; return TRUE; }
API BOOL GetVolumeNameForVolumeMountPointW(const WCHAR *m, WCHAR *out, DWORD n)
{
    (void)m;
    return lsiw_ret_str_w("\\\\?\\Volume{4c534957-0000-0000-0000-000000000001}\\", out, n) < n;
}

static BOOL disk_free(const char *win, uint64_t *avail, uint64_t *total, uint64_t *freeb)
{
    char *p = win ? lsiw_win_to_linux(win) : strdup("/");
    struct statvfs sv;
    int r = statvfs(p, &sv);
    free(p);
    if (r < 0) { lsiw_set_errno_error(errno); return FALSE; }
    if (avail) *avail = (uint64_t)sv.f_bavail * sv.f_frsize;
    if (total) *total = (uint64_t)sv.f_blocks * sv.f_frsize;
    if (freeb) *freeb = (uint64_t)sv.f_bfree * sv.f_frsize;
    return TRUE;
}
API BOOL GetDiskFreeSpaceExA(const char *d, uint64_t *a, uint64_t *t, uint64_t *f) { return disk_free(d, a, t, f); }
API BOOL GetDiskFreeSpaceExW(const WCHAR *d, uint64_t *a, uint64_t *t, uint64_t *f) { char *s = lsiw_w2u(d); BOOL r = disk_free(s, a, t, f); free(s); return r; }
API BOOL GetDiskFreeSpaceA(const char *d, DWORD *spc, DWORD *bps, DWORD *fc, DWORD *tc)
{
    uint64_t a, t, f;
    if (!disk_free(d, &a, &t, &f)) return FALSE;
    if (spc) *spc = 8;
    if (bps) *bps = 512;
    if (fc) *fc = (DWORD)(a / 4096 > 0xFFFFFFFF ? 0xFFFFFFFF : a / 4096);
    if (tc) *tc = (DWORD)(t / 4096 > 0xFFFFFFFF ? 0xFFFFFFFF : t / 4096);
    return TRUE;
}
API BOOL GetDiskFreeSpaceW(const WCHAR *d, DWORD *spc, DWORD *bps, DWORD *fc, DWORD *tc) { char *s = lsiw_w2u(d); BOOL r = GetDiskFreeSpaceA(s, spc, bps, fc, tc); free(s); return r; }
API DWORD QueryDosDeviceW(const WCHAR *dev, WCHAR *out, DWORD n) { (void)dev; DWORD r = lsiw_ret_str_w("\\Device\\HarddiskVolume1", out, n); if (r < n && r + 1 < n) out[r + 1] = 0; return r + 2; }
API BOOL DefineDosDeviceW(DWORD f, const WCHAR *d, const WCHAR *t) { (void)f; (void)d; (void)t; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

/* ================================================================== */
/* File mappings                                                        */
/* ================================================================== */

struct view { void *addr; size_t len; struct view *next; };
static struct view *g_views;
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;

static HANDLE create_mapping(HANDLE file, DWORD protect, uint64_t size, const char *name)
{
    (void)name;
    int fd;
    if (file == INVALID_HANDLE_VALUE || !file) {
        fd = memfd_create("lsiw-mapping", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)size) < 0) { lsiw_set_errno_error(errno); if (fd >= 0) close(fd); return NULL; }
    } else {
        int f = fd_of(file);
        if (f < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
        fd = fcntl(f, F_DUPFD_CLOEXEC, 0);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) < 0) { lsiw_set_errno_error(errno); if (fd >= 0) close(fd); return NULL; }
        /* A writable mapping larger than the file extends the file, as on Windows. */
        if (size > (uint64_t)st.st_size && (protect & (0x04 | 0x40)) && ftruncate(fd, (off_t)size) < 0) {
            lsiw_set_errno_error(errno); close(fd); return NULL;
        }
        if (!size) size = (uint64_t)st.st_size;
    }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_MAPPING, &o);
    o->fd = fd;
    o->map_size = size;
    o->map_protect = protect;
    lsiw_set_last_error(0);
    return h;
}

API HANDLE CreateFileMappingA(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const char *name)
{ (void)sa; return create_mapping(f, prot, ((uint64_t)hi << 32) | lo, name); }
API HANDLE CreateFileMappingW(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const WCHAR *name)
{ (void)sa; (void)name; return create_mapping(f, prot, ((uint64_t)hi << 32) | lo, NULL); }
API HANDLE CreateFileMappingFromApp(HANDLE f, void *sa, ULONG prot, ULONGLONG size, const WCHAR *name)
{ (void)sa; (void)name; return create_mapping(f, prot, size, NULL); }
API HANDLE OpenFileMappingW(DWORD a, BOOL i, const WCHAR *n) { (void)a; (void)i; (void)n; lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
API HANDLE OpenFileMappingA(DWORD a, BOOL i, const char *n) { (void)a; (void)i; (void)n; lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }

static void *map_view(HANDLE h, DWORD access, uint64_t off, SIZE_T size, void *base)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_MAPPING);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    if (!size) size = (SIZE_T)(o->map_size - off);
    int prot = PROT_READ;
    if (access & 0x2 /* FILE_MAP_WRITE */) prot |= PROT_WRITE;
    if (access & 0x20 /* FILE_MAP_EXECUTE */) prot |= PROT_EXEC;
    int flags = (access & 0x1 /* FILE_MAP_COPY */) ? MAP_PRIVATE : MAP_SHARED;
    if (access & 0x1) prot |= PROT_WRITE;
    void *p = mmap(base, size, prot, flags | (base ? MAP_FIXED_NOREPLACE : 0), o->fd, (off_t)off);
    if (p == MAP_FAILED) { lsiw_set_errno_error(errno); return NULL; }
    struct view *v = malloc(sizeof *v);
    v->addr = p; v->len = size;
    pthread_mutex_lock(&g_view_lock);
    v->next = g_views; g_views = v;
    pthread_mutex_unlock(&g_view_lock);
    return p;
}
API void *MapViewOfFile(HANDLE h, DWORD a, DWORD hi, DWORD lo, SIZE_T n) { return map_view(h, a, ((uint64_t)hi << 32) | lo, n, NULL); }
API void *MapViewOfFileEx(HANDLE h, DWORD a, DWORD hi, DWORD lo, SIZE_T n, void *base) { return map_view(h, a, ((uint64_t)hi << 32) | lo, n, base); }
API void *MapViewOfFileFromApp(HANDLE h, ULONG a, ULONGLONG off, SIZE_T n) { return map_view(h, a, off, n, NULL); }
API BOOL UnmapViewOfFile(const void *addr)
{
    pthread_mutex_lock(&g_view_lock);
    for (struct view **pp = &g_views; *pp; pp = &(*pp)->next) {
        if ((*pp)->addr == addr) {
            struct view *v = *pp; *pp = v->next;
            pthread_mutex_unlock(&g_view_lock);
            munmap(v->addr, v->len);
            free(v);
            return TRUE;
        }
    }
    pthread_mutex_unlock(&g_view_lock);
    lsiw_set_last_error(487 /* ERROR_INVALID_ADDRESS */);
    return FALSE;
}
API BOOL UnmapViewOfFileEx(const void *addr, ULONG f) { (void)f; return UnmapViewOfFile(addr); }
API BOOL FlushViewOfFile(const void *addr, SIZE_T n)
{
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)0xfff;
    return msync((void *)a, n + ((uintptr_t)addr - a), MS_SYNC) == 0;
}

/* ================================================================== */
/* Console                                                              */
/* ================================================================== */

API HANDLE GetStdHandle(DWORD which)
{
    int i = which == (DWORD)-10 ? 0 : which == (DWORD)-11 ? 1 : which == (DWORD)-12 ? 2 : -1;
    if (i < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    return lsiw_std_handle(i);
}

API BOOL SetStdHandle(DWORD which, HANDLE h)
{
    int i = which == (DWORD)-10 ? 0 : which == (DWORD)-11 ? 1 : which == (DWORD)-12 ? 2 : -1;
    if (i < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    lsiw_std_handle_set(i, h);
    return TRUE;
}

static int console_fd(HANDLE h)
{
    int fd = fd_of(h);
    if (fd < 0 || !isatty(fd)) return -1;
    return fd;
}

static DWORD g_out_mode = 0x1 | 0x2 | 0x4;  /* processed, wrap, virtual-terminal */

API BOOL GetConsoleMode(HANDLE h, DWORD *mode)
{
    int fd = console_fd(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *mode = fd == 0 ? g_in_mode : g_out_mode;
    return TRUE;
}

API BOOL SetConsoleMode(HANDLE h, DWORD mode)
{
    int fd = console_fd(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (fd != 0) { g_out_mode = mode; return TRUE; }
    g_in_mode = mode;
    struct termios t;
    if (tcgetattr(0, &t) == 0) {
        if (!g_tio_saved) { g_orig_tio = t; g_tio_saved = 1; atexit(restore_tty); }
        if (mode & 0x2) t.c_lflag |= ICANON; else t.c_lflag &= ~(tcflag_t)ICANON;
        if (mode & 0x4) t.c_lflag |= ECHO; else t.c_lflag &= ~(tcflag_t)ECHO;
        if (mode & 0x1) t.c_lflag |= ISIG; else t.c_lflag &= ~(tcflag_t)ISIG;
        t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    }
    return TRUE;
}

static void write_all(int fd, const char *s, size_t n)
{
    while (n) { ssize_t w = write(fd, s, n); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; } s += w; n -= (size_t)w; }
}

API BOOL WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *res)
{
    (void)res;
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_crt_flush_all();
    write_all(fd, buf, n);
    if (written) *written = n;
    return TRUE;
}

API BOOL WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *res)
{
    (void)res;
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    lsiw_crt_flush_all();
    char *u = lsiw_w2u_n(buf, (int)n);
    write_all(fd, u, strlen(u));
    free(u);
    if (written) *written = n;
    return TRUE;
}

API BOOL ReadConsoleA(HANDLE h, void *buf, DWORD n, DWORD *nread, void *ctl)
{
    (void)ctl;
    return ReadFile(h, buf, n, nread, NULL);
}

API BOOL ReadConsoleW(HANDLE h, WCHAR *buf, DWORD n, DWORD *nread, void *ctl)
{
    (void)ctl;
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    char tmp[4096];
    size_t cap = n < sizeof tmp ? n : sizeof tmp - 1;
    ssize_t r;
    do r = read(fd, tmp, cap); while (r < 0 && errno == EINTR);
    if (r < 0) { lsiw_set_errno_error(errno); return FALSE; }
    /* Do not split a UTF-8 sequence across reads. */
    while (r > 0 && ((unsigned char)tmp[r - 1] & 0xC0) == 0x80) {
        ssize_t k = r - 1;
        while (k > 0 && ((unsigned char)tmp[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)tmp[k];
        int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (r - k >= need) break;
        ssize_t more = read(fd, tmp + r, (size_t)(need - (r - k)));
        if (more <= 0) break;
        r += more;
    }
    int w = lsiw_utf8_to_utf16(tmp, (int)r, buf, (int)n);
    if (w < 0) w = (int)n;
    if (nread) *nread = (DWORD)w;
    return TRUE;
}

#pragma pack(push, 4)
struct key_event { BOOL down; WORD repeat, vk, scan; WCHAR ch; DWORD ctrl; };
struct input_record { WORD type; WORD pad; union { struct key_event key; BYTE raw[16]; } ev; };
#pragma pack(pop)

static int g_pending = -1;

static int read_key(int fd, int block)
{
    if (g_pending >= 0) { int c = g_pending; if (block) g_pending = -1; return c; }
    struct pollfd p = { fd, POLLIN, 0 };
    if (!block && poll(&p, 1, 0) <= 0) return -1;
    unsigned char c;
    if (read(fd, &c, 1) != 1) return -1;
    if (!block) g_pending = c;
    return c;
}

static void fill_key(struct input_record *r, int c)
{
    memset(r, 0, sizeof *r);
    r->type = 1; /* KEY_EVENT */
    r->ev.key.down = TRUE;
    r->ev.key.repeat = 1;
    if (c == '\n') c = '\r';
    r->ev.key.ch = (WCHAR)c;
    r->ev.key.vk = (WORD)(c >= 'a' && c <= 'z' ? c - 32 : c == '\r' ? 0x0D : c == 27 ? 0x1B : c == 127 ? 0x08 : c);
}

static BOOL console_input(HANDLE h, struct input_record *recs, DWORD n, DWORD *nread, int peek)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!n) { if (nread) *nread = 0; return TRUE; }
    struct termios t, saved;
    int raw = isatty(fd) && tcgetattr(fd, &t) == 0;
    if (raw) { saved = t; t.c_lflag &= ~(tcflag_t)(ICANON | ECHO); t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; tcsetattr(fd, TCSANOW, &t); }
    int c = read_key(fd, !peek);
    if (raw) tcsetattr(fd, TCSANOW, &saved);
    if (c < 0) { if (nread) *nread = 0; return TRUE; }
    fill_key(&recs[0], c);
    if (nread) *nread = 1;
    return TRUE;
}

API BOOL ReadConsoleInputA(HANDLE h, struct input_record *r, DWORD n, DWORD *nr) { return console_input(h, r, n, nr, 0); }
API BOOL ReadConsoleInputW(HANDLE h, struct input_record *r, DWORD n, DWORD *nr) { return console_input(h, r, n, nr, 0); }
API BOOL PeekConsoleInputA(HANDLE h, struct input_record *r, DWORD n, DWORD *nr) { return console_input(h, r, n, nr, 1); }
API BOOL PeekConsoleInputW(HANDLE h, struct input_record *r, DWORD n, DWORD *nr) { return console_input(h, r, n, nr, 1); }
API BOOL GetNumberOfConsoleInputEvents(HANDLE h, DWORD *n)
{
    int fd = fd_of(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    struct pollfd p = { fd, POLLIN, 0 };
    *n = (g_pending >= 0 || poll(&p, 1, 0) > 0) ? 1 : 0;
    return TRUE;
}
API BOOL FlushConsoleInputBuffer(HANDLE h) { int fd = fd_of(h); g_pending = -1; if (fd >= 0 && isatty(fd)) tcflush(fd, TCIFLUSH); return TRUE; }
API BOOL WriteConsoleInputW(HANDLE h, const void *r, DWORD n, DWORD *w) { (void)h; (void)r; if (w) *w = n; return TRUE; }

/* Colours: Windows attribute bits (BGRI) -> ANSI SGR. */
static WORD g_attr = 0x07;
static const int ansi_of_win[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

API BOOL SetConsoleTextAttribute(HANDLE h, WORD attr)
{
    int fd = console_fd(h);
    g_attr = attr;
    if (fd < 0) return TRUE;
    char seq[48];
    int fg = ansi_of_win[attr & 7], bg = ansi_of_win[(attr >> 4) & 7];
    int n = snprintf(seq, sizeof seq, "\x1b[0;%d;%dm", (attr & 8) ? 90 + fg : 30 + fg, (attr & 0x80) ? 100 + bg : 40 + bg);
    if ((attr & 0xF0) == 0) n = snprintf(seq, sizeof seq, "\x1b[0;%dm", (attr & 8) ? 90 + fg : 30 + fg);
    if (attr == 0x07) n = snprintf(seq, sizeof seq, "\x1b[0m");
    lsiw_crt_flush_all();
    write_all(fd, seq, (size_t)n);
    return TRUE;
}

#pragma pack(push, 2)
typedef struct { short x, y; } COORD;
typedef struct { short left, top, right, bottom; } SMALL_RECT;
struct csbi { COORD size; COORD cursor; WORD attr; SMALL_RECT window; COORD max; };
#pragma pack(pop)

static void term_size(int fd, int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col) { *cols = ws.ws_col; *rows = ws.ws_row; }
    else { *cols = 80; *rows = 25; }
}

API BOOL GetConsoleScreenBufferInfo(HANDLE h, struct csbi *info)
{
    int fd = console_fd(h);
    if (fd < 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    int c, r;
    term_size(fd, &c, &r);
    info->size = (COORD){ (short)c, (short)r };
    info->cursor = (COORD){ 0, 0 };
    info->attr = g_attr;
    info->window = (SMALL_RECT){ 0, 0, (short)(c - 1), (short)(r - 1) };
    info->max = (COORD){ (short)c, (short)r };
    return TRUE;
}

struct csbiex { ULONG cb; COORD size; COORD cursor; WORD attr; SMALL_RECT window; COORD max; WORD popup; BOOL fullscreen; DWORD table[16]; };
API BOOL GetConsoleScreenBufferInfoEx(HANDLE h, struct csbiex *info)
{
    struct csbi b;
    if (!GetConsoleScreenBufferInfo(h, &b)) return FALSE;
    info->size = b.size; info->cursor = b.cursor; info->attr = b.attr; info->window = b.window; info->max = b.max;
    info->popup = 0; info->fullscreen = FALSE;
    static const DWORD pal[16] = { 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                   0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };
    memcpy(info->table, pal, sizeof pal);
    return TRUE;
}
API BOOL SetConsoleScreenBufferInfoEx(HANDLE h, void *info) { (void)h; (void)info; return TRUE; }

API BOOL SetConsoleCursorPosition(HANDLE h, COORD pos)
{
    int fd = console_fd(h);
    if (fd < 0) return TRUE;
    char seq[32];
    int n = snprintf(seq, sizeof seq, "\x1b[%d;%dH", pos.y + 1, pos.x + 1);
    lsiw_crt_flush_all();
    write_all(fd, seq, (size_t)n);
    return TRUE;
}

static BOOL fill_output(HANDLE h, DWORD len, COORD at, DWORD *written, int is_char, int ch)
{
    int fd = console_fd(h);
    if (written) *written = len;
    if (fd < 0) return TRUE;
    int c, r;
    term_size(fd, &c, &r);
    lsiw_crt_flush_all();
    if (at.x == 0 && at.y == 0 && len >= (DWORD)(c * r)) {
        if (is_char) write_all(fd, "\x1b[2J\x1b[3J\x1b[H", 11);   /* the classic "cls" */
        return TRUE;
    }
    if (!is_char) return TRUE;
    char seq[32];
    int n = snprintf(seq, sizeof seq, "\x1b[s\x1b[%d;%dH", at.y + 1, at.x + 1);
    write_all(fd, seq, (size_t)n);
    for (DWORD i = 0; i < len; i++) { char cc = (char)ch; write_all(fd, &cc, 1); }
    write_all(fd, "\x1b[u", 3);
    return TRUE;
}
API BOOL FillConsoleOutputCharacterA(HANDLE h, char ch, DWORD len, COORD at, DWORD *w) { return fill_output(h, len, at, w, 1, (unsigned char)ch); }
API BOOL FillConsoleOutputCharacterW(HANDLE h, WCHAR ch, DWORD len, COORD at, DWORD *w) { return fill_output(h, len, at, w, 1, ch < 0x80 ? ch : '?'); }
API BOOL FillConsoleOutputAttribute(HANDLE h, WORD a, DWORD len, COORD at, DWORD *w) { (void)a; return fill_output(h, len, at, w, 0, 0); }
API BOOL ScrollConsoleScreenBufferA(HANDLE h, const SMALL_RECT *s, const SMALL_RECT *c, COORD d, const void *f) { (void)h; (void)s; (void)c; (void)d; (void)f; return TRUE; }
API BOOL ScrollConsoleScreenBufferW(HANDLE h, const SMALL_RECT *s, const SMALL_RECT *c, COORD d, const void *f) { (void)h; (void)s; (void)c; (void)d; (void)f; return TRUE; }

struct char_info { WCHAR ch; WORD attr; };
API BOOL WriteConsoleOutputW(HANDLE h, const struct char_info *buf, COORD size, COORD from, SMALL_RECT *rect)
{
    int fd = console_fd(h);
    if (fd < 0) return TRUE;
    lsiw_crt_flush_all();
    for (int y = rect->top; y <= rect->bottom; y++) {
        char seq[32];
        int n = snprintf(seq, sizeof seq, "\x1b[%d;%dH", y + 1, rect->left + 1);
        write_all(fd, seq, (size_t)n);
        for (int x = rect->left; x <= rect->right; x++) {
            const struct char_info *ci = &buf[(y - rect->top + from.y) * size.x + (x - rect->left + from.x)];
            WCHAR w[2] = { ci->ch ? ci->ch : ' ', 0 };
            char *u = lsiw_w2u(w);
            write_all(fd, u, strlen(u));
            free(u);
        }
    }
    return TRUE;
}
API BOOL WriteConsoleOutputA(HANDLE h, const struct char_info *buf, COORD size, COORD from, SMALL_RECT *rect) { return WriteConsoleOutputW(h, buf, size, from, rect); }
API BOOL WriteConsoleOutputCharacterA(HANDLE h, const char *s, DWORD n, COORD at, DWORD *w)
{
    int fd = console_fd(h);
    if (w) *w = n;
    if (fd < 0) return TRUE;
    char seq[32];
    int k = snprintf(seq, sizeof seq, "\x1b[%d;%dH", at.y + 1, at.x + 1);
    lsiw_crt_flush_all();
    write_all(fd, seq, (size_t)k);
    write_all(fd, s, n);
    return TRUE;
}
API BOOL WriteConsoleOutputCharacterW(HANDLE h, const WCHAR *s, DWORD n, COORD at, DWORD *w)
{
    char *u = lsiw_w2u_n(s, (int)n);
    BOOL r = WriteConsoleOutputCharacterA(h, u, (DWORD)strlen(u), at, NULL);
    free(u);
    if (w) *w = n;
    return r;
}

struct cursor_info { DWORD size; BOOL visible; };
static BOOL g_cursor_visible = TRUE;
API BOOL GetConsoleCursorInfo(HANDLE h, struct cursor_info *ci) { (void)h; ci->size = 25; ci->visible = g_cursor_visible; return TRUE; }
API BOOL SetConsoleCursorInfo(HANDLE h, const struct cursor_info *ci)
{
    int fd = console_fd(h);
    g_cursor_visible = ci->visible;
    if (fd >= 0) { lsiw_crt_flush_all(); write_all(fd, ci->visible ? "\x1b[?25h" : "\x1b[?25l", 6); }
    return TRUE;
}

static char g_title[1024] = "";
API BOOL SetConsoleTitleA(const char *t)
{
    snprintf(g_title, sizeof g_title, "%s", t ? t : "");
    if (isatty(1)) { char seq[1100]; int n = snprintf(seq, sizeof seq, "\x1b]0;%s\a", g_title); lsiw_crt_flush_all(); write_all(1, seq, (size_t)n); }
    return TRUE;
}
API BOOL SetConsoleTitleW(const WCHAR *t) { char *u = lsiw_w2u(t); BOOL r = SetConsoleTitleA(u); free(u); return r; }
API DWORD GetConsoleTitleA(char *b, DWORD n) { return lsiw_ret_str_a(g_title, b, n); }
API DWORD GetConsoleTitleW(WCHAR *b, DWORD n) { return lsiw_ret_str_w(g_title, b, n); }
API UINT GetConsoleCP(void) { return 65001; }
API UINT GetConsoleOutputCP(void) { return 65001; }
API BOOL SetConsoleCP(UINT cp) { (void)cp; return TRUE; }
API BOOL SetConsoleOutputCP(UINT cp) { (void)cp; return TRUE; }
API void *GetConsoleWindow(void) { return NULL; }
API BOOL AllocConsole(void) { return TRUE; }
API BOOL FreeConsole(void) { return TRUE; }
API BOOL AttachConsole(DWORD pid) { (void)pid; return TRUE; }
API BOOL SetConsoleScreenBufferSize(HANDLE h, COORD c) { (void)h; (void)c; return TRUE; }
API BOOL SetConsoleWindowInfo(HANDLE h, BOOL a, const SMALL_RECT *r) { (void)h; (void)a; (void)r; return TRUE; }
API COORD GetLargestConsoleWindowSize(HANDLE h) { int c, r; term_size(fd_of(h) >= 0 ? fd_of(h) : 1, &c, &r); return (COORD){ (short)c, (short)r }; }
API HANDLE CreateConsoleScreenBuffer(DWORD a, DWORD s, void *sa, DWORD f, void *d) { (void)a; (void)s; (void)sa; (void)f; (void)d; return lsiw_handle_from_fd(dup(1), isatty(1)); }
API BOOL SetConsoleActiveScreenBuffer(HANDLE h) { (void)h; return TRUE; }
API DWORD GetConsoleProcessList(DWORD *l, DWORD n) { if (n) l[0] = (DWORD)getpid(); return 1; }
API BOOL SetCurrentConsoleFontEx(HANDLE h, BOOL m, void *f) { (void)h; (void)m; (void)f; return TRUE; }
API BOOL GetCurrentConsoleFontEx(HANDLE h, BOOL m, void *f) { (void)h; (void)m; (void)f; return FALSE; }
API BOOL GetConsoleDisplayMode(DWORD *m) { *m = 0; return TRUE; }

/* Ctrl+C: handlers run on a helper thread, as on Windows. */
typedef BOOL (WINAPI *ctrl_handler)(DWORD);
static ctrl_handler g_handlers[32];
static int g_nhandlers, g_ignore_ctrl_c;
static int g_ctrl_pipe[2] = { -1, -1 };

static void on_sigint(int sig)
{
    char c = sig == SIGINT ? 0 : 2;
    if (g_ctrl_pipe[1] >= 0) { ssize_t r = write(g_ctrl_pipe[1], &c, 1); (void)r; }
}

static void *ctrl_thread(void *arg)
{
    (void)arg;
    lsiw_teb_create();
    char c;
    while (read(g_ctrl_pipe[0], &c, 1) == 1) {
        if (c == 0 && g_ignore_ctrl_c) continue;
        int handled = 0;
        for (int i = g_nhandlers - 1; i >= 0 && !handled; i--) handled = g_handlers[i]((DWORD)c);
        if (!handled) lsiw_exit_process(0xC000013A);
    }
    return NULL;
}

API BOOL SetConsoleCtrlHandler(ctrl_handler h, BOOL add)
{
    if (!h) { g_ignore_ctrl_c = add; return TRUE; }
    if (add) {
        if (g_nhandlers >= 32) return FALSE;
        g_handlers[g_nhandlers++] = h;
        if (g_ctrl_pipe[0] < 0 && pipe2(g_ctrl_pipe, O_CLOEXEC) == 0) {
            pthread_t t;
            pthread_create(&t, NULL, ctrl_thread, NULL);
            pthread_detach(t);
            struct sigaction sa = { 0 };
            sa.sa_handler = on_sigint;
            sigaction(SIGINT, &sa, NULL);
            sigaction(SIGTERM, &sa, NULL);
            sigaction(SIGHUP, &sa, NULL);
        }
    } else {
        for (int i = 0; i < g_nhandlers; i++)
            if (g_handlers[i] == h) { g_handlers[i] = g_handlers[--g_nhandlers]; break; }
    }
    return TRUE;
}
API BOOL GenerateConsoleCtrlEvent(DWORD ev, DWORD group) { (void)group; on_sigint(ev == 0 ? SIGINT : SIGTERM); return TRUE; }

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_k32file_exports[] = {
    E(CreateFileA), E(CreateFileW), E(CreateFile2), E(ReadFile), E(WriteFile), E(GetOverlappedResult), E(GetOverlappedResultEx),
    E(CancelIo), E(CancelIoEx), E(FlushFileBuffers), E(GetFileType), E(GetFileSize), E(GetFileSizeEx),
    E(SetFilePointer), E(SetFilePointerEx), E(SetEndOfFile), E(SetFileValidData), E(GetFileTime), E(SetFileTime),
    E(GetFileInformationByHandle), E(GetFileInformationByHandleEx), E(SetFileInformationByHandle),
    E(LockFile), E(UnlockFile), E(LockFileEx), E(UnlockFileEx), E(DeviceIoControl),
    E(GetFinalPathNameByHandleA), E(GetFinalPathNameByHandleW),
    E(GetFileAttributesA), E(GetFileAttributesW), E(GetFileAttributesExA), E(GetFileAttributesExW),
    E(SetFileAttributesA), E(SetFileAttributesW), E(CreateDirectoryA), E(CreateDirectoryW), E(CreateDirectoryExW),
    E(RemoveDirectoryA), E(RemoveDirectoryW), E(DeleteFileA), E(DeleteFileW), E(CopyFileA), E(CopyFileW),
    E(CopyFileExA), E(CopyFileExW), E(MoveFileA), E(MoveFileW), E(MoveFileExA), E(MoveFileExW), E(MoveFileWithProgressW),
    E(ReplaceFileW), E(CreateHardLinkA), E(CreateHardLinkW), E(CreateSymbolicLinkA), E(CreateSymbolicLinkW),
    E(FindFirstFileA), E(FindFirstFileW), E(FindFirstFileExA), E(FindFirstFileExW), E(FindNextFileA), E(FindNextFileW), E(FindClose),
    E(FindFirstChangeNotificationW), E(FindNextChangeNotification), E(FindCloseChangeNotification),
    E(GetCurrentDirectoryA), E(GetCurrentDirectoryW), E(SetCurrentDirectoryA), E(SetCurrentDirectoryW),
    E(GetFullPathNameA), E(GetFullPathNameW), E(GetLongPathNameA), E(GetLongPathNameW), E(GetShortPathNameA), E(GetShortPathNameW),
    E(GetTempPathA), E(GetTempPathW), E(GetTempPath2A), E(GetTempPath2W), E(GetTempFileNameA), E(GetTempFileNameW),
    E(SearchPathA), E(SearchPathW),
    E(GetDriveTypeA), E(GetDriveTypeW), E(GetLogicalDrives), E(GetLogicalDriveStringsA), E(GetLogicalDriveStringsW),
    E(GetVolumeInformationA), E(GetVolumeInformationW), E(SetVolumeLabelA), E(SetVolumeLabelW), E(GetVolumeInformationByHandleW), E(GetVolumePathNameA), E(GetVolumePathNameW),
    E(GetVolumeNameForVolumeMountPointW), E(GetDiskFreeSpaceA), E(GetDiskFreeSpaceW), E(GetDiskFreeSpaceExA), E(GetDiskFreeSpaceExW),
    E(QueryDosDeviceW), E(DefineDosDeviceW),
    E(CreateFileMappingA), E(CreateFileMappingW), E(CreateFileMappingFromApp), E(OpenFileMappingA), E(OpenFileMappingW),
    E(MapViewOfFile), E(MapViewOfFileEx), E(MapViewOfFileFromApp), E(UnmapViewOfFile), E(UnmapViewOfFileEx), E(FlushViewOfFile),
    E(GetStdHandle), E(SetStdHandle), E(GetConsoleMode), E(SetConsoleMode), E(WriteConsoleA), E(WriteConsoleW),
    E(ReadConsoleA), E(ReadConsoleW), E(ReadConsoleInputA), E(ReadConsoleInputW), E(PeekConsoleInputA), E(PeekConsoleInputW),
    E(GetNumberOfConsoleInputEvents), E(FlushConsoleInputBuffer), E(WriteConsoleInputW),
    E(SetConsoleTextAttribute), E(GetConsoleScreenBufferInfo), E(GetConsoleScreenBufferInfoEx), E(SetConsoleScreenBufferInfoEx),
    E(SetConsoleCursorPosition), E(FillConsoleOutputCharacterA), E(FillConsoleOutputCharacterW), E(FillConsoleOutputAttribute),
    E(ScrollConsoleScreenBufferA), E(ScrollConsoleScreenBufferW), E(WriteConsoleOutputA), E(WriteConsoleOutputW),
    E(WriteConsoleOutputCharacterA), E(WriteConsoleOutputCharacterW),
    E(GetConsoleCursorInfo), E(SetConsoleCursorInfo), E(SetConsoleTitleA), E(SetConsoleTitleW), E(GetConsoleTitleA), E(GetConsoleTitleW),
    E(GetConsoleCP), E(GetConsoleOutputCP), E(SetConsoleCP), E(SetConsoleOutputCP), E(GetConsoleWindow), E(AllocConsole),
    E(FreeConsole), E(AttachConsole), E(SetConsoleScreenBufferSize), E(SetConsoleWindowInfo), E(GetLargestConsoleWindowSize),
    E(CreateConsoleScreenBuffer), E(SetConsoleActiveScreenBuffer), E(GetConsoleProcessList), E(SetCurrentConsoleFontEx),
    E(GetCurrentConsoleFontEx), E(GetConsoleDisplayMode), E(SetConsoleCtrlHandler), E(GenerateConsoleCtrlEvent),
    { NULL, NULL }
};
