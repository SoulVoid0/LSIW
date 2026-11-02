/* Small helpers shared by every LSIW component.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int lsiw_verbose;

void lsiw_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("lsiw: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

char *lsiw_strdup_printf(const char *fmt, ...)
{
    va_list ap;
    char *s = NULL;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0) s = NULL;
    va_end(ap);
    return s;
}

int lsiw_mkdirs(const char *path, int mode)
{
    if (!path || !*path) return -1;
    char *p = strdup(path);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, mode) < 0 && errno != EEXIST) { free(p); return -1; }
        *s = '/';
    }
    int r = mkdir(p, mode);
    free(p);
    return (r < 0 && errno != EEXIST) ? -1 : 0;
}

const char *lsiw_home_dir(void)
{
    static char *home;
    if (!home) {
        const char *h = getenv("HOME");
        if (!h || h[0] != '/') {
            struct passwd *pw = getpwuid(getuid());
            h = pw ? pw->pw_dir : "/";
        }
        home = strdup(h);
    }
    return home;
}

const char *lsiw_user_name(void)
{
    static char *user;
    if (!user) {
        struct passwd *pw = getpwuid(getuid());
        const char *u = pw ? pw->pw_name : getenv("USER");
        user = strdup(u && *u ? u : "user");
    }
    return user;
}

const char *lsiw_data_dir(void)
{
    static char *dir;
    if (!dir) {
        const char *ov = getenv("LSIW_DATA_DIR");
        const char *xdg = getenv("XDG_DATA_HOME");
        if (ov && *ov) dir = strdup(ov);
        else if (xdg && *xdg == '/') dir = lsiw_strdup_printf("%s/lsiw", xdg);
        else dir = lsiw_strdup_printf("%s/.local/share/lsiw", lsiw_home_dir());
    }
    return dir;
}
