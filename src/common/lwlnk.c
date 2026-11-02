/* LSIW shortcuts (.lwlnk files): a file that starts a program of the
 * library with the settings it has there.
 *
 * The file is UTF-8 text in the Desktop Entry style:
 *
 *   [LSIW Shortcut]
 *   Version=1
 *   Name=Notepad++
 *   Program=notepad-plus-plus          ; the id in the library
 *   Exe=/home/me/Program Files/Notepad++/notepad++.exe
 *   Args=-multiInst
 *   Workdir=
 *   Engine=wine:wine-staging-11.0
 *   Prefix=shared
 *   DXVK=default
 *   VKD3D=default
 *   MangoHud=false
 *   Debug=false
 *   Env:DXVK_HUD=fps
 *   Icon=/home/me/.local/share/lsiw/icons/notepad-plus-plus.png
 *   IconData=data:image/png;base64,iVBORw0KGgo...
 *
 * Starting it runs the program of the library named by Program= with the
 * library's current settings, so later changes on the Programs page apply.
 * The other keys are a copy of those settings taken when the shortcut was
 * made; they are used only when the library has no such program (it was
 * removed, or the shortcut came from another computer). IconData is the
 * icon itself, so a file manager's thumbnailer can show it without reading
 * anything else (GNOME's thumbnailers run in a sandbox that sees only the
 * file). Values escape \n, \r, \t and \\ as in Desktop Entry files.
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
#include <sys/stat.h>
#include <unistd.h>

#define GROUP    "[LSIW Shortcut]"
#define PNG_URI  "data:image/png;base64,"

const char lsiw_lwlnk_magic[] = GROUP;

static char *dup0(const char *s) { return strdup(s ? s : ""); }

static char *escape(const char *s)
{
    char *r = malloc(strlen(s) * 2 + 1), *o = r;
    if (!r) return NULL;
    for (; *s; s++) {
        switch (*s) {
        case '\n': *o++ = '\\'; *o++ = 'n'; break;
        case '\r': *o++ = '\\'; *o++ = 'r'; break;
        case '\t': *o++ = '\\'; *o++ = 't'; break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        default: *o++ = *s;
        }
    }
    *o = 0;
    return r;
}

static char *unescape(const char *s)
{
    char *r = malloc(strlen(s) + 1), *o = r;
    if (!r) return NULL;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == 't' ? '\t' : *s == 's' ? ' ' : *s;
        } else *o++ = *s;
    }
    *o = 0;
    return r;
}

static const char *tri_name(int v) { return v < 0 ? "default" : v ? "on" : "off"; }

static int tri_parse(const char *v)
{
    if (!v || !*v || !strcasecmp(v, "default")) return -1;
    return !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1");
}

static void kv(FILE *f, const char *key, const char *value)
{
    char *e = escape(value ? value : "");
    fprintf(f, "%s=%s\n", key, e ? e : "");
    free(e);
}

int lsiw_lwlnk_write(const char *path, const struct lsiw_program *p, const char *icon,
                     const unsigned char *png, size_t png_len)
{
    char *tmp = lsiw_strdup_printf("%s.tmp-%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) { int e = errno; free(tmp); errno = e; return -1; }
    fprintf(f, GROUP "\nVersion=1\n");
    kv(f, "Name", p->name);
    kv(f, "Comment", "Windows program, run by LSIW");
    kv(f, "Program", p->id);
    kv(f, "Exe", p->exe);
    kv(f, "Args", p->args);
    kv(f, "Workdir", p->workdir);
    kv(f, "Engine", p->engine);
    kv(f, "Prefix", *p->prefix ? p->prefix : "shared");
    kv(f, "DXVK", tri_name(p->dxvk));
    kv(f, "VKD3D", tri_name(p->vkd3d));
    kv(f, "MangoHud", p->mangohud ? "true" : "false");
    kv(f, "Debug", p->debug ? "true" : "false");
    char *env = dup0(p->env), *save = NULL;
    for (char *l = strtok_r(env, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(l, '=');
        if (!eq || eq == l) continue;
        *eq = 0;
        char *k = lsiw_strdup_printf("Env:%s", l);
        kv(f, k, eq + 1);
        free(k);
    }
    free(env);
    kv(f, "Icon", icon);
    if (png && png_len) {
        char *b = lsiw_base64_encode(png, png_len);
        if (b) fprintf(f, "IconData=" PNG_URI "%s\n", b);
        free(b);
    }
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, path) < 0) {
        int e = errno;
        unlink(tmp);
        free(tmp);
        errno = e ? e : EIO;
        return -1;
    }
    free(tmp);
    return 0;
}

void lsiw_lwlnk_free(struct lsiw_lwlnk *l)
{
    if (!l) return;
    lsiw_program_free(l->program);
    free(l->icon);
    free(l->icon_png);
    memset(l, 0, sizeof *l);
}

int lsiw_lwlnk_parse(const char *text, struct lsiw_lwlnk *l)
{
    memset(l, 0, sizeof *l);
    while (*text == '\n' || *text == '\r' || *text == ' ' || *text == '\t') text++;
    if (strncmp(text, GROUP, sizeof GROUP - 1)) { errno = EINVAL; return -1; }
    struct lsiw_program *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->dxvk = p->vkd3d = -1;
    char *env = dup0(NULL);
    int in_group = 0;
    for (const char *line = text; *line; ) {
        const char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        char *cur = strndup(line, ll);
        line = nl ? nl + 1 : line + ll;
        if (!cur) break;
        size_t cl = strlen(cur);
        while (cl && (cur[cl - 1] == '\r' || isspace((unsigned char)cur[cl - 1]))) cur[--cl] = 0;
        char *s = cur;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '[') { in_group = !strcmp(s, GROUP); free(cur); continue; }
        char *eq = strchr(s, '=');
        if (!in_group || !eq || *s == '#' || *s == ';') { free(cur); continue; }
        *eq = 0;
        char *k = s, *ke = eq;
        while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
        char *raw = eq + 1;
        while (isspace((unsigned char)*raw)) raw++;
        if (!strcmp(k, "IconData")) {
            if (!strncasecmp(raw, PNG_URI, sizeof PNG_URI - 1) && !l->icon_png)
                l->icon_png = lsiw_base64_decode(raw + sizeof PNG_URI - 1, &l->icon_png_len);
            free(cur);
            continue;
        }
        char *v = unescape(raw);
        char **field = !strcmp(k, "Name") ? &p->name : !strcmp(k, "Program") ? &p->id
                     : !strcmp(k, "Exe") ? &p->exe : !strcmp(k, "Args") ? &p->args
                     : !strcmp(k, "Workdir") ? &p->workdir : !strcmp(k, "Engine") ? &p->engine
                     : !strcmp(k, "Prefix") ? &p->prefix : !strcmp(k, "Icon") ? &l->icon : NULL;
        if (field) { free(*field); *field = v; v = NULL; }
        else if (!strcmp(k, "DXVK")) p->dxvk = tri_parse(v);
        else if (!strcmp(k, "VKD3D")) p->vkd3d = tri_parse(v);
        else if (!strcmp(k, "MangoHud")) p->mangohud = tri_parse(v) == 1;
        else if (!strcmp(k, "Debug")) p->debug = tri_parse(v) == 1;
        else if (!strncmp(k, "Env:", 4) && k[4] && !strchr(k + 4, '=')) {
            char *n = lsiw_strdup_printf("%s%s=%s\n", env, k + 4, v);
            free(env);
            env = n;
        }
        free(v);
        free(cur);
    }
    p->env = env;
    char **strs[] = { &p->id, &p->name, &p->exe, &p->args, &p->workdir, &p->engine, &p->prefix, &p->icon };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) if (!*strs[i]) *strs[i] = dup0(NULL);
    if (!*p->prefix) { free(p->prefix); p->prefix = strdup("shared"); }
    if (l->icon) { free(p->icon); p->icon = strdup(l->icon); }
    else l->icon = dup0(NULL);
    if (!*p->name) { free(p->name); p->name = strdup(*p->id ? p->id : "Windows program"); }
    l->program = p;
    return 0;
}

int lsiw_lwlnk_read(const char *path, struct lsiw_lwlnk *l)
{
    memset(l, 0, sizeof *l);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    /* Icons make a shortcut a few hundred KB at most; refuse anything huge. */
    struct stat st;
    if (fstat(fileno(f), &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > 16 * 1024 * 1024) {
        fclose(f);
        errno = EINVAL;
        return -1;
    }
    char *text = malloc((size_t)st.st_size + 1);
    size_t n = text ? fread(text, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!text) return -1;
    text[n] = 0;
    int rc = lsiw_lwlnk_parse(text, l);
    free(text);
    return rc;
}
