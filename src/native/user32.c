/* LSIW - user32 for console programs.
 *
 * The parts of the Windows user-interface library that console programs
 * import without ever creating a window: message boxes, wsprintf and the
 * character helpers, string resources, per-thread message queues and timers,
 * the US keyboard layout, system metrics and parameters, display and monitor
 * information, DPI awareness, the clipboard (bridged to the Linux clipboard)
 * and window-station / desktop queries.
 *
 * Window classes and window creation (RegisterClass*, CreateWindow*,
 * DefWindowProc*, dialogs, painting) are deliberately NOT exported: their
 * absence is how 'lsiw check' recognises GUI programs and sends them to the
 * Wine engine. Every HWND a program can hold here is therefore invalid,
 * except the pseudo desktop window, and window functions fail the way
 * Windows fails for a destroyed window (ERROR_INVALID_WINDOW_HANDLE).
 *
 * The ANSI and OEM code pages are UTF-8 throughout LSIW.
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
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ================================================================== */
/* Types and constants                                                  */
/* ================================================================== */

typedef uint64_t UINT_PTR;
typedef uint64_t WPARAM;
typedef int64_t  LPARAM;
typedef int64_t  LRESULT;
typedef int16_t  SHORT;
typedef void *HWND, *HMENU, *HICON, *HCURSOR, *HBRUSH, *HMONITOR, *HDC, *HHOOK, *HKL,
             *HDESK, *HWINSTA, *HINSTANCE;

typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
_Static_assert(sizeof(MSG) == 48, "MSG layout");

typedef void    (WINAPI *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef BOOL    (WINAPI *MONITORENUMPROC)(HMONITOR, HDC, RECT *, LPARAM);

typedef struct {
    UINT cbSize; HWND hwndOwner; HINSTANCE hInstance; const char *lpszText; const char *lpszCaption;
    DWORD dwStyle; const char *lpszIcon; ULONG_PTR dwContextHelpId; void *lpfnMsgBoxCallback; DWORD dwLanguageId;
} MSGBOXPARAMSA;
typedef struct {
    UINT cbSize; HWND hwndOwner; HINSTANCE hInstance; const WCHAR *lpszText; const WCHAR *lpszCaption;
    DWORD dwStyle; const WCHAR *lpszIcon; ULONG_PTR dwContextHelpId; void *lpfnMsgBoxCallback; DWORD dwLanguageId;
} MSGBOXPARAMSW;
_Static_assert(sizeof(MSGBOXPARAMSW) == 80, "MSGBOXPARAMS layout");

typedef struct {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char lfFaceName[32];
} LOGFONTA;
typedef struct {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONTW;
_Static_assert(sizeof(LOGFONTA) == 60 && sizeof(LOGFONTW) == 92, "LOGFONT layout");

typedef struct {
    UINT cbSize; int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight;
    LOGFONTA lfCaptionFont; int iSmCaptionWidth, iSmCaptionHeight; LOGFONTA lfSmCaptionFont;
    int iMenuWidth, iMenuHeight; LOGFONTA lfMenuFont, lfStatusFont, lfMessageFont; int iPaddedBorderWidth;
} NONCLIENTMETRICSA;
typedef struct {
    UINT cbSize; int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight;
    LOGFONTW lfCaptionFont; int iSmCaptionWidth, iSmCaptionHeight; LOGFONTW lfSmCaptionFont;
    int iMenuWidth, iMenuHeight; LOGFONTW lfMenuFont, lfStatusFont, lfMessageFont; int iPaddedBorderWidth;
} NONCLIENTMETRICSW;
_Static_assert(sizeof(NONCLIENTMETRICSA) == 344 && sizeof(NONCLIENTMETRICSW) == 504, "NONCLIENTMETRICS layout");

typedef struct { UINT cbSize; int iHorzSpacing, iVertSpacing, iTitleWrap; LOGFONTA lfFont; } ICONMETRICSA;
typedef struct { UINT cbSize; int iHorzSpacing, iVertSpacing, iTitleWrap; LOGFONTW lfFont; } ICONMETRICSW;
typedef struct { UINT cbSize; int iWidth, iHorzGap, iVertGap, iArrange; } MINIMIZEDMETRICS;
typedef struct { UINT cbSize; DWORD dwFlags; void *lpszDefaultScheme; } HIGHCONTRAST;
typedef struct { UINT cbSize; int iMinAnimate; } ANIMATIONINFO;
typedef struct { UINT cbSize; BOOL Enabled; DWORD Locale; } AUDIODESCRIPTION;
typedef struct { UINT cbSize; DWORD dwFlags; } STICKYKEYS, TOGGLEKEYS;
typedef struct { UINT cbSize; DWORD dwFlags, iWaitMSec, iDelayMSec, iRepeatMSec, iBounceMSec; } FILTERKEYS;
typedef struct { UINT cbSize; DWORD dwFlags, iMaxSpeed, iTimeToMaxSpeed, iCtrlSpeed, dwReserved1, dwReserved2; } MOUSEKEYS;
typedef struct { UINT cbSize; DWORD dwFlags, iTimeOutMSec; } ACCESSTIMEOUT;

/* DEVMODE: the display variant of the printer/display union is used. */
typedef struct {
    char  dmDeviceName[32];
    WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    LONG  dmPositionX, dmPositionY;
    DWORD dmDisplayOrientation, dmDisplayFixedOutput;
    int16_t dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    char  dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODEA;
typedef struct {
    WCHAR dmDeviceName[32];
    WORD  dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    DWORD dmFields;
    LONG  dmPositionX, dmPositionY;
    DWORD dmDisplayOrientation, dmDisplayFixedOutput;
    int16_t dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    WCHAR dmFormName[32];
    WORD  dmLogPixels;
    DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODEW;
_Static_assert(sizeof(DEVMODEA) == 156 && sizeof(DEVMODEW) == 220, "DEVMODE layout");
_Static_assert(offsetof(DEVMODEW, dmBitsPerPel) == 168 && offsetof(DEVMODEA, dmBitsPerPel) == 104, "DEVMODE layout");

typedef struct { DWORD cb; char DeviceName[32]; char DeviceString[128]; DWORD StateFlags; char DeviceID[128]; char DeviceKey[128]; } DISPLAY_DEVICEA;
typedef struct { DWORD cb; WCHAR DeviceName[32]; WCHAR DeviceString[128]; DWORD StateFlags; WCHAR DeviceID[128]; WCHAR DeviceKey[128]; } DISPLAY_DEVICEW;
_Static_assert(sizeof(DISPLAY_DEVICEA) == 424 && sizeof(DISPLAY_DEVICEW) == 840, "DISPLAY_DEVICE layout");

typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { MONITORINFO mi; char szDevice[32]; } MONITORINFOEXA;
typedef struct { MONITORINFO mi; WCHAR szDevice[32]; } MONITORINFOEXW;
_Static_assert(sizeof(MONITORINFO) == 40 && sizeof(MONITORINFOEXA) == 72 && sizeof(MONITORINFOEXW) == 104, "MONITORINFO layout");

typedef struct { UINT cbSize; DWORD dwTime; } LASTINPUTINFO;
typedef struct { BOOL fInherit, fReserved; DWORD dwFlags; } USEROBJECTFLAGS;
typedef struct { HANDLE hDevice; DWORD dwType; } RAWINPUTDEVICELIST;

/* Error codes beyond native.h */
#define ERROR_INVALID_FLAGS_          1004
#define ERROR_NOACCESS_               998
#define ERROR_PRIVILEGE_NOT_HELD_     1314
#define ERROR_INVALID_WINDOW_HANDLE   1400
#define ERROR_INVALID_CURSOR_HANDLE   1402
#define ERROR_INVALID_HOOK_HANDLE     1404
#define ERROR_CLIPBOARD_NOT_OPEN      1418
#define ERROR_INVALID_MSGBOX_STYLE    1438
#define ERROR_INVALID_SPI_VALUE       1439
#define ERROR_INVALID_THREAD_ID       1444
#define ERROR_INVALID_MONITOR_HANDLE  1461
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814
#define ERROR_NOT_ENOUGH_QUOTA        1816

/* Messages */
#define WM_QUIT        0x0012
#define WM_KEYDOWN     0x0100
#define WM_KEYUP       0x0101
#define WM_CHAR        0x0102
#define WM_SYSKEYDOWN  0x0104
#define WM_SYSKEYUP    0x0105
#define WM_SYSCHAR     0x0106
#define WM_TIMER       0x0113
#define PM_REMOVE      0x0001
#define QS_KEY         0x0001
#define QS_POSTMESSAGE 0x0008
#define QS_TIMER       0x0010
#define QS_SENDMESSAGE 0x0040
#define QS_ALLPOSTMESSAGE 0x0100
#define QS_VALID       0x1DFF
#define MWMO_WAITALL   0x0001
#define MWMO_ALERTABLE 0x0002
#define MWMO_INPUTAVAILABLE 0x0004
#define MAX_POSTED     10000   /* Windows' default per-queue posted-message quota */

#define HWND_BROADCAST ((HWND)(uintptr_t)0xFFFF)
#define IS_INTRES(p)   ((((uintptr_t)(p)) >> 16) == 0)

/* Stand-in handle values. None of them is a kernel handle (those are small
 * multiples of 4 from the handle table), so they can never be confused. */
#define DESKTOP_HWND   ((HWND)(uintptr_t)0x00010010)   /* GetDesktopWindow's usual value */
#define PRIMARY_MON    ((HMONITOR)(uintptr_t)0x00010001)
#define TOK_ICON_SYS   0x7F010000u
#define TOK_CURSOR_SYS 0x7F020000u
#define TOK_IMAGE      0x7F030000u
#define TOK_BRUSH      0x7F040000u
#define TOK_WINSTA     ((HANDLE)(uintptr_t)0x7F050004)
#define TOK_DESKTOP    ((HANDLE)(uintptr_t)0x7F050008)
#define TOK_NOTIFY     0x7F060000u
#define US_HKL         ((HKL)(uintptr_t)0x04090409)

/* ================================================================== */
/* Small helpers                                                        */
/* ================================================================== */

static void *k32(const char *name)
{
    static const struct lsiw_builtin_dll *dll;
    if (!dll) dll = lsiw_builtin_find("kernel32.dll");
    return dll ? lsiw_builtin_lookup(dll, name) : NULL;
}

/* Same clock as kernel32's GetTickCount (CLOCK_MONOTONIC, milliseconds). */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static DWORD cur_tid(void) { return (DWORD)gettid(); }

static int set_err(DWORD e) { lsiw_set_last_error(e); return 0; }

/* Copy UTF-8 into an ANSI buffer of `max` bytes, cutting at a character
 * boundary. Returns the bytes stored, without the NUL. */
static int put_a(const char *s, char *buf, int max)
{
    if (!buf || max <= 0) return 0;
    size_t n = strlen(s);
    if (n > (size_t)max - 1) {
        n = (size_t)max - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(buf, s, n);
    buf[n] = 0;
    return (int)n;
}

/* Copy UTF-16 into a buffer of `max` WCHARs, never splitting a surrogate pair. */
static int put_ww(const WCHAR *s, size_t len, WCHAR *buf, int max)
{
    if (!buf || max <= 0) return 0;
    size_t n = len;
    if (n > (size_t)max - 1) {
        n = (size_t)max - 1;
        if (n > 0 && s[n - 1] >= 0xD800 && s[n - 1] <= 0xDBFF) n--;
    }
    memcpy(buf, s, n * sizeof(WCHAR));
    buf[n] = 0;
    return (int)n;
}

static int put_w(const char *s, WCHAR *buf, int max)
{
    if (!buf || max <= 0) return 0;
    WCHAR *w = lsiw_u2w(s);
    if (!w) { buf[0] = 0; return 0; }
    int n = put_ww(w, lsiw_wcslen(w), buf, max);
    free(w);
    return n;
}

static int env_true(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* A desktop session we may open dialogs in / talk to the clipboard of. */
static int gui_session(void)
{
    if (env_true("LSIW_NO_GUI")) return 0;
    const char *w = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    return (w && *w) || (x && *x);
}

static int have_tool(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[4096];
    for (const char *p = path;;) {
        const char *e = strchr(p, ':');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        size_t nl = strlen(name);
        if (l && l + nl + 2 < sizeof buf) {
            memcpy(buf, p, l);
            buf[l] = '/';
            memcpy(buf + l + 1, name, nl + 1);
            if (access(buf, X_OK) == 0) return 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Wait until fd is ready; 0 when the deadline (0 = none) passes first. */
static int wait_fd(int fd, short events, uint64_t deadline)
{
    for (;;) {
        int tmo = -1;
        if (deadline) {
            uint64_t now = now_ms();
            if (now >= deadline) return 0;
            tmo = (int)(deadline - now > 0x7FFFFFFF ? 0x7FFFFFFF : deadline - now);
        }
        struct pollfd p = { fd, events, 0 };
        int r = poll(&p, 1, tmo);
        if (r > 0) return 1;
        if (r == 0) return 0;
        if (errno != EINTR) return 1;     /* let read/write report the error */
    }
}

/* Run a Linux helper program (zenity, wl-copy, xclip...). `in` (may be NULL)
 * is fed to its stdin; with `out` its stdout is captured (malloc'd, NUL
 * terminated). stderr goes to /dev/null so GTK chatter never reaches the
 * Windows program's console. timeout_ms 0 = no limit. Returns the exit
 * status, -1 if it could not be started, -2 if it was killed on timeout. */
static int run_tool(char *const argv[], const char *in, size_t inlen, char **out, size_t *outlen, int timeout_ms)
{
    int ip[2] = { -1, -1 }, op[2] = { -1, -1 };
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    if (in && pipe2(ip, O_CLOEXEC) < 0) return -1;
    if (out && pipe2(op, O_CLOEXEC) < 0) {
        if (in) { close(ip[0]); close(ip[1]); }
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in) posix_spawn_file_actions_adddup2(&fa, ip[0], 0);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out) posix_spawn_file_actions_adddup2(&fa, op[1], 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t mask, def;
    sigemptyset(&mask);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);                 /* the engine ignores SIGPIPE; the tool must not */
    posix_spawnattr_setsigmask(&at, &mask);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (in) close(ip[0]);
    if (out) close(op[1]);
    if (err) {
        if (in) close(ip[1]);
        if (out) close(op[0]);
        return -1;
    }
    uint64_t deadline = timeout_ms > 0 ? now_ms() + (uint64_t)timeout_ms : 0;
    int timed_out = 0;
    if (in) {
        size_t off = 0;
        while (off < inlen) {
            if (!wait_fd(ip[1], POLLOUT, deadline)) { timed_out = 1; break; }
            ssize_t w = write(ip[1], in + off, inlen - off);
            if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
            off += (size_t)w;
        }
        close(ip[1]);
    }
    if (out) {
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        while (buf && !timed_out) {
            if (!wait_fd(op[0], POLLIN, deadline)) { timed_out = 1; break; }
            if (len + 1 >= cap) {
                if (cap >= ((size_t)64 << 20)) break;     /* 64 MB is plenty for text */
                char *nb = realloc(buf, cap * 2);
                if (!nb) break;
                buf = nb;
                cap *= 2;
            }
            ssize_t r = read(op[0], buf + len, cap - len - 1);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;
            len += (size_t)r;
        }
        close(op[0]);
        if (buf) buf[len] = 0;
        *out = buf;
        if (outlen) *outlen = buf ? len : 0;
    }
    int st = 0;
    for (;;) {
        if (timed_out) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            return -2;
        }
        pid_t r = waitpid(pid, &st, deadline ? WNOHANG : 0);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (r == 0) {
            if (now_ms() >= deadline) { timed_out = 1; continue; }
            struct timespec ts = { 0, 10 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ================================================================== */
/* Character case and classification                                    */
/* ================================================================== */

/* Ranges of alternating upper/lower pairs: [first, last], upper on even
 * code points (even = 1) or on odd ones (even = 0). */
static const struct { WCHAR first, last; BYTE even; } g_pairs[] = {
    { 0x0100, 0x012F, 1 }, { 0x0132, 0x0137, 1 }, { 0x0139, 0x0148, 0 }, { 0x014A, 0x0177, 1 },
    { 0x0179, 0x017E, 0 }, { 0x01A0, 0x01A5, 1 }, { 0x01CD, 0x01DC, 0 }, { 0x01DE, 0x01EF, 1 },
    { 0x01F8, 0x021F, 1 }, { 0x0222, 0x0233, 1 }, { 0x03D8, 0x03EF, 1 }, { 0x0460, 0x0481, 1 },
    { 0x048A, 0x04BF, 1 }, { 0x04C1, 0x04CE, 0 }, { 0x04D0, 0x052F, 1 }, { 0x1E00, 0x1E95, 1 },
    { 0x1EA0, 0x1EFF, 1 },
};
/* Ranges where lower = upper + delta. */
static const struct { WCHAR ufirst, ulast; WORD delta; } g_shift[] = {
    { 'A', 'Z', 32 }, { 0x00C0, 0x00D6, 32 }, { 0x00D8, 0x00DE, 32 }, { 0x0391, 0x03A1, 32 },
    { 0x03A3, 0x03AB, 32 }, { 0x0388, 0x038A, 37 }, { 0x038E, 0x038F, 63 }, { 0x0410, 0x042F, 32 },
    { 0x0400, 0x040F, 80 }, { 0x0531, 0x0556, 48 }, { 0x2160, 0x216F, 16 }, { 0x24B6, 0x24CF, 26 },
    { 0xFF21, 0xFF3A, 32 },
};

static WCHAR wupper(WCHAR c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
    for (size_t i = 0; i < sizeof g_shift / sizeof g_shift[0]; i++)
        if (c >= g_shift[i].ufirst + g_shift[i].delta && c <= g_shift[i].ulast + g_shift[i].delta)
            return (WCHAR)(c - g_shift[i].delta);
    for (size_t i = 0; i < sizeof g_pairs / sizeof g_pairs[0]; i++)
        if (c >= g_pairs[i].first && c <= g_pairs[i].last)
            return (((c & 1) != 0) == g_pairs[i].even) ? (WCHAR)(c - 1) : c;
    switch (c) {
    case 0x00FF: return 0x0178;
    case 0x0131: return 'I';
    case 0x017F: return 'S';
    case 0x03AC: return 0x0386;
    case 0x03CC: return 0x038C;
    case 0x03C2: return 0x03A3;      /* final sigma */
    case 0x04CF: return 0x04C0;
    }
    return c;
}

static WCHAR wlower(WCHAR c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
    for (size_t i = 0; i < sizeof g_shift / sizeof g_shift[0]; i++)
        if (c >= g_shift[i].ufirst && c <= g_shift[i].ulast) return (WCHAR)(c + g_shift[i].delta);
    for (size_t i = 0; i < sizeof g_pairs / sizeof g_pairs[0]; i++)
        if (c >= g_pairs[i].first && c <= g_pairs[i].last)
            return (((c & 1) != 0) == g_pairs[i].even) ? c : (WCHAR)(c + 1);
    switch (c) {
    case 0x0178: return 0x00FF;
    case 0x0130: return 'i';
    case 0x0386: return 0x03AC;
    case 0x038C: return 0x03CC;
    case 0x04C0: return 0x04CF;
    }
    return c;
}

static int w_isalpha(WCHAR c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (wupper(c) != c || wlower(c) != c) return 1;
    if (c == 0xAA || c == 0xB5 || c == 0xBA || c == 0xDF) return 1;
    if (c >= 0xC0 && c <= 0x24F) return c != 0xD7 && c != 0xF7;
    if (c >= 0x250 && c <= 0x2AF) return 1;                                   /* IPA */
    if (c >= 0x370 && c <= 0x3FF) return c != 0x375 && c != 0x37E && c != 0x384 && c != 0x385 && c != 0x387 && c != 0x3F6;
    if (c >= 0x400 && c <= 0x52F) return !(c >= 0x482 && c <= 0x489);
    if (c >= 0x531 && c <= 0x587) return !(c >= 0x557 && c <= 0x560);
    if ((c >= 0x5D0 && c <= 0x5EA) || (c >= 0x620 && c <= 0x64A) || (c >= 0x671 && c <= 0x6D3)) return 1;
    if ((c >= 0x904 && c <= 0x939) || (c >= 0xE01 && c <= 0xE30) || (c >= 0x10A0 && c <= 0x10FF)) return 1;
    if (c >= 0x1E00 && c <= 0x1FFF) return 1;
    if ((c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA)) return 1;  /* kana */
    if ((c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF)) return 1;
    if (c >= 0xAC00 && c <= 0xD7A3) return 1;                                 /* Hangul */
    if (c >= 0xFF66 && c <= 0xFF9D) return 1;
    return 0;
}

static int w_isdigit(WCHAR c)
{
    return (c >= '0' && c <= '9') || c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0x660 && c <= 0x669) ||
           (c >= 0x6F0 && c <= 0x6F9) || (c >= 0x966 && c <= 0x96F) || (c >= 0xFF10 && c <= 0xFF19);
}
static int w_isupper(WCHAR c) { return wlower(c) != c; }
static int w_islower(WCHAR c) { return wupper(c) != c || c == 0xDF || c == 0xB5 || c == 0x138 || c == 0x149; }

static int utf8_seqlen(unsigned char c) { return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1; }

/* Change case in place inside a UTF-8 buffer: n bytes, or up to the NUL when
 * n is SIZE_MAX. A character is only replaced when the new one has the same
 * encoded length, so the buffer never grows. */
static void utf8_case(char *s, size_t n, int upper)
{
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n && (n != SIZE_MAX || p[i]);) {
        unsigned c = p[i];
        if (c < 0x80) {
            if (upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) p[i] = (unsigned char)(c ^ 0x20);
            i++;
            continue;
        }
        int len = utf8_seqlen((unsigned char)c);
        if (len < 2 || len > 3 || (n != SIZE_MAX && i + (size_t)len > n)) { i++; continue; }
        int ok = 1;
        for (int k = 1; k < len; k++) if ((p[i + k] & 0xC0) != 0x80) ok = 0;
        if (!ok) { i++; continue; }
        unsigned cp = len == 2 ? ((c & 0x1F) << 6) | (p[i + 1] & 0x3F)
                               : ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
        unsigned m = upper ? wupper((WCHAR)cp) : wlower((WCHAR)cp);
        int mlen = m < 0x80 ? 1 : m < 0x800 ? 2 : 3;
        if (m != cp && mlen == len) {
            if (len == 2) { p[i] = (unsigned char)(0xC0 | (m >> 6)); p[i + 1] = (unsigned char)(0x80 | (m & 0x3F)); }
            else { p[i] = (unsigned char)(0xE0 | (m >> 12)); p[i + 1] = (unsigned char)(0x80 | ((m >> 6) & 0x3F)); p[i + 2] = (unsigned char)(0x80 | (m & 0x3F)); }
        }
        i += (size_t)len;
    }
}

/* CharUpper/CharLower take either a string or, when the high word of the
 * pointer is zero, a single character in the low word. */
API char *CharUpperA(char *s)
{
    if (IS_INTRES(s)) { unsigned c = (unsigned)(uintptr_t)s & 0xFF; return (char *)(uintptr_t)((c >= 'a' && c <= 'z') ? c - 32 : c); }
    utf8_case(s, SIZE_MAX, 1);
    return s;
}
API char *CharLowerA(char *s)
{
    if (IS_INTRES(s)) { unsigned c = (unsigned)(uintptr_t)s & 0xFF; return (char *)(uintptr_t)((c >= 'A' && c <= 'Z') ? c + 32 : c); }
    utf8_case(s, SIZE_MAX, 0);
    return s;
}
API WCHAR *CharUpperW(WCHAR *s)
{
    if (IS_INTRES(s)) return (WCHAR *)(uintptr_t)wupper((WCHAR)(uintptr_t)s);
    for (WCHAR *p = s; *p; p++) *p = wupper(*p);
    return s;
}
API WCHAR *CharLowerW(WCHAR *s)
{
    if (IS_INTRES(s)) return (WCHAR *)(uintptr_t)wlower((WCHAR)(uintptr_t)s);
    for (WCHAR *p = s; *p; p++) *p = wlower(*p);
    return s;
}
API DWORD CharUpperBuffA(char *s, DWORD n) { if (!s) return 0; utf8_case(s, n, 1); return n; }
API DWORD CharLowerBuffA(char *s, DWORD n) { if (!s) return 0; utf8_case(s, n, 0); return n; }
API DWORD CharUpperBuffW(WCHAR *s, DWORD n) { if (!s) return 0; for (DWORD i = 0; i < n; i++) s[i] = wupper(s[i]); return n; }
API DWORD CharLowerBuffW(WCHAR *s, DWORD n) { if (!s) return 0; for (DWORD i = 0; i < n; i++) s[i] = wlower(s[i]); return n; }

/* Character stepping. Code page 0/1/3 (ACP/OEMCP/THREAD_ACP) and 65001 are
 * UTF-8 here; the East Asian DBCS code pages use their lead-byte ranges;
 * everything else is single-byte. */
static int dbcs_lead(UINT cp, unsigned char b)
{
    switch (cp) {
    case 932:  return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
    case 936: case 949: case 950: return b >= 0x81 && b <= 0xFE;
    case 1361: return (b >= 0x84 && b <= 0xD3) || (b >= 0xD8 && b <= 0xDE) || (b >= 0xE0 && b <= 0xF9);
    }
    return 0;
}
static const char *next_char_cp(UINT cp, const char *p)
{
    if (!*p) return p;
    if (cp == 0 || cp == 1 || cp == 3 || cp == 65001) {
        int len = utf8_seqlen((unsigned char)*p);
        int k = 1;
        while (k < len && ((unsigned char)p[k] & 0xC0) == 0x80) k++;
        return p + k;
    }
    if (dbcs_lead(cp, (unsigned char)*p) && p[1]) return p + 2;
    return p + 1;
}
static const char *prev_char_cp(UINT cp, const char *start, const char *cur)
{
    if (!start || !cur || cur <= start) return start;
    const char *p = start, *prev = start;
    while (p < cur) {
        prev = p;
        const char *n = next_char_cp(cp, p);
        if (n == p) break;
        p = n;
    }
    return prev;
}
API char *CharNextA(const char *p) { return p ? (char *)next_char_cp(65001, p) : NULL; }
API char *CharPrevA(const char *start, const char *cur) { return (char *)prev_char_cp(65001, start, cur); }
API char *CharNextExA(WORD cp, const char *p, DWORD flags) { (void)flags; return p ? (char *)next_char_cp(cp, p) : NULL; }
API char *CharPrevExA(WORD cp, const char *start, const char *cur, DWORD flags) { (void)flags; return (char *)prev_char_cp(cp, start, cur); }

/* CharNextW steps over a whole user-perceived character: a surrogate pair
 * counts as one and trailing combining marks belong to their base. */
static int w_combining(WCHAR c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
}
API WCHAR *CharNextW(const WCHAR *p)
{
    if (!p || !*p) return (WCHAR *)p;
    if (p[0] >= 0xD800 && p[0] <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) p += 2; else p++;
    while (*p && w_combining(*p)) p++;
    return (WCHAR *)p;
}
API WCHAR *CharPrevW(const WCHAR *start, const WCHAR *cur)
{
    if (!start || !cur || cur <= start) return (WCHAR *)start;
    const WCHAR *p = cur - 1;
    while (p > start && w_combining(*p)) p--;
    if (p > start && *p >= 0xDC00 && *p <= 0xDFFF && p[-1] >= 0xD800 && p[-1] <= 0xDBFF) p--;
    return (WCHAR *)p;
}

/* OEM <-> ANSI: both are UTF-8, so the A forms copy and the W forms convert. */
API BOOL CharToOemA(const char *s, char *d) { if (!s || !d) return set_err(ERROR_INVALID_PARAMETER); memmove(d, s, strlen(s) + 1); return TRUE; }
API BOOL OemToCharA(const char *s, char *d) { if (!s || !d) return set_err(ERROR_INVALID_PARAMETER); memmove(d, s, strlen(s) + 1); return TRUE; }
API BOOL CharToOemBuffA(const char *s, char *d, DWORD n) { if (!s || !d) return set_err(ERROR_INVALID_PARAMETER); memmove(d, s, n); return TRUE; }
API BOOL OemToCharBuffA(const char *s, char *d, DWORD n) { if (!s || !d) return set_err(ERROR_INVALID_PARAMETER); memmove(d, s, n); return TRUE; }
API BOOL CharToOemW(const WCHAR *s, char *d)
{
    if (!s || !d) return set_err(ERROR_INVALID_PARAMETER);
    int need = lsiw_utf16_to_utf8(s, -1, NULL, 0);
    lsiw_utf16_to_utf8(s, -1, d, need);
    return TRUE;
}
API BOOL OemToCharW(const char *s, WCHAR *d)
{
    if (!s || !d) return set_err(ERROR_INVALID_PARAMETER);
    int need = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    lsiw_utf8_to_utf16(s, -1, d, need);
    return TRUE;
}
/* The Buff forms convert exactly n source characters into at most n bytes /
 * n WCHARs of output (UTF-8 output stops at the last whole character). */
API BOOL CharToOemBuffW(const WCHAR *s, char *d, DWORD n)
{
    if (!s || !d) return set_err(ERROR_INVALID_PARAMETER);
    char *u = lsiw_w2u_n(s, (int)n);
    if (!u) return FALSE;
    size_t l = strlen(u);
    if (l > n) { l = n; while (l > 0 && ((unsigned char)u[l] & 0xC0) == 0x80) l--; }
    memcpy(d, u, l);
    free(u);
    return TRUE;
}
API BOOL OemToCharBuffW(const char *s, WCHAR *d, DWORD n)
{
    if (!s || !d) return set_err(ERROR_INVALID_PARAMETER);
    if (!n) return TRUE;
    int need = lsiw_utf8_to_utf16(s, (int)n, NULL, 0);
    WCHAR *tmp = malloc((size_t)need * sizeof(WCHAR) + 2);
    if (!tmp) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    lsiw_utf8_to_utf16(s, (int)n, tmp, need);
    memcpy(d, tmp, (size_t)(need < (int)n ? need : (int)n) * sizeof(WCHAR));
    free(tmp);
    return TRUE;
}

/* The A forms take one byte of a UTF-8 string: only ASCII bytes are characters. */
API BOOL IsCharAlphaW(WCHAR c) { return w_isalpha(c); }
API BOOL IsCharAlphaNumericW(WCHAR c) { return w_isalpha(c) || w_isdigit(c); }
API BOOL IsCharUpperW(WCHAR c) { return w_isupper(c); }
API BOOL IsCharLowerW(WCHAR c) { return w_islower(c); }
API BOOL IsCharAlphaA(char c) { return (unsigned char)c < 0x80 && w_isalpha((WCHAR)(unsigned char)c); }
API BOOL IsCharAlphaNumericA(char c) { return (unsigned char)c < 0x80 && (w_isalpha((WCHAR)(unsigned char)c) || w_isdigit((WCHAR)(unsigned char)c)); }
API BOOL IsCharUpperA(char c) { return (unsigned char)c < 0x80 && w_isupper((WCHAR)(unsigned char)c); }
API BOOL IsCharLowerA(char c) { return (unsigned char)c < 0x80 && w_islower((WCHAR)(unsigned char)c); }

/* ================================================================== */
/* wsprintf                                                             */
/* ================================================================== */

/* wsprintf writes at most 1024 characters including the NUL and returns the
 * number stored, not counting it. */
#define WSPRINTF_MAX 1024

API int wvsprintfA(char *buf, const char *fmt, ms_va_list ap)
{
    if (!buf || !fmt) return 0;
    int r = lsiw_ms_vsnprintf(buf, WSPRINTF_MAX, fmt, ap, 0);
    return r < 0 ? 0 : r >= WSPRINTF_MAX ? WSPRINTF_MAX - 1 : r;
}
API int wvsprintfW(WCHAR *buf, const WCHAR *fmt, ms_va_list ap)
{
    if (!buf || !fmt) return 0;
    int r = lsiw_ms_vsnwprintf(buf, WSPRINTF_MAX, fmt, ap);
    return r < 0 ? (int)lsiw_wcslen(buf) : r;
}
API int wsprintfA(char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    int r = wvsprintfA(buf, fmt, ap);
    ms_va_end(ap);
    return r;
}
API int wsprintfW(WCHAR *buf, const WCHAR *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    int r = wvsprintfW(buf, fmt, ap);
    ms_va_end(ap);
    return r;
}

/* ================================================================== */
/* String resources                                                     */
/* ================================================================== */

/* RT_STRING resources hold blocks of 16 strings: block (id >> 4) + 1, entry
 * id & 15, each entry a WORD length followed by that many UTF-16 units (not
 * NUL-terminated). Returns a pointer into the mapped image and the length,
 * or NULL with the last error set. */
static const WCHAR *find_string(HINSTANCE h, UINT id, int *len)
{
    void *(WINAPI *find)(HMODULE, const WCHAR *, const WCHAR *, WORD) = k32("FindResourceExW");
    void *(WINAPI *load)(HMODULE, void *) = k32("LoadResource");
    void *(WINAPI *lock)(void *) = k32("LockResource");
    DWORD (WINAPI *size)(HMODULE, void *) = k32("SizeofResource");
    if (!find || !load || !lock || !size) { set_err(ERROR_RESOURCE_NAME_NOT_FOUND); return NULL; }
    void *res = find(h, (const WCHAR *)(uintptr_t)6 /* RT_STRING */, (const WCHAR *)(uintptr_t)((id >> 4) + 1), 0);
    if (!res) return NULL;
    void *mem = load(h, res);
    const WCHAR *p = mem ? lock(mem) : NULL;
    if (!p) { set_err(ERROR_RESOURCE_NAME_NOT_FOUND); return NULL; }
    const WCHAR *end = p + size(h, res) / sizeof(WCHAR);
    for (UINT i = 0; i < (id & 15); i++) {
        if (p >= end) { set_err(ERROR_RESOURCE_NAME_NOT_FOUND); return NULL; }
        p += 1 + *p;
    }
    if (p >= end || p + 1 + *p > end) { set_err(ERROR_RESOURCE_NAME_NOT_FOUND); return NULL; }
    *len = *p;
    return p + 1;
}

API int LoadStringW(HINSTANCE h, UINT id, WCHAR *buf, int max)
{
    if (!buf || max < 0) return set_err(ERROR_INVALID_PARAMETER);
    int len = 0;
    const WCHAR *s = find_string(h, id, &len);
    if (max == 0) {
        /* Documented: with cchBufferMax 0 the buffer receives a read-only
         * pointer to the resource itself, and the length is returned. */
        *(const WCHAR **)buf = s;
        return s ? len : 0;
    }
    if (!s) { buf[0] = 0; return 0; }
    return put_ww(s, (size_t)len, buf, max);
}

API int LoadStringA(HINSTANCE h, UINT id, char *buf, int max)
{
    if (!buf || max <= 0) return set_err(ERROR_INVALID_PARAMETER);
    int len = 0;
    const WCHAR *s = find_string(h, id, &len);
    if (!s) { buf[0] = 0; return 0; }
    char *u = lsiw_w2u_n(s, len);
    int n = u ? put_a(u, buf, max) : 0;
    free(u);
    return n;
}

/* ================================================================== */
/* Message boxes                                                        */
/* ================================================================== */

#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDTRYAGAIN 10
#define IDCONTINUE 11
#define MB_TIMEDOUT 32000
#define MB_ICONHAND 0x10
#define MB_ICONQUESTION 0x20
#define MB_ICONEXCLAMATION 0x30
#define MB_ICONASTERISK 0x40

/* Buttons of MB_OK .. MB_CANCELTRYCONTINUE, left to right. */
static const BYTE g_mb_buttons[7][3] = {
    { IDOK }, { IDOK, IDCANCEL }, { IDABORT, IDRETRY, IDIGNORE }, { IDYES, IDNO, IDCANCEL },
    { IDYES, IDNO }, { IDRETRY, IDCANCEL }, { IDCANCEL, IDTRYAGAIN, IDCONTINUE },
};
static const BYTE g_mb_count[7] = { 1, 2, 3, 3, 2, 2, 3 };
/* How the set maps onto a question dialog: affirmative button, the button
 * that closing the dialog means, and the optional third ("extra") button. */
static const BYTE g_mb_dialog[7][3] = {
    { IDOK, IDOK, 0 }, { IDOK, IDCANCEL, 0 }, { IDRETRY, IDABORT, IDIGNORE }, { IDYES, IDCANCEL, IDNO },
    { IDYES, IDNO, 0 }, { IDRETRY, IDCANCEL, 0 }, { IDCONTINUE, IDCANCEL, IDTRYAGAIN },
};

static const char *mb_label(int id)
{
    switch (id) {
    case IDOK: return "OK";
    case IDCANCEL: return "Cancel";
    case IDABORT: return "Abort";
    case IDRETRY: return "Retry";
    case IDIGNORE: return "Ignore";
    case IDYES: return "Yes";
    case IDNO: return "No";
    case IDTRYAGAIN: return "Try Again";
    case IDCONTINUE: return "Continue";
    }
    return "OK";
}

/* MB_DEFBUTTONn picks the n-th button; out-of-range values mean the first. */
static int mb_default(UINT type)
{
    UINT set = type & 0xF, def = (type >> 8) & 0xF;
    if (def >= g_mb_count[set]) def = 0;
    return g_mb_buttons[set][def];
}

static void trim_nl(char *s) { if (!s) return; size_t l = strlen(s); while (l && (s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = 0; }

/* zenity: one-button boxes use --error/--warning/--info by icon; the others
 * use --question with relabelled buttons and --extra-button for a third.
 * Returns the button ID, MB_TIMEDOUT, or -1 if zenity could not run. */
static int mb_zenity(const char *text, const char *title, UINT type, DWORD timeout)
{
    UINT set = type & 0xF, icon = type & 0xF0;
    char *argv[16], ok[64], cancel[64], extra[64], ttl[512], tmo[32];
    int n = 0, def = mb_default(type);
    argv[n++] = "zenity";
    if (set == 0) argv[n++] = icon == MB_ICONHAND ? "--error" : icon == MB_ICONEXCLAMATION ? "--warning" : "--info";
    else {
        argv[n++] = "--question";
        snprintf(ok, sizeof ok, "--ok-label=%s", mb_label(g_mb_dialog[set][0]));
        snprintf(cancel, sizeof cancel, "--cancel-label=%s", mb_label(g_mb_dialog[set][1]));
        argv[n++] = ok;
        argv[n++] = cancel;
        if (g_mb_dialog[set][2]) {
            snprintf(extra, sizeof extra, "--extra-button=%s", mb_label(g_mb_dialog[set][2]));
            argv[n++] = extra;
        }
        if (def == g_mb_dialog[set][1]) argv[n++] = "--default-cancel";
    }
    snprintf(ttl, sizeof ttl, "--title=%s", title);
    argv[n++] = ttl;
    argv[n++] = "--no-markup";
    argv[n++] = "--text";
    argv[n++] = (char *)text;
    if (timeout != INFINITE && timeout) {
        snprintf(tmo, sizeof tmo, "--timeout=%u", (unsigned)((timeout + 999) / 1000));
        argv[n++] = tmo;
    }
    argv[n] = NULL;
    char *out = NULL;
    int st = run_tool(argv, NULL, 0, &out, NULL, 0);
    trim_nl(out);
    int r;
    if (st == 0) r = g_mb_dialog[set][0];
    else if (st == 1) r = (g_mb_dialog[set][2] && out && !strcmp(out, mb_label(g_mb_dialog[set][2]))) ? g_mb_dialog[set][2] : g_mb_dialog[set][1];
    else if (st == 5) r = MB_TIMEDOUT;
    else r = -1;
    free(out);
    return r;
}

/* kdialog: --msgbox/--error/--sorry, --yesno or --yesnocancel (exit 0/1/2). */
static int mb_kdialog(const char *text, const char *title, UINT type)
{
    UINT set = type & 0xF, icon = type & 0xF0;
    char *argv[16];
    int n = 0;
    argv[n++] = "kdialog";
    argv[n++] = "--title";
    argv[n++] = (char *)title;
    if (set == 0) argv[n++] = icon == MB_ICONHAND ? "--error" : icon == MB_ICONEXCLAMATION ? "--sorry" : "--msgbox";
    else argv[n++] = g_mb_dialog[set][2] ? "--yesnocancel" : "--yesno";
    argv[n++] = (char *)text;
    if (set) {
        argv[n++] = "--yes-label";
        argv[n++] = (char *)mb_label(g_mb_dialog[set][0]);
        argv[n++] = "--no-label";
        argv[n++] = (char *)mb_label(g_mb_dialog[set][2] ? g_mb_dialog[set][2] : g_mb_dialog[set][1]);
        if (g_mb_dialog[set][2]) { argv[n++] = "--cancel-label"; argv[n++] = (char *)mb_label(g_mb_dialog[set][1]); }
    }
    argv[n] = NULL;
    int st = run_tool(argv, NULL, 0, NULL, NULL, 0);
    if (st == 0) return g_mb_dialog[set][0];
    if (st == 1) return g_mb_dialog[set][2] ? g_mb_dialog[set][2] : g_mb_dialog[set][1];
    if (st == 2) return g_mb_dialog[set][1];
    return -1;
}

/* The text always goes to stderr as "[Title] text". With a desktop session
 * (and LSIW_NO_GUI unset) a native dialog is shown and its answer mapped to
 * the Windows button ID; otherwise the default button is the answer. */
static int message_box(const char *text, const char *title, UINT type, DWORD timeout)
{
    if ((type & 0xF) > 6) return set_err(ERROR_INVALID_MSGBOX_STYLE);
    if (!title) title = "Error";                  /* Windows' default caption */
    if (!text) text = "";
    fprintf(stderr, "[%s] %s\n", title, text);
    fflush(stderr);
    int r = -1;
    if (gui_session()) {
        if (have_tool("zenity")) r = mb_zenity(text, title, type, timeout);
        else if (have_tool("kdialog")) r = mb_kdialog(text, title, type);
    }
    if (r == -1 || (r == MB_TIMEDOUT && (timeout == INFINITE || !timeout))) r = mb_default(type);
    return r;
}

static int message_box_w(const WCHAR *text, const WCHAR *title, UINT type, DWORD timeout)
{
    char *t = lsiw_w2u(text), *c = lsiw_w2u(title);
    int r = message_box(t, c, type, timeout);
    free(t);
    free(c);
    return r;
}

API int MessageBoxA(HWND w, const char *text, const char *title, UINT type) { (void)w; return message_box(text, title, type, INFINITE); }
API int MessageBoxW(HWND w, const WCHAR *text, const WCHAR *title, UINT type) { (void)w; return message_box_w(text, title, type, INFINITE); }
API int MessageBoxExA(HWND w, const char *text, const char *title, UINT type, WORD lang) { (void)w; (void)lang; return message_box(text, title, type, INFINITE); }
API int MessageBoxExW(HWND w, const WCHAR *text, const WCHAR *title, UINT type, WORD lang) { (void)w; (void)lang; return message_box_w(text, title, type, INFINITE); }
API int MessageBoxTimeoutA(HWND w, const char *text, const char *title, UINT type, WORD lang, DWORD ms) { (void)w; (void)lang; return message_box(text, title, type, ms); }
API int MessageBoxTimeoutW(HWND w, const WCHAR *text, const WCHAR *title, UINT type, WORD lang, DWORD ms) { (void)w; (void)lang; return message_box_w(text, title, type, ms); }

/* MSGBOXPARAMS text and caption may be string-resource IDs of hInstance. */
static char *resource_or_string_w(HINSTANCE h, const WCHAR *s)
{
    if (!s) return NULL;
    if (!IS_INTRES(s)) return lsiw_w2u(s);
    int len = 0;
    const WCHAR *r = find_string(h, (UINT)(uintptr_t)s, &len);
    return r ? lsiw_w2u_n(r, len) : strdup("");
}

API int MessageBoxIndirectW(const MSGBOXPARAMSW *p)
{
    if (!p || p->cbSize != sizeof *p) return set_err(ERROR_INVALID_PARAMETER);
    char *t = resource_or_string_w(p->hInstance, p->lpszText), *c = resource_or_string_w(p->hInstance, p->lpszCaption);
    int r = message_box(t, c, p->dwStyle, INFINITE);
    free(t);
    free(c);
    return r;
}
API int MessageBoxIndirectA(const MSGBOXPARAMSA *p)
{
    if (!p || p->cbSize != sizeof *p) return set_err(ERROR_INVALID_PARAMETER);
    char *t = IS_INTRES(p->lpszText) ? resource_or_string_w(p->hInstance, (const WCHAR *)p->lpszText) : strdup(p->lpszText);
    char *c = IS_INTRES(p->lpszCaption) ? resource_or_string_w(p->hInstance, (const WCHAR *)p->lpszCaption) : strdup(p->lpszCaption);
    int r = message_box(t, c, p->dwStyle, INFINITE);
    free(t);
    free(c);
    return r;
}

/* ================================================================== */
/* Screen, system metrics, colours                                      */
/* ================================================================== */

static pthread_once_t g_screen_once = PTHREAD_ONCE_INIT;
static int g_scr_w = 1920, g_scr_h = 1080;

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (ok) trim_nl(buf);
    return ok;
}

/* Screen size: $LSIW_SCREEN ("2560x1440"), else the preferred mode of the
 * first connected DRM connector, else 1920x1080. */
static void screen_init(void)
{
    const char *s = getenv("LSIW_SCREEN");
    int w, h;
    if (s && sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w < 65536 && h < 65536) { g_scr_w = w; g_scr_h = h; return; }
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    char best[256] = "";
    int bw = 0, bh = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) || !strchr(e->d_name, '-')) continue;
        char path[512], line[64];
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
        if (!read_line(path, line, sizeof line) || strcmp(line, "connected")) continue;
        snprintf(path, sizeof path, "/sys/class/drm/%s/modes", e->d_name);
        if (!read_line(path, line, sizeof line) || sscanf(line, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) continue;
        if (!best[0] || strcmp(e->d_name, best) < 0) {
            snprintf(best, sizeof best, "%s", e->d_name);
            bw = w;
            bh = h;
        }
    }
    closedir(d);
    if (bw > 0) { g_scr_w = bw; g_scr_h = bh; }
}
static RECT screen_rect(void)
{
    pthread_once(&g_screen_once, screen_init);
    RECT r = { 0, 0, g_scr_w, g_scr_h };
    return r;
}

static int mul_div(int v, UINT dpi) { return (int)(((int64_t)v * dpi + 48) / 96); }

/* Windows 10 values at 96 DPI. */
static int system_metric(int i, UINT dpi)
{
    RECT s = screen_rect();
    int w = s.right, h = s.bottom, v = -1;
    switch (i) {
    case 0: case 16: case 78: return w;               /* CXSCREEN, CXFULLSCREEN, CXVIRTUALSCREEN */
    case 1: case 79: return h;                        /* CYSCREEN, CYVIRTUALSCREEN */
    case 17: return h - mul_div(23, dpi);             /* CYFULLSCREEN: work area minus caption */
    case 59: return w + 2 * mul_div(10, dpi);         /* CXMAXTRACK */
    case 60: return h + 2 * mul_div(10, dpi);
    case 61: return w + 2 * mul_div(8, dpi);          /* CXMAXIMIZED */
    case 62: return h + 2 * mul_div(8, dpi);
    case 76: case 77: return 0;                       /* X/YVIRTUALSCREEN */
    case 19: case 75: case 80: case 81: case 0x2002: case 0x2003: return 1;  /* MOUSEPRESENT, MOUSEWHEELPRESENT, CMONITORS, SAMEDISPLAYFORMAT, CARETBLINKINGENABLED, CONVERTIBLESLATEMODE (laptop) */
    case 43: return 3;                                /* CMOUSEBUTTONS */
    case 56: return 8;                                /* ARRANGE: ARW_HIDE */
    case 63: return 3;                                /* NETWORK: present */
    case 2: case 3: case 9: case 10: case 20: case 21: v = 17; break;   /* scroll bars */
    case 4: case 51: v = 23; break;                   /* CYCAPTION, CYSMCAPTION */
    case 5: case 6: case 83: case 84: v = 1; break;   /* borders, focus borders */
    case 7: case 8: v = 3; break;                     /* DLGFRAME */
    case 11: case 12: case 13: case 14: v = 32; break;/* icon, cursor */
    case 15: v = 20; break;                           /* CYMENU */
    case 28: case 34: v = 136; break;                 /* CXMIN, CXMINTRACK */
    case 29: case 35: v = 39; break;
    case 30: v = 36; break;                           /* CXSIZE */
    case 31: case 52: case 53: v = 22; break;         /* CYSIZE, small caption buttons */
    case 32: case 33: v = 4; break;                   /* FRAME */
    case 36: case 37: case 68: case 69: v = 4; break; /* DOUBLECLK, DRAG */
    case 38: case 39: v = 75; break;                  /* ICONSPACING */
    case 45: case 46: v = 2; break;                   /* EDGE */
    case 47: case 57: v = 160; break;                 /* MINSPACING, MINIMIZED */
    case 48: case 58: v = 28; break;
    case 49: case 50: v = 16; break;                  /* SMICON */
    case 54: case 55: v = 19; break;                  /* MENUSIZE */
    case 71: case 72: v = 15; break;                  /* MENUCHECK */
    case 92: v = 4; break;                            /* CXPADDEDBORDER */
    default: return 0;                                /* flags such as SM_REMOTESESSION, SM_CLEANBOOT, unknown indexes */
    }
    return mul_div(v, dpi);
}
API int GetSystemMetrics(int i) { return system_metric(i, 96); }
API int GetSystemMetricsForDpi(int i, UINT dpi) { if (!dpi) return set_err(ERROR_INVALID_PARAMETER); return system_metric(i, dpi); }

#define RGBV(r, g, b) ((DWORD)(r) | ((DWORD)(g) << 8) | ((DWORD)(b) << 16))
static const DWORD g_syscolors[31] = {
    RGBV(200, 200, 200), RGBV(0, 0, 0), RGBV(153, 180, 209), RGBV(191, 205, 219), RGBV(240, 240, 240),
    RGBV(255, 255, 255), RGBV(100, 100, 100), RGBV(0, 0, 0), RGBV(0, 0, 0), RGBV(0, 0, 0),
    RGBV(180, 180, 180), RGBV(244, 247, 252), RGBV(171, 171, 171), RGBV(0, 120, 215), RGBV(255, 255, 255),
    RGBV(240, 240, 240), RGBV(160, 160, 160), RGBV(109, 109, 109), RGBV(0, 0, 0), RGBV(0, 0, 0),
    RGBV(255, 255, 255), RGBV(105, 105, 105), RGBV(227, 227, 227), RGBV(0, 0, 0), RGBV(255, 255, 225),
    RGBV(0, 0, 0), RGBV(0, 102, 204), RGBV(185, 209, 234), RGBV(215, 228, 242), RGBV(0, 120, 215),
    RGBV(240, 240, 240),
};
API DWORD GetSysColor(int i) { return i >= 0 && i < 31 ? g_syscolors[i] : 0; }
/* Brushes are GDI objects; LSIW hands out stable tokens (no gdi32 here). */
API HBRUSH GetSysColorBrush(int i) { return i >= 0 && i < 31 ? (HBRUSH)(uintptr_t)(TOK_BRUSH | (unsigned)i) : NULL; }

API UINT GetDoubleClickTime(void) { return 500; }
API UINT GetCaretBlinkTime(void) { return 530; }

/* ================================================================== */
/* SystemParametersInfo                                                 */
/* ================================================================== */

static void logfont_w(LOGFONTW *lf)
{
    memset(lf, 0, sizeof *lf);
    lf->lfHeight = -12;           /* 9 pt at 96 DPI */
    lf->lfWeight = 400;
    lf->lfCharSet = 1;            /* DEFAULT_CHARSET */
    static const char face[] = "Segoe UI";
    for (size_t i = 0; i < sizeof face; i++) lf->lfFaceName[i] = (WCHAR)face[i];
}
static void logfont_a(LOGFONTA *lf)
{
    memset(lf, 0, sizeof *lf);
    lf->lfHeight = -12;
    lf->lfWeight = 400;
    lf->lfCharSet = 1;
    memcpy(lf->lfFaceName, "Segoe UI", 9);
}

static pthread_mutex_t g_spi_lock = PTHREAD_MUTEX_INITIALIZER;
static STICKYKEYS g_sticky = { sizeof(STICKYKEYS), 0x1FA };
static TOGGLEKEYS g_toggle = { sizeof(TOGGLEKEYS), 0x1A };
static FILTERKEYS g_filter = { sizeof(FILTERKEYS), 0x7E, 1000, 1000, 500, 0 };
static MOUSEKEYS g_mousekeys = { sizeof(MOUSEKEYS), 0x1E, 80, 1200, 99, 0, 0 };
static ACCESSTIMEOUT g_accesstimeout = { sizeof(ACCESSTIMEOUT), 0x2, 300000 };

/* Accessibility structures: GET copies the stored value, SET replaces it.
 * Both require cbSize to match, like Windows. */
static BOOL spi_struct(void *store, UINT size, void *pv, int set)
{
    if (!pv || *(UINT *)pv != size) return set_err(ERROR_INVALID_PARAMETER);
    pthread_mutex_lock(&g_spi_lock);
    if (set) memcpy(store, pv, size); else memcpy(pv, store, size);
    pthread_mutex_unlock(&g_spi_lock);
    return TRUE;
}

/* BOOL settings in the 0x1000 range (GET is even, SET is odd). */
static const struct { WORD action; BYTE value; } g_spi_bool[] = {
    { 0x1000, 0 }, { 0x1002, 1 }, { 0x1004, 1 }, { 0x1006, 1 }, { 0x1008, 1 }, { 0x100A, 0 }, { 0x100C, 0 },
    { 0x100E, 1 }, { 0x1012, 1 }, { 0x1014, 1 }, { 0x1016, 1 }, { 0x1018, 1 }, { 0x101A, 1 }, { 0x101C, 0 },
    { 0x101E, 0 }, { 0x1020, 1 }, { 0x1022, 1 }, { 0x1024, 1 }, { 0x1026, 0 }, { 0x103E, 1 }, { 0x1040, 0 },
    { 0x1042, 1 }, { 0x1048, 1 }, { 0x104A, 0 },
};
/* DWORD settings in the 0x2000 range. */
static const struct { WORD action; DWORD value; } g_spi_dword[] = {
    { 0x2000, 200000 }, { 0x2002, 0 }, { 0x2004, 7 }, { 0x2006, 1 }, { 0x2008, 1200 }, { 0x200A, 2 },
    { 0x200C, 1200 }, { 0x200E, 1 }, { 0x2010, 1 }, { 0x2012, 1 }, { 0x2014, 24 }, { 0x2016, 5 },
    { 0x2018, 0 }, { 0x201A, 0 }, { 0x201C, 0 }, { 0x2022, 5000 },
};
/* Simple GETs below 0x100: action, value, and how many bytes pvParam takes. */
static const struct { BYTE action; BYTE size; DWORD value; } g_spi_simple[] = {
    { 0x01, 4, 1 },      /* GETBEEP */
    { 0x05, 4, 1 },      /* GETBORDER */
    { 0x0A, 4, 31 },     /* GETKEYBOARDSPEED */
    { 0x0E, 4, 0 },      /* GETSCREENSAVETIMEOUT */
    { 0x10, 4, 0 },      /* GETSCREENSAVEACTIVE */
    { 0x16, 4, 1 },      /* GETKEYBOARDDELAY */
    { 0x19, 4, 1 },      /* GETICONTITLEWRAP */
    { 0x1B, 4, 0 },      /* GETMENUDROPALIGNMENT */
    { 0x23, 4, 1 },      /* GETFASTTASKSWITCH */
    { 0x26, 4, 1 },      /* GETDRAGFULLWINDOWS */
    { 0x38, 4, 0 },      /* GETSHOWSOUNDS */
    { 0x44, 4, 0 },      /* GETKEYBOARDPREF */
    { 0x46, 4, 0 },      /* GETSCREENREADER */
    { 0x4A, 4, 1 },      /* GETFONTSMOOTHING */
    { 0x4F, 4, 0 },      /* GETLOWPOWERTIMEOUT */
    { 0x50, 4, 0 },      /* GETPOWEROFFTIMEOUT */
    { 0x53, 4, 0 },      /* GETLOWPOWERACTIVE */
    { 0x54, 4, 0 },      /* GETPOWEROFFACTIVE */
    { 0x5E, 4, 0 },      /* GETMOUSETRAILS */
    { 0x5F, 4, 0 },      /* GETSNAPTODEFBUTTON */
    { 0x62, 4, 4 },      /* GETMOUSEHOVERWIDTH */
    { 0x64, 4, 4 },      /* GETMOUSEHOVERHEIGHT */
    { 0x66, 4, 400 },    /* GETMOUSEHOVERTIME */
    { 0x68, 4, 3 },      /* GETWHEELSCROLLLINES */
    { 0x6A, 4, 400 },    /* GETMENUSHOWDELAY */
    { 0x6C, 4, 3 },      /* GETWHEELSCROLLCHARS */
    { 0x6E, 4, 1 },      /* GETSHOWIMEUI */
    { 0x70, 4, 10 },     /* GETMOUSESPEED */
    { 0x72, 4, 0 },      /* GETSCREENSAVERRUNNING */
    { 0x76, 4, 0 },      /* GETSCREENSAVESECURE */
    { 0x78, 4, 5000 },   /* GETHUNGAPPTIMEOUT */
    { 0x7A, 4, 5000 },   /* GETWAITTOKILLTIMEOUT */
    { 0x7C, 4, 20000 },  /* GETWAITTOKILLSERVICETIMEOUT */
    { 0x82, 4, 1 },      /* GETWINARRANGING */
    { 0x8E, 4, 1 },      /* GETSNAPSIZING */
    { 0x90, 4, 1 },      /* GETDOCKMOVING */
};
/* SET actions below 0x62 (from 0x62 on, odd actions are SETs). */
static const BYTE g_spi_sets[] = {
    0x02, 0x04, 0x06, 0x0B, 0x0D, 0x0F, 0x11, 0x13, 0x14, 0x15, 0x17, 0x18, 0x1A, 0x1C, 0x1D, 0x1E, 0x20,
    0x21, 0x22, 0x24, 0x25, 0x2A, 0x2C, 0x2E, 0x2F, 0x31, 0x39, 0x3F, 0x41, 0x43, 0x45, 0x47, 0x49, 0x4B,
    0x4C, 0x4D, 0x4E, 0x51, 0x52, 0x55, 0x56, 0x57, 0x58, 0x5A, 0x5B, 0x5D, 0x60, 0x61,
};

static int spi_is_set(UINT a)
{
    if (a >= 0x62 && a < 0x100) return a & 1;
    if ((a >= 0x1000 && a <= 0x104B) || (a >= 0x2000 && a <= 0x2029)) return a & 1;
    for (size_t i = 0; i < sizeof g_spi_sets; i++) if (g_spi_sets[i] == a) return 1;
    return 0;
}

static BOOL spi(UINT a, UINT ui, void *pv, int wide)
{
    for (size_t i = 0; i < sizeof g_spi_simple / sizeof g_spi_simple[0]; i++)
        if (g_spi_simple[i].action == a) {
            if (!pv) return set_err(ERROR_NOACCESS_);
            *(DWORD *)pv = g_spi_simple[i].value;
            return TRUE;
        }
    for (size_t i = 0; i < sizeof g_spi_bool / sizeof g_spi_bool[0]; i++)
        if (g_spi_bool[i].action == a) {
            if (!pv) return set_err(ERROR_NOACCESS_);
            *(BOOL *)pv = g_spi_bool[i].value;
            return TRUE;
        }
    for (size_t i = 0; i < sizeof g_spi_dword / sizeof g_spi_dword[0]; i++)
        if (g_spi_dword[i].action == a) {
            if (!pv) return set_err(ERROR_NOACCESS_);
            *(DWORD *)pv = g_spi_dword[i].value;
            return TRUE;
        }
    switch (a) {
    case 0x03:                                          /* GETMOUSE: thresholds and acceleration */
        if (!pv) return set_err(ERROR_NOACCESS_);
        ((int *)pv)[0] = 6; ((int *)pv)[1] = 10; ((int *)pv)[2] = 1;
        return TRUE;
    case 0x0D: case 0x18:                               /* ICONHORIZONTAL/VERTICALSPACING */
        if (pv) *(int *)pv = 75;
        return TRUE;
    case 0x1F:                                          /* GETICONTITLELOGFONT */
        if (!pv) return set_err(ERROR_NOACCESS_);
        if (wide) logfont_w(pv); else logfont_a(pv);
        return TRUE;
    case 0x29: {                                        /* GETNONCLIENTMETRICS */
        if (!pv) return set_err(ERROR_NOACCESS_);
        UINT cb = *(UINT *)pv;
        UINT full = wide ? sizeof(NONCLIENTMETRICSW) : sizeof(NONCLIENTMETRICSA);
        if (cb != full && cb != full - 4) return set_err(ERROR_INVALID_PARAMETER);  /* pre-Vista size lacks iPaddedBorderWidth */
        if (wide) {
            NONCLIENTMETRICSW m;
            memset(&m, 0, sizeof m);
            m.iBorderWidth = 1; m.iScrollWidth = 17; m.iScrollHeight = 17; m.iCaptionWidth = 36; m.iCaptionHeight = 22;
            m.iSmCaptionWidth = 22; m.iSmCaptionHeight = 22; m.iMenuWidth = 19; m.iMenuHeight = 19; m.iPaddedBorderWidth = 4;
            logfont_w(&m.lfCaptionFont); logfont_w(&m.lfSmCaptionFont); logfont_w(&m.lfMenuFont);
            logfont_w(&m.lfStatusFont); logfont_w(&m.lfMessageFont);
            m.cbSize = cb;
            memcpy(pv, &m, cb);
        } else {
            NONCLIENTMETRICSA m;
            memset(&m, 0, sizeof m);
            m.iBorderWidth = 1; m.iScrollWidth = 17; m.iScrollHeight = 17; m.iCaptionWidth = 36; m.iCaptionHeight = 22;
            m.iSmCaptionWidth = 22; m.iSmCaptionHeight = 22; m.iMenuWidth = 19; m.iMenuHeight = 19; m.iPaddedBorderWidth = 4;
            logfont_a(&m.lfCaptionFont); logfont_a(&m.lfSmCaptionFont); logfont_a(&m.lfMenuFont);
            logfont_a(&m.lfStatusFont); logfont_a(&m.lfMessageFont);
            m.cbSize = cb;
            memcpy(pv, &m, cb);
        }
        return TRUE;
    }
    case 0x2B: {                                        /* GETMINIMIZEDMETRICS */
        MINIMIZEDMETRICS *m = pv;
        if (!m || m->cbSize != sizeof *m) return set_err(ERROR_INVALID_PARAMETER);
        m->iWidth = 160; m->iHorzGap = 0; m->iVertGap = 0; m->iArrange = 8;
        return TRUE;
    }
    case 0x2D:                                          /* GETICONMETRICS */
        if (wide) {
            ICONMETRICSW *m = pv;
            if (!m || m->cbSize != sizeof *m) return set_err(ERROR_INVALID_PARAMETER);
            m->iHorzSpacing = 75; m->iVertSpacing = 75; m->iTitleWrap = 1;
            logfont_w(&m->lfFont);
        } else {
            ICONMETRICSA *m = pv;
            if (!m || m->cbSize != sizeof *m) return set_err(ERROR_INVALID_PARAMETER);
            m->iHorzSpacing = 75; m->iVertSpacing = 75; m->iTitleWrap = 1;
            logfont_a(&m->lfFont);
        }
        return TRUE;
    case 0x30:                                          /* GETWORKAREA: no taskbar, the whole screen */
        if (!pv) return set_err(ERROR_NOACCESS_);
        *(RECT *)pv = screen_rect();
        return TRUE;
    case 0x32: case 0x33: return spi_struct(&g_filter, sizeof g_filter, pv, a & 1);
    case 0x34: case 0x35: return spi_struct(&g_toggle, sizeof g_toggle, pv, a & 1);
    case 0x36: case 0x37: return spi_struct(&g_mousekeys, sizeof g_mousekeys, pv, a & 1);
    case 0x3A: case 0x3B: return spi_struct(&g_sticky, sizeof g_sticky, pv, a & 1);
    case 0x3C: case 0x3D: return spi_struct(&g_accesstimeout, sizeof g_accesstimeout, pv, a & 1);
    case 0x42: {                                        /* GETHIGHCONTRAST: available, off */
        static const WCHAR empty_w[1] = { 0 };
        HIGHCONTRAST *h = pv;
        if (!h || h->cbSize != sizeof *h) return set_err(ERROR_INVALID_PARAMETER);
        h->dwFlags = 0x2;                               /* HCF_AVAILABLE */
        h->lpszDefaultScheme = wide ? (void *)empty_w : (void *)"";
        return TRUE;
    }
    case 0x48: {                                        /* GETANIMATION */
        ANIMATIONINFO *ai = pv;
        if (!ai || ai->cbSize != sizeof *ai) return set_err(ERROR_INVALID_PARAMETER);
        ai->iMinAnimate = 1;
        return TRUE;
    }
    case 0x59:                                          /* GETDEFAULTINPUTLANG */
        if (!pv) return set_err(ERROR_NOACCESS_);
        *(HKL *)pv = US_HKL;
        return TRUE;
    case 0x73:                                          /* GETDESKWALLPAPER: none */
        if (!pv) return set_err(ERROR_NOACCESS_);
        if (ui) { if (wide) *(WCHAR *)pv = 0; else *(char *)pv = 0; }
        return TRUE;
    case 0x74: {                                        /* GETAUDIODESCRIPTION */
        AUDIODESCRIPTION *ad = pv;
        if (!ad || ad->cbSize != sizeof *ad || ui != sizeof *ad) return set_err(ERROR_INVALID_PARAMETER);
        ad->Enabled = FALSE;
        ad->Locale = 0x0409;
        return TRUE;
    }
    }
    if (spi_is_set(a)) return TRUE;                     /* settings changes are accepted and ignored */
    return set_err(ERROR_INVALID_SPI_VALUE);
}
API BOOL SystemParametersInfoA(UINT a, UINT ui, void *pv, UINT winini) { (void)winini; return spi(a, ui, pv, 0); }
API BOOL SystemParametersInfoW(UINT a, UINT ui, void *pv, UINT winini) { (void)winini; return spi(a, ui, pv, 1); }

/* ================================================================== */
/* Windows, monitors, display settings                                  */
/* ================================================================== */

static BOOL bad_window(void) { return set_err(ERROR_INVALID_WINDOW_HANDLE); }

API HWND GetDesktopWindow(void) { return DESKTOP_HWND; }
API HWND GetForegroundWindow(void) { return NULL; }
API HWND GetActiveWindow(void) { return NULL; }
API HWND GetShellWindow(void) { return NULL; }
API BOOL IsWindow(HWND w) { return w == DESKTOP_HWND; }
API BOOL IsWindowVisible(HWND w) { return w == DESKTOP_HWND; }
API DWORD GetWindowThreadProcessId(HWND w, DWORD *pid)
{
    if (pid) *pid = 0;
    if (w != DESKTOP_HWND) set_err(ERROR_INVALID_WINDOW_HANDLE);
    return 0;
}
API BOOL GetWindowRect(HWND w, RECT *r)
{
    if (w != DESKTOP_HWND) return bad_window();
    if (!r) return set_err(ERROR_NOACCESS_);
    *r = screen_rect();
    return TRUE;
}
API BOOL GetClientRect(HWND w, RECT *r) { return GetWindowRect(w, r); }
/* No top-level windows exist, so searches find nothing and enumerations
 * succeed without calling back. */
API HWND FindWindowA(const char *cls, const char *name) { (void)cls; (void)name; return NULL; }
API HWND FindWindowW(const WCHAR *cls, const WCHAR *name) { (void)cls; (void)name; return NULL; }
API HWND FindWindowExA(HWND parent, HWND after, const char *cls, const char *name) { (void)parent; (void)after; (void)cls; (void)name; return NULL; }
API HWND FindWindowExW(HWND parent, HWND after, const WCHAR *cls, const WCHAR *name) { (void)parent; (void)after; (void)cls; (void)name; return NULL; }
API BOOL EnumWindows(void *proc, LPARAM lp) { (void)proc; (void)lp; return TRUE; }
API BOOL EnumDesktopWindows(HDESK d, void *proc, LPARAM lp) { (void)d; (void)proc; (void)lp; return TRUE; }
API BOOL ShowWindow(HWND w, int cmd) { (void)cmd; if (w != DESKTOP_HWND) bad_window(); return FALSE; }
API int GetWindowTextA(HWND w, char *buf, int max) { (void)w; if (buf && max > 0) buf[0] = 0; return bad_window(); }
API int GetWindowTextW(HWND w, WCHAR *buf, int max) { (void)w; if (buf && max > 0) buf[0] = 0; return bad_window(); }
API int GetWindowTextLengthA(HWND w) { (void)w; return bad_window(); }
API int GetWindowTextLengthW(HWND w) { (void)w; return bad_window(); }
API BOOL SetWindowTextA(HWND w, const char *s) { (void)w; (void)s; return bad_window(); }
API BOOL SetWindowTextW(HWND w, const WCHAR *s) { (void)w; (void)s; return bad_window(); }
API BOOL SetForegroundWindow(HWND w) { (void)w; return FALSE; }
API BOOL FlashWindow(HWND w, BOOL invert) { (void)w; (void)invert; return FALSE; }
API BOOL FlashWindowEx(void *info) { (void)info; return FALSE; }
API HMENU GetSystemMenu(HWND w, BOOL revert) { (void)w; (void)revert; bad_window(); return NULL; }

/* The desktop window's class is the atom "#32769". */
API int GetClassNameA(HWND w, char *buf, int max) { if (w == DESKTOP_HWND) return put_a("#32769", buf, max); return bad_window(); }
API int GetClassNameW(HWND w, WCHAR *buf, int max) { if (w == DESKTOP_HWND) return put_w("#32769", buf, max); return bad_window(); }
API UINT RealGetWindowClassA(HWND w, char *buf, UINT max) { return (UINT)GetClassNameA(w, buf, (int)max); }
API UINT RealGetWindowClassW(HWND w, WCHAR *buf, UINT max) { return (UINT)GetClassNameW(w, buf, (int)max); }

#define MONITOR_DEFAULTTONULL 0
static HMONITOR monitor_or_default(int inside, DWORD flags) { return inside || flags != MONITOR_DEFAULTTONULL ? PRIMARY_MON : NULL; }
API HMONITOR MonitorFromWindow(HWND w, DWORD flags) { return monitor_or_default(w == DESKTOP_HWND, flags); }
API HMONITOR MonitorFromPoint(POINT pt, DWORD flags)
{
    RECT s = screen_rect();
    return monitor_or_default(pt.x >= s.left && pt.x < s.right && pt.y >= s.top && pt.y < s.bottom, flags);
}
API HMONITOR MonitorFromRect(const RECT *r, DWORD flags)
{
    RECT s = screen_rect();
    int hit = r && r->left < s.right && r->right > s.left && r->top < s.bottom && r->bottom > s.top;
    return monitor_or_default(hit, flags);
}

static BOOL monitor_info(HMONITOR m, void *p, int wide)
{
    if (m != PRIMARY_MON) return set_err(ERROR_INVALID_MONITOR_HANDLE);
    MONITORINFO *mi = p;
    if (!mi) return set_err(ERROR_INVALID_PARAMETER);
    DWORD ex = wide ? sizeof(MONITORINFOEXW) : sizeof(MONITORINFOEXA);
    if (mi->cbSize != sizeof(MONITORINFO) && mi->cbSize != ex) return set_err(ERROR_INVALID_PARAMETER);
    mi->rcMonitor = mi->rcWork = screen_rect();
    mi->dwFlags = 1;                                    /* MONITORINFOF_PRIMARY */
    if (mi->cbSize == ex) {
        if (wide) put_w("\\\\.\\DISPLAY1", ((MONITORINFOEXW *)p)->szDevice, 32);
        else put_a("\\\\.\\DISPLAY1", ((MONITORINFOEXA *)p)->szDevice, 32);
    }
    return TRUE;
}
API BOOL GetMonitorInfoA(HMONITOR m, void *p) { return monitor_info(m, p, 0); }
API BOOL GetMonitorInfoW(HMONITOR m, void *p) { return monitor_info(m, p, 1); }

/* One monitor: the callback runs once with the screen (clipped to the clip
 * rectangle when one is given) unless the clip misses the screen. */
API BOOL EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC proc, LPARAM lp)
{
    if (!proc) return set_err(ERROR_INVALID_PARAMETER);
    RECT r = screen_rect();
    if (clip) {
        if (clip->left > r.left) r.left = clip->left;
        if (clip->top > r.top) r.top = clip->top;
        if (clip->right < r.right) r.right = clip->right;
        if (clip->bottom < r.bottom) r.bottom = clip->bottom;
        if (r.left >= r.right || r.top >= r.bottom) return TRUE;
    }
    proc(PRIMARY_MON, dc, &r, lp);
    return TRUE;
}

/* Display modes: the common sizes up to the current one, plus the current
 * one, all at 32 bpp and 60 Hz. */
static int display_mode(DWORD n, int *w, int *h)
{
    static const int sizes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 },
                                    { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    RECT s = screen_rect();
    if (n == 0xFFFFFFFFu || n == 0xFFFFFFFEu) { *w = s.right; *h = s.bottom; return 1; }   /* ENUM_CURRENT/REGISTRY_SETTINGS */
    DWORD k = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        if (sizes[i][0] > s.right || sizes[i][1] > s.bottom) continue;
        if (sizes[i][0] == s.right && sizes[i][1] == s.bottom) continue;
        if (k++ == n) { *w = sizes[i][0]; *h = sizes[i][1]; return 1; }
    }
    if (k == n) { *w = s.right; *h = s.bottom; return 1; }
    return 0;
}

static int is_display1(const char *dev) { return !dev || !*dev || !strcasecmp(dev, "\\\\.\\DISPLAY1"); }

#define DM_FIELDS (0x20 /* POSITION */ | 0x40000 /* BITSPERPEL */ | 0x80000 /* PELSWIDTH */ | 0x100000 /* PELSHEIGHT */ | \
                   0x200000 /* DISPLAYFLAGS */ | 0x400000 /* DISPLAYFREQUENCY */)

/* The caller's dmSize says how much of DEVMODE it has room for. */
static BOOL enum_settings(const char *dev, DWORD n, void *p, int wide)
{
    int w, h;
    if (!p) return set_err(ERROR_INVALID_PARAMETER);
    if (!is_display1(dev) || !display_mode(n, &w, &h)) return FALSE;
    size_t full = wide ? sizeof(DEVMODEW) : sizeof(DEVMODEA);
    size_t size = wide ? ((DEVMODEW *)p)->dmSize : ((DEVMODEA *)p)->dmSize;
    if (!size || size > full) size = full;
    DEVMODEW dw;
    DEVMODEA da;
    memset(&dw, 0, sizeof dw);
    memset(&da, 0, sizeof da);
#define FILL(d) do { (d).dmSpecVersion = 0x0401; (d).dmSize = (WORD)size; (d).dmFields = DM_FIELDS; \
        (d).dmLogPixels = 96; (d).dmBitsPerPel = 32; (d).dmPelsWidth = (DWORD)w; (d).dmPelsHeight = (DWORD)h; \
        (d).dmDisplayFrequency = 60; } while (0)
    if (wide) { FILL(dw); put_w("\\\\.\\DISPLAY1", dw.dmDeviceName, 32); memcpy(p, &dw, size); }
    else { FILL(da); put_a("\\\\.\\DISPLAY1", da.dmDeviceName, 32); memcpy(p, &da, size); }
#undef FILL
    return TRUE;
}
API BOOL EnumDisplaySettingsA(const char *dev, DWORD n, DEVMODEA *dm) { return enum_settings(dev, n, dm, 0); }
API BOOL EnumDisplaySettingsExA(const char *dev, DWORD n, DEVMODEA *dm, DWORD f) { (void)f; return enum_settings(dev, n, dm, 0); }
API BOOL EnumDisplaySettingsW(const WCHAR *dev, DWORD n, DEVMODEW *dm)
{
    char *d = lsiw_w2u(dev);
    BOOL r = enum_settings(d, n, dm, 1);
    free(d);
    return r;
}
API BOOL EnumDisplaySettingsExW(const WCHAR *dev, DWORD n, DEVMODEW *dm, DWORD f) { (void)f; return EnumDisplaySettingsW(dev, n, dm); }

/* One adapter (\\.\DISPLAY1) with one monitor attached to it. */
static BOOL enum_devices(const char *dev, DWORD n, void *p, int wide)
{
    if (!p || n != 0) return FALSE;
    const char *name, *desc, *id, *key;
    DWORD state;
    if (!dev || !*dev) {
        name = "\\\\.\\DISPLAY1"; desc = "LSIW Display Adapter"; state = 0x1 | 0x4;  /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
        id = "PCI\\VEN_0000&DEV_0000";
        key = "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\{00000000-0000-0000-0000-000000000000}\\0000";
    } else if (is_display1(dev)) {
        name = "\\\\.\\DISPLAY1\\Monitor0"; desc = "Generic PnP Monitor"; state = 0x1 | 0x2;  /* ACTIVE | ATTACHED */
        id = "MONITOR\\Default_Monitor\\{4d36e96e-e325-11ce-bfc1-08002be10318}\\0000";
        key = "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\{4d36e96e-e325-11ce-bfc1-08002be10318}\\0000";
    } else return FALSE;
    DWORD cb = *(DWORD *)p;
    if (wide) {
        DISPLAY_DEVICEW d;
        memset(&d, 0, sizeof d);
        put_w(name, d.DeviceName, 32); put_w(desc, d.DeviceString, 128); put_w(id, d.DeviceID, 128); put_w(key, d.DeviceKey, 128);
        d.StateFlags = state;
        if (cb > sizeof d) cb = sizeof d;
        if (cb > sizeof(DWORD)) memcpy((char *)p + sizeof(DWORD), (char *)&d + sizeof(DWORD), cb - sizeof(DWORD));
    } else {
        DISPLAY_DEVICEA d;
        memset(&d, 0, sizeof d);
        put_a(name, d.DeviceName, 32); put_a(desc, d.DeviceString, 128); put_a(id, d.DeviceID, 128); put_a(key, d.DeviceKey, 128);
        d.StateFlags = state;
        if (cb > sizeof d) cb = sizeof d;
        if (cb > sizeof(DWORD)) memcpy((char *)p + sizeof(DWORD), (char *)&d + sizeof(DWORD), cb - sizeof(DWORD));
    }
    return TRUE;
}
API BOOL EnumDisplayDevicesA(const char *dev, DWORD n, DISPLAY_DEVICEA *dd, DWORD f) { (void)f; return enum_devices(dev, n, dd, 0); }
API BOOL EnumDisplayDevicesW(const WCHAR *dev, DWORD n, DISPLAY_DEVICEW *dd, DWORD f)
{
    (void)f;
    char *d = lsiw_w2u(dev);
    BOOL r = enum_devices(d, n, dd, 1);
    free(d);
    return r;
}

/* ---- DPI awareness: everything runs at 96 DPI ---- */
#define DPI_CTX_MIN (-5)
static int g_proc_dpi_ctx = -1;                 /* DPI_AWARENESS_CONTEXT_UNAWARE */
static int g_proc_dpi_set;
static __thread int t_dpi_ctx;                  /* 0 = follow the process */
static pthread_mutex_t g_dpi_lock = PTHREAD_MUTEX_INITIALIZER;

static int dpi_ctx(void *c) { intptr_t v = (intptr_t)c; return v <= -1 && v >= DPI_CTX_MIN ? (int)v : 0; }
static int ctx_awareness(int c) { return c == -2 ? 1 : (c == -3 || c == -4) ? 2 : 0; }

API UINT GetDpiForSystem(void) { return 96; }
/* LSIW's console window handle is NULL, so NULL also answers 96. */
API UINT GetDpiForWindow(HWND w) { return (!w || w == DESKTOP_HWND) ? 96 : 0; }
API UINT GetSystemDpiForProcess(HANDLE p) { (void)p; return 96; }
API BOOL IsProcessDPIAware(void) { return ctx_awareness(g_proc_dpi_ctx) != 0; }
API BOOL SetProcessDPIAware(void)
{
    pthread_mutex_lock(&g_dpi_lock);
    if (!g_proc_dpi_set) { g_proc_dpi_ctx = -2; g_proc_dpi_set = 1; }
    pthread_mutex_unlock(&g_dpi_lock);
    return TRUE;
}
/* The process awareness can be chosen once; later calls fail with ERROR_ACCESS_DENIED. */
API BOOL SetProcessDpiAwarenessContext(void *ctx)
{
    int c = dpi_ctx(ctx);
    if (!c) return set_err(ERROR_INVALID_PARAMETER);
    pthread_mutex_lock(&g_dpi_lock);
    BOOL ok = !g_proc_dpi_set;
    if (ok) { g_proc_dpi_ctx = c; g_proc_dpi_set = 1; }
    pthread_mutex_unlock(&g_dpi_lock);
    return ok ? TRUE : set_err(ERROR_ACCESS_DENIED);
}
API void *GetThreadDpiAwarenessContext(void) { return (void *)(intptr_t)(t_dpi_ctx ? t_dpi_ctx : g_proc_dpi_ctx); }
API void *SetThreadDpiAwarenessContext(void *ctx)
{
    int c = dpi_ctx(ctx);
    if (!c) { set_err(ERROR_INVALID_PARAMETER); return NULL; }
    void *old = GetThreadDpiAwarenessContext();
    t_dpi_ctx = c;
    return old;
}
API int GetAwarenessFromDpiAwarenessContext(void *ctx) { int c = dpi_ctx(ctx); return c ? ctx_awareness(c) : -1; }
API BOOL AreDpiAwarenessContextsEqual(void *a, void *b) { int x = dpi_ctx(a), y = dpi_ctx(b); return x && x == y; }
API BOOL IsValidDpiAwarenessContext(void *ctx) { return dpi_ctx(ctx) != 0; }

/* Non-client size for a window style: outer edge, sizing border (with the
 * padded border), single border, caption, menu bar and client edge, using
 * the Windows 10 metrics scaled to the DPI. */
#define WS_BORDER     0x00800000u
#define WS_DLGFRAME   0x00400000u
#define WS_CAPTION    0x00C00000u
#define WS_THICKFRAME 0x00040000u
#define WS_EX_DLGMODALFRAME 0x00000001u
#define WS_EX_TOOLWINDOW    0x00000080u
#define WS_EX_CLIENTEDGE    0x00000200u
#define WS_EX_STATICEDGE    0x00020000u
static BOOL adjust_rect(RECT *r, DWORD style, BOOL menu, DWORD ex, UINT dpi)
{
    if (!r) return set_err(ERROR_INVALID_PARAMETER);
    int a;
    if ((ex & (WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME)) == WS_EX_STATICEDGE) a = 1;
    else a = ((ex & WS_EX_DLGMODALFRAME) || (style & (WS_THICKFRAME | WS_DLGFRAME))) ? 2 : 0;
    if (style & WS_THICKFRAME) a += mul_div(1, dpi) + mul_div(4, dpi);   /* border width + padded border */
    if ((style & (WS_BORDER | WS_DLGFRAME)) || (ex & WS_EX_DLGMODALFRAME)) a++;
    r->left -= a; r->top -= a; r->right += a; r->bottom += a;
    if ((style & WS_CAPTION) == WS_CAPTION) r->top -= mul_div(22, dpi) + 1;   /* caption (same height for tool windows on Windows 10) */
    (void)WS_EX_TOOLWINDOW;
    if (menu) r->top -= mul_div(20, dpi);
    if (ex & WS_EX_CLIENTEDGE) { r->left -= 2; r->top -= 2; r->right += 2; r->bottom += 2; }
    return TRUE;
}
API BOOL AdjustWindowRect(RECT *r, DWORD style, BOOL menu) { return adjust_rect(r, style, menu, 0, 96); }
API BOOL AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex) { return adjust_rect(r, style, menu, ex, 96); }
API BOOL AdjustWindowRectExForDpi(RECT *r, DWORD style, BOOL menu, DWORD ex, UINT dpi) { return adjust_rect(r, style, menu, ex, dpi ? dpi : 96); }

/* ================================================================== */
/* Icons, cursors, images                                               */
/* ================================================================== */

/* No GDI here: icons, cursors and images are tokens that keep programs'
 * bookkeeping working. System ones (hInstance NULL, IDI_/IDC_/OIC_/OCR_
 * IDs) always exist; a program's own must exist as a resource. */
static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static HCURSOR g_cursor = (HCURSOR)(uintptr_t)(TOK_CURSOR_SYS | 32512);   /* IDC_ARROW */
static POINT g_cursor_pos;
static int g_show_count;
static unsigned g_image_seq;

static int is_image_token(void *h)
{
    uintptr_t v = (uintptr_t)h, hi = v & 0xFFFF0000u;
    return v <= 0xFFFFFFFFu && (hi == TOK_ICON_SYS || hi == TOK_CURSOR_SYS || hi == TOK_IMAGE);
}

static void *load_image(HINSTANCE h, const void *name, int wide, UINT type, UINT flags)
{
    static const WORD rt[3] = { 2 /* RT_BITMAP */, 14 /* RT_GROUP_ICON */, 12 /* RT_GROUP_CURSOR */ };
    if (type > 2) return (void *)(uintptr_t)set_err(ERROR_INVALID_PARAMETER);
    if (flags & 0x10) {                                   /* LR_LOADFROMFILE */
        char *p = wide ? lsiw_path_w(name) : lsiw_path_a(name);
        int ok = p && access(p, R_OK) == 0;
        free(p);
        if (!ok) return (void *)(uintptr_t)set_err(ERROR_FILE_NOT_FOUND);
    } else if (!h) {
        if (!IS_INTRES(name)) return (void *)(uintptr_t)set_err(ERROR_RESOURCE_NAME_NOT_FOUND);
        UINT id = (UINT)(uintptr_t)name;
        return (void *)(uintptr_t)((type == 2 ? TOK_CURSOR_SYS : type == 1 ? TOK_ICON_SYS : TOK_IMAGE) | (id & 0xFFFF));
    } else {
        void *(WINAPI *findw)(HMODULE, const WCHAR *, const WCHAR *) = k32("FindResourceW");
        void *(WINAPI *finda)(HMODULE, const char *, const char *) = k32("FindResourceA");
        void *res = wide ? (findw ? findw(h, name, (const WCHAR *)(uintptr_t)rt[type]) : NULL)
                         : (finda ? finda(h, name, (const char *)(uintptr_t)rt[type]) : NULL);
        if (!res) return NULL;                          /* FindResource set the error */
    }
    unsigned n = __atomic_add_fetch(&g_image_seq, 1, __ATOMIC_RELAXED) & 0x3FFF;
    return (void *)(uintptr_t)(TOK_IMAGE | (n << 2) | type);
}
API HICON LoadIconA(HINSTANCE h, const char *name) { return load_image(h, name, 0, 1, 0); }
API HICON LoadIconW(HINSTANCE h, const WCHAR *name) { return load_image(h, name, 1, 1, 0); }
API HCURSOR LoadCursorA(HINSTANCE h, const char *name) { return load_image(h, name, 0, 2, 0); }
API HCURSOR LoadCursorW(HINSTANCE h, const WCHAR *name) { return load_image(h, name, 1, 2, 0); }
API HANDLE LoadImageA(HINSTANCE h, const char *name, UINT type, int cx, int cy, UINT flags) { (void)cx; (void)cy; return load_image(h, name, 0, type, flags); }
API HANDLE LoadImageW(HINSTANCE h, const WCHAR *name, UINT type, int cx, int cy, UINT flags) { (void)cx; (void)cy; return load_image(h, name, 1, type, flags); }
API BOOL DestroyIcon(HICON h) { return is_image_token(h) ? TRUE : set_err(ERROR_INVALID_CURSOR_HANDLE); }
API BOOL DestroyCursor(HCURSOR h) { return is_image_token(h) ? TRUE : set_err(ERROR_INVALID_CURSOR_HANDLE); }
/* Building an icon from raw bits needs GDI, which LSIW does not provide. */
API HICON CreateIconFromResourceEx(BYTE *bits, DWORD size, BOOL icon, DWORD ver, int cx, int cy, UINT flags)
{
    (void)bits; (void)size; (void)icon; (void)ver; (void)cx; (void)cy; (void)flags;
    set_err(ERROR_NOT_SUPPORTED);
    return NULL;
}
API HCURSOR SetCursor(HCURSOR c)
{
    pthread_mutex_lock(&g_cursor_lock);
    HCURSOR old = g_cursor;
    g_cursor = c;
    pthread_mutex_unlock(&g_cursor_lock);
    return old;
}
API BOOL GetCursorPos(POINT *pt)
{
    if (!pt) return set_err(ERROR_NOACCESS_);
    pthread_mutex_lock(&g_cursor_lock);
    *pt = g_cursor_pos;
    pthread_mutex_unlock(&g_cursor_lock);
    return TRUE;
}
/* The position is remembered (clipped to the screen) but no pointer moves. */
API BOOL SetCursorPos(int x, int y)
{
    RECT s = screen_rect();
    pthread_mutex_lock(&g_cursor_lock);
    g_cursor_pos.x = x < 0 ? 0 : x >= s.right ? s.right - 1 : x;
    g_cursor_pos.y = y < 0 ? 0 : y >= s.bottom ? s.bottom - 1 : y;
    pthread_mutex_unlock(&g_cursor_lock);
    return TRUE;
}
API int ShowCursor(BOOL show) { return __atomic_add_fetch(&g_show_count, show ? 1 : -1, __ATOMIC_SEQ_CST); }

static POINT cursor_now(void) { POINT p; GetCursorPos(&p); return p; }

/* ================================================================== */
/* Registered names (window messages and clipboard formats)             */
/* ================================================================== */

/* Like Windows, RegisterWindowMessage and RegisterClipboardFormat share one
 * case-insensitive atom table with values from 0xC000. */
#define ATOM_BASE 0xC000
#define ATOM_MAX  0x3FFF
static pthread_mutex_t g_atom_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_atoms[ATOM_MAX];
static int g_natoms;

static UINT atom_add(const char *name)
{
    if (!name || !*name || strlen(name) > 255) return set_err(ERROR_INVALID_PARAMETER);
    pthread_mutex_lock(&g_atom_lock);
    for (int i = 0; i < g_natoms; i++)
        if (!strcasecmp(g_atoms[i], name)) { pthread_mutex_unlock(&g_atom_lock); return ATOM_BASE + (UINT)i; }
    UINT r = 0;
    if (g_natoms < ATOM_MAX && (g_atoms[g_natoms] = strdup(name))) r = ATOM_BASE + (UINT)g_natoms++;
    pthread_mutex_unlock(&g_atom_lock);
    if (!r) set_err(ERROR_NOT_ENOUGH_MEMORY);
    return r;
}
static char *atom_name(UINT id)
{
    char *r = NULL;
    pthread_mutex_lock(&g_atom_lock);
    if (id >= ATOM_BASE && (int)(id - ATOM_BASE) < g_natoms) r = strdup(g_atoms[id - ATOM_BASE]);
    pthread_mutex_unlock(&g_atom_lock);
    return r;
}
static UINT atom_add_w(const WCHAR *name)
{
    if (!name) return set_err(ERROR_INVALID_PARAMETER);
    char *u = lsiw_w2u(name);
    UINT r = atom_add(u);
    free(u);
    return r;
}
API UINT RegisterWindowMessageA(const char *name) { return atom_add(name); }
API UINT RegisterWindowMessageW(const WCHAR *name) { return atom_add_w(name); }
API UINT RegisterClipboardFormatA(const char *name) { return atom_add(name); }
API UINT RegisterClipboardFormatW(const WCHAR *name) { return atom_add_w(name); }
API int GetClipboardFormatNameA(UINT fmt, char *buf, int max)
{
    char *n = atom_name(fmt);
    if (!n) return set_err(ERROR_INVALID_PARAMETER);
    int r = put_a(n, buf, max);
    free(n);
    return r;
}
API int GetClipboardFormatNameW(UINT fmt, WCHAR *buf, int max)
{
    char *n = atom_name(fmt);
    if (!n) return set_err(ERROR_INVALID_PARAMETER);
    int r = put_w(n, buf, max);
    free(n);
    return r;
}

/* ================================================================== */
/* Thread message queues and timers                                     */
/* ================================================================== */

struct qmsg { struct qmsg *next; MSG msg; };
struct qtimer { struct qtimer *next; UINT_PTR id; UINT elapse; void *proc; uint64_t due; };
struct queue {
    struct queue *next;
    DWORD tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;             /* CLOCK_MONOTONIC; signalled on every post */
    struct qmsg *head, *tail;
    int count;
    int quit, quit_code;             /* PostQuitMessage flag */
    struct qtimer *timers;
    HANDLE event;                    /* manual-reset; signalled while a posted message waits (MsgWait*) */
    int event_set;
    DWORD wake_bits;                 /* QS_ bits added since the last status check */
    DWORD last_time;
    POINT last_pt;
    BYTE keys[256];                  /* SetKeyboardState / GetKeyState */
};
static pthread_mutex_t g_queues_lock = PTHREAD_MUTEX_INITIALIZER;
static struct queue *g_queues;
static __thread struct queue *t_queue;
static UINT_PTR g_timer_seq;

static struct queue *queue_for(DWORD tid, int create)
{
    pthread_mutex_lock(&g_queues_lock);
    struct queue *q;
    for (q = g_queues; q; q = q->next) if (q->tid == tid) break;
    if (!q && create && (q = calloc(1, sizeof *q))) {
        q->tid = tid;
        pthread_mutex_init(&q->lock, NULL);
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&q->cond, &ca);
        pthread_condattr_destroy(&ca);
        q->next = g_queues;
        g_queues = q;
    }
    pthread_mutex_unlock(&g_queues_lock);
    return q;
}

/* A thread's queue is created the first time it uses a message function. */
static struct queue *my_queue(void)
{
    if (!t_queue) t_queue = queue_for(cur_tid(), 1);
    return t_queue;
}

/* Keep the MsgWait event in step with the queue. Called with q->lock held
 * (lock order: queue lock, then kernel32's wait lock inside Set/ResetEvent). */
static void queue_sync_event(struct queue *q)
{
    int want = q->head != NULL || q->quit;
    if (!q->event || want == q->event_set) return;
    BOOL (WINAPI *fn)(HANDLE) = k32(want ? "SetEvent" : "ResetEvent");
    if (fn) fn(q->event);
    q->event_set = want;
}

static BOOL queue_post(struct queue *q, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct qmsg *m = malloc(sizeof *m);
    if (!m) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    m->next = NULL;
    m->msg.hwnd = hwnd;
    m->msg.message = msg;
    m->msg.wParam = wp;
    m->msg.lParam = lp;
    m->msg.time = (DWORD)now_ms();
    m->msg.pt = cursor_now();
    pthread_mutex_lock(&q->lock);
    if (q->count >= MAX_POSTED) { pthread_mutex_unlock(&q->lock); free(m); return set_err(ERROR_NOT_ENOUGH_QUOTA); }
    if (q->tail) q->tail->next = m; else q->head = m;
    q->tail = m;
    q->count++;
    q->wake_bits |= QS_POSTMESSAGE | QS_ALLPOSTMESSAGE;
    queue_sync_event(q);
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return TRUE;
}

static int msg_in_range(UINT m, UINT lo, UINT hi) { return (!lo && !hi) || (m >= lo && m <= hi); }

#define K_POSTED 1
#define K_TIMER  2
/* Next message for GetMessage/PeekMessage, called with q->lock held. Order:
 * posted messages (WM_QUIT always passes the filter, as documented), then
 * the PostQuitMessage flag, then due timers. *next_due receives the
 * earliest pending timer when nothing is ready. */
static int queue_take(struct queue *q, MSG *out, UINT lo, UINT hi, int remove, int kinds, uint64_t *next_due)
{
    *next_due = UINT64_MAX;
    if (kinds & K_POSTED) {
        struct qmsg *prev = NULL;
        for (struct qmsg *m = q->head; m; prev = m, m = m->next) {
            if (m->msg.message != WM_QUIT && !msg_in_range(m->msg.message, lo, hi)) continue;
            *out = m->msg;
            if (remove) {
                if (prev) prev->next = m->next; else q->head = m->next;
                if (q->tail == m) q->tail = prev;
                q->count--;
                free(m);
            }
            return 1;
        }
        if (q->quit) {
            out->hwnd = NULL;
            out->message = WM_QUIT;
            out->wParam = (WPARAM)(intptr_t)q->quit_code;
            out->lParam = 0;
            out->time = (DWORD)now_ms();
            out->pt = cursor_now();
            if (remove) q->quit = 0;
            return 1;
        }
    }
    if ((kinds & K_TIMER) && msg_in_range(WM_TIMER, lo, hi)) {
        uint64_t now = now_ms();
        struct qtimer *best = NULL;
        for (struct qtimer *t = q->timers; t; t = t->next) {
            if (t->due <= now) { if (!best || t->due < best->due) best = t; }
            else if (t->due < *next_due) *next_due = t->due;
        }
        if (best) {
            out->hwnd = NULL;
            out->message = WM_TIMER;
            out->wParam = best->id;
            out->lParam = (LPARAM)(uintptr_t)best->proc;
            out->time = (DWORD)now;
            out->pt = cursor_now();
            if (remove) best->due = now + best->elapse;     /* missed periods coalesce, as on Windows */
            return 1;
        }
    }
    return 0;
}

static void queue_wait(struct queue *q, uint64_t due)
{
    if (due == UINT64_MAX) { pthread_cond_wait(&q->cond, &q->lock); return; }
    struct timespec ts = { (time_t)(due / 1000), (long)(due % 1000) * 1000000L };
    pthread_cond_timedwait(&q->cond, &q->lock, &ts);
}

/* hWnd NULL = any message of the thread; (HWND)-1 = only thread messages
 * (all of them here); anything else names a window that cannot exist. */
static int bad_filter_window(HWND w) { return w && w != (HWND)(intptr_t)-1; }

static BOOL get_message(MSG *msg, HWND hwnd, UINT lo, UINT hi)
{
    if (!msg) { set_err(ERROR_NOACCESS_); return -1; }
    if (bad_filter_window(hwnd)) { set_err(ERROR_INVALID_WINDOW_HANDLE); return -1; }
    struct queue *q = my_queue();
    if (!q) { set_err(ERROR_NOT_ENOUGH_MEMORY); return -1; }
    pthread_mutex_lock(&q->lock);
    for (;;) {
        uint64_t due;
        if (queue_take(q, msg, lo, hi, 1, K_POSTED | K_TIMER, &due)) break;
        queue_wait(q, due);
    }
    q->last_time = msg->time;
    q->last_pt = msg->pt;
    q->wake_bits = 0;
    queue_sync_event(q);
    pthread_mutex_unlock(&q->lock);
    return msg->message != WM_QUIT;
}

static BOOL peek_message(MSG *msg, HWND hwnd, UINT lo, UINT hi, UINT flags)
{
    if (!msg) return set_err(ERROR_NOACCESS_);
    if (bad_filter_window(hwnd)) return set_err(ERROR_INVALID_WINDOW_HANDLE);
    struct queue *q = my_queue();
    if (!q) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    int kinds = K_POSTED | K_TIMER;
    UINT qs = flags >> 16;                              /* PM_QS_* restrict the kinds examined */
    if (qs) kinds = ((qs & (QS_POSTMESSAGE | QS_ALLPOSTMESSAGE)) ? K_POSTED : 0) | ((qs & QS_TIMER) ? K_TIMER : 0);
    uint64_t due;
    pthread_mutex_lock(&q->lock);
    int got = queue_take(q, msg, lo, hi, flags & PM_REMOVE, kinds, &due);
    if (got && (flags & PM_REMOVE)) { q->last_time = msg->time; q->last_pt = msg->pt; }
    q->wake_bits = 0;
    queue_sync_event(q);
    pthread_mutex_unlock(&q->lock);
    return got;
}

API BOOL GetMessageA(MSG *m, HWND w, UINT lo, UINT hi) { return get_message(m, w, lo, hi); }
API BOOL GetMessageW(MSG *m, HWND w, UINT lo, UINT hi) { return get_message(m, w, lo, hi); }
API BOOL PeekMessageA(MSG *m, HWND w, UINT lo, UINT hi, UINT f) { return peek_message(m, w, lo, hi, f); }
API BOOL PeekMessageW(MSG *m, HWND w, UINT lo, UINT hi, UINT f) { return peek_message(m, w, lo, hi, f); }

/* Returns once a message or due timer is available (it is not removed). */
API BOOL WaitMessage(void)
{
    struct queue *q = my_queue();
    if (!q) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    pthread_mutex_lock(&q->lock);
    for (;;) {
        MSG m;
        uint64_t due;
        if (queue_take(q, &m, 0, 0, 0, K_POSTED | K_TIMER, &due)) break;
        queue_wait(q, due);
    }
    pthread_mutex_unlock(&q->lock);
    return TRUE;
}

static int timer_due(struct queue *q, uint64_t *next)
{
    uint64_t now = now_ms();
    int due = 0;
    *next = UINT64_MAX;
    pthread_mutex_lock(&q->lock);
    for (struct qtimer *t = q->timers; t; t = t->next) {
        if (t->due <= now) due = 1;
        else if (t->due < *next) *next = t->due;
    }
    pthread_mutex_unlock(&q->lock);
    return due;
}

/* High word: kinds of messages currently queued; low word: kinds added
 * since the last GetQueueStatus/GetMessage/PeekMessage and still queued. */
API DWORD GetQueueStatus(UINT flags)
{
    if (flags & ~QS_VALID) return set_err(ERROR_INVALID_FLAGS_);
    struct queue *q = my_queue();
    if (!q) return 0;
    uint64_t next;
    DWORD cur = timer_due(q, &next) ? QS_TIMER : 0;
    pthread_mutex_lock(&q->lock);
    if (q->head || q->quit) cur |= QS_POSTMESSAGE | QS_ALLPOSTMESSAGE;
    DWORD added = (q->wake_bits | (cur & QS_TIMER)) & cur;
    q->wake_bits = 0;
    pthread_mutex_unlock(&q->lock);
    return ((cur & flags) << 16) | (added & flags);
}
API BOOL GetInputState(void) { return FALSE; }     /* no keyboard or mouse input is ever queued */

API void PostQuitMessage(int code)
{
    struct queue *q = my_queue();
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->quit = 1;
    q->quit_code = code;
    queue_sync_event(q);
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

/* A thread of this process gets its queue on demand, so posting before the
 * target's first PeekMessage cannot lose the race Windows programs guard
 * against; other thread IDs fail with ERROR_INVALID_THREAD_ID. */
static BOOL post_thread(DWORD tid, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!tid) return set_err(ERROR_INVALID_THREAD_ID);
    struct queue *q = tid == cur_tid() ? my_queue() : queue_for(tid, 0);
    if (!q) {
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%u", (unsigned)tid);
        if (access(path, F_OK) != 0) return set_err(ERROR_INVALID_THREAD_ID);
        if (!(q = queue_for(tid, 1))) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    }
    return queue_post(q, NULL, msg, wp, lp);
}
API BOOL PostThreadMessageA(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { return post_thread(tid, msg, wp, lp); }
API BOOL PostThreadMessageW(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { return post_thread(tid, msg, wp, lp); }

/* hWnd NULL posts a thread message to the caller; a broadcast reaches no
 * top-level window and succeeds; any other window is invalid. */
static BOOL post_message(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w) {
        struct queue *q = my_queue();
        return q ? queue_post(q, NULL, msg, wp, lp) : set_err(ERROR_NOT_ENOUGH_MEMORY);
    }
    if (w == HWND_BROADCAST) return TRUE;
    return bad_window();
}
API BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return post_message(w, msg, wp, lp); }
API BOOL PostMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return post_message(w, msg, wp, lp); }

/* Sent messages need a window procedure, so only broadcasts "succeed". */
API LRESULT SendMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) { (void)msg; (void)wp; (void)lp; if (w != HWND_BROADCAST) bad_window(); return 0; }
API LRESULT SendMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { (void)msg; (void)wp; (void)lp; if (w != HWND_BROADCAST) bad_window(); return 0; }
static LRESULT send_timeout(HWND w, ULONG_PTR *result)
{
    if (result) *result = 0;
    if (w == HWND_BROADCAST) return 1;                  /* e.g. WM_SETTINGCHANGE after editing the environment */
    return bad_window();
}
API LRESULT SendMessageTimeoutA(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT ms, ULONG_PTR *res) { (void)msg; (void)wp; (void)lp; (void)f; (void)ms; return send_timeout(w, res); }
API LRESULT SendMessageTimeoutW(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT ms, ULONG_PTR *res) { (void)msg; (void)wp; (void)lp; (void)f; (void)ms; return send_timeout(w, res); }
API BOOL SendNotifyMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) { (void)msg; (void)wp; (void)lp; return w == HWND_BROADCAST ? TRUE : bad_window(); }
API BOOL SendNotifyMessageW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { (void)msg; (void)wp; (void)lp; return w == HWND_BROADCAST ? TRUE : bad_window(); }

API LONG GetMessageTime(void) { struct queue *q = my_queue(); return q ? (LONG)q->last_time : 0; }
API DWORD GetMessagePos(void)
{
    struct queue *q = my_queue();
    if (!q) return 0;
    return (DWORD)(WORD)q->last_pt.x | ((DWORD)(WORD)q->last_pt.y << 16);
}

/* With hWnd NULL a new ID is generated unless nIDEvent names one of the
 * thread's timers, which is then reset. Periods are clamped to 10 ms ..
 * 0x7FFFFFFF ms. WM_TIMER messages are produced when the queue is read;
 * their lParam is the TIMERPROC, which DispatchMessage calls. */
API UINT_PTR SetTimer(HWND w, UINT_PTR id, UINT elapse, TIMERPROC proc)
{
    if (w) return bad_window();
    struct queue *q = my_queue();
    if (!q) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    if (elapse < 10) elapse = 10;
    if (elapse > 0x7FFFFFFF) elapse = 0x7FFFFFFF;
    pthread_mutex_lock(&q->lock);
    struct qtimer *t = NULL;
    if (id) for (t = q->timers; t; t = t->next) if (t->id == id) break;
    if (!t) {
        if (!(t = calloc(1, sizeof *t))) { pthread_mutex_unlock(&q->lock); return set_err(ERROR_NOT_ENOUGH_MEMORY); }
        t->id = __atomic_add_fetch(&g_timer_seq, 1, __ATOMIC_RELAXED);
        t->next = q->timers;
        q->timers = t;
    }
    t->elapse = elapse;
    t->proc = (void *)proc;
    t->due = now_ms() + elapse;
    UINT_PTR r = t->id;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return r;
}

API BOOL KillTimer(HWND w, UINT_PTR id)
{
    if (w) return bad_window();
    struct queue *q = my_queue();
    if (!q) return FALSE;
    pthread_mutex_lock(&q->lock);
    for (struct qtimer **pp = &q->timers; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            struct qtimer *t = *pp;
            *pp = t->next;
            free(t);
            pthread_mutex_unlock(&q->lock);
            return TRUE;
        }
    }
    pthread_mutex_unlock(&q->lock);
    return FALSE;
}

/* Only a TIMERPROC that belongs to a live timer of this thread is called. */
static LRESULT dispatch(const MSG *m)
{
    if (!m) return set_err(ERROR_INVALID_PARAMETER);
    if (m->message == WM_TIMER && m->lParam) {
        struct queue *q = my_queue();
        int ok = 0;
        if (q) {
            pthread_mutex_lock(&q->lock);
            for (struct qtimer *t = q->timers; t; t = t->next)
                if (t->id == m->wParam && (LPARAM)(uintptr_t)t->proc == m->lParam) ok = 1;
            pthread_mutex_unlock(&q->lock);
        }
        if (ok) ((TIMERPROC)(uintptr_t)m->lParam)(m->hwnd, WM_TIMER, m->wParam, (DWORD)now_ms());
        return 0;
    }
    if (m->hwnd) bad_window();
    return 0;     /* thread messages have no window procedure */
}
API LRESULT DispatchMessageA(const MSG *m) { return dispatch(m); }
API LRESULT DispatchMessageW(const MSG *m) { return dispatch(m); }

static int to_unicode(UINT vk, UINT scan, const BYTE *state, WCHAR *out);

/* WM_KEYDOWN / WM_SYSKEYDOWN post WM_CHAR / WM_SYSCHAR for keys that produce
 * a character; every key message returns nonzero. */
API BOOL TranslateMessage(const MSG *m)
{
    if (!m) return FALSE;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN && m->message != WM_KEYUP && m->message != WM_SYSKEYUP) return FALSE;
    if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) {
        struct queue *q = my_queue();
        WCHAR c;
        if (q && to_unicode((UINT)m->wParam, (UINT)(m->lParam >> 16) & 0xFF, q->keys, &c))
            queue_post(q, m->hwnd, m->message == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, c, m->lParam);
    }
    return TRUE;
}

/* Waits on the handles plus the thread's queue: a manual-reset event that is
 * signalled while a posted message is waiting is appended as handle nCount.
 * Timers are checked between waits. The queue counts as ready whenever it
 * holds a message (the MWMO_INPUTAVAILABLE behaviour), so a loop can never
 * sleep on a message it peeked at but left in the queue. */
static DWORD msg_wait(DWORD n, const HANDLE *hs, DWORD ms, DWORD mask, DWORD flags)
{
    if (n > 63 || (n && !hs) || (flags & ~(MWMO_WAITALL | MWMO_ALERTABLE | MWMO_INPUTAVAILABLE)) || (mask & ~QS_VALID)) {
        set_err(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    struct queue *q = my_queue();
    DWORD (WINAPI *wfmo)(DWORD, const HANDLE *, BOOL, DWORD, BOOL) = k32("WaitForMultipleObjectsEx");
    if (!q || !wfmo) { set_err(ERROR_NOT_ENOUGH_MEMORY); return WAIT_FAILED; }
    HANDLE arr[64];
    if (n) memcpy(arr, hs, n * sizeof(HANDLE));
    DWORD total = n;
    int want_timer = (mask & QS_TIMER) != 0;
    if (mask & (QS_POSTMESSAGE | QS_ALLPOSTMESSAGE | QS_SENDMESSAGE)) {
        pthread_mutex_lock(&q->lock);
        if (!q->event) {
            HANDLE (WINAPI *ce)(void *, BOOL, BOOL, const char *) = k32("CreateEventA");
            int want = q->head != NULL || q->quit;
            if (ce && (q->event = ce(NULL, TRUE, want, NULL))) q->event_set = want;
        }
        queue_sync_event(q);
        HANDLE ev = q->event;
        pthread_mutex_unlock(&q->lock);
        if (ev) arr[total++] = ev;
    }
    uint64_t deadline = ms == INFINITE ? UINT64_MAX : now_ms() + ms;
    for (;;) {
        uint64_t next = UINT64_MAX;
        if (want_timer && timer_due(q, &next)) return WAIT_OBJECT_0 + n;
        uint64_t now = now_ms(), until = deadline < next ? deadline : next;
        DWORD slice = until == UINT64_MAX ? INFINITE : until <= now ? 0 : (until - now > 0xFFFFFFFEu ? 0xFFFFFFFEu : (DWORD)(until - now));
        DWORD r;
        if (total) r = wfmo(total, arr, (flags & MWMO_WAITALL) != 0, slice, (flags & MWMO_ALERTABLE) != 0);
        else {
            struct timespec ts = { slice == INFINITE ? 1 : (time_t)(slice / 1000), slice == INFINITE ? 0 : (long)(slice % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            r = WAIT_TIMEOUT;
        }
        if (r != WAIT_TIMEOUT) return r;    /* index n is the queue event: a message is available */
        if (now_ms() >= deadline) return want_timer && timer_due(q, &next) ? WAIT_OBJECT_0 + n : WAIT_TIMEOUT;
    }
}
API DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD mask) { return msg_wait(n, hs, ms, mask, all ? MWMO_WAITALL : 0); }
API DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD mask, DWORD flags) { return msg_wait(n, hs, ms, mask, flags); }

/* ================================================================== */
/* Keyboard (US layout, KLID 00000409)                                  */
/* ================================================================== */

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_102 0xE2

/* Set-1 scan code -> virtual key (keypad keys give their navigation VKs,
 * as the Windows layout table does; NumLock translation happens later). */
static const BYTE g_sc2vk[0x59] = {
    0, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_RSHIFT, VK_MULTIPLY, VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F1 + 1, VK_F1 + 2, VK_F1 + 3, VK_F1 + 4,
    VK_F1 + 5, VK_F1 + 6, VK_F1 + 7, VK_F1 + 8, VK_F10, VK_NUMLOCK, VK_SCROLL, VK_HOME, VK_UP, VK_PRIOR, VK_SUBTRACT, VK_LEFT, VK_CLEAR, VK_RIGHT, VK_ADD, VK_END,
    VK_DOWN, VK_NEXT, VK_INSERT, VK_DELETE, VK_SNAPSHOT, 0, VK_OEM_102, VK_F11, VK_F12,
};
/* Keys that arrive with the E0 prefix. */
static const struct { BYTE sc, vk; } g_ext[] = {
    { 0x1C, VK_RETURN }, { 0x1D, VK_RCONTROL }, { 0x35, VK_DIVIDE }, { 0x37, VK_SNAPSHOT }, { 0x38, VK_RMENU },
    { 0x47, VK_HOME }, { 0x48, VK_UP }, { 0x49, VK_PRIOR }, { 0x4B, VK_LEFT }, { 0x4D, VK_RIGHT }, { 0x4F, VK_END },
    { 0x50, VK_DOWN }, { 0x51, VK_NEXT }, { 0x52, VK_INSERT }, { 0x53, VK_DELETE }, { 0x5B, VK_LWIN },
    { 0x5C, VK_RWIN }, { 0x5D, VK_APPS },
};
static const BYTE g_numpad_sc[10] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49 };

static int vk_is_extended(UINT vk)
{
    switch (vk) {
    case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: return 1;
    }
    return 0;
}

static UINT vk_to_sc(UINT vk, int ext_form)
{
    UINT sc = 0;
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: sc = 0x2A; break;
    case VK_RSHIFT: sc = 0x36; break;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: sc = 0x1D; break;
    case VK_MENU: case VK_LMENU: case VK_RMENU: sc = 0x38; break;
    case VK_DECIMAL: sc = 0x53; break;
    case VK_SNAPSHOT: return ext_form ? 0xE037 : 0x54;
    default:
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { sc = g_numpad_sc[vk - VK_NUMPAD0]; break; }
        for (size_t i = 0; i < sizeof g_ext / sizeof g_ext[0] && !sc; i++)
            if (g_ext[i].vk == vk && vk != VK_RETURN && vk != VK_SNAPSHOT) sc = g_ext[i].sc;
        for (UINT i = 1; i < sizeof g_sc2vk && !sc; i++) if (g_sc2vk[i] == vk) sc = i;
    }
    if (!sc) return 0;
    return ext_form && vk_is_extended(vk) ? 0xE000 | sc : sc;
}

/* distinguish = 0 folds left/right modifiers to VK_SHIFT/VK_CONTROL/VK_MENU. */
static UINT sc_to_vk(UINT sc, int distinguish)
{
    UINT vk = 0;
    if ((sc & 0xFF00) == 0xE000) {
        for (size_t i = 0; i < sizeof g_ext / sizeof g_ext[0]; i++) if (g_ext[i].sc == (sc & 0xFF)) vk = g_ext[i].vk;
    } else if ((sc & 0xFF00) == 0 && sc < sizeof g_sc2vk) vk = g_sc2vk[sc];
    if (!distinguish) {
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
        else if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
        else if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
    }
    return vk;
}

/* The character a key gives on the US layout, without Ctrl/Alt. */
static WCHAR vk_char(UINT vk, int shift)
{
    static const char sym_digits[] = ")!@#$%^&*(";
    static const struct { BYTE vk; char lo, hi; } oem[] = {
        { VK_OEM_1, ';', ':' }, { VK_OEM_PLUS, '=', '+' }, { VK_OEM_COMMA, ',', '<' }, { VK_OEM_MINUS, '-', '_' },
        { VK_OEM_PERIOD, '.', '>' }, { VK_OEM_2, '/', '?' }, { VK_OEM_3, '`', '~' }, { VK_OEM_4, '[', '{' },
        { VK_OEM_5, '\\', '|' }, { VK_OEM_6, ']', '}' }, { VK_OEM_7, '\'', '"' }, { VK_OEM_102, '\\', '|' },
    };
    if (vk >= 'A' && vk <= 'Z') return (WCHAR)(shift ? vk : vk + 32);
    if (vk >= '0' && vk <= '9') return (WCHAR)(shift ? sym_digits[vk - '0'] : (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (WCHAR)('0' + vk - VK_NUMPAD0);
    for (size_t i = 0; i < sizeof oem / sizeof oem[0]; i++) if (oem[i].vk == vk) return (WCHAR)(shift ? oem[i].hi : oem[i].lo);
    switch (vk) {
    case VK_SPACE: return ' ';
    case VK_RETURN: return '\r';
    case VK_TAB: return '\t';
    case VK_BACK: return '\b';
    case VK_ESCAPE: return 0x1B;
    case VK_MULTIPLY: return '*';
    case VK_ADD: return '+';
    case VK_SUBTRACT: return '-';
    case VK_DECIMAL: return '.';
    case VK_DIVIDE: return '/';
    }
    return 0;
}

/* Key + modifier state -> character (Ctrl gives the control characters). */
static int to_unicode(UINT vk, UINT scan, const BYTE *st, WCHAR *out)
{
    if (scan & 0x8000) return 0;                          /* key release */
    int shift = st && ((st[VK_SHIFT] | st[VK_LSHIFT] | st[VK_RSHIFT]) & 0x80);
    int ctrl = st && ((st[VK_CONTROL] | st[VK_LCONTROL] | st[VK_RCONTROL]) & 0x80);
    int alt = st && ((st[VK_MENU] | st[VK_LMENU] | st[VK_RMENU]) & 0x80);
    int caps = st && (st[VK_CAPITAL] & 1);
    WCHAR c;
    if (ctrl && alt) return 0;                            /* AltGr: nothing on the US layout */
    if (ctrl) {
        if (vk >= 'A' && vk <= 'Z') c = (WCHAR)(vk - 'A' + 1);
        else if (vk == VK_OEM_4) c = 0x1B;
        else if (vk == VK_OEM_5 || vk == VK_OEM_102) c = 0x1C;
        else if (vk == VK_OEM_6) c = 0x1D;
        else if (vk == '6' && shift) c = 0x1E;
        else if (vk == VK_OEM_MINUS && shift) c = 0x1F;
        else if (vk == VK_RETURN) c = '\n';
        else if (vk == VK_BACK) c = 0x7F;
        else if (vk == VK_SPACE) c = ' ';
        else if (vk == VK_ESCAPE) c = 0x1B;
        else if (vk == VK_TAB) return 0;
        else return 0;
    } else {
        int s = (vk >= 'A' && vk <= 'Z') ? shift ^ caps : shift;
        c = vk_char(vk, s);
        if (!c) return 0;
    }
    *out = c;
    return 1;
}

API SHORT GetKeyState(int vk)
{
    struct queue *q = my_queue();
    if (!q || vk < 0 || vk > 255) return 0;
    BYTE b = q->keys[vk];
    return (SHORT)(((b & 0x80) ? 0xFF80 : 0) | (b & 1));
}
API SHORT GetAsyncKeyState(int vk) { (void)vk; return 0; }       /* no physical keyboard is observed */
API BOOL GetKeyboardState(BYTE *keys)
{
    if (!keys) return set_err(ERROR_NOACCESS_);
    struct queue *q = my_queue();
    if (q) memcpy(keys, q->keys, 256); else memset(keys, 0, 256);
    return TRUE;
}
API BOOL SetKeyboardState(BYTE *keys)
{
    struct queue *q = my_queue();
    if (!keys) return set_err(ERROR_NOACCESS_);
    if (!q) return set_err(ERROR_NOT_ENOUGH_MEMORY);
    memcpy(q->keys, keys, 256);
    return TRUE;
}
API HKL GetKeyboardLayout(DWORD tid) { (void)tid; return US_HKL; }
API BOOL GetKeyboardLayoutNameA(char *buf) { if (!buf) return set_err(ERROR_NOACCESS_); memcpy(buf, "00000409", 9); return TRUE; }
API BOOL GetKeyboardLayoutNameW(WCHAR *buf) { if (!buf) return set_err(ERROR_NOACCESS_); put_w("00000409", buf, 9); return TRUE; }
API int GetKeyboardLayoutList(int n, HKL *list)
{
    if (n > 0 && list) list[0] = US_HKL;
    return 1;
}
/* HKL_PREV (0) / HKL_NEXT (1) and the US layout itself are accepted. */
API HKL ActivateKeyboardLayout(HKL hkl, UINT flags)
{
    (void)flags;
    if ((uintptr_t)hkl <= 1 || hkl == US_HKL) return US_HKL;
    set_err(ERROR_INVALID_PARAMETER);
    return NULL;
}
/* Only the US layout exists: any well-formed KLID loads it. */
static HKL load_layout(const char *klid)
{
    if (!klid || strlen(klid) != 8 || strspn(klid, "0123456789abcdefABCDEF") != 8) { set_err(ERROR_INVALID_PARAMETER); return NULL; }
    return US_HKL;
}
API HKL LoadKeyboardLayoutA(const char *klid, UINT flags) { (void)flags; return load_layout(klid); }
API HKL LoadKeyboardLayoutW(const WCHAR *klid, UINT flags)
{
    (void)flags;
    char *u = lsiw_w2u(klid);
    HKL r = load_layout(u);
    free(u);
    return r;
}
API int GetKeyboardType(int what) { return what == 0 ? 4 : what == 1 ? 0 : what == 2 ? 12 : 0; }

static UINT map_vk(UINT code, UINT type)
{
    switch (type) {
    case 0: return vk_to_sc(code, 0);                                   /* MAPVK_VK_TO_VSC */
    case 1: return sc_to_vk(code, 0);                                   /* MAPVK_VSC_TO_VK */
    case 2: { WCHAR c = vk_char(code, 0); return (c >= 'a' && c <= 'z') ? (UINT)c - 32 : c; }  /* MAPVK_VK_TO_CHAR */
    case 3: return sc_to_vk(code, 1);                                   /* MAPVK_VSC_TO_VK_EX */
    case 4: return vk_to_sc(code, 1);                                   /* MAPVK_VK_TO_VSC_EX */
    }
    return 0;
}
API UINT MapVirtualKeyA(UINT code, UINT type) { return map_vk(code, type); }
API UINT MapVirtualKeyW(UINT code, UINT type) { return map_vk(code, type); }
API UINT MapVirtualKeyExA(UINT code, UINT type, HKL hkl) { (void)hkl; return map_vk(code, type); }
API UINT MapVirtualKeyExW(UINT code, UINT type, HKL hkl) { (void)hkl; return map_vk(code, type); }

/* Low byte: virtual key; high byte: 1 = Shift, 2 = Ctrl, 4 = Alt; -1 if no key types it. */
static SHORT vk_scan(WCHAR c)
{
    static const BYTE vks[] = { VK_SPACE, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
                                VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7 };
    switch (c) {
    case '\b': return VK_BACK;
    case '\t': return VK_TAB;
    case '\r': return VK_RETURN;
    case '\n': return 0x200 | VK_RETURN;
    case 0x1B: return VK_ESCAPE;
    case 0x1C: return 0x200 | VK_OEM_5;
    case 0x1D: return 0x200 | VK_OEM_6;
    case 0x1E: return 0x300 | '6';
    case 0x1F: return 0x300 | VK_OEM_MINUS;
    case 0x7F: return 0x200 | VK_BACK;
    }
    if (c >= 1 && c <= 26) return (SHORT)(0x200 | ('A' + c - 1));
    if (c >= 'a' && c <= 'z') return (SHORT)(c - 32);
    if (c >= 'A' && c <= 'Z') return (SHORT)(0x100 | c);
    for (int s = 0; s < 2; s++) {
        for (UINT d = '0'; d <= '9'; d++) if (vk_char(d, s) == c) return (SHORT)((s << 8) | d);
        for (size_t i = 0; i < sizeof vks; i++) if (vk_char(vks[i], s) == c) return (SHORT)((s << 8) | vks[i]);
    }
    return -1;
}
API SHORT VkKeyScanA(char c) { return (unsigned char)c < 0x80 ? vk_scan((WCHAR)(unsigned char)c) : -1; }
API SHORT VkKeyScanW(WCHAR c) { return vk_scan(c); }
API SHORT VkKeyScanExA(char c, HKL hkl) { (void)hkl; return VkKeyScanA(c); }
API SHORT VkKeyScanExW(WCHAR c, HKL hkl) { (void)hkl; return vk_scan(c); }

/* Low word: scan code; high word: shift state. 0xFFFFFFFF if untypeable. */
API DWORD OemKeyScan(WORD c)
{
    SHORT r = c < 0x80 ? vk_scan(c) : -1;
    if (r == -1) return 0xFFFFFFFFu;
    UINT sc = vk_to_sc((UINT)r & 0xFF, 0);
    return sc ? ((DWORD)(((UINT)r >> 8) & 0xFF) << 16) | sc : 0xFFFFFFFFu;
}

API int ToUnicode(UINT vk, UINT sc, const BYTE *st, WCHAR *buf, int n, UINT f)
{
    (void)f;
    WCHAR c;
    if (!buf || n < 1 || !to_unicode(vk, sc, st, &c)) return 0;
    buf[0] = c;
    if (n > 1) buf[1] = 0;
    return 1;
}
API int ToUnicodeEx(UINT vk, UINT sc, const BYTE *st, WCHAR *buf, int n, UINT f, HKL hkl) { (void)hkl; return ToUnicode(vk, sc, st, buf, n, f); }
API int ToAscii(UINT vk, UINT sc, const BYTE *st, WORD *out, UINT f)
{
    (void)f;
    WCHAR c;
    if (!out || !to_unicode(vk, sc, st, &c) || c >= 0x80) return 0;
    *out = c;
    return 1;
}
API int ToAsciiEx(UINT vk, UINT sc, const BYTE *st, WORD *out, UINT f, HKL hkl) { (void)hkl; return ToAscii(vk, sc, st, out, f); }

/* lParam as in WM_KEYDOWN: bits 16-23 scan code, bit 24 extended key,
 * bit 25 "do not care" about left/right. Names are those of US Windows. */
static int key_name(LONG lp, char *out, size_t n)
{
    UINT sc = ((UINT)lp >> 16) & 0xFF;
    int ext = (lp >> 24) & 1, dontcare = (lp >> 25) & 1;
    static const char *const pad[] = { "Num 7", "Num 8", "Num 9", "Num -", "Num 4", "Num 5", "Num 6", "Num +", "Num 1", "Num 2", "Num 3", "Num 0", "Num Del" };
    static const char *const nav[] = { "Home", "Up", "Page Up", NULL, "Left", NULL, "Right", NULL, "End", "Down", "Page Down", "Insert", "Delete" };
    const char *s = NULL;
    char tmp[8];
    if (sc >= 0x47 && sc <= 0x53) s = ext ? nav[sc - 0x47] : pad[sc - 0x47];
    else switch (sc) {
    case 0x01: s = "Esc"; break;
    case 0x0E: s = "Backspace"; break;
    case 0x0F: s = "Tab"; break;
    case 0x1C: s = ext ? "Num Enter" : "Enter"; break;
    case 0x1D: s = ext && !dontcare ? "Right Ctrl" : "Ctrl"; break;
    case 0x2A: s = "Shift"; break;
    case 0x36: s = dontcare ? "Shift" : "Right Shift"; break;
    case 0x35: s = ext ? "Num /" : "/"; break;
    case 0x37: s = ext ? "Prnt Scrn" : "Num *"; break;
    case 0x38: s = ext && !dontcare ? "Right Alt" : "Alt"; break;
    case 0x39: s = "Space"; break;
    case 0x3A: s = "Caps Lock"; break;
    case 0x45: s = "Pause"; break;
    case 0x46: s = "Scroll Lock"; break;
    case 0x54: s = "Sys Req"; break;
    case 0x5B: s = ext ? "Left Windows" : NULL; break;
    case 0x5C: s = ext ? "Right Windows" : NULL; break;
    case 0x5D: s = ext ? "Application" : NULL; break;
    default: {
        UINT vk = sc_to_vk(sc, 0);
        if (vk >= VK_F1 && vk <= VK_F12) { snprintf(tmp, sizeof tmp, "F%u", vk - VK_F1 + 1); s = tmp; }
        else if (vk) {
            WCHAR c = vk_char(vk, 0);
            if (c > ' ' && c < 0x7F) { tmp[0] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c); tmp[1] = 0; s = tmp; }
        }
    }
    }
    if (sc == 0x45 && !ext) s = "Pause";
    if (sc == 0x45 && ext) s = "Num Lock";
    if (!s) return 0;
    snprintf(out, n, "%s", s);
    return 1;
}
API int GetKeyNameTextA(LONG lp, char *buf, int max) { char n[32]; return key_name(lp, n, sizeof n) ? put_a(n, buf, max) : 0; }
API int GetKeyNameTextW(LONG lp, WCHAR *buf, int max) { char n[32]; return key_name(lp, n, sizeof n) ? put_w(n, buf, max) : 0; }

/* Input injection and observation. LSIW never sees the desktop's input. */
API BOOL GetLastInputInfo(LASTINPUTINFO *li)
{
    if (!li || li->cbSize != sizeof *li) return set_err(ERROR_INVALID_PARAMETER);
    li->dwTime = (DWORD)now_ms();
    return TRUE;
}
API BOOL BlockInput(BOOL block) { (void)block; return set_err(ERROR_ACCESS_DENIED); }
API UINT SendInput(UINT n, void *inputs, int cb)
{
    (void)n; (void)inputs;
    if (cb != 40) return set_err(ERROR_INVALID_PARAMETER);    /* sizeof(INPUT) on x64 */
    return set_err(ERROR_ACCESS_DENIED);
}
API void keybd_event(BYTE vk, BYTE sc, DWORD flags, ULONG_PTR extra) { (void)vk; (void)sc; (void)flags; (void)extra; }
API void mouse_event(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra) { (void)flags; (void)dx; (void)dy; (void)data; (void)extra; }
API BOOL RegisterRawInputDevices(const void *devs, UINT n, UINT cb) { (void)devs; (void)n; (void)cb; return set_err(ERROR_INVALID_PARAMETER); }
API UINT GetRawInputDeviceList(RAWINPUTDEVICELIST *list, UINT *n, UINT cb)
{
    if (cb != sizeof(RAWINPUTDEVICELIST)) { set_err(ERROR_INVALID_PARAMETER); return (UINT)-1; }
    if (!n) { set_err(ERROR_NOACCESS_); return (UINT)-1; }
    (void)list;
    *n = 0;
    return 0;
}

/* ================================================================== */
/* Clipboard                                                            */
/* ================================================================== */

#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_OEMTEXT 7
#define CF_PALETTE 9
#define CF_UNICODETEXT 13
#define CF_ENHMETAFILE 14
#define CF_LOCALE 16
#define GMEM_MOVEABLE 0x2

struct clip_entry { UINT fmt; HANDLE h; int synth; };
static pthread_mutex_t g_clip_lock = PTHREAD_MUTEX_INITIALIZER;
static struct clip_entry g_clip[64];
static int g_nclip;
static DWORD g_clip_tid;               /* thread that has the clipboard open, 0 = closed */
static HWND g_clip_hwnd;
static DWORD g_clip_seq = 1;
static int g_clip_dirty;               /* changed while open: push to the host on close */
static char *g_host_text;              /* last text seen on / given to the Linux clipboard */
static uint64_t g_host_checked;

/* The Linux clipboard tool for this session, if any. */
enum { CLIP_NONE, CLIP_WAYLAND, CLIP_XCLIP, CLIP_XSEL };
static int clip_tool(void)
{
    if (!gui_session()) return CLIP_NONE;
    const char *w = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    if (w && *w && have_tool("wl-copy") && have_tool("wl-paste")) return CLIP_WAYLAND;
    if (x && *x && have_tool("xclip")) return CLIP_XCLIP;
    if (x && *x && have_tool("xsel")) return CLIP_XSEL;
    return CLIP_NONE;
}

static char *host_read(int tool)
{
    char *wl[] = { "wl-paste", "--no-newline", "--type", "text", NULL };
    char *xc[] = { "xclip", "-selection", "clipboard", "-o", "-target", "UTF8_STRING", NULL };
    char *xs[] = { "xsel", "--clipboard", "--output", NULL };
    char *out = NULL;
    int st = run_tool(tool == CLIP_WAYLAND ? wl : tool == CLIP_XCLIP ? xc : xs, NULL, 0, &out, NULL, 3000);
    if (st != 0) { free(out); return strdup(""); }     /* empty clipboard or no text on it */
    return out ? out : strdup("");
}

/* text NULL clears the Linux clipboard. */
static int host_write(int tool, const char *text)
{
    char *wl[] = { "wl-copy", NULL }, *wlc[] = { "wl-copy", "--clear", NULL };
    char *xc[] = { "xclip", "-selection", "clipboard", "-i", NULL };
    char *xs[] = { "xsel", "--clipboard", "--input", NULL }, *xsc[] = { "xsel", "--clipboard", "--clear", NULL };
    if (!text) {
        if (tool == CLIP_WAYLAND) return run_tool(wlc, NULL, 0, NULL, NULL, 3000) == 0;
        if (tool == CLIP_XSEL) return run_tool(xsc, NULL, 0, NULL, NULL, 3000) == 0;
        text = "";
    }
    char *const *argv = tool == CLIP_WAYLAND ? wl : tool == CLIP_XCLIP ? xc : xs;
    return run_tool(argv, text, strlen(text), NULL, NULL, 3000) == 0;
}

static HANDLE galloc(size_t n)
{
    HANDLE (WINAPI *ga)(UINT, SIZE_T) = k32("GlobalAlloc");
    return ga ? ga(GMEM_MOVEABLE, n) : NULL;
}
static void *glock(HANDLE h) { void *(WINAPI *gl)(HANDLE) = k32("GlobalLock"); return gl && h ? gl(h) : NULL; }
static void gunlock(HANDLE h) { BOOL (WINAPI *gu)(HANDLE) = k32("GlobalUnlock"); if (gu && h) gu(h); }
static size_t gsize(HANDLE h) { SIZE_T (WINAPI *gs)(HANDLE) = k32("GlobalSize"); return gs && h ? gs(h) : 0; }

/* Only HGLOBAL formats are freed by the system; GDI formats and the
 * private range are the owner's business. */
static void clip_release(struct clip_entry *e)
{
    UINT f = e->fmt;
    int gdi = f == CF_BITMAP || f == CF_PALETTE || f == CF_ENHMETAFILE || f == 0x82 || f == 0x8E || (f >= 0x200 && f <= 0x3FF);
    if (e->h && !gdi) {
        HANDLE (WINAPI *gf)(HANDLE) = k32("GlobalFree");
        if (gf) gf(e->h);
    }
}
static void clip_clear(void)
{
    for (int i = 0; i < g_nclip; i++) clip_release(&g_clip[i]);
    g_nclip = 0;
}
static void clip_drop_synth(void)
{
    int k = 0;
    for (int i = 0; i < g_nclip; i++) {
        if (g_clip[i].synth) clip_release(&g_clip[i]);
        else g_clip[k++] = g_clip[i];
    }
    g_nclip = k;
}
static struct clip_entry *clip_find(UINT fmt)
{
    for (int i = 0; i < g_nclip; i++) if (g_clip[i].fmt == fmt) return &g_clip[i];
    return NULL;
}
static int is_text_fmt(UINT f) { return f == CF_TEXT || f == CF_OEMTEXT || f == CF_UNICODETEXT; }

/* The text format the program set first (it drives synthesis). */
static struct clip_entry *clip_text_source(void)
{
    for (int i = 0; i < g_nclip; i++) if (!g_clip[i].synth && is_text_fmt(g_clip[i].fmt)) return &g_clip[i];
    return NULL;
}

/* Clipboard text as UTF-8 with Windows line ends. NULL if there is none. */
static char *clip_text_utf8(void)
{
    struct clip_entry *e = clip_text_source();
    if (!e || !e->h) return NULL;
    size_t n = gsize(e->h);
    void *p = glock(e->h);
    if (!p) return NULL;
    char *r;
    if (e->fmt == CF_UNICODETEXT) {
        size_t len = 0, max = n / sizeof(WCHAR);
        while (len < max && ((WCHAR *)p)[len]) len++;
        r = lsiw_w2u_n(p, (int)len);
    } else r = strndup(p, n);
    gunlock(e->h);
    return r;
}

static char *crlf_to_lf(const char *s)
{
    char *r = malloc(strlen(s) + 1), *d = r;
    if (!r) return NULL;
    for (; *s; s++) if (!(s[0] == '\r' && s[1] == '\n')) *d++ = *s;
    *d = 0;
    return r;
}
static char *lf_to_crlf(const char *s)
{
    size_t extra = 0;
    for (const char *p = s; *p; p++) if (*p == '\n' && (p == s || p[-1] != '\r')) extra++;
    char *r = malloc(strlen(s) + extra + 1), *d = r;
    if (!r) return NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '\n' && (p == s || p[-1] != '\r')) *d++ = '\r';
        *d++ = *p;
    }
    *d = 0;
    return r;
}

static HANDLE make_text(UINT fmt, const char *utf8)
{
    HANDLE h;
    if (fmt == CF_UNICODETEXT) {
        int need = lsiw_utf8_to_utf16(utf8, -1, NULL, 0);
        if (!(h = galloc((size_t)need * sizeof(WCHAR)))) return NULL;
        WCHAR *p = glock(h);
        if (p) lsiw_utf8_to_utf16(utf8, -1, p, need);
    } else {
        size_t n = strlen(utf8) + 1;
        if (!(h = galloc(n))) return NULL;
        char *p = glock(h);
        if (p) memcpy(p, utf8, n);
    }
    gunlock(h);
    return h;
}

/* Pick up changes made on the Linux side. Throttled so a program polling
 * IsClipboardFormatAvailable does not start a helper every few ms. */
static void clip_refresh(int force)
{
    int tool = clip_tool();
    if (tool == CLIP_NONE) return;
    uint64_t now = now_ms();
    if (!force && g_host_checked && now - g_host_checked < 200) return;
    g_host_checked = now;
    char *t = host_read(tool);
    if (!t) return;
    if (g_host_text && !strcmp(t, g_host_text)) { free(t); return; }
    int first = !g_host_text;
    free(g_host_text);
    g_host_text = t;
    if (first && !*t) return;
    clip_clear();
    if (*t) {
        char *w = lf_to_crlf(t);
        HANDLE h = w ? make_text(CF_UNICODETEXT, w) : NULL;
        free(w);
        if (h) g_clip[g_nclip++] = (struct clip_entry){ CF_UNICODETEXT, h, 0 };
    }
    g_clip_seq++;
}

static void clip_push(void)
{
    int tool = clip_tool();
    if (tool == CLIP_NONE) return;
    char *t = clip_text_utf8();
    char *lf = t ? crlf_to_lf(t) : NULL;
    free(t);
    if (host_write(tool, lf)) {
        free(g_host_text);
        g_host_text = lf ? lf : strdup("");
        g_host_checked = now_ms();
    } else free(lf);
}

/* Formats in EnumClipboardFormats order: the ones set, then those Windows
 * synthesizes from text (CF_LOCALE, CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT). */
static int clip_formats(UINT *out)
{
    int n = 0;
    for (int i = 0; i < g_nclip; i++) if (!g_clip[i].synth) out[n++] = g_clip[i].fmt;
    if (clip_text_source()) {
        static const UINT extra[] = { CF_LOCALE, CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT };
        for (size_t k = 0; k < 4; k++) {
            int have = 0;
            for (int i = 0; i < n; i++) if (out[i] == extra[k]) have = 1;
            if (!have) out[n++] = extra[k];
        }
    }
    return n;
}

static int clip_open_by_me(void) { return g_clip_tid && g_clip_tid == cur_tid(); }

API BOOL OpenClipboard(HWND owner)
{
    DWORD me = cur_tid();
    pthread_mutex_lock(&g_clip_lock);
    if (g_clip_tid && g_clip_tid != me) { pthread_mutex_unlock(&g_clip_lock); return set_err(ERROR_ACCESS_DENIED); }
    if (!g_clip_tid) { g_clip_tid = me; g_clip_dirty = 0; clip_refresh(1); }
    g_clip_hwnd = owner;
    pthread_mutex_unlock(&g_clip_lock);
    return TRUE;
}
API BOOL CloseClipboard(void)
{
    pthread_mutex_lock(&g_clip_lock);
    if (!clip_open_by_me()) { pthread_mutex_unlock(&g_clip_lock); return set_err(ERROR_CLIPBOARD_NOT_OPEN); }
    if (g_clip_dirty) clip_push();
    g_clip_tid = 0;
    g_clip_hwnd = NULL;
    g_clip_dirty = 0;
    pthread_mutex_unlock(&g_clip_lock);
    return TRUE;
}
API BOOL EmptyClipboard(void)
{
    pthread_mutex_lock(&g_clip_lock);
    if (!clip_open_by_me()) { pthread_mutex_unlock(&g_clip_lock); return set_err(ERROR_CLIPBOARD_NOT_OPEN); }
    clip_clear();
    g_clip_seq++;
    g_clip_dirty = 1;
    pthread_mutex_unlock(&g_clip_lock);
    return TRUE;
}
/* The system owns the handle from now on; NULL (delayed rendering) only
 * announces the format, since no window can be asked to render it. */
API HANDLE SetClipboardData(UINT fmt, HANDLE h)
{
    pthread_mutex_lock(&g_clip_lock);
    if (!clip_open_by_me()) { pthread_mutex_unlock(&g_clip_lock); set_err(ERROR_CLIPBOARD_NOT_OPEN); return NULL; }
    if (!fmt) { pthread_mutex_unlock(&g_clip_lock); set_err(ERROR_INVALID_PARAMETER); return NULL; }
    if (is_text_fmt(fmt) || fmt == CF_LOCALE) clip_drop_synth();
    struct clip_entry *e = clip_find(fmt);
    if (e) { if (e->h != h) clip_release(e); e->h = h; e->synth = 0; }
    else if (g_nclip < (int)(sizeof g_clip / sizeof g_clip[0])) g_clip[g_nclip++] = (struct clip_entry){ fmt, h, 0 };
    else { pthread_mutex_unlock(&g_clip_lock); set_err(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    g_clip_seq++;
    g_clip_dirty = 1;
    pthread_mutex_unlock(&g_clip_lock);
    return h;
}
API HANDLE GetClipboardData(UINT fmt)
{
    pthread_mutex_lock(&g_clip_lock);
    if (!clip_open_by_me()) { pthread_mutex_unlock(&g_clip_lock); set_err(ERROR_CLIPBOARD_NOT_OPEN); return NULL; }
    struct clip_entry *e = clip_find(fmt);
    HANDLE h = e ? e->h : NULL;
    if (!e && (is_text_fmt(fmt) || fmt == CF_LOCALE) && clip_text_source() && g_nclip < (int)(sizeof g_clip / sizeof g_clip[0])) {
        if (fmt == CF_LOCALE) {
            if ((h = galloc(sizeof(DWORD)))) { DWORD *p = glock(h); if (p) *p = 0x0409; gunlock(h); }
        } else {
            char *t = clip_text_utf8();
            h = t ? make_text(fmt, t) : NULL;
            free(t);
        }
        if (h) g_clip[g_nclip++] = (struct clip_entry){ fmt, h, 1 };
    }
    pthread_mutex_unlock(&g_clip_lock);
    return h;     /* NULL: the format is not on the clipboard */
}
API BOOL IsClipboardFormatAvailable(UINT fmt)
{
    UINT f[80];
    pthread_mutex_lock(&g_clip_lock);
    if (!g_clip_tid) clip_refresh(0);
    int n = clip_formats(f), r = 0;
    for (int i = 0; i < n; i++) if (f[i] == fmt) r = 1;
    pthread_mutex_unlock(&g_clip_lock);
    return r;
}
API int CountClipboardFormats(void)
{
    UINT f[80];
    pthread_mutex_lock(&g_clip_lock);
    if (!g_clip_tid) clip_refresh(0);
    int n = clip_formats(f);
    pthread_mutex_unlock(&g_clip_lock);
    return n;
}
API UINT EnumClipboardFormats(UINT fmt)
{
    UINT f[80], r = 0;
    pthread_mutex_lock(&g_clip_lock);
    if (!clip_open_by_me()) { pthread_mutex_unlock(&g_clip_lock); return set_err(ERROR_CLIPBOARD_NOT_OPEN); }
    int n = clip_formats(f);
    if (!fmt) r = n ? f[0] : 0;
    else for (int i = 0; i + 1 < n; i++) if (f[i] == fmt) { r = f[i + 1]; break; }
    pthread_mutex_unlock(&g_clip_lock);
    if (!r) set_err(ERROR_SUCCESS);
    return r;
}
/* First format of the list that is on the clipboard; 0 if the clipboard is
 * empty, -1 if none of them is there. */
API int GetPriorityClipboardFormat(UINT *list, int n)
{
    UINT f[80];
    pthread_mutex_lock(&g_clip_lock);
    int k = clip_formats(f), r = k ? -1 : 0;
    for (int i = 0; list && i < n && r == -1; i++)
        for (int j = 0; j < k; j++) if (f[j] == list[i]) { r = (int)list[i]; break; }
    pthread_mutex_unlock(&g_clip_lock);
    return r;
}
API HWND GetClipboardOwner(void) { return NULL; }
API HWND GetOpenClipboardWindow(void) { pthread_mutex_lock(&g_clip_lock); HWND w = g_clip_tid ? g_clip_hwnd : NULL; pthread_mutex_unlock(&g_clip_lock); return w; }
API DWORD GetClipboardSequenceNumber(void) { pthread_mutex_lock(&g_clip_lock); DWORD s = g_clip_seq; pthread_mutex_unlock(&g_clip_lock); return s; }

/* ================================================================== */
/* Window stations, desktops, session                                   */
/* ================================================================== */

/* The interactive window station "WinSta0" with its desktop "Default". */
API HWINSTA GetProcessWindowStation(void) { return TOK_WINSTA; }
API HDESK GetThreadDesktop(DWORD tid) { if (!tid) { set_err(ERROR_INVALID_PARAMETER); return NULL; } return TOK_DESKTOP; }
static HDESK open_desktop(const char *name)
{
    if (name && !strcasecmp(name, "Default")) return TOK_DESKTOP;
    if (name && (!strcasecmp(name, "Winlogon") || !strcasecmp(name, "Disconnect"))) { set_err(ERROR_ACCESS_DENIED); return NULL; }
    set_err(ERROR_FILE_NOT_FOUND);
    return NULL;
}
API HDESK OpenDesktopA(const char *name, DWORD f, BOOL inherit, DWORD access) { (void)f; (void)inherit; (void)access; return open_desktop(name); }
API HDESK OpenDesktopW(const WCHAR *name, DWORD f, BOOL inherit, DWORD access)
{
    (void)f; (void)inherit; (void)access;
    char *u = lsiw_w2u(name);
    HDESK d = open_desktop(u);
    free(u);
    return d;
}
API HDESK OpenInputDesktop(DWORD f, BOOL inherit, DWORD access) { (void)f; (void)inherit; (void)access; return TOK_DESKTOP; }
API BOOL CloseDesktop(HDESK d) { return d == TOK_DESKTOP ? TRUE : set_err(ERROR_INVALID_HANDLE); }
API BOOL CloseWindowStation(HWINSTA w) { return w == TOK_WINSTA ? TRUE : set_err(ERROR_INVALID_HANDLE); }
API BOOL SwitchDesktop(HDESK d) { (void)d; return set_err(ERROR_ACCESS_DENIED); }

static BOOL put_bytes(const void *src, DWORD size, void *pv, DWORD len, DWORD *needed)
{
    if (needed) *needed = size;
    if (!pv || len < size) return set_err(ERROR_INSUFFICIENT_BUFFER);
    memcpy(pv, src, size);
    return TRUE;
}
static BOOL user_object_info(HANDLE h, int idx, void *pv, DWORD len, DWORD *needed, int wide)
{
    int winsta = h == TOK_WINSTA;
    if (!winsta && h != TOK_DESKTOP) return set_err(ERROR_INVALID_HANDLE);
    const char *str = NULL;
    switch (idx) {
    case 1: {                                           /* UOI_FLAGS: visible, i.e. interactive */
        USEROBJECTFLAGS f = { FALSE, FALSE, 1 };
        return put_bytes(&f, sizeof f, pv, len, needed);
    }
    case 2: str = winsta ? "WinSta0" : "Default"; break;            /* UOI_NAME */
    case 3: str = winsta ? "WindowStation" : "Desktop"; break;      /* UOI_TYPE */
    case 4: if (needed) *needed = 0; return TRUE;                   /* UOI_USER_SID: none associated */
    case 5: { ULONG kb = 20480; if (winsta) return set_err(ERROR_INVALID_PARAMETER); return put_bytes(&kb, sizeof kb, pv, len, needed); }  /* UOI_HEAPSIZE */
    case 6: { BOOL io = TRUE; if (winsta) return set_err(ERROR_INVALID_PARAMETER); return put_bytes(&io, sizeof io, pv, len, needed); }   /* UOI_IO */
    default: return set_err(ERROR_INVALID_PARAMETER);
    }
    if (wide) {
        WCHAR w[32];
        int n = put_w(str, w, 32);
        return put_bytes(w, (DWORD)(n + 1) * sizeof(WCHAR), pv, len, needed);
    }
    return put_bytes(str, (DWORD)strlen(str) + 1, pv, len, needed);
}
API BOOL GetUserObjectInformationA(HANDLE h, int idx, void *pv, DWORD len, DWORD *needed) { return user_object_info(h, idx, pv, len, needed, 0); }
API BOOL GetUserObjectInformationW(HANDLE h, int idx, void *pv, DWORD len, DWORD *needed) { return user_object_info(h, idx, pv, len, needed, 1); }
static BOOL set_user_object_info(HANDLE h, int idx, void *pv, DWORD len)
{
    if (h != TOK_WINSTA && h != TOK_DESKTOP) return set_err(ERROR_INVALID_HANDLE);
    if (idx != 1 || !pv || len < sizeof(USEROBJECTFLAGS)) return set_err(ERROR_INVALID_PARAMETER);
    return TRUE;
}
API BOOL SetUserObjectInformationA(HANDLE h, int idx, void *pv, DWORD len) { return set_user_object_info(h, idx, pv, len); }
API BOOL SetUserObjectInformationW(HANDLE h, int idx, void *pv, DWORD len) { return set_user_object_info(h, idx, pv, len); }

API DWORD GetGuiResources(HANDLE p, DWORD flags) { (void)p; (void)flags; return 0; }
/* Shutting down or logging off needs SE_SHUTDOWN_NAME, which LSIW never grants. */
API BOOL ExitWindowsEx(UINT flags, DWORD reason) { (void)flags; (void)reason; return set_err(ERROR_PRIVILEGE_NOT_HELD_); }
/* Locks the desktop session through logind, as Windows locks the workstation. */
API BOOL LockWorkStation(void)
{
    if (!gui_session() || !have_tool("loginctl")) return set_err(ERROR_NOT_SUPPORTED);
    char *argv[] = { "loginctl", "lock-session", NULL };
    return run_tool(argv, NULL, 0, NULL, NULL, 5000) == 0 ? TRUE : set_err(ERROR_ACCESS_DENIED);
}
API BOOL MessageBeep(UINT type)
{
    (void)type;
    if (isatty(2)) { ssize_t r = write(2, "\a", 1); (void)r; }
    return TRUE;
}
API BOOL AllowSetForegroundWindow(DWORD pid) { (void)pid; return TRUE; }
API BOOL LockSetForegroundWindow(UINT code) { (void)code; return TRUE; }
/* No process LSIW starts has a GUI message loop to wait for. */
API DWORD WaitForInputIdle(HANDLE p, DWORD ms)
{
    (void)ms;
    if (p != (HANDLE)(intptr_t)-1 && !lsiw_handle_get(p, H_PROCESS)) { set_err(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return 0;
}
API BOOL AttachThreadInput(DWORD a, DWORD b, BOOL attach) { (void)a; (void)b; (void)attach; return set_err(ERROR_INVALID_PARAMETER); }
/* There is no foreground window and no GUI thread state to report. */
API BOOL GetGUIThreadInfo(DWORD tid, void *info) { (void)tid; (void)info; return set_err(ERROR_INVALID_PARAMETER); }
API BOOL IsWow64Message(void) { return FALSE; }
API BOOL UserHandleGrantAccess(HANDLE h, HANDLE job, BOOL grant) { (void)h; (void)job; (void)grant; return TRUE; }
API BOOL ChangeWindowMessageFilter(UINT msg, DWORD flag) { (void)msg; (void)flag; return TRUE; }
API BOOL ChangeWindowMessageFilterEx(HWND w, UINT msg, DWORD action, void *info) { (void)w; (void)msg; (void)action; (void)info; return bad_window(); }

/* Notification registrations succeed; no power, device or accessibility
 * events are ever delivered. */
static unsigned g_notify_seq;
static void *notify_token(void) { return (void *)(uintptr_t)(TOK_NOTIFY | ((__atomic_add_fetch(&g_notify_seq, 1, __ATOMIC_RELAXED) & 0x3FFF) << 2)); }
static int is_notify_token(void *h) { return ((uintptr_t)h & 0xFFFF0000u) == TOK_NOTIFY && (uintptr_t)h <= 0xFFFFFFFFu; }
API void *RegisterPowerSettingNotification(HANDLE r, const void *guid, DWORD f) { (void)r; (void)f; if (!guid) { set_err(ERROR_INVALID_PARAMETER); return NULL; } return notify_token(); }
API BOOL UnregisterPowerSettingNotification(void *h) { return is_notify_token(h) ? TRUE : set_err(ERROR_INVALID_HANDLE); }
API void *RegisterDeviceNotificationA(HANDLE r, void *filter, DWORD f) { (void)r; (void)f; if (!filter) { set_err(ERROR_INVALID_DATA); return NULL; } return notify_token(); }
API void *RegisterDeviceNotificationW(HANDLE r, void *filter, DWORD f) { return RegisterDeviceNotificationA(r, filter, f); }
API BOOL UnregisterDeviceNotification(void *h) { return is_notify_token(h) ? TRUE : set_err(ERROR_INVALID_HANDLE); }
API void *SetWinEventHook(DWORD lo, DWORD hi, HMODULE m, void *proc, DWORD pid, DWORD tid, DWORD f)
{
    (void)m; (void)pid; (void)tid; (void)f;
    if (!proc || lo > hi) { set_err(ERROR_INVALID_PARAMETER); return NULL; }
    return notify_token();
}
API BOOL UnhookWinEvent(void *h) { return is_notify_token(h) ? TRUE : set_err(ERROR_INVALID_HANDLE); }
/* Windows hooks would need the desktop's input and windows. */
API HHOOK SetWindowsHookExA(int id, void *proc, HINSTANCE m, DWORD tid) { (void)id; (void)proc; (void)m; (void)tid; set_err(ERROR_ACCESS_DENIED); return NULL; }
API HHOOK SetWindowsHookExW(int id, void *proc, HINSTANCE m, DWORD tid) { (void)id; (void)proc; (void)m; (void)tid; set_err(ERROR_ACCESS_DENIED); return NULL; }
API BOOL UnhookWindowsHookEx(HHOOK h) { (void)h; return set_err(ERROR_INVALID_HOOK_HANDLE); }
API LRESULT CallNextHookEx(HHOOK h, int code, WPARAM wp, LPARAM lp) { (void)h; (void)code; (void)wp; (void)lp; return 0; }

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_user32_exports[] = {
    /* message boxes */
    E(MessageBoxA), E(MessageBoxW), E(MessageBoxExA), E(MessageBoxExW), E(MessageBoxIndirectA), E(MessageBoxIndirectW),
    E(MessageBoxTimeoutA), E(MessageBoxTimeoutW),
    /* formatting and characters */
    E(wsprintfA), E(wsprintfW), E(wvsprintfA), E(wvsprintfW),
    E(CharUpperA), E(CharUpperW), E(CharLowerA), E(CharLowerW), E(CharUpperBuffA), E(CharUpperBuffW),
    E(CharLowerBuffA), E(CharLowerBuffW), E(CharNextA), E(CharNextW), E(CharPrevA), E(CharPrevW),
    E(CharNextExA), E(CharPrevExA), E(CharToOemA), E(CharToOemW), E(OemToCharA), E(OemToCharW),
    E(CharToOemBuffA), E(CharToOemBuffW), E(OemToCharBuffA), E(OemToCharBuffW),
    E(IsCharAlphaA), E(IsCharAlphaW), E(IsCharAlphaNumericA), E(IsCharAlphaNumericW),
    E(IsCharUpperA), E(IsCharUpperW), E(IsCharLowerA), E(IsCharLowerW),
    /* resources, icons, cursors */
    E(LoadStringA), E(LoadStringW), E(LoadIconA), E(LoadIconW), E(LoadCursorA), E(LoadCursorW),
    E(LoadImageA), E(LoadImageW), E(DestroyIcon), E(DestroyCursor), E(CreateIconFromResourceEx),
    E(SetCursor), E(GetCursorPos), E(SetCursorPos), E(ShowCursor),
    /* metrics, colours, parameters */
    E(GetSystemMetrics), E(GetSystemMetricsForDpi), E(GetSysColor), E(GetSysColorBrush),
    E(GetDoubleClickTime), E(GetCaretBlinkTime), E(SystemParametersInfoA), E(SystemParametersInfoW),
    /* windows, monitors, display */
    E(GetDesktopWindow), E(GetForegroundWindow), E(GetActiveWindow), E(GetShellWindow), E(IsWindow),
    E(IsWindowVisible), E(GetWindowThreadProcessId), E(GetWindowRect), E(GetClientRect),
    E(FindWindowA), E(FindWindowW), E(FindWindowExA), E(FindWindowExW), E(EnumWindows), E(EnumDesktopWindows),
    E(ShowWindow), E(GetWindowTextA), E(GetWindowTextW), E(GetWindowTextLengthA), E(GetWindowTextLengthW),
    E(SetWindowTextA), E(SetWindowTextW), E(SetForegroundWindow), E(FlashWindow), E(FlashWindowEx),
    E(GetSystemMenu), E(GetClassNameA), E(GetClassNameW), E(RealGetWindowClassA), E(RealGetWindowClassW),
    E(MonitorFromWindow), E(MonitorFromPoint), E(MonitorFromRect), E(GetMonitorInfoA), E(GetMonitorInfoW),
    E(EnumDisplayMonitors), E(EnumDisplaySettingsA), E(EnumDisplaySettingsW), E(EnumDisplaySettingsExA),
    E(EnumDisplaySettingsExW), E(EnumDisplayDevicesA), E(EnumDisplayDevicesW),
    E(GetDpiForSystem), E(GetDpiForWindow), E(GetSystemDpiForProcess), E(SetProcessDPIAware), E(IsProcessDPIAware),
    E(SetProcessDpiAwarenessContext), E(SetThreadDpiAwarenessContext), E(GetThreadDpiAwarenessContext),
    E(GetAwarenessFromDpiAwarenessContext), E(AreDpiAwarenessContextsEqual), E(IsValidDpiAwarenessContext),
    E(AdjustWindowRect), E(AdjustWindowRectEx), E(AdjustWindowRectExForDpi),
    /* message queue */
    E(PostThreadMessageA), E(PostThreadMessageW), E(PostMessageA), E(PostMessageW), E(PostQuitMessage),
    E(GetMessageA), E(GetMessageW), E(PeekMessageA), E(PeekMessageW), E(WaitMessage), E(GetQueueStatus),
    E(GetInputState), E(TranslateMessage), E(DispatchMessageA), E(DispatchMessageW), E(SendMessageA),
    E(SendMessageW), E(SendMessageTimeoutA), E(SendMessageTimeoutW), E(SendNotifyMessageA), E(SendNotifyMessageW),
    E(MsgWaitForMultipleObjects), E(MsgWaitForMultipleObjectsEx), E(SetTimer), E(KillTimer),
    E(RegisterWindowMessageA), E(RegisterWindowMessageW), E(GetMessageTime), E(GetMessagePos),
    /* keyboard and input */
    E(GetKeyState), E(GetAsyncKeyState), E(GetKeyboardState), E(SetKeyboardState), E(GetKeyboardLayout),
    E(GetKeyboardLayoutNameA), E(GetKeyboardLayoutNameW), E(GetKeyboardLayoutList), E(ActivateKeyboardLayout),
    E(LoadKeyboardLayoutA), E(LoadKeyboardLayoutW), E(GetKeyboardType), E(MapVirtualKeyA), E(MapVirtualKeyW),
    E(MapVirtualKeyExA), E(MapVirtualKeyExW), E(VkKeyScanA), E(VkKeyScanW), E(VkKeyScanExA), E(VkKeyScanExW),
    E(OemKeyScan), E(ToAscii), E(ToAsciiEx), E(ToUnicode), E(ToUnicodeEx), E(GetKeyNameTextA), E(GetKeyNameTextW),
    E(GetLastInputInfo), E(BlockInput), E(SendInput), E(keybd_event), E(mouse_event),
    E(RegisterRawInputDevices), E(GetRawInputDeviceList),
    /* clipboard */
    E(OpenClipboard), E(CloseClipboard), E(EmptyClipboard), E(GetClipboardData), E(SetClipboardData),
    E(IsClipboardFormatAvailable), E(CountClipboardFormats), E(EnumClipboardFormats), E(GetPriorityClipboardFormat),
    E(GetClipboardOwner), E(GetOpenClipboardWindow), E(GetClipboardSequenceNumber),
    E(RegisterClipboardFormatA), E(RegisterClipboardFormatW), E(GetClipboardFormatNameA), E(GetClipboardFormatNameW),
    /* window stations, desktops, session */
    E(GetProcessWindowStation), E(GetThreadDesktop), E(OpenDesktopA), E(OpenDesktopW), E(OpenInputDesktop),
    E(CloseDesktop), E(CloseWindowStation), E(SwitchDesktop), E(GetUserObjectInformationA), E(GetUserObjectInformationW),
    E(SetUserObjectInformationA), E(SetUserObjectInformationW), E(GetGuiResources), E(ExitWindowsEx),
    E(LockWorkStation), E(MessageBeep), E(AllowSetForegroundWindow), E(LockSetForegroundWindow),
    E(WaitForInputIdle), E(AttachThreadInput), E(GetGUIThreadInfo), E(IsWow64Message), E(UserHandleGrantAccess),
    E(ChangeWindowMessageFilter), E(ChangeWindowMessageFilterEx),
    E(RegisterPowerSettingNotification), E(UnregisterPowerSettingNotification),
    E(RegisterDeviceNotificationA), E(RegisterDeviceNotificationW), E(UnregisterDeviceNotification),
    E(SetWinEventHook), E(UnhookWinEvent), E(SetWindowsHookExA), E(SetWindowsHookExW),
    E(UnhookWindowsHookEx), E(CallNextHookEx),
    { NULL, NULL }
};
