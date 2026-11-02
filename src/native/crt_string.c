/* C runtime strings: string.h, wchar.h (UTF-16 wchar_t), ctype.h, wctype.h,
 * number <-> string conversions, multibyte conversions (UTF-8), mbstring.h.
 *
 * Windows wchar_t is 16 bits, so every wide function is implemented here
 * rather than forwarded to the host C library.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "crt.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

/* ================================================================== */
/* Locale objects used for classification and number parsing           */
/* ================================================================== */

static locale_t g_c_locale, g_utf8_locale;
static pthread_once_t g_loc_once = PTHREAD_ONCE_INIT;
static void init_locales(void)
{
    g_c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    g_utf8_locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    if (!g_utf8_locale) g_utf8_locale = newlocale(LC_CTYPE_MASK, "C.utf8", (locale_t)0);
    if (!g_utf8_locale) g_utf8_locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
}
static locale_t c_loc(void) { pthread_once(&g_loc_once, init_locales); return g_c_locale; }
static locale_t u_loc(void) { pthread_once(&g_loc_once, init_locales); return g_utf8_locale; }

/* ================================================================== */
/* ctype                                                                */
/* ================================================================== */

#define C_UPPER   0x0001
#define C_LOWER   0x0002
#define C_DIGIT   0x0004
#define C_SPACE   0x0008
#define C_PUNCT   0x0010
#define C_CONTROL 0x0020
#define C_BLANK   0x0040
#define C_HEX     0x0080
#define C_ALPHA   (0x0100 | C_UPPER | C_LOWER)

static unsigned short g_ctype_table[257];          /* [0] is EOF */
static unsigned short g_wctype_table[256];
const unsigned short *crt_pctype = g_ctype_table + 1;
const unsigned short *crt_pwctype = g_wctype_table;
static pthread_once_t g_ctype_once = PTHREAD_ONCE_INIT;

static unsigned short classify_ascii(int c)
{
    unsigned short f = 0;
    if (c >= 'A' && c <= 'Z') f |= C_UPPER | 0x100;
    if (c >= 'a' && c <= 'z') f |= C_LOWER | 0x100;
    if (c >= '0' && c <= '9') f |= C_DIGIT;
    if ((c >= 9 && c <= 13) || c == ' ') f |= C_SPACE;
    if (c == ' ' || c == '\t') f |= C_BLANK;
    if (c < 32 || c == 127) f |= C_CONTROL;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= C_HEX;
    if (c > 32 && c < 127 && !isalnum(c)) f |= C_PUNCT;
    return f;
}

static unsigned short classify_wide(unsigned c)
{
    if (c < 128) return classify_ascii((int)c);
    locale_t l = u_loc();
    if (!l) return 0;
    unsigned short f = 0;
    if (iswupper_l(c, l)) f |= C_UPPER;
    if (iswlower_l(c, l)) f |= C_LOWER;
    if (iswalpha_l(c, l)) f |= 0x100;
    if (iswdigit_l(c, l)) f |= C_DIGIT;
    if (iswspace_l(c, l)) f |= C_SPACE;
    if (iswblank_l(c, l)) f |= C_BLANK;
    if (iswcntrl_l(c, l)) f |= C_CONTROL;
    if (iswpunct_l(c, l)) f |= C_PUNCT;
    if (c == 0xA0) f |= C_SPACE | C_BLANK;
    return f;
}

__attribute__((constructor)) static void init_ctype_ctor(void);
static void init_ctype(void)
{
    g_ctype_table[0] = 0;
    for (int c = 0; c < 256; c++) g_ctype_table[c + 1] = c < 128 ? classify_ascii(c) : 0;
    for (unsigned c = 0; c < 256; c++) g_wctype_table[c] = classify_wide(c);
}

static unsigned short ctype_of(int c)
{
    pthread_once(&g_ctype_once, init_ctype);
    if (c == -1) return 0;
    if (c < -1 || c > 255) return c < 0 ? g_ctype_table[(c & 0xff) + 1] : 0;
    return g_ctype_table[c + 1];
}

static unsigned short wctype_of(unsigned c)
{
    pthread_once(&g_ctype_once, init_ctype);
    if (c < 256) return g_wctype_table[c];
    if (c == 0xFFFF) return 0;
    return classify_wide(c);
}

WCHAR crt_towlower(WCHAR c)
{
    if (c < 128) return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
    locale_t l = u_loc();
    if (!l || (c >= 0xD800 && c <= 0xDFFF)) return c;
    wint_t r = towlower_l(c, l);
    return r <= 0xFFFF ? (WCHAR)r : c;
}
WCHAR crt_towupper(WCHAR c)
{
    if (c < 128) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
    locale_t l = u_loc();
    if (!l || (c >= 0xD800 && c <= 0xDFFF)) return c;
    wint_t r = towupper_l(c, l);
    return r <= 0xFFFF ? (WCHAR)r : c;
}
static int a_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int a_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

API int ms_isalpha(int c) { return ctype_of(c) & C_ALPHA; }
API int ms_isupper(int c) { return ctype_of(c) & C_UPPER; }
API int ms_islower(int c) { return ctype_of(c) & C_LOWER; }
API int ms_isdigit(int c) { return ctype_of(c) & C_DIGIT; }
API int ms_isxdigit(int c) { return ctype_of(c) & C_HEX; }
API int ms_isspace(int c) { return ctype_of(c) & C_SPACE; }
API int ms_ispunct(int c) { return ctype_of(c) & C_PUNCT; }
API int ms_isalnum(int c) { return ctype_of(c) & (C_ALPHA | C_DIGIT); }
API int ms_isprint(int c) { return (ctype_of(c) & (C_ALPHA | C_DIGIT | C_PUNCT)) || c == ' '; }
API int ms_isgraph(int c) { return ctype_of(c) & (C_ALPHA | C_DIGIT | C_PUNCT); }
API int ms_iscntrl(int c) { return ctype_of(c) & C_CONTROL; }
API int ms_isblank(int c) { return c == '\t' ? C_BLANK : (ctype_of(c) & C_BLANK); }
API int ms_toupper(int c) { return a_upper(c); }
API int ms_tolower(int c) { return a_lower(c); }
API int ms__toupper(int c) { return c - 'a' + 'A'; }
API int ms__tolower(int c) { return c - 'A' + 'a'; }
API int ms___isascii(int c) { return (unsigned)c < 0x80; }
API int ms___toascii(int c) { return c & 0x7f; }
API int __iscsym(int c) { return ms_isalnum(c) || c == '_'; }
API int __iscsymf(int c) { return ms_isalpha(c) || c == '_'; }
API int _isctype(int c, int mask) { return ctype_of(c) & mask; }
API int _isctype_l(int c, int mask, void *l) { (void)l; return ctype_of(c) & mask; }
API const unsigned short *__pctype_func(void) { pthread_once(&g_ctype_once, init_ctype); return crt_pctype; }
API const unsigned short **__p__pctype(void) { pthread_once(&g_ctype_once, init_ctype); return &crt_pctype; }
API const unsigned short *__pwctype_func(void) { pthread_once(&g_ctype_once, init_ctype); return crt_pwctype; }
API const unsigned short **__p__pwctype(void) { pthread_once(&g_ctype_once, init_ctype); return &crt_pwctype; }

#define L_CTYPE(name) API int _##name##_l(int c, void *l) { (void)l; return ms_##name(c); }
L_CTYPE(isalpha) L_CTYPE(isupper) L_CTYPE(islower) L_CTYPE(isdigit) L_CTYPE(isxdigit) L_CTYPE(isspace)
L_CTYPE(ispunct) L_CTYPE(isalnum) L_CTYPE(isprint) L_CTYPE(isgraph) L_CTYPE(iscntrl) L_CTYPE(isblank)
L_CTYPE(toupper) L_CTYPE(tolower)

API int ms_iswalpha(unsigned c) { return wctype_of(c) & C_ALPHA; }
API int ms_iswupper(unsigned c) { return wctype_of(c) & C_UPPER; }
API int ms_iswlower(unsigned c) { return wctype_of(c) & C_LOWER; }
API int ms_iswdigit(unsigned c) { return c >= '0' && c <= '9'; }
API int ms_iswxdigit(unsigned c) { return c < 128 && (wctype_of(c) & C_HEX); }
API int ms_iswspace(unsigned c) { return wctype_of(c) & C_SPACE; }
API int ms_iswpunct(unsigned c) { return wctype_of(c) & C_PUNCT; }
API int ms_iswalnum(unsigned c) { return wctype_of(c) & (C_ALPHA | C_DIGIT); }
API int ms_iswprint(unsigned c) { return (wctype_of(c) & (C_ALPHA | C_DIGIT | C_PUNCT)) || c == ' ' || (c > 255 && c != 0xFFFF && !(wctype_of(c) & C_CONTROL)); }
API int ms_iswgraph(unsigned c) { return c != ' ' && ms_iswprint(c) && !(wctype_of(c) & C_SPACE); }
API int ms_iswcntrl(unsigned c) { return wctype_of(c) & C_CONTROL; }
API int ms_iswblank(unsigned c) { return wctype_of(c) & C_BLANK; }
API int ms_iswascii(unsigned c) { return c < 0x80; }
API int ms_iswctype(unsigned c, unsigned short mask) { return wctype_of(c) & mask; }
API int is_wctype(unsigned c, unsigned short mask) { return wctype_of(c) & mask; }
API int _iswctype_l(unsigned c, unsigned short mask, void *l) { (void)l; return wctype_of(c) & mask; }
API unsigned ms_towupper(unsigned c) { return c > 0xFFFF ? c : crt_towupper((WCHAR)c); }
API unsigned ms_towlower(unsigned c) { return c > 0xFFFF ? c : crt_towlower((WCHAR)c); }
API int __iswcsym(unsigned c) { return ms_iswalnum(c) || c == '_'; }
API int __iswcsymf(unsigned c) { return ms_iswalpha(c) || c == '_'; }

#define L_WCTYPE(name) API int _##name##_l(unsigned c, void *l) { (void)l; return ms_##name(c); }
L_WCTYPE(iswalpha) L_WCTYPE(iswupper) L_WCTYPE(iswlower) L_WCTYPE(iswdigit) L_WCTYPE(iswxdigit) L_WCTYPE(iswspace)
L_WCTYPE(iswpunct) L_WCTYPE(iswalnum) L_WCTYPE(iswprint) L_WCTYPE(iswgraph) L_WCTYPE(iswcntrl) L_WCTYPE(iswblank)
API unsigned _towupper_l(unsigned c, void *l) { (void)l; return ms_towupper(c); }
API unsigned _towlower_l(unsigned c, void *l) { (void)l; return ms_towlower(c); }

static const struct { const char *name; unsigned short mask; } g_wctype_names[] = {
    { "alnum", C_ALPHA | C_DIGIT }, { "alpha", C_ALPHA }, { "blank", C_BLANK }, { "cntrl", C_CONTROL },
    { "digit", C_DIGIT }, { "graph", C_ALPHA | C_DIGIT | C_PUNCT }, { "lower", C_LOWER },
    { "print", C_ALPHA | C_DIGIT | C_PUNCT | C_BLANK }, { "punct", C_PUNCT }, { "space", C_SPACE },
    { "upper", C_UPPER }, { "xdigit", C_HEX },
};
API unsigned short ms_wctype(const char *name)
{
    for (size_t i = 0; i < sizeof g_wctype_names / sizeof *g_wctype_names; i++)
        if (!strcmp(name, g_wctype_names[i].name)) return g_wctype_names[i].mask;
    return 0;
}
API unsigned ms_towctrans(unsigned c, unsigned t) { return t == 1 ? ms_towupper(c) : t == 2 ? ms_towlower(c) : c; }
API unsigned ms_wctrans(const char *n) { return !strcmp(n, "toupper") ? 1 : !strcmp(n, "tolower") ? 2 : 0; }

/* ================================================================== */
/* Narrow strings                                                       */
/* ================================================================== */

API void *ms_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
API void *ms_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
API void *ms_memset(void *d, int c, size_t n) { return memset(d, c, n); }
API int   ms_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
API void *ms_memchr(const void *s, int c, size_t n) { return memchr(s, c, n); }
API void *_memccpy(void *d, const void *s, int c, size_t n) { return memccpy(d, s, c, n); }
API int _memicmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) { int d = a_lower(x[i]) - a_lower(y[i]); if (d) return d; }
    return 0;
}
API int _memicmp_l(const void *a, const void *b, size_t n, void *l) { (void)l; return _memicmp(a, b, n); }
API int memcpy_s(void *d, size_t dn, const void *s, size_t n)
{
    if (!n) return 0;
    if (!d) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!s || dn < n) { memset(d, 0, dn); crt_invalid_parameter(); return !s ? MS_EINVAL : MS_ERANGE; }
    memcpy(d, s, n);
    return 0;
}
API int memmove_s(void *d, size_t dn, const void *s, size_t n)
{
    if (!n) return 0;
    if (!d || !s) { crt_invalid_parameter(); return MS_EINVAL; }
    if (dn < n) { crt_invalid_parameter(); return MS_ERANGE; }
    memmove(d, s, n);
    return 0;
}

API size_t ms_strlen(const char *s) { return strlen(s); }
API size_t ms_strnlen(const char *s, size_t n) { return s ? strnlen(s, n) : 0; }
API char *ms_strcpy(char *d, const char *s) { return strcpy(d, s); }
API char *ms_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
API char *ms_strcat(char *d, const char *s) { return strcat(d, s); }
API char *ms_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
API int ms_strcmp(const char *a, const char *b) { int r = strcmp(a, b); return r < 0 ? -1 : r > 0; }
API int ms_strncmp(const char *a, const char *b, size_t n) { int r = strncmp(a, b, n); return r < 0 ? -1 : r > 0; }
API char *ms_strchr(const char *s, int c) { return strchr(s, c); }
API char *ms_strrchr(const char *s, int c) { return strrchr(s, c); }
API char *ms_strstr(const char *h, const char *n) { return strstr(h, n); }
API size_t ms_strspn(const char *s, const char *a) { return strspn(s, a); }
API size_t ms_strcspn(const char *s, const char *a) { return strcspn(s, a); }
API char *ms_strpbrk(const char *s, const char *a) { return strpbrk(s, a); }
API int ms_strcoll(const char *a, const char *b) { return ms_strcmp(a, b); }
API int _strcoll_l(const char *a, const char *b, void *l) { (void)l; return ms_strcmp(a, b); }
API int _strncoll(const char *a, const char *b, size_t n) { return ms_strncmp(a, b, n); }
API size_t ms_strxfrm(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (d && n) { size_t c = l < n ? l + 1 : n; memcpy(d, s, c); }
    return l;
}

API int _stricmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (;; x++, y++) {
        int d = a_lower(*x) - a_lower(*y);
        if (d || !*x) return d;
    }
}
API int _strnicmp(const char *a, const char *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        int d = a_lower(x[i]) - a_lower(y[i]);
        if (d || !x[i]) return d;
    }
    return 0;
}
API int _stricmp_l(const char *a, const char *b, void *l) { (void)l; return _stricmp(a, b); }
API int _strnicmp_l(const char *a, const char *b, size_t n, void *l) { (void)l; return _strnicmp(a, b, n); }
API int _stricoll(const char *a, const char *b) { return _stricmp(a, b); }
API int _strnicoll(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
API int _stricoll_l(const char *a, const char *b, void *l) { (void)l; return _stricmp(a, b); }

API char *_strdup(const char *s)
{
    if (!s) return NULL;
    size_t l = strlen(s) + 1;
    char *d = lsiw_heap_alloc(l, 0);
    if (!d) { crt_set_errno(MS_ENOMEM); return NULL; }
    return memcpy(d, s, l);
}
API char *_strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)a_lower((unsigned char)*p); return s; }
API char *_strupr(char *s) { for (char *p = s; *p; p++) *p = (char)a_upper((unsigned char)*p); return s; }
API char *_strlwr_l(char *s, void *l) { (void)l; return _strlwr(s); }
API char *_strupr_l(char *s, void *l) { (void)l; return _strupr(s); }
API int _strlwr_s(char *s, size_t n) { if (!s || strnlen(s, n) == n) { crt_invalid_parameter(); return MS_EINVAL; } _strlwr(s); return 0; }
API int _strupr_s(char *s, size_t n) { if (!s || strnlen(s, n) == n) { crt_invalid_parameter(); return MS_EINVAL; } _strupr(s); return 0; }
API char *_strrev(char *s)
{
    size_t l = strlen(s);
    for (size_t i = 0; i < l / 2; i++) { char t = s[i]; s[i] = s[l - 1 - i]; s[l - 1 - i] = t; }
    return s;
}
API char *_strset(char *s, int c) { for (char *p = s; *p; p++) *p = (char)c; return s; }
API char *_strnset(char *s, int c, size_t n) { for (size_t i = 0; i < n && s[i]; i++) s[i] = (char)c; return s; }

API int strcpy_s(char *d, size_t n, const char *s)
{
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = strlen(s);
    if (l >= n) { d[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    memcpy(d, s, l + 1);
    return 0;
}
#define TRUNCATE ((size_t)-1)
API int strncpy_s(char *d, size_t n, const char *s, size_t count)
{
    if (!count && !d && !n) return 0;
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!count) { d[0] = 0; return 0; }
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = strnlen(s, count == TRUNCATE ? n : count);
    if (l >= n) {
        if (count == TRUNCATE) { memcpy(d, s, n - 1); d[n - 1] = 0; return MS_STRUNCATE; }
        d[0] = 0; crt_invalid_parameter(); return MS_ERANGE;
    }
    memcpy(d, s, l);
    d[l] = 0;
    return 0;
}
API int strcat_s(char *d, size_t n, const char *s)
{
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t dl = strnlen(d, n);
    if (dl == n) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = strlen(s);
    if (dl + l >= n) { d[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    memcpy(d + dl, s, l + 1);
    return 0;
}
API int strncat_s(char *d, size_t n, const char *s, size_t count)
{
    if (!count && !d && !n) return 0;
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!count) return 0;
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t dl = strnlen(d, n);
    if (dl == n) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = strnlen(s, count == TRUNCATE ? n : count);
    if (dl + l >= n) {
        if (count == TRUNCATE) { memcpy(d + dl, s, n - dl - 1); d[n - 1] = 0; return MS_STRUNCATE; }
        d[0] = 0; crt_invalid_parameter(); return MS_ERANGE;
    }
    memcpy(d + dl, s, l);
    d[dl + l] = 0;
    return 0;
}
API size_t strnlen_s(const char *s, size_t n) { return s ? strnlen(s, n) : 0; }

API char *strtok_s(char *s, const char *delim, char **ctx)
{
    if (!ctx || !delim || (!s && !*ctx)) { crt_invalid_parameter(); return NULL; }
    if (!s) s = *ctx;
    s += strspn(s, delim);
    if (!*s) { *ctx = s; return NULL; }
    char *e = s + strcspn(s, delim);
    if (*e) *e++ = 0;
    *ctx = e;
    return s;
}
API char *ms_strtok(char *s, const char *delim)
{
    char **ctx = &lsiw_thread()->strtok_ctx;
    if (!s && !*ctx) return NULL;
    return strtok_s(s, delim, ctx);
}

/* ---- error messages ---- */

static const char *const g_errlist[] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted function call", "Input/output error", "No such device or address", "Arg list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
    "Not enough space", "Permission denied", "Bad address", "Unknown error", "Resource device",
    "File exists", "Improper link", "No such device", "Not a directory", "Is a directory",
    "Invalid argument", "Too many open files in system", "Too many open files",
    "Inappropriate I/O control operation", "Unknown error", "File too large", "No space left on device",
    "Invalid seek", "Read-only file system", "Too many links", "Broken pipe", "Domain error",
    "Result too large", "Unknown error", "Resource deadlock avoided", "Unknown error", "Filename too long",
    "No locks available", "Function not implemented", "Directory not empty", "Illegal byte sequence",
    "Unknown error"
};
#define NERR ((int)(sizeof g_errlist / sizeof *g_errlist))
const char *const *crt_sys_errlist = g_errlist;
int crt_sys_nerr = NERR - 1;

static const char *err_text(int e)
{
    if (e == MS_STRUNCATE) return "Truncation occurred";
    if (e < 0 || e >= NERR) return "Unknown error";
    return g_errlist[e];
}
const char *crt_strerror_text(int e) { return err_text(e); }
API char *ms_strerror(int e) { return (char *)err_text(e); }
API int strerror_s(char *buf, size_t n, int e)
{
    if (!buf || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    snprintf(buf, n, "%s", err_text(e));
    return 0;
}
API char *_strerror(const char *s)
{
    static __thread char buf[512];
    if (s && *s) snprintf(buf, sizeof buf, "%s: %s\n", s, err_text(*crt_errno()));
    else snprintf(buf, sizeof buf, "%s\n", err_text(*crt_errno()));
    return buf;
}
API int _strerror_s(char *buf, size_t n, const char *s)
{
    if (!buf || !n) return MS_EINVAL;
    if (s && *s) snprintf(buf, n, "%s: %s\n", s, err_text(*crt_errno()));
    else snprintf(buf, n, "%s\n", err_text(*crt_errno()));
    return 0;
}
API const char *const *__sys_errlist(void) { return g_errlist; }
API int *__sys_nerr(void) { return &crt_sys_nerr; }

/* ================================================================== */
/* Wide strings (16-bit)                                                */
/* ================================================================== */

size_t crt_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }

API size_t ms_wcslen(const WCHAR *s) { return crt_wcslen(s); }
API size_t ms_wcsnlen(const WCHAR *s, size_t n) { size_t i = 0; if (!s) return 0; while (i < n && s[i]) i++; return i; }
API size_t wcsnlen_s(const WCHAR *s, size_t n) { return ms_wcsnlen(s, n); }
API WCHAR *ms_wcscpy(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
API WCHAR *ms_wcsncpy(WCHAR *d, const WCHAR *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
API WCHAR *ms_wcscat(WCHAR *d, const WCHAR *s) { ms_wcscpy(d + crt_wcslen(d), s); return d; }
API WCHAR *ms_wcsncat(WCHAR *d, const WCHAR *s, size_t n)
{
    WCHAR *e = d + crt_wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
API int ms_wcscmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a < *b ? -1 : *a > *b;
}
API int ms_wcsncmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}
API WCHAR *ms_wcschr(const WCHAR *s, WCHAR c)
{
    for (;; s++) { if (*s == c) return (WCHAR *)s; if (!*s) return NULL; }
}
API WCHAR *ms_wcsrchr(const WCHAR *s, WCHAR c)
{
    const WCHAR *r = NULL;
    for (;; s++) { if (*s == c) r = s; if (!*s) return (WCHAR *)r; }
}
API WCHAR *ms_wcsstr(const WCHAR *h, const WCHAR *n)
{
    size_t nl = crt_wcslen(n);
    if (!nl) return (WCHAR *)h;
    for (; *h; h++) if (*h == *n && !ms_wcsncmp(h, n, nl)) return (WCHAR *)h;
    return NULL;
}
API size_t ms_wcsspn(const WCHAR *s, const WCHAR *a)
{
    size_t i = 0;
    for (; s[i]; i++) { const WCHAR *p = a; while (*p && *p != s[i]) p++; if (!*p) break; }
    return i;
}
API size_t ms_wcscspn(const WCHAR *s, const WCHAR *a)
{
    size_t i = 0;
    for (; s[i]; i++) { const WCHAR *p = a; while (*p && *p != s[i]) p++; if (*p) break; }
    return i;
}
API WCHAR *ms_wcspbrk(const WCHAR *s, const WCHAR *a) { s += ms_wcscspn(s, a); return *s ? (WCHAR *)s : NULL; }
API int ms_wcscoll(const WCHAR *a, const WCHAR *b) { return ms_wcscmp(a, b); }
API int _wcscoll_l(const WCHAR *a, const WCHAR *b, void *l) { (void)l; return ms_wcscmp(a, b); }
API int _wcsncoll(const WCHAR *a, const WCHAR *b, size_t n) { return ms_wcsncmp(a, b, n); }
API size_t ms_wcsxfrm(WCHAR *d, const WCHAR *s, size_t n)
{
    size_t l = crt_wcslen(s);
    if (d && n) { size_t c = l < n ? l + 1 : n; memcpy(d, s, c * 2); }
    return l;
}
API int _wcsicmp(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        int d = (int)crt_towlower(*a) - (int)crt_towlower(*b);
        if (d || !*a) return d;
    }
}
API int _wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int d = (int)crt_towlower(a[i]) - (int)crt_towlower(b[i]);
        if (d || !a[i]) return d;
    }
    return 0;
}
API int _wcsicmp_l(const WCHAR *a, const WCHAR *b, void *l) { (void)l; return _wcsicmp(a, b); }
API int _wcsnicmp_l(const WCHAR *a, const WCHAR *b, size_t n, void *l) { (void)l; return _wcsnicmp(a, b, n); }
API int _wcsicoll(const WCHAR *a, const WCHAR *b) { return _wcsicmp(a, b); }
API int _wcsnicoll(const WCHAR *a, const WCHAR *b, size_t n) { return _wcsnicmp(a, b, n); }
API WCHAR *_wcsdup(const WCHAR *s)
{
    if (!s) return NULL;
    size_t l = (crt_wcslen(s) + 1) * 2;
    WCHAR *d = lsiw_heap_alloc(l, 0);
    if (!d) { crt_set_errno(MS_ENOMEM); return NULL; }
    return memcpy(d, s, l);
}
API WCHAR *_wcslwr(WCHAR *s) { for (WCHAR *p = s; *p; p++) *p = crt_towlower(*p); return s; }
API WCHAR *_wcsupr(WCHAR *s) { for (WCHAR *p = s; *p; p++) *p = crt_towupper(*p); return s; }
API WCHAR *_wcslwr_l(WCHAR *s, void *l) { (void)l; return _wcslwr(s); }
API WCHAR *_wcsupr_l(WCHAR *s, void *l) { (void)l; return _wcsupr(s); }
API int _wcslwr_s(WCHAR *s, size_t n) { if (!s || ms_wcsnlen(s, n) == n) { crt_invalid_parameter(); return MS_EINVAL; } _wcslwr(s); return 0; }
API int _wcsupr_s(WCHAR *s, size_t n) { if (!s || ms_wcsnlen(s, n) == n) { crt_invalid_parameter(); return MS_EINVAL; } _wcsupr(s); return 0; }
API WCHAR *_wcsrev(WCHAR *s)
{
    size_t l = crt_wcslen(s);
    for (size_t i = 0; i < l / 2; i++) { WCHAR t = s[i]; s[i] = s[l - 1 - i]; s[l - 1 - i] = t; }
    return s;
}
API WCHAR *_wcsset(WCHAR *s, WCHAR c) { for (WCHAR *p = s; *p; p++) *p = c; return s; }
API WCHAR *_wcsnset(WCHAR *s, WCHAR c, size_t n) { for (size_t i = 0; i < n && s[i]; i++) s[i] = c; return s; }

API int wcscpy_s(WCHAR *d, size_t n, const WCHAR *s)
{
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = crt_wcslen(s);
    if (l >= n) { d[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    memcpy(d, s, (l + 1) * 2);
    return 0;
}
API int wcsncpy_s(WCHAR *d, size_t n, const WCHAR *s, size_t count)
{
    if (!count && !d && !n) return 0;
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!count) { d[0] = 0; return 0; }
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = ms_wcsnlen(s, count == TRUNCATE ? n : count);
    if (l >= n) {
        if (count == TRUNCATE) { memcpy(d, s, (n - 1) * 2); d[n - 1] = 0; return MS_STRUNCATE; }
        d[0] = 0; crt_invalid_parameter(); return MS_ERANGE;
    }
    memcpy(d, s, l * 2);
    d[l] = 0;
    return 0;
}
API int wcscat_s(WCHAR *d, size_t n, const WCHAR *s)
{
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t dl = ms_wcsnlen(d, n);
    if (dl == n) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = crt_wcslen(s);
    if (dl + l >= n) { d[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    memcpy(d + dl, s, (l + 1) * 2);
    return 0;
}
API int wcsncat_s(WCHAR *d, size_t n, const WCHAR *s, size_t count)
{
    if (!count && !d && !n) return 0;
    if (!d || !n) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!count) return 0;
    if (!s) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t dl = ms_wcsnlen(d, n);
    if (dl == n) { d[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    size_t l = ms_wcsnlen(s, count == TRUNCATE ? n : count);
    if (dl + l >= n) {
        if (count == TRUNCATE) { memcpy(d + dl, s, (n - dl - 1) * 2); d[n - 1] = 0; return MS_STRUNCATE; }
        d[0] = 0; crt_invalid_parameter(); return MS_ERANGE;
    }
    memcpy(d + dl, s, l * 2);
    d[dl + l] = 0;
    return 0;
}

API WCHAR *wcstok_s(WCHAR *s, const WCHAR *delim, WCHAR **ctx)
{
    if (!ctx || !delim || (!s && !*ctx)) { crt_invalid_parameter(); return NULL; }
    if (!s) s = *ctx;
    s += ms_wcsspn(s, delim);
    if (!*s) { *ctx = s; return NULL; }
    WCHAR *e = s + ms_wcscspn(s, delim);
    if (*e) *e++ = 0;
    *ctx = e;
    return s;
}
/* msvcrt's two-argument wcstok and ucrt's three-argument one. */
API WCHAR *msvcrt_wcstok(WCHAR *s, const WCHAR *delim)
{
    WCHAR **ctx = &lsiw_thread()->wcstok_ctx;
    if (!s && !*ctx) return NULL;
    return wcstok_s(s, delim, ctx);
}
API WCHAR *ucrt_wcstok(WCHAR *s, const WCHAR *delim, WCHAR **ctx)
{
    if (!ctx) return msvcrt_wcstok(s, delim);
    if (!s && !*ctx) return NULL;
    return wcstok_s(s, delim, ctx);
}

API WCHAR *ms_wmemcpy(WCHAR *d, const WCHAR *s, size_t n) { return memcpy(d, s, n * 2); }
API WCHAR *ms_wmemmove(WCHAR *d, const WCHAR *s, size_t n) { return memmove(d, s, n * 2); }
API WCHAR *ms_wmemset(WCHAR *d, WCHAR c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }
API int ms_wmemcmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
API WCHAR *ms_wmemchr(const WCHAR *s, WCHAR c, size_t n)
{
    for (size_t i = 0; i < n; i++) if (s[i] == c) return (WCHAR *)s + i;
    return NULL;
}
API int wmemcpy_s(WCHAR *d, size_t dn, const WCHAR *s, size_t n) { return memcpy_s(d, dn * 2, s, n * 2); }
API int wmemmove_s(WCHAR *d, size_t dn, const WCHAR *s, size_t n) { return memmove_s(d, dn * 2, s, n * 2); }

API WCHAR *_wcserror(int e)
{
    static __thread WCHAR buf[128];
    lsiw_u2w_buf(err_text(e), buf, 128);
    return buf;
}
API int _wcserror_s(WCHAR *buf, size_t n, int e)
{
    if (!buf || !n) return MS_EINVAL;
    lsiw_u2w_buf(err_text(e), buf, (int)n);
    buf[n - 1] = 0;
    return 0;
}
API WCHAR *__wcserror(const WCHAR *s)
{
    static __thread WCHAR buf[512];
    char *u = s ? lsiw_w2u(s) : NULL;
    char tmp[512];
    if (u && *u) snprintf(tmp, sizeof tmp, "%s: %s\n", u, err_text(*crt_errno()));
    else snprintf(tmp, sizeof tmp, "%s\n", err_text(*crt_errno()));
    free(u);
    lsiw_u2w_buf(tmp, buf, 512);
    return buf;
}

/* ================================================================== */
/* String -> number                                                     */
/* ================================================================== */

static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* Microsoft strtol family semantics. `bits` is 32 or 64. */
static unsigned long long parse_int(const char *s, const char **endp, int base, int is_signed, int bits)
{
    const char *p = s;
    int neg = 0;
    if (base < 0 || base == 1 || base > 36) { if (endp) *endp = s; crt_set_errno(MS_EINVAL); return 0; }
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_val((unsigned char)p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long long limit = bits == 64 ? ULLONG_MAX : 0xFFFFFFFFull, v = 0;
    int any = 0, ovf = 0;
    for (;; p++) {
        int d = digit_val((unsigned char)*p);
        if (d >= base) break;
        any = 1;
        if (v > (limit - (unsigned)d) / (unsigned)base) ovf = 1;
        else v = v * (unsigned)base + (unsigned)d;
    }
    if (!any) { if (endp) *endp = s; return 0; }
    if (endp) *endp = p;
    if (is_signed) {
        unsigned long long maxp = bits == 64 ? (unsigned long long)LLONG_MAX : (unsigned long long)INT32_MAX;
        if (ovf || (!neg && v > maxp) || (neg && v > maxp + 1)) {
            crt_set_errno(MS_ERANGE);
            return neg ? (unsigned long long)(-(long long)maxp - 1) : maxp;
        }
        return neg ? (unsigned long long)(-(long long)v) : v;
    }
    if (ovf) { crt_set_errno(MS_ERANGE); return limit; }
    return neg ? (0 - v) & limit : v;
}

API int32_t ms_strtol(const char *s, char **e, int b) { return (int32_t)parse_int(s, (const char **)e, b, 1, 32); }
API uint32_t ms_strtoul(const char *s, char **e, int b) { return (uint32_t)parse_int(s, (const char **)e, b, 0, 32); }
API long long ms_strtoll(const char *s, char **e, int b) { return (long long)parse_int(s, (const char **)e, b, 1, 64); }
API unsigned long long ms_strtoull(const char *s, char **e, int b) { return parse_int(s, (const char **)e, b, 0, 64); }
API int32_t _strtol_l(const char *s, char **e, int b, void *l) { (void)l; return ms_strtol(s, e, b); }
API uint32_t _strtoul_l(const char *s, char **e, int b, void *l) { (void)l; return ms_strtoul(s, e, b); }
API long long _strtoi64_l(const char *s, char **e, int b, void *l) { (void)l; return ms_strtoll(s, e, b); }
API unsigned long long _strtoui64_l(const char *s, char **e, int b, void *l) { (void)l; return ms_strtoull(s, e, b); }
API int ms_atoi(const char *s) { int e = *crt_errno(); int r = (int)parse_int(s, NULL, 10, 1, 32); *crt_errno() = e; return r; }
API int32_t ms_atol(const char *s) { return ms_atoi(s); }
API long long ms_atoll(const char *s) { int e = *crt_errno(); long long r = (long long)parse_int(s, NULL, 10, 1, 64); *crt_errno() = e; return r; }
API int _atoi_l(const char *s, void *l) { (void)l; return ms_atoi(s); }
API int32_t _atol_l(const char *s, void *l) { (void)l; return ms_atoi(s); }
API long long _atoi64_l(const char *s, void *l) { (void)l; return ms_atoll(s); }

static double parse_double(const char *s, char **e)
{
    errno = 0;
    double r = strtod_l(s, e, c_loc());
    if (errno == ERANGE) crt_set_errno(MS_ERANGE);
    return r;
}
API double ms_strtod(const char *s, char **e) { return parse_double(s, e); }
API double ms_strtold(const char *s, char **e) { return parse_double(s, e); }
API float ms_strtof(const char *s, char **e)
{
    errno = 0;
    float r = strtof_l(s, e, c_loc());
    if (errno == ERANGE) crt_set_errno(MS_ERANGE);
    return r;
}
API double ms_atof(const char *s) { return strtod_l(s, NULL, c_loc()); }
API double _strtod_l(const char *s, char **e, void *l) { (void)l; return parse_double(s, e); }
API float _strtof_l(const char *s, char **e, void *l) { (void)l; return ms_strtof(s, e); }
API double _atof_l(const char *s, void *l) { (void)l; return ms_atof(s); }
API int _atodbl(double *out, char *s) { *out = strtod_l(s, NULL, c_loc()); return 0; }
API int _atoflt(float *out, const char *s) { *out = strtof_l(s, NULL, c_loc()); return 0; }
API int _atoldbl(double *out, char *s) { *out = strtod_l(s, NULL, c_loc()); return 0; }

/* Wide: map to ASCII 1:1 so end pointers line up. */
static char *w_ascii(const WCHAR *w, size_t *len)
{
    size_t n = crt_wcslen(w);
    char *a = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        WCHAR c = w[i];
        a[i] = c < 128 ? (char)c : (c == 0x3000 || c == 0xA0 || (c >= 0x2000 && c <= 0x200A)) ? ' ' : '\x01';
    }
    a[n] = 0;
    if (len) *len = n;
    return a;
}
#define W_PARSE(type, call)                                                   \
    char *a = w_ascii(s, NULL);                                               \
    const char *end;                                                          \
    type r = (type)call;                                                      \
    if (e) *e = (WCHAR *)s + (end - a);                                       \
    free(a);                                                                  \
    return r;

API int32_t ms_wcstol(const WCHAR *s, WCHAR **e, int b) { W_PARSE(int32_t, parse_int(a, &end, b, 1, 32)) }
API uint32_t ms_wcstoul(const WCHAR *s, WCHAR **e, int b) { W_PARSE(uint32_t, parse_int(a, &end, b, 0, 32)) }
API long long ms_wcstoll(const WCHAR *s, WCHAR **e, int b) { W_PARSE(long long, parse_int(a, &end, b, 1, 64)) }
API unsigned long long ms_wcstoull(const WCHAR *s, WCHAR **e, int b) { W_PARSE(unsigned long long, parse_int(a, &end, b, 0, 64)) }
API double ms_wcstod(const WCHAR *s, WCHAR **e) { W_PARSE(double, parse_double(a, (char **)&end)) }
API float ms_wcstof(const WCHAR *s, WCHAR **e)
{
    char *a = w_ascii(s, NULL);
    char *end;
    float r = ms_strtof(a, &end);
    if (e) *e = (WCHAR *)s + (end - a);
    free(a);
    return r;
}
API int32_t _wcstol_l(const WCHAR *s, WCHAR **e, int b, void *l) { (void)l; return ms_wcstol(s, e, b); }
API uint32_t _wcstoul_l(const WCHAR *s, WCHAR **e, int b, void *l) { (void)l; return ms_wcstoul(s, e, b); }
API long long _wcstoi64_l(const WCHAR *s, WCHAR **e, int b, void *l) { (void)l; return ms_wcstoll(s, e, b); }
API unsigned long long _wcstoui64_l(const WCHAR *s, WCHAR **e, int b, void *l) { (void)l; return ms_wcstoull(s, e, b); }
API double _wcstod_l(const WCHAR *s, WCHAR **e, void *l) { (void)l; return ms_wcstod(s, e); }
API int _wtoi(const WCHAR *s) { int e = *crt_errno(); int r = ms_wcstol(s, NULL, 10); *crt_errno() = e; return r; }
API long long _wtoi64(const WCHAR *s) { int e = *crt_errno(); long long r = ms_wcstoll(s, NULL, 10); *crt_errno() = e; return r; }
API double _wtof(const WCHAR *s) { return ms_wcstod(s, NULL); }
API int _wtoi_l(const WCHAR *s, void *l) { (void)l; return _wtoi(s); }
API double _wtof_l(const WCHAR *s, void *l) { (void)l; return _wtof(s); }

/* ================================================================== */
/* Number -> string                                                     */
/* ================================================================== */

static void utoa_base(unsigned long long v, char *buf, int radix, int neg)
{
    char tmp[72];
    int n = 0;
    do { int d = (int)(v % (unsigned)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    char *o = buf;
    if (neg) *o++ = '-';
    while (n) *o++ = tmp[--n];
    *o = 0;
}
static void itoa_any(long long v, int bits, int is_signed, char *buf, int radix)
{
    if (radix < 2 || radix > 36) { buf[0] = 0; crt_set_errno(MS_EINVAL); return; }
    unsigned long long u = bits == 32 ? (unsigned long long)(uint32_t)v : (unsigned long long)v;
    if (is_signed && radix == 10 && v < 0) utoa_base(bits == 32 ? (unsigned long long)(-(long long)(int32_t)v) : 0 - (unsigned long long)v, buf, radix, 1);
    else utoa_base(u, buf, radix, 0);
}
static WCHAR *widen_into(const char *a, WCHAR *w) { size_t i = 0; for (; a[i]; i++) w[i] = (unsigned char)a[i]; w[i] = 0; return w; }

API char *_itoa(int v, char *b, int r) { itoa_any(v, 32, 1, b, r); return b; }
API char *_ltoa(int32_t v, char *b, int r) { itoa_any(v, 32, 1, b, r); return b; }
API char *_ultoa(uint32_t v, char *b, int r) { itoa_any((long long)v, 32, 0, b, r); return b; }
API char *_i64toa(long long v, char *b, int r) { itoa_any(v, 64, 1, b, r); return b; }
API char *_ui64toa(unsigned long long v, char *b, int r) { itoa_any((long long)v, 64, 0, b, r); return b; }
API WCHAR *_itow(int v, WCHAR *b, int r) { char t[72]; itoa_any(v, 32, 1, t, r); return widen_into(t, b); }
API WCHAR *_ltow(int32_t v, WCHAR *b, int r) { char t[72]; itoa_any(v, 32, 1, t, r); return widen_into(t, b); }
API WCHAR *_ultow(uint32_t v, WCHAR *b, int r) { char t[72]; itoa_any((long long)v, 32, 0, t, r); return widen_into(t, b); }
API WCHAR *_i64tow(long long v, WCHAR *b, int r) { char t[72]; itoa_any(v, 64, 1, t, r); return widen_into(t, b); }
API WCHAR *_ui64tow(unsigned long long v, WCHAR *b, int r) { char t[72]; itoa_any((long long)v, 64, 0, t, r); return widen_into(t, b); }

static int itoa_s_any(long long v, int bits, int is_signed, char *b, size_t n, int r)
{
    if (!b || !n || r < 2 || r > 36) { if (b && n) b[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    char t[72];
    itoa_any(v, bits, is_signed, t, r);
    if (strlen(t) >= n) { b[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    strcpy(b, t);
    return 0;
}
static int itow_s_any(long long v, int bits, int is_signed, WCHAR *b, size_t n, int r)
{
    if (!b || !n || r < 2 || r > 36) { if (b && n) b[0] = 0; crt_invalid_parameter(); return MS_EINVAL; }
    char t[72];
    itoa_any(v, bits, is_signed, t, r);
    if (strlen(t) >= n) { b[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    widen_into(t, b);
    return 0;
}
API int _itoa_s(int v, char *b, size_t n, int r) { return itoa_s_any(v, 32, 1, b, n, r); }
API int _ltoa_s(int32_t v, char *b, size_t n, int r) { return itoa_s_any(v, 32, 1, b, n, r); }
API int _ultoa_s(uint32_t v, char *b, size_t n, int r) { return itoa_s_any((long long)v, 32, 0, b, n, r); }
API int _i64toa_s(long long v, char *b, size_t n, int r) { return itoa_s_any(v, 64, 1, b, n, r); }
API int _ui64toa_s(unsigned long long v, char *b, size_t n, int r) { return itoa_s_any((long long)v, 64, 0, b, n, r); }
API int _itow_s(int v, WCHAR *b, size_t n, int r) { return itow_s_any(v, 32, 1, b, n, r); }
API int _ltow_s(int32_t v, WCHAR *b, size_t n, int r) { return itow_s_any(v, 32, 1, b, n, r); }
API int _ultow_s(uint32_t v, WCHAR *b, size_t n, int r) { return itow_s_any((long long)v, 32, 0, b, n, r); }
API int _i64tow_s(long long v, WCHAR *b, size_t n, int r) { return itow_s_any(v, 64, 1, b, n, r); }
API int _ui64tow_s(unsigned long long v, WCHAR *b, size_t n, int r) { return itow_s_any((long long)v, 64, 0, b, n, r); }

/* _ecvt / _fcvt / _gcvt */
static void cvt_digits(double v, int ndig, int fixed, char *out, size_t outn, int *dec, int *sign)
{
    char buf[400];
    *sign = signbit(v) ? 1 : 0;
    if (*sign) v = -v;
    if (!isfinite(v)) { snprintf(out, outn, "%s", isnan(v) ? "1#QNAN" : "1#INF"); *dec = 1; return; }
    if (fixed) snprintf(buf, sizeof buf, "%.*f", ndig < 0 ? 0 : (ndig > 340 ? 340 : ndig), v);
    else snprintf(buf, sizeof buf, "%.*e", ndig > 1 ? ndig - 1 : 0, v);
    /* collect digits and compute decimal point position */
    char digits[400];
    size_t nd = 0;
    int point = -1, exp10 = 0;
    for (char *p = buf; *p; p++) {
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
        else if (*p == '.') point = (int)nd;
        else if (*p == 'e') { exp10 = atoi(p + 1); break; }
    }
    if (point < 0) point = (int)nd;
    digits[nd] = 0;
    if (fixed) {
        /* strip leading zeros */
        size_t lz = 0;
        while (lz < nd && digits[lz] == '0') lz++;
        if (lz == nd) { *dec = 0; snprintf(out, outn, "%s", ndig > 0 ? digits + nd - (size_t)(ndig > (int)nd ? (int)nd : ndig) : ""); return; }
        *dec = point - (int)lz;
        snprintf(out, outn, "%s", digits + lz);
    } else {
        *dec = v == 0 ? 0 : exp10 + 1;
        if (ndig <= 0) { *dec = exp10 + 1; out[0] = 0; return; }
        snprintf(out, outn, "%s", digits);
    }
}
API char *_ecvt(double v, int n, int *dec, int *sign)
{
    static __thread char buf[400];
    cvt_digits(v, n, 0, buf, sizeof buf, dec, sign);
    return buf;
}
API char *_fcvt(double v, int n, int *dec, int *sign)
{
    static __thread char buf[400];
    cvt_digits(v, n, 1, buf, sizeof buf, dec, sign);
    return buf;
}
API int _ecvt_s(char *b, size_t n, double v, int nd, int *dec, int *sign)
{ if (!b || !n) return MS_EINVAL; cvt_digits(v, nd, 0, b, n, dec, sign); return 0; }
API int _fcvt_s(char *b, size_t n, double v, int nd, int *dec, int *sign)
{ if (!b || !n) return MS_EINVAL; cvt_digits(v, nd, 1, b, n, dec, sign); return 0; }
API char *_gcvt(double v, int n, char *b)
{
    char *s = b;
    snprintf(b, 400, "%.*g", n, v);
    /* Microsoft keeps a trailing '.' for integral values: 3 -> "3." */
    if (!strpbrk(s, ".eEnN")) strcat(s, ".");
    return b;
}
API int _gcvt_s(char *b, size_t n, double v, int nd)
{
    if (!b || !n) return MS_EINVAL;
    char t[400];
    _gcvt(v, nd, t);
    if (strlen(t) >= n) { b[0] = 0; return MS_ERANGE; }
    strcpy(b, t);
    return 0;
}

/* ================================================================== */
/* Multibyte <-> wide (UTF-8 <-> UTF-16)                                */
/* ================================================================== */

typedef struct { uint32_t wchar; uint16_t bytes, state; } ms_mbstate;   /* 8 bytes */
static __thread ms_mbstate t_mbrtowc_state, t_wcrtomb_state, t_mbrlen_state;

static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return -1;
}

/* Decode one UTF-8 character from s (at most n bytes). Returns bytes used,
 * 0 for NUL, -1 invalid, -2 incomplete. */
static int utf8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    if (!n) return -2;
    int l = utf8_len(s[0]);
    if (l < 0) return -1;
    if ((size_t)l > n) {
        for (size_t i = 1; i < n; i++) if ((s[i] & 0xC0) != 0x80) return -1;
        return -2;
    }
    uint32_t c = l == 1 ? s[0] : l == 2 ? (s[0] & 0x1Fu) : l == 3 ? (s[0] & 0x0Fu) : (s[0] & 0x07u);
    for (int i = 1; i < l; i++) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        c = (c << 6) | (s[i] & 0x3Fu);
    }
    if ((l == 2 && c < 0x80) || (l == 3 && c < 0x800) || (l == 4 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF)) return -1;
    *cp = c;
    return c ? l : 0;
}

static int utf8_encode(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

API size_t ms_mbrtowc(WCHAR *out, const char *s, size_t n, ms_mbstate *st)
{
    if (!st) st = &t_mbrtowc_state;
    if (st->state == 2) {                 /* pending low surrogate */
        if (out) *out = (WCHAR)st->wchar;
        st->state = 0; st->wchar = 0;
        return (size_t)-3;
    }
    if (!s) { memset(st, 0, sizeof *st); return 0; }
    unsigned char buf[4];
    size_t have = st->state == 1 ? st->bytes : 0;
    for (size_t i = 0; i < have; i++) buf[i] = (unsigned char)(st->wchar >> (8 * i));
    size_t take = 0;
    while (have + take < 4 && take < n) { buf[have + take] = (unsigned char)s[take]; take++; }
    uint32_t cp = 0;
    int r = utf8_decode(buf, have + take, &cp);
    if (r == -1) { memset(st, 0, sizeof *st); crt_set_errno(MS_EILSEQ); return (size_t)-1; }
    if (r == -2) {
        uint32_t acc = 0;
        for (size_t i = 0; i < have + take; i++) acc |= (uint32_t)buf[i] << (8 * i);
        st->wchar = acc; st->bytes = (uint16_t)(have + take); st->state = 1;
        return (size_t)-2;
    }
    size_t used = r == 0 ? 1 : (size_t)r;
    memset(st, 0, sizeof *st);
    if (cp >= 0x10000) {
        cp -= 0x10000;
        if (out) *out = (WCHAR)(0xD800 + (cp >> 10));
        st->state = 2; st->wchar = 0xDC00 + (cp & 0x3FF);
    } else if (out) *out = (WCHAR)cp;
    return r == 0 ? 0 : used - have;
}
API size_t ms_mbrlen(const char *s, size_t n, ms_mbstate *st) { return ms_mbrtowc(NULL, s, n, st ? st : &t_mbrlen_state); }
API size_t ms_wcrtomb(char *out, WCHAR wc, ms_mbstate *st)
{
    if (!st) st = &t_wcrtomb_state;
    char tmp[4];
    if (!out) { memset(st, 0, sizeof *st); return 1; }
    if (wc >= 0xD800 && wc <= 0xDBFF) { st->wchar = wc; st->state = 3; return 0; }
    uint32_t cp = wc;
    if (wc >= 0xDC00 && wc <= 0xDFFF) {
        if (st->state != 3) { crt_set_errno(MS_EILSEQ); return (size_t)-1; }
        cp = 0x10000 + ((st->wchar - 0xD800) << 10) + (wc - 0xDC00u);
    }
    memset(st, 0, sizeof *st);
    int l = utf8_encode(cp, tmp);
    memcpy(out, tmp, (size_t)l);
    return (size_t)l;
}
API int wcrtomb_s(size_t *ret, char *out, size_t n, WCHAR wc, ms_mbstate *st)
{
    char tmp[4];
    size_t r = ms_wcrtomb(tmp, wc, st);
    if (r == (size_t)-1) { if (ret) *ret = r; return MS_EILSEQ; }
    if (out && r > n) return MS_ERANGE;
    if (out) memcpy(out, tmp, r);
    if (ret) *ret = r;
    return 0;
}
API int ms_mbsinit(const ms_mbstate *st) { return !st || !st->state; }
API unsigned ms_btowc(int c) { return (c >= 0 && c < 0x80) ? (unsigned)c : 0xFFFF; }
API int ms_wctob(unsigned c) { return c < 0x80 ? (int)c : -1; }

API int ms_mbtowc(WCHAR *out, const char *s, size_t n)
{
    if (!s) return 0;
    if (!n) return -1;
    uint32_t cp = 0;
    int r = utf8_decode((const unsigned char *)s, n, &cp);
    if (r < 0) { crt_set_errno(MS_EILSEQ); return -1; }
    if (out) *out = cp >= 0x10000 ? (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)) : (WCHAR)cp;
    return r;
}
API int _mbtowc_l(WCHAR *out, const char *s, size_t n, void *l) { (void)l; return ms_mbtowc(out, s, n); }
API int ms_mblen(const char *s, size_t n) { return ms_mbtowc(NULL, s, n); }
API int _mblen_l(const char *s, size_t n, void *l) { (void)l; return ms_mblen(s, n); }
API int ms_wctomb(char *out, WCHAR wc)
{
    if (!out) return 0;
    if (wc >= 0xD800 && wc <= 0xDFFF) { crt_set_errno(MS_EILSEQ); return -1; }
    return utf8_encode(wc, out);
}
API int wctomb_s(int *ret, char *out, size_t n, WCHAR wc)
{
    char tmp[4];
    if (!out) { if (ret) *ret = 0; return 0; }
    int l = ms_wctomb(tmp, wc);
    if (l < 0) { if (ret) *ret = -1; return MS_EILSEQ; }
    if ((size_t)l > n) return MS_ERANGE;
    memcpy(out, tmp, (size_t)l);
    if (ret) *ret = l;
    return 0;
}

/* Convert up to `max` WCHARs (excluding NUL). Returns count or -1. */
static size_t mbs_to_wcs(WCHAR *d, const char **src, size_t max)
{
    const unsigned char *s = (const unsigned char *)*src;
    size_t n = 0;
    for (;;) {
        uint32_t cp = 0;
        int r = utf8_decode(s, 4, &cp);
        if (r == -2) { r = utf8_decode(s, strnlen((const char *)s, 4), &cp); }
        if (r < 0) { crt_set_errno(MS_EILSEQ); *src = (const char *)s; return (size_t)-1; }
        if (r == 0) { if (d && n < max) d[n] = 0; *src = NULL; return n; }
        size_t need = cp >= 0x10000 ? 2 : 1;
        if (d && n + need > max) { *src = (const char *)s; return n; }
        if (d) {
            if (need == 2) { d[n] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); d[n + 1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
            else d[n] = (WCHAR)cp;
        }
        n += need;
        s += r;
    }
}

static size_t wcs_to_mbs(char *d, const WCHAR **src, size_t max)
{
    const WCHAR *s = *src;
    size_t n = 0;
    for (;;) {
        uint32_t cp = *s;
        size_t used = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[1] - 0xDC00u); used = 2; }
        else if (cp >= 0xD800 && cp <= 0xDFFF) { crt_set_errno(MS_EILSEQ); *src = s; return (size_t)-1; }
        if (!cp) { if (d && n < max) d[n] = 0; *src = NULL; return n; }
        char tmp[4];
        int l = utf8_encode(cp, tmp);
        if (d && n + (size_t)l > max) { *src = s; return n; }
        if (d) memcpy(d + n, tmp, (size_t)l);
        n += (size_t)l;
        s += used;
    }
}

API size_t ms_mbstowcs(WCHAR *d, const char *s, size_t n) { const char *p = s; return mbs_to_wcs(d, &p, n); }
API size_t ms_wcstombs(char *d, const WCHAR *s, size_t n) { const WCHAR *p = s; return wcs_to_mbs(d, &p, n); }
API size_t _mbstowcs_l(WCHAR *d, const char *s, size_t n, void *l) { (void)l; return ms_mbstowcs(d, s, n); }
API size_t _wcstombs_l(char *d, const WCHAR *s, size_t n, void *l) { (void)l; return ms_wcstombs(d, s, n); }
API size_t ms_mbsrtowcs(WCHAR *d, const char **s, size_t n, ms_mbstate *st) { (void)st; return mbs_to_wcs(d, s, n); }
API size_t ms_wcsrtombs(char *d, const WCHAR **s, size_t n, ms_mbstate *st) { (void)st; return wcs_to_mbs(d, s, n); }

#define S_TRUNC ((size_t)-1)
API int mbstowcs_s(size_t *ret, WCHAR *d, size_t dn, const char *s, size_t count)
{
    if (!s && count) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!d) {
        size_t r = ms_mbstowcs(NULL, s, 0);
        if (r == (size_t)-1) return MS_EILSEQ;
        if (ret) *ret = r + 1;
        return 0;
    }
    if (!dn) { crt_invalid_parameter(); return MS_EINVAL; }
    size_t lim = count == S_TRUNC ? dn - 1 : (count < dn ? count : dn);
    const char *p = s;
    size_t r = mbs_to_wcs(d, &p, lim);
    if (r == (size_t)-1) { d[0] = 0; return MS_EILSEQ; }
    if (p && count != S_TRUNC && r >= dn) { d[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    if (r >= dn) r = dn - 1;
    d[r] = 0;
    if (ret) *ret = r + 1;
    return p && count == S_TRUNC ? MS_STRUNCATE : 0;
}
API int wcstombs_s(size_t *ret, char *d, size_t dn, const WCHAR *s, size_t count)
{
    if (!s && count) { crt_invalid_parameter(); return MS_EINVAL; }
    if (!d) {
        size_t r = ms_wcstombs(NULL, s, 0);
        if (r == (size_t)-1) return MS_EILSEQ;
        if (ret) *ret = r + 1;
        return 0;
    }
    if (!dn) { crt_invalid_parameter(); return MS_EINVAL; }
    size_t lim = count == S_TRUNC ? dn - 1 : (count < dn ? count : dn - 1);
    const WCHAR *p = s;
    size_t r = wcs_to_mbs(d, &p, lim);
    if (r == (size_t)-1) { d[0] = 0; return MS_EILSEQ; }
    if (p && count != S_TRUNC && count >= dn) { d[0] = 0; crt_invalid_parameter(); return MS_ERANGE; }
    d[r] = 0;
    if (ret) *ret = r + 1;
    return p && count == S_TRUNC ? MS_STRUNCATE : 0;
}
API int mbsrtowcs_s(size_t *ret, WCHAR *d, size_t dn, const char **s, size_t count, ms_mbstate *st)
{
    (void)st;
    int r = mbstowcs_s(ret, d, dn, *s, count);
    if (!r) *s = NULL;
    return r;
}
API int wcsrtombs_s(size_t *ret, char *d, size_t dn, const WCHAR **s, size_t count, ms_mbstate *st)
{
    (void)st;
    int r = wcstombs_s(ret, d, dn, *s, count);
    if (!r) *s = NULL;
    return r;
}

/* ================================================================== */
/* mbstring.h: LSIW's code page is UTF-8, which has no DBCS lead bytes,  */
/* so the _mbs* functions behave like their byte-string counterparts.   */
/* ================================================================== */

API size_t _mbslen(const unsigned char *s) { return strlen((const char *)s); }
API size_t _mbstrlen(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}
API size_t _mbstrnlen(const char *s, size_t max)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && (size_t)(p - (const unsigned char *)s) < max; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}
API size_t _mbsnlen(const unsigned char *s, size_t max) { return strnlen((const char *)s, max); }
API int _mbscmp(const unsigned char *a, const unsigned char *b) { return ms_strcmp((const char *)a, (const char *)b); }
API int _mbsncmp(const unsigned char *a, const unsigned char *b, size_t n) { return ms_strncmp((const char *)a, (const char *)b, n); }
API int _mbsnbcmp(const unsigned char *a, const unsigned char *b, size_t n) { return ms_strncmp((const char *)a, (const char *)b, n); }
API int _mbsicmp(const unsigned char *a, const unsigned char *b) { return _stricmp((const char *)a, (const char *)b); }
API int _mbsnicmp(const unsigned char *a, const unsigned char *b, size_t n) { return _strnicmp((const char *)a, (const char *)b, n); }
API int _mbsnbicmp(const unsigned char *a, const unsigned char *b, size_t n) { return _strnicmp((const char *)a, (const char *)b, n); }
API int _mbscoll(const unsigned char *a, const unsigned char *b) { return ms_strcmp((const char *)a, (const char *)b); }
API int _mbsicoll(const unsigned char *a, const unsigned char *b) { return _stricmp((const char *)a, (const char *)b); }
API unsigned char *_mbschr(const unsigned char *s, unsigned c) { return (unsigned char *)strchr((const char *)s, (int)c); }
API unsigned char *_mbsrchr(const unsigned char *s, unsigned c) { return (unsigned char *)strrchr((const char *)s, (int)c); }
API unsigned char *_mbsstr(const unsigned char *h, const unsigned char *n) { return (unsigned char *)strstr((const char *)h, (const char *)n); }
API unsigned char *_mbscpy(unsigned char *d, const unsigned char *s) { return (unsigned char *)strcpy((char *)d, (const char *)s); }
API unsigned char *_mbscat(unsigned char *d, const unsigned char *s) { return (unsigned char *)strcat((char *)d, (const char *)s); }
API unsigned char *_mbsncpy(unsigned char *d, const unsigned char *s, size_t n) { return (unsigned char *)strncpy((char *)d, (const char *)s, n); }
API unsigned char *_mbsnbcpy(unsigned char *d, const unsigned char *s, size_t n) { return (unsigned char *)strncpy((char *)d, (const char *)s, n); }
API unsigned char *_mbsncat(unsigned char *d, const unsigned char *s, size_t n) { return (unsigned char *)strncat((char *)d, (const char *)s, n); }
API unsigned char *_mbsnbcat(unsigned char *d, const unsigned char *s, size_t n) { return (unsigned char *)strncat((char *)d, (const char *)s, n); }
API unsigned char *_mbsdup(const unsigned char *s) { return (unsigned char *)_strdup((const char *)s); }
API unsigned char *_mbslwr(unsigned char *s) { return (unsigned char *)_strlwr((char *)s); }
API unsigned char *_mbsupr(unsigned char *s) { return (unsigned char *)_strupr((char *)s); }
API unsigned char *_mbsrev(unsigned char *s) { return (unsigned char *)_strrev((char *)s); }
API size_t _mbsspn(const unsigned char *s, const unsigned char *a) { return strspn((const char *)s, (const char *)a); }
API size_t _mbscspn(const unsigned char *s, const unsigned char *a) { return strcspn((const char *)s, (const char *)a); }
API unsigned char *_mbsspnp(const unsigned char *s, const unsigned char *a) { s += strspn((const char *)s, (const char *)a); return *s ? (unsigned char *)s : NULL; }
API unsigned char *_mbspbrk(const unsigned char *s, const unsigned char *a) { return (unsigned char *)strpbrk((const char *)s, (const char *)a); }
API unsigned char *_mbstok(unsigned char *s, const unsigned char *d) { return (unsigned char *)ms_strtok((char *)s, (const char *)d); }
API unsigned char *_mbstok_s(unsigned char *s, const unsigned char *d, unsigned char **ctx) { return (unsigned char *)strtok_s((char *)s, (const char *)d, (char **)ctx); }
API unsigned char *_mbsinc(const unsigned char *s) { return (unsigned char *)s + 1; }
API unsigned char *_mbsninc(const unsigned char *s, size_t n) { return (unsigned char *)s + strnlen((const char *)s, n); }
API unsigned char *_mbsdec(const unsigned char *start, const unsigned char *cur) { return cur > start ? (unsigned char *)cur - 1 : NULL; }
API unsigned _mbsnextc(const unsigned char *s) { return *s; }
API size_t _mbclen(const unsigned char *s) { (void)s; return 1; }
API void _mbccpy(unsigned char *d, const unsigned char *s) { *d = *s; }
API size_t _mbsnccnt(const unsigned char *s, size_t n) { return strnlen((const char *)s, n); }
API size_t _mbsnbcnt(const unsigned char *s, size_t n) { return strnlen((const char *)s, n); }
API unsigned char *_mbsset(unsigned char *s, unsigned c) { return (unsigned char *)_strset((char *)s, (int)c); }
API unsigned char *_mbsnset(unsigned char *s, unsigned c, size_t n) { return (unsigned char *)_strnset((char *)s, (int)c, n); }
API unsigned char *_mbsnbset(unsigned char *s, unsigned c, size_t n) { return (unsigned char *)_strnset((char *)s, (int)c, n); }
API int _mbscpy_s(unsigned char *d, size_t n, const unsigned char *s) { return strcpy_s((char *)d, n, (const char *)s); }
API int _mbscat_s(unsigned char *d, size_t n, const unsigned char *s) { return strcat_s((char *)d, n, (const char *)s); }
API int _mbsnbcpy_s(unsigned char *d, size_t n, const unsigned char *s, size_t c) { return strncpy_s((char *)d, n, (const char *)s, c); }
API int _mbslwr_s(unsigned char *s, size_t n) { return _strlwr_s((char *)s, n); }
API int _mbsupr_s(unsigned char *s, size_t n) { return _strupr_s((char *)s, n); }
API int _ismbblead(unsigned c) { (void)c; return 0; }
API int _ismbbtrail(unsigned c) { (void)c; return 0; }
API int _ismbslead(const unsigned char *s, const unsigned char *c) { (void)s; (void)c; return 0; }
API int _ismbstrail(const unsigned char *s, const unsigned char *c) { (void)s; (void)c; return 0; }
API int _ismbcalpha(unsigned c) { return c < 256 && ms_isalpha((int)c); }
API int _ismbcdigit(unsigned c) { return c < 256 && ms_isdigit((int)c); }
API int _ismbcspace(unsigned c) { return c < 256 && ms_isspace((int)c); }
API int _ismbcupper(unsigned c) { return c < 256 && ms_isupper((int)c); }
API int _ismbclower(unsigned c) { return c < 256 && ms_islower((int)c); }
API int _ismbcprint(unsigned c) { return c < 256 && ms_isprint((int)c); }
API int _ismbcalnum(unsigned c) { return c < 256 && ms_isalnum((int)c); }
API int _ismbcpunct(unsigned c) { return c < 256 && ms_ispunct((int)c); }
API int _ismbcgraph(unsigned c) { return c < 256 && ms_isgraph((int)c); }
API int _ismbblank(unsigned c) { return c == ' ' || c == '\t'; }
API unsigned _mbctolower(unsigned c) { return c < 256 ? (unsigned)a_lower((int)c) : c; }
API unsigned _mbctoupper(unsigned c) { return c < 256 ? (unsigned)a_upper((int)c) : c; }
API unsigned _mbbtombc(unsigned c) { return c; }
API unsigned _mbctombb(unsigned c) { return c; }
static unsigned char g_mbctype[257];
unsigned char *crt_mbctype = g_mbctype;
API unsigned char *__p__mbctype(void) { return g_mbctype; }

const struct lsiw_export lsiw_crt_string_exports[] = {
    /* ctype */
    EM(isalpha), EM(isupper), EM(islower), EM(isdigit), EM(isxdigit), EM(isspace), EM(ispunct), EM(isalnum),
    EM(isprint), EM(isgraph), EM(iscntrl), EM(isblank), EM(toupper), EM(tolower), EM(_toupper), EM(_tolower),
    EM(__isascii), EM(__toascii), EX("isascii", ms___isascii), EX("toascii", ms___toascii),
    E(__iscsym), E(__iscsymf), EX("iscsym", __iscsym), EX("iscsymf", __iscsymf), E(_isctype), E(_isctype_l),
    E(__pctype_func), E(__p__pctype), E(__pwctype_func), E(__p__pwctype),
    E(_isalpha_l), E(_isupper_l), E(_islower_l), E(_isdigit_l), E(_isxdigit_l), E(_isspace_l), E(_ispunct_l),
    E(_isalnum_l), E(_isprint_l), E(_isgraph_l), E(_iscntrl_l), E(_isblank_l), E(_toupper_l), E(_tolower_l),
    EM(iswalpha), EM(iswupper), EM(iswlower), EM(iswdigit), EM(iswxdigit), EM(iswspace), EM(iswpunct), EM(iswalnum),
    EM(iswprint), EM(iswgraph), EM(iswcntrl), EM(iswblank), EM(iswascii), EM(iswctype), E(is_wctype), E(_iswctype_l),
    EM(towupper), EM(towlower), E(__iswcsym), E(__iswcsymf),
    E(_iswalpha_l), E(_iswupper_l), E(_iswlower_l), E(_iswdigit_l), E(_iswxdigit_l), E(_iswspace_l), E(_iswpunct_l),
    E(_iswalnum_l), E(_iswprint_l), E(_iswgraph_l), E(_iswcntrl_l), E(_iswblank_l), E(_towupper_l), E(_towlower_l),
    EM(wctype), EM(towctrans), EM(wctrans),
    /* narrow */
    EM(memcpy), EM(memmove), EM(memset), EM(memcmp), EM(memchr), E(_memccpy), E(_memicmp), E(_memicmp_l),
    E(memcpy_s), E(memmove_s), EX("memccpy", _memccpy), EX("memicmp", _memicmp),
    EM(strlen), EM(strnlen), EM(strcpy), EM(strncpy), EM(strcat), EM(strncat), EM(strcmp), EM(strncmp),
    EM(strchr), EM(strrchr), EM(strstr), EM(strspn), EM(strcspn), EM(strpbrk), EM(strcoll), E(_strcoll_l), E(_strncoll), EM(strxfrm),
    E(_stricmp), E(_strnicmp), E(_stricmp_l), E(_strnicmp_l), EX("_strcmpi", _stricmp), E(_stricoll), E(_strnicoll), E(_stricoll_l),
    E(_strdup), E(_strlwr), E(_strupr), E(_strlwr_l), E(_strupr_l), E(_strlwr_s), E(_strupr_s), E(_strrev), E(_strset), E(_strnset),
    EX("strdup", _strdup), EX("stricmp", _stricmp), EX("strnicmp", _strnicmp), EX("strcmpi", _stricmp), EX("strlwr", _strlwr),
    EX("strupr", _strupr), EX("strrev", _strrev), EX("strset", _strset), EX("strnset", _strnset),
    E(strcpy_s), E(strncpy_s), E(strcat_s), E(strncat_s), E(strnlen_s), E(strtok_s), EM(strtok),
    EM(strerror), E(strerror_s), E(_strerror), E(_strerror_s), E(__sys_errlist), E(__sys_nerr),
    /* wide */
    EM(wcslen), EM(wcsnlen), E(wcsnlen_s), EM(wcscpy), EM(wcsncpy), EM(wcscat), EM(wcsncat), EM(wcscmp), EM(wcsncmp),
    EM(wcschr), EM(wcsrchr), EM(wcsstr), EM(wcsspn), EM(wcscspn), EM(wcspbrk), EM(wcscoll), E(_wcscoll_l), E(_wcsncoll), EM(wcsxfrm),
    E(_wcsicmp), E(_wcsnicmp), E(_wcsicmp_l), E(_wcsnicmp_l), E(_wcsicoll), E(_wcsnicoll), E(_wcsdup),
    E(_wcslwr), E(_wcsupr), E(_wcslwr_l), E(_wcsupr_l), E(_wcslwr_s), E(_wcsupr_s), E(_wcsrev), E(_wcsset), E(_wcsnset),
    EX("wcsdup", _wcsdup), EX("wcsicmp", _wcsicmp), EX("wcsnicmp", _wcsnicmp), EX("wcslwr", _wcslwr), EX("wcsupr", _wcsupr),
    EX("wcsrev", _wcsrev), EX("wcsset", _wcsset), EX("wcsnset", _wcsnset), EX("wcsicoll", _wcsicoll),
    E(wcscpy_s), E(wcsncpy_s), E(wcscat_s), E(wcsncat_s), E(wcstok_s), EX("_wcstok", msvcrt_wcstok),
    EM(wmemcpy), EM(wmemmove), EM(wmemset), EM(wmemcmp), EM(wmemchr), E(wmemcpy_s), E(wmemmove_s),
    E(_wcserror), E(_wcserror_s), E(__wcserror),
    /* conversions */
    EM(strtol), EM(strtoul), EM(strtoll), EM(strtoull), EX("_strtoi64", ms_strtoll), EX("_strtoui64", ms_strtoull),
    EX("strtoimax", ms_strtoll), EX("strtoumax", ms_strtoull),
    E(_strtol_l), E(_strtoul_l), E(_strtoi64_l), E(_strtoui64_l), EX("_strtoll_l", _strtoi64_l), EX("_strtoull_l", _strtoui64_l),
    EM(atoi), EM(atol), EM(atoll), EX("_atoi64", ms_atoll), EX("_atoll", ms_atoll), E(_atoi_l), E(_atol_l), E(_atoi64_l), EX("_atoll_l", _atoi64_l),
    EM(strtod), EM(strtold), EM(strtof), EM(atof), E(_strtod_l), E(_strtof_l), E(_atof_l), EX("_strtold_l", _strtod_l),
    E(_atodbl), E(_atoflt), E(_atoldbl),
    EM(wcstol), EM(wcstoul), EM(wcstoll), EM(wcstoull), EX("_wcstoi64", ms_wcstoll), EX("_wcstoui64", ms_wcstoull),
    EX("wcstoimax", ms_wcstoll), EX("wcstoumax", ms_wcstoull),
    EM(wcstod), EM(wcstof), EX("wcstold", ms_wcstod), E(_wcstol_l), E(_wcstoul_l), E(_wcstoi64_l), E(_wcstoui64_l), E(_wcstod_l),
    E(_wtoi), EX("_wtol", _wtoi), E(_wtoi64), EX("_wtoll", _wtoi64), E(_wtof), E(_wtoi_l), EX("_wtol_l", _wtoi_l), E(_wtof_l),
    E(_itoa), E(_ltoa), E(_ultoa), E(_i64toa), E(_ui64toa), E(_itow), E(_ltow), E(_ultow), E(_i64tow), E(_ui64tow),
    EX("itoa", _itoa), EX("ltoa", _ltoa), EX("ultoa", _ultoa),
    E(_itoa_s), E(_ltoa_s), E(_ultoa_s), E(_i64toa_s), E(_ui64toa_s), E(_itow_s), E(_ltow_s), E(_ultow_s), E(_i64tow_s), E(_ui64tow_s),
    E(_ecvt), E(_fcvt), E(_ecvt_s), E(_fcvt_s), E(_gcvt), E(_gcvt_s), EX("ecvt", _ecvt), EX("fcvt", _fcvt), EX("gcvt", _gcvt),
    /* multibyte */
    EM(mbrtowc), EM(mbrlen), EM(wcrtomb), E(wcrtomb_s), EM(mbsinit), EM(btowc), EM(wctob), EM(mbtowc), E(_mbtowc_l),
    EM(mblen), E(_mblen_l), EM(wctomb), E(wctomb_s), EM(mbstowcs), EM(wcstombs), E(_mbstowcs_l), E(_wcstombs_l),
    EM(mbsrtowcs), EM(wcsrtombs), E(mbstowcs_s), E(wcstombs_s), E(mbsrtowcs_s), E(wcsrtombs_s),
    /* mbstring */
    E(_mbslen), E(_mbstrlen), E(_mbstrnlen), E(_mbsnlen), E(_mbscmp), E(_mbsncmp), E(_mbsnbcmp), E(_mbsicmp), E(_mbsnicmp),
    E(_mbsnbicmp), E(_mbscoll), E(_mbsicoll), E(_mbschr), E(_mbsrchr), E(_mbsstr), E(_mbscpy), E(_mbscat), E(_mbsncpy),
    E(_mbsnbcpy), E(_mbsncat), E(_mbsnbcat), E(_mbsdup), E(_mbslwr), E(_mbsupr), E(_mbsrev), E(_mbsspn), E(_mbscspn),
    E(_mbsspnp), E(_mbspbrk), E(_mbstok), E(_mbstok_s), E(_mbsinc), E(_mbsninc), E(_mbsdec), E(_mbsnextc), E(_mbclen),
    E(_mbccpy), E(_mbsnccnt), E(_mbsnbcnt), E(_mbsset), E(_mbsnset), E(_mbsnbset), E(_mbscpy_s), E(_mbscat_s),
    E(_mbsnbcpy_s), E(_mbslwr_s), E(_mbsupr_s), E(_ismbblead), E(_ismbbtrail), E(_ismbslead), E(_ismbstrail),
    E(_ismbcalpha), E(_ismbcdigit), E(_ismbcspace), E(_ismbcupper), E(_ismbclower), E(_ismbcprint), E(_ismbcalnum),
    E(_ismbcpunct), E(_ismbcgraph), E(_ismbblank), E(_mbctolower), E(_mbctoupper), E(_mbbtombc), E(_mbctombb), E(__p__mbctype),
    /* data */
    EX("_pctype", &crt_pctype), EX("_pwctype", &crt_pwctype), EX("_ctype", g_ctype_table),
    EX("_sys_errlist", g_errlist), EX("_sys_nerr", &crt_sys_nerr), EX("_mbctype", g_mbctype),
    { NULL, NULL }
};

/* The two flavours of wcstok, referenced from the msvcrt/ucrt tables. */
/* wcstok: two arguments in msvcrt.dll, three (ISO C) in ucrtbase. */
WINAPI WCHAR *crt_wcstok_msvcrt(WCHAR *s, const WCHAR *d) { return msvcrt_wcstok(s, d); }
WINAPI WCHAR *crt_wcstok_ucrt(WCHAR *s, const WCHAR *d, WCHAR **ctx) { return ucrt_wcstok(s, d, ctx); }

__attribute__((constructor)) static void init_ctype_ctor(void) { pthread_once(&g_ctype_once, init_ctype); }
