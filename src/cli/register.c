/* Desktop and kernel integration for the lsiw command.
 *
 * --user    A desktop entry, lsiw.desktop, that opens Windows programs with
 *           'lsiw run', made the default application for their MIME types
 *           (freedesktop.org Desktop Entry and MIME Applications specs), so
 *           double-clicking setup.exe in a file manager runs it with LSIW.
 *           LSIW shortcuts (.lwlnk) open the same way; when LSIW is not
 *           installed system-wide, their MIME type and thumbnailer (which
 *           shows each shortcut's icon) are added for the user too.
 * --system  A binfmt_misc handler: the kernel itself hands every file that
 *           starts with "MZ" to lsiw-binfmt (a symlink to lsiw), so
 *           ./setup.exe runs from a shell like a Linux program. The handler
 *           is written to /etc/binfmt.d for every boot and registered live.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* strcasestr */
#endif
#include "lsiw.h"
#include "wine.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define DESKTOP_NAME   "lsiw.desktop"
#define DESKTOP_MARK   "X-LSIW-Generated=true"
#define BINFMT_DIR     "/proc/sys/fs/binfmt_misc"
#define BINFMT_NAME    "LSIW"
/* systemd-binfmt loads /etc/binfmt.d in file name order. On Fedora, Wine
 * installs wine.conf for the same "MZ" magic; the entry registered last is
 * the one the kernel uses, so ours sorts after it. */
#define BINFMT_CONF    "/etc/binfmt.d/zz-lsiw.conf"
#define BINFMT_INTERP  "/usr/libexec/lsiw/lsiw-binfmt"

/* Where packages and 'make install' put the same rule. A file of the same
 * name in /etc/binfmt.d replaces it; a symlink to /dev/null there masks it
 * (systemd's binfmt.d rules). */
static const char *const vendor_binfmt[] = {
    "/usr/lib/binfmt.d/zz-lsiw.conf",
    "/usr/local/lib/binfmt.d/zz-lsiw.conf",
    NULL
};

/* application/x-msdownload is the name shared-mime-info 2.2+ uses for
 * .exe files (x-ms-dos-executable is its alias, the name on older systems). */
const char *const lsiw_mime_types[] = {
    "application/x-msdownload",
    "application/x-ms-dos-executable",
    "application/x-msdos-program",
    "application/vnd.microsoft.portable-executable",
    "application/x-msi",
    "application/x-ms-shortcut",            /* .lnk, Windows shortcuts */
    LSIW_LWLNK_MIME,                        /* .lwlnk, LSIW shortcuts */
    NULL
};
_Static_assert(sizeof lsiw_mime_types / sizeof lsiw_mime_types[0] <= 9,
               "mimeapps_edit() tracks at most 8 MIME types");

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static char *xdg_home(const char *var, const char *fallback)
{
    const char *v = getenv(var);
    if (v && *v == '/') return lsiw_cli_strdup(v);
    return lsiw_cli_printf("%s/%s", lsiw_home_dir(), fallback);
}

static char *applications_dir(void)
{
    char *data = xdg_home("XDG_DATA_HOME", ".local/share");
    char *dir = lsiw_cli_printf("%s/applications", data);
    free(data);
    return dir;
}

static char *desktop_path(void)
{
    char *dir = applications_dir();
    char *p = lsiw_cli_printf("%s/" DESKTOP_NAME, dir);
    free(dir);
    return p;
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* lsiw.desktop installed system-wide by the package or 'make install'
 * ($XDG_DATA_DIRS/applications). Caller frees; NULL if there is none. */
static char *system_desktop_file(void)
{
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    for (const char *p = dirs; *p; ) {
        const char *e = strchr(p, ':');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && p[0] == '/') {
            char *cand = lsiw_cli_printf("%.*s/applications/" DESKTOP_NAME, (int)l, p);
            struct stat st;
            if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) return cand;
            free(cand);
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

/* 1 if `path` is a symlink to /dev/null (a systemd mask). */
static int is_mask(const char *path)
{
    char *t = lsiw_cli_readlink(path);
    int r = t && !strcmp(t, "/dev/null");
    free(t);
    return r;
}

static const char *vendor_binfmt_file(void)
{
    for (int i = 0; vendor_binfmt[i]; i++)
        if (exists(vendor_binfmt[i])) return vendor_binfmt[i];
    return NULL;
}

/* Run a helper tool if it is installed. Returns its exit status, or -2 if
 * the tool is not installed. */
static int run_tool(char *const argv[])
{
    char *bin = lsiw_cli_which(argv[0]);
    if (!bin) return -2;
    free(bin);
    int st = lsiw_cli_spawn(argv, NULL, 1);
    if (st < 0) lsiw_log("cannot run %s: %s\n", argv[0], strerror(errno));
    return st;
}

/* The Exec= command: plain "lsiw" when the lsiw in $PATH is this very
 * program, otherwise this program's absolute path, quoted as the Desktop
 * Entry spec requires (inside quotes, " ` $ and \ are backslash-escaped,
 * and the value's own string escaping doubles every backslash; a literal
 * % is written %%). */
static char *exec_command(void)
{
    char *self = lsiw_cli_self_path();
    if (!self) return lsiw_cli_strdup("lsiw");
    char *inpath = lsiw_cli_which("lsiw");
    struct stat a, b;
    int same = inpath && stat(inpath, &a) == 0 && stat(self, &b) == 0 &&
               a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    free(inpath);
    if (same) {
        free(self);
        return lsiw_cli_strdup("lsiw");
    }
    int plain = 1;
    for (const char *p = self; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("/._+-,", *p)) plain = 0;
    if (plain) return self;

    size_t cap = strlen(self) * 4 + 3;
    char *q = malloc(cap), *o = q;
    if (!q) {
        fputs("lsiw: out of memory\n", stderr);
        exit(1);
    }
    *o++ = '"';
    for (const char *p = self; *p; p++) {
        if (*p == '"' || *p == '`' || *p == '$') { *o++ = '\\'; *o++ = '\\'; *o++ = *p; }
        else if (*p == '\\') { memcpy(o, "\\\\\\\\", 4); o += 4; }
        else if (*p == '%') { *o++ = '%'; *o++ = '%'; }
        else *o++ = *p;
    }
    *o++ = '"';
    *o = 0;
    free(self);
    return q;
}

/* A file under $XDG_DATA_DIRS (rel: "mime/packages/lsiw.xml"). Caller
 * frees; NULL if there is none. */
static char *system_data_file(const char *rel)
{
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    for (const char *p = dirs; *p; ) {
        const char *e = strchr(p, ':');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && p[0] == '/') {
            char *cand = lsiw_cli_printf("%.*s/%s", (int)l, p, rel);
            struct stat st;
            if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) return cand;
            free(cand);
        }
        if (!e) break;
        p = e + 1;
    }
    return NULL;
}

#define USER_MIME_XML     "mime/packages/lsiw-lwlnk.xml"
#define USER_THUMBNAILER  "thumbnailers/lsiw-lwlnk.thumbnailer"

/* .lwlnk files for a user whose LSIW is not installed system-wide (built
 * from source and run from its folder): their MIME type and thumbnailer
 * in ~/.local/share. The package and 'make install' put the same in
 * /usr/share. add=0 removes what this wrote. */
static void lwlnk_user_files(int add)
{
    char *data = xdg_home("XDG_DATA_HOME", ".local/share");
    char *xml = lsiw_cli_printf("%s/" USER_MIME_XML, data);
    char *thumb = lsiw_cli_printf("%s/" USER_THUMBNAILER, data);
    char *mime_dir = lsiw_cli_printf("%s/mime", data);
    char *sys_xml = system_data_file("mime/packages/lsiw.xml");
    char *sys_thumb = system_data_file("thumbnailers/lsiw-lwlnk.thumbnailer");
    char *sys_text = sys_xml ? lsiw_cli_read_file(sys_xml, NULL) : NULL;
    int sys_has_type = sys_text && strstr(sys_text, LSIW_LWLNK_MIME);
    int changed = 0;
    if (add && !sys_has_type) {
        static const char text[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!-- Written by lsiw register (" DESKTOP_MARK "). -->\n"
            "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
            "  <mime-type type=\"" LSIW_LWLNK_MIME "\">\n"
            "    <comment>LSIW shortcut</comment>\n"
            "    <sub-class-of type=\"text/plain\"/>\n"
            "    <generic-icon name=\"application-x-executable\"/>\n"
            "    <glob pattern=\"*.lwlnk\"/>\n"
            "    <magic priority=\"60\"><match type=\"string\" offset=\"0\" value=\"[LSIW Shortcut]\"/></magic>\n"
            "  </mime-type>\n"
            "</mime-info>\n";
        char *dir = lsiw_cli_printf("%s/mime/packages", data);
        lsiw_mkdirs(dir, 0755);
        free(dir);
        if (lsiw_cli_write_file(xml, text, sizeof text - 1, 0644) == 0) {
            printf("Added the LSIW shortcut (.lwlnk) file type: %s\n", xml);
            changed = 1;
        }
    } else if (!add || sys_has_type) {
        char *t = lsiw_cli_read_file(xml, NULL);
        if (t && strstr(t, DESKTOP_MARK) && unlink(xml) == 0) changed = 1;
        free(t);
    }
    if (changed) {
        char *umd[] = { "update-mime-database", mime_dir, NULL };
        if (run_tool(umd) == -2) printf("update-mime-database is not installed; .lwlnk files are recognised by name only\n");
    }
    if (add && !sys_thumb) {
        char *exec = exec_command();
        char *text = lsiw_cli_printf(
            "# Written by 'lsiw register --user' (" DESKTOP_MARK ").\n"
            "[Thumbnailer Entry]\n"
            "Exec=%s lwlnk thumbnail %%i %%o %%s\n"
            "MimeType=" LSIW_LWLNK_MIME ";\n", exec);
        char *dir = lsiw_cli_printf("%s/thumbnailers", data);
        lsiw_mkdirs(dir, 0755);
        free(dir);
        if (lsiw_cli_write_file(thumb, text, strlen(text), 0644) == 0)
            printf("Added the thumbnailer that shows the icons of .lwlnk files: %s\n", thumb);
        free(text);
        free(exec);
    } else {
        char *t = lsiw_cli_read_file(thumb, NULL);
        if (t && strstr(t, DESKTOP_MARK)) unlink(thumb);
        free(t);
    }
    free(sys_text); free(sys_thumb); free(sys_xml); free(mime_dir); free(thumb); free(xml); free(data);
}

/* ------------------------------------------------------------------ */
/* mimeapps.list                                                        */
/* ------------------------------------------------------------------ */

static int is_our_type(const char *type)
{
    for (int i = 0; lsiw_mime_types[i]; i++)
        if (!strcasecmp(type, lsiw_mime_types[i])) return 1;
    return 0;
}

/* The list `value` ("a.desktop;b.desktop;") without lsiw.desktop, with
 * lsiw.desktop first when `first` is set. */
static char *filter_list(const char *value, int first)
{
    size_t cap = strlen(value) + sizeof DESKTOP_NAME + 4;
    char *out = malloc(cap);
    if (!out) {
        fputs("lsiw: out of memory\n", stderr);
        exit(1);
    }
    out[0] = 0;
    if (first) strcat(out, DESKTOP_NAME ";");
    const char *p = value;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && isspace((unsigned char)*p)) { p++; l--; }
        size_t tl = l;
        while (tl && isspace((unsigned char)p[tl - 1])) tl--;
        if (tl && !(tl == strlen(DESKTOP_NAME) && !strncmp(p, DESKTOP_NAME, tl))) {
            strncat(out, p, tl);
            strcat(out, ";");
        }
        if (!e) break;
        p = e + 1;
    }
    return out;
}

struct text { char *p; size_t len, cap; };

static void text_add(struct text *t, const char *s, size_t n)
{
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 1024;
        while (t->len + n + 1 > cap) cap *= 2;
        char *np = realloc(t->p, cap);
        if (!np) {
            fputs("lsiw: out of memory\n", stderr);
            exit(1);
        }
        t->p = np;
        t->cap = cap;
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = 0;
}

static void text_puts(struct text *t, const char *s) { text_add(t, s, strlen(s)); }

/* add: make lsiw.desktop the default application for each LSIW MIME type
 * in [Default Applications]. remove: take lsiw.desktop out of every list in
 * [Default Applications] and [Added Associations]. Every other line,
 * comment and blank line is kept as it is. Returns 1 if the file changed,
 * 0 if not, -1 on error. */
static int mimeapps_edit(const char *path, int add)
{
    char *in = lsiw_cli_read_file(path, NULL);
    if (!in && errno != ENOENT) {
        lsiw_log("cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (!in && !add) return 0;

    struct text out = {0};
    text_add(&out, "", 0);
    int ntypes = 0;
    while (lsiw_mime_types[ntypes]) ntypes++;
    int done[8] = {0};
    int group = 0;            /* 1 = Default Applications, 2 = Added Associations */
    int saw_default = 0, changed = 0, blanks = 0;

    for (const char *line = in ? in : ""; *line; ) {
        const char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        char *cur = strndup(line, ll);
        if (!cur) {
            fputs("lsiw: out of memory\n", stderr);
            exit(1);
        }
        line = nl ? nl + 1 : line + ll;

        char *s = cur;
        while (isspace((unsigned char)*s)) s++;
        if (!*s) {            /* blank lines are written before the next real line */
            blanks++;
            free(cur);
            continue;
        }
        if (*s == '[') {
            if (group == 1 && add) {
                /* Leaving [Default Applications]: add the types not seen,
                 * before the blank lines that separate the groups. */
                for (int i = 0; i < ntypes; i++)
                    if (!done[i]) {
                        text_puts(&out, lsiw_mime_types[i]);
                        text_puts(&out, "=" DESKTOP_NAME ";\n");
                        done[i] = 1;
                        changed = 1;
                    }
            }
            group = !strncmp(s, "[Default Applications]", 22) ? 1
                  : !strncmp(s, "[Added Associations]", 20) ? 2 : 0;
            if (group == 1) saw_default = 1;
        }
        for (; blanks; blanks--) text_puts(&out, "\n");

        char *eq = strchr(s, '=');
        if (*s == '[' || !group || !eq || *s == '#') {
            text_puts(&out, cur);
            text_puts(&out, "\n");
            free(cur);
            continue;
        }
        char *type = strndup(s, (size_t)(eq - s));
        if (!type) {
            fputs("lsiw: out of memory\n", stderr);
            exit(1);
        }
        size_t tl = strlen(type);
        while (tl && isspace((unsigned char)type[tl - 1])) type[--tl] = 0;
        int make_first = add && group == 1 && is_our_type(type);
        char *list = (add && !make_first) ? NULL : filter_list(eq + 1, make_first);
        if (!list) {
            text_puts(&out, cur);
            text_puts(&out, "\n");
        } else {
            if (strcmp(list, eq + 1)) changed = 1;
            if (*list) {
                text_puts(&out, type);
                text_puts(&out, "=");
                text_puts(&out, list);
                text_puts(&out, "\n");
            }
        }
        if (make_first)
            for (int i = 0; i < ntypes; i++)
                if (!strcasecmp(type, lsiw_mime_types[i])) done[i] = 1;
        free(list);
        free(type);
        free(cur);
    }
    if (add) {
        if (!saw_default) {
            if (out.len) text_puts(&out, "\n");
            text_puts(&out, "[Default Applications]\n");
            group = 1;
            changed = 1;
        }
        if (group == 1)
            for (int i = 0; i < ntypes; i++)
                if (!done[i]) {
                    text_puts(&out, lsiw_mime_types[i]);
                    text_puts(&out, "=" DESKTOP_NAME ";\n");
                    changed = 1;
                }
    }
    for (; blanks; blanks--) text_puts(&out, "\n");
    free(in);

    int rc = 0;
    if (changed) {
        char *dir = lsiw_cli_strdup(path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            lsiw_mkdirs(dir, 0755);
        }
        free(dir);
        if (lsiw_cli_write_file(path, out.p, out.len, 0644) < 0) {
            lsiw_log("cannot write %s: %s\n", path, strerror(errno));
            rc = -1;
        } else rc = 1;
    }
    free(out.p);
    return rc;
}

/* ------------------------------------------------------------------ */
/* --user                                                               */
/* ------------------------------------------------------------------ */

/* GNOME, Cinnamon, MATE and Xfce list the files in the Templates folder
 * under "New Document"; KDE has its own entry (DATADIR/templates) and
 * would show this one twice. */
#define GNOME_TEMPLATE "Link to Windows Application.lnk"

static void gnome_template(int add)
{
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    if (add && (!de || strstr(de, "KDE") ||
                !(strstr(de, "GNOME") || strstr(de, "Unity") || strstr(de, "Cinnamon") || strstr(de, "MATE") ||
                  strstr(de, "XFCE") || strstr(de, "Budgie") || strstr(de, "Pantheon"))))
        return;
    char *argv[] = { "xdg-user-dir", "TEMPLATES", NULL };
    char *tool = lsiw_cli_which("xdg-user-dir");
    char *dir = tool ? lsiw_cli_capture(argv, NULL, NULL) : NULL;
    free(tool);
    if (dir) dir[strcspn(dir, "\n")] = 0;
    if (!dir || !*dir || !strcmp(dir, lsiw_home_dir()) || !exists(dir)) { free(dir); return; }
    char *path = lsiw_cli_printf("%s/" GNOME_TEMPLATE, dir);
    if (add && !exists(path)) {
        struct lsiw_lnk l;
        memset(&l, 0, sizeof l);
        if (lsiw_lnk_write(path, &l) == 0) printf("Added \"%s\" to %s (New Document menu)\n", GNOME_TEMPLATE, dir);
    } else if (!add) {
        struct lsiw_lnk l;
        /* Only the empty template LSIW made, never a shortcut someone filled in. */
        if (lsiw_lnk_read(path, &l) == 0) {
            if ((!l.target || !*l.target) && unlink(path) == 0) printf("Removed %s\n", path);
            lsiw_lnk_free(&l);
        }
    }
    free(path);
    free(dir);
}

int lsiw_register_user(void)
{
    int rc = 0;
    char *apps = applications_dir();
    char *path = desktop_path();
    char *sys = system_desktop_file();

    if (exists(path)) {
        printf("Desktop entry already present: %s\n", path);
    } else if (sys) {
        /* The installed entry has the same name; a per-user copy would only
         * hide later updates of it. */
        printf("Using the installed desktop entry %s\n", sys);
    } else if (lsiw_mkdirs(apps, 0755) < 0) {
        lsiw_log("cannot create %s: %s\n", apps, strerror(errno));
        rc = -1;
    } else {
        char *exec = exec_command();
        char *mimes = lsiw_cli_strdup("");
        for (int i = 0; lsiw_mime_types[i]; i++) {
            char *m = lsiw_cli_printf("%s%s;", mimes, lsiw_mime_types[i]);
            free(mimes);
            mimes = m;
        }
        char *text = lsiw_cli_printf(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Version=1.5\n"
            "Name=LSIW\n"
            "GenericName=Windows Program Runner\n"
            "Comment=Run Windows programs on Linux with LSIW\n"
            "Exec=%s run %%f\n"
            "Icon=application-x-executable\n"
            "Terminal=false\n"
            "NoDisplay=true\n"
            "StartupNotify=true\n"
            "Categories=System;Emulator;\n"
            "MimeType=%s\n"
            DESKTOP_MARK "\n", exec, mimes);
        if (lsiw_cli_write_file(path, text, strlen(text), 0644) < 0) {
            lsiw_log("cannot write %s: %s\n", path, strerror(errno));
            rc = -1;
        } else {
            printf("Installed desktop entry: %s\n", path);
        }
        free(text);
        free(mimes);
        free(exec);
    }

    if (rc == 0) {
        char *udd[] = { "update-desktop-database", "-q", apps, NULL };
        int st = run_tool(udd);
        if (st == -2) printf("update-desktop-database is not installed; skipped (only needed by older desktops)\n");
        else if (st != 0) lsiw_log("warning: update-desktop-database failed (status %d)\n", st);

        char *probe = lsiw_cli_which("xdg-mime");
        if (probe) {
            free(probe);
            for (int i = 0; lsiw_mime_types[i]; i++) {
                char *xm[] = { "xdg-mime", "default", DESKTOP_NAME, (char *)lsiw_mime_types[i], NULL };
                st = run_tool(xm);
                if (st != 0) {
                    lsiw_log("xdg-mime default %s %s failed (status %d)\n", DESKTOP_NAME, lsiw_mime_types[i], st);
                    rc = -1;
                }
            }
        } else {
            /* Without xdg-utils, write the user's mimeapps.list directly. */
            char *cfg = xdg_home("XDG_CONFIG_HOME", ".config");
            char *list = lsiw_cli_printf("%s/mimeapps.list", cfg);
            if (mimeapps_edit(list, 1) < 0) rc = -1;
            free(list);
            free(cfg);
        }
        lwlnk_user_files(1);
        if (rc == 0) printf("Windows programs (.exe, .msi), Windows shortcuts (.lnk) and LSIW shortcuts (.lwlnk)\n"
                            "now open with LSIW for user %s.\n", lsiw_user_name());
        gnome_template(1);
    }
    free(sys);
    free(path);
    free(apps);
    return rc;
}

int lsiw_unregister_user(void)
{
    int rc = 0;
    gnome_template(0);
    lwlnk_user_files(0);
    char *apps = applications_dir();
    char *path = desktop_path();

    char *text = lsiw_cli_read_file(path, NULL);
    if (!text) {
        if (errno == ENOENT) printf("No desktop entry to remove (%s)\n", path);
        else { lsiw_log("cannot read %s: %s\n", path, strerror(errno)); rc = -1; }
    } else if (!strstr(text, DESKTOP_MARK)) {
        printf("%s was not created by 'lsiw register'; leaving it in place\n", path);
    } else if (unlink(path) < 0) {
        lsiw_log("cannot remove %s: %s\n", path, strerror(errno));
        rc = -1;
    } else {
        printf("Removed %s\n", path);
    }
    free(text);

    /* xdg-mime cannot unset a default; edit the lists it writes. The copy
     * under applications/ is the location older desktops used. */
    char *cfg = xdg_home("XDG_CONFIG_HOME", ".config");
    char *lists[] = { lsiw_cli_printf("%s/mimeapps.list", cfg), lsiw_cli_printf("%s/mimeapps.list", apps) };
    for (int i = 0; i < 2; i++) {
        int r = mimeapps_edit(lists[i], 0);
        if (r < 0) rc = -1;
        else if (r > 0) printf("Removed LSIW from the default applications in %s\n", lists[i]);
        free(lists[i]);
    }
    free(cfg);

    char *udd[] = { "update-desktop-database", "-q", apps, NULL };
    if (exists(apps)) run_tool(udd);
    free(path);
    free(apps);
    return rc;
}

/* ------------------------------------------------------------------ */
/* --system                                                             */
/* ------------------------------------------------------------------ */

static int need_root(const char *cmd)
{
    if (geteuid() == 0) return 0;
    lsiw_log("'%s --system' changes system settings and needs root. Run it with sudo:\n"
             "    sudo lsiw %s --system\n"
             "or, from a graphical session:\n"
             "    pkexec lsiw %s --system\n", cmd, cmd, cmd);
    return -1;
}

/* The binfmt interpreter: <prefix>/libexec/lsiw/lsiw-binfmt next to this
 * program's <prefix>/bin, else the packaged location. It must stay a
 * symlink named lsiw-binfmt: that name is how lsiw knows the kernel started
 * it, so the path is never canonicalised. */
static char *find_interpreter(void)
{
    char *self = lsiw_cli_readlink("/proc/self/exe");
    if (self) {
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;                           /* <prefix>/bin */
            slash = strrchr(self, '/');
            if (slash) {
                *slash = 0;                       /* <prefix> */
                char *cand = lsiw_cli_printf("%s/libexec/lsiw/lsiw-binfmt", self);
                if (access(cand, X_OK) == 0) {
                    free(self);
                    return cand;
                }
                free(cand);
            }
        }
        free(self);
    }
    if (access(BINFMT_INTERP, X_OK) == 0) return lsiw_cli_strdup(BINFMT_INTERP);
    return NULL;
}

/* Make sure binfmt_misc is mounted (systemd mounts it on first access
 * through an automount; elsewhere it may need mounting). */
static int binfmt_mount(void)
{
    struct stat st;
    if (stat(BINFMT_DIR "/register", &st) == 0) return 0;
    if (stat(BINFMT_DIR, &st) < 0) {
        lsiw_log("%s does not exist: this kernel has no binfmt_misc support\n", BINFMT_DIR);
        return -1;
    }
    if (mount("binfmt_misc", BINFMT_DIR, "binfmt_misc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0 &&
        errno != EBUSY) {
        lsiw_log("cannot mount binfmt_misc on %s: %s\n", BINFMT_DIR, strerror(errno));
        return -1;
    }
    if (stat(BINFMT_DIR "/register", &st) < 0) {
        lsiw_log("binfmt_misc is not available: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Control files under /proc take one write() each. */
static int proc_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    ssize_t w;
    do w = write(fd, text, len); while (w < 0 && errno == EINTR);
    int e = errno;
    if (close(fd) < 0 && w >= 0) { e = errno; w = -1; }
    if (w < 0 || (size_t)w != len) {
        errno = w < 0 ? e : EIO;
        return -1;
    }
    return 0;
}

int lsiw_register_system(void)
{
    if (need_root("register") < 0) return -1;
    char *interp = find_interpreter();
    if (!interp) {
        char *self = lsiw_cli_readlink("/proc/self/exe");
        lsiw_log("cannot find the binfmt interpreter %s (a symlink to lsiw that 'make install'\n"
                 "creates). Create it with:\n"
                 "    sudo mkdir -p /usr/libexec/lsiw && sudo ln -s %s %s\n",
                 BINFMT_INTERP, self ? self : "/usr/bin/lsiw", BINFMT_INTERP);
        free(self);
        return -1;
    }
    /* The register string uses ':' as separator; the kernel keeps at most
     * 255 bytes of interpreter path. */
    if (strchr(interp, ':') || strlen(interp) > 255) {
        lsiw_log("the interpreter path %s cannot be used by binfmt_misc (contains ':' or is too long)\n", interp);
        free(interp);
        return -1;
    }

    int rc = 0;
    char *line = lsiw_cli_printf(":" BINFMT_NAME ":M::MZ::%s:", interp);
    char *conf = lsiw_cli_printf(
        "# LSIW: run Windows programs (files that start with \"MZ\") with lsiw.\n"
        "# The zz- prefix makes systemd-binfmt load this after Wine's wine.conf;\n"
        "# the binfmt_misc entry registered last wins.\n"
        "# Remove with: sudo lsiw unregister --system\n"
        "%s\n", line);
    if (lsiw_mkdirs("/etc/binfmt.d", 0755) < 0) {
        lsiw_log("cannot create /etc/binfmt.d: %s\n", strerror(errno));
        rc = -1;
    } else if (lsiw_cli_write_file(BINFMT_CONF, conf, strlen(conf), 0644) < 0) {
        lsiw_log("cannot write %s: %s\n", BINFMT_CONF, strerror(errno));
        rc = -1;
    } else {
        printf("Wrote %s (registered at every boot)\n", BINFMT_CONF);
    }

    if (binfmt_mount() < 0) {
        rc = -1;
    } else {
        /* Re-registering moves the entry to the front again, ahead of any
         * handler registered since (e.g. Wine's after a package update). */
        if (exists(BINFMT_DIR "/" BINFMT_NAME) && proc_write(BINFMT_DIR "/" BINFMT_NAME, "-1") < 0) {
            lsiw_log("cannot remove the old %s entry: %s\n", BINFMT_NAME, strerror(errno));
            rc = -1;
        } else if (proc_write(BINFMT_DIR "/register", line) < 0) {
            lsiw_log("cannot register with binfmt_misc: %s\n", strerror(errno));
            rc = -1;
        } else {
            printf("Registered %s with binfmt_misc: Windows programs now run with %s\n", BINFMT_NAME, interp);
            printf("A program needs execute permission to start directly: chmod +x program.exe && ./program.exe\n");
        }
    }
    free(conf);
    free(line);
    free(interp);
    return rc;
}

int lsiw_unregister_system(void)
{
    if (need_root("unregister") < 0) return -1;
    int rc = 0;
    const char *vendor = vendor_binfmt_file();
    if (vendor) {
        /* The installed rule would come back at the next boot: mask it. */
        if (is_mask(BINFMT_CONF)) {
            printf("%s is already disabled at boot (%s -> /dev/null)\n", vendor, BINFMT_CONF);
        } else if (lsiw_mkdirs("/etc/binfmt.d", 0755) < 0 ||
                   (unlink(BINFMT_CONF) < 0 && errno != ENOENT) ||
                   symlink("/dev/null", BINFMT_CONF) < 0) {
            lsiw_log("cannot mask %s with %s: %s\n", vendor, BINFMT_CONF, strerror(errno));
            rc = -1;
        } else {
            printf("Disabled %s at boot (%s -> /dev/null; 'lsiw register --system' enables it again)\n",
                   vendor, BINFMT_CONF);
        }
    } else if (unlink(BINFMT_CONF) == 0) {
        printf("Removed %s\n", BINFMT_CONF);
    } else if (errno == ENOENT) {
        printf("%s was not installed\n", BINFMT_CONF);
    } else {
        lsiw_log("cannot remove %s: %s\n", BINFMT_CONF, strerror(errno));
        rc = -1;
    }

    if (exists(BINFMT_DIR "/" BINFMT_NAME)) {
        if (proc_write(BINFMT_DIR "/" BINFMT_NAME, "-1") < 0) {
            lsiw_log("cannot remove the %s binfmt_misc entry: %s\n", BINFMT_NAME, strerror(errno));
            rc = -1;
        } else {
            printf("Removed the %s binfmt_misc entry (other handlers for .exe files, such as Wine's, apply again)\n",
                   BINFMT_NAME);
        }
    } else {
        printf("No %s binfmt_misc entry is active\n", BINFMT_NAME);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* lsiw info                                                            */
/* ------------------------------------------------------------------ */

/* "enabled"/"disabled" and the interpreter of a binfmt_misc entry. */
static int binfmt_entry(const char *name, char **interp, int *is_mz)
{
    char *path = lsiw_cli_printf(BINFMT_DIR "/%s", name);
    char *text = lsiw_cli_read_file(path, NULL);
    free(path);
    if (!text) return -1;
    int enabled = !strncmp(text, "enabled", 7);
    if (interp) {
        char *p = strstr(text, "interpreter ");
        *interp = NULL;
        if (p) {
            p += 12;
            *interp = strndup(p, strcspn(p, "\n"));
        }
    }
    if (is_mz) {
        char *off = strstr(text, "offset ");
        *is_mz = strstr(text, "magic 4d5a") != NULL && (!off || atoi(off + 7) == 0);
    }
    free(text);
    return enabled;
}

static char *mime_default(const char *type)
{
    char *probe = lsiw_cli_which("xdg-mime");
    if (probe) {
        free(probe);
        char *argv[] = { "xdg-mime", "query", "default", (char *)type, NULL };
        int st;
        char *out = lsiw_cli_capture(argv, NULL, &st);
        if (out) out[strcspn(out, "\n")] = 0;
        if (out && st == 0 && *out) return out;
        free(out);
        return NULL;
    }
    /* No xdg-utils: look at the user's own list only. */
    char *cfg = xdg_home("XDG_CONFIG_HOME", ".config");
    char *path = lsiw_cli_printf("%s/mimeapps.list", cfg);
    free(cfg);
    char *text = lsiw_cli_read_file(path, NULL);
    free(path);
    if (!text) return NULL;
    char *res = NULL, *save = NULL;
    int group = 0;
    size_t tl = strlen(type);
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (*line == '[') { group = !strncmp(line, "[Default Applications]", 22); continue; }
        if (group && !strncmp(line, type, tl) && line[tl] == '=') {
            res = strndup(line + tl + 1, strcspn(line + tl + 1, ";"));
            break;
        }
    }
    free(text);
    return res;
}

void lsiw_print_integration(int width)
{
    struct stat st;
    /* binfmt_misc */
    if (stat(BINFMT_DIR "/register", &st) < 0) {
        printf("%-*s not mounted, so no handler is active ('sudo lsiw register --system' sets it up)\n",
               width, "binfmt_misc:");
    } else {
        char *status = lsiw_cli_read_file(BINFMT_DIR "/status", NULL);
        int global_on = !status || !strncmp(status, "enabled", 7);
        free(status);
        char *interp = NULL;
        int en = binfmt_entry(BINFMT_NAME, &interp, NULL);
        if (en < 0)
            printf("%-*s LSIW handler not registered ('sudo lsiw register --system')\n", width, "binfmt_misc:");
        else
            printf("%-*s LSIW handler %s, interpreter %s%s\n", width, "binfmt_misc:",
                   en ? "enabled" : "disabled", interp ? interp : "?",
                   global_on ? "" : " (binfmt_misc itself is disabled)");
        free(interp);

        /* Other handlers for the same magic; the newest registration wins. */
        DIR *d = opendir(BINFMT_DIR);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                const char *n = de->d_name;
                if (n[0] == '.' || !strcmp(n, "register") || !strcmp(n, "status") || !strcmp(n, BINFMT_NAME))
                    continue;
                int mz = 0;
                char *other = NULL;
                int oe = binfmt_entry(n, &other, &mz);
                if (oe >= 0 && mz)
                    printf("%-*s also handles .exe: %s (%s, %s)\n", width, "", n,
                           oe ? "enabled" : "disabled", other ? other : "?");
                free(other);
            }
            closedir(d);
        }
    }
    const char *vendor = vendor_binfmt_file();
    if (is_mask(BINFMT_CONF))
        printf("%-*s disabled at boot (%s -> /dev/null)\n", width, "", BINFMT_CONF);
    else if (exists(BINFMT_CONF))
        printf("%-*s registered at boot by %s\n", width, "", BINFMT_CONF);
    else if (vendor)
        printf("%-*s registered at boot by %s\n", width, "", vendor);
    else
        printf("%-*s not registered at boot (no %s)\n", width, "", BINFMT_CONF);

    /* Desktop */
    char *path = desktop_path();
    if (!exists(path)) {
        char *sys = system_desktop_file();
        if (sys) {
            free(path);
            path = sys;
        }
    }
    char *def = mime_default(lsiw_mime_types[0]);
    printf("%-*s %s %s; .exe files open with %s\n", width, "Desktop:", path,
           exists(path) ? "installed" : "not installed ('lsiw register')", def ? def : "(no default)");
    free(def);
    free(path);
}
