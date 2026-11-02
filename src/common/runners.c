/* Wine and Proton installations ("runners") and the Steam folders LSIW
 * reads.
 *
 * LSIW's own engine runs most programs; Wine and Proton are the engines a
 * user can pick instead, in any version that is installed:
 *
 *   Wine    the wine (or wine64) command in $PATH ("system"), WineHQ's
 *           /opt/wine-*, builds LSIW downloaded (<data>/runners/wine),
 *           Lutris and Bottles runners, and Wine programs the user added
 *           ([wine] custom).
 *   Proton  every Proton in a Steam library (steamapps/common/Proton*),
 *           custom tools in Steam's compatibilitytools.d (GE-Proton and
 *           friends), builds LSIW downloaded (<data>/runners/proton) and
 *           folders the user added ([proton] custom). The "LSIW Proton"
 *           tools LSIW itself puts into compatibilitytools.d are not
 *           listed: they are wrappers around the others.
 *
 * Each runner has an id, which is what [engine] wine and [engine] proton
 * store. The id is the folder name ("GE-Proton9-20", "Proton 9.0 (Beta)",
 * "wine-staging"), "system" for Wine from $PATH, and the path itself for
 * custom entries; when two sources hold the same folder name, the later
 * one is prefixed with its source ("lutris:wine-ge-8-26").
 *
 * Steam's own files are read with a small parser for its KeyValues text
 * format (.vdf/.acf): quoted strings, nested braces, // comments.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s) { return strdup(s ? s : ""); }

static int is_dir(const char *p) { struct stat st; return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return p && stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int is_exec(const char *p) { return is_file(p) && access(p, X_OK) == 0; }

char *lsiw_read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 2) {
            if (cap > (64u << 20)) break;              /* Steam files are small */
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (buf) buf[len] = 0;
    return buf;
}

/* Directory names, sorted, without . and .. ; NULL-terminated. */
static char **list_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;
    size_t n = 0, cap = 16;
    char **v = calloc(cap, sizeof *v);
    struct dirent *de;
    while (v && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n + 2 > cap) {
            char **nv = realloc(v, (cap *= 2) * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        v[n++] = strdup(de->d_name);
        v[n] = NULL;
    }
    closedir(d);
    if (v) qsort(v, n, sizeof *v, (int (*)(const void *, const void *))strcmp);
    return v;
}

static int cmp_str(const void *a, const void *b)
{
    return strverscmp(*(char *const *)a, *(char *const *)b);
}

static void free_list(char **v)
{
    if (!v) return;
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

/* ------------------------------------------------------------------ */
/* Steam KeyValues                                                      */
/* ------------------------------------------------------------------ */

/* Next token: a quoted or bare string (returned in *tok, malloc'd), '{' or
 * '}'. Returns 0 at the end of the text. */
static int vdf_token(const char **pp, char **tok)
{
    const char *p = *pp;
    *tok = NULL;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    if (!*p) { *pp = p; return 0; }
    if (*p == '{' || *p == '}') { *pp = p + 1; return *p; }
    char *out = malloc(strlen(p) + 1);
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                out[o++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                p++;
            } else out[o++] = *p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}' && *p != '"') out[o++] = *p++;
    }
    out[o] = 0;
    *tok = out;
    *pp = p;
    return 's';
}

/* Every value whose key is `key` (case-insensitive), at any depth, in
 * document order. NULL-terminated; NULL if none. */
char **lsiw_vdf_values(const char *text, const char *key)
{
    if (!text) return NULL;
    const char *p = text;
    char **v = NULL;
    size_t n = 0;
    char *pending = NULL;         /* a key waiting for its value */
    char *tok;
    int t;
    while ((t = vdf_token(&p, &tok))) {
        if (t == 's') {
            if (!pending) { pending = tok; continue; }
            if (!strcasecmp(pending, key)) {
                char **nv = realloc(v, (n + 2) * sizeof *v);
                if (nv) { v = nv; v[n++] = tok; v[n] = NULL; tok = NULL; }
            }
            free(tok);
            free(pending);
            pending = NULL;
        } else {
            free(pending);          /* "key" { ... } : a section */
            pending = NULL;
        }
    }
    free(pending);
    return v;
}

char *lsiw_vdf_value(const char *text, const char *key)
{
    char **v = lsiw_vdf_values(text, key);
    char *r = v && v[0] ? strdup(v[0]) : NULL;
    free_list(v);
    return r;
}

/* ------------------------------------------------------------------ */
/* Steam installations and libraries                                    */
/* ------------------------------------------------------------------ */

static char **add_unique_real(char **v, size_t *n, const char *path)
{
    char *real = realpath(path, NULL);
    if (!real) return v;
    for (size_t i = 0; i < *n; i++)
        if (!strcmp(v[i], real)) { free(real); return v; }
    char **nv = realloc(v, (*n + 2) * sizeof *v);
    if (!nv) { free(real); return v; }
    nv[(*n)++] = real;
    nv[*n] = NULL;
    return nv;
}

char **lsiw_steam_roots(void)
{
    const char *home = lsiw_home_dir();
    const char *ov = lsiw_setting_get("steam", "root", NULL);
    const char *cands[] = { "/.steam/root", "/.steam/steam", "/.local/share/Steam",
                            "/.var/app/com.valvesoftware.Steam/data/Steam",
                            "/snap/steam/common/.local/share/Steam", NULL };
    char **v = NULL;
    size_t n = 0;
    if (ov && *ov) {
        char *p = (ov[0] == '~' && ov[1] == '/') ? lsiw_strdup_printf("%s%s", home, ov + 1) : strdup(ov);
        v = add_unique_real(v, &n, p);
        free(p);
    }
    for (int i = 0; cands[i]; i++) {
        char *p = lsiw_strdup_printf("%s%s", home, cands[i]);
        char *sa = lsiw_strdup_printf("%s/steamapps", p);
        if (is_dir(sa)) v = add_unique_real(v, &n, p);
        free(sa);
        free(p);
    }
    return v;
}

int lsiw_steam_root_sandboxed(const char *root)
{
    return strstr(root, "/.var/app/") != NULL || strstr(root, "/snap/") != NULL;
}

char **lsiw_steam_libraries(void)
{
    char **roots = lsiw_steam_roots();
    char **v = NULL;
    size_t n = 0;
    for (char **r = roots; r && *r; r++) {
        v = add_unique_real(v, &n, *r);
        char *lf = lsiw_strdup_printf("%s/steamapps/libraryfolders.vdf", *r);
        char *text = lsiw_read_text(lf);
        char **paths = lsiw_vdf_values(text, "path");
        for (char **p = paths; p && *p; p++) {
            char *sa = lsiw_strdup_printf("%s/steamapps", *p);
            if (is_dir(sa)) v = add_unique_real(v, &n, *p);
            free(sa);
        }
        free_list(paths);
        free(text);
        free(lf);
    }
    free_list(roots);
    return v;
}

void lsiw_strv_free(char **v) { free_list(v); }

char *lsiw_steam_app_dir(const char *appid)
{
    char **libs = lsiw_steam_libraries();
    char *found = NULL;
    for (char **l = libs; l && *l && !found; l++) {
        char *acf = lsiw_strdup_printf("%s/steamapps/appmanifest_%s.acf", *l, appid);
        char *text = lsiw_read_text(acf);
        char *inst = lsiw_vdf_value(text, "installdir");
        if (inst) {
            char *dir = lsiw_strdup_printf("%s/steamapps/common/%s", *l, inst);
            if (is_dir(dir)) found = dir; else free(dir);
        }
        free(inst);
        free(text);
        free(acf);
    }
    free_list(libs);
    return found;
}

/* ------------------------------------------------------------------ */
/* Runners                                                              */
/* ------------------------------------------------------------------ */

const char *lsiw_runners_dir(void)
{
    static char *dir;
    if (!dir) dir = lsiw_strdup_printf("%s/runners", lsiw_data_dir());
    return dir;
}

struct rlist { struct lsiw_runner *v; size_t n; };

static int seen_path(struct rlist *l, const char *path)
{
    char *real = realpath(path, NULL);
    int seen = 0;
    for (size_t i = 0; i < l->n && !seen; i++) {
        char *o = realpath(l->v[i].path, NULL);
        seen = o && real && !strcmp(o, real);
        free(o);
    }
    free(real);
    return seen;
}

static void add_runner(struct rlist *l, int kind, const char *id, const char *name, const char *path,
                       const char *source, const char *version, int ge)
{
    if (seen_path(l, path)) return;
    char *uid = xstrdup(id);
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].id, uid)) {
            free(uid);
            char *src = xstrdup(source);
            for (char *c = src; *c; c++) *c = (char)tolower((unsigned char)*c);
            char *sp = strchr(src, ' ');
            if (sp) *sp = 0;
            uid = lsiw_strdup_printf("%s:%s", src, id);
            free(src);
            break;
        }
    struct lsiw_runner *nv = realloc(l->v, (l->n + 1) * sizeof *nv);
    if (!nv) { free(uid); return; }
    l->v = nv;
    struct lsiw_runner *r = &l->v[l->n++];
    r->kind = kind;
    r->id = uid;
    r->name = xstrdup(name);
    r->path = xstrdup(path);
    r->source = xstrdup(source);
    r->version = version ? strdup(version) : NULL;
    r->ge = ge;
    r->foreign = lsiw_runner_foreign(path);
}

/* Wine builds keep their programs in <dir>/bin. */
static void scan_wine_dir(struct rlist *l, const char *parent, const char *idprefix, const char *source)
{
    char **names = list_dir(parent);
    if (!names) return;
    size_t cnt = 0;
    while (names[cnt]) cnt++;
    qsort(names, cnt, sizeof *names, cmp_str);
    for (char **nm = names; *nm; nm++) {
        char *bin = lsiw_strdup_printf("%s/%s/bin/wine", parent, *nm);
        if (!is_exec(bin)) { free(bin); bin = lsiw_strdup_printf("%s/%s/bin/wine64", parent, *nm); }
        if (is_exec(bin)) {
            char *id = lsiw_strdup_printf("%s%s", idprefix, *nm);
            char *name = lsiw_strdup_printf("%s (%s)", *nm, source);
            add_runner(l, LSIW_RUNNER_WINE, id, name, bin, source, *nm, 0);
            free(id);
            free(name);
        }
        free(bin);
    }
    free_list(names);
}

static char *which(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path), *save = NULL;
    char *found = NULL;
    for (char *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
        char *p = lsiw_strdup_printf("%s/%s", *d ? d : ".", name);
        if (is_exec(p)) found = p; else free(p);
    }
    free(copy);
    return found;
}

static char **split_list(const char *s)
{
    char **v = NULL;
    size_t n = 0;
    if (!s || !*s) return NULL;
    char *copy = strdup(s), *save = NULL;
    for (char *t = strtok_r(copy, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        while (isspace((unsigned char)*t)) t++;
        size_t len = strlen(t);
        while (len && isspace((unsigned char)t[len - 1])) t[--len] = 0;
        if (!*t) continue;
        char **nv = realloc(v, (n + 2) * sizeof *v);
        if (!nv) break;
        v = nv;
        v[n++] = (t[0] == '~' && t[1] == '/') ? lsiw_strdup_printf("%s%s", lsiw_home_dir(), t + 1) : strdup(t);
        v[n] = NULL;
    }
    free(copy);
    return v;
}

/* Where each build is published. GE-Proton has its own code in proton.c
 * (it names archives by processor and has a release API it relies on);
 * the others are found from their tags (`git ls-remote`, no API limit) and
 * the archive names their projects use. */
static const struct lsiw_proton_build g_builds[] = {
    { "ge", "GE-Proton", "GloriousEggroll's Proton with extra game fixes and media codecs",
      "GloriousEggroll/proton-ge-custom", "GE-Proton", NULL, NULL, NULL, NULL, NULL },
    { "cachyos", "Proton-CachyOS", "the CachyOS distribution's Proton with performance patches",
      "CachyOS/proton-cachyos", "cachyos-", "-slr", NULL,
      "proton-$tag-x86_64.tar.xz", "proton-$tag-x86_64.sha512sum", NULL },
    { "dwproton", "DW-Proton", "Dawn Winery's Proton with fixes for various games",
      "dawn-winery/dwproton-mirror", "dwproton-", NULL, "wip",
      "$tag-x86_64.tar.xz", "$tag-x86_64.sha512sum", NULL },
    { "rtsp", "Proton-GE RTSP", "GE-Proton with RTSP codec patches for VRChat",
      "SpookySkeletons/proton-rtsp", "proton-rtsp-|GE-Proton", NULL, "hotfix",
      "$tag.tar.gz", NULL, NULL },
    { "tkg", "Proton-Tkg", "Proton built from Wine-tkg (the latest automatic build)",
      "Frogging-Family/wine-tkg-git", NULL, NULL, NULL, NULL, NULL,
      "https://nightly.link/Frogging-Family/wine-tkg-git/workflows/proton-valvexbe-sniper.yml/master/proton-tkg-build.zip" },
    { "em", "Proton-EM", "Etaash Mathamsetty's Proton with FSR 4 support and Wine Wayland work",
      "BananaWorks07/Proton", "EM-", NULL, "alpha|beta|rc|TEST",
      "proton-$tag.tar.xz", "proton-$tag.sha512sum", NULL },
    { "wineland", "Proton-Wineland", "Proton-CachyOS with native Wayland support",
      "nanomatters/proton-cachyos", "cachyos-wineland-", "-slr", "alpha|beta|rc",
      "proton-$tag-x86_64.tar.xz", "proton-$tag-x86_64.sha512sum", NULL },
};

const struct lsiw_proton_build *lsiw_proton_builds(size_t *count)
{
    *count = sizeof g_builds / sizeof g_builds[0];
    return g_builds;
}

const struct lsiw_proton_build *lsiw_proton_build_find(const char *id)
{
    for (size_t i = 0; i < sizeof g_builds / sizeof g_builds[0]; i++) {
        const struct lsiw_proton_build *b = &g_builds[i];
        /* "cachyos", "Proton-CachyOS", "proton-cachyos" */
        if (!strcasecmp(id, b->id) || !strcasecmp(id, b->title) ||
            (!strncasecmp(id, "proton-", 7) && !strcasecmp(id + 7, b->id)))
            return b;
    }
    if (!strcasecmp(id, "ge-proton") || !strcasecmp(id, "proton") || !strcasecmp(id, "proton-ge"))
        return &g_builds[0];
    return NULL;
}

char *lsiw_runner_wine_dir_name(const char *name)
{
    size_t n = strlen(name);
    if (n > 6 && !strcmp(name + n - 6, "-amd64"))
        return lsiw_strdup_printf("%.*s-x86_64", (int)(n - 6), name);
    if (n > 12 && !strcmp(name + n - 12, "-amd64-wow64"))
        return lsiw_strdup_printf("%.*s-x86_64-wow64", (int)(n - 12), name);
    return xstrdup(name);
}

/* Builds LSIW downloaded before 1.2.1 kept Kron4ek's "-amd64" names. */
static void rename_amd64_builds(const char *parent)
{
    char **names = list_dir(parent);
    if (!names) return;
    for (char **nm = names; *nm; nm++) {
        char *to = lsiw_runner_wine_dir_name(*nm);
        if (strcmp(to, *nm)) {
            char *a = lsiw_strdup_printf("%s/%s", parent, *nm);
            char *b = lsiw_strdup_printf("%s/%s", parent, to);
            if (!is_dir(b)) (void)rename(a, b);
            free(a);
            free(b);
        }
        free(to);
    }
    free_list(names);
}

static void wine_runners(struct rlist *l)
{
    char *sys = which("wine");
    if (!sys) sys = which("wine64");
    if (sys) add_runner(l, LSIW_RUNNER_WINE, "system", "Wine from $PATH", sys, "system", NULL, 0);
    free(sys);

    /* WineHQ packages: /opt/wine-stable, /opt/wine-staging, /opt/wine-devel. */
    char **opt = list_dir("/opt");
    for (char **nm = opt; nm && *nm; nm++) {
        if (strncasecmp(*nm, "wine", 4)) continue;
        char *bin = lsiw_strdup_printf("/opt/%s/bin/wine", *nm);
        if (!is_exec(bin)) { free(bin); bin = lsiw_strdup_printf("/opt/%s/bin/wine64", *nm); }
        if (is_exec(bin)) {
            char *name = lsiw_strdup_printf("%s (WineHQ)", *nm);
            add_runner(l, LSIW_RUNNER_WINE, *nm, name, bin, "WineHQ", *nm, 0);
            free(name);
        }
        free(bin);
    }
    free_list(opt);

    char *d = lsiw_strdup_printf("%s/wine", lsiw_runners_dir());
    rename_amd64_builds(d);
    scan_wine_dir(l, d, "", "LSIW");
    free(d);
    const char *home = lsiw_home_dir();
    d = lsiw_strdup_printf("%s/.local/share/lutris/runners/wine", home);
    scan_wine_dir(l, d, "", "Lutris");
    free(d);
    d = lsiw_strdup_printf("%s/.local/share/bottles/runners", home);
    scan_wine_dir(l, d, "", "Bottles");
    free(d);
    d = lsiw_strdup_printf("%s/.var/app/com.usebottles.bottles/data/bottles/runners", home);
    scan_wine_dir(l, d, "", "Bottles");
    free(d);

    char **custom = split_list(lsiw_setting_get("wine", "custom", NULL));
    for (char **c = custom; c && *c; c++) {
        char *bin = is_dir(*c) ? lsiw_strdup_printf("%s/bin/wine", *c) : strdup(*c);
        if (is_exec(bin)) add_runner(l, LSIW_RUNNER_WINE, *c, *c, bin, "custom", NULL, 0);
        free(bin);
    }
    free_list(custom);
}

/* "1715694155 proton-9.0-2" -> "9.0-2"; GE writes "GE-Proton9-20". */
static char *proton_version(const char *dir)
{
    char *vf = lsiw_strdup_printf("%s/version", dir);
    char *text = lsiw_read_text(vf);
    free(vf);
    if (!text) return NULL;
    char *nl = strpbrk(text, "\r\n");
    if (nl) *nl = 0;
    char *v = strrchr(text, ' ');
    v = v ? v + 1 : text;
    if (!strncmp(v, "proton-", 7)) v += 7;
    char *r = *v ? strdup(v) : NULL;
    free(text);
    return r;
}

static int is_ge(const char *name)
{
    return !strncasecmp(name, "GE-Proton", 9) || strcasestr(name, "-GE") != NULL ||
           !strncasecmp(name, "Proton-GE", 9);
}

/* A Proton folder: has the `proton` launcher. LSIW's own Steam tools carry
 * lsiw-tool.conf and are skipped. */
static void scan_proton_dir(struct rlist *l, const char *parent, int only_proton_names, const char *source)
{
    char **names = list_dir(parent);
    if (!names) return;
    size_t cnt = 0;
    while (names[cnt]) cnt++;
    qsort(names, cnt, sizeof *names, cmp_str);
    for (char **nm = names; *nm; nm++) {
        if (only_proton_names && strncasecmp(*nm, "Proton", 6)) continue;
        char *dir = lsiw_strdup_printf("%s/%s", parent, *nm);
        char *launcher = lsiw_strdup_printf("%s/proton", dir);
        char *mine = lsiw_strdup_printf("%s/lsiw-tool.conf", dir);
        if (is_file(launcher) && !is_file(mine)) {
            char *ver = proton_version(dir);
            add_runner(l, LSIW_RUNNER_PROTON, *nm, *nm, dir, source, ver, is_ge(*nm));
            free(ver);
        }
        free(mine);
        free(launcher);
        free(dir);
    }
    free_list(names);
}

static void proton_runners(struct rlist *l)
{
    char **libs = lsiw_steam_libraries();
    for (char **lib = libs; lib && *lib; lib++) {
        char *c = lsiw_strdup_printf("%s/steamapps/common", *lib);
        scan_proton_dir(l, c, 1, "Steam");
        free(c);
    }
    free_list(libs);
    char **roots = lsiw_steam_roots();
    for (char **r = roots; r && *r; r++) {
        char *c = lsiw_strdup_printf("%s/compatibilitytools.d", *r);
        scan_proton_dir(l, c, 0, "Steam compatibility tool");
        free(c);
    }
    free_list(roots);
    scan_proton_dir(l, "/usr/share/steam/compatibilitytools.d", 0, "system compatibility tool");
    scan_proton_dir(l, "/usr/local/share/steam/compatibilitytools.d", 0, "system compatibility tool");
    char *d = lsiw_strdup_printf("%s/proton", lsiw_runners_dir());
    scan_proton_dir(l, d, 0, "LSIW");
    free(d);

    char **custom = split_list(lsiw_setting_get("proton", "custom", NULL));
    for (char **c = custom; c && *c; c++) {
        char *launcher = lsiw_strdup_printf("%s/proton", *c);
        if (is_file(launcher)) {
            const char *base = strrchr(*c, '/');
            base = base && base[1] ? base + 1 : *c;
            char *ver = proton_version(*c);
            add_runner(l, LSIW_RUNNER_PROTON, *c, base, *c, "custom", ver, is_ge(base));
            free(ver);
        }
        free(launcher);
    }
    free_list(custom);
}

size_t lsiw_runners(int kind, struct lsiw_runner **out)
{
    struct rlist l = { NULL, 0 };
    if (kind == LSIW_RUNNER_WINE) wine_runners(&l);
    else proton_runners(&l);
    *out = l.v;
    return l.n;
}

static void runner_clear(struct lsiw_runner *r)
{
    free(r->id); free(r->name); free(r->path); free(r->source); free(r->version);
}

void lsiw_runners_free(struct lsiw_runner *v, size_t n)
{
    for (size_t i = 0; i < n; i++) runner_clear(&v[i]);
    free(v);
}

void lsiw_runner_free(struct lsiw_runner *r)
{
    if (!r) return;
    runner_clear(r);
    free(r);
}

int lsiw_proton_official(const char *setting, char **appid, char **display)
{
    if (appid) *appid = NULL;
    if (display) *display = NULL;
    if (!setting || strncmp(setting, "steam:", 6) || !setting[6]) return 0;
    struct lsiw_steam_tool *v;
    size_t n = lsiw_steam_official_tools(&v);
    const struct lsiw_steam_tool *t = lsiw_steam_tool_by_alias(v, n, setting + 6);
    if (t) {
        if (appid) *appid = xstrdup(t->appid);
        if (display) *display = xstrdup(t->display);
    }
    lsiw_steam_tools_free(v, n);
    return t != NULL;
}

/* "steam:KEY": that official Proton where Steam installed it, or NULL. */
static struct lsiw_runner *pick_official(const char *setting)
{
    char *appid, *display;
    if (!lsiw_proton_official(setting, &appid, &display)) return NULL;
    char *dir = lsiw_steam_app_dir(appid);
    char *launcher = dir ? lsiw_strdup_printf("%s/proton", dir) : NULL;
    struct lsiw_runner *r = NULL;
    if (launcher && is_file(launcher)) {
        r = calloc(1, sizeof *r);
        r->kind = LSIW_RUNNER_PROTON;
        r->id = xstrdup(setting);
        r->name = display;
        display = NULL;
        r->path = dir;
        dir = NULL;
        r->source = xstrdup("Steam");
        r->version = proton_version(r->path);
        r->foreign = lsiw_runner_foreign(r->path);
    }
    free(launcher);
    free(dir);
    free(display);
    free(appid);
    return r;
}

struct lsiw_runner *lsiw_runner_pick(int kind, const char *setting)
{
    if (kind == LSIW_RUNNER_PROTON && setting && !strncmp(setting, "steam:", 6)) return pick_official(setting);
    struct lsiw_runner *v;
    size_t n = lsiw_runners(kind, &v);
    struct lsiw_runner *pick = NULL;
    int want_default = !setting || !*setting || !strcasecmp(setting, "auto");
    /* A setting naming a Wine build by its old "-amd64" folder name. */
    char *renamed = !want_default && kind == LSIW_RUNNER_WINE ? lsiw_runner_wine_dir_name(setting) : NULL;
    for (size_t i = 0; i < n && !pick; i++) {
        struct lsiw_runner *r = &v[i];
        if (want_default && r->foreign) continue;
        if (want_default || !strcmp(r->id, setting) || !strcmp(r->path, setting) ||
            !strcasecmp(r->name, setting) || (renamed && !strcmp(r->id, renamed))) {
            pick = malloc(sizeof *pick);
            *pick = *r;
            memset(r, 0, sizeof *r);
        }
    }
    /* Proton: the newest official "Proton X.Y" is a better default than
     * the first folder name in sort order. */
    if (want_default && kind == LSIW_RUNNER_PROTON) {
        struct lsiw_runner *best = NULL;
        for (size_t i = 0; i < n; i++) {
            struct lsiw_runner *r = &v[i];
            if (!r->id || r->foreign || strcmp(r->source, "Steam") || strncmp(r->name, "Proton ", 7) ||
                !isdigit((unsigned char)r->name[7]))
                continue;
            if (!best || strverscmp(r->name, best->name) > 0) best = r;
        }
        if (best && pick && strcmp(best->path, pick->path)) {
            runner_clear(pick);
            *pick = *best;
            memset(best, 0, sizeof *best);
        }
    }
    if (!pick && !want_default && kind == LSIW_RUNNER_PROTON) {
        /* A folder that is not listed anywhere. */
        char *launcher = lsiw_strdup_printf("%s/proton", setting);
        if (is_file(launcher)) {
            pick = calloc(1, sizeof *pick);
            pick->kind = kind;
            pick->id = strdup(setting);
            pick->name = strdup(setting);
            pick->path = strdup(setting);
            pick->source = strdup("custom");
            pick->version = proton_version(setting);
            pick->foreign = lsiw_runner_foreign(setting);
        }
        free(launcher);
    }
    lsiw_runners_free(v, n);
    free(renamed);
    return pick;
}

/* The name Steam shows for LSIW's tool around a Proton runner. */
char *lsiw_steam_tool_display_name(const struct lsiw_runner *r)
{
    return lsiw_strdup_printf("LSIW %s", r->name);
}
