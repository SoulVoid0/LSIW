/* t_env: command line, environment, Windows folders and system information.
 *
 * run-tests.sh passes these arguments (Linux argv, quoted for the shell):
 *     alpha "two words" 'with"quote' 'back\slash' 'trail\' ''
 * LSIW turns them into a Windows command line and the C runtime parses it
 * back; the program must see exactly the same strings. Started without
 * arguments, the argv checks are skipped.
 *
 * advapi32 and shell32 functions are called last, so an engine without them
 * still runs (and reports) every kernel32 / CRT check first.
 *
 * The folder variables must be Windows paths on C: (LSIW maps every one of
 * them to a Linux folder, the whole Linux filesystem being C:).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"
#include <shlobj.h>
#include <shellapi.h>
#include <time.h>

typedef LONG (WINAPI *RtlGetVersion_fn)(OSVERSIONINFOW *);

static const char *const expected_args[] = {
    "alpha", "two words", "with\"quote", "back\\slash", "trail\\", "",
};
#define NEXPECTED (int)(sizeof expected_args / sizeof expected_args[0])

static void test_args(int argc, char **argv)
{
    CHECK(argc >= 1 && argv[0] && (t_ends_with_i(argv[0], "t_env.exe") || t_ends_with_i(argv[0], "t_env")),
          "argv[0] names the program");
    LPWSTR cl = GetCommandLineW();
    CHECK(cl && cl[0], "GetCommandLineW non-empty");
    LPSTR cla = GetCommandLineA();
    CHECK(cla && strstr(cla, "t_env") != NULL, "GetCommandLineA mentions the program");

    if (argc == 1) {
        SKIP("argv matches the arguments given", "started without the run-tests.sh arguments");
        return;
    }
    CHECKF(argc == NEXPECTED + 1, "argc", "argc=%d expected %d", argc, NEXPECTED + 1);
    for (int i = 1; i < argc && i <= NEXPECTED; i++) {
        char name[64];
        snprintf(name, sizeof name, "argv[%d]", i);
        CHECKF(strcmp(argv[i], expected_args[i - 1]) == 0, name, "got [%s] expected [%s]",
               argv[i], expected_args[i - 1]);
    }

}

static void test_env_vars(void)
{
    static const char *const names[] = {
        "APPDATA", "LOCALAPPDATA", "USERPROFILE", "TEMP", "TMP", "SystemRoot", "ProgramFiles",
        "windir", "ProgramData", "SystemDrive", "ComSpec",
    };
    char v[MAX_PATH], name[64];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        DWORD n = GetEnvironmentVariableA(names[i], v, sizeof v);
        snprintf(name, sizeof name, "%%%s%% is a C:\\ path", names[i]);
        if (!strcmp(names[i], "SystemDrive"))
            CHECKF(n == 2 && _stricmp(v, "C:") == 0, "%SystemDrive% is C:", "n=%lu v=%s", (unsigned long)n, n ? v : "");
        else
            CHECKF(n > 3 && n < sizeof v && t_is_c_path(v), name, "n=%lu v=%s", (unsigned long)n, n ? v : "");
    }

    char home[MAX_PATH], exp[MAX_PATH * 2], want[MAX_PATH * 2];
    GetEnvironmentVariableA("USERPROFILE", home, sizeof home);
    DWORD n = ExpandEnvironmentStringsA("%USERPROFILE%\\Docs", exp, sizeof exp);
    snprintf(want, sizeof want, "%s\\Docs", home);
    CHECKF(strcmp(exp, want) == 0 && n == strlen(want) + 1, "ExpandEnvironmentStringsA %USERPROFILE%",
           "got \"%s\" n=%lu", exp, (unsigned long)n);
    ExpandEnvironmentStringsA("[%LSIW_NOT_DEFINED_VAR%] %%", exp, sizeof exp);
    CHECKF(strcmp(exp, "[%LSIW_NOT_DEFINED_VAR%] %%") == 0, "ExpandEnvironmentStringsA leaves unknown names",
           "got \"%s\"", exp);
    ExpandEnvironmentStringsA("%systemroot%", exp, sizeof exp);
    GetEnvironmentVariableA("SystemRoot", want, sizeof want);
    CHECK(strcmp(exp, want) == 0, "ExpandEnvironmentStringsA is case-insensitive");
    n = ExpandEnvironmentStringsA("%USERPROFILE%", exp, 2);
    CHECKF(n == strlen(home) + 1, "ExpandEnvironmentStringsA small buffer returns the size needed",
           "n=%lu", (unsigned long)n);

    CHECK(SetEnvironmentVariableA("LSIW_T_ENV", "value 1"), "SetEnvironmentVariableA");
    n = GetEnvironmentVariableA("lsiw_t_env", v, sizeof v);
    CHECKF(n == 7 && strcmp(v, "value 1") == 0, "GetEnvironmentVariableA round trip, any case",
           "n=%lu v=%s", (unsigned long)n, v);
    n = GetEnvironmentVariableA("LSIW_T_ENV", v, 3);
    CHECKF(n == 8, "GetEnvironmentVariableA small buffer returns size incl. NUL", "n=%lu", (unsigned long)n);
    WCHAR wv[64];
    n = GetEnvironmentVariableW(L"LSIW_T_ENV", wv, 64);
    CHECK(n == 7 && wcscmp(wv, L"value 1") == 0, "GetEnvironmentVariableW sees the same variable");
    const char *crt = getenv("LSIW_T_ENV");
    printf("# getenv after SetEnvironmentVariableA: %s\n", crt ? "visible" : "not visible (as on Windows)");

    LPWCH block = GetEnvironmentStringsW();
    int found = 0;
    for (LPWCH p = block; p && *p; p += wcslen(p) + 1)
        if (!wcsncmp(p, L"LSIW_T_ENV=value 1", 18)) found = 1;
    CHECK(found, "GetEnvironmentStringsW contains the new variable");
    FreeEnvironmentStringsW(block);

    CHECK(SetEnvironmentVariableA("LSIW_T_ENV", NULL), "SetEnvironmentVariableA(NULL) deletes");
    SetLastError(0);
    n = GetEnvironmentVariableA("LSIW_T_ENV", v, sizeof v);
    CHECKF(n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND, "deleted variable -> ERROR_ENVVAR_NOT_FOUND",
           "n=%lu err=%lu", (unsigned long)n, GetLastError());
}

static void test_module(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    CHECKF(n > 0 && n == strlen(path) && t_ends_with_i(path, "\\t_env.exe"),
           "GetModuleFileNameA ends with \\t_env.exe", "got \"%s\"", path);
    CHECKF_LSIW(t_is_c_path(path), "GetModuleFileNameA is a C:\\ path", "got \"%s\"", path);
    char small[4];
    SetLastError(0);
    n = GetModuleFileNameA(NULL, small, sizeof small);
    CHECKF(n == 4 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetModuleFileNameA truncation",
           "n=%lu err=%lu", (unsigned long)n, GetLastError());
    WCHAR wpath[MAX_PATH];
    n = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    CHECK(n > 0 && wcslen(wpath) == n, "GetModuleFileNameW");

    HMODULE self = GetModuleHandleA(NULL);
    CHECK(self != NULL && ((const char *)self)[0] == 'M' && ((const char *)self)[1] == 'Z',
          "GetModuleHandleA(NULL) points at the MZ header");
    CHECK(GetModuleHandleA("t_env.exe") == self, "GetModuleHandleA(\"t_env.exe\")");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    CHECK(k32 != NULL && GetProcAddress(k32, "GetProcAddress") != NULL, "GetProcAddress in kernel32");
    SetLastError(0);
    CHECKF(GetProcAddress(k32, "NoSuchFunctionInKernel32") == NULL && GetLastError() == ERROR_PROC_NOT_FOUND,
           "GetProcAddress missing -> ERROR_PROC_NOT_FOUND", "err=%lu", GetLastError());
    SetLastError(0);
    CHECKF(LoadLibraryA("lsiw_no_such_library.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND,
           "LoadLibraryA missing -> ERROR_MOD_NOT_FOUND", "err=%lu", GetLastError());
}

static void test_system(void)
{
    char name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = sizeof name;
    CHECKF(GetComputerNameA(name, &n) && n > 0 && n == strlen(name), "GetComputerNameA",
           "n=%lu", (unsigned long)n);


    OSVERSIONINFOW rv;
    memset(&rv, 0, sizeof rv);
    rv.dwOSVersionInfoSize = sizeof rv;
    RtlGetVersion_fn rgv = (RtlGetVersion_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    CHECKF(rgv && rgv(&rv) == 0 && rv.dwMajorVersion >= 10, "RtlGetVersion major >= 10",
           "major=%lu", (unsigned long)rv.dwMajorVersion);
    OSVERSIONINFOA ov;
    memset(&ov, 0, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    BOOL ok = GetVersionExA(&ov);
#pragma GCC diagnostic pop
    /* A program without a compatibility manifest is told 6.2 on Windows 8.1
     * and later; LSIW and Wine may report the real version (10). */
    CHECKF(ok && (ov.dwMajorVersion >= 10 || (ov.dwMajorVersion == 6 && ov.dwMinorVersion == 2)),
           "GetVersionExA reports 10 (or 6.2 for unmanifested programs)", "%lu.%lu",
           (unsigned long)ov.dwMajorVersion, (unsigned long)ov.dwMinorVersion);
    CHECK(ov.dwPlatformId == VER_PLATFORM_WIN32_NT, "VER_PLATFORM_WIN32_NT");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECKF(si.dwNumberOfProcessors >= 1, "GetSystemInfo processors >= 1", "n=%lu", (unsigned long)si.dwNumberOfProcessors);
    CHECK(si.dwPageSize == 4096 && si.dwAllocationGranularity == 65536, "page size 4096, granularity 64K");
    CHECK(si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "PROCESSOR_ARCHITECTURE_AMD64");
    SYSTEM_INFO nsi;
    GetNativeSystemInfo(&nsi);
    CHECK(nsi.dwNumberOfProcessors == si.dwNumberOfProcessors, "GetNativeSystemInfo");
    BOOL wow = TRUE;
    CHECK(IsWow64Process(GetCurrentProcess(), &wow) && !wow, "IsWow64Process is FALSE for x64");
    CHECK(GetCurrentProcess() == (HANDLE)(LONG_PTR)-1 && GetCurrentThread() == (HANDLE)(LONG_PTR)-2,
          "pseudo handles");
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    CHECK(GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0 && ms.ullAvailPhys <= ms.ullTotalPhys,
          "GlobalMemoryStatusEx");

    char dir[MAX_PATH], root[MAX_PATH];
    n = GetWindowsDirectoryA(dir, sizeof dir);
    GetEnvironmentVariableA("SystemRoot", root, sizeof root);
    CHECKF(n > 0 && _stricmp(dir, root) == 0, "GetWindowsDirectoryA matches %SystemRoot%", "dir=%s", dir);
    n = GetSystemDirectoryA(dir, sizeof dir);
    CHECKF(n > 0 && t_is_c_path(dir) && t_ends_with_i(dir, "\\system32"), "GetSystemDirectoryA",
           "dir=%s", dir);

    /* Clocks: consistent with each other, monotonic where they must be. */
    LARGE_INTEGER f, c1, c2;
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart > 0, "QueryPerformanceFrequency");
    QueryPerformanceCounter(&c1);
    Sleep(20);
    QueryPerformanceCounter(&c2);
    double ms_el = (double)(c2.QuadPart - c1.QuadPart) * 1000.0 / (double)f.QuadPart;
    CHECKF(ms_el >= 15 && ms_el < 2000, "QueryPerformanceCounter measures Sleep(20)", "%.1f ms", ms_el);
    ULONGLONG t1 = GetTickCount64();
    CHECK(t1 > 0 && GetTickCount64() >= t1, "GetTickCount64 monotonic");
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG ft64 = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    long long unix_from_ft = (long long)(ft64 / 10000000ULL) - 11644473600LL;
    long long now = (long long)time(NULL);
    CHECKF(unix_from_ft - now < 5 && now - unix_from_ft < 5, "GetSystemTimeAsFileTime agrees with time()",
           "ft=%lld time=%lld", unix_from_ft, now);
    SYSTEMTIME st;
    GetSystemTime(&st);
    CHECK(st.wYear >= 2024 && st.wMonth >= 1 && st.wMonth <= 12, "GetSystemTime");
    FILETIME back;
    CHECK(SystemTimeToFileTime(&st, &back) &&
          (((ULONGLONG)back.dwHighDateTime << 32 | back.dwLowDateTime) / 10000000ULL) - ft64 / 10000000ULL < 5,
          "SystemTimeToFileTime");
    TIME_ZONE_INFORMATION tz;
    CHECK(GetTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID, "GetTimeZoneInformation");
}

/* shell32's parser must agree with the C runtime's. */
static void test_cmdline_shell32(int argc, char **argv)
{
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    CHECKF(wargv && wargc == argc, "CommandLineToArgvW count", "wargc=%d argc=%d", wargc, argc);
    int same = wargv != NULL;
    for (int i = 1; wargv && i < argc && i < wargc; i++) {
        char u[256];
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u, sizeof u, NULL, NULL);
        if (strcmp(u, argv[i])) { same = 0; printf("# CommandLineToArgvW[%d]=[%s]\n", i, u); }
    }
    CHECK(same, "CommandLineToArgvW agrees with argv");
    LocalFree(wargv);
}

/* advapi32 */
static void test_user(void)
{
    char user[256];
    DWORD un = sizeof user;
    CHECKF(GetUserNameA(user, &un) && un == strlen(user) + 1 && user[0], "GetUserNameA (size includes NUL)",
           "un=%lu", (unsigned long)un);
    char envuser[256];
    if (GetEnvironmentVariableA("USERNAME", envuser, sizeof envuser))
        CHECKF(_stricmp(envuser, user) == 0, "GetUserNameA matches %USERNAME%", "env=%s api=%s", envuser, user);
    else
        CHECK(0, "%USERNAME% is set");
}

static void test_folders(void)
{
    static const struct { int csidl; const char *name, *env; } f[] = {
        { CSIDL_APPDATA, "CSIDL_APPDATA", "APPDATA" },
        { CSIDL_LOCAL_APPDATA, "CSIDL_LOCAL_APPDATA", "LOCALAPPDATA" },
        { CSIDL_PERSONAL, "CSIDL_PERSONAL", NULL },
        { CSIDL_DESKTOPDIRECTORY, "CSIDL_DESKTOPDIRECTORY", NULL },
        { CSIDL_PROFILE, "CSIDL_PROFILE", "USERPROFILE" },
        { CSIDL_PROGRAM_FILES, "CSIDL_PROGRAM_FILES", "ProgramFiles" },
        { CSIDL_COMMON_APPDATA, "CSIDL_COMMON_APPDATA", "ProgramData" },
        { CSIDL_WINDOWS, "CSIDL_WINDOWS", "SystemRoot" },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        char path[MAX_PATH] = "", name[96];
        HRESULT hr = SHGetFolderPathA(NULL, f[i].csidl, NULL, SHGFP_TYPE_CURRENT, path);
        snprintf(name, sizeof name, "SHGetFolderPathA(%s) is a C:\\ path", f[i].name);
        CHECKF(hr == S_OK && t_is_c_path(path), name, "hr=%lx path=%s", (unsigned long)hr, path);
        if (f[i].env) {
            char env[MAX_PATH] = "";
            GetEnvironmentVariableA(f[i].env, env, sizeof env);
            snprintf(name, sizeof name, "SHGetFolderPathA(%s) matches %%%s%%", f[i].name, f[i].env);
            CHECKF(_stricmp(env, path) == 0, name, "env=%s path=%s", env, path);
        }
    }
    char path[MAX_PATH];
    HRESULT hr = SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, path);
    DWORD a = GetFileAttributesA(path);
    CHECK(hr == S_OK && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY),
          "CSIDL_FLAG_CREATE: the AppData folder exists");
}

int main(int argc, char **argv)
{
    t_init("t_env");
    test_args(argc, argv);
    test_env_vars();
    test_module();
    test_system();
    /* The checks below need advapi32 and shell32; they come last so a
     * missing function stops the program after everything else ran. */
    test_user();
    if (argc > 1) test_cmdline_shell32(argc, argv);
    test_folders();
    return t_done();
}
