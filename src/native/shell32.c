/* shell32, shlwapi and api-ms-win-core-path: special folders (CSIDL and
 * KNOWNFOLDERID mapped onto LSIW variables), PIDLs, CommandLineToArgvW,
 * ShellExecute, SHFileOperation, SHGetFileInfo, the recycle bin (the
 * freedesktop Trash), Path* / PathCch* / Str* helpers, registry shortcuts,
 * IMalloc and IStream objects.
 *
 * Every path handled here is a Windows path (C:\..., backslashes). The W
 * functions hold the logic; A functions convert UTF-8 <-> UTF-16 around them.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ------------------------------------------------------------------ */
/* Windows types and constants used by this module                      */
/* ------------------------------------------------------------------ */

typedef int32_t HRESULT;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HKEY;
typedef void *HICON;
typedef DWORD COLORREF;
typedef LONG LSTATUS;
typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;
typedef GUID IID;

#define S_OK            ((HRESULT)0)
#define S_FALSE         ((HRESULT)1)
#define E_NOTIMPL       ((HRESULT)0x80004001u)
#define E_NOINTERFACE   ((HRESULT)0x80004002u)
#define E_POINTER       ((HRESULT)0x80004003u)
#define E_FAIL          ((HRESULT)0x80004005u)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000Eu)
#define E_INVALIDARG    ((HRESULT)0x80070057u)
#define STRSAFE_E_INSUFFICIENT_BUFFER ((HRESULT)0x8007007Au)
#define STG_E_INVALIDFUNCTION ((HRESULT)0x80030001u)
#define STG_E_ACCESSDENIED    ((HRESULT)0x80030005u)
#define STG_E_INVALIDPOINTER  ((HRESULT)0x80030009u)
#define HRESULT_FROM_WIN32(x) ((HRESULT)((x) == 0 ? 0u : (((uint32_t)(x) & 0xFFFFu) | 0x80070000u)))

#define ERROR_NO_ASSOCIATION 1155
#define ERROR_CANCELLED      1223

#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_HIDDEN    0x02
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE   0x20
#define FILE_ATTRIBUTE_NORMAL    0x80
#define INVALID_FILE_ATTRIBUTES  0xFFFFFFFFu

#define PATHCCH_MAX_CCH 0x8000

static const WCHAR g_empty_w[1];

/* ------------------------------------------------------------------ */
/* Other builtin DLLs, looked up at run time                            */
/* ------------------------------------------------------------------ */

static void *k32(const char *n) { return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), n); }
static void *adv(const char *n) { return lsiw_builtin_lookup(lsiw_builtin_find("advapi32.dll"), n); }

static BOOL k32_close(HANDLE h)
{
    BOOL (WINAPI *f)(HANDLE) = k32("CloseHandle");
    return f ? f(h) : FALSE;
}

/* ------------------------------------------------------------------ */
/* UTF-16 string helpers                                                */
/* ------------------------------------------------------------------ */

#define wlen lsiw_wcslen

/* Simple case mapping: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic.
 * That covers what file names and identifiers use in practice. */
static WCHAR wlower(WCHAR c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (WCHAR)(c + 32);
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        int odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (odd_upper) return (c & 1) ? (WCHAR)(c + 1) : c;
        return (c & 1) ? c : (WCHAR)(c + 1);
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return (WCHAR)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (WCHAR)(c + 32);
    if (c >= 0x400 && c <= 0x40F) return (WCHAR)(c + 80);
    return c;
}

static WCHAR wupper(WCHAR c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        int odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (odd_upper) return (c & 1) ? c : (WCHAR)(c - 1);
        return (c & 1) ? (WCHAR)(c - 1) : c;
    }
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return (WCHAR)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (WCHAR)(c - 80);
    return c;
}

static int wncmpi(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        WCHAR x = wlower(a[i]), y = wlower(b[i]);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static int wcmpi(const WCHAR *a, const WCHAR *b) { return wncmpi(a, b, (size_t)-1); }

static WCHAR *wchr(const WCHAR *s, WCHAR c)
{
    for (; *s; s++) if (*s == c) return (WCHAR *)s;
    return c ? NULL : (WCHAR *)s;
}

static WCHAR *wdup(const WCHAR *s)
{
    size_t n = wlen(s) + 1;
    WCHAR *d = malloc(n * sizeof(WCHAR));
    if (d) memcpy(d, s, n * sizeof(WCHAR));
    return d;
}

/* Copy with truncation into a buffer of `cap` characters (cap > 0). */
static void wcopyn(WCHAR *d, const WCHAR *s, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static WCHAR *heap_wdup(const WCHAR *s)
{
    size_t n = wlen(s) + 1;
    WCHAR *d = lsiw_heap_alloc(n * sizeof(WCHAR), 0);
    if (d) memcpy(d, s, n * sizeof(WCHAR));
    return d;
}

static WCHAR *heap_wdup_u(const char *s)
{
    WCHAR *w = lsiw_u2w(s);
    WCHAR *r = w ? heap_wdup(w) : NULL;
    free(w);
    return r;
}

/* Copy a UTF-8 string into a caller's wide buffer; FALSE if it does not fit. */
static BOOL u2w_into(const char *s, WCHAR *out, size_t cap)
{
    int need = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    if (!out || need < 0 || (size_t)need > cap) { if (out && cap) out[0] = 0; return FALSE; }
    lsiw_utf8_to_utf16(s, -1, out, (int)cap);
    return TRUE;
}

/* Copy a wide string into a caller's UTF-8 buffer; FALSE if it does not fit. */
static BOOL w2a_into(const WCHAR *w, char *out, size_t cap)
{
    int need = lsiw_utf16_to_utf8(w, -1, NULL, 0);
    if (!out || need < 0 || (size_t)need > cap) { if (out && cap) out[0] = 0; return FALSE; }
    lsiw_utf16_to_utf8(w, -1, out, (int)cap);
    return TRUE;
}

/* Byte offset in a UTF-8 string of the UTF-16 index `wi`, decoding exactly
 * as strings.c does so that A functions can return pointers into their own
 * argument after running the W implementation. */
static size_t a_offset(const char *a, size_t wi)
{
    const unsigned char *p = (const unsigned char *)a, *end = p + strlen(a);
    size_t units = 0;
    while (p < end && units < wi) {
        uint32_t c = *p++;
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else { units++; continue; }
        int bad = 0;
        for (int i = 0; i < extra; i++) {
            if (p >= end || (*p & 0xC0) != 0x80) { bad = 1; break; }
            c = (c << 6) | (*p++ & 0x3F);
        }
        units += (!bad && c >= 0x10000) ? 2 : 1;
    }
    return (size_t)(p - (const unsigned char *)a);
}

/* Map a pointer into the wide copy back to the ANSI original. */
static char *a_ptr(const char *a, const WCHAR *w, const WCHAR *wp)
{
    if (!wp) return NULL;
    return (char *)a + a_offset(a, (size_t)(wp - w));
}

/* Wide working copy of an ANSI in/out path buffer with room for MAX_PATH. */
static WCHAR *a2w_path(const char *a)
{
    size_t need = a ? (size_t)lsiw_utf8_to_utf16(a, -1, NULL, 0) : 1;
    size_t cap = need > MAX_PATH ? need : MAX_PATH;
    WCHAR *w = calloc(cap + 1, sizeof(WCHAR));
    if (w && a) lsiw_utf8_to_utf16(a, -1, w, (int)cap);
    return w;
}

static HRESULT hr_from_errno(int e) { return HRESULT_FROM_WIN32(lsiw_errno_to_win(e)); }

/* ------------------------------------------------------------------ */
/* Windows path parsing shared by Path*, PathCch* and the shell code    */
/* ------------------------------------------------------------------ */

static int is_sep(WCHAR c) { return c == '\\' || c == '/'; }
static int is_drive(const WCHAR *p) { return ((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':'; }

/* Length of the root of `p`: "C:\" 3, "C:" 2, "\" 1, "\\server\share\" ..,
 * "\\?\C:\" 7, "\\?\UNC\server\share\" ..; 0 for a relative path. */
static size_t root_len(const WCHAR *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') {
        const WCHAR *q = p + 4;
        if (is_drive(q)) return 4 + (is_sep(q[2]) ? 3 : 2);
        if (!wncmpi(q, u"UNC\\", 4)) {
            q += 4;
            while (*q && !is_sep(*q)) q++;
            if (*q) q++;
            while (*q && !is_sep(*q)) q++;
            if (*q) q++;
            return (size_t)(q - p);
        }
        return 4;
    }
    if (is_sep(p[0]) && is_sep(p[1])) {
        const WCHAR *q = p + 2;
        while (*q && !is_sep(*q)) q++;
        if (*q) q++;
        while (*q && !is_sep(*q)) q++;
        if (*q) q++;
        return (size_t)(q - p);
    }
    if (is_drive(p)) return is_sep(p[2]) ? 3 : 2;
    if (is_sep(p[0])) return 1;
    return 0;
}

/* "C:\..." or "\\..." */
static int fully_qualified(const WCHAR *p)
{
    return p && ((is_drive(p) && is_sep(p[2])) || (is_sep(p[0]) && is_sep(p[1])));
}

/* Remove "." and ".." segments. Keeps the root; ".." never climbs above it.
 * Returns the length written, or (size_t)-1 if `cap` is too small. */
static size_t canon_w(const WCHAR *src, WCHAR *out, size_t cap)
{
    size_t n = wlen(src);
    WCHAR *buf = malloc((n + 4) * sizeof(WCHAR));
    if (!buf) return (size_t)-1;
    size_t r = root_len(src), o = 0;
    for (size_t i = 0; i < r; i++) buf[o++] = src[i] == '/' ? '\\' : src[i];
    size_t base = o;
    const WCHAR *p = src + r;
    while (*p) {
        const WCHAR *s = p;
        while (*p && !is_sep(*p)) p++;
        size_t sl = (size_t)(p - s);
        int has_sep = *p != 0;
        if (has_sep) p++;
        if (sl == 1 && s[0] == '.') continue;
        if (sl == 2 && s[0] == '.' && s[1] == '.') {
            /* drop the previous segment (and its separator) */
            if (o > base) {
                if (buf[o - 1] == '\\') o--;
                while (o > base && buf[o - 1] != '\\') o--;
            }
            continue;
        }
        memcpy(buf + o, s, sl * sizeof(WCHAR));
        o += sl;
        if (has_sep) buf[o++] = '\\';
    }
    /* "C:\a\.." leaves "C:\a\" -> "C:\"; a trailing "\." leaves "a\" -> "a" */
    if (o > base && buf[o - 1] == '\\' && n && !is_sep(src[n - 1])) o--;
    if (o == 0) buf[o++] = '\\';         /* Windows turns "" into "\" */
    buf[o] = 0;
    if (o + 1 > cap) { free(buf); return (size_t)-1; }
    memcpy(out, buf, (o + 1) * sizeof(WCHAR));
    free(buf);
    return o;
}

/* Case-insensitive wildcard match ('*' and '?'), as FindFirstFile and
 * PathMatchSpec use it. "*.*" also matches names without a dot. */
static int wild_match(const WCHAR *pat, const WCHAR *s)
{
    if (!wcmpi(pat, u"*.*")) return 1;
    const WCHAR *star = NULL, *ss = NULL;
    while (*s) {
        if (*pat == '?' || (*pat && *pat != '*' && wlower(*pat) == wlower(*s))) { pat++; s++; continue; }
        if (*pat == '*') { star = pat++; ss = s; continue; }
        if (star) { pat = star + 1; s = ++ss; continue; }
        return 0;
    }
    while (*pat == '*' || (pat[0] == '.' && pat[1] == '*' && !pat[2])) pat += (*pat == '*') ? 1 : 2;
    return *pat == 0;
}

static int has_wild(const WCHAR *s) { for (; *s; s++) if (*s == '*' || *s == '?') return 1; return 0; }

/* Current Windows directory (UTF-8, malloc'd). */
static char *cwd_win(void)
{
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof buf)) return strdup("C:\\");
    return lsiw_linux_to_win(buf);
}

/* Full Windows path (UTF-8) of `p` relative to `dir` (or the current dir). */
static char *qualify_u(const char *p, const char *dir)
{
    WCHAR *w = lsiw_u2w(p);
    char *r;
    if (fully_qualified(w) || (w[0] == '\\' && w[1] != '\\')) {
        r = (w[0] == '\\' && w[1] != '\\') ? lsiw_strdup_printf("C:%s", p) : strdup(p);
    } else if (is_drive(w)) {
        r = lsiw_strdup_printf("C:\\%s", p + 2);
    } else {
        char *base = dir && *dir ? strdup(dir) : cwd_win();
        size_t bl = strlen(base);
        r = lsiw_strdup_printf("%s%s%s", base, bl && base[bl - 1] == '\\' ? "" : "\\", p);
        free(base);
    }
    free(w);
    WCHAR *rw = lsiw_u2w(r);
    WCHAR *c = malloc((wlen(rw) + 4) * sizeof(WCHAR));
    if (c && canon_w(rw, c, wlen(rw) + 4) != (size_t)-1) { free(r); r = lsiw_w2u(c); }
    free(c); free(rw);
    return r;
}

/* ================================================================== */
/* Special folders: CSIDL                                               */
/* ================================================================== */

#define CSIDL_FLAG_CREATE      0x8000
#define CSIDL_FLAG_DONT_VERIFY 0x4000
#define CSIDL_FLAG_MASK        0xFF00
#define SHGFP_TYPE_DEFAULT     1

/* For the "Default User" token ((HANDLE)-1) user folders come from the
 * CSIDL_DEFAULT_* variables. */
static const struct lsiw_var *default_user_var(const struct lsiw_var *v)
{
    static const struct { const char *user, *def; } map[] = {
        { "CSIDL_PROFILE", "DEFAULTUSERPROFILE" },
        { "CSIDL_DESKTOPDIRECTORY", "CSIDL_DEFAULT_DESKTOP" },
        { "CSIDL_PERSONAL", "CSIDL_DEFAULT_PERSONAL" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(v->name, map[i].user)) return lsiw_var_find(map[i].def);
    if (!strncasecmp(v->name, "CSIDL_", 6)) {
        char name[96];
        snprintf(name, sizeof name, "CSIDL_DEFAULT_%s", v->name + 6);
        const struct lsiw_var *d = lsiw_var_find(name);
        if (d) return d;
    }
    return v;
}

/* Resolve a CSIDL to its Windows path (UTF-8, malloc'd in *out). Creates or
 * verifies the folder according to the CSIDL flags. */
static HRESULT csidl_path(int csidl, HANDLE token, char **out)
{
    *out = NULL;
    const struct lsiw_var *v = lsiw_var_by_csidl(csidl & ~CSIDL_FLAG_MASK);
    if (!v) return E_INVALIDARG;
    if (token == (HANDLE)(intptr_t)-1 && v->scope == LSIW_SCOPE_USER) v = default_user_var(v);
    const struct lsiw_var *r = lsiw_var_resolve(v);
    if (!r) return E_INVALIDARG;
    /* Recycle Bin, This PC, Control Panel... have no file-system path. */
    if (r->kind == LSIW_VAR_VIRTUAL || r->kind == LSIW_VAR_VALUE) return E_FAIL;
    const char *win = lsiw_var_windows(r->name), *lin = lsiw_var_linux(r->name);
    if (!win || !lin) return E_FAIL;
    if (csidl & CSIDL_FLAG_CREATE) {
        if (lsiw_mkdirs(lin, 0755) < 0) return hr_from_errno(errno);
    } else if (!(csidl & CSIDL_FLAG_DONT_VERIFY)) {
        struct stat st;
        if (stat(lin, &st) < 0 || !S_ISDIR(st.st_mode)) {
            *out = strdup(win);              /* Windows fills the buffer anyway */
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
    }
    *out = strdup(win);
    return S_OK;
}

/* SHGFP_TYPE_DEFAULT: LSIW cannot evaluate a variable's default while it is
 * overridden without disturbing the shared variable cache, so the current
 * location is returned for both types. */
API HRESULT SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD type, WCHAR *path)
{
    (void)hwnd; (void)type;
    if (!path) return E_INVALIDARG;
    path[0] = 0;
    char *p;
    HRESULT hr = csidl_path(csidl, token, &p);
    if (p && !u2w_into(p, path, MAX_PATH)) hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    free(p);
    return hr;
}

API HRESULT SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD type, char *path)
{
    (void)hwnd; (void)type;
    if (!path) return E_INVALIDARG;
    path[0] = 0;
    char *p;
    HRESULT hr = csidl_path(csidl, token, &p);
    if (p && strlen(p) >= MAX_PATH) hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    else if (p) strcpy(path, p);
    free(p);
    return hr;
}

static HRESULT folder_and_subdir(int csidl, HANDLE token, const char *sub, char **out)
{
    *out = NULL;
    char *base;
    HRESULT hr = csidl_path(csidl | CSIDL_FLAG_DONT_VERIFY, token, &base);
    if (hr != S_OK) { free(base); return hr; }
    char *full = (sub && *sub) ? lsiw_strdup_printf("%s\\%s", base, sub) : strdup(base);
    free(base);
    char *lin = lsiw_win_to_linux(full);
    struct stat st;
    if (csidl & CSIDL_FLAG_CREATE) {
        if (lsiw_mkdirs(lin, 0755) < 0) hr = hr_from_errno(errno);
    } else if (!(csidl & CSIDL_FLAG_DONT_VERIFY) && (stat(lin, &st) < 0 || !S_ISDIR(st.st_mode))) {
        hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    }
    free(lin);
    *out = full;
    return hr;
}

API HRESULT SHGetFolderPathAndSubDirW(HWND hwnd, int csidl, HANDLE token, DWORD type, const WCHAR *sub, WCHAR *path)
{
    (void)hwnd; (void)type;
    if (!path) return E_INVALIDARG;
    path[0] = 0;
    char *s = lsiw_w2u(sub), *p;
    HRESULT hr = folder_and_subdir(csidl, token, s, &p);
    if (p && !u2w_into(p, path, MAX_PATH)) hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    free(s); free(p);
    return hr;
}

API HRESULT SHGetFolderPathAndSubDirA(HWND hwnd, int csidl, HANDLE token, DWORD type, const char *sub, char *path)
{
    (void)hwnd; (void)type;
    if (!path) return E_INVALIDARG;
    path[0] = 0;
    char *p;
    HRESULT hr = folder_and_subdir(csidl, token, sub, &p);
    if (p && strlen(p) >= MAX_PATH) hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    else if (p) strcpy(path, p);
    free(p);
    return hr;
}

API BOOL SHGetSpecialFolderPathW(HWND hwnd, WCHAR *path, int csidl, BOOL create)
{
    return SHGetFolderPathW(hwnd, csidl | (create ? CSIDL_FLAG_CREATE : 0), NULL, 0, path) == S_OK;
}

API BOOL SHGetSpecialFolderPathA(HWND hwnd, char *path, int csidl, BOOL create)
{
    return SHGetFolderPathA(hwnd, csidl | (create ? CSIDL_FLAG_CREATE : 0), NULL, 0, path) == S_OK;
}

/* SHSetFolderPath relocates a folder: the LSIW variable is changed and the
 * user's configuration saved, so every later program sees the new place. */
static HRESULT set_var_path(const struct lsiw_var *v, const char *path)
{
    if (!v) return E_INVALIDARG;
    const struct lsiw_var *r = lsiw_var_resolve(v);
    if (!r || r->kind != LSIW_VAR_FOLDER) return E_INVALIDARG;
    int rc = lsiw_var_set(r->name, (path && *path) ? path : NULL);
    if (rc == -2) return E_INVALIDARG;
    if (rc < 0) return E_FAIL;
    return lsiw_config_save() == 0 ? S_OK : E_FAIL;
}

API HRESULT SHSetFolderPathW(int csidl, HANDLE token, DWORD flags, const WCHAR *path)
{
    (void)token; (void)flags;
    char *p = lsiw_w2u(path);
    HRESULT hr = set_var_path(lsiw_var_by_csidl(csidl & ~CSIDL_FLAG_MASK), p);
    free(p);
    return hr;
}

API HRESULT SHSetFolderPathA(int csidl, HANDLE token, DWORD flags, const char *path)
{
    (void)token; (void)flags;
    return set_var_path(lsiw_var_by_csidl(csidl & ~CSIDL_FLAG_MASK), path);
}

/* ================================================================== */
/* Known folders (KNOWNFOLDERID, KnownFolders.h)                        */
/* ================================================================== */

#define KF_FLAG_CREATE      0x8000
#define KF_FLAG_DONT_VERIFY 0x4000

/* A known folder maps to a CSIDL, to an LSIW variable, or to a sub-folder
 * of a variable (folders Windows has but CSIDLs never covered). `virt`
 * marks shell namespace folders without a path. */
struct known_folder { const char *guid; int csidl; const char *var; const char *sub; int virt; };

static const struct known_folder g_known[] = {
    { "{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}", 0x10, NULL, NULL, 0 },  /* Desktop */
    { "{FDD39AD0-238F-46AF-ADB4-6C85480369C7}", 0x05, NULL, NULL, 0 },  /* Documents */
    { "{374DE290-123F-4565-9164-39C4925E467B}", -1, "CSIDL_DOWNLOADS", NULL, 0 },
    { "{4BD8D571-6D19-48D3-BE97-422220080E43}", 0x0d, NULL, NULL, 0 },  /* Music */
    { "{33E28130-4E1E-4676-835A-98395C3BC3BB}", 0x27, NULL, NULL, 0 },  /* Pictures */
    { "{18989B1D-99B5-455B-841C-AB7C74E4DDFC}", 0x0e, NULL, NULL, 0 },  /* Videos */
    { "{3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}", 0x1a, NULL, NULL, 0 },  /* RoamingAppData */
    { "{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}", 0x1c, NULL, NULL, 0 },  /* LocalAppData */
    { "{A520A1A4-1780-4FF6-BD18-167343C5AF16}", -1, "CSIDL_APPDATA_LOCALLOW", NULL, 0 },
    { "{5E6C858F-0E22-4760-9AFE-EA3317B67173}", 0x28, NULL, NULL, 0 },  /* Profile */
    { "{0762D272-C50A-4BB0-A382-697DCD729B80}", -1, "PROFILESFOLDER", NULL, 0 },  /* UserProfiles */
    { "{DFDF76A2-C82A-4D63-906A-5644AC457385}", -1, "PUBLIC", NULL, 0 },
    { "{ED4824AF-DCE4-45A8-81E2-FC7965083634}", 0x2e, NULL, NULL, 0 },  /* PublicDocuments */
    { "{C4AA340D-F20F-4863-AFEF-F87EF2E6BA25}", 0x19, NULL, NULL, 0 },  /* PublicDesktop */
    { "{3214FAB5-9757-4298-BB61-92A9DEAA44FF}", 0x35, NULL, NULL, 0 },  /* PublicMusic */
    { "{B6EBFB86-6907-413C-9AF7-4FC2ABF07CC5}", 0x36, NULL, NULL, 0 },  /* PublicPictures */
    { "{2400183A-6185-49FB-A2D8-4A392A602BA3}", 0x37, NULL, NULL, 0 },  /* PublicVideos */
    { "{3D644C9B-1FB8-4F30-9B45-F670235F79C0}", -1, "PUBLIC", "Downloads", 0 },  /* PublicDownloads */
    { "{62AB5D82-FDC1-4DC3-A9DD-070D1D495D97}", 0x23, NULL, NULL, 0 },  /* ProgramData */
    { "{905E63B6-C1BF-494E-B29C-65B732D3D21A}", 0x26, NULL, NULL, 0 },  /* ProgramFiles */
    { "{6D809377-6AF0-444B-8957-A3773F02200E}", 0x26, NULL, NULL, 0 },  /* ProgramFilesX64 */
    { "{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}", 0x2a, NULL, NULL, 0 },  /* ProgramFilesX86 */
    { "{F7F1ED05-9F6D-47A2-AAAE-29D317C6F066}", 0x2b, NULL, NULL, 0 },  /* ProgramFilesCommon */
    { "{6365D5A7-0F0D-45E5-87F6-0DA56B6A4F7D}", 0x2b, NULL, NULL, 0 },  /* ProgramFilesCommonX64 */
    { "{DE974D24-D9C6-4D3E-BF91-F4455120B917}", 0x2c, NULL, NULL, 0 },  /* ProgramFilesCommonX86 */
    { "{5CD7AEE2-2219-4A67-B85D-6C9CE15660CB}", -1, "CSIDL_LOCAL_APPDATA", "Programs", 0 },  /* UserProgramFiles */
    { "{BCBD3057-CA5C-4622-B42D-BC56DB0AE516}", -1, "CSIDL_LOCAL_APPDATA", "Programs\\Common", 0 },
    { "{F38BF404-1D43-42F2-9305-67DE0B28FC23}", 0x24, NULL, NULL, 0 },  /* Windows */
    { "{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}", 0x25, NULL, NULL, 0 },  /* System */
    { "{D65231B0-B2F1-4857-A4CE-A8E7C6EA7D27}", 0x29, NULL, NULL, 0 },  /* SystemX86 */
    { "{FD228CB7-AE11-4AE3-864C-16F3910AB8FE}", 0x14, NULL, NULL, 0 },  /* Fonts */
    { "{8AD10C31-2ADB-4296-A8F7-E4701232C972}", 0x38, NULL, NULL, 0 },  /* ResourceDir */
    { "{B97D20BB-F46A-4C97-BA10-5E3608430854}", 0x07, NULL, NULL, 0 },  /* Startup */
    { "{625B53C3-AB48-4EC1-BA1F-A1EF4146FC19}", 0x0b, NULL, NULL, 0 },  /* StartMenu */
    { "{A77F5D77-2E2B-44C3-A6A2-ABA601054A51}", 0x02, NULL, NULL, 0 },  /* Programs */
    { "{A63293E8-664E-48DB-A079-DF759E0509F7}", 0x15, NULL, NULL, 0 },  /* Templates */
    { "{1777F761-68AD-4D8A-87BD-30B759FA33DD}", 0x06, NULL, NULL, 0 },  /* Favorites */
    { "{AE50C081-EBD2-438A-8655-8A092E34987A}", 0x08, NULL, NULL, 0 },  /* Recent */
    { "{8983036C-27C0-404B-8F08-102D10DCFD74}", 0x09, NULL, NULL, 0 },  /* SendTo */
    { "{C5ABBF53-E17F-4121-8900-86626FC2C973}", 0x13, NULL, NULL, 0 },  /* NetHood */
    { "{9274BD8D-CFD1-41C3-B35E-B13F55A758F4}", 0x1b, NULL, NULL, 0 },  /* PrintHood */
    { "{2B0F765D-C0E9-4171-908E-08A611B84FF6}", 0x21, NULL, NULL, 0 },  /* Cookies */
    { "{D9DC8A3B-B784-432E-A781-5A1130A75963}", 0x22, NULL, NULL, 0 },  /* History */
    { "{352481E8-33BE-4251-BA85-6007CAEDCF9D}", 0x20, NULL, NULL, 0 },  /* InternetCache */
    { "{9E52AB10-F80D-49DF-ACB8-4330F5687855}", 0x3b, NULL, NULL, 0 },  /* CDBurning */
    { "{724EF170-A42D-4FEF-9F26-B60E846FBA4F}", 0x30, NULL, NULL, 0 },  /* AdminTools */
    { "{D0384E7D-BAC3-4797-8F14-CBA229B392B5}", 0x2f, NULL, NULL, 0 },  /* CommonAdminTools */
    { "{0139D44E-6AFE-49F2-8690-3DAFCAE6FFB8}", 0x17, NULL, NULL, 0 },  /* CommonPrograms */
    { "{A4115719-D62E-491D-AA7C-E74B8BE3B067}", 0x16, NULL, NULL, 0 },  /* CommonStartMenu */
    { "{82A5EA35-D9CD-47C5-9629-E15D2F714E6E}", 0x18, NULL, NULL, 0 },  /* CommonStartup */
    { "{B94237E7-57AC-4347-9151-B08C6C32D1F7}", 0x2d, NULL, NULL, 0 },  /* CommonTemplates */
    { "{56784854-C6CB-462B-8169-88E350ACB882}", 0x43, NULL, NULL, 0 },  /* Contacts */
    { "{DE92C1C7-837F-4F69-A3BB-86E631204A23}", -1, "CSIDL_PLAYLISTS", NULL, 0 },
    { "{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}", -1, "CSIDL_SAVED_GAMES", NULL, 0 },  /* SavedGames */
    { "{BFB9D5E0-C6A9-404C-B2B2-AE6DB6AF4968}", -1, "CSIDL_PROFILE", "Links", 0 },
    { "{7D1D3A04-DEBB-4115-95CF-2F29DA2920DA}", -1, "CSIDL_PROFILE", "Searches", 0 },  /* SavedSearches */
    { "{31C0DD25-9439-4F12-BF41-7FF4EDA38722}", -1, "CSIDL_PROFILE", "3D Objects", 0 },
    { "{A52BBA46-E9E1-435F-B3D9-28DAA648C0F6}", -1, "CSIDL_PROFILE", "OneDrive", 0 },
    { "{52A4F021-7B75-48A9-9F6B-4B87A210BC8F}", -1, "CSIDL_APPDATA", "Microsoft\\Internet Explorer\\Quick Launch", 0 },
    { "{B7BEDE81-DF94-4682-A7D8-57A52620B86F}", -1, "CSIDL_MYPICTURES", "Screenshots", 0 },
    { "{AB5FB87B-7CE2-4F83-915D-550846C9537B}", -1, "CSIDL_MYPICTURES", "Camera Roll", 0 },
    { "{B7534046-3ECB-4C18-BE4E-64CD4CB7D6AC}", 0x0a, NULL, NULL, 1 },  /* RecycleBinFolder */
    { "{0AC0837C-BBF8-452A-850D-79D08E667CA7}", 0x11, NULL, NULL, 1 },  /* ComputerFolder */
    { "{D20BEEC4-5CA8-4905-AE3B-BF251EA09B53}", 0x12, NULL, NULL, 1 },  /* NetworkFolder */
    { "{82A74AEB-AEB4-465C-A014-D097EE346D63}", 0x03, NULL, NULL, 1 },  /* ControlPanelFolder */
    { "{4D9F7874-4E0C-4904-967B-40B0D20C3E4B}", 0x01, NULL, NULL, 1 },  /* InternetFolder */
    { "{76FC4E2D-D6AD-4519-A663-37BD56068185}", 0x04, NULL, NULL, 1 },  /* PrintersFolder */
    { "{6F0CD92B-2E97-45D1-88FF-B0D186B8DEDD}", 0x31, NULL, NULL, 1 },  /* ConnectionsFolder */
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)(c | 32);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Parse "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" into a GUID. */
static int parse_guid(const char *s, GUID *g)
{
    static const int pos[16] = { 1, 3, 5, 7, 10, 12, 15, 17, 20, 22, 25, 27, 29, 31, 33, 35 };
    if (strlen(s) != 38 || s[0] != '{' || s[37] != '}') return 0;
    BYTE b[16];
    for (int i = 0; i < 16; i++) {
        int h = hexval(s[pos[i]]), l = hexval(s[pos[i] + 1]);
        if (h < 0 || l < 0) return 0;
        b[i] = (BYTE)(h << 4 | l);
    }
    g->Data1 = (DWORD)b[0] << 24 | (DWORD)b[1] << 16 | (DWORD)b[2] << 8 | b[3];
    g->Data2 = (WORD)(b[4] << 8 | b[5]);
    g->Data3 = (WORD)(b[6] << 8 | b[7]);
    memcpy(g->Data4, b + 8, 8);
    return 1;
}

static int guid_eq(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof *a); }

static const struct known_folder *known_find(const GUID *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof g_known / sizeof g_known[0]; i++) {
        GUID g;
        if (parse_guid(g_known[i].guid, &g) && guid_eq(&g, id)) return &g_known[i];
    }
    return NULL;
}

/* Windows path of a known folder (UTF-8, malloc'd). */
static HRESULT known_path(const GUID *id, DWORD flags, HANDLE token, char **out)
{
    *out = NULL;
    const struct known_folder *k = known_find(id);
    if (!k) return E_INVALIDARG;
    if (k->virt) return E_FAIL;
    DWORD cflags = flags & (KF_FLAG_CREATE | KF_FLAG_DONT_VERIFY);
    if (k->csidl >= 0) return csidl_path(k->csidl | (int)cflags, token, out);
    const struct lsiw_var *v = lsiw_var_resolve(lsiw_var_find(k->var));
    if (!v) return E_FAIL;
    const char *win = lsiw_var_windows(v->name);
    if (!win) return E_FAIL;
    char *full = k->sub ? lsiw_strdup_printf("%s\\%s", win, k->sub) : strdup(win);
    char *lin = lsiw_win_to_linux(full);
    HRESULT hr = S_OK;
    struct stat st;
    if (cflags & KF_FLAG_CREATE) {
        if (lsiw_mkdirs(lin, 0755) < 0) hr = hr_from_errno(errno);
    } else if (!(cflags & KF_FLAG_DONT_VERIFY) && (stat(lin, &st) < 0 || !S_ISDIR(st.st_mode))) {
        hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    free(lin);
    if (hr == S_OK) *out = full; else free(full);
    return hr;
}

API HRESULT SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, WCHAR **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    char *p;
    HRESULT hr = known_path(id, flags, token, &p);
    if (hr == S_OK) {
        *out = heap_wdup_u(p);
        if (!*out) hr = E_OUTOFMEMORY;
    }
    free(p);
    return hr;
}

API HRESULT SHSetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, const WCHAR *path)
{
    (void)flags; (void)token;
    const struct known_folder *k = known_find(id);
    if (!k || k->virt || k->sub) return E_INVALIDARG;
    char *p = lsiw_w2u(path);
    HRESULT hr = set_var_path(k->csidl >= 0 ? lsiw_var_by_csidl(k->csidl) : lsiw_var_find(k->var), p);
    free(p);
    return hr;
}

/* ================================================================== */
/* Item ID lists                                                        */
/* ================================================================== */

/* LSIW's private PIDL: one SHITEMID { WORD cb; DWORD magic; WORD kind;
 * WORD csidl; WCHAR path[] } followed by the terminating zero cb. `kind`
 * 0 carries a file-system path, 1 a virtual folder (csidl). An empty list
 * (just the terminator) is the desktop, as on Windows. */
#define PIDL_MAGIC 0x4449504Cu    /* "LPID" */
#pragma pack(push, 1)
struct pidl_item { WORD cb; DWORD magic; WORD kind; WORD csidl; WCHAR path[1]; };
#pragma pack(pop)
#define PIDL_HDR 10

static void *pidl_make(int kind, int csidl, const char *path)
{
    WCHAR *w = lsiw_u2w(path ? path : "");
    size_t n = wlen(w) + 1;
    size_t cb = PIDL_HDR + n * sizeof(WCHAR);
    BYTE *p = lsiw_heap_alloc(cb + 2, 1);
    if (p) {
        struct pidl_item *it = (struct pidl_item *)p;
        it->cb = (WORD)cb;
        it->magic = PIDL_MAGIC;
        it->kind = (WORD)kind;
        it->csidl = (WORD)csidl;
        memcpy(p + PIDL_HDR, w, n * sizeof(WCHAR));
    }
    free(w);
    return p;
}

static UINT pidl_size(const void *pidl)
{
    if (!pidl) return 0;
    const BYTE *p = pidl;
    UINT n = 0;
    WORD cb;
    while (memcpy(&cb, p + n, 2), cb) n += cb;
    return n + 2;
}

/* Path (UTF-8, malloc'd) carried by a PIDL; NULL for virtual items. */
static char *pidl_path(const void *pidl)
{
    if (!pidl) return NULL;
    const BYTE *p = pidl;
    WORD cb;
    memcpy(&cb, p, 2);
    if (!cb) {                                   /* the desktop */
        const char *d = lsiw_var_windows("CSIDL_DESKTOPDIRECTORY");
        return d ? strdup(d) : NULL;
    }
    DWORD magic;
    WORD kind;
    memcpy(&magic, p + 2, 4);
    memcpy(&kind, p + 6, 2);
    if (magic != PIDL_MAGIC || kind != 0 || cb < PIDL_HDR + 2) return NULL;
    size_t nch = (cb - PIDL_HDR) / 2;
    WCHAR *w = malloc((nch + 1) * sizeof(WCHAR));
    memcpy(w, p + PIDL_HDR, nch * sizeof(WCHAR));
    w[nch] = 0;
    char *u = lsiw_w2u(w);
    free(w);
    return u;
}

API HRESULT SHGetFolderLocation(HWND hwnd, int csidl, HANDLE token, DWORD reserved, void **out)
{
    (void)hwnd; (void)reserved;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    int folder = csidl & ~CSIDL_FLAG_MASK;
    if (folder == 0) {                           /* CSIDL_DESKTOP: the empty list */
        *out = lsiw_heap_alloc(2, 1);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    const struct lsiw_var *v = lsiw_var_resolve(lsiw_var_by_csidl(folder));
    if (!v) return E_INVALIDARG;
    if (v->kind == LSIW_VAR_VIRTUAL) {
        *out = pidl_make(1, folder, NULL);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    char *p;
    HRESULT hr = csidl_path(csidl, token, &p);
    if (hr == S_OK) { *out = pidl_make(0, folder, p); if (!*out) hr = E_OUTOFMEMORY; }
    free(p);
    return hr;
}

API HRESULT SHGetSpecialFolderLocation(HWND hwnd, int csidl, void **out)
{
    return SHGetFolderLocation(hwnd, csidl, NULL, 0, out);
}

API HRESULT SHGetKnownFolderIDList(const GUID *id, DWORD flags, HANDLE token, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    const struct known_folder *k = known_find(id);
    if (!k) return E_INVALIDARG;
    if (k->virt) { *out = pidl_make(1, k->csidl, NULL); return *out ? S_OK : E_OUTOFMEMORY; }
    char *p;
    HRESULT hr = known_path(id, flags, token, &p);
    if (hr == S_OK) { *out = pidl_make(0, k->csidl, p); if (!*out) hr = E_OUTOFMEMORY; }
    free(p);
    return hr;
}

API BOOL SHGetPathFromIDListEx(const void *pidl, WCHAR *path, DWORD cch, int flags)
{
    (void)flags;
    if (!path || !cch) return FALSE;
    path[0] = 0;
    char *p = pidl_path(pidl);
    BOOL ok = p && u2w_into(p, path, cch);
    free(p);
    return ok;
}

API BOOL SHGetPathFromIDListW(const void *pidl, WCHAR *path) { return SHGetPathFromIDListEx(pidl, path, MAX_PATH, 0); }

API BOOL SHGetPathFromIDListA(const void *pidl, char *path)
{
    if (!path) return FALSE;
    path[0] = 0;
    char *p = pidl_path(pidl);
    BOOL ok = p && strlen(p) < MAX_PATH;
    if (ok) strcpy(path, p);
    free(p);
    return ok;
}

API void ILFree(void *pidl) { if (pidl) lsiw_heap_free(pidl); }
API UINT ILGetSize(const void *pidl) { return pidl_size(pidl); }

API void *ILClone(const void *pidl)
{
    if (!pidl) return NULL;
    UINT n = pidl_size(pidl);
    void *c = lsiw_heap_alloc(n, 0);
    if (c) memcpy(c, pidl, n);
    return c;
}

API BOOL ILIsEqual(const void *a, const void *b)
{
    if (!a || !b) return FALSE;
    UINT na = pidl_size(a), nb = pidl_size(b);
    if (na != nb) return FALSE;
    char *pa = pidl_path(a), *pb = pidl_path(b);
    BOOL r;
    if (pa && pb) r = !strcasecmp(pa, pb);
    else r = !memcmp(a, b, na);
    free(pa); free(pb);
    return r;
}

API void *ILGetNext(const void *pidl)
{
    if (!pidl) return NULL;
    WORD cb;
    memcpy(&cb, pidl, 2);
    if (!cb) return NULL;
    const BYTE *n = (const BYTE *)pidl + cb;
    memcpy(&cb, n, 2);
    return cb ? (void *)n : NULL;
}

API void *ILFindLastID(const void *pidl)
{
    if (!pidl) return NULL;
    const BYTE *p = pidl, *last = p;
    WORD cb;
    while (memcpy(&cb, p, 2), cb) { last = p; p += cb; }
    return (void *)last;
}

static void *pidl_from_win(const char *path)
{
    char *full = qualify_u(path, NULL);
    char *lin = lsiw_win_to_linux(full);
    struct stat st;
    void *r = NULL;
    if (stat(lin, &st) == 0) r = pidl_make(0, 0xFFFF, full);
    else lsiw_set_last_error(ERROR_FILE_NOT_FOUND);
    free(lin); free(full);
    return r;
}

API void *ILCreateFromPathW(const WCHAR *path)
{
    if (!path) return NULL;
    char *p = lsiw_w2u(path);
    void *r = pidl_from_win(p);
    free(p);
    return r;
}

API void *ILCreateFromPathA(const char *path) { return path ? pidl_from_win(path) : NULL; }

API HRESULT SHILCreateFromPath(const WCHAR *path, void **out, DWORD *attrs)
{
    if (attrs) *attrs = 0;
    if (!out) return E_INVALIDARG;
    *out = ILCreateFromPathW(path);
    return *out ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

API HRESULT SHParseDisplayName(const WCHAR *name, void *bindctx, void **out, DWORD sfgao_in, DWORD *sfgao_out)
{
    (void)bindctx; (void)sfgao_in;
    if (sfgao_out) *sfgao_out = 0;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!name) return E_INVALIDARG;
    *out = ILCreateFromPathW(name);
    return *out ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

/* LSIW has no shell item objects, so no object can hand out an ID list. */
API HRESULT SHGetIDListFromObject(void *unk, void **out)
{
    (void)unk;
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

/* The shell namespace (IShellFolder) is not implemented. */
API HRESULT SHGetDesktopFolder(void **out)
{
    if (out) *out = NULL;
    return E_NOTIMPL;
}

/* ================================================================== */
/* CommandLineToArgvW                                                   */
/* ================================================================== */

/* Split one argument starting at *pp into `out` (if non-NULL); returns the
 * number of characters produced. Rules: 2N backslashes + quote -> N
 * backslashes and the quote toggles quoting; 2N+1 backslashes + quote -> N
 * backslashes and a literal quote; other backslashes are literal; inside
 * quotes "" is a literal quote. */
static size_t argv_one(const WCHAR **pp, WCHAR *out)
{
    const WCHAR *p = *pp;
    size_t n = 0;
    int inq = 0;
#define PUT(c) do { if (out) out[n] = (c); n++; } while (0)
    while (*p) {
        if (!inq && (*p == ' ' || *p == '\t')) break;
        if (*p == '\\') {
            size_t bs = 0;
            while (*p == '\\') { p++; bs++; }
            if (*p == '"') {
                for (size_t i = 0; i < bs / 2; i++) PUT('\\');
                if (bs & 1) { PUT('"'); p++; }
            } else {
                for (size_t i = 0; i < bs; i++) PUT('\\');
            }
            continue;
        }
        if (*p == '"') {
            if (inq && p[1] == '"') { PUT('"'); p += 2; }
            else { inq = !inq; p++; }
            continue;
        }
        PUT(*p);
        p++;
    }
#undef PUT
    *pp = p;
    return n;
}

/* argv[0] (the program name) follows simpler rules: a leading quote runs to
 * the next quote, otherwise it ends at the first space or tab; backslashes
 * are never special. */
static size_t argv_zero(const WCHAR **pp, WCHAR *out)
{
    const WCHAR *p = *pp;
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (out) out[n] = *p; n++; p++; }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') { if (out) out[n] = *p; n++; p++; }
    }
    *pp = p;
    return n;
}

/* Pass 1 (out == NULL) counts arguments and characters, pass 2 fills. */
static void argv_split(const WCHAR *cmd, WCHAR **argv, WCHAR *chars, int *argc, size_t *nchars)
{
    const WCHAR *p = cmd;
    int c = 0;
    size_t total = 0;
    size_t n = argv_zero(&p, chars ? chars : NULL);
    if (argv) argv[0] = chars;
    if (chars) chars[n] = 0;
    total += n + 1;
    c = 1;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        WCHAR *dst = chars ? chars + total : NULL;
        n = argv_one(&p, dst);
        if (argv) argv[c] = dst;
        if (dst) dst[n] = 0;
        total += n + 1;
        c++;
    }
    if (argv) argv[c] = NULL;
    *argc = c;
    *nchars = total;
}

/* The result is one LocalAlloc-compatible block: the pointer array (with a
 * NULL after the last entry) followed by the strings, freed by LocalFree. */
API WCHAR **CommandLineToArgvW(const WCHAR *cmd, int *argc)
{
    if (!argc) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    WCHAR modname[MAX_PATH + 1];
    if (!cmd || !*cmd) {
        DWORD (WINAPI *gmf)(HMODULE, WCHAR *, DWORD) = k32("GetModuleFileNameW");
        modname[0] = 0;
        if (gmf) gmf(NULL, modname, MAX_PATH);
        /* the module name becomes argv[0] verbatim, even with spaces */
        size_t ml = wlen(modname);
        WCHAR **r = lsiw_heap_alloc(2 * sizeof(WCHAR *) + (ml + 1) * sizeof(WCHAR), 1);
        if (!r) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        WCHAR *s = (WCHAR *)(r + 2);
        memcpy(s, modname, (ml + 1) * sizeof(WCHAR));
        r[0] = s; r[1] = NULL;
        *argc = 1;
        return r;
    }
    int n; size_t chars;
    argv_split(cmd, NULL, NULL, &n, &chars);
    size_t ptrs = ((size_t)n + 1) * sizeof(WCHAR *);
    WCHAR **r = lsiw_heap_alloc(ptrs + chars * sizeof(WCHAR), 1);
    if (!r) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    argv_split(cmd, r, (WCHAR *)((BYTE *)r + ptrs), &n, &chars);
    *argc = n;
    return r;
}

/* ================================================================== */
/* Launching Linux helpers (xdg-open)                                   */
/* ================================================================== */

/* Absolute path of a Linux program on $PATH, or NULL. */
static char *linux_which(const char *prog)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path), *save = NULL, *found = NULL;
    for (char *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char *c = lsiw_strdup_printf("%s/%s", *d ? d : ".", prog);
        if (access(c, X_OK) == 0) found = c; else free(c);
    }
    free(copy);
    return found;
}

/* Start `argv` detached: a short-lived /bin/sh puts it in the background and
 * exits at once, so nothing is left to reap and we do not wait. */
static int spawn_detached(char *const argv[])
{
    size_t n = 0;
    while (argv[n]) n++;
    char **sh = calloc(n + 4, sizeof(char *));
    sh[0] = (char *)"/bin/sh";
    sh[1] = (char *)"-c";
    sh[2] = (char *)"\"$0\" \"$@\" </dev/null >/dev/null 2>&1 &";
    for (size_t i = 0; i < n; i++) sh[3 + i] = argv[i];
    pid_t pid;
    int r = posix_spawn(&pid, "/bin/sh", NULL, NULL, sh, environ);
    free(sh);
    if (r) return -1;
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Open a URL or a Linux path with the desktop's default application. */
static int xdg_open(const char *target)
{
    char *opener = linux_which("xdg-open");
    int r = -1;
    if (opener) {
        char *argv[] = { opener, (char *)target, NULL };
        r = spawn_detached(argv);
        free(opener);
        return r;
    }
    if ((opener = linux_which("gio"))) {
        char *argv[] = { opener, (char *)"open", (char *)target, NULL };
        r = spawn_detached(argv);
        free(opener);
    }
    return r;
}

/* ================================================================== */
/* ShellExecute                                                         */
/* ================================================================== */

#define SE_ERR_FNF          2
#define SE_ERR_PNF          3
#define SE_ERR_ACCESSDENIED 5
#define SE_ERR_OOM          8
#define SE_ERR_NOASSOC      31
#define SE_OK               42     /* any value above 32 means success */

#define SEE_MASK_IDLIST        0x00000004
#define SEE_MASK_NOCLOSEPROCESS 0x00000040
#define SEE_MASK_DOENVSUBST    0x00000200

typedef struct {
    DWORD cbSize; ULONG fMask; HWND hwnd;
    const WCHAR *lpVerb, *lpFile, *lpParameters, *lpDirectory;
    int nShow; HINSTANCE hInstApp;
    void *lpIDList; const WCHAR *lpClass; HKEY hkeyClass; DWORD dwHotKey;
    HANDLE hIcon; HANDLE hProcess;
} SHELLEXECUTEINFOW;

typedef struct {
    DWORD cbSize; ULONG fMask; HWND hwnd;
    const char *lpVerb, *lpFile, *lpParameters, *lpDirectory;
    int nShow; HINSTANCE hInstApp;
    void *lpIDList; const char *lpClass; HKEY hkeyClass; DWORD dwHotKey;
    HANDLE hIcon; HANDLE hProcess;
} SHELLEXECUTEINFOA;

_Static_assert(sizeof(SHELLEXECUTEINFOW) == 112, "SHELLEXECUTEINFOW layout");

struct w_startupinfo {
    DWORD cb; WCHAR *reserved, *desktop, *title;
    DWORD x, y, xs, ys, xcc, ycc, fill, flags; WORD show, cb2; BYTE *r2;
    HANDLE in, out, err;
};
struct w_procinfo { HANDLE process, thread; DWORD pid, tid; };

static const char *ext_of(const char *p)
{
    const char *b = strrchr(p, '\\'), *d = strrchr(p, '.');
    return (d && (!b || d > b)) ? d : "";
}

static int is_program_ext(const char *e)
{
    return !strcasecmp(e, ".exe") || !strcasecmp(e, ".com") || !strcasecmp(e, ".bat") ||
           !strcasecmp(e, ".cmd") || !strcasecmp(e, ".pif") || !strcasecmp(e, ".scr");
}

/* A scheme of two or more characters ("http:", "mailto:"); "C:" is a drive. */
static int url_scheme_len(const char *s)
{
    if (!isalpha((unsigned char)s[0])) return 0;
    int i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    return (s[i] == ':' && i >= 2) ? i : 0;
}

static char *expand_env_u(const char *s)
{
    DWORD (WINAPI *f)(const WCHAR *, WCHAR *, DWORD) = k32("ExpandEnvironmentStringsW");
    if (!f || !s) return s ? strdup(s) : NULL;
    WCHAR *w = lsiw_u2w(s);
    DWORD n = f(w, NULL, 0);
    WCHAR *o = calloc(n + 1, sizeof(WCHAR));
    f(w, o, n + 1);
    char *r = lsiw_w2u(o);
    free(w); free(o);
    return r;
}

/* Find the file ShellExecute or FindExecutable refers to: relative to the
 * directory, then with the program extensions, then on PATH. Returns the
 * Windows path (malloc'd) or NULL with *err set. */
static char *resolve_target(const char *file, const char *dir, int *err)
{
    static const char *exts[] = { "", ".exe", ".com", ".bat", ".cmd", NULL };
    struct stat st;
    char *full = qualify_u(file, dir);
    for (int i = 0; exts[i]; i++) {
        if (i && *ext_of(full)) break;
        char *c = lsiw_strdup_printf("%s%s", full, exts[i]);
        char *lin = lsiw_win_to_linux(c);
        int ok = stat(lin, &st) == 0;
        free(lin);
        if (ok) { free(full); return c; }
        free(c);
    }
    if (!strchr(file, '\\') && !strchr(file, '/') && !strchr(file, ':')) {
        const char *path = lsiw_env_get("PATH");
        char *copy = strdup(path ? path : ""), *save = NULL;
        for (char *d = strtok_r(copy, ";", &save); d; d = strtok_r(NULL, ";", &save)) {
            for (int i = 0; exts[i]; i++) {
                if (i && *ext_of(file)) break;
                char *c = lsiw_strdup_printf("%s\\%s%s", d, file, exts[i]);
                char *lin = lsiw_win_to_linux(c);
                int ok = stat(lin, &st) == 0 && S_ISREG(st.st_mode);
                free(lin);
                if (ok) { free(copy); free(full); return c; }
                free(c);
            }
        }
        free(copy);
    }
    /* Missing file or missing folder? */
    char *parent = strdup(full), *bs = strrchr(parent, '\\');
    if (bs) *bs = 0;
    char *plin = lsiw_win_to_linux(parent);
    *err = (stat(plin, &st) == 0) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
    free(plin); free(parent); free(full);
    return NULL;
}

static int se_code(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND: return SE_ERR_FNF;
    case ERROR_PATH_NOT_FOUND: return SE_ERR_PNF;
    case ERROR_ACCESS_DENIED: return SE_ERR_ACCESSDENIED;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return SE_ERR_OOM;
    case ERROR_NO_ASSOCIATION: return SE_ERR_NOASSOC;
    default: return SE_ERR_FNF;
    }
}

/* Run a Windows program through kernel32's CreateProcessW. */
static DWORD run_program(const char *path, const char *params, const char *dir, int show, HANDLE *hproc)
{
    BOOL (WINAPI *cp)(const WCHAR *, WCHAR *, void *, void *, BOOL, DWORD, void *, const WCHAR *,
                      struct w_startupinfo *, struct w_procinfo *) = k32("CreateProcessW");
    if (!cp) return ERROR_CALL_NOT_IMPLEMENTED;
    const char *e = ext_of(path);
    char *cmd;
    if (!strcasecmp(e, ".bat") || !strcasecmp(e, ".cmd")) {
        /* Batch files run in the command interpreter: %ComSpec% /c ""file" args" */
        const char *comspec = lsiw_env_get("ComSpec");
        if (!comspec) comspec = lsiw_var_windows("COMSPEC");
        cmd = lsiw_strdup_printf("\"%s\" /c \"\"%s\"%s%s\"", comspec ? comspec : "C:\\windows\\system32\\cmd.exe",
                                 path, params && *params ? " " : "", params ? params : "");
    } else {
        cmd = lsiw_strdup_printf("\"%s\"%s%s", path, params && *params ? " " : "", params ? params : "");
    }
    WCHAR *wcmd = lsiw_u2w(cmd), *wdir = NULL;
    if (dir && *dir) {
        char *q = qualify_u(dir, NULL);
        wdir = lsiw_u2w(q);
        free(q);
    }
    struct w_startupinfo si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.flags = 1;                     /* STARTF_USESHOWWINDOW */
    si.show = (WORD)show;
    struct w_procinfo pi;
    memset(&pi, 0, sizeof pi);
    void (WINAPI *sle)(DWORD) = k32("SetLastError");
    if (sle) sle(0);
    BOOL ok = cp(NULL, wcmd, NULL, NULL, FALSE, 0x400 /* CREATE_UNICODE_ENVIRONMENT */, NULL, wdir, &si, &pi);
    DWORD err = ok ? 0 : lsiw_get_last_error();
    free(cmd); free(wcmd); free(wdir);
    if (!ok) return err ? err : ERROR_FILE_NOT_FOUND;
    if (pi.thread) k32_close(pi.thread);
    if (hproc) *hproc = pi.process;
    else if (pi.process) k32_close(pi.process);
    return 0;
}

API HRESULT PathCreateFromUrlW(const WCHAR *url, WCHAR *path, DWORD *pcch, DWORD reserved);

/* Shared ShellExecute logic on UTF-8 arguments. Returns 0 or a Win32 error;
 * *hproc receives the process handle when one is created and wanted. */
static DWORD shell_execute(const char *verb, const char *file, const char *params, const char *dir,
                           int show, HANDLE *hproc)
{
    if (hproc) *hproc = NULL;
    if (!file || !*file) return ERROR_FILE_NOT_FOUND;
    int v_open = !verb || !*verb || !strcasecmp(verb, "open") || !strcasecmp(verb, "opennew") ||
                 !strcasecmp(verb, "runas") || !strcasecmp(verb, "runasuser");
    int v_explore = verb && (!strcasecmp(verb, "explore") || !strcasecmp(verb, "find"));
    int v_edit = verb && !strcasecmp(verb, "edit");
    if (!v_open && !v_explore && !v_edit) return ERROR_NO_ASSOCIATION;   /* print, properties, unknown */

    /* URLs go to the desktop's handler; file: URLs are mapped to Linux paths. */
    if (url_scheme_len(file)) {
        if (!strncasecmp(file, "file:", 5)) {
            WCHAR *w = lsiw_u2w(file), out[4096];
            DWORD n = 4096;
            if (PathCreateFromUrlW(w, out, &n, 0) != S_OK) { free(w); return ERROR_FILE_NOT_FOUND; }
            free(w);
            char *win = lsiw_w2u(out);
            DWORD r = shell_execute(verb, win, params, dir, show, hproc);
            free(win);
            return r;
        }
        return xdg_open(file) == 0 ? 0 : ERROR_NO_ASSOCIATION;
    }

    int err = 0;
    char *path = resolve_target(file, dir, &err);
    if (!path) return (DWORD)err;
    char *lin = lsiw_win_to_linux(path);
    struct stat st;
    DWORD r;
    if (stat(lin, &st) == 0 && S_ISDIR(st.st_mode)) {
        r = (v_open || v_explore) && xdg_open(lin) == 0 ? 0 : ERROR_NO_ASSOCIATION;
    } else if (is_program_ext(ext_of(path))) {
        /* "runas" cannot elevate on Linux; the program runs as the user. */
        r = v_open ? run_program(path, params, dir, show, hproc) : ERROR_NO_ASSOCIATION;
    } else {
        r = (v_open || v_edit) && xdg_open(lin) == 0 ? 0 : ERROR_NO_ASSOCIATION;
    }
    free(lin); free(path);
    return r;
}

API HINSTANCE ShellExecuteW(HWND hwnd, const WCHAR *verb, const WCHAR *file, const WCHAR *params, const WCHAR *dir, int show)
{
    (void)hwnd;
    char *v = lsiw_w2u(verb), *f = lsiw_w2u(file), *p = lsiw_w2u(params), *d = lsiw_w2u(dir);
    DWORD e = shell_execute(v, f, p, d, show, NULL);
    free(v); free(f); free(p); free(d);
    lsiw_set_last_error(e);
    return (HINSTANCE)(intptr_t)(e ? se_code(e) : SE_OK);
}

API HINSTANCE ShellExecuteA(HWND hwnd, const char *verb, const char *file, const char *params, const char *dir, int show)
{
    (void)hwnd;
    DWORD e = shell_execute(verb, file, params, dir, show, NULL);
    lsiw_set_last_error(e);
    return (HINSTANCE)(intptr_t)(e ? se_code(e) : SE_OK);
}

static BOOL shell_execute_ex(ULONG mask, const char *verb, const char *file, const char *params,
                             const char *dir, int show, void *idlist, HINSTANCE *inst, HANDLE *proc)
{
    char *f = NULL, *d = NULL;
    if ((mask & SEE_MASK_IDLIST) && idlist && (!file || !*file)) f = pidl_path(idlist);
    else if (mask & SEE_MASK_DOENVSUBST) f = expand_env_u(file);
    else f = file ? strdup(file) : NULL;
    d = (mask & SEE_MASK_DOENVSUBST) ? expand_env_u(dir) : (dir ? strdup(dir) : NULL);
    HANDLE h = NULL;
    DWORD e = shell_execute(verb, f, params, d, show, (mask & SEE_MASK_NOCLOSEPROCESS) ? &h : NULL);
    free(f); free(d);
    *inst = (HINSTANCE)(intptr_t)(e ? se_code(e) : SE_OK);
    *proc = h;
    lsiw_set_last_error(e);
    return e == 0;
}

API BOOL ShellExecuteExW(SHELLEXECUTEINFOW *sei)
{
    if (!sei) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *v = lsiw_w2u(sei->lpVerb), *f = lsiw_w2u(sei->lpFile), *p = lsiw_w2u(sei->lpParameters), *d = lsiw_w2u(sei->lpDirectory);
    HANDLE h;
    BOOL r = shell_execute_ex(sei->fMask, v, f, p, d, sei->nShow, sei->lpIDList, &sei->hInstApp, &h);
    if (sei->fMask & SEE_MASK_NOCLOSEPROCESS) sei->hProcess = h;
    free(v); free(f); free(p); free(d);
    return r;
}

API BOOL ShellExecuteExA(SHELLEXECUTEINFOA *sei)
{
    if (!sei) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    HANDLE h;
    BOOL r = shell_execute_ex(sei->fMask, sei->lpVerb, sei->lpFile, sei->lpParameters, sei->lpDirectory,
                              sei->nShow, sei->lpIDList, &sei->hInstApp, &h);
    if (sei->fMask & SEE_MASK_NOCLOSEPROCESS) sei->hProcess = h;
    return r;
}

/* FindExecutable: a program is its own executable. Documents would need a
 * Windows program registered for the type, which LSIW does not have (the
 * Linux default application is not a Windows .exe): SE_ERR_NOASSOC. */
static int find_executable(const char *file, const char *dir, char **out)
{
    *out = NULL;
    if (!file || !*file) return SE_ERR_FNF;
    int err = 0;
    char *path = resolve_target(file, dir, &err);
    if (!path) return se_code((DWORD)err);
    if (is_program_ext(ext_of(path))) { *out = path; return SE_OK; }
    free(path);
    return SE_ERR_NOASSOC;
}

API HINSTANCE FindExecutableW(const WCHAR *file, const WCHAR *dir, WCHAR *result)
{
    char *f = lsiw_w2u(file), *d = lsiw_w2u(dir), *o;
    int r = find_executable(f, d, &o);
    if (result) { result[0] = 0; if (o && !u2w_into(o, result, MAX_PATH)) r = SE_ERR_FNF; }
    free(f); free(d); free(o);
    return (HINSTANCE)(intptr_t)r;
}

API HINSTANCE FindExecutableA(const char *file, const char *dir, char *result)
{
    char *o;
    int r = find_executable(file, dir, &o);
    if (result) { result[0] = 0; if (o) { if (strlen(o) < MAX_PATH) strcpy(result, o); else r = SE_ERR_FNF; } }
    free(o);
    return (HINSTANCE)(intptr_t)r;
}

/* ================================================================== */
/* Linux file helpers for SHFileOperation                               */
/* ================================================================== */

static int copy_file_linux(const char *src, const char *dst, const struct stat *st)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st->st_mode & 07777);
    if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        for (ssize_t off = 0; off < n; ) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += w;
        }
        if (rc) break;
    }
    int e = errno;
    close(in);
    if (close(out) < 0 && !rc) { rc = -1; e = errno; }
    if (!rc) {
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        utimensat(AT_FDCWD, dst, ts, 0);
    }
    errno = e;
    return rc;
}

/* Recursive copy; symbolic links are copied as links. */
static int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) < 0) return -1;
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX];
        ssize_t n = readlink(src, t, sizeof t - 1);
        if (n < 0) return -1;
        t[n] = 0;
        unlink(dst);
        return symlink(t, dst);
    }
    if (!S_ISDIR(st.st_mode)) return copy_file_linux(src, dst, &st);
    if (mkdir(dst, (st.st_mode & 07777) | 0700) < 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) && !rc) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *s = lsiw_strdup_printf("%s/%s", src, de->d_name), *t = lsiw_strdup_printf("%s/%s", dst, de->d_name);
        rc = copy_tree(s, t);
        free(s); free(t);
    }
    int e = errno;
    closedir(d);
    errno = e;
    return rc;
}

static int delete_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) && !rc) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *s = lsiw_strdup_printf("%s/%s", path, de->d_name);
        rc = delete_tree(s);
        free(s);
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}

static unsigned long long tree_size(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *s = lsiw_strdup_printf("%s/%s", path, de->d_name);
        total += tree_size(s);
        free(s);
    }
    closedir(d);
    return total;
}

/* ---- The Recycle Bin is the freedesktop Trash (home trash) ---- */

static char *trash_dir(const char *sub)
{
    const char *files = lsiw_var_linux("CSIDL_BITBUCKET");
    char *base;
    if (files) {
        base = strdup(files);
        size_t l = strlen(base);
        if (l > 6 && !strcmp(base + l - 6, "/files")) base[l - 6] = 0;
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        base = (xdg && *xdg == '/') ? lsiw_strdup_printf("%s/Trash", xdg)
                                    : lsiw_strdup_printf("%s/.local/share/Trash", lsiw_home_dir());
    }
    char *r = lsiw_strdup_printf("%s/%s", base, sub);
    free(base);
    return r;
}

static char *url_escape_path(const char *p)
{
    size_t n = strlen(p);
    char *o = malloc(n * 3 + 1), *w = o;
    for (const unsigned char *s = (const unsigned char *)p; *s; s++) {
        if (isalnum(*s) || strchr("/-_.~!$&'()*+,;=:@", *s)) *w++ = (char)*s;
        else w += sprintf(w, "%%%02X", *s);
    }
    *w = 0;
    return o;
}

/* Move one Linux path to the trash following the Trash specification:
 * reserve files/<name> by creating info/<name>.trashinfo exclusively, then
 * rename. Paths on another file system go through `gio trash`, else they
 * are copied in and removed. */
static int trash_path(const char *lin)
{
    char *abs = realpath(lin, NULL);
    if (!abs) return -1;
    char *files = trash_dir("files"), *info = trash_dir("info");
    lsiw_mkdirs(files, 0700);
    lsiw_mkdirs(info, 0700);
    const char *base = strrchr(abs, '/');
    base = (base && base[1]) ? base + 1 : "item";
    int rc = -1, fd = -1;
    char *name = NULL, *infofile = NULL;
    for (int i = 1; i < 10000; i++) {
        free(name); free(infofile);
        name = i == 1 ? strdup(base) : lsiw_strdup_printf("%s.%d", base, i);
        infofile = lsiw_strdup_printf("%s/%s.trashinfo", info, name);
        char *target = lsiw_strdup_printf("%s/%s", files, name);
        struct stat st;
        int taken = lstat(target, &st) == 0;
        free(target);
        if (taken) continue;
        fd = open(infofile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd >= 0) {
        char date[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
        char *esc = url_escape_path(abs);
        char *body = lsiw_strdup_printf("[Trash Info]\nPath=%s\nDeletionDate=%s\n", esc, date);
        ssize_t wr = write(fd, body, strlen(body));
        close(fd);
        free(esc); free(body);
        char *target = lsiw_strdup_printf("%s/%s", files, name);
        if (wr < 0) rc = -1;
        else if (rename(abs, target) == 0) rc = 0;
        else if (errno == EXDEV) {
            char *gio = linux_which("gio");
            if (gio) {
                char *argv[] = { gio, (char *)"trash", abs, NULL };
                pid_t pid;
                int st = 1;
                if (!posix_spawn(&pid, gio, NULL, NULL, argv, environ)) waitpid(pid, &st, 0);
                free(gio);
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { unlink(infofile); rc = 0; }
            }
            if (rc && copy_tree(abs, target) == 0 && delete_tree(abs) == 0) rc = 0;
        }
        if (rc) { int e = errno; unlink(infofile); errno = e; }
        free(target);
    }
    free(name); free(infofile); free(files); free(info); free(abs);
    return rc;
}

/* ================================================================== */
/* SHFileOperation                                                      */
/* ================================================================== */

#define FO_MOVE   1
#define FO_COPY   2
#define FO_DELETE 3
#define FO_RENAME 4
#define FOF_MULTIDESTFILES    0x0001
#define FOF_RENAMEONCOLLISION 0x0008
#define FOF_ALLOWUNDO         0x0040
#define FOF_FILESONLY         0x0080
#define FOF_NORECURSION       0x1000
#define DE_SAMEFILE           0x71
#define DE_MANYSRC1DEST       0x72
#define DE_INVALIDFILES       0x7C

typedef struct {
    HWND hwnd; UINT wFunc; const WCHAR *pFrom, *pTo; WORD fFlags;
    BOOL fAnyOperationsAborted; void *hNameMappings; const WCHAR *lpszProgressTitle;
} SHFILEOPSTRUCTW;
typedef struct {
    HWND hwnd; UINT wFunc; const char *pFrom, *pTo; WORD fFlags;
    BOOL fAnyOperationsAborted; void *hNameMappings; const char *lpszProgressTitle;
} SHFILEOPSTRUCTA;

struct strlist { char **v; size_t n; };

static void sl_add(struct strlist *l, char *s)
{
    l->v = realloc(l->v, (l->n + 1) * sizeof(char *));
    l->v[l->n++] = s;
}
static void sl_free(struct strlist *l) { for (size_t i = 0; i < l->n; i++) free(l->v[i]); free(l->v); }

/* Split a double-NUL-terminated list into qualified Windows paths (UTF-8). */
static void parse_list_w(const WCHAR *p, struct strlist *out)
{
    if (!p) return;
    for (; *p; p += wlen(p) + 1) {
        char *u = lsiw_w2u(p);
        sl_add(out, qualify_u(u, NULL));
        free(u);
    }
}

/* Expand wildcards in the last component of each source into Linux paths. */
static int expand_sources(struct strlist *win, struct strlist *lin, WORD flags, int *wild)
{
    *wild = 0;
    for (size_t i = 0; i < win->n; i++) {
        WCHAR *w = lsiw_u2w(win->v[i]);
        WCHAR *bs = NULL;
        for (WCHAR *q = w; *q; q++) if (*q == '\\') bs = q;
        WCHAR *name = bs ? bs + 1 : w;
        if (!has_wild(name)) {
            char *l = lsiw_win_to_linux(win->v[i]);
            struct stat st;
            if (lstat(l, &st) < 0) { free(l); free(w); return ERROR_FILE_NOT_FOUND; }
            sl_add(lin, l);
            free(w);
            continue;
        }
        *wild = 1;
        WCHAR save = bs ? bs[1] : 0;
        if (bs) bs[1] = 0;
        char *dirw = bs ? lsiw_w2u(w) : strdup("."), *dir = lsiw_win_to_linux(dirw);
        if (bs) bs[1] = save;
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                WCHAR *dn = lsiw_u2w(de->d_name);
                int m = wild_match(name, dn);
                free(dn);
                if (!m) continue;
                char *full = lsiw_strdup_printf("%s/%s", dir, de->d_name);
                struct stat st;
                int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
                if (isdir && (flags & (FOF_FILESONLY | FOF_NORECURSION))) { free(full); continue; }
                sl_add(lin, full);
            }
            closedir(d);
        }
        free(dirw); free(dir); free(w);
    }
    return 0;
}

static int same_file(const char *a, const char *b)
{
    struct stat x, y;
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

/* "name - Copy.ext", "name - Copy (2).ext", ... as Explorer names collisions. */
static char *collision_name(const char *dst)
{
    const char *slash = strrchr(dst, '/');
    const char *dot = strrchr(dst, '.');
    if (!dot || (slash && dot < slash) || dot == (slash ? slash + 1 : dst)) dot = dst + strlen(dst);
    for (int i = 1; i < 10000; i++) {
        char *c = i == 1 ? lsiw_strdup_printf("%.*s - Copy%s", (int)(dot - dst), dst, dot)
                         : lsiw_strdup_printf("%.*s - Copy (%d)%s", (int)(dot - dst), dst, i, dot);
        struct stat st;
        if (lstat(c, &st) < 0) return c;
        free(c);
    }
    return strdup(dst);
}

static int move_tree(const char *src, const char *dst)
{
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) return -1;
    if (copy_tree(src, dst) < 0) return -1;
    return delete_tree(src);
}

static int file_operation(UINT func, struct strlist *from, struct strlist *to, WORD flags)
{
    if (!from->n) return DE_INVALIDFILES;
    struct strlist src = { 0 };
    int wild, rc = expand_sources(from, &src, flags, &wild);
    if (rc) { sl_free(&src); return rc; }

    if (func == FO_DELETE) {
        for (size_t i = 0; i < src.n && !rc; i++) {
            if (!strcmp(src.v[i], "/") || !strcmp(src.v[i], lsiw_root_dir())) { rc = ERROR_ACCESS_DENIED; break; }
            int r = (flags & FOF_ALLOWUNDO) ? trash_path(src.v[i]) : delete_tree(src.v[i]);
            if (r < 0) rc = (int)lsiw_errno_to_win(errno);
        }
        sl_free(&src);
        return rc;
    }
    if (!to->n) { sl_free(&src); return DE_INVALIDFILES; }

    if (func == FO_RENAME) {
        if (src.n != 1 || to->n != 1) { sl_free(&src); return DE_MANYSRC1DEST; }
        char *dst = lsiw_win_to_linux(to->v[0]);
        struct stat st;
        if (lstat(dst, &st) == 0 && !same_file(src.v[0], dst)) rc = ERROR_ALREADY_EXISTS;
        else if (rename(src.v[0], dst) < 0) rc = (int)lsiw_errno_to_win(errno);
        free(dst);
        sl_free(&src);
        return rc;
    }

    /* FO_COPY / FO_MOVE */
    int multi = (flags & FOF_MULTIDESTFILES) && to->n > 1;
    char *dest0 = lsiw_win_to_linux(to->v[0]);
    struct stat st;
    int dest_is_dir = stat(dest0, &st) == 0 && S_ISDIR(st.st_mode);
    if (!multi && !dest_is_dir && (src.n > 1 || wild)) {
        /* Several items into a folder that does not exist yet: create it. */
        if (lsiw_mkdirs(dest0, 0755) < 0) rc = (int)lsiw_errno_to_win(errno);
        dest_is_dir = 1;
    }
    for (size_t i = 0; i < src.n && !rc; i++) {
        char *dst;
        if (multi) {
            if (i >= to->n) { rc = DE_MANYSRC1DEST; break; }
            dst = lsiw_win_to_linux(to->v[i]);
            if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
                const char *b = strrchr(src.v[i], '/');
                char *t = lsiw_strdup_printf("%s/%s", dst, b ? b + 1 : src.v[i]);
                free(dst); dst = t;
            }
        } else if (dest_is_dir) {
            const char *b = strrchr(src.v[i], '/');
            dst = lsiw_strdup_printf("%s/%s", dest0, b ? b + 1 : src.v[i]);
        } else {
            dst = strdup(dest0);
        }
        if (same_file(src.v[i], dst)) {
            if (func == FO_COPY && (flags & FOF_RENAMEONCOLLISION)) { char *c = collision_name(dst); free(dst); dst = c; }
            else { free(dst); rc = DE_SAMEFILE; break; }
        } else if ((flags & FOF_RENAMEONCOLLISION) && lstat(dst, &st) == 0) {
            char *c = collision_name(dst); free(dst); dst = c;
        }
        int r = func == FO_COPY ? copy_tree(src.v[i], dst) : move_tree(src.v[i], dst);
        if (r < 0) rc = (int)lsiw_errno_to_win(errno);
        free(dst);
    }
    free(dest0);
    sl_free(&src);
    return rc;
}

/* No confirmation or progress UI exists, so the operation is never
 * aborted by the user and fAnyOperationsAborted stays FALSE. The return
 * value is a Win32 error code (or a DE_* code) as on Windows. */
API int SHFileOperationW(SHFILEOPSTRUCTW *op)
{
    if (!op) return ERROR_INVALID_PARAMETER;
    op->fAnyOperationsAborted = FALSE;
    op->hNameMappings = NULL;
    if (op->wFunc < FO_MOVE || op->wFunc > FO_RENAME) return ERROR_INVALID_PARAMETER;
    struct strlist from = { 0 }, to = { 0 };
    parse_list_w(op->pFrom, &from);
    if (op->wFunc != FO_DELETE) parse_list_w(op->pTo, &to);
    int rc = file_operation(op->wFunc, &from, &to, op->fFlags);
    sl_free(&from); sl_free(&to);
    return rc;
}

static WCHAR *dbl_a2w(const char *p)
{
    if (!p) return NULL;
    size_t n = 0;
    while (p[n]) n += strlen(p + n) + 1;
    WCHAR *w = calloc(n * 2 + 2, sizeof(WCHAR)), *o = w;
    for (; *p; p += strlen(p) + 1) o += lsiw_utf8_to_utf16(p, -1, o, (int)(n * 2 + 2 - (size_t)(o - w)));
    *o = 0;
    return w;
}

API int SHFileOperationA(SHFILEOPSTRUCTA *op)
{
    if (!op) return ERROR_INVALID_PARAMETER;
    SHFILEOPSTRUCTW w = { op->hwnd, op->wFunc, dbl_a2w(op->pFrom), dbl_a2w(op->pTo), op->fFlags, FALSE, NULL, NULL };
    int rc = SHFileOperationW(&w);
    op->fAnyOperationsAborted = w.fAnyOperationsAborted;
    op->hNameMappings = NULL;
    free((void *)w.pFrom); free((void *)w.pTo);
    return rc;
}

API void SHFreeNameMappings(void *h) { (void)h; }

/* ---- Recycle Bin ---- */

typedef struct { DWORD cbSize; int64_t i64Size; int64_t i64NumItems; } SHQUERYRBINFO;

API HRESULT SHQueryRecycleBinW(const WCHAR *root, SHQUERYRBINFO *info)
{
    (void)root;
    if (!info || info->cbSize != sizeof *info) return E_INVALIDARG;
    info->i64Size = 0; info->i64NumItems = 0;
    char *files = trash_dir("files");
    DIR *d = opendir(files);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char *p = lsiw_strdup_printf("%s/%s", files, de->d_name);
            info->i64NumItems++;
            info->i64Size += (int64_t)tree_size(p);
            free(p);
        }
        closedir(d);
    }
    free(files);
    return S_OK;
}

API HRESULT SHQueryRecycleBinA(const char *root, SHQUERYRBINFO *info) { (void)root; return SHQueryRecycleBinW(NULL, info); }

static void empty_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *p = lsiw_strdup_printf("%s/%s", dir, de->d_name);
        delete_tree(p);
        free(p);
    }
    closedir(d);
}

API HRESULT SHEmptyRecycleBinW(HWND hwnd, const WCHAR *root, DWORD flags)
{
    (void)hwnd; (void)root; (void)flags;
    char *files = trash_dir("files"), *info = trash_dir("info");
    empty_dir(files);
    empty_dir(info);
    free(files); free(info);
    return S_OK;
}

API HRESULT SHEmptyRecycleBinA(HWND hwnd, const char *root, DWORD flags) { (void)root; return SHEmptyRecycleBinW(hwnd, NULL, flags); }

/* ================================================================== */
/* SHCreateDirectory                                                    */
/* ================================================================== */

static int create_directory_u(const char *path)
{
    WCHAR *w = lsiw_u2w(path);
    int fq = fully_qualified(w);
    free(w);
    if (!fq) { lsiw_set_last_error(ERROR_BAD_PATHNAME); return ERROR_BAD_PATHNAME; }
    char *lin = lsiw_win_to_linux(path);
    struct stat st;
    int rc = 0;
    if (stat(lin, &st) == 0) rc = ERROR_ALREADY_EXISTS;
    else if (lsiw_mkdirs(lin, 0755) < 0) {
        rc = errno == ENOENT ? ERROR_PATH_NOT_FOUND : errno == ENAMETOOLONG ? ERROR_FILENAME_EXCED_RANGE
           : (int)lsiw_errno_to_win(errno);
    }
    free(lin);
    lsiw_set_last_error((uint32_t)rc);
    return rc;
}

API int SHCreateDirectoryExW(HWND hwnd, const WCHAR *path, void *sa)
{
    (void)hwnd; (void)sa;
    if (!path) { lsiw_set_last_error(ERROR_BAD_PATHNAME); return ERROR_BAD_PATHNAME; }
    char *p = lsiw_w2u(path);
    int r = create_directory_u(p);
    free(p);
    return r;
}

API int SHCreateDirectoryExA(HWND hwnd, const char *path, void *sa)
{
    (void)hwnd; (void)sa;
    if (!path) { lsiw_set_last_error(ERROR_BAD_PATHNAME); return ERROR_BAD_PATHNAME; }
    return create_directory_u(path);
}

API int SHCreateDirectory(HWND hwnd, const WCHAR *path) { return SHCreateDirectoryExW(hwnd, path, NULL); }

/* ================================================================== */
/* SHGetFileInfo                                                        */
/* ================================================================== */

#define SHGFI_ICON              0x000000100
#define SHGFI_DISPLAYNAME       0x000000200
#define SHGFI_TYPENAME          0x000000400
#define SHGFI_ATTRIBUTES        0x000000800
#define SHGFI_ICONLOCATION      0x000001000
#define SHGFI_EXETYPE           0x000002000
#define SHGFI_SYSICONINDEX      0x000004000
#define SHGFI_ATTR_SPECIFIED    0x000020000
#define SHGFI_PIDL              0x000000008
#define SHGFI_USEFILEATTRIBUTES 0x000000010

#define SFGAO_CANCOPY        0x00000001
#define SFGAO_CANMOVE        0x00000002
#define SFGAO_CANLINK        0x00000004
#define SFGAO_CANRENAME      0x00000010
#define SFGAO_CANDELETE      0x00000020
#define SFGAO_HASPROPSHEET   0x00000040
#define SFGAO_DROPTARGET     0x00000100
#define SFGAO_LINK           0x00010000
#define SFGAO_READONLY       0x00040000
#define SFGAO_HIDDEN         0x00080000
#define SFGAO_STREAM         0x00400000
#define SFGAO_STORAGEANCESTOR 0x00800000
#define SFGAO_FILESYSANCESTOR 0x10000000
#define SFGAO_FOLDER         0x20000000
#define SFGAO_FILESYSTEM     0x40000000
#define SFGAO_HASSUBFOLDER   0x80000000u

typedef struct { HICON hIcon; int iIcon; DWORD dwAttributes; WCHAR szDisplayName[MAX_PATH]; WCHAR szTypeName[80]; } SHFILEINFOW;
typedef struct { HICON hIcon; int iIcon; DWORD dwAttributes; char szDisplayName[MAX_PATH]; char szTypeName[80]; } SHFILEINFOA;

/* SHGFI_EXETYPE: MAKELONG('PE', version) for GUI programs, MAKELONG('PE', 0)
 * for console programs, 'MZ' for MS-DOS programs, .com and .bat files. */
static DWORD exe_type(const char *win)
{
    const char *e = ext_of(win);
    if (!strcasecmp(e, ".com") || !strcasecmp(e, ".bat") || !strcasecmp(e, ".cmd")) return 0x5A4D;
    if (strcasecmp(e, ".exe")) return 0;
    char *lin = lsiw_win_to_linux(win);
    int fd = open(lin, O_RDONLY | O_CLOEXEC);
    free(lin);
    if (fd < 0) return 0;
    BYTE h[0x40], pe[0x60];
    DWORD r = 0;
    if (pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && h[0] == 'M' && h[1] == 'Z') {
        r = 0x5A4D;
        uint32_t off;
        memcpy(&off, h + 0x3C, 4);
        ssize_t n = pread(fd, pe, sizeof pe, off);
        if (n >= 4 && pe[0] == 'N' && pe[1] == 'E') r = 0x454E | (DWORD)(pe[0x3F] << 24 | pe[0x3E] << 16);
        else if (n == (ssize_t)sizeof pe && !memcmp(pe, "PE\0\0", 4)) {
            const BYTE *opt = pe + 24;
            WORD maj = (WORD)(opt[48] | opt[49] << 8), min = (WORD)(opt[50] | opt[51] << 8);
            WORD subsys = (WORD)(opt[68] | opt[69] << 8);
            r = 0x4550;
            if (subsys == 2) r |= (DWORD)((maj & 0xFF) << 8 | (min & 0xFF)) << 16;   /* GUI */
        }
    }
    close(fd);
    return r;
}

static void type_name(const char *win, int isdir, int isroot, char *out, size_t cap)
{
    static const struct { const char *ext, *name; } known[] = {
        { ".exe", "Application" }, { ".com", "MS-DOS Application" }, { ".dll", "Application extension" },
        { ".txt", "Text Document" }, { ".bat", "Windows Batch File" }, { ".cmd", "Windows Command Script" },
        { ".lnk", "Shortcut" }, { ".url", "Internet Shortcut" }, { ".ini", "Configuration settings" },
        { ".sys", "System file" }, { ".msi", "Windows Installer Package" }, { ".zip", "Compressed (zipped) Folder" },
    };
    if (isroot) { snprintf(out, cap, "Local Disk"); return; }
    if (isdir) { snprintf(out, cap, "File folder"); return; }
    const char *e = ext_of(win);
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcasecmp(e, known[i].ext)) { snprintf(out, cap, "%s", known[i].name); return; }
    if (!e[0] || !e[1]) { snprintf(out, cap, "File"); return; }
    size_t k = 0;
    for (const char *p = e + 1; *p && k + 6 < cap; p++) out[k++] = (char)toupper((unsigned char)*p);
    snprintf(out + k, cap - k, " File");
}

/* Fills the UTF-8 fields; returns the SHGetFileInfo result. */
static uintptr_t file_info(const char *path_in, DWORD fattr, UINT flags, DWORD in_attrs,
                           DWORD *attrs, char *disp, char *type)
{
    if (!path_in) return 0;
    char *win = qualify_u(path_in, NULL);
    if (flags & SHGFI_EXETYPE) { DWORD t = exe_type(win); free(win); return t; }
    WCHAR *w = lsiw_u2w(win);
    int isroot = root_len(w) == wlen(w);
    free(w);
    int isdir, readonly = 0, hidden = 0, hassub = 0;
    if (flags & SHGFI_USEFILEATTRIBUTES) {
        isdir = (fattr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        readonly = (fattr & FILE_ATTRIBUTE_READONLY) != 0;
        hidden = (fattr & FILE_ATTRIBUTE_HIDDEN) != 0;
    } else {
        char *lin = lsiw_win_to_linux(win);
        struct stat st;
        if (stat(lin, &st) < 0) { free(lin); free(win); return 0; }
        isdir = S_ISDIR(st.st_mode);
        readonly = access(lin, W_OK) != 0;
        const char *b = strrchr(lin, '/');
        hidden = b && b[1] == '.';
        if (isdir && (flags & SHGFI_ATTRIBUTES)) {
            DIR *d = opendir(lin);
            struct dirent *de;
            while (d && !hassub && (de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char *c = lsiw_strdup_printf("%s/%s", lin, de->d_name);
                struct stat cs;
                hassub = stat(c, &cs) == 0 && S_ISDIR(cs.st_mode);
                free(c);
            }
            if (d) closedir(d);
        }
        free(lin);
    }
    if (flags & SHGFI_DISPLAYNAME) {
        const char *b = strrchr(win, '\\');
        if (isroot) snprintf(disp, MAX_PATH, "Local Disk (C:)");
        else snprintf(disp, MAX_PATH, "%s", b && b[1] ? b + 1 : win);
    }
    if (flags & SHGFI_TYPENAME) type_name(win, isdir, isroot, type, 80);
    if (flags & SHGFI_ATTRIBUTES) {
        DWORD a = SFGAO_CANCOPY | SFGAO_CANMOVE | SFGAO_CANLINK | SFGAO_CANRENAME | SFGAO_CANDELETE |
                  SFGAO_HASPROPSHEET | SFGAO_FILESYSTEM;
        if (isdir) a |= SFGAO_FOLDER | SFGAO_FILESYSANCESTOR | SFGAO_STORAGEANCESTOR | SFGAO_DROPTARGET | (hassub ? SFGAO_HASSUBFOLDER : 0);
        else a |= SFGAO_STREAM;
        if (readonly) a |= SFGAO_READONLY;
        if (hidden) a |= SFGAO_HIDDEN;
        if (!strcasecmp(ext_of(win), ".lnk")) a |= SFGAO_LINK;
        if (flags & SHGFI_ATTR_SPECIFIED) a &= in_attrs;
        *attrs = a;
    }
    free(win);
    /* No icons or system image list exist: icon fields stay empty, and a
     * request for the image list handle returns NULL. */
    return (flags & SHGFI_SYSICONINDEX) ? 0 : 1;
}

API uintptr_t SHGetFileInfoW(const WCHAR *path, DWORD fattr, SHFILEINFOW *sfi, UINT cb, UINT flags)
{
    char *p = (flags & SHGFI_PIDL) ? pidl_path(path) : lsiw_w2u(path);
    char disp[MAX_PATH] = "", type[80] = "";
    DWORD attrs = 0, in_attrs = sfi ? sfi->dwAttributes : 0;
    if (!sfi && !(flags & SHGFI_EXETYPE)) { free(p); return 0; }
    uintptr_t r = file_info(p, fattr, flags, in_attrs, &attrs, disp, type);
    free(p);
    if (sfi && !(flags & SHGFI_EXETYPE) && cb >= sizeof *sfi) {
        if (flags & (SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ICONLOCATION)) { sfi->hIcon = NULL; sfi->iIcon = 0; }
        if (flags & SHGFI_ICONLOCATION) sfi->szDisplayName[0] = 0;
        if (flags & SHGFI_DISPLAYNAME) u2w_into(disp, sfi->szDisplayName, MAX_PATH);
        if (flags & SHGFI_TYPENAME) u2w_into(type, sfi->szTypeName, 80);
        if (flags & SHGFI_ATTRIBUTES) sfi->dwAttributes = attrs;
    }
    return r;
}

API uintptr_t SHGetFileInfoA(const char *path, DWORD fattr, SHFILEINFOA *sfi, UINT cb, UINT flags)
{
    char *p = (flags & SHGFI_PIDL) ? pidl_path(path) : (path ? strdup(path) : NULL);
    char disp[MAX_PATH] = "", type[80] = "";
    DWORD attrs = 0, in_attrs = sfi ? sfi->dwAttributes : 0;
    if (!sfi && !(flags & SHGFI_EXETYPE)) { free(p); return 0; }
    uintptr_t r = file_info(p, fattr, flags, in_attrs, &attrs, disp, type);
    free(p);
    if (sfi && !(flags & SHGFI_EXETYPE) && cb >= sizeof *sfi) {
        if (flags & (SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ICONLOCATION)) { sfi->hIcon = NULL; sfi->iIcon = 0; }
        if (flags & SHGFI_ICONLOCATION) sfi->szDisplayName[0] = 0;
        if (flags & SHGFI_DISPLAYNAME) snprintf(sfi->szDisplayName, MAX_PATH, "%s", disp);
        if (flags & SHGFI_TYPENAME) snprintf(sfi->szTypeName, 80, "%s", type);
        if (flags & SHGFI_ATTRIBUTES) sfi->dwAttributes = attrs;
    }
    return r;
}

/* ================================================================== */
/* Small shell functions                                                */
/* ================================================================== */

API void SHChangeNotify(LONG event, UINT flags, const void *a, const void *b) { (void)event; (void)flags; (void)a; (void)b; }
API void SHAddToRecentDocs(UINT flags, const void *pv) { (void)flags; (void)pv; }
API BOOL IsUserAnAdmin(void) { return geteuid() == 0; }

/* Drag and drop needs a window system; no files are ever dropped. */
API void DragAcceptFiles(HWND hwnd, BOOL accept) { (void)hwnd; (void)accept; }
API UINT DragQueryFileW(void *hdrop, UINT i, WCHAR *buf, UINT cch) { (void)hdrop; (void)i; if (buf && cch) buf[0] = 0; return 0; }
API UINT DragQueryFileA(void *hdrop, UINT i, char *buf, UINT cch) { (void)hdrop; (void)i; if (buf && cch) buf[0] = 0; return 0; }
API BOOL DragQueryPoint(void *hdrop, void *pt) { (void)hdrop; (void)pt; return FALSE; }
API void DragFinish(void *hdrop) { (void)hdrop; }

/* There is no taskbar or notification area to talk to. */
API BOOL Shell_NotifyIconW(DWORD msg, void *data) { (void)msg; (void)data; return FALSE; }
API BOOL Shell_NotifyIconA(DWORD msg, void *data) { (void)msg; (void)data; return FALSE; }
API uintptr_t SHAppBarMessage(DWORD msg, void *data) { (void)msg; (void)data; return 0; }

/* Icons are not extracted: no icon is found (a count request gets 0). */
API HICON ExtractIconW(HINSTANCE inst, const WCHAR *file, UINT idx) { (void)inst; (void)file; (void)idx; return NULL; }
API HICON ExtractIconA(HINSTANCE inst, const char *file, UINT idx) { (void)inst; (void)file; (void)idx; return NULL; }
API UINT ExtractIconExW(const WCHAR *file, int idx, HICON *large, HICON *small, UINT n)
{
    (void)file; (void)idx;
    for (UINT i = 0; i < n; i++) { if (large) large[i] = NULL; if (small) small[i] = NULL; }
    return 0;
}
API UINT ExtractIconExA(const char *file, int idx, HICON *large, HICON *small, UINT n) { (void)file; return ExtractIconExW(NULL, idx, large, small, n); }

API HRESULT SHOpenFolderAndSelectItems(const void *folder, UINT cidl, const void **items, DWORD flags)
{
    (void)flags;
    char *p = pidl_path((cidl && items && items[0]) ? items[0] : folder);
    if (!p) return E_INVALIDARG;
    char *lin = lsiw_win_to_linux(p);
    struct stat st;
    if (stat(lin, &st) == 0 && !S_ISDIR(st.st_mode)) { char *s = strrchr(lin, '/'); if (s && s != lin) *s = 0; }
    HRESULT hr = xdg_open(lin) == 0 ? S_OK : E_FAIL;
    free(lin); free(p);
    return hr;
}

/* Explorer settings: all options off. */
API void SHGetSetSettings(void *ss, DWORD mask, BOOL set) { (void)mask; if (ss && !set) memset(ss, 0, 32); }
API void SHGetSettings(void *sfs, DWORD mask) { (void)mask; if (sfs) memset(sfs, 0, 4); }

API void *SHAlloc(SIZE_T n) { return lsiw_heap_alloc(n, 0); }
API void SHFree(void *p) { if (p) lsiw_heap_free(p); }

API HRESULT SHStrDupW(const WCHAR *s, WCHAR **out)
{
    if (!s || !out) { if (out) *out = NULL; return E_INVALIDARG; }
    *out = heap_wdup(s);
    return *out ? S_OK : E_OUTOFMEMORY;
}

API HRESULT SHStrDupA(const char *s, WCHAR **out)
{
    if (!s || !out) { if (out) *out = NULL; return E_INVALIDARG; }
    *out = heap_wdup_u(s);
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* "@file.dll,-id" strings are loaded from the module's string table with
 * user32's LoadStringW; anything else is copied unchanged. */
API HRESULT SHLoadIndirectString(const WCHAR *src, WCHAR *out, UINT cch, void **reserved)
{
    (void)reserved;
    if (!src || !out || !cch) return E_INVALIDARG;
    if (src[0] != '@') {
        if (src != out) wcopyn(out, src, cch);
        return S_OK;
    }
    HRESULT hr = E_FAIL;
    WCHAR *copy = wdup(src + 1), *comma = NULL;
    for (WCHAR *q = copy; *q; q++) if (*q == ',') comma = q;
    if (comma && comma[1] == '-') {
        *comma = 0;
        int id = 0;
        for (WCHAR *q = comma + 2; *q >= '0' && *q <= '9'; q++) id = id * 10 + (*q - '0');
        HMODULE (WINAPI *lle)(const WCHAR *, HANDLE, DWORD) = k32("LoadLibraryExW");
        BOOL (WINAPI *fl)(HMODULE) = k32("FreeLibrary");
        int (WINAPI *ls)(HMODULE, UINT, WCHAR *, int) =
            lsiw_builtin_lookup(lsiw_builtin_find("user32.dll"), "LoadStringW");
        HMODULE m = lle ? lle(copy, NULL, 2 /* LOAD_LIBRARY_AS_DATAFILE */) : NULL;
        if (m && ls && ls(m, (UINT)id, out, (int)cch) > 0) hr = S_OK;
        if (m && fl) fl(m);
    }
    free(copy);
    if (hr != S_OK) out[0] = 0;
    return hr;
}

/* ================================================================== */
/* IMalloc (SHGetMalloc) over the process heap                          */
/* ================================================================== */

static const GUID IID_IUnknown_ = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IMalloc_  = { 0x00000002, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_ISequentialStream_ = { 0x0c733a30, 0x2a1c, 0x11ce, { 0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d } };
static const GUID IID_IStream_  = { 0x0000000C, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IServiceProvider_ = { 0x6d5140c1, 0x7436, 0x11ce, { 0x80, 0x34, 0x00, 0xaa, 0x00, 0x60, 0x09, 0xfa } };

struct imalloc;
struct imalloc_vtbl {
    HRESULT (WINAPI *QueryInterface)(struct imalloc *, const GUID *, void **);
    ULONG (WINAPI *AddRef)(struct imalloc *);
    ULONG (WINAPI *Release)(struct imalloc *);
    void *(WINAPI *Alloc)(struct imalloc *, SIZE_T);
    void *(WINAPI *Realloc)(struct imalloc *, void *, SIZE_T);
    void (WINAPI *Free)(struct imalloc *, void *);
    SIZE_T (WINAPI *GetSize)(struct imalloc *, void *);
    int (WINAPI *DidAlloc)(struct imalloc *, void *);
    void (WINAPI *HeapMinimize)(struct imalloc *);
};
struct imalloc { const struct imalloc_vtbl *vtbl; };

API HRESULT malloc_qi(struct imalloc *self, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (iid && (guid_eq(iid, &IID_IUnknown_) || guid_eq(iid, &IID_IMalloc_))) { *out = self; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
API ULONG malloc_addref(struct imalloc *self) { (void)self; return 1; }
API ULONG malloc_release(struct imalloc *self) { (void)self; return 1; }
API void *malloc_alloc(struct imalloc *self, SIZE_T n) { (void)self; return lsiw_heap_alloc(n, 0); }
API void *malloc_realloc(struct imalloc *self, void *p, SIZE_T n)
{
    (void)self;
    if (p && !n) { lsiw_heap_free(p); return NULL; }
    return lsiw_heap_realloc(p, n);
}
API void malloc_free(struct imalloc *self, void *p) { (void)self; if (p) lsiw_heap_free(p); }
API SIZE_T malloc_getsize(struct imalloc *self, void *p) { (void)self; return p ? lsiw_heap_size(p) : (SIZE_T)-1; }
API int malloc_didalloc(struct imalloc *self, void *p) { (void)self; return p ? (lsiw_heap_owns(p) ? 1 : 0) : -1; }
API void malloc_minimize(struct imalloc *self) { (void)self; }

static const struct imalloc_vtbl g_malloc_vtbl = {
    malloc_qi, malloc_addref, malloc_release, malloc_alloc, malloc_realloc,
    malloc_free, malloc_getsize, malloc_didalloc, malloc_minimize,
};
static struct imalloc g_malloc = { &g_malloc_vtbl };

API HRESULT SHGetMalloc(struct imalloc **out)
{
    if (!out) return E_INVALIDARG;
    *out = &g_malloc;
    return S_OK;
}

/* ================================================================== */
/* shlwapi: Path functions                                              */
/* ================================================================== */

/* A wrappers: in-place functions work on a wide copy and convert back;
 * pointer-returning functions map the result back into the ANSI string. */
#define A_INPLACE_BOOL(fn) \
    API BOOL fn##A(char *path) { \
        if (!path) return FALSE; \
        WCHAR *w = a2w_path(path); \
        BOOL r = fn##W(w); \
        if (!w2a_into(w, path, MAX_PATH)) r = FALSE; \
        free(w); \
        return r; \
    }
#define A_PTR(fn) \
    API char *fn##A(const char *path) { \
        if (!path) return NULL; \
        WCHAR *w = lsiw_u2w(path); \
        char *r = a_ptr(path, w, fn##W(w)); \
        free(w); \
        return r; \
    }
#define A_PRED(fn) \
    API BOOL fn##A(const char *path) { \
        if (!path) return FALSE; \
        WCHAR *w = lsiw_u2w(path); \
        BOOL r = fn##W(w); \
        free(w); \
        return r; \
    }

API BOOL PathFileExistsW(const WCHAR *path)
{
    if (!path || !*path) return FALSE;
    char *lin = lsiw_path_w(path);
    struct stat st;
    BOOL r = lin && stat(lin, &st) == 0;
    free(lin);
    return r;
}
A_PRED(PathFileExists)

API BOOL PathIsDirectoryW(const WCHAR *path)
{
    if (!path || !*path) return FALSE;
    char *lin = lsiw_path_w(path);
    struct stat st;
    BOOL r = lin && stat(lin, &st) == 0 && S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FALSE;
    free(lin);
    return r;
}
A_PRED(PathIsDirectory)

API BOOL PathIsDirectoryEmptyW(const WCHAR *path)
{
    if (!path) return FALSE;
    char *lin = lsiw_path_w(path);
    DIR *d = lin ? opendir(lin) : NULL;
    free(lin);
    if (!d) return FALSE;
    struct dirent *de;
    BOOL empty = TRUE;
    while (empty && (de = readdir(d)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) empty = FALSE;
    closedir(d);
    return empty;
}
A_PRED(PathIsDirectoryEmpty)

API BOOL PathIsRelativeW(const WCHAR *path)
{
    if (!path || !*path) return TRUE;
    return !(is_sep(path[0]) || is_drive(path));
}
A_PRED(PathIsRelative)

API BOOL PathIsRootW(const WCHAR *path)
{
    if (!path || !*path) return FALSE;
    if (is_sep(path[0]) && !path[1]) return TRUE;
    if (is_drive(path) && is_sep(path[2]) && !path[3]) return TRUE;
    if (is_sep(path[0]) && is_sep(path[1])) {
        const WCHAR *p = path + 2;
        while (*p && !is_sep(*p)) p++;          /* server */
        if (!*p) return TRUE;
        p++;
        while (*p && !is_sep(*p)) p++;          /* share */
        return !*p;
    }
    return FALSE;
}
A_PRED(PathIsRoot)

API BOOL PathIsUNCW(const WCHAR *path)
{
    if (!path) return FALSE;
    if (path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\') return !wncmpi(path + 4, u"UNC\\", 4);
    return path[0] == '\\' && path[1] == '\\';
}
A_PRED(PathIsUNC)

/* Only UNC paths can be network paths: LSIW has no mapped network drives. */
API BOOL PathIsNetworkPathW(const WCHAR *path) { return PathIsUNCW(path); }
A_PRED(PathIsNetworkPath)

API BOOL PathIsFileSpecW(const WCHAR *path)
{
    if (!path) return FALSE;
    for (; *path; path++) if (*path == '\\' || *path == ':') return FALSE;
    return TRUE;
}
A_PRED(PathIsFileSpec)

API BOOL PathIsURLW(const WCHAR *path)
{
    if (!path) return FALSE;
    char *u = lsiw_w2u(path);
    BOOL r = url_scheme_len(u) > 0;
    free(u);
    return r;
}
A_PRED(PathIsURL)

API WCHAR *PathFindFileNameW(const WCHAR *path)
{
    if (!path) return NULL;
    const WCHAR *last = path;
    for (const WCHAR *p = path; *p; p++)
        if ((*p == '\\' || *p == '/' || *p == ':') && p[1] && p[1] != '\\' && p[1] != '/') last = p + 1;
    return (WCHAR *)last;
}
A_PTR(PathFindFileName)

/* The extension starts at the last dot of the last component; a space
 * after that dot means there is no extension. */
API WCHAR *PathFindExtensionW(const WCHAR *path)
{
    if (!path) return NULL;
    const WCHAR *dot = NULL, *p = path;
    for (; *p; p++) {
        if (*p == '\\' || *p == ' ') dot = NULL;
        else if (*p == '.') dot = p;
    }
    return (WCHAR *)(dot ? dot : p);
}
A_PTR(PathFindExtension)

API WCHAR *PathGetArgsW(const WCHAR *path)
{
    if (!path) return NULL;
    int inq = 0;
    for (; *path; path++) {
        if (*path == '"') inq = !inq;
        else if (*path == ' ' && !inq) return (WCHAR *)path + 1;
    }
    return (WCHAR *)path;
}
A_PTR(PathGetArgs)

API WCHAR *PathSkipRootW(const WCHAR *path)
{
    if (!path || !*path) return NULL;
    if (is_sep(path[0]) && is_sep(path[1])) {
        const WCHAR *p = path + 2;
        while (*p && !is_sep(*p)) p++;
        if (*p) p++;
        while (*p && !is_sep(*p)) p++;
        if (*p) p++;
        return (WCHAR *)p;
    }
    if (is_drive(path) && is_sep(path[2])) return (WCHAR *)path + 3;
    if (is_sep(path[0])) return (WCHAR *)path + 1;
    return NULL;
}
A_PTR(PathSkipRoot)

API WCHAR *PathFindNextComponentW(const WCHAR *path)
{
    if (!path || !*path) return NULL;
    while (*path && *path != '\\') path++;
    return (WCHAR *)(*path ? path + 1 : path);
}
A_PTR(PathFindNextComponent)

API void PathRemoveExtensionW(WCHAR *path) { if (path) *PathFindExtensionW(path) = 0; }
API void PathRemoveExtensionA(char *path) { if (path) *PathFindExtensionA(path) = 0; }

API BOOL PathRemoveFileSpecW(WCHAR *path)
{
    if (!path || !*path) return FALSE;
    size_t r = root_len(path), n = wlen(path);
    WCHAR *last = NULL;
    for (WCHAR *p = path + r; *p; p++) if (*p == '\\') last = p;
    if (last) { *last = 0; return TRUE; }
    if (r == n) return FALSE;                    /* only a root left */
    path[r] = 0;                                 /* "a" -> "", "C:\a" -> "C:\" */
    return TRUE;
}
A_INPLACE_BOOL(PathRemoveFileSpec)

API WCHAR *PathAddBackslashW(WCHAR *path)
{
    if (!path) return NULL;
    size_t n = wlen(path);
    if (n >= MAX_PATH - 1) return NULL;
    if (n && path[n - 1] != '\\') { path[n++] = '\\'; path[n] = 0; }
    return path + n;
}

API char *PathAddBackslashA(char *path)
{
    if (!path) return NULL;
    size_t n = strlen(path);
    if (n >= MAX_PATH - 1) return NULL;
    if (n && path[n - 1] != '\\') { path[n++] = '\\'; path[n] = 0; }
    return path + n;
}

API WCHAR *PathRemoveBackslashW(WCHAR *path)
{
    if (!path) return NULL;
    size_t n = wlen(path);
    if (!n) return path;
    if (path[n - 1] == '\\' && !PathIsRootW(path)) { path[n - 1] = 0; return path + n - 1; }
    return path + n - 1;
}

API char *PathRemoveBackslashA(char *path)
{
    if (!path) return NULL;
    WCHAR *w = a2w_path(path);
    WCHAR *r = PathRemoveBackslashW(w);
    size_t wi = (size_t)(r - w);
    w2a_into(w, path, MAX_PATH);
    char *ret = path + a_offset(path, wi);
    free(w);
    return ret;
}

API void PathRemoveBlanksW(WCHAR *path)
{
    if (!path) return;
    WCHAR *s = path;
    while (*s == ' ') s++;
    size_t n = wlen(s);
    while (n && s[n - 1] == ' ') n--;
    memmove(path, s, n * sizeof(WCHAR));
    path[n] = 0;
}
API void PathRemoveBlanksA(char *path)
{
    if (!path) return;
    char *s = path;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    memmove(path, s, n);
    path[n] = 0;
}

API BOOL PathStripToRootW(WCHAR *path)
{
    if (!path) return FALSE;
    while (!PathIsRootW(path)) if (!PathRemoveFileSpecW(path)) return FALSE;
    return TRUE;
}
A_INPLACE_BOOL(PathStripToRoot)

API void PathStripPathW(WCHAR *path)
{
    if (!path) return;
    WCHAR *f = PathFindFileNameW(path);
    if (f != path) memmove(path, f, (wlen(f) + 1) * sizeof(WCHAR));
}
API void PathStripPathA(char *path)
{
    if (!path) return;
    char *f = PathFindFileNameA(path);
    if (f != path) memmove(path, f, strlen(f) + 1);
}

API BOOL PathCanonicalizeW(WCHAR *dst, const WCHAR *src)
{
    if (!dst || !src) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    WCHAR tmp[MAX_PATH];
    if (canon_w(src, tmp, MAX_PATH) == (size_t)-1) { dst[0] = 0; lsiw_set_last_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    memcpy(dst, tmp, (wlen(tmp) + 1) * sizeof(WCHAR));
    return TRUE;
}

API BOOL PathCanonicalizeA(char *dst, const char *src)
{
    if (!dst || !src) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    WCHAR *w = lsiw_u2w(src), out[MAX_PATH];
    BOOL r = PathCanonicalizeW(out, w) && w2a_into(out, dst, MAX_PATH);
    free(w);
    return r;
}

API WCHAR *PathCombineW(WCHAR *dst, const WCHAR *dir, const WCHAR *file)
{
    if (!dst) return NULL;
    if (!dir && !file) { dst[0] = 0; return NULL; }
    size_t dl = dir ? wlen(dir) : 0, fl = file ? wlen(file) : 0;
    WCHAR *tmp = calloc(dl + fl + 4, sizeof(WCHAR));
    if (!file || !*file) memcpy(tmp, dir, dl * sizeof(WCHAR));
    else if (!dir || !*dir || fully_qualified(file) || is_drive(file)) memcpy(tmp, file, fl * sizeof(WCHAR));
    else if (is_sep(file[0])) {
        /* root-relative: keep only the drive or share of `dir` */
        size_t r = root_len(dir);
        if (r && is_sep(dir[r - 1])) r--;
        memcpy(tmp, dir, r * sizeof(WCHAR));
        memcpy(tmp + r, file, fl * sizeof(WCHAR));
    } else {
        memcpy(tmp, dir, dl * sizeof(WCHAR));
        size_t o = dl;
        if (o && tmp[o - 1] != '\\') tmp[o++] = '\\';
        memcpy(tmp + o, file, fl * sizeof(WCHAR));
    }
    WCHAR out[MAX_PATH];
    size_t n = canon_w(tmp, out, MAX_PATH);
    free(tmp);
    if (n == (size_t)-1) { dst[0] = 0; return NULL; }
    memcpy(dst, out, (n + 1) * sizeof(WCHAR));
    return dst;
}

API char *PathCombineA(char *dst, const char *dir, const char *file)
{
    if (!dst) return NULL;
    WCHAR *d = lsiw_u2w(dir), *f = lsiw_u2w(file), out[MAX_PATH];
    WCHAR *r = PathCombineW(out, d, f);
    free(d); free(f);
    if (!r || !w2a_into(out, dst, MAX_PATH)) { dst[0] = 0; return NULL; }
    return dst;
}

API BOOL PathAppendW(WCHAR *path, const WCHAR *more)
{
    if (!path || !more) return FALSE;
    if (!fully_qualified(more) && !is_drive(more)) while (*more == '\\') more++;
    WCHAR *copy = wdup(path);
    WCHAR *r = PathCombineW(path, copy, more);
    free(copy);
    return r != NULL;
}

API BOOL PathAppendA(char *path, const char *more)
{
    if (!path || !more) return FALSE;
    WCHAR *w = a2w_path(path), *m = lsiw_u2w(more);
    BOOL r = PathAppendW(w, m) && w2a_into(w, path, MAX_PATH);
    free(w); free(m);
    return r;
}

API BOOL PathRenameExtensionW(WCHAR *path, const WCHAR *ext)
{
    if (!path || !ext) return FALSE;
    WCHAR *e = PathFindExtensionW(path);
    if ((size_t)(e - path) + wlen(ext) >= MAX_PATH) return FALSE;
    memcpy(e, ext, (wlen(ext) + 1) * sizeof(WCHAR));
    return TRUE;
}

API BOOL PathRenameExtensionA(char *path, const char *ext)
{
    if (!path || !ext) return FALSE;
    char *e = PathFindExtensionA(path);
    if ((size_t)(e - path) + strlen(ext) >= MAX_PATH) return FALSE;
    memcpy(e, ext, strlen(ext) + 1);
    return TRUE;
}

API BOOL PathAddExtensionW(WCHAR *path, const WCHAR *ext)
{
    if (!path) return FALSE;
    if (*PathFindExtensionW(path)) return FALSE;
    if (!ext || !*ext) ext = u".exe";
    size_t n = wlen(path);
    if (n + wlen(ext) >= MAX_PATH) return FALSE;
    memcpy(path + n, ext, (wlen(ext) + 1) * sizeof(WCHAR));
    return TRUE;
}

API BOOL PathAddExtensionA(char *path, const char *ext)
{
    if (!path) return FALSE;
    if (*PathFindExtensionA(path)) return FALSE;
    if (!ext || !*ext) ext = ".exe";
    size_t n = strlen(path);
    if (n + strlen(ext) >= MAX_PATH) return FALSE;
    memcpy(path + n, ext, strlen(ext) + 1);
    return TRUE;
}

API BOOL PathQuoteSpacesW(WCHAR *path)
{
    if (!path || !wchr(path, ' ')) return FALSE;
    size_t n = wlen(path);
    if (n + 3 > MAX_PATH) return FALSE;
    memmove(path + 1, path, n * sizeof(WCHAR));
    path[0] = '"'; path[n + 1] = '"'; path[n + 2] = 0;
    return TRUE;
}
A_INPLACE_BOOL(PathQuoteSpaces)

API BOOL PathUnquoteSpacesW(WCHAR *path)
{
    if (!path) return FALSE;
    size_t n = wlen(path);
    if (n < 2 || path[0] != '"' || path[n - 1] != '"') return FALSE;
    memmove(path, path + 1, (n - 2) * sizeof(WCHAR));
    path[n - 2] = 0;
    return TRUE;
}
A_INPLACE_BOOL(PathUnquoteSpaces)

API void PathRemoveArgsW(WCHAR *path)
{
    if (!path) return;
    WCHAR *a = PathGetArgsW(path);
    if (*a) a[-1] = 0;
    else { size_t n = wlen(path); if (n && path[n - 1] == ' ') path[n - 1] = 0; }
}
API void PathRemoveArgsA(char *path)
{
    if (!path) return;
    char *a = PathGetArgsA(path);
    if (*a) a[-1] = 0;
    else { size_t n = strlen(path); if (n && path[n - 1] == ' ') path[n - 1] = 0; }
}

/* PathMatchSpec: `spec` is a ';'-separated list of patterns. */
static BOOL match_spec_list(const WCHAR *file, const WCHAR *spec, int multiple, int strip)
{
    if (!multiple) return wild_match(spec, file);
    WCHAR *copy = wdup(spec), *p = copy;
    BOOL r = FALSE;
    while (p && !r) {
        WCHAR *semi = wchr(p, ';');
        if (semi) *semi = 0;
        WCHAR *s = p;
        if (strip) {
            while (*s == ' ') s++;
            size_t n = wlen(s);
            while (n && s[n - 1] == ' ') s[--n] = 0;
        }
        if (*s && wild_match(s, file)) r = TRUE;
        p = semi ? semi + 1 : NULL;
    }
    free(copy);
    return r;
}

API BOOL PathMatchSpecW(const WCHAR *file, const WCHAR *spec)
{
    if (!file || !spec) return FALSE;
    if (!wcmpi(spec, u"*.*")) return TRUE;
    return match_spec_list(file, spec, 1, 1);
}

API BOOL PathMatchSpecA(const char *file, const char *spec)
{
    if (!file || !spec) return FALSE;
    WCHAR *f = lsiw_u2w(file), *s = lsiw_u2w(spec);
    BOOL r = PathMatchSpecW(f, s);
    free(f); free(s);
    return r;
}

#define PMSF_MULTIPLE 0x1
#define PMSF_DONT_STRIP_SPACES 0x10000
API HRESULT PathMatchSpecExW(const WCHAR *file, const WCHAR *spec, DWORD flags)
{
    if (!file || !spec) return E_INVALIDARG;
    return match_spec_list(file, spec, (flags & PMSF_MULTIPLE) != 0, !(flags & PMSF_DONT_STRIP_SPACES)) ? S_OK : S_FALSE;
}

API HRESULT PathMatchSpecExA(const char *file, const char *spec, DWORD flags)
{
    if (!file || !spec) return E_INVALIDARG;
    WCHAR *f = lsiw_u2w(file), *s = lsiw_u2w(spec);
    HRESULT r = PathMatchSpecExW(f, s, flags);
    free(f); free(s);
    return r;
}

API BOOL PathCompactPathExW(WCHAR *out, const WCHAR *src, UINT cch, DWORD flags)
{
    (void)flags;
    if (!out || !src || !cch) return FALSE;
    size_t n = wlen(src);
    if (n < cch) { memcpy(out, src, (n + 1) * sizeof(WCHAR)); return TRUE; }
    const WCHAR *file = PathFindFileNameW(src);
    size_t fl = wlen(file);
    if (cch < 4) {                                   /* room for dots only */
        size_t i = 0;
        for (; i + 1 < cch; i++) out[i] = '.';
        out[i] = 0;
        return TRUE;
    }
    if (file == src || fl + 4 >= cch) {
        /* even the name does not fit: "name..." */
        size_t keep = cch - 4;
        memcpy(out, file, keep * sizeof(WCHAR));
        memcpy(out + keep, u"...", 4 * sizeof(WCHAR));
        return TRUE;
    }
    /* "C:\dir...\name": as much of the start as fits, "...", the name */
    size_t keep = cch - 1 - 3 - fl - 1;
    memcpy(out, src, keep * sizeof(WCHAR));
    memcpy(out + keep, u"...\\", 4 * sizeof(WCHAR));
    memcpy(out + keep + 4, file, (fl + 1) * sizeof(WCHAR));
    return TRUE;
}

API BOOL PathCompactPathExA(char *out, const char *src, UINT cch, DWORD flags)
{
    if (!out || !src || !cch) return FALSE;
    WCHAR *w = lsiw_u2w(src), *o = calloc(cch + 1, sizeof(WCHAR));
    BOOL r = PathCompactPathExW(o, w, cch, flags) && w2a_into(o, out, cch);
    free(w); free(o);
    return r;
}

API int PathCommonPrefixW(const WCHAR *a, const WCHAR *b, WCHAR *out)
{
    if (out) out[0] = 0;
    if (!a || !b) return 0;
    size_t i = 0, last = 0;
    for (;; i++) {
        WCHAR x = a[i], y = b[i];
        if ((!x || x == '\\') && (!y || y == '\\')) { last = i; if (!x || !y) break; }
        if (!x || !y || wlower(x) != wlower(y)) break;
    }
    if (last == 2 && is_drive(a) && a[2] == '\\') last = 3;     /* keep "C:\" */
    if (last <= 2 && is_sep(a[0]) && is_sep(a[1])) last = 0;    /* "\\" alone is no prefix */
    if (out && last) { memcpy(out, a, last * sizeof(WCHAR)); out[last] = 0; }
    return (int)last;
}

API int PathCommonPrefixA(const char *a, const char *b, char *out)
{
    if (out) out[0] = 0;
    if (!a || !b) return 0;
    WCHAR *wa = lsiw_u2w(a), *wb = lsiw_u2w(b);
    int n = PathCommonPrefixW(wa, wb, NULL);
    size_t bytes = a_offset(a, (size_t)n);
    if (out && n) { memcpy(out, a, bytes); out[bytes] = 0; }
    free(wa); free(wb);
    return (int)bytes;
}

API BOOL PathIsPrefixW(const WCHAR *prefix, const WCHAR *path)
{
    if (!prefix || !path) return FALSE;
    return (size_t)PathCommonPrefixW(prefix, path, NULL) == wlen(prefix);
}

API BOOL PathIsPrefixA(const char *prefix, const char *path)
{
    if (!prefix || !path) return FALSE;
    WCHAR *a = lsiw_u2w(prefix), *b = lsiw_u2w(path);
    BOOL r = PathIsPrefixW(a, b);
    free(a); free(b);
    return r;
}

API BOOL PathIsSameRootW(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b) return FALSE;
    const WCHAR *ra = PathSkipRootW(a), *rb = PathSkipRootW(b);
    if (!ra || !rb) return FALSE;
    size_t la = (size_t)(ra - a), lb = (size_t)(rb - b);
    if (la && is_sep(a[la - 1])) la--;
    if (lb && is_sep(b[lb - 1])) lb--;
    return la == lb && !wncmpi(a, b, la);
}

API BOOL PathIsSameRootA(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    WCHAR *x = lsiw_u2w(a), *y = lsiw_u2w(b);
    BOOL r = PathIsSameRootW(x, y);
    free(x); free(y);
    return r;
}

API BOOL PathRelativePathToW(WCHAR *out, const WCHAR *from, DWORD attr_from, const WCHAR *to, DWORD attr_to)
{
    (void)attr_to;
    if (!out || !from || !to) return FALSE;
    out[0] = 0;
    WCHAR f[MAX_PATH], t[MAX_PATH];
    wcopyn(f, from, MAX_PATH);
    wcopyn(t, to, MAX_PATH);
    if (!(attr_from & FILE_ATTRIBUTE_DIRECTORY)) PathRemoveFileSpecW(f);
    int n = PathCommonPrefixW(f, t, NULL);
    if (n <= 0) return FALSE;
    WCHAR buf[MAX_PATH * 2];
    size_t o = 0;
    const WCHAR *rest = f + n;
    while (*rest == '\\') rest++;
    if (!*rest) buf[o++] = '.';
    while (*rest) {
        if (o) buf[o++] = '\\';
        buf[o++] = '.'; buf[o++] = '.';
        while (*rest && *rest != '\\') rest++;
        while (*rest == '\\') rest++;
    }
    const WCHAR *tail = t + n;
    if (*tail && *tail != '\\') buf[o++] = '\\';
    size_t tl = wlen(tail);
    if (o + tl >= MAX_PATH) return FALSE;
    memcpy(buf + o, tail, (tl + 1) * sizeof(WCHAR));
    memcpy(out, buf, (wlen(buf) + 1) * sizeof(WCHAR));
    return TRUE;
}

API BOOL PathRelativePathToA(char *out, const char *from, DWORD af, const char *to, DWORD at)
{
    if (!out || !from || !to) return FALSE;
    WCHAR *f = lsiw_u2w(from), *t = lsiw_u2w(to), o[MAX_PATH];
    BOOL r = PathRelativePathToW(o, f, af, t, at) && w2a_into(o, out, MAX_PATH);
    free(f); free(t);
    return r;
}

API int PathGetDriveNumberW(const WCHAR *path)
{
    if (!path) return -1;
    if (path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\') path += 4;
    if (is_drive(path)) return (path[0] | 32) - 'a';
    return -1;
}

API int PathGetDriveNumberA(const char *path)
{
    if (!path) return -1;
    WCHAR *w = lsiw_u2w(path);
    int r = PathGetDriveNumberW(w);
    free(w);
    return r;
}

API WCHAR *PathBuildRootW(WCHAR *root, int drive)
{
    if (root && drive >= 0 && drive < 26) { root[0] = (WCHAR)('A' + drive); root[1] = ':'; root[2] = '\\'; root[3] = 0; }
    return root;
}

API char *PathBuildRootA(char *root, int drive)
{
    if (root && drive >= 0 && drive < 26) { root[0] = (char)('A' + drive); root[1] = ':'; root[2] = '\\'; root[3] = 0; }
    return root;
}

API BOOL PathMakePrettyW(WCHAR *path)
{
    if (!path || !*path) return FALSE;
    for (WCHAR *p = path; *p; p++) if (wupper(*p) != *p) return FALSE;
    for (WCHAR *p = path + 1; *p; p++) *p = wlower(*p);
    return TRUE;
}
A_INPLACE_BOOL(PathMakePretty)

static int win_file_exists(const char *win, int want_file)
{
    char *lin = lsiw_win_to_linux(win);
    struct stat st;
    int r = stat(lin, &st) == 0 && (!want_file || S_ISREG(st.st_mode));
    free(lin);
    return r;
}

/* Search `dirs`, then the system folders, then %PATH% for a file name. */
static char *find_on_path(const char *file, char **dirs)
{
    for (size_t i = 0; dirs && dirs[i]; i++) {
        char *c = lsiw_strdup_printf("%s%s%s", dirs[i], dirs[i][0] && dirs[i][strlen(dirs[i]) - 1] == '\\' ? "" : "\\", file);
        if (win_file_exists(c, 0)) return c;
        free(c);
    }
    const char *fixed[] = { lsiw_var_windows("SYSTEM32"), lsiw_var_windows("SYSTEM16"), lsiw_var_windows("WINDIR") };
    for (int i = 0; i < 3; i++) {
        if (!fixed[i]) continue;
        char *c = lsiw_strdup_printf("%s\\%s", fixed[i], file);
        if (win_file_exists(c, 0)) return c;
        free(c);
    }
    const char *path = lsiw_env_get("PATH");
    char *copy = strdup(path ? path : ""), *save = NULL, *found = NULL;
    for (char *d = strtok_r(copy, ";", &save); d && !found; d = strtok_r(NULL, ";", &save)) {
        char *c = lsiw_strdup_printf("%s\\%s", d, file);
        if (win_file_exists(c, 0)) found = c; else free(c);
    }
    free(copy);
    return found;
}

API BOOL PathFindOnPathW(WCHAR *file, const WCHAR **other)
{
    if (!file) return FALSE;
    char *f = lsiw_w2u(file);
    size_t n = 0;
    while (other && other[n]) n++;
    char **dirs = calloc(n + 1, sizeof(char *));
    for (size_t i = 0; i < n; i++) dirs[i] = lsiw_w2u(other[i]);
    char *r = find_on_path(f, dirs);
    for (size_t i = 0; i < n; i++) free(dirs[i]);
    free(dirs); free(f);
    BOOL ok = r && u2w_into(r, file, MAX_PATH);
    free(r);
    return ok;
}

API BOOL PathFindOnPathA(char *file, const char **other)
{
    if (!file) return FALSE;
    char *r = find_on_path(file, (char **)other);
    BOOL ok = r && strlen(r) < MAX_PATH;
    if (ok) strcpy(file, r);
    free(r);
    return ok;
}

static char *search_and_qualify(const char *path)
{
    if (!strchr(path, '\\') && !strchr(path, '/') && !strchr(path, ':')) {
        char *r = find_on_path(path, NULL);
        if (r) return r;
    }
    return qualify_u(path, NULL);
}

API BOOL PathSearchAndQualifyW(const WCHAR *path, WCHAR *buf, UINT cch)
{
    if (!path || !buf || !cch) return FALSE;
    char *p = lsiw_w2u(path), *r = search_and_qualify(p);
    BOOL ok = u2w_into(r, buf, cch);
    free(p); free(r);
    return ok;
}

API BOOL PathSearchAndQualifyA(const char *path, char *buf, UINT cch)
{
    if (!path || !buf || !cch) return FALSE;
    char *r = search_and_qualify(path);
    BOOL ok = strlen(r) < cch;
    if (ok) strcpy(buf, r); else buf[0] = 0;
    free(r);
    return ok;
}

/* ---- file: URLs ---- */

/* "C:\a b\c" -> "file:///C:/a%20b/c", "\\srv\share" -> "file://srv/share". */
static char *url_from_path(const char *path)
{
    const char *p = path;
    size_t n = strlen(p);
    char *o = malloc(n * 3 + 16), *w = o;
    w += sprintf(w, "file:");
    if (p[0] == '\\' && p[1] == '\\') { w += sprintf(w, "//"); p += 2; }
    else w += sprintf(w, "///");
    for (const unsigned char *s = (const unsigned char *)p; *s; s++) {
        if (*s == '\\') *w++ = '/';
        else if (*s <= ' ' || strchr("%#?\"<>^`{|}", *s)) w += sprintf(w, "%%%02X", *s);
        else *w++ = (char)*s;
    }
    *w = 0;
    return o;
}

API HRESULT UrlCreateFromPathW(const WCHAR *path, WCHAR *url, DWORD *pcch, DWORD reserved)
{
    (void)reserved;
    if (!path || !url || !pcch) return E_INVALIDARG;
    char *p = lsiw_w2u(path);
    HRESULT hr = S_OK;
    char *u;
    if (url_scheme_len(p)) { u = strdup(p); hr = S_FALSE; }
    else u = url_from_path(p);
    free(p);
    int need = lsiw_utf8_to_utf16(u, -1, NULL, 0);
    if ((DWORD)need > *pcch) { *pcch = (DWORD)need; free(u); return E_POINTER; }
    lsiw_utf8_to_utf16(u, -1, url, (int)*pcch);
    *pcch = (DWORD)need - 1;
    free(u);
    return hr;
}

API HRESULT UrlCreateFromPathA(const char *path, char *url, DWORD *pcch, DWORD reserved)
{
    (void)reserved;
    if (!path || !url || !pcch) return E_INVALIDARG;
    HRESULT hr = S_OK;
    char *u;
    if (url_scheme_len(path)) { u = strdup(path); hr = S_FALSE; }
    else u = url_from_path(path);
    size_t need = strlen(u) + 1;
    if (need > *pcch) { *pcch = (DWORD)need; free(u); return E_POINTER; }
    memcpy(url, u, need);
    *pcch = (DWORD)need - 1;
    free(u);
    return hr;
}

/* "file:///C:/x%20y" -> "C:\x y"; "file://srv/share" -> "\\srv\share";
 * "file:///home/me" (no drive) is a Linux path -> its C:\ form. */
static HRESULT path_from_url(const char *url, char **out)
{
    *out = NULL;
    if (strncasecmp(url, "file:", 5)) return E_INVALIDARG;
    const char *p = url + 5;
    int unc = 0;
    if (!strncmp(p, "///", 3)) p += 3;
    else if (!strncmp(p, "//", 2)) {
        p += 2;
        if (!strncasecmp(p, "localhost/", 10)) p += 10;
        else if (!(isalpha((unsigned char)p[0]) && (p[1] == ':' || p[1] == '|'))) unc = 1;
    }
    size_t n = strlen(p);
    char *dec = malloc(n + 1), *w = dec;
    for (const char *s = p; *s; s++) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) { *w++ = (char)(hexval(s[1]) << 4 | hexval(s[2])); s += 2; }
        else *w++ = *s;
    }
    *w = 0;
    if (isalpha((unsigned char)dec[0]) && dec[1] == '|') dec[1] = ':';
    char *r;
    if (unc) r = lsiw_strdup_printf("\\\\%s", dec);
    else if (isalpha((unsigned char)dec[0]) && dec[1] == ':') r = strdup(dec);
    else { char *l = lsiw_strdup_printf("/%s", dec); r = lsiw_linux_to_win(l); free(l); }
    free(dec);
    for (char *c = r; *c; c++) if (*c == '/') *c = '\\';
    *out = r;
    return S_OK;
}

API HRESULT PathCreateFromUrlW(const WCHAR *url, WCHAR *path, DWORD *pcch, DWORD reserved)
{
    (void)reserved;
    if (!url || !path || !pcch || !*pcch) return E_INVALIDARG;
    char *u = lsiw_w2u(url), *p;
    HRESULT hr = path_from_url(u, &p);
    free(u);
    if (hr != S_OK) return hr;
    int need = lsiw_utf8_to_utf16(p, -1, NULL, 0);
    if ((DWORD)need > *pcch) { *pcch = (DWORD)need; free(p); return E_POINTER; }
    lsiw_utf8_to_utf16(p, -1, path, (int)*pcch);
    *pcch = (DWORD)need - 1;
    free(p);
    return S_OK;
}

API HRESULT PathCreateFromUrlA(const char *url, char *path, DWORD *pcch, DWORD reserved)
{
    (void)reserved;
    if (!url || !path || !pcch || !*pcch) return E_INVALIDARG;
    char *p;
    HRESULT hr = path_from_url(url, &p);
    if (hr != S_OK) return hr;
    size_t need = strlen(p) + 1;
    if (need > *pcch) { *pcch = (DWORD)need; free(p); return E_POINTER; }
    memcpy(path, p, need);
    *pcch = (DWORD)need - 1;
    free(p);
    return S_OK;
}

API HRESULT PathCreateFromUrlAlloc(const WCHAR *url, WCHAR **out, DWORD reserved)
{
    (void)reserved;
    if (!url || !out) return E_INVALIDARG;
    *out = NULL;
    char *u = lsiw_w2u(url), *p;
    HRESULT hr = path_from_url(u, &p);
    free(u);
    if (hr != S_OK) return hr;
    *out = heap_wdup_u(p);
    free(p);
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* ================================================================== */
/* shlwapi: string functions                                            */
/* ================================================================== */

/* Comparison close to CompareString's default ordering for the characters
 * programs use: case-insensitive first, then lower case before upper case
 * at the first difference in case. */
static int lingcmp(const WCHAR *a, const WCHAR *b, size_t n, int ignore_case)
{
    int case_diff = 0;
    for (size_t i = 0; i < n; i++) {
        WCHAR x = a[i], y = b[i];
        WCHAR lx = wlower(x), ly = wlower(y);
        if (lx != ly) return lx < ly ? -1 : 1;
        if (!x) break;
        if (!case_diff && x != y) case_diff = (x == lx) ? -1 : 1;   /* lower sorts first */
    }
    return ignore_case ? 0 : case_diff;
}

API int StrCmpW(const WCHAR *a, const WCHAR *b) { return lingcmp(a, b, (size_t)-1, 0); }
API int StrCmpIW(const WCHAR *a, const WCHAR *b) { return lingcmp(a, b, (size_t)-1, 1); }
API int StrCmpNW(const WCHAR *a, const WCHAR *b, int n) { return n <= 0 ? 0 : lingcmp(a, b, (size_t)n, 0); }
API int StrCmpNIW(const WCHAR *a, const WCHAR *b, int n) { return n <= 0 ? 0 : lingcmp(a, b, (size_t)n, 1); }

/* A comparisons: compare the first n characters of the UTF-8 strings. */
/* The A comparisons count `n` in bytes: compare the first n bytes of each
 * string (as UTF-16) with the same rules as the W functions. */
static int cmp_a(const char *a, const char *b, int n, int ic)
{
    char *ca = n < 0 ? strdup(a) : strndup(a, (size_t)n);
    char *cb = n < 0 ? strdup(b) : strndup(b, (size_t)n);
    WCHAR *x = lsiw_u2w(ca), *y = lsiw_u2w(cb);
    int r = (x && y) ? lingcmp(x, y, (size_t)-1, ic) : strcmp(ca, cb);
    free(x); free(y); free(ca); free(cb);
    return r;
}
API int StrCmpA(const char *a, const char *b) { return cmp_a(a, b, -1, 0); }
API int StrCmpIA(const char *a, const char *b) { return cmp_a(a, b, -1, 1); }
API int StrCmpNA(const char *a, const char *b, int n) { return n <= 0 ? 0 : cmp_a(a, b, n, 0); }
API int StrCmpNIA(const char *a, const char *b, int n) { return n <= 0 ? 0 : cmp_a(a, b, n, 1); }

API BOOL StrIsIntlEqualW(BOOL case_sens, const WCHAR *a, const WCHAR *b, int n)
{
    if (!a || !b) return FALSE;
    return lingcmp(a, b, n < 0 ? (size_t)-1 : (size_t)n, !case_sens) == 0;
}

API BOOL StrIsIntlEqualA(BOOL case_sens, const char *a, const char *b, int n)
{
    if (!a || !b) return FALSE;
    return cmp_a(a, b, n, !case_sens) == 0;
}

/* Natural ("logical") order as Explorer sorts names: digit runs compare by
 * value, everything else case-insensitively. */
API int StrCmpLogicalW(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    const WCHAR *p = a, *q = b;
    while (*p && *q) {
        if (*p >= '0' && *p <= '9' && *q >= '0' && *q <= '9') {
            while (*p == '0') p++;
            while (*q == '0') q++;
            const WCHAR *ps = p, *qs = q;
            while (*p >= '0' && *p <= '9') p++;
            while (*q >= '0' && *q <= '9') q++;
            size_t lp = (size_t)(p - ps), lq = (size_t)(q - qs);
            if (lp != lq) return lp < lq ? -1 : 1;
            for (size_t i = 0; i < lp; i++) if (ps[i] != qs[i]) return ps[i] < qs[i] ? -1 : 1;
            continue;
        }
        WCHAR x = wlower(*p), y = wlower(*q);
        if (x != y) return x < y ? -1 : 1;
        p++; q++;
    }
    if (*p || *q) return *p ? 1 : -1;
    return 0;
}

static const WCHAR *find_sub(const WCHAR *s, const WCHAR *sub, int ic)
{
    size_t n = wlen(sub);
    for (; *s; s++) {
        if (ic ? !wncmpi(s, sub, n) : !memcmp(s, sub, n * sizeof(WCHAR))) return s;
    }
    return n ? NULL : s;
}

API WCHAR *StrStrW(const WCHAR *s, const WCHAR *sub) { return (s && sub) ? (WCHAR *)find_sub(s, sub, 0) : NULL; }
API WCHAR *StrStrIW(const WCHAR *s, const WCHAR *sub) { return (s && sub) ? (WCHAR *)find_sub(s, sub, 1) : NULL; }
API char *StrStrA(const char *s, const char *sub) { return (s && sub) ? strstr(s, sub) : NULL; }
API char *StrStrIA(const char *s, const char *sub)
{
    if (!s || !sub) return NULL;
    WCHAR *w = lsiw_u2w(s), *x = lsiw_u2w(sub);
    char *r = a_ptr(s, w, find_sub(w, x, 1));
    free(w); free(x);
    return r;
}

API WCHAR *StrChrW(const WCHAR *s, WCHAR c) { if (!s) return NULL; for (; *s; s++) if (*s == c) return (WCHAR *)s; return NULL; }
API WCHAR *StrChrIW(const WCHAR *s, WCHAR c) { if (!s) return NULL; c = wlower(c); for (; *s; s++) if (wlower(*s) == c) return (WCHAR *)s; return NULL; }

/* ANSI characters are bytes; a WORD above 0xFF is a two-byte character
 * (lead byte in the high half). */
static char *chr_a(const char *s, WORD c, int ic, int last, const char *end)
{
    if (!s) return NULL;
    if (!end) end = s + strlen(s);
    char *found = NULL;
    for (const char *p = s; p < end && *p; p++) {
        int m;
        if (c > 0xFF) m = (unsigned char)p[0] == (c >> 8) && (unsigned char)p[1] == (c & 0xFF);
        else if (ic) m = tolower((unsigned char)*p) == tolower(c);
        else m = (unsigned char)*p == c;
        if (m) { found = (char *)p; if (!last) break; }
    }
    return found;
}

API char *StrChrA(const char *s, WORD c) { return chr_a(s, c, 0, 0, NULL); }
API char *StrChrIA(const char *s, WORD c) { return chr_a(s, c, 1, 0, NULL); }
API char *StrRChrA(const char *s, const char *end, WORD c) { return chr_a(s, c, 0, 1, end); }
API char *StrRChrIA(const char *s, const char *end, WORD c) { return chr_a(s, c, 1, 1, end); }

static WCHAR *rchr_w(const WCHAR *s, const WCHAR *end, WCHAR c, int ic)
{
    if (!s) return NULL;
    if (!end) end = s + wlen(s);
    WCHAR *found = NULL;
    if (ic) c = wlower(c);
    for (const WCHAR *p = s; p < end && *p; p++) if ((ic ? wlower(*p) : *p) == c) found = (WCHAR *)p;
    return found;
}
API WCHAR *StrRChrW(const WCHAR *s, const WCHAR *end, WCHAR c) { return rchr_w(s, end, c, 0); }
API WCHAR *StrRChrIW(const WCHAR *s, const WCHAR *end, WCHAR c) { return rchr_w(s, end, c, 1); }

API WCHAR *StrPBrkW(const WCHAR *s, const WCHAR *set)
{
    if (!s || !set) return NULL;
    for (; *s; s++) if (wchr(set, *s) && *s) return (WCHAR *)s;
    return NULL;
}
API char *StrPBrkA(const char *s, const char *set) { return (s && set) ? strpbrk(s, set) : NULL; }

static int span_w(const WCHAR *s, const WCHAR *set, int in, int ic)
{
    int n = 0;
    for (; s[n]; n++) {
        int found = 0;
        for (const WCHAR *c = set; *c && !found; c++) found = ic ? wlower(*c) == wlower(s[n]) : *c == s[n];
        if (found != in) break;
    }
    return n;
}
API int StrSpnW(const WCHAR *s, const WCHAR *set) { return (s && set) ? span_w(s, set, 1, 0) : 0; }
API int StrCSpnW(const WCHAR *s, const WCHAR *set) { return (s && set) ? span_w(s, set, 0, 0) : 0; }
API int StrCSpnIW(const WCHAR *s, const WCHAR *set) { return (s && set) ? span_w(s, set, 0, 1) : 0; }
API int StrSpnA(const char *s, const char *set) { return (s && set) ? (int)strspn(s, set) : 0; }
API int StrCSpnA(const char *s, const char *set) { return (s && set) ? (int)strcspn(s, set) : 0; }
API int StrCSpnIA(const char *s, const char *set)
{
    if (!s || !set) return 0;
    int n = 0;
    for (; s[n]; n++) {
        const char *c = set;
        for (; *c; c++) if (tolower((unsigned char)*c) == tolower((unsigned char)s[n])) break;
        if (*c) break;
    }
    return n;
}

API WCHAR *StrCpyW(WCHAR *d, const WCHAR *s) { if (d && s) memmove(d, s, (wlen(s) + 1) * sizeof(WCHAR)); return d; }
API WCHAR *StrCpyNW(WCHAR *d, const WCHAR *s, int n) { if (d && s && n > 0) wcopyn(d, s, (size_t)n); return d; }
API WCHAR *StrCatW(WCHAR *d, const WCHAR *s) { if (d && s) StrCpyW(d + wlen(d), s); return d; }
API WCHAR *StrNCatW(WCHAR *d, const WCHAR *s, int n) { if (d && s && n > 0) wcopyn(d + wlen(d), s, (size_t)n); return d; }
API char *StrNCatA(char *d, const char *s, int n)
{
    if (d && s && n > 0) {
        char *e = d + strlen(d);
        int i = 0;
        for (; i + 1 < n && s[i]; i++) e[i] = s[i];
        e[i] = 0;
    }
    return d;
}

API WCHAR *StrCatBuffW(WCHAR *d, const WCHAR *s, int cap)
{
    if (!d || !s || cap <= 0) return d;
    size_t l = wlen(d);
    if (l < (size_t)cap) wcopyn(d + l, s, (size_t)cap - l);
    return d;
}

API char *StrCatBuffA(char *d, const char *s, int cap)
{
    if (!d || !s || cap <= 0) return d;
    size_t l = strlen(d);
    if (l < (size_t)cap) snprintf(d + l, (size_t)cap - l, "%s", s);
    return d;
}

API WCHAR *StrDupW(const WCHAR *s) { return heap_wdup(s ? s : g_empty_w); }
API char *StrDupA(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *d = lsiw_heap_alloc(n, 0);
    if (d) memcpy(d, s, n);
    return d;
}

#define STIF_SUPPORT_HEX 1

static BOOL to_int64(const WCHAR *s, DWORD flags, LONGLONG *out, int skip_ws)
{
    if (!s) return FALSE;
    if (skip_ws) while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    ULONGLONG v = 0;
    int digits = 0;
    if ((flags & STIF_SUPPORT_HEX) && s[0] == '0' && (s[1] | 32) == 'x') {
        s += 2;
        for (; *s < 128 && hexval((char)*s) >= 0; s++, digits++) v = v * 16 + (ULONGLONG)hexval((char)*s);
    } else {
        for (; *s >= '0' && *s <= '9'; s++, digits++) v = v * 10 + (ULONGLONG)(*s - '0');
    }
    if (!digits) return FALSE;
    *out = neg ? -(LONGLONG)v : (LONGLONG)v;
    return TRUE;
}

API BOOL StrToInt64ExW(const WCHAR *s, DWORD flags, LONGLONG *ret)
{
    LONGLONG v = 0;
    BOOL ok = to_int64(s, flags, &v, 1);
    if (ret) *ret = v;
    return ok;
}

API BOOL StrToIntExW(const WCHAR *s, DWORD flags, int *ret)
{
    LONGLONG v = 0;
    BOOL ok = to_int64(s, flags, &v, 1);
    if (ret) *ret = (int)v;
    return ok;
}

API int StrToIntW(const WCHAR *s)
{
    LONGLONG v = 0;
    if (!s) return 0;
    to_int64(s, 0, &v, 0);
    return (int)v;
}

API BOOL StrToInt64ExA(const char *s, DWORD flags, LONGLONG *ret)
{
    WCHAR *w = lsiw_u2w(s);
    BOOL r = StrToInt64ExW(w, flags, ret);
    free(w);
    return r;
}
API BOOL StrToIntExA(const char *s, DWORD flags, int *ret)
{
    WCHAR *w = lsiw_u2w(s);
    BOOL r = StrToIntExW(w, flags, ret);
    free(w);
    return r;
}
API int StrToIntA(const char *s)
{
    WCHAR *w = lsiw_u2w(s);
    int r = StrToIntW(w);
    free(w);
    return r;
}

API BOOL StrTrimW(WCHAR *s, const WCHAR *trim)
{
    if (!s || !trim) return FALSE;
    WCHAR *b = s;
    while (*b && wchr(trim, *b)) b++;
    size_t n = wlen(b);
    while (n && wchr(trim, b[n - 1])) n--;
    BOOL changed = b != s || n != wlen(b);
    memmove(s, b, n * sizeof(WCHAR));
    s[n] = 0;
    return changed;
}

API BOOL StrTrimA(char *s, const char *trim)
{
    if (!s || !trim) return FALSE;
    char *b = s;
    while (*b && strchr(trim, *b)) b++;
    size_t n = strlen(b);
    while (n && strchr(trim, b[n - 1])) n--;
    BOOL changed = b != s || n != strlen(b);
    memmove(s, b, n);
    s[n] = 0;
    return changed;
}

/* "532 bytes", "1.30 KB", "22.9 KB", "2.28 MB": three significant digits,
 * a unit change once the value reaches 1000, truncated unless rounding is
 * asked for (StrFormatByteSizeEx). */
static void format_bytes(ULONGLONG v, int round, char *out, size_t cap)
{
    static const char *units[] = { "KB", "MB", "GB", "TB", "PB", "EB" };
    if (v < 1024) { snprintf(out, cap, "%llu bytes", (unsigned long long)v); return; }
    /* value in hundredths of the unit, computed without floating point */
    int u = 0;
    ULONGLONG div = 1024;
    while (u < 5 && v / div >= 1000) { div *= 1024; u++; }
    unsigned __int128 h100 = (unsigned __int128)v * 100;
    ULONGLONG whole = (ULONGLONG)(h100 / div), rem = (ULONGLONG)(h100 % div);
    int dec = whole < 1000 ? 2 : whole < 10000 ? 1 : 0;      /* hundredths: <10, <100, else */
    ULONGLONG scale = dec == 2 ? 1 : dec == 1 ? 10 : 100;
    ULONGLONG q = whole / scale;
    if (round) {
        unsigned __int128 rr = ((unsigned __int128)(whole % scale) * div + rem) * 2;
        if (rr >= (unsigned __int128)scale * div) q++;
    }
    if (dec == 2) snprintf(out, cap, "%llu.%02llu %s", (unsigned long long)(q / 100), (unsigned long long)(q % 100), units[u]);
    else if (dec == 1) snprintf(out, cap, "%llu.%llu %s", (unsigned long long)(q / 10), (unsigned long long)(q % 10), units[u]);
    else snprintf(out, cap, "%llu %s", (unsigned long long)q, units[u]);
}

API WCHAR *StrFormatByteSizeW(LONGLONG size, WCHAR *buf, UINT cch)
{
    if (!buf || !cch) return buf;
    char tmp[64];
    format_bytes((ULONGLONG)size, 0, tmp, sizeof tmp);
    if (!u2w_into(tmp, buf, cch)) { WCHAR *w = lsiw_u2w(tmp); wcopyn(buf, w, cch); free(w); }
    return buf;
}

API char *StrFormatByteSize64A(LONGLONG size, char *buf, UINT cch)
{
    if (!buf || !cch) return buf;
    char tmp[64];
    format_bytes((ULONGLONG)size, 0, tmp, sizeof tmp);
    snprintf(buf, cch, "%s", tmp);
    return buf;
}

API char *StrFormatByteSizeA(DWORD size, char *buf, UINT cch) { return StrFormatByteSize64A(size, buf, cch); }

#define SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT    1
#define SFBS_FLAGS_TRUNCATE_UNDISPLAYED_DECIMAL_DIGITS 2
API HRESULT StrFormatByteSizeEx(ULONGLONG size, int flags, WCHAR *buf, UINT cch)
{
    if (!buf || !cch) return E_INVALIDARG;
    if (flags != SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT && flags != SFBS_FLAGS_TRUNCATE_UNDISPLAYED_DECIMAL_DIGITS)
        return E_INVALIDARG;
    char tmp[64];
    format_bytes(size, flags == SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, tmp, sizeof tmp);
    return u2w_into(tmp, buf, cch) ? S_OK : STRSAFE_E_INSUFFICIENT_BUFFER;
}

/* "1,234 KB": kilobytes rounded up, thousands grouped. */
static void format_kb(LONGLONG size, char *out, size_t cap)
{
    ULONGLONG kb = ((ULONGLONG)size + 1023) / 1024;
    char digits[32], grouped[48];
    int n = snprintf(digits, sizeof digits, "%llu", (unsigned long long)kb), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) grouped[o++] = ',';
        grouped[o++] = digits[i];
    }
    grouped[o] = 0;
    snprintf(out, cap, "%s KB", grouped);
}

API WCHAR *StrFormatKBSizeW(LONGLONG size, WCHAR *buf, UINT cch)
{
    char tmp[64];
    format_kb(size, tmp, sizeof tmp);
    if (!buf || !u2w_into(tmp, buf, cch)) return NULL;
    return buf;
}

API char *StrFormatKBSizeA(LONGLONG size, char *buf, UINT cch)
{
    char tmp[64];
    format_kb(size, tmp, sizeof tmp);
    if (!buf || strlen(tmp) >= cch) return NULL;
    strcpy(buf, tmp);
    return buf;
}

/* ---- STRRET ---- */

#define STRRET_WSTR   0
#define STRRET_OFFSET 1
#define STRRET_CSTR   2
typedef struct { UINT uType; union { WCHAR *pOleStr; UINT uOffset; char cStr[MAX_PATH]; }; } STRRET;

/* UTF-8 copy of a STRRET's text; frees pOleStr as Windows does. */
static char *strret_text(STRRET *s, const void *pidl)
{
    char *r = NULL;
    switch (s->uType) {
    case STRRET_WSTR:
        r = lsiw_w2u(s->pOleStr ? s->pOleStr : g_empty_w);
        if (s->pOleStr) lsiw_heap_free(s->pOleStr);
        s->pOleStr = NULL;
        break;
    case STRRET_CSTR:
        r = strndup(s->cStr, MAX_PATH);
        break;
    case STRRET_OFFSET:
        r = pidl ? strdup((const char *)pidl + s->uOffset) : NULL;
        break;
    }
    return r;
}

API HRESULT StrRetToStrW(STRRET *s, const void *pidl, WCHAR **out)
{
    if (!s || !out) return E_INVALIDARG;
    *out = NULL;
    char *t = strret_text(s, pidl);
    if (!t) return E_FAIL;
    *out = heap_wdup_u(t);
    free(t);
    return *out ? S_OK : E_OUTOFMEMORY;
}

API HRESULT StrRetToStrA(STRRET *s, const void *pidl, char **out)
{
    if (!s || !out) return E_INVALIDARG;
    *out = NULL;
    char *t = strret_text(s, pidl);
    if (!t) return E_FAIL;
    size_t n = strlen(t) + 1;
    *out = lsiw_heap_alloc(n, 0);
    if (*out) memcpy(*out, t, n);
    free(t);
    return *out ? S_OK : E_OUTOFMEMORY;
}

API HRESULT StrRetToBufW(STRRET *s, const void *pidl, WCHAR *buf, UINT cch)
{
    if (!s || !buf || !cch) return E_INVALIDARG;
    buf[0] = 0;
    char *t = strret_text(s, pidl);
    if (!t) return E_FAIL;
    WCHAR *w = lsiw_u2w(t);
    wcopyn(buf, w, cch);
    free(w); free(t);
    return S_OK;
}

API HRESULT StrRetToBufA(STRRET *s, const void *pidl, char *buf, UINT cch)
{
    if (!s || !buf || !cch) return E_INVALIDARG;
    buf[0] = 0;
    char *t = strret_text(s, pidl);
    if (!t) return E_FAIL;
    snprintf(buf, cch, "%s", t);
    free(t);
    return S_OK;
}

/* ---- wnsprintf: at most cch-1 characters, -1 when truncated ---- */

API int wvnsprintfA(char *buf, int cch, const char *fmt, ms_va_list ap)
{
    if (!buf || cch <= 0 || !fmt) return -1;
    int n = lsiw_ms_vsnprintf(buf, (size_t)cch, fmt, ap, 0);
    return (n < 0 || n >= cch) ? -1 : n;
}

API int wvnsprintfW(WCHAR *buf, int cch, const WCHAR *fmt, ms_va_list ap)
{
    if (!buf || cch <= 0 || !fmt) return -1;
    return lsiw_ms_vsnwprintf(buf, (size_t)cch, fmt, ap);
}

static int WINAPI wnsprintfA(char *buf, int cch, const char *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    int r = wvnsprintfA(buf, cch, fmt, ap);
    ms_va_end(ap);
    return r;
}

static int WINAPI wnsprintfW(WCHAR *buf, int cch, const WCHAR *fmt, ...)
{
    ms_va_list ap;
    ms_va_start(ap, fmt);
    int r = wvnsprintfW(buf, cch, fmt, ap);
    ms_va_end(ap);
    return r;
}

/* ================================================================== */
/* shlwapi: registry shortcuts over advapi32                            */
/* ================================================================== */

#define REG_SZ        1
#define REG_EXPAND_SZ 2
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE   0x0002
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_ALL_ACCESS  0xF003F

typedef LSTATUS (WINAPI *reg_open_w)(HKEY, const WCHAR *, DWORD, DWORD, HKEY *);
typedef LSTATUS (WINAPI *reg_query_w)(HKEY, const WCHAR *, DWORD *, DWORD *, BYTE *, DWORD *);
typedef LSTATUS (WINAPI *reg_close)(HKEY);

/* RegQueryValueEx plus SHQueryValueEx's extra: REG_EXPAND_SZ data comes
 * back expanded and typed REG_SZ. Wide data. */
static LSTATUS query_value_w(HKEY key, const WCHAR *value, DWORD *type, void *data, DWORD *cb)
{
    reg_query_w q = adv("RegQueryValueExW");
    if (!q) return ERROR_CALL_NOT_IMPLEMENTED;
    DWORD t = 0, size = 0;
    LSTATUS r = q(key, value, NULL, &t, NULL, &size);
    if (r) return r;
    if (t != REG_EXPAND_SZ) {
        if (type) *type = t;
        return q(key, value, NULL, NULL, data, cb);
    }
    BYTE *raw = calloc(size + 4, 1);
    r = q(key, value, NULL, NULL, raw, &size);
    if (r) { free(raw); return r; }
    DWORD (WINAPI *ex)(const WCHAR *, WCHAR *, DWORD) = k32("ExpandEnvironmentStringsW");
    DWORD n = ex ? ex((WCHAR *)raw, NULL, 0) : (DWORD)wlen((WCHAR *)raw) + 1;
    WCHAR *exp = calloc(n + 1, sizeof(WCHAR));
    if (ex) ex((WCHAR *)raw, exp, n + 1); else memcpy(exp, raw, size);
    free(raw);
    DWORD need = (DWORD)((wlen(exp) + 1) * sizeof(WCHAR));
    if (type) *type = REG_SZ;
    if (cb) {
        if (data && *cb < need) r = ERROR_MORE_DATA;
        else if (data) memcpy(data, exp, need);
        *cb = need;
    }
    free(exp);
    return r;
}

API LSTATUS SHQueryValueExW(HKEY key, const WCHAR *value, DWORD *reserved, DWORD *type, void *data, DWORD *cb)
{
    (void)reserved;
    return query_value_w(key, value, type, data, cb);
}

API LSTATUS SHGetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, DWORD *type, void *data, DWORD *cb)
{
    reg_open_w o = adv("RegOpenKeyExW");
    reg_close c = adv("RegCloseKey");
    if (!o || !c) return ERROR_CALL_NOT_IMPLEMENTED;
    HKEY k = key;
    if (sub && *sub) { LSTATUS r = o(key, sub, 0, KEY_QUERY_VALUE, &k); if (r) return r; }
    LSTATUS r = query_value_w(k, value, type, data, cb);
    if (k != key) c(k);
    return r;
}

/* ANSI: the wide value is converted; string data is converted to UTF-8. */
static LSTATUS get_value_a(HKEY key, const char *sub, const char *value, DWORD *type, void *data, DWORD *cb, int query_only)
{
    WCHAR *ws = lsiw_u2w(sub), *wv = lsiw_u2w(value);
    DWORD t = 0, size = 0;
    LSTATUS r = query_only ? query_value_w(key, wv, &t, NULL, &size) : SHGetValueW(key, ws, wv, &t, NULL, &size);
    BYTE *buf = NULL;
    if (!r) {
        buf = calloc(size + 4, 1);
        r = query_only ? query_value_w(key, wv, &t, buf, &size) : SHGetValueW(key, ws, wv, &t, buf, &size);
    }
    if (!r) {
        if (type) *type = t;
        if (t == REG_SZ || t == REG_EXPAND_SZ || t == 7 /* REG_MULTI_SZ */) {
            size_t nch = size / 2;
            DWORD need = (DWORD)lsiw_utf16_to_utf8((WCHAR *)buf, (int)nch, NULL, 0);
            if (!nch || ((WCHAR *)buf)[nch - 1]) need++;
            if (cb) {
                if (data && *cb < need) r = ERROR_MORE_DATA;
                else if (data) { lsiw_utf16_to_utf8((WCHAR *)buf, (int)nch, data, (int)need); ((char *)data)[need - 1] = 0; }
                *cb = need;
            }
        } else if (cb) {
            if (data && *cb < size) r = ERROR_MORE_DATA;
            else if (data) memcpy(data, buf, size);
            *cb = size;
        }
    }
    free(buf); free(ws); free(wv);
    return r;
}

API LSTATUS SHGetValueA(HKEY key, const char *sub, const char *value, DWORD *type, void *data, DWORD *cb)
{ return get_value_a(key, sub, value, type, data, cb, 0); }
API LSTATUS SHQueryValueExA(HKEY key, const char *value, DWORD *reserved, DWORD *type, void *data, DWORD *cb)
{ (void)reserved; return get_value_a(key, NULL, value, type, data, cb, 1); }

API LSTATUS SHSetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, DWORD type, const void *data, DWORD cb)
{
    LSTATUS (WINAPI *create)(HKEY, const WCHAR *, DWORD, WCHAR *, DWORD, DWORD, void *, HKEY *, DWORD *) = adv("RegCreateKeyExW");
    LSTATUS (WINAPI *set)(HKEY, const WCHAR *, DWORD, DWORD, const void *, DWORD) = adv("RegSetValueExW");
    reg_close c = adv("RegCloseKey");
    if (!create || !set || !c) return ERROR_CALL_NOT_IMPLEMENTED;
    HKEY k = key;
    if (sub && *sub) { LSTATUS r = create(key, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL); if (r) return r; }
    LSTATUS r = set(k, value, 0, type, data, cb);
    if (k != key) c(k);
    return r;
}

API LSTATUS SHSetValueA(HKEY key, const char *sub, const char *value, DWORD type, const void *data, DWORD cb)
{
    WCHAR *ws = lsiw_u2w(sub), *wv = lsiw_u2w(value), *wd = NULL;
    const void *d = data;
    if ((type == REG_SZ || type == REG_EXPAND_SZ) && data) {
        char *s = strndup(data, cb);
        wd = lsiw_u2w(s);
        free(s);
        d = wd;
        cb = (DWORD)((wlen(wd) + 1) * sizeof(WCHAR));
    }
    LSTATUS r = SHSetValueW(key, ws, wv, type, d, cb);
    free(ws); free(wv); free(wd);
    return r;
}

/* Recursive key deletion, by RegDeleteTreeW or by walking the subkeys. */
static LSTATUS delete_key_tree(HKEY key, const WCHAR *sub)
{
    LSTATUS (WINAPI *tree)(HKEY, const WCHAR *) = adv("RegDeleteTreeW");
    if (tree) return tree(key, sub);
    reg_open_w o = adv("RegOpenKeyExW");
    reg_close c = adv("RegCloseKey");
    LSTATUS (WINAPI *en)(HKEY, DWORD, WCHAR *, DWORD *, DWORD *, WCHAR *, DWORD *, void *) = adv("RegEnumKeyExW");
    LSTATUS (WINAPI *del)(HKEY, const WCHAR *) = adv("RegDeleteKeyW");
    if (!o || !c || !en || !del) return ERROR_CALL_NOT_IMPLEMENTED;
    HKEY k;
    LSTATUS r = o(key, sub, 0, KEY_ALL_ACCESS, &k);
    if (r) return r;
    WCHAR name[256];
    for (;;) {
        DWORD n = 256;
        if (en(k, 0, name, &n, NULL, NULL, NULL, NULL)) break;
        if ((r = delete_key_tree(k, name))) break;
    }
    c(k);
    return r ? r : del(key, sub);
}

API LSTATUS SHDeleteKeyW(HKEY key, const WCHAR *sub) { return delete_key_tree(key, sub); }
API LSTATUS SHDeleteKeyA(HKEY key, const char *sub)
{
    WCHAR *w = lsiw_u2w(sub);
    LSTATUS r = delete_key_tree(key, w);
    free(w);
    return r;
}

API LSTATUS SHDeleteValueW(HKEY key, const WCHAR *sub, const WCHAR *value)
{
    reg_open_w o = adv("RegOpenKeyExW");
    reg_close c = adv("RegCloseKey");
    LSTATUS (WINAPI *del)(HKEY, const WCHAR *) = adv("RegDeleteValueW");
    if (!o || !c || !del) return ERROR_CALL_NOT_IMPLEMENTED;
    HKEY k = key;
    if (sub && *sub) { LSTATUS r = o(key, sub, 0, KEY_SET_VALUE, &k); if (r) return r; }
    LSTATUS r = del(k, value);
    if (k != key) c(k);
    return r;
}

API LSTATUS SHDeleteValueA(HKEY key, const char *sub, const char *value)
{
    WCHAR *ws = lsiw_u2w(sub), *wv = lsiw_u2w(value);
    LSTATUS r = SHDeleteValueW(key, ws, wv);
    free(ws); free(wv);
    return r;
}

/* SRRF_RM_* bits share values with RRF_SUBKEY_WOW64* and are dropped. */
#define SRRF_RM_MASK 0x00070000
API LSTATUS SHRegGetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, DWORD flags, DWORD *type, void *data, DWORD *cb)
{
    LSTATUS (WINAPI *f)(HKEY, const WCHAR *, const WCHAR *, DWORD, DWORD *, void *, DWORD *) = adv("RegGetValueW");
    if (!f) return ERROR_CALL_NOT_IMPLEMENTED;
    return f(key, sub, value, flags & ~(DWORD)SRRF_RM_MASK, type, data, cb);
}

API LSTATUS SHRegGetValueA(HKEY key, const char *sub, const char *value, DWORD flags, DWORD *type, void *data, DWORD *cb)
{
    LSTATUS (WINAPI *f)(HKEY, const char *, const char *, DWORD, DWORD *, void *, DWORD *) = adv("RegGetValueA");
    if (!f) return ERROR_CALL_NOT_IMPLEMENTED;
    return f(key, sub, value, flags & ~(DWORD)SRRF_RM_MASK, type, data, cb);
}

/* No Windows file associations exist under LSIW. */
API HRESULT AssocQueryStringW(DWORD flags, int str, const WCHAR *assoc, const WCHAR *extra, WCHAR *out, DWORD *pcch)
{
    (void)flags; (void)str; (void)assoc; (void)extra;
    if (out && pcch && *pcch) out[0] = 0;
    return HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION);
}

API HRESULT AssocQueryStringA(DWORD flags, int str, const char *assoc, const char *extra, char *out, DWORD *pcch)
{
    (void)flags; (void)str; (void)assoc; (void)extra;
    if (out && pcch && *pcch) out[0] = 0;
    return HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION);
}

/* ================================================================== */
/* shlwapi: miscellany                                                  */
/* ================================================================== */

/* LSIW presents itself as 64-bit Windows 10 Pro workstation, not joined to
 * a domain. */
API BOOL IsOS(DWORD os)
{
    switch (os) {
    case 1:  /* OS_NT */
    case 2:  /* OS_WIN95ORGREATER */
    case 3:  /* OS_NT4ORGREATER */
    case 5:  /* OS_WIN98ORGREATER */
    case 7:  /* OS_WIN2000ORGREATER */
    case 8:  /* OS_WIN2000PRO */
    case 18: /* OS_XPORGREATER */
    case 20: /* OS_PROFESSIONAL */
    case 26: /* OS_FASTUSERSWITCHING */
    case 27: /* OS_WELCOMELOGONUI */
        return TRUE;
    default: /* 9x, server, embedded, terminal, home, domain, WOW64, tablet, media center */
        return FALSE;
    }
}

API HRESULT SHAutoComplete(HWND hwnd, DWORD flags) { (void)hwnd; (void)flags; return S_OK; }

/* RGB <-> HLS on the 0..240 scale, following Microsoft's published
 * conversion algorithm (achromatic colours get hue 160). */
#define HLSMAX 240
#define RGBMAX 255
#define GetR(c) ((int)((c) & 0xFF))
#define GetG(c) ((int)(((c) >> 8) & 0xFF))
#define GetB(c) ((int)(((c) >> 16) & 0xFF))

API void ColorRGBToHLS(COLORREF c, WORD *ph, WORD *pl, WORD *ps)
{
    int r = GetR(c), g = GetG(c), b = GetB(c);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int l = ((mx + mn) * HLSMAX + RGBMAX) / (2 * RGBMAX), h, s;
    if (mx == mn) { s = 0; h = HLSMAX * 2 / 3; }
    else {
        if (l <= HLSMAX / 2) s = ((mx - mn) * HLSMAX + (mx + mn) / 2) / (mx + mn);
        else s = ((mx - mn) * HLSMAX + (2 * RGBMAX - mx - mn) / 2) / (2 * RGBMAX - mx - mn);
        int rd = ((mx - r) * (HLSMAX / 6) + (mx - mn) / 2) / (mx - mn);
        int gd = ((mx - g) * (HLSMAX / 6) + (mx - mn) / 2) / (mx - mn);
        int bd = ((mx - b) * (HLSMAX / 6) + (mx - mn) / 2) / (mx - mn);
        if (r == mx) h = bd - gd;
        else if (g == mx) h = HLSMAX / 3 + rd - bd;
        else h = 2 * HLSMAX / 3 + gd - rd;
        if (h < 0) h += HLSMAX;
        if (h > HLSMAX) h -= HLSMAX;
    }
    if (ph) *ph = (WORD)h;
    if (pl) *pl = (WORD)l;
    if (ps) *ps = (WORD)s;
}

static int hue_to_rgb(int n1, int n2, int hue)
{
    if (hue < 0) hue += HLSMAX;
    if (hue > HLSMAX) hue -= HLSMAX;
    if (hue < HLSMAX / 6) return n1 + ((n2 - n1) * hue + HLSMAX / 12) / (HLSMAX / 6);
    if (hue < HLSMAX / 2) return n2;
    if (hue < HLSMAX * 2 / 3) return n1 + ((n2 - n1) * (HLSMAX * 2 / 3 - hue) + HLSMAX / 12) / (HLSMAX / 6);
    return n1;
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

API COLORREF ColorHLSToRGB(WORD h, WORD l, WORD s)
{
    int r, g, b;
    if (s == 0) r = g = b = (l * RGBMAX) / HLSMAX;
    else {
        int m2 = l <= HLSMAX / 2 ? (l * (HLSMAX + s) + HLSMAX / 2) / HLSMAX : l + s - (l * s + HLSMAX / 2) / HLSMAX;
        int m1 = 2 * l - m2;
        r = (hue_to_rgb(m1, m2, h + HLSMAX / 3) * RGBMAX + HLSMAX / 2) / HLSMAX;
        g = (hue_to_rgb(m1, m2, h) * RGBMAX + HLSMAX / 2) / HLSMAX;
        b = (hue_to_rgb(m1, m2, h - HLSMAX / 3) * RGBMAX + HLSMAX / 2) / HLSMAX;
    }
    return (COLORREF)(clamp255(r) | clamp255(g) << 8 | clamp255(b) << 16);
}

/* Luminance change in 1/1000 of the range (fScale) or in HLS units. */
API COLORREF ColorAdjustLuma(COLORREF c, int n, BOOL scale)
{
    if (!n) return c;
    WORD h, l, s;
    ColorRGBToHLS(c, &h, &l, &s);
    int nl = l;
    if (scale) nl = n > 0 ? l + ((HLSMAX - l) * n) / 1000 : l + (l * n) / 1000;
    else nl = l + n;
    if (nl < 0) nl = 0;
    if (nl > HLSMAX) nl = HLSMAX;
    return ColorHLSToRGB(h, (WORD)nl, s);
}

/* HashData: a Pearson hash (one 256-entry permutation, one pass per output
 * byte). The permutation is LSIW's own, so values are stable across runs
 * but are not the same bytes Windows produces. */
static BYTE g_pearson[256];
static pthread_once_t g_pearson_once = PTHREAD_ONCE_INIT;
static void pearson_init(void)
{
    for (int i = 0; i < 256; i++) g_pearson[i] = (BYTE)i;
    uint32_t x = 0x9E3779B9u;
    for (int i = 255; i > 0; i--) {
        x = x * 1103515245u + 12345u;
        int j = (int)((x >> 8) % (uint32_t)(i + 1));
        BYTE t = g_pearson[i]; g_pearson[i] = g_pearson[j]; g_pearson[j] = t;
    }
}

API HRESULT HashData(const BYTE *data, DWORD cb, BYTE *hash, DWORD cbhash)
{
    if (!data || !cb || !hash || !cbhash || cbhash > 256) return E_INVALIDARG;
    pthread_once(&g_pearson_once, pearson_init);
    for (DWORD i = 0; i < cbhash; i++) {
        BYTE h = g_pearson[(data[0] + i) & 0xFF];
        for (DWORD j = 1; j < cb; j++) h = g_pearson[h ^ data[j]];
        hash[i] = h;
    }
    return S_OK;
}

/* ================================================================== */
/* IStream over a file (SHCreateStreamOnFile*) and over memory          */
/* (SHCreateMemStream), plus the shlwapi IStream_* helpers              */
/* ================================================================== */

#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr)    ((HRESULT)(hr) < 0)

#define STGM_READ            0x00000000
#define STGM_WRITE           0x00000001
#define STGM_READWRITE       0x00000002
#define STGM_ACCESS_MASK     0x00000003
#define STGM_CREATE          0x00001000
#define STGM_CONVERT         0x00020000
#define STGM_TRANSACTED      0x00010000
#define STGM_DELETEONRELEASE 0x04000000
#define STGTY_STREAM         2
#define STATFLAG_NONAME      1
#define STREAM_SEEK_SET      0
#define STREAM_SEEK_CUR      1
#define STREAM_SEEK_END      2
#define STG_E_MEDIUMFULL     ((HRESULT)0x80030070u)
#define STG_E_WRITEFAULT     ((HRESULT)0x8003001Du)
#define STG_E_READFAULT      ((HRESULT)0x8003001Eu)

typedef struct { DWORD lo, hi; } FILETIME_;
typedef struct {
    WCHAR *pwcsName; DWORD type; ULONGLONG cbSize; FILETIME_ mtime, ctime, atime;
    DWORD grfMode, grfLocksSupported; GUID clsid; DWORD grfStateBits, reserved;
} STATSTG;
_Static_assert(sizeof(STATSTG) == 80, "STATSTG layout");
_Static_assert(__builtin_offsetof(STATSTG, cbSize) == 16, "STATSTG layout");
_Static_assert(__builtin_offsetof(STATSTG, grfMode) == 48, "STATSTG layout");

struct stream;
struct stream_vtbl {
    HRESULT (WINAPI *QueryInterface)(struct stream *, const GUID *, void **);
    ULONG (WINAPI *AddRef)(struct stream *);
    ULONG (WINAPI *Release)(struct stream *);
    HRESULT (WINAPI *Read)(struct stream *, void *, ULONG, ULONG *);
    HRESULT (WINAPI *Write)(struct stream *, const void *, ULONG, ULONG *);
    HRESULT (WINAPI *Seek)(struct stream *, LONGLONG, DWORD, ULONGLONG *);
    HRESULT (WINAPI *SetSize)(struct stream *, ULONGLONG);
    HRESULT (WINAPI *CopyTo)(struct stream *, struct stream *, ULONGLONG, ULONGLONG *, ULONGLONG *);
    HRESULT (WINAPI *Commit)(struct stream *, DWORD);
    HRESULT (WINAPI *Revert)(struct stream *);
    HRESULT (WINAPI *LockRegion)(struct stream *, ULONGLONG, ULONGLONG, DWORD);
    HRESULT (WINAPI *UnlockRegion)(struct stream *, ULONGLONG, ULONGLONG, DWORD);
    HRESULT (WINAPI *Stat)(struct stream *, STATSTG *, DWORD);
    HRESULT (WINAPI *Clone)(struct stream *, struct stream **);
};

/* One object type serves both kinds: fd >= 0 for a file stream, otherwise
 * the data lives in buf. Each object has its own seek pointer, so a clone
 * of a file stream (a dup'd fd) moves independently, as on Windows. Calls
 * on one object are serialised by its mutex. */
struct stream {
    const struct stream_vtbl *vtbl;
    LONG ref;
    pthread_mutex_t lock;
    int fd;
    DWORD mode;
    char *linux_path;          /* file streams: for DELETEONRELEASE          */
    WCHAR *name;               /* file streams: the Windows path given       */
    BYTE *buf;                 /* memory streams                             */
    size_t size, cap;
    ULONGLONG pos;
};

static const struct stream_vtbl g_stream_vtbl;

static struct stream *stream_new(void)
{
    struct stream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->vtbl = &g_stream_vtbl;
    s->ref = 1;
    s->fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

static void stream_destroy(struct stream *s)
{
    if (s->fd >= 0) close(s->fd);
    if ((s->mode & STGM_DELETEONRELEASE) && s->linux_path) unlink(s->linux_path);
    free(s->linux_path);
    free(s->name);
    free(s->buf);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

static void filetime_from_ts(const struct timespec *ts, FILETIME_ *ft)
{
    ULONGLONG t = ((ULONGLONG)ts->tv_sec + 11644473600ull) * 10000000ull + (ULONGLONG)ts->tv_nsec / 100;
    ft->lo = (DWORD)t;
    ft->hi = (DWORD)(t >> 32);
}

/* Grow a memory stream's buffer to hold `n` bytes (new bytes are zero). */
static int mem_reserve(struct stream *s, size_t n)
{
    if (n <= s->cap) return 1;
    size_t c = s->cap ? s->cap : 256;
    while (c < n) c *= 2;
    BYTE *b = realloc(s->buf, c);
    if (!b) return 0;
    memset(b + s->cap, 0, c - s->cap);
    s->buf = b;
    s->cap = c;
    return 1;
}

API HRESULT stream_qi(struct stream *s, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    if (iid && (guid_eq(iid, &IID_IUnknown_) || guid_eq(iid, &IID_ISequentialStream_) || guid_eq(iid, &IID_IStream_))) {
        __atomic_add_fetch(&s->ref, 1, __ATOMIC_SEQ_CST);
        *out = s;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

API ULONG stream_addref(struct stream *s) { return (ULONG)__atomic_add_fetch(&s->ref, 1, __ATOMIC_SEQ_CST); }

API ULONG stream_release(struct stream *s)
{
    LONG r = __atomic_sub_fetch(&s->ref, 1, __ATOMIC_SEQ_CST);
    if (r == 0) stream_destroy(s);
    return (ULONG)(r < 0 ? 0 : r);
}

/* ISequentialStream::Read: S_FALSE when fewer bytes than asked were read
 * (end of stream), as the interface documents. */
API HRESULT stream_read(struct stream *s, void *buf, ULONG cb, ULONG *got)
{
    if (got) *got = 0;
    if (!buf && cb) return STG_E_INVALIDPOINTER;
    if ((s->mode & STGM_ACCESS_MASK) == STGM_WRITE) return STG_E_ACCESSDENIED;
    HRESULT hr = S_OK;
    ULONG n = 0;
    pthread_mutex_lock(&s->lock);
    if (s->fd >= 0) {
        while (n < cb) {
            ssize_t r = pread(s->fd, (BYTE *)buf + n, cb - n, (off_t)(s->pos + n));
            if (r < 0) { if (errno == EINTR) continue; hr = STG_E_READFAULT; break; }
            if (r == 0) break;
            n += (ULONG)r;
        }
    } else if (s->pos < s->size) {
        size_t avail = s->size - (size_t)s->pos;
        n = cb < avail ? cb : (ULONG)avail;
        memcpy(buf, s->buf + s->pos, n);
    }
    s->pos += n;
    pthread_mutex_unlock(&s->lock);
    if (got) *got = n;
    if (hr == S_OK && n < cb) hr = S_FALSE;
    return hr;
}

API HRESULT stream_write(struct stream *s, const void *buf, ULONG cb, ULONG *put)
{
    if (put) *put = 0;
    if (!buf && cb) return STG_E_INVALIDPOINTER;
    if ((s->mode & STGM_ACCESS_MASK) == STGM_READ) return STG_E_ACCESSDENIED;
    HRESULT hr = S_OK;
    ULONG n = 0;
    pthread_mutex_lock(&s->lock);
    if (s->fd >= 0) {
        while (n < cb) {
            ssize_t r = pwrite(s->fd, (const BYTE *)buf + n, cb - n, (off_t)(s->pos + n));
            if (r < 0) {
                if (errno == EINTR) continue;
                hr = errno == ENOSPC ? STG_E_MEDIUMFULL : STG_E_WRITEFAULT;
                break;
            }
            n += (ULONG)r;
        }
    } else {
        if (s->pos + cb > SIZE_MAX / 2 || !mem_reserve(s, (size_t)s->pos + cb)) hr = E_OUTOFMEMORY;
        else {
            memcpy(s->buf + s->pos, buf, cb);
            n = cb;
            if (s->pos + cb > s->size) s->size = (size_t)s->pos + cb;
        }
    }
    s->pos += n;
    pthread_mutex_unlock(&s->lock);
    if (put) *put = n;
    return hr;
}

static ULONGLONG stream_size_locked(struct stream *s)
{
    if (s->fd < 0) return s->size;
    struct stat st;
    return fstat(s->fd, &st) == 0 ? (ULONGLONG)st.st_size : 0;
}

API HRESULT stream_seek(struct stream *s, LONGLONG move, DWORD origin, ULONGLONG *newpos)
{
    pthread_mutex_lock(&s->lock);
    LONGLONG base;
    switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = (LONGLONG)s->pos; break;
    case STREAM_SEEK_END: base = (LONGLONG)stream_size_locked(s); break;
    default:
        pthread_mutex_unlock(&s->lock);
        return STG_E_INVALIDFUNCTION;
    }
    LONGLONG np = base + move;
    HRESULT hr = S_OK;
    if (np < 0) hr = STG_E_INVALIDFUNCTION;    /* seeking before the start */
    else s->pos = (ULONGLONG)np;
    if (newpos) *newpos = s->pos;
    pthread_mutex_unlock(&s->lock);
    return hr;
}

API HRESULT stream_setsize(struct stream *s, ULONGLONG size)
{
    if ((s->mode & STGM_ACCESS_MASK) == STGM_READ) return STG_E_ACCESSDENIED;
    HRESULT hr = S_OK;
    pthread_mutex_lock(&s->lock);
    if (s->fd >= 0) {
        if (ftruncate(s->fd, (off_t)size) < 0) hr = errno == ENOSPC ? STG_E_MEDIUMFULL : STG_E_WRITEFAULT;
    } else if (size > SIZE_MAX / 2 || !mem_reserve(s, (size_t)size)) {
        hr = E_OUTOFMEMORY;
    } else {
        if (size < s->size) memset(s->buf + size, 0, s->size - (size_t)size);
        s->size = (size_t)size;
    }
    pthread_mutex_unlock(&s->lock);
    return hr;
}

/* CopyTo works through the destination's own vtable, so the target may be
 * any IStream (one of ours or one the program implements). */
API HRESULT stream_copyto(struct stream *s, struct stream *dst, ULONGLONG cb, ULONGLONG *rd, ULONGLONG *wr)
{
    if (rd) *rd = 0;
    if (wr) *wr = 0;
    if (!dst) return STG_E_INVALIDPOINTER;
    BYTE chunk[16384];
    ULONGLONG tr = 0, tw = 0;
    HRESULT hr = S_OK;
    while (tr < cb) {
        ULONG want = cb - tr < sizeof chunk ? (ULONG)(cb - tr) : (ULONG)sizeof chunk, got = 0, put = 0;
        hr = s->vtbl->Read(s, chunk, want, &got);
        if (FAILED(hr)) break;
        tr += got;
        if (!got) { hr = S_OK; break; }
        hr = dst->vtbl->Write(dst, chunk, got, &put);
        tw += put;
        if (FAILED(hr) || put < got) break;
    }
    if (hr == S_FALSE) hr = S_OK;
    if (rd) *rd = tr;
    if (wr) *wr = tw;
    return hr;
}

/* Not transacted (STGM_TRANSACTED is refused at creation): Commit flushes,
 * Revert has nothing to undo. */
API HRESULT stream_commit(struct stream *s, DWORD flags)
{
    (void)flags;
    if (s->fd >= 0) fdatasync(s->fd);
    return S_OK;
}
API HRESULT stream_revert(struct stream *s) { (void)s; return S_OK; }

/* Range locking is not supported by these streams, which Windows reports
 * with STG_E_INVALIDFUNCTION (grfLocksSupported in Stat is 0). */
API HRESULT stream_lock(struct stream *s, ULONGLONG off, ULONGLONG cb, DWORD type)
{
    (void)s; (void)off; (void)cb; (void)type;
    return STG_E_INVALIDFUNCTION;
}
API HRESULT stream_unlock(struct stream *s, ULONGLONG off, ULONGLONG cb, DWORD type)
{
    (void)s; (void)off; (void)cb; (void)type;
    return STG_E_INVALIDFUNCTION;
}

API HRESULT stream_stat(struct stream *s, STATSTG *st, DWORD flags)
{
    if (!st) return STG_E_INVALIDPOINTER;
    memset(st, 0, sizeof *st);
    st->type = STGTY_STREAM;
    st->grfMode = s->mode;
    pthread_mutex_lock(&s->lock);
    if (s->fd >= 0) {
        struct stat fs;
        if (fstat(s->fd, &fs) == 0) {
            st->cbSize = (ULONGLONG)fs.st_size;
            filetime_from_ts(&fs.st_mtim, &st->mtime);
            filetime_from_ts(&fs.st_ctim, &st->ctime);
            filetime_from_ts(&fs.st_atim, &st->atime);
        }
    } else {
        st->cbSize = s->size;
    }
    pthread_mutex_unlock(&s->lock);
    if (!(flags & STATFLAG_NONAME) && s->name) {
        /* The file name only; the caller frees it with CoTaskMemFree. */
        const WCHAR *base = s->name;
        for (const WCHAR *p = s->name; *p; p++) if (is_sep(*p)) base = p + 1;
        st->pwcsName = heap_wdup(base);
        if (!st->pwcsName) return E_OUTOFMEMORY;
    }
    return S_OK;
}

API HRESULT stream_clone(struct stream *s, struct stream **out)
{
    if (!out) return STG_E_INVALIDPOINTER;
    *out = NULL;
    /* Windows' shlwapi memory streams do not implement Clone. */
    if (s->fd < 0) return E_NOTIMPL;
    struct stream *c = stream_new();
    if (!c) return E_OUTOFMEMORY;
    c->fd = fcntl(s->fd, F_DUPFD_CLOEXEC, 0);
    if (c->fd < 0) { HRESULT hr = hr_from_errno(errno); stream_destroy(c); return hr; }
    c->mode = s->mode & ~(DWORD)STGM_DELETEONRELEASE;   /* only the original deletes */
    c->name = s->name ? wdup(s->name) : NULL;
    pthread_mutex_lock(&s->lock);
    c->pos = s->pos;
    pthread_mutex_unlock(&s->lock);
    *out = c;
    return S_OK;
}

static const struct stream_vtbl g_stream_vtbl = {
    stream_qi, stream_addref, stream_release, stream_read, stream_write, stream_seek,
    stream_setsize, stream_copyto, stream_commit, stream_revert, stream_lock,
    stream_unlock, stream_stat, stream_clone,
};

/* SHCreateStreamOnFileEx: fCreate and STGM_CREATE choose the disposition
 * like CreateFile: STGM_CREATE -> CREATE_ALWAYS; fCreate alone -> CREATE_NEW
 * (fails if the file exists); neither -> OPEN_EXISTING. */
API HRESULT SHCreateStreamOnFileEx(const WCHAR *file, DWORD mode, DWORD attrs, BOOL create,
                                   struct stream *tmpl, struct stream **out)
{
    (void)tmpl;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!file) return E_INVALIDARG;
    if (mode & (STGM_TRANSACTED | STGM_CONVERT)) return E_INVALIDARG;
    DWORD acc = mode & STGM_ACCESS_MASK;
    if (acc == STGM_ACCESS_MASK) return E_INVALIDARG;

    int flags = O_CLOEXEC | (acc == STGM_READ ? O_RDONLY : acc == STGM_WRITE ? O_WRONLY : O_RDWR);
    if (mode & STGM_CREATE) flags |= O_CREAT | O_TRUNC;
    else if (create) flags |= O_CREAT | O_EXCL;
    /* O_TRUNC needs write access on Linux; the stream's own mode still
     * refuses writes for a read-only stream. */
    if ((flags & O_CREAT) && acc == STGM_READ) flags = (flags & ~O_ACCMODE) | O_RDWR;

    char *lin = lsiw_path_w(file);
    if (!lin) return E_OUTOFMEMORY;
    struct stat st;
    if (stat(lin, &st) == 0 && S_ISDIR(st.st_mode)) {
        free(lin);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
    mode_t perm = (attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    int fd = open(lin, flags, perm);
    if (fd < 0) {
        int e = errno;
        HRESULT hr = e == EEXIST ? HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) : hr_from_errno(e);
        free(lin);
        return hr;
    }
    struct stream *s = stream_new();
    if (!s) { close(fd); free(lin); return E_OUTOFMEMORY; }
    s->fd = fd;
    s->mode = mode;
    s->linux_path = lin;
    s->name = wdup(file);
    *out = s;
    return S_OK;
}

API HRESULT SHCreateStreamOnFileW(const WCHAR *file, DWORD mode, struct stream **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!file) return E_INVALIDARG;
    return SHCreateStreamOnFileEx(file, mode, FILE_ATTRIBUTE_NORMAL, FALSE, NULL, out);
}

API HRESULT SHCreateStreamOnFileA(const char *file, DWORD mode, struct stream **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!file) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    WCHAR *w = lsiw_u2w(file);
    if (!w) return E_OUTOFMEMORY;
    HRESULT hr = SHCreateStreamOnFileW(w, mode, out);
    free(w);
    return hr;
}

/* SHCreateMemStream copies the initial data; NULL on failure (no HRESULT). */
API struct stream *SHCreateMemStream(const BYTE *init, UINT cb)
{
    struct stream *s = stream_new();
    if (!s) return NULL;
    s->mode = STGM_READWRITE;
    if (init && cb) {
        if (!mem_reserve(s, cb)) { stream_destroy(s); return NULL; }
        memcpy(s->buf, init, cb);
        s->size = cb;
    }
    return s;
}

/* shlwapi helpers: these work on any IStream through its vtable and turn
 * a partial transfer into E_FAIL. */
API HRESULT IStream_Read(struct stream *s, void *buf, ULONG cb)
{
    if (!s) return E_INVALIDARG;
    ULONG got = 0;
    HRESULT hr = s->vtbl->Read(s, buf, cb, &got);
    if (SUCCEEDED(hr) && got != cb) hr = E_FAIL;
    return hr;
}

API HRESULT IStream_Write(struct stream *s, const void *buf, ULONG cb)
{
    if (!s) return E_INVALIDARG;
    ULONG put = 0;
    HRESULT hr = s->vtbl->Write(s, buf, cb, &put);
    if (SUCCEEDED(hr) && put != cb) hr = E_FAIL;
    return hr;
}

API HRESULT IStream_Reset(struct stream *s)
{
    if (!s) return E_INVALIDARG;
    return s->vtbl->Seek(s, 0, STREAM_SEEK_SET, NULL);
}

API HRESULT IStream_Size(struct stream *s, ULONGLONG *size)
{
    if (!s || !size) return E_INVALIDARG;
    STATSTG st;
    HRESULT hr = s->vtbl->Stat(s, &st, STATFLAG_NONAME);
    *size = SUCCEEDED(hr) ? st.cbSize : 0;
    return hr;
}

API HRESULT IStream_Copy(struct stream *src, struct stream *dst, DWORD cb)
{
    if (!src || !dst) return E_INVALIDARG;
    ULONGLONG rd = 0, wr = 0;
    HRESULT hr = src->vtbl->CopyTo(src, dst, cb, &rd, &wr);
    if (SUCCEEDED(hr) && wr != cb) hr = E_FAIL;
    return hr;
}

/* ================================================================== */
/* QISearch and the IUnknown_* helpers (shlwapi)                        */
/* ================================================================== */

/* Any COM object, seen through the first three slots of its vtable. */
struct unk;
struct unk_vtbl {
    HRESULT (WINAPI *QueryInterface)(struct unk *, const GUID *, void **);
    ULONG (WINAPI *AddRef)(struct unk *);
    ULONG (WINAPI *Release)(struct unk *);
};
struct unk { const struct unk_vtbl *vtbl; };

/* IObjectWithSite: SetSite and GetSite follow IUnknown. */
struct objsite_vtbl {
    struct unk_vtbl unk;
    HRESULT (WINAPI *SetSite)(struct unk *, struct unk *);
    HRESULT (WINAPI *GetSite)(struct unk *, const GUID *, void **);
};
/* IServiceProvider: QueryService follows IUnknown. */
struct svcprov_vtbl {
    struct unk_vtbl unk;
    HRESULT (WINAPI *QueryService)(struct unk *, const GUID *, const GUID *, void **);
};

static const GUID IID_IObjectWithSite_ = { 0xFC4801A3, 0x2BA9, 0x11CF, { 0xA2, 0x29, 0x00, 0xAA, 0x00, 0x3D, 0x73, 0x52 } };

/* QITAB as in shlwapi.h: { const IID *piid; int dwOffset; }, x64 size 16. */
typedef struct { const GUID *piid; int dwOffset; } QITAB;
_Static_assert(sizeof(QITAB) == 16, "QITAB layout");

/* QISearch implements QueryInterface from a table: each entry gives an IID
 * and the offset of that interface's vtable pointer inside the object.
 * IID_IUnknown is answered with the first entry. */
API HRESULT QISearch(void *base, const QITAB *tab, const GUID *iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!tab || !iid) return E_NOINTERFACE;
    const QITAB *hit = NULL;
    for (const QITAB *t = tab; t->piid; t++)
        if (guid_eq(t->piid, iid)) { hit = t; break; }
    if (!hit && guid_eq(iid, &IID_IUnknown_) && tab->piid) hit = tab;
    if (!hit) return E_NOINTERFACE;
    struct unk *u = (struct unk *)((char *)base + hit->dwOffset);
    u->vtbl->AddRef(u);
    *out = u;
    return S_OK;
}

/* Release *pp and clear it (the pointer is cleared before Release runs, so
 * a re-entrant caller never sees a dangling pointer). */
API void IUnknown_AtomicRelease(struct unk **pp)
{
    if (!pp || !*pp) return;
    struct unk *u = *pp;
    *pp = NULL;
    u->vtbl->Release(u);
}

/* Replace *pp with `u`, taking a reference on the new object and dropping
 * the old one. */
API void IUnknown_Set(struct unk **pp, struct unk *u)
{
    if (!pp) return;
    if (u) u->vtbl->AddRef(u);
    struct unk *old = *pp;
    *pp = u;
    if (old) old->vtbl->Release(old);
}

API HRESULT IUnknown_SetSite(struct unk *obj, struct unk *site)
{
    if (!obj) return E_FAIL;
    struct unk *ows = NULL;
    HRESULT hr = obj->vtbl->QueryInterface(obj, &IID_IObjectWithSite_, (void **)&ows);
    if (SUCCEEDED(hr) && ows) {
        hr = ((const struct objsite_vtbl *)ows->vtbl)->SetSite(ows, site);
        ows->vtbl->Release(ows);
    }
    return hr;
}

API HRESULT IUnknown_GetSite(struct unk *obj, const GUID *iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!obj) return E_INVALIDARG;
    struct unk *ows = NULL;
    HRESULT hr = obj->vtbl->QueryInterface(obj, &IID_IObjectWithSite_, (void **)&ows);
    if (SUCCEEDED(hr) && ows) {
        hr = ((const struct objsite_vtbl *)ows->vtbl)->GetSite(ows, iid, out);
        ows->vtbl->Release(ows);
    }
    return hr;
}

/* Ask `obj` for its IServiceProvider and query the service through it. */
API HRESULT IUnknown_QueryService(struct unk *obj, const GUID *sid, const GUID *iid, void **out)
{
    if (!out) return E_FAIL;
    *out = NULL;
    if (!obj) return E_FAIL;
    struct unk *sp = NULL;
    HRESULT hr = obj->vtbl->QueryInterface(obj, &IID_IServiceProvider_, (void **)&sp);
    if (SUCCEEDED(hr) && sp) {
        hr = ((const struct svcprov_vtbl *)sp->vtbl)->QueryService(sp, sid, iid, out);
        sp->vtbl->Release(sp);
    }
    return hr;
}

/* ================================================================== */
/* PathCch* (api-ms-win-core-path-l1-1-0, pathcch.h)                    */
/* ================================================================== */
/* These are the length-checked successors of the Path* functions. They
 * return HRESULTs, take a buffer size in characters (at most
 * PATHCCH_MAX_CCH), understand "\\?\" extended paths, and treat only the
 * backslash as a separator. */

#define PATHCCH_NONE                            0x00
#define PATHCCH_ALLOW_LONG_PATHS                0x01
#define PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS  0x02
#define PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS 0x04
#define PATHCCH_DO_NOT_NORMALIZE_SEGMENTS       0x08
#define PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH  0x10
#define PATHCCH_ENSURE_TRAILING_SLASH           0x20

/* Length of `p` if it is terminated within `cch` characters, else cch. */
static size_t wnlen(const WCHAR *p, size_t cch)
{
    size_t n = 0;
    while (n < cch && p[n]) n++;
    return n;
}

/* The common argument check: a buffer, a valid size, a terminator in it. */
static int cch_bad(const WCHAR *path, size_t cch)
{
    return !path || !cch || cch > PATHCCH_MAX_CCH || wnlen(path, cch) >= cch;
}

static int is_ext_prefix(const WCHAR *p) { return p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\'; }

/* "\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" is 48 characters. */
static int is_volume_guid(const WCHAR *p)
{
    if (!is_ext_prefix(p) || wncmpi(p + 4, u"Volume{", 7)) return 0;
    for (int i = 0; i < 36; i++) {
        WCHAR c = p[11 + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f'))) return 0;
    }
    return p[47] == '}';
}

/* Skip "server\share" plus the backslash after it, if any. */
static const WCHAR *skip_server_share(const WCHAR *q)
{
    while (*q && *q != '\\') q++;
    if (*q) q++;
    while (*q && *q != '\\') q++;
    if (*q) q++;
    return q;
}

/* End of the root of `p`, or NULL when it has none (a relative path):
 * "C:\" "C:" "\" "\\server\share\" "\\?\C:\" "\\?\UNC\server\share\"
 * "\\?\Volume{GUID}\". */
static const WCHAR *cch_root_end(const WCHAR *p)
{
    if (!p || !*p) return NULL;
    if (is_ext_prefix(p)) {
        const WCHAR *q = p + 4;
        if (!wncmpi(q, u"UNC\\", 4)) return skip_server_share(q + 4);
        if (is_volume_guid(p)) return p + 48 + (p[48] == '\\');
        if (is_drive(q)) return q + 2 + (q[2] == '\\');
        return NULL;
    }
    if (p[0] == '\\' && p[1] == '\\') return skip_server_share(p + 2);
    if (p[0] == '\\') return p + 1;
    if (is_drive(p)) return p + 2 + (p[2] == '\\');
    return NULL;
}

/* A UNC root is "\\server" or "\\server\share" with no trailing backslash
 * (with or without "\\?\UNC\"); "\\server\share\" is not a root. */
static int unc_tail_is_root(const WCHAR *q)
{
    const WCHAR *bs = NULL;
    int n = 0;
    for (const WCHAR *r = q; *r; r++) if (*r == '\\') { n++; bs = r; }
    if (n == 0) return 1;
    return n == 1 && bs[1] != 0;
}

API BOOL PathCchIsRoot(const WCHAR *path)
{
    if (!path || !*path) return FALSE;
    if (is_ext_prefix(path)) {
        const WCHAR *q = path + 4;
        if (!wncmpi(q, u"UNC\\", 4)) return unc_tail_is_root(q + 4);
        if (is_volume_guid(path)) return path[48] == '\\' && !path[49];
        return is_drive(q) && q[2] == '\\' && !q[3];
    }
    if (path[0] == '\\' && path[1] == '\\') return unc_tail_is_root(path + 2);
    if (path[0] == '\\') return !path[1];
    return is_drive(path) && path[2] == '\\' && !path[3];
}

API HRESULT PathCchSkipRoot(const WCHAR *path, const WCHAR **end)
{
    if (!path || !*path || !end) return E_INVALIDARG;
    const WCHAR *r = cch_root_end(path);
    if (!r) return E_INVALIDARG;
    *end = r;
    return S_OK;
}

API HRESULT PathCchAddBackslashEx(WCHAR *path, size_t cch, WCHAR **end, size_t *remaining)
{
    if (end) *end = NULL;
    if (remaining) *remaining = 0;
    if (cch_bad(path, cch)) return E_INVALIDARG;
    size_t n = wlen(path);
    /* An empty path or one already ending in a backslash is left alone. */
    if (!n || path[n - 1] == '\\') {
        if (end) *end = path + n;
        if (remaining) *remaining = cch - n;
        return S_FALSE;
    }
    if (n + 2 > cch) return STRSAFE_E_INSUFFICIENT_BUFFER;
    path[n] = '\\';
    path[n + 1] = 0;
    if (end) *end = path + n + 1;
    if (remaining) *remaining = cch - n - 1;
    return S_OK;
}

API HRESULT PathCchAddBackslash(WCHAR *path, size_t cch) { return PathCchAddBackslashEx(path, cch, NULL, NULL); }

/* The trailing backslash of a root ("C:\", "\") is kept. */
API HRESULT PathCchRemoveBackslashEx(WCHAR *path, size_t cch, WCHAR **end, size_t *remaining)
{
    if (end) *end = NULL;
    if (remaining) *remaining = 0;
    if (cch_bad(path, cch)) return E_INVALIDARG;
    size_t n = wlen(path);
    if (n && path[n - 1] == '\\' && !PathCchIsRoot(path)) {
        path[n - 1] = 0;
        if (end) *end = path + n - 1;
        if (remaining) *remaining = cch - n + 1;
        return S_OK;
    }
    if (end) *end = path + n;
    if (remaining) *remaining = cch - n;
    return S_FALSE;
}

API HRESULT PathCchRemoveBackslash(WCHAR *path, size_t cch) { return PathCchRemoveBackslashEx(path, cch, NULL, NULL); }

/* Extension: from the last '.' of the last component; a space or a
 * backslash after the dot means there is none. Points at the terminator
 * when there is no extension. */
API HRESULT PathCchFindExtension(const WCHAR *path, size_t cch, const WCHAR **ext)
{
    if (!ext) return E_INVALIDARG;
    *ext = NULL;
    if (cch_bad(path, cch)) return E_INVALIDARG;
    const WCHAR *dot = NULL, *p = path;
    for (; *p; p++) {
        if (*p == '\\' || *p == ' ') dot = NULL;
        else if (*p == '.') dot = p;
    }
    *ext = dot ? dot : p;
    return S_OK;
}

/* Validate an extension argument ("txt" or ".txt"); returns the part after
 * the optional dot, or NULL if it holds characters an extension cannot. */
static const WCHAR *cch_ext_body(const WCHAR *ext)
{
    if (!ext) return NULL;
    const WCHAR *e = ext[0] == '.' ? ext + 1 : ext;
    for (const WCHAR *p = e; *p; p++) if (*p == '.' || *p == ' ' || *p == '\\') return NULL;
    return e;
}

API HRESULT PathCchAddExtension(WCHAR *path, size_t cch, const WCHAR *ext)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    const WCHAR *e = cch_ext_body(ext);
    if (!e) return E_INVALIDARG;
    const WCHAR *cur;
    PathCchFindExtension(path, cch, &cur);
    if (*cur) return S_FALSE;                  /* already has one */
    if (!*e) return S_OK;                       /* nothing to add */
    size_t n = wlen(path), el = wlen(e);
    if (n + 1 + el + 1 > cch) return STRSAFE_E_INSUFFICIENT_BUFFER;
    path[n] = '.';
    memcpy(path + n + 1, e, (el + 1) * sizeof(WCHAR));
    return S_OK;
}

API HRESULT PathCchRemoveExtension(WCHAR *path, size_t cch)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    const WCHAR *cur;
    PathCchFindExtension(path, cch, &cur);
    if (!*cur) return S_FALSE;
    *(WCHAR *)cur = 0;
    return S_OK;
}

/* Replace (or add) the extension; an empty `ext` just removes it. */
API HRESULT PathCchRenameExtension(WCHAR *path, size_t cch, const WCHAR *ext)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    const WCHAR *e = cch_ext_body(ext);
    if (!e) return E_INVALIDARG;
    const WCHAR *cur;
    PathCchFindExtension(path, cch, &cur);
    size_t base = (size_t)(cur - path), el = wlen(e);
    if (el && base + 1 + el + 1 > cch) return STRSAFE_E_INSUFFICIENT_BUFFER;
    path[base] = 0;
    if (el) {
        path[base] = '.';
        memcpy(path + base + 1, e, (el + 1) * sizeof(WCHAR));
    }
    return S_OK;
}

/* Remove the last component (and the backslash before it), keeping the
 * root: "C:\a\b" -> "C:\a", "C:\a" -> "C:\", "\\s\h\x" -> "\\s\h". */
API HRESULT PathCchRemoveFileSpec(WCHAR *path, size_t cch)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    const WCHAR *root = cch_root_end(path);
    WCHAR *start = (WCHAR *)(root ? root : path);
    WCHAR *last = NULL;
    for (WCHAR *p = start; *p; p++) if (*p == '\\') last = p;
    int changed = 0;
    WCHAR *cut = last ? last : start;
    if (*cut) { *cut = 0; changed = 1; }
    if (PathCchRemoveBackslash(path, cch) == S_OK) changed = 1;
    return changed ? S_OK : S_FALSE;
}

/* Keep only the root; a UNC root loses its trailing backslash. A path
 * without a root is invalid here. */
API HRESULT PathCchStripToRoot(WCHAR *path, size_t cch)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    WCHAR *r = (WCHAR *)cch_root_end(path);
    if (!r) return E_INVALIDARG;
    int changed = 0;
    if (*r) { *r = 0; changed = 1; }
    if (PathCchRemoveBackslash(path, cch) == S_OK) changed = 1;
    return changed ? S_OK : S_FALSE;
}

/* "\\?\C:\x" -> "C:\x", "\\?\UNC\s\h" -> "\\s\h"; anything else S_FALSE. */
API HRESULT PathCchStripPrefix(WCHAR *path, size_t cch)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    if (!is_ext_prefix(path)) return S_FALSE;
    size_t n = wlen(path);
    if (!wncmpi(path + 4, u"UNC\\", 4)) {
        memmove(path + 2, path + 8, (n - 8 + 1) * sizeof(WCHAR));
        return S_OK;
    }
    if (is_drive(path + 4)) {
        memmove(path, path + 4, (n - 4 + 1) * sizeof(WCHAR));
        return S_OK;
    }
    return S_FALSE;
}

/* The core of PathCchCanonicalizeEx: resolve "." and ".." (never above the
 * root), drop one trailing dot of the last component unless asked not to,
 * turn a bare "X:" into "X:\" and "" into "\", and add the "\\?\" prefix
 * to long (or, on request, all) fully qualified paths. malloc'd result. */
static HRESULT cch_canon(const WCHAR *in, DWORD flags, WCHAR **res)
{
    *res = NULL;
    if ((flags & PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS) && (flags & PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS))
        return E_INVALIDARG;
    if ((flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH) && !(flags & PATHCCH_ALLOW_LONG_PATHS))
        return E_INVALIDARG;
    size_t n = wlen(in);
    if (n >= PATHCCH_MAX_CCH) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    WCHAR *buf = malloc((n + 16) * sizeof(WCHAR));
    if (!buf) return E_OUTOFMEMORY;
    const WCHAR *re = cch_root_end(in);
    size_t r = re ? (size_t)(re - in) : 0, o = 0;
    memcpy(buf, in, r * sizeof(WCHAR));
    o = r;
    size_t base = o;
    const WCHAR *p = in + r;
    while (*p) {
        const WCHAR *s = p;
        while (*p && *p != '\\') p++;
        size_t sl = (size_t)(p - s);
        int has_sep = *p != 0;
        if (has_sep) p++;
        if (sl == 1 && s[0] == '.') continue;
        if (sl == 2 && s[0] == '.' && s[1] == '.') {
            if (o > base) {
                if (buf[o - 1] == '\\') o--;
                while (o > base && buf[o - 1] != '\\') o--;
            }
            continue;
        }
        memcpy(buf + o, s, sl * sizeof(WCHAR));
        o += sl;
        if (has_sep) buf[o++] = '\\';
    }
    /* "C:\a\.." leaves "C:\a\" -> "C:\"; "a\." leaves "a\" -> "a" */
    if (o > base && buf[o - 1] == '\\' && n && in[n - 1] != '\\') o--;
    if (!(flags & PATHCCH_DO_NOT_NORMALIZE_SEGMENTS) && o > base && buf[o - 1] == '.') {
        size_t k = o;
        while (k > base && buf[k - 1] == '.') k--;
        if (k > base && buf[k - 1] != '\\') o--;      /* "a." -> "a", but not "..." */
    }
    if (o == 0) buf[o++] = '\\';
    if (o == 2 && is_drive(buf)) buf[o++] = '\\';
    if ((flags & PATHCCH_ENSURE_TRAILING_SLASH) && buf[o - 1] != '\\') buf[o++] = '\\';
    buf[o] = 0;

    int need_prefix = !is_ext_prefix(buf) && fully_qualified(buf) &&
                      ((flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH) || o + 1 > MAX_PATH);
    if (o + 1 > MAX_PATH && !(flags & PATHCCH_ALLOW_LONG_PATHS)) {
        free(buf);
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    }
    if (need_prefix && (flags & PATHCCH_ALLOW_LONG_PATHS)) {
        int unc = buf[0] == '\\';
        const WCHAR *pre = unc ? u"\\\\?\\UNC" : u"\\\\?\\";
        size_t pl = wlen(pre), skip = unc ? 1 : 0;   /* "\\s\h" keeps one backslash after "UNC" */
        WCHAR *t = malloc((pl + o + 1) * sizeof(WCHAR));
        if (!t) { free(buf); return E_OUTOFMEMORY; }
        memcpy(t, pre, pl * sizeof(WCHAR));
        memcpy(t + pl, buf + skip, (o - skip + 1) * sizeof(WCHAR));
        free(buf);
        buf = t;
    }
    *res = buf;
    return S_OK;
}

static HRESULT cch_out(WCHAR *out, size_t cch, WCHAR *res)
{
    size_t n = wlen(res);
    if (n + 1 > cch) { free(res); out[0] = 0; return STRSAFE_E_INSUFFICIENT_BUFFER; }
    memcpy(out, res, (n + 1) * sizeof(WCHAR));
    free(res);
    return S_OK;
}

API HRESULT PathCchCanonicalizeEx(WCHAR *out, size_t cch, const WCHAR *in, ULONG flags)
{
    if (!out || !cch || cch > PATHCCH_MAX_CCH) return E_INVALIDARG;
    out[0] = 0;
    if (!in) return E_INVALIDARG;
    WCHAR *res;
    HRESULT hr = cch_canon(in, flags, &res);
    if (FAILED(hr)) return hr;
    return cch_out(out, cch, res);
}

API HRESULT PathCchCanonicalize(WCHAR *out, size_t cch, const WCHAR *in)
{
    /* The non-Ex form is limited to MAX_PATH results. */
    return PathCchCanonicalizeEx(out, cch > MAX_PATH ? MAX_PATH : cch, in, PATHCCH_NONE);
}

API HRESULT PathAllocCanonicalize(const WCHAR *in, ULONG flags, WCHAR **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!in) return E_INVALIDARG;
    WCHAR *res;
    HRESULT hr = cch_canon(in, flags, &res);
    if (FAILED(hr)) return hr;
    *out = heap_wdup(res);                     /* freed with LocalFree */
    free(res);
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* Join two paths before canonicalizing: a fully qualified (or drive)
 * second path wins; one starting with a single backslash goes on the root
 * of the first; otherwise the two are joined with one backslash. */
static HRESULT cch_combine(const WCHAR *p1, const WCHAR *p2, ULONG flags, WCHAR **res)
{
    *res = NULL;
    if (!p1 && !p2) return E_INVALIDARG;
    if (!p1) p1 = g_empty_w;
    if (!p2) p2 = g_empty_w;
    size_t l1 = wlen(p1), l2 = wlen(p2);
    WCHAR *j = malloc((l1 + l2 + 3) * sizeof(WCHAR));
    if (!j) return E_OUTOFMEMORY;
    const WCHAR *r2 = cch_root_end(p2);
    if (r2 && !(p2[0] == '\\' && p2[1] != '\\')) {
        memcpy(j, p2, (l2 + 1) * sizeof(WCHAR));
    } else if (r2) {
        const WCHAR *r1 = cch_root_end(p1);
        size_t rl = r1 ? (size_t)(r1 - p1) : 0;
        memcpy(j, p1, rl * sizeof(WCHAR));
        if (rl && j[rl - 1] == '\\') rl--;
        memcpy(j + rl, p2, (l2 + 1) * sizeof(WCHAR));
    } else {
        memcpy(j, p1, l1 * sizeof(WCHAR));
        size_t o = l1;
        if (o && l2 && j[o - 1] != '\\') j[o++] = '\\';
        memcpy(j + o, p2, (l2 + 1) * sizeof(WCHAR));
    }
    HRESULT hr = cch_canon(j, flags, res);
    free(j);
    return hr;
}

API HRESULT PathCchCombineEx(WCHAR *out, size_t cch, const WCHAR *p1, const WCHAR *p2, ULONG flags)
{
    if (!out || !cch || cch > PATHCCH_MAX_CCH) return E_INVALIDARG;
    WCHAR *res;
    HRESULT hr = cch_combine(p1, p2, flags, &res);
    if (FAILED(hr)) { out[0] = 0; return hr; }
    return cch_out(out, cch, res);
}

API HRESULT PathCchCombine(WCHAR *out, size_t cch, const WCHAR *p1, const WCHAR *p2)
{
    return PathCchCombineEx(out, cch > MAX_PATH ? MAX_PATH : cch, p1, p2, PATHCCH_NONE);
}

API HRESULT PathAllocCombine(const WCHAR *p1, const WCHAR *p2, ULONG flags, WCHAR **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    WCHAR *res;
    HRESULT hr = cch_combine(p1, p2, flags, &res);
    if (FAILED(hr)) return hr;
    *out = heap_wdup(res);
    free(res);
    return *out ? S_OK : E_OUTOFMEMORY;
}

/* Append: a single leading backslash on `more` does not make it rooted
 * ("C:\a" + "\b" -> "C:\a\b"); a UNC or drive path replaces the whole. */
API HRESULT PathCchAppendEx(WCHAR *path, size_t cch, const WCHAR *more, ULONG flags)
{
    if (cch_bad(path, cch)) return E_INVALIDARG;
    if (more && more[0] == '\\' && more[1] != '\\') more++;
    WCHAR *res;
    HRESULT hr = cch_combine(path, more, flags, &res);
    if (FAILED(hr)) return hr;
    size_t n = wlen(res);
    if (n + 1 > cch) { free(res); return STRSAFE_E_INSUFFICIENT_BUFFER; }
    memcpy(path, res, (n + 1) * sizeof(WCHAR));
    free(res);
    return S_OK;
}

API HRESULT PathCchAppend(WCHAR *path, size_t cch, const WCHAR *more)
{
    return PathCchAppendEx(path, cch, more, PATHCCH_NONE);
}

/* PathIsUNCEx: "\\server..." or "\\?\UNC\server..."; *server points at
 * the server name. "\\?\" and "\\.\" device paths are not UNC. */
API BOOL PathIsUNCEx(const WCHAR *path, const WCHAR **server)
{
    if (server) *server = NULL;
    if (!path) return FALSE;
    const WCHAR *s = NULL;
    if (is_ext_prefix(path)) {
        if (!wncmpi(path + 4, u"UNC\\", 4)) s = path + 8;
    } else if (path[0] == '\\' && path[1] == '\\' && !(path[2] == '.' && path[3] == '\\')) {
        s = path + 2;
    }
    if (!s) return FALSE;
    if (server) *server = s;
    return TRUE;
}

/* ================================================================== */
/* Export table (shell32.dll, shlwapi.dll, api-ms-win-core-path-*)      */
/* ================================================================== */

const struct lsiw_export lsiw_shell32_exports[] = {
    E(AssocQueryStringA), E(AssocQueryStringW), E(ColorAdjustLuma), E(ColorHLSToRGB),
    E(ColorRGBToHLS), E(CommandLineToArgvW), E(DragAcceptFiles), E(DragFinish),
    E(DragQueryFileA), E(DragQueryFileW), E(DragQueryPoint), E(ExtractIconA),
    E(ExtractIconExA), E(ExtractIconExW), E(ExtractIconW), E(FindExecutableA),
    E(FindExecutableW), E(HashData), E(ILClone), E(ILCreateFromPathA),
    E(ILCreateFromPathW), E(ILFindLastID), E(ILFree), E(ILGetNext),
    E(ILGetSize), E(ILIsEqual), E(IStream_Copy), E(IStream_Read),
    E(IStream_Reset), E(IStream_Size), E(IStream_Write), E(IUnknown_AtomicRelease),
    E(IUnknown_GetSite), E(IUnknown_QueryService), E(IUnknown_Set), E(IUnknown_SetSite),
    E(IsOS), E(IsUserAnAdmin), E(PathAddBackslashA), E(PathAddBackslashW),
    E(PathAddExtensionA), E(PathAddExtensionW), E(PathAllocCanonicalize), E(PathAllocCombine),
    E(PathAppendA), E(PathAppendW), E(PathBuildRootA), E(PathBuildRootW),
    E(PathCanonicalizeA), E(PathCanonicalizeW), E(PathCchAddBackslash), E(PathCchAddBackslashEx),
    E(PathCchAddExtension), E(PathCchAppend), E(PathCchAppendEx), E(PathCchCanonicalize),
    E(PathCchCanonicalizeEx), E(PathCchCombine), E(PathCchCombineEx), E(PathCchFindExtension),
    E(PathCchIsRoot), E(PathCchRemoveBackslash), E(PathCchRemoveBackslashEx), E(PathCchRemoveExtension),
    E(PathCchRemoveFileSpec), E(PathCchRenameExtension), E(PathCchSkipRoot), E(PathCchStripPrefix),
    E(PathCchStripToRoot), E(PathCombineA), E(PathCombineW), E(PathCommonPrefixA),
    E(PathCommonPrefixW), E(PathCompactPathExA), E(PathCompactPathExW), E(PathCreateFromUrlA),
    E(PathCreateFromUrlAlloc), E(PathCreateFromUrlW), E(PathFileExistsA), E(PathFileExistsW),
    E(PathFindExtensionA), E(PathFindExtensionW), E(PathFindFileNameA), E(PathFindFileNameW),
    E(PathFindNextComponentA), E(PathFindNextComponentW), E(PathFindOnPathA), E(PathFindOnPathW),
    E(PathGetArgsA), E(PathGetArgsW), E(PathGetDriveNumberA), E(PathGetDriveNumberW),
    E(PathIsDirectoryA), E(PathIsDirectoryEmptyA), E(PathIsDirectoryEmptyW), E(PathIsDirectoryW),
    E(PathIsFileSpecA), E(PathIsFileSpecW), E(PathIsNetworkPathA), E(PathIsNetworkPathW),
    E(PathIsPrefixA), E(PathIsPrefixW), E(PathIsRelativeA), E(PathIsRelativeW),
    E(PathIsRootA), E(PathIsRootW), E(PathIsSameRootA), E(PathIsSameRootW),
    E(PathIsUNCA), E(PathIsUNCEx), E(PathIsUNCW), E(PathIsURLA),
    E(PathIsURLW), E(PathMakePrettyA), E(PathMakePrettyW), E(PathMatchSpecA),
    E(PathMatchSpecExA), E(PathMatchSpecExW), E(PathMatchSpecW), E(PathQuoteSpacesA),
    E(PathQuoteSpacesW), E(PathRelativePathToA), E(PathRelativePathToW), E(PathRemoveArgsA),
    E(PathRemoveArgsW), E(PathRemoveBackslashA), E(PathRemoveBackslashW), E(PathRemoveBlanksA),
    E(PathRemoveBlanksW), E(PathRemoveExtensionA), E(PathRemoveExtensionW), E(PathRemoveFileSpecA),
    E(PathRemoveFileSpecW), E(PathRenameExtensionA), E(PathRenameExtensionW), E(PathSearchAndQualifyA),
    E(PathSearchAndQualifyW), E(PathSkipRootA), E(PathSkipRootW), E(PathStripPathA),
    E(PathStripPathW), E(PathStripToRootA), E(PathStripToRootW), E(PathUnquoteSpacesA),
    E(PathUnquoteSpacesW), E(QISearch), E(SHAddToRecentDocs), E(SHAlloc),
    E(SHAppBarMessage), E(SHAutoComplete), E(SHChangeNotify), E(SHCreateDirectory),
    E(SHCreateDirectoryExA), E(SHCreateDirectoryExW), E(SHCreateMemStream), E(SHCreateStreamOnFileA),
    E(SHCreateStreamOnFileEx), E(SHCreateStreamOnFileW), E(SHDeleteKeyA), E(SHDeleteKeyW),
    E(SHDeleteValueA), E(SHDeleteValueW), E(SHEmptyRecycleBinA), E(SHEmptyRecycleBinW),
    E(SHFileOperationA), E(SHFileOperationW), E(SHFree), E(SHFreeNameMappings),
    E(SHGetDesktopFolder), E(SHGetFileInfoA), E(SHGetFileInfoW), E(SHGetFolderLocation),
    E(SHGetFolderPathA), E(SHGetFolderPathAndSubDirA), E(SHGetFolderPathAndSubDirW), E(SHGetFolderPathW),
    E(SHGetIDListFromObject), E(SHGetKnownFolderIDList), E(SHGetKnownFolderPath), E(SHGetMalloc),
    E(SHGetPathFromIDListA), E(SHGetPathFromIDListEx), E(SHGetPathFromIDListW), E(SHGetSetSettings),
    E(SHGetSettings), E(SHGetSpecialFolderLocation), E(SHGetSpecialFolderPathA), E(SHGetSpecialFolderPathW),
    E(SHGetValueA), E(SHGetValueW), E(SHILCreateFromPath), E(SHLoadIndirectString),
    E(SHOpenFolderAndSelectItems), E(SHParseDisplayName), E(SHQueryRecycleBinA), E(SHQueryRecycleBinW),
    E(SHQueryValueExA), E(SHQueryValueExW), E(SHRegGetValueA), E(SHRegGetValueW),
    E(SHSetFolderPathA), E(SHSetFolderPathW), E(SHSetKnownFolderPath), E(SHSetValueA),
    E(SHSetValueW), E(SHStrDupA), E(SHStrDupW), E(ShellExecuteA),
    E(ShellExecuteExA), E(ShellExecuteExW), E(ShellExecuteW), E(Shell_NotifyIconA),
    E(Shell_NotifyIconW), E(StrCSpnA), E(StrCSpnIA), E(StrCSpnIW),
    E(StrCSpnW), E(StrCatBuffA), E(StrCatBuffW), E(StrCatW),
    E(StrChrA), E(StrChrIA), E(StrChrIW), E(StrChrW),
    E(StrCmpA), E(StrCmpIA), E(StrCmpIW), E(StrCmpLogicalW),
    E(StrCmpNA), E(StrCmpNIA), E(StrCmpNIW), E(StrCmpNW),
    E(StrCmpW), E(StrCpyNW), E(StrCpyW), E(StrDupA),
    E(StrDupW), E(StrFormatByteSize64A), E(StrFormatByteSizeA), E(StrFormatByteSizeEx),
    E(StrFormatByteSizeW), E(StrFormatKBSizeA), E(StrFormatKBSizeW), E(StrIsIntlEqualA),
    E(StrIsIntlEqualW), E(StrNCatA), E(StrNCatW), E(StrPBrkA),
    E(StrPBrkW), E(StrRChrA), E(StrRChrIA), E(StrRChrIW),
    E(StrRChrW), E(StrRetToBufA), E(StrRetToBufW), E(StrRetToStrA),
    E(StrRetToStrW), E(StrSpnA), E(StrSpnW), E(StrStrA),
    E(StrStrIA), E(StrStrIW), E(StrStrW), E(StrToInt64ExA),
    E(StrToInt64ExW), E(StrToIntA), E(StrToIntExA), E(StrToIntExW),
    E(StrToIntW), E(StrTrimA), E(StrTrimW), E(UrlCreateFromPathA),
    E(UrlCreateFromPathW), E(wnsprintfA), E(wnsprintfW), E(wvnsprintfA),
    E(wvnsprintfW),
    { NULL, NULL }
};
