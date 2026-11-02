# Contributing to LSIW

Thank you for helping. LSIW is licensed under the GNU AGPL v3 or later; by sending a contribution you
agree that it is published under the same license.

## Original code only

LSIW's native engine is an independent implementation of Windows
behaviour. To keep it that way:

* **Do not copy code** from Wine, ReactOS, or any other LGPL/GPL project,
  and never use leaked or disassembled Microsoft code. Do not translate such
  code line by line either.
* **Do use** public documentation: Microsoft's API reference
  (learn.microsoft.com), the PE/COFF specification, the x64 calling
  convention and exception-handling documents, published file formats, and
  behaviour you observe on a real Windows system (write a small test program
  and record what Windows does).
* Say where the behaviour comes from in a comment when it is not obvious
  ("Windows returns ERROR_INVALID_PARAMETER here even though the
  documentation says ..., checked on Windows 11 23H2").

## Building and testing

See [docs/BUILDING.md](docs/BUILDING.md). Before sending a change:

```sh
make                      # must build with zero warnings
make test-binaries check  # needs mingw64-gcc
```

A single file can be checked quickly with:

```sh
gcc -std=gnu11 -fsyntax-only -Wall -Wextra -D_GNU_SOURCE= -Iinclude -Isrc/native src/native/k32_file.c
```

## Code style

* C11 with GNU extensions (`-std=gnu11`), 4-space indentation, no tabs.
* Every file starts with a comment that says what it contains, followed by
  `SPDX-License-Identifier: AGPL-3.0-or-later`. Engine files then have
  `#define _GNU_SOURCE` and `#include "native.h"`.
* Comments explain the Windows behaviour being reproduced and why the code
  does what it does, not what the next line says.
* Warnings are errors in review: the tree builds with `-Wall -Wextra`
  without any.

## Writing Win32 functions

* A Windows-callable function is declared `API <type> Name(...)`
  (`static`, Microsoft x64 calling convention) and listed in its file's
  export table:

  ```c
  API DWORD GetTickCount(void) { ... }

  const struct lsiw_export lsiw_k32proc_exports[] = {
      E(GetTickCount),
      EX("GetTickCount64", GetTickCount64),
      { NULL, NULL }
  };
  ```

* Use the Windows types from `native.h` and define any other Windows
  structure locally with its exact x64 layout. Wide strings are UTF-16
  (`WCHAR`, 16 bits); never use `wchar_t`, which is 32 bits on Linux.
* Convert strings with `lsiw_w2u` / `lsiw_u2w` and paths with
  `lsiw_path_a` / `lsiw_path_w`, which apply LSIW's C: drive rules.
* Set the last error exactly as Windows does (`lsiw_set_last_error`,
  `lsiw_set_errno_error`) and return what Windows returns on failure.
* Memory a program frees with `HeapFree`, `LocalFree`, `free` or
  `CoTaskMemFree` must come from `lsiw_heap_alloc`.
* **Export only what you really implement.** The automatic engine choice
  sends a program to Wine when it imports a function LSIW does not export.
  A stub that pretends to succeed hides that and breaks programs in ways
  that are hard to trace. When Linux cannot provide a behaviour, fail the
  way Windows fails in that situation (for example `ERROR_NOT_SUPPORTED` or
  `ERROR_CALL_NOT_IMPLEMENTED`) and say so in a comment.

## Adding a DLL

1. Create `src/native/<name>.c` with its export table
   `lsiw_<name>_exports[]`.
2. Declare the table in `src/native/native.h`.
3. Map the DLL name (and any API-set names that forward to it) to the table
   in `src/native/builtins.c`.
4. Add a test program under `tests/`.
5. Update [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)
   (`make export-stats` prints the counts).

## Reporting a program that does not work

Please include:

* the output of `lsiw check program.exe`;
* the output of `LSIW_VERBOSE=1 lsiw run program.exe` (remove anything
  private);
* your distribution and version, and `lsiw info`.

Kernel-mode drivers cannot run on Linux at all (see the README); reports
about them will be closed with an explanation.

## Pull requests

* One topic per pull request, with a description of the Windows behaviour
  it adds or fixes.
* Update `CHANGELOG.md` under "Unreleased".
* CI (`.github/workflows/build.yml`) must pass: it builds on Fedora, runs
  the tests and builds the RPMs.
