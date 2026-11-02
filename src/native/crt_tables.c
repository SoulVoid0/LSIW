/* The few C runtime names whose meaning depends on which DLL a program
 * imports them from. builtins.c searches these tables first:
 * lsiw_msvcrt_exports for msvcrt.dll, lsiw_ucrt_exports for ucrtbase.dll
 * and the api-ms-win-crt-* family. Everything else is shared.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "crt.h"

const struct lsiw_export lsiw_msvcrt_exports[] = {
    /* msvcrt.dll keeps the pre-standard forms */
    EX("swprintf", crt_swprintf_msvcrt),
    EX("vswprintf", crt_vswprintf_msvcrt),
    EX("wcstok", crt_wcstok_msvcrt),
    { NULL, NULL }
};

const struct lsiw_export lsiw_ucrt_exports[] = {
    /* ucrtbase.dll follows ISO C */
    EX("swprintf", crt_swprintf_iso),
    EX("vswprintf", crt_vswprintf_iso),
    EX("wcstok", crt_wcstok_ucrt),
    { NULL, NULL }
};
