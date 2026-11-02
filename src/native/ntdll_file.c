/* ntdll: the NT file-information layer.
 *
 * NtQueryInformationFile / NtSetInformationFile, NtQueryVolumeInformationFile,
 * NtQueryDirectoryFile(Ex), NtDeviceIoControlFile / NtFsControlFile (reparse
 * points, sparse ranges, pipe peek, the AFD socket poll used by libuv and
 * wepoll), cancellation, byte-range locks, EAs, quotas, attribute queries by
 * name, NtCreateNamedPipeFile and the Rtl DOS->NT path helpers.
 *
 * Handles are LSIW H_FILE objects: o->fd is the Linux descriptor and o->path
 * the Linux path. Every call completes synchronously except IOCTL_AFD_POLL,
 * which runs on a worker thread and completes through the IO_STATUS_BLOCK,
 * the event, an APC or the handle's completion port, as on Windows.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sockios.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* NTSTATUS values                                                      */
/* ------------------------------------------------------------------ */
#define ST(v) ((NTSTATUS)(v))
#define STATUS_SUCCESS                 ST(0x00000000u)
#define STATUS_TIMEOUT                 ST(0x00000102u)
#define STATUS_PENDING                 ST(0x00000103u)
#define STATUS_BUFFER_OVERFLOW         ST(0x80000005u)
#define STATUS_NO_MORE_FILES           ST(0x80000006u)
#define STATUS_NO_MORE_ENTRIES         ST(0x8000001Au)
#define STATUS_UNSUCCESSFUL            ST(0xC0000001u)
#define STATUS_INFO_LENGTH_MISMATCH    ST(0xC0000004u)
#define STATUS_INVALID_INFO_CLASS      ST(0xC0000003u)
#define STATUS_INVALID_HANDLE          ST(0xC0000008u)
#define STATUS_INVALID_PARAMETER       ST(0xC000000Du)
#define STATUS_NO_SUCH_FILE            ST(0xC000000Fu)
#define STATUS_INVALID_DEVICE_REQUEST  ST(0xC0000010u)
#define STATUS_NO_MEMORY               ST(0xC0000017u)
#define STATUS_ACCESS_DENIED           ST(0xC0000022u)
#define STATUS_BUFFER_TOO_SMALL        ST(0xC0000023u)
#define STATUS_OBJECT_TYPE_MISMATCH    ST(0xC0000024u)
#define STATUS_OBJECT_NAME_INVALID     ST(0xC0000033u)
#define STATUS_OBJECT_NAME_NOT_FOUND   ST(0xC0000034u)
#define STATUS_OBJECT_NAME_COLLISION   ST(0xC0000035u)
#define STATUS_OBJECT_PATH_NOT_FOUND   ST(0xC000003Au)
#define STATUS_EAS_NOT_SUPPORTED       ST(0xC000004Fu)
#define STATUS_NO_EAS_ON_FILE          ST(0xC0000052u)
#define STATUS_LOCK_NOT_GRANTED        ST(0xC0000054u)
#define STATUS_DELETE_PENDING          ST(0xC0000056u)
#define STATUS_PIPE_BROKEN             ST(0xC000014Bu)
#define STATUS_FILE_IS_A_DIRECTORY     ST(0xC00000BAu)
#define STATUS_NOT_SUPPORTED           ST(0xC00000BBu)
#define STATUS_DIRECTORY_NOT_EMPTY     ST(0xC0000101u)
#define STATUS_NOT_A_DIRECTORY         ST(0xC0000103u)
#define STATUS_CANCELLED               ST(0xC0000120u)
#define STATUS_CANNOT_DELETE           ST(0xC0000121u)
#define STATUS_NOT_FOUND               ST(0xC0000225u)
#define STATUS_CONNECTION_REFUSED      ST(0xC0000236u)
#define STATUS_CONNECTION_RESET        ST(0xC000020Du)
#define STATUS_NOT_A_REPARSE_POINT     ST(0xC0000275u)
#define STATUS_IO_REPARSE_TAG_MISMATCH ST(0xC0000277u)
#define STATUS_IO_REPARSE_DATA_INVALID ST(0xC0000278u)
#define STATUS_NOT_SAME_DEVICE         ST(0xC00000D4u)
#define STATUS_NAME_TOO_LONG           ST(0xC0000106u)

#define NT_ERROR(s) ((uint32_t)(s) >= 0xC0000000u)

/* FILE_INFORMATION_CLASS */
enum {
    FileDirectoryInformation = 1, FileFullDirectoryInformation = 2, FileBothDirectoryInformation = 3,
    FileBasicInformation = 4, FileStandardInformation = 5, FileInternalInformation = 6,
    FileEaInformation = 7, FileAccessInformation = 8, FileNameInformation = 9,
    FileRenameInformation = 10, FileLinkInformation = 11, FileNamesInformation = 12,
    FileDispositionInformation = 13, FilePositionInformation = 14, FileModeInformation = 16,
    FileAlignmentInformation = 17, FileAllInformation = 18, FileAllocationInformation = 19,
    FileEndOfFileInformation = 20, FileAlternateNameInformation = 21, FileStreamInformation = 22,
    FilePipeInformation = 23, FilePipeLocalInformation = 24, FileCompressionInformation = 28,
    FileCompletionInformation = 30, FileNetworkOpenInformation = 34, FileAttributeTagInformation = 35,
    FileIdBothDirectoryInformation = 37, FileIdFullDirectoryInformation = 38,
    FileValidDataLengthInformation = 39, FileIoCompletionNotificationInformation = 41,
    FileIoPriorityHintInformation = 43, FileNormalizedNameInformation = 48,
    FileIsRemoteDeviceInformation = 51, FileIdInformation = 59, FileIdExtdDirectoryInformation = 60,
    FileIdExtdBothDirectoryInformation = 63, FileDispositionInformationEx = 64,
    FileRenameInformationEx = 65, FileStatInformation = 68, FileStatLxInformation = 70,
    FileCaseSensitiveInformation = 71, FileLinkInformationEx = 72
};

#define FILE_ATTRIBUTE_READONLY      0x01
#define FILE_ATTRIBUTE_HIDDEN        0x02
#define FILE_ATTRIBUTE_DIRECTORY     0x10
#define FILE_ATTRIBUTE_ARCHIVE       0x20
#define FILE_ATTRIBUTE_NORMAL        0x80
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define IO_REPARSE_TAG_SYMLINK       0xA000000Cu
#define IO_REPARSE_TAG_MOUNT_POINT   0xA0000003u

#define DELETE_ACCESS          0x00010000u
#define READ_CONTROL           0x00020000u
#define SYNCHRONIZE            0x00100000u
#define FILE_READ_DATA         0x0001u
#define FILE_WRITE_DATA        0x0002u
#define FILE_READ_ATTRIBUTES   0x0080u
#define FILE_WRITE_ATTRIBUTES  0x0100u
#define FILE_GENERIC_READ      0x00120089u
#define FILE_GENERIC_WRITE     0x00120116u

#define EPOCH_DIFF 116444736000000000LL

typedef struct { WORD Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes;
                 void *SecurityDescriptor; void *SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef struct { union { NTSTATUS Status; void *Pointer; }; ULONG_PTR Information; } IO_STATUS_BLOCK;
typedef void (WINAPI *PIO_APC_ROUTINE)(void *ctx, IO_STATUS_BLOCK *iosb, ULONG reserved);

/* k32_extra.c: report an operation on `file` completed to its completion port. */
void lsiw_iocp_io_done(HANDLE file, void *overlapped, DWORD bytes, uint32_t ntstatus);

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static void *k32(const char *name)
{
    static const struct lsiw_builtin_dll *dll;
    if (!dll) dll = lsiw_builtin_find("kernel32.dll");
    return dll ? lsiw_builtin_lookup(dll, name) : NULL;
}

static void p16(void *b, size_t off, uint16_t v) { memcpy((char *)b + off, &v, 2); }
static void p32(void *b, size_t off, uint32_t v) { memcpy((char *)b + off, &v, 4); }
static void p64(void *b, size_t off, uint64_t v) { memcpy((char *)b + off, &v, 8); }
static uint32_t g32(const void *b, size_t off) { uint32_t v; memcpy(&v, (const char *)b + off, 4); return v; }
static uint64_t g64(const void *b, size_t off) { uint64_t v; memcpy(&v, (const char *)b + off, 8); return v; }

static NTSTATUS errno_st(int e) { return (NTSTATUS)lsiw_errno_to_ntstatus(e); }

/* Win32 error (from a kernel32 call this file reuses) -> NTSTATUS. */
static NTSTATUS win32_st(DWORD e)
{
    switch (e) {
    case 0:                         return STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:      return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND:      return STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_ACCESS_DENIED:       return STATUS_ACCESS_DENIED;
    case ERROR_INVALID_HANDLE:      return STATUS_INVALID_HANDLE;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:      return STATUS_OBJECT_NAME_COLLISION;
    case ERROR_INVALID_PARAMETER:   return STATUS_INVALID_PARAMETER;
    case ERROR_INVALID_NAME:        return STATUS_OBJECT_NAME_INVALID;
    case ERROR_NOT_ENOUGH_MEMORY:   return STATUS_NO_MEMORY;
    case ERROR_BROKEN_PIPE:         return STATUS_PIPE_BROKEN;
    case ERROR_INVALID_FUNCTION:    return STATUS_INVALID_DEVICE_REQUEST;
    case 231: /* ERROR_PIPE_BUSY */ return ST(0xC00000AEu);   /* STATUS_PIPE_BUSY */
    case 536: /* ERROR_PIPE_LISTENING */ return ST(0xC00000B3u);
    case 233: /* ERROR_PIPE_NOT_CONNECTED */ return ST(0xC00000B0u);
    default:                        return ST(0xC0070000u | (e & 0xFFFF));
    }
}

static int64_t ft_of(int64_t sec, uint32_t nsec) { return sec * 10000000LL + nsec / 100 + EPOCH_DIFF; }

static int64_t now_ft(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ft_of(ts.tv_sec, (uint32_t)ts.tv_nsec);
}

static struct timespec ts_of(int64_t ft)
{
    if (ft < EPOCH_DIFF) ft = EPOCH_DIFF;
    ft -= EPOCH_DIFF;
    return (struct timespec){ (time_t)(ft / 10000000LL), (long)(ft % 10000000LL) * 100 };
}

/* Upper-case one UTF-16 unit the way NTFS's upcase table does for the
 * common alphabets (ASCII, Latin-1, Greek, Cyrillic). */
static WCHAR upw(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 32);
    if (c < 0x80) return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (WCHAR)(c - 32);
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (WCHAR)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (WCHAR)(c - 80);
    return c;
}

/* ------------------------------------------------------------------ */
/* Stat data in Windows units                                           */
/* ------------------------------------------------------------------ */

struct xst {
    uint32_t mode, nlink, uid, gid, rmajor, rminor;
    uint64_t ino, dev, size, blocks;
    int64_t atime, mtime, ctime, btime;   /* FILETIME units */
};

/* statx() gives the birth time where the file system keeps one; otherwise
 * the creation time is the earlier of the modification and change times. */
static int xstat_at(int dirfd, const char *name, int flags, struct xst *x)
{
    struct statx sx;
    if (statx(dirfd, name, flags, STATX_BASIC_STATS | STATX_BTIME, &sx) == 0) {
        x->mode = sx.stx_mode; x->nlink = sx.stx_nlink; x->uid = sx.stx_uid; x->gid = sx.stx_gid;
        x->rmajor = sx.stx_rdev_major; x->rminor = sx.stx_rdev_minor;
        x->ino = sx.stx_ino; x->dev = makedev(sx.stx_dev_major, sx.stx_dev_minor);
        x->size = sx.stx_size; x->blocks = sx.stx_blocks;
        x->atime = ft_of(sx.stx_atime.tv_sec, sx.stx_atime.tv_nsec);
        x->mtime = ft_of(sx.stx_mtime.tv_sec, sx.stx_mtime.tv_nsec);
        x->ctime = ft_of(sx.stx_ctime.tv_sec, sx.stx_ctime.tv_nsec);
        if ((sx.stx_mask & STATX_BTIME) && sx.stx_btime.tv_sec)
            x->btime = ft_of(sx.stx_btime.tv_sec, sx.stx_btime.tv_nsec);
        else
            x->btime = x->mtime < x->ctime ? x->mtime : x->ctime;
        return 0;
    }
    if (errno != ENOSYS && errno != EPERM) return -1;
    struct stat st;
    if (fstatat(dirfd, name, &st, flags) < 0) return -1;
    x->mode = st.st_mode; x->nlink = (uint32_t)st.st_nlink; x->uid = st.st_uid; x->gid = st.st_gid;
    x->rmajor = major(st.st_rdev); x->rminor = minor(st.st_rdev);
    x->ino = st.st_ino; x->dev = st.st_dev; x->size = (uint64_t)st.st_size; x->blocks = (uint64_t)st.st_blocks;
    x->atime = ft_of(st.st_atim.tv_sec, (uint32_t)st.st_atim.tv_nsec);
    x->mtime = ft_of(st.st_mtim.tv_sec, (uint32_t)st.st_mtim.tv_nsec);
    x->ctime = ft_of(st.st_ctim.tv_sec, (uint32_t)st.st_ctim.tv_nsec);
    x->btime = x->mtime < x->ctime ? x->mtime : x->ctime;
    return 0;
}

/* The same attribute rules as kernel32 (k32_file.c attrs_of): a symbolic
 * link is a reparse point (and a folder when it points at one), dot-files are
 * hidden, and read-only is the owner write bit or a file this user cannot
 * write. */
static DWORD attrs_of(const char *path, const struct xst *x)
{
    if (S_ISLNK(x->mode)) {
        struct stat t;
        return FILE_ATTRIBUTE_REPARSE_POINT |
               ((path && stat(path, &t) == 0 && S_ISDIR(t.st_mode)) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
    }
    if (!S_ISDIR(x->mode) && !S_ISREG(x->mode) && !S_ISBLK(x->mode)) return FILE_ATTRIBUTE_NORMAL;
    DWORD a = 0;
    int dir = S_ISDIR(x->mode);
    if (dir) a |= FILE_ATTRIBUTE_DIRECTORY;
    if (path) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (base[0] == '.' && base[1] && strcmp(base, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    }
    if (!dir && (!(x->mode & S_IWUSR) || (path && access(path, W_OK) != 0 && errno == EACCES)))
        a |= FILE_ATTRIBUTE_READONLY;
    if (!a) a = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    else if (!dir) a |= FILE_ATTRIBUTE_ARCHIVE;
    return a;
}

/* What kind of object a descriptor is, as the I/O manager would see it. */
enum { K_DISK, K_DIR, K_PIPE, K_SOCK, K_NULL, K_CONSOLE, K_CHAR };

struct fctx {
    HANDLE h;
    struct lsiw_handle_obj *o;
    int fd;
    int kind;
    struct xst x;
};

static NTSTATUS bad_handle(HANDLE h)
{
    return lsiw_handle_get(h, -1) ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_INVALID_HANDLE;
}

static NTSTATUS get_ctx(HANDLE h, struct fctx *c)
{
    memset(c, 0, sizeof *c);
    c->o = lsiw_handle_get(h, H_FILE);
    if (!c->o) return bad_handle(h);
    c->h = h;
    c->fd = c->o->fd;
    if (xstat_at(c->fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, &c->x) < 0) return STATUS_INVALID_HANDLE;
    mode_t m = c->x.mode;
    if (S_ISDIR(m)) c->kind = K_DIR;
    else if (S_ISFIFO(m)) c->kind = K_PIPE;
    else if (S_ISSOCK(m)) {
        struct sockaddr_storage ss;
        socklen_t l = sizeof ss;
        ss.ss_family = AF_UNSPEC;
        /* AF_UNIX sockets carry Windows named pipes (k32_extra.c). */
        c->kind = (getsockname(c->fd, (struct sockaddr *)&ss, &l) == 0 && ss.ss_family == AF_UNIX) ? K_PIPE : K_SOCK;
    } else if (S_ISCHR(m)) {
        if (c->x.rmajor == 1 && c->x.rminor == 3) c->kind = K_NULL;
        else if (c->o->is_console || isatty(c->fd)) c->kind = K_CONSOLE;
        else c->kind = K_CHAR;
    } else c->kind = K_DISK;
    return STATUS_SUCCESS;
}

static int is_fs(const struct fctx *c) { return c->kind == K_DISK || c->kind == K_DIR; }
static int is_opath(int fd) { int fl = fcntl(fd, F_GETFL); return fl >= 0 && (fl & O_PATH); }

/* The access mask the handle effectively has, from its open mode. */
static DWORD access_of(const struct fctx *c)
{
    int fl = fcntl(c->fd, F_GETFL);
    if (fl < 0) return 0;
    if (fl & O_PATH) return FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE_ACCESS | READ_CONTROL | SYNCHRONIZE;
    DWORD a = SYNCHRONIZE | READ_CONTROL | FILE_READ_ATTRIBUTES;
    int acc = fl & O_ACCMODE;
    if (acc == O_RDONLY || acc == O_RDWR) a |= FILE_GENERIC_READ;
    if (acc == O_WRONLY || acc == O_RDWR) a |= (fl & O_APPEND) ? (FILE_GENERIC_WRITE & ~FILE_WRITE_DATA) : FILE_GENERIC_WRITE;
    if (is_fs(c)) a |= DELETE_ACCESS | FILE_WRITE_ATTRIBUTES;
    return a;
}

/* ------------------------------------------------------------------ */
/* Completion: IO_STATUS_BLOCK, event, APC, completion port            */
/* ------------------------------------------------------------------ */

static void event_set(HANDLE ev, int on)
{
    if (!ev) return;
    BOOL (WINAPI *fn)(HANDLE) = k32(on ? "SetEvent" : "ResetEvent");
    /* The low two bits of a handle value are tag bits the kernel ignores
     * (kernel32 uses bit 0 to mean "no completion port packet"). */
    if (fn) fn((HANDLE)((uintptr_t)ev & ~(uintptr_t)3));
}

struct apc_call { PIO_APC_ROUTINE fn; void *ctx; IO_STATUS_BLOCK *iosb; };
static void WINAPI apc_tramp(ULONG_PTR p)
{
    struct apc_call *a = (struct apc_call *)p;
    a->fn(a->ctx, a->iosb, 0);
    free(a);
}

/* Deliver a completion. A request that fails synchronously writes the status
 * block but neither signals the event nor queues an APC or a port packet;
 * one that was pending (pended != 0) always completes fully. */
static void io_finish(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, HANDLE apc_thread,
                      IO_STATUS_BLOCK *iosb, NTSTATUS st, ULONG_PTR info, int pended)
{
    if (iosb) {
        iosb->Information = info;
        __atomic_store_n(&iosb->Status, st, __ATOMIC_RELEASE);
    }
    if (NT_ERROR(st) && !pended) return;
    event_set(event, 1);
    if (apc) {
        DWORD (WINAPI *q)(void (WINAPI *)(ULONG_PTR), HANDLE, ULONG_PTR) = k32("QueueUserAPC");
        struct apc_call *a = malloc(sizeof *a);
        if (q && a) {
            a->fn = apc; a->ctx = ctx; a->iosb = iosb;
            if (!q(apc_tramp, apc_thread ? apc_thread : (HANDLE)(intptr_t)-2, (ULONG_PTR)a)) free(a);
        } else free(a);
    } else if (ctx) {
        lsiw_iocp_io_done(file, ctx, (DWORD)info, (uint32_t)st);
    }
}

/* ------------------------------------------------------------------ */
/* NT names -> Linux paths                                              */
/* ------------------------------------------------------------------ */

/* Strip the NT prefixes that name the DOS device namespace. Returns the DOS
 * part ("C:\x"), or NULL for names outside it. */
static const char *dos_part(const char *n)
{
    if (!strncmp(n, "\\??\\", 4) || !strncmp(n, "\\\\?\\", 4) || !strncmp(n, "\\\\.\\", 4)) return n + 4;
    if (!strncasecmp(n, "\\DosDevices\\", 12)) return n + 12;
    if (!strncasecmp(n, "\\GLOBAL??\\", 10)) return n + 10;
    return n;
}

/* Build the Windows form ("C:\dir\name") of an NT name, possibly relative to
 * a directory handle. */
static char *nt_to_win(const WCHAR *name, size_t nch, HANDLE root, NTSTATUS *st)
{
    *st = STATUS_SUCCESS;
    char *u = name && nch ? lsiw_w2u_n(name, (int)nch) : strdup("");
    if (!u) { *st = STATUS_NO_MEMORY; return NULL; }
    for (char *c = u; *c; c++) if (*c == '/') *c = '\\';
    char *win;
    if (root) {
        struct lsiw_handle_obj *r = lsiw_handle_get(root, H_FILE);
        if (!r || !r->path) { free(u); *st = r ? STATUS_INVALID_PARAMETER : bad_handle(root); return NULL; }
        if (u[0] == '\\') { free(u); *st = STATUS_OBJECT_NAME_INVALID; return NULL; }
        char *rw = lsiw_linux_to_win(r->path);
        size_t rl = strlen(rw);
        win = *u ? lsiw_strdup_printf("%s%s%s", rw, (rl && rw[rl - 1] == '\\') ? "" : "\\", u) : strdup(rw);
        free(rw);
    } else {
        const char *d = dos_part(u);
        if (!strncasecmp(d, "UNC\\", 4) || (d == u && u[0] == '\\' && u[1] == '\\')) {
            free(u); *st = STATUS_OBJECT_PATH_NOT_FOUND; return NULL;
        }
        if (d[0] && d[1] == ':') win = strdup(d);
        else {
            *st = d == u && u[0] != '\\' ? STATUS_OBJECT_NAME_INVALID : STATUS_OBJECT_PATH_NOT_FOUND;
            free(u);
            return NULL;
        }
    }
    free(u);
    return win;
}

/* errno from a lookup of `lin` -> NTSTATUS, telling a missing file from a
 * missing folder on the way to it. */
static NTSTATUS lookup_st(int e, const char *lin)
{
    if (e == ENOENT && lin) {
        char *d = strdup(lin);
        char *s = strrchr(d, '/');
        if (s && s != d) *s = 0;
        struct stat st;
        int parent = stat(d, &st) == 0 && S_ISDIR(st.st_mode);
        free(d);
        return parent ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (e == ENOTDIR) return STATUS_OBJECT_PATH_NOT_FOUND;
    return errno_st(e);
}

static char *oa_to_linux(const OBJECT_ATTRIBUTES *oa, NTSTATUS *st)
{
    if (!oa) { *st = STATUS_INVALID_PARAMETER; return NULL; }
    const UNICODE_STRING *n = oa->ObjectName;
    char *win = nt_to_win(n ? n->Buffer : NULL, n ? n->Length / 2 : 0, oa->RootDirectory, st);
    if (!win) return NULL;
    char *lin = lsiw_win_to_linux(win);
    free(win);
    if (!lin) *st = STATUS_OBJECT_PATH_NOT_FOUND;
    return lin;
}

/* Find an existing entry of `dir` named `base` ignoring case (the exact
 * spelling wins). Returns the entry's real name or NULL. */
static char *ci_entry(const char *dir, const char *base)
{
    char *full = lsiw_strdup_printf("%s/%s", dir, base);
    struct stat st;
    int exact = lstat(full, &st) == 0;
    free(full);
    if (exact) return strdup(base);
    DIR *d = opendir(dir);
    if (!d) return NULL;
    char *found = NULL;
    struct dirent *de;
    while ((de = readdir(d)))
        if (!strcasecmp(de->d_name, base)) { found = strdup(de->d_name); break; }
    closedir(d);
    return found;
}

/* Target of a rename or link: FileName is a full NT/DOS path, a name relative
 * to RootDirectory, or (no separators, no root) a new name in the source's own
 * folder. The folder is resolved like any Windows path; the last component is
 * kept as written so case-only renames work. Returns parent + base. */
static NTSTATUS rename_target(const WCHAR *name, size_t nch, HANDLE root, const char *src,
                              char **parent_out, char **base_out)
{
    NTSTATUS st = STATUS_SUCCESS;
    char *u = lsiw_w2u_n(name, (int)nch);
    if (!u) return STATUS_NO_MEMORY;
    char *parent = NULL, *base = NULL;
    if (!root && !strchr(u, '\\') && !strchr(u, '/') && !strchr(u, ':')) {
        parent = strdup(src);
        char *s = strrchr(parent, '/');
        if (s && s != parent) *s = 0; else strcpy(parent, "/");
        base = u;
        u = NULL;
    } else {
        char *win;
        if (!root && u[0] == '\\' && u[1] != '\\' && strncmp(u, "\\??\\", 4) &&
            strncasecmp(u, "\\DosDevices\\", 12) && strncasecmp(u, "\\GLOBAL??\\", 10)) {
            win = lsiw_strdup_printf("C:%s", u);      /* "\dir\name" on the same volume */
        } else {
            WCHAR *w = lsiw_u2w(u);
            win = nt_to_win(w, lsiw_wcslen(w), root, &st);
            free(w);
        }
        free(u);
        u = NULL;
        if (!win) return st;
        for (char *c = win; *c; c++) if (*c == '/') *c = '\\';
        char *s = strrchr(win, '\\');
        if (!s || !s[1]) { free(win); return STATUS_OBJECT_NAME_INVALID; }
        base = strdup(s + 1);
        if (s == win + 2) s[1] = 0; else *s = 0;      /* keep "C:\" for the root */
        parent = lsiw_win_to_linux(win);
        free(win);
    }
    free(u);
    struct stat ps;
    if (!parent || stat(parent, &ps) < 0 || !S_ISDIR(ps.st_mode)) { st = STATUS_OBJECT_PATH_NOT_FOUND; goto fail; }
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..") || strpbrk(base, "<>:\"|?*")) { st = STATUS_OBJECT_NAME_INVALID; goto fail; }
    *parent_out = parent;
    *base_out = base;
    return STATUS_SUCCESS;
fail:
    free(parent);
    free(base);
    return st;
}

/* The handle's path still names the object the descriptor refers to? (An
 * outside rename would otherwise make us delete or move the wrong file.)
 * A handle opened through a symbolic link refers to the link's target, so
 * the real path is returned in that case. */
static char *checked_path(const struct fctx *c, NTSTATUS *st)
{
    *st = STATUS_SUCCESS;
    if (!c->o->path) { *st = STATUS_INVALID_PARAMETER; return NULL; }
    struct stat ls;
    if (lstat(c->o->path, &ls) == 0) {
        if (ls.st_dev == c->x.dev && ls.st_ino == c->x.ino) return strdup(c->o->path);
        if (S_ISLNK(ls.st_mode)) {
            char *real = realpath(c->o->path, NULL);
            struct stat rs;
            if (real && stat(real, &rs) == 0 && rs.st_dev == c->x.dev && rs.st_ino == c->x.ino) return real;
            free(real);
        }
    }
    *st = c->x.nlink == 0 ? STATUS_DELETE_PENDING : STATUS_OBJECT_NAME_NOT_FOUND;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* NtQueryInformationFile                                               */
/* ------------------------------------------------------------------ */

static void fill_basic(const struct fctx *c, void *b)
{
    p64(b, 0, (uint64_t)c->x.btime);
    p64(b, 8, (uint64_t)c->x.atime);
    p64(b, 16, (uint64_t)c->x.mtime);
    p64(b, 24, (uint64_t)c->x.ctime);
    p32(b, 32, attrs_of(c->o->path, &c->x));
    p32(b, 36, 0);
}

static uint64_t alloc_size(const struct xst *x) { return S_ISDIR(x->mode) || S_ISLNK(x->mode) ? 0 : x->blocks * 512; }
static uint64_t eof_size(const struct xst *x)   { return S_ISDIR(x->mode) || S_ISLNK(x->mode) ? 0 : x->size; }

static void fill_standard(const struct fctx *c, void *b)
{
    p64(b, 0, alloc_size(&c->x));
    p64(b, 8, eof_size(&c->x));
    p32(b, 16, c->x.nlink);
    ((BYTE *)b)[20] = c->x.nlink == 0 && is_fs(c);   /* DeletePending: already unlinked */
    ((BYTE *)b)[21] = S_ISDIR(c->x.mode) != 0;
    p16(b, 22, 0);
}

static DWORD mode_of(const struct fctx *c)
{
    /* Every LSIW handle does synchronous I/O (calls complete before they
     * return), which is what FILE_SYNCHRONOUS_IO_NONALERT says; libuv relies
     * on it to read such pipes from a helper thread. */
    DWORD m = 0x20;
    int fl = fcntl(c->fd, F_GETFL);
    if (fl >= 0) {
        if (fl & (O_SYNC | O_DSYNC)) m |= 0x2;            /* FILE_WRITE_THROUGH */
        if (fl & O_DIRECT) m |= 0x8;                      /* FILE_NO_INTERMEDIATE_BUFFERING */
    }
    if (c->x.nlink == 0 && is_fs(c)) m |= 0x1000;         /* FILE_DELETE_ON_CLOSE */
    return m;
}

static int64_t position_of(int fd)
{
    off_t p = lseek(fd, 0, SEEK_CUR);
    return p < 0 ? 0 : (int64_t)p;
}

/* The file's name without the drive ("\dir\file"), UTF-16, malloc'd. */
static WCHAR *name_of(const struct fctx *c, size_t *nch, NTSTATUS *st)
{
    char *s = NULL;
    *st = STATUS_SUCCESS;
    if (c->kind == K_PIPE) {
        if (c->o->path) {
            const char *b = strrchr(c->o->path, '/');
            s = lsiw_strdup_printf("\\%s", b ? b + 1 : c->o->path);
        } else {
            /* Anonymous pipes are named pipes "Win32Pipes.<pid>.<serial>". */
            s = lsiw_strdup_printf("\\Win32Pipes.%08x.%08x", (unsigned)getpid(), (unsigned)c->x.ino);
        }
    } else if (c->o->path && is_fs(c)) {
        char *w = lsiw_linux_to_win(c->o->path);
        s = strdup((w[0] && w[1] == ':') ? w + 2 : w);
        if (!*s) { free(s); s = strdup("\\"); }
        free(w);
    } else {
        *st = STATUS_INVALID_PARAMETER;      /* consoles, sockets, devices */
        return NULL;
    }
    WCHAR *ws = lsiw_u2w(s);
    free(s);
    *nch = lsiw_wcslen(ws);
    return ws;
}

/* FILE_NAME_INFORMATION at b (room bytes): returns the status and bytes used. */
static NTSTATUS fill_name(const struct fctx *c, void *b, ULONG room, ULONG *used)
{
    size_t n;
    NTSTATUS st;
    WCHAR *ws = name_of(c, &n, &st);
    if (!ws) { *used = 0; return st; }
    ULONG bytes = (ULONG)n * 2;
    p32(b, 0, bytes);
    ULONG fit = room - 4 < bytes ? (room - 4) & ~1u : bytes;
    memcpy((char *)b + 4, ws, fit);
    free(ws);
    *used = 4 + fit;
    return fit < bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

static void fill_network_open(const struct xst *x, DWORD attrs, void *b)
{
    p64(b, 0, (uint64_t)x->btime);
    p64(b, 8, (uint64_t)x->atime);
    p64(b, 16, (uint64_t)x->mtime);
    p64(b, 24, (uint64_t)x->ctime);
    p64(b, 32, alloc_size(x));
    p64(b, 40, eof_size(x));
    p32(b, 48, attrs);
    p32(b, 52, 0);
}

static void fill_stat(const struct xst *x, DWORD attrs, DWORD access, void *b)
{
    p64(b, 0, x->ino);
    p64(b, 8, (uint64_t)x->btime);
    p64(b, 16, (uint64_t)x->atime);
    p64(b, 24, (uint64_t)x->mtime);
    p64(b, 32, (uint64_t)x->ctime);
    p64(b, 40, alloc_size(x));
    p64(b, 48, eof_size(x));
    p32(b, 56, attrs);
    p32(b, 60, S_ISLNK(x->mode) ? IO_REPARSE_TAG_SYMLINK : 0);
    p32(b, 64, x->nlink);
    p32(b, 68, access);
}

/* FILE_STAT_LX_INFORMATION (96 bytes): FILE_STAT_INFORMATION's 72 bytes
 * followed by the Linux metadata. */
static void fill_stat_lx(const struct xst *x, void *b)
{
    p32(b, 72, 0x1 | 0x2 | 0x4 | 0x8);    /* LX_FILE_METADATA_HAS_UID|GID|MODE|DEVICE_ID */
    p32(b, 76, x->uid);
    p32(b, 80, x->gid);
    p32(b, 84, x->mode);
    p32(b, 88, x->rmajor);
    p32(b, 92, x->rminor);
}

static NTSTATUS pipe_local(const struct fctx *c, void *b)
{
    DWORD fl = 0, outq = 4096, inq = 4096, maxi = 1;
    BOOL (WINAPI *info)(HANDLE, DWORD *, DWORD *, DWORD *, DWORD *) = k32("GetNamedPipeInfo");
    int fifo = S_ISFIFO(c->x.mode);
    if (!info || !info(c->h, &fl, &outq, &inq, &maxi)) {
        if (!fifo) return STATUS_INVALID_PARAMETER;
        int sz = fcntl(c->fd, F_GETPIPE_SZ);
        if (sz > 0) outq = inq = (DWORD)sz;
        fl = (fcntl(c->fd, F_GETFL) & O_ACCMODE) == O_RDONLY ? 1 : 0;
    }
    int queued = 0;
    if (ioctl(c->fd, FIONREAD, &queued) < 0) queued = 0;
    DWORD write_quota;
    if (fifo) {
        int cap = fcntl(c->fd, F_GETPIPE_SZ);
        if (cap <= 0) cap = (int)inq;
        write_quota = queued < cap ? (DWORD)(cap - queued) : 0;
    } else {
        int sndbuf = 0, outstanding = 0;
        socklen_t l = sizeof sndbuf;
        getsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &l);
        if (ioctl(c->fd, SIOCOUTQ, &outstanding) < 0) outstanding = 0;
        write_quota = sndbuf > outstanding ? (DWORD)(sndbuf - outstanding) : 0;
    }
    /* State: connected, or closing once the other end is gone and the data drained. */
    DWORD state = 3;                                   /* FILE_PIPE_CONNECTED_STATE */
    struct pollfd p = { c->fd, POLLIN | POLLRDHUP, 0 };
    if (!queued && poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLRDHUP | POLLERR))) state = 4;
    p32(b, 0, (fl & 4) ? 1 : 0);                       /* NamedPipeType: byte / message */
    /* CreatePipe makes an inbound pipe (client writes, server reads). */
    p32(b, 4, fifo ? 0 : 2);                           /* NamedPipeConfiguration */
    p32(b, 8, maxi);
    p32(b, 12, 1);                                     /* CurrentInstances */
    p32(b, 16, inq);
    p32(b, 20, (DWORD)queued);                         /* ReadDataAvailable */
    p32(b, 24, outq);
    p32(b, 28, write_quota);
    p32(b, 32, state);
    p32(b, 36, (fl & 1) ? 1 : 0);                      /* NamedPipeEnd: server / client */
    return STATUS_SUCCESS;
}

/* Minimum buffer size for each query class (sizeof the Windows structure). */
static ULONG query_min(int cls)
{
    switch (cls) {
    case FileBasicInformation: return 40;
    case FileStandardInformation: return 24;
    case FileInternalInformation: return 8;
    case FileEaInformation: return 4;
    case FileAccessInformation: return 4;
    case FileNameInformation: case FileNormalizedNameInformation: return 8;
    case FilePositionInformation: return 8;
    case FileModeInformation: return 4;
    case FileAlignmentInformation: return 4;
    case FileAllInformation: return 104;
    case FileAlternateNameInformation: return 8;
    case FileStreamInformation: return 24;
    case FilePipeInformation: return 8;
    case FilePipeLocalInformation: return 40;
    case FileCompressionInformation: return 16;
    case FileNetworkOpenInformation: return 56;
    case FileAttributeTagInformation: return 8;
    case FileIoPriorityHintInformation: return 4;
    case FileIsRemoteDeviceInformation: return 1;
    case FileIdInformation: return 24;
    case FileStatInformation: return 72;
    case FileStatLxInformation: return 96;
    case FileCaseSensitiveInformation: return 4;
    default: return 0;
    }
}

API NTSTATUS NtQueryInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, int cls)
{
    ULONG need = query_min(cls);
    if (!need) return STATUS_INVALID_INFO_CLASS;
    if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
    if (!buf) return ST(0xC0000005u);                    /* STATUS_ACCESS_VIOLATION */
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    ULONG used = need;
    switch (cls) {
    case FileBasicInformation: fill_basic(&c, buf); break;
    case FileStandardInformation: fill_standard(&c, buf); break;
    case FileInternalInformation: p64(buf, 0, c.x.ino); break;
    case FileEaInformation: p32(buf, 0, 0); break;
    case FileAccessInformation: p32(buf, 0, access_of(&c)); break;
    case FileNameInformation:
    case FileNormalizedNameInformation:
        st = fill_name(&c, buf, len, &used);
        if (NT_ERROR(st)) return st;
        break;
    case FilePositionInformation: p64(buf, 0, (uint64_t)position_of(c.fd)); break;
    case FileModeInformation: p32(buf, 0, mode_of(&c)); break;
    case FileAlignmentInformation:
        p32(buf, 0, (mode_of(&c) & 0x8) ? 511 : 0);     /* FILE_512_BYTE_ALIGNMENT / FILE_BYTE_ALIGNMENT */
        break;
    case FileAllInformation: {
        fill_basic(&c, buf);
        fill_standard(&c, (char *)buf + 40);
        p64(buf, 64, c.x.ino);
        p32(buf, 72, 0);
        p32(buf, 76, access_of(&c));
        p64(buf, 80, (uint64_t)position_of(c.fd));
        p32(buf, 88, mode_of(&c));
        p32(buf, 92, (mode_of(&c) & 0x8) ? 511 : 0);
        ULONG nused;
        st = fill_name(&c, (char *)buf + 96, len - 96, &nused);
        if (NT_ERROR(st)) { p32(buf, 96, 0); nused = 4; st = STATUS_SUCCESS; }   /* unnamed object */
        used = 96 + nused;
        break; }
    case FileAlternateNameInformation:
        /* No 8.3 short names exist (as on an NTFS volume with them disabled). */
        if (!is_fs(&c)) return STATUS_INVALID_PARAMETER;
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case FileStreamInformation: {
        if (!is_fs(&c)) return STATUS_INVALID_PARAMETER;
        if (S_ISDIR(c.x.mode)) { used = 0; break; }        /* folders have no unnamed stream */
        static const char data[] = "::$DATA";
        p32(buf, 0, 0);
        p32(buf, 4, 14);
        p64(buf, 8, eof_size(&c.x));
        p64(buf, 16, alloc_size(&c.x));
        used = len < 38 ? len : 38;
        for (int i = 0; i < 7 && 24 + 2 * (ULONG)i + 2 <= len; i++) p16(buf, 24 + 2 * (size_t)i, (uint16_t)data[i]);
        if (len < 38) st = STATUS_BUFFER_OVERFLOW;
        break; }
    case FilePipeInformation: {
        if (c.kind != K_PIPE) return STATUS_INVALID_PARAMETER;
        DWORD state = 0;
        BOOL (WINAPI *gs)(HANDLE, DWORD *, DWORD *, DWORD *, DWORD *, WCHAR *, DWORD) = k32("GetNamedPipeHandleStateW");
        if (!gs || !gs(h, &state, NULL, NULL, NULL, NULL, 0)) {
            int fl = fcntl(c.fd, F_GETFL);
            state = (fl >= 0 && (fl & O_NONBLOCK)) ? 1 : 0;
        }
        p32(buf, 0, (state & 2) ? 1 : 0);                /* ReadMode: message */
        p32(buf, 4, (state & 1) ? 1 : 0);                /* CompletionMode: complete operation */
        break; }
    case FilePipeLocalInformation:
        if (c.kind != K_PIPE) return STATUS_INVALID_PARAMETER;
        st = pipe_local(&c, buf);
        if (st) return st;
        break;
    case FileCompressionInformation:
        if (!is_fs(&c)) return STATUS_INVALID_PARAMETER;
        memset(buf, 0, 16);
        p64(buf, 0, eof_size(&c.x));                     /* COMPRESSION_FORMAT_NONE */
        break;
    case FileNetworkOpenInformation:
        fill_network_open(&c.x, attrs_of(c.o->path, &c.x), buf);
        break;
    case FileAttributeTagInformation:
        p32(buf, 0, attrs_of(c.o->path, &c.x));
        p32(buf, 4, S_ISLNK(c.x.mode) ? IO_REPARSE_TAG_SYMLINK : 0);
        break;
    case FileIoPriorityHintInformation: p32(buf, 0, 2); break;   /* IoPriorityNormal */
    case FileIsRemoteDeviceInformation: ((BYTE *)buf)[0] = 0; used = 1; break;
    case FileIdInformation:
        p64(buf, 0, c.x.dev);                            /* same serial as GetFileInformationByHandle */
        p64(buf, 8, c.x.ino);
        p64(buf, 16, 0);
        break;
    case FileStatInformation:
        fill_stat(&c.x, attrs_of(c.o->path, &c.x), access_of(&c), buf);
        break;
    case FileStatLxInformation:
        fill_stat(&c.x, attrs_of(c.o->path, &c.x), access_of(&c), buf);
        fill_stat_lx(&c.x, buf);
        break;
    case FileCaseSensitiveInformation:
        /* LSIW resolves names ignoring case, like an ordinary NTFS folder. */
        p32(buf, 0, 0);
        break;
    }
    if (iosb) { iosb->Status = st; iosb->Information = used; }
    return st;
}

/* ------------------------------------------------------------------ */
/* NtSetInformationFile                                                 */
/* ------------------------------------------------------------------ */

static NTSTATUS set_basic(const struct fctx *c, const void *b)
{
    int64_t t[4];
    for (int i = 0; i < 4; i++) t[i] = (int64_t)g64(b, (size_t)i * 8);
    DWORD attrs = g32(b, 32);
    /* 0 leaves a time alone; -1 / -2 (stop / resume automatic updates) have no
     * Linux equivalent and leave it alone too. Creation and change times
     * cannot be set on Linux; Windows would set them, so they are ignored. */
    struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
    if (t[1] > 0) ts[0] = ts_of(t[1]);
    if (t[2] > 0) ts[1] = ts_of(t[2]);
    if (ts[0].tv_nsec != UTIME_OMIT || ts[1].tv_nsec != UTIME_OMIT) {
        int r = is_opath(c->fd) ? (c->o->path ? utimensat(AT_FDCWD, c->o->path, ts, AT_SYMLINK_NOFOLLOW) : -1)
                                : futimens(c->fd, ts);
        if (r < 0) {
            if (!is_fs(c)) return STATUS_SUCCESS;    /* pipes and devices keep no times */
            return errno == EPERM ? STATUS_ACCESS_DENIED : errno_st(errno);
        }
    }
    /* Attributes: only READONLY maps (to the owner write bit, as kernel32's
     * SetFileAttributes does); HIDDEN is a matter of the name; folders and
     * links keep their mode, READONLY on a folder being advisory on Windows. */
    if (attrs && S_ISREG(c->x.mode)) {
        mode_t m = c->x.mode & 07777;
        if (attrs & FILE_ATTRIBUTE_READONLY) m &= ~(mode_t)0222; else m |= S_IWUSR;
        if (m != (c->x.mode & 07777)) {
            int r = is_opath(c->fd) ? (c->o->path ? chmod(c->o->path, m) : -1) : fchmod(c->fd, m);
            if (r < 0) return errno == EPERM ? STATUS_ACCESS_DENIED : errno_st(errno);
        }
    }
    return STATUS_SUCCESS;
}

#define FILE_RENAME_REPLACE_IF_EXISTS      0x1
#define FILE_RENAME_POSIX_SEMANTICS        0x2
#define FILE_RENAME_IGNORE_READONLY        0x40

static void set_handle_path(struct lsiw_handle_obj *o, char *path) { free(o->path); o->path = path; }

static NTSTATUS rename_or_link(const struct fctx *c, const void *b, ULONG len, int cls)
{
    int is_link = cls == FileLinkInformation || cls == FileLinkInformationEx;
    ULONG flags = (cls == FileRenameInformation || cls == FileLinkInformation)
                ? (((const BYTE *)b)[0] ? FILE_RENAME_REPLACE_IF_EXISTS : 0) : g32(b, 0);
    HANDLE root;
    memcpy(&root, (const char *)b + 8, sizeof root);
    ULONG nlen = g32(b, 16);
    if (!nlen || (nlen & 1) || nlen > len - 20) return STATUS_INVALID_PARAMETER;
    if (!is_fs(c)) return STATUS_INVALID_PARAMETER;
    if (is_link && S_ISDIR(c->x.mode)) return STATUS_FILE_IS_A_DIRECTORY;
    NTSTATUS st;
    char *src = checked_path(c, &st);
    if (!src) return st;
    WCHAR *wn = malloc(nlen + 2);
    memcpy(wn, (const char *)b + 20, nlen);
    wn[nlen / 2] = 0;
    char *parent = NULL, *base = NULL;
    st = rename_target(wn, nlen / 2, root, src, &parent, &base);
    free(wn);
    if (st) { free(src); return st; }
    char *dst = lsiw_strdup_printf("%s%s%s", parent, strcmp(parent, "/") ? "/" : "", base);
    char *existing_name = ci_entry(parent, base);
    char *existing = existing_name ? lsiw_strdup_printf("%s%s%s", parent, strcmp(parent, "/") ? "/" : "", existing_name) : NULL;
    int r = 0;
    st = STATUS_SUCCESS;
    if (existing) {
        struct stat es;
        if (lstat(existing, &es) < 0) { st = errno_st(errno); goto out; }
        int same = es.st_dev == c->x.dev && es.st_ino == c->x.ino;
        if (same && !is_link) {
            /* Renaming onto itself: nothing to do, unless only the case changes. */
            if (strcmp(existing_name, base)) r = rename(existing, dst);
            if (r < 0) st = errno_st(errno);
            else set_handle_path(c->o, dst), dst = NULL;
            goto out;
        }
        if (same && is_link) goto out;                  /* already a link to this file */
        if (!(flags & FILE_RENAME_REPLACE_IF_EXISTS)) { st = STATUS_OBJECT_NAME_COLLISION; goto out; }
        /* A folder is never replaced, nor a read-only file unless asked. */
        if (S_ISDIR(es.st_mode)) { st = STATUS_ACCESS_DENIED; goto out; }
        if (S_ISREG(es.st_mode) && !(es.st_mode & S_IWUSR) && !(flags & FILE_RENAME_IGNORE_READONLY)) { st = STATUS_ACCESS_DENIED; goto out; }
        if (is_link) {
            if (unlink(existing) < 0 || link(src, dst) < 0) st = errno_st(errno);
            goto out;
        }
        if (S_ISDIR(c->x.mode)) { st = STATUS_ACCESS_DENIED; goto out; }
        r = rename(src, existing);                      /* atomic replace */
        if (r == 0 && strcmp(existing_name, base)) r = rename(existing, dst);
        if (r < 0) st = errno == EXDEV ? STATUS_NOT_SAME_DEVICE : errno_st(errno);
        else set_handle_path(c->o, dst), dst = NULL;
        goto out;
    }
    if (is_link) {
        if (link(src, dst) < 0) st = errno == EXDEV ? STATUS_NOT_SAME_DEVICE : errno_st(errno);
        goto out;
    }
    r = renameat2(AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE);
    if (r < 0 && (errno == EINVAL || errno == ENOSYS)) r = rename(src, dst);
    if (r < 0) st = errno == EXDEV ? STATUS_NOT_SAME_DEVICE : errno == EEXIST ? STATUS_OBJECT_NAME_COLLISION : errno_st(errno);
    else set_handle_path(c->o, dst), dst = NULL;
out:
    free(src); free(parent); free(base); free(dst); free(existing_name); free(existing);
    return st;
}

#define FILE_DISPOSITION_DELETE           0x01
#define FILE_DISPOSITION_POSIX_SEMANTICS  0x02
#define FILE_DISPOSITION_IGNORE_READONLY  0x10

/* Delete-on-close. Linux keeps an unlinked file usable through its open
 * descriptors, so the name is removed now (as kernel32's FileDispositionInfo
 * does): the program's handle keeps working and the file is gone when the
 * last handle closes, which is what both the default and the POSIX semantics
 * promise. Only clearing the flag again cannot be honoured once set. */
static NTSTATUS dispose(const struct fctx *c, ULONG flags)
{
    if (!(flags & FILE_DISPOSITION_DELETE)) return STATUS_SUCCESS;
    if (!is_fs(c)) return STATUS_INVALID_PARAMETER;
    if (c->x.nlink == 0) return STATUS_SUCCESS;          /* already pending */
    NTSTATUS st;
    char *path = checked_path(c, &st);
    if (!path) return st;
    struct stat ls;
    if (lstat(path, &ls) < 0) { st = errno_st(errno); free(path); return st; }
    if (S_ISREG(ls.st_mode) && !(ls.st_mode & S_IWUSR) && !(flags & FILE_DISPOSITION_IGNORE_READONLY)) {
        free(path);
        return STATUS_CANNOT_DELETE;
    }
    int r = S_ISDIR(ls.st_mode) ? rmdir(path) : unlink(path);
    int e = errno;
    free(path);
    if (r == 0) return STATUS_SUCCESS;
    if (e == ENOTEMPTY || e == EEXIST) return STATUS_DIRECTORY_NOT_EMPTY;
    return errno_st(e);
}

static ULONG set_min(int cls)
{
    switch (cls) {
    case FileBasicInformation: return 40;
    case FileRenameInformation: case FileRenameInformationEx:
    case FileLinkInformation: case FileLinkInformationEx: return 24;
    case FileDispositionInformation: return 1;
    case FilePositionInformation: return 8;
    case FileModeInformation: return 4;
    case FileAllocationInformation: return 8;
    case FileEndOfFileInformation: return 8;
    case FilePipeInformation: return 8;
    case FileCompletionInformation: return 16;
    case FileValidDataLengthInformation: return 8;
    case FileIoCompletionNotificationInformation: return 4;
    case FileIoPriorityHintInformation: return 4;
    case FileDispositionInformationEx: return 4;
    case FileCaseSensitiveInformation: return 4;
    default: return 0;
    }
}

static NTSTATUS trunc_st(int e)
{
    if (e == EISDIR) return STATUS_INVALID_PARAMETER;
    if (e == EBADF || e == EINVAL) return STATUS_ACCESS_DENIED;   /* handle not open for writing */
    return errno_st(e);
}

API NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, int cls)
{
    ULONG need = set_min(cls);
    if (!need) return STATUS_INVALID_INFO_CLASS;
    if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
    if (!buf) return ST(0xC0000005u);
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    switch (cls) {
    case FileBasicInformation: st = set_basic(&c, buf); break;
    case FileRenameInformation: case FileRenameInformationEx:
    case FileLinkInformation: case FileLinkInformationEx:
        st = rename_or_link(&c, buf, len, cls);
        break;
    case FileDispositionInformation: st = dispose(&c, ((BYTE *)buf)[0] ? FILE_DISPOSITION_DELETE : 0); break;
    case FileDispositionInformationEx: st = dispose(&c, g32(buf, 0)); break;
    case FilePositionInformation: {
        int64_t pos = (int64_t)g64(buf, 0);
        if (pos < 0) { st = STATUS_INVALID_PARAMETER; break; }
        if (lseek(c.fd, (off_t)pos, SEEK_SET) < 0 && errno != ESPIPE) st = errno_st(errno);
        break; }
    case FileModeInformation: {
        ULONG m = g32(buf, 0);
        /* Only these can change on an open handle. */
        if (m & ~(ULONG)(0x2 | 0x4 | 0x10 | 0x20)) { st = STATUS_INVALID_PARAMETER; break; }
        if (m & 0x4) posix_fadvise(c.fd, 0, 0, POSIX_FADV_SEQUENTIAL);   /* FILE_SEQUENTIAL_ONLY */
        break; }
    case FileEndOfFileInformation: {
        int64_t n = (int64_t)g64(buf, 0);
        if (n < 0 || !is_fs(&c)) { st = STATUS_INVALID_PARAMETER; break; }
        if (ftruncate(c.fd, (off_t)n) < 0) st = trunc_st(errno);
        break; }
    case FileAllocationInformation: {
        /* Smaller than the data: the file is cut; larger: space is reserved
         * without changing the end of file. */
        int64_t n = (int64_t)g64(buf, 0);
        if (n < 0 || !is_fs(&c)) { st = STATUS_INVALID_PARAMETER; break; }
        if ((uint64_t)n < c.x.size) { if (ftruncate(c.fd, (off_t)n) < 0) st = trunc_st(errno); }
        else if (n > 0 && fallocate(c.fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)n) < 0 &&
                 errno != EOPNOTSUPP && errno != ENOSYS) st = trunc_st(errno);
        break; }
    case FileValidDataLengthInformation: {
        int64_t n = (int64_t)g64(buf, 0);
        /* Linux has no valid-data watermark: every byte below EOF reads as
         * written or zero, which is what extending the VDL exposes. */
        if (n < 0 || (uint64_t)n > c.x.size || !S_ISREG(c.x.mode)) st = STATUS_INVALID_PARAMETER;
        break; }
    case FilePipeInformation: {
        if (c.kind != K_PIPE) { st = STATUS_INVALID_PARAMETER; break; }
        ULONG rm = g32(buf, 0), cm = g32(buf, 4);
        if (rm > 1 || cm > 1) { st = STATUS_INVALID_PARAMETER; break; }
        DWORD mode = (rm ? 2 : 0) | (cm ? 1 : 0);        /* PIPE_READMODE_MESSAGE | PIPE_NOWAIT */
        BOOL (WINAPI *ss)(HANDLE, DWORD *, DWORD *, DWORD *) = k32("SetNamedPipeHandleState");
        if (!ss || !ss(h, &mode, NULL, NULL)) {
            if (S_ISFIFO(c.x.mode) && !rm) {
                int fl = fcntl(c.fd, F_GETFL);
                fcntl(c.fd, F_SETFL, cm ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
            } else st = win32_st(lsiw_get_last_error());
        }
        break; }
    case FileCompletionInformation: {
        HANDLE port; ULONG_PTR key;
        memcpy(&port, buf, sizeof port);
        memcpy(&key, (char *)buf + 8, sizeof key);
        HANDLE (WINAPI *cp)(HANDLE, HANDLE, ULONG_PTR, DWORD) = k32("CreateIoCompletionPort");
        if (!cp || !cp(h, port, key, 0)) st = win32_st(lsiw_get_last_error());
        break; }
    case FileIoCompletionNotificationInformation: {
        BOOL (WINAPI *sm)(HANDLE, BYTE) = k32("SetFileCompletionNotificationModes");
        if (!sm || !sm(h, (BYTE)g32(buf, 0))) st = win32_st(lsiw_get_last_error());
        break; }
    case FileIoPriorityHintInformation:
        if (g32(buf, 0) > 4) st = STATUS_INVALID_PARAMETER;   /* accepted; Linux I/O priority is per thread */
        break;
    case FileCaseSensitiveInformation:
        /* Per-folder case sensitivity needs the WSL feature on Windows; LSIW
         * folders are always case-insensitive. */
        if (!S_ISDIR(c.x.mode)) st = STATUS_INVALID_PARAMETER;
        else if (g32(buf, 0) & 1) st = STATUS_NOT_SUPPORTED;
        break;
    }
    if (iosb) { iosb->Status = st; iosb->Information = 0; }
    return st;
}

/* ------------------------------------------------------------------ */
/* NtQueryVolumeInformationFile / NtSetVolumeInformationFile            */
/* ------------------------------------------------------------------ */

#define FS_FLAGS (0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x80 | 0x400 | 0x00400000)
/* case-sensitive search, case-preserved names, Unicode, persistent ACLs,
 * sparse files, reparse points, POSIX unlink/rename, hard links */

static const char g_label[] = "LSIW";     /* the label GetVolumeInformation reports */

API NTSTATUS NtQueryVolumeInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, int cls)
{
    static const ULONG mins[] = { 0, 24, 0, 24, 8, 16, 0, 32, 0, 0, 0, 28 };
    if (cls <= 0 || cls >= 16) return STATUS_INVALID_INFO_CLASS;
    if (cls >= (int)(sizeof mins / sizeof mins[0]) || !mins[cls]) {
        /* Label (2, set-only), control/quota (6), object id (8), driver path
         * (9), volume flags (10), FullSizeEx (14): not provided. */
        return cls == 2 ? STATUS_INVALID_INFO_CLASS : STATUS_INVALID_DEVICE_REQUEST;
    }
    if (len < mins[cls]) return STATUS_INFO_LENGTH_MISMATCH;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    ULONG used = mins[cls];
    if (cls == 4) {                                      /* FileFsDeviceInformation */
        DWORD type, chars = 0;
        switch (c.kind) {
        case K_PIPE: case K_SOCK: type = 0x11; break;    /* FILE_DEVICE_NAMED_PIPE (AFD reports it too) */
        case K_NULL: type = 0x15; break;                 /* FILE_DEVICE_NULL */
        case K_CONSOLE: type = 0x50; break;              /* FILE_DEVICE_CONSOLE */
        case K_CHAR: type = 0x22; break;                 /* FILE_DEVICE_UNKNOWN */
        default: type = 0x07; chars = 0x20; break;       /* FILE_DEVICE_DISK, FILE_DEVICE_IS_MOUNTED */
        }
        p32(buf, 0, type);
        p32(buf, 4, chars);
    } else {
        if (!is_fs(&c)) return STATUS_INVALID_DEVICE_REQUEST;
        struct statvfs sv;
        if (fstatvfs(c.fd, &sv) < 0) memset(&sv, 0, sizeof sv);
        uint64_t cluster = sv.f_frsize ? sv.f_frsize : sv.f_bsize ? sv.f_bsize : 4096;
        uint32_t sector = cluster % 512 == 0 ? 512 : (uint32_t)cluster;
        switch (cls) {
        case 1: {                                        /* FileFsVolumeInformation */
            p64(buf, 0, 0);
            p32(buf, 8, (DWORD)c.x.dev);                 /* same serial as GetFileInformationByHandle */
            p32(buf, 12, (sizeof g_label - 1) * 2);
            ((BYTE *)buf)[16] = 0;
            ((BYTE *)buf)[17] = 0;
            ULONG room = (len - 18) / 2, n = sizeof g_label - 1;
            for (ULONG i = 0; i < n && i < room; i++) p16(buf, 18 + 2 * (size_t)i, (uint16_t)g_label[i]);
            if (room < n) { st = STATUS_BUFFER_OVERFLOW; used = 18 + room * 2; }
            else used = 18 + n * 2;
            break; }
        case 3:                                          /* FileFsSizeInformation */
            p64(buf, 0, sv.f_blocks);
            p64(buf, 8, sv.f_bavail);
            p32(buf, 16, (uint32_t)(cluster / sector));
            p32(buf, 20, sector);
            break;
        case 5: {                                        /* FileFsAttributeInformation */
            static const char fs[] = "NTFS";
            p32(buf, 0, FS_FLAGS);
            p32(buf, 4, sv.f_namemax ? (uint32_t)sv.f_namemax : 255);
            p32(buf, 8, (sizeof fs - 1) * 2);
            ULONG room = (len - 12) / 2, n = sizeof fs - 1;
            for (ULONG i = 0; i < n && i < room; i++) p16(buf, 12 + 2 * (size_t)i, (uint16_t)fs[i]);
            if (room < n) { st = STATUS_BUFFER_OVERFLOW; used = 12 + room * 2; }
            else used = 12 + n * 2;
            break; }
        case 7:                                          /* FileFsFullSizeInformation */
            p64(buf, 0, sv.f_blocks);
            p64(buf, 8, sv.f_bavail);
            p64(buf, 16, sv.f_bfree);
            p32(buf, 24, (uint32_t)(cluster / sector));
            p32(buf, 28, sector);
            break;
        case 11:                                         /* FileFsSectorSizeInformation */
            p32(buf, 0, sector);
            p32(buf, 4, sector);
            p32(buf, 8, sector);
            p32(buf, 12, sector);
            p32(buf, 16, 0x1 | 0x2);                     /* SSINFO_FLAGS_ALIGNED_DEVICE | PARTITION_ALIGNED */
            p32(buf, 20, 0);
            p32(buf, 24, 0);
            break;
        }
    }
    if (iosb) { iosb->Status = st; iosb->Information = used; }
    return st;
}

API NTSTATUS NtSetVolumeInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, int cls)
{
    (void)buf; (void)len; (void)iosb;
    if (cls != 2 && cls != 6 && cls != 8 && cls != 10) return STATUS_INVALID_INFO_CLASS;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    /* The label of C: (the whole Linux file system) is fixed, and changing it
     * needs a volume handle opened by an administrator: ordinary file handles
     * get access denied. Quotas, object ids and volume flags are features
     * this volume does not have. */
    return cls == 2 ? STATUS_ACCESS_DENIED : STATUS_INVALID_DEVICE_REQUEST;
}

API NTSTATUS NtQueryQuotaInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, BOOL single,
                                         void *sids, ULONG sidlen, void *start, BOOL restart)
{
    (void)iosb; (void)buf; (void)len; (void)single; (void)sids; (void)sidlen; (void)start; (void)restart;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    /* A volume without quota support (no FILE_VOLUME_QUOTAS) rejects the request. */
    return st ? st : STATUS_INVALID_DEVICE_REQUEST;
}

API NTSTATUS NtSetQuotaInformationFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len)
{
    (void)iosb; (void)buf; (void)len;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    return st ? st : STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* NtQueryDirectoryFile(Ex)                                             */
/* ------------------------------------------------------------------ */

/* Name matching with FsRtlIsNameInExpression rules, ignoring case:
 *   *  any run of characters        ?  any one character
 *   <  (DOS_STAR) any run not reaching past the last '.'
 *   >  (DOS_QM)   any one character, or nothing at a '.' or the end
 *   "  (DOS_DOT)  a '.' or nothing at the end */
static int name_match(const WCHAR *p, size_t pl, const WCHAR *n, size_t nl)
{
    if (pl == 1 && p[0] == '*') return 1;
    size_t lastdot = nl;
    for (size_t i = nl; i-- > 0;) if (n[i] == '.') { lastdot = i; break; }
    size_t w = nl + 1;
    unsigned char *dp = calloc((pl + 1) * w, 1);
    if (!dp) return 0;
#define DP(i, j) dp[(size_t)(i) * w + (j)]
    DP(pl, nl) = 1;
    for (size_t i = pl; i-- > 0;) {
        for (size_t j = nl + 1; j-- > 0;) {
            WCHAR c = p[i];
            int r;
            switch (c) {
            case '*': r = DP(i + 1, j) || (j < nl && DP(i, j + 1)); break;
            case '?': r = j < nl && DP(i + 1, j + 1); break;
            case '<': r = DP(i + 1, j) || (j < nl && j < lastdot && DP(i, j + 1)); break;
            case '>': r = (j == nl || n[j] == '.') ? DP(i + 1, j) : DP(i + 1, j + 1); break;
            case '"': r = j == nl ? DP(i + 1, j) : (n[j] == '.' && DP(i + 1, j + 1)); break;
            default:  r = j < nl && upw(c) == upw(n[j]) && DP(i + 1, j + 1); break;
            }
            DP(i, j) = (unsigned char)r;
        }
    }
    int m = DP(0, 0);
#undef DP
    free(dp);
    return m;
}

/* Layout of each directory class: offsets of the name, its length, EaSize,
 * ShortName, the 64- or 128-bit FileId and ReparsePointTag (-1 = absent). */
struct dir_layout { int cls, name, namelen, ea, shortname, id, id128, tag; };
static const struct dir_layout g_layouts[] = {
    { FileDirectoryInformation,            64, 60, -1, -1, -1, 0, -1 },
    { FileFullDirectoryInformation,        68, 60, 64, -1, -1, 0, -1 },
    { FileBothDirectoryInformation,        94, 60, 64, 68, -1, 0, -1 },
    { FileNamesInformation,                12,  8, -1, -1, -1, 0, -1 },
    { FileIdBothDirectoryInformation,     104, 60, 64, 68, 96, 0, -1 },
    { FileIdFullDirectoryInformation,      80, 60, 64, -1, 72, 0, -1 },
    { FileIdExtdDirectoryInformation,      88, 60, 64, -1, 72, 1, 68 },
    { FileIdExtdBothDirectoryInformation, 114, 60, 64, 88, 72, 1, 68 },
};

/* Per-handle enumeration state lives in obj->pattern (released with the
 * handle; kernel32 uses that field only for FindFirstFile handles):
 *   [0] 0x01 marker  [1] phase '0' ".", '1' "..", '2' folder entries
 *   [2] '1' once an entry was returned  [3...] the UTF-8 search mask.
 * obj->dir is the open DIR stream (also closed with the handle). */
struct dir_entry { char name[NAME_MAX + 1]; struct xst x; DWORD attrs; long resume; int phase_next; };

static int next_entry(struct lsiw_handle_obj *o, struct dir_entry *e, int *phase)
{
    for (;;) {
        if (*phase == 0 || *phase == 1) {
            const char *nm = *phase == 0 ? "." : "..";
            e->phase_next = *phase + 1;
            e->resume = -1;
            /* The root folder of an NTFS volume has no "." and ".." entries. */
            if (!strcmp(o->path, "/")) { (*phase)++; continue; }
            if (xstat_at(dirfd((DIR *)o->dir), nm, 0, &e->x) < 0) { (*phase)++; continue; }
            strcpy(e->name, nm);
            e->attrs = attrs_of(nm, &e->x);
            return 1;
        }
        long pos = telldir((DIR *)o->dir);
        struct dirent *de = readdir((DIR *)o->dir);
        if (!de) return 0;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (xstat_at(dirfd((DIR *)o->dir), de->d_name, AT_SYMLINK_NOFOLLOW, &e->x) < 0) continue;
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char *full = lsiw_strdup_printf("%s/%s", o->path, de->d_name);
        e->attrs = attrs_of(full, &e->x);
        free(full);
        e->resume = pos;
        e->phase_next = 2;
        return 1;
    }
}

static NTSTATUS query_directory(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                                void *buf, ULONG len, int cls, ULONG qflags, const UNICODE_STRING *mask)
{
    const struct dir_layout *L = NULL;
    for (size_t i = 0; i < sizeof g_layouts / sizeof g_layouts[0]; i++) if (g_layouts[i].cls == cls) L = &g_layouts[i];
    if (!L) return STATUS_INVALID_INFO_CLASS;
    if (len < (ULONG)((L->name + 2 + 7) & ~7)) return STATUS_INFO_LENGTH_MISMATCH;
    if (!buf || !iosb) return ST(0xC0000005u);
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    if (!S_ISDIR(c.x.mode) || !c.o->path) return STATUS_INVALID_PARAMETER;
    event_set(event, 0);
    struct lsiw_handle_obj *o = c.o;
    int restart = (qflags & 1) != 0, single = (qflags & 2) != 0;
    int first = !o->pattern || o->pattern[0] != 1;
    if (first || restart) {
        char *m = NULL;
        if (mask && mask->Buffer && mask->Length) m = lsiw_w2u_n(mask->Buffer, mask->Length / 2);
        else if (!first) m = strdup(o->pattern + 3);      /* a restart keeps the first mask */
        char *s = lsiw_strdup_printf("\001" "00%s", m ? m : "");
        free(m);
        free(o->pattern);
        o->pattern = s;
        if (!o->dir) {
            int dfd = openat(c.fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd < 0 || !(o->dir = fdopendir(dfd))) {
                if (dfd >= 0) close(dfd);
                st = errno_st(errno);
                io_finish(h, event, apc, ctx, NULL, iosb, st, 0, 0);
                return st;
            }
        }
        rewinddir((DIR *)o->dir);
    }
    int phase = o->pattern[1] - '0';
    WCHAR *wmask = o->pattern[3] ? lsiw_u2w(o->pattern + 3) : NULL;
    size_t mlen = wmask ? lsiw_wcslen(wmask) : 0;
    char *out = buf;
    size_t used = 0, prev = (size_t)-1;
    st = STATUS_SUCCESS;
    struct dir_entry e;
    WCHAR wname[NAME_MAX + 2];
    while (next_entry(o, &e, &phase)) {
        int wl = lsiw_utf8_to_utf16(e.name, -1, wname, NAME_MAX + 2);
        if (wl > 0 && !wname[wl - 1]) wl--;
        if (wl < 0) wl = 0;
        if (wmask && !name_match(wmask, mlen, wname, (size_t)wl)) { phase = e.phase_next; continue; }
        size_t start = (used + 7) & ~(size_t)7;
        size_t need = (size_t)L->name + (size_t)wl * 2;
        size_t fit_name = (size_t)wl * 2;
        if (prev != (size_t)-1 && start + need > len) {
            if (e.resume >= 0) seekdir((DIR *)o->dir, e.resume);   /* hand it out next time */
            break;
        }
        if (prev == (size_t)-1 && need > len) {
            /* Not even one whole entry fits: fill what fits, keep the cursor. */
            fit_name = (len - (size_t)L->name) & ~(size_t)1;
            st = STATUS_BUFFER_OVERFLOW;
        }
        char *ent = out + start;
        memset(ent, 0, (size_t)L->name);
        DWORD tag = S_ISLNK(e.x.mode) ? IO_REPARSE_TAG_SYMLINK : 0;
        if (cls != FileNamesInformation) {
            p64(ent, 8, (uint64_t)e.x.btime);
            p64(ent, 16, (uint64_t)e.x.atime);
            p64(ent, 24, (uint64_t)e.x.mtime);
            p64(ent, 32, (uint64_t)e.x.ctime);
            p64(ent, 40, eof_size(&e.x));
            p64(ent, 48, alloc_size(&e.x));
            p32(ent, 56, e.attrs);
        }
        p32(ent, (size_t)L->namelen, (uint32_t)wl * 2);
        /* EaSize carries the reparse tag for reparse points (classes
         * without a ReparsePointTag field). */
        if (L->ea >= 0 && L->tag < 0) p32(ent, (size_t)L->ea, tag);
        if (L->tag >= 0) p32(ent, (size_t)L->tag, tag);
        if (L->id >= 0) p64(ent, (size_t)L->id, e.x.ino);   /* 128-bit ids: low half, rest zero */
        memcpy(ent + L->name, wname, fit_name);
        if (prev != (size_t)-1) p32(out, prev, (uint32_t)(start - prev));
        prev = start;
        used = start + (size_t)L->name + fit_name;
        if (st == STATUS_BUFFER_OVERFLOW) { if (e.resume >= 0) seekdir((DIR *)o->dir, e.resume); break; }
        phase = e.phase_next;
        if (single) break;
    }
    free(wmask);
    if (prev == (size_t)-1) {
        /* Nothing on a first pass: no such file; later: the end. */
        st = o->pattern[2] == '1' ? STATUS_NO_MORE_FILES : STATUS_NO_SUCH_FILE;
        used = 0;
    } else {
        p32(out, prev, 0);
        o->pattern[2] = '1';
    }
    o->pattern[1] = (char)('0' + phase);
    io_finish(h, event, apc, ctx, NULL, iosb, st, used, 0);
    return st;
}

API NTSTATUS NtQueryDirectoryFile(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                                  void *buf, ULONG len, int cls, BOOL single, UNICODE_STRING *mask, BOOL restart)
{
    ULONG fl = ((BYTE)restart ? 1 : 0) | ((BYTE)single ? 2 : 0);
    return query_directory(h, event, apc, ctx, iosb, buf, len, cls, fl, mask);
}

API NTSTATUS NtQueryDirectoryFileEx(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                                    void *buf, ULONG len, int cls, ULONG flags, UNICODE_STRING *mask)
{
    /* SL_RESTART_SCAN 1, SL_RETURN_SINGLE_ENTRY 2; SL_INDEX_SPECIFIED (4)
     * needs FileIndex positions, which NTFS does not support either. */
    if (flags & 4) return STATUS_INVALID_PARAMETER;
    return query_directory(h, event, apc, ctx, iosb, buf, len, cls, flags, mask);
}

/* ------------------------------------------------------------------ */
/* Pending requests (IOCTL_AFD_POLL) and cancellation                   */
/* ------------------------------------------------------------------ */

#define IOCTL_AFD_POLL 0x00012024u
#define AFD_POLL_RECEIVE           0x0001
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define AFD_POLL_SEND              0x0004
#define AFD_POLL_DISCONNECT        0x0008
#define AFD_POLL_ABORT             0x0010
#define AFD_POLL_LOCAL_CLOSE       0x0020
#define AFD_POLL_ACCEPT            0x0080
#define AFD_POLL_CONNECT_FAIL      0x0100

struct afd_entry { HANDLE h; struct lsiw_handle_obj *obj; int fd; ULONG events; int ignore_hup; };

struct pending {
    struct pending *next;
    HANDLE file;
    IO_STATUS_BLOCK *iosb;
    HANDLE event;
    PIO_APC_ROUTINE apc;
    void *ctx;
    HANDLE apc_thread;
    int cancel[2];
    /* AFD poll */
    int64_t timeout;                 /* as given */
    int64_t deadline_ms;             /* CLOCK_MONOTONIC ms, -1 = none */
    ULONG exclusive;
    ULONG n;
    struct afd_entry *ent;
    void *out;
    ULONG outlen;
};

static struct pending *g_pending;
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Poll all sockets once (wait up to `ms`): returns the number of handles
 * with events, written into p->out; -1 when the poll was cancelled. */
static int afd_poll_once(struct pending *p, int ms)
{
    struct pollfd *pf = calloc(p->n + 1, sizeof *pf);
    if (!pf) return 0;
    for (ULONG i = 0; i < p->n; i++) {
        struct afd_entry *a = &p->ent[i];
        short ev = 0;
        if (a->events & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT | AFD_POLL_ABORT)) ev |= POLLIN | POLLRDHUP;
        if (a->events & AFD_POLL_RECEIVE_EXPEDITED) ev |= POLLPRI;
        if (a->events & (AFD_POLL_SEND | AFD_POLL_CONNECT_FAIL)) ev |= POLLOUT;
        pf[i].fd = a->ignore_hup || a->fd < 0 ? -1 : a->fd;
        pf[i].events = ev;
    }
    pf[p->n].fd = p->cancel[0];
    pf[p->n].events = POLLIN;
    int r = poll(pf, p->n + 1, ms);
    int cancelled = r > 0 && (pf[p->n].revents & POLLIN);
    int count = 0;
    char *out = p->out;
    for (ULONG i = 0; i < p->n && !cancelled; i++) {
        struct afd_entry *a = &p->ent[i];
        ULONG got = 0;
        NTSTATUS est = STATUS_SUCCESS;
        struct lsiw_handle_obj *now = lsiw_handle_get(a->h, H_FILE);
        if (now != a->obj || !now || now->fd != a->fd) got |= AFD_POLL_LOCAL_CLOSE;   /* socket closed */
        else if (r > 0) {
            short re = pf[i].revents;
            if (re & POLLNVAL) got |= AFD_POLL_LOCAL_CLOSE;
            if (re & POLLIN) {
                int listening = 0;
                socklen_t l = sizeof listening;
                getsockopt(a->fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &l);
                if (listening) got |= AFD_POLL_ACCEPT;
                else {
                    char ch;
                    ssize_t k = recv(a->fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (k == 0) got |= AFD_POLL_DISCONNECT; else got |= AFD_POLL_RECEIVE;
                }
            }
            if (re & POLLRDHUP) got |= AFD_POLL_DISCONNECT;
            if (re & POLLPRI) got |= AFD_POLL_RECEIVE_EXPEDITED;
            if (re & POLLOUT) got |= AFD_POLL_SEND;
            if (re & (POLLERR | POLLHUP)) {
                int err = 0;
                socklen_t l = sizeof err;
                getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &err, &l);
                struct sockaddr_storage ss;
                socklen_t sl = sizeof ss;
                int connected = getpeername(a->fd, (struct sockaddr *)&ss, &sl) == 0;
                if (err && !connected) {
                    got = (got & ~(ULONG)AFD_POLL_SEND) | AFD_POLL_CONNECT_FAIL;
                    est = err == ECONNREFUSED ? STATUS_CONNECTION_REFUSED : errno_st(err);
                } else if (err || connected) got |= AFD_POLL_ABORT;
                else if (!(re & ~(POLLHUP | POLLOUT))) {
                    /* A fresh, unconnected socket polls as "hung up" on
                     * Linux; AFD reports nothing for it. */
                    a->ignore_hup = 1;
                    got &= ~(ULONG)AFD_POLL_SEND;
                }
                if (err == ECONNRESET) est = STATUS_CONNECTION_RESET;
            }
        }
        got &= a->events;
        if (!got) continue;
        if (16 + 16 * (size_t)(count + 1) <= p->outlen) {
            char *e = out + 16 + 16 * (size_t)count;
            memcpy(e, &a->h, sizeof a->h);
            p32(e, 8, got);
            p32(e, 12, (uint32_t)est);
        }
        count++;
    }
    free(pf);
    if (cancelled) return -1;
    if (count && p->outlen >= 16) {
        p64(out, 0, (uint64_t)p->timeout);
        p32(out, 8, (uint32_t)count);
        p32(out, 12, p->exclusive);
    }
    return count;
}

static void pending_free(struct pending *p)
{
    if (p->cancel[0] >= 0) close(p->cancel[0]);
    if (p->cancel[1] >= 0) close(p->cancel[1]);
    if (p->apc_thread) {
        BOOL (WINAPI *ch)(HANDLE) = k32("CloseHandle");
        if (ch) ch(p->apc_thread);
    }
    free(p->ent);
    free(p);
}

static void pending_unlink(struct pending *p)
{
    pthread_mutex_lock(&g_pending_lock);
    for (struct pending **pp = &g_pending; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    pthread_mutex_unlock(&g_pending_lock);
}

static void *afd_worker(void *arg)
{
    struct pending *p = arg;
    NTSTATUS st = STATUS_SUCCESS;
    int count;
    for (;;) {
        int slice = 200;     /* re-check closed sockets this often */
        if (p->deadline_ms >= 0) {
            int64_t left = p->deadline_ms - mono_ms();
            if (left <= 0) { count = 0; break; }
            if (left < slice) slice = (int)left;
        }
        count = afd_poll_once(p, slice);
        if (count < 0) { st = STATUS_CANCELLED; count = 0; break; }
        if (count > 0) break;
    }
    pending_unlink(p);
    ULONG_PTR info = 0;
    if (st == STATUS_SUCCESS) {
        if (!count && p->outlen >= 16) {          /* timed out: no handles */
            p64(p->out, 0, (uint64_t)p->timeout);
            p32(p->out, 8, 0);
            p32(p->out, 12, p->exclusive);
        }
        info = 16 + 16 * (ULONG_PTR)count;
        if (info > p->outlen) info = p->outlen;
    }
    io_finish(p->file, p->event, p->apc, p->ctx, p->apc_thread, p->iosb, st, info, 1);
    pending_free(p);
    return NULL;
}

/* IOCTL_AFD_POLL: wait for socket events, the primitive under libuv's
 * uv_poll and wepoll. Input and output are AFD_POLL_INFO:
 *   LONGLONG Timeout; ULONG NumberOfHandles; ULONG Exclusive;
 *   { HANDLE Handle; ULONG Events; NTSTATUS Status; } Handles[]. */
static NTSTATUS afd_poll(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                         const void *in, ULONG inlen, void *out, ULONG outlen)
{
    if (!in || inlen < 16 || !out || outlen < 16) return STATUS_INVALID_PARAMETER;
    ULONG n = g32(in, 8);
    if (!n || inlen < 16 + 16 * (uint64_t)n) return STATUS_INVALID_PARAMETER;
    struct pending *p = calloc(1, sizeof *p);
    if (!p) return STATUS_NO_MEMORY;
    p->cancel[0] = p->cancel[1] = -1;
    p->ent = calloc(n, sizeof *p->ent);
    if (!p->ent || pipe2(p->cancel, O_CLOEXEC | O_NONBLOCK) < 0) { pending_free(p); return STATUS_NO_MEMORY; }
    p->timeout = (int64_t)g64(in, 0);
    p->exclusive = g32(in, 12);
    p->n = n;
    for (ULONG i = 0; i < n; i++) {
        const char *e = (const char *)in + 16 + 16 * (size_t)i;
        memcpy(&p->ent[i].h, e, sizeof(HANDLE));
        p->ent[i].events = g32(e, 8);
        p->ent[i].obj = lsiw_handle_get(p->ent[i].h, H_FILE);
        p->ent[i].fd = p->ent[i].obj ? p->ent[i].obj->fd : -1;
    }
    /* Timeout: INT64_MAX infinite, negative relative, positive absolute (100 ns). */
    if (p->timeout == INT64_MAX) p->deadline_ms = -1;
    else if (p->timeout < 0) p->deadline_ms = mono_ms() + (-p->timeout) / 10000;
    else {
        int64_t rel = p->timeout - now_ft();
        p->deadline_ms = mono_ms() + (rel > 0 ? rel / 10000 : 0);
    }
    p->file = h; p->iosb = iosb; p->event = event; p->apc = apc; p->ctx = ctx;
    p->out = out; p->outlen = outlen;
    event_set(event, 0);
    /* Ready now (or a zero timeout): complete synchronously. */
    int count = afd_poll_once(p, 0);
    if (count > 0 || (p->deadline_ms >= 0 && p->deadline_ms <= mono_ms())) {
        if (!count) { p64(out, 0, (uint64_t)p->timeout); p32(out, 8, 0); p32(out, 12, p->exclusive); }
        ULONG_PTR info = 16 + 16 * (ULONG_PTR)(count > 0 ? count : 0);
        if (info > outlen) info = outlen;
        pending_free(p);
        io_finish(h, event, apc, ctx, NULL, iosb, STATUS_SUCCESS, info, 0);
        return STATUS_SUCCESS;
    }
    if (apc) {
        HANDLE (WINAPI *ot)(DWORD, BOOL, DWORD) = k32("OpenThread");
        if (ot) p->apc_thread = ot(0x0010 /* THREAD_SET_CONTEXT */, FALSE, lsiw_thread()->tid);
    }
    if (iosb) { iosb->Information = 0; iosb->Status = STATUS_PENDING; }
    pthread_mutex_lock(&g_pending_lock);
    p->next = g_pending;
    g_pending = p;
    pthread_mutex_unlock(&g_pending_lock);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int r = pthread_create(&t, &at, afd_worker, p);
    pthread_attr_destroy(&at);
    if (r) {
        pending_unlink(p);
        pending_free(p);
        if (iosb) iosb->Status = STATUS_NO_MEMORY;
        return STATUS_NO_MEMORY;
    }
    return STATUS_PENDING;
}

/* Cancel pending requests on `h` (all, or the one whose status block is
 * `which`). Returns how many were asked to stop. */
static int cancel_pending(HANDLE h, IO_STATUS_BLOCK *which)
{
    int n = 0;
    pthread_mutex_lock(&g_pending_lock);
    for (struct pending *p = g_pending; p; p = p->next) {
        if (p->file != h || (which && p->iosb != which)) continue;
        char b = 1;
        if (write(p->cancel[1], &b, 1) < 0) { /* already signalled */ }
        n++;
    }
    pthread_mutex_unlock(&g_pending_lock);
    return n;
}

API NTSTATUS NtCancelIoFile(HANDLE h, IO_STATUS_BLOCK *iosb)
{
    if (!lsiw_handle_get(h, H_FILE)) return bad_handle(h);
    cancel_pending(h, NULL);
    /* Succeeds whether or not anything was pending. */
    if (iosb) { iosb->Status = STATUS_SUCCESS; iosb->Information = 0; }
    return STATUS_SUCCESS;
}

API NTSTATUS NtCancelIoFileEx(HANDLE h, IO_STATUS_BLOCK *request, IO_STATUS_BLOCK *iosb)
{
    if (!lsiw_handle_get(h, H_FILE)) return bad_handle(h);
    if (!cancel_pending(h, request)) return STATUS_NOT_FOUND;
    if (iosb) { iosb->Status = STATUS_SUCCESS; iosb->Information = 0; }
    return STATUS_SUCCESS;
}

/* Synchronous requests here finish before returning, so another thread
 * never finds one to cancel. */
API NTSTATUS NtCancelSynchronousIoFile(HANDLE thread, IO_STATUS_BLOCK *request, IO_STATUS_BLOCK *iosb)
{
    (void)thread; (void)request; (void)iosb;
    return STATUS_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* NtFsControlFile / NtDeviceIoControlFile                              */
/* ------------------------------------------------------------------ */

#define FSCTL_IS_VOLUME_MOUNTED        0x00090028u
#define FSCTL_GET_COMPRESSION          0x0009003Cu
#define FSCTL_SET_COMPRESSION          0x0009C040u
#define FSCTL_SET_REPARSE_POINT        0x000900A4u
#define FSCTL_GET_REPARSE_POINT        0x000900A8u
#define FSCTL_DELETE_REPARSE_POINT     0x000900ACu
#define FSCTL_SET_SPARSE               0x000900C4u
#define FSCTL_SET_ZERO_DATA            0x000980C8u
#define FSCTL_QUERY_ALLOCATED_RANGES   0x000940CFu
#define FSCTL_PIPE_PEEK                0x0011400Cu

/* Symbolic link -> REPARSE_DATA_BUFFER (IO_REPARSE_TAG_SYMLINK):
 *   ULONG Tag; USHORT DataLength, Reserved;
 *   USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
 *   ULONG Flags (SYMLINK_FLAG_RELATIVE); WCHAR PathBuffer[]. */
static NTSTATUS get_reparse(const struct fctx *c, void *out, ULONG outlen, ULONG_PTR *info)
{
    if (!c->o->path || !S_ISLNK(c->x.mode)) return STATUS_NOT_A_REPARSE_POINT;
    if (outlen < 8) return STATUS_BUFFER_TOO_SMALL;
    char target[PATH_MAX];
    ssize_t n = readlinkat(c->fd, "", target, sizeof target - 1);
    if (n < 0) n = readlink(c->o->path, target, sizeof target - 1);
    if (n < 0) return errno_st(errno);
    target[n] = 0;
    char *print, *subst;
    int relative = target[0] != '/';
    if (relative) {
        for (char *p = target; *p; p++) if (*p == '/') *p = '\\';
        print = strdup(target);
        subst = strdup(target);
    } else {
        print = lsiw_linux_to_win(target);
        subst = lsiw_strdup_printf("\\??\\%s", print);
    }
    WCHAR *wp = lsiw_u2w(print), *ws = lsiw_u2w(subst);
    size_t lp = lsiw_wcslen(wp) * 2, ls = lsiw_wcslen(ws) * 2;
    size_t total = 20 + lp + ls;
    char *rb = calloc(1, total);
    p32(rb, 0, IO_REPARSE_TAG_SYMLINK);
    p16(rb, 4, (uint16_t)(total - 8));
    p16(rb, 8, (uint16_t)lp);              /* SubstituteNameOffset (after the print name) */
    p16(rb, 10, (uint16_t)ls);
    p16(rb, 12, 0);                        /* PrintNameOffset */
    p16(rb, 14, (uint16_t)lp);
    p32(rb, 16, relative ? 1 : 0);
    memcpy(rb + 20, wp, lp);
    memcpy(rb + 20 + lp, ws, ls);
    size_t copy = total < outlen ? total : outlen;
    memcpy(out, rb, copy);
    *info = copy;
    free(rb); free(wp); free(ws); free(print); free(subst);
    return copy < total ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* Keep the handle's descriptor number while making it refer to `path`
 * (other modules key their per-handle records on the number). */
static void reopen_as(struct lsiw_handle_obj *o, const char *path, int flags)
{
    int nfd = open(path, flags | O_CLOEXEC);
    if (nfd < 0) return;
    if (nfd != o->fd) { dup3(nfd, o->fd, O_CLOEXEC); close(nfd); }
}

/* Make the file or empty folder a symbolic link (symlink or junction data). */
static NTSTATUS set_reparse(const struct fctx *c, const void *in, ULONG inlen)
{
    if (!in || inlen < 8) return STATUS_IO_REPARSE_DATA_INVALID;
    ULONG tag = g32(in, 0);
    ULONG dlen = g32(in, 4) & 0xFFFF;
    if (8 + dlen > inlen) return STATUS_IO_REPARSE_DATA_INVALID;
    size_t pathbuf;
    int relative = 0;
    if (tag == IO_REPARSE_TAG_SYMLINK) { pathbuf = 20; if (dlen < 12) return STATUS_IO_REPARSE_DATA_INVALID; relative = g32(in, 16) & 1; }
    else if (tag == IO_REPARSE_TAG_MOUNT_POINT) { pathbuf = 16; if (dlen < 8) return STATUS_IO_REPARSE_DATA_INVALID; }
    else return STATUS_IO_REPARSE_TAG_MISMATCH;   /* other tags need their filter driver */
    const BYTE *b = in;
    size_t so = b[8] | b[9] << 8, sl = b[10] | b[11] << 8, po = b[12] | b[13] << 8, pl = b[14] | b[15] << 8;
    if (!sl) { so = po; sl = pl; }
    if (!sl || pathbuf + so + sl > 8 + (size_t)dlen) return STATUS_IO_REPARSE_DATA_INVALID;
    if (!c->o->path || !is_fs(c)) return STATUS_INVALID_PARAMETER;
    char *t = lsiw_w2u_n((const WCHAR *)(b + pathbuf + so), (int)(sl / 2));
    char *lt;
    if (relative) {
        lt = t;
        for (char *p = lt; *p; p++) if (*p == '\\') *p = '/';
    } else {
        const char *d = dos_part(t);
        if (!(d[0] && d[1] == ':')) { free(t); return STATUS_IO_REPARSE_DATA_INVALID; }
        lt = lsiw_win_to_linux(d);
        free(t);
    }
    NTSTATUS st;
    char *path = checked_path(c, &st);
    if (!path) { free(lt); return st; }
    struct stat ls;
    if (lstat(path, &ls) < 0) st = errno_st(errno);
    else if (S_ISDIR(ls.st_mode)) { if (rmdir(path) < 0) st = errno == ENOTEMPTY || errno == EEXIST ? STATUS_DIRECTORY_NOT_EMPTY : errno_st(errno); }
    else if (tag == IO_REPARSE_TAG_MOUNT_POINT) st = STATUS_NOT_A_DIRECTORY;   /* junctions need a folder */
    else if (S_ISLNK(ls.st_mode) || (S_ISREG(ls.st_mode) && ls.st_size == 0)) { if (unlink(path) < 0) st = errno_st(errno); }
    else st = STATUS_NOT_SUPPORTED;      /* a link cannot keep a file's data */
    if (!st) {
        if (symlink(lt, path) < 0) {
            st = errno_st(errno);
            if (S_ISDIR(ls.st_mode)) mkdir(path, ls.st_mode & 07777);
            else if (!S_ISLNK(ls.st_mode)) { int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, ls.st_mode & 07777); if (fd >= 0) close(fd); }
        } else reopen_as(c->o, path, O_PATH | O_NOFOLLOW);
    }
    free(lt);
    free(path);
    return st;
}

/* Remove the link, leaving an empty folder or file as Windows does. */
static NTSTATUS delete_reparse(const struct fctx *c, const void *in, ULONG inlen)
{
    if (!in || inlen < 8) return STATUS_IO_REPARSE_DATA_INVALID;
    if (!S_ISLNK(c->x.mode) || !c->o->path) return STATUS_NOT_A_REPARSE_POINT;
    ULONG tag = g32(in, 0);
    if (tag != IO_REPARSE_TAG_SYMLINK && tag != IO_REPARSE_TAG_MOUNT_POINT) return STATUS_IO_REPARSE_TAG_MISMATCH;
    struct stat ts;
    int dir = stat(c->o->path, &ts) == 0 && S_ISDIR(ts.st_mode);
    if (unlink(c->o->path) < 0) return errno_st(errno);
    if (dir) {
        if (mkdir(c->o->path, 0777) < 0) return errno_st(errno);
        reopen_as(c->o, c->o->path, O_RDONLY | O_DIRECTORY);
    } else {
        int fd = open(c->o->path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0666);
        if (fd < 0) return errno_st(errno);
        close(fd);
        reopen_as(c->o, c->o->path, O_RDWR);
    }
    return STATUS_SUCCESS;
}

/* FILE_ALLOCATED_RANGE_BUFFER { LONGLONG FileOffset, Length } list from
 * SEEK_DATA / SEEK_HOLE. */
static NTSTATUS allocated_ranges(const struct fctx *c, const void *in, ULONG inlen, void *out, ULONG outlen, ULONG_PTR *info)
{
    if (!in || inlen < 16) return STATUS_INVALID_PARAMETER;
    if (outlen < 16) return STATUS_BUFFER_TOO_SMALL;
    int64_t off = (int64_t)g64(in, 0), len = (int64_t)g64(in, 8);
    if (off < 0 || len < 0 || off > INT64_MAX - len) return STATUS_INVALID_PARAMETER;
    if (!S_ISREG(c->x.mode)) return STATUS_INVALID_PARAMETER;
    int64_t end = off + len;
    if (end > (int64_t)c->x.size) end = (int64_t)c->x.size;
    size_t n = 0, cap = outlen / 16;
    int64_t pos = off;
    NTSTATUS st = STATUS_SUCCESS;
    while (pos < end) {
        off_t d = lseek(c->fd, pos, SEEK_DATA), hole;
        if (d < 0) {
            if (errno == ENXIO) break;
            d = pos; hole = end;                /* no hole support: all data */
        } else {
            if (d >= end) break;
            hole = lseek(c->fd, d, SEEK_HOLE);
            if (hole < 0 || hole > end) hole = end;
        }
        if (n == cap) { st = STATUS_BUFFER_OVERFLOW; break; }
        p64(out, n * 16, (uint64_t)d);
        p64(out, n * 16 + 8, (uint64_t)(hole - d));
        n++;
        pos = hole;
    }
    *info = n * 16;
    return st;
}

static NTSTATUS zero_data(const struct fctx *c, const void *in, ULONG inlen)
{
    if (!in || inlen < 16) return STATUS_INVALID_PARAMETER;
    int64_t b = (int64_t)g64(in, 0), e = (int64_t)g64(in, 8);
    if (b < 0 || e < b) return STATUS_INVALID_PARAMETER;
    if (!S_ISREG(c->x.mode)) return STATUS_INVALID_PARAMETER;
    if (e > (int64_t)c->x.size) e = (int64_t)c->x.size;     /* never extends the file */
    if (e <= b) return STATUS_SUCCESS;
    if (fallocate(c->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, b, e - b) == 0) return STATUS_SUCCESS;
    if (errno == EBADF) return STATUS_ACCESS_DENIED;
    static const char zeros[65536];
    while (b < e) {
        size_t k = (size_t)(e - b) < sizeof zeros ? (size_t)(e - b) : sizeof zeros;
        ssize_t w = pwrite(c->fd, zeros, k, b);
        if (w < 0) { if (errno == EINTR) continue; return errno == EBADF ? STATUS_ACCESS_DENIED : errno_st(errno); }
        b += w;
    }
    return STATUS_SUCCESS;
}

/* FILE_PIPE_PEEK_BUFFER { ULONG NamedPipeState, ReadDataAvailable,
 * NumberOfMessages, MessageLength; CHAR Data[]; } */
static NTSTATUS pipe_peek(const struct fctx *c, void *out, ULONG outlen, ULONG_PTR *info)
{
    if (c->kind != K_PIPE) return STATUS_INVALID_DEVICE_REQUEST;
    if (!out || outlen < 16) return STATUS_BUFFER_TOO_SMALL;
    DWORD got = 0, avail = 0, left = 0, fl = 0;
    BOOL (WINAPI *peek)(HANDLE, void *, DWORD, DWORD *, DWORD *, DWORD *) = k32("PeekNamedPipe");
    BOOL (WINAPI *info_fn)(HANDLE, DWORD *, DWORD *, DWORD *, DWORD *) = k32("GetNamedPipeInfo");
    if (!peek) return STATUS_INVALID_DEVICE_REQUEST;
    if (!peek(c->h, (char *)out + 16, outlen - 16, &got, &avail, &left)) return win32_st(lsiw_get_last_error());
    int message = info_fn && info_fn(c->h, &fl, NULL, NULL, NULL) && (fl & 4);
    p32(out, 0, 3);                                   /* FILE_PIPE_CONNECTED_STATE */
    p32(out, 4, avail);
    p32(out, 8, message && avail ? 1 : 0);
    p32(out, 12, message ? got + left : 0);
    *info = 16 + (ULONG_PTR)got;
    return (message ? left : avail - got) && got < avail ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

static NTSTATUS io_control(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                           ULONG code, void *in, ULONG inlen, void *out, ULONG outlen)
{
    if (!iosb) return ST(0xC0000005u);
    if (code == IOCTL_AFD_POLL) {
        struct fctx c;
        NTSTATUS st = get_ctx(h, &c);
        if (st) return st;
        if (c.kind != K_SOCK) return STATUS_INVALID_DEVICE_REQUEST;
        return afd_poll(h, event, apc, ctx, iosb, in, inlen, out, outlen);
    }
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    event_set(event, 0);
    ULONG_PTR info = 0;
    switch (code) {
    case FSCTL_GET_REPARSE_POINT: st = get_reparse(&c, out, outlen, &info); break;
    case FSCTL_SET_REPARSE_POINT: st = set_reparse(&c, in, inlen); break;
    case FSCTL_DELETE_REPARSE_POINT: st = delete_reparse(&c, in, inlen); break;
    case FSCTL_SET_SPARSE:
        /* Linux files are always sparse-capable. */
        st = is_fs(&c) ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
        break;
    case FSCTL_QUERY_ALLOCATED_RANGES: st = allocated_ranges(&c, in, inlen, out, outlen, &info); break;
    case FSCTL_SET_ZERO_DATA: st = zero_data(&c, in, inlen); break;
    case FSCTL_IS_VOLUME_MOUNTED: st = is_fs(&c) ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST; break;
    case FSCTL_GET_COMPRESSION:
        if (!is_fs(&c)) { st = STATUS_INVALID_DEVICE_REQUEST; break; }
        if (!out || outlen < 2) { st = STATUS_INVALID_PARAMETER; break; }
        p16(out, 0, 0);                               /* COMPRESSION_FORMAT_NONE */
        info = 2;
        break;
    case FSCTL_SET_COMPRESSION:
        /* No FILE_FILE_COMPRESSION on this volume: only "none" is accepted. */
        if (!is_fs(&c)) st = STATUS_INVALID_DEVICE_REQUEST;
        else if (!in || inlen < 2) st = STATUS_INVALID_PARAMETER;
        else st = (((BYTE *)in)[0] | ((BYTE *)in)[1]) ? STATUS_INVALID_DEVICE_REQUEST : STATUS_SUCCESS;
        break;
    case FSCTL_PIPE_PEEK: st = pipe_peek(&c, out, outlen, &info); break;
    default: st = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    io_finish(h, event, apc, ctx, NULL, iosb, st, info, 0);
    return st;
}

API NTSTATUS NtDeviceIoControlFile(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                                   ULONG code, void *in, ULONG inlen, void *out, ULONG outlen)
{ return io_control(h, event, apc, ctx, iosb, code, in, inlen, out, outlen); }

API NTSTATUS NtFsControlFile(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                             ULONG code, void *in, ULONG inlen, void *out, ULONG outlen)
{ return io_control(h, event, apc, ctx, iosb, code, in, inlen, out, outlen); }

/* ------------------------------------------------------------------ */
/* Locks, EAs, flushing                                                 */
/* ------------------------------------------------------------------ */

/* Byte-range locks are open-file-description locks: owned by the handle
 * (and its duplicates), like Windows locks. Differences: Linux merges
 * overlapping locks of one handle instead of refusing them, unlocking a
 * range that is not locked succeeds, locks are advisory (reads and writes
 * are not refused), and a lock type the handle's access cannot hold is
 * taken as the type it can. */
static int lock_range(int fd, int type, uint64_t off, uint64_t len, int wait)
{
    if (!len || off > (uint64_t)INT64_MAX) return 0;     /* nothing Linux can represent */
    struct flock fl = { .l_type = (short)type, .l_whence = SEEK_SET, .l_start = (off_t)off, .l_len = 0 };
    if (len <= (uint64_t)INT64_MAX - off) fl.l_len = (off_t)len;   /* else: to the end of all offsets */
    int r;
    do r = fcntl(fd, wait ? F_OFD_SETLKW : F_OFD_SETLK, &fl); while (r < 0 && errno == EINTR);
    return r;
}

API NTSTATUS NtLockFile(HANDLE h, HANDLE event, PIO_APC_ROUTINE apc, void *ctx, IO_STATUS_BLOCK *iosb,
                        int64_t *offset, int64_t *length, ULONG key, BOOL fail_now, BOOL exclusive)
{
    (void)key;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    if (!offset || !length) return STATUS_INVALID_PARAMETER;
    if (S_ISDIR(c.x.mode) || !is_fs(&c)) return STATUS_INVALID_PARAMETER;
    event_set(event, 0);
    int acc = fcntl(c.fd, F_GETFL) & O_ACCMODE;
    int type = (BYTE)exclusive ? F_WRLCK : F_RDLCK;
    if (type == F_WRLCK && acc == O_RDONLY) type = F_RDLCK;
    if (type == F_RDLCK && acc == O_WRONLY) type = F_WRLCK;
    if (lock_range(c.fd, type, (uint64_t)*offset, (uint64_t)*length, !(BYTE)fail_now) < 0)
        st = (errno == EAGAIN || errno == EACCES || errno == EDEADLK) ? STATUS_LOCK_NOT_GRANTED : errno_st(errno);
    io_finish(h, event, apc, ctx, NULL, iosb, st, 0, 0);
    return st;
}

API NTSTATUS NtUnlockFile(HANDLE h, IO_STATUS_BLOCK *iosb, int64_t *offset, int64_t *length, ULONG key)
{
    (void)key;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    if (!offset || !length) return STATUS_INVALID_PARAMETER;
    if (lock_range(c.fd, F_UNLCK, (uint64_t)*offset, (uint64_t)*length, 0) < 0) st = errno_st(errno);
    if (iosb) { iosb->Status = st; iosb->Information = 0; }
    return st;
}

/* Extended attributes: the volume does not advertise
 * FILE_SUPPORTS_EXTENDED_ATTRIBUTES, so files have none and none can be set. */
API NTSTATUS NtQueryEaFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, BOOL single,
                           void *list, ULONG listlen, ULONG *index, BOOL restart)
{
    (void)buf; (void)len; (void)single; (void)list; (void)listlen; (void)index; (void)restart;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    if (st) return st;
    if (iosb) { iosb->Status = STATUS_NO_EAS_ON_FILE; iosb->Information = 0; }
    return STATUS_NO_EAS_ON_FILE;
}

API NTSTATUS NtSetEaFile(HANDLE h, IO_STATUS_BLOCK *iosb, void *buf, ULONG len)
{
    (void)iosb; (void)buf; (void)len;
    struct fctx c;
    NTSTATUS st = get_ctx(h, &c);
    return st ? st : STATUS_EAS_NOT_SUPPORTED;
}

static NTSTATUS flush_fd(HANDLE h, int data_only, IO_STATUS_BLOCK *iosb)
{
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) return bad_handle(h);
    NTSTATUS st = STATUS_SUCCESS;
    if ((data_only ? fdatasync(fd) : fsync(fd)) < 0 && errno != EINVAL && errno != EROFS) st = errno_st(errno);
    if (iosb) { iosb->Status = st; iosb->Information = 0; }
    return st;
}

API NTSTATUS ZwFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK *iosb) { return flush_fd(h, 0, iosb); }

API NTSTATUS NtFlushBuffersFileEx(HANDLE h, ULONG flags, void *params, ULONG plen, IO_STATUS_BLOCK *iosb)
{
    (void)params; (void)plen;
    /* FLUSH_FLAGS_FILE_DATA_ONLY 1, FLUSH_FLAGS_NO_SYNC 2, FLUSH_FLAGS_FILE_DATA_SYNC_ONLY 4 */
    if (flags & ~(ULONG)7) return STATUS_INVALID_PARAMETER;
    if (flags & 2) {
        if (lsiw_fd_from_handle(h) < 0) return bad_handle(h);
        if (iosb) { iosb->Status = STATUS_SUCCESS; iosb->Information = 0; }
        return STATUS_SUCCESS;
    }
    return flush_fd(h, (flags & 5) != 0, iosb);
}

/* ------------------------------------------------------------------ */
/* Queries and deletion by name                                         */
/* ------------------------------------------------------------------ */

static NTSTATUS stat_by_name(const OBJECT_ATTRIBUTES *oa, int follow, char **path, struct xst *x)
{
    NTSTATUS st;
    *path = oa_to_linux(oa, &st);
    if (!*path) return st;
    if (xstat_at(AT_FDCWD, *path, follow ? 0 : AT_SYMLINK_NOFOLLOW, x) < 0) {
        st = lookup_st(errno, *path);
        free(*path);
        *path = NULL;
        return st;
    }
    return STATUS_SUCCESS;
}

API NTSTATUS NtQueryAttributesFile(const OBJECT_ATTRIBUTES *oa, void *info)
{
    char *path;
    struct xst x;
    NTSTATUS st = stat_by_name(oa, 1, &path, &x);
    if (st) return st;
    struct fctx c = { 0 };
    struct lsiw_handle_obj fake = { 0 };
    fake.path = path;
    c.o = &fake; c.x = x;
    c.kind = S_ISDIR(x.mode) ? K_DIR : K_DISK;
    fill_basic(&c, info);
    free(path);
    return STATUS_SUCCESS;
}

API NTSTATUS NtQueryFullAttributesFile(const OBJECT_ATTRIBUTES *oa, void *info)
{
    char *path;
    struct xst x;
    NTSTATUS st = stat_by_name(oa, 1, &path, &x);
    if (st) return st;
    fill_network_open(&x, attrs_of(path, &x), info);
    free(path);
    return STATUS_SUCCESS;
}

/* NtQueryInformationByName: the classes Windows allows here. The final
 * reparse point is not followed (FILE_STAT_INFORMATION carries its tag). */
API NTSTATUS NtQueryInformationByName(const OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb, void *buf, ULONG len, int cls)
{
    ULONG need = cls == FileStatInformation ? 72 : cls == FileStatLxInformation ? 96 :
                 cls == FileCaseSensitiveInformation ? 4 : 0;
    if (!need) return STATUS_INVALID_INFO_CLASS;
    if (len < need) return STATUS_INFO_LENGTH_MISMATCH;
    char *path;
    struct xst x;
    NTSTATUS st = stat_by_name(oa, 0, &path, &x);
    if (st) return st;
    DWORD acc = FILE_GENERIC_READ | FILE_WRITE_ATTRIBUTES | DELETE_ACCESS;
    if (access(path, W_OK) == 0) acc |= FILE_GENERIC_WRITE;
    if (cls == FileCaseSensitiveInformation) p32(buf, 0, 0);
    else {
        fill_stat(&x, attrs_of(path, &x), acc, buf);
        if (cls == FileStatLxInformation) fill_stat_lx(&x, buf);
    }
    free(path);
    if (iosb) { iosb->Status = STATUS_SUCCESS; iosb->Information = need; }
    return STATUS_SUCCESS;
}

API NTSTATUS NtDeleteFile(const OBJECT_ATTRIBUTES *oa)
{
    char *path;
    struct xst x;
    NTSTATUS st = stat_by_name(oa, 0, &path, &x);
    if (st) return st;
    if (S_ISREG(x.mode) && !(x.mode & S_IWUSR)) st = STATUS_CANNOT_DELETE;
    else if ((S_ISDIR(x.mode) ? rmdir(path) : unlink(path)) < 0)
        st = errno == ENOTEMPTY || errno == EEXIST ? STATUS_DIRECTORY_NOT_EMPTY : errno_st(errno);
    free(path);
    return st;
}

/* ------------------------------------------------------------------ */
/* NtCreateNamedPipeFile                                                */
/* ------------------------------------------------------------------ */

/* The pipe name comes as \??\pipe\NAME, \Device\NamedPipe\NAME or NAME
 * relative to a handle on the pipe file system; an empty name (CreatePipe's
 * anonymous pipes) gets a unique one. The pipe itself is k32_extra.c's
 * named pipe (an AF_UNIX socket), made through CreateNamedPipeW. */
API NTSTATUS NtCreateNamedPipeFile(HANDLE *out, ULONG access, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb,
                                   ULONG share, ULONG disposition, ULONG options, ULONG type, ULONG readmode,
                                   ULONG completion, ULONG max_inst, ULONG inquota, ULONG outquota, int64_t *timeout)
{
    (void)access;
    if (!out || !oa || !iosb) return STATUS_INVALID_PARAMETER;
    if (disposition != 1 && disposition != 2 && disposition != 3) return STATUS_INVALID_PARAMETER;   /* OPEN, CREATE, OPEN_IF */
    if (readmode && !type) return STATUS_INVALID_PARAMETER;     /* message reads need a message pipe */
    *out = NULL;
    char *n = (oa->ObjectName && oa->ObjectName->Buffer)
            ? lsiw_w2u_n(oa->ObjectName->Buffer, oa->ObjectName->Length / 2) : strdup("");
    const char *base = n;
    if (!strncasecmp(base, "\\??\\pipe\\", 9)) base += 9;
    else if (!strncasecmp(base, "\\Device\\NamedPipe\\", 18)) base += 18;
    else if (!strncasecmp(base, "\\\\.\\pipe\\", 9)) base += 9;
    else if (base[0] == '\\' && !oa->RootDirectory) { free(n); return STATUS_OBJECT_NAME_INVALID; }
    static unsigned serial;
    char *full = *base ? lsiw_strdup_printf("\\\\.\\pipe\\%s", base)
                       : lsiw_strdup_printf("\\\\.\\pipe\\Win32Pipes.%08x.%08x", (unsigned)getpid(),
                                            __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    free(n);
    DWORD open_mode = ((share & 1) ? 0x2 : 0) | ((share & 2) ? 0x1 : 0);   /* OUTBOUND / INBOUND */
    if (!open_mode) open_mode = 0x3;
    if (!(options & 0x30)) open_mode |= 0x40000000;      /* not FILE_SYNCHRONOUS_IO_*: FILE_FLAG_OVERLAPPED */
    if (options & 0x2) open_mode |= 0x80000000;          /* FILE_WRITE_THROUGH */
    if (disposition == 2) open_mode |= 0x00080000;       /* FILE_CREATE: FILE_FLAG_FIRST_PIPE_INSTANCE */
    DWORD pipe_mode = (type ? 0x4 : 0) | (readmode ? 0x2 : 0) | (completion ? 0x1 : 0);
    DWORD ms = 50;
    if (timeout) ms = *timeout < 0 ? (DWORD)((-*timeout) / 10000) : 0;
    if (max_inst == 0 || max_inst > 255) max_inst = 255;  /* PIPE_UNLIMITED_INSTANCES */
    HANDLE (WINAPI *cnp)(const WCHAR *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void *) = k32("CreateNamedPipeW");
    WCHAR *w = lsiw_u2w(full);
    free(full);
    HANDLE h = cnp ? cnp(w, open_mode, pipe_mode, max_inst, outquota, inquota, ms, NULL) : INVALID_HANDLE_VALUE;
    free(w);
    NTSTATUS st;
    if (h == INVALID_HANDLE_VALUE || !h) {
        DWORD e = cnp ? lsiw_get_last_error() : ERROR_CALL_NOT_IMPLEMENTED;
        st = e == ERROR_ACCESS_DENIED && disposition == 2 ? STATUS_OBJECT_NAME_COLLISION : win32_st(e);
        iosb->Status = st; iosb->Information = 0;
        return st;
    }
    *out = h;
    iosb->Status = STATUS_SUCCESS;
    iosb->Information = 2;                                /* FILE_CREATED */
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Rtl DOS <-> NT path helpers                                          */
/* ------------------------------------------------------------------ */

enum { PT_UNKNOWN, PT_UNC_ABSOLUTE, PT_DRIVE_ABSOLUTE, PT_DRIVE_RELATIVE, PT_ROOTED,
       PT_RELATIVE, PT_LOCAL_DEVICE, PT_ROOT_LOCAL_DEVICE };

static int is_sep(WCHAR c) { return c == '\\' || c == '/'; }

API ULONG RtlDetermineDosPathNameType_U(const WCHAR *p)
{
    if (!p || !p[0]) return PT_RELATIVE;
    if (is_sep(p[0])) {
        if (!is_sep(p[1])) return PT_ROOTED;
        if ((p[2] == '.' || p[2] == '?')) {
            if (is_sep(p[3])) return PT_LOCAL_DEVICE;
            if (!p[3]) return PT_ROOT_LOCAL_DEVICE;
        }
        return PT_UNC_ABSOLUTE;
    }
    if (p[0] && p[1] == ':') return is_sep(p[2]) ? PT_DRIVE_ABSOLUTE : PT_DRIVE_RELATIVE;
    return PT_RELATIVE;
}

/* Reserved DOS device names stay devices in any folder ("C:\x\NUL.txt"). */
static const char *dos_device(const char *full)
{
    static const char *const names[] = { "NUL", "CON", "AUX", "PRN", "CONIN$", "CONOUT$" };
    const char *b = full;
    for (const char *s = full; *s; s++) if (*s == '\\' || *s == '/' || *s == ':') b = s + 1;
    size_t l = 0;
    while (b[l] && b[l] != '.' && b[l] != ' ' && b[l] != ':') l++;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i]) == l && !strncasecmp(b, names[i], l)) return names[i];
    if (l == 4 && (!strncasecmp(b, "COM", 3) || !strncasecmp(b, "LPT", 3)) && b[3] >= '1' && b[3] <= '9') {
        static char dev[5];
        memcpy(dev, b, 4); dev[4] = 0;
        for (int i = 0; i < 3; i++) dev[i] = (char)(dev[i] & ~0x20);
        return dev;
    }
    return NULL;
}

static NTSTATUS dos_to_nt(const WCHAR *dos, UNICODE_STRING *nt, WCHAR **file_part, void *relative)
{
    if (relative) memset(relative, 0, 32);   /* RTL_RELATIVE_NAME_U: not relative to a handle */
    if (!nt) return STATUS_INVALID_PARAMETER;
    nt->Buffer = NULL; nt->Length = nt->MaximumLength = 0;
    if (file_part) *file_part = NULL;
    if (!dos || !dos[0]) return STATUS_OBJECT_NAME_INVALID;
    char *u = lsiw_w2u(dos);
    char *res = NULL;
    if (!strncmp(u, "\\\\?\\", 4) || !strncmp(u, "\\??\\", 4)) {
        res = lsiw_strdup_printf("\\??\\%s", u + 4);         /* verbatim */
    } else {
        DWORD (WINAPI *gfp)(const WCHAR *, DWORD, WCHAR *, WCHAR **) = k32("GetFullPathNameW");
        WCHAR *full = NULL;
        DWORD n = gfp ? gfp(dos, 0, NULL, NULL) : 0;
        if (n) { full = malloc((size_t)n * 2 + 2); n = gfp(dos, n + 1, full, NULL); }
        if (!n) { free(full); free(u); return STATUS_OBJECT_NAME_INVALID; }
        char *f = lsiw_w2u(full);
        free(full);
        const char *dev = RtlDetermineDosPathNameType_U(dos) == PT_LOCAL_DEVICE ? NULL : dos_device(f);
        if (dev) res = lsiw_strdup_printf("\\??\\%s", dev);
        else if (!strncmp(f, "\\\\.\\", 4)) res = lsiw_strdup_printf("\\??\\%s", f + 4);
        else if (f[0] == '\\' && f[1] == '\\') res = lsiw_strdup_printf("\\??\\UNC\\%s", f + 2);
        else res = lsiw_strdup_printf("\\??\\%s", f);
        free(f);
    }
    free(u);
    WCHAR *w = lsiw_u2w(res);
    free(res);
    size_t l = lsiw_wcslen(w);
    if (l * 2 + 2 > 0xFFFF) { free(w); return STATUS_NAME_TOO_LONG; }
    /* The caller frees this with RtlFreeUnicodeString or RtlFreeHeap on the
     * process heap. */
    WCHAR *b = lsiw_heap_alloc(l * 2 + 2, 0);
    if (!b) { free(w); return STATUS_NO_MEMORY; }
    memcpy(b, w, l * 2 + 2);
    free(w);
    nt->Buffer = b;
    nt->Length = (WORD)(l * 2);
    nt->MaximumLength = (WORD)(l * 2 + 2);
    if (file_part) {
        WCHAR *last = NULL;
        for (WCHAR *p = b + 4; *p; p++) if (*p == '\\') last = p;
        *file_part = (last && last[1]) ? last + 1 : NULL;
    }
    return STATUS_SUCCESS;
}

API NTSTATUS RtlDosPathNameToNtPathName_U_WithStatus(const WCHAR *dos, UNICODE_STRING *nt, WCHAR **part, void *rel)
{ return dos_to_nt(dos, nt, part, rel); }
API BOOL RtlDosPathNameToNtPathName_U(const WCHAR *dos, UNICODE_STRING *nt, WCHAR **part, void *rel)
{ return (BYTE)(dos_to_nt(dos, nt, part, rel) == STATUS_SUCCESS); }
API NTSTATUS RtlDosPathNameToRelativeNtPathName_U_WithStatus(const WCHAR *dos, UNICODE_STRING *nt, WCHAR **part, void *rel)
{ return dos_to_nt(dos, nt, part, rel); }
API BOOL RtlDosPathNameToRelativeNtPathName_U(const WCHAR *dos, UNICODE_STRING *nt, WCHAR **part, void *rel)
{ return (BYTE)(dos_to_nt(dos, nt, part, rel) == STATUS_SUCCESS); }
API void RtlReleaseRelativeName(void *rel) { (void)rel; }   /* never holds a handle */

/* Returns the length in bytes without the NUL, or the size needed (with it). */
API ULONG RtlGetFullPathName_U(const WCHAR *name, ULONG size, WCHAR *buf, WCHAR **file_part)
{
    if (file_part) *file_part = NULL;
    if (!name || !name[0]) return 0;
    DWORD (WINAPI *gfp)(const WCHAR *, DWORD, WCHAR *, WCHAR **) = k32("GetFullPathNameW");
    if (!gfp) return 0;
    DWORD r = gfp(name, size / 2, buf, file_part);
    if (!r) return 0;
    return r * 2;   /* bytes without the NUL when it fit, else bytes needed with it */
}

/* ------------------------------------------------------------------ */

const struct lsiw_export lsiw_ntfile_exports[] = {
    E(NtQueryInformationFile), E(NtSetInformationFile),
    EX("ZwQueryInformationFile", NtQueryInformationFile), EX("ZwSetInformationFile", NtSetInformationFile),
    E(NtQueryVolumeInformationFile), E(NtSetVolumeInformationFile),
    EX("ZwQueryVolumeInformationFile", NtQueryVolumeInformationFile), EX("ZwSetVolumeInformationFile", NtSetVolumeInformationFile),
    E(NtQueryQuotaInformationFile), E(NtSetQuotaInformationFile),
    EX("ZwQueryQuotaInformationFile", NtQueryQuotaInformationFile), EX("ZwSetQuotaInformationFile", NtSetQuotaInformationFile),
    E(NtQueryDirectoryFile), E(NtQueryDirectoryFileEx),
    EX("ZwQueryDirectoryFile", NtQueryDirectoryFile), EX("ZwQueryDirectoryFileEx", NtQueryDirectoryFileEx),
    E(NtDeviceIoControlFile), E(NtFsControlFile),
    EX("ZwDeviceIoControlFile", NtDeviceIoControlFile), EX("ZwFsControlFile", NtFsControlFile),
    E(NtCancelIoFile), E(NtCancelIoFileEx), E(NtCancelSynchronousIoFile),
    EX("ZwCancelIoFile", NtCancelIoFile), EX("ZwCancelIoFileEx", NtCancelIoFileEx), EX("ZwCancelSynchronousIoFile", NtCancelSynchronousIoFile),
    E(NtLockFile), E(NtUnlockFile), EX("ZwLockFile", NtLockFile), EX("ZwUnlockFile", NtUnlockFile),
    E(NtQueryEaFile), E(NtSetEaFile), EX("ZwQueryEaFile", NtQueryEaFile), EX("ZwSetEaFile", NtSetEaFile),
    E(ZwFlushBuffersFile), E(NtFlushBuffersFileEx), EX("ZwFlushBuffersFileEx", NtFlushBuffersFileEx),
    E(NtQueryAttributesFile), E(NtQueryFullAttributesFile), E(NtQueryInformationByName), E(NtDeleteFile),
    EX("ZwQueryAttributesFile", NtQueryAttributesFile), EX("ZwQueryFullAttributesFile", NtQueryFullAttributesFile),
    EX("ZwQueryInformationByName", NtQueryInformationByName), EX("ZwDeleteFile", NtDeleteFile),
    E(NtCreateNamedPipeFile), EX("ZwCreateNamedPipeFile", NtCreateNamedPipeFile),
    E(RtlDosPathNameToNtPathName_U), E(RtlDosPathNameToNtPathName_U_WithStatus),
    E(RtlDosPathNameToRelativeNtPathName_U), E(RtlDosPathNameToRelativeNtPathName_U_WithStatus),
    E(RtlReleaseRelativeName), E(RtlGetFullPathName_U), E(RtlDetermineDosPathNameType_U),
    { NULL, NULL }
};
