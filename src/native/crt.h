/* Internal definitions shared by LSIW's C runtime (msvcrt.dll, ucrtbase.dll
 * and the api-ms-win-crt-* families).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_CRT_H
#define LSIW_CRT_H

#include "native.h"
#include <pthread.h>

/* Microsoft errno values (1..34 coincide with Linux). */
#define MS_EPERM   1
#define MS_ENOENT  2
#define MS_ESRCH   3
#define MS_EINTR   4
#define MS_EIO     5
#define MS_ENXIO   6
#define MS_E2BIG   7
#define MS_ENOEXEC 8
#define MS_EBADF   9
#define MS_ECHILD  10
#define MS_EAGAIN  11
#define MS_ENOMEM  12
#define MS_EACCES  13
#define MS_EFAULT  14
#define MS_EBUSY   16
#define MS_EEXIST  17
#define MS_EXDEV   18
#define MS_ENODEV  19
#define MS_ENOTDIR 20
#define MS_EISDIR  21
#define MS_EINVAL  22
#define MS_ENFILE  23
#define MS_EMFILE  24
#define MS_ENOTTY  25
#define MS_EFBIG   27
#define MS_ENOSPC  28
#define MS_ESPIPE  29
#define MS_EROFS   30
#define MS_EMLINK  31
#define MS_EPIPE   32
#define MS_EDOM    33
#define MS_ERANGE  34
#define MS_EDEADLK 36
#define MS_ENAMETOOLONG 38
#define MS_ENOLCK  39
#define MS_ENOSYS  40
#define MS_ENOTEMPTY 41
#define MS_EILSEQ  42
#define MS_STRUNCATE 80

int  *crt_errno(void);
void  crt_set_errno(int ms_err);
int   crt_errno_from_host(int host_err);
void  crt_set_errno_host(int host_err);     /* also sets _doserrno */
void  crt_invalid_parameter(void);          /* calls the handler; may not return */

/* Low-level I/O (crt_stdio.c). */
#define O_MS_RDONLY  0x0000
#define O_MS_WRONLY  0x0001
#define O_MS_RDWR    0x0002
#define O_MS_APPEND  0x0008
#define O_MS_CREAT   0x0100
#define O_MS_TRUNC   0x0200
#define O_MS_EXCL    0x0400
#define O_MS_TEXT    0x4000
#define O_MS_BINARY  0x8000
#define O_MS_WTEXT   0x10000
#define O_MS_U16TEXT 0x20000
#define O_MS_U8TEXT  0x40000
#define O_MS_NOINHERIT 0x0080
#define O_MS_TEMPORARY 0x0040
#define O_MS_SHORT_LIVED 0x1000
#define O_MS_SEQUENTIAL 0x0020
#define O_MS_RANDOM  0x0010

extern int crt_fmode;      /* _fmode   */
extern int crt_commode;    /* _commode */

int  crt_fd_from_handle(HANDLE h, int flags);
HANDLE crt_fd_handle(int fd);
int  crt_fd_host(int fd);
int  crt_fd_mode(int fd);                 /* O_MS_TEXT / BINARY / WTEXT / U16TEXT / U8TEXT */
char *crt_cmd_strip_quotes(const char *cmd);  /* cmd.exe /c outer-quote rule (crt_core.c) */
int  crt_read_fd(int fd, void *buf, unsigned n);
int  crt_write_fd(int fd, const void *buf, unsigned n);

/* Streams. */
typedef struct ms_file {
    char *_ptr;
    int   _cnt;
    char *_base;
    int   _flag;
    int   _file;
    int   _charbuf;
    int   _bufsiz;
    char *_tmpfname;
} MSFILE;

#define MS_IOREAD  0x0001
#define MS_IOWRT   0x0002
#define MS_IOMYBUF 0x0008
#define MS_IOEOF   0x0010
#define MS_IOERR   0x0020
#define MS_IOSTRG  0x0040
#define MS_IORW    0x0080

#define MS_IOFBF 0x0000
#define MS_IOLBF 0x0040
#define MS_IONBF 0x0004

extern MSFILE crt_iob[];                 /* _iob[]: stdin, stdout, stderr, ... */
MSFILE *crt_stdin(void);
MSFILE *crt_stdout(void);
MSFILE *crt_stderr(void);
int  crt_fputc(int c, MSFILE *f);
int  crt_fwrite_bytes(const void *p, size_t n, MSFILE *f);
int  crt_fputws_utf8(const char *u, MSFILE *f);  /* write converted wide text */
int  crt_fgetc(MSFILE *f);
int  crt_ungetc(int c, MSFILE *f);
int  crt_fflush(MSFILE *f);
void crt_lock_stream(MSFILE *f);
void crt_unlock_stream(MSFILE *f);
int  crt_stream_is_wide_mode(MSFILE *f);

/* Formatted output helpers. */
char *crt_vformat(const char *fmt, ms_va_list ap, int wide_default, int *len);
char *crt_vformat_w(const WCHAR *fmt, ms_va_list ap, int wide_default, int *len);

/* Environment (crt_core.c). */
void crt_env_changed(void);

/* Time (crt_time.c): Microsoft struct tm is 9 ints. */
struct ms_tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };

/* Wide helpers (crt_string.c). */
size_t crt_wcslen(const WCHAR *s);
WCHAR  crt_towlower(WCHAR c);
WCHAR  crt_towupper(WCHAR c);

/* Locale (crt_time.c). */
extern int crt_codepage;    /* 0 in the "C" locale, 65001 after setlocale(LC_ALL, "") */
extern int crt_mb_cur_max;

/* Functions whose signature differs between msvcrt.dll and ucrtbase.dll
 * (crt_stdio.c, crt_string.c); crt_tables.c exports the right one. */
WINAPI int crt_swprintf_msvcrt(WCHAR *b, const WCHAR *f, ...);
WINAPI int crt_vswprintf_msvcrt(WCHAR *b, const WCHAR *f, ms_va_list ap);
WINAPI int crt_swprintf_iso(WCHAR *b, size_t n, const WCHAR *f, ...);
WINAPI int crt_vswprintf_iso(WCHAR *b, size_t n, const WCHAR *f, ms_va_list ap);
WINAPI WCHAR *crt_wcstok_msvcrt(WCHAR *s, const WCHAR *d);
WINAPI WCHAR *crt_wcstok_ucrt(WCHAR *s, const WCHAR *d, WCHAR **ctx);

/* CRT functions whose names clash with the host C library are defined as
 * ms_<name> and exported under their Windows name with EM(). */
#define EM(n) { #n, (void *)ms_##n }

/* An ms_abi function pointer for qsort and friends. */
typedef int (WINAPI *ms_cmp_fn)(const void *, const void *);
typedef int (WINAPI *ms_cmp_s_fn)(void *, const void *, const void *);
typedef void (WINAPI *ms_vfn)(void);
typedef int  (WINAPI *ms_ifn)(void);

#endif
