/* lsiw: the LSIW command-line program.
 *
 *   lsiw run PROGRAM.exe [ARGS...]   run a Windows program
 *   lsiw PROGRAM.exe [ARGS...]       the same, shorter
 *   lsiw check | config | path | sync | info | register | unregister |
 *        wine-setup | help
 *
 * Windows programs run with LSIW's own native engine (lsiw_native_run):
 * its PE loader and its Win32 implementation, in this process. Wine is used
 * only when the native engine cannot run a program: 32-bit programs,
 * programs that import functions LSIW does not implement yet, installers
 * (.msi) and batch files.
 *
 * Installed as /usr/libexec/lsiw/lsiw-binfmt (a symlink to lsiw), this is
 * also the binfmt_misc interpreter: the kernel starts
 * "lsiw-binfmt /path/to/program.exe ARGS..." when a Windows program is
 * executed directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include "wine.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exit codes of lsiw itself, following the shell's conventions. A program
 * that runs passes its own exit code through. */
enum {
    EXIT_FAIL       = 1,
    EXIT_USAGE      = 2,
    EXIT_CANNOT_RUN = 126,
    EXIT_NOT_FOUND  = 127
};

enum engine { ENGINE_AUTO, ENGINE_NATIVE, ENGINE_WINE, ENGINE_PROTON, ENGINE_CUSTOM };
static const char *const engine_names[] = { "auto", "native", "wine", "proton", "custom" };
#define N_ENGINES 5

/* --engine given before the command (lsiw --engine=wine prog.exe). */
static int g_engine = -1;

/* ------------------------------------------------------------------ */
/* Help texts                                                           */
/* ------------------------------------------------------------------ */

static const char help_main[] =
    "Usage: lsiw [OPTIONS] COMMAND [ARGS...]\n"
    "       lsiw [OPTIONS] PROGRAM.exe [ARGS...]   (same as 'lsiw run PROGRAM.exe ...')\n"
    "\n"
    "LSIW (Linux Subsystem for Improved Wine) runs 64-bit Windows programs on\n"
    "Linux with its own native engine. Wine is used only as a fallback for\n"
    "programs the native engine cannot run (yet). C:\\ is one folder you choose\n"
    "(~/.cdrive by default) that also shows every Linux folder:\n"
    "C:\\home\\me\\file.txt is /home/me/file.txt. Every program and game, under Wine\n"
    "or Proton, shares one prefix unless you give it its own.\n"
    "\n"
    "Commands:\n"
    "  run PROGRAM [ARGS...]           run a Windows program\n"
    "  check PROGRAM                   can the native engine run a program?\n"
    "  config ...                      show or move Windows folders; pick the engine\n"
    "  path win|linux|expand ...       convert between Linux and Windows paths\n"
    "  sync                            rebuild the C:\\ view used by Wine\n"
    "  info                            versions, folders, Wine, desktop integration\n"
    "  register [--user|--system]      open/run .exe files with LSIW\n"
    "  unregister [--user|--system]    undo 'register'\n"
    "  wine-setup                      create or refresh the shared prefix for Wine\n"
    "  cdrive [show|set PATH|reset]    where the C: drive is\n"
    "  prefix [status]                 the shared prefix and the prefixes of programs\n"
    "  programs [list|add|set|run|...] the library of Windows programs and games\n"
    "  lnk LINK.lnk | lnk create ...   show or make a Windows shortcut\n"
    "  launch FILE.lwlnk [ARGS...]     start an LSIW shortcut (a program of the library)\n"
    "  lwlnk create ID | lwlnk FILE    make or show an LSIW shortcut (.lwlnk)\n"
    "  runners [list|install|remove]   the Wine and Proton versions LSIW can use\n"
    "  steam [status|add|remove]       \"LSIW Proton\" compatibility tools in Steam\n"
    "  help [COMMAND]                  show help for a command\n"
    "\n"
    "Options:\n"
    "  -e, --engine=ENGINE   engine for running a program: auto, native,\n"
    "                        wine[:VERSION], proton[:VERSION] or custom\n"
    "  -v, --verbose         print what LSIW is doing\n"
    "  -h, --help            show this help\n"
    "      --version         show the version\n"
    "\n"
    "Run 'lsiw help COMMAND' or 'lsiw COMMAND --help' for details.\n";

static const char help_run[] =
    "Usage: lsiw run [OPTIONS] PROGRAM [ARGS...]\n"
    "       lsiw PROGRAM.exe [ARGS...]\n"
    "\n"
    "Run a Windows program. PROGRAM is a Linux path (~/Downloads/setup.exe) or a\n"
    "Windows path (C:\\home\\me\\Downloads\\setup.exe, %APPDATA%\\App\\app.exe; quote\n"
    "backslashes in the shell). As on Windows, \".exe\" may be left out. ARGS are\n"
    "passed to the program unchanged. PROGRAM may be a Windows shortcut (.lnk):\n"
    "its target starts in the shortcut's working directory, with its arguments.\n"
    "It may also be an LSIW shortcut (.lwlnk): the program of the library it\n"
    "names starts with its own settings ('lsiw launch').\n"
    "\n"
    "Options (before PROGRAM):\n"
    "  -e, --engine=ENGINE   auto, native, wine, proton or custom (default: 'lsiw\n"
    "                        config engine'); wine:VERSION or proton:VERSION also\n"
    "                        picks the version ('lsiw runners' lists them)\n"
    "  -v, --verbose         print what LSIW is doing\n"
    "  -h, --help            show this help\n"
    "\n"
    "Engines:\n"
    "  native  LSIW's own engine: loads the program into this process and serves\n"
    "          its Windows API calls with LSIW's implementation.\n"
    "  wine    Wine, in LSIW's shared prefix with the same C: drive and folders.\n"
    "          Every Wine and Proton version shares that prefix.\n"
    "  proton  Proton (Steam's, GE-Proton, ...), in the same shared prefix.\n"
    "  custom  the engines of 'lsiw config chain', in order: the first that can\n"
    "          run the program does (a default engine and up to 3 fallbacks).\n"
    "  auto    native when every function the program imports is implemented\n"
    "          natively ('lsiw check PROGRAM' lists what is missing), otherwise\n"
    "          Wine if it is installed. 32-bit programs, .msi packages and batch\n"
    "          files always go to Wine.\n"
    "\n"
    "Exit status: the program's exit code; 126 if it cannot be run, 127 if it\n"
    "(or Wine, when needed) is not found.\n";

static const char help_check[] =
    "Usage: lsiw check PROGRAM\n"
    "\n"
    "Inspect a Windows program without running it: its type, every function it\n"
    "imports (from itself and the DLLs it loads) that the native engine does not\n"
    "implement yet, and the engine 'lsiw run' would choose.\n"
    "\n"
    "Exit status: 0 if the native engine can run it entirely, 1 if not (missing\n"
    "functions, 32-bit program, ...), 2 if it is not found or not a program.\n";

static const char help_config[] =
    "Usage: lsiw config list [--all]\n"
    "       lsiw config get [--linux|--windows] NAME\n"
    "       lsiw config set NAME PATH\n"
    "       lsiw config reset NAME|--all\n"
    "       lsiw config engine [auto|native|wine|proton|custom]\n"
    "       lsiw config chain [ENGINE[,ENGINE...]]\n"
    "       lsiw config wine [VERSION|PATH|auto]\n"
    "       lsiw config proton [VERSION|FOLDER|auto]\n"
    "\n"
    "Every Windows folder (Documents, AppData, Program Files, the Windows folder\n"
    "...) is a variable whose Linux location you choose. Names are the ones\n"
    "Microsoft's USMT documents (CSIDL_PERSONAL, APPDATA, PROGRAMFILES, ...),\n"
    "case-insensitive, with or without %.\n"
    "\n"
    "  list     table of variables: name, kind, Linux location, Windows path;\n"
    "           '*' marks a moved variable. --all also lists the variables USMT\n"
    "           does not document (LOCALAPPDATA, PUBLIC, ...).\n"
    "  get      details of one variable; --linux or --windows prints only that\n"
    "           path, for scripts\n"
    "  set      move a folder. PATH is a Linux path (~/Docs, /data/docs) or a\n"
    "           Windows path (C:\\data\\docs). The folder is created if needed.\n"
    "  reset    put a variable (or every variable, --all) back to its default\n"
    "  engine   show or set the default engine of 'lsiw run'\n"
    "  chain    show or set the engines 'custom' tries, in order, separated by\n"
    "           commas: the default engine and up to 3 fallbacks, each one of\n"
    "           native, auto, wine, wine:VERSION, proton, proton:VERSION. A\n"
    "           fallback is used when the engine before it cannot run the\n"
    "           program (missing functions, not installed, wrong program type,\n"
    "           cannot start). Example: native,wine:wine-11.0,proton\n"
    "  wine     show or set the Wine version of the wine engine: an ID from\n"
    "           'lsiw runners', a wine binary, or 'auto' (wine, then wine64,\n"
    "           from $PATH). Every Wine version uses LSIW's one Wine prefix.\n"
    "  proton   show or set the Proton version of the proton engine: an ID\n"
    "           from 'lsiw runners', a Proton folder, steam:NAME for one of\n"
    "           Valve's Proton versions (steam:proton-stable, steam:proton_11,\n"
    "           steam:proton_experimental, ...; Steam downloads it when first\n"
    "           used), or 'auto' (the newest official Proton installed in Steam).\n"
    "           Every one runs in LSIW's Proton prefix with LSIW's folders.\n"
    "\n"
    "Without a subcommand, 'list' is shown.\n";

static const char help_path[] =
    "Usage: lsiw path win LINUXPATH...\n"
    "       lsiw path linux WINPATH...\n"
    "       lsiw path expand STRING...\n"
    "\n"
    "  win     Linux path -> the path Windows programs see (/etc -> C:\\etc)\n"
    "  linux   Windows path -> Linux path (any drive letter means C:; %VARIABLES%\n"
    "          and case-insensitive names are resolved)\n"
    "  expand  replace %VARIABLES% with the values Windows programs see\n";

static const char help_sync[] =
    "Usage: lsiw sync [-q|--quiet]\n"
    "\n"
    "Rebuild the C: drive (links to every top-level Linux directory and to the\n"
    "Windows folders such as C:\\windows) and create every configured folder\n"
    "that does not exist yet. 'lsiw run' does this automatically.\n";

static const char help_info[] =
    "Usage: lsiw info\n"
    "\n"
    "Show the LSIW version, the configuration file, LSIW's folders, the default\n"
    "engine, whether Wine is installed (and its version), the binfmt_misc\n"
    "handler and the desktop integration.\n";

static const char help_register[] =
    "Usage: lsiw register [--user] [--system]\n"
    "       lsiw unregister [--user] [--system]\n"
    "\n"
    "  --user    (default) install lsiw.desktop in ~/.local/share/applications\n"
    "            and make it the default application for Windows programs, so\n"
    "            file managers run .exe and .msi files with LSIW.\n"
    "  --system  (root) register LSIW with the kernel's binfmt_misc, so a\n"
    "            Windows program with execute permission runs directly\n"
    "            (./setup.exe). Writes /etc/binfmt.d/zz-lsiw.conf, which is\n"
    "            loaded after Wine's own handler, and registers it immediately.\n"
    "\n"
    "'unregister' removes what 'register' added.\n";

static const char help_wine_setup[] =
    "Usage: lsiw wine-setup [-q|--quiet]\n"
    "\n"
    "Create or refresh LSIW's shared prefix for Wine (Proton uses the same one):\n"
    "run wineboot, make C: the C: drive (with every Linux folder) with no other\n"
    "drive letters, and write LSIW's folder locations into the Wine registry.\n"
    "This happens automatically the first time a program needs Wine and\n"
    "whenever a folder moves.\n";

static const char help_help[] =
    "Usage: lsiw help [COMMAND]\n";

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static int usage_error(const char *cmd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int usage_error(const char *cmd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("lsiw: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\nTry 'lsiw help%s%s'.\n", cmd ? " " : "", cmd ? cmd : "");
    return EXIT_USAGE;
}

static int is_help(const char *a)
{
    return !strcmp(a, "-h") || !strcmp(a, "--help");
}

static int parse_engine(const char *s)
{
    for (int i = 0; i < N_ENGINES; i++)
        if (!strcasecmp(s, engine_names[i])) return i;
    return -1;
}

/* Default engine: [engine] default, or the older [runtime] backend key. */
static int configured_engine(void)
{
    const char *v = lsiw_setting_get("engine", "default", NULL);
    if (!v || !*v) v = lsiw_setting_get("runtime", "backend", NULL);
    if (!v || !*v) return ENGINE_AUTO;
    int e = parse_engine(v);
    if (e < 0) {
        lsiw_log("warning: unknown engine '%s' in %s; using auto\n", v, lsiw_config_path());
        return ENGINE_AUTO;
    }
    return e;
}

/* Parse "-e X", "--engine X" or "--engine=X" at argv[*i]. Returns 1 if it
 * was an engine option (stored in *engine), 0 if not, -1 on error. */
static int engine_option(int argc, char **argv, int *i, int *engine, const char *cmd)
{
    const char *a = argv[*i], *val = NULL;
    if (!strncmp(a, "--engine=", 9)) val = a + 9;
    else if (!strcmp(a, "--engine") || !strcmp(a, "-e")) {
        if (*i + 1 >= argc) {
            usage_error(cmd, "%s needs a value: auto, native, wine[:VERSION] or proton[:VERSION]", a);
            return -1;
        }
        val = argv[++*i];
    } else return 0;
    /* wine:VERSION and proton:VERSION pick a version for this run only. */
    char *name = lsiw_cli_strdup(val);
    char *colon = strchr(name, ':');
    if (colon) *colon++ = 0;
    int e = parse_engine(name);
    if (e < 0 || (colon && e != ENGINE_WINE && e != ENGINE_PROTON) || (colon && !*colon)) {
        usage_error(cmd, "unknown engine '%s' (use auto, native, wine[:VERSION] or proton[:VERSION])", val);
        free(name);
        return -1;
    }
    if (colon) {
        int kind = e == ENGINE_WINE ? LSIW_RUNNER_WINE : LSIW_RUNNER_PROTON;
        struct lsiw_runner *r = lsiw_runner_pick(kind, colon);
        if (!r && kind == LSIW_RUNNER_PROTON && lsiw_proton_official(colon, NULL, NULL)) {
            /* Not downloaded yet: lsiw_proton_run asks Steam for it. */
        } else if (!r) {
            usage_error(cmd, "no %s version '%s' is installed ('lsiw runners' lists them)",
                        engine_names[e], colon);
            free(name);
            return -1;
        }
        lsiw_runner_free(r);
        lsiw_setting_set("engine", engine_names[e], colon);   /* in memory, not saved */
    }
    free(name);
    *engine = e;
    return 1;
}

static char *current_dir(void)
{
    for (size_t cap = 256; cap <= (1u << 20); cap *= 2) {
        char *buf = malloc(cap);
        if (!buf) break;
        if (getcwd(buf, cap)) return buf;
        free(buf);
        if (errno != ERANGE) break;
    }
    return lsiw_cli_strdup("/");
}

/* An absolute, tidy Linux path: relative to the current directory, with
 * "." components and repeated slashes removed and ".." applied lexically
 * (the way Windows treats paths). Caller frees. */
static char *absolute_path(const char *p)
{
    char *raw;
    if (p[0] == '/') raw = lsiw_cli_strdup(p);
    else {
        char *cwd = current_dir();
        raw = lsiw_cli_printf("%s/%s", cwd, p);
        free(cwd);
    }
    char *out = malloc(strlen(raw) + 2);
    if (!out) {
        fputs("lsiw: out of memory\n", stderr);
        exit(EXIT_FAIL);
    }
    size_t o = 0;
    out[o++] = '/';
    for (char *s = raw; *s; ) {
        while (*s == '/') s++;
        if (!*s) break;
        char *e = s;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - s);
        if (l == 1 && s[0] == '.') { /* skip */ }
        else if (l == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 1 && out[o - 1] == '/') o--;
            while (o > 1 && out[o - 1] != '/') o--;
        } else {
            if (out[o - 1] != '/') out[o++] = '/';
            memcpy(out + o, s, l);
            o += l;
        }
        s = e;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    free(raw);
    return out;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Windows-only syntax: a drive letter, a leading backslash (\\server, \x). */
static int clearly_windows(const char *s)
{
    return (isalpha((unsigned char)s[0]) && s[1] == ':') || s[0] == '\\';
}

/* PROGRAM as typed -> absolute Linux path. Linux paths are taken as they
 * are; Windows paths go through lsiw_win_to_linux. As CreateProcess does,
 * ".exe" is tried when the name has no extension and does not exist.
 * linux_only: the name comes from the kernel (binfmt_misc). */
static char *resolve_program(const char *arg, int linux_only)
{
    char *path;
    if (linux_only || (!clearly_windows(arg) && file_exists(arg)) ||
        (!clearly_windows(arg) && !strchr(arg, '\\') && !strchr(arg, '%')))
        path = absolute_path(arg);
    else {
        path = lsiw_win_to_linux(arg);
        if (!path) path = absolute_path(arg);
    }
    if (!linux_only && !file_exists(path)) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (!strchr(base, '.')) {
            char *exe = lsiw_cli_printf("%s.exe", path);
            if (file_exists(exe)) {
                free(path);
                return exe;
            }
            free(exe);
        }
    }
    return path;
}

/* ------------------------------------------------------------------ */
/* Looking at a program file                                            */
/* ------------------------------------------------------------------ */

enum prog_kind {
    PROG_MISSING,      /* does not exist                               */
    PROG_UNREADABLE,   /* cannot be opened, or is a directory          */
    PROG_NOT_WINDOWS,  /* not a Windows program at all                 */
    PROG_DOS,          /* MZ without a PE header: DOS or 16-bit Windows */
    PROG_PE32,         /* 32-bit x86                                   */
    PROG_PE64,         /* 64-bit x86-64: what the native engine runs    */
    PROG_PE_OTHER,     /* another CPU (ARM64, ...)                      */
    PROG_MSI,          /* Windows Installer package                    */
    PROG_BATCH         /* .bat / .cmd                                  */
};

struct prog_info {
    int kind;
    int err;           /* errno for PROG_MISSING / PROG_UNREADABLE     */
    unsigned machine;
    int is_dll, is_dotnet, is_efi, subsystem;
};

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static ssize_t read_at(int fd, void *buf, size_t n, off_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, off + (off_t)got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Classify a file by its headers (IMAGE_DOS_HEADER, IMAGE_NT_HEADERS). */
static void probe_program(const char *path, struct prog_info *pi)
{
    memset(pi, 0, sizeof *pi);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        pi->err = errno;
        pi->kind = errno == ENOENT || errno == ENOTDIR ? PROG_MISSING : PROG_UNREADABLE;
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        pi->err = errno;
        pi->kind = PROG_UNREADABLE;
        close(fd);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        pi->err = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        pi->kind = PROG_UNREADABLE;
        close(fd);
        return;
    }
    unsigned char dos[64];
    ssize_t n = read_at(fd, dos, sizeof dos, 0);
    static const unsigned char ole[8] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 };

    if (lsiw_cli_has_ext(path, ".bat") || lsiw_cli_has_ext(path, ".cmd")) pi->kind = PROG_BATCH;
    else if (n >= 8 && !memcmp(dos, ole, 8) &&
             (lsiw_cli_has_ext(path, ".msi") || lsiw_cli_has_ext(path, ".msp"))) pi->kind = PROG_MSI;
    else if (n < 64 || dos[0] != 'M' || dos[1] != 'Z') pi->kind = PROG_NOT_WINDOWS;
    else {
        /* "PE\0\0", IMAGE_FILE_HEADER (20 bytes), then the optional header
         * up to and including the CLR data directory (index 14). */
        unsigned char nt[4 + 20 + 240];
        uint32_t lfanew = le32(dos + 0x3c);
        ssize_t m = lfanew > 0 ? read_at(fd, nt, sizeof nt, (off_t)lfanew) : -1;
        if (m < 26 || memcmp(nt, "PE\0\0", 4)) pi->kind = PROG_DOS;
        else {
            const unsigned char *fh = nt + 4, *opt = nt + 24;
            size_t optlen = (size_t)m - 24;
            pi->machine = le16(fh);
            pi->is_dll = (le16(fh + 18) & 0x2000) != 0;          /* IMAGE_FILE_DLL */
            uint16_t magic = le16(opt);
            if (optlen >= 70) pi->subsystem = le16(opt + 68);
            /* PE32+: NumberOfRvaAndSizes at 108, directories at 112;
             * PE32: at 92 and 96. */
            size_t nrva = magic == 0x20b ? 108 : 92, dirs = magic == 0x20b ? 112 : 96;
            if ((magic == 0x20b || magic == 0x10b) && optlen >= dirs + 15 * 8 && le32(opt + nrva) > 14)
                pi->is_dotnet = le32(opt + dirs + 14 * 8) != 0;
            /* IMAGE_SUBSYSTEM_EFI_*: firmware programs share the PE format
             * (and the "MZ" binfmt magic) but are not Windows programs. */
            if (pi->subsystem >= 10 && pi->subsystem <= 13) {
                pi->kind = PROG_NOT_WINDOWS;
                pi->is_efi = 1;
            } else if (pi->machine == 0x8664 && magic == 0x20b) pi->kind = PROG_PE64;
            else if (pi->machine == 0x14c) pi->kind = PROG_PE32;
            else pi->kind = PROG_PE_OTHER;
        }
    }
    close(fd);
}

static const char *machine_name(unsigned m)
{
    switch (m) {
    case 0x14c:  return "x86";
    case 0x8664: return "x86-64";
    case 0xaa64: return "ARM64";
    case 0x1c4:  return "ARM";
    case 0x200:  return "Itanium";
    default:     return "unknown CPU";
    }
}

/* One-line description of why a program needs Wine. */
static const char *wine_reason(const struct prog_info *pi)
{
    switch (pi->kind) {
    case PROG_PE32:  return "32-bit program";
    case PROG_DOS:   return "DOS or 16-bit Windows program";
    case PROG_MSI:   return "Windows Installer package";
    case PROG_BATCH: return "batch file";
    default:         return "not a 64-bit x86 program";
    }
}

static void describe_program(const struct prog_info *pi, char *buf, size_t n)
{
    static const char *const subsys[] = { "", "driver", "GUI", "console" };
    const char *ss = !pi->is_dll && pi->subsystem >= 1 && pi->subsystem <= 3 ? subsys[pi->subsystem] : "";
    switch (pi->kind) {
    case PROG_PE64:
    case PROG_PE32:
    case PROG_PE_OTHER:
        snprintf(buf, n, "%s Windows %s%s%s (%s)%s",
                 pi->kind == PROG_PE64 ? "64-bit" : pi->kind == PROG_PE32 ? "32-bit" : "Windows",
                 ss, *ss ? " " : "", pi->is_dll ? "DLL" : "program",
                 machine_name(pi->machine), pi->is_dotnet ? ", .NET" : "");
        if (pi->kind == PROG_PE_OTHER && !strncmp(buf, "Windows Windows", 15)) memmove(buf, buf + 8, strlen(buf + 8) + 1);
        break;
    case PROG_DOS:   snprintf(buf, n, "DOS or 16-bit Windows program"); break;
    case PROG_MSI:   snprintf(buf, n, "Windows Installer package"); break;
    case PROG_BATCH: snprintf(buf, n, "batch file"); break;
    default:         snprintf(buf, n, pi->is_efi ? "EFI firmware program, not a Windows program"
                                                 : "not a Windows program"); break;
    }
}

/* "kernel32.dll!Foo, user32.dll!Bar, d3d11.dll (+5 more)" from the
 * native check report (one problem per line). Caller frees. */
static char *summarize_report(const char *report)
{
    char *items[3] = { NULL, NULL, NULL };
    int count = 0, total = 0;
    const char *p = report ? report : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l) {
            char *line = strndup(p, l);
            if (!line) {
                fputs("lsiw: out of memory\n", stderr);
                exit(EXIT_FAIL);
            }
            /* "app.exe needs user32.dll!X (not implemented)",
             * "app.exe needs foo.dll, which is not available",
             * "app.exe: 32-bit program (...)" */
            char *item = line, *k;
            if ((k = strstr(line, " needs "))) item = k + 7;
            else if ((k = strstr(line, ": "))) item = k + 2;
            if ((k = strstr(item, " (not implemented)"))) *k = 0;
            if ((k = strstr(item, ", which is not available"))) *k = 0;
            int dup = 0;
            for (int i = 0; i < count; i++) if (!strcmp(items[i], item)) dup = 1;
            if (!dup) {
                total++;
                if (count < 3) items[count++] = lsiw_cli_strdup(item);
            }
            free(line);
        }
        if (!e) break;
        p = e + 1;
    }
    char *s = lsiw_cli_strdup(count ? "" : "the native engine cannot load it");
    for (int i = 0; i < count; i++) {
        char *t = lsiw_cli_printf("%s%s%s", s, i ? ", " : "", items[i]);
        free(s);
        free(items[i]);
        s = t;
    }
    if (total > count) {
        char *t = lsiw_cli_printf("%s (+%d more)", s, total - count);
        free(s);
        s = t;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* run                                                                  */
/* ------------------------------------------------------------------ */

static int wine_failed(void)
{
    return errno == ENOENT ? EXIT_NOT_FOUND : EXIT_CANNOT_RUN;
}

/* Engine 'auto': 0 = native, 1 = Wine, -1 = cannot run (message printed). */
static int choose_auto(const char *arg, const char *path, const struct prog_info *pi)
{
    const char *wine = lsiw_wine_binary();
    if (pi->kind == PROG_PE64) {
        char *report = NULL;
        int rc = lsiw_native_check(path, &report);
        if (rc == 0) {
            free(report);
            LSIW_TRACE("every import is implemented natively: using the native engine\n");
            return 0;
        }
        char *summary = summarize_report(report);
        free(report);
        int choice;
        if (wine) {
            fprintf(stderr, "lsiw: using the Wine fallback: %s\n", summary);
            choice = 1;
        } else if (rc < 0) {
            lsiw_log("%s cannot be run: %s; the Wine fallback is not installed (on Fedora: sudo dnf install wine)\n",
                     arg, summary);
            choice = -1;
        } else {
            lsiw_log("warning: not implemented natively yet: %s. Wine (the fallback) is not installed, so the "
                     "native engine runs the program anyway; it stops if it calls one of these.\n", summary);
            choice = 0;
        }
        free(summary);
        return choice;
    }
    if (pi->kind == PROG_NOT_WINDOWS) {
        lsiw_log("%s is not a Windows program%s\n", arg, pi->is_efi ? " (it is an EFI firmware program)" : "");
        return -1;
    }
    if (pi->kind == PROG_PE_OTHER) {
        lsiw_log("%s is a Windows program for %s processors; LSIW and Wine run x86 programs only\n",
                 arg, machine_name(pi->machine));
        return -1;
    }
    if (wine) {
        fprintf(stderr, "lsiw: using the Wine fallback: %s\n", wine_reason(pi));
        return 1;
    }
    lsiw_log("%s is a %s, which only the Wine fallback can run, and Wine is not installed.\n"
             "      Install it (on Fedora: sudo dnf install wine) and run the program again.\n",
             arg, wine_reason(pi));
    return -1;
}

/* A parent LSIW process passes a Windows program's exact command line in
 * LSIW_CMDLINE (the native engine uses it verbatim). Wine takes an argument
 * vector, so split it the way CommandLineToArgvW does; argv[0] stays. */
static void wine_args_from_cmdline(int *argc, char ***argv)
{
    const char *cmd = getenv("LSIW_CMDLINE");
    if (!cmd || !*cmd) return;
    size_t bl = strlen(cmd) + 1;
    int n = 1, cap = 16;
    char **out = malloc((size_t)cap * sizeof *out);
    out[0] = (*argv)[0];
    const char *p = cmd;
    int first = 1;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *arg = malloc(bl), *o = arg;
        int inq = 0;
        if (first) {            /* argv[0]: quotes only, no backslash escapes */
            if (*p == '"') { p++; while (*p && *p != '"') *o++ = *p++; if (*p) p++; }
            else while (*p && *p != ' ' && *p != '\t') *o++ = *p++;
        } else {
            while (*p && (inq || (*p != ' ' && *p != '\t'))) {
                int bs = 0;
                while (*p == '\\') { p++; bs++; }
                if (*p == '"') {
                    for (int i = 0; i < bs / 2; i++) *o++ = '\\';
                    if (bs % 2) { *o++ = '"'; p++; }
                    else { if (inq && p[1] == '"') { *o++ = '"'; p++; } else inq = !inq; p++; }
                } else {
                    for (int i = 0; i < bs; i++) *o++ = '\\';
                    if (*p && (inq || (*p != ' ' && *p != '\t'))) *o++ = *p++;
                }
            }
        }
        *o = 0;
        if (first) { free(arg); first = 0; continue; }
        if (n + 2 > cap) { cap *= 2; out = realloc(out, (size_t)cap * sizeof *out); }
        out[n++] = arg;
    }
    out[n] = NULL;
    unsetenv("LSIW_CMDLINE");
    *argc = n;
    *argv = out;
}

/* Engine 'custom': try the engines of [engine] chain in order. Each one
 * that cannot run the program (not installed, missing functions, wrong kind
 * of program, cannot be started) passes it on to the next. Returns only
 * when none could (or with the exit code of the native engine). */
static int chain_entry(const char *s, int quiet);

static int run_chain(int argc, char **argv, const char *arg, const char *path, const struct prog_info *pi)
{
    char **chain;
    size_t n = lsiw_engine_chain(&chain);
    int windows = pi->kind == PROG_PE64 || pi->kind == PROG_PE32 || pi->kind == PROG_MSI ||
                  pi->kind == PROG_BATCH || pi->kind == PROG_DOS;
    if (!windows) {
        lsiw_strv_free(chain);
        choose_auto(arg, path, pi);            /* says why */
        return EXIT_CANNOT_RUN;
    }
    for (size_t i = 0; i < n; i++) {
        int e = chain_entry(chain[i], 1);
        const char *colon = strchr(chain[i], ':');
        char *why = NULL;
        if (e < 0) why = lsiw_cli_strdup("not an engine");
        else if (e == ENGINE_NATIVE || e == ENGINE_AUTO) {
            int use_native = 0;
            if (pi->kind == PROG_PE64) {
                char *report = NULL;
                int rc = lsiw_native_check(path, &report);
                if (rc == 0) use_native = 1;
                else if (e == ENGINE_NATIVE) {
                    char *s = summarize_report(report);
                    why = lsiw_cli_printf("not implemented natively yet: %s", s);
                    free(s);
                }
                free(report);
            } else if (e == ENGINE_NATIVE) why = lsiw_cli_printf("it is a %s", wine_reason(pi));
            if (use_native) {
                fflush(NULL);
                char *nwhy = NULL;
                int rc = lsiw_native_run(path, argc, argv, &nwhy);
                if (!(rc == -1 && nwhy)) { free(nwhy); lsiw_strv_free(chain); return rc; }
                why = lsiw_cli_printf("the native engine cannot load it (%s)", nwhy);
                free(nwhy);
            } else if (e == ENGINE_AUTO) e = ENGINE_WINE;     /* auto: Wine next */
        }
        if (!why && e == ENGINE_WINE) {
            if (colon) lsiw_setting_set("engine", "wine", colon + 1);     /* in memory only */
            struct lsiw_runner *r = colon ? lsiw_runner_pick(LSIW_RUNNER_WINE, colon + 1) : NULL;
            if (colon && !r) why = lsiw_cli_printf("Wine %s is not installed", colon + 1);
            else if (!lsiw_wine_binary()) why = lsiw_cli_strdup("Wine is not installed");
            lsiw_runner_free(r);
            if (!why) {
                if (i) fprintf(stderr, "lsiw: trying %s\n", chain[i]);
                wine_args_from_cmdline(&argc, &argv);
                lsiw_wine_run(path, argc, argv, lsiw_verbose);     /* returns only on failure */
                why = lsiw_cli_strdup("Wine could not be started");
            }
        } else if (!why && e == ENGINE_PROTON) {
            if (colon) lsiw_setting_set("engine", "proton", colon + 1);
            if (!lsiw_proton_available()) why = lsiw_cli_strdup("that Proton is not installed");
            else {
                if (i) fprintf(stderr, "lsiw: trying %s\n", chain[i]);
                wine_args_from_cmdline(&argc, &argv);
                lsiw_proton_run(path, argc, argv, lsiw_verbose);
                why = lsiw_cli_strdup("Proton could not be started");
            }
        }
        fprintf(stderr, "lsiw: %s cannot run %s: %s%s\n", chain[i], arg, why,
                i + 1 < n ? "; trying the next engine" : "");
        free(why);
    }
    if (!n) lsiw_log("the custom engine has no engines ('lsiw config chain native,wine' sets them)\n");
    lsiw_strv_free(chain);
    return EXIT_CANNOT_RUN;
}

/* Run PROGRAM (argv[0]) with ARGS (argv[1..argc-1]). Returns the exit
 * status for this process. */
static int run_program(int argc, char **argv, int engine, int linux_only)
{
    const char *arg = argv[0];
    char *path = resolve_program(arg, linux_only);
    struct prog_info pi;
    probe_program(path, &pi);
    LSIW_TRACE("program: %s\n", path);

    if (pi.kind == PROG_MISSING) {
        lsiw_log("%s: no such file\n", arg);
        free(path);
        return EXIT_NOT_FOUND;
    }
    if (pi.kind == PROG_UNREADABLE) {
        lsiw_log("%s: %s\n", arg, strerror(pi.err));
        free(path);
        return EXIT_CANNOT_RUN;
    }
    if (pi.is_dll && (pi.kind == PROG_PE64 || pi.kind == PROG_PE32)) {
        lsiw_log("%s is a DLL, not a program\n", arg);
        free(path);
        return EXIT_CANNOT_RUN;
    }

    /* The C:\ view: Wine's C: drive, and where new folders are created. */
    if (lsiw_sync_root(0) < 0)
        lsiw_log("warning: the C:\\ view %s could not be built\n", lsiw_root_dir());

    if (engine == ENGINE_CUSTOM) {
        LSIW_TRACE("engine: custom chain\n");
        int rc = run_chain(argc, argv, arg, path, &pi);
        free(path);
        return rc;
    }

    if (engine == ENGINE_PROTON) {
        LSIW_TRACE("engine: proton\n");
        wine_args_from_cmdline(&argc, &argv);
        lsiw_proton_run(path, argc, argv, lsiw_verbose);   /* returns only on failure */
        free(path);
        return wine_failed();
    }

    int use_wine;
    if (engine == ENGINE_NATIVE) use_wine = 0;
    else if (engine == ENGINE_WINE) use_wine = 1;
    else if ((use_wine = choose_auto(arg, path, &pi)) < 0) {
        free(path);
        return EXIT_CANNOT_RUN;
    }
    LSIW_TRACE("engine: %s\n", use_wine ? "wine" : "native");

    if (use_wine) {
        wine_args_from_cmdline(&argc, &argv);
        lsiw_wine_run(path, argc, argv, lsiw_verbose);   /* returns only on failure */
        free(path);
        return wine_failed();
    }

    fflush(NULL);
    char *why = NULL;
    int rc = lsiw_native_run(path, argc, argv, &why);
    if (rc == -1 && why) {
        if (engine == ENGINE_AUTO && lsiw_wine_binary()) {
            fprintf(stderr, "lsiw: using the Wine fallback: the native engine cannot load %s (%s)\n", arg, why);
            free(why);
            wine_args_from_cmdline(&argc, &argv);
            lsiw_wine_run(path, argc, argv, lsiw_verbose);
            free(path);
            return wine_failed();
        }
        lsiw_log("cannot run %s: %s\n", arg, why);
        free(why);
        free(path);
        return EXIT_CANNOT_RUN;
    }
    free(why);
    free(path);
    return rc;
}

int lsiw_cli_run(int argc, char **argv, const char *engine)
{
    int e = g_engine >= 0 ? g_engine : configured_engine();
    if (engine && *engine && strcasecmp(engine, "default")) {
        int i = 0;
        char *opt = lsiw_cli_printf("--engine=%s", engine);
        char *av[] = { opt, NULL };
        int r = engine_option(1, av, &i, &e, "programs");
        free(opt);
        if (r < 0) return EXIT_USAGE;
    }
    if (lsiw_cli_has_ext(argv[0], ".lnk")) {
        char **av, *cmdline;
        int n = lsiw_lnk_command(argv[0], argc - 1, argv + 1, &av, &cmdline);
        if (n < 0) return EXIT_NOT_FOUND;
        setenv("LSIW_CMDLINE", cmdline, 1);
        free(cmdline);
        return run_program(n, av, e, 0);
    }
    return run_program(argc, argv, e, 0);
}

static int cmd_run(int argc, char **argv)
{
    int engine = g_engine >= 0 ? g_engine : configured_engine();
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        int r;
        if (is_help(a)) { fputs(help_run, stdout); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) lsiw_verbose = 1;
        else if ((r = engine_option(argc, argv, &i, &engine, "run")) < 0) return EXIT_USAGE;
        else if (r > 0) continue;
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-' && a[1]) return usage_error("run", "unknown option '%s'", a);
        else break;
    }
    if (i >= argc) return usage_error("run", "no program given");
    if (lsiw_cli_has_ext(argv[i], ".lwlnk")) return lsiw_launch_lwlnk(argv[i], argc - i - 1, argv + i + 1);
    if (lsiw_cli_has_ext(argv[i], ".lnk")) {
        /* A Windows shortcut: what it points at, as Windows starts it. */
        char **av, *cmdline;
        int n = lsiw_lnk_command(argv[i], argc - i - 1, argv + i + 1, &av, &cmdline);
        if (n < 0) return EXIT_NOT_FOUND;
        setenv("LSIW_CMDLINE", cmdline, 1);
        free(cmdline);
        return run_program(n, av, engine, 0);
    }
    return run_program(argc - i, argv + i, engine, 0);
}

/* ------------------------------------------------------------------ */
/* check                                                                */
/* ------------------------------------------------------------------ */

static int cmd_check(int argc, char **argv)
{
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (is_help(argv[i])) { fputs(help_check, stdout); return 0; }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) { lsiw_verbose = 1; continue; }
        if (arg) return usage_error("check", "only one program can be checked at a time");
        arg = argv[i];
    }
    if (!arg) return usage_error("check", "no program given");

    char *path = resolve_program(arg, 0);
    struct prog_info pi;
    probe_program(path, &pi);
    if (pi.kind == PROG_MISSING || pi.kind == PROG_UNREADABLE) {
        lsiw_log("%s: %s\n", arg, pi.kind == PROG_MISSING ? "no such file" : strerror(pi.err));
        free(path);
        return EXIT_USAGE;
    }

    char desc[160];
    describe_program(&pi, desc, sizeof desc);
    char *win = lsiw_linux_to_win(path);
    printf("Program:        %s\n", path);
    printf("Windows path:   %s\n", win ? win : "?");
    printf("Type:           %s\n", desc);
    free(win);

    int status = 1, native_ok = 0, native_usable = 0;
    if (pi.kind == PROG_PE64 && !pi.is_dll) {
        char *report = NULL;
        int rc = lsiw_native_check(path, &report);
        native_ok = rc == 0;
        native_usable = rc >= 0;
        if (rc == 0) {
            printf("Native engine:  every imported function is implemented\n");
            status = 0;
        } else {
            int lines = 0;
            for (const char *p = report; p && *p; p++) if (*p == '\n') lines++;
            if (report && *report && report[strlen(report) - 1] != '\n') lines++;
            printf("Native engine:  %d problem%s:\n", lines, lines == 1 ? "" : "s");
            const char *p = report ? report : "";
            while (*p) {
                const char *e = strchr(p, '\n');
                int l = e ? (int)(e - p) : (int)strlen(p);
                if (l) printf("                  %.*s\n", l, p);
                if (!e) break;
                p = e + 1;
            }
        }
        free(report);
    } else if (pi.kind == PROG_NOT_WINDOWS || pi.is_dll) {
        printf("Native engine:  -\n");
        free(path);
        return EXIT_USAGE;
    } else {
        printf("Native engine:  cannot run it (%s; the native engine runs 64-bit x86 programs)\n",
               pi.kind == PROG_PE_OTHER ? machine_name(pi.machine) : wine_reason(&pi));
    }

    const char *wine = lsiw_wine_binary();
    const char *pick;
    if (native_ok) pick = "native";
    else if (pi.kind == PROG_PE_OTHER) pick = "none (no engine runs this processor type)";
    else if (wine) pick = "wine (the fallback)";
    else if (pi.kind == PROG_PE64 && native_usable)
        pick = "native anyway (Wine is not installed; the program stops if it calls a missing function)";
    else pick = "none (needs Wine, which is not installed)";
    printf("Engine (auto):  %s\n", pick);
    int conf = configured_engine();
    if (conf != ENGINE_AUTO)
        printf("Default engine: %s (set with 'lsiw config engine'), so 'lsiw run' uses %s\n",
               engine_names[conf], engine_names[conf]);
    free(path);
    return status;
}

/* ------------------------------------------------------------------ */
/* config                                                               */
/* ------------------------------------------------------------------ */

static const char *kind_name(int kind)
{
    switch (kind) {
    case LSIW_VAR_FOLDER:  return "folder";
    case LSIW_VAR_VIRTUAL: return "virtual";
    case LSIW_VAR_VALUE:   return "value";
    case LSIW_VAR_ALIAS:   return "alias";
    case LSIW_VAR_DERIVED: return "derived";
    default:               return "?";
    }
}

/* "APPDATA", "%APPDATA%" and "%appdata" all name APPDATA. Caller frees. */
static char *var_name_arg(const char *s)
{
    size_t l = strlen(s);
    if (l && s[0] == '%') { s++; l--; }
    if (l && s[l - 1] == '%') l--;
    char *r = strndup(s, l);
    if (!r) {
        fputs("lsiw: out of memory\n", stderr);
        exit(EXIT_FAIL);
    }
    return r;
}

static int overridden(const struct lsiw_var *v)
{
    const struct lsiw_var *r = lsiw_var_resolve(v);
    return r && r->kind != LSIW_VAR_DERIVED && lsiw_var_override(v->name) != NULL;
}

/* Edit distance (case-insensitive), to suggest names for typos. */
static int name_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la > 64 || lb > 64) return 99;
    int row[65];
    for (size_t j = 0; j <= lb; j++) row[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        int diag = row[0];
        row[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int up = row[j];
            int cost = tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]);
            int best = diag + cost;
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[lb];
}

static int unknown_var(const char *name)
{
    lsiw_log("unknown variable '%s'\n", name);
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    int shown = 0;
    for (size_t i = 0; i < n && shown < 6; i++) {
        if (!*name) break;
        if (!strcasestr(t[i].name, name) && name_distance(t[i].name, name) > 2) continue;
        fprintf(stderr, "%s%s", shown ? ", " : "      Did you mean: ", t[i].name);
        shown++;
    }
    if (shown) fputc('\n', stderr);
    fprintf(stderr, "      'lsiw config list --all' shows every name.\n");
    return EXIT_FAIL;
}

/* The variable a DERIVED one follows: "${VAR:WINDIR}/system32" -> WINDIR,
 * and the part after it ("system32"). Caller frees. */
static char *derived_parent(const struct lsiw_var *v, const char **rest)
{
    *rest = "";
    const char *p = v->def ? strstr(v->def, "${VAR:") : NULL;
    if (!p) return NULL;
    p += 6;
    const char *e = strchr(p, '}');
    if (!e) return NULL;
    *rest = e[1] == '/' ? e + 2 : e + 1;
    char *r = strndup(p, (size_t)(e - p));
    if (!r) {
        fputs("lsiw: out of memory\n", stderr);
        exit(EXIT_FAIL);
    }
    return r;
}

static int not_relocatable(const struct lsiw_var *v)
{
    const char *rest;
    char *parent = derived_parent(v, &rest);
    if (parent)
        lsiw_log("%s cannot be moved on its own: it is always %s%s%s inside %s. Move %s instead.\n",
                 v->name, *rest ? "the folder '" : "the same folder as ", rest, *rest ? "'" : "",
                 parent, parent);
    else
        lsiw_log("%s cannot be moved on its own\n", v->name);
    free(parent);
    return EXIT_FAIL;
}

static int save_and_sync(void)
{
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        return -1;
    }
    if (lsiw_sync_root(1) < 0)
        lsiw_log("warning: the C:\\ view %s could not be rebuilt\n", lsiw_root_dir());
    if (lsiw_wine_prefix_state() == LSIW_PREFIX_STALE)
        printf("The Wine prefix will be updated the next time a program runs with Wine.\n");
    return 0;
}

static void print_location(const struct lsiw_var *v)
{
    const char *lin = lsiw_var_linux(v->name), *win = lsiw_var_windows(v->name);
    const struct lsiw_var *r = lsiw_var_resolve(v);
    if (r && r->kind == LSIW_VAR_VALUE) printf("%s = %s\n", v->name, lin ? lin : "");
    else printf("%s: %s  (%s)\n", v->name, lin ? lin : "-", win ? win : "-");
}

/* On a terminal, $HOME is shown as ~ to keep the table readable. */
static char *display_path(const char *p, int tty)
{
    if (!p) return lsiw_cli_strdup("-");
    const char *home = lsiw_home_dir();
    size_t hl = strlen(home);
    if (tty && hl > 1 && !strncmp(p, home, hl) && (p[hl] == '/' || !p[hl]))
        return lsiw_cli_printf("~%s", p + hl);
    return lsiw_cli_strdup(p);
}

static int config_list(int argc, char **argv)
{
    int all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all") || !strcmp(argv[i], "-a")) all = 1;
        else if (is_help(argv[i])) { fputs(help_config, stdout); return 0; }
        else return usage_error("config", "unknown argument '%s' for 'config list'", argv[i]);
    }
    int tty = isatty(STDOUT_FILENO);
    size_t n;
    const struct lsiw_var *t = lsiw_vars(&n);
    char **lin = calloc(n, sizeof *lin);
    if (!lin) {
        fputs("lsiw: out of memory\n", stderr);
        return EXIT_FAIL;
    }
    /* Column widths fit the content; a very long Linux path overflows its
     * column instead of pushing every Windows path off the screen. */
    int wn = 4, wk = 4, wl = 14, shown = 0;
    for (size_t i = 0; i < n; i++) {
        if (!all && !t[i].usmt) continue;
        lin[i] = display_path(lsiw_var_linux(t[i].name), tty);
        int l = (int)strlen(t[i].name);
        if (l > wn) wn = l;
        l = (int)strlen(kind_name(t[i].kind));
        if (l > wk) wk = l;
        l = (int)strlen(lin[i]);
        if (l > wl) wl = l;
    }
    if (wl > 56) wl = 56;
    printf("  %-*s  %-*s  %-*s  %s\n", wn, "NAME", wk, "KIND", wl, "LINUX LOCATION", "WINDOWS PATH");
    for (size_t i = 0; i < n; i++) {
        if (!lin[i]) continue;
        const char *win = lsiw_var_windows(t[i].name);
        printf("%c %-*s  %-*s  %-*s  %s\n", overridden(&t[i]) ? '*' : ' ', wn, t[i].name,
               wk, kind_name(t[i].kind), wl, lin[i], win ? win : "-");
        free(lin[i]);
        shown++;
    }
    free(lin);
    if (tty)
        printf("\n* = moved from the default location. %d of %zu variables shown%s.\n",
               shown, n, all ? "" : " ('lsiw config list --all' shows every one)");
    return 0;
}

static int config_get(int argc, char **argv)
{
    int mode = 0;        /* 0 details, 1 Linux path, 2 Windows path */
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--linux")) mode = 1;
        else if (!strcmp(argv[i], "--windows")) mode = 2;
        else if (is_help(argv[i])) { fputs(help_config, stdout); return 0; }
        else if (arg) return usage_error("config", "'config get' takes one variable name");
        else arg = argv[i];
    }
    if (!arg) return usage_error("config", "'config get' needs a variable name");
    char *name = var_name_arg(arg);
    const struct lsiw_var *v = lsiw_var_find(name);
    if (!v) {
        int rc = unknown_var(name);
        free(name);
        return rc;
    }
    free(name);
    const struct lsiw_var *r = lsiw_var_resolve(v);
    const char *lin = lsiw_var_linux(v->name), *win = lsiw_var_windows(v->name);

    if (mode) {
        const char *s = mode == 1 ? lin : win;
        if (!s) {
            lsiw_log("%s has no %s location\n", v->name, mode == 1 ? "Linux" : "Windows");
            return EXIT_FAIL;
        }
        puts(s);
        return 0;
    }
    const int w = 15;
    printf("%-*s %s\n", w, "Name:", v->name);
    printf("%-*s %s, %s\n", w, "Kind:", kind_name(v->kind), v->scope == LSIW_SCOPE_USER ? "per user" : "system");
    if (v->desc) printf("%-*s %s\n", w, "Description:", v->desc);
    if (v->kind == LSIW_VAR_ALIAS && r) printf("%-*s %s\n", w, "Alias of:", r->name);
    if (r && r->kind == LSIW_VAR_DERIVED) {
        const char *rest;
        char *parent = derived_parent(r, &rest);
        if (parent) printf("%-*s %s%s%s (cannot be moved on its own)\n", w, "Follows:", parent, *rest ? "/" : "", rest);
        free(parent);
    }
    if (r && r->kind == LSIW_VAR_VALUE) {
        printf("%-*s %s\n", w, "Value:", lin ? lin : "(none)");
    } else {
        printf("%-*s %s\n", w, "Linux:", lin ? lin : "(none)");
        printf("%-*s %s\n", w, "Windows:", win ? win : "(none)");
    }
    if (r && r->def) printf("%-*s %s\n", w, "Default:", *r->def ? r->def : "(none)");
    if (r && overridden(v)) printf("%-*s %s\n", w, "Set to:", lsiw_var_override(v->name));
    if (r && r->anchor) printf("%-*s C:\\%s (fixed name)\n", w, "Appears as:", r->anchor);
    if (r && r->csidl >= 0) printf("%-*s 0x%02x\n", w, "CSIDL:", (unsigned)r->csidl);
    if (r && r->shell_value) printf("%-*s %s\n", w, "Shell folder:", r->shell_value);
    printf("%-*s %s\n", w, "USMT:", v->usmt ? "documented by Microsoft's USMT" : "standard Windows variable");
    return 0;
}

static int config_set(int argc, char **argv)
{
    if (argc == 2 && is_help(argv[1])) { fputs(help_config, stdout); return 0; }
    if (argc != 3) return usage_error("config", "usage: lsiw config set NAME PATH");
    char *name = var_name_arg(argv[1]);
    const char *value = argv[2];
    const struct lsiw_var *v = lsiw_var_find(name);
    if (!v) {
        int rc = unknown_var(name);
        free(name);
        return rc;
    }
    free(name);
    const struct lsiw_var *r = lsiw_var_resolve(v);
    if (!r) return unknown_var(v->name);
    if (r->kind == LSIW_VAR_DERIVED) return not_relocatable(v);
    if (!*value) return usage_error("config", "empty value; 'lsiw config reset %s' restores the default", v->name);

    char *val = NULL;
    int folder = r->kind == LSIW_VAR_FOLDER || r->kind == LSIW_VAR_VIRTUAL;
    /* Windows paths and ~ are converted by lsiw_var_set; a relative Linux
     * path is taken relative to the current directory. */
    if (folder && value[0] != '/' && value[0] != '~' && !clearly_windows(value) &&
        !strchr(value, '\\') && !strchr(value, '%'))
        val = absolute_path(value);
    else
        val = lsiw_cli_strdup(value);

    int rc = lsiw_var_set(v->name, val);
    free(val);
    if (rc == -1) return unknown_var(v->name);
    if (rc == -2) return not_relocatable(v);

    if (folder) {
        const char *lin = lsiw_var_linux(v->name);
        struct stat st;
        if (!lin || (stat(lin, &st) == 0 && !S_ISDIR(st.st_mode))) {
            lsiw_log("%s is not a folder; %s was not changed\n", lin ? lin : value, v->name);
            lsiw_config_load();     /* drop the unsaved change */
            return EXIT_FAIL;
        }
    }
    if (save_and_sync() < 0) return EXIT_FAIL;
    print_location(v);
    if (v != r) printf("(%s is another name for %s; both changed)\n", v->name, r->name);
    return 0;
}

static int config_reset(int argc, char **argv)
{
    if (argc == 2 && is_help(argv[1])) { fputs(help_config, stdout); return 0; }
    if (argc != 2) return usage_error("config", "usage: lsiw config reset NAME|--all");
    if (!strcmp(argv[1], "--all")) {
        size_t n;
        const struct lsiw_var *t = lsiw_vars(&n);
        int count = 0;
        for (size_t i = 0; i < n; i++) {
            if (t[i].kind == LSIW_VAR_ALIAS || t[i].kind == LSIW_VAR_DERIVED || !overridden(&t[i])) continue;
            if (lsiw_var_set(t[i].name, NULL) == 0) count++;
        }
        if (!count) {
            printf("Every variable already has its default location.\n");
            return 0;
        }
        if (save_and_sync() < 0) return EXIT_FAIL;
        printf("%d variable%s reset to the default location.\n", count, count == 1 ? "" : "s");
        return 0;
    }
    char *name = var_name_arg(argv[1]);
    const struct lsiw_var *v = lsiw_var_find(name);
    if (!v) {
        int rc = unknown_var(name);
        free(name);
        return rc;
    }
    free(name);
    int was = overridden(v);
    int rc = lsiw_var_set(v->name, NULL);
    if (rc == -1) return unknown_var(v->name);
    if (rc == -2) return not_relocatable(v);
    if (!was) {
        printf("%s already has its default location.\n", v->name);
        print_location(v);
        return 0;
    }
    if (save_and_sync() < 0) return EXIT_FAIL;
    print_location(v);
    return 0;
}

static int config_engine(int argc, char **argv)
{
    if (argc == 1) {
        printf("%s\n", engine_names[configured_engine()]);
        return 0;
    }
    if (is_help(argv[1])) { fputs(help_config, stdout); return 0; }
    if (argc != 2) return usage_error("config", "usage: lsiw config engine [auto|native|wine|proton|custom]");
    int e = parse_engine(argv[1]);
    if (e < 0) return usage_error("config", "unknown engine '%s' (use auto, native, wine, proton or custom)", argv[1]);
    lsiw_setting_set("engine", "default", engine_names[e]);
    lsiw_setting_set("runtime", "backend", NULL);       /* older key */
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        return EXIT_FAIL;
    }
    printf("Default engine: %s\n", engine_names[e]);
    if (e == ENGINE_WINE && !lsiw_wine_binary())
        lsiw_log("warning: Wine is not installed (on Fedora: sudo dnf install wine)\n");
    if (e == ENGINE_PROTON && !lsiw_proton_available())
        lsiw_log("warning: no Proton is installed (install one in Steam, or run 'lsiw runners install ge')\n");
    if (e == ENGINE_CUSTOM) {
        char **chain;
        size_t n = lsiw_engine_chain(&chain);
        printf("Engines, in order:");
        for (size_t i = 0; i < n; i++) printf("%s %s", i ? "," : "", chain[i]);
        printf(" ('lsiw config chain' changes them)\n");
        lsiw_strv_free(chain);
    }
    return 0;
}

/* One entry of the custom chain: ENGINE or ENGINE:VERSION. Returns the
 * engine, or -1 (message printed). */
static int chain_entry(const char *s, int quiet)
{
    char *name = lsiw_cli_strdup(s);
    char *colon = strchr(name, ':');
    if (colon) *colon++ = 0;
    int e = parse_engine(name);
    if (e < 0 || e == ENGINE_CUSTOM || (colon && e != ENGINE_WINE && e != ENGINE_PROTON) || (colon && !*colon)) {
        if (!quiet) lsiw_log("'%s' is not an engine (use native, auto, wine[:VERSION] or proton[:VERSION])\n", s);
        e = -1;
    }
    free(name);
    return e;
}

static int config_chain(int argc, char **argv)
{
    if (argc == 1) {
        char **chain;
        size_t n = lsiw_engine_chain(&chain);
        for (size_t i = 0; i < n; i++)
            printf("%zu. %s%s\n", i + 1, chain[i], i ? " (fallback)" : " (default)");
        lsiw_strv_free(chain);
        if (configured_engine() != ENGINE_CUSTOM)
            printf("Used when the default engine is 'custom' ('lsiw config engine custom').\n");
        return 0;
    }
    if (is_help(argv[1])) { fputs(help_config, stdout); return 0; }
    /* "native wine" or "native,wine": every argument, split on commas. */
    char *joined = lsiw_cli_strdup("");
    int count = 0;
    for (int i = 1; i < argc; i++) {
        char *copy = lsiw_cli_strdup(argv[i]), *save = NULL;
        for (char *t = strtok_r(copy, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
            if (chain_entry(t, 0) < 0) { free(copy); free(joined); return EXIT_USAGE; }
            if (++count > LSIW_CHAIN_MAX) {
                free(copy); free(joined);
                return usage_error("config", "at most %d engines: a default and %d fallbacks",
                                   LSIW_CHAIN_MAX, LSIW_CHAIN_MAX - 1);
            }
            char *j = *joined ? lsiw_cli_printf("%s,%s", joined, t) : lsiw_cli_strdup(t);
            free(joined);
            joined = j;
        }
        free(copy);
    }
    lsiw_setting_set("engine", "chain", joined);
    if (configured_engine() != ENGINE_CUSTOM) {
        lsiw_setting_set("engine", "default", "custom");
        lsiw_setting_set("runtime", "backend", NULL);
    }
    free(joined);
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        return EXIT_FAIL;
    }
    printf("Default engine: custom\n");
    return config_chain(1, argv);
}

static void print_wine_line(int width)
{
    const char *wine = lsiw_wine_binary();
    if (!wine) {
        printf("%-*s not found (on Fedora: sudo dnf install wine)\n", width, "Wine:");
        return;
    }
    char *ver = lsiw_wine_version();
    printf("%-*s %s (%s)\n", width, "Wine:", wine, ver ? ver : "version unknown");
    free(ver);
}

static int config_wine(int argc, char **argv)
{
    if (argc == 1) {
        const char *cfg = lsiw_setting_get("engine", "wine", NULL);
        if (!cfg || !*cfg) cfg = lsiw_setting_get("runtime", "wine", NULL);
        print_wine_line(0);
        printf("Configured: %s\n", cfg && *cfg ? cfg : "auto (wine, then wine64, from $PATH)");
        return 0;
    }
    if (is_help(argv[1])) { fputs(help_config, stdout); return 0; }
    if (argc != 2) return usage_error("config", "usage: lsiw config wine [PATH|auto]");
    const char *a = argv[1];
    if (!*a || !strcasecmp(a, "auto") || !strcasecmp(a, "default")) {
        lsiw_setting_set("engine", "wine", NULL);
        lsiw_setting_set("runtime", "wine", NULL);
    } else {
        char *p;
        if (a[0] == '~' && a[1] == '/') p = lsiw_cli_printf("%s%s", lsiw_home_dir(), a + 1);
        else if (strchr(a, '/')) p = absolute_path(a);
        else p = lsiw_cli_strdup(a);                    /* a name looked up in $PATH */
        struct lsiw_runner *r = strchr(a, '/') ? NULL : lsiw_runner_pick(LSIW_RUNNER_WINE, a);
        char *found = r ? lsiw_cli_strdup(r->path) : lsiw_cli_which(p);
        if (r) { free(p); p = lsiw_cli_strdup(r->id); lsiw_runner_free(r); }
        if (!found) {
            lsiw_log("%s: no such executable%s\n", a, strchr(a, '/') ? "" : " in $PATH or Wine version ('lsiw runners')");
            free(p);
            return EXIT_FAIL;
        }
        free(found);
        lsiw_setting_set("engine", "wine", p);
        lsiw_setting_set("runtime", "wine", NULL);
        free(p);
    }
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        return EXIT_FAIL;
    }
    print_wine_line(0);
    return 0;
}

static void print_proton_line(int width)
{
    struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_PROTON, lsiw_proton_setting());
    char *display = NULL;
    if (!r && lsiw_proton_official(lsiw_proton_setting(), NULL, &display))
        printf("%-*s LSIW %s (Steam downloads it the first time it is used)\n", width, "Proton:", display);
    else if (!r) printf("%-*s not found (install one in Steam, or 'lsiw runners install ge')\n", width, "Proton:");
    else printf("%-*s LSIW %s (%s)\n", width, "Proton:", r->name, r->path);
    free(display);
    lsiw_runner_free(r);
}

static int config_proton(int argc, char **argv)
{
    if (argc == 1) {
        const char *cfg = lsiw_proton_setting();
        print_proton_line(0);
        printf("Configured: %s\n", cfg ? cfg : "auto (the newest official Proton installed in Steam)");
        return 0;
    }
    if (is_help(argv[1])) { fputs(help_config, stdout); return 0; }
    if (argc != 2) return usage_error("config", "usage: lsiw config proton [VERSION|FOLDER|auto]");
    const char *a = argv[1];
    if (!*a || !strcasecmp(a, "auto") || !strcasecmp(a, "default")) lsiw_setting_set("engine", "proton", NULL);
    else {
        char *p = a[0] == '~' && a[1] == '/' ? lsiw_cli_printf("%s%s", lsiw_home_dir(), a + 1)
                : strchr(a, '/') ? absolute_path(a) : lsiw_cli_strdup(a);
        struct lsiw_runner *r = lsiw_runner_pick(LSIW_RUNNER_PROTON, p);
        if (!r && lsiw_proton_official(p, NULL, NULL)) {
            lsiw_setting_set("engine", "proton", p);
            free(p);
            p = NULL;
        } else if (!r) {
            lsiw_log("%s: no such Proton ('lsiw runners' lists the installed versions)\n", a);
            free(p);
            return EXIT_FAIL;
        }
        if (r) lsiw_setting_set("engine", "proton", strchr(a, '/') ? r->path : r->id);
        lsiw_runner_free(r);
        free(p);
    }
    if (lsiw_config_save() < 0) {
        lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
        return EXIT_FAIL;
    }
    print_proton_line(0);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "chain")) return config_chain(argc - 1, argv + 1);
    if (argc < 2) return config_list(1, argv);
    const char *sub = argv[1];
    if (is_help(sub)) { fputs(help_config, stdout); return 0; }
    if (!strcmp(sub, "list")) return config_list(argc - 1, argv + 1);
    if (!strcmp(sub, "get")) return config_get(argc - 1, argv + 1);
    if (!strcmp(sub, "set")) return config_set(argc - 1, argv + 1);
    if (!strcmp(sub, "reset")) return config_reset(argc - 1, argv + 1);
    if (!strcmp(sub, "engine")) return config_engine(argc - 1, argv + 1);
    if (!strcmp(sub, "wine")) return config_wine(argc - 1, argv + 1);
    if (!strcmp(sub, "proton")) return config_proton(argc - 1, argv + 1);
    return usage_error("config", "unknown subcommand 'config %s'", sub);
}

/* ------------------------------------------------------------------ */
/* path, sync                                                           */
/* ------------------------------------------------------------------ */

static int cmd_path(int argc, char **argv)
{
    if (argc >= 2 && is_help(argv[1])) { fputs(help_path, stdout); return 0; }
    if (argc < 3) return usage_error("path", "usage: lsiw path win|linux|expand ARG...");
    const char *sub = argv[1];
    int mode = !strcmp(sub, "win") || !strcmp(sub, "windows") ? 1
             : !strcmp(sub, "linux") ? 2 : !strcmp(sub, "expand") ? 3 : 0;
    if (!mode) return usage_error("path", "unknown conversion '%s' (use win, linux or expand)", sub);
    for (int i = 2; i < argc; i++) {
        char *out;
        if (mode == 1) {
            char *abs = absolute_path(argv[i]);
            out = lsiw_linux_to_win(abs);
            free(abs);
        } else if (mode == 2) out = lsiw_win_to_linux(argv[i]);
        else out = lsiw_expand(argv[i]);
        if (!out) {
            lsiw_log("cannot convert '%s'\n", argv[i]);
            return EXIT_FAIL;
        }
        puts(out);
        free(out);
    }
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (is_help(argv[i])) { fputs(help_sync, stdout); return 0; }
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else return usage_error("sync", "unknown argument '%s'", argv[i]);
    }
    if (lsiw_sync_root(!quiet) < 0) return EXIT_FAIL;
    if (!quiet) printf("C:\\ view up to date: %s\n", lsiw_root_dir());
    return 0;
}

/* ------------------------------------------------------------------ */
/* info                                                                 */
/* ------------------------------------------------------------------ */

static int cmd_info(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (is_help(argv[i])) { fputs(help_info, stdout); return 0; }
        return usage_error("info", "unknown argument '%s'", argv[i]);
    }
    const int w = 16;
    struct stat st;
    printf("LSIW %s - Linux Subsystem for Improved Wine\n", LSIW_VERSION);
    printf("%-*s built in: 64-bit x86 Windows programs, in this process\n", w, "Native engine:");
    int e = configured_engine();
    printf("%-*s %s%s\n", w, "Default engine:", engine_names[e],
           e == ENGINE_AUTO ? " (native, Wine when the native engine cannot run a program)" : "");
    if (e == ENGINE_CUSTOM) {
        char **chain;
        size_t n = lsiw_engine_chain(&chain);
        printf("%-*s", w + 1, "Engine order:");
        for (size_t i = 0; i < n; i++) printf("%s %s", i ? "," : "", chain[i]);
        printf("\n");
        lsiw_strv_free(chain);
    }
    const char *cfg = lsiw_config_path();
    printf("%-*s %s%s\n", w, "Config file:", cfg, stat(cfg, &st) == 0 ? "" : " (not created yet)");
    printf("%-*s %s\n", w, "Data folder:", lsiw_data_dir());
    const char *root = lsiw_root_dir();
    printf("%-*s %s%s\n", w, "C: drive:", root, stat(root, &st) == 0 ? "" : " (not built yet: 'lsiw sync')");
    printf("%-*s %s (every program and game, unless it has its own)\n", w, "Shared prefix:", lsiw_shared_dir());
    print_wine_line(w);
    print_proton_line(w);
    printf("%-*s %s\n", w, "Steam:",
           !strcasecmp(lsiw_setting_get("steam", "integration", "false"), "true")
               ? "LSIW Proton tools added ('lsiw steam' shows them)" : "not integrated ('lsiw steam add')");
    static const char *const states[] = { "not created yet", "out of date (refreshed on next use)", "ready" };
    int ps = lsiw_wine_prefix_state();
    printf("%-*s %s (%s)\n", w, "Wine prefix:", lsiw_prefix_dir(),
           ps >= 0 && ps <= 2 ? states[ps] : "?");
    lsiw_print_integration(w);
    return 0;
}

/* ------------------------------------------------------------------ */
/* register, unregister, wine-setup                                     */
/* ------------------------------------------------------------------ */

static int register_flags(int argc, char **argv, const char *cmd, int *user, int *sys)
{
    *user = *sys = 0;
    for (int i = 1; i < argc; i++) {
        if (is_help(argv[i])) { fputs(help_register, stdout); return 1; }
        if (!strcmp(argv[i], "--user")) *user = 1;
        else if (!strcmp(argv[i], "--system")) *sys = 1;
        else return usage_error(cmd, "unknown argument '%s'", argv[i]);
    }
    if (!*user && !*sys) *user = 1;
    return 0;
}

static int cmd_register(int argc, char **argv)
{
    int user, sys, r = register_flags(argc, argv, "register", &user, &sys);
    if (r) return r == 1 ? 0 : r;
    int rc = 0;
    if (user && lsiw_register_user() < 0) rc = EXIT_FAIL;
    if (sys && lsiw_register_system() < 0) rc = EXIT_FAIL;
    return rc;
}

static int cmd_unregister(int argc, char **argv)
{
    int user, sys, r = register_flags(argc, argv, "unregister", &user, &sys);
    if (r) return r == 1 ? 0 : r;
    int rc = 0;
    if (user && lsiw_unregister_user() < 0) rc = EXIT_FAIL;
    if (sys && lsiw_unregister_system() < 0) rc = EXIT_FAIL;
    return rc;
}

static int cmd_wine_setup(int argc, char **argv)
{
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (is_help(argv[i])) { fputs(help_wine_setup, stdout); return 0; }
        if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else return usage_error("wine-setup", "unknown argument '%s'", argv[i]);
    }
    const char *wine = lsiw_wine_binary();
    if (!wine) {
        lsiw_log("Wine is not installed (on Fedora: sudo dnf install wine)\n");
        return EXIT_NOT_FOUND;
    }
    /* An explicit request redoes everything, even if the stamp is current. */
    if (lsiw_wine_invalidate() < 0) {
        lsiw_log("cannot reset the Wine prefix stamp: %s\n", strerror(errno));
        return EXIT_FAIL;
    }
    if (lsiw_wine_setup(!quiet) < 0) return EXIT_FAIL;
    if (!quiet) {
        printf("Wine prefix:  %s\n", lsiw_prefix_dir());
        printf("Wine:         %s\n", lsiw_wine_binary());   /* setup may have switched it */
        printf("Drive C:      %s (with every Linux folder; no other drive letters)\n", lsiw_root_dir());
        printf("Registry:     shell folders, Program Files and TEMP point at LSIW's folders\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* help, dispatch                                                       */
/* ------------------------------------------------------------------ */

static int cmd_help(int argc, char **argv);

static const struct command {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *help;
} commands[] = {
    { "run",        cmd_run,        help_run },
    { "check",      cmd_check,      help_check },
    { "config",     cmd_config,     help_config },
    { "path",       cmd_path,       help_path },
    { "sync",       cmd_sync,       help_sync },
    { "info",       cmd_info,       help_info },
    { "register",   cmd_register,   help_register },
    { "unregister", cmd_unregister, help_register },
    { "wine-setup", cmd_wine_setup, help_wine_setup },
    { "lnk",        lsiw_cmd_lnk,   lsiw_help_lnk },
    { "lwlnk",      lsiw_cmd_lwlnk, lsiw_help_lwlnk },
    { "launch",     lsiw_cmd_launch, lsiw_help_lwlnk },
    { "runners",    lsiw_cmd_runners, lsiw_help_runners },
    { "steam",      lsiw_cmd_steam, lsiw_help_steam },
    { "cdrive",     lsiw_cmd_cdrive, lsiw_help_cdrive },
    { "prefix",     lsiw_cmd_prefix, lsiw_help_prefix },
    { "programs",   lsiw_cmd_programs, lsiw_help_programs },
    { "steam-run",  lsiw_cmd_steam_run, lsiw_help_steam },
    { "help",       cmd_help,       help_help },
};

static const struct command *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++)
        if (!strcmp(commands[i].name, name)) return &commands[i];
    return NULL;
}

static int cmd_help(int argc, char **argv)
{
    if (argc < 2) {
        fputs(help_main, stdout);
        return 0;
    }
    const struct command *c = find_command(argv[1]);
    if (!c) return usage_error(NULL, "no help for unknown command '%s'", argv[1]);
    fputs(c->help, stdout);
    if (c->fn == cmd_config) printf("\nSettings are stored in %s.\n", lsiw_config_path());
    return 0;
}

/* 'lsiw PROGRAM.exe ...' without 'run'. */
static int looks_like_program(const char *a)
{
    struct stat st;
    if (stat(a, &st) == 0 && S_ISREG(st.st_mode)) return 1;
    return lsiw_cli_has_ext(a, ".exe") || lsiw_cli_has_ext(a, ".com");
}

/* Started by the kernel through binfmt_misc: argv[1] is the program file,
 * argv[2..] its arguments (the original argv[0] is not passed on). */
static int binfmt_main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "lsiw-binfmt is LSIW's binfmt_misc handler: the kernel runs it as\n"
                        "'lsiw-binfmt PROGRAM.exe [ARGS...]'. To run a program yourself, use 'lsiw run'.\n");
        return EXIT_USAGE;
    }
    return run_program(argc - 1, argv + 1, configured_engine(), 1);
}

int main(int argc, char **argv)
{
    const char *self = argc > 0 && argv[0] ? argv[0] : "lsiw";
    const char *base = strrchr(self, '/');
    base = base ? base + 1 : self;

    lsiw_config_load();
    if (!strcmp(base, "lsiw-binfmt")) return binfmt_main(argc, argv);

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        int r;
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) lsiw_verbose = 1;
        else if (is_help(a)) { fputs(help_main, stdout); return 0; }
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
            printf("lsiw %s\n", LSIW_VERSION);
            return 0;
        }
        else if ((r = engine_option(argc, argv, &i, &g_engine, NULL)) < 0) return EXIT_USAGE;
        else if (r > 0) continue;
        else if (!strcmp(a, "--")) {
            /* Everything after "--" is a program and its arguments. */
            if (i + 1 >= argc) return usage_error(NULL, "no program given after '--'");
            return run_program(argc - i - 1, argv + i + 1, g_engine >= 0 ? g_engine : configured_engine(), 0);
        }
        else if (a[0] == '-' && a[1]) return usage_error(NULL, "unknown option '%s'", a);
        else break;
    }
    if (i >= argc) {
        fputs(help_main, stderr);
        return EXIT_USAGE;
    }

    const struct command *c = find_command(argv[i]);
    if (c) return c->fn(argc - i, argv + i);
    if (lsiw_cli_has_ext(argv[i], ".lwlnk")) return lsiw_launch_lwlnk(argv[i], argc - i - 1, argv + i + 1);
    /* Anything path-like is a program too: run reports a missing file. */
    if (looks_like_program(argv[i]) || strchr(argv[i], '/') || strchr(argv[i], '\\') ||
        clearly_windows(argv[i]))
        return run_program(argc - i, argv + i, g_engine >= 0 ? g_engine : configured_engine(), 0);
    return usage_error(NULL, "unknown command '%s'", argv[i]);
}
