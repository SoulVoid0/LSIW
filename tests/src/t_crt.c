/* t_crt: the C runtime (msvcrt.dll, or ucrtbase / api-ms-win-crt-* when
 * built with -D_UCRT as t_crt_ucrt.exe).
 *
 * Everything is checked against the documented Microsoft behaviour, which
 * differs from glibc in places: three-digit exponents in msvcrt's %e/%g,
 * %S meaning "the other width" string, %p printed as 16 upper-case hex
 * digits, 32-bit long, _snprintf not terminating on truncation, the MS rand()
 * sequence.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
/* Use the DLL's printf family, not MinGW's own replacement. */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"
#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <limits.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

/* strncpy without a terminator is exactly what is being tested. */
#pragma GCC diagnostic ignored "-Wstringop-truncation"

#ifdef _UCRT
#define CRT_NAME "ucrt"
#else
#define CRT_NAME "msvcrt"
#endif

static char g_buf[512];

/* sprintf into g_buf through the CRT's vsprintf. */
static const char *fmt(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsprintf(g_buf, f, ap);
    va_end(ap);
    return g_buf;
}

#define CHECK_FMT(expect, ...)                                              \
    do {                                                                    \
        const char *got_ = fmt(__VA_ARGS__);                                \
        CHECKF(strcmp(got_, expect) == 0, "printf " #__VA_ARGS__,           \
               "got \"%s\", expected \"%s\"", got_, expect);                \
    } while (0)

static void test_printf(void)
{
    CHECK_FMT("-1234567890123", "%I64d", (__int64)-1234567890123LL);
    CHECK_FMT("18446744073709551615", "%I64u", (unsigned __int64)UINT64_MAX);
    CHECK_FMT("18446744073709551615", "%llu", (unsigned long long)UINT64_MAX);
    CHECK_FMT("-9223372036854775808", "%lld", (long long)INT64_MIN);
    CHECK_FMT("123456789012", "%Iu", (size_t)123456789012ULL);
#ifdef _UCRT
    /* C99 size_t length modifier: ucrt only (msvcrt.dll does not know 'z'). */
    CHECK_FMT("123456789012", "%zu", (size_t)123456789012ULL);
    CHECK_FMT("-5", "%zd", (ptrdiff_t)-5);
    CHECK_FMT("1.234568e+04", "%e", 12345.678);
    CHECK_FMT("1e-05", "%g", 0.00001);
    CHECK_FMT("1.5E+300", "%.1E", 1.5e300);
#else
    CHECK_FMT("1.234568e+004", "%e", 12345.678);
    CHECK_FMT("1e-005", "%g", 0.00001);
    CHECK_FMT("1.5E+300", "%.1E", 1.5e300);
#endif
    CHECK_FMT("wide string", "%ls", L"wide string");
    CHECK_FMT("wide S", "%S", L"wide S");
    CHECK_FMT("narrow hs", "%hs", "narrow hs");
    CHECK_FMT(" 3.14", "%5.2f", 3.14159);
    CHECK_FMT("3.14 |-2.50", "%-5.2f|%.2f", 3.14159, -2.5);
    CHECK_FMT("0.0001", "%g", 0.0001);
    CHECK_FMT("123457", "%g", 123456.7);
    CHECK_FMT("100", "%g", 100.0);
    CHECK_FMT("ff FF 0xff 377", "%x %X %#x %o", 255, 255, 255, 255);
    CHECK_FMT("0000000000001234", "%p", (void *)(uintptr_t)0x1234);
    CHECK_FMT("00042|42   |  +42|-42", "%05d|%-5d|%+5d|%d", 42, 42, 42, -42);
    CHECK_FMT("abc|  x|100%", "%.3s|%3c|%d%%", "abcdef", 'x', 100);
    CHECK_FMT("4294967295", "%u", -1);
    CHECK_FMT("(null)", "%s", (char *)NULL);
    CHECK_FMT("  7|7  |007", "%*d|%-*d|%.*d", 3, 7, 3, 7, 3, 7);
    CHECK_FMT("1.000000", "%f", 1.0);
    CHECK_FMT("0.333", "%.3f", 1.0 / 3);
    CHECK_FMT("2.5", "%.1f", 2.45 + 0.01);
    CHECK_FMT("-0", "%.0f", -0.2);
}

/* msvcrt prints at least three exponent digits, ucrt at least two. */
static void test_printf_exp_digits(void)
{
#ifdef _UCRT
    CHECK_FMT("1e+10", "%.0e", 1e10);
#else
    CHECK_FMT("1e+010", "%.0e", 1e10);
#endif
}

static void test_snprintf(void)
{
    char b[16];
    int r;

    memset(b, 'X', sizeof b);
    r = _snprintf(b, 5, "%s", "hello world");
    CHECKF(r == -1, "_snprintf returns -1 on truncation", "r=%d", r);
    CHECK(memcmp(b, "helloX", 6) == 0, "_snprintf truncation leaves no terminator");

    memset(b, 'X', sizeof b);
    r = _snprintf(b, 11, "%s", "hello world");
    CHECKF(r == 11, "_snprintf exact fit returns length", "r=%d", r);
    CHECK(b[11] == 'X', "_snprintf exact fit: no terminator written");

    memset(b, 'X', sizeof b);
    r = _snprintf(b, sizeof b, "%d-%d", 12, 34);
    CHECK(r == 5 && strcmp(b, "12-34") == 0, "_snprintf fits");

    r = _scprintf("%d %s", 12345, "abc");
    CHECKF(r == 9, "_scprintf counts", "r=%d", r);

    memset(b, 'X', sizeof b);
    r = sprintf(b, "%s", "");
    CHECK(r == 0 && b[0] == 0, "sprintf empty");

    wchar_t w[8];
    r = _snwprintf(w, 3, L"%s", L"abcdef");
    CHECKF(r == -1 && w[0] == L'a' && w[2] == L'c', "_snwprintf truncation", "r=%d", r);
}

static void test_sscanf(void)
{
    int i = 0, x = 0;
    double d = 0;
    char s[32] = "";
    int n = sscanf("42 3.5 hello 0x1f", "%d %lf %31s %x", &i, &d, s, &x);
    CHECKF(n == 4 && i == 42 && d == 3.5 && strcmp(s, "hello") == 0 && x == 0x1f,
           "sscanf basic", "n=%d i=%d d=%g s=%s x=%x", n, i, d, s, x);

    __int64 big = 0;
    const char *i64fmt = "%I64d";   /* not a literal: GCC's ucrt-mode checker rejects I64 */
    n = sscanf("-9000000000", i64fmt, &big);
    CHECK(n == 1 && big == -9000000000LL, "sscanf %I64d");

    char a[8] = "", b[8] = "";
    n = sscanf("key=value", "%7[^=]=%7s", a, b);
    CHECK(n == 2 && !strcmp(a, "key") && !strcmp(b, "value"), "sscanf scanset");

    n = sscanf("", "%d", &i);
    CHECKF(n == EOF, "sscanf empty input returns EOF", "n=%d", n);

    unsigned u = 0;
    n = sscanf("abc 17", "%*s %u", &u);
    CHECK(n == 1 && u == 17, "sscanf assignment suppression");

    wchar_t ws[16];
    n = swscanf(L"12 wide", L"%d %15ls", &i, ws);
    CHECK(n == 2 && i == 12 && wcscmp(ws, L"wide") == 0, "swscanf");
}

static void test_strto(void)
{
    char *end;
    errno = 0;
    long l = strtol("  -123abc", &end, 10);
    CHECK(l == -123 && strcmp(end, "abc") == 0, "strtol with trailing text");
    CHECK(strtol("0x7f", NULL, 0) == 127, "strtol base 0 hex");
    CHECK(strtol("0755", NULL, 0) == 493, "strtol base 0 octal");
    CHECK(strtol("z", NULL, 36) == 35, "strtol base 36");
    CHECK(sizeof(long) == 4, "long is 32-bit on Windows");

    errno = 0;
    l = strtol("99999999999", &end, 10);
    CHECKF(l == LONG_MAX && errno == ERANGE, "strtol overflow -> LONG_MAX, ERANGE",
           "l=%ld errno=%d", l, errno);
    CHECK(ERANGE == 34, "ERANGE is 34");

    errno = 0;
    unsigned long ul = strtoul("4294967295", &end, 10);
    CHECK(ul == 4294967295UL && *end == 0 && errno == 0, "strtoul max");

    double d = strtod("1.5e3xyz", &end);
    CHECK(d == 1500.0 && strcmp(end, "xyz") == 0, "strtod");
    d = strtod("-0.125", NULL);
    CHECK(d == -0.125, "strtod negative fraction");

    __int64 v = _strtoi64("-9223372036854775807", &end, 10);
    CHECK(v == -9223372036854775807LL && *end == 0, "_strtoi64");
    unsigned __int64 uv = _strtoui64("ffffffffffffffff", NULL, 16);
    CHECK(uv == UINT64_MAX, "_strtoui64 hex");

    CHECK(atoi("  77xyz") == 77, "atoi");
    CHECK(atol("-5") == -5, "atol");
    CHECK(atof("2.5") == 2.5, "atof");
    CHECK(_atoi64("123456789012") == 123456789012LL, "_atoi64");

    CHECK(wcstol(L"-42", NULL, 10) == -42, "wcstol");
    CHECK(_wtoi(L"314") == 314, "_wtoi");
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void test_sort(void)
{
    int v[] = { 9, -3, 7, 0, 42, 7, 1, -100, 5, 3 };
    const int sorted[] = { -100, -3, 0, 1, 3, 5, 7, 7, 9, 42 };
    qsort(v, 10, sizeof v[0], cmp_int);
    CHECK(memcmp(v, sorted, sizeof v) == 0, "qsort");
    int key = 9;
    int *p = bsearch(&key, v, 10, sizeof v[0], cmp_int);
    CHECK(p && p - v == 8, "bsearch found");
    key = 8;
    CHECK(bsearch(&key, v, 10, sizeof v[0], cmp_int) == NULL, "bsearch missing");
}

static void test_string(void)
{
    char b[64];
    CHECK(strlen("hello") == 5, "strlen");
    CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("x", "x") == 0, "strcmp");
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp");
    CHECK(_stricmp("HeLLo", "hello") == 0 && _strnicmp("ABCx", "abcy", 3) == 0, "_stricmp/_strnicmp");
    strcpy(b, "foo");
    strcat(b, "bar");
    CHECK(strcmp(b, "foobar") == 0, "strcpy/strcat");
    CHECK(strchr(b, 'o') == b + 1 && strrchr(b, 'o') == b + 2, "strchr/strrchr");
    CHECK(strstr(b, "bar") == b + 3 && strstr(b, "baz") == NULL, "strstr");
    CHECK(strspn("aabbc", "ab") == 4 && strcspn("hello, world", ",") == 5, "strspn/strcspn");
    memset(b, 0, sizeof b);
    strncpy(b, "abcdef", 3);
    CHECK(strcmp(b, "abc") == 0, "strncpy");
    strcpy(b, "123456789");
    memmove(b + 2, b, 5);
    CHECK(strcmp(b, "121234589") == 0, "memmove overlapping");
    CHECK(memcmp("abc", "abd", 3) < 0, "memcmp");
    char *dup = _strdup("duplicate");
    CHECK(dup && strcmp(dup, "duplicate") == 0, "_strdup");
    free(dup);
    strcpy(b, "MiXeD");
    CHECK(strcmp(_strlwr(b), "mixed") == 0, "_strlwr");
    CHECK(strcmp(_strupr(b), "MIXED") == 0, "_strupr");
    strcpy(b, "a,b,,c");
    char *t1 = strtok(b, ","), *t2 = strtok(NULL, ","), *t3 = strtok(NULL, ","), *t4 = strtok(NULL, ",");
    CHECK(t1 && t2 && t3 && !t4 && !strcmp(t1, "a") && !strcmp(t2, "b") && !strcmp(t3, "c"), "strtok");
    CHECK(memchr("abcdef", 'd', 6) != NULL && memchr("abc", 'z', 3) == NULL, "memchr");
    CHECK(toupper('a') == 'A' && tolower('Q') == 'q' && isdigit('5') && !isalpha('5') && isspace('\t'), "ctype");
    strcpy(b, "abc");
    CHECK(strcmp(_strrev(b), "cba") == 0, "_strrev");
}

static void test_wide(void)
{
    wchar_t w[64];
    CHECK(sizeof(wchar_t) == 2, "wchar_t is 16-bit");
    CHECK(wcslen(L"hello") == 5, "wcslen");
    CHECK(wcscmp(L"abc", L"abc") == 0 && wcscmp(L"abc", L"abd") < 0, "wcscmp");
    CHECK(_wcsicmp(L"ABC", L"abc") == 0 && _wcsicmp(L"ABC", L"abd") < 0, "_wcsicmp");
    CHECK(_wcsnicmp(L"HELLOx", L"helloy", 5) == 0, "_wcsnicmp");
    wcscpy(w, L"foo");
    wcscat(w, L"bar");
    CHECK(wcscmp(w, L"foobar") == 0, "wcscpy/wcscat");
    CHECK(wcschr(w, L'b') == w + 3 && wcsrchr(w, L'o') == w + 2, "wcschr/wcsrchr");
    CHECK(wcsstr(w, L"oba") == w + 2, "wcsstr");
    int r = swprintf(w, 64, L"%d-%s-%hs-%ls", 7, L"wide", "narrow", L"ls");
    CHECKF(r == 16 && wcscmp(w, L"7-wide-narrow-ls") == 0, "swprintf %s is wide in wide printf",
           "r=%d", r);
    r = _snwprintf(w, 64, L"%S|%c|%x", "narrowS", L'Z', 0xbeef);
    CHECK(r > 0 && wcscmp(w, L"narrowS|Z|beef") == 0, "_snwprintf %S is narrow");
    wchar_t *d = _wcsdup(L"dupw");
    CHECK(d && wcscmp(d, L"dupw") == 0, "_wcsdup");
    free(d);
    wcscpy(w, L"MiXeD");
    CHECK(wcscmp(_wcslwr(w), L"mixed") == 0 && wcscmp(_wcsupr(w), L"MIXED") == 0, "_wcslwr/_wcsupr");
    CHECK(towupper(L'a') == L'A' && iswdigit(L'3'), "wide ctype");

    char mb[32];
    size_t n = wcstombs(mb, L"plain", sizeof mb);
    CHECK(n == 5 && strcmp(mb, "plain") == 0, "wcstombs");
    n = mbstowcs(w, "ascii", 64);
    CHECK(n == 5 && wcscmp(w, L"ascii") == 0, "mbstowcs");

    /* UTF-8 <-> UTF-16 through kernel32, as programs do for file names. */
    const char *utf8 = "h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80";
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 64);
    CHECKF(wn == 11 && w[1] == 0xe9 && w[6] == 0x20ac && w[8] == 0xd83d && w[9] == 0xde00,
           "MultiByteToWideChar UTF-8 incl. surrogate pair", "wn=%d", wn);
    int bn = WideCharToMultiByte(CP_UTF8, 0, w, -1, mb, sizeof mb, NULL, NULL);
    CHECK(bn == (int)strlen(utf8) + 1 && strcmp(mb, utf8) == 0, "WideCharToMultiByte UTF-8 round trip");
}

static void test_itoa(void)
{
    char b[72];
    CHECK(strcmp(_itoa(255, b, 16), "ff") == 0, "_itoa hex");
    CHECK(strcmp(_itoa(-5, b, 10), "-5") == 0, "_itoa negative");
    CHECK(strcmp(_itoa(-1, b, 16), "ffffffff") == 0, "_itoa negative non-decimal is unsigned");
    CHECK(strcmp(_itoa(5, b, 2), "101") == 0, "_itoa binary");
    CHECK(strcmp(_ultoa(4294967295UL, b, 10), "4294967295") == 0, "_ultoa");
    CHECK(strcmp(_ltoa(-123456L, b, 10), "-123456") == 0, "_ltoa");
    CHECK(strcmp(_i64toa(-9000000000LL, b, 10), "-9000000000") == 0, "_i64toa");
    CHECK(strcmp(_ui64toa(0xFFFFFFFFFFFFFFFFULL, b, 16), "ffffffffffffffff") == 0, "_ui64toa");
    wchar_t w[40];
    CHECK(wcscmp(_itow(1234, w, 10), L"1234") == 0, "_itow");
}

static void test_time(void)
{
    time_t now = time(NULL);
    CHECKF(now > 1600000000, "time() is after 2020", "now=%lld", (long long)now);
    CHECK(sizeof(time_t) == 8, "time_t is 64-bit");

    time_t zero = 0;
    struct tm *g = gmtime(&zero);
    CHECK(g && g->tm_year == 70 && g->tm_mon == 0 && g->tm_mday == 1 && g->tm_hour == 0 &&
          g->tm_wday == 4, "gmtime(0) is Thursday 1970-01-01");

    struct tm tm = { 0 };
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 1;
    tm.tm_mday = 29;
    tm.tm_hour = 12;
    tm.tm_min = 34;
    tm.tm_sec = 56;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    CHECK(t != (time_t)-1, "mktime");
    CHECK(tm.tm_wday == 4 && tm.tm_yday == 59, "mktime fills wday/yday (2024-02-29 is a Thursday)");
    struct tm *l = localtime(&t);
    CHECK(l && l->tm_year == 124 && l->tm_mon == 1 && l->tm_mday == 29 && l->tm_hour == 12 &&
          l->tm_min == 34 && l->tm_sec == 56, "localtime(mktime(x)) round trip");
    char b[64];
    size_t n = strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", l);
    CHECKF(n == 19 && strcmp(b, "2024-02-29 12:34:56") == 0, "strftime", "got \"%s\"", b);
    n = strftime(b, sizeof b, "%a %b %j %y %%", l);
    CHECKF(strcmp(b, "Thu Feb 060 24 %") == 0, "strftime names", "got \"%s\"", b);

    /* Normalisation: Jan 32nd is Feb 1st. */
    struct tm tn = { 0 };
    tn.tm_year = 123; tn.tm_mon = 0; tn.tm_mday = 32; tn.tm_hour = 10; tn.tm_isdst = -1;
    CHECK(mktime(&tn) != (time_t)-1 && tn.tm_mon == 1 && tn.tm_mday == 1, "mktime normalises");

    struct tm tg = { 0 };
    tg.tm_year = 100; tg.tm_mon = 0; tg.tm_mday = 1;
    CHECK(_mkgmtime(&tg) == 946684800, "_mkgmtime 2000-01-01");

    clock_t c = clock();
    CHECK(c >= 0, "clock()");
}

static void test_locale(void)
{
    const char *loc = setlocale(LC_ALL, "");
    CHECK(loc != NULL, "setlocale(LC_ALL, \"\") returns non-NULL");
    loc = setlocale(LC_ALL, "C");
    CHECK(loc && strcmp(loc, "C") == 0, "setlocale back to C");
    CHECK(strcmp(fmt("%.1f", 1.5), "1.5") == 0, "C locale decimal point");
    struct lconv *lc = localeconv();
    CHECK(lc && lc->decimal_point && lc->decimal_point[0] == '.', "localeconv decimal point");
}

static int approx(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_math(void)
{
    volatile double two = 2.0, half = 0.5;
    CHECK(approx(sqrt(two), 1.4142135623730951), "sqrt");
    CHECK(pow(two, 10) == 1024.0 && approx(pow(two, half), 1.4142135623730951), "pow");
    CHECK(sin(0.0) == 0.0 && approx(sin(3.14159265358979323846 / 2), 1.0), "sin");
    CHECK(approx(cos(0.0), 1.0) && approx(tan(0.0), 0.0), "cos/tan");
    CHECK(floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 && floor(2.5) == 2.0, "floor/ceil");
    CHECK(fmod(7.5, two) == 1.5 && fmod(-7.5, two) == -1.5, "fmod");
    CHECK(lround(2.5) == 3 && lround(-2.5) == -3 && lround(2.4) == 2, "lround");
    CHECK(approx(exp(1.0), 2.718281828459045) && approx(log(exp(two)), 2.0), "exp/log");
    CHECK(approx(log10(1000.0), 3.0), "log10");
    CHECK(approx(atan2(1.0, 1.0), 3.14159265358979323846 / 4), "atan2");
    CHECK(fabs(-3.5) == 3.5 && abs(-7) == 7 && labs(-8L) == 8, "fabs/abs/labs");
    CHECK(_isnan(sqrt(-two)) && !_finite(1.0 / (two - 2.0)), "_isnan/_finite");
    double ip;
    CHECK(modf(3.75, &ip) == 0.75 && ip == 3.0, "modf");
    int e;
    CHECK(frexp(8.0, &e) == 0.5 && e == 4 && ldexp(0.5, 4) == 8.0, "frexp/ldexp");
    div_t dv = div(17, 5);
    CHECK(dv.quot == 3 && dv.rem == 2, "div");
}

static void test_alloc(void)
{
    char *p = malloc(100);
    CHECK(p != NULL, "malloc");
    memset(p, 'a', 100);
    p = realloc(p, 100000);
    CHECK(p && p[0] == 'a' && p[99] == 'a', "realloc keeps contents");
    free(p);
    int *z = calloc(1000, sizeof(int));
    int allzero = 1;
    for (int i = 0; z && i < 1000; i++) if (z[i]) allzero = 0;
    CHECK(z && allzero, "calloc zeroes");
    CHECK(_msize(z) >= 1000 * sizeof(int), "_msize");
    free(z);
    void *a = _aligned_malloc(100, 64);
    CHECK(a && ((uintptr_t)a & 63) == 0, "_aligned_malloc alignment 64");
    _aligned_free(a);
    a = _aligned_malloc(10, 4096);
    CHECK(a && ((uintptr_t)a & 4095) == 0, "_aligned_malloc alignment 4096");
    _aligned_free(a);
    free(NULL);
    CHECK(1, "free(NULL)");
    p = realloc(NULL, 10);
    CHECK(p != NULL, "realloc(NULL, n) allocates");
    free(p);
}

static void test_env(void)
{
    CHECK(_putenv("LSIW_T_VAR=abc def") == 0, "_putenv");
    const char *v = getenv("LSIW_T_VAR");
    CHECK(v && strcmp(v, "abc def") == 0, "getenv after _putenv");
    char b[64];
    DWORD n = GetEnvironmentVariableA("LSIW_T_VAR", b, sizeof b);
    CHECK(n == 7 && strcmp(b, "abc def") == 0, "_putenv also sets the Win32 environment");
    CHECK(_putenv("LSIW_T_VAR=") == 0 && getenv("LSIW_T_VAR") == NULL, "_putenv NAME= removes");
    CHECK(getenv("LSIW_SURELY_UNSET_VARIABLE") == NULL, "getenv unset is NULL");
    CHECK(getenv("PATH") != NULL || getenv("Path") != NULL, "PATH is set");
    CHECK(getenv("path") != NULL, "getenv is case-insensitive");
}

static void test_errno(void)
{
    errno = 0;
    FILE *f = fopen("lsiw_no_such_file.txt", "r");
    CHECKF(f == NULL && errno == ENOENT, "fopen missing file -> ENOENT", "f=%p errno=%d", (void *)f, errno);
    CHECK(ENOENT == 2 && EACCES == 13 && EEXIST == 17 && EINVAL == 22, "errno values match MS");
    errno = 0;
    f = fopen("lsiw_no_such_dir\\x.txt", "w");
    CHECKF(f == NULL && errno == ENOENT, "fopen in missing dir -> ENOENT", "errno=%d", errno);
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0, "strerror(ENOENT)");
    _set_errno(EDOM);
    int e = 0;
    _get_errno(&e);
    CHECK(e == EDOM && errno == EDOM, "_set_errno/_get_errno");
}

static void test_rand(void)
{
    /* The Microsoft LCG: state = state * 214013 + 2531011, output bits 16..30. */
    static const int ms_seq[] = { 41, 18467, 6334, 26500, 19169 };
    int a[5], b[5];
    srand(1);
    for (int i = 0; i < 5; i++) a[i] = rand();
    srand(1);
    for (int i = 0; i < 5; i++) b[i] = rand();
    CHECK(memcmp(a, b, sizeof a) == 0, "srand/rand deterministic");
    CHECKF(memcmp(a, ms_seq, sizeof a) == 0, "rand matches the Microsoft sequence",
           "got %d %d %d %d %d", a[0], a[1], a[2], a[3], a[4]);
    CHECK(RAND_MAX == 32767, "RAND_MAX");
}

static void test_stdio_misc(void)
{
    char b[32];
    FILE *f = tmpfile();
    CHECK(f != NULL, "tmpfile");
    if (f) {
        fputs("line1\nline2\n", f);
        rewind(f);
        CHECK(fgets(b, sizeof b, f) && strcmp(b, "line1\n") == 0, "tmpfile read back");
        fclose(f);
    }
    CHECK(_fileno(stdout) == 1 && _fileno(stdin) == 0 && _fileno(stderr) == 2, "_fileno std streams");
    CHECK(_isatty(-1) == 0, "_isatty(-1)");
}

/* atexit: handlers run in reverse order of registration, after main
 * returns. The last one to run (registered first) checks the order and
 * prints the result line. */
static char g_order[8];
static void at1(void) { strcat(g_order, "1"); }
static void at2(void) { strcat(g_order, "2"); }
static void at3(void) { strcat(g_order, "3"); }
static void at_final(void)
{
    CHECKF(strcmp(g_order, "321") == 0, "atexit handlers run in reverse order", "order=\"%s\"", g_order);
    int rc = t_done();
    fflush(stdout);
    if (rc) _exit(rc);
}

int main(void)
{
    t_init("t_crt (" CRT_NAME ")");
    test_printf();
    test_printf_exp_digits();
    test_snprintf();
    test_sscanf();
    test_strto();
    test_sort();
    test_string();
    test_wide();
    test_itoa();
    test_time();
    test_locale();
    test_math();
    test_alloc();
    test_env();
    test_errno();
    test_rand();
    test_stdio_misc();

    atexit(at_final);
    atexit(at1);
    atexit(at2);
    atexit(at3);
    printf("# main returns; atexit handlers follow\n");
    return t_failures ? 1 : 0;
}
