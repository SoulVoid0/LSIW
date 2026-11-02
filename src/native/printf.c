/* printf and scanf engines that read arguments from a Microsoft x64 va_list.
 *
 * glibc's vprintf expects a System V va_list, which has a different layout,
 * so msvcrt's printf family cannot simply forward to it. These walk the
 * MS-ABI argument area themselves and hand single, already-extracted values
 * to the host snprintf/strtod for the final formatting of each field.
 *
 * Windows is LLP64: "long" is 32-bit, so %ld reads 32 bits; %lld / %I64d /
 * %zd / %Id read 64 bits. For the wide printf (lsiw_ms_vsnwprintf) the format
 * string is UTF-16 and %s defaults to wide, %hs to narrow; the reverse of the
 * narrow printf.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "native.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ================================================================== */
/* printf                                                               */
/* ================================================================== */

struct outbuf { char *buf; size_t cap; size_t len; };

static void put(struct outbuf *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (o->cap && o->len + 1 < o->cap) o->buf[o->len] = s[i];
        o->len++;
    }
}

/* UTF-16LE -> UTF-8 for %ls / %S. */
static void put_wide(struct outbuf *o, const WCHAR *w, int prec)
{
    if (!w) { put(o, "(null)", 6); return; }
    int count = 0;
    for (; *w && (prec < 0 || count < prec); w++, count++) {
        uint32_t c = *w;
        if (c >= 0xD800 && c <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[1] - 0xDC00); w++;
        }
        char b[4]; size_t n;
        if (c < 0x80) { b[0] = (char)c; n = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                                b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
        else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
               b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
        put(o, b, n);
    }
}

static void pad_put(struct outbuf *o, const char *s, size_t sl, int width, const char *flags)
{
    int left = strchr(flags, '-') != NULL;
    int pad = width > (int)sl ? width - (int)sl : 0;
    if (!left) for (int i = 0; i < pad; i++) put(o, " ", 1);
    put(o, s, sl);
    if (left) for (int i = 0; i < pad; i++) put(o, " ", 1);
}

/* msvcrt.dll prints at least three exponent digits ("1.5e+004"); the
 * Universal CRT and C99 print two. lsiw_printf_three_digit_exp selects the default,
 * and the ucrt __stdio_common_* entry points override it per call. */
int lsiw_printf_three_digit_exp = 1;
__thread int lsiw_printf_ucrt_call;
static int lsiw_printf_exp3(void) { return lsiw_printf_three_digit_exp && !lsiw_printf_ucrt_call; }

/* Widen a two-digit exponent in tmp[0..w) to three digits, giving back one
 * padding character when the field was padded to its width. */
static int exp3_fix(char *tmp, int w, int width, const char *flags)
{
    int e = -1;
    for (int i = 0; i + 3 < w + 1 && i < w; i++)
        if ((tmp[i] == 'e' || tmp[i] == 'E') && (tmp[i + 1] == '+' || tmp[i + 1] == '-') &&
            i + 3 < w + 1 && isdigit((unsigned char)tmp[i + 2]) && isdigit((unsigned char)tmp[i + 3]) &&
            (i + 4 == w || !isdigit((unsigned char)tmp[i + 4]))) { e = i; break; }
    if (e < 0 || w + 2 > 510) return w;
    memmove(tmp + e + 3, tmp + e + 2, (size_t)(w - e - 2) + 1);
    tmp[e + 2] = '0';
    w++;
    if (w - 1 == width && width > 0) {     /* field was padded: drop one pad char */
        if (tmp[0] == ' ' && (tmp[1] == ' ' || !strchr(flags, ' '))) { memmove(tmp, tmp + 1, (size_t)w); w--; }
        else if (tmp[w - 1] == ' ') { tmp[--w] = 0; }
        else {
            int z = (tmp[0] == '-' || tmp[0] == '+' || tmp[0] == ' ') ? 1 : 0;
            if (tmp[z] == '0' && isdigit((unsigned char)tmp[z + 1])) { memmove(tmp + z, tmp + z + 1, (size_t)(w - z)); w--; }
        }
    }
    return w;
}

/* Core narrow-output formatter. fmt_wide tells how a bare %s/%c is sized:
 * 0 = narrow default (the C runtime's printf), 1 = wide default (wprintf). */
static int format_core(struct outbuf *o, const char *fmt, ms_va_list ap, int fmt_wide)
{
    char spec[80], tmp[512];

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(o, p, 1); continue; }
        const char *start = p++;
        if (*p == '%') { put(o, "%", 1); continue; }

        char flags[8]; int nf = 0;
        while (*p && strchr("-+ #0", *p) && nf < 7) flags[nf++] = *p++;
        flags[nf] = 0;
        int width = -1;
        if (*p == '*') { width = ms_va_arg(ap, int); p++; if (width < 0) { flags[nf++] = '-'; flags[nf] = 0; width = -width; } }
        else if (*p >= '0' && *p <= '9') { width = 0; while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0'); }
        int prec = -1;
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') { prec = ms_va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        /* length: 0=int, 1=long(32), 2=64-bit, 3=short, 4=char; wide: -1 narrow, 0 default, 1 wide */
        int len = 0, wide = 0;
        for (;;) {
            if (*p == 'h') { len = (p[1] == 'h') ? (p++, 4) : 3; p++; wide = -1; }
            else if (*p == 'l') { if (p[1] == 'l') { len = 2; p += 2; } else { len = 1; wide = 1; p++; } }
            else if (*p == 'w') { wide = 1; p++; }
            else if (*p == 'L') { p++; }
            else if (*p == 'z' || *p == 'j' || *p == 't') { len = 2; p++; }
            else if (*p == 'I') {
                if (p[1] == '6' && p[2] == '4') { len = 2; p += 3; }
                else if (p[1] == '3' && p[2] == '2') { len = 0; p += 3; }
                else { len = 2; p++; }
            }
            else break;
        }
        char conv = *p;
        if (!conv) break;

        int sl = snprintf(spec, sizeof spec, "%%%s", flags);
        if (width >= 0) sl += snprintf(spec + sl, sizeof spec - sl, "%d", width);
        if (prec >= 0)  sl += snprintf(spec + sl, sizeof spec - sl, ".%d", prec);

        int w = 0;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (len == 2) v = ms_va_arg(ap, long long);
            else { int x = ms_va_arg(ap, int); v = len == 3 ? (short)x : len == 4 ? (signed char)x : x; }
            snprintf(spec + sl, sizeof spec - sl, "lld");
            w = snprintf(tmp, sizeof tmp, spec, v);
            break; }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v;
            if (len == 2) v = ms_va_arg(ap, unsigned long long);
            else { unsigned x = ms_va_arg(ap, unsigned); v = len == 3 ? (unsigned short)x : len == 4 ? (unsigned char)x : x; }
            snprintf(spec + sl, sizeof spec - sl, "ll%c", conv);
            w = snprintf(tmp, sizeof tmp, spec, v);
            break; }
        case 'p': case 'P': {
            void *v = ms_va_arg(ap, void *);
            char hx[32];
            int hl = snprintf(hx, sizeof hx, "%016llX", (unsigned long long)(uintptr_t)v);
            pad_put(o, hx, (size_t)hl, width, flags);
            continue; }
        case 'c': {
            int v = ms_va_arg(ap, int);
            if (wide == 1 || (wide == 0 && fmt_wide)) {
                WCHAR wc[2] = { (WCHAR)v, 0 };
                struct outbuf t = { tmp, sizeof tmp, 0 };
                put_wide(&t, wc, -1);
                pad_put(o, tmp, t.len < sizeof tmp ? t.len : sizeof tmp, width, flags);
            } else { char ch = (char)v; pad_put(o, &ch, 1, width, flags); }
            continue; }
        case 'C': {
            int v = ms_va_arg(ap, int);
            if (fmt_wide) { char ch = (char)v; pad_put(o, &ch, 1, width, flags); }
            else { WCHAR wc[2] = { (WCHAR)v, 0 }; struct outbuf t = { tmp, sizeof tmp, 0 }; put_wide(&t, wc, -1); pad_put(o, tmp, t.len, width, flags); }
            continue; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = ms_va_arg(ap, double);
            snprintf(spec + sl, sizeof spec - sl, "%c", conv);
            w = snprintf(tmp, sizeof tmp, spec, v);
            if (w >= (int)sizeof tmp) { char *big = malloc((size_t)w + 1); snprintf(big, (size_t)w + 1, spec, v); put(o, big, (size_t)w); free(big); continue; }
            if (conv != 'a' && conv != 'A' && lsiw_printf_exp3()) w = exp3_fix(tmp, w, width, flags);
            break; }
        case 's': {
            int is_wide = wide == 1 || (wide == 0 && fmt_wide);
            if (wide == -1) is_wide = 0;
            if (is_wide) {
                const WCHAR *ws = ms_va_arg(ap, const WCHAR *);
                struct outbuf t = { tmp, sizeof tmp, 0 };
                put_wide(&t, ws, prec);
                size_t tl = t.len < sizeof tmp ? t.len : sizeof tmp - 1;
                pad_put(o, tmp, tl, width, flags);
                continue;
            }
            const char *s = ms_va_arg(ap, const char *);
            if (!s) s = "(null)";
            snprintf(spec + sl, sizeof spec - sl, "s");
            int need = snprintf(NULL, 0, spec, s);
            if (need < (int)sizeof tmp) w = snprintf(tmp, sizeof tmp, spec, s);
            else { char *big = malloc((size_t)need + 1); snprintf(big, (size_t)need + 1, spec, s); put(o, big, (size_t)need); free(big); continue; }
            break; }
        case 'S': {
            int is_wide = !fmt_wide;
            if (wide == -1) is_wide = 0;
            if (wide == 1) is_wide = 1;
            if (is_wide) {
                const WCHAR *ws = ms_va_arg(ap, const WCHAR *);
                struct outbuf t = { tmp, sizeof tmp, 0 };
                put_wide(&t, ws, prec);
                pad_put(o, tmp, t.len < sizeof tmp ? t.len : sizeof tmp - 1, width, flags);
                continue;
            }
            const char *s = ms_va_arg(ap, const char *);
            if (!s) s = "(null)";
            snprintf(spec + sl, sizeof spec - sl, "s");
            w = snprintf(tmp, sizeof tmp, spec, s);
            break; }
        case 'n': {
            void *np = ms_va_arg(ap, void *);
            if (np) *(int *)np = (int)o->len;
            continue; }
        default:
            put(o, start, (size_t)(p - start + 1));
            continue;
        }
        if (w > 0) put(o, tmp, (size_t)(w < (int)sizeof tmp ? w : (int)sizeof tmp - 1));
    }
    return (int)o->len;
}

int lsiw_ms_vsnprintf(char *buf, size_t n, const char *fmt, ms_va_list ap, int wide_fmt)
{
    struct outbuf o = { buf, n, 0 };
    format_core(&o, fmt, ap, wide_fmt);
    if (o.cap) o.buf[o.len < o.cap ? o.len : o.cap - 1] = 0;
    return (int)o.len;
}

int lsiw_ms_vsnwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, ms_va_list ap)
{
    /* Convert the wide format to UTF-8, format into a UTF-8 buffer, then
     * convert the result back to UTF-16. */
    char *f = lsiw_w2u(fmt);
    struct outbuf o = { NULL, 0, 0 };
    ms_va_list copy = ap;   /* ms_va_list is a plain char* on x86-64 */
    int need = format_core(&o, f, copy, 1);     /* measure */
    char *u = malloc((size_t)need + 1);
    struct outbuf o2 = { u, (size_t)need + 1, 0 };
    format_core(&o2, f, ap, 1);
    u[need < (int)o2.cap ? need : (int)o2.cap - 1] = 0;
    free(f);
    int wn = lsiw_utf8_to_utf16(u, -1, NULL, 0) - 1;   /* chars without NUL */
    if (!buf || n == 0) { free(u); return wn; }
    if ((size_t)wn >= n) { lsiw_utf8_to_utf16(u, -1, buf, (int)n); buf[n - 1] = 0; free(u); return -1; }
    lsiw_utf8_to_utf16(u, -1, buf, (int)n);
    free(u);
    return wn;
}

/* ================================================================== */
/* scanf                                                                */
/* ================================================================== */

struct scan_src { lsiw_getc_fn get; lsiw_ungetc_fn unget; void *ctx; int consumed; };

static int sgetc(struct scan_src *s) { int c = s->get(s->ctx); if (c >= 0) s->consumed++; return c; }
static void sunget(struct scan_src *s, int c) { if (c >= 0) { s->unget(c, s->ctx); s->consumed--; } }

static int skip_ws(struct scan_src *s)
{
    int c;
    do c = sgetc(s); while (c >= 0 && isspace(c));
    return c;
}

/* Store one input character into a %c/%s/%[ destination. Narrow targets
 * receive the bytes as they are; wide targets receive the UTF-16 form of the
 * UTF-8 character starting with byte c. `cap` counts elements and already
 * excludes room for a terminating NUL where one is needed. Returns -1 when the
 * destination is full. */
static int scan_store(void *dst, int is_wide, size_t *i, size_t cap, int c, struct scan_src *s)
{
    if (!is_wide) {
        if (dst) { if (*i >= cap) return -1; ((char *)dst)[*i] = (char)c; }
        (*i)++;
        return 0;
    }
    uint32_t cp = (unsigned)c & 0xFF;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) {
        cp &= extra == 3 ? 0x07u : extra == 2 ? 0x0Fu : 0x1Fu;
        for (int k = 0; k < extra; k++) {
            int n = sgetc(s);
            if (n < 0 || (n & 0xC0) != 0x80) { if (n >= 0) sunget(s, n); cp = 0xFFFD; break; }
            cp = (cp << 6) | ((unsigned)n & 0x3F);
        }
    }
    if (cp >= 0x10000) {
        if (dst) {
            if (*i + 1 >= cap) return -1;
            ((WCHAR *)dst)[*i] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10));
            ((WCHAR *)dst)[*i + 1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        }
        *i += 2;
    } else {
        if (dst) { if (*i >= cap) return -1; ((WCHAR *)dst)[*i] = (WCHAR)cp; }
        (*i)++;
    }
    return 0;
}

static void scan_terminate(void *dst, int is_wide, size_t i)
{
    if (!dst) return;
    if (is_wide) ((WCHAR *)dst)[i] = 0; else ((char *)dst)[i] = 0;
}

int lsiw_ms_vscanf(lsiw_getc_fn get, lsiw_ungetc_fn unget, void *ctx, const char *fmt, ms_va_list ap)
{
    return lsiw_ms_vscanf_ex(get, unget, ctx, fmt, ap, 0);
}

int lsiw_ms_vscanf_ex(lsiw_getc_fn get, lsiw_ungetc_fn unget, void *ctx, const char *fmt, ms_va_list ap, int flags)
{
    struct scan_src s = { get, unget, ctx, 0 };
    int assigned = 0;

    for (const char *p = fmt; *p; p++) {
        if (isspace((unsigned char)*p)) { int c = skip_ws(&s); if (c >= 0) sunget(&s, c); continue; }
        if (*p != '%') {
            int c = sgetc(&s);
            if (c != (unsigned char)*p) { if (c >= 0) sunget(&s, c); return assigned ? assigned : (c < 0 ? -1 : assigned); }
            continue;
        }
        p++;
        if (*p == '%') { int c = skip_ws(&s); if (c != '%') { if (c >= 0) sunget(&s, c); return assigned; } continue; }

        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (!width) width = INT32_MAX;
        int len = 0, wide = 0;
        for (;;) {
            if (*p == 'h') { len = (p[1] == 'h') ? (p++, 4) : 3; p++; wide = -1; }
            else if (*p == 'l') { if (p[1] == 'l') { len = 2; p += 2; } else { len = 1; wide = 1; p++; } }
            else if (*p == 'w') { wide = 1; p++; }
            else if (*p == 'L') { len = 2; p++; }
            else if (*p == 'z' || *p == 'j' || *p == 't') { len = 2; p++; }
            else if (*p == 'I') {
                if (p[1] == '6' && p[2] == '4') { len = 2; p += 3; }
                else if (p[1] == '3' && p[2] == '2') { len = 0; p += 3; }
                else { len = 2; p++; }
            }
            else break;
        }
        char conv = *p;
        if (!conv) break;
        if (conv == 'C') { conv = 'c'; if (!wide) wide = (flags & LSIW_SCAN_WIDE) ? -1 : 1; }
        if (conv == 'S') { conv = 's'; if (!wide) wide = (flags & LSIW_SCAN_WIDE) ? -1 : 1; }
        int is_wide = wide == 1 || (wide == 0 && (flags & LSIW_SCAN_WIDE));

        if (conv == 'n') {
            if (!suppress) { void *np = ms_va_arg(ap, void *); *(int *)np = s.consumed; }
            continue;
        }

        if (conv == 'c' || conv == 's' || conv == '[') {
            void *dst = suppress ? NULL : ms_va_arg(ap, void *);
            size_t cap = (size_t)-1;
            if (!suppress && (flags & LSIW_SCAN_SECURE)) cap = ms_va_arg(ap, unsigned);
            size_t i = 0;
            int c;

            if (conv == 'c') {
                int n = width == INT32_MAX ? 1 : width, got = 0;
                for (int k = 0; k < n; k++) {
                    c = sgetc(&s);
                    if (c < 0) break;
                    if (scan_store(dst, is_wide, &i, cap, c, &s) < 0) { scan_terminate(dst, is_wide, 0); return assigned; }
                    got++;
                }
                if (got == 0) return assigned ? assigned : -1;
                if (!suppress) assigned++;
                continue;
            }

            size_t room = cap == (size_t)-1 ? cap : (cap ? cap - 1 : 0);
            if (conv == 's') {
                c = skip_ws(&s);
                if (c < 0) return assigned ? assigned : -1;
                int k = 0;
                while (c >= 0 && !isspace(c) && k < width) {
                    if (scan_store(dst, is_wide, &i, room, c, &s) < 0) { scan_terminate(dst, is_wide, 0); return assigned; }
                    k++;
                    c = sgetc(&s);
                }
                if (c >= 0) sunget(&s, c);
                scan_terminate(dst, is_wide, i);
                if (!suppress) assigned++;
                continue;
            }

            /* %[ */
            p++;
            int negate = 0;
            if (*p == '^') { negate = 1; p++; }
            char set[256] = { 0 };
            if (*p == ']') { set[(unsigned char)']'] = 1; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') { for (int ch = (unsigned char)p[0]; ch <= (unsigned char)p[2]; ch++) set[ch] = 1; p += 3; }
                else set[(unsigned char)*p++] = 1;
            }
            int k = 0;
            c = 0;
            while (k < width && (c = sgetc(&s)) >= 0) {
                int in = c < 256 ? set[c] : 0;
                if (negate ? in : !in) { sunget(&s, c); break; }
                if (scan_store(dst, is_wide, &i, room, c, &s) < 0) { scan_terminate(dst, is_wide, 0); return assigned; }
                k++;
            }
            if (k == 0) return assigned ? assigned : (c < 0 ? -1 : assigned);
            scan_terminate(dst, is_wide, i);
            if (!suppress) assigned++;
            continue;
        }

        /* Numeric conversions: gather into a token then convert. */
        char tok[128]; int ti = 0;
        int c = skip_ws(&s);
        int base = 10, is_float = 0, is_unsigned = 0;
        switch (conv) {
        case 'd': base = 10; break;
        case 'i': base = 0; break;
        case 'u': base = 10; is_unsigned = 1; break;
        case 'x': case 'X': base = 16; is_unsigned = 1; break;
        case 'o': base = 8; is_unsigned = 1; break;
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': is_float = 1; break;
        case 'p': base = 16; is_unsigned = 1; break;
        default: if (c >= 0) sunget(&s, c); return assigned;
        }
        if (c >= 0 && (c == '+' || c == '-') && ti < width) { tok[ti++] = (char)c; c = sgetc(&s); }
        if (is_float) {
            while (c >= 0 && ti < width && (isdigit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-' || c == 'x' || c == 'X' || c == 'p' || c == 'P' || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { tok[ti++] = (char)c; c = sgetc(&s); }
        } else {
            if (base == 0 || base == 16) {
                if (c == '0' && ti < width) { tok[ti++] = (char)c; c = sgetc(&s); if ((c == 'x' || c == 'X') && ti < width) { tok[ti++] = (char)c; c = sgetc(&s); base = 16; } else if (base == 0) base = 8; }
            }
            while (c >= 0 && ti < width) {
                int d;
                if (isdigit(c)) d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                if (base && d >= base) break;
                tok[ti++] = (char)c; c = sgetc(&s);
            }
        }
        if (c >= 0) sunget(&s, c);
        if (ti == 0 || (ti == 1 && (tok[0] == '+' || tok[0] == '-'))) return assigned ? assigned : -1;
        tok[ti] = 0;
        if (!suppress) {
            if (is_float) {
                double v = strtod(tok, NULL);
                if (len >= 1 && len <= 2) *(double *)ms_va_arg(ap, void *) = v;
                else *(float *)ms_va_arg(ap, void *) = (float)v;
            } else {
                unsigned long long v = strtoull(tok, NULL, base ? base : 10);
                void *dst = ms_va_arg(ap, void *);
                if (conv == 'p' || len == 2) *(uint64_t *)dst = v;
                else if (len == 3) { if (is_unsigned) *(uint16_t *)dst = (uint16_t)v; else *(int16_t *)dst = (int16_t)v; }
                else if (len == 4) { if (is_unsigned) *(uint8_t *)dst = (uint8_t)v; else *(int8_t *)dst = (int8_t)v; }
                else *(uint32_t *)dst = (uint32_t)v;
            }
            assigned++;
        }
    }
    return assigned;
}
