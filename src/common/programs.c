/* The program library and the engine fallback chain.
 *
 * Each Windows program or game the user adds (LSIW Control's Programs page,
 * `lsiw programs add`) is a [program:ID] section of the user's config:
 *
 *   [program:notepad-plus-plus]
 *   name = Notepad++
 *   exe = /home/me/Program Files/Notepad++/notepad++.exe
 *   engine = wine:wine-staging-11.0
 *   prefix = shared            ; shared (default), own, or a folder
 *   args = -multiInst
 *   dxvk = on                  ; default, on, off
 *   vkd3d = default
 *   mangohud = false
 *   debug = false
 *   env:DXVK_HUD = fps         ; one key per environment variable
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SECTION "program:"

static char *dup0(const char *s) { return strdup(s ? s : ""); }

static int tristate(const char *v)
{
    if (!v || !*v || !strcasecmp(v, "default")) return -1;
    return !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1");
}

static int truthy(const char *v)
{
    return v && (!strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcmp(v, "1"));
}

static void load(struct lsiw_program *p, const char *section)
{
    p->id = strdup(section + strlen(SECTION));
    p->name = dup0(lsiw_setting_get(section, "name", NULL));
    p->exe = dup0(lsiw_setting_get(section, "exe", NULL));
    p->args = dup0(lsiw_setting_get(section, "args", NULL));
    p->workdir = dup0(lsiw_setting_get(section, "workdir", NULL));
    p->engine = dup0(lsiw_setting_get(section, "engine", NULL));
    p->prefix = dup0(lsiw_setting_get(section, "prefix", NULL));
    p->icon = dup0(lsiw_setting_get(section, "icon", NULL));
    p->dxvk = tristate(lsiw_setting_get(section, "dxvk", NULL));
    p->vkd3d = tristate(lsiw_setting_get(section, "vkd3d", NULL));
    p->mangohud = truthy(lsiw_setting_get(section, "mangohud", NULL));
    p->debug = truthy(lsiw_setting_get(section, "debug", NULL));
    size_t len = 0;
    char *env = calloc(1, 1);
    char **keys = lsiw_setting_keys(section);
    for (char **k = keys; k && *k; k++) {
        if (strncasecmp(*k, "env:", 4) || !(*k)[4]) continue;
        const char *v = lsiw_setting_get(section, *k, "");
        char *line = lsiw_strdup_printf("%s=%s\n", *k + 4, v);
        size_t ll = strlen(line);
        env = realloc(env, len + ll + 1);
        memcpy(env + len, line, ll + 1);
        len += ll;
        free(line);
    }
    lsiw_strv_free(keys);
    p->env = env;
    if (!*p->name) {
        free(p->name);
        p->name = strdup(p->id);
    }
}

void lsiw_program_free(struct lsiw_program *p)
{
    if (!p) return;
    free(p->id); free(p->name); free(p->exe); free(p->args); free(p->workdir);
    free(p->engine); free(p->prefix); free(p->env); free(p->icon);
    free(p);
}

static int by_name(const void *a, const void *b)
{
    const struct lsiw_program *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->id, y->id);
}

size_t lsiw_programs(struct lsiw_program **out)
{
    char **secs = lsiw_setting_sections(SECTION);
    size_t n = 0;
    for (char **s = secs; s && *s; s++) n++;
    struct lsiw_program *v = calloc(n ? n : 1, sizeof *v);
    for (size_t i = 0; i < n; i++) load(&v[i], secs[i]);
    lsiw_strv_free(secs);
    qsort(v, n, sizeof *v, by_name);
    *out = v;
    return n;
}

void lsiw_programs_free(struct lsiw_program *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        struct lsiw_program *p = &v[i];
        free(p->id); free(p->name); free(p->exe); free(p->args); free(p->workdir);
        free(p->engine); free(p->prefix); free(p->env); free(p->icon);
    }
    free(v);
}

static int exists(const char *id)
{
    char *sec = lsiw_strdup_printf(SECTION "%s", id);
    char **keys = lsiw_setting_keys(sec);
    int r = keys != NULL;
    lsiw_strv_free(keys);
    free(sec);
    return r;
}

struct lsiw_program *lsiw_program_get(const char *id)
{
    if (!id || !*id || !exists(id)) return NULL;
    struct lsiw_program *p = calloc(1, sizeof *p);
    char *sec = lsiw_strdup_printf(SECTION "%s", id);
    load(p, sec);
    free(sec);
    return p;
}

/* "Notepad++ 8" -> "notepad-8"; unique among the library. */
static char *new_id(const char *name)
{
    size_t l = strlen(name);
    char *base = malloc(l + 8), *o = base;
    int dash = 0;
    for (const char *c = name; *c; c++) {
        if (isalnum((unsigned char)*c)) { *o++ = (char)tolower((unsigned char)*c); dash = 0; }
        else if (o > base && !dash) { *o++ = '-'; dash = 1; }
    }
    while (o > base && o[-1] == '-') o--;
    *o = 0;
    if (!*base) strcpy(base, "program");
    char *id = strdup(base);
    for (int i = 2; exists(id); i++) {
        free(id);
        id = lsiw_strdup_printf("%s-%d", base, i);
    }
    free(base);
    return id;
}

struct lsiw_program *lsiw_program_new(const char *exe, const char *name)
{
    struct lsiw_program *p = calloc(1, sizeof *p);
    char *nm;
    if (name && *name) nm = strdup(name);
    else {
        const char *b = strrchr(exe, '/');
        nm = strdup(b ? b + 1 : exe);
        char *dot = strrchr(nm, '.');
        if (dot && dot != nm) *dot = 0;
    }
    p->id = new_id(nm);
    p->name = nm;
    p->exe = strdup(exe);
    p->args = dup0(NULL); p->workdir = dup0(NULL); p->engine = dup0(NULL);
    p->prefix = strdup("shared"); p->env = dup0(NULL); p->icon = dup0(NULL);
    p->dxvk = p->vkd3d = -1;
    return p;
}

static void put(const char *sec, const char *key, const char *v)
{
    lsiw_setting_set(sec, key, v && *v ? v : NULL);
}

static const char *tri_name(int v) { return v < 0 ? NULL : v ? "on" : "off"; }

void lsiw_program_store(const struct lsiw_program *p)
{
    char *sec = lsiw_strdup_printf(SECTION "%s", p->id);
    /* Rewrite the section from scratch so removed variables go away. */
    lsiw_setting_remove_section(sec);
    lsiw_setting_set(sec, "name", p->name && *p->name ? p->name : p->id);
    put(sec, "exe", p->exe);
    put(sec, "args", p->args);
    put(sec, "workdir", p->workdir);
    put(sec, "engine", p->engine);
    put(sec, "prefix", p->prefix && *p->prefix ? p->prefix : "shared");
    put(sec, "icon", p->icon);
    put(sec, "dxvk", tri_name(p->dxvk));
    put(sec, "vkd3d", tri_name(p->vkd3d));
    put(sec, "mangohud", p->mangohud ? "true" : NULL);
    put(sec, "debug", p->debug ? "true" : NULL);
    char *env = strdup(p->env ? p->env : ""), *save = NULL;
    for (char *line = strtok_r(env, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        char *k = lsiw_strdup_printf("env:%s", line);
        lsiw_setting_set(sec, k, eq + 1);
        free(k);
    }
    free(env);
    free(sec);
}

void lsiw_program_remove(const char *id)
{
    char *sec = lsiw_strdup_printf(SECTION "%s", id);
    lsiw_setting_remove_section(sec);
    free(sec);
}

char *lsiw_program_compat_dir(const struct lsiw_program *p)
{
    const char *pf = p->prefix;
    if (!pf || !*pf || !strcasecmp(pf, "shared")) return strdup(lsiw_shared_dir());
    if (!strcasecmp(pf, "own")) return lsiw_strdup_printf("%s/prefixes/%s", lsiw_data_dir(), p->id);
    if (pf[0] == '~' && pf[1] == '/') return lsiw_strdup_printf("%s%s", lsiw_home_dir(), pf + 1);
    if (pf[0] == '/') return strdup(pf);
    return lsiw_strdup_printf("%s/prefixes/%s", lsiw_data_dir(), pf);   /* a named prefix */
}

char *lsiw_program_desktop_file(const struct lsiw_program *p)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') return lsiw_strdup_printf("%s/applications/lsiw-%s.desktop", xdg, p->id);
    return lsiw_strdup_printf("%s/.local/share/applications/lsiw-%s.desktop", lsiw_home_dir(), p->id);
}

int lsiw_is_png_file(const char *path)
{
    unsigned char sig[8] = {0};
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    size_t n = fread(sig, 1, sizeof sig, f);
    fclose(f);
    return n == 8 && !memcmp(sig, "\x89PNG\r\n\x1a\n", 8);
}

static int has_ext(const char *path, const char *ext)
{
    size_t l = strlen(path), e = strlen(ext);
    return l > e && !strcasecmp(path + l - e, ext);
}

/* A path from the icon setting or a .lnk: Linux, ~/ or C:\ ("x.exe,0":
 * the icon index is dropped). Caller frees. */
static char *icon_source_path(const char *v)
{
    if (!v || !*v) return NULL;
    char *s = strdup(v);
    char *comma = strrchr(s, ',');
    if (comma && comma[1] && strspn(comma + 1, "-0123456789") == strlen(comma + 1)) *comma = 0;
    char *r;
    if (s[0] == '~' && s[1] == '/') r = lsiw_strdup_printf("%s%s", lsiw_home_dir(), s + 1);
    else if (s[0] == '/') r = strdup(s);
    else r = lsiw_win_to_linux(s);
    free(s);
    return r;
}

static time_t mtime_of(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 ? st.st_mtime : 0;
}

char *lsiw_program_icon(const struct lsiw_program *p, int refresh)
{
    char *src = NULL;
    if (p->icon && *p->icon) src = icon_source_path(p->icon);
    else if (p->exe && *p->exe) {
        if (has_ext(p->exe, ".lnk")) {
            struct lsiw_lnk l;
            if (lsiw_lnk_read(p->exe, &l) == 0) {
                src = icon_source_path(l.icon && *l.icon ? l.icon : l.target);
                lsiw_lnk_free(&l);
            }
        } else src = strdup(p->exe);
    }
    if (!src) return NULL;
    struct stat st;
    if (stat(src, &st) < 0 || !S_ISREG(st.st_mode)) { free(src); return NULL; }
    /* Another image format: desktops show it as it is. */
    if (p->icon && *p->icon && !lsiw_is_png_file(src) &&
        !has_ext(src, ".ico") && !has_ext(src, ".exe") && !has_ext(src, ".dll") && !has_ext(src, ".icl"))
        return src;
    char *dir = lsiw_strdup_printf("%s/icons", lsiw_data_dir());
    char *cache = lsiw_strdup_printf("%s/%s.png", dir, p->id);
    /* A PNG the user chose is used where it is. */
    if (lsiw_is_png_file(src)) { free(dir); free(cache); return src; }
    if (!refresh && mtime_of(cache) >= st.st_mtime && lsiw_is_png_file(cache)) {
        free(dir); free(src);
        return cache;
    }
    unsigned char *png = NULL;
    size_t len = 0;
    char *res = NULL;
    if (lsiw_icon_png(src, &png, &len) == 0) {
        lsiw_mkdirs(dir, 0755);
        char *tmp = lsiw_strdup_printf("%s.tmp-%d", cache, (int)getpid());
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(png, 1, len, f) == len;
        if (f && fclose(f) != 0) ok = 0;
        if (ok && rename(tmp, cache) == 0) res = strdup(cache);
        else unlink(tmp);
        free(tmp);
        free(png);
    } else {
        LSIW_TRACE("%s: no icon in %s\n", p->id, src);
        unlink(cache);
    }
    free(cache); free(dir); free(src);
    return res;
}

/* ------------------------------------------------------------------ */
/* Engine fallback chain                                                */
/* ------------------------------------------------------------------ */

size_t lsiw_engine_chain(char ***out)
{
    const char *v = lsiw_setting_get("engine", "chain", NULL);
    char **list = calloc(LSIW_CHAIN_MAX + 1, sizeof *list);
    size_t n = 0;
    char *copy = strdup(v && *v ? v : "native,wine,proton"), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t && n < LSIW_CHAIN_MAX; t = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*t)) t++;
        char *e = t + strlen(t);
        while (e > t && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*t || !strcasecmp(t, "none")) continue;
        list[n++] = strdup(t);
    }
    free(copy);
    if (!n) { free(list); list = NULL; }
    *out = list;
    return n;
}
