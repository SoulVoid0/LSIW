/* UTF-8 <-> UTF-16 conversion and small string helpers.
 * Windows "W" functions use UTF-16; LSIW treats the ANSI code page as UTF-8.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "native.h"
#include <stdlib.h>
#include <string.h>

size_t lsiw_wcslen(const WCHAR *s)
{
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

/* Decode one UTF-8 sequence; invalid bytes become U+FFFD. */
static uint32_t utf8_next(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int extra = 0;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { *pp = p; return 0xFFFD; }
    for (int i = 0; i < extra; i++) {
        if (p >= end || (*p & 0xC0) != 0x80) { *pp = p; return 0xFFFD; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *pp = p;
    return c;
}

int lsiw_utf8_to_utf16(const char *s, int slen, WCHAR *out, int outlen)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + (slen < 0 ? strlen(s) + 1 : (size_t)slen);
    int n = 0;
    while (p < end) {
        uint32_t c = utf8_next(&p, end);
        if (c >= 0x10000) {
            c -= 0x10000;
            if (out && n + 1 < outlen) { out[n] = (WCHAR)(0xD800 + (c >> 10)); out[n + 1] = (WCHAR)(0xDC00 + (c & 0x3FF)); }
            else if (out) return -1;
            n += 2;
        } else {
            if (out && n < outlen) out[n] = (WCHAR)c;
            else if (out) return -1;
            n++;
        }
    }
    return n;
}

int lsiw_utf16_to_utf8(const WCHAR *s, int slen, char *out, int outlen)
{
    size_t len = slen < 0 ? lsiw_wcslen(s) + 1 : (size_t)slen;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        char b[4]; int k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); k = 3; }
        else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); k = 4; }
        if (out) {
            if (n + k > outlen) return -1;
            memcpy(out + n, b, (size_t)k);
        }
        n += k;
    }
    return n;
}

char *lsiw_w2u_n(const WCHAR *w, int n)
{
    if (!w) return NULL;
    int need = lsiw_utf16_to_utf8(w, n, NULL, 0);
    char *s = malloc((size_t)need + 1);
    lsiw_utf16_to_utf8(w, n, s, need);
    s[need] = 0;
    return s;
}

char *lsiw_w2u(const WCHAR *w) { return lsiw_w2u_n(w, -1); }

WCHAR *lsiw_u2w(const char *s)
{
    if (!s) return NULL;
    int need = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    WCHAR *w = malloc(((size_t)need + 1) * sizeof(WCHAR));
    lsiw_utf8_to_utf16(s, -1, w, need);
    w[need] = 0;
    return w;
}

int lsiw_u2w_buf(const char *s, WCHAR *out, int outlen)
{
    int need = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    if (!out || need > outlen) return need;
    lsiw_utf8_to_utf16(s, -1, out, outlen);
    return need;
}

DWORD lsiw_ret_str_a(const char *s, char *buf, DWORD size)
{
    size_t l = strlen(s);
    if (!buf || l + 1 > size) return (DWORD)l + 1;
    memcpy(buf, s, l + 1);
    return (DWORD)l;
}

DWORD lsiw_ret_str_w(const char *s, WCHAR *buf, DWORD size)
{
    int need = lsiw_utf8_to_utf16(s, -1, NULL, 0);    /* includes NUL */
    if (!buf || (DWORD)need > size) return (DWORD)need;
    lsiw_utf8_to_utf16(s, -1, buf, (int)size);
    return (DWORD)need - 1;
}

char *lsiw_path_a(const char *win)
{
    if (!win) return NULL;
    return lsiw_win_to_linux(win);
}

char *lsiw_path_w(const WCHAR *win)
{
    if (!win) return NULL;
    char *u = lsiw_w2u(win);
    char *r = lsiw_win_to_linux(u);
    free(u);
    return r;
}
