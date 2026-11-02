/* The native engine's entry point: prepare this Linux process to host a
 * Windows program (TEB, handles, exception handling, environment, command
 * line, C runtime state), load the program with LSIW's PE loader and jump
 * to its entry point.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

int lsiw_native_run(const char *linux_path, int argc, char **argv, char **why)
{
    char *full = realpath(linux_path, NULL);
    if (!full) {
        if (why) *why = lsiw_strdup_printf("%s: no such file", linux_path);
        return -1;
    }

    /* A Windows program writing to a closed pipe gets ERROR_BROKEN_PIPE /
     * EPIPE back instead of being killed. */
    signal(SIGPIPE, SIG_IGN);

    lsiw_handles_init();
    if (!lsiw_teb_create()) {
        if (why) *why = lsiw_strdup_printf("cannot set up the thread environment block");
        free(full);
        return -1;
    }
    lsiw_fault_init();
    lsiw_env_init();
    /* Programs expect the system folder to exist (Go probes a path inside it
     * at start-up); outside a Wine prefix it may not exist yet. */
    const char *sys = lsiw_var_windows("SYSTEM32");
    char *sys_lin = lsiw_win_to_linux(sys ? sys : "C:\\windows\\system32");
    if (sys_lin) { lsiw_mkdirs(sys_lin, 0755); free(sys_lin); }

    char *win = lsiw_linux_to_win(full);
    lsiw_set_command_line(argc, argv, win);
    lsiw_crt_init();

    LSIW_TRACE("native engine: %s (%s)\n", full, win);
    struct lsiw_module *exe = lsiw_load_pe(full, 1, why);
    free(win);
    free(full);
    if (!exe) return -1;
    if (!exe->entry) {
        if (why) *why = lsiw_strdup_printf("%s has no entry point", exe->name);
        return -1;
    }
    return lsiw_run_exe(exe);
}
