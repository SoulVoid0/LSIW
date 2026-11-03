/* Extra drives: a Linux folder configured as D: ([drives] d = FOLDER).
 *
 * Run by tests/drives.sh with D: set up (and a file D:\marker.txt that
 * holds "lsiw-drive"); without a D: drive every check is skipped, so
 * tests/run-tests.sh passes on any configuration.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "lsiw_test.h"

int main(void)
{
    t_init("t_drives");
    DWORD drives = GetLogicalDrives();
    if (!(drives & (1u << 3))) {
        SKIP("extra drive D:", "no D: drive is configured");
        return t_done();
    }
    CHECKF(drives & (1u << 2), "GetLogicalDrives has C:", "drives=%lx", (unsigned long)drives);
    CHECK(GetDriveTypeA("D:\\") == DRIVE_FIXED || GetDriveTypeA("D:\\") == DRIVE_REMOVABLE, "GetDriveTypeA D:\\");
    CHECK(GetDriveTypeA("D:") == GetDriveTypeA("D:\\"), "GetDriveTypeA D: without backslash");
    if (!(drives & (1u << 16)))
        CHECK(GetDriveTypeA("Q:\\") == DRIVE_NO_ROOT_DIR, "GetDriveTypeA of a letter that is no drive");

    char buf[512];
    DWORD n = GetLogicalDriveStringsA(sizeof buf, buf);
    int has_c = 0, has_d = 0;
    for (char *p = buf; n && *p; p += strlen(p) + 1) {
        if (!strcmp(p, "C:\\")) has_c = 1;
        if (!strcmp(p, "D:\\")) has_d = 1;
    }
    CHECK(has_c && has_d, "GetLogicalDriveStringsA lists C:\\ and D:\\");

    /* A file in the folder. */
    HANDLE h = CreateFileA("D:\\marker.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "open D:\\marker.txt");
    if (h != INVALID_HANDLE_VALUE) {
        char data[32] = "";
        DWORD got = 0;
        ReadFile(h, data, sizeof data - 1, &got, NULL);
        CloseHandle(h);
        CHECKF(got >= 10 && !strncmp(data, "lsiw-drive", 10), "D:\\marker.txt has the folder's content", "got '%s'", data);
    }
    h = CreateFileA("D:\\sub\\made.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && CreateDirectoryA("D:\\sub", NULL))
        h = CreateFileA("D:\\sub\\made.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "create D:\\sub\\made.txt");
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, "x", 1, &w, NULL);
        CloseHandle(h);
    }

    /* Paths keep the letter. */
    char full[MAX_PATH];
    GetFullPathNameA("D:\\a\\..\\b.txt", sizeof full, full, NULL);
    CHECKF(!_stricmp(full, "D:\\b.txt"), "GetFullPathNameA D:\\a\\..\\b.txt", "got %s", full);
    CHECK(SetCurrentDirectoryA("D:\\sub"), "SetCurrentDirectoryA D:\\sub");
    char cwd[MAX_PATH] = "";
    GetCurrentDirectoryA(sizeof cwd, cwd);
    CHECKF(!_stricmp(cwd, "D:\\sub"), "GetCurrentDirectoryA is D:\\sub", "got %s", cwd);
    GetFullPathNameA("made.txt", sizeof full, full, NULL);
    CHECKF(!_stricmp(full, "D:\\sub\\made.txt"), "relative path on D:", "got %s", full);
    GetFullPathNameA("\\top.txt", sizeof full, full, NULL);
    CHECKF(!_stricmp(full, "D:\\top.txt"), "rooted path on the current drive", "got %s", full);
    CHECK(GetFileAttributesA("made.txt") != INVALID_FILE_ATTRIBUTES, "relative file on D: exists");
    CHECK(SetCurrentDirectoryA("C:\\"), "back to C:\\");

    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("D:\\*.txt", &fd);
    int found = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do if (!_stricmp(fd.cFileName, "marker.txt")) found = 1; while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    CHECK(found, "FindFirstFileA D:\\*.txt finds marker.txt");

    char label[MAX_PATH] = "", fs[32] = "";
    DWORD serial = 0, cserial = 0;
    CHECK(GetVolumeInformationA("D:\\", label, sizeof label, &serial, NULL, NULL, fs, sizeof fs), "GetVolumeInformationA D:\\");
    GetVolumeInformationA("C:\\", NULL, 0, &cserial, NULL, NULL, NULL, 0);
    CHECKF(label[0] != 0, "D: has a label", "label '%s'", label);
    CHECK(serial != cserial, "D: and C: have different serial numbers");
    char vol[8] = "";
    CHECK(GetVolumePathNameA("D:\\sub\\made.txt", vol, sizeof vol) && !strcmp(vol, "D:\\"), "GetVolumePathNameA on D:");
    ULARGE_INTEGER a, t, fr;
    CHECK(GetDiskFreeSpaceExA("D:\\", &a, &t, &fr) && t.QuadPart > 0, "GetDiskFreeSpaceExA D:\\");
    return t_done();
}
