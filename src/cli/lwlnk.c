/* LSIW command-line program: LSIW shortcuts (.lwlnk).
 *
 *   lsiw lwlnk create ID [PATH] [--icon IMAGE]   a shortcut to a program of
 *                                   the library (default: on the desktop)
 *   lsiw launch FILE.lwlnk [ARGS...]   start it (also 'lsiw run FILE.lwlnk'
 *                                   and opening it in a file manager)
 *   lsiw lwlnk show FILE.lwlnk      what it holds
 *   lsiw lwlnk thumbnail FILE OUT [SIZE]   its icon as a PNG, for file
 *                                   managers (lsiw-lwlnk.thumbnailer)
 *
 * The file format is described in src/common/lwlnk.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include "wine.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char lsiw_help_lwlnk[] =
    "Usage: lsiw launch FILE.lwlnk [ARGS...]\n"
    "       lsiw lwlnk create ID [PATH] [--icon IMAGE] [--no-icon]\n"
    "       lsiw lwlnk [show] FILE.lwlnk\n"
    "       lsiw lwlnk thumbnail FILE.lwlnk OUTPUT.png [SIZE]\n"
    "\n"
    "LSIW shortcuts (.lwlnk files) start a program of the library ('lsiw\n"
    "programs') with the settings it has there: engine, Wine or Proton\n"
    "version, prefix, DXVK, VKD3D, arguments, start folder, environment,\n"
    "MangoHud and debug log. Changing those settings later changes what the\n"
    "shortcut does. The shortcut also keeps a copy of the settings, used if\n"
    "the program is no longer in the library (or on another computer), and\n"
    "its icon, which file managers show. Double-click one, or:\n"
    "\n"
    "  launch     start it; ARGS are added after the program's own arguments\n"
    "             ('lsiw run FILE.lwlnk' and 'lsiw FILE.lwlnk' do the same)\n"
    "  create     make a shortcut to program ID: PATH is a file or a folder\n"
    "             (default: the desktop, named after the program). The icon is\n"
    "             IMAGE (.png, .ico, .exe, .dll; .svg/.jpg are linked, not\n"
    "             embedded), else the program's icon setting, else the icon\n"
    "             inside the program; --no-icon leaves it out\n"
    "  show       what a shortcut holds\n"
    "  thumbnail  write its icon as a PNG (used by file managers)\n";

static int is_help(const char *a) { return !strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "help"); }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

static const char *tri(int v) { return v < 0 ? "default" : v ? "on" : "off"; }

/* A file name from a program name: no '/', no leading dot. */
static char *file_name(const char *name, const char *id)
{
    char *s = lsiw_cli_strdup(name);
    for (char *c = s; *c; c++)
        if (*c == '/' || *c == '\n' || *c == '\r' || *c == '\t' || (unsigned char)*c < 0x20) *c = *c == '/' ? '-' : ' ';
    char *b = s;
    while (*b == '.' || *b == ' ') b++;
    size_t l = strlen(b);
    while (l && b[l - 1] == ' ') b[--l] = 0;
    char *r = lsiw_cli_strdup(*b ? b : id);
    free(s);
    return r;
}

/* 1 if `path` is a .lwlnk to program `id` (it may be replaced). */
static int same_shortcut(const char *path, const char *id)
{
    struct lsiw_lwlnk l;
    if (lsiw_lwlnk_read(path, &l) < 0) return 0;
    int r = !strcmp(l.program->id, id);
    lsiw_lwlnk_free(&l);
    return r;
}

/* file:// URI of a local path (RFC 3986 percent-encoding). */
static char *file_uri(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    char *r = malloc(strlen(path) * 3 + 8), *o = r;
    if (!r) abort();
    o += sprintf(o, "file://");
    for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            strchr("/-._~!$&'()*+,;=:@", *c))
            *o++ = (char)*c;
        else { *o++ = '%'; *o++ = hex[*c >> 4]; *o++ = hex[*c & 15]; }
    }
    *o = 0;
    return r;
}

/* Desktops that read GIO metadata (GNOME Files, Nemo, Caja, the GNOME
 * desktop) show this icon for the file right away, before (or without) a
 * thumbnail. Others ignore it. */
static void set_custom_icon(const char *file, const char *icon)
{
    char *gio = lsiw_cli_which("gio");
    if (!gio) return;
    char *uri = icon ? file_uri(icon) : NULL;
    char *set[] = { gio, "set", (char *)file, "metadata::custom-icon", uri, NULL };
    char *unset[] = { gio, "set", "-t", "unset", (char *)file, "metadata::custom-icon", NULL };
    char **a = icon ? set : unset;
    free(lsiw_cli_capture(a, NULL, NULL));     /* "not supported" off GNOME: ignored */
    free(uri);
    free(gio);
}

static int lwlnk_create(int argc, char **argv)
{
    const char *id = NULL, *dest = NULL, *image = NULL;
    int no_icon = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--icon") && i + 1 < argc) image = argv[++i];
        else if (!strncmp(argv[i], "--icon=", 7)) image = argv[i] + 7;
        else if (!strcmp(argv[i], "--no-icon")) no_icon = 1;
        else if (!strcmp(argv[i], "--desktop")) dest = NULL;
        else if (argv[i][0] == '-' && argv[i][1]) { lsiw_log("unknown option '%s'\n", argv[i]); return 2; }
        else if (!id) id = argv[i];
        else if (!dest) dest = argv[i];
        else { lsiw_log("too many arguments (try 'lsiw lwlnk --help')\n"); return 2; }
    }
    if (!id) { lsiw_log("usage: lsiw lwlnk create ID [PATH] [--icon IMAGE]\n"); return 2; }
    struct lsiw_program *p = lsiw_program_get(id);
    if (!p) { lsiw_log("no program '%s' in the library ('lsiw programs' lists them)\n", id); return 1; }

    /* Where: a file, a folder (named after the program) or the desktop. */
    char *dir = NULL, *path = NULL;
    if (dest && !is_dir(dest)) {
        path = lsiw_cli_has_ext(dest, ".lwlnk") ? lsiw_cli_strdup(dest) : lsiw_cli_printf("%s.lwlnk", dest);
    } else {
        dir = dest ? lsiw_cli_strdup(dest) : lsiw_cli_desktop_dir();
        if (lsiw_mkdirs(dir, 0755) < 0) {
            lsiw_log("cannot create %s: %s\n", dir, strerror(errno));
            free(dir);
            lsiw_program_free(p);
            return 1;
        }
        char *base = file_name(p->name, p->id);
        path = lsiw_cli_printf("%s/%s.lwlnk", dir, base);
        /* Never replace another file; a shortcut to the same program is
         * brought up to date. */
        for (int n = 2; exists(path) && !same_shortcut(path, p->id); n++) {
            free(path);
            path = lsiw_cli_printf("%s/%s (%d).lwlnk", dir, base, n);
        }
        free(base);
    }

    /* The icon: chosen here, else the program's (setting or its own). */
    char *icon = NULL;
    unsigned char *png = NULL;
    size_t png_len = 0;
    if (!no_icon) {
        if (image) {
            char *abs = image[0] == '/' ? lsiw_cli_strdup(image) : NULL;
            if (!abs) { char *cwd = getcwd(NULL, 0); abs = lsiw_cli_printf("%s/%s", cwd ? cwd : ".", image); free(cwd); }
            free(p->icon);
            p->icon = abs;
            if (!exists(abs)) lsiw_log("warning: the icon %s does not exist\n", abs);
        }
        icon = lsiw_program_icon(p, 1);
        if (icon && lsiw_icon_png(icon, &png, &png_len) < 0) { png = NULL; png_len = 0; }
        if (!icon) LSIW_TRACE("%s has no icon\n", p->id);
    }
    int rc = 0;
    if (lsiw_lwlnk_write(path, p, icon, png, png_len) < 0) {
        lsiw_log("cannot write %s: %s\n", path, strerror(errno));
        rc = 1;
    } else {
        set_custom_icon(path, icon);
        printf("%s\n", path);
        if (icon && !png)
            printf("note: %s is linked but not embedded (only .png, .ico, .exe and .dll icons are);\n"
                   "      file managers that use thumbnails show the LSIW icon instead\n", icon);
    }
    free(png); free(icon); free(path); free(dir);
    lsiw_program_free(p);
    return rc;
}

static int lwlnk_show(const char *file)
{
    struct lsiw_lwlnk l;
    if (lsiw_lwlnk_read(file, &l) < 0) {
        lsiw_log("%s: %s\n", file, errno == EINVAL ? "not an LSIW shortcut" : strerror(errno));
        return 1;
    }
    struct lsiw_program *s = l.program;
    struct lsiw_program *lib = *s->id ? lsiw_program_get(s->id) : NULL;
    printf("Name:      %s\n", s->name);
    printf("Program:   %s (%s)\n", *s->id ? s->id : "(none)",
           lib ? "in the library: started with its current settings"
               : "not in the library: started with the settings below");
    printf("Saved settings:\n");
    printf("  Program:   %s\n", s->exe);
    printf("  Arguments: %s\n", *s->args ? s->args : "(none)");
    printf("  Starts in: %s\n", *s->workdir ? s->workdir : "its own folder");
    printf("  Engine:    %s\n", *s->engine ? s->engine : "default (the default engine)");
    printf("  Prefix:    %s\n", s->prefix);
    printf("  DXVK:      %s\n  VKD3D:     %s\n", tri(s->dxvk), tri(s->vkd3d));
    printf("  MangoHud:  %s\n  Debug log: %s\n", s->mangohud ? "on" : "off", s->debug ? "on" : "off");
    if (*s->env) {
        char *copy = lsiw_cli_strdup(s->env), *save = NULL;
        for (char *e = strtok_r(copy, "\n", &save); e; e = strtok_r(NULL, "\n", &save)) printf("  Env:       %s\n", e);
        free(copy);
    }
    printf("Icon:      %s%s\n", *l.icon ? l.icon : "(none)",
           l.icon_png ? " (embedded in the shortcut)" : "");
    lsiw_program_free(lib);
    lsiw_lwlnk_free(&l);
    return 0;
}

/* Thumbnailers run sandboxed (GNOME: only the input file and the output
 * folder are visible), so the embedded PNG is what normally serves. */
static int lwlnk_thumbnail(const char *file, const char *out)
{
    struct lsiw_lwlnk l;
    if (lsiw_lwlnk_read(file, &l) < 0) return 1;
    unsigned char *png = l.icon_png;
    size_t len = l.icon_png_len;
    unsigned char *own = NULL;
    if (!png && *l.icon && lsiw_icon_png(l.icon, &own, &len) == 0) png = own;
    int rc = 1;
    if (png && len > 8 && !memcmp(png, "\x89PNG", 4))
        rc = lsiw_cli_write_file(out, png, len, 0644) == 0 ? 0 : 1;
    free(own);
    lsiw_lwlnk_free(&l);
    return rc;
}

int lsiw_launch_lwlnk(const char *file, int argc, char **argv)
{
    struct lsiw_lwlnk l;
    if (lsiw_lwlnk_read(file, &l) < 0) {
        if (errno == EINVAL) lsiw_log("%s is not an LSIW shortcut\n", file);
        else lsiw_log("cannot read %s: %s\n", file, strerror(errno));
        return errno == ENOENT ? 127 : 126;
    }
    struct lsiw_program *p = *l.program->id ? lsiw_program_get(l.program->id) : NULL;
    if (p) {
        LSIW_TRACE("%s: program %s of the library\n", file, p->id);
    } else {
        p = l.program;
        l.program = NULL;
        if (!*p->id) { free(p->id); p->id = lsiw_cli_strdup("shortcut"); }
        lsiw_log("note: '%s' is not in the library; using the settings saved in %s\n", p->id, file);
    }
    int rc = lsiw_cli_program_run(p, argc, argv);
    lsiw_program_free(p);
    lsiw_lwlnk_free(&l);
    return rc;
}

int lsiw_cmd_launch(int argc, char **argv)
{
    int i = 1;
    for (; i < argc; i++) {
        if (is_help(argv[i])) { fputs(lsiw_help_lwlnk, stdout); return 0; }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) lsiw_verbose = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else break;
    }
    if (i >= argc) { lsiw_log("usage: lsiw launch FILE.lwlnk [ARGS...]\n"); return 2; }
    return lsiw_launch_lwlnk(argv[i], argc - i - 1, argv + i + 1);
}

int lsiw_cmd_lwlnk(int argc, char **argv)
{
    if (argc < 2) { fputs(lsiw_help_lwlnk, stderr); return 2; }
    const char *sub = argv[1];
    if (is_help(sub)) { fputs(lsiw_help_lwlnk, stdout); return 0; }
    if (!strcmp(sub, "create")) return lwlnk_create(argc - 2, argv + 2);
    if (!strcmp(sub, "launch")) return lsiw_cmd_launch(argc - 1, argv + 1);
    if (!strcmp(sub, "thumbnail")) {
        if (argc < 4) { lsiw_log("usage: lsiw lwlnk thumbnail FILE.lwlnk OUTPUT.png [SIZE]\n"); return 2; }
        return lwlnk_thumbnail(argv[2], argv[3]);
    }
    if (!strcmp(sub, "show")) {
        if (argc < 3) { lsiw_log("usage: lsiw lwlnk show FILE.lwlnk\n"); return 2; }
        return lwlnk_show(argv[2]);
    }
    if (argc == 2) return lwlnk_show(sub);
    lsiw_log("unknown subcommand 'lwlnk %s' (try 'lsiw lwlnk --help')\n", sub);
    return 2;
}
