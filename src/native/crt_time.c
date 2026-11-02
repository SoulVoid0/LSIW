/* C runtime time and locale: time.h, sys/timeb.h, locale.h.
 *
 * time_t is 64-bit, as on Windows x64 (the _32 variants are provided too).
 * The time zone comes from the Linux system (TZ / /etc/localtime).
 * The locale reports UTF-8 (code page 65001) for every category.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "crt.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ================================================================== */
/* Time zone                                                            */
/* ================================================================== */

int32_t crt_timezone;            /* seconds west of UTC */
int     crt_daylight;
int32_t crt_dstbias = -3600;
static char g_tzname0[64] = "UTC", g_tzname1[64] = "UTC";
char   *crt_tzname[2] = { g_tzname0, g_tzname1 };
static pthread_once_t g_tz_once = PTHREAD_ONCE_INIT;

static void do_tzset(void)
{
    tzset();
    crt_timezone = (int32_t)timezone;
    crt_daylight = daylight;
    snprintf(g_tzname0, sizeof g_tzname0, "%s", tzname[0] ? tzname[0] : "UTC");
    snprintf(g_tzname1, sizeof g_tzname1, "%s", tzname[1] ? tzname[1] : g_tzname0);
}
static void tz_init(void) { pthread_once(&g_tz_once, do_tzset); }

API void _tzset(void) { do_tzset(); }
API int _get_timezone(int32_t *s) { tz_init(); if (!s) return MS_EINVAL; *s = crt_timezone; return 0; }
API int _get_daylight(int *d) { tz_init(); if (!d) return MS_EINVAL; *d = crt_daylight; return 0; }
API int _get_dstbias(int32_t *b) { tz_init(); if (!b) return MS_EINVAL; *b = crt_dstbias; return 0; }
API int _get_tzname(size_t *ret, char *buf, size_t n, int idx)
{
    tz_init();
    if (idx < 0 || idx > 1) return MS_EINVAL;
    const char *s = crt_tzname[idx];
    size_t l = strlen(s) + 1;
    if (ret) *ret = l;
    if (!buf) return 0;
    if (n < l) return MS_ERANGE;
    memcpy(buf, s, l);
    return 0;
}
API int32_t *__p__timezone(void) { tz_init(); return &crt_timezone; }
API int *__p__daylight(void) { tz_init(); return &crt_daylight; }
API int32_t *__p__dstbias(void) { tz_init(); return &crt_dstbias; }
API char **__p__tzname(void) { tz_init(); return crt_tzname; }

/* ================================================================== */
/* struct tm conversion                                                 */
/* ================================================================== */

#define MAX_TIME64 32535215999LL     /* 3000-12-31 23:59:59 UTC */
#define MAX_TIME32 0x7fffffffLL

static void to_ms(const struct tm *h, struct ms_tm *m)
{
    m->tm_sec = h->tm_sec; m->tm_min = h->tm_min; m->tm_hour = h->tm_hour;
    m->tm_mday = h->tm_mday; m->tm_mon = h->tm_mon; m->tm_year = h->tm_year;
    m->tm_wday = h->tm_wday; m->tm_yday = h->tm_yday; m->tm_isdst = h->tm_isdst > 0;
}
static void to_host(const struct ms_tm *m, struct tm *h)
{
    memset(h, 0, sizeof *h);
    h->tm_sec = m->tm_sec; h->tm_min = m->tm_min; h->tm_hour = m->tm_hour;
    h->tm_mday = m->tm_mday; h->tm_mon = m->tm_mon; h->tm_year = m->tm_year;
    h->tm_wday = m->tm_wday; h->tm_yday = m->tm_yday; h->tm_isdst = m->tm_isdst;
}

static __thread struct ms_tm t_tm;

static int gm64(int64_t t, struct ms_tm *out)
{
    if (t < 0 || t > MAX_TIME64) { crt_set_errno(MS_EINVAL); return MS_EINVAL; }
    time_t tt = (time_t)t;
    struct tm h;
    if (!gmtime_r(&tt, &h)) { crt_set_errno(MS_EINVAL); return MS_EINVAL; }
    to_ms(&h, out);
    return 0;
}
static int local64(int64_t t, struct ms_tm *out)
{
    tz_init();
    if (t < 0 || t > MAX_TIME64) { crt_set_errno(MS_EINVAL); return MS_EINVAL; }
    time_t tt = (time_t)t;
    struct tm h;
    if (!localtime_r(&tt, &h)) { crt_set_errno(MS_EINVAL); return MS_EINVAL; }
    to_ms(&h, out);
    return 0;
}

API struct ms_tm *_gmtime64(const int64_t *t) { if (!t) { crt_invalid_parameter(); return NULL; } return gm64(*t, &t_tm) ? NULL : &t_tm; }
API struct ms_tm *_gmtime32(const int32_t *t) { if (!t) { crt_invalid_parameter(); return NULL; } return gm64(*t, &t_tm) ? NULL : &t_tm; }
API int _gmtime64_s(struct ms_tm *o, const int64_t *t)
{
    if (!o) return MS_EINVAL;
    if (!t) { memset(o, 0xff, sizeof *o); return MS_EINVAL; }
    int r = gm64(*t, o);
    if (r) memset(o, 0xff, sizeof *o);
    return r;
}
API int _gmtime32_s(struct ms_tm *o, const int32_t *t) { int64_t v = t ? *t : 0; return _gmtime64_s(o, t ? &v : NULL); }
API struct ms_tm *_localtime64(const int64_t *t) { if (!t) { crt_invalid_parameter(); return NULL; } return local64(*t, &t_tm) ? NULL : &t_tm; }
API struct ms_tm *_localtime32(const int32_t *t) { if (!t) { crt_invalid_parameter(); return NULL; } return local64(*t, &t_tm) ? NULL : &t_tm; }
API int _localtime64_s(struct ms_tm *o, const int64_t *t)
{
    if (!o) return MS_EINVAL;
    if (!t) { memset(o, 0xff, sizeof *o); return MS_EINVAL; }
    int r = local64(*t, o);
    if (r) memset(o, 0xff, sizeof *o);
    return r;
}
API int _localtime32_s(struct ms_tm *o, const int32_t *t) { int64_t v = t ? *t : 0; return _localtime64_s(o, t ? &v : NULL); }

static int64_t mk(struct ms_tm *m, int utc, int64_t max)
{
    tz_init();
    if (!m) { crt_invalid_parameter(); return -1; }
    struct tm h;
    to_host(m, &h);
    if (!utc && h.tm_isdst > 0) h.tm_isdst = 1;
    time_t r = utc ? timegm(&h) : mktime(&h);
    if (r == (time_t)-1 || r < 0 || r > max) { crt_set_errno(MS_EINVAL); return -1; }
    to_ms(&h, m);
    return (int64_t)r;
}
API int64_t _mktime64(struct ms_tm *m) { return mk(m, 0, MAX_TIME64); }
API int32_t _mktime32(struct ms_tm *m) { return (int32_t)mk(m, 0, MAX_TIME32); }
API int64_t _mkgmtime64(struct ms_tm *m) { return mk(m, 1, MAX_TIME64); }
API int32_t _mkgmtime32(struct ms_tm *m) { return (int32_t)mk(m, 1, MAX_TIME32); }

/* ================================================================== */
/* Clocks                                                               */
/* ================================================================== */

API int64_t _time64(int64_t *t) { int64_t v = (int64_t)time(NULL); if (t) *t = v; return v; }
API int32_t _time32(int32_t *t) { int32_t v = (int32_t)time(NULL); if (t) *t = v; return v; }
API double _difftime64(int64_t a, int64_t b) { return (double)(a - b); }
API double _difftime32(int32_t a, int32_t b) { return (double)a - (double)b; }

static struct timespec g_start;
__attribute__((constructor)) static void clock_start(void) { clock_gettime(CLOCK_MONOTONIC, &g_start); }
API int32_t ms_clock(void)
{
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return (int32_t)((n.tv_sec - g_start.tv_sec) * 1000 + (n.tv_nsec - g_start.tv_nsec) / 1000000);
}

struct timeb64 { int64_t time; unsigned short millitm; short timezone, dstflag; };
struct timeb32 { int32_t time; unsigned short millitm; short timezone, dstflag; };
static void ftime_fill(int64_t *sec, unsigned short *ms, short *tz, short *dst)
{
    tz_init();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm h;
    time_t t = ts.tv_sec;
    localtime_r(&t, &h);
    *sec = ts.tv_sec;
    *ms = (unsigned short)(ts.tv_nsec / 1000000);
    *tz = (short)(crt_timezone / 60);
    *dst = (short)(h.tm_isdst > 0);
}
API void _ftime64(struct timeb64 *b) { int64_t s; ftime_fill(&s, &b->millitm, &b->timezone, &b->dstflag); b->time = s; }
API void _ftime32(struct timeb32 *b) { int64_t s; ftime_fill(&s, &b->millitm, &b->timezone, &b->dstflag); b->time = (int32_t)s; }
API int _ftime64_s(struct timeb64 *b) { if (!b) return MS_EINVAL; _ftime64(b); return 0; }
API int _ftime32_s(struct timeb32 *b) { if (!b) return MS_EINVAL; _ftime32(b); return 0; }

struct ms_timespec64 { int64_t tv_sec; int32_t tv_nsec; };
struct ms_timespec32 { int32_t tv_sec; int32_t tv_nsec; };
API int _timespec64_get(struct ms_timespec64 *ts, int base)
{
    if (!ts || base != 1) return 0;
    struct timespec n;
    clock_gettime(CLOCK_REALTIME, &n);
    ts->tv_sec = n.tv_sec;
    ts->tv_nsec = (int32_t)n.tv_nsec;
    return base;
}
API int _timespec32_get(struct ms_timespec32 *ts, int base)
{
    if (!ts || base != 1) return 0;
    struct timespec n;
    clock_gettime(CLOCK_REALTIME, &n);
    ts->tv_sec = (int32_t)n.tv_sec;
    ts->tv_nsec = (int32_t)n.tv_nsec;
    return base;
}

/* ================================================================== */
/* Text forms                                                           */
/* ================================================================== */

static const char *const g_days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const g_months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                        "September", "October", "November", "December" };

static int tm_valid(const struct ms_tm *m)
{
    return m && m->tm_sec >= 0 && m->tm_sec <= 60 && m->tm_min >= 0 && m->tm_min <= 59 && m->tm_hour >= 0 && m->tm_hour <= 23 &&
           m->tm_mday >= 1 && m->tm_mday <= 31 && m->tm_mon >= 0 && m->tm_mon <= 11 && m->tm_wday >= 0 && m->tm_wday <= 6 &&
           m->tm_year >= 0 && m->tm_year <= 8099;
}

static void asctime_into(char *out, size_t n, const struct ms_tm *m)
{
    snprintf(out, n, "%.3s %.3s %02d %02d:%02d:%02d %d\n", g_days[m->tm_wday], g_months[m->tm_mon], m->tm_mday,
             m->tm_hour, m->tm_min, m->tm_sec, m->tm_year + 1900);
}
API char *ms_asctime(const struct ms_tm *m)
{
    static __thread char buf[64];
    if (!tm_valid(m)) { crt_set_errno(MS_EINVAL); return NULL; }
    asctime_into(buf, sizeof buf, m);
    return buf;
}
API int asctime_s(char *buf, size_t n, const struct ms_tm *m)
{
    if (!buf || n < 26) { if (buf && n) buf[0] = 0; return MS_EINVAL; }
    if (!tm_valid(m)) { buf[0] = 0; return MS_EINVAL; }
    asctime_into(buf, n, m);
    return 0;
}
API WCHAR *_wasctime(const struct ms_tm *m)
{
    static __thread WCHAR buf[64];
    char *a = ms_asctime(m);
    if (!a) return NULL;
    lsiw_u2w_buf(a, buf, 64);
    return buf;
}
API int _wasctime_s(WCHAR *buf, size_t n, const struct ms_tm *m)
{
    char tmp[64];
    int r = asctime_s(tmp, sizeof tmp, m);
    if (r) { if (buf && n) buf[0] = 0; return r; }
    if (!buf || n < 26) return MS_EINVAL;
    lsiw_u2w_buf(tmp, buf, (int)n);
    return 0;
}
API char *_ctime64(const int64_t *t) { struct ms_tm m; if (!t || local64(*t, &m)) return NULL; return ms_asctime(&m); }
API char *_ctime32(const int32_t *t) { struct ms_tm m; if (!t || local64(*t, &m)) return NULL; return ms_asctime(&m); }
API int _ctime64_s(char *buf, size_t n, const int64_t *t) { struct ms_tm m; if (!t || local64(*t, &m)) { if (buf && n) buf[0] = 0; return MS_EINVAL; } return asctime_s(buf, n, &m); }
API int _ctime32_s(char *buf, size_t n, const int32_t *t) { int64_t v = t ? *t : -1; return _ctime64_s(buf, n, t ? &v : NULL); }
API WCHAR *_wctime64(const int64_t *t) { struct ms_tm m; if (!t || local64(*t, &m)) return NULL; return _wasctime(&m); }
API WCHAR *_wctime32(const int32_t *t) { struct ms_tm m; if (!t || local64(*t, &m)) return NULL; return _wasctime(&m); }
API int _wctime64_s(WCHAR *buf, size_t n, const int64_t *t) { struct ms_tm m; if (!t || local64(*t, &m)) { if (buf && n) buf[0] = 0; return MS_EINVAL; } return _wasctime_s(buf, n, &m); }
API int _wctime32_s(WCHAR *buf, size_t n, const int32_t *t) { int64_t v = t ? *t : -1; return _wctime64_s(buf, n, t ? &v : NULL); }

API char *_strdate(char *buf)
{
    time_t t = time(NULL);
    struct tm h;
    localtime_r(&t, &h);
    strftime(buf, 9, "%m/%d/%y", &h);
    return buf;
}
API char *_strtime(char *buf)
{
    time_t t = time(NULL);
    struct tm h;
    localtime_r(&t, &h);
    snprintf(buf, 9, "%02d:%02d:%02d", h.tm_hour, h.tm_min, h.tm_sec);
    return buf;
}
API int _strdate_s(char *buf, size_t n) { if (!buf || n < 9) { if (buf && n) buf[0] = 0; return MS_ERANGE; } _strdate(buf); return 0; }
API int _strtime_s(char *buf, size_t n) { if (!buf || n < 9) { if (buf && n) buf[0] = 0; return MS_ERANGE; } _strtime(buf); return 0; }
API WCHAR *_wstrdate(WCHAR *buf) { char t[9]; _strdate(t); lsiw_u2w_buf(t, buf, 9); return buf; }
API WCHAR *_wstrtime(WCHAR *buf) { char t[9]; _strtime(t); lsiw_u2w_buf(t, buf, 9); return buf; }
API int _wstrdate_s(WCHAR *buf, size_t n) { if (!buf || n < 9) return MS_ERANGE; _wstrdate(buf); return 0; }
API int _wstrtime_s(WCHAR *buf, size_t n) { if (!buf || n < 9) return MS_ERANGE; _wstrtime(buf); return 0; }

/* strftime with the Microsoft "C" locale forms of %c, %x and %X and the
 * '#' flag (alternate form: no leading zeros, long date for %#c / %#x). */
static size_t ms_strftime_impl(char *out, size_t max, const char *fmt, const struct ms_tm *m)
{
    if (!out || !max || !fmt || !m) { crt_invalid_parameter(); if (out && max) out[0] = 0; return 0; }
    tz_init();
    size_t cap = strlen(fmt) * 4 + 64;
    char *f = malloc(cap);
    size_t o = 0;
#define FADD(s) do { size_t l_ = strlen(s); if (o + l_ + 1 >= cap) { cap = (o + l_ + 1) * 2; f = realloc(f, cap); } memcpy(f + o, s, l_); o += l_; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { char c[2] = { *p, 0 }; FADD(c); continue; }
        p++;
        int alt = 0;
        if (*p == '#') { alt = 1; p++; }
        if (!*p) break;
        switch (*p) {
        case 'c': FADD(alt ? "%A, %B %-d, %Y %H:%M:%S" : "%m/%d/%y %H:%M:%S"); break;
        case 'x': FADD(alt ? "%A, %B %-d, %Y" : "%m/%d/%y"); break;
        case 'X': FADD("%H:%M:%S"); break;
        case 'z': {
            long off = -crt_timezone + (m->tm_isdst > 0 ? 3600 : 0);
            char z[16];
            snprintf(z, sizeof z, "%c%02ld%02ld", off < 0 ? '-' : '+', labs(off) / 3600, (labs(off) % 3600) / 60);
            FADD(z);
            break; }
        case 'Z': { const char *n = crt_tzname[m->tm_isdst > 0]; for (const char *q = n; *q; q++) { if (*q == '%') FADD("%%"); else { char c[2] = { *q, 0 }; FADD(c); } } break; }
        case 'd': case 'H': case 'I': case 'j': case 'm': case 'M': case 'S': case 'U': case 'W': case 'y': case 'e':
            { char s[4] = { '%', (char)(alt ? '-' : *p), alt ? *p : 0, 0 }; FADD(s); break; }
        case '%': FADD("%%"); break;
        default: { char s[3] = { '%', *p, 0 }; FADD(s); break; }
        }
    }
    f[o] = 0;
#undef FADD
    struct tm h;
    to_host(m, &h);
    h.tm_zone = crt_tzname[m->tm_isdst > 0];
    h.tm_gmtoff = -crt_timezone + (m->tm_isdst > 0 ? 3600 : 0);
    static locale_t cl;
    if (!cl) cl = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    size_t r = strftime_l(out, max, f, &h, cl);
    free(f);
    if (!r && max) out[0] = 0;
    if (!r) crt_set_errno(MS_ERANGE);
    return r;
}
API size_t ms_strftime(char *out, size_t max, const char *fmt, const struct ms_tm *m) { return ms_strftime_impl(out, max, fmt, m); }
API size_t _strftime_l(char *out, size_t max, const char *fmt, const struct ms_tm *m, void *l) { (void)l; return ms_strftime_impl(out, max, fmt, m); }
API size_t _Strftime(char *out, size_t max, const char *fmt, const struct ms_tm *m, void *lc) { (void)lc; return ms_strftime_impl(out, max, fmt, m); }
API size_t ms_wcsftime(WCHAR *out, size_t max, const WCHAR *fmt, const struct ms_tm *m)
{
    if (!out || !max || !fmt) { crt_invalid_parameter(); return 0; }
    char *f = lsiw_w2u(fmt);
    size_t cap = max * 4 + 16;
    char *tmp = malloc(cap);
    size_t r = ms_strftime_impl(tmp, cap, f, m);
    free(f);
    if (!r) { free(tmp); out[0] = 0; return 0; }
    int wl = lsiw_utf8_to_utf16(tmp, -1, NULL, 0);
    if (wl < 0 || (size_t)wl > max) { free(tmp); out[0] = 0; crt_set_errno(MS_ERANGE); return 0; }
    lsiw_utf8_to_utf16(tmp, -1, out, (int)max);
    free(tmp);
    return (size_t)wl - 1;
}
API size_t _wcsftime_l(WCHAR *out, size_t max, const WCHAR *fmt, const struct ms_tm *m, void *l) { (void)l; return ms_wcsftime(out, max, fmt, m); }

/* ================================================================== */
/* Locale                                                               */
/* ================================================================== */

int crt_codepage = 65001;
int crt_mb_cur_max = 4;

#define MS_LC_ALL 0
#define MS_LC_MAX 5
static char *g_lc[MS_LC_MAX + 1];
static pthread_mutex_t g_lc_lock = PTHREAD_MUTEX_INITIALIZER;

static char *system_locale_name(void)
{
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LANG");
    char lang[16] = "en", country[16] = "US";
    if (l && *l && strcmp(l, "C") && strcmp(l, "POSIX")) {
        sscanf(l, "%15[a-zA-Z]_%15[a-zA-Z]", lang, country);
    }
    return lsiw_strdup_printf("%s-%s.utf8", lang, country);
}

static const char *set_category(int cat, const char *name)
{
    char *v;
    if (!strcmp(name, "")) v = system_locale_name();
    else v = strdup(name);
    free(g_lc[cat]);
    g_lc[cat] = v;
    return v;
}

static char g_lc_all_buf[512];
API char *ms_setlocale(int cat, const char *name)
{
    if (cat < 0 || cat > MS_LC_MAX) { crt_set_errno(MS_EINVAL); return NULL; }
    pthread_mutex_lock(&g_lc_lock);
    for (int i = 0; i <= MS_LC_MAX; i++) if (!g_lc[i]) g_lc[i] = strdup("C");
    if (name) {
        if (cat == MS_LC_ALL) for (int i = 1; i <= MS_LC_MAX; i++) set_category(i, name);
        else set_category(cat, name);
    }
    const char *r;
    if (cat == MS_LC_ALL) {
        int same = 1;
        for (int i = 2; i <= MS_LC_MAX; i++) if (strcmp(g_lc[i], g_lc[1])) same = 0;
        if (same) snprintf(g_lc_all_buf, sizeof g_lc_all_buf, "%s", g_lc[1]);
        else snprintf(g_lc_all_buf, sizeof g_lc_all_buf, "LC_COLLATE=%s;LC_CTYPE=%s;LC_MONETARY=%s;LC_NUMERIC=%s;LC_TIME=%s",
                      g_lc[1], g_lc[2], g_lc[3], g_lc[4], g_lc[5]);
        r = g_lc_all_buf;
    } else r = g_lc[cat];
    pthread_mutex_unlock(&g_lc_lock);
    return (char *)r;
}
API WCHAR *_wsetlocale(int cat, const WCHAR *name)
{
    static __thread WCHAR buf[512];
    char *u = name ? lsiw_w2u(name) : NULL;
    char *r = ms_setlocale(cat, u);
    free(u);
    if (!r) return NULL;
    lsiw_u2w_buf(r, buf, 512);
    return buf;
}

struct ms_lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
         *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    WCHAR *w_decimal_point, *w_thousands_sep, *w_int_curr_symbol, *w_currency_symbol, *w_mon_decimal_point,
          *w_mon_thousands_sep, *w_positive_sign, *w_negative_sign;
};
static WCHAR g_wdot[] = { '.', 0 }, g_wempty[] = { 0 };
static struct ms_lconv g_lconv = {
    ".", "", "", "", "", "", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127,
    g_wdot, g_wempty, g_wempty, g_wempty, g_wempty, g_wempty, g_wempty, g_wempty
};
API struct ms_lconv *ms_localeconv(void) { return &g_lconv; }

/* _locale_t: { locinfo, mbcinfo }; locinfo begins with the ucrt fields
 * (pctype pointer, mb_cur_max, code page). */
struct ms_locinfo { const unsigned short *pctype; int mb_cur_max; unsigned codepage; char pad[1024]; };
struct ms_mbcinfo { int refcount; int mbcodepage; char pad[1024]; };
struct ms_locale { struct ms_locinfo *locinfo; struct ms_mbcinfo *mbcinfo; };
static struct ms_locinfo g_locinfo;
static struct ms_mbcinfo g_mbcinfo = { 1, 65001, { 0 } };
static struct ms_locale g_locale = { &g_locinfo, &g_mbcinfo };
extern const unsigned short *crt_pctype;

static struct ms_locale *current_locale(void)
{
    g_locinfo.pctype = crt_pctype;
    g_locinfo.mb_cur_max = crt_mb_cur_max;
    g_locinfo.codepage = (unsigned)crt_codepage;
    return &g_locale;
}
API struct ms_locale *_create_locale(int cat, const char *name) { (void)cat; (void)name; return current_locale(); }
API struct ms_locale *_wcreate_locale(int cat, const WCHAR *name) { (void)cat; (void)name; return current_locale(); }
API struct ms_locale *_get_current_locale(void) { return current_locale(); }
API void _free_locale(struct ms_locale *l) { (void)l; }
API unsigned ___lc_codepage_func(void) { return (unsigned)crt_codepage; }
API unsigned ___lc_collate_cp_func(void) { return (unsigned)crt_codepage; }
static unsigned g_lc_handle[6];
API unsigned *___lc_handle_func(void) { return g_lc_handle; }
API WCHAR **___lc_locale_name_func(void) { static WCHAR name[] = { 'C', 0 }; static WCHAR *names[6] = { name, name, name, name, name, name }; return names; }
API int ___mb_cur_max_func(void) { return crt_mb_cur_max; }
API int ___mb_cur_max_l_func(void *l) { (void)l; return crt_mb_cur_max; }
API int *__p___mb_cur_max(void) { return &crt_mb_cur_max; }
API int _getmbcp(void) { return crt_codepage; }
API int _setmbcp(int cp) { (void)cp; return 0; }
API int __lconv_init(void) { return 0; }

const struct lsiw_export lsiw_crt_time_exports[] = {
    E(_tzset), EX("tzset", _tzset), E(_get_timezone), E(_get_daylight), E(_get_dstbias), E(_get_tzname),
    E(__p__timezone), E(__p__daylight), E(__p__dstbias), E(__p__tzname),
    EX("__timezone", __p__timezone), EX("__daylight", __p__daylight), EX("__dstbias", __p__dstbias), EX("__tzname", __p__tzname),
    E(_gmtime64), E(_gmtime32), E(_gmtime64_s), E(_gmtime32_s), EX("gmtime", _gmtime64), EX("gmtime_s", _gmtime64_s),
    E(_localtime64), E(_localtime32), E(_localtime64_s), E(_localtime32_s), EX("localtime", _localtime64), EX("localtime_s", _localtime64_s),
    E(_mktime64), E(_mktime32), E(_mkgmtime64), E(_mkgmtime32), EX("mktime", _mktime64), EX("_mkgmtime", _mkgmtime64),
    E(_time64), E(_time32), EX("time", _time64), E(_difftime64), E(_difftime32), EX("difftime", _difftime64),
    EM(clock), E(_ftime64), E(_ftime32), E(_ftime64_s), E(_ftime32_s), EX("_ftime", _ftime64), EX("_ftime_s", _ftime64_s), EX("ftime", _ftime64),
    E(_timespec64_get), E(_timespec32_get), EX("timespec_get", _timespec64_get),
    EM(asctime), E(asctime_s), E(_wasctime), E(_wasctime_s),
    E(_ctime64), E(_ctime32), E(_ctime64_s), E(_ctime32_s), EX("ctime", _ctime64), EX("ctime_s", _ctime64_s),
    E(_wctime64), E(_wctime32), E(_wctime64_s), E(_wctime32_s), EX("_wctime", _wctime64), EX("_wctime_s", _wctime64_s),
    E(_strdate), E(_strtime), E(_strdate_s), E(_strtime_s), E(_wstrdate), E(_wstrtime), E(_wstrdate_s), E(_wstrtime_s),
    EM(strftime), E(_strftime_l), E(_Strftime), EM(wcsftime), E(_wcsftime_l),
    EM(setlocale), E(_wsetlocale), EM(localeconv), E(_create_locale), E(_wcreate_locale), E(_get_current_locale), E(_free_locale),
    E(___lc_codepage_func), E(___lc_collate_cp_func), E(___lc_handle_func), E(___lc_locale_name_func),
    E(___mb_cur_max_func), E(___mb_cur_max_l_func), E(__p___mb_cur_max), E(_getmbcp), E(_setmbcp), E(__lconv_init),
    /* data */
    EX("_timezone", &crt_timezone), EX("_daylight", &crt_daylight), EX("_dstbias", &crt_dstbias), EX("_tzname", crt_tzname),
    EX("__mb_cur_max", &crt_mb_cur_max),
    { NULL, NULL }
};
