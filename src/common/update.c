/* What LSIW knows about newer releases: the [update] check setting and
 * the result of the last check (<data>/update-check), shared by `lsiw
 * update` and LSIW Control. Checking itself (the network) is done by the
 * lsiw command (src/cli/update.c).
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
#include <time.h>
#include <unistd.h>

int lsiw_update_auto(void)
{
    const char *env = getenv("LSIW_NO_UPDATE_CHECK");
    if (env && *env && strcmp(env, "0")) return 0;
    const char *v = lsiw_setting_get("update", "check", "true");
    return !(!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0"));
}

char *lsiw_update_state_path(void)
{
    return lsiw_strdup_printf("%s/update-check", lsiw_data_dir());
}

int lsiw_update_cached(char **latest, long long *checked)
{
    if (latest) *latest = NULL;
    if (checked) *checked = 0;
    char *path = lsiw_update_state_path();
    char *text = lsiw_read_text(path);
    free(path);
    if (!text) return -1;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "checked=", 8) && checked) *checked = strtoll(line + 8, NULL, 10);
        else if (!strncmp(line, "latest=", 7) && latest && line[7]) {
            free(*latest);
            *latest = strdup(line + 7);
        }
    }
    free(text);
    return 0;
}

int lsiw_update_store(const char *latest)
{
    lsiw_mkdirs(lsiw_data_dir(), 0755);
    char *path = lsiw_update_state_path();
    char *tmp = lsiw_strdup_printf("%s.%ld.tmp", path, (long)getpid());
    FILE *f = fopen(tmp, "w");
    int rc = -1;
    if (f) {
        fprintf(f, "checked=%lld\nlatest=%s\n", (long long)time(NULL), latest ? latest : "");
        if (fclose(f) == 0 && rename(tmp, path) == 0) rc = 0;
    }
    if (rc < 0) unlink(tmp);
    free(tmp);
    free(path);
    return rc;
}

/* "v1.8.0" and "1.8.0" alike; numeric parts compared as numbers. */
int lsiw_version_newer(const char *a, const char *b)
{
    if (!a || !b) return 0;
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    return strverscmp(a, b) > 0;
}

/* A plain version string: digits, dots, and nothing that could surprise
 * a file name or a shell. */
int lsiw_version_valid(const char *v)
{
    if (!v || !isdigit((unsigned char)*v) || strlen(v) > 32) return 0;
    for (; *v; v++)
        if (!isdigit((unsigned char)*v) && *v != '.') return 0;
    return 1;
}
