/* t_process: child processes.
 *
 * The program starts copies of itself in a child mode ("t_process.exe
 * child ..."): CreateProcessA with stdout redirected into an anonymous pipe
 * (STARTF_USESTDHANDLES), the child's exit code (42), environment and
 * current-folder inheritance, TerminateProcess, then the C runtime's
 * system() and _popen().
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"

static char g_self[MAX_PATH];

static int child_main(int argc, char **argv)
{
    char cwd[MAX_PATH], env[64] = "";
    GetCurrentDirectoryA(sizeof cwd, cwd);
    if (!GetEnvironmentVariableA("LSIW_T_PROCESS", env, sizeof env)) strcpy(env, "(unset)");
    printf("child says hello\n");
    printf("child pid=%lu\n", (unsigned long)GetCurrentProcessId());
    printf("child argc=%d", argc);
    for (int i = 2; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\nchild env=%s\nchild cwd=%s\n", env, cwd);
    fflush(stdout);
    return 42;
}

/* Run "self child ARGS" with stdout+stderr in a pipe; returns the output. */
static int run_child(const char *args, const char *cwd, char *out, size_t outsz, DWORD *code, DWORD *pid)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    char cmd[2 * MAX_PATH];
    snprintf(cmd, sizeof cmd, "\"%s\" child %s", g_self, args);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return 0; }
    size_t total = 0;
    DWORD n;
    while (total < outsz - 1 && ReadFile(rd, out + total, (DWORD)(outsz - 1 - total), &n, NULL) && n) total += n;
    out[total] = 0;
    CloseHandle(rd);
    DWORD w = WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, code);
    *pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return w == WAIT_OBJECT_0;
}

static void test_createprocess(void)
{
    char out[4096], expect[256], tmp[MAX_PATH];
    DWORD code = 0, pid = 0;
    SetEnvironmentVariableA("LSIW_T_PROCESS", "inherited value");
    GetTempPathA(sizeof tmp, tmp);
    if (strlen(tmp) > 3) tmp[strlen(tmp) - 1] = 0;           /* no trailing backslash */
    int ok = run_child("one \"two words\"", tmp, out, sizeof out, &code, &pid);
    CHECK(ok, "CreateProcessA + WaitForSingleObject");
    CHECKF(strstr(out, "child says hello\n") != NULL || strstr(out, "child says hello\r\n") != NULL,
           "read the child's stdout through the pipe", "output: %s", out);
    CHECKF(code == 42, "GetExitCodeProcess is the child's return value", "code=%lu", (unsigned long)code);
    snprintf(expect, sizeof expect, "child pid=%lu", (unsigned long)pid);
    CHECKF(pid != 0 && pid != GetCurrentProcessId() && strstr(out, expect), "dwProcessId matches the child's GetCurrentProcessId",
           "pid=%lu output: %s", (unsigned long)pid, out);
    CHECKF(strstr(out, "child argc=4 [one] [two words]") != NULL, "child gets the command-line arguments",
           "output: %s", out);
    CHECKF(strstr(out, "child env=inherited value") != NULL, "child inherits the environment", "output: %s", out);
    snprintf(expect, sizeof expect, "child cwd=%s", tmp);
    char *p = strstr(out, "child cwd=");
    CHECKF(p && _strnicmp(p, expect, strlen(expect)) == 0, "lpCurrentDirectory sets the child's folder",
           "expected %s, output: %s", expect, out);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" sleep", g_self);
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "CreateProcessA sleeper");
    CHECK(WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT, "running child: wait times out");
    DWORD c = 0;
    CHECK(GetExitCodeProcess(pi.hProcess, &c) && c == STILL_ACTIVE, "running child: STILL_ACTIVE");
    CHECK(TerminateProcess(pi.hProcess, 7), "TerminateProcess");
    CHECK(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "terminated child is signalled");
    CHECKF(GetExitCodeProcess(pi.hProcess, &c) && c == 7, "TerminateProcess exit code", "code=%lu", (unsigned long)c);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    SetLastError(0);
    strcpy(cmd, "C:\\lsiw_no_such_dir\\nothing.exe");
    CHECKF(!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) &&
           (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND),
           "CreateProcessA missing program -> ERROR_FILE/PATH_NOT_FOUND", "err=%lu", GetLastError());
    CHECK(GetCurrentProcessId() != 0, "GetCurrentProcessId");
}

static void test_crt(void)
{
    fflush(stdout);
    int r = system("echo hi");
    CHECKF(r == 0, "system(\"echo hi\") returns 0", "r=%d", r);
    r = system("exit 3");
    CHECKF(r == 3, "system(\"exit 3\") returns 3", "r=%d", r);
    CHECK(system(NULL) != 0, "system(NULL): a command interpreter exists");

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"\"%s\" child popen\"", g_self);   /* cmd /c quoting */
    FILE *f = _popen(cmd, "r");
    CHECK(f != NULL, "_popen");
    int hello = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "child says hello", 16)) hello = 1;
        int rc = _pclose(f);
        CHECKF(rc == 42, "_pclose returns the child's exit code", "rc=%d", rc);
    }
    CHECK(hello, "_popen reads the child's output");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "sleep")) { Sleep(30000); return 0; }
    t_init("t_process");
    GetModuleFileNameA(NULL, g_self, sizeof g_self);
    test_createprocess();
    test_crt();
    return t_done();
}
