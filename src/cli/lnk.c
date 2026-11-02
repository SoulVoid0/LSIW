/* LSIW command-line program: Windows shortcuts (.lnk).
 *
 *   lsiw run LINK.lnk [ARGS...]     start what the shortcut points at, in
 *                                   its working directory, with its arguments
 *   lsiw lnk LINK.lnk               show what a shortcut holds
 *   lsiw lnk create LINK.lnk PROGRAM [ARGS...]   make one
 *
 * A shortcut made from the "Link to Windows Application..." template in a
 * file manager's Create New menu has no target yet; opening it asks for
 * the program (with zenity or kdialog) and saves it into the shortcut.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "lsiw.h"
#include "wine.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char lsiw_help_lnk[] =
    "Usage: lsiw lnk LINK.lnk\n"
    "       lsiw lnk create LINK.lnk PROGRAM [ARGS...] [--workdir DIR] [--description TEXT]\n"
    "       lsiw lnk set LINK.lnk [PROGRAM]\n"
    "\n"
    "Windows shortcuts (.lnk files). 'lsiw run LINK.lnk' (or opening one in\n"
    "the file manager) starts the program it points at, in its working\n"
    "directory and with its arguments.\n"
    "\n"
    "  lsiw lnk LINK.lnk   show the target, arguments, working directory,\n"
    "                      description and icon, with their Linux paths\n"
    "  create              make a shortcut; PROGRAM and DIR may be Linux or\n"
    "                      Windows paths (the shortcut stores Windows paths,\n"
    "                      so it also works on Windows)\n"
    "  set                 change the program of a shortcut, keeping the rest;\n"
    "                      without PROGRAM, a file chooser asks for it\n";

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

/* A Windows path for something the person typed: Linux paths (absolute,
 * relative or ~/) are converted, Windows paths are kept. */
static char *as_windows_path(const char *s)
{
    if (s[0] && s[1] == ':') return lsiw_cli_strdup(s);
    if (s[0] == '%') return lsiw_cli_strdup(s);
    char *abs;
    if (s[0] == '/') abs = lsiw_cli_strdup(s);
    else if (s[0] == '~' && s[1] == '/') abs = lsiw_cli_printf("%s%s", lsiw_home_dir(), s + 1);
    else {
        char *cwd = getcwd(NULL, 0);
        abs = lsiw_cli_printf("%s/%s", cwd ? cwd : ".", s);
        free(cwd);
    }
    char *real = realpath(abs, NULL);
    char *w = lsiw_linux_to_win(real ? real : abs);
    free(real);
    free(abs);
    return w ? w : lsiw_cli_strdup(s);
}

/* One argument quoted the way CommandLineToArgvW reads it back. */
static void append_quoted(char **cmd, const char *a)
{
    int plain = *a != 0;
    for (const char *c = a; *c && plain; c++) plain = *c != ' ' && *c != '\t' && *c != '"';
    size_t l = strlen(*cmd), n = l + 3 + 2 * strlen(a) + 2;
    char *r = malloc(n), *o = r;
    if (!r) abort();
    memcpy(o, *cmd, l);
    o += l;
    if (l) *o++ = ' ';
    if (plain) { strcpy(o, a); free(*cmd); *cmd = r; return; }
    *o++ = '"';
    for (const char *c = a;; c++) {
        size_t bs = 0;
        while (*c == '\\') { bs++; c++; }
        if (!*c) { for (size_t i = 0; i < 2 * bs; i++) *o++ = '\\'; break; }
        if (*c == '"') { for (size_t i = 0; i < 2 * bs + 1; i++) *o++ = '\\'; }
        else for (size_t i = 0; i < bs; i++) *o++ = '\\';
        *o++ = *c;
    }
    *o++ = '"';
    *o = 0;
    free(*cmd);
    *cmd = r;
}

/* The program for a shortcut, from a file chooser. Caller frees; NULL if
 * none was chosen or no chooser is installed. */
static char *choose_program(const char *link)
{
    char *title = lsiw_cli_printf("Choose the Windows program \"%s\" opens", link);
    char *out = NULL;
    int st = -1;
    char *zen = lsiw_cli_which("zenity");
    char *kd = zen ? NULL : lsiw_cli_which("kdialog");
    if (zen) {
        char *argv[] = { zen, "--file-selection", "--title", title,
                         "--file-filter=Windows programs | *.exe *.EXE *.msi *.MSI *.bat *.BAT *.cmd *.com",
                         "--file-filter=All files | *", NULL };
        out = lsiw_cli_capture(argv, NULL, &st);
    } else if (kd) {
        char *argv[] = { kd, "--title", title, "--getopenfilename", (char *)lsiw_home_dir(),
                         "Windows programs (*.exe *.EXE *.msi *.MSI *.bat *.cmd *.com)", NULL };
        out = lsiw_cli_capture(argv, NULL, &st);
    } else lsiw_log("%s has no program yet: set one with 'lsiw lnk set %s PROGRAM'\n", link, link);
    free(zen);
    free(kd);
    free(title);
    if (out) out[strcspn(out, "\n")] = 0;
    if (st != 0 || !out || !*out) { free(out); return NULL; }
    return out;
}

/* Give a template shortcut (no target) its program. 0 on success. */
static int fill_template(const char *link, struct lsiw_lnk *l, const char *program)
{
    char *chosen = program ? NULL : choose_program(link);
    if (!program && !chosen) return -1;
    const char *p = program ? program : chosen;
    free(l->target);
    l->target = as_windows_path(p);
    free(l->relative_path);
    l->relative_path = NULL;
    if (!l->workdir || !*l->workdir) {
        free(l->workdir);
        char *w = lsiw_cli_strdup(l->target);
        char *bs = strrchr(w, '\\');
        if (bs && bs > w + 2) *bs = 0; else if (bs) bs[1] = 0;
        l->workdir = w;
    }
    free(chosen);
    if (lsiw_lnk_write(link, l) < 0) {
        lsiw_log("cannot write %s: %s\n", link, strerror(errno));
        return -1;
    }
    printf("%s now opens %s\n", link, l->target);
    return 0;
}

int lsiw_lnk_command(const char *link, int argc, char **argv, char ***out_argv, char **out_cmdline)
{
    struct lsiw_lnk l;
    if (lsiw_lnk_read(link, &l) < 0) {
        lsiw_log("%s: %s\n", link, errno == EINVAL ? "not a Windows shortcut" : strerror(errno));
        return -1;
    }
    if (!l.target || !*l.target) {
        /* A new shortcut from the Create New menu: ask what it opens. */
        if (fill_template(link, &l, NULL) < 0) { lsiw_lnk_free(&l); return -1; }
    }
    char *target = lsiw_win_to_linux(l.target);
    if (!target || !is_file(target)) {
        lsiw_log("%s points at %s (%s), which does not exist\n", link, l.target, target ? target : "?");
        free(target);
        lsiw_lnk_free(&l);
        errno = ENOENT;
        return -1;
    }
    if (l.workdir && *l.workdir) {
        char *wd = lsiw_win_to_linux(l.workdir);
        if (!wd || chdir(wd) < 0)
            lsiw_log("warning: the shortcut's working directory %s is missing; using %s\n", l.workdir,
                     "the current one");
        free(wd);
    } else {
        /* Windows starts a shortcut without one in the program's folder. */
        char *dir = lsiw_cli_strdup(target);
        char *s = strrchr(dir, '/');
        if (s && s != dir) { *s = 0; if (chdir(dir) < 0) { /* keep the current one */ } }
        free(dir);
    }
    /* The command line exactly as Windows builds it: the target, the
     * shortcut's arguments verbatim, then any files handed over. */
    char *cmd = lsiw_cli_strdup("");
    append_quoted(&cmd, l.target);
    if (l.args && *l.args) {
        char *c = lsiw_cli_printf("%s %s", cmd, l.args);
        free(cmd);
        cmd = c;
    }
    char **av = calloc((size_t)argc + 2, sizeof *av);
    if (!av) abort();
    av[0] = target;
    for (int i = 0; i < argc; i++) {
        struct stat st;
        char *a = argv[i][0] == '/' && stat(argv[i], &st) == 0 ? as_windows_path(argv[i]) : lsiw_cli_strdup(argv[i]);
        append_quoted(&cmd, a);
        av[i + 1] = a;
    }
    *out_argv = av;
    *out_cmdline = cmd;
    lsiw_lnk_free(&l);
    return argc + 1;
}

static void show_path(const char *label, const char *win)
{
    if (!win || !*win) return;
    char *lin = lsiw_win_to_linux(win);
    printf("%-18s %s\n", label, win);
    if (lin && strcmp(lin, win)) printf("%-18s %s\n", "", lin);
    free(lin);
}

int lsiw_cmd_lnk(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        fputs(lsiw_help_lnk, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    if (!strcmp(argv[1], "create") || !strcmp(argv[1], "set")) {
        int create = !strcmp(argv[1], "create");
        if (argc < 3 || (create && argc < 4)) {
            fputs(lsiw_help_lnk, stderr);
            return 2;
        }
        const char *link = argv[2];
        struct lsiw_lnk l;
        if (create) memset(&l, 0, sizeof l);
        else if (lsiw_lnk_read(link, &l) < 0) {
            lsiw_log("%s: %s\n", link, errno == EINVAL ? "not a Windows shortcut" : strerror(errno));
            return 1;
        }
        const char *program = argc > 3 ? argv[3] : NULL;
        char *args = NULL;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--workdir") && i + 1 < argc) {
                free(l.workdir);
                l.workdir = as_windows_path(argv[++i]);
            } else if (!strcmp(argv[i], "--description") && i + 1 < argc) {
                free(l.description);
                l.description = lsiw_cli_strdup(argv[++i]);
            } else {
                if (!args) args = lsiw_cli_strdup("");
                append_quoted(&args, argv[i]);
            }
        }
        if (args) { free(l.args); l.args = args; }
        if (program) { free(l.target); l.target = NULL; }
        if (create && !program) { fputs(lsiw_help_lnk, stderr); return 2; }
        int rc = fill_template(link, &l, program);
        lsiw_lnk_free(&l);
        return rc < 0 ? 1 : 0;
    }
    struct lsiw_lnk l;
    if (lsiw_lnk_read(argv[1], &l) < 0) {
        lsiw_log("%s: %s\n", argv[1], errno == EINVAL ? "not a Windows shortcut" : strerror(errno));
        return 1;
    }
    printf("Shortcut:          %s\n", argv[1]);
    if (l.target && *l.target) show_path("Target:", l.target);
    else printf("Target:            none yet (opening it asks for the program)\n");
    if (l.args && *l.args) printf("Arguments:         %s\n", l.args);
    show_path("Start in:", l.workdir);
    if (l.description && *l.description) printf("Description:       %s\n", l.description);
    if (l.icon && *l.icon) printf("Icon:              %s,%d\n", l.icon, l.icon_index);
    printf("Window:            %s\n", l.show == 3 ? "maximized" : l.show == 7 ? "minimized" : "normal");
    lsiw_lnk_free(&l);
    return 0;
}
