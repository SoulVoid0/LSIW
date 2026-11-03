/* A Windows program that waits: sleep.exe [SECONDS [EXIT_CODE]] (3 s,
 * exit 0 by default). Used by tests/playtime.sh.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    Sleep(argc > 1 ? (DWORD)atoi(argv[1]) * 1000 : 3000);
    return argc > 2 ? atoi(argv[2]) : 0;
}
