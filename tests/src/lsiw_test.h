/* Tiny self-checking test framework shared by the LSIW Windows test programs.
 *
 * Every test prints one line per check ("ok - name" or "FAIL - name: why")
 * and ends with "RESULT: PASS" or "RESULT: FAIL (<n> failures)", which is
 * what tests/run-tests.sh looks for. Output is flushed after every line so
 * a crash still leaves everything printed so far in the log.
 *
 * A few checks describe LSIW's own design (the whole Linux filesystem is
 * C:, there is no D: drive, ...). Wine maps drives differently, so when the
 * program runs under plain Wine those checks are reported as "skip" lines
 * instead of failures; that lets the same binaries validate themselves on
 * Wine. Detection: Wine's ntdll exports wine_get_version, LSIW's does not.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef LSIW_TEST_H
#define LSIW_TEST_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures;
static int t_checks;

static inline int t_is_wine(void)
{
    static int cached = -1;
    if (cached < 0) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        cached = nt && GetProcAddress(nt, "wine_get_version") != NULL;
    }
    return cached;
}

static inline void t_init(const char *name)
{
    /* Keep stdout and stderr in program order even when they go to a pipe. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("# %s\n", name);
}

#define CHECK(cond, name) CHECKF((cond), name, "%s", #cond)

#define CHECKF(cond, name, ...)                                   \
    do {                                                          \
        t_checks++;                                               \
        if (cond) {                                               \
            printf("ok - %s\n", name);                            \
        } else {                                                  \
            t_failures++;                                         \
            printf("FAIL - %s: ", name);                          \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
        fflush(stdout);                                           \
    } while (0)

/* A check of LSIW-specific behaviour: skipped (not failed) under Wine. */
#define CHECK_LSIW(cond, name) CHECKF_LSIW((cond), name, "%s", #cond)
#define CHECKF_LSIW(cond, name, ...)                              \
    do {                                                          \
        if (t_is_wine() && !(cond)) {                             \
            printf("skip - %s: LSIW-specific, not true on Wine\n", name); \
            fflush(stdout);                                       \
        } else {                                                  \
            CHECKF((cond), name, __VA_ARGS__);                    \
        }                                                         \
    } while (0)

#define SKIP(name, why)                                           \
    do { printf("skip - %s: %s\n", name, why); fflush(stdout); } while (0)

static inline int t_done(void)
{
    if (t_failures)
        printf("RESULT: FAIL (%d failures)\n", t_failures);
    else
        printf("RESULT: PASS\n");
    fflush(stdout);
    return t_failures ? 1 : 0;
}

/* Starts with "C:\" (any case of the drive letter). */
static inline int t_is_c_path(const char *s)
{
    return s && (s[0] == 'C' || s[0] == 'c') && s[1] == ':' && s[2] == '\\';
}

static inline int t_ends_with_i(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && _stricmp(s + n - m, suffix) == 0;
}

#endif
