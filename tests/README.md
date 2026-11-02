# LSIW test suite

Windows x86-64 programs that check LSIW's native engine. They are cross-built
on Linux, run through `lsiw run --engine=native`, and check themselves: each
prints one line per check (`ok - name`, `FAIL - name: details`, or
`skip - name: why`) and ends with `RESULT: PASS` or `RESULT: FAIL (<n> failures)`.

```sh
make -C tests                 # build tests/bin/*.exe (skips what the tools cannot build)
bash tests/run-tests.sh       # run them all with LSIW; exit status = number of failures
bash tests/run-tests.sh t_crt t_files      # selected tests
bash tests/run-tests.sh --wine             # the same binaries under plain Wine
make check                    # from the top level: build lsiw and the tests, run them
bash tests/lwlnk.sh           # LSIW shortcuts (.lwlnk) and icons; needs no Windows tools
```

`run-tests.sh` finds LSIW in `$LSIW`, then `./lsiw` or `build/lsiw` at the top
of the repository, then `lsiw` in `PATH`. `LSIW_CMD` replaces the whole
command (for a development runner that takes `program.exe args`), `WINE`
picks the Wine binary, `TEST_TIMEOUT` the per-test timeout (60 s). Output of
each test is kept in `tests/out/NAME.txt` (`NAME.wine.txt` for `--wine`). A
test passes when it prints `RESULT: PASS` and exits with status 0. The table
marks a test `UNIMPL` when it stopped at a function the native engine does
not implement yet (the name is shown), `TIMEOUT` when it was killed.

## Tools

| Tool | Fedora | Debian/Ubuntu | Needed for |
|------|--------|---------------|------------|
| `x86_64-w64-mingw32-gcc`, `-g++` | `mingw64-gcc mingw64-gcc-c++` | `gcc-mingw-w64 g++-mingw-w64` | every C/C++ test |
| `clang` | `clang` | `clang` | `t_seh_try.exe` (`__try`) |
| `clang-cl`, `lld-link`, `llvm-dlltool` | `clang lld llvm` | `clang lld llvm` | `msvc_eh.exe` |

The Makefile looks for versioned names too (`clang-cl-18`, ...). A missing
tool skips the tests that need it with a message. C++ tests are linked with
`-static-libgcc -static-libstdc++ -static`, so every test imports only
system DLLs.

## The tests

| Test | What it checks |
|------|----------------|
| `hello` | the smallest program (prints a greeting) |
| `t_crt` | msvcrt.dll: printf family (`%I64d %llu %Iu %ls %S %hs %5.2f %e %g %x %p`, MS three-digit exponents), `_snprintf` truncation, `sscanf`/`swscanf`, `strto*`, `atoi`, `qsort`/`bsearch`, `string.h`, wide strings, `swprintf`, UTF-8 conversion, `_itoa` family, `time`/`mktime`/`localtime`/`strftime`, `setlocale`, math, heap incl. `_aligned_malloc`, `getenv`/`_putenv`, `errno` values, the Microsoft `rand()` sequence, `atexit` order (checked in the last atexit handler, which prints the result line) |
| `t_crt_ucrt` | the same source against the Universal CRT (`api-ms-win-crt-*` → ucrtbase), as MSVC programs use it; adds `%zu`, expects two-digit exponents |
| `t_files` | `GetTempPath`, `CreateFileW`/`ReadFile`/`WriteFile`/`SetFilePointerEx`/`GetFileSizeEx`/`SetEndOfFile`, error codes of `CREATE_NEW`/`OPEN_EXISTING`, a non-ASCII file name, `FindFirstFile` enumeration (with `.` and `..`, wildcards), `MoveFileEx`, `CopyFile`, `DeleteFile`, read-only files, `RemoveDirectory`, `GetFullPathName`, `C:\` attributes, case-insensitive names, forward slashes, CRT text mode, `_stat`, `_access`, `_open` family; LSIW-only: `D:\` folds onto C:, `GetLogicalDrives() == C:` only, `_getcwd` on C: |
| `t_env` | command line and argv (run-tests.sh passes `alpha "two words" 'with"quote' 'back\slash' 'trail\' ''`), folder variables (`APPDATA`, `USERPROFILE`, `TEMP`, `SystemRoot`, `ProgramFiles`, `windir`, ...) on C:, `ExpandEnvironmentStrings`, `SetEnvironmentVariable`, `GetModuleFileName`, `GetProcAddress`/`LoadLibrary` errors, computer name, version (`RtlGetVersion` ≥ 10), `GetSystemInfo`, clocks, then `GetUserNameA` (advapi32), `CommandLineToArgvW` and `SHGetFolderPathA` (shell32) last |
| `t_threads` | `CreateThread`/exit codes/`CREATE_SUSPENDED`, `_beginthreadex`, 8 threads with `InterlockedIncrement` and a critical section, SRW lock + condition variable queue, events, named objects (`ERROR_ALREADY_EXISTS`), semaphores, mutex recursion/ownership/abandonment, `WaitForMultipleObjects` all/any, `TlsAlloc`, GCC `__thread`, the PE static `.tls` section and TLS callbacks, FLS, `InitOnceExecuteOnce`, `Sleep` timing, fibers, APCs, thread pool work |
| `t_seh` | vectored handlers: continuable `RaiseException`, `ud2`, divide by zero (handler leaves with `__builtin_longjmp`), msvcrt `longjmp`, `RtlLookupFunctionEntry`/`RtlCaptureContext`/`RtlCaptureStackBackTrace`, an access violation in a child run (`t_seh.exe child-av`) reaching `SetUnhandledExceptionFilter` with exit code `0xC0000005`, `int3` last |
| `t_seh_try` | (clang) `__try`/`__except` on a NULL write and division by zero, `GetExceptionCode`/`GetExceptionInformation`, `EXCEPTION_CONTINUE_SEARCH` to an outer handler, `__finally` during unwind and on normal exit, `__leave`, `EXCEPTION_CONTINUE_EXECUTION`, 100 faults in a row |
| `t_cpp` | g++ exceptions on top of LSIW's unwinder (rethrow, nested, `exception_ptr`, destructor order over 5 frames, `bad_alloc`), containers, iostreams, `std::thread`/`mutex`/`condition_variable`/`atomic`, `steady_clock`, RTTI and `dynamic_cast`, `std::filesystem` |
| `t_process` | `CreateProcessA` of itself in child mode with stdout in a pipe (`STARTF_USESTDHANDLES`), exit code 42, arguments, environment and current-folder inheritance, `TerminateProcess`, `system()`, `_popen`/`_pclose` |
| `t_registry` | `HKCU\Software\LSIWTest`: `REG_SZ`/`DWORD`/`BINARY`/`MULTI_SZ`/`QWORD`, sizes and `ERROR_MORE_DATA`, `RegGetValueA` with `RRF_RT_*`, `RegEnumValue`/`RegEnumKeyEx`, `RegQueryInfoKey`, `RegDeleteTree`, deleted key → `ERROR_FILE_NOT_FOUND`, `HKLM\...\CurrentVersion` `ProductName` |
| `t_net` | ws2_32: `WSAStartup`, TCP echo with a client thread over `127.0.0.1:0`, `select` timeouts, `WSAECONNREFUSED`, non-blocking connect, `getaddrinfo("localhost")`, `inet_pton`/`inet_ntop`, UDP `sendto`/`recvfrom`, `SO_RCVTIMEO`, `WSACleanup` |
| `msvc_eh` | (clang-cl, MSVC ABI, no SDK) LSIW's vcruntime140: `_CxxThrowException`, `__CxxFrameHandler3` (base-class catch, `catch(...)`, rethrow, throw from a catch block, nested try, destructors while unwinding, catch by value, pointers), `__RTtypeid`, `__std_type_info_name`, `__RTDynamicCast` (pointer, reference, cross cast, `void *`), `bad_cast`/`bad_typeid` |

Tests that need a DLL LSIW does not implement yet stop with LSIW's message
at the first missing function; the checks before it are still reported.

### LSIW-specific checks under Wine

A few checks describe LSIW's own design (the whole Linux filesystem is C:,
there is no D:). Wine maps drives differently, so these are printed as
`skip - ...` under Wine (detected by `wine_get_version` in ntdll) instead of
failing. Everything else expects real Windows behaviour.

### msvc/

`msvc_eh.cpp` declares what it needs itself: a `type_info` with the vcruntime
layout, `operator new`/`delete` over `malloc`/`free`, a small printf over
`WriteFile`, and `mainCRTStartup`, which runs the static constructors
(`.CRT$XCA`..`.CRT$XCZ`) and calls `ExitProcess`. Import libraries are made
from `vcruntime140.def`, `kernel32.def` and `api-ms-win-crt-heap-l1-1-0.def`
with `llvm-dlltool`. The vftable of `type_info` is referenced by absolute
address from every RTTI type descriptor, so it cannot be imported; the
program links a local stand-in (`/alternatename`), as the MSVC CRT links a
static copy. The executable imports `vcruntime140.dll` (`_CxxThrowException`,
`__CxxFrameHandler3`, `__RTDynamicCast`, `__RTCastToVoid`, `__RTtypeid`,
`__std_type_info_name`, `__std_type_info_compare`, `__type_info_root_node`),
`kernel32.dll` and `api-ms-win-crt-heap-l1-1-0.dll`
(check with `llvm-objdump -p tests/bin/msvc_eh.exe`).

## Validation on Wine

The tests were validated against Wine 9.0 (64-bit, Ubuntu build, 2026-10):
`bash tests/run-tests.sh --wine` passes all 13 programs. Expectations
adjusted to documented Windows behaviour while doing so:

- `RegQueryInfoKeyA` reports the stored (UTF-16) size of the longest string
  value, so the check only requires at least the ANSI size.
- `GetVersionExA` is allowed to answer 6.2: Windows reports that to programs
  without a compatibility manifest, and MinGW links none.
- Wine needs a UTF-8 locale to create non-ASCII file names; run-tests.sh sets
  `LC_ALL=C.UTF-8` for `--wine` when the locale is not UTF-8.
- `RaiseException` with `EXCEPTION_NONCONTINUABLE` followed by a filter
  returning `EXCEPTION_CONTINUE_EXECUTION` is not tested: the nested
  `EXCEPTION_NONCONTINUABLE_EXCEPTION` did not reach the outer `__except`
  under Wine either, so the expectation could not be validated.
- `t_seh_try` needs `-fasync-exceptions`: without it clang does not cover
  faulting instructions (only calls) with the `__try` scope table.

The tests run as an ordinary user; run as root, `t_files`' read-only checks
depend on LSIW not using `access()` (root may always write).
