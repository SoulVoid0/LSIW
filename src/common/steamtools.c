/* The Proton versions Valve publishes through Steam, and the processor a
 * Wine or Proton build is made for.
 *
 * Steam does not keep a compatibilitytool.vdf for its own tools: their
 * declarations come with Steam's app information, cached in
 * <Steam>/appcache/appinfo.vdf. App 891390 ("Steam Play 2.0 Manifests")
 * and a few other apps (the ARM64 tools, for one) carry
 *
 *     appinfo > extended > compat_tools > <name> >
 *         appid, display_name, aliases, from_oslist, to_oslist,
 *         unlisted, legacy, require_tool_appid, ...
 *
 * which is the list Steam shows under "Select compatibility tool"
 * ("Proton Experimental", "Proton 11.0-2", ...). "Proton Stable" in that
 * list is the tool with the alias "proton-stable".
 *
 * appinfo.vdf is a binary file: a header, then one record per app (app ID,
 * size, a 60-byte record header, binary KeyValues), then, in version 29,
 * a table of the key names the KeyValues refer to by index. The layout is
 * the one Steam has used since 2023 (version 28: magic 0x07564428; 29:
 * 0x07564429). When the file is missing or has a format LSIW does not
 * know, the list below (Valve's tools as of October 2026) is used.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define STEAMPLAY_MANIFESTS_APPID 891390u

/* ------------------------------------------------------------------ */
/* Processor of a build                                                 */
/* ------------------------------------------------------------------ */

/* ELF e_machine values */
#define EM_X86_64  62
#define EM_AARCH64 183

static int host_machine(void)
{
    struct utsname u;
    if (uname(&u) == 0 && (!strcmp(u.machine, "aarch64") || !strcmp(u.machine, "arm64")))
        return EM_AARCH64;
    return EM_X86_64;
}

/* e_machine of an ELF file, 0 if it is not one. */
static int elf_machine(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[20];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < sizeof h || memcmp(h, "\177ELF", 4)) return 0;
    return h[5] == 2 ? (h[18] << 8 | h[19]) : (h[18] | h[19] << 8);
}

int lsiw_runner_foreign(const char *path)
{
    int host = host_machine();
    struct stat st;
    if (path && stat(path, &st) == 0) {
        /* The Wine loader of a Proton build, or the wine program itself. */
        static const char *const loaders[] = {
            "files/bin/wine64", "files/bin/wine", "dist/bin/wine64", "dist/bin/wine", NULL
        };
        if (S_ISDIR(st.st_mode)) {
            for (int i = 0; loaders[i]; i++) {
                char *p = lsiw_strdup_printf("%s/%s", path, loaders[i]);
                int m = elf_machine(p);
                free(p);
                if (m) return m != host;
            }
        } else {
            int m = elf_machine(path);
            if (m) return m != host;
        }
    }
    /* Not unpacked or a script: the name says it ("GE-Proton11-11-aarch64"). */
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path ? path : "";
    int arm = strcasestr(base, "aarch64") || strcasestr(base, "arm64");
    return host == EM_AARCH64 ? (strcasestr(base, "x86_64") != NULL && !arm) : arm;
}

/* ------------------------------------------------------------------ */
/* Binary KeyValues                                                     */
/* ------------------------------------------------------------------ */

struct kv {
    char *key;
    char *str;           /* strings and numbers, as text */
    struct kv *child;    /* first child of a subtree     */
    struct kv *next;
};

static void kv_free(struct kv *k)
{
    while (k) {
        struct kv *n = k->next;
        kv_free(k->child);
        free(k->key);
        free(k->str);
        free(k);
        k = n;
    }
}

static struct kv *kv_get(struct kv *k, const char *key)
{
    for (k = k ? k->child : NULL; k; k = k->next)
        if (k->key && !strcasecmp(k->key, key)) return k;
    return NULL;
}

static const char *kv_str(struct kv *k, const char *key)
{
    struct kv *c = kv_get(k, key);
    return c && c->str ? c->str : NULL;
}

struct reader {
    const unsigned char *p, *end;
    char **keys;         /* version 29: key names by index */
    uint32_t nkeys;
    int bad;
};

static uint32_t rd32(struct reader *r)
{
    if (r->end - r->p < 4) { r->bad = 1; r->p = r->end; return 0; }
    uint32_t v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
    r->p += 4;
    return v;
}

static uint64_t rd64(struct reader *r)
{
    uint64_t lo = rd32(r);
    return lo | (uint64_t)rd32(r) << 32;
}

static char *rdstr(struct reader *r)
{
    const unsigned char *z = memchr(r->p, 0, (size_t)(r->end - r->p));
    if (!z) { r->bad = 1; r->p = r->end; return NULL; }
    char *s = strndup((const char *)r->p, (size_t)(z - r->p));
    r->p = z + 1;
    return s;
}

static char *rdkey(struct reader *r)
{
    if (!r->keys) return rdstr(r);
    uint32_t i = rd32(r);
    if (i >= r->nkeys) { r->bad = 1; return NULL; }
    return strdup(r->keys[i]);
}

/* The children of a subtree, up to its end marker. */
static struct kv *kv_parse(struct reader *r, int depth)
{
    struct kv *first = NULL, **tail = &first;
    while (!r->bad && r->p < r->end) {
        unsigned char type = *r->p++;
        if (type == 0x08 || type == 0x0b) return first;       /* end of subtree */
        if (depth > 32) { r->bad = 1; break; }
        struct kv *k = calloc(1, sizeof *k);
        if (!k) { r->bad = 1; break; }
        *tail = k;
        tail = &k->next;
        k->key = rdkey(r);
        char buf[32];
        switch (type) {
        case 0x00: k->child = kv_parse(r, depth + 1); break;
        case 0x01: k->str = rdstr(r); break;
        case 0x02: case 0x04: case 0x06:
            snprintf(buf, sizeof buf, "%d", (int32_t)rd32(r)); k->str = strdup(buf); break;
        case 0x03: { uint32_t u = rd32(r); float f; memcpy(&f, &u, 4);
                     snprintf(buf, sizeof buf, "%g", f); k->str = strdup(buf); break; }
        case 0x07: snprintf(buf, sizeof buf, "%llu", (unsigned long long)rd64(r)); k->str = strdup(buf); break;
        case 0x0a: snprintf(buf, sizeof buf, "%lld", (long long)rd64(r)); k->str = strdup(buf); break;
        case 0x05: {                       /* UTF-16 string: skipped */
            while (r->end - r->p >= 2 && (r->p[0] || r->p[1])) r->p += 2;
            r->p += 2;
            if (r->p > r->end) { r->p = r->end; r->bad = 1; }
            break;
        }
        default: r->bad = 1; break;
        }
    }
    return first;
}

/* ------------------------------------------------------------------ */
/* appinfo.vdf                                                          */
/* ------------------------------------------------------------------ */

struct tools { struct lsiw_steam_tool *v; size_t n; };

static int host_is_arm(void) { return host_machine() == EM_AARCH64; }

/* Valve names its ARM64 tools "proton_11-arm64", "proton-experimental-arm64". */
static int arm_name(const char *key)
{
    return strstr(key, "arm64") || strstr(key, "aarch64");
}

static void tools_add(struct tools *t, const char *key, const char *appid, const char *display,
                      const char *aliases, int legacy)
{
    for (size_t i = 0; i < t->n; i++)
        if (!strcmp(t->v[i].key, key)) return;          /* first manifest wins */
    struct lsiw_steam_tool *nv = realloc(t->v, (t->n + 1) * sizeof *nv);
    if (!nv) return;
    t->v = nv;
    struct lsiw_steam_tool *e = &t->v[t->n++];
    e->key = strdup(key);
    e->appid = strdup(appid);
    e->display = strdup(display && *display ? display : key);
    e->aliases = strdup(aliases ? aliases : "");
    e->legacy = legacy;
}

static void take_compat_tools(struct tools *t, struct kv *app)
{
    struct kv *ct = kv_get(kv_get(kv_get(app, "appinfo"), "extended"), "compat_tools");
    if (!ct) ct = kv_get(kv_get(app, "extended"), "compat_tools");
    for (struct kv *e = ct ? ct->child : NULL; e; e = e->next) {
        if (!e->key || !e->child) continue;
        const char *appid = kv_str(e, "appid");
        const char *from = kv_str(e, "from_oslist"), *to = kv_str(e, "to_oslist");
        const char *unlisted = kv_str(e, "unlisted");
        const char *aliases = kv_str(e, "aliases");
        if (!appid || !from || !to || !strstr(from, "windows") || !strstr(to, "linux")) continue;
        if (unlisted && strcmp(unlisted, "0")) continue;
        if (arm_name(e->key) != host_is_arm()) continue;
        const char *legacy = kv_str(e, "legacy");
        tools_add(t, e->key, appid, kv_str(e, "display_name"), aliases, legacy && strcmp(legacy, "0"));
    }
}

static int read_appinfo(const char *path, struct tools *t)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return -1; }
    long size = ftell(f);
    rewind(f);
    if (size < 16 || size > (1L << 31)) { fclose(f); return -1; }
    unsigned char *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); fclose(f); return -1; }
    fclose(f);

    struct reader r = { data, data + size, NULL, 0, 0 };
    uint32_t magic = rd32(&r);
    rd32(&r);                                   /* universe */
    int v29 = magic == 0x07564429;
    if (!v29 && magic != 0x07564428) { free(data); return -1; }
    if (v29) {
        uint64_t off = rd64(&r);
        if (off + 4 > (uint64_t)size) { free(data); return -1; }
        struct reader kt = { data + off, data + size, NULL, 0, 0 };
        uint32_t n = rd32(&kt);
        if (n > (uint32_t)size) { free(data); return -1; }
        r.keys = calloc(n ? n : 1, sizeof *r.keys);
        for (uint32_t i = 0; i < n && !kt.bad; i++) r.keys[i] = rdstr(&kt);
        r.nkeys = kt.bad ? 0 : n;
        r.end = data + off;
    }
    static const char needle[] = "compat_tools";
    uint32_t needle_idx = UINT32_MAX;
    for (uint32_t i = 0; i < r.nkeys; i++)
        if (r.keys[i] && !strcmp(r.keys[i], needle)) { needle_idx = i; break; }

    int found = 0;
    while (!r.bad && r.end - r.p >= 8) {
        uint32_t appid = rd32(&r);
        if (!appid) break;
        uint32_t len = rd32(&r);
        if (len < 60 || len > (uint32_t)(r.end - r.p)) break;
        const unsigned char *blob = r.p + 60, *next = r.p + len;
        size_t blen = (size_t)(next - blob);
        /* Only apps that declare tools are worth parsing. */
        int maybe;
        if (v29) {
            unsigned char idx[4] = { (unsigned char)needle_idx, (unsigned char)(needle_idx >> 8),
                                     (unsigned char)(needle_idx >> 16), (unsigned char)(needle_idx >> 24) };
            maybe = needle_idx != UINT32_MAX && memmem(blob, blen, idx, 4);
        } else maybe = memmem(blob, blen, needle, sizeof needle) != NULL;
        if (appid == STEAMPLAY_MANIFESTS_APPID || maybe) {
            struct reader br = { blob, next, r.keys, r.nkeys, 0 };
            struct kv root = { 0 };
            root.child = kv_parse(&br, 0);
            if (!br.bad) {
                size_t before = t->n;
                take_compat_tools(t, &root);
                found += t->n > before;
            }
            kv_free(root.child);
        }
        r.p = next;
    }
    for (uint32_t i = 0; r.keys && i < r.nkeys; i++) free(r.keys[i]);
    free(r.keys);
    free(data);
    return found ? 0 : -1;
}

/* Valve's tools as of October 2026 (app 891390), for when Steam's cache
 * cannot be read. */
static const struct { const char *key, *appid, *display, *aliases; int legacy; } builtin[] = {
    { "proton_experimental", "1493710", "Proton Experimental", "proton-experimental", 0 },
    { "proton_11", "4628710", "Proton 11.0-2", "proton-stable,proton-11.0-2pin", 0 },
    { "proton_10", "3658110", "Proton 10.0-4", "proton-10.0-4pin", 0 },
    { "proton_9", "2805730", "Proton 9.0-4", "proton-9.0-4pin", 0 },
    { "proton_8", "2348590", "Proton 8.0-5", "proton-8.0-5pin", 0 },
    { "proton_7", "1887720", "Proton 7.0-6", "proton-7.0-6pin", 0 },
    { "proton_63", "1580130", "Proton 6.3-8", "proton-6.3-8", 1 },
    { "proton_513", "1420170", "Proton 5.13-6", "proton-5.13-6", 1 },
    { "proton_5", "1245040", "Proton 5.0-10", "proton-5.0-10", 1 },
    { "proton_411", "1113280", "Proton 4.11-13", "proton-4.11-13", 1 },
    { "proton_42", "1054830", "Proton 4.2-9", "proton-4.2-9", 1 },
    { "proton_316", "961940", "Proton 3.16-9", "proton_316_beta,proton-3.16-9", 1 },
    { "proton_37", "858280", "Proton 3.7-8", "proton_37_beta,proton-3.7-8", 1 },
    { "proton_hotfix", "2180100", "Proton Hotfix", "proton-hotfix", 1 },
};

/* Valve's tools that Steam's tool list carries only part of the time:
 * Proton Next exists while a new version is being tested. When the app
 * cache does not list one but Steam has it installed, it still gets a twin. */
static const struct { const char *key, *appid, *display, *aliases; } occasional[] = {
    { "proton_next", "2230260", "Proton Next", "proton-next" },
};

size_t lsiw_steam_official_tools(struct lsiw_steam_tool **out)
{
    struct tools t = { NULL, 0 };
    char **roots = lsiw_steam_roots();
    for (char **r = roots; r && *r && !t.n; r++) {
        char *p = lsiw_strdup_printf("%s/appcache/appinfo.vdf", *r);
        read_appinfo(p, &t);
        free(p);
    }
    lsiw_strv_free(roots);
    if (!t.n && !host_is_arm())
        for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
            tools_add(&t, builtin[i].key, builtin[i].appid, builtin[i].display, builtin[i].aliases,
                      builtin[i].legacy);
    /* Steam's tool list may leave out the oldest versions (5.13 back to
     * 3.7); they are still Steam apps, so they get a twin too. */
    if (!host_is_arm())
        for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++) {
            int listed = 0;
            for (size_t k = 0; k < t.n && !listed; k++) listed = !strcmp(t.v[k].appid, builtin[i].appid);
            if (builtin[i].legacy && !listed)
                tools_add(&t, builtin[i].key, builtin[i].appid, builtin[i].display, builtin[i].aliases, 1);
        }
    for (size_t i = 0; i < sizeof occasional / sizeof occasional[0]; i++) {
        int listed = 0;
        for (size_t k = 0; k < t.n && !listed; k++) listed = !strcmp(t.v[k].appid, occasional[i].appid);
        char *dir = listed ? NULL : lsiw_steam_app_dir(occasional[i].appid);
        if (dir) tools_add(&t, occasional[i].key, occasional[i].appid, occasional[i].display,
                           occasional[i].aliases, 0);
        free(dir);
    }
    *out = t.v;
    return t.n;
}

void lsiw_steam_tools_free(struct lsiw_steam_tool *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(v[i].key);
        free(v[i].appid);
        free(v[i].display);
        free(v[i].aliases);
    }
    free(v);
}

const struct lsiw_steam_tool *lsiw_steam_tool_by_alias(const struct lsiw_steam_tool *v, size_t n,
                                                        const char *alias)
{
    size_t al = strlen(alias);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(v[i].key, alias)) return &v[i];
        for (const char *p = v[i].aliases; (p = strstr(p, alias)); p += al)
            if ((p == v[i].aliases || p[-1] == ',') && (p[al] == 0 || p[al] == ',')) return &v[i];
    }
    return NULL;
}
