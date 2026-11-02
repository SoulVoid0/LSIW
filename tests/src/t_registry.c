/* t_registry: the registry through advapi32.
 *
 * Creates HKEY_CURRENT_USER\Software\LSIWTest, stores every common value
 * type, reads them back (sizes included), enumerates values and subkeys,
 * counts them with RegQueryInfoKey, deletes the tree and checks it is gone.
 * Also reads the Windows version under HKLM, which installers query.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"

#define TEST_KEY "Software\\LSIWTest"

static void test_values(HKEY k)
{
    const char *sz = "hello registry";
    LONG r = RegSetValueExA(k, "Str", 0, REG_SZ, (const BYTE *)sz, (DWORD)strlen(sz) + 1);
    CHECKF(r == ERROR_SUCCESS, "RegSetValueExA REG_SZ", "r=%ld", r);
    DWORD dw = 0x12345678;
    CHECK(RegSetValueExA(k, "Num", 0, REG_DWORD, (const BYTE *)&dw, sizeof dw) == ERROR_SUCCESS, "RegSetValueExA REG_DWORD");
    const BYTE bin[5] = { 1, 2, 0, 255, 7 };
    CHECK(RegSetValueExA(k, "Bin", 0, REG_BINARY, bin, sizeof bin) == ERROR_SUCCESS, "RegSetValueExA REG_BINARY");
    const char multi[] = "one\0two\0three\0";
    CHECK(RegSetValueExA(k, "Multi", 0, REG_MULTI_SZ, (const BYTE *)multi, sizeof multi) == ERROR_SUCCESS,
          "RegSetValueExA REG_MULTI_SZ");
    ULONGLONG q = 0x1122334455667788ULL;
    CHECK(RegSetValueExA(k, "Quad", 0, REG_QWORD, (const BYTE *)&q, sizeof q) == ERROR_SUCCESS, "RegSetValueExA REG_QWORD");
    const char *def = "default value";
    CHECK(RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *)def, (DWORD)strlen(def) + 1) == ERROR_SUCCESS,
          "RegSetValueExA default value");

    char buf[64];
    DWORD type = 0, size = sizeof buf;
    r = RegQueryValueExA(k, "Str", NULL, &type, (BYTE *)buf, &size);
    CHECKF(r == ERROR_SUCCESS && type == REG_SZ && size == strlen(sz) + 1 && strcmp(buf, sz) == 0,
           "RegQueryValueExA REG_SZ data, type and size", "r=%ld type=%lu size=%lu", r, type, size);
    size = 0;
    r = RegQueryValueExA(k, "Str", NULL, &type, NULL, &size);
    CHECKF(r == ERROR_SUCCESS && size == strlen(sz) + 1, "RegQueryValueExA size query", "size=%lu", size);
    size = 4;
    r = RegQueryValueExA(k, "Str", NULL, &type, (BYTE *)buf, &size);
    CHECKF(r == ERROR_MORE_DATA && size == strlen(sz) + 1, "RegQueryValueExA small buffer -> ERROR_MORE_DATA",
           "r=%ld size=%lu", r, size);
    DWORD got = 0;
    size = sizeof got;
    CHECK(RegQueryValueExA(k, "Num", NULL, &type, (BYTE *)&got, &size) == ERROR_SUCCESS && type == REG_DWORD &&
          got == dw && size == 4, "RegQueryValueExA REG_DWORD");
    BYTE bb[16];
    size = sizeof bb;
    CHECK(RegQueryValueExA(k, "Bin", NULL, &type, bb, &size) == ERROR_SUCCESS && type == REG_BINARY && size == 5 &&
          memcmp(bb, bin, 5) == 0, "RegQueryValueExA REG_BINARY");
    size = sizeof buf;
    CHECK(RegQueryValueExA(k, "Multi", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ &&
          size == sizeof multi && memcmp(buf, multi, sizeof multi) == 0, "RegQueryValueExA REG_MULTI_SZ");
    size = sizeof buf;
    CHECK(RegQueryValueExA(k, "", NULL, &type, (BYTE *)buf, &size) == ERROR_SUCCESS && strcmp(buf, def) == 0,
          "default value read back");
    size = sizeof buf;
    CHECK(RegQueryValueExA(k, "num", NULL, &type, (BYTE *)&got, &size) == ERROR_SUCCESS && got == dw,
          "value names are case-insensitive");
    size = sizeof buf;
    r = RegQueryValueExA(k, "Missing", NULL, &type, (BYTE *)buf, &size);
    CHECKF(r == ERROR_FILE_NOT_FOUND, "missing value -> ERROR_FILE_NOT_FOUND", "r=%ld", r);

    WCHAR w[64];
    size = sizeof w;
    CHECK(RegQueryValueExW(k, L"Str", NULL, &type, (BYTE *)w, &size) == ERROR_SUCCESS &&
          wcscmp(w, L"hello registry") == 0 && size == (wcslen(w) + 1) * 2, "RegQueryValueExW converts to UTF-16");

    got = 0;
    size = sizeof got;
    r = RegGetValueA(HKEY_CURRENT_USER, TEST_KEY, "Num", RRF_RT_REG_DWORD, &type, &got, &size);
    CHECKF(r == ERROR_SUCCESS && got == dw && type == REG_DWORD, "RegGetValueA RRF_RT_REG_DWORD", "r=%ld", r);
    size = sizeof buf;
    r = RegGetValueA(HKEY_CURRENT_USER, TEST_KEY, "Str", RRF_RT_REG_DWORD, &type, buf, &size);
    CHECKF(r == ERROR_UNSUPPORTED_TYPE, "RegGetValueA type mismatch -> ERROR_UNSUPPORTED_TYPE", "r=%ld", r);
    size = sizeof buf;
    r = RegGetValueA(k, NULL, "Str", RRF_RT_REG_SZ, NULL, buf, &size);
    CHECK(r == ERROR_SUCCESS && strcmp(buf, sz) == 0, "RegGetValueA RRF_RT_REG_SZ");

    CHECK(RegDeleteValueA(k, "Quad") == ERROR_SUCCESS, "RegDeleteValueA");
    CHECK(RegDeleteValueA(k, "Quad") == ERROR_FILE_NOT_FOUND, "RegDeleteValueA missing -> ERROR_FILE_NOT_FOUND");
}

static void test_enum(HKEY k)
{
    HKEY sub;
    const char *names[] = { "Alpha", "Beta", "Gamma" };
    for (int i = 0; i < 3; i++) {
        DWORD disp = 0;
        CHECK(RegCreateKeyExA(k, names[i], 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, &disp) == ERROR_SUCCESS &&
              disp == REG_CREATED_NEW_KEY, "RegCreateKeyExA subkey (new)");
        RegCloseKey(sub);
    }
    HKEY deep;
    CHECK(RegCreateKeyExA(k, "Alpha\\Deeper\\Deepest", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &deep, NULL) == ERROR_SUCCESS,
          "RegCreateKeyExA creates intermediate keys");
    RegSetValueExA(deep, "leaf", 0, REG_SZ, (const BYTE *)"x", 2);
    RegCloseKey(deep);

    int seen = 0;
    char name[64];
    for (DWORD i = 0;; i++) {
        DWORD n = sizeof name;
        LONG r = RegEnumKeyExA(k, i, name, &n, NULL, NULL, NULL, NULL);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || i > 10) { seen = -100; break; }
        for (int j = 0; j < 3; j++) if (!strcmp(name, names[j]) && n == strlen(name)) seen |= 1 << j;
    }
    CHECKF(seen == 7, "RegEnumKeyExA lists the 3 subkeys", "seen=%d", seen);

    int vals = 0, found_num = 0;
    for (DWORD i = 0;; i++) {
        DWORD n = sizeof name, type = 0;
        BYTE data[64];
        DWORD dsz = sizeof data;
        LONG r = RegEnumValueA(k, i, name, &n, NULL, &type, data, &dsz);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || i > 20) { vals = -100; break; }
        vals++;
        if (!strcmp(name, "Num") && type == REG_DWORD && dsz == 4 && *(DWORD *)data == 0x12345678) found_num = 1;
    }
    CHECKF(vals == 5 && found_num, "RegEnumValueA lists 5 values (default included) with data", "vals=%d", vals);

    DWORD nsub = 0, maxsub = 0, nval = 0, maxname = 0, maxdata = 0;
    /* maxdata: Windows reports the stored (UTF-16) size of the longest string. */
    LONG r = RegQueryInfoKeyA(k, NULL, NULL, NULL, &nsub, &maxsub, NULL, &nval, &maxname, &maxdata, NULL, NULL);
    CHECKF(r == ERROR_SUCCESS && nsub == 3 && nval == 5 && maxsub == 5 && maxname == 5 && maxdata >= 15,
           "RegQueryInfoKeyA counts and maximum lengths", "r=%ld sub=%lu val=%lu maxsub=%lu maxname=%lu maxdata=%lu",
           r, nsub, nval, maxsub, maxname, maxdata);

    DWORD disp = 0;
    CHECK(RegCreateKeyExA(k, "beta", 0, NULL, 0, KEY_READ, NULL, &sub, &disp) == ERROR_SUCCESS &&
          disp == REG_OPENED_EXISTING_KEY, "RegCreateKeyExA existing key (any case) -> REG_OPENED_EXISTING_KEY");
    RegCloseKey(sub);
    CHECK(RegDeleteKeyA(k, "Gamma") == ERROR_SUCCESS, "RegDeleteKeyA leaf");
    CHECK(RegOpenKeyExA(k, "Gamma", 0, KEY_READ, &sub) == ERROR_FILE_NOT_FOUND, "deleted subkey is gone");
}

int main(void)
{
    t_init("t_registry");
    HKEY k;
    RegDeleteTreeA(HKEY_CURRENT_USER, TEST_KEY);   /* leftovers of an interrupted run */
    DWORD disp = 0;
    LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &k, &disp);
    CHECKF(r == ERROR_SUCCESS && disp == REG_CREATED_NEW_KEY, "RegCreateKeyExA HKCU\\Software\\LSIWTest", "r=%ld", r);
    if (r != ERROR_SUCCESS) return t_done();
    test_values(k);
    test_enum(k);
    RegCloseKey(k);

    HKEY again;
    CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY "\\Alpha\\Deeper", 0, KEY_READ, &again) == ERROR_SUCCESS,
          "RegOpenKeyExA nested key");
    RegCloseKey(again);
    r = RegDeleteTreeA(HKEY_CURRENT_USER, TEST_KEY);
    CHECKF(r == ERROR_SUCCESS, "RegDeleteTreeA", "r=%ld", r);
    r = RegOpenKeyExA(HKEY_CURRENT_USER, TEST_KEY, 0, KEY_READ, &again);
    CHECKF(r == ERROR_FILE_NOT_FOUND, "opening the deleted key -> ERROR_FILE_NOT_FOUND", "r=%ld", r);

    HKEY cv;
    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &cv);
    CHECKF(r == ERROR_SUCCESS, "RegOpenKeyExA HKLM\\...\\CurrentVersion", "r=%ld", r);
    if (r == ERROR_SUCCESS) {
        char product[128] = "";
        DWORD size = sizeof product, type = 0;
        r = RegQueryValueExA(cv, "ProductName", NULL, &type, (BYTE *)product, &size);
        CHECKF(r == ERROR_SUCCESS && type == REG_SZ && strstr(product, "Windows") != NULL,
               "ProductName names Windows", "r=%ld product=%s", r, product);
        DWORD major = 0;
        size = sizeof major;
        r = RegQueryValueExA(cv, "CurrentMajorVersionNumber", NULL, &type, (BYTE *)&major, &size);
        CHECKF(r == ERROR_SUCCESS && major == 10, "CurrentMajorVersionNumber is 10", "r=%ld major=%lu", r, major);
        RegCloseKey(cv);
    }
    r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\LSIW\\Surely\\Missing", 0, KEY_READ, &cv);
    CHECKF(r == ERROR_FILE_NOT_FOUND, "missing HKLM key -> ERROR_FILE_NOT_FOUND", "r=%ld", r);
    return t_done();
}
