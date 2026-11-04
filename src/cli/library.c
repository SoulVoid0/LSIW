/* lsiw cdrive, lsiw drives, lsiw prefix and lsiw programs: where C: is,
 * the extra drives (D:, E:, ...), the shared
 * prefix, and the library of Windows programs and games with their own
 * settings (engine, prefix, DXVK/VKD3D, arguments, environment, MangoHud,
 * debug logs, desktop shortcut).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include "wine.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static int save(void)
{
    if (lsiw_config_save() == 0) return 0;
    lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
    return 1;
}

static int is_help(const char *a) { return !strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "help"); }

/* ------------------------------------------------------------------ */
/* lsiw cdrive                                                          */
/* ------------------------------------------------------------------ */

const char lsiw_help_cdrive[] =
    "Usage: lsiw cdrive [show]\n"
    "       lsiw cdrive set PATH [--with-folders]\n"
    "       lsiw cdrive reset [--with-folders]\n"
    "\n"
    "C:\\ is one Linux folder, the same for every engine: LSIW's native engine,\n"
    "Wine and Proton (the shared prefix's drive_c is a link to it). It also shows\n"
    "every top-level Linux folder (C:\\home, C:\\mnt, ...) and the Windows folders\n"
    "(C:\\windows, C:\\Program Files, C:\\users, ...). Default: ~/.cdrive.\n"
    "\n"
    "  show    where C: is (default)\n"
    "  set     move C: to PATH, a new or empty folder. What the old C: folder\n"
    "          holds moves along (across disks it is copied and the original\n"
    "          is kept as <old>.old), and the old place becomes a link to the\n"
    "          new one. With --with-folders, the Windows folder, Program Files,\n"
    "          Program Files (x86) and ProgramData move into the new C: too.\n"
    "  reset   move C: back to ~/.cdrive\n";

int lsiw_cmd_cdrive(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "show";
    if (is_help(sub)) { fputs(lsiw_help_cdrive, stdout); return 0; }
    if (!strcmp(sub, "show")) {
        const char *set = lsiw_cdrive_setting();
        printf("C: drive: %s%s\n", lsiw_root_dir(), set ? "" : " (default)");
        printf("Shared prefix: %s (its drive_c is the C: drive)\n", lsiw_shared_dir());
        return 0;
    }
    int folders = 0, set = !strcmp(sub, "set");
    const char *path = NULL;
    if (!set && strcmp(sub, "reset")) {
        lsiw_log("unknown subcommand 'cdrive %s' (try 'lsiw cdrive --help')\n", sub);
        return 2;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--with-folders")) folders = 1;
        else if (argv[i][0] == '-') { lsiw_log("unknown option '%s'\n", argv[i]); return 2; }
        else if (set && !path) path = argv[i];
        else { lsiw_log("too many arguments (try 'lsiw cdrive --help')\n"); return 2; }
    }
    if (set && !path) { lsiw_log("usage: lsiw cdrive set PATH [--with-folders]\n"); return 2; }
    if (!set) path = "${HOME}/.cdrive";
    if (lsiw_cdrive_move(path, folders, 1) < 0) return 1;
    printf("C: drive: %s\n", lsiw_root_dir());
    return 0;
}

/* ------------------------------------------------------------------ */
/* lsiw drives                                                          */
/* ------------------------------------------------------------------ */

const char lsiw_help_drives[] =
    "Usage: lsiw drives [list]\n"
    "       lsiw drives add LETTER FOLDER\n"
    "       lsiw drives remove LETTER\n"
    "\n"
    "Besides C:, any Linux folder can be a drive of its own: after\n"
    "'lsiw drives add D ~/Games', D:\\ is ~/Games for LSIW's native engine, for\n"
    "Wine and for Proton (Steam games included). Any letter but C can be used\n"
    "(A to Z, 'D', 'd' or 'D:'). A folder that does not exist yet is created.\n"
    "The settings are [drives] d = FOLDER in the configuration.\n"
    "\n"
    "  list     C: and every extra drive (default)\n"
    "  add      make FOLDER the drive LETTER (replaces what LETTER was)\n"
    "  remove   LETTER is no drive any more (the folder itself is kept)\n";

static int drive_letter(const char *a)
{
    if (!a || !isalpha((unsigned char)a[0]) || (a[1] && (a[1] != ':' || a[2]))) return 0;
    return toupper((unsigned char)a[0]);
}

static void drives_apply(void)
{
    /* The shared prefix's dosdevices now; Wine and Proton prefixes of
     * programs follow when they next start. */
    lsiw_prefix_link_drives(lsiw_shared_dir(), 0);
}

int lsiw_cmd_drives(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (is_help(sub)) { fputs(lsiw_help_drives, stdout); return 0; }
    if (!strcmp(sub, "list") || !strcmp(sub, "show")) {
        printf("C:  %s%s\n", lsiw_root_dir(), lsiw_cdrive_setting() ? "" : " (default; move it with 'lsiw cdrive set')");
        for (int l = 'A'; l <= 'Z'; l++) {
            const char *p = l == 'C' ? NULL : lsiw_drive_path(l);
            if (!p) continue;
            printf("%c:  %s%s\n", l, p, is_dir(p) ? "" : " (does not exist yet)");
        }
        return 0;
    }
    if (!strcmp(sub, "add") || !strcmp(sub, "set")) {
        if (argc != 4) { lsiw_log("usage: lsiw drives add LETTER FOLDER\n"); return 2; }
        int l = drive_letter(argv[2]);
        if (!l || l == 'C') {
            lsiw_log("'%s' is not a drive letter LSIW can use (A to Z, but not C: move C: with 'lsiw cdrive set')\n", argv[2]);
            return 2;
        }
        const char *path = argv[3];
        char *abs = NULL;
        if (path[0] != '/' && path[0] != '~' && path[0] != '$') {
            char cwd[4096];
            if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
            abs = lsiw_strdup_printf("%s/%s", cwd, path);
            path = abs;
        }
        int r = lsiw_drive_set(l, path);
        free(abs);
        if (r == -2) { lsiw_log("'%s' is not a folder LSIW can use as a drive\n", argv[3]); return 2; }
        const char *p = lsiw_drive_path(l);
        if (p && lsiw_mkdirs(p, 0755) < 0) lsiw_log("warning: cannot create %s: %s\n", p, strerror(errno));
        if (save()) return 1;
        drives_apply();
        printf("%c: is now %s\n", l, p ? p : argv[3]);
        return 0;
    }
    if (!strcmp(sub, "remove") || !strcmp(sub, "rm")) {
        if (argc != 3) { lsiw_log("usage: lsiw drives remove LETTER\n"); return 2; }
        int l = drive_letter(argv[2]);
        if (!l || l == 'C') { lsiw_log("'%s' is not an extra drive\n", argv[2]); return 2; }
        if (!lsiw_drive_setting(l)) { lsiw_log("%c: is not a drive\n", l); return 1; }
        lsiw_drive_set(l, NULL);
        if (save()) return 1;
        drives_apply();
        printf("%c: removed (its folder is kept)\n", l);
        return 0;
    }
    lsiw_log("unknown subcommand 'drives %s' (try 'lsiw drives --help')\n", sub);
    return 2;
}

/* ------------------------------------------------------------------ */
/* lsiw prefix                                                          */
/* ------------------------------------------------------------------ */

const char lsiw_help_prefix[] =
    "Usage: lsiw prefix [status]\n"
    "\n"
    "Every Windows program and game uses one shared prefix by default, under\n"
    "Wine and under Proton, Steam games included: one registry, one set of\n"
    "installed runtimes, C:\\ being the C: drive. A program of the library can\n"
    "have a prefix of its own instead ('lsiw programs set ID prefix=own'), and\n"
    "so can a Steam game ('lsiw steam own APPID').\n"
    "\n"
    "  status  the shared prefix, the prefixes of programs, and prefixes of\n"
    "          earlier LSIW versions that are kept but no longer used\n"
    "\n"
    "The folder is [prefix] shared in the configuration (default:\n"
    "~/.local/share/lsiw/shared; the Wine prefix is its pfx folder).\n";

static void print_prefix(const char *label, const char *compat)
{
    char *pfx = lsiw_strdup_printf("%s/pfx", compat);
    char *reg = lsiw_strdup_printf("%s/system.reg", pfx);
    char *ver = lsiw_strdup_printf("%s/version", compat);
    char *drive = lsiw_strdup_printf("%s/drive_c", pfx);
    struct stat st;
    char *pv = lsiw_read_text(ver);
    if (pv) pv[strcspn(pv, "\n")] = 0;
    char *link = realpath(drive, NULL);
    printf("%-14s %s\n", label, compat);
    printf("%-14s %s%s%s%s\n", "", stat(reg, &st) == 0 ? "set up" : "not set up yet",
           pv ? "; last used by Proton " : "", pv ? pv : "",
           link && !strcmp(link, lsiw_root_dir()) ? "; C: is the C: drive" : "");
    free(link); free(pv); free(drive); free(ver); free(reg); free(pfx);
}

int lsiw_cmd_prefix(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (is_help(sub)) { fputs(lsiw_help_prefix, stdout); return 0; }
    if (strcmp(sub, "status")) {
        lsiw_log("unknown subcommand 'prefix %s' (try 'lsiw prefix --help')\n", sub);
        return 2;
    }
    print_prefix("Shared prefix:", lsiw_shared_dir());
    printf("%-14s %s\n", "C: drive:", lsiw_root_dir());
    struct lsiw_program *v;
    size_t n = lsiw_programs(&v);
    for (size_t i = 0; i < n; i++) {
        char *c = lsiw_program_compat_dir(&v[i]);
        if (!lsiw_is_shared_compat(c) && strcmp(c, lsiw_shared_dir())) {
            char *label = lsiw_strdup_printf("%s:", v[i].id);
            print_prefix(label, c);
            free(label);
        }
        free(c);
    }
    lsiw_programs_free(v, n);
    /* Prefixes of LSIW 1.5 and earlier: kept, never deleted by LSIW. */
    static const char *const old[] = { "proton-prefix", "prefix", NULL };
    for (int i = 0; old[i]; i++) {
        char *p = lsiw_strdup_printf("%s/%s", lsiw_data_dir(), old[i]);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
            printf("%-14s %s (from an earlier LSIW, no longer used; delete it when you no longer need it)\n",
                   "Old prefix:", p);
        free(p);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* lsiw programs                                                        */
/* ------------------------------------------------------------------ */

const char lsiw_help_programs[] =
    "Usage: lsiw programs [list]\n"
    "       lsiw programs show ID\n"
    "       lsiw programs add PROGRAM [--name NAME] [SETTING=VALUE...]\n"
    "       lsiw programs set ID SETTING=VALUE...\n"
    "       lsiw programs remove ID\n"
    "       lsiw programs run ID [ARGS...]\n"
    "       lsiw programs shortcut ID [--desktop] [--remove]\n"
    "       lsiw programs lwlnk ID [PATH] [--icon IMAGE]\n"
    "       lsiw programs scan [--add]\n"
    "       lsiw programs playtime [ID] [--reset]\n"
    "\n"
    "The library of Windows programs and games, each with its own settings.\n"
    "LSIW Control's Programs page shows the same library.\n"
    "\n"
    "Settings:\n"
    "  name=TEXT        the name shown\n"
    "  exe=PATH         the program (.exe, .msi, .bat or .lnk; Linux or C:\\ path)\n"
    "  args=ARGS        arguments, quoted as in a shell\n"
    "  workdir=PATH     the folder it starts in (default: its own folder)\n"
    "  engine=ENGINE    default (the default engine), auto, native, wine,\n"
    "                   wine:VERSION, proton, proton:VERSION or custom\n"
    "  prefix=PREFIX    shared (default: the one prefix everything uses), own\n"
    "                   (a prefix of its own), NAME (a named prefix programs can\n"
    "                   share) or a folder\n"
    "  dxvk=on|off|default    Direct3D 9-11 through Vulkan (DXVK). Proton: on\n"
    "                   by default; off uses WineD3D. Wine: off by default; on\n"
    "                   uses the DXVK of an installed Proton\n"
    "  vkd3d=on|off|default   Direct3D 12 through Vulkan (VKD3D-Proton), the same\n"
    "  mangohud=on|off  show the MangoHud overlay (Vulkan games; MANGOHUD=1)\n"
    "  debug=on|off     verbose LSIW, Wine and Proton logs (PROTON_LOG=1)\n"
    "  env:NAME=VALUE   an environment variable (env:NAME= removes it)\n"
    "  icon=FILE        icon of its shortcuts: an image (.png, .ico, .svg, ...)\n"
    "                   or an .exe/.dll to take it from (default: the icon\n"
    "                   inside the program)\n"
    "\n"
    "  shortcut  a launcher in the applications menu (and with --desktop on the\n"
    "            desktop) that runs 'lsiw programs run ID'\n"
    "  lwlnk     an LSIW shortcut (.lwlnk) on the desktop, or at PATH, with the\n"
    "            program's icon ('lsiw lwlnk create ID [PATH]'; see 'lsiw help\n"
    "            lwlnk')\n"
    "  scan      list the programs installed in the shared prefix (Start menu\n"
    "            shortcuts) that are not in the library; --add adds them\n"
    "  playtime  how long each program ran (or ID only), how often it was\n"
    "            started and when it was last played; --reset forgets it\n"
    "\n"
    "Play time: 'lsiw programs run', LSIW shortcuts (.lwlnk), LSIW Control and\n"
    "applications-menu shortcuts record how long a program runs. lsiw waits for\n"
    "it and saves the time every 30 seconds, so a crash or a power loss loses at\n"
    "most that much; time the computer sleeps does not count. Records are in\n"
    "~/.local/share/lsiw/playtime. Off: [programs] track_playtime = false or\n"
    "LSIW_NO_PLAYTIME=1. Programs started with 'lsiw run' are not counted.\n";

static struct lsiw_program *need_program(const char *id)
{
    struct lsiw_program *p = lsiw_program_get(id);
    if (!p) lsiw_log("no program '%s' in the library ('lsiw programs' lists them)\n", id);
    return p;
}

static const char *tri(int v) { return v < 0 ? "default" : v ? "on" : "off"; }

static int parse_tri(const char *v, int *out)
{
    if (!strcasecmp(v, "default") || !*v) *out = -1;
    else if (!strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes")) *out = 1;
    else if (!strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcmp(v, "0") || !strcasecmp(v, "no")) *out = 0;
    else return -1;
    return 0;
}

static void set_str(char **field, const char *v)
{
    free(*field);
    *field = lsiw_cli_strdup(v);
}

/* A program as a Linux path: C:\... and %VAR%\... are converted. */
static char *program_path(const char *v)
{
    if (strchr(v, '\\') || (isalpha((unsigned char)v[0]) && v[1] == ':') || v[0] == '%') return lsiw_win_to_linux(v);
    if (v[0] == '~' && v[1] == '/') return lsiw_cli_printf("%s%s", lsiw_home_dir(), v + 1);
    if (v[0] != '/') {
        char *cwd = getcwd(NULL, 0);
        char *r = lsiw_cli_printf("%s/%s", cwd ? cwd : "", v);
        free(cwd);
        return r;
    }
    return lsiw_cli_strdup(v);
}

/* Apply one SETTING=VALUE. Returns 0, or -1 with a message. */
static int apply_setting(struct lsiw_program *p, const char *arg)
{
    const char *eq = strchr(arg, '=');
    if (!eq) { lsiw_log("'%s' is not SETTING=VALUE\n", arg); return -1; }
    char *key = strndup(arg, (size_t)(eq - arg));
    const char *v = eq + 1;
    int rc = 0;
    if (!strcasecmp(key, "name")) set_str(&p->name, v);
    else if (!strcasecmp(key, "exe")) { char *x = program_path(v); set_str(&p->exe, x); free(x); }
    else if (!strcasecmp(key, "args")) set_str(&p->args, v);
    else if (!strcasecmp(key, "workdir")) {
        char *x = *v ? program_path(v) : lsiw_cli_strdup("");
        set_str(&p->workdir, x);
        free(x);
    }
    else if (!strcasecmp(key, "engine")) set_str(&p->engine, strcasecmp(v, "default") ? v : "");
    else if (!strcasecmp(key, "prefix")) set_str(&p->prefix, *v ? v : "shared");
    else if (!strcasecmp(key, "icon")) set_str(&p->icon, v);
    else if (!strcasecmp(key, "dxvk")) rc = parse_tri(v, &p->dxvk);
    else if (!strcasecmp(key, "vkd3d")) rc = parse_tri(v, &p->vkd3d);
    else if (!strcasecmp(key, "mangohud") || !strcasecmp(key, "debug")) {
        int t;
        rc = parse_tri(v, &t);
        if (!rc) *(!strcasecmp(key, "debug") ? &p->debug : &p->mangohud) = t == 1;
    }
    else if (!strncasecmp(key, "env:", 4) && key[4]) {
        /* Rebuild the NAME=value lines without this name, then add it. */
        const char *name = key + 4;
        size_t nl = strlen(name);
        char *out = lsiw_cli_strdup(""), *copy = lsiw_cli_strdup(p->env), *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(line, name, nl) && line[nl] == '=') continue;
            char *o = lsiw_cli_printf("%s%s\n", out, line);
            free(out);
            out = o;
        }
        if (*v) { char *o = lsiw_cli_printf("%s%s=%s\n", out, name, v); free(out); out = o; }
        free(copy);
        free(p->env);
        p->env = out;
    }
    else { lsiw_log("unknown setting '%s' (try 'lsiw programs --help')\n", key); rc = -1; }
    if (rc < 0 && strncasecmp(key, "env:", 4) && (!strcasecmp(key, "dxvk") || !strcasecmp(key, "vkd3d") ||
                                                  !strcasecmp(key, "mangohud") || !strcasecmp(key, "debug")))
        lsiw_log("%s: use on, off or default (not '%s')\n", key, v);
    free(key);
    return rc;
}

/* Play time: a block for `programs show`, one line per program for
 * `programs playtime`. */
static void print_playtime(const struct lsiw_program *p, int line)
{
    struct lsiw_playtime pt;
    lsiw_playtime_get(p->id, &pt);
    char *total = lsiw_playtime_format(pt.total), *last = lsiw_playtime_when(pt.last_end ? pt.last_end : pt.last_start);
    if (line) {
        printf("%-24s %-28.28s %12s %8lld  %s%s\n", p->id, p->name, total, pt.launches,
               pt.running ? "running now" : last, pt.running > 1 ? " (more than once)" : "");
    } else {
        printf("Play time: %s in %lld session%s%s\n", total, pt.launches, pt.launches == 1 ? "" : "s",
               pt.running ? " (running now)" : "");
        if (pt.launches) {
            char *first = lsiw_playtime_when(pt.first), *len = lsiw_playtime_format(pt.last_session);
            printf("Last:      %s, for %s (%s)\nFirst:     %s\n", last, len,
                   *pt.last_result ? pt.last_result : "unknown", first);
            if (pt.unclean)
                printf("Lost:      %lld session%s ended without LSIW seeing it (counted until the last save)\n",
                       pt.unclean, pt.unclean == 1 ? "" : "s");
            free(first);
            free(len);
        }
    }
    free(total);
    free(last);
}

static void print_program(const struct lsiw_program *p, int full)
{
    if (!full) {
        printf("%-24s %-28.28s %s\n", p->id, p->name, p->exe);
        return;
    }
    char *compat = lsiw_program_compat_dir(p);
    printf("ID:        %s\nName:      %s\nProgram:   %s\n", p->id, p->name, p->exe);
    printf("Arguments: %s\n", *p->args ? p->args : "(none)");
    printf("Starts in: %s\n", *p->workdir ? p->workdir : "its own folder");
    printf("Engine:    %s\n", *p->engine ? p->engine : "default (the default engine)");
    printf("Prefix:    %s (%s)\n", *p->prefix ? p->prefix : "shared", compat);
    printf("DXVK:      %s\nVKD3D:     %s\n", tri(p->dxvk), tri(p->vkd3d));
    printf("MangoHud:  %s\nDebug log: %s\n", p->mangohud ? "on" : "off", p->debug ? "on" : "off");
    if (*p->env) {
        printf("Environment:\n");
        char *copy = lsiw_cli_strdup(p->env), *save = NULL;
        for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) printf("  %s\n", l);
        free(copy);
    }
    char *df = lsiw_program_desktop_file(p);
    struct stat st;
    printf("Shortcut:  %s\n", stat(df, &st) == 0 ? df : "none ('lsiw programs shortcut ID')");
    free(df);
    free(compat);
    print_playtime(p, 0);
}

/* Split shell-quoted text into words ('...', "...", backslash). */
static char **split_args(const char *s, int *count)
{
    size_t cap = 8;
    int n = 0;
    char **v = calloc(cap, sizeof *v);
    const char *p = s;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *w = malloc(strlen(p) + 1), *o = w;
        char q = 0;
        while (*p && (q || !isspace((unsigned char)*p))) {
            if (q) {
                if (*p == q) { q = 0; p++; continue; }
                if (q == '"' && *p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
                *o++ = *p++;
            } else if (*p == '\'' || *p == '"') q = *p++;
            else if (*p == '\\' && p[1]) { p++; *o++ = *p++; }
            else *o++ = *p++;
        }
        *o = 0;
        if ((size_t)n + 2 > cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
        v[n++] = w;
    }
    v[n] = NULL;
    *count = n;
    return v;
}

/* Run the program in a child process and record the session's play time
 * here, the parent, which only waits: the native engine runs programs in
 * the process that loads them, and Wine and Proton replace it (exec), so
 * nothing is left to record the end otherwise. Ctrl+C from the terminal
 * reaches the program itself (same process group); SIGTERM and SIGHUP sent
 * to lsiw are passed on to it. */
static int run_tracked(struct lsiw_program *p, int argc, char **argv)
{
    /* A process group of its own (unless it runs in the foreground of a
     * terminal, where Ctrl+C must keep reaching it), so LSIW Control can
     * stop this program and everything it started, however it was started
     * (a .lwlnk, the applications menu, Launch). */
    pid_t fg = isatty(STDIN_FILENO) ? tcgetpgrp(STDIN_FILENO) : -1;
    if (fg < 0 || fg != getpgrp()) setpgid(0, 0);
    struct lsiw_session *s = lsiw_playtime_begin(p->id);
    if (!s) return lsiw_cli_run(argc, argv, p->engine);
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    sigprocmask(SIG_BLOCK, &set, &old);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        lsiw_playtime_end(s, "unknown");
        return lsiw_cli_run(argc, argv, p->engine);
    }
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        _exit(lsiw_cli_run(argc, argv, p->engine) & 0xff);
    }
    int status = 0;
    const char *t = getenv("LSIW_PLAYTIME_TICK");       /* tests */
    long tick = t && atol(t) > 0 ? atol(t) : LSIW_PLAYTIME_TICK;
    for (;;) {
        struct timespec ts = { (time_t)tick, 0 };
        siginfo_t si;
        int sig = sigtimedwait(&set, &si, &ts);
        if (sig < 0) {
            if (errno == EAGAIN) lsiw_playtime_tick(s);
            continue;
        }
        if (sig == SIGCHLD) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            continue;
        }
        /* Keys typed in the terminal already reached the program. */
        if ((sig == SIGINT || sig == SIGQUIT) && si.si_code == SI_KERNEL) continue;
        kill(pid, sig);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    char result[32];
    int rc;
    if (WIFSIGNALED(status)) {
        snprintf(result, sizeof result, "signal %d", WTERMSIG(status));
        rc = 128 + WTERMSIG(status);
    } else {
        rc = WEXITSTATUS(status);
        snprintf(result, sizeof result, "exit %d", rc);
    }
    lsiw_playtime_end(s, result);
    return rc;
}

int lsiw_cli_program_run(struct lsiw_program *p, int argc, char **argv)
{
    struct stat st;
    if (!*p->exe || stat(p->exe, &st) < 0) {
        lsiw_log("%s: the program %s is not there ('lsiw programs set %s exe=PATH')\n", p->id,
                 *p->exe ? p->exe : "(none)", p->id);
        return 127;
    }
    /* The environment first, so the program's own variables can override
     * what the switches below set only when unset. */
    char *copy = lsiw_cli_strdup(p->env), *save = NULL;
    for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(l, '=');
        if (!eq || eq == l) continue;
        *eq = 0;
        setenv(l, eq + 1, 1);
    }
    free(copy);
    if (p->dxvk == 0) { setenv("LSIW_DXVK", "0", 1); setenv("PROTON_USE_WINED3D", "1", 0); }
    else if (p->dxvk == 1) setenv("LSIW_DXVK", "1", 1);
    if (p->vkd3d == 0) { setenv("LSIW_VKD3D", "0", 1); setenv("PROTON_NO_D3D12", "1", 0); }
    else if (p->vkd3d == 1) setenv("LSIW_VKD3D", "1", 1);
    if (p->mangohud) setenv("MANGOHUD", "1", 0);
    if (p->debug) {
        lsiw_verbose = 1;
        setenv("LSIW_VERBOSE", "1", 1);
        setenv("PROTON_LOG", "1", 0);
        setenv("WINEDEBUG", "err+all,warn+module,+loaddll", 0);
        setenv("DXVK_LOG_LEVEL", "info", 0);
    }
    char *compat = lsiw_program_compat_dir(p);
    if (strcmp(compat, lsiw_shared_dir())) setenv("LSIW_PREFIX", compat, 1);
    else unsetenv("LSIW_PREFIX");
    free(compat);

    char *dir = *p->workdir ? lsiw_cli_strdup(p->workdir) : lsiw_cli_strdup(p->exe);
    if (!*p->workdir) { char *sl = strrchr(dir, '/'); if (sl && sl != dir) *sl = 0; }
    if (is_dir(dir) && chdir(dir) < 0) lsiw_log("cannot start in %s: %s\n", dir, strerror(errno));
    free(dir);

    int na;
    char **words = split_args(p->args, &na);
    char **av = calloc((size_t)(na + argc + 2), sizeof *av);
    int k = 0;
    av[k++] = p->exe;
    for (int i = 0; i < na; i++) av[k++] = words[i];
    for (int i = 0; i < argc; i++) av[k++] = argv[i];
    av[k] = NULL;
    if (lsiw_verbose) lsiw_log("starting %s (%s)\n", p->name, p->id);
    lsiw_cli_identity(p, NULL);
    return run_tracked(p, k, av);
}

static char *self_path(void)
{
    char *s = lsiw_cli_self_path();
    return s ? s : lsiw_cli_strdup("lsiw");
}

char *lsiw_cli_desktop_dir(void)
{
    char *conf = lsiw_cli_printf("%s/.config/user-dirs.dirs", lsiw_home_dir());
    char *text = lsiw_read_text(conf);
    free(conf);
    char *res = NULL;
    char *l = text ? strstr(text, "XDG_DESKTOP_DIR=\"") : NULL;
    if (l) {
        l += 17;
        char *e = strchr(l, '"');
        if (e) {
            *e = 0;
            res = !strncmp(l, "$HOME", 5) ? lsiw_cli_printf("%s%s", lsiw_home_dir(), l + 5) : lsiw_cli_strdup(l);
        }
    }
    free(text);
    return res ? res : lsiw_cli_printf("%s/Desktop", lsiw_home_dir());
}

static char *desktop_escape(const char *s)
{
    char *r = malloc(strlen(s) * 2 + 1), *o = r;
    for (; *s; s++) {
        if (*s == '\n') { *o++ = ' '; continue; }
        *o++ = *s;
    }
    *o = 0;
    return r;
}

static int program_shortcut(struct lsiw_program *p, int desktop, int remove)
{
    char *file = lsiw_program_desktop_file(p);
    char *dd = lsiw_cli_desktop_dir();
    char *on_desktop = lsiw_cli_printf("%s/lsiw-%s.desktop", dd, p->id);
    free(dd);
    int rc = 0;
    if (remove) {
        if (unlink(file) == 0) printf("removed %s\n", file);
        if (unlink(on_desktop) == 0) printf("removed %s\n", on_desktop);
        goto out;
    }
    /* The hidden entry a run made (lsiw_cli_identity) gives way to it. */
    char *hidden = lsiw_cli_strdup(file);
    char *dash = strrchr(hidden, '/');
    if (dash && !strncmp(dash + 1, "lsiw-", 5)) { dash[5] = '.'; unlink(hidden); }
    free(hidden);
    char *lsiw = self_path();
    char *name = desktop_escape(p->name);
    char *workdir = *p->workdir ? lsiw_cli_strdup(p->workdir) : lsiw_cli_strdup(p->exe);
    if (!*p->workdir) { char *sl = strrchr(workdir, '/'); if (sl && sl != workdir) *sl = 0; }
    /* The icon inside the program (or the one chosen), as a PNG. */
    char *icon = lsiw_program_icon(p, 1);
    /* How taskbars know the program's windows belong to this launcher. */
    char *wmclass = lsiw_cli_window_class(p->id);
    char *text = lsiw_cli_printf(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=%s\n"
        "Comment=Windows program, run by LSIW\n"
        "Exec=\"%s\" programs run %s\n"
        "Path=%s\n"
        "Icon=%s\n"
        "Terminal=false\n"
        "StartupNotify=true\n"
        "Categories=Wine;\n"
        "StartupWMClass=%s\n"
        "X-LSIW-Program=%s\n",
        name, lsiw, p->id, workdir, icon ? icon : *p->icon ? p->icon : "lsiw", wmclass, p->id);
    free(icon);
    free(wmclass);
    char *parent = lsiw_cli_strdup(file);
    *strrchr(parent, '/') = 0;
    lsiw_mkdirs(parent, 0755);
    free(parent);
    if (lsiw_cli_write_file(file, text, strlen(text), 0755) < 0) {
        lsiw_log("cannot write %s: %s\n", file, strerror(errno));
        rc = 1;
    } else printf("applications menu: %s\n", file);
    if (desktop && !rc) {
        char *ddir = lsiw_cli_strdup(on_desktop);
        *strrchr(ddir, '/') = 0;
        lsiw_mkdirs(ddir, 0755);
        free(ddir);
        if (lsiw_cli_write_file(on_desktop, text, strlen(text), 0755) < 0) {
            lsiw_log("cannot write %s: %s\n", on_desktop, strerror(errno));
            rc = 1;
        } else {
            /* GNOME starts desktop launchers only once they are trusted. */
            char *gio = lsiw_cli_which("gio");
            if (gio) {
                char *a[] = { gio, "set", on_desktop, "metadata::trusted", "true", NULL };
                lsiw_cli_spawn(a, NULL, 1);
                free(gio);
            }
            printf("desktop: %s\n", on_desktop);
        }
    }
    free(text); free(workdir); free(name); free(lsiw);
out:
    free(on_desktop);
    free(file);
    return rc;
}

/* Start menu shortcuts of the shared prefix that lead to a program. */
struct found { char *name, *exe; };

static void scan_dir(const char *dir, struct found **v, size_t *n, int depth)
{
    DIR *d = opendir(dir);
    if (!d || depth > 6) { if (d) closedir(d); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *path = lsiw_cli_printf("%s/%s", dir, de->d_name);
        if (is_dir(path)) scan_dir(path, v, n, depth + 1);
        else if (lsiw_cli_has_ext(path, ".lnk")) {
            struct lsiw_lnk l;
            if (lsiw_lnk_read(path, &l) == 0) {
                char *exe = l.target ? lsiw_win_to_linux(l.target) : NULL;
                struct stat st;
                char *lower = lsiw_cli_strdup(de->d_name);
                for (char *c = lower; *c; c++) *c = (char)tolower((unsigned char)*c);
                int skip = strstr(lower, "uninstal") || strstr(lower, "unins0") || strstr(lower, "readme");
                if (exe && !skip && stat(exe, &st) == 0 && S_ISREG(st.st_mode) && lsiw_cli_has_ext(exe, ".exe")) {
                    int dup = 0;
                    for (size_t i = 0; i < *n && !dup; i++) dup = !strcmp((*v)[i].exe, exe);
                    if (!dup) {
                        *v = realloc(*v, (*n + 1) * sizeof **v);
                        char *nm = lsiw_cli_strdup(de->d_name);
                        nm[strlen(nm) - 4] = 0;
                        (*v)[(*n)++] = (struct found){ nm, exe };
                        exe = NULL;
                    }
                }
                free(lower);
                free(exe);
                lsiw_lnk_free(&l);
            }
        }
        free(path);
    }
    closedir(d);
}

static int program_scan(int add)
{
    static const char *const vars[] = { "CSIDL_PROGRAMS", "CSIDL_COMMON_PROGRAMS", "CSIDL_DESKTOPDIRECTORY", NULL };
    struct found *v = NULL;
    size_t n = 0;
    for (int i = 0; vars[i]; i++) {
        const char *d = lsiw_var_linux(vars[i]);
        if (d) scan_dir(d, &v, &n, 0);
    }
    struct lsiw_program *lib;
    size_t nl = lsiw_programs(&lib);
    int shown = 0, added = 0;
    for (size_t i = 0; i < n; i++) {
        int have = 0;
        for (size_t j = 0; j < nl && !have; j++) have = !strcmp(lib[j].exe, v[i].exe);
        if (!have) {
            printf("%-32s %s\n", v[i].name, v[i].exe);
            shown++;
            if (add) {
                struct lsiw_program *p = lsiw_program_new(v[i].exe, v[i].name);
                lsiw_program_store(p);
                printf("  added as %s\n", p->id);
                lsiw_program_free(p);
                added++;
            }
        }
        free(v[i].name);
        free(v[i].exe);
    }
    free(v);
    lsiw_programs_free(lib, nl);
    if (!shown) printf("No installed program that is not in the library yet.\n");
    else if (!add) printf("'lsiw programs scan --add' adds them.\n");
    return added ? save() : 0;
}

static int program_playtime(int argc, char **argv)
{
    const char *id = NULL;
    int reset = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--reset")) reset = 1;
        else if (argv[i][0] == '-') { lsiw_log("unknown option '%s'\n", argv[i]); return 2; }
        else if (!id) id = argv[i];
        else { lsiw_log("usage: lsiw programs playtime [ID] [--reset]\n"); return 2; }
    }
    if (reset && !id) { lsiw_log("usage: lsiw programs playtime ID --reset\n"); return 2; }
    if (id) {
        struct lsiw_program *p = need_program(id);
        if (!p) return 1;
        if (reset) {
            lsiw_playtime_reset(p->id);
            printf("forgot the play time of %s\n", p->id);
        } else {
            printf("%s (%s)\n", p->name, p->id);
            print_playtime(p, 0);
        }
        lsiw_program_free(p);
        return 0;
    }
    struct lsiw_program *v;
    size_t n = lsiw_programs(&v);
    if (!n) printf("The library is empty ('lsiw programs add PROGRAM', or 'lsiw programs scan').\n");
    else printf("%-24s %-28s %12s %8s  %s\n", "ID", "NAME", "PLAY TIME", "LAUNCHES", "LAST PLAYED");
    for (size_t i = 0; i < n; i++) print_playtime(&v[i], 1);
    lsiw_programs_free(v, n);
    if (!lsiw_playtime_enabled()) printf("(play time tracking is off: [programs] track_playtime = false or LSIW_NO_PLAYTIME)\n");
    return 0;
}

int lsiw_cmd_programs(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (is_help(sub)) { fputs(lsiw_help_programs, stdout); return 0; }
    if (!strcmp(sub, "list")) {
        struct lsiw_program *v;
        size_t n = lsiw_programs(&v);
        if (!n) printf("The library is empty ('lsiw programs add PROGRAM', or 'lsiw programs scan').\n");
        for (size_t i = 0; i < n; i++) print_program(&v[i], 0);
        lsiw_programs_free(v, n);
        return 0;
    }
    if (!strcmp(sub, "scan")) return program_scan(argc > 2 && !strcmp(argv[2], "--add"));
    if (!strcmp(sub, "playtime")) return program_playtime(argc - 2, argv + 2);
    if (argc < 3) { lsiw_log("usage: lsiw programs %s %s\n", sub, strcmp(sub, "add") ? "ID" : "PROGRAM"); return 2; }
    if (!strcmp(sub, "add")) {
        const char *name = NULL;
        char *exe = program_path(argv[2]);
        struct stat st;
        if (stat(exe, &st) < 0) lsiw_log("warning: %s does not exist (yet)\n", exe);
        int i = 3;
        if (i + 1 < argc && !strcmp(argv[i], "--name")) { name = argv[i + 1]; i += 2; }
        struct lsiw_program *p = lsiw_program_new(exe, name);
        free(exe);
        for (; i < argc; i++) if (apply_setting(p, argv[i]) < 0) { lsiw_program_free(p); return 2; }
        lsiw_program_store(p);
        int rc = save();
        if (!rc) printf("added %s (%s)\n", p->id, p->name);
        lsiw_program_free(p);
        return rc;
    }
    struct lsiw_program *p = need_program(argv[2]);
    if (!p) return 1;
    int rc = 0;
    if (!strcmp(sub, "show")) print_program(p, 1);
    else if (!strcmp(sub, "set")) {
        for (int i = 3; i < argc && !rc; i++) if (apply_setting(p, argv[i]) < 0) rc = 2;
        if (!rc) { lsiw_program_store(p); rc = save(); free(lsiw_program_icon(p, 1)); }
        if (!rc) print_program(p, 1);
    } else if (!strcmp(sub, "remove")) {
        program_shortcut(p, 0, 1);
        char *df = lsiw_program_desktop_file(p);      /* and the hidden entry of its runs */
        char *sl = strrchr(df, '/');
        if (sl && !strncmp(sl + 1, "lsiw-", 5)) { sl[5] = '.'; unlink(df); }
        free(df);
        lsiw_program_remove(p->id);
        rc = save();
        if (!rc) lsiw_playtime_reset(p->id);
        if (!rc) printf("removed %s from the library (its files and prefix are left as they are)\n", p->id);
    } else if (!strcmp(sub, "run")) {
        rc = lsiw_cli_program_run(p, argc - 3, argv + 3);
    } else if (!strcmp(sub, "lwlnk")) {
        char **av = calloc((size_t)argc + 1, sizeof *av);
        int k = 0;
        av[k++] = "lwlnk";
        av[k++] = "create";
        for (int i = 2; i < argc; i++) av[k++] = argv[i];
        rc = lsiw_cmd_lwlnk(k, av);
        free(av);
    } else if (!strcmp(sub, "shortcut")) {
        int desktop = 0, remove = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--desktop")) desktop = 1;
            else if (!strcmp(argv[i], "--remove")) remove = 1;
            else { lsiw_log("unknown option '%s'\n", argv[i]); rc = 2; }
        }
        if (!rc) rc = program_shortcut(p, desktop, remove);
    } else {
        lsiw_log("unknown subcommand 'programs %s' (try 'lsiw programs --help')\n", sub);
        rc = 2;
    }
    lsiw_program_free(p);
    return rc;
}
