/* Registry of the DLLs LSIW implements itself.
 *
 * Windows programs import from many DLL names that are really the same
 * implementation (kernel32 / kernelbase / api-ms-win-core-*, msvcrt /
 * ucrtbase / api-ms-win-crt-*). Each name maps to an ordered list of
 * export tables that are searched in turn.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

#define K32   lsiw_k32proc_exports, lsiw_k32file_exports, lsiw_k32mem_exports, lsiw_k32extra_exports, lsiw_k32sys_exports, lsiw_k32compat_exports, lsiw_k32more_exports
#define CRT   lsiw_crt_core_exports, lsiw_crt_stdio_exports, lsiw_crt_string_exports, lsiw_crt_math_exports, lsiw_crt_time_exports
#define MSVCRT { lsiw_msvcrt_exports, CRT, lsiw_ucrt_exports, lsiw_vcruntime_exports, lsiw_seh_exports }
#define VCRT  { lsiw_vcruntime_exports, lsiw_seh_exports, lsiw_crt_string_exports, lsiw_crt_core_exports, lsiw_ucrt_exports }
#define MISC  { lsiw_misc_exports }
#define OLE   { lsiw_ole_exports, lsiw_misc_exports }
#define WS2   { lsiw_ws2_exports }

static const struct lsiw_builtin_dll g_dlls[] = {
    { "kernel32.dll",     { K32, lsiw_seh_exports, lsiw_ntdll_exports, lsiw_ntobj_exports, lsiw_misc_exports } },
    { "kernelbase.dll",   { K32, lsiw_seh_exports, lsiw_ntdll_exports, lsiw_ntobj_exports, lsiw_advapi32_exports, lsiw_shell32_exports } },
    { "ntdll.dll",        { lsiw_ntfile_exports, lsiw_ntdll_exports, lsiw_ntobj_exports, lsiw_seh_exports, K32, lsiw_crt_string_exports, lsiw_crt_stdio_exports, lsiw_crt_math_exports } },
    { "msvcrt.dll",       MSVCRT },
    { "msvcr70.dll",      MSVCRT }, { "msvcr71.dll",  MSVCRT }, { "msvcr80.dll",  MSVCRT },
    { "msvcr90.dll",      MSVCRT }, { "msvcr100.dll", MSVCRT }, { "msvcr110.dll", MSVCRT },
    { "msvcr120.dll",     MSVCRT },
    { "ucrtbase.dll",     { lsiw_ucrt_exports, CRT, lsiw_msvcrt_exports, lsiw_vcruntime_exports, lsiw_seh_exports } },
    { "vcruntime140.dll", VCRT },
    { "vcruntime140_1.dll", VCRT },
    { "user32.dll",       { lsiw_user32_exports } },
    { "shell32.dll",      { lsiw_shell32_exports } },
    { "shlwapi.dll",      { lsiw_shell32_exports, lsiw_misc_exports } },
    { "advapi32.dll",     { lsiw_advapi32_exports, lsiw_k32more_exports } },
    { "sechost.dll",      { lsiw_advapi32_exports } },
    { "ole32.dll",        OLE }, { "oleaut32.dll", OLE }, { "combase.dll", OLE },
    { "rpcrt4.dll",       OLE }, { "crypt32.dll",  OLE }, { "bcrypt.dll",  OLE },
    { "bcryptprimitives.dll", OLE },
    { "ws2_32.dll",       WS2 }, { "wsock32.dll", { lsiw_wsock32_exports, lsiw_ws2_exports } }, { "mswsock.dll", WS2 }, { "iphlpapi.dll", { lsiw_ws2_exports, lsiw_k32more_exports } },
    { "winmm.dll",        MISC }, { "version.dll",  MISC }, { "userenv.dll", MISC },
    { "secur32.dll",      MISC }, { "sspicli.dll",  MISC }, { "dbghelp.dll", MISC },
    { "dbgcore.dll",      MISC }, { "powrprof.dll", MISC }, { "netapi32.dll", MISC },
    { "mpr.dll",          MISC }, { "wtsapi32.dll", MISC }, { "dwmapi.dll",  MISC },
    { "shcore.dll",       MISC }, { "dnsapi.dll",   MISC },
    { "lz32.dll",         { lsiw_k32compat_exports } },
    { "psapi.dll",        { lsiw_misc_exports, lsiw_k32proc_exports, lsiw_k32mem_exports, lsiw_k32extra_exports } },
};

/* Generic families: api-ms-win-core-* -> kernel32, api-ms-win-crt-* -> ucrtbase. */
static const struct lsiw_builtin_dll g_core_family = { "api-ms-win-core", { K32, lsiw_seh_exports, lsiw_ntdll_exports, lsiw_ntobj_exports, lsiw_advapi32_exports, lsiw_shell32_exports, lsiw_ole_exports, lsiw_misc_exports } };
static const struct lsiw_builtin_dll g_crt_family  = { "api-ms-win-crt",  { lsiw_ucrt_exports, CRT, lsiw_msvcrt_exports, lsiw_vcruntime_exports, lsiw_seh_exports } };
static const struct lsiw_builtin_dll g_misc_family = { "api-ms-win",      { K32, lsiw_advapi32_exports, lsiw_misc_exports, lsiw_ole_exports, lsiw_shell32_exports, lsiw_user32_exports } };

const struct lsiw_builtin_dll *lsiw_builtin_find(const char *dllname)
{
    for (size_t i = 0; i < sizeof g_dlls / sizeof g_dlls[0]; i++)
        if (!strcasecmp(g_dlls[i].name, dllname)) return &g_dlls[i];
    if (!strncasecmp(dllname, "api-ms-win-crt-", 15)) return &g_crt_family;
    if (!strncasecmp(dllname, "api-ms-win-core-", 16) ||
        !strncasecmp(dllname, "api-ms-win-security-", 20) ||
        !strncasecmp(dllname, "api-ms-win-eventing-", 20)) return &g_core_family;
    if (!strncasecmp(dllname, "api-ms-win-", 11) || !strncasecmp(dllname, "ext-ms-win-", 11))
        return &g_misc_family;
    return NULL;
}

void *lsiw_builtin_lookup(const struct lsiw_builtin_dll *dll, const char *sym)
{
    if (!dll || !sym) return NULL;
    for (int t = 0; t < LSIW_MAX_TABLES && dll->tables[t]; t++)
        for (const struct lsiw_export *e = dll->tables[t]; e->name; e++)
            if (!strcmp(e->name, sym)) return e->addr;
    return NULL;
}

/* Each builtin DLL gets a distinct, readable HMODULE that starts with a
 * minimal PE header, so programs that peek at it do not crash. */
static struct { const struct lsiw_builtin_dll *dll; void *page; } g_handles[64];

HMODULE lsiw_builtin_handle(const struct lsiw_builtin_dll *dll)
{
    int i;
    for (i = 0; i < 64 && g_handles[i].dll; i++)
        if (g_handles[i].dll == dll) return g_handles[i].page;
    if (i == 64) return NULL;
    uint8_t *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    p[0] = 'M'; p[1] = 'Z';
    *(int32_t *)(p + 0x3c) = 0x40;
    memcpy(p + 0x40, "PE\0\0", 4);
    *(uint16_t *)(p + 0x44) = 0x8664;          /* Machine */
    *(uint16_t *)(p + 0x54) = 0xf0;            /* SizeOfOptionalHeader */
    *(uint16_t *)(p + 0x56) = 0x2022;          /* DLL | EXECUTABLE | LARGE_ADDRESS_AWARE */
    *(uint16_t *)(p + 0x58) = 0x20b;           /* PE32+ */
    *(uint64_t *)(p + 0x58 + 24) = (uint64_t)(uintptr_t)p;  /* ImageBase */
    *(uint32_t *)(p + 0x58 + 56) = 0x1000;     /* SizeOfImage */
    mprotect(p, 0x1000, PROT_READ);
    g_handles[i].dll = dll;
    g_handles[i].page = p;
    return p;
}

const struct lsiw_builtin_dll *lsiw_builtin_from_handle(HMODULE h)
{
    for (int i = 0; i < 64 && g_handles[i].dll; i++)
        if (g_handles[i].page == h) return g_handles[i].dll;
    return NULL;
}
