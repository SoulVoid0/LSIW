/* LSIW: Windows shortcuts (.lnk files, [MS-SHLLINK]).
 *
 * A shortcut names its target three ways, any of which may be missing:
 * the LinkInfo structure (the local path, "C:\Program Files\x\x.exe"), the
 * item ID list (one shell item per path component, as Explorer browses
 * to it), and an environment-variable data block ("%ProgramFiles%\x.exe").
 * Reading tries them in that order. The string data after them holds the
 * arguments, working directory, description and icon.
 *
 * Writing produces what Windows itself writes for a file on C: (an item
 * ID list and LinkInfo), so the shortcut also works on Windows.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "lsiw.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    HAS_ID_LIST       = 0x1,
    HAS_LINK_INFO     = 0x2,
    HAS_NAME          = 0x4,
    HAS_RELATIVE_PATH = 0x8,
    HAS_WORKING_DIR   = 0x10,
    HAS_ARGUMENTS     = 0x20,
    HAS_ICON_LOCATION = 0x40,
    IS_UNICODE        = 0x80,
    HAS_EXP_STRING    = 0x200,
};

static const unsigned char link_clsid[16] = {
    0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};
/* {20D04FE0-3AEA-1069-A2D8-08002B30309D}: "This PC" */
static const unsigned char my_computer[16] = {
    0xE0, 0x4F, 0xD0, 0x20, 0xEA, 0x3A, 0x69, 0x10, 0xA2, 0xD8, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D
};

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ------------------------------------------------------------------ */
/* Text                                                                 */
/* ------------------------------------------------------------------ */

static void put_utf8(char **o, uint32_t c)
{
    char *p = *o;
    if (c < 0x80) *p++ = (char)c;
    else if (c < 0x800) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        *p++ = (char)(0xE0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F));
    } else {
        *p++ = (char)(0xF0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 0x3F));
        *p++ = (char)(0x80 | (c >> 6 & 0x3F)); *p++ = (char)(0x80 | (c & 0x3F));
    }
    *o = p;
}

/* `n` UTF-16LE units (or up to a NUL when n is SIZE_MAX, bounded by end). */
static char *from_utf16(const unsigned char *p, size_t n, const unsigned char *end)
{
    size_t max = (size_t)(end - p) / 2;
    if (n > max) n = max;
    char *out = malloc(n * 3 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = rd16(p + 2 * i);
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t lo = rd16(p + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        put_utf8(&o, c);
    }
    *o = 0;
    return out;
}

/* Windows-1252 text up to a NUL (or n bytes), as UTF-8. */
static char *from_ansi(const unsigned char *p, size_t n, const unsigned char *end)
{
    static const uint16_t cp1252[32] = {
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152,
        0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122,
        0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
    };
    if (n > (size_t)(end - p)) n = (size_t)(end - p);
    char *out = malloc(n * 3 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n && p[i]; i++) {
        uint32_t c = p[i];
        if (c >= 0x80 && c < 0xA0) c = cp1252[c - 0x80];
        put_utf8(&o, c);
    }
    *o = 0;
    return out;
}

static size_t to_utf16(const char *s, unsigned char *out)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (int k = 0; k < extra && (*p & 0xC0) == 0x80; k++) c = c << 6 | (*p++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            uint32_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            if (out) { out[2 * n] = (unsigned char)hi; out[2 * n + 1] = (unsigned char)(hi >> 8); }
            n++;
            c = lo;
        }
        if (out) { out[2 * n] = (unsigned char)c; out[2 * n + 1] = (unsigned char)(c >> 8); }
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Reading                                                              */
/* ------------------------------------------------------------------ */

static char *join_path(char *a, const char *b)
{
    if (!a) return strdup(b);
    size_t l = strlen(a);
    char *r = lsiw_strdup_printf("%s%s%s", a, l && a[l - 1] == '\\' ? "" : "\\", b);
    free(a);
    return r;
}

/* The path an item ID list browses to: This PC > C:\ > folders > file. */
static char *path_from_id_list(const unsigned char *p, const unsigned char *end)
{
    char *path = NULL;
    while (p + 2 <= end) {
        uint16_t sz = rd16(p);
        if (sz == 0 || p + sz > end) break;
        const unsigned char *it = p + 2, *ie = p + sz;
        unsigned type = sz > 2 ? it[0] : 0;
        if ((type & 0x70) == 0x20 && ie - it > 1) {
            /* A volume item: "C:\" */
            free(path);
            path = from_ansi(it + 1, (size_t)(ie - it - 1), ie);
        } else if ((type & 0x70) == 0x30 && ie - it > 12 && path) {
            /* A file or folder: an 8.3 name, then (on Windows XP and later)
             * an extension block 0xBEEF0004 with the long name. */
            const unsigned char *nm = it + 12;
            char *name = (type & 0x04) ? from_utf16(nm, SIZE_MAX, ie) : from_ansi(nm, (size_t)(ie - nm), ie);
            size_t skip = (type & 0x04) ? 2 * (strlen(name) + 1) : strlen(name) + 1;
            const unsigned char *x = nm + skip;
            if (!(type & 0x04) && ((x - it) & 1)) x++;
            /* The extension block: size(2) version(2) signature(4) ... */
            if (x + 8 <= ie && rd32(x + 4) == 0xBEEF0004) {
                uint16_t xs = rd16(x), ver = rd16(x + 2);
                size_t off = ver >= 9 ? 46 : ver >= 8 ? 42 : ver >= 7 ? 38 : ver >= 3 ? 20 : 0;
                if (off && x + off < ie && xs >= off) {
                    char *lng = from_utf16(x + off, SIZE_MAX, x + xs <= ie ? x + xs : ie);
                    if (lng && *lng) { free(name); name = lng; } else free(lng);
                }
            }
            if (name && *name) path = join_path(path, name);
            free(name);
        }
        p += sz;
    }
    return path;
}

static char *read_string(const unsigned char **pp, const unsigned char *end, int unicode)
{
    const unsigned char *p = *pp;
    if (p + 2 > end) return NULL;
    uint16_t n = rd16(p);
    p += 2;
    size_t bytes = unicode ? 2u * n : n;
    if (p + bytes > end) { *pp = end; return NULL; }
    char *s = unicode ? from_utf16(p, n, p + bytes) : from_ansi(p, n, p + bytes);
    *pp = p + bytes;
    return s;
}

int lsiw_lnk_parse(const unsigned char *d, size_t len, struct lsiw_lnk *l)
{
    memset(l, 0, sizeof *l);
    if (len < 0x4C || rd32(d) != 0x4C || memcmp(d + 4, link_clsid, 16)) { errno = EINVAL; return -1; }
    const unsigned char *end = d + len, *p = d + 0x4C;
    uint32_t flags = rd32(d + 0x14);
    l->show = (int)rd32(d + 0x3C);
    int unicode = !!(flags & IS_UNICODE);

    if (flags & HAS_ID_LIST) {
        if (p + 2 > end) goto bad;
        uint16_t n = rd16(p);
        if (p + 2 + n > end) goto bad;
        l->target = path_from_id_list(p + 2, p + 2 + n);
        p += 2 + n;
    }
    if (flags & HAS_LINK_INFO) {
        if (p + 4 > end) goto bad;
        uint32_t size = rd32(p);
        if (size < 0x1C || p + size > end) goto bad;
        uint32_t hsize = rd32(p + 4), lflags = rd32(p + 8);
        uint32_t base = rd32(p + 16), suffix = rd32(p + 24);
        const unsigned char *le = p + size;
        char *path = NULL;
        if (lflags & 1) {
            if (hsize >= 0x24 && rd32(p + 28)) {
                char *b = from_utf16(p + rd32(p + 28), SIZE_MAX, le);
                char *s = hsize >= 0x24 && rd32(p + 32) ? from_utf16(p + rd32(p + 32), SIZE_MAX, le) : strdup("");
                path = lsiw_strdup_printf("%s%s", b ? b : "", s ? s : "");
                free(b); free(s);
            } else if (base && base < size) {
                char *b = from_ansi(p + base, size - base, le);
                char *s = suffix && suffix < size ? from_ansi(p + suffix, size - suffix, le) : strdup("");
                path = lsiw_strdup_printf("%s%s", b ? b : "", s ? s : "");
                free(b); free(s);
            }
        }
        if (path && *path) { free(l->target); l->target = path; } else free(path);
        p += size;
    }
    char **slots[5] = { &l->description, &l->relative_path, &l->workdir, &l->args, &l->icon };
    uint32_t bits[5] = { HAS_NAME, HAS_RELATIVE_PATH, HAS_WORKING_DIR, HAS_ARGUMENTS, HAS_ICON_LOCATION };
    for (int i = 0; i < 5; i++)
        if (flags & bits[i]) *slots[i] = read_string(&p, end, unicode);
    l->icon_index = (int)rd32(d + 0x38);

    /* Extra data blocks: the environment-variable target is the last resort. */
    while (p + 8 <= end) {
        uint32_t size = rd32(p), sig = rd32(p + 4);
        if (size < 8 || p + size > end) break;
        if (sig == 0xA0000001 && size >= 0x314 && (!l->target || !*l->target)) {
            char *t = from_utf16(p + 8 + 260, 260, p + size);
            if (!t || !*t) { free(t); t = from_ansi(p + 8, 260, p + size); }
            if (t && *t) { free(l->target); l->target = t; } else free(t);
        }
        p += size;
    }
    if (!l->target && l->relative_path && *l->relative_path) l->target = strdup(l->relative_path);
    return 0;
bad:
    lsiw_lnk_free(l);
    errno = EINVAL;
    return -1;
}

int lsiw_lnk_read(const char *path, struct lsiw_lnk *l)
{
    memset(l, 0, sizeof *l);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, 1 << 20, f) : 0;
    fclose(f);
    int rc = buf ? lsiw_lnk_parse(buf, n, l) : -1;
    free(buf);
    return rc;
}

void lsiw_lnk_free(struct lsiw_lnk *l)
{
    free(l->target);
    free(l->args);
    free(l->workdir);
    free(l->description);
    free(l->relative_path);
    free(l->icon);
    memset(l, 0, sizeof *l);
}

/* ------------------------------------------------------------------ */
/* Writing                                                              */
/* ------------------------------------------------------------------ */

struct buf { unsigned char *v; size_t n, cap; };

static void put(struct buf *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->v = realloc(b->v, b->cap);
        if (!b->v) abort();
    }
    memcpy(b->v + b->n, p, n);
    b->n += n;
}
static void put16(struct buf *b, unsigned v) { unsigned char c[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; put(b, c, 2); }
static void put32(struct buf *b, uint32_t v)
{
    unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    put(b, c, 4);
}
static void put_zero(struct buf *b, size_t n) { while (n--) put(b, "", 1); }
static void set16(struct buf *b, size_t at, unsigned v) { b->v[at] = (unsigned char)v; b->v[at + 1] = (unsigned char)(v >> 8); }

static void put_wstr(struct buf *b, const char *s, int nul)
{
    size_t n = to_utf16(s, NULL);
    unsigned char *w = malloc(2 * n + 2);
    to_utf16(s, w);
    put(b, w, 2 * n);
    if (nul) put16(b, 0);
    free(w);
}

/* One file or folder item: the name as both 8.3-ish ANSI (ASCII only) and
 * the long Unicode name in a version 3 extension block. */
static void put_file_item(struct buf *b, const char *name, int dir)
{
    size_t start = b->n;
    put16(b, 0);                              /* size, set below */
    put(b, (unsigned char[]){ (unsigned char)(dir ? 0x31 : 0x32), 0 }, 2);
    put32(b, 0);                              /* file size */
    put32(b, 0);                              /* modification time (DOS) */
    put16(b, dir ? 0x10 : 0x20);              /* attributes */
    size_t nl = 0;
    for (const char *c = name; *c; c++, nl++) put(b, (unsigned char)*c < 0x80 ? c : "_", 1);
    put(b, "", 1);
    if ((b->n - start) & 1) put(b, "", 1);
    size_t ext = b->n;
    put16(b, 0);                              /* extension size */
    put16(b, 3);                              /* version */
    put32(b, 0xBEEF0004);
    put32(b, 0);                              /* creation time */
    put32(b, 0);                              /* access time */
    put16(b, 0x14);                           /* identifier */
    put16(b, 0);                              /* localized name size: none */
    put_wstr(b, name, 1);
    put16(b, (unsigned)(ext - start));        /* offset of the extension block */
    set16(b, ext, (unsigned)(b->n - ext));
    set16(b, start, (unsigned)(b->n - start));
    (void)nl;
}

int lsiw_lnk_write(const char *path, const struct lsiw_lnk *l)
{
    const char *target = l->target ? l->target : "";
    struct buf b = { NULL, 0, 0 };
    uint32_t flags = IS_UNICODE;
    int has_target = target[0] && target[1] == ':' && target[2] == '\\';
    if (has_target) flags |= HAS_ID_LIST | HAS_LINK_INFO;
    if (l->description && *l->description) flags |= HAS_NAME;
    if (has_target || (l->relative_path && *l->relative_path)) flags |= HAS_RELATIVE_PATH;
    if (l->workdir && *l->workdir) flags |= HAS_WORKING_DIR;
    if (l->args && *l->args) flags |= HAS_ARGUMENTS;
    if (l->icon && *l->icon) flags |= HAS_ICON_LOCATION;

    /* ShellLinkHeader */
    put32(&b, 0x4C);
    put(&b, link_clsid, 16);
    put32(&b, flags);
    put32(&b, 0x20);                          /* FILE_ATTRIBUTE_ARCHIVE */
    put_zero(&b, 24);                         /* creation, access, write times */
    put32(&b, 0);                             /* file size */
    put32(&b, (uint32_t)l->icon_index);
    put32(&b, l->show ? (uint32_t)l->show : 1);
    put16(&b, 0);                             /* hotkey */
    put_zero(&b, 10);

    if (has_target) {
        /* LinkTargetIDList: This PC, the drive, then each path component. */
        size_t at = b.n;
        put16(&b, 0);
        put16(&b, 0x14);
        put(&b, (unsigned char[]){ 0x1F, 0x50 }, 2);
        put(&b, my_computer, 16);
        unsigned char drive[25] = { 0x19, 0x00, 0x2F };
        drive[3] = (unsigned char)target[0];
        drive[4] = ':';
        drive[5] = '\\';
        put(&b, drive, 25);
        char *copy = strdup(target + 3);
        char *save = NULL;
        for (char *c = strtok_r(copy, "\\", &save); c; c = strtok_r(NULL, "\\", &save)) {
            char *next = save && *save ? save : NULL;
            put_file_item(&b, c, next != NULL);
        }
        free(copy);
        put16(&b, 0);
        set16(&b, at, (unsigned)(b.n - at - 2));

        /* LinkInfo with a local base path, ANSI and Unicode. */
        size_t li = b.n;
        char *ansi = strdup(target);
        for (char *c = ansi; *c; c++) if ((unsigned char)*c >= 0x80) *c = '?';
        size_t hdr = 0x24, vol = 0x11;
        size_t base_off = hdr + vol, suffix_off = base_off + strlen(ansi) + 1;
        size_t ubase_off = suffix_off + 1;
        if (ubase_off & 1) ubase_off++;
        size_t ubase_len = 2 * (to_utf16(target, NULL) + 1);
        size_t total = ubase_off + ubase_len + 2;
        put32(&b, (uint32_t)total);
        put32(&b, (uint32_t)hdr);
        put32(&b, 1);                         /* VolumeIDAndLocalBasePath */
        put32(&b, (uint32_t)hdr);             /* VolumeID offset */
        put32(&b, (uint32_t)base_off);
        put32(&b, 0);                         /* no network part */
        put32(&b, (uint32_t)suffix_off);
        put32(&b, (uint32_t)ubase_off);
        put32(&b, (uint32_t)(ubase_off + ubase_len));
        /* VolumeID: size, DRIVE_FIXED, serial, label offset, empty label */
        put32(&b, (uint32_t)vol);
        put32(&b, 3);
        put32(&b, 0);
        put32(&b, 0x10);
        put(&b, "", 1);
        put(&b, ansi, strlen(ansi) + 1);
        put(&b, "", 1);                       /* common path suffix */
        while (b.n - li < ubase_off) put(&b, "", 1);
        put_wstr(&b, target, 1);
        put16(&b, 0);                         /* Unicode suffix */
        free(ansi);
    }
    const char *strs[5] = { l->description, l->relative_path, l->workdir, l->args, l->icon };
    uint32_t bits[5] = { HAS_NAME, HAS_RELATIVE_PATH, HAS_WORKING_DIR, HAS_ARGUMENTS, HAS_ICON_LOCATION };
    char *rel = NULL;
    if ((flags & HAS_RELATIVE_PATH) && !(l->relative_path && *l->relative_path)) {
        /* Windows writes the target relative to the shortcut; the absolute
         * path also resolves, and stays right wherever the link is moved. */
        rel = strdup(target);
        strs[1] = rel;
    }
    for (int i = 0; i < 5; i++)
        if (flags & bits[i]) {
            put16(&b, (unsigned)to_utf16(strs[i], NULL));
            put_wstr(&b, strs[i], 0);
        }
    free(rel);
    put32(&b, 0);                             /* TerminalBlock */

    FILE *f = fopen(path, "wb");
    int rc = -1;
    if (f) {
        rc = fwrite(b.v, 1, b.n, f) == b.n ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    }
    free(b.v);
    return rc;
}
