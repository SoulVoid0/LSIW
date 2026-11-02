/* Configuration storage and variable resolution.
 *
 * Config file (INI):
 *   [runtime]
 *   backend = auto          ; auto | native | wine
 *   wine = wine             ; Wine binary used by the fallback engine
 *   block_drive_letters = true
 *   [variables]
 *   APPDATA = /home/me/.local/share/lsiw/users/me/AppData/Roaming
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct entry { char *section, *key, *value; struct entry *next; };
static struct entry *g_entries;
static int g_loaded;

/* Per-variable caches, indexed like the table in vars.c. */
static char **g_linux_cache, **g_win_cache;
static unsigned char *g_busy;

static void invalidate(void)
{
    size_t n; lsiw_vars(&n);
    if (!g_linux_cache) {
        g_linux_cache = calloc(n, sizeof(char *));
        g_win_cache = calloc(n, sizeof(char *));
        g_busy = calloc(n, 1);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(g_linux_cache[i]); g_linux_cache[i] = NULL;
        free(g_win_cache[i]);   g_win_cache[i] = NULL;
    }
}

static struct entry *find_entry(const char *section, const char *key)
{
    for (struct entry *e = g_entries; e; e = e->next)
        if (!strcasecmp(e->section, section) && !strcasecmp(e->key, key)) return e;
    return NULL;
}

static void put_entry(const char *section, const char *key, const char *value)
{
    struct entry *e = find_entry(section, key), **pp;
    if (!value) {
        for (pp = &g_entries; *pp; pp = &(*pp)->next)
            if (*pp == e) {
                *pp = e->next;
                free(e->section); free(e->key); free(e->value); free(e);
                break;
            }
        return;
    }
    if (e) { free(e->value); e->value = strdup(value); return; }
    e = calloc(1, sizeof *e);
    e->section = strdup(section); e->key = strdup(key); e->value = strdup(value);
    /* Keep insertion order so saved files stay stable. */
    for (pp = &g_entries; *pp; pp = &(*pp)->next) ;
    *pp = e;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096], section[128] = "runtime";
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", trim(s + 1)); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        size_t vl = strlen(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
        if (*k) put_entry(section, k, v);
    }
    fclose(f);
}

const char *lsiw_config_path(void)
{
    static char *path;
    if (!path) {
        const char *ov = getenv("LSIW_CONFIG");
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (ov && *ov) path = strdup(ov);
        else if (xdg && *xdg == '/') path = lsiw_strdup_printf("%s/lsiw/lsiw.conf", xdg);
        else path = lsiw_strdup_printf("%s/.config/lsiw/lsiw.conf", lsiw_home_dir());
    }
    return path;
}

void lsiw_config_load(void)
{
    while (g_entries) {
        struct entry *e = g_entries; g_entries = e->next;
        free(e->section); free(e->key); free(e->value); free(e);
    }
    if (!getenv("LSIW_CONFIG")) load_file("/etc/lsiw/lsiw.conf");
    load_file(lsiw_config_path());
    g_loaded = 1;
    invalidate();
    const char *v = getenv("LSIW_VERBOSE");
    if (v && *v && *v != '0') lsiw_verbose = 1;
}

static void ensure_loaded(void) { if (!g_loaded) lsiw_config_load(); }

int lsiw_config_save(void)
{
    ensure_loaded();
    const char *path = lsiw_config_path();
    char *dir = strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; lsiw_mkdirs(dir, 0755); }
    free(dir);

    char *tmp = lsiw_strdup_printf("%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(tmp); return -1; }
    fprintf(f, "# LSIW configuration. Edit with lsiw-control, `lsiw config`, or by hand.\n");
    fprintf(f, "# Variables hold the Linux location of each Windows folder.\n\n");
    const char *sections[] = { "runtime", "variables", NULL };
    for (int i = 0; sections[i]; i++) {
        int header = 0;
        for (struct entry *e = g_entries; e; e = e->next) {
            if (strcasecmp(e->section, sections[i])) continue;
            if (!header) { fprintf(f, "[%s]\n", sections[i]); header = 1; }
            fprintf(f, "%s = %s\n", e->key, e->value);
        }
        if (header) fputc('\n', f);
    }
    /* Every other section ([engine], [drive], [program:ID], ...), each
     * once, in the order they first appear. */
    for (struct entry *e = g_entries; e; e = e->next) {
        if (!strcasecmp(e->section, "runtime") || !strcasecmp(e->section, "variables")) continue;
        int seen = 0;
        for (struct entry *b = g_entries; b != e && !seen; b = b->next)
            seen = !strcasecmp(b->section, e->section);
        if (seen) continue;
        fprintf(f, "[%s]\n", e->section);
        for (struct entry *x = e; x; x = x->next)
            if (!strcasecmp(x->section, e->section)) {
                /* Spaces at either end, or a leading quote, survive in quotes. */
                size_t vl = strlen(x->value);
                int q = vl && (x->value[0] == ' ' || x->value[vl - 1] == ' ' || x->value[0] == '"');
                fprintf(f, q ? "%s = \"%s\"\n" : "%s = %s\n", x->key, x->value);
            }
        fputc('\n', f);
    }
    int ok = fclose(f) == 0 && rename(tmp, path) == 0;
    free(tmp);
    return ok ? 0 : -1;
}

const char *lsiw_setting_get(const char *section, const char *key, const char *def)
{
    ensure_loaded();
    struct entry *e = find_entry(section, key);
    return e ? e->value : def;
}

void lsiw_setting_set(const char *section, const char *key, const char *value)
{
    ensure_loaded();
    put_entry(section, key, value);
    invalidate();
}

static void strv_add(char ***v, size_t *n, const char *s)
{
    *v = realloc(*v, (*n + 2) * sizeof **v);
    (*v)[(*n)++] = strdup(s);
    (*v)[*n] = NULL;
}

char **lsiw_setting_sections(const char *prefix)
{
    ensure_loaded();
    char **v = NULL;
    size_t n = 0, pl = strlen(prefix);
    for (struct entry *e = g_entries; e; e = e->next) {
        if (strncasecmp(e->section, prefix, pl)) continue;
        int seen = 0;
        for (size_t i = 0; i < n && !seen; i++) seen = !strcasecmp(v[i], e->section);
        if (!seen) strv_add(&v, &n, e->section);
    }
    return v;
}

char **lsiw_setting_keys(const char *section)
{
    ensure_loaded();
    char **v = NULL;
    size_t n = 0;
    for (struct entry *e = g_entries; e; e = e->next)
        if (!strcasecmp(e->section, section)) strv_add(&v, &n, e->key);
    return v;
}

void lsiw_setting_remove_section(const char *section)
{
    ensure_loaded();
    struct entry **pp = &g_entries;
    while (*pp) {
        struct entry *e = *pp;
        if (!strcasecmp(e->section, section)) {
            *pp = e->next;
            free(e->section); free(e->key); free(e->value); free(e);
        } else pp = &e->next;
    }
    invalidate();
}

static char *expand_template(const char *tpl);

char *lsiw_expand_template(const char *tpl) { return expand_template(tpl); }

/* A folder path without "//" or a trailing slash. */
static void normalise_path(char *val)
{
    char *r = val, *w = val;
    while (*r) { *w++ = *r; if (*r == '/') while (r[1] == '/') r++; r++; }
    *w = 0;
    size_t l = strlen(val);
    while (l > 1 && val[l - 1] == '/') val[--l] = 0;
}

/* A folder set to exactly where its default puts it (for example written
 * out in full, /home/me/Windows for ${HOME}/Windows) is not a change. */
static int same_as_default(const struct lsiw_var *v, const char *value)
{
    if (v->kind != LSIW_VAR_FOLDER || !v->def) return 0;
    char *a = expand_template(value), *b = expand_template(v->def);
    normalise_path(a);
    normalise_path(b);
    int same = !strcmp(a, b);
    free(a);
    free(b);
    return same;
}

const char *lsiw_var_override(const char *name)
{
    ensure_loaded();
    const struct lsiw_var *v = lsiw_var_resolve(lsiw_var_find(name));
    if (!v) return NULL;
    struct entry *e = find_entry("variables", v->name);
    /* Accept overrides written under an alias name too (APPDATA = ...). */
    size_t n; const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n && !e; i++)
        if (t[i].kind == LSIW_VAR_ALIAS && lsiw_var_resolve(&t[i]) == v)
            e = find_entry("variables", t[i].name);
    if (!e || same_as_default(v, e->value)) return NULL;
    return e->value;
}

int lsiw_var_set(const char *name, const char *value)
{
    ensure_loaded();
    const struct lsiw_var *v0 = lsiw_var_find(name);
    if (!v0) return -1;
    const struct lsiw_var *v = lsiw_var_resolve(v0);
    if (!v) return -1;
    if (v->kind == LSIW_VAR_DERIVED) return -2;

    /* Drop overrides stored under alias names so only one survives. */
    size_t n; const struct lsiw_var *t = lsiw_vars(&n);
    for (size_t i = 0; i < n; i++)
        if (lsiw_var_resolve(&t[i]) == v) put_entry("variables", t[i].name, NULL);

    if (value && *value && (v->kind == LSIW_VAR_FOLDER || v->kind == LSIW_VAR_VIRTUAL)) {
        /* Windows-style input (C:\x, %APPDATA%\x) is converted to Linux. */
        if (strchr(value, '\\') || strchr(value, '%') ||
            (isalpha((unsigned char)value[0]) && value[1] == ':')) {
            invalidate();
            char *lin = lsiw_win_to_linux(value);
            put_entry("variables", v->name, lin);
            free(lin);
        } else if (value[0] == '~' && (value[1] == '/' || !value[1])) {
            char *lin = lsiw_strdup_printf("%s%s", lsiw_home_dir(), value + 1);
            put_entry("variables", v->name, lin);
            free(lin);
        } else {
            put_entry("variables", v->name, value);
        }
    } else if (value) {
        put_entry("variables", v->name, value);
    }
    invalidate();
    /* Choosing the default location again leaves the setting at "default". */
    struct entry *e = find_entry("variables", v->name);
    if (e && same_as_default(v, e->value)) {
        put_entry("variables", v->name, NULL);
        invalidate();
    }
    return 0;
}

/* ---- template expansion ---- */

static char *xdg_user_dir(const char *name)
{
    /* Parse ~/.config/user-dirs.dirs: XDG_DOCUMENTS_DIR="$HOME/Documents" */
    char *path = lsiw_strdup_printf("%s/.config/user-dirs.dirs", lsiw_home_dir());
    FILE *f = fopen(path, "r");
    free(path);
    char *res = NULL;
    char want[192];
    snprintf(want, sizeof want, "XDG_%s_DIR=", name);
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *s = trim(line);
            if (strncmp(s, want, strlen(want))) continue;
            s += strlen(want);
            size_t l = strlen(s);
            if (l >= 2 && s[0] == '"' && s[l - 1] == '"') { s[l - 1] = 0; s++; }
            if (!strncmp(s, "$HOME", 5)) res = lsiw_strdup_printf("%s%s", lsiw_home_dir(), s + 5);
            else if (s[0] == '/') res = strdup(s);
            break;
        }
        fclose(f);
    }
    if (!res) {
        static const struct { const char *x, *d; } dflt[] = {
            { "DOCUMENTS", "Documents" }, { "DESKTOP", "Desktop" }, { "MUSIC", "Music" },
            { "PICTURES", "Pictures" }, { "VIDEOS", "Videos" }, { "DOWNLOAD", "Downloads" },
            { "TEMPLATES", "Templates" }, { "PUBLICSHARE", "Public" },
        };
        for (size_t i = 0; i < sizeof dflt / sizeof dflt[0]; i++)
            if (!strcmp(dflt[i].x, name))
                res = lsiw_strdup_printf("%s/%s", lsiw_home_dir(), dflt[i].d);
        if (!res) res = strdup(lsiw_home_dir());
    }
    return res;
}

static const char *linux_of(const struct lsiw_var *v);

static char *expand_template(const char *tpl)
{
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    for (const char *p = tpl; *p; ) {
        const char *rep = NULL;
        char *owned = NULL;
        if (p[0] == '$' && p[1] == '{') {
            const char *e = strchr(p, '}');
            if (e) {
                char tok[128];
                size_t tl = (size_t)(e - p - 2);
                if (tl >= sizeof tok) tl = sizeof tok - 1;
                memcpy(tok, p + 2, tl); tok[tl] = 0;
                if (!strcmp(tok, "HOME")) rep = lsiw_home_dir();
                else if (!strcmp(tok, "HOMES")) {
                    rep = owned = strdup(lsiw_home_dir());
                    char *sl = strrchr(owned, '/');
                    if (sl && sl != owned) *sl = 0;
                    else strcpy(owned, "/");
                }
                else if (!strcmp(tok, "USER")) rep = lsiw_user_name();
                else if (!strcmp(tok, "DATA")) rep = lsiw_data_dir();
                else if (!strcmp(tok, "ROOT")) rep = lsiw_root_dir();
                else if (!strcmp(tok, "UID")) rep = owned = lsiw_strdup_printf("%u", (unsigned)getuid());
                else if (!strcmp(tok, "HOSTNAME")) {
                    char h[256] = "LSIW";
                    gethostname(h, sizeof h); h[sizeof h - 1] = 0;
                    char *dot = strchr(h, '.'); if (dot) *dot = 0;
                    for (char *c = h; *c; c++) *c = toupper((unsigned char)*c);
                    rep = owned = strdup(h);
                }
                else if (!strcmp(tok, "TRASH")) {
                    const char *xdg = getenv("XDG_DATA_HOME");
                    rep = owned = (xdg && *xdg == '/')
                        ? lsiw_strdup_printf("%s/Trash/files", xdg)
                        : lsiw_strdup_printf("%s/.local/share/Trash/files", lsiw_home_dir());
                }
                else if (!strncmp(tok, "XDG:", 4)) rep = owned = xdg_user_dir(tok + 4);
                else if (!strncmp(tok, "VAR:", 4)) rep = linux_of(lsiw_var_resolve(lsiw_var_find(tok + 4)));
                if (!rep) rep = "";
                p = e + 1;
            }
        }
        if (rep) {
            size_t rl = strlen(rep);
            while (len + rl + 1 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + len, rep, rl); len += rl;
            free(owned);
            continue;
        }
        if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        out[len++] = *p++;
    }
    out[len] = 0;
    return out;
}

static size_t var_index(const struct lsiw_var *v)
{
    size_t n; const struct lsiw_var *t = lsiw_vars(&n);
    return (size_t)(v - t);
}

static const char *linux_of(const struct lsiw_var *v)
{
    if (!v) return NULL;
    ensure_loaded();
    size_t i = var_index(v);
    if (g_linux_cache[i]) return g_linux_cache[i];
    if (g_busy[i]) return "";           /* cycle in user overrides */
    g_busy[i] = 1;
    const char *ov = (v->kind != LSIW_VAR_DERIVED) ? lsiw_var_override(v->name) : NULL;
    char *val;
    if (ov) val = (v->kind == LSIW_VAR_VALUE) ? strdup(ov) : expand_template(ov);
    else val = expand_template(v->def ? v->def : "");
    /* Normalise folder paths: no trailing slash, collapse "//". */
    if (v->kind != LSIW_VAR_VALUE) normalise_path(val);
    g_busy[i] = 0;
    g_linux_cache[i] = val;
    return val;
}

const char *lsiw_var_linux(const char *name)
{
    const struct lsiw_var *v = lsiw_var_resolve(lsiw_var_find(name));
    if (!v) return NULL;
    const char *s = linux_of(v);
    return (s && *s) ? s : NULL;
}

/* Find the anchored variable whose Linux location contains `path`, so the
 * Windows path can be expressed relative to its fixed C:\<anchor> name. */
static const struct lsiw_var *anchor_for(const char *path, size_t *matched)
{
    size_t n; const struct lsiw_var *t = lsiw_vars(&n);
    const struct lsiw_var *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < n; i++) {
        if (!t[i].anchor) continue;
        const char *loc = linux_of(&t[i]);
        if (!loc || !*loc) continue;
        size_t l = strlen(loc);
        if (strncmp(path, loc, l) == 0 && (path[l] == 0 || path[l] == '/') && l > best_len) {
            best = &t[i]; best_len = l;
        }
    }
    if (matched) *matched = best_len;
    return best;
}

char *lsiw_linux_to_win(const char *linuxpath)
{
    char *abs;
    if (linuxpath[0] == '/') abs = strdup(linuxpath);
    else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
        abs = lsiw_strdup_printf("%s/%s", cwd, linuxpath);
    }
    const char *rest = abs;
    const char *anchor = NULL;
    size_t m = 0;
    const struct lsiw_var *a = anchor_for(abs, &m);
    char *profile_anchor = NULL;
    /* The closest of three folders decides: an anchored folder
     * (C:\windows = ~/Windows), the C: drive itself (~/.cdrive/Games is
     * C:\Games), or the profile ($HOME is C:\users\<you>). */
    const char *root = lsiw_root_dir();
    size_t rl = strlen(root);
    if (strncmp(abs, root, rl) || (abs[rl] != 0 && abs[rl] != '/')) rl = 0;
    const char *prof = linux_of(lsiw_var_find("CSIDL_PROFILE"));
    size_t pl = prof ? strlen(prof) : 0;
    if (pl < 2 || strncmp(abs, prof, pl) || (abs[pl] != 0 && abs[pl] != '/')) pl = 0;
    if (pl > m && pl > rl) {
        const struct lsiw_var *pf = lsiw_var_find("PROFILESFOLDER");
        profile_anchor = lsiw_strdup_printf("%s\\%s", pf && pf->anchor ? pf->anchor : "users", lsiw_user_name());
        anchor = profile_anchor;
        rest = abs + pl;
    } else if (a && m >= rl) { anchor = a->anchor; rest = abs + m; }
    else if (rl) rest = abs + rl;
    size_t cap = strlen(rest) + (anchor ? strlen(anchor) : 0) + 8;
    char *out = malloc(cap), *o = out;
    *o++ = 'C'; *o++ = ':'; *o++ = '\\';
    if (anchor) { strcpy(o, anchor); o += strlen(anchor); }
    const char *r = rest;
    while (*r == '/') r++;
    if (anchor && *r) *o++ = '\\';
    for (; *r; r++) {
        if (*r == '/') { if (o[-1] != '\\') *o++ = '\\'; }
        else *o++ = *r;
    }
    *o = 0;
    size_t l = strlen(out);
    while (l > 3 && out[l - 1] == '\\') out[--l] = 0;   /* keep "C:\" */
    free(profile_anchor);
    free(abs);
    return out;
}

const char *lsiw_var_windows(const char *name)
{
    const struct lsiw_var *v0 = lsiw_var_find(name);
    const struct lsiw_var *v = lsiw_var_resolve(v0);
    if (!v) return NULL;
    size_t i = var_index(v);
    if (g_win_cache && g_win_cache[i]) return g_win_cache[i];
    const char *lin = linux_of(v);
    if (!lin || !*lin) return NULL;
    char *w;
    if (v->kind == LSIW_VAR_VALUE) w = strdup(lin);
    else if (v->anchor) w = lsiw_strdup_printf("C:\\%s", v->anchor);
    else w = lsiw_linux_to_win(lin);
    if (!strcasecmp(v->name, "HOMEPATH")) {           /* drop the "C:" */
        char *t = strdup(w + 2); free(w); w = t;
    }
    g_win_cache[i] = w;
    return w;
}

char *lsiw_expand(const char *s)
{
    size_t cap = strlen(s) + 64, len = 0;
    char *out = malloc(cap);
    for (const char *p = s; *p; ) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            if (e && e > p + 1) {
                char name[128];
                size_t nl = (size_t)(e - p - 1);
                if (nl < sizeof name) {
                    memcpy(name, p + 1, nl); name[nl] = 0;
                    const char *v = lsiw_var_windows(name);
                    if (!v) v = getenv(name);
                    if (v) {
                        size_t vl = strlen(v);
                        while (len + vl + 1 > cap) { cap *= 2; out = realloc(out, cap); }
                        memcpy(out + len, v, vl); len += vl;
                        p = e + 1;
                        continue;
                    }
                }
            }
        }
        if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        out[len++] = *p++;
    }
    out[len] = 0;
    return out;
}
