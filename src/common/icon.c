/* LSIW: program icons as PNG images.
 *
 * Windows keeps a program's icon inside the program: the resource section
 * of the .exe or .dll holds icon groups (RT_GROUP_ICON), each listing the
 * sizes and colour depths of one icon, and the images (RT_ICON). Explorer
 * shows the first group. An .ico file holds one group with its images.
 * Each image is a PNG (usually the 256x256 one) or a DIB: a BITMAPINFOHEADER
 * with twice the height, the colour bitmap bottom-up, then a 1-bit AND mask.
 *
 * lsiw_icon_png() picks the largest image of that group (at most 256x256,
 * the deepest colours among equal sizes) and returns it as a PNG: a PNG
 * image as it is, a DIB converted to RGBA and written as a PNG with
 * uncompressed ("stored") deflate blocks, so no zlib is needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RT_ICON       3
#define RT_GROUP_ICON 14
#define MAX_SIDE      256

static const unsigned char png_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ------------------------------------------------------------------ */
/* PNG writing                                                          */
/* ------------------------------------------------------------------ */

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void put32be(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

/* Append a chunk: length, type, data, CRC of type and data. */
static unsigned char *chunk(unsigned char *o, const char *type, const unsigned char *data, size_t len)
{
    put32be(o, (uint32_t)len);
    memcpy(o + 4, type, 4);
    if (len) memcpy(o + 8, data, len);
    put32be(o + 8 + len, crc32_of(o + 4, len + 4));
    return o + 12 + len;
}

int lsiw_png_encode(const uint32_t *rgba, int w, int h, unsigned char **out, size_t *out_len)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { errno = EINVAL; return -1; }
    crc_init();
    size_t row = (size_t)w * 4 + 1, raw_len = row * (size_t)h;
    size_t blocks = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    size_t total = 8 + (12 + 13) + (12 + z_len) + 12;
    unsigned char *png = malloc(total), *z = malloc(z_len), *raw = malloc(raw_len);
    if (!png || !z || !raw) { free(png); free(z); free(raw); errno = ENOMEM; return -1; }

    for (int y = 0; y < h; y++) {
        unsigned char *r = raw + row * (size_t)y;
        *r++ = 0;                                   /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t px = rgba[(size_t)y * (size_t)w + (size_t)x];   /* 0xAARRGGBB */
            *r++ = (unsigned char)(px >> 16);
            *r++ = (unsigned char)(px >> 8);
            *r++ = (unsigned char)px;
            *r++ = (unsigned char)(px >> 24);
        }
    }
    /* zlib stream: header, stored deflate blocks, Adler-32. */
    unsigned char *o = z;
    *o++ = 0x78; *o++ = 0x01;
    size_t left = raw_len, pos = 0;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    do {
        size_t n = left > 65535 ? 65535 : left;
        *o++ = left == n ? 1 : 0;
        *o++ = (unsigned char)n; *o++ = (unsigned char)(n >> 8);
        *o++ = (unsigned char)~n; *o++ = (unsigned char)(~n >> 8);
        memcpy(o, raw + pos, n);
        o += n; pos += n; left -= n;
    } while (left);
    put32be(o, b << 16 | a);
    o += 4;

    unsigned char ihdr[13];
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;  /* 8-bit RGBA */
    unsigned char *p = png;
    memcpy(p, png_sig, 8);
    p = chunk(p + 8, "IHDR", ihdr, 13);
    p = chunk(p, "IDAT", z, (size_t)(o - z));
    p = chunk(p, "IEND", NULL, 0);
    free(z);
    free(raw);
    *out = png;
    *out_len = (size_t)(p - png);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Icon images                                                          */
/* ------------------------------------------------------------------ */

/* Width and height of a PNG from its IHDR; 0 if it is not one. */
static int png_size(const unsigned char *d, size_t n, int *w, int *h)
{
    if (n < 24 || memcmp(d, png_sig, 8) || memcmp(d + 12, "IHDR", 4)) return 0;
    *w = (int)(d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19]);
    *h = (int)(d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23]);
    return 1;
}

/* One icon image (PNG or DIB) -> PNG bytes. */
static int image_to_png(const unsigned char *d, size_t n, unsigned char **out, size_t *out_len)
{
    int w, h;
    if (png_size(d, n, &w, &h)) {
        *out = malloc(n);
        if (!*out) return -1;
        memcpy(*out, d, n);
        *out_len = n;
        return 0;
    }
    if (n < 40) return -1;
    uint32_t hsize = rd32(d);
    int32_t bw = (int32_t)rd32(d + 4), bh = (int32_t)rd32(d + 8);
    uint16_t bpp = rd16(d + 14);
    uint32_t compression = rd32(d + 16), used = rd32(d + 32);
    if (hsize < 40 || hsize > n || bw <= 0 || bw > MAX_SIDE * 2 || bh == 0 || compression != 0) return -1;
    int top_down = bh < 0;
    if (top_down) bh = -bh;
    w = bw;
    h = bh / 2;                       /* colour bitmap + AND mask */
    if (h <= 0 || h > MAX_SIDE * 2) return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return -1;
    size_t ncolors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    if (ncolors > 256) return -1;
    const unsigned char *pal = d + hsize;
    const unsigned char *bits = pal + ncolors * 4;
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    size_t mstride = (((size_t)w + 31) / 32) * 4;
    const unsigned char *mask = bits + stride * (size_t)h;
    if ((size_t)(bits - d) > n || stride * (size_t)h > n - (size_t)(bits - d)) return -1;
    int have_mask = (size_t)(mask - d) + mstride * (size_t)h <= n;

    uint32_t *px = calloc((size_t)w * (size_t)h, sizeof *px);
    if (!px) return -1;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *r = bits + stride * (size_t)(top_down ? y : h - 1 - y);
        uint32_t *o = px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 32) {
                const unsigned char *q = r + x * 4;
                c = (uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
                if (q[3]) any_alpha = 1;
            } else if (bpp == 24) {
                const unsigned char *q = r + x * 3;
                c = 0xff000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            } else {
                unsigned idx;
                if (bpp == 8) idx = r[x];
                else if (bpp == 4) idx = (r[x / 2] >> (x & 1 ? 0 : 4)) & 0xf;
                else idx = (r[x / 8] >> (7 - (x & 7))) & 1;
                const unsigned char *q = idx < ncolors ? pal + idx * 4 : (const unsigned char *)"\0\0\0\0";
                c = 0xff000000u | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
            }
            o[x] = c;
        }
    }
    /* 32-bit images carry their own alpha; the others (and 32-bit images
     * whose alpha is all zero, from old icon editors) use the AND mask. */
    if ((bpp != 32 || !any_alpha) && have_mask) {
        for (int y = 0; y < h; y++) {
            const unsigned char *m = mask + mstride * (size_t)(top_down ? y : h - 1 - y);
            uint32_t *o = px + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++)
                if ((m[x / 8] >> (7 - (x & 7))) & 1) o[x] = 0;
                else o[x] |= 0xff000000u;
        }
    } else if (bpp == 32 && !any_alpha) {
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) px[i] |= 0xff000000u;
    }
    int rc = lsiw_png_encode(px, w, h, out, out_len);
    free(px);
    return rc;
}

/* The better of two entries: the larger side up to MAX_SIDE, then the
 * deeper colours. width 0 means 256. */
static int better(int w, int bpp, int best_w, int best_bpp)
{
    if (best_w < 0) return 1;
    if (w > MAX_SIDE) return 0;
    if (best_w > MAX_SIDE) return 1;
    if (w != best_w) return w > best_w;
    return bpp > best_bpp;
}

/* An .ico file in memory -> PNG of its best image. */
static int ico_to_png(const unsigned char *d, size_t n, unsigned char **out, size_t *out_len)
{
    if (n < 6 || rd16(d) != 0 || rd16(d + 2) != 1) return -1;
    unsigned count = rd16(d + 4);
    if (!count || 6 + (size_t)count * 16 > n) return -1;
    int best = -1, bw = -1, bb = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = d + 6 + i * 16;
        int w = e[0] ? e[0] : 256, bpp = rd16(e + 6);
        uint32_t size = rd32(e + 8), off = rd32(e + 12);
        if (off > n || size > n - off) continue;
        int pw, ph;
        if (png_size(d + off, size, &pw, &ph)) w = pw;      /* entries of big PNGs say 0 */
        if (better(w, bpp, bw, bb)) { best = (int)i; bw = w; bb = bpp; }
    }
    if (best < 0) return -1;
    const unsigned char *e = d + 6 + best * 16;
    return image_to_png(d + rd32(e + 12), rd32(e + 8), out, out_len);
}

/* ------------------------------------------------------------------ */
/* PE resources                                                         */
/* ------------------------------------------------------------------ */

struct pe {
    const unsigned char *d;
    size_t n;
    const unsigned char *sections;
    unsigned nsections;
    uint32_t rsrc_rva, rsrc_size;
};

/* File offset of an RVA; (size_t)-1 if it is in no section. */
static size_t rva_off(const struct pe *pe, uint32_t rva)
{
    for (unsigned i = 0; i < pe->nsections; i++) {
        const unsigned char *s = pe->sections + i * 40;
        uint32_t va = rd32(s + 12), vsize = rd32(s + 8), raw_size = rd32(s + 16), raw = rd32(s + 20);
        uint32_t span = vsize > raw_size ? vsize : raw_size;
        if (rva >= va && rva - va < span) {
            if (rva - va >= raw_size) return (size_t)-1;
            return (size_t)raw + (rva - va);
        }
    }
    return (size_t)-1;
}

static int pe_open(struct pe *pe, const unsigned char *d, size_t n)
{
    memset(pe, 0, sizeof *pe);
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return -1;
    uint32_t lfanew = rd32(d + 0x3c);
    if (lfanew > n - 24 || memcmp(d + lfanew, "PE\0\0", 4)) return -1;
    const unsigned char *coff = d + lfanew + 4;
    unsigned nsec = rd16(coff + 2), optsize = rd16(coff + 16);
    const unsigned char *opt = coff + 20;
    if ((size_t)(opt - d) + optsize > n || optsize < 2) return -1;
    uint16_t magic = rd16(opt);
    size_t dd = magic == 0x20b ? 112 : magic == 0x10b ? 96 : 0;
    if (!dd || optsize < dd + 3 * 8) return -1;
    uint32_t ndirs = rd32(opt + dd - 4);
    if (ndirs < 3) return -1;
    pe->rsrc_rva = rd32(opt + dd + 2 * 8);
    pe->rsrc_size = rd32(opt + dd + 2 * 8 + 4);
    pe->sections = opt + optsize;
    pe->nsections = nsec;
    if ((size_t)(pe->sections - d) + (size_t)nsec * 40 > n) return -1;
    pe->d = d;
    pe->n = n;
    return pe->rsrc_rva && pe->rsrc_size ? 0 : -1;
}

/* Entries of a resource directory at offset `dir` inside .rsrc. */
static int res_dir(const struct pe *pe, size_t base, uint32_t dir, const unsigned char **ents, unsigned *count)
{
    if (dir > pe->rsrc_size - 16 || base + dir + 16 > pe->n) return -1;
    const unsigned char *t = pe->d + base + dir;
    unsigned c = (unsigned)rd16(t + 12) + rd16(t + 14);
    if (base + dir + 16 + (size_t)c * 8 > pe->n) return -1;
    *ents = t + 16;
    *count = c;
    return 0;
}

/* The data of the first leaf under a directory entry (any language). */
static int res_leaf(const struct pe *pe, size_t base, uint32_t off, int depth, const unsigned char **data, size_t *len)
{
    if (depth > 3) return -1;
    if (off & 0x80000000u) {
        const unsigned char *e;
        unsigned c;
        if (res_dir(pe, base, off & 0x7fffffffu, &e, &c) < 0 || !c) return -1;
        return res_leaf(pe, base, rd32(e + 4), depth + 1, data, len);
    }
    if (off > pe->rsrc_size - 16 || base + off + 16 > pe->n) return -1;
    const unsigned char *de = pe->d + base + off;
    uint32_t rva = rd32(de), size = rd32(de + 4);
    size_t fo = rva_off(pe, rva);
    if (fo == (size_t)-1 || fo > pe->n || size > pe->n - fo) return -1;
    *data = pe->d + fo;
    *len = size;
    return 0;
}

/* The type directory of `type` (RT_ICON, RT_GROUP_ICON). */
static int res_type(const struct pe *pe, size_t base, unsigned type, const unsigned char **ents, unsigned *count)
{
    const unsigned char *e;
    unsigned c;
    if (res_dir(pe, base, 0, &e, &c) < 0) return -1;
    for (unsigned i = 0; i < c; i++) {
        uint32_t name = rd32(e + i * 8), off = rd32(e + i * 8 + 4);
        if (!(name & 0x80000000u) && name == type && (off & 0x80000000u))
            return res_dir(pe, base, off & 0x7fffffffu, ents, count);
    }
    return -1;
}

static int pe_to_png(const unsigned char *d, size_t n, unsigned char **out, size_t *out_len)
{
    struct pe pe;
    if (pe_open(&pe, d, n) < 0) return -1;
    size_t base = rva_off(&pe, pe.rsrc_rva);
    if (base == (size_t)-1 || base >= n) return -1;
    if (pe.rsrc_size > n - base) pe.rsrc_size = (uint32_t)(n - base);
    const unsigned char *groups, *icons;
    unsigned ng, ni;
    if (res_type(&pe, base, RT_GROUP_ICON, &groups, &ng) < 0 || !ng) return -1;
    if (res_type(&pe, base, RT_ICON, &icons, &ni) < 0 || !ni) return -1;
    /* Explorer's icon: the first group (named entries sort first, then IDs). */
    for (unsigned g = 0; g < ng; g++) {
        const unsigned char *grp;
        size_t glen;
        if (res_leaf(&pe, base, rd32(groups + g * 8 + 4), 0, &grp, &glen) < 0) continue;
        if (glen < 6 || rd16(grp + 2) != 1) continue;
        unsigned count = rd16(grp + 4);
        if (6 + (size_t)count * 14 > glen) continue;
        int bw = -1, bb = 0;
        const unsigned char *bdata = NULL;
        size_t blen = 0;
        for (unsigned i = 0; i < count; i++) {
            const unsigned char *e = grp + 6 + i * 14;
            int w = e[0] ? e[0] : 256, bpp = rd16(e + 6);
            unsigned id = rd16(e + 12);
            for (unsigned k = 0; k < ni; k++) {
                if (rd32(icons + k * 8) != id) continue;
                const unsigned char *img;
                size_t ilen;
                if (res_leaf(&pe, base, rd32(icons + k * 8 + 4), 0, &img, &ilen) < 0) break;
                int pw, ph;
                if (png_size(img, ilen, &pw, &ph)) w = pw;
                if (better(w, bpp, bw, bb)) { bw = w; bb = bpp; bdata = img; blen = ilen; }
                break;
            }
        }
        if (bdata && image_to_png(bdata, blen, out, out_len) == 0) return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                         */
/* ------------------------------------------------------------------ */

int lsiw_icon_png_from_data(const unsigned char *d, size_t n, unsigned char **out, size_t *out_len)
{
    int w, h;
    if (png_size(d, n, &w, &h)) return image_to_png(d, n, out, out_len);
    if (n >= 2 && d[0] == 'M' && d[1] == 'Z') return pe_to_png(d, n, out, out_len);
    return ico_to_png(d, n, out, out_len);
}

int lsiw_icon_png(const char *path, unsigned char **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 6) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    int rc = lsiw_icon_png_from_data(m, (size_t)st.st_size, out, out_len);
    munmap(m, (size_t)st.st_size);
    if (rc < 0) errno = ENOENT;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648)                                                    */
/* ------------------------------------------------------------------ */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *lsiw_base64_encode(const unsigned char *d, size_t n)
{
    char *s = malloc((n + 2) / 3 * 4 + 1), *o = s;
    if (!s) return NULL;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        *o++ = b64[v >> 18]; *o++ = b64[(v >> 12) & 63]; *o++ = b64[(v >> 6) & 63]; *o++ = b64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0);
        *o++ = b64[v >> 18]; *o++ = b64[(v >> 12) & 63];
        *o++ = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    *o = 0;
    return s;
}

unsigned char *lsiw_base64_decode(const char *s, size_t *out_len)
{
    size_t n = strlen(s);
    unsigned char *d = malloc(n / 4 * 3 + 3), *o = d;
    if (!d) return NULL;
    uint32_t v = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        const char *p = strchr(b64, *s);
        if (!p || !*s) {
            if (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t') continue;
            free(d);
            return NULL;
        }
        v = v << 6 | (uint32_t)(p - b64);
        bits += 6;
        if (bits >= 8) { bits -= 8; *o++ = (unsigned char)(v >> bits); }
    }
    *out_len = (size_t)(o - d);
    return d;
}
