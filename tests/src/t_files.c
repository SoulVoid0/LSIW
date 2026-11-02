/* t_files: files and directories through kernel32 and the C runtime.
 *
 * Works inside a fresh folder under GetTempPath and removes it at the end.
 * The LSIW-specific checks (the whole Linux filesystem is C:, other drive
 * letters fold onto C:, GetLogicalDrives reports only C:) are skipped under
 * plain Wine, which maps drives differently.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>

static char g_dir[MAX_PATH];

static const char *in_dir(const char *name)
{
    static char buf[4][MAX_PATH];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, MAX_PATH, "%s\\%s", g_dir, name);
    return b;
}

static void write_file(const char *path, const char *text)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
        CloseHandle(h);
    }
}

static void test_temp_dir(void)
{
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof tmp, tmp);
    CHECKF(n > 3 && n < sizeof tmp && tmp[n - 1] == '\\', "GetTempPathA ends with a backslash",
           "n=%lu", (unsigned long)n);
    CHECKF(t_is_c_path(tmp), "GetTempPathA is on C:", "tmp=%s", tmp);
    DWORD a = GetFileAttributesA(tmp);
    CHECK(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY), "temp folder exists");

    snprintf(g_dir, sizeof g_dir, "%slsiw_t_files_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CHECK(CreateDirectoryA(g_dir, NULL), "CreateDirectoryA");
    SetLastError(0);
    CHECKF(!CreateDirectoryA(g_dir, NULL) && GetLastError() == ERROR_ALREADY_EXISTS,
           "CreateDirectoryA twice -> ERROR_ALREADY_EXISTS", "err=%lu", GetLastError());
    SetLastError(0);
    CHECKF(!CreateDirectoryA(in_dir("no\\such\\parent"), NULL) && GetLastError() == ERROR_PATH_NOT_FOUND,
           "CreateDirectoryA without parent -> ERROR_PATH_NOT_FOUND", "err=%lu", GetLastError());
}

static void test_handles(void)
{
    static unsigned char data[10000], back[10000];
    for (int i = 0; i < (int)sizeof data; i++) data[i] = (unsigned char)(i * 7 + 3);

    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, in_dir("data.bin"), -1, wpath, MAX_PATH);
    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW CREATE_ALWAYS");
    CHECKF(GetLastError() == 0, "CreateFileW CREATE_ALWAYS on a new file: last error 0",
           "err=%lu", GetLastError());
    DWORD n = 0;
    CHECK(WriteFile(h, data, sizeof data, &n, NULL) && n == sizeof data, "WriteFile 10000 bytes");
    LARGE_INTEGER size = { 0 }, pos = { 0 }, newpos = { 0 };
    CHECK(GetFileSizeEx(h, &size) && size.QuadPart == 10000, "GetFileSizeEx");
    DWORD hi = 1;
    CHECK(GetFileSize(h, &hi) == 10000 && hi == 0, "GetFileSize");
    pos.QuadPart = 0;
    CHECK(SetFilePointerEx(h, pos, &newpos, FILE_BEGIN) && newpos.QuadPart == 0, "SetFilePointerEx FILE_BEGIN");
    CHECK(ReadFile(h, back, sizeof back, &n, NULL) && n == sizeof back, "ReadFile 10000 bytes");
    CHECK(memcmp(data, back, sizeof data) == 0, "read back what was written");
    CHECK(ReadFile(h, back, 100, &n, NULL) && n == 0, "ReadFile at EOF: TRUE with 0 bytes");
    pos.QuadPart = -10;
    CHECK(SetFilePointerEx(h, pos, &newpos, FILE_END) && newpos.QuadPart == 9990, "SetFilePointerEx FILE_END");
    CHECK(ReadFile(h, back, 100, &n, NULL) && n == 10 && memcmp(back, data + 9990, 10) == 0, "short read");
    pos.QuadPart = -20;
    CHECK(SetFilePointerEx(h, pos, &newpos, FILE_CURRENT) && newpos.QuadPart == 9980, "SetFilePointerEx FILE_CURRENT");
    CHECK(SetFilePointer(h, 0, NULL, FILE_CURRENT) == 9980, "SetFilePointer query");
    pos.QuadPart = -1;
    SetLastError(0);
    CHECKF(!SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && GetLastError() == ERROR_NEGATIVE_SEEK,
           "seek before start -> ERROR_NEGATIVE_SEEK", "err=%lu", GetLastError());
    pos.QuadPart = 5000;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    CHECK(SetEndOfFile(h) && GetFileSizeEx(h, &size) && size.QuadPart == 5000, "SetEndOfFile truncates");
    CHECK(FlushFileBuffers(h), "FlushFileBuffers");
    FILETIME ct, at, wt;
    CHECK(GetFileTime(h, &ct, &at, &wt) && (wt.dwHighDateTime || wt.dwLowDateTime), "GetFileTime");
    BY_HANDLE_FILE_INFORMATION info;
    CHECK(GetFileInformationByHandle(h, &info) && info.nFileSizeLow == 5000 && info.nNumberOfLinks == 1,
          "GetFileInformationByHandle");
    CHECK(GetFileType(h) == FILE_TYPE_DISK, "GetFileType disk");
    CHECK(CloseHandle(h), "CloseHandle");

    SetLastError(0);
    h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
    CHECKF(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS,
           "CREATE_NEW on an existing file -> ERROR_FILE_EXISTS", "err=%lu", GetLastError());
    h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    CHECKF(h != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS,
           "OPEN_ALWAYS on an existing file sets ERROR_ALREADY_EXISTS", "err=%lu", GetLastError());
    CloseHandle(h);
    SetLastError(0);
    h = CreateFileA(in_dir("missing.bin"), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECKF(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND,
           "OPEN_EXISTING missing file -> ERROR_FILE_NOT_FOUND", "err=%lu", GetLastError());
    SetLastError(0);
    h = CreateFileA(in_dir("nodir\\missing.bin"), GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECKF(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
           "missing folder -> ERROR_PATH_NOT_FOUND", "err=%lu", GetLastError());

    /* Unicode file name: stored and listed with the exact UTF-16 name. */
    WCHAR uni[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, g_dir, -1, uni, MAX_PATH);
    wcscat(uni, L"\\\u00fcn\u00efc\u00f6de-\u20ac.txt");
    h = CreateFileW(uni, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW non-ASCII name");
    CloseHandle(h);
    WIN32_FIND_DATAW fw;
    HANDLE fh = FindFirstFileW(uni, &fw);
    CHECK(fh != INVALID_HANDLE_VALUE && wcscmp(fw.cFileName, L"\u00fcn\u00efc\u00f6de-\u20ac.txt") == 0,
          "FindFirstFileW returns the non-ASCII name");
    if (fh != INVALID_HANDLE_VALUE) FindClose(fh);
    CHECK(DeleteFileW(uni), "DeleteFileW non-ASCII name");
}

static void test_find(void)
{
    write_file(in_dir("a.txt"), "aaaa");
    write_file(in_dir("b.txt"), "bb");
    write_file(in_dir("c.log"), "c");
    CreateDirectoryA(in_dir("sub"), NULL);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(in_dir("*"), &fd);
    CHECK(h != INVALID_HANDLE_VALUE, "FindFirstFileA *");
    int dot = 0, dotdot = 0, a = 0, b = 0, c = 0, sub = 0, other = 0, data = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *n = fd.cFileName;
            if (!strcmp(n, ".")) dot++;
            else if (!strcmp(n, "..")) dotdot++;
            else if (!strcmp(n, "a.txt")) a += fd.nFileSizeLow == 4 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            else if (!strcmp(n, "b.txt")) b += fd.nFileSizeLow == 2;
            else if (!strcmp(n, "c.log")) c++;
            else if (!strcmp(n, "sub")) sub += (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            else if (!strcmp(n, "data.bin")) data++;
            else { other++; printf("# unexpected entry %s\n", n); }
        } while (FindNextFileA(h, &fd));
        CHECKF(GetLastError() == ERROR_NO_MORE_FILES, "FindNextFileA ends with ERROR_NO_MORE_FILES",
               "err=%lu", GetLastError());
        FindClose(h);
    }
    CHECKF(a == 1 && b == 1 && c == 1 && sub == 1 && data == 1 && other == 0,
           "enumeration finds every entry once with sizes and attributes",
           "a=%d b=%d c=%d sub=%d data=%d other=%d", a, b, c, sub, data, other);
    CHECKF(dot == 1 && dotdot == 1, "enumeration of a folder lists . and ..", "dot=%d dotdot=%d", dot, dotdot);

    int txt = 0;
    h = FindFirstFileA(in_dir("*.txt"), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do txt++; while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CHECKF(txt == 2, "FindFirstFileA *.txt", "matches=%d", txt);
    int q = 0;
    h = FindFirstFileA(in_dir("?.LOG"), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do q += !strcmp(fd.cFileName, "c.log"); while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CHECK(q == 1, "FindFirstFileA ?.LOG matches case-insensitively");
    SetLastError(0);
    h = FindFirstFileA(in_dir("*.nothing"), &fd);
    CHECKF(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND,
           "FindFirstFileA without match -> ERROR_FILE_NOT_FOUND", "err=%lu", GetLastError());
}

static void test_move_delete(void)
{
    CHECK(MoveFileExA(in_dir("a.txt"), in_dir("moved.txt"), 0), "MoveFileExA rename");
    CHECK(GetFileAttributesA(in_dir("a.txt")) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesA(in_dir("moved.txt")) != INVALID_FILE_ATTRIBUTES, "rename moved the file");
    SetLastError(0);
    CHECKF(!MoveFileExA(in_dir("moved.txt"), in_dir("b.txt"), 0) && GetLastError() == ERROR_ALREADY_EXISTS,
           "MoveFileExA onto an existing file -> ERROR_ALREADY_EXISTS", "err=%lu", GetLastError());
    CHECK(MoveFileExA(in_dir("moved.txt"), in_dir("b.txt"), MOVEFILE_REPLACE_EXISTING),
          "MoveFileExA MOVEFILE_REPLACE_EXISTING");
    WIN32_FILE_ATTRIBUTE_DATA ad;
    CHECK(GetFileAttributesExA(in_dir("b.txt"), GetFileExInfoStandard, &ad) && ad.nFileSizeLow == 4,
          "replaced file has the moved contents");
    CHECK(MoveFileA(in_dir("c.log"), in_dir("sub\\c.log")), "MoveFileA into a subfolder");

    CHECK(CopyFileA(in_dir("b.txt"), in_dir("copy.txt"), TRUE), "CopyFileA");
    SetLastError(0);
    CHECKF(!CopyFileA(in_dir("b.txt"), in_dir("copy.txt"), TRUE) && GetLastError() == ERROR_FILE_EXISTS,
           "CopyFileA fail-if-exists -> ERROR_FILE_EXISTS", "err=%lu", GetLastError());

    CHECK(DeleteFileA(in_dir("copy.txt")), "DeleteFileA");
    SetLastError(0);
    CHECKF(GetFileAttributesA(in_dir("copy.txt")) == INVALID_FILE_ATTRIBUTES &&
           GetLastError() == ERROR_FILE_NOT_FOUND, "deleted file is gone (ERROR_FILE_NOT_FOUND)",
           "err=%lu", GetLastError());
    SetLastError(0);
    CHECKF(!DeleteFileA(in_dir("copy.txt")) && GetLastError() == ERROR_FILE_NOT_FOUND,
           "DeleteFileA missing -> ERROR_FILE_NOT_FOUND", "err=%lu", GetLastError());

    SetLastError(0);
    CHECKF(!RemoveDirectoryA(in_dir("sub")) && GetLastError() == ERROR_DIR_NOT_EMPTY,
           "RemoveDirectoryA non-empty -> ERROR_DIR_NOT_EMPTY", "err=%lu", GetLastError());
    DeleteFileA(in_dir("sub\\c.log"));
    CHECK(RemoveDirectoryA(in_dir("sub")), "RemoveDirectoryA");
    CHECK(GetFileAttributesA(in_dir("sub")) == INVALID_FILE_ATTRIBUTES, "folder is gone");

    CHECK(SetFileAttributesA(in_dir("b.txt"), FILE_ATTRIBUTE_READONLY) &&
          (GetFileAttributesA(in_dir("b.txt")) & FILE_ATTRIBUTE_READONLY), "SetFileAttributesA read-only");
    SetLastError(0);
    CHECKF(!DeleteFileA(in_dir("b.txt")) && GetLastError() == ERROR_ACCESS_DENIED,
           "DeleteFileA read-only -> ERROR_ACCESS_DENIED", "err=%lu", GetLastError());
    SetFileAttributesA(in_dir("b.txt"), FILE_ATTRIBUTE_NORMAL);
    CHECK(DeleteFileA(in_dir("b.txt")), "DeleteFileA after clearing read-only");
}

static void test_paths(void)
{
    char out[MAX_PATH], *file = NULL;
    DWORD n = GetFullPathNameA("C:\\a\\..\\b", sizeof out, out, &file);
    CHECKF(n == 4 && strcmp(out, "C:\\b") == 0 && file == out + 3, "GetFullPathNameA C:\\a\\..\\b",
           "got \"%s\" n=%lu", out, (unsigned long)n);
    n = GetFullPathNameA("C:/x/./y//z.txt", sizeof out, out, &file);
    CHECKF(strcmp(out, "C:\\x\\y\\z.txt") == 0 && file && strcmp(file, "z.txt") == 0,
           "GetFullPathNameA forward slashes and dot", "got \"%s\"", out);
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);
    n = GetFullPathNameA("rel.txt", sizeof out, out, NULL);
    char expect[MAX_PATH * 2];
    snprintf(expect, sizeof expect, "%s%srel.txt", cwd, cwd[strlen(cwd) - 1] == '\\' ? "" : "\\");
    CHECKF(_stricmp(out, expect) == 0, "GetFullPathNameA relative", "got \"%s\" expected \"%s\"", out, expect);
    n = GetFullPathNameA("C:\\a\\b", 3, out, NULL);
    CHECKF(n == 7, "GetFullPathNameA small buffer returns needed size", "n=%lu", (unsigned long)n);

    DWORD a = GetFileAttributesA("C:\\");
    CHECK(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY), "GetFileAttributesA C:\\ is a folder");
    CHECK(GetDriveTypeA("C:\\") == DRIVE_FIXED, "GetDriveTypeA C:\\ is fixed");
    ULARGE_INTEGER freeb, total, totfree;
    CHECK(GetDiskFreeSpaceExA("C:\\", &freeb, &total, &totfree) && total.QuadPart > 0, "GetDiskFreeSpaceExA");

    /* LSIW: other drive letters are folders under C:\ and only C: exists. */
    a = GetFileAttributesA("D:\\");
    CHECKF_LSIW(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY),
                "D:\\ folds onto a folder under C:", "attrs=%lx", (unsigned long)a);
    DWORD drives = GetLogicalDrives();
    CHECKF_LSIW(drives == (1u << 2), "GetLogicalDrives is only C:", "drives=%lx", (unsigned long)drives);

    char gcwd[MAX_PATH] = "";
    CHECK(_getcwd(gcwd, sizeof gcwd) != NULL, "_getcwd");
    CHECKF_LSIW(t_is_c_path(gcwd), "_getcwd starts with C:\\", "cwd=%s", gcwd);
    CHECK(_stricmp(gcwd, cwd) == 0, "_getcwd matches GetCurrentDirectoryA");

    CHECK(SetCurrentDirectoryA(g_dir), "SetCurrentDirectoryA");
    GetCurrentDirectoryA(sizeof out, out);
    CHECKF(_stricmp(out, g_dir) == 0, "GetCurrentDirectoryA after Set", "got %s", out);
    write_file("relative.txt", "rel");
    CHECK(GetFileAttributesA(in_dir("relative.txt")) != INVALID_FILE_ATTRIBUTES, "relative path uses the current folder");
    DeleteFileA("relative.txt");
    CHECK(SetCurrentDirectoryA(cwd), "SetCurrentDirectoryA back");
    SetLastError(0);
    CHECKF(!SetCurrentDirectoryA(in_dir("nope")) && GetLastError() == ERROR_FILE_NOT_FOUND,
           "SetCurrentDirectoryA missing -> ERROR_FILE_NOT_FOUND", "err=%lu", GetLastError());
}

static void test_case_and_slashes(void)
{
    write_file(in_dir("Foo.TXT"), "case");
    HANDLE h = CreateFileA(in_dir("foo.txt"), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "open Foo.TXT as foo.txt (case-insensitive)");
    char b[16] = "";
    DWORD n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, b, sizeof b - 1, &n, NULL);
        CloseHandle(h);
    }
    CHECK(n == 4 && memcmp(b, "case", 4) == 0, "case-insensitive open reads the file");
    WIN32_FIND_DATAA fd;
    h = FindFirstFileA(in_dir("FOO.txt"), &fd);
    CHECKF(h != INVALID_HANDLE_VALUE && strcmp(fd.cFileName, "Foo.TXT") == 0,
           "FindFirstFileA reports the stored case", "name=%s", h != INVALID_HANDLE_VALUE ? fd.cFileName : "-");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    /* Opening with a different case must not create a second file. */
    write_file(in_dir("FOO.txt"), "CASE2");
    int count = 0;
    h = FindFirstFileA(in_dir("f*"), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do count++; while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CHECKF(count == 1, "CREATE_ALWAYS with another case reuses the file", "count=%d", count);
    FILE *f = fopen(in_dir("FOO.TXT"), "r");
    CHECK(f && fgets(b, sizeof b, f) && strcmp(b, "CASE2") == 0, "fopen with another case");
    if (f) fclose(f);
    CHECK(DeleteFileA(in_dir("fOo.TxT")), "DeleteFileA with another case");

    char fwd[MAX_PATH];
    snprintf(fwd, sizeof fwd, "%s/fwd.txt", g_dir);
    for (char *p = fwd; *p; p++) if (*p == '\\') *p = '/';
    write_file(fwd, "slash");
    CHECK(GetFileAttributesA(in_dir("fwd.txt")) != INVALID_FILE_ATTRIBUTES, "forward-slash path creates the file");
    CHECK(DeleteFileA(fwd), "DeleteFileA forward-slash path");
}

static void test_crt_files(void)
{
    const char *path = in_dir("text.txt");
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "fopen w");
    if (!f) return;
    CHECK(fprintf(f, "line %s\n", "one") == 9, "fprintf");
    fputs("line two\n", f);
    fclose(f);

    struct _stat st;
    CHECKF(_stat(path, &st) == 0 && st.st_size == 20 && (st.st_mode & _S_IFREG),
           "text mode writes CRLF (size 20)", "size=%ld", (long)st.st_size);
    f = fopen(path, "rb");
    char raw[32] = "";
    size_t got = f ? fread(raw, 1, sizeof raw, f) : 0;
    if (f) fclose(f);
    CHECK(got == 20 && memcmp(raw, "line one\r\nline two\r\n", 20) == 0, "binary read sees CR LF");

    f = fopen(path, "r");
    char line[32];
    CHECK(f && fgets(line, sizeof line, f) && strcmp(line, "line one\n") == 0, "fgets text mode strips CR");
    CHECK(f && fgets(line, sizeof line, f) && strcmp(line, "line two\n") == 0, "fgets second line");
    CHECK(f && fgets(line, sizeof line, f) == NULL && feof(f), "fgets at EOF");
    if (f) fclose(f);

    f = fopen(path, "a+");
    if (f) {
        fputs("three", f);
        fseek(f, 0, SEEK_SET);
        CHECK(fgets(line, sizeof line, f) && strcmp(line, "line one\n") == 0, "a+ reads from start");
        fseek(f, 0, SEEK_END);
        CHECK(ftell(f) == 25, "ftell after append");
        fclose(f);
    }

    f = fopen(path, "wb");
    if (f) {
        int vals[3] = { 1, 2, 3 };
        CHECK(fwrite(vals, sizeof vals[0], 3, f) == 3, "fwrite binary");
        fclose(f);
        f = fopen(path, "rb");
        int rv[3] = { 0 };
        CHECK(f && fread(rv, sizeof rv[0], 3, f) == 3 && rv[2] == 3, "fread binary");
        if (f) fclose(f);
    }

    CHECK(_stat(g_dir, &st) == 0 && (st.st_mode & _S_IFDIR), "_stat folder");
    errno = 0;
    CHECK(_stat(in_dir("nope.txt"), &st) == -1 && errno == ENOENT, "_stat missing -> ENOENT");
    CHECK(_access(path, 0) == 0 && _access(path, 6) == 0, "_access existing file");
    errno = 0;
    CHECK(_access(in_dir("nope.txt"), 0) == -1 && errno == ENOENT, "_access missing -> ENOENT");
    CHECK(remove(path) == 0, "remove");
    CHECK(_mkdir(in_dir("crtdir")) == 0 && _rmdir(in_dir("crtdir")) == 0, "_mkdir/_rmdir");

    /* Low-level io. */
    int fd = _open(in_dir("io.bin"), _O_CREAT | _O_RDWR | _O_BINARY | _O_TRUNC, _S_IREAD | _S_IWRITE);
    CHECK(fd >= 3, "_open");
    if (fd >= 0) {
        CHECK(_write(fd, "12345", 5) == 5, "_write");
        CHECK(_lseek(fd, 1, SEEK_SET) == 1, "_lseek");
        char b[8] = "";
        CHECK(_read(fd, b, 8) == 4 && memcmp(b, "2345", 4) == 0, "_read");
        CHECK(_filelength(fd) == 5, "_filelength");
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        LARGE_INTEGER sz;
        CHECK(h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &sz) && sz.QuadPart == 5, "_get_osfhandle");
        _close(fd);
    }
    _unlink(in_dir("io.bin"));
}

static void cleanup(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(in_dir("*"), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
                printf("# leftover %s\n", fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryA(in_dir(fd.cFileName));
                else DeleteFileA(in_dir(fd.cFileName));
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

int main(void)
{
    t_init("t_files");
    test_temp_dir();
    test_handles();
    test_find();
    test_move_delete();
    test_paths();
    test_case_and_slashes();
    test_crt_files();
    DeleteFileA(in_dir("data.bin"));
    cleanup();
    CHECK(RemoveDirectoryA(g_dir), "remove the test folder");
    return t_done();
}
