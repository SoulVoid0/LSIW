/* C runtime I/O: low-level file descriptors (io.h), streams (stdio.h),
 * formatted I/O, directories (direct.h), file status (sys/stat.h),
 * file finding, path helpers and the console (conio.h).
 *
 * CRT descriptors wrap Win32 handles, which wrap Linux file descriptors.
 * Text mode writes "\r\n" for "\n" to regular files (not to terminals or
 * pipes) and drops the "\r" of "\r\n" when reading, as Windows does.
 * Wide-character modes (_O_U16TEXT, _O_U8TEXT, _O_WTEXT) write UTF-8 to the
 * terminal and to standard handles, UTF-16LE to other files.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "crt.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void *k32(const char *n) { return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), n); }

#define MS_EOF (-1)
#define WEOF16 0xFFFF

/* ================================================================== */
/* CRT file descriptors                                                 */
/* ================================================================== */

#define MAX_FDS 2048
#define WIDE_MODES (O_MS_WTEXT | O_MS_U16TEXT | O_MS_U8TEXT)

struct cfd {
    HANDLE h;
    int used;
    int mode;          /* O_MS_TEXT, O_MS_BINARY or a wide mode */
    int flags;         /* open flags */
    int is_std;
    int peek;          /* byte read ahead while handling "\r", or -1 */
    unsigned char pend[4]; int npend;   /* partial UTF-8 input (wide modes) */
    int odd;           /* pending odd byte of UTF-16 output, or -1 */
    WCHAR hi;          /* pending high surrogate of UTF-16 output */
};
static struct cfd g_fd[MAX_FDS];
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_fd_once = PTHREAD_ONCE_INIT;

static void fd_init(void)
{
    for (int i = 0; i < 3; i++) {
        g_fd[i].h = lsiw_std_handle(i);
        g_fd[i].used = g_fd[i].h != NULL;
        g_fd[i].mode = O_MS_TEXT;
        g_fd[i].flags = i == 0 ? O_MS_RDONLY : O_MS_WRONLY;
        g_fd[i].is_std = 1;
        g_fd[i].peek = -1;
        g_fd[i].odd = -1;
    }
}

static struct cfd *fdp(int fd)
{
    pthread_once(&g_fd_once, fd_init);
    if (fd < 0 || fd >= MAX_FDS || !g_fd[fd].used) return NULL;
    return &g_fd[fd];
}
static int badf(void) { crt_set_errno(MS_EBADF); lsiw_thread()->doserrno = ERROR_INVALID_HANDLE; return -1; }
static int hostfd(struct cfd *c) { return lsiw_fd_from_handle(c->h); }
static int mode_from_flags(int flags)
{
    if (flags & O_MS_U16TEXT) return O_MS_U16TEXT;
    if (flags & O_MS_U8TEXT) return O_MS_U8TEXT;
    if (flags & O_MS_WTEXT) return O_MS_WTEXT;
    if (flags & O_MS_BINARY) return O_MS_BINARY;
    if (flags & O_MS_TEXT) return O_MS_TEXT;
    return (crt_fmode & O_MS_BINARY) ? O_MS_BINARY : O_MS_TEXT;
}

static int fd_alloc_at(int start, HANDLE h, int flags)
{
    pthread_once(&g_fd_once, fd_init);
    pthread_mutex_lock(&g_fd_lock);
    for (int i = start; i < MAX_FDS; i++) {
        if (!g_fd[i].used) {
            memset(&g_fd[i], 0, sizeof g_fd[i]);
            g_fd[i].h = h;
            g_fd[i].used = 1;
            g_fd[i].mode = mode_from_flags(flags);
            g_fd[i].flags = flags;
            g_fd[i].peek = -1;
            g_fd[i].odd = -1;
            g_fd[i].is_std = i < 3;
            pthread_mutex_unlock(&g_fd_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_fd_lock);
    crt_set_errno(MS_EMFILE);
    return -1;
}

int crt_fd_from_handle(HANDLE h, int flags) { return fd_alloc_at(0, h, flags); }
HANDLE crt_fd_handle(int fd) { struct cfd *c = fdp(fd); return c ? c->h : INVALID_HANDLE_VALUE; }
int crt_fd_host(int fd) { struct cfd *c = fdp(fd); return c ? hostfd(c) : -1; }
int crt_fd_mode(int fd) { struct cfd *c = fdp(fd); return c ? c->mode : O_MS_TEXT; }

static int wide_to_utf8_target(struct cfd *c)
{
    int h = hostfd(c);
    return c->is_std || (c->mode & O_MS_U8TEXT) || (h >= 0 && isatty(h)) || h <= 2;
}

static ssize_t raw_read(struct cfd *c, char *buf, size_t n)
{
    if (!n) return 0;
    size_t got = 0;
    if (c->peek >= 0) { buf[0] = (char)c->peek; c->peek = -1; got = 1; if (n == 1) return 1; }
    int h = hostfd(c);
    ssize_t r;
    do r = read(h, buf + got, n - got); while (r < 0 && errno == EINTR);
    if (r < 0) return got ? (ssize_t)got : -1;
    return (ssize_t)got + r;
}

static int write_all(int h, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(h, (const char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)r;
    }
    return 0;
}

/* Text mode: drop the '\r' of every "\r\n". */
static ssize_t text_read(struct cfd *c, char *buf, size_t n)
{
    for (;;) {
        ssize_t r = raw_read(c, buf, n);
        if (r <= 0) return r;
        size_t o = 0;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\r') {
                if (i + 1 < r) { if (buf[i + 1] == '\n') continue; }
                else {
                    int h = hostfd(c);
                    struct pollfd pf = { h, POLLIN, 0 };
                    char nx;
                    if (poll(&pf, 1, isatty(h) ? 0 : -1) > 0 && read(h, &nx, 1) == 1) {
                        if (nx == '\n') { buf[o++] = '\n'; continue; }
                        c->peek = (unsigned char)nx;
                    }
                }
            }
            buf[o++] = buf[i];
        }
        if (o) return (ssize_t)o;
    }
}

/* Wide modes on the terminal/standard handles: UTF-8 in, UTF-16LE out. */
static ssize_t utf8_to_utf16_read(struct cfd *c, char *out, size_t n)
{
    size_t want = n / 2;
    if (want < 4) want = 4;
    unsigned char *tmp = malloc(want + 4);
    memcpy(tmp, c->pend, (size_t)c->npend);
    size_t have = (size_t)c->npend;
    c->npend = 0;
    ssize_t r = 0;
    if (have < 4) {
        r = (c->mode & O_MS_BINARY) ? raw_read(c, (char *)tmp + have, want / 2 - have / 2 + 1) : text_read(c, (char *)tmp + have, want / 2 + 1 > have ? want / 2 + 1 - have : 1);
        if (r < 0) { free(tmp); return -1; }
    }
    size_t total = have + (size_t)r, i = 0, o = 0;
    while (i < total && o + 4 <= n) {
        unsigned b = tmp[i];
        int l = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        if (i + (size_t)l > total) break;
        uint32_t cp = l == 1 ? b : l == 2 ? (b & 0x1F) : l == 3 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k < l; k++) cp = (cp << 6) | (tmp[i + (size_t)k] & 0x3F);
        if (l == 1 && b >= 0x80) cp = 0xFFFD;
        i += (size_t)l;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            uint16_t a = (uint16_t)(0xD800 + (cp >> 10)), d = (uint16_t)(0xDC00 + (cp & 0x3FF));
            memcpy(out + o, &a, 2); memcpy(out + o + 2, &d, 2); o += 4;
        } else { uint16_t u = (uint16_t)cp; memcpy(out + o, &u, 2); o += 2; }
    }
    c->npend = (int)(total - i < 4 ? total - i : 4);
    memcpy(c->pend, tmp + i, (size_t)c->npend);
    free(tmp);
    return (ssize_t)o;
}

int crt_read_fd(int fd, void *buf, unsigned n)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    if ((c->flags & 3) == O_MS_WRONLY) { crt_set_errno(MS_EBADF); return -1; }
    ssize_t r;
    if ((c->mode & WIDE_MODES) && wide_to_utf8_target(c)) r = utf8_to_utf16_read(c, buf, n);
    else if (c->mode == O_MS_TEXT) r = text_read(c, buf, n);
    else r = raw_read(c, buf, n);
    if (r < 0) { crt_set_errno_host(errno); return -1; }
    return (int)r;
}

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int write_wide_as_utf8(struct cfd *c, const unsigned char *p, size_t n)
{
    char *out = malloc(n * 2 + 8);
    size_t o = 0, i = 0;
    for (;;) {
        uint16_t u;
        if (c->odd >= 0) {
            if (i >= n) break;
            u = (uint16_t)(c->odd | (p[i] << 8));
            c->odd = -1;
            i++;
        } else {
            if (i + 1 >= n) { if (i < n) c->odd = p[i]; break; }
            u = (uint16_t)(p[i] | (p[i + 1] << 8));
            i += 2;
        }
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (c->hi) o += put_utf8(out + o, 0xFFFD);
            c->hi = u;
            continue;
        }
        uint32_t cp = u;
        if (u >= 0xDC00 && u <= 0xDFFF) {
            cp = c->hi ? 0x10000 + (((uint32_t)c->hi - 0xD800) << 10) + (u - 0xDC00u) : 0xFFFD;
            c->hi = 0;
        } else if (c->hi) {
            o += put_utf8(out + o, 0xFFFD);
            c->hi = 0;
        }
        o += put_utf8(out + o, cp);
    }
    int r = write_all(hostfd(c), out, o);
    free(out);
    return r;
}

/* Text mode writes "\r\n" for "\n" to regular files, as on Windows, so a
 * program that reads its own output back in binary mode sees what it wrote.
 * Terminals and pipes keep "\n" so output stays usable in Linux pipelines.
 * runtime.text_line_endings = lf turns the translation off for files too. */
static int text_crlf_target(struct cfd *c)
{
    static int setting = -1;
    if (setting < 0) setting = strcasecmp(lsiw_setting_get("runtime", "text_line_endings", "crlf"), "lf") != 0;
    if (!setting || c->is_std) return 0;
    struct stat st;
    return fstat(hostfd(c), &st) == 0 && S_ISREG(st.st_mode);
}

static int write_crlf(struct cfd *c, const void *buf, size_t n, int utf16)
{
    const unsigned char *p = buf;
    unsigned char *out = malloc(n * 2 + 2);
    if (!out) { errno = ENOMEM; return -1; }
    size_t o = 0;
    if (utf16) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            if (p[i] == '\n' && p[i + 1] == 0) { out[o++] = '\r'; out[o++] = 0; }
            out[o++] = p[i]; out[o++] = p[i + 1];
        }
        if (n & 1) out[o++] = p[n - 1];
    } else {
        for (size_t i = 0; i < n; i++) { if (p[i] == '\n') out[o++] = '\r'; out[o++] = p[i]; }
    }
    int r = write_all(hostfd(c), out, o);
    free(out);
    return r;
}

int crt_write_fd(int fd, const void *buf, unsigned n)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    if ((c->flags & 3) == O_MS_RDONLY && !c->is_std) { crt_set_errno(MS_EBADF); return -1; }
    int r;
    if ((c->mode & WIDE_MODES) && wide_to_utf8_target(c)) r = write_wide_as_utf8(c, buf, n);
    else if ((c->mode == O_MS_TEXT || (c->mode & WIDE_MODES)) && text_crlf_target(c)) r = write_crlf(c, buf, n, c->mode != O_MS_TEXT);
    else r = write_all(hostfd(c), buf, n);
    if (r < 0) { crt_set_errno_host(errno); return -1; }
    return (int)n;
}

/* ---- io.h ---- */

static int host_open_flags(int of)
{
    int f = O_CLOEXEC;
    switch (of & 3) { case O_MS_WRONLY: f |= O_WRONLY; break; case O_MS_RDWR: f |= O_RDWR; break; default: f |= O_RDONLY; }
    if (of & O_MS_APPEND) f |= O_APPEND;
    if (of & O_MS_CREAT) f |= O_CREAT;
    if (of & O_MS_TRUNC) f |= O_TRUNC;
    if (of & O_MS_EXCL) f |= O_EXCL;
    return f;
}

/* Special device names: NUL, CON, CONIN$, CONOUT$. */
static char *device_path(const char *win, int of)
{
    const char *b = win;
    if (!strncmp(b, "\\\\.\\", 4)) b += 4;
    if (!strcasecmp(b, "NUL") || !strcasecmp(b, "NUL:")) return strdup("/dev/null");
    if (!strcasecmp(b, "CONIN$")) return strdup(isatty(0) ? "/dev/tty" : "/dev/stdin");
    if (!strcasecmp(b, "CONOUT$")) return strdup(isatty(1) ? "/dev/tty" : "/dev/stdout");
    if (!strcasecmp(b, "CON") || !strcasecmp(b, "CON:")) {
        if ((of & 3) == O_MS_RDONLY) return strdup(isatty(0) ? "/dev/tty" : "/dev/stdin");
        return strdup(isatty(1) ? "/dev/tty" : "/dev/stdout");
    }
    return NULL;
}

static int open_path_utf8(const char *path, int oflag, int pmode)
{
    if (!path) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
    char *lin = device_path(path, oflag);
    if (!lin) lin = lsiw_path_a(path);
    if (!lin) { crt_set_errno(MS_ENOENT); return -1; }
    struct stat st;
    if (stat(lin, &st) == 0 && (S_ISDIR(st.st_mode) ||
        (S_ISREG(st.st_mode) && !(st.st_mode & S_IWUSR) && ((oflag & 3) != O_MS_RDONLY || (oflag & O_MS_TRUNC))))) {
        free(lin); crt_set_errno(MS_EACCES); return -1;    /* directories; writing a read-only file */
    }
    int mode = (pmode & 0x80 /* _S_IWRITE */) ? 0666 : 0444;
    if (!(oflag & O_MS_CREAT)) mode = 0666;
    int h = open(lin, host_open_flags(oflag), mode);
    if (h < 0) {
        int e = errno;
        free(lin);
        crt_set_errno_host(e == EISDIR ? EACCES : e);
        return -1;
    }
    if (oflag & O_MS_TEMPORARY) unlink(lin);
    HANDLE hd = lsiw_handle_from_fd(h, isatty(h));
    struct lsiw_handle_obj *o = lsiw_handle_get(hd, H_FILE);
    if (o) { o->path = lin; o->append = (oflag & O_MS_APPEND) != 0; } else free(lin);
    int fd = crt_fd_from_handle(hd, oflag);
    if (fd < 0) lsiw_handle_close(hd);
    return fd;
}

API int _open(const char *path, int oflag, ...)
{
    int pmode = 0x180;
    if (oflag & O_MS_CREAT) { ms_va_list ap; ms_va_start(ap, oflag); pmode = ms_va_arg(ap, int); ms_va_end(ap); }
    return open_path_utf8(path, oflag, pmode);
}
API int _wopen(const WCHAR *path, int oflag, ...)
{
    int pmode = 0x180;
    if (oflag & O_MS_CREAT) { ms_va_list ap; ms_va_start(ap, oflag); pmode = ms_va_arg(ap, int); ms_va_end(ap); }
    char *u = path ? lsiw_w2u(path) : NULL;
    int r = open_path_utf8(u, oflag, pmode);
    free(u);
    return r;
}
API int _sopen(const char *path, int oflag, int sh, ...)
{
    (void)sh;
    int pmode = 0x180;
    if (oflag & O_MS_CREAT) { ms_va_list ap; ms_va_start(ap, sh); pmode = ms_va_arg(ap, int); ms_va_end(ap); }
    return open_path_utf8(path, oflag, pmode);
}
API int _wsopen(const WCHAR *path, int oflag, int sh, ...)
{
    (void)sh;
    int pmode = 0x180;
    if (oflag & O_MS_CREAT) { ms_va_list ap; ms_va_start(ap, sh); pmode = ms_va_arg(ap, int); ms_va_end(ap); }
    char *u = path ? lsiw_w2u(path) : NULL;
    int r = open_path_utf8(u, oflag, pmode);
    free(u);
    return r;
}
API int _sopen_s(int *pfh, const char *path, int oflag, int sh, int pmode)
{
    (void)sh;
    if (!pfh) { crt_invalid_parameter(); return MS_EINVAL; }
    *pfh = open_path_utf8(path, oflag, pmode);
    return *pfh < 0 ? *crt_errno() : 0;
}
API int _wsopen_s(int *pfh, const WCHAR *path, int oflag, int sh, int pmode)
{
    (void)sh;
    if (!pfh) { crt_invalid_parameter(); return MS_EINVAL; }
    char *u = path ? lsiw_w2u(path) : NULL;
    *pfh = open_path_utf8(u, oflag, pmode);
    free(u);
    return *pfh < 0 ? *crt_errno() : 0;
}
API int _sopen_dispatch(const char *path, int oflag, int sh, int pmode, int *pfh, int secure)
{ (void)secure; return _sopen_s(pfh, path, oflag, sh, pmode); }
API int _wsopen_dispatch(const WCHAR *path, int oflag, int sh, int pmode, int *pfh, int secure)
{ (void)secure; return _wsopen_s(pfh, path, oflag, sh, pmode); }
API int _creat(const char *path, int pmode) { return open_path_utf8(path, O_MS_CREAT | O_MS_TRUNC | O_MS_WRONLY, pmode); }
API int _wcreat(const WCHAR *path, int pmode)
{ char *u = lsiw_w2u(path); int r = open_path_utf8(u, O_MS_CREAT | O_MS_TRUNC | O_MS_WRONLY, pmode); free(u); return r; }

API int _close(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    pthread_mutex_lock(&g_fd_lock);
    HANDLE h = c->h;
    c->used = 0;
    c->h = NULL;
    pthread_mutex_unlock(&g_fd_lock);
    if (fd < 3 && lsiw_std_handle(fd) == h) lsiw_std_handle_set(fd, NULL);
    lsiw_handle_close(h);
    return 0;
}

API int _read(int fd, void *buf, unsigned n) { return crt_read_fd(fd, buf, n); }
API int _write(int fd, const void *buf, unsigned n) { return crt_write_fd(fd, buf, n); }

static int64_t seek_fd(int fd, int64_t off, int whence)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    if (whence < 0 || whence > 2) { crt_set_errno(MS_EINVAL); return -1; }
    if (c->peek >= 0 && whence == SEEK_CUR) off -= 1;
    c->peek = -1;
    c->npend = 0;
    off_t r = lseek(hostfd(c), (off_t)off, whence);
    if (r < 0) { crt_set_errno_host(errno); return -1; }
    return (int64_t)r;
}
API int32_t _lseek(int fd, int32_t off, int whence)
{
    int64_t r = seek_fd(fd, off, whence);
    if (r > INT32_MAX) { crt_set_errno(MS_EINVAL); return -1; }
    return (int32_t)r;
}
API int64_t _lseeki64(int fd, int64_t off, int whence) { return seek_fd(fd, off, whence); }
API int32_t _tell(int fd) { return _lseek(fd, 0, SEEK_CUR); }
API int64_t _telli64(int fd) { return seek_fd(fd, 0, SEEK_CUR); }
API int _eof(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    if (c->peek >= 0) return 0;
    struct stat st;
    int h = hostfd(c);
    if (fstat(h, &st) < 0) { crt_set_errno_host(errno); return -1; }
    if (!S_ISREG(st.st_mode)) return 0;
    off_t cur = lseek(h, 0, SEEK_CUR);
    return cur >= st.st_size;
}
API int64_t _filelengthi64(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    struct stat st;
    if (fstat(hostfd(c), &st) < 0) { crt_set_errno_host(errno); return -1; }
    return st.st_size;
}
API int32_t _filelength(int fd) { int64_t r = _filelengthi64(fd); return r > INT32_MAX ? -1 : (int32_t)r; }
API int _chsize_s(int fd, int64_t size)
{
    struct cfd *c = fdp(fd);
    if (!c) { badf(); return MS_EBADF; }
    if (size < 0) { crt_set_errno(MS_EINVAL); return MS_EINVAL; }
    if (ftruncate(hostfd(c), (off_t)size) < 0) { crt_set_errno_host(errno); return *crt_errno(); }
    return 0;
}
API int _chsize(int fd, int32_t size) { return _chsize_s(fd, size) ? -1 : 0; }
API int _commit(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    if (fsync(hostfd(c)) < 0 && errno != EINVAL && errno != EROFS) { crt_set_errno_host(errno); return -1; }
    return 0;
}

API int _dup(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    int nh = fcntl(hostfd(c), F_DUPFD_CLOEXEC, 3);
    if (nh < 0) { crt_set_errno_host(errno); return -1; }
    HANDLE h = lsiw_handle_from_fd(nh, isatty(nh));
    int nfd = crt_fd_from_handle(h, c->flags);
    if (nfd < 0) { lsiw_handle_close(h); return -1; }
    g_fd[nfd].mode = c->mode;
    return nfd;
}

API int _dup2(int fd1, int fd2)
{
    struct cfd *c = fdp(fd1);
    if (!c || fd2 < 0 || fd2 >= MAX_FDS) return badf();
    if (fd1 == fd2) return 0;
    if (fdp(fd2)) {
        if (fd2 < 3) lsiw_crt_flush_all();
        _close(fd2);
    }
    int nh;
    if (fd2 < 3) {
        /* Standard descriptors: redirect the real Linux descriptor too, so
         * child processes inherit the redirection. */
        nh = dup2(hostfd(c), fd2);
    } else nh = fcntl(hostfd(c), F_DUPFD_CLOEXEC, 3);
    if (nh < 0) { crt_set_errno_host(errno); return -1; }
    HANDLE h = lsiw_handle_from_fd(nh, isatty(nh));
    pthread_mutex_lock(&g_fd_lock);
    memset(&g_fd[fd2], 0, sizeof g_fd[fd2]);
    g_fd[fd2].h = h;
    g_fd[fd2].used = 1;
    g_fd[fd2].mode = c->mode;
    g_fd[fd2].flags = c->flags;
    g_fd[fd2].is_std = fd2 < 3;
    g_fd[fd2].peek = -1;
    g_fd[fd2].odd = -1;
    pthread_mutex_unlock(&g_fd_lock);
    if (fd2 < 3) lsiw_std_handle_set(fd2, h);
    return 0;
}

API int _pipe(int *pfds, unsigned size, int mode)
{
    (void)size;
    int p[2];
    if (pipe2(p, (mode & O_MS_NOINHERIT) ? O_CLOEXEC : 0) < 0) { crt_set_errno_host(errno); return -1; }
    HANDLE r = lsiw_handle_from_fd(p[0], 0), w = lsiw_handle_from_fd(p[1], 0);
    pfds[0] = crt_fd_from_handle(r, O_MS_RDONLY | (mode & ~3));
    pfds[1] = crt_fd_from_handle(w, O_MS_WRONLY | (mode & ~3));
    return 0;
}

API int _isatty(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) { badf(); return 0; }
    return isatty(hostfd(c)) ? 0x40 : 0;
}
API intptr_t _get_osfhandle(int fd)
{
    struct cfd *c = fdp(fd);
    if (!c) { badf(); return -1; }
    return (intptr_t)c->h;
}
API int _open_osfhandle(intptr_t h, int flags)
{
    if (!lsiw_handle_get((HANDLE)h, -1)) { crt_set_errno(MS_EBADF); return -1; }
    int fd = crt_fd_from_handle((HANDLE)h, flags);
    /* The CRT does not check the access mode of a wrapped handle (callers pass
     * just O_BINARY for a write handle); ReadFile/WriteFile decide. */
    struct cfd *c = fd >= 0 ? fdp(fd) : NULL;
    if (c) c->flags = (c->flags & ~3) | O_MS_RDWR;
    return fd;
}
API int _setmode(int fd, int mode)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    int m = mode & (O_MS_TEXT | O_MS_BINARY | WIDE_MODES);
    if (!m || (m & (m - 1))) { crt_set_errno(MS_EINVAL); return -1; }
    int old = c->mode;
    c->mode = m;
    return old;
}

API int _locking(int fd, int mode, int32_t nbytes)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_whence = SEEK_CUR;
    fl.l_start = 0;
    fl.l_len = nbytes;
    int cmd = F_SETLK;
    switch (mode) {
    case 0: fl.l_type = F_UNLCK; break;                      /* _LK_UNLCK  */
    case 1: case 3: fl.l_type = mode == 3 ? F_RDLCK : F_WRLCK;  /* _LK_LOCK / _LK_RLCK: retry 10 times */
        for (int i = 0; i < 10; i++) { if (fcntl(hostfd(c), F_SETLK, &fl) == 0) return 0; sleep(1); }
        crt_set_errno(MS_EDEADLK); return -1;
    case 2: fl.l_type = F_WRLCK; break;                      /* _LK_NBLCK  */
    case 4: fl.l_type = F_RDLCK; break;                      /* _LK_NBRLCK */
    default: crt_set_errno(MS_EINVAL); return -1;
    }
    if (fcntl(hostfd(c), cmd, &fl) < 0) { crt_set_errno(MS_EACCES); return -1; }
    return 0;
}

/* ---- file-system functions taking paths ---- */

static char *lin_a(const char *p) { return p ? lsiw_path_a(p) : NULL; }
static char *lin_w(const WCHAR *p) { return p ? lsiw_path_w(p) : NULL; }

static int access_lin(char *lin, int mode)
{
    if (!lin) { crt_set_errno(MS_EINVAL); return -1; }
    struct stat st;
    int r = stat(lin, &st);
    if (r < 0) { crt_set_errno_host(errno); free(lin); return -1; }
    if ((mode & 2) && !S_ISDIR(st.st_mode) && (!(st.st_mode & S_IWUSR) || access(lin, W_OK) < 0)) { crt_set_errno(MS_EACCES); free(lin); return -1; }
    free(lin);
    return 0;
}
API int _access(const char *p, int m) { return access_lin(lin_a(p), m); }
API int _waccess(const WCHAR *p, int m) { return access_lin(lin_w(p), m); }
API int _access_s(const char *p, int m) { return _access(p, m) ? *crt_errno() : 0; }
API int _waccess_s(const WCHAR *p, int m) { return _waccess(p, m) ? *crt_errno() : 0; }

static int chmod_lin(char *lin, int pmode)
{
    struct stat st;
    if (!lin || stat(lin, &st) < 0) { crt_set_errno_host(lin ? errno : EINVAL); free(lin); return -1; }
    mode_t m = st.st_mode & 07777;
    if (pmode & 0x80) m |= (S_IWUSR | ((m & S_IRGRP) ? S_IWGRP : 0)); else m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH);
    int r = chmod(lin, m);
    if (r < 0) crt_set_errno_host(errno);
    free(lin);
    return r < 0 ? -1 : 0;
}
API int _chmod(const char *p, int m) { return chmod_lin(lin_a(p), m); }
API int _wchmod(const WCHAR *p, int m) { return chmod_lin(lin_w(p), m); }
static int g_umask;
API int _umask(int m) { int o = g_umask; g_umask = m & 0x180; return o; }
API int _umask_s(int m, int *old) { if (old) *old = g_umask; g_umask = m & 0x180; return 0; }

static int unlink_lin(char *lin)
{
    if (!lin) { crt_set_errno(MS_EINVAL); return -1; }
    struct stat st;
    if (lstat(lin, &st) == 0 && (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && !(st.st_mode & S_IWUSR)))) { free(lin); crt_set_errno(MS_EACCES); return -1; }
    int r = unlink(lin);
    if (r < 0) crt_set_errno_host(errno);
    free(lin);
    return r < 0 ? -1 : 0;
}
API int _unlink(const char *p) { return unlink_lin(lin_a(p)); }
API int _wunlink(const WCHAR *p) { return unlink_lin(lin_w(p)); }
API int ms_remove(const char *p) { return unlink_lin(lin_a(p)); }
API int _wremove(const WCHAR *p) { return unlink_lin(lin_w(p)); }

static int rename_lin(char *a, char *b)
{
    int r = -1;
    struct stat st;
    if (!a || !b) crt_set_errno(MS_EINVAL);
    else if (lstat(b, &st) == 0) crt_set_errno(MS_EACCES);     /* Windows: target must not exist */
    else if (rename(a, b) < 0) crt_set_errno_host(errno == EXDEV ? EACCES : errno);
    else r = 0;
    free(a); free(b);
    return r;
}
API int ms_rename(const char *a, const char *b) { return rename_lin(lin_a(a), lin_a(b)); }
API int _wrename(const WCHAR *a, const WCHAR *b) { return rename_lin(lin_w(a), lin_w(b)); }

static int mkdir_lin(char *lin) { int r = lin ? mkdir(lin, 0777) : -1; if (r < 0) crt_set_errno_host(lin ? errno : EINVAL); free(lin); return r < 0 ? -1 : 0; }
static int rmdir_lin(char *lin) { int r = lin ? rmdir(lin) : -1; if (r < 0) crt_set_errno_host(lin ? (errno == EEXIST ? ENOTEMPTY : errno) : EINVAL); free(lin); return r < 0 ? -1 : 0; }
static int chdir_lin(char *lin) { int r = lin ? chdir(lin) : -1; if (r < 0) crt_set_errno_host(lin ? errno : EINVAL); free(lin); return r < 0 ? -1 : 0; }
API int _mkdir(const char *p) { return mkdir_lin(lin_a(p)); }
API int _wmkdir(const WCHAR *p) { return mkdir_lin(lin_w(p)); }
API int _rmdir(const char *p) { return rmdir_lin(lin_a(p)); }
API int _wrmdir(const WCHAR *p) { return rmdir_lin(lin_w(p)); }
API int _chdir(const char *p) { return chdir_lin(lin_a(p)); }
API int _wchdir(const WCHAR *p) { return chdir_lin(lin_w(p)); }

static char *cwd_win(void)
{
    char *c = getcwd(NULL, 0);
    char *w = lsiw_linux_to_win(c ? c : "/");
    free(c);
    return w;
}
API char *_getcwd(char *buf, int size)
{
    char *w = cwd_win();
    size_t l = strlen(w) + 1;
    if (!buf) {
        size_t n = (size_t)size > l ? (size_t)size : l;
        buf = lsiw_heap_alloc(n, 0);
        memcpy(buf, w, l);
    } else if ((size_t)size < l) { free(w); crt_set_errno(MS_ERANGE); return NULL; }
    else memcpy(buf, w, l);
    free(w);
    return buf;
}
API WCHAR *_wgetcwd(WCHAR *buf, int size)
{
    char *w = cwd_win();
    WCHAR *ww = lsiw_u2w(w);
    free(w);
    size_t l = lsiw_wcslen(ww) + 1;
    if (!buf) {
        size_t n = (size_t)size > l ? (size_t)size : l;
        buf = lsiw_heap_alloc(n * 2, 0);
        memcpy(buf, ww, l * 2);
    } else if ((size_t)size < l) { free(ww); crt_set_errno(MS_ERANGE); return NULL; }
    else memcpy(buf, ww, l * 2);
    free(ww);
    return buf;
}
API char *_getdcwd(int drive, char *buf, int size) { (void)drive; return _getcwd(buf, size); }
API WCHAR *_wgetdcwd(int drive, WCHAR *buf, int size) { (void)drive; return _wgetcwd(buf, size); }
API int _getdrive(void) { return 3; }
API int _chdrive(int d) { if (d == 3) return 0; crt_set_errno(MS_EACCES); return -1; }
API unsigned long _getdrives(void) { return 1u << 2; }
struct ms_diskfree { unsigned total_clusters, avail_clusters, sectors_per_cluster, bytes_per_sector; };
API unsigned _getdiskfree(unsigned drive, struct ms_diskfree *d)
{
    (void)drive;
    struct statvfs v;
    if (!d) return MS_EINVAL;
    char *cwd = getcwd(NULL, 0);
    int r = statvfs(cwd ? cwd : "/", &v);
    free(cwd);
    if (r < 0) return MS_EINVAL;
    unsigned long long bs = v.f_frsize ? v.f_frsize : v.f_bsize;
    d->bytes_per_sector = 512;
    d->sectors_per_cluster = (unsigned)(bs / 512 ? bs / 512 : 1);
    unsigned long long tot = v.f_blocks, av = v.f_bavail;
    d->total_clusters = tot > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)tot;
    d->avail_clusters = av > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)av;
    return 0;
}

/* ---- _fullpath, _splitpath, _makepath, _searchenv ---- */

API char *_fullpath(char *buf, const char *rel, size_t max)
{
    DWORD (WINAPI *gfp)(const char *, DWORD, char *, char **) = k32("GetFullPathNameA");
    char tmp[4096];
    DWORD n = gfp(rel && *rel ? rel : ".", sizeof tmp, tmp, NULL);
    if (!n || n >= sizeof tmp) { crt_set_errno(MS_ERANGE); return NULL; }
    if (!buf) { buf = lsiw_heap_alloc(n + 1 > max ? n + 1 : max, 0); max = n + 1 > max ? n + 1 : max; }
    if (n + 1 > max) { crt_set_errno(MS_ERANGE); return NULL; }
    memcpy(buf, tmp, n + 1);
    return buf;
}
API WCHAR *_wfullpath(WCHAR *buf, const WCHAR *rel, size_t max)
{
    static const WCHAR dot[] = { '.', 0 };
    DWORD (WINAPI *gfp)(const WCHAR *, DWORD, WCHAR *, WCHAR **) = k32("GetFullPathNameW");
    WCHAR tmp[4096];
    DWORD n = gfp(rel && *rel ? rel : dot, 4096, tmp, NULL);
    if (!n || n >= 4096) { crt_set_errno(MS_ERANGE); return NULL; }
    if (!buf) { max = n + 1 > max ? n + 1 : max; buf = lsiw_heap_alloc(max * 2, 0); }
    if (n + 1 > max) { crt_set_errno(MS_ERANGE); return NULL; }
    memcpy(buf, tmp, (n + 1) * 2);
    return buf;
}

static void copy_part(char *dst, size_t dn, const char *s, size_t l)
{
    if (!dst) return;
    if (dn != (size_t)-1 && l >= dn) l = dn ? dn - 1 : 0;
    memcpy(dst, s, l);
    dst[l] = 0;
}
static void split_path(const char *p, char *drive, size_t dn, char *dir, size_t dirn, char *fname, size_t fn, char *ext, size_t en)
{
    const char *s = p;
    if (s[0] && s[1] == ':') { copy_part(drive, dn, s, 2); s += 2; } else copy_part(drive, dn, "", 0);
    const char *last = NULL, *dot = NULL;
    for (const char *q = s; *q; q++) {
        if (*q == '\\' || *q == '/') { last = q; dot = NULL; }
        else if (*q == '.') dot = q;
    }
    if (last) { copy_part(dir, dirn, s, (size_t)(last - s + 1)); s = last + 1; } else copy_part(dir, dirn, "", 0);
    if (dot) { copy_part(fname, fn, s, (size_t)(dot - s)); copy_part(ext, en, dot, strlen(dot)); }
    else { copy_part(fname, fn, s, strlen(s)); copy_part(ext, en, "", 0); }
}
API void _splitpath(const char *p, char *drive, char *dir, char *fname, char *ext)
{ split_path(p, drive, (size_t)-1, dir, (size_t)-1, fname, (size_t)-1, ext, (size_t)-1); }
API int _splitpath_s(const char *p, char *drive, size_t dn, char *dir, size_t dirn, char *fname, size_t fn, char *ext, size_t en)
{
    if (!p) { crt_invalid_parameter(); return MS_EINVAL; }
    split_path(p, drive, dn, dir, dirn, fname, fn, ext, en);
    return 0;
}
static void wsplit(const WCHAR *p, WCHAR *drive, size_t dn, WCHAR *dir, size_t dirn, WCHAR *fname, size_t fn, WCHAR *ext, size_t en)
{
    char *u = lsiw_w2u(p);
    size_t l = strlen(u) + 1;
    char *d = malloc(l), *di = malloc(l), *f = malloc(l), *e = malloc(l);
    split_path(u, d, l, di, l, f, l, e, l);
    if (drive) { lsiw_u2w_buf(d, drive, dn == (size_t)-1 ? 3 : (int)dn); }
    if (dir) { lsiw_u2w_buf(di, dir, dirn == (size_t)-1 ? 260 : (int)dirn); }
    if (fname) { lsiw_u2w_buf(f, fname, fn == (size_t)-1 ? 260 : (int)fn); }
    if (ext) { lsiw_u2w_buf(e, ext, en == (size_t)-1 ? 260 : (int)en); }
    free(u); free(d); free(di); free(f); free(e);
}
API void _wsplitpath(const WCHAR *p, WCHAR *drive, WCHAR *dir, WCHAR *fname, WCHAR *ext)
{ wsplit(p, drive, (size_t)-1, dir, (size_t)-1, fname, (size_t)-1, ext, (size_t)-1); }
API int _wsplitpath_s(const WCHAR *p, WCHAR *drive, size_t dn, WCHAR *dir, size_t dirn, WCHAR *fname, size_t fn, WCHAR *ext, size_t en)
{
    if (!p) { crt_invalid_parameter(); return MS_EINVAL; }
    wsplit(p, drive, dn, dir, dirn, fname, fn, ext, en);
    return 0;
}

static void make_path(char *out, size_t n, const char *drive, const char *dir, const char *fname, const char *ext)
{
    size_t o = 0;
#define ADD(s) do { for (const char *q_ = (s); *q_ && o + 1 < n; q_++) out[o++] = *q_; } while (0)
    if (drive && *drive) { ADD(drive); if (drive[1] == 0 && o + 1 < n) out[o++] = ':'; }
    if (dir && *dir) { ADD(dir); char l = dir[strlen(dir) - 1]; if (l != '\\' && l != '/' && o + 1 < n) out[o++] = '\\'; }
    if (fname) ADD(fname);
    if (ext && *ext) { if (*ext != '.' && o + 1 < n) out[o++] = '.'; ADD(ext); }
#undef ADD
    out[o] = 0;
}
API void _makepath(char *out, const char *drive, const char *dir, const char *fname, const char *ext)
{ make_path(out, 260, drive, dir, fname, ext); }
API int _makepath_s(char *out, size_t n, const char *drive, const char *dir, const char *fname, const char *ext)
{ if (!out || !n) return MS_EINVAL; make_path(out, n, drive, dir, fname, ext); return 0; }
API void _wmakepath(WCHAR *out, const WCHAR *drive, const WCHAR *dir, const WCHAR *fname, const WCHAR *ext)
{
    char *a = drive ? lsiw_w2u(drive) : NULL, *b = dir ? lsiw_w2u(dir) : NULL, *c = fname ? lsiw_w2u(fname) : NULL, *d = ext ? lsiw_w2u(ext) : NULL;
    char tmp[1040];
    make_path(tmp, sizeof tmp, a, b, c, d);
    lsiw_u2w_buf(tmp, out, 260);
    free(a); free(b); free(c); free(d);
}
API int _wmakepath_s(WCHAR *out, size_t n, const WCHAR *drive, const WCHAR *dir, const WCHAR *fname, const WCHAR *ext)
{
    if (!out || !n) return MS_EINVAL;
    char *a = drive ? lsiw_w2u(drive) : NULL, *b = dir ? lsiw_w2u(dir) : NULL, *c = fname ? lsiw_w2u(fname) : NULL, *d = ext ? lsiw_w2u(ext) : NULL;
    char tmp[4096];
    make_path(tmp, sizeof tmp, a, b, c, d);
    lsiw_u2w_buf(tmp, out, (int)n);
    out[n - 1] = 0;
    free(a); free(b); free(c); free(d);
    return 0;
}

static int search_env(const char *file, const char *var, char *out, size_t n)
{
    out[0] = 0;
    char *lin = lsiw_path_a(file);
    if (lin && access(lin, F_OK) == 0) { free(lin); return _fullpath(out, file, n) ? 0 : MS_ERANGE; }
    free(lin);
    const char *v = lsiw_env_get(var);
    if (!v) { crt_set_errno(MS_ENOENT); return MS_ENOENT; }
    char *copy = strdup(v), *save = NULL;
    for (char *d = strtok_r(copy, ";", &save); d; d = strtok_r(NULL, ";", &save)) {
        char *w = lsiw_strdup_printf("%s%s%s", d, (d[strlen(d) - 1] == '\\' || d[strlen(d) - 1] == '/') ? "" : "\\", file);
        char *l = lsiw_path_a(w);
        int ok = l && access(l, F_OK) == 0;
        free(l);
        if (ok && strlen(w) < n) { strcpy(out, w); free(w); free(copy); return 0; }
        free(w);
    }
    free(copy);
    crt_set_errno(MS_ENOENT);
    return MS_ENOENT;
}
API void _searchenv(const char *file, const char *var, char *out) { search_env(file, var, out, 260); }
API int _searchenv_s(const char *file, const char *var, char *out, size_t n) { if (!out || !n) return MS_EINVAL; return search_env(file, var, out, n); }
API void _wsearchenv(const WCHAR *file, const WCHAR *var, WCHAR *out)
{
    char *f = lsiw_w2u(file), *v = lsiw_w2u(var), tmp[4096];
    search_env(f, v, tmp, sizeof tmp);
    lsiw_u2w_buf(tmp, out, 260);
    free(f); free(v);
}
API int _wsearchenv_s(const WCHAR *file, const WCHAR *var, WCHAR *out, size_t n)
{
    char *f = lsiw_w2u(file), *v = lsiw_w2u(var), tmp[4096];
    int r = search_env(f, v, tmp, sizeof tmp);
    if (!r) lsiw_u2w_buf(tmp, out, (int)n);
    free(f); free(v);
    return r;
}

/* ---- _mktemp ---- */

static int mktemp_lin(char *tmpl)
{
    size_t l = strlen(tmpl);
    if (l < 6 || strcmp(tmpl + l - 6, "XXXXXX")) { crt_set_errno(MS_EINVAL); return -1; }
    char *x = tmpl + l - 6;
    unsigned pid = (unsigned)getpid() % 100000;
    snprintf(x + 1, 6, "%05u", pid);
    for (char c = 'a'; c <= 'z'; c++) {
        x[0] = c;
        char *lin = lsiw_path_a(tmpl);
        int exists = lin && access(lin, F_OK) == 0;
        free(lin);
        if (!exists) return 0;
    }
    crt_set_errno(MS_EEXIST);
    return -1;
}
API char *_mktemp(char *t) { return t && mktemp_lin(t) == 0 ? t : NULL; }
API int _mktemp_s(char *t, size_t n) { (void)n; if (!t) return MS_EINVAL; return mktemp_lin(t) ? *crt_errno() : 0; }
API WCHAR *_wmktemp(WCHAR *t)
{
    if (!t) return NULL;
    char *u = lsiw_w2u(t);
    if (mktemp_lin(u)) { free(u); return NULL; }
    WCHAR *w = lsiw_u2w(u);
    memcpy(t, w, (lsiw_wcslen(w) + 1) * 2);
    free(u); free(w);
    return t;
}
API int _wmktemp_s(WCHAR *t, size_t n) { (void)n; return _wmktemp(t) ? 0 : *crt_errno(); }

/* ================================================================== */
/* stat                                                                 */
/* ================================================================== */

struct gstat { uint32_t dev; uint16_t mode; int64_t size; int64_t at, mt, ct; };

static int is_exec_name(const char *p)
{
    const char *d = strrchr(p, '.');
    return d && (!strcasecmp(d, ".exe") || !strcasecmp(d, ".com") || !strcasecmp(d, ".bat") || !strcasecmp(d, ".cmd"));
}

static void fill_gstat(const struct stat *st, const char *name, struct gstat *g)
{
    uint16_t m = 0;
    if (S_ISDIR(st->st_mode)) m = 0x4000 | 0x40;
    else if (S_ISREG(st->st_mode)) m = 0x8000;
    else if (S_ISCHR(st->st_mode)) m = 0x2000;
    else if (S_ISFIFO(st->st_mode) || S_ISSOCK(st->st_mode)) m = 0x1000;
    else m = 0x8000;
    m |= 0x100;
    if (st->st_mode & S_IWUSR) m |= 0x80;
    if (name && is_exec_name(name)) m |= 0x40;
    m |= (uint16_t)(((m & 0x1C0) >> 3) | ((m & 0x1C0) >> 6));
    g->mode = m;
    g->dev = S_ISCHR(st->st_mode) || S_ISFIFO(st->st_mode) ? 0 : 2;
    g->size = S_ISDIR(st->st_mode) ? 0 : st->st_size;
    g->at = st->st_atime;
    g->mt = st->st_mtime;
    g->ct = st->st_ctime;
}

static int stat_path(char *lin, const char *orig, struct gstat *g)
{
    if (!lin) { crt_set_errno(MS_ENOENT); return -1; }
    /* Windows does not accept a trailing separator on a file name; strip it
     * like the C runtime does for directories. */
    size_t l = strlen(lin);
    while (l > 1 && lin[l - 1] == '/') lin[--l] = 0;
    struct stat st;
    int r = stat(lin, &st);
    if (r < 0) { crt_set_errno_host(errno); free(lin); return -1; }
    fill_gstat(&st, orig, g);
    free(lin);
    return 0;
}
static int stat_fd(int fd, struct gstat *g)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    struct stat st;
    if (fstat(hostfd(c), &st) < 0) { crt_set_errno_host(errno); return -1; }
    fill_gstat(&st, NULL, g);
    if (S_ISCHR(st.st_mode)) g->dev = (uint32_t)fd;
    return 0;
}

/* Layout writers (x64 Microsoft structures). */
static void put_hdr(unsigned char *o, const struct gstat *g)
{
    memset(o, 0, 16);
    memcpy(o + 0, &g->dev, 4);                  /* st_dev */
    memcpy(o + 6, &g->mode, 2);                 /* st_mode (st_ino at 4 = 0) */
    int16_t one = 1; memcpy(o + 8, &one, 2);    /* st_nlink; uid/gid 0 */
    memcpy(o + 16, &g->dev, 4);                 /* st_rdev */
}
static void put_st64(void *buf, const struct gstat *g)     /* _stat64, _stati64 (x64): 56 bytes */
{
    unsigned char *o = buf;
    memset(o, 0, 56);
    put_hdr(o, g);
    memcpy(o + 24, &g->size, 8);
    memcpy(o + 32, &g->at, 8); memcpy(o + 40, &g->mt, 8); memcpy(o + 48, &g->ct, 8);
}
static void put_st64i32(void *buf, const struct gstat *g)  /* _stat, _stat64i32: 48 bytes */
{
    unsigned char *o = buf;
    memset(o, 0, 48);
    put_hdr(o, g);
    int32_t s = g->size > INT32_MAX ? INT32_MAX : (int32_t)g->size;
    memcpy(o + 20, &s, 4);
    memcpy(o + 24, &g->at, 8); memcpy(o + 32, &g->mt, 8); memcpy(o + 40, &g->ct, 8);
}
static void put_st32(void *buf, const struct gstat *g)     /* _stat32: 36 bytes */
{
    unsigned char *o = buf;
    memset(o, 0, 36);
    put_hdr(o, g);
    int32_t s = g->size > INT32_MAX ? INT32_MAX : (int32_t)g->size;
    int32_t a = (int32_t)g->at, m = (int32_t)g->mt, c = (int32_t)g->ct;
    memcpy(o + 20, &s, 4); memcpy(o + 24, &a, 4); memcpy(o + 28, &m, 4); memcpy(o + 32, &c, 4);
}
static void put_st32i64(void *buf, const struct gstat *g)  /* _stat32i64: 48 bytes */
{
    unsigned char *o = buf;
    memset(o, 0, 48);
    put_hdr(o, g);
    int32_t a = (int32_t)g->at, m = (int32_t)g->mt, c = (int32_t)g->ct;
    memcpy(o + 24, &g->size, 8); memcpy(o + 32, &a, 4); memcpy(o + 36, &m, 4); memcpy(o + 40, &c, 4);
}

#define STAT_FUNCS(suffix, writer)                                                                   \
    API int _stat##suffix(const char *p, void *b)                                                    \
    { struct gstat g; if (!p || !b) { crt_set_errno(MS_EINVAL); return -1; }                         \
      if (stat_path(lsiw_path_a(p), p, &g)) { return -1; } writer(b, &g); return 0; }                    \
    API int _wstat##suffix(const WCHAR *p, void *b)                                                  \
    { struct gstat g; if (!p || !b) { crt_set_errno(MS_EINVAL); return -1; }                         \
      char *u = lsiw_w2u(p); int r = stat_path(lsiw_path_a(u), u, &g); free(u);                      \
      if (r) { return -1; } writer(b, &g); return 0; }                                                   \
    API int _fstat##suffix(int fd, void *b)                                                          \
    { struct gstat g; if (!b) { crt_set_errno(MS_EINVAL); return -1; }                               \
      if (stat_fd(fd, &g)) { return -1; } writer(b, &g); return 0; }

STAT_FUNCS(, put_st64i32)
STAT_FUNCS(64i32, put_st64i32)
STAT_FUNCS(64, put_st64)
STAT_FUNCS(i64, put_st64)
STAT_FUNCS(32, put_st32)
STAT_FUNCS(32i64, put_st32i64)

/* ---- utime ---- */

static int set_times(char *lin, int fd, int64_t at, int64_t mt, int now)
{
    struct timespec ts[2];
    if (now) { ts[0].tv_nsec = ts[1].tv_nsec = UTIME_NOW; ts[0].tv_sec = ts[1].tv_sec = 0; }
    else { ts[0].tv_sec = at; ts[0].tv_nsec = 0; ts[1].tv_sec = mt; ts[1].tv_nsec = 0; }
    int r = lin ? utimensat(AT_FDCWD, lin, ts, 0) : futimens(fd, ts);
    if (r < 0) crt_set_errno_host(errno);
    free(lin);
    return r < 0 ? -1 : 0;
}
API int _utime64(const char *p, const int64_t *t) { return set_times(lsiw_path_a(p), -1, t ? t[0] : 0, t ? t[1] : 0, !t); }
API int _wutime64(const WCHAR *p, const int64_t *t) { return set_times(lsiw_path_w(p), -1, t ? t[0] : 0, t ? t[1] : 0, !t); }
API int _utime32(const char *p, const int32_t *t) { return set_times(lsiw_path_a(p), -1, t ? t[0] : 0, t ? t[1] : 0, !t); }
API int _wutime32(const WCHAR *p, const int32_t *t) { return set_times(lsiw_path_w(p), -1, t ? t[0] : 0, t ? t[1] : 0, !t); }
API int _futime64(int fd, const int64_t *t)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    return set_times(NULL, hostfd(c), t ? t[0] : 0, t ? t[1] : 0, !t);
}
API int _futime32(int fd, const int32_t *t)
{
    struct cfd *c = fdp(fd);
    if (!c) return badf();
    return set_times(NULL, hostfd(c), t ? t[0] : 0, t ? t[1] : 0, !t);
}

/* ================================================================== */
/* _findfirst / _findnext                                               */
/* ================================================================== */

#pragma pack(push, 4)
struct find_w {
    DWORD attrs;
    uint64_t ctime, atime, wtime;
    DWORD size_hi, size_lo, res0, res1;
    WCHAR name[260];
    WCHAR alt[14];
};
#pragma pack(pop)

enum { FD32, FD32I64, FD64I32, FD64 };

static int64_t ft_to_time(uint64_t ft) { return ft ? (int64_t)((ft - 116444736000000000ULL) / 10000000ULL) : -1; }

static void put_find(void *out, int layout, int wide, const struct find_w *f)
{
    unsigned char *o = out;
    uint64_t size = ((uint64_t)f->size_hi << 32) | f->size_lo;
    unsigned attrib = f->attrs & 0x37;   /* RDONLY HIDDEN SYSTEM SUBDIR ARCH */
    int64_t c = ft_to_time(f->ctime), a = ft_to_time(f->atime), w = ft_to_time(f->wtime);
    size_t name_off;
    memcpy(o, &attrib, 4);
    switch (layout) {
    case FD32: { int32_t t[3] = { (int32_t)c, (int32_t)a, (int32_t)w }; uint32_t s = (uint32_t)size; memcpy(o + 4, t, 12); memcpy(o + 16, &s, 4); name_off = 20; break; }
    case FD32I64: { int32_t t[3] = { (int32_t)c, (int32_t)a, (int32_t)w }; memcpy(o + 4, t, 12); memset(o + 16, 0, 0); memcpy(o + 16, &size, 8); name_off = 24; break; }
    case FD64I32: { int64_t t[3] = { c, a, w }; uint32_t s = (uint32_t)size; memset(o + 4, 0, 4); memcpy(o + 8, t, 24); memcpy(o + 32, &s, 4); name_off = 36; break; }
    default: { int64_t t[3] = { c, a, w }; memset(o + 4, 0, 4); memcpy(o + 8, t, 24); memcpy(o + 32, &size, 8); name_off = 40; break; }
    }
    if (wide) memcpy(o + name_off, f->name, sizeof f->name);
    else {
        char *u = lsiw_w2u(f->name);
        size_t l = strlen(u);
        if (l > 259) l = 259;
        memcpy(o + name_off, u, l);
        o[name_off + l] = 0;
        free(u);
    }
}

static intptr_t find_first(const WCHAR *spec, void *out, int layout, int wide)
{
    HANDLE (WINAPI *ff)(const WCHAR *, struct find_w *) = k32("FindFirstFileW");
    struct find_w f;
    HANDLE h = ff(spec, &f);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = lsiw_get_last_error();
        crt_set_errno(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_NO_MORE_FILES ? MS_ENOENT : MS_EINVAL);
        return -1;
    }
    put_find(out, layout, wide, &f);
    return (intptr_t)h;
}
static int find_next(intptr_t h, void *out, int layout, int wide)
{
    BOOL (WINAPI *fn)(HANDLE, struct find_w *) = k32("FindNextFileW");
    struct find_w f;
    if (!fn((HANDLE)h, &f)) { crt_set_errno(MS_ENOENT); return -1; }
    put_find(out, layout, wide, &f);
    return 0;
}
API int _findclose(intptr_t h)
{
    BOOL (WINAPI *fc)(HANDLE) = k32("FindClose");
    if (!fc((HANDLE)h)) { crt_set_errno(MS_ENOENT); return -1; }
    return 0;
}

#define FIND_FUNCS(sfx, layout)                                                                         \
    API intptr_t _findfirst##sfx(const char *spec, void *out)                                          \
    { WCHAR *w = lsiw_u2w(spec); intptr_t r = find_first(w, out, layout, 0); free(w); return r; }      \
    API int _findnext##sfx(intptr_t h, void *out) { return find_next(h, out, layout, 0); }             \
    API intptr_t _wfindfirst##sfx(const WCHAR *spec, void *out) { return find_first(spec, out, layout, 1); } \
    API int _wfindnext##sfx(intptr_t h, void *out) { return find_next(h, out, layout, 1); }

FIND_FUNCS(, FD64I32)
FIND_FUNCS(32, FD32)
FIND_FUNCS(32i64, FD32I64)
FIND_FUNCS(64i32, FD64I32)
FIND_FUNCS(64, FD64)
FIND_FUNCS(i64, FD64)

/* ================================================================== */
/* Streams                                                              */
/* ================================================================== */

#define NSTREAMS 512
#define BUFSZ 4096

struct sx {
    int used;
    pthread_mutex_t lock;
    char *buf;
    size_t cap, wlen, rpos, rlen;
    int bufmode;
    int userbuf;
    int last;              /* 0, 'r' or 'w' */
    unsigned char ung[16];
    int nung;
    int autoflush;         /* flush at the end of every call (console) */
    char *tmp_path;        /* tmpfile(): unlink on close */
    HANDLE proc;           /* _popen() child process */
    int pid;               /* _popen() through /bin/sh */
    WCHAR wlo;             /* pending low surrogate for fgetwc */
    WCHAR whi;             /* pending high surrogate for fputwc */
};

MSFILE crt_iob[NSTREAMS];
static struct sx g_sx[NSTREAMS];
static pthread_once_t g_streams_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_streams_lock = PTHREAD_MUTEX_INITIALIZER;

static void init_stream_lock(struct sx *x)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&x->lock, &a);
    pthread_mutexattr_destroy(&a);
}

static void streams_init(void)
{
    pthread_once(&g_fd_once, fd_init);
    for (int i = 0; i < NSTREAMS; i++) init_stream_lock(&g_sx[i]);
    for (int i = 0; i < 3; i++) {
        crt_iob[i]._file = i;
        crt_iob[i]._flag = i == 0 ? MS_IOREAD : MS_IOWRT;
        g_sx[i].used = 1;
        int h = crt_fd_host(i);
        int tty = h >= 0 && isatty(h);
        g_sx[i].bufmode = i == 2 ? MS_IONBF : MS_IOFBF;
        g_sx[i].autoflush = i == 2 || tty;
    }
}

static struct sx *X(MSFILE *f)
{
    pthread_once(&g_streams_once, streams_init);
    if (f < crt_iob || f >= crt_iob + NSTREAMS) return NULL;
    struct sx *x = &g_sx[f - crt_iob];
    return x->used ? x : NULL;
}

MSFILE *crt_stdin(void) { pthread_once(&g_streams_once, streams_init); return &crt_iob[0]; }
MSFILE *crt_stdout(void) { pthread_once(&g_streams_once, streams_init); return &crt_iob[1]; }
MSFILE *crt_stderr(void) { pthread_once(&g_streams_once, streams_init); return &crt_iob[2]; }

void crt_lock_stream(MSFILE *f) { struct sx *x = X(f); if (x) pthread_mutex_lock(&x->lock); }
void crt_unlock_stream(MSFILE *f) { struct sx *x = X(f); if (x) pthread_mutex_unlock(&x->lock); }
int crt_stream_is_wide_mode(MSFILE *f) { return (crt_fd_mode(f->_file) & WIDE_MODES) != 0; }

static int s_flush(MSFILE *f, struct sx *x)
{
    int r = 0;
    if (x->last == 'w' && x->wlen) {
        if (crt_write_fd(f->_file, x->buf, (unsigned)x->wlen) < 0) { f->_flag |= MS_IOERR; r = MS_EOF; }
        x->wlen = 0;
    } else if (x->last == 'r') {
        /* Give unread input back to the descriptor where possible. */
        size_t unread = x->rlen - x->rpos + (size_t)x->nung;
        if (unread) {
            struct cfd *c = fdp(f->_file);
            if (c && c->mode != O_MS_TEXT && lseek(hostfd(c), -(off_t)unread, SEEK_CUR) >= 0) { x->rpos = x->rlen = 0; x->nung = 0; }
            else if (c && c->mode == O_MS_TEXT) {
                int h = hostfd(c);
                struct stat st;
                if (fstat(h, &st) == 0 && S_ISREG(st.st_mode) && lseek(h, -(off_t)unread, SEEK_CUR) >= 0) { x->rpos = x->rlen = 0; x->nung = 0; }
            }
        }
    }
    f->_cnt = 0;
    return r;
}

static int ensure_buf(struct sx *x)
{
    if (x->buf) return 0;
    x->cap = BUFSZ;
    x->buf = malloc(x->cap);
    return x->buf ? 0 : -1;
}

static void s_to_write(MSFILE *f, struct sx *x)
{
    if (x->last == 'r') {
        s_flush(f, x);
        x->rpos = x->rlen = 0;
        x->nung = 0;
    }
    x->last = 'w';
}

static int s_write(MSFILE *f, struct sx *x, const void *p, size_t n)
{
    if (!(f->_flag & (MS_IOWRT | MS_IORW))) { f->_flag |= MS_IOERR; crt_set_errno(MS_EBADF); return -1; }
    s_to_write(f, x);
    if (x->bufmode == MS_IONBF) {
        if (x->wlen && s_flush(f, x)) return -1;
        if (crt_write_fd(f->_file, p, (unsigned)n) < 0) { f->_flag |= MS_IOERR; return -1; }
        return (int)n;
    }
    if (ensure_buf(x)) return -1;
    const char *s = p;
    size_t left = n;
    while (left) {
        size_t room = x->cap - x->wlen;
        if (!room) { if (s_flush(f, x)) return -1; x->last = 'w'; continue; }
        if (!x->wlen && left >= x->cap) {           /* large write: bypass the buffer */
            if (crt_write_fd(f->_file, s, (unsigned)left) < 0) { f->_flag |= MS_IOERR; return -1; }
            left = 0;
            break;
        }
        size_t k = left < room ? left : room;
        memcpy(x->buf + x->wlen, s, k);
        x->wlen += k; s += k; left -= k;
    }
    if (x->bufmode == MS_IOLBF && memchr(p, '\n', n)) { if (s_flush(f, x)) return -1; x->last = 'w'; }
    return (int)n;
}

static void flush_console_outputs(void)
{
    for (int i = 1; i <= 2; i++) {
        struct sx *x = &g_sx[i];
        if (!x->used || !x->wlen) continue;
        if (pthread_mutex_trylock(&x->lock) == 0) { s_flush(&crt_iob[i], x); pthread_mutex_unlock(&x->lock); }
    }
}

static int s_fill(MSFILE *f, struct sx *x)
{
    if (!(f->_flag & (MS_IOREAD | MS_IORW))) { f->_flag |= MS_IOERR; crt_set_errno(MS_EBADF); return -1; }
    if (x->last == 'w') { if (s_flush(f, x)) return -1; }
    x->last = 'r';
    if (ensure_buf(x)) return -1;
    int h = crt_fd_host(f->_file);
    if (h >= 0 && isatty(h)) flush_console_outputs();
    size_t want = x->bufmode == MS_IONBF ? 1 : x->cap;
    if (h >= 0 && isatty(h) && (crt_fd_mode(f->_file) & WIDE_MODES)) want = x->cap;
    int r = crt_read_fd(f->_file, x->buf, (unsigned)want);
    if (r <= 0) { f->_flag |= r == 0 ? MS_IOEOF : MS_IOERR; x->rpos = x->rlen = 0; return -1; }
    x->rpos = 0;
    x->rlen = (size_t)r;
    return 0;
}

static int s_getc(MSFILE *f, struct sx *x)
{
    if (x->nung) return x->ung[--x->nung];
    if (x->last != 'r' || x->rpos >= x->rlen) { if (s_fill(f, x)) return MS_EOF; }
    return (unsigned char)x->buf[x->rpos++];
}

static int s_ungetc(MSFILE *f, struct sx *x, int c)
{
    if (c == MS_EOF) return MS_EOF;
    if (x->last == 'w') { s_flush(f, x); }
    x->last = 'r';
    if (x->rpos > 0 && x->rpos <= x->rlen && x->nung == 0 && (unsigned char)x->buf[x->rpos - 1] == (unsigned char)c) x->rpos--;
    else if (x->nung < (int)sizeof x->ung) x->ung[x->nung++] = (unsigned char)c;
    else return MS_EOF;
    f->_flag &= ~MS_IOEOF;
    return c & 0xff;
}

static void done(MSFILE *f, struct sx *x)
{
    if (x->autoflush && x->wlen) s_flush(f, x);
    f->_cnt = 0;
}

/* Narrow bytes into a stream; wide-mode streams receive them as UTF-16. */
static int s_write_text(MSFILE *f, struct sx *x, const char *p, size_t n)
{
    if (!crt_stream_is_wide_mode(f)) return s_write(f, x, p, n);
    int wl = lsiw_utf8_to_utf16(p, (int)n, NULL, 0);
    if (wl <= 0) return 0;
    WCHAR *w = malloc((size_t)wl * 2);
    lsiw_utf8_to_utf16(p, (int)n, w, wl);
    int r = s_write(f, x, w, (size_t)wl * 2);
    free(w);
    return r < 0 ? -1 : (int)n;
}

int crt_fwrite_bytes(const void *p, size_t n, MSFILE *f)
{
    struct sx *x = X(f);
    if (!x) { crt_set_errno(MS_EINVAL); return -1; }
    pthread_mutex_lock(&x->lock);
    int r = s_write_text(f, x, p, n);
    done(f, x);
    pthread_mutex_unlock(&x->lock);
    return r;
}
int crt_fputws_utf8(const char *u, MSFILE *f) { return crt_fwrite_bytes(u, strlen(u), f); }
int crt_fputc(int c, MSFILE *f) { char ch = (char)c; return crt_fwrite_bytes(&ch, 1, f) == 1 ? (c & 0xff) : MS_EOF; }
int crt_fgetc(MSFILE *f)
{
    struct sx *x = X(f);
    if (!x) return MS_EOF;
    pthread_mutex_lock(&x->lock);
    int c = s_getc(f, x);
    pthread_mutex_unlock(&x->lock);
    return c;
}
int crt_ungetc(int c, MSFILE *f)
{
    struct sx *x = X(f);
    if (!x) return MS_EOF;
    pthread_mutex_lock(&x->lock);
    int r = s_ungetc(f, x, c);
    pthread_mutex_unlock(&x->lock);
    return r;
}
int crt_fflush(MSFILE *f)
{
    struct sx *x = X(f);
    if (!x) return MS_EOF;
    pthread_mutex_lock(&x->lock);
    int r = s_flush(f, x);
    if (x->last == 'w') x->last = 0;
    pthread_mutex_unlock(&x->lock);
    return r;
}

void lsiw_crt_flush_all(void)
{
    pthread_once(&g_streams_once, streams_init);
    for (int i = 0; i < NSTREAMS; i++) {
        struct sx *x = &g_sx[i];
        if (!x->used || !x->wlen) continue;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_mutex_timedlock(&x->lock, &ts) == 0) {
            s_flush(&crt_iob[i], x);
            pthread_mutex_unlock(&x->lock);
        }
    }
}

/* ---- wide characters on streams ---- */

static int s_putwc(MSFILE *f, struct sx *x, WCHAR wc)
{
    if (crt_stream_is_wide_mode(f)) return s_write(f, x, &wc, 2) == 2 ? wc : -1;
    char b[4];
    int l;
    uint32_t cp = wc;
    if (wc >= 0xD800 && wc <= 0xDBFF) { x->whi = wc; return wc; }
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (!x->whi) { crt_set_errno(MS_EILSEQ); f->_flag |= MS_IOERR; return -1; }
        cp = 0x10000 + (((uint32_t)x->whi - 0xD800) << 10) + (wc - 0xDC00u);
        x->whi = 0;
    }
    if (cp < 0x80) { b[0] = (char)cp; l = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); l = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); l = 3; }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); l = 4; }
    return s_write(f, x, b, (size_t)l) == l ? wc : -1;
}

static int s_getwc(MSFILE *f, struct sx *x)
{
    if (x->wlo) { WCHAR w = x->wlo; x->wlo = 0; return w; }
    if (crt_stream_is_wide_mode(f)) {
        int a = s_getc(f, x);
        if (a == MS_EOF) return WEOF16;
        int b = s_getc(f, x);
        if (b == MS_EOF) return WEOF16;
        return a | (b << 8);
    }
    int c = s_getc(f, x);
    if (c == MS_EOF) return WEOF16;
    if (c < 0x80) return c;
    int l = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (l == 1) return 0xFFFD;
    uint32_t cp = (uint32_t)c & (l == 2 ? 0x1F : l == 3 ? 0x0F : 0x07);
    for (int i = 1; i < l; i++) {
        int n = s_getc(f, x);
        if (n == MS_EOF) return 0xFFFD;
        if ((n & 0xC0) != 0x80) { s_ungetc(f, x, n); return 0xFFFD; }
        cp = (cp << 6) | ((unsigned)n & 0x3F);
    }
    if (cp >= 0x10000) {
        cp -= 0x10000;
        x->wlo = (WCHAR)(0xDC00 + (cp & 0x3FF));
        return 0xD800 + (int)(cp >> 10);
    }
    return (int)cp;
}

static int s_ungetwc(MSFILE *f, struct sx *x, unsigned wc)
{
    if (wc == WEOF16) return WEOF16;
    if (crt_stream_is_wide_mode(f)) {
        if (s_ungetc(f, x, (int)(wc >> 8)) == MS_EOF) return WEOF16;
        if (s_ungetc(f, x, (int)(wc & 0xff)) == MS_EOF) return WEOF16;
        return wc;
    }
    char b[4];
    int l;
    if (wc < 0x80) { b[0] = (char)wc; l = 1; }
    else if (wc < 0x800) { b[0] = (char)(0xC0 | (wc >> 6)); b[1] = (char)(0x80 | (wc & 0x3F)); l = 2; }
    else { b[0] = (char)(0xE0 | (wc >> 12)); b[1] = (char)(0x80 | ((wc >> 6) & 0x3F)); b[2] = (char)(0x80 | (wc & 0x3F)); l = 3; }
    for (int i = l - 1; i >= 0; i--) if (s_ungetc(f, x, (unsigned char)b[i]) == MS_EOF) return WEOF16;
    return wc;
}

/* ---- opening and closing ---- */

static int parse_mode(const char *m, int *oflag, int *sflag)
{
    int of = 0, sf = 0;
    switch (*m++) {
    case 'r': of = O_MS_RDONLY; sf = MS_IOREAD; break;
    case 'w': of = O_MS_WRONLY | O_MS_CREAT | O_MS_TRUNC; sf = MS_IOWRT; break;
    case 'a': of = O_MS_WRONLY | O_MS_CREAT | O_MS_APPEND; sf = MS_IOWRT; break;
    default: crt_set_errno(MS_EINVAL); return -1;
    }
    for (; *m; m++) {
        switch (*m) {
        case '+': of = (of & ~3) | O_MS_RDWR; sf = MS_IORW | MS_IOREAD | MS_IOWRT; break;
        case 'b': of |= O_MS_BINARY; break;
        case 't': of |= O_MS_TEXT; break;
        case 'x': of |= O_MS_EXCL; break;
        case 'D': of |= O_MS_TEMPORARY; break;
        case 'N': of |= O_MS_NOINHERIT; break;
        case ',':
            while (*m == ',' || *m == ' ') m++;
            if (!strncasecmp(m, "ccs=", 4)) {
                m += 4;
                if (!strncasecmp(m, "UTF-8", 5)) of |= O_MS_U8TEXT;
                else if (!strncasecmp(m, "UTF-16LE", 8) || !strncasecmp(m, "UNICODE", 7)) of |= O_MS_U16TEXT;
            }
            m += strlen(m) - 1;
            break;
        default: break;   /* c, n, S, R, T, ' ' */
        }
    }
    *oflag = of;
    *sflag = sf;
    return 0;
}

static MSFILE *stream_alloc(int fd, int sflag)
{
    pthread_once(&g_streams_once, streams_init);
    pthread_mutex_lock(&g_streams_lock);
    for (int i = 3; i < NSTREAMS; i++) {
        if (!g_sx[i].used) {
            struct sx *x = &g_sx[i];
            pthread_mutex_t keep = x->lock;
            memset(x, 0, sizeof *x);
            x->lock = keep;
            x->used = 1;
            x->bufmode = MS_IOFBF;
            int h = crt_fd_host(fd);
            x->autoflush = h >= 0 && isatty(h);
            memset(&crt_iob[i], 0, sizeof crt_iob[i]);
            crt_iob[i]._file = fd;
            crt_iob[i]._flag = sflag;
            pthread_mutex_unlock(&g_streams_lock);
            return &crt_iob[i];
        }
    }
    pthread_mutex_unlock(&g_streams_lock);
    crt_set_errno(MS_EMFILE);
    return NULL;
}

static MSFILE *open_stream(const char *path, const char *mode)
{
    int of, sf;
    if (!path || !mode) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return NULL; }
    if (parse_mode(mode, &of, &sf)) return NULL;
    int fd = open_path_utf8(path, of, 0x180);
    if (fd < 0) return NULL;
    MSFILE *f = stream_alloc(fd, sf);
    if (!f) _close(fd);
    return f;
}

static void stream_release(MSFILE *f, struct sx *x)
{
    if (!x->userbuf) free(x->buf);
    x->buf = NULL;
    x->cap = x->wlen = x->rpos = x->rlen = 0;
    if (x->tmp_path) { unlink(x->tmp_path); free(x->tmp_path); x->tmp_path = NULL; }
    if (f - crt_iob >= 3) x->used = 0;
    f->_flag = 0;
    f->_file = -1;
}

static int close_stream(MSFILE *f, struct sx *x)
{
    int r = s_flush(f, x);
    if (f->_file >= 0 && _close(f->_file) < 0) r = MS_EOF;
    stream_release(f, x);
    return r;
}

API MSFILE *ms_fopen(const char *p, const char *m) { return open_stream(p, m); }
API MSFILE *_wfopen(const WCHAR *p, const WCHAR *m)
{
    char *a = p ? lsiw_w2u(p) : NULL, *b = m ? lsiw_w2u(m) : NULL;
    MSFILE *f = open_stream(a, b);
    free(a); free(b);
    return f;
}
API MSFILE *_fsopen(const char *p, const char *m, int sh) { (void)sh; return open_stream(p, m); }
API MSFILE *_wfsopen(const WCHAR *p, const WCHAR *m, int sh) { (void)sh; return _wfopen(p, m); }
API int fopen_s(MSFILE **out, const char *p, const char *m)
{
    if (!out) { crt_invalid_parameter(); return MS_EINVAL; }
    *out = open_stream(p, m);
    return *out ? 0 : *crt_errno();
}
API int _wfopen_s(MSFILE **out, const WCHAR *p, const WCHAR *m)
{
    if (!out) { crt_invalid_parameter(); return MS_EINVAL; }
    *out = _wfopen(p, m);
    return *out ? 0 : *crt_errno();
}

static MSFILE *reopen_stream(const char *path, const char *mode, MSFILE *f)
{
    struct sx *x = X(f);
    if (!x || !mode) { crt_set_errno(MS_EINVAL); return NULL; }
    pthread_mutex_lock(&x->lock);
    int of, sf;
    if (parse_mode(mode, &of, &sf)) { pthread_mutex_unlock(&x->lock); return NULL; }
    int oldfd = f->_file;
    s_flush(f, x);
    if (!path) {
        /* Only the mode changes. */
        if (oldfd >= 0) _setmode(oldfd, mode_from_flags(of));
        f->_flag = sf;
        pthread_mutex_unlock(&x->lock);
        return f;
    }
    int nfd = open_path_utf8(path, of, 0x180);
    if (nfd < 0) { pthread_mutex_unlock(&x->lock); return NULL; }
    if (oldfd >= 0 && oldfd < 3) {
        /* freopen(..., stdout) on Windows ends up on descriptor 1 again. */
        _dup2(nfd, oldfd);
        _close(nfd);
        g_fd[oldfd].mode = mode_from_flags(of);
        g_fd[oldfd].flags = of;
        nfd = oldfd;
    } else if (oldfd >= 0) _close(oldfd);
    if (!x->userbuf) free(x->buf);
    x->buf = NULL;
    x->cap = x->wlen = x->rpos = x->rlen = 0;
    x->nung = 0;
    x->last = 0;
    x->userbuf = 0;
    int h = crt_fd_host(nfd);
    x->autoflush = (f == &crt_iob[2]) || (h >= 0 && isatty(h));
    if (f != &crt_iob[2]) x->bufmode = MS_IOFBF;
    f->_file = nfd;
    f->_flag = sf;
    f->_cnt = 0;
    pthread_mutex_unlock(&x->lock);
    return f;
}
API MSFILE *ms_freopen(const char *p, const char *m, MSFILE *f) { return reopen_stream(p, m, f); }
API MSFILE *_wfreopen(const WCHAR *p, const WCHAR *m, MSFILE *f)
{
    char *a = p ? lsiw_w2u(p) : NULL, *b = m ? lsiw_w2u(m) : NULL;
    MSFILE *r = reopen_stream(a, b, f);
    free(a); free(b);
    return r;
}
API int freopen_s(MSFILE **out, const char *p, const char *m, MSFILE *f)
{ MSFILE *r = reopen_stream(p, m, f); if (out) *out = r; return r ? 0 : *crt_errno(); }
API int _wfreopen_s(MSFILE **out, const WCHAR *p, const WCHAR *m, MSFILE *f)
{ MSFILE *r = _wfreopen(p, m, f); if (out) *out = r; return r ? 0 : *crt_errno(); }

API MSFILE *_fdopen(int fd, const char *mode)
{
    int of, sf;
    if (!fdp(fd)) { badf(); return NULL; }
    if (!mode || parse_mode(mode, &of, &sf)) return NULL;
    if (of & (O_MS_TEXT | O_MS_BINARY | WIDE_MODES)) g_fd[fd].mode = mode_from_flags(of);
    return stream_alloc(fd, sf);
}
API MSFILE *_wfdopen(int fd, const WCHAR *mode) { char *m = lsiw_w2u(mode); MSFILE *f = _fdopen(fd, m); free(m); return f; }

API int ms_fclose(MSFILE *f)
{
    struct sx *x = X(f);
    if (!x) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return MS_EOF; }
    pthread_mutex_lock(&x->lock);
    int r = close_stream(f, x);
    pthread_mutex_unlock(&x->lock);
    return r;
}
API int _fclose_nolock(MSFILE *f) { return ms_fclose(f); }
API int _fcloseall(void)
{
    int n = 0;
    pthread_once(&g_streams_once, streams_init);
    for (int i = 3; i < NSTREAMS; i++) if (g_sx[i].used && !g_sx[i].proc && !g_sx[i].pid) { ms_fclose(&crt_iob[i]); n++; }
    return n;
}
API int _rmtmp(void)
{
    int n = 0;
    pthread_once(&g_streams_once, streams_init);
    for (int i = 3; i < NSTREAMS; i++) if (g_sx[i].used && g_sx[i].tmp_path) { ms_fclose(&crt_iob[i]); n++; }
    return n;
}

API int ms_fflush(MSFILE *f)
{
    if (!f) { lsiw_crt_flush_all(); return 0; }
    struct sx *x = X(f);
    if (!x) { crt_set_errno(MS_EINVAL); return MS_EOF; }
    return crt_fflush(f);
}
API int _fflush_nolock(MSFILE *f) { return ms_fflush(f); }
API int _flushall(void)
{
    int n = 0;
    pthread_once(&g_streams_once, streams_init);
    for (int i = 0; i < NSTREAMS; i++) if (g_sx[i].used) { crt_fflush(&crt_iob[i]); n++; }
    return n;
}

/* ---- character and block I/O ---- */

#define LOCKED(f, x, err)                                                       \
    struct sx *x = X(f);                                                        \
    if (!x) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return err; }  \
    pthread_mutex_lock(&x->lock)
#define UNLOCK(f, x) do { done(f, x); pthread_mutex_unlock(&x->lock); } while (0)

API int ms_fgetc(MSFILE *f) { LOCKED(f, x, MS_EOF); int c = s_getc(f, x); UNLOCK(f, x); return c; }
API int ms_fputc(int c, MSFILE *f) { LOCKED(f, x, MS_EOF); char ch = (char)c; int r = s_write_text(f, x, &ch, 1); UNLOCK(f, x); return r == 1 ? (c & 0xff) : MS_EOF; }
API int ms_getchar(void) { return ms_fgetc(crt_stdin()); }
API int ms_putchar(int c) { return ms_fputc(c, crt_stdout()); }
API int ms_ungetc(int c, MSFILE *f) { LOCKED(f, x, MS_EOF); int r = s_ungetc(f, x, c); UNLOCK(f, x); return r; }

API unsigned ms_fgetwc(MSFILE *f) { LOCKED(f, x, WEOF16); int c = s_getwc(f, x); UNLOCK(f, x); return (unsigned)c & 0xFFFF; }
API unsigned ms_fputwc(WCHAR c, MSFILE *f) { LOCKED(f, x, WEOF16); int r = s_putwc(f, x, c); UNLOCK(f, x); return r < 0 ? WEOF16 : c; }
API unsigned ms_ungetwc(unsigned c, MSFILE *f) { LOCKED(f, x, WEOF16); int r = s_ungetwc(f, x, c); UNLOCK(f, x); return (unsigned)r; }
API unsigned ms_getwchar(void) { return ms_fgetwc(crt_stdin()); }
API unsigned ms_putwchar(WCHAR c) { return ms_fputwc(c, crt_stdout()); }

/* Old-style getc/putc macros call these with _cnt exhausted. */
API int _filbuf(MSFILE *f) { int c = ms_fgetc(f); f->_cnt = 0; return c; }
API int _flsbuf(int c, MSFILE *f) { int r = ms_fputc(c, f); f->_cnt = 0; return r; }
API unsigned _filwbuf(MSFILE *f) { unsigned c = ms_fgetwc(f); f->_cnt = 0; return c; }
API unsigned _flswbuf(WCHAR c, MSFILE *f) { unsigned r = ms_fputwc(c, f); f->_cnt = 0; return r; }

API size_t ms_fread(void *buf, size_t size, size_t n, MSFILE *f)
{
    if (!size || !n) return 0;
    if (!buf) { crt_invalid_parameter(); return 0; }
    LOCKED(f, x, 0);
    size_t total = size * n, got = 0;
    char *o = buf;
    while (got < total && x->nung) o[got++] = (char)x->ung[--x->nung];
    while (got < total) {
        if (x->last == 'r' && x->rpos < x->rlen) {
            size_t k = x->rlen - x->rpos;
            if (k > total - got) k = total - got;
            memcpy(o + got, x->buf + x->rpos, k);
            x->rpos += k;
            got += k;
            continue;
        }
        if (total - got >= BUFSZ && !crt_stream_is_wide_mode(f)) {
            if (x->last == 'w') s_flush(f, x);
            x->last = 'r';
            x->rpos = x->rlen = 0;
            int r = crt_read_fd(f->_file, o + got, (unsigned)((total - got) > 0x7fffffff ? 0x7fffffff : total - got));
            if (r <= 0) { f->_flag |= r == 0 ? MS_IOEOF : MS_IOERR; break; }
            got += (size_t)r;
            continue;
        }
        if (s_fill(f, x)) break;
    }
    UNLOCK(f, x);
    return got / size;
}
API size_t _fread_nolock(void *b, size_t s, size_t n, MSFILE *f) { return ms_fread(b, s, n, f); }
API size_t fread_s(void *b, size_t bs, size_t s, size_t n, MSFILE *f)
{
    if (s && n > bs / s) { crt_invalid_parameter(); crt_set_errno(MS_ERANGE); return 0; }
    return ms_fread(b, s, n, f);
}
API size_t _fread_nolock_s(void *b, size_t bs, size_t s, size_t n, MSFILE *f) { return fread_s(b, bs, s, n, f); }

API size_t ms_fwrite(const void *buf, size_t size, size_t n, MSFILE *f)
{
    if (!size || !n) return 0;
    LOCKED(f, x, 0);
    int r = s_write(f, x, buf, size * n);
    UNLOCK(f, x);
    return r < 0 ? 0 : n;
}
API size_t _fwrite_nolock(const void *b, size_t s, size_t n, MSFILE *f) { return ms_fwrite(b, s, n, f); }

API char *ms_fgets(char *buf, int n, MSFILE *f)
{
    if (!buf || n <= 0) { crt_invalid_parameter(); return NULL; }
    LOCKED(f, x, NULL);
    int i = 0;
    while (i < n - 1) {
        int c = s_getc(f, x);
        if (c == MS_EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    UNLOCK(f, x);
    if (!i) return NULL;
    buf[i] = 0;
    return buf;
}
API WCHAR *ms_fgetws(WCHAR *buf, int n, MSFILE *f)
{
    if (!buf || n <= 0) { crt_invalid_parameter(); return NULL; }
    LOCKED(f, x, NULL);
    int i = 0;
    while (i < n - 1) {
        int c = s_getwc(f, x);
        if (c == WEOF16) break;
        buf[i++] = (WCHAR)c;
        if (c == '\n') break;
    }
    UNLOCK(f, x);
    if (!i) return NULL;
    buf[i] = 0;
    return buf;
}
API int ms_fputs(const char *s, MSFILE *f)
{
    if (!s) { crt_invalid_parameter(); return MS_EOF; }
    LOCKED(f, x, MS_EOF);
    int r = s_write_text(f, x, s, strlen(s));
    UNLOCK(f, x);
    return r < 0 ? MS_EOF : 0;
}
API int ms_fputws(const WCHAR *s, MSFILE *f)
{
    if (!s) { crt_invalid_parameter(); return MS_EOF; }
    LOCKED(f, x, MS_EOF);
    int r = 0;
    for (; *s && r >= 0; s++) r = s_putwc(f, x, *s);
    UNLOCK(f, x);
    return r < 0 ? MS_EOF : 0;
}
API int ms_puts(const char *s)
{
    MSFILE *f = crt_stdout();
    LOCKED(f, x, MS_EOF);
    int r = s_write_text(f, x, s, strlen(s));
    if (r >= 0) r = s_write_text(f, x, "\n", 1);
    UNLOCK(f, x);
    return r < 0 ? MS_EOF : 0;
}
API int _putws(const WCHAR *s)
{
    MSFILE *f = crt_stdout();
    LOCKED(f, x, MS_EOF);
    int r = 0;
    for (; *s && r >= 0; s++) r = s_putwc(f, x, *s);
    if (r >= 0) r = s_putwc(f, x, '\n');
    UNLOCK(f, x);
    return r < 0 ? MS_EOF : 0;
}
API char *ms_gets(char *buf)
{
    MSFILE *f = crt_stdin();
    LOCKED(f, x, NULL);
    int i = 0, c;
    while ((c = s_getc(f, x)) != MS_EOF && c != '\n') buf[i++] = (char)c;
    UNLOCK(f, x);
    if (c == MS_EOF && !i) return NULL;
    buf[i] = 0;
    return buf;
}
API char *gets_s(char *buf, size_t n)
{
    if (!buf || !n) { crt_invalid_parameter(); return NULL; }
    MSFILE *f = crt_stdin();
    LOCKED(f, x, NULL);
    size_t i = 0;
    int c, over = 0;
    while ((c = s_getc(f, x)) != MS_EOF && c != '\n') { if (i + 1 < n) buf[i++] = (char)c; else over = 1; }
    UNLOCK(f, x);
    if (c == MS_EOF && !i) { buf[0] = 0; return NULL; }
    if (over) { buf[0] = 0; crt_invalid_parameter(); return NULL; }
    buf[i] = 0;
    return buf;
}
API WCHAR *_getws(WCHAR *buf)
{
    MSFILE *f = crt_stdin();
    LOCKED(f, x, NULL);
    int i = 0, c;
    while ((c = s_getwc(f, x)) != WEOF16 && c != '\n') buf[i++] = (WCHAR)c;
    UNLOCK(f, x);
    if (c == WEOF16 && !i) return NULL;
    buf[i] = 0;
    return buf;
}
API WCHAR *_getws_s(WCHAR *buf, size_t n)
{
    if (!buf || !n) { crt_invalid_parameter(); return NULL; }
    MSFILE *f = crt_stdin();
    LOCKED(f, x, NULL);
    size_t i = 0;
    int c;
    while ((c = s_getwc(f, x)) != WEOF16 && c != '\n') if (i + 1 < n) buf[i++] = (WCHAR)c;
    UNLOCK(f, x);
    if (c == WEOF16 && !i) return NULL;
    buf[i] = 0;
    return buf;
}

/* ---- positioning ---- */

static int64_t s_tell(MSFILE *f, struct sx *x)
{
    int h = crt_fd_host(f->_file);
    off_t pos = lseek(h, 0, SEEK_CUR);
    if (pos < 0) { crt_set_errno_host(errno); return -1; }
    if (x->last == 'r') pos -= (off_t)(x->rlen - x->rpos) + x->nung;
    else if (x->last == 'w') pos += (off_t)x->wlen;
    struct cfd *c = fdp(f->_file);
    if (c && c->peek >= 0) pos--;
    return pos < 0 ? 0 : pos;
}
static int s_seek(MSFILE *f, struct sx *x, int64_t off, int whence)
{
    if (whence < 0 || whence > 2) { crt_set_errno(MS_EINVAL); return -1; }
    if (x->last == 'w' && s_flush(f, x)) return -1;
    if (whence == SEEK_CUR && x->last == 'r') off -= (int64_t)(x->rlen - x->rpos) + x->nung;
    x->rpos = x->rlen = 0;
    x->nung = 0;
    x->last = 0;
    x->wlo = 0;
    if (seek_fd(f->_file, off, whence) < 0) return -1;
    f->_flag &= ~MS_IOEOF;
    if (f->_flag & MS_IORW) f->_flag |= MS_IOREAD | MS_IOWRT;
    return 0;
}
API int ms_fseek(MSFILE *f, int32_t off, int whence) { LOCKED(f, x, -1); int r = s_seek(f, x, off, whence); UNLOCK(f, x); return r; }
API int _fseeki64(MSFILE *f, int64_t off, int whence) { LOCKED(f, x, -1); int r = s_seek(f, x, off, whence); UNLOCK(f, x); return r; }
API int32_t ms_ftell(MSFILE *f)
{
    LOCKED(f, x, -1);
    int64_t r = s_tell(f, x);
    UNLOCK(f, x);
    if (r > INT32_MAX) { crt_set_errno(MS_EINVAL); return -1; }
    return (int32_t)r;
}
API int64_t _ftelli64(MSFILE *f) { LOCKED(f, x, -1); int64_t r = s_tell(f, x); UNLOCK(f, x); return r; }
API void ms_rewind(MSFILE *f) { LOCKED(f, x, ); s_seek(f, x, 0, SEEK_SET); f->_flag &= ~(MS_IOERR | MS_IOEOF); UNLOCK(f, x); }
API int ms_fgetpos(MSFILE *f, int64_t *pos) { if (!pos) return MS_EINVAL; *pos = _ftelli64(f); return *pos < 0 ? -1 : 0; }
API int ms_fsetpos(MSFILE *f, const int64_t *pos) { if (!pos) return MS_EINVAL; return _fseeki64(f, *pos, SEEK_SET); }
API int _fseek_nolock(MSFILE *f, int32_t off, int whence) { return ms_fseek(f, off, whence); }
API int _fseeki64_nolock(MSFILE *f, int64_t off, int whence) { return _fseeki64(f, off, whence); }
API int32_t _ftell_nolock(MSFILE *f) { return ms_ftell(f); }
API int64_t _ftelli64_nolock(MSFILE *f) { return _ftelli64(f); }

API int ms_feof(MSFILE *f) { if (!X(f)) return 0; return (f->_flag & MS_IOEOF) ? MS_IOEOF : 0; }
API int ms_ferror(MSFILE *f) { if (!X(f)) return 0; return (f->_flag & MS_IOERR) ? MS_IOERR : 0; }
API void ms_clearerr(MSFILE *f) { if (X(f)) f->_flag &= ~(MS_IOERR | MS_IOEOF); }
API int clearerr_s(MSFILE *f) { if (!X(f)) return MS_EINVAL; f->_flag &= ~(MS_IOERR | MS_IOEOF); return 0; }
API int _fileno(MSFILE *f) { if (!X(f)) { crt_set_errno(MS_EINVAL); return -1; } return f->_file; }

API int ms_setvbuf(MSFILE *f, char *buf, int mode, size_t size)
{
    LOCKED(f, x, -1);
    if ((mode != MS_IOFBF && mode != MS_IOLBF && mode != MS_IONBF) || (mode != MS_IONBF && (size < 2 || size > 0x7fffffff))) {
        pthread_mutex_unlock(&x->lock);
        crt_invalid_parameter();
        return -1;
    }
    s_flush(f, x);
    if (!x->userbuf) free(x->buf);
    x->buf = NULL; x->cap = 0; x->userbuf = 0; x->rpos = x->rlen = 0;
    x->bufmode = mode;
    if (mode != MS_IONBF) {
        if (buf) { x->buf = buf; x->userbuf = 1; x->cap = size; }
        else { x->cap = size; x->buf = malloc(size); }
        /* An explicit buffering request overrides console auto-flush, except
         * that line buffering on a terminal keeps flushing per line. */
        x->autoflush = 0;
    } else x->autoflush = 1;
    pthread_mutex_unlock(&x->lock);
    return 0;
}
API void ms_setbuf(MSFILE *f, char *buf) { ms_setvbuf(f, buf, buf ? MS_IOFBF : MS_IONBF, 512); }

API void _lock_file(MSFILE *f) { crt_lock_stream(f); }
API void _unlock_file(MSFILE *f) { crt_unlock_stream(f); }
API int _getmaxstdio(void) { return NSTREAMS; }
API int _setmaxstdio(int n) { return n <= NSTREAMS ? n : -1; }
API MSFILE *__iob_func(void) { pthread_once(&g_streams_once, streams_init); return crt_iob; }
API MSFILE *__acrt_iob_func(unsigned i) { pthread_once(&g_streams_once, streams_init); return i < NSTREAMS ? &crt_iob[i] : NULL; }
API MSFILE *__p__iob(void) { return __iob_func(); }
API int _get_stream_buffer_pointers(MSFILE *f, char ***base, char ***ptr, int **cnt)
{
    if (base) *base = &f->_base;
    if (ptr) *ptr = &f->_ptr;
    if (cnt) *cnt = &f->_cnt;
    return 0;
}
static int g_count_output, g_output_format;
API int _set_printf_count_output(int v) { int o = g_count_output; g_count_output = v; return o; }
API int _get_printf_count_output(void) { return g_count_output; }
extern int lsiw_printf_three_digit_exp;
API unsigned _set_output_format(unsigned v) { unsigned o = (unsigned)g_output_format; g_output_format = (int)v; lsiw_printf_three_digit_exp = !(v & 1); return o; }
API unsigned _get_output_format(void) { return (unsigned)g_output_format; }

/* ---- temporary files ---- */

static char *temp_dir_win(void)
{
    DWORD (WINAPI *gtp)(DWORD, char *) = k32("GetTempPathA");
    char buf[1024];
    DWORD n = gtp(sizeof buf, buf);
    if (!n || n >= sizeof buf) strcpy(buf, "C:\\tmp\\");
    return strdup(buf);
}

API MSFILE *ms_tmpfile(void)
{
    char *dir = temp_dir_win();
    char *ldir = lsiw_path_a(dir);
    free(dir);
    char *tmpl = lsiw_strdup_printf("%s/lsiw-tmpXXXXXX", ldir ? ldir : "/tmp");
    free(ldir);
    int h = mkostemp(tmpl, O_CLOEXEC);
    if (h < 0) { free(tmpl); crt_set_errno_host(errno); return NULL; }
    unlink(tmpl);
    free(tmpl);
    HANDLE hd = lsiw_handle_from_fd(h, 0);
    int fd = crt_fd_from_handle(hd, O_MS_RDWR | O_MS_BINARY);
    if (fd < 0) { lsiw_handle_close(hd); return NULL; }
    MSFILE *f = stream_alloc(fd, MS_IORW | MS_IOREAD | MS_IOWRT);
    if (!f) _close(fd);
    return f;
}
API int tmpfile_s(MSFILE **out) { if (!out) return MS_EINVAL; *out = ms_tmpfile(); return *out ? 0 : *crt_errno(); }

static unsigned g_tmpnam_counter;
static void make_tmpnam(char *out, size_t n, const char *dir, const char *prefix)
{
    char *d = dir ? strdup(dir) : temp_dir_win();
    size_t dl = strlen(d);
    for (;;) {
        unsigned k = __atomic_add_fetch(&g_tmpnam_counter, 1, __ATOMIC_SEQ_CST);
        snprintf(out, n, "%s%s%s%x%x.tmp", d, dl && (d[dl - 1] == '\\' || d[dl - 1] == '/') ? "" : "\\", prefix ? prefix : "s", (unsigned)getpid() & 0xffff, k);
        char *l = lsiw_path_a(out);
        int exists = l && access(l, F_OK) == 0;
        free(l);
        if (!exists) break;
    }
    free(d);
}
API char *ms_tmpnam(char *buf)
{
    static __thread char sbuf[1024];
    char *o = buf ? buf : sbuf;
    make_tmpnam(o, buf ? 260 : sizeof sbuf, NULL, NULL);
    return o;
}
API int tmpnam_s(char *buf, size_t n) { if (!buf || !n) return MS_EINVAL; make_tmpnam(buf, n, NULL, NULL); return 0; }
API WCHAR *_wtmpnam(WCHAR *buf)
{
    static __thread WCHAR sbuf[1024];
    char tmp[1024];
    make_tmpnam(tmp, sizeof tmp, NULL, NULL);
    WCHAR *o = buf ? buf : sbuf;
    lsiw_u2w_buf(tmp, o, buf ? 260 : 1024);
    return o;
}
API int _wtmpnam_s(WCHAR *buf, size_t n) { if (!buf || !n) return MS_EINVAL; char tmp[1024]; make_tmpnam(tmp, sizeof tmp, NULL, NULL); lsiw_u2w_buf(tmp, buf, (int)n); return 0; }
API char *_tempnam(const char *dir, const char *prefix)
{
    char tmp[4096];
    const char *d = dir;
    char *l = d ? lsiw_path_a(d) : NULL;
    struct stat st;
    if (!l || stat(l, &st) < 0 || !S_ISDIR(st.st_mode)) d = NULL;
    free(l);
    make_tmpnam(tmp, sizeof tmp, d, prefix);
    size_t n = strlen(tmp) + 1;
    char *r = lsiw_heap_alloc(n, 0);
    memcpy(r, tmp, n);
    return r;
}
API WCHAR *_wtempnam(const WCHAR *dir, const WCHAR *prefix)
{
    char *d = dir ? lsiw_w2u(dir) : NULL, *p = prefix ? lsiw_w2u(prefix) : NULL;
    char *r = _tempnam(d, p);
    free(d); free(p);
    WCHAR *w = lsiw_u2w(r);
    size_t n = (lsiw_wcslen(w) + 1) * 2;
    WCHAR *out = lsiw_heap_alloc(n, 0);
    memcpy(out, w, n);
    free(w);
    lsiw_heap_free(r);
    return out;
}

/* ---- perror ---- */

const char *crt_strerror_text(int e);
API void ms_perror(const char *s)
{
    lsiw_crt_flush_all();
    MSFILE *f = crt_stderr();
    LOCKED(f, x, );
    if (s && *s) { s_write_text(f, x, s, strlen(s)); s_write_text(f, x, ": ", 2); }
    const char *m = crt_strerror_text(*crt_errno());
    s_write_text(f, x, m, strlen(m));
    s_write_text(f, x, "\n", 1);
    UNLOCK(f, x);
}
API void _wperror(const WCHAR *s) { char *u = s ? lsiw_w2u(s) : NULL; ms_perror(u); free(u); }

/* ---- _popen ---- */

struct startupinfo_a {
    DWORD cb; char *reserved, *desktop, *title;
    DWORD x, y, xsize, ysize, xchars, ychars, fill, flags;
    WORD show, cbres2; void *res2;
    HANDLE in, out, err;
};
struct procinfo_a { HANDLE process, thread; DWORD pid, tid; };

API MSFILE *_popen(const char *cmd, const char *mode)
{
    if (!cmd || !mode) { crt_invalid_parameter(); return NULL; }
    int reading = mode[0] == 'r';
    if (!reading && mode[0] != 'w') { crt_set_errno(MS_EINVAL); return NULL; }
    int binary = strchr(mode, 'b') != NULL;
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) { crt_set_errno_host(errno); return NULL; }
    int mine = reading ? p[0] : p[1], theirs = reading ? p[1] : p[0];
    lsiw_crt_flush_all();

    BOOL (WINAPI *cp)(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, struct startupinfo_a *, struct procinfo_a *) = k32("CreateProcessA");
    struct startupinfo_a si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.flags = 0x100;
    HANDLE th = lsiw_handle_from_fd(theirs, 0);
    si.in = reading ? lsiw_std_handle(0) : th;
    si.out = reading ? th : lsiw_std_handle(1);
    si.err = lsiw_std_handle(2);
    struct procinfo_a pi;
    char *line = crt_cmd_strip_quotes(cmd);
    HANDLE proc = NULL;
    int pid = 0;
    if (cp(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        proc = pi.process;
        BOOL (WINAPI *ch)(HANDLE) = k32("CloseHandle");
        ch(pi.thread);
    } else {
        /* Not a Windows program: run it through the Linux shell. */
        pid = fork();
        if (pid == 0) {
            dup2(theirs, reading ? 1 : 0);
            execl("/bin/sh", "sh", "-c", line, (char *)NULL);
            _exit(127);
        }
    }
    free(line);
    lsiw_handle_close(th);   /* closes `theirs` in this process */
    if (!proc && pid <= 0) { close(mine); crt_set_errno(MS_ENOENT); return NULL; }
    HANDLE mh = lsiw_handle_from_fd(mine, 0);
    int fd = crt_fd_from_handle(mh, (reading ? O_MS_RDONLY : O_MS_WRONLY) | (binary ? O_MS_BINARY : O_MS_TEXT));
    MSFILE *f = stream_alloc(fd, reading ? MS_IOREAD : MS_IOWRT);
    if (!f) { _close(fd); return NULL; }
    struct sx *x = X(f);
    x->proc = proc;
    x->pid = pid;
    return f;
}
API MSFILE *_wpopen(const WCHAR *cmd, const WCHAR *mode)
{
    char *a = lsiw_w2u(cmd), *b = lsiw_w2u(mode);
    MSFILE *f = _popen(a, b);
    free(a); free(b);
    return f;
}
API int _pclose(MSFILE *f)
{
    struct sx *x = X(f);
    if (!x || (!x->proc && !x->pid)) { crt_set_errno(MS_EINVAL); return -1; }
    HANDLE proc = x->proc;
    int pid = x->pid;
    x->proc = NULL; x->pid = 0;
    ms_fclose(f);
    if (proc) {
        DWORD (WINAPI *wait)(HANDLE, DWORD) = k32("WaitForSingleObject");
        BOOL (WINAPI *gec)(HANDLE, DWORD *) = k32("GetExitCodeProcess");
        BOOL (WINAPI *ch)(HANDLE) = k32("CloseHandle");
        wait(proc, INFINITE);
        DWORD code = 0;
        gec(proc, &code);
        ch(proc);
        return (int)code;
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) { crt_set_errno(MS_ECHILD); return -1; }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

/* ================================================================== */
/* Formatted output                                                     */
/* ================================================================== */

char *crt_vformat(const char *fmt, ms_va_list ap, int wide_default, int *len)
{
    char small[512];
    ms_va_list c = ap;
    int n = lsiw_ms_vsnprintf(small, sizeof small, fmt, c, wide_default);
    char *r = malloc((size_t)n + 1);
    if ((size_t)n < sizeof small) memcpy(r, small, (size_t)n + 1);
    else lsiw_ms_vsnprintf(r, (size_t)n + 1, fmt, ap, wide_default);
    if (len) *len = n;
    return r;
}
char *crt_vformat_w(const WCHAR *fmt, ms_va_list ap, int wide_default, int *len)
{
    char *f = lsiw_w2u(fmt);
    char *r = crt_vformat(f, ap, wide_default, len);
    free(f);
    return r;
}
static WCHAR *wformat(const WCHAR *fmt, ms_va_list ap, int wide_default, int *wlen)
{
    int n;
    char *u = crt_vformat_w(fmt, ap, wide_default, &n);
    int wl = lsiw_utf8_to_utf16(u, n, NULL, 0);
    WCHAR *w = malloc(((size_t)wl + 1) * 2);
    lsiw_utf8_to_utf16(u, n, w, wl);
    w[wl] = 0;
    free(u);
    *wlen = wl;
    return w;
}

static int vfprintf_impl(MSFILE *f, const char *fmt, ms_va_list ap, int wide_default)
{
    if (!fmt) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
    int n;
    char *s = crt_vformat(fmt, ap, wide_default, &n);
    int r = crt_fwrite_bytes(s, (size_t)n, f);
    free(s);
    return r < 0 ? -1 : n;
}
static int vfwprintf_impl(MSFILE *f, const WCHAR *fmt, ms_va_list ap, int wide_default)
{
    if (!fmt) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
    int n;
    char *s = crt_vformat_w(fmt, ap, wide_default, &n);
    int wl = lsiw_utf8_to_utf16(s, n, NULL, 0);
    int r = crt_fwrite_bytes(s, (size_t)n, f);
    free(s);
    return r < 0 ? -1 : wl;
}

API int ms_vfprintf(MSFILE *f, const char *fmt, ms_va_list ap) { return vfprintf_impl(f, fmt, ap, 0); }
API int ms_vprintf(const char *fmt, ms_va_list ap) { return vfprintf_impl(crt_stdout(), fmt, ap, 0); }
API int ms_fprintf(MSFILE *f, const char *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfprintf_impl(f, fmt, ap, 0); ms_va_end(ap); return r; }
API int ms_printf(const char *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfprintf_impl(crt_stdout(), fmt, ap, 0); ms_va_end(ap); return r; }
API int _vfprintf_l(MSFILE *f, const char *fmt, void *l, ms_va_list ap) { (void)l; return vfprintf_impl(f, fmt, ap, 0); }
API int _vprintf_l(const char *fmt, void *l, ms_va_list ap) { (void)l; return vfprintf_impl(crt_stdout(), fmt, ap, 0); }
API int _fprintf_l(MSFILE *f, const char *fmt, void *l, ...) { ms_va_list ap; ms_va_start(ap, l); int r = vfprintf_impl(f, fmt, ap, 0); ms_va_end(ap); return r; }
API int _printf_l(const char *fmt, void *l, ...) { ms_va_list ap; ms_va_start(ap, l); int r = vfprintf_impl(crt_stdout(), fmt, ap, 0); ms_va_end(ap); return r; }
API int ms_vfwprintf(MSFILE *f, const WCHAR *fmt, ms_va_list ap) { return vfwprintf_impl(f, fmt, ap, 1); }
API int ms_vwprintf(const WCHAR *fmt, ms_va_list ap) { return vfwprintf_impl(crt_stdout(), fmt, ap, 1); }
API int ms_fwprintf(MSFILE *f, const WCHAR *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfwprintf_impl(f, fmt, ap, 1); ms_va_end(ap); return r; }
API int ms_wprintf(const WCHAR *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfwprintf_impl(crt_stdout(), fmt, ap, 1); ms_va_end(ap); return r; }
API int _vfwprintf_l(MSFILE *f, const WCHAR *fmt, void *l, ms_va_list ap) { (void)l; return vfwprintf_impl(f, fmt, ap, 1); }
API int _fwprintf_l(MSFILE *f, const WCHAR *fmt, void *l, ...) { ms_va_list ap; ms_va_start(ap, l); int r = vfwprintf_impl(f, fmt, ap, 1); ms_va_end(ap); return r; }
API int _wprintf_l(const WCHAR *fmt, void *l, ...) { ms_va_list ap; ms_va_start(ap, l); int r = vfwprintf_impl(crt_stdout(), fmt, ap, 1); ms_va_end(ap); return r; }

/* Copy a formatted result into a caller buffer with one of the Microsoft
 * truncation policies. */
enum { POL_C99, POL_LEGACY, POL_SECURE, POL_SECURE_COUNT, POL_UNBOUNDED };
#define CRT_TRUNCATE ((size_t)-1)

static int place_a(char *buf, size_t n, size_t count, const char *s, int len, int pol)
{
    size_t l = (size_t)len;
    switch (pol) {
    case POL_UNBOUNDED:
        memcpy(buf, s, l + 1);
        return len;
    case POL_C99:
        if (buf && n) { size_t k = l < n ? l : n - 1; memcpy(buf, s, k); buf[k] = 0; }
        return len;
    case POL_LEGACY:          /* _snprintf: NUL only if it fits, -1 when truncated */
        if (!buf && !n) return len;
        if (l < n) { memcpy(buf, s, l + 1); return len; }
        memcpy(buf, s, n);
        return l == n ? len : -1;
    case POL_SECURE:          /* sprintf_s */
        if (!buf || !n) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
        if (l >= n) { buf[0] = 0; crt_invalid_parameter(); crt_set_errno(MS_ERANGE); return -1; }
        memcpy(buf, s, l + 1);
        return len;
    default: {                /* _snprintf_s */
        if (!buf || !n) { if (!buf && !n && !count) return 0; crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
        size_t lim = count == CRT_TRUNCATE ? n - 1 : (count < n ? count : n - 1);
        if (l <= lim) { memcpy(buf, s, l); buf[l] = 0; return len; }
        if (count == CRT_TRUNCATE || count < n) { memcpy(buf, s, lim); buf[lim] = 0; return -1; }
        buf[0] = 0; crt_invalid_parameter(); crt_set_errno(MS_ERANGE); return -1; }
    }
}
static int place_w(WCHAR *buf, size_t n, size_t count, const WCHAR *s, int len, int pol)
{
    size_t l = (size_t)len;
    switch (pol) {
    case POL_UNBOUNDED:
        memcpy(buf, s, (l + 1) * 2);
        return len;
    case POL_C99:
        if (buf && n) { size_t k = l < n ? l : n - 1; memcpy(buf, s, k * 2); buf[k] = 0; }
        return len;
    case POL_LEGACY:
        if (!buf && !n) return len;
        if (l < n) { memcpy(buf, s, (l + 1) * 2); return len; }
        memcpy(buf, s, n * 2);
        return l == n ? len : -1;
    case POL_SECURE:
        if (!buf || !n) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
        if (l >= n) { buf[0] = 0; crt_invalid_parameter(); crt_set_errno(MS_ERANGE); return -1; }
        memcpy(buf, s, (l + 1) * 2);
        return len;
    default: {
        if (!buf || !n) { if (!buf && !n && !count) return 0; crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
        size_t lim = count == CRT_TRUNCATE ? n - 1 : (count < n ? count : n - 1);
        if (l <= lim) { memcpy(buf, s, l * 2); buf[l] = 0; return len; }
        if (count == CRT_TRUNCATE || count < n) { memcpy(buf, s, lim * 2); buf[lim] = 0; return -1; }
        buf[0] = 0; crt_invalid_parameter(); crt_set_errno(MS_ERANGE); return -1; }
    }
}

static int vs_a(char *buf, size_t n, size_t count, const char *fmt, ms_va_list ap, int pol, int wide_default)
{
    if (!fmt) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
    int len;
    char *s = crt_vformat(fmt, ap, wide_default, &len);
    int r = place_a(buf, n, count, s, len, pol);
    free(s);
    return r;
}
static int vs_w(WCHAR *buf, size_t n, size_t count, const WCHAR *fmt, ms_va_list ap, int pol, int wide_default)
{
    if (!fmt) { crt_invalid_parameter(); crt_set_errno(MS_EINVAL); return -1; }
    int len;
    WCHAR *s = wformat(fmt, ap, wide_default, &len);
    int r = place_w(buf, n, count, s, len, pol);
    free(s);
    return r;
}

#define VA_CALL(last, expr) ms_va_list ap; ms_va_start(ap, last); int r_ = (expr); ms_va_end(ap); return r_

API int ms_vsprintf(char *b, const char *f, ms_va_list ap) { return vs_a(b, 0, 0, f, ap, POL_UNBOUNDED, 0); }
API int ms_sprintf(char *b, const char *f, ...) { VA_CALL(f, vs_a(b, 0, 0, f, ap, POL_UNBOUNDED, 0)); }
API int _vsnprintf(char *b, size_t n, const char *f, ms_va_list ap) { return vs_a(b, n, 0, f, ap, POL_LEGACY, 0); }
API int _snprintf(char *b, size_t n, const char *f, ...) { VA_CALL(f, vs_a(b, n, 0, f, ap, POL_LEGACY, 0)); }
API int ms_vsnprintf(char *b, size_t n, const char *f, ms_va_list ap) { return vs_a(b, n, 0, f, ap, POL_C99, 0); }
API int ms_snprintf(char *b, size_t n, const char *f, ...) { VA_CALL(f, vs_a(b, n, 0, f, ap, POL_C99, 0)); }
API int vsprintf_s(char *b, size_t n, const char *f, ms_va_list ap) { return vs_a(b, n, 0, f, ap, POL_SECURE, 0); }
API int sprintf_s(char *b, size_t n, const char *f, ...) { VA_CALL(f, vs_a(b, n, 0, f, ap, POL_SECURE, 0)); }
API int _vsnprintf_s(char *b, size_t n, size_t c, const char *f, ms_va_list ap) { return vs_a(b, n, c, f, ap, POL_SECURE_COUNT, 0); }
API int _snprintf_s(char *b, size_t n, size_t c, const char *f, ...) { VA_CALL(f, vs_a(b, n, c, f, ap, POL_SECURE_COUNT, 0)); }
API int _vscprintf(const char *f, ms_va_list ap) { return vs_a(NULL, 0, 0, f, ap, POL_C99, 0); }
API int _scprintf(const char *f, ...) { VA_CALL(f, vs_a(NULL, 0, 0, f, ap, POL_C99, 0)); }
API int _vsnprintf_l(char *b, size_t n, const char *f, void *l, ms_va_list ap) { (void)l; return vs_a(b, n, 0, f, ap, POL_LEGACY, 0); }
API int _vsprintf_l(char *b, const char *f, void *l, ms_va_list ap) { (void)l; return vs_a(b, 0, 0, f, ap, POL_UNBOUNDED, 0); }
API int _sprintf_l(char *b, const char *f, void *l, ...) { VA_CALL(l, vs_a(b, 0, 0, f, ap, POL_UNBOUNDED, 0)); }
API int _snprintf_l(char *b, size_t n, const char *f, void *l, ...) { VA_CALL(l, vs_a(b, n, 0, f, ap, POL_LEGACY, 0)); }
API int _vscprintf_l(const char *f, void *l, ms_va_list ap) { (void)l; return vs_a(NULL, 0, 0, f, ap, POL_C99, 0); }

/* msvcrt's swprintf/vswprintf take no count; _snwprintf/_vsnwprintf do. */
API int msvcrt_vswprintf(WCHAR *b, const WCHAR *f, ms_va_list ap) { return vs_w(b, 0, 0, f, ap, POL_UNBOUNDED, 1); }
/* ISO swprintf/vswprintf (with count), as exported by later runtimes. */
API int iso_vswprintf(WCHAR *b, size_t n, const WCHAR *f, ms_va_list ap) { int r = vs_w(b, n, 0, f, ap, POL_C99, 1); return r >= 0 && (size_t)r >= n ? -1 : r; }
API int _vsnwprintf(WCHAR *b, size_t n, const WCHAR *f, ms_va_list ap) { return vs_w(b, n, 0, f, ap, POL_LEGACY, 1); }
API int _snwprintf(WCHAR *b, size_t n, const WCHAR *f, ...) { VA_CALL(f, vs_w(b, n, 0, f, ap, POL_LEGACY, 1)); }
API int vswprintf_s(WCHAR *b, size_t n, const WCHAR *f, ms_va_list ap) { return vs_w(b, n, 0, f, ap, POL_SECURE, 1); }
API int swprintf_s(WCHAR *b, size_t n, const WCHAR *f, ...) { VA_CALL(f, vs_w(b, n, 0, f, ap, POL_SECURE, 1)); }
API int _vsnwprintf_s(WCHAR *b, size_t n, size_t c, const WCHAR *f, ms_va_list ap) { return vs_w(b, n, c, f, ap, POL_SECURE_COUNT, 1); }
API int _snwprintf_s(WCHAR *b, size_t n, size_t c, const WCHAR *f, ...) { VA_CALL(f, vs_w(b, n, c, f, ap, POL_SECURE_COUNT, 1)); }
API int _vscwprintf(const WCHAR *f, ms_va_list ap) { return vs_w(NULL, 0, 0, f, ap, POL_C99, 1); }
API int _scwprintf(const WCHAR *f, ...) { VA_CALL(f, vs_w(NULL, 0, 0, f, ap, POL_C99, 1)); }
API int _vsnwprintf_l(WCHAR *b, size_t n, const WCHAR *f, void *l, ms_va_list ap) { (void)l; return vs_w(b, n, 0, f, ap, POL_LEGACY, 1); }
API int _vswprintf(WCHAR *b, const WCHAR *f, ms_va_list ap) { return vs_w(b, 0, 0, f, ap, POL_UNBOUNDED, 1); }
API int _swprintf(WCHAR *b, const WCHAR *f, ...) { VA_CALL(f, vs_w(b, 0, 0, f, ap, POL_UNBOUNDED, 1)); }
API int _vswprintf_c(WCHAR *b, size_t n, const WCHAR *f, ms_va_list ap) { return vs_w(b, n, 0, f, ap, POL_LEGACY, 1); }
API int _swprintf_c(WCHAR *b, size_t n, const WCHAR *f, ...) { VA_CALL(f, vs_w(b, n, 0, f, ap, POL_LEGACY, 1)); }

API int printf_s(const char *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfprintf_impl(crt_stdout(), fmt, ap, 0); ms_va_end(ap); return r; }
API int fprintf_s(MSFILE *f, const char *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfprintf_impl(f, fmt, ap, 0); ms_va_end(ap); return r; }
API int vprintf_s(const char *fmt, ms_va_list ap) { return vfprintf_impl(crt_stdout(), fmt, ap, 0); }
API int vfprintf_s(MSFILE *f, const char *fmt, ms_va_list ap) { return vfprintf_impl(f, fmt, ap, 0); }
API int wprintf_s(const WCHAR *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfwprintf_impl(crt_stdout(), fmt, ap, 1); ms_va_end(ap); return r; }
API int fwprintf_s(MSFILE *f, const WCHAR *fmt, ...) { ms_va_list ap; ms_va_start(ap, fmt); int r = vfwprintf_impl(f, fmt, ap, 1); ms_va_end(ap); return r; }
API int vwprintf_s(const WCHAR *fmt, ms_va_list ap) { return vfwprintf_impl(crt_stdout(), fmt, ap, 1); }
API int vfwprintf_s(MSFILE *f, const WCHAR *fmt, ms_va_list ap) { return vfwprintf_impl(f, fmt, ap, 1); }

/* ---- ucrt __stdio_common_* ---- */

#define PO_LEGACY_NULLTERM 1ull
#define PO_STD_SNPRINTF    2ull
#define PO_LEGACY_WIDE     4ull
#define PO_LEGACY_EXP3     0x10ull   /* _CRT_INTERNAL_PRINTF_LEGACY_THREE_DIGIT_EXPONENTS */

/* ucrt entry points print two exponent digits unless the caller asks for the
 * legacy three; the setting lives for the duration of the call. */
extern __thread int lsiw_printf_ucrt_call;
static int ucrt_enter(uint64_t o) { int old = lsiw_printf_ucrt_call; lsiw_printf_ucrt_call = !(o & PO_LEGACY_EXP3); return old; }
static void ucrt_restore(int *old) { lsiw_printf_ucrt_call = *old; }
#define UCRT_SCOPE(o) int ucrt_saved_ __attribute__((cleanup(ucrt_restore))) = ucrt_enter(o)

static int common_policy(uint64_t o)
{
    if (o & PO_STD_SNPRINTF) return POL_C99;
    if (o & PO_LEGACY_NULLTERM) return POL_LEGACY;
    return POL_C99;
}

API int __stdio_common_vfprintf(uint64_t o, MSFILE *f, const char *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)o; (void)l; return vfprintf_impl(f, fmt, ap, 0); }
API int __stdio_common_vfprintf_s(uint64_t o, MSFILE *f, const char *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)o; (void)l; return vfprintf_impl(f, fmt, ap, 0); }
API int __stdio_common_vfprintf_p(uint64_t o, MSFILE *f, const char *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)o; (void)l; return vfprintf_impl(f, fmt, ap, 0); }
API int __stdio_common_vfwprintf(uint64_t o, MSFILE *f, const WCHAR *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)l; return vfwprintf_impl(f, fmt, ap, (o & PO_LEGACY_WIDE) != 0); }
API int __stdio_common_vfwprintf_s(uint64_t o, MSFILE *f, const WCHAR *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)l; return vfwprintf_impl(f, fmt, ap, (o & PO_LEGACY_WIDE) != 0); }
API int __stdio_common_vfwprintf_p(uint64_t o, MSFILE *f, const WCHAR *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)l; return vfwprintf_impl(f, fmt, ap, (o & PO_LEGACY_WIDE) != 0); }
API int __stdio_common_vsprintf(uint64_t o, char *b, size_t n, const char *fmt, void *l, ms_va_list ap)
{ UCRT_SCOPE(o);
    (void)l;
    if (!b && !n) return vs_a(NULL, 0, 0, fmt, ap, POL_C99, 0);
    return vs_a(b, n, 0, fmt, ap, n == (size_t)-1 ? POL_UNBOUNDED : common_policy(o), 0);
}
API int __stdio_common_vsprintf_s(uint64_t o, char *b, size_t n, const char *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)o; (void)l; return vs_a(b, n, 0, fmt, ap, POL_SECURE, 0); }
API int __stdio_common_vsnprintf_s(uint64_t o, char *b, size_t n, size_t c, const char *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)o; (void)l; return vs_a(b, n, c, fmt, ap, POL_SECURE_COUNT, 0); }
API int __stdio_common_vsprintf_p(uint64_t o, char *b, size_t n, const char *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)o; (void)l; return vs_a(b, n, 0, fmt, ap, POL_SECURE, 0); }
API int __stdio_common_vswprintf(uint64_t o, WCHAR *b, size_t n, const WCHAR *fmt, void *l, ms_va_list ap)
{ UCRT_SCOPE(o);
    (void)l;
    int wd = (o & PO_LEGACY_WIDE) != 0;
    if (!b && !n) return vs_w(NULL, 0, 0, fmt, ap, POL_C99, wd);
    if (n == (size_t)-1) return vs_w(b, 0, 0, fmt, ap, POL_UNBOUNDED, wd);
    int pol = common_policy(o);
    int r = vs_w(b, n, 0, fmt, ap, pol, wd);
    /* ISO swprintf (no flags): -1 when the output did not fit. */
    if (!(o & (PO_STD_SNPRINTF | PO_LEGACY_NULLTERM)) && r >= 0 && (size_t)r >= n) return -1;
    return r;
}
API int __stdio_common_vswprintf_s(uint64_t o, WCHAR *b, size_t n, const WCHAR *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)l; return vs_w(b, n, 0, fmt, ap, POL_SECURE, (o & PO_LEGACY_WIDE) != 0); }
API int __stdio_common_vsnwprintf_s(uint64_t o, WCHAR *b, size_t n, size_t c, const WCHAR *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)l; return vs_w(b, n, c, fmt, ap, POL_SECURE_COUNT, (o & PO_LEGACY_WIDE) != 0); }
API int __stdio_common_vswprintf_p(uint64_t o, WCHAR *b, size_t n, const WCHAR *fmt, void *l, ms_va_list ap) { UCRT_SCOPE(o); (void)l; return vs_w(b, n, 0, fmt, ap, POL_SECURE, (o & PO_LEGACY_WIDE) != 0); }

/* ================================================================== */
/* Formatted input                                                      */
/* ================================================================== */

struct str_src { const unsigned char *s; size_t i, n; };
static int str_get(void *c) { struct str_src *s = c; if (s->i >= s->n || !s->s[s->i]) { s->i++; return -1; } return s->s[s->i++]; }
static void str_unget(int ch, void *c) { (void)ch; struct str_src *s = c; if (s->i) s->i--; }

struct stream_src { MSFILE *f; struct sx *x; unsigned char q[8]; int nq; int wide; };
static int stream_get(void *c)
{
    struct stream_src *s = c;
    if (s->nq) return s->q[--s->nq];
    if (!s->wide) return s_getc(s->f, s->x);
    int w = s_getwc(s->f, s->x);
    if (w == WEOF16) return -1;
    char b[4];
    int l;
    if (w < 0x80) { b[0] = (char)w; l = 1; }
    else if (w < 0x800) { b[0] = (char)(0xC0 | (w >> 6)); b[1] = (char)(0x80 | (w & 0x3F)); l = 2; }
    else { b[0] = (char)(0xE0 | (w >> 12)); b[1] = (char)(0x80 | ((w >> 6) & 0x3F)); b[2] = (char)(0x80 | (w & 0x3F)); l = 3; }
    for (int i = l - 1; i >= 1; i--) s->q[s->nq++] = (unsigned char)b[i];
    return (unsigned char)b[0];
}
static void stream_unget(int ch, void *c)
{
    struct stream_src *s = c;
    if (!s->wide) { s_ungetc(s->f, s->x, ch); return; }
    if (s->nq < (int)sizeof s->q) s->q[s->nq++] = (unsigned char)ch;
}
static void stream_src_finish(struct stream_src *s)
{
    /* Return bytes queued from a decoded wide character to the stream. */
    if (!s->nq) return;
    char b[8];
    int n = 0;
    while (s->nq) b[n++] = (char)s->q[--s->nq];
    b[n] = 0;
    WCHAR w[8];
    int wl = lsiw_utf8_to_utf16(b, n, w, 8);
    for (int i = wl - 1; i >= 0; i--) s_ungetwc(s->f, s->x, w[i]);
}

static int vfscanf_impl(MSFILE *f, const char *fmt, ms_va_list ap, int flags, int wide_input)
{
    if (!fmt) { crt_invalid_parameter(); return -1; }
    LOCKED(f, x, -1);
    struct stream_src s = { f, x, { 0 }, 0, wide_input && crt_stream_is_wide_mode(f) };
    int r = lsiw_ms_vscanf_ex(stream_get, stream_unget, &s, fmt, ap, flags);
    stream_src_finish(&s);
    UNLOCK(f, x);
    return r;
}
static int vsscanf_impl(const char *src, size_t n, const char *fmt, ms_va_list ap, int flags)
{
    if (!src || !fmt) { crt_invalid_parameter(); return -1; }
    struct str_src s = { (const unsigned char *)src, 0, n };
    return lsiw_ms_vscanf_ex(str_get, str_unget, &s, fmt, ap, flags);
}
static int vfwscanf_impl(MSFILE *f, const WCHAR *fmt, ms_va_list ap, int flags)
{
    if (!fmt) { crt_invalid_parameter(); return -1; }
    char *u = lsiw_w2u(fmt);
    int r = vfscanf_impl(f, u, ap, flags, 1);
    free(u);
    return r;
}
static size_t ms_min_len(const WCHAR *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
static int vswscanf_impl(const WCHAR *src, size_t n, const WCHAR *fmt, ms_va_list ap, int flags)
{
    if (!src || !fmt) { crt_invalid_parameter(); return -1; }
    char *s = lsiw_w2u_n(src, n == (size_t)-1 ? -1 : (int)ms_min_len(src, n));
    char *u = lsiw_w2u(fmt);
    int r = vsscanf_impl(s, (size_t)-1, u, ap, flags);
    free(s); free(u);
    return r;
}

#define SCAN_A 0
#define SCAN_AS LSIW_SCAN_SECURE
#define SCAN_W LSIW_SCAN_WIDE
#define SCAN_WS (LSIW_SCAN_WIDE | LSIW_SCAN_SECURE)

API int ms_vfscanf(MSFILE *f, const char *fmt, ms_va_list ap) { return vfscanf_impl(f, fmt, ap, SCAN_A, 0); }
API int ms_vscanf(const char *fmt, ms_va_list ap) { return vfscanf_impl(crt_stdin(), fmt, ap, SCAN_A, 0); }
API int ms_vsscanf(const char *s, const char *fmt, ms_va_list ap) { return vsscanf_impl(s, (size_t)-1, fmt, ap, SCAN_A); }
API int ms_fscanf(MSFILE *f, const char *fmt, ...) { VA_CALL(fmt, vfscanf_impl(f, fmt, ap, SCAN_A, 0)); }
API int ms_scanf(const char *fmt, ...) { VA_CALL(fmt, vfscanf_impl(crt_stdin(), fmt, ap, SCAN_A, 0)); }
API int ms_sscanf(const char *s, const char *fmt, ...) { VA_CALL(fmt, vsscanf_impl(s, (size_t)-1, fmt, ap, SCAN_A)); }
API int fscanf_s(MSFILE *f, const char *fmt, ...) { VA_CALL(fmt, vfscanf_impl(f, fmt, ap, SCAN_AS, 0)); }
API int scanf_s(const char *fmt, ...) { VA_CALL(fmt, vfscanf_impl(crt_stdin(), fmt, ap, SCAN_AS, 0)); }
API int sscanf_s(const char *s, const char *fmt, ...) { VA_CALL(fmt, vsscanf_impl(s, (size_t)-1, fmt, ap, SCAN_AS)); }
API int vfscanf_s(MSFILE *f, const char *fmt, ms_va_list ap) { return vfscanf_impl(f, fmt, ap, SCAN_AS, 0); }
API int vscanf_s(const char *fmt, ms_va_list ap) { return vfscanf_impl(crt_stdin(), fmt, ap, SCAN_AS, 0); }
API int vsscanf_s(const char *s, const char *fmt, ms_va_list ap) { return vsscanf_impl(s, (size_t)-1, fmt, ap, SCAN_AS); }
API int _snscanf(const char *s, size_t n, const char *fmt, ...) { VA_CALL(fmt, vsscanf_impl(s, n, fmt, ap, SCAN_A)); }
API int _snscanf_s(const char *s, size_t n, const char *fmt, ...) { VA_CALL(fmt, vsscanf_impl(s, n, fmt, ap, SCAN_AS)); }
API int _sscanf_l(const char *s, const char *fmt, void *l, ...) { VA_CALL(l, vsscanf_impl(s, (size_t)-1, fmt, ap, SCAN_A)); }
API int ms_vfwscanf(MSFILE *f, const WCHAR *fmt, ms_va_list ap) { return vfwscanf_impl(f, fmt, ap, SCAN_W); }
API int ms_vwscanf(const WCHAR *fmt, ms_va_list ap) { return vfwscanf_impl(crt_stdin(), fmt, ap, SCAN_W); }
API int ms_vswscanf(const WCHAR *s, const WCHAR *fmt, ms_va_list ap) { return vswscanf_impl(s, (size_t)-1, fmt, ap, SCAN_W); }
API int ms_fwscanf(MSFILE *f, const WCHAR *fmt, ...) { VA_CALL(fmt, vfwscanf_impl(f, fmt, ap, SCAN_W)); }
API int ms_wscanf(const WCHAR *fmt, ...) { VA_CALL(fmt, vfwscanf_impl(crt_stdin(), fmt, ap, SCAN_W)); }
API int ms_swscanf(const WCHAR *s, const WCHAR *fmt, ...) { VA_CALL(fmt, vswscanf_impl(s, (size_t)-1, fmt, ap, SCAN_W)); }
API int fwscanf_s(MSFILE *f, const WCHAR *fmt, ...) { VA_CALL(fmt, vfwscanf_impl(f, fmt, ap, SCAN_WS)); }
API int wscanf_s(const WCHAR *fmt, ...) { VA_CALL(fmt, vfwscanf_impl(crt_stdin(), fmt, ap, SCAN_WS)); }
API int swscanf_s(const WCHAR *s, const WCHAR *fmt, ...) { VA_CALL(fmt, vswscanf_impl(s, (size_t)-1, fmt, ap, SCAN_WS)); }
API int _snwscanf(const WCHAR *s, size_t n, const WCHAR *fmt, ...) { VA_CALL(fmt, vswscanf_impl(s, n, fmt, ap, SCAN_W)); }
API int _snwscanf_s(const WCHAR *s, size_t n, const WCHAR *fmt, ...) { VA_CALL(fmt, vswscanf_impl(s, n, fmt, ap, SCAN_WS)); }

#define SO_SECURE 1ull
#define SO_LEGACY_WIDE 2ull
static int scan_flags(uint64_t o, int wide_fn)
{
    int fl = (o & SO_SECURE) ? LSIW_SCAN_SECURE : 0;
    if (wide_fn && (o & SO_LEGACY_WIDE)) fl |= LSIW_SCAN_WIDE;
    return fl;
}
API int __stdio_common_vfscanf(uint64_t o, MSFILE *f, const char *fmt, void *l, ms_va_list ap) { (void)l; return vfscanf_impl(f, fmt, ap, scan_flags(o, 0), 0); }
API int __stdio_common_vsscanf(uint64_t o, const char *s, size_t n, const char *fmt, void *l, ms_va_list ap) { (void)l; return vsscanf_impl(s, n, fmt, ap, scan_flags(o, 0)); }
API int __stdio_common_vfwscanf(uint64_t o, MSFILE *f, const WCHAR *fmt, void *l, ms_va_list ap) { (void)l; return vfwscanf_impl(f, fmt, ap, scan_flags(o, 1)); }
API int __stdio_common_vswscanf(uint64_t o, const WCHAR *s, size_t n, const WCHAR *fmt, void *l, ms_va_list ap) { (void)l; return vswscanf_impl(s, n, fmt, ap, scan_flags(o, 1)); }

/* ================================================================== */
/* Console (conio.h)                                                    */
/* ================================================================== */

static int g_ungetch = -1;

static int raw_key(int echo)
{
    if (g_ungetch >= 0) { int c = g_ungetch; g_ungetch = -1; return c; }
    lsiw_crt_flush_all();
    struct termios t, saved;
    int tty = isatty(0) && tcgetattr(0, &t) == 0;
    if (tty) {
        saved = t;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    }
    unsigned char c;
    ssize_t r;
    do r = read(0, &c, 1); while (r < 0 && errno == EINTR);
    int key = r == 1 ? c : -1;
    if (key == '\n') key = '\r';             /* Enter reads as CR from _getch */
    if (key == 0x7f) key = 8;                /* Backspace */
    if (key == 0x1b && tty) {
        /* Arrow keys and friends: ESC [ X -> 0xE0 prefix + scan code. */
        struct pollfd pf = { 0, POLLIN, 0 };
        if (poll(&pf, 1, 30) > 0) {
            unsigned char seq[4] = { 0 };
            ssize_t k = read(0, seq, sizeof seq);
            if (k >= 2 && seq[0] == '[') {
                int sc = 0;
                switch (seq[1]) { case 'A': sc = 72; break; case 'B': sc = 80; break; case 'C': sc = 77; break; case 'D': sc = 75; break;
                                  case 'H': sc = 71; break; case 'F': sc = 79; break;
                                  case '3': sc = 83; break; case '2': sc = 82; break; case '5': sc = 73; break; case '6': sc = 81; break; }
                if (sc) { g_ungetch = sc; key = 0xE0; }
            }
        }
    }
    if (tty) tcsetattr(0, TCSANOW, &saved);
    if (echo && key >= 0 && key != 0xE0) { char ch = (char)key; ssize_t w = write(1, &ch, 1); (void)w; }
    return key;
}
API int _getch(void) { return raw_key(0); }
API int _getche(void) { return raw_key(1); }
API unsigned _getwch(void) { int c = raw_key(0); return c < 0 ? WEOF16 : (unsigned)c; }
API unsigned _getwche(void) { int c = raw_key(1); return c < 0 ? WEOF16 : (unsigned)c; }
API int _getch_nolock(void) { return raw_key(0); }
API int _getche_nolock(void) { return raw_key(1); }
API int _ungetch(int c) { if (g_ungetch >= 0) return MS_EOF; g_ungetch = c; return c; }
API unsigned _ungetwch(unsigned c) { if (g_ungetch >= 0) return WEOF16; g_ungetch = (int)c; return c; }
API int _kbhit(void)
{
    if (g_ungetch >= 0) return 1;
    lsiw_crt_flush_all();
    struct termios t, saved;
    int tty = isatty(0) && tcgetattr(0, &t) == 0;
    if (tty) { saved = t; t.c_lflag &= ~(tcflag_t)(ICANON | ECHO); tcsetattr(0, TCSANOW, &t); }
    struct pollfd pf = { 0, POLLIN, 0 };
    int r = poll(&pf, 1, 0) > 0;
    if (tty) tcsetattr(0, TCSANOW, &saved);
    return r;
}
static void con_write(const char *s, size_t n) { lsiw_crt_flush_all(); int fd = isatty(1) ? 1 : 2; ssize_t r = write(fd, s, n); (void)r; }
API int _putch(int c) { char ch = (char)c; con_write(&ch, 1); return c & 0xff; }
API int _putch_nolock(int c) { return _putch(c); }
API unsigned _putwch(WCHAR c) { WCHAR w[2] = { c, 0 }; char *u = lsiw_w2u(w); con_write(u, strlen(u)); free(u); return c; }
API int _cputs(const char *s) { con_write(s, strlen(s)); return 0; }
API int _cputws(const WCHAR *s) { char *u = lsiw_w2u(s); con_write(u, strlen(u)); free(u); return 0; }
API int _cprintf(const char *fmt, ...)
{
    ms_va_list ap; ms_va_start(ap, fmt);
    int n; char *s = crt_vformat(fmt, ap, 0, &n);
    ms_va_end(ap);
    con_write(s, (size_t)n);
    free(s);
    return n;
}
API int _cwprintf(const WCHAR *fmt, ...)
{
    ms_va_list ap; ms_va_start(ap, fmt);
    int n; char *s = crt_vformat_w(fmt, ap, 1, &n);
    ms_va_end(ap);
    con_write(s, (size_t)n);
    free(s);
    return n;
}
API int _vcprintf(const char *fmt, ms_va_list ap) { int n; char *s = crt_vformat(fmt, ap, 0, &n); con_write(s, (size_t)n); free(s); return n; }
API char *_cgets(char *buf)
{
    int max = (unsigned char)buf[0];
    char *line = NULL;
    MSFILE *in = crt_stdin();
    char tmp[1024];
    if (ms_fgets(tmp, max + 1 < (int)sizeof tmp ? max + 1 : (int)sizeof tmp, in)) line = tmp;
    size_t l = line ? strcspn(line, "\r\n") : 0;
    buf[1] = (char)l;
    if (line) memcpy(buf + 2, line, l);
    buf[2 + l] = 0;
    return buf + 2;
}

const struct lsiw_export lsiw_crt_stdio_exports[] = {
    /* io.h */
    E(_open), E(_wopen), E(_sopen), E(_wsopen), E(_sopen_s), E(_wsopen_s), E(_sopen_dispatch), E(_wsopen_dispatch),
    E(_creat), E(_wcreat), E(_close), E(_read), E(_write), E(_lseek), E(_lseeki64), E(_tell), E(_telli64), E(_eof),
    E(_filelength), E(_filelengthi64), E(_chsize), E(_chsize_s), E(_commit), E(_dup), E(_dup2), E(_pipe), E(_isatty),
    E(_get_osfhandle), E(_open_osfhandle), E(_setmode), E(_locking), E(_access), E(_waccess), E(_access_s), E(_waccess_s),
    E(_chmod), E(_wchmod), E(_umask), E(_umask_s), E(_unlink), E(_wunlink), EM(remove), E(_wremove), EM(rename), E(_wrename),
    E(_mktemp), E(_mktemp_s), E(_wmktemp), E(_wmktemp_s),
    EX("open", _open), EX("close", _close), EX("read", _read), EX("write", _write), EX("lseek", _lseek), EX("dup", _dup),
    EX("dup2", _dup2), EX("isatty", _isatty), EX("access", _access), EX("chmod", _chmod), EX("umask", _umask), EX("unlink", _unlink),
    EX("creat", _creat), EX("tell", _tell), EX("eof", _eof), EX("filelength", _filelength), EX("chsize", _chsize),
    EX("setmode", _setmode), EX("mktemp", _mktemp), EX("locking", _locking), EX("sopen", _sopen),
    /* direct.h */
    E(_mkdir), E(_wmkdir), E(_rmdir), E(_wrmdir), E(_chdir), E(_wchdir), E(_getcwd), E(_wgetcwd), E(_getdcwd), E(_wgetdcwd),
    E(_getdrive), E(_chdrive), E(_getdrives), E(_getdiskfree),
    EX("mkdir", _mkdir), EX("rmdir", _rmdir), EX("chdir", _chdir), EX("getcwd", _getcwd),
    /* paths */
    E(_fullpath), E(_wfullpath), E(_splitpath), E(_splitpath_s), E(_wsplitpath), E(_wsplitpath_s),
    E(_makepath), E(_makepath_s), E(_wmakepath), E(_wmakepath_s), E(_searchenv), E(_searchenv_s), E(_wsearchenv), E(_wsearchenv_s),
    /* stat */
    E(_stat), E(_wstat), E(_fstat), E(_stat64i32), E(_wstat64i32), E(_fstat64i32), E(_stat64), E(_wstat64), E(_fstat64),
    E(_stati64), E(_wstati64), E(_fstati64), E(_stat32), E(_wstat32), E(_fstat32), E(_stat32i64), E(_wstat32i64), E(_fstat32i64),
    EX("stat", _stat), EX("fstat", _fstat),
    E(_utime64), E(_wutime64), E(_utime32), E(_wutime32), E(_futime64), E(_futime32),
    EX("_utime", _utime64), EX("_wutime", _wutime64), EX("_futime", _futime64), EX("utime", _utime64),
    /* find */
    E(_findfirst), E(_findnext), E(_wfindfirst), E(_wfindnext), E(_findfirst32), E(_findnext32), E(_wfindfirst32), E(_wfindnext32),
    E(_findfirst32i64), E(_findnext32i64), E(_wfindfirst32i64), E(_wfindnext32i64), E(_findfirst64i32), E(_findnext64i32),
    E(_wfindfirst64i32), E(_wfindnext64i32), E(_findfirst64), E(_findnext64), E(_wfindfirst64), E(_wfindnext64),
    E(_findfirsti64), E(_findnexti64), E(_wfindfirsti64), E(_wfindnexti64), E(_findclose),
    /* streams */
    EM(fopen), E(_wfopen), E(_fsopen), E(_wfsopen), E(fopen_s), E(_wfopen_s), EM(freopen), E(_wfreopen), E(freopen_s), E(_wfreopen_s),
    E(_fdopen), E(_wfdopen), EX("fdopen", _fdopen), EM(fclose), E(_fclose_nolock), E(_fcloseall), E(_rmtmp), EM(fflush), E(_fflush_nolock), E(_flushall),
    EM(fgetc), EX("getc", ms_fgetc), EM(fputc), EX("putc", ms_fputc), EM(getchar), EM(putchar), EM(ungetc),
    EX("_fgetchar", ms_getchar), EX("_fputchar", ms_putchar), EX("_fgetc_nolock", ms_fgetc), EX("_fputc_nolock", ms_fputc),
    EX("_getc_nolock", ms_fgetc), EX("_putc_nolock", ms_fputc), EX("_ungetc_nolock", ms_ungetc), EX("_getchar_nolock", ms_getchar), EX("_putchar_nolock", ms_putchar),
    EM(fgetwc), EX("getwc", ms_fgetwc), EM(fputwc), EX("putwc", ms_fputwc), EM(ungetwc), EM(getwchar), EM(putwchar),
    EX("_fgetwchar", ms_getwchar), EX("_fputwchar", ms_putwchar), EX("_fgetwc_nolock", ms_fgetwc), EX("_fputwc_nolock", ms_fputwc),
    EX("_getwc_nolock", ms_fgetwc), EX("_putwc_nolock", ms_fputwc), EX("_ungetwc_nolock", ms_ungetwc),
    E(_filbuf), E(_flsbuf), E(_filwbuf), E(_flswbuf),
    EM(fread), E(_fread_nolock), E(fread_s), E(_fread_nolock_s), EM(fwrite), E(_fwrite_nolock),
    EM(fgets), EM(fgetws), EM(fputs), EM(fputws), EM(puts), E(_putws), EM(gets), E(gets_s), E(_getws), E(_getws_s),
    EM(fseek), E(_fseeki64), EM(ftell), E(_ftelli64), EM(rewind), EM(fgetpos), EM(fsetpos),
    E(_fseek_nolock), E(_fseeki64_nolock), E(_ftell_nolock), E(_ftelli64_nolock),
    EM(feof), EM(ferror), EM(clearerr), E(clearerr_s), E(_fileno), EX("fileno", _fileno), EM(setvbuf), EM(setbuf),
    E(_lock_file), E(_unlock_file), E(_getmaxstdio), E(_setmaxstdio), E(__iob_func), E(__acrt_iob_func), E(__p__iob),
    E(_get_stream_buffer_pointers), E(_set_printf_count_output), E(_get_printf_count_output), E(_set_output_format), E(_get_output_format),
    EM(tmpfile), E(tmpfile_s), EM(tmpnam), E(tmpnam_s), E(_wtmpnam), E(_wtmpnam_s), E(_tempnam), E(_wtempnam), EX("tempnam", _tempnam),
    EM(perror), E(_wperror), E(_popen), E(_wpopen), E(_pclose), EX("popen", _popen), EX("pclose", _pclose),
    /* printf */
    EM(vfprintf), EM(vprintf), EM(fprintf), EM(printf), E(_vfprintf_l), E(_vprintf_l), E(_fprintf_l), E(_printf_l),
    EM(vfwprintf), EM(vwprintf), EM(fwprintf), EM(wprintf), E(_vfwprintf_l), E(_fwprintf_l), E(_wprintf_l),
    EM(vsprintf), EM(sprintf), E(_vsnprintf), E(_snprintf), EM(vsnprintf), EM(snprintf), E(vsprintf_s), E(sprintf_s),
    E(_vsnprintf_s), E(_snprintf_s), E(_vscprintf), E(_scprintf), E(_vsnprintf_l), E(_vsprintf_l), E(_sprintf_l), E(_snprintf_l), E(_vscprintf_l),
    E(_vsnwprintf), E(_snwprintf), E(vswprintf_s), E(swprintf_s), E(_vsnwprintf_s), E(_snwprintf_s), E(_vscwprintf), E(_scwprintf),
    E(_vsnwprintf_l), E(_vswprintf), E(_swprintf), E(_vswprintf_c), E(_swprintf_c),
    E(printf_s), E(fprintf_s), E(vprintf_s), E(vfprintf_s), E(wprintf_s), E(fwprintf_s), E(vwprintf_s), E(vfwprintf_s),
    E(__stdio_common_vfprintf), E(__stdio_common_vfprintf_s), E(__stdio_common_vfprintf_p),
    E(__stdio_common_vfwprintf), E(__stdio_common_vfwprintf_s), E(__stdio_common_vfwprintf_p),
    E(__stdio_common_vsprintf), E(__stdio_common_vsprintf_s), E(__stdio_common_vsnprintf_s), E(__stdio_common_vsprintf_p),
    E(__stdio_common_vswprintf), E(__stdio_common_vswprintf_s), E(__stdio_common_vsnwprintf_s), E(__stdio_common_vswprintf_p),
    /* scanf */
    EM(vfscanf), EM(vscanf), EM(vsscanf), EM(fscanf), EM(scanf), EM(sscanf), E(fscanf_s), E(scanf_s), E(sscanf_s),
    E(vfscanf_s), E(vscanf_s), E(vsscanf_s), E(_snscanf), E(_snscanf_s), E(_sscanf_l),
    EM(vfwscanf), EM(vwscanf), EM(vswscanf), EM(fwscanf), EM(wscanf), EM(swscanf), E(fwscanf_s), E(wscanf_s), E(swscanf_s),
    E(_snwscanf), E(_snwscanf_s),
    E(__stdio_common_vfscanf), E(__stdio_common_vsscanf), E(__stdio_common_vfwscanf), E(__stdio_common_vswscanf),
    /* conio */
    E(_getch), E(_getche), E(_getwch), E(_getwche), E(_getch_nolock), E(_getche_nolock), E(_ungetch), E(_ungetwch), E(_kbhit),
    E(_putch), E(_putch_nolock), E(_putwch), E(_cputs), E(_cputws), E(_cprintf), E(_cwprintf), E(_vcprintf), E(_cgets),
    EX("getch", _getch), EX("getche", _getche), EX("kbhit", _kbhit), EX("putch", _putch), EX("ungetch", _ungetch), EX("cputs", _cputs), EX("cprintf", _cprintf),
    /* data */
    EX("_iob", crt_iob),
    { NULL, NULL }
};

/* The two flavours of swprintf/vswprintf, referenced from the msvcrt/ucrt tables. */
/* swprintf differs between runtimes: msvcrt.dll has the old form without a
 * count, ucrtbase the ISO C form. crt_tables.c picks one per DLL. */
WINAPI int crt_vswprintf_msvcrt(WCHAR *b, const WCHAR *f, ms_va_list ap) { return msvcrt_vswprintf(b, f, ap); }
WINAPI int crt_swprintf_msvcrt(WCHAR *b, const WCHAR *f, ...) { ms_va_list ap; ms_va_start(ap, f); int r = msvcrt_vswprintf(b, f, ap); ms_va_end(ap); return r; }
WINAPI int crt_vswprintf_iso(WCHAR *b, size_t n, const WCHAR *f, ms_va_list ap) { return iso_vswprintf(b, n, f, ap); }
WINAPI int crt_swprintf_iso(WCHAR *b, size_t n, const WCHAR *f, ...) { ms_va_list ap; ms_va_start(ap, f); int r = iso_vswprintf(b, n, f, ap); ms_va_end(ap); return r; }
