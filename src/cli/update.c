/* lsiw update: find out whether a newer LSIW has been released, and
 * install it.
 *
 * Releases are the GitHub releases of LSIW_REPO. Checking asks GitHub
 * where /releases/latest leads (one HTTPS request, no API key, nothing
 * about this computer is sent besides what any download sends). Checks
 * run by themselves at most once a day, in the background, after an
 * interactive `lsiw` command, and LSIW Control runs one when it starts;
 * [update] check = false (`lsiw update auto off`) or LSIW_NO_UPDATE_CHECK=1
 * turns them off. `lsiw update install` downloads the new release's
 * install.sh and the packages for the format LSIW was installed with,
 * checks every file against the release's SHA256SUMS, and installs them
 * (an AppImage in a folder of your own is replaced in place).
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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK_INTERVAL (24 * 60 * 60)

const char lsiw_help_update[] =
    "Usage: lsiw update [check] [--porcelain]\n"
    "       lsiw update status [--porcelain]\n"
    "       lsiw update install [--yes] [--version=VERSION] [--format=FORMAT] [--force]\n"
    "       lsiw update auto [on|off]\n"
    "\n"
    "LSIW looks for new releases on https://github.com/" LSIW_REPO "/releases.\n"
    "\n"
    "  check     ask GitHub for the newest release now (default)\n"
    "  status    what the last check found, without going online\n"
    "  install   download the newest release (or VERSION) for the way LSIW is\n"
    "            installed (rpm, deb or AppImage; --format chooses), verify every\n"
    "            file against the release's SHA256SUMS and install it. The\n"
    "            packages are installed with sudo, or pkexec without a terminal;\n"
    "            an AppImage in a folder you can write to is replaced in place.\n"
    "            --force installs even when VERSION is not newer.\n"
    "  auto      show, or turn on or off, the automatic check: at most once a\n"
    "            day, in the background, after a command typed in a terminal\n"
    "            (never while a program runs), and when LSIW Control starts.\n"
    "            A newer release is then shown in one line; nothing is ever\n"
    "            installed without you asking. LSIW_NO_UPDATE_CHECK=1 turns it\n"
    "            off too.\n"
    "\n"
    "--porcelain prints key=value lines (current, latest, available, checked,\n"
    "format, auto) for scripts and LSIW Control.\n";

static int is_help(const char *a) { return !strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "help"); }

/* $LSIW_UPDATE_BASE replaces https://github.com/<repo> (for tests). */
static const char *repo_base(void)
{
    const char *b = getenv("LSIW_UPDATE_BASE");
    return b && *b ? b : "https://github.com/" LSIW_REPO;
}

static const char *curl_proto(void)
{
    return getenv("LSIW_UPDATE_BASE") ? "=https,http,file" : "=https";
}

/* The newest release's version, or NULL (message printed unless quiet). */
static char *fetch_latest(int quiet)
{
    char *url = lsiw_cli_printf("%s/releases/latest", repo_base());
    char *argv[] = { "curl", "-fsS", "--proto", (char *)curl_proto(), "--max-time", "20",
                     "-o", "/dev/null", "-w", "%{redirect_url}", url, NULL };
    int st = -1;
    char *out = lsiw_cli_capture(argv, NULL, &st);
    free(url);
    char *ver = NULL;
    if (st == 0 && out) {
        /* .../releases/tag/v1.8.0 */
        char *t = strstr(out, "/releases/tag/");
        if (t) {
            t += 14;
            if (*t == 'v' || *t == 'V') t++;
            size_t n = strcspn(t, "/?#\r\n \t");
            ver = strndup(t, n);
        }
    }
    free(out);
    if (!ver && !getenv("LSIW_UPDATE_BASE")) {
        /* The API says the same, with a rate limit. */
        char *api[] = { "curl", "-fsS", "--proto", "=https", "--max-time", "20",
                        "-H", "Accept: application/vnd.github+json",
                        "https://api.github.com/repos/" LSIW_REPO "/releases/latest", NULL };
        out = lsiw_cli_capture(api, NULL, &st);
        char *k = st == 0 && out ? strstr(out, "\"tag_name\"") : NULL;
        if (k && (k = strchr(k + 10, '"'))) {
            k++;
            if (*k == 'v' || *k == 'V') k++;
            ver = strndup(k, strcspn(k, "\""));
        }
        free(out);
    }
    if (ver && !lsiw_version_valid(ver)) { free(ver); ver = NULL; }
    if (!ver && !quiet)
        lsiw_log("cannot find the newest LSIW release on %s (is the network up and curl installed?)\n", repo_base());
    return ver;
}

/* How this lsiw was installed: "appimage", "rpm", "deb", or NULL. */
static const char *installed_format(void)
{
    if (getenv("APPIMAGE")) return "appimage";
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return NULL;
    self[n] = 0;
    char *w;
    if ((w = lsiw_cli_which("rpm"))) {
        free(w);
        char *argv[] = { "rpm", "-qf", self, NULL };
        if (lsiw_cli_spawn(argv, NULL, 1) == 0) return "rpm";
    }
    if ((w = lsiw_cli_which("dpkg-query"))) {
        free(w);
        char *argv[] = { "dpkg-query", "-S", self, NULL };
        int st = -1;
        char *out = lsiw_cli_capture(argv, NULL, &st);
        free(out);
        if (st == 0) return "deb";
    }
    return NULL;
}

static int control_installed(const char *format)
{
    if (!strcmp(format, "rpm")) {
        char *argv[] = { "rpm", "-q", "lsiw-control", NULL };
        return lsiw_cli_spawn(argv, NULL, 1) == 0;
    }
    if (!strcmp(format, "deb")) {
        char *argv[] = { "dpkg-query", "-W", "-f=${Status}", "lsiw-control", NULL };
        int st = -1;
        char *out = lsiw_cli_capture(argv, NULL, &st);
        int ok = st == 0 && out && strstr(out, "ok installed");
        free(out);
        return ok;
    }
    return 1;
}

static void print_state(int porcelain, const char *latest, long long checked)
{
    int avail = latest && lsiw_version_newer(latest, LSIW_VERSION);
    const char *fmt = installed_format();
    if (porcelain) {
        printf("current=%s\nlatest=%s\navailable=%d\nchecked=%lld\nformat=%s\nauto=%d\n",
               LSIW_VERSION, latest ? latest : "", avail, checked, fmt ? fmt : "", lsiw_update_auto());
        return;
    }
    if (!latest) printf("LSIW %s; no release information yet ('lsiw update check')\n", LSIW_VERSION);
    else if (avail) printf("LSIW %s is available (installed: %s). Install it with 'lsiw update install'.\n",
                           latest, LSIW_VERSION);
    else printf("LSIW %s is up to date (newest release: %s).\n", LSIW_VERSION, latest);
    if (checked) {
        time_t t = (time_t)checked;
        char buf[64];
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", localtime(&t));
        printf("Last check: %s; automatic checks: %s\n", buf, lsiw_update_auto() ? "on" : "off");
    }
}

/* ---- downloading and verifying ---- */

static int download_to(const char *url, const char *file, int progress)
{
    char *part = lsiw_cli_printf("%s.part", file);
    char *argv[] = { "curl", "-fL", "--proto", (char *)curl_proto(), "--retry", "3",
                     progress ? "--progress-bar" : "-sS", "-o", part, (char *)url, NULL };
    int rc = lsiw_cli_spawn(argv, NULL, 0) == 0 && rename(part, file) == 0 ? 0 : -1;
    if (rc < 0) { unlink(part); lsiw_log("cannot download %s\n", url); }
    free(part);
    return rc;
}

/* Release files that belong to a format. */
static int wanted(const char *name, const char *format, const char *v)
{
    char *a = NULL, *b = NULL, *c = NULL;
    int ok = 0;
    if (!strcmp(name, "install.sh")) return 1;
    if (!strcmp(format, "rpm")) {
        a = lsiw_cli_printf("lsiw-%s-", v);
        b = lsiw_cli_printf("lsiw-control-%s-", v);
        size_t n = strlen(name);
        ok = (!strncmp(name, a, strlen(a)) || !strncmp(name, b, strlen(b))) &&
             n > 4 && !strcmp(name + n - 4, ".rpm") && !strstr(name, "debug");
    } else if (!strcmp(format, "deb")) {
        a = lsiw_cli_printf("lsiw_%s-", v);
        b = lsiw_cli_printf("lsiw-control_%s-", v);
        size_t n = strlen(name);
        ok = (!strncmp(name, a, strlen(a)) || !strncmp(name, b, strlen(b))) &&
             n > 10 && !strcmp(name + n - 10, "_amd64.deb");
    } else if (!strcmp(format, "appimage")) {
        c = lsiw_cli_printf("LSIW-%s-x86_64.AppImage", v);
        ok = !strcmp(name, c);
    }
    free(a); free(b); free(c);
    return ok;
}

/* A file name SHA256SUMS may list: no folders, nothing odd. */
static int plain_name(const char *n)
{
    if (!*n || *n == '.' || strlen(n) > 200) return 0;
    for (; *n; n++)
        if (!(isalnum((unsigned char)*n) || strchr("._+-", *n))) return 0;
    return 1;
}

static int sh_in(const char *dir, const char *script)
{
    char *argv[] = { "sh", "-c", (char *)script, "sh", (char *)dir, NULL };
    return lsiw_cli_spawn(argv, NULL, 0);
}

/* Download what `format` needs of release v into dir and check it against
 * the release's SHA256SUMS. Returns the number of files, -1 on error. */
static int fetch_release(const char *v, const char *format, const char *dir)
{
    char *base = lsiw_cli_printf("%s/releases/download/v%s", repo_base(), v);
    char *sums_path = lsiw_cli_printf("%s/SHA256SUMS", dir);
    char *url = lsiw_cli_printf("%s/SHA256SUMS", base);
    int count = -1;
    char *sums = NULL;
    FILE *sel = NULL;
    printf("Downloading LSIW %s (%s) from %s\n", v, format, base);
    fflush(stdout);
    if (download_to(url, sums_path, 0) < 0) goto out;
    sums = lsiw_cli_read_file(sums_path, NULL);
    if (!sums) goto out;
    char *selpath = lsiw_cli_printf("%s/selected.sums", dir);
    sel = fopen(selpath, "w");
    free(selpath);
    if (!sel) goto out;
    count = 0;
    char *save = NULL;
    for (char *line = strtok_r(sums, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *sp = strpbrk(line, " \t");
        if (!sp || sp - line != 64) continue;
        char *name = sp;
        while (*name == ' ' || *name == '\t' || *name == '*') name++;
        if (!plain_name(name) || !wanted(name, format, v)) continue;
        fprintf(sel, "%.64s  %s\n", line, name);
        char *file = lsiw_cli_printf("%s/%s", dir, name);
        free(url);
        url = lsiw_cli_printf("%s/%s", base, name);
        struct stat st;
        if (stat(file, &st) < 0 && download_to(url, file, 1) < 0) { free(file); count = -1; goto out; }
        free(file);
        count++;
    }
    fclose(sel);
    sel = NULL;
    if (count < 2) {
        lsiw_log("the release v%s has no %s packages\n", v, format);
        count = -1;
        goto out;
    }
    printf("Checking the files against SHA256SUMS\n");
    fflush(stdout);
    if (sh_in(dir, "cd \"$1\" && sha256sum --strict --check selected.sums") != 0) {
        lsiw_log("checksum mismatch: a download is damaged or was altered; nothing was installed\n");
        /* Downloaded again next time. */
        sh_in(dir, "cd \"$1\" && for f in $(cut -c67- selected.sums); do rm -f -- \"$f\"; done");
        count = -1;
    }
out:
    if (sel) fclose(sel);
    free(sums);
    free(url);
    free(sums_path);
    free(base);
    return count;
}

static char *cache_dir(const char *v)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    char *root = xdg && xdg[0] == '/' ? lsiw_cli_printf("%s/lsiw/update", xdg)
                                       : lsiw_cli_printf("%s/.cache/lsiw/update", lsiw_home_dir());
    char *d = lsiw_cli_printf("%s/%s", root, v);
    free(root);
    return d;
}

/* An AppImage the user owns: replace it in place. 1 done, 0 not such a
 * case, -1 failed. */
static int replace_appimage(const char *dir, const char *v)
{
    const char *cur = getenv("APPIMAGE");
    if (!cur || cur[0] != '/') return 0;
    char *real = realpath(cur, NULL);
    if (!real) return 0;
    char *slash = strrchr(real, '/');
    *slash = 0;
    int writable = access(real, W_OK) == 0;
    *slash = '/';
    writable = writable && access(real, W_OK) == 0;
    if (!writable) { free(real); return 0; }
    char *src = lsiw_cli_printf("%s/LSIW-%s-x86_64.AppImage", dir, v);
    char *tmp = lsiw_cli_printf("%s.lsiw-update", real);
    char *argv[] = { "cp", "--", src, tmp, NULL };
    int rc = -1;
    if (lsiw_cli_spawn(argv, NULL, 0) == 0 && chmod(tmp, 0755) == 0 && rename(tmp, real) == 0) {
        printf("Replaced %s with LSIW %s.\n", real, v);
        rc = 1;
    } else {
        lsiw_log("cannot replace %s: %s\n", real, strerror(errno));
        unlink(tmp);
    }
    free(tmp);
    free(src);
    free(real);
    return rc;
}

static int run_installer(const char *dir, const char *format)
{
    char *script = lsiw_cli_printf("%s/install.sh", dir);
    char *fmt = lsiw_cli_printf("--format=%s", format);
    const char *args[16];
    int n = 0;
    char *sudo = NULL, *pkexec = NULL;
    if (geteuid() != 0 && !isatty(0) && (pkexec = lsiw_cli_which("pkexec"))) {
        /* No terminal for sudo's password (LSIW Control): ask through
         * the desktop. */
        args[n++] = pkexec;
        args[n++] = "/bin/sh";
    } else if (geteuid() != 0 && !(sudo = lsiw_cli_which("sudo"))) {
        lsiw_log("installing packages needs root: run 'sudo lsiw update install', or install sudo or pkexec\n");
        free(script); free(fmt);
        return 1;
    } else {
        args[n++] = "sh";
    }
    args[n++] = script;
    args[n++] = "--yes";
    args[n++] = "--offline";
    args[n++] = fmt;
    args[n++] = "--no-steam";
    args[n++] = "--no-wine";
    args[n++] = "--update";
    if (!control_installed(format)) args[n++] = "--no-control";
    args[n] = NULL;
    printf("Running the installer of the new release%s\n", pkexec ? " (with pkexec)" : "");
    fflush(stdout);
    int st = lsiw_cli_spawn((char *const *)args, NULL, 0);
    free(sudo); free(pkexec); free(script); free(fmt);
    if (st != 0) { lsiw_log("the installer failed (exit status %d)\n", st); return 1; }
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    const char *want = NULL, *format = NULL;
    int force = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y")) continue;   /* never asks */
        else if (!strncmp(argv[i], "--version=", 10)) want = argv[i] + 10;
        else if (!strncmp(argv[i], "--format=", 9)) format = argv[i] + 9;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else { lsiw_log("unknown option '%s' (try 'lsiw update --help')\n", argv[i]); return 2; }
    }
    if (want && (*want == 'v' || *want == 'V')) want++;
    if (want && !lsiw_version_valid(want)) { lsiw_log("'%s' is not a version\n", want); return 2; }
    if (format && strcmp(format, "rpm") && strcmp(format, "deb") && strcmp(format, "appimage")) {
        lsiw_log("--format must be rpm, deb or appimage\n");
        return 2;
    }
    if (!format) format = installed_format();
    if (!format) {
        lsiw_log("this lsiw was not installed from an rpm, a deb or an AppImage (built from source?),\n"
                 "so it cannot be updated in place; pass --format=rpm, deb or appimage to switch to a\n"
                 "package, or update the source tree and run 'make install' again\n");
        return 1;
    }
    char *v = want ? lsiw_cli_strdup(want) : fetch_latest(0);
    if (!v) return 1;
    if (!want) lsiw_update_store(v);
    if (!force && !lsiw_version_newer(v, LSIW_VERSION)) {
        printf("LSIW %s is up to date (newest release: %s).\n", LSIW_VERSION, v);
        free(v);
        return 0;
    }
    char *dir = cache_dir(v);
    if (lsiw_mkdirs(dir, 0700) < 0) {
        lsiw_log("cannot create %s: %s\n", dir, strerror(errno));
        free(dir); free(v);
        return 1;
    }
    int rc = 1;
    if (fetch_release(v, format, dir) < 0) goto out;
    if (!strcmp(format, "appimage")) {
        int r = replace_appimage(dir, v);
        if (r < 0) goto out;
        if (r > 0) { rc = 0; goto done; }
    }
    if (run_installer(dir, format)) goto out;
    rc = 0;
done:
    printf("LSIW %s is installed. Restart LSIW Control to use the new version.\n", v);
    /* The downloads are not needed any more. */
    sh_in(dir, "rm -rf -- \"$1\"");
out:
    free(dir);
    free(v);
    return rc;
}

int lsiw_cmd_update(int argc, char **argv)
{
    const char *sub = argc > 1 && argv[1][0] != '-' ? argv[1] : "check";
    int first_opt = argc > 1 && argv[1][0] != '-' ? 2 : 1;
    if (argc > 1 && is_help(argv[1])) { fputs(lsiw_help_update, stdout); return 0; }
    int porcelain = 0;
    if (strcmp(sub, "install") && strcmp(sub, "auto")) {
        for (int i = first_opt; i < argc; i++) {
            if (!strcmp(argv[i], "--porcelain")) porcelain = 1;
            else if (is_help(argv[i])) { fputs(lsiw_help_update, stdout); return 0; }
            else { lsiw_log("unknown option '%s' (try 'lsiw update --help')\n", argv[i]); return 2; }
        }
    }
    if (!strcmp(sub, "check")) {
        char *v = fetch_latest(porcelain);
        if (v) lsiw_update_store(v);
        char *latest = NULL;
        long long checked = 0;
        lsiw_update_cached(&latest, &checked);
        print_state(porcelain, v ? v : latest, checked);
        free(latest);
        int ok = v != NULL;
        free(v);
        return ok ? 0 : 1;
    }
    if (!strcmp(sub, "status")) {
        char *latest = NULL;
        long long checked = 0;
        lsiw_update_cached(&latest, &checked);
        print_state(porcelain, latest, checked);
        free(latest);
        return 0;
    }
    if (!strcmp(sub, "install")) return cmd_install(argc, argv);
    if (!strcmp(sub, "auto")) {
        if (argc < 3) {
            printf("Automatic update checks: %s\n", lsiw_update_auto() ? "on" : "off");
            return 0;
        }
        const char *a = argv[2];
        int on = !strcmp(a, "on") || !strcmp(a, "yes") || !strcmp(a, "true");
        if (!on && strcmp(a, "off") && strcmp(a, "no") && strcmp(a, "false")) {
            lsiw_log("usage: lsiw update auto [on|off]\n");
            return 2;
        }
        lsiw_setting_set("update", "check", on ? NULL : "false");
        if (lsiw_config_save() < 0) {
            lsiw_log("cannot save %s: %s\n", lsiw_config_path(), strerror(errno));
            return 1;
        }
        printf("Automatic update checks: %s\n", on ? "on" : "off");
        if (on && getenv("LSIW_NO_UPDATE_CHECK")) printf("(but LSIW_NO_UPDATE_CHECK is set, which turns them off)\n");
        return 0;
    }
    lsiw_log("unknown subcommand 'update %s' (try 'lsiw update --help')\n", sub);
    return 2;
}

/* ---- the automatic check ---- */

void lsiw_update_notice(void)
{
    if (!lsiw_update_auto() || !isatty(STDERR_FILENO) || !isatty(STDOUT_FILENO)) return;
    char *latest = NULL;
    long long checked = 0;
    lsiw_update_cached(&latest, &checked);
    if (latest && lsiw_version_newer(latest, LSIW_VERSION))
        fprintf(stderr, "\nLSIW %s is available (this is %s): 'lsiw update install' installs it "
                        "('lsiw update auto off' stops these checks).\n", latest, LSIW_VERSION);
    long long now = (long long)time(NULL);
    if (checked && now - checked < CHECK_INTERVAL && checked <= now) { free(latest); return; }
    /* Note the attempt first, so commands run meanwhile do not start
     * checks of their own; then check in a detached process. */
    lsiw_update_store(latest);
    free(latest);
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) { waitpid(pid, NULL, 0); return; }
    setsid();
    if (fork() != 0) _exit(0);
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
    char *v = fetch_latest(1);
    if (v) lsiw_update_store(v);
    _exit(0);
}
