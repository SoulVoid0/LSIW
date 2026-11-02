# Compatibility

What LSIW's native engine implements, DLL by DLL, and what it does not.
Programs that need something missing run on the Wine fallback instead
(see the README's "How the engine is chosen").

## Checking a program

```sh
lsiw check program.exe
```

lists every function the program (and every DLL it loads) imports that LSIW
does not implement, without running it. LSIW only exports functions it
really implements, or that fail exactly the way Windows fails when the
feature is unavailable; a missing function is never replaced by a stub that
pretends to work. If a program run with `--engine=native` calls a missing
function anyway, it stops with
`lsiw: the program called DLL!Function, which the LSIW native engine does
not implement` (exit status 127).

## Kinds of programs

| Program                                                    | LSIW engine | Notes |
|------------------------------------------------------------|-------------|-------|
| 64-bit console program, MinGW-w64 (C, C++)                 | yes         | msvcrt or ucrt runtime, libstdc++/libgcc DLLs next to the program or linked in |
| 64-bit console program, MSVC (C, C++)                      | yes         | ucrtbase / `api-ms-win-crt-*`, vcruntime140, C++ exceptions and RTTI |
| 64-bit program using only implemented DLLs (see below)     | yes         | check with `lsiw check` |
| 64-bit windowed (GUI) program                              | Wine        | window management, GDI drawing and input are not implemented; gdi32 is not provided |
| 32-bit program                                             | Wine        | the native engine runs x86-64 code only |
| `.msi` installer, `.bat` / `.cmd` script                   | Wine        | |
| .NET program                                               | Wine        | Wine Mono, installed in LSIW's Wine prefix; not the native engine |
| DirectX / Vulkan / OpenGL game                             | no          | no graphics or audio stack; use Wine or Proton |
| 16-bit Windows or MS-DOS program                           | no          | use DOSBox |
| ARM64 Windows program                                      | no          | |
| Kernel-mode driver (`.sys`)                                | never       | needs the Windows kernel itself; see the README |

## Installers on the Wine engine

LSIW's Wine prefix (`lsiw wine setup`, or Set up Wine prefix in LSIW
Control) has Wine's Windows services (RpcSs, MSIServer, Eventlog,
Winmgmt, Schedule, ...), Wine Mono for .NET, and Wine's PowerShell stub
turned off (it runs nothing and reports success, which misleads
installers). Results for the installers Ninite offers, checked with Wine
11.19:

| Installer                         | Result | Why |
|-----------------------------------|--------|-----|
| Visual C++ 2008, 2010             | cause fixed in 1.5.0 | needed the Windows services (installer not retested) |
| VSTO runtime, GitHub Desktop, Discord | cause fixed in 1.5.0 and 1.5.1 | need .NET (Wine Mono) and the services. Squirrel installers like Discord's (Atom, Insomnia tested) install; without .NET they quit with exit code 8 |
| Bitwarden                         | works since 1.5.0 | exit 2 came from the PowerShell stub (retested) |
| Node.js (MSI)                     | fails, error 1603 | Wine cannot save the installer's web shortcut (`IPersistFile::Save` on an Internet shortcut returns STG_E_ACCESSDENIED) and the install rolls back; a Wine bug |
| PowerToys                         | fails, error 1603 | needs Windows.Data.Json (missing in Wine) and MSIX packages |
| Windows App, other MSIX/Store apps | no    | Wine has no MSIX/AppX deployment |
| Docker Desktop                    | no     | needs WSL 2 or Hyper-V |
| .NET Framework 4.8.1              | no     | Microsoft's installer does not run on Wine; Wine Mono provides .NET 4.x instead |

## The Windows LSIW presents

* **Version:** Windows 10, version 10.0, build 19045 (`GetVersionEx`,
  `RtlGetVersion`), 64-bit (`IsWow64Process` reports no WOW64).
* **Code pages:** the ANSI and OEM code page is UTF-8 (65001); `A` and `W`
  functions agree, and file names round-trip with Linux.
* **File system:** one drive, `C:`, which is the Linux root; other drive
  letters are folded onto C:, and `GetLogicalDrives` reports C: only.
  Names are matched case-insensitively. Volume functions describe C: as
  an NTFS volume labelled "LSIW".
* **Environment:** `PATH` in Windows form with `C:\windows\system32` first,
  `USERPROFILE`, `APPDATA`, `LOCALAPPDATA`, `TEMP`, `ProgramFiles`,
  `windir` and the other folder variables from LSIW's configuration,
  `OS=Windows_NT`, `PROCESSOR_ARCHITECTURE=AMD64`.
* **Command line:** arguments that are existing absolute Linux paths are
  passed as `C:\` paths (`[runtime] translate_args`).
* **Console:** standard handles are the Linux terminal or pipes; the
  console code page is UTF-8 (65001).
* **IDs:** process and thread IDs are the Linux process and thread IDs.

## DLLs implemented by LSIW

Counts are entries in LSIW's export tables in version 1.0.0 (4439 in all)
(`make export-stats` prints the current numbers). One table can serve
several DLL names, and some functions are exported under more than one
name; the lists of functions are the export tables at the end of each
source file.

### kernel32.dll, kernelbase.dll, `api-ms-win-core-*`

| Table (source file)                     | Entries | Contents |
|-----------------------------------------|--------:|----------|
| `lsiw_k32proc_exports` (k32_process.c)  | 317 | processes (`CreateProcess`, `OpenProcess`, exit codes, priorities, affinity), threads, TLS and FLS, critical sections, SRW locks, condition variables, `InitOnce`, `WaitOnAddress`, events, mutexes, semaphores, waitable timers, `WaitForMultipleObjects`, anonymous pipes (`CreatePipe`), environment,
command line, modules (`LoadLibrary*`, `GetProcAddress`, `GetModuleFileName*`, DLL directories), resources (`FindResource*`, `LoadResource`), system information, memory status, time and time zones, code pages and locale (`MultiByteToWideChar`, `CompareString*`, `LCMapString*`, `GetLocaleInfo*`, date/time formatting), `FormatMessage`, version checks, `lstr*` |
| `lsiw_k32file_exports` (k32_file.c)     | 168 | `CreateFile*`, `ReadFile`/`WriteFile` (including `OVERLAPPED` offsets and `GetOverlappedResult`), file information and attributes, times, locking, directories, `FindFirstFile*`, copy/move/delete, hard and symbolic links, change notifications, full/long/short/temporary path names, `SearchPath`, drives and volumes, disk space, file mappings (`CreateFileMapping`, `MapViewOfFile`), the console (modes, screen buffer, cursor, colours, titles, input events, control handlers) |
| `lsiw_k32mem_exports` (k32_mem.c)       | 106 | heaps (`HeapAlloc` & co., `GetProcessHeap`), `Global*`/`Local*` memory, `VirtualAlloc`/`VirtualProtect`/`VirtualQuery` and their `Ex` forms, `ReadProcessMemory` (also from child processes), `WriteProcessMemory` (own process), interlocked singly-linked lists, fibers, `IsBad*Ptr`, working set and process memory information |
| `lsiw_k32extra_exports` (k32_extra.c)   | 331 | named pipes (`CreateNamedPipe`, `ConnectNamedPipe`, `CallNamedPipe`, `PeekNamedPipe`), I/O completion ports, thread pools, `Transacted` file functions, job objects, more console and locale functions |
| `lsiw_k32sys_exports` (k32_sys.c)       |  51 | system and firmware information, power and performance counters |
| `lsiw_k32compat_exports` (k32_compat.c) |  40 | 16-bit-era functions (`_lopen`, `OpenFile`, `lz32`) |
| `lsiw_k32more_exports` (ntdll_obj.c)    |  62 | kernel32 functions built on the ntdll object layer |
| `lsiw_seh_exports` (seh.c)              |  26 | `RaiseException`, vectored exception and continue handlers, `SetUnhandledExceptionFilter`, `RtlCaptureContext`, `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwind(Ex)`, `RtlRestoreContext`, dynamic function tables, `CaptureStackBackTrace`, `__C_specific_handler`, `/GS` handlers |

kernel32 also serves the functions in ntdll's table. kernelbase also
reaches the advapi32 and shell32 tables, and the `api-ms-win-core-*` sets
the advapi32, shell32 and ole32 tables, as the real API sets do.

### ntdll.dll

| Table (source file)                     | Entries | Contents |
|-----------------------------------------|--------:|----------|
| `lsiw_ntfile_exports` (ntdll_file.c)    |  54 | `NtCreateFile`/`NtOpenFile`, `NtReadFile`/`NtWriteFile`, `NtQueryInformationFile`/`NtSetInformationFile` (all common classes, pipe information), `NtQueryDirectoryFile`, `NtQueryVolumeInformationFile`, `NtDeviceIoControlFile`, `NtFsControlFile`, `NtLockFile`, `NtCancelIoFile(Ex)`; `DeviceIoControl`, `LockFileEx`, `CancelIo(Ex)` go through them |
| `lsiw_ntobj_exports` (ntdll_obj.c)      | 136 | registry keys (`NtCreateKey`, `NtQueryValueKey`, ...), sections, events, mutants, semaphores, timers, keyed events, atoms, object names and directories, `RtlVerifyVersionInfo` and other `Rtl` helpers |
| `lsiw_ntdll_exports` (ntdll.c)          |  83 | `Nt*` memory, file information, process/thread information, waits, `NtQuerySystemInformation`; `Ldr*` loader calls; `Rtl` strings (`UNICODE_STRING`/`ANSI_STRING`), bitmaps, CRC32, random numbers, version, status-to-error conversion, PEB lock |

ntdll also exports the heap, SList, SEH and memory functions above, and
the C runtime string, stdio and math functions that the real ntdll
exports.

### The C runtime: msvcrt.dll, msvcr70–msvcr120.dll, ucrtbase.dll, `api-ms-win-crt-*`

| Table (source file)                     | Entries | Contents |
|-----------------------------------------|--------:|----------|
| `lsiw_crt_core_exports` (crt_core.c)    | 224 | startup (`__getmainargs`, `__wgetmainargs`, `_initterm(_e)`, `_configure_*_argv`, `__set_app_type`, ...), `exit`/`atexit`/`_onexit` tables, `quick_exit`, `errno`/`_doserrno`, `malloc` family including `_aligned_*` and `_msize`, environment (`getenv`, `_putenv`, `_dupenv_s`, ...), `system`, `_spawn*`, `_exec*`, `_cwait`, `_beginthread(ex)`, `signal`/`raise`, `qsort`/`bsearch` (and `_s`), `rand`/`rand_s`, `setjmp`/`longjmp`, invalid-parameter and purecall handlers |
| `lsiw_crt_stdio_exports` (crt_stdio.c)  | 397 | `FILE` streams and the `__acrt_iob_func`/`_iob` tables, the ucrt `__stdio_common_*` entry points, the whole printf/scanf family (narrow, wide, `_s`, `_l`, `_p`), low-level I/O (`_open`, `_read`, `_write`, `_lseeki64`, `_pipe`, `_dup2`, `_setmode`, `_get_osfhandle`, ...), `_popen`, directories (`_mkdir`, `_getcwd`, `_chdir`, ...), `_stat*`/`_fstat*`, `_find*`, `_fullpath`, `_splitpath`/`_makepath`, `_tempnam`, `_utime*`, `conio.h` (`_getch`, `_kbhit`, `_cprintf`, ...) |
| `lsiw_crt_string_exports` (crt_string.c)| 396 | `string.h` and the `_s` variants, `wchar.h` with 16-bit `wchar_t`, `ctype.h`/`wctype.h` and the `_l` forms, number conversions (`strtod`, `_strtoi64`, `_itoa`, `_ecvt`, ...), multibyte conversions (UTF-8), `mbstring.h` |
| `lsiw_crt_math_exports` (crt_math.c)    | 296 | `math.h` in double, float and long double (= double on Windows x64) forms, Bessel functions, `_fpclass` & co., `_controlfp`/`_control87`/`_statusfp`/`_clearfp`, `fenv.h`, the ucrt `complex.h` functions |
| `lsiw_crt_time_exports` (crt_time.c)    | 100 | `time.h` with 64-bit `time_t` and the `_32`/`_64` variants, `_s` forms, `_ftime`, `strftime`/`wcsftime`, time zone variables, `setlocale`/`_wsetlocale`, `localeconv`, `_create_locale`, multibyte code page queries |
| `lsiw_msvcrt_exports`, `lsiw_ucrt_exports` (crt_tables.c) | 3 + 3 | `swprintf`, `vswprintf` and `wcstok`, whose signatures differ between msvcrt and ucrt |

### vcruntime140.dll, vcruntime140_1.dll

| Table (source file)                     | Entries | Contents |
|-----------------------------------------|--------:|----------|
| `lsiw_vcruntime_exports` (vcruntime.c)  |  64 | `_CxxThrowException`, `__CxxFrameHandler`/`2`/`3`/`4`, `__std_terminate`, `terminate`/`unexpected`, `__RTDynamicCast`, `__RTtypeid`, `type_info` members, `__std_exception_copy`/`destroy`, `__std_type_info_*`, `_set_se_translator`, `__uncaught_exceptions`, `__current_exception`, `__report_gsfailure`, `operator new`/`delete` |

### Other DLLs registered in `builtins.c`

`src/native/builtins.c` also maps these DLL names to LSIW tables:

| DLL names                                                     | Table(s) |
|---------------------------------------------------------------|----------|
| user32.dll                                                    | `lsiw_user32_exports` |
| shell32.dll, shlwapi.dll                                      | `lsiw_shell32_exports` (+ `lsiw_misc_exports` for shlwapi) |
| advapi32.dll, sechost.dll                                     | `lsiw_advapi32_exports` |
| ole32, oleaut32, combase, rpcrt4, crypt32, bcrypt, bcryptprimitives | `lsiw_ole_exports`, `lsiw_misc_exports` |
| ws2_32, mswsock, iphlpapi                                     | `lsiw_ws2_exports` (sockets, `WSAPoll`, overlapped I/O, `AcceptEx`/`ConnectEx`, name resolution, adapter addresses) |
| wsock32                                                       | `lsiw_wsock32_exports`, `lsiw_ws2_exports` |
| winmm, version, userenv, secur32, sspicli, dbghelp, dbgcore, powrprof, netapi32, mpr, wtsapi32, dwmapi, shcore, dnsapi | `lsiw_misc_exports` |
| psapi.dll                                                     | `lsiw_misc_exports`, kernel32's `K32*` functions |
| kernel32 additions                                            | `lsiw_k32extra_exports` |

Their function counts are printed by `make export-stats`, and their
function lists are the export tables in their source files.

## Programs tested

Upstream Windows release builds run on the native engine
(`tests/corpus.sh` repeats these runs):

| Program | Build | What was run |
|---------|-------|--------------|
| zstd 1.5.6 | MinGW-w64 | compress a file |
| jq 1.7 | MinGW-w64 | filter JSON |
| ninja 1.12.1 | MSVC | `--version`, builds generated by CMake |
| ripgrep 14.1.1 | MSVC and GNU | search files |
| fd 10.2.0 | MSVC | find files |
| uutils coreutils 0.0.28 | MSVC (Rust) | `sha256sum` and other tools |
| fzf 0.56.3, lazygit 0.44.1, GitHub CLI 2.61.0 | Go | `--version`; Go programs need the KUSER_SHARED_DATA page LSIW maps |
| Python 3.12 (python-build-standalone) | MSVC | `json`, `sqlite3`, `hashlib` |
| CMake 3.30.5 | MSVC | `-E` tools, configure and build a project with Ninja |
| bat 0.24.0, delta 0.18.2, xsv 0.13.0 | MSVC (Rust) | print, diff, count CSV |
| Node.js 20.18.0 | MSVC | `fs`, `crypto` |
| Git for Windows 2.47.0 (MinGit) | MinGW-w64 | `init`, `add`, `commit`, `log`, `diff`, `status`, `branch`, `checkout`, `stash`, `gc`, `fsck`, `blame`, `clone`/`fetch` over HTTPS with `-c http.sslBackend=openssl` |
| Wine's `cmd.exe` | MinGW-w64 | `cmd /c` scripts |

Programs that the native engine cannot run yet, such as 7-Zip's 32-bit
`7zr.exe` or CMake's `cmake-gui.exe`, run on the Wine fallback.

## Wine and Proton

Programs the native engine cannot run go to Wine (any installed version)
or, when chosen, to Proton and GE-Proton, all with LSIW's folders. Games
belong there: DirectX, Vulkan and anti-cheat are Proton's job, not the
native engine's. See [PROTON-STEAM.md](PROTON-STEAM.md), including the Steam
integration (Flatpak and Snap Steam are not supported).

## Not implemented

These are outside the native engine in 1.6.0. `lsiw check` names the exact
functions a program needs.

* **Windows and graphics:** window creation and message loops, GDI
  (gdi32.dll is not provided), common controls, DirectX/DXGI, OpenGL,
  Vulkan, Media Foundation.
* **Sound and devices:** audio output, joysticks, printers, `DeviceIoControl`
  requests to Windows drivers. Serial ports work: `COM1` is `/dev/ttyS0`.
* **Certificates and TLS:** the crypt32 certificate store and chain
  functions and the Schannel TLS provider are not implemented. Programs that
  carry their own TLS library work (Go programs, Python, Node.js, Git with
  `http.sslBackend=openssl`); programs that rely on Schannel or
  `CertGetCertificateChain` fail to make HTTPS connections, and programs that
  import the missing crypt32 functions run on Wine.
* **MSYS2 / Cygwin programs:** programs linked against `msys-2.0.dll` or
  `cygwin1.dll` (the `sh.exe`, `bash.exe`, coreutils shipped in Git for
  Windows' `usr/bin`) need the Cygwin `fork()` emulation and do not run on
  the native engine. Git's own `git.exe` does; Git features that start
  `sh.exe` (local `git clone` of a path, hooks, `git submodule`) do not.
* **Other processes' memory:** writing to another process
  (`WriteProcessMemory`) fails with `ERROR_ACCESS_DENIED`; reading works
  through `/proc`.
* **Windows services and drivers:** services cannot be installed, and
  kernel-mode drivers can never run on Linux.
* **.NET:** `mscoree.dll` is not provided.
* **32-bit (WOW64):** a 64-bit program cannot load 32-bit DLLs.

Behaviour that differs from Windows by necessity:

* There are no drive letters other than C:.
* Windows file security (ACLs, owners as SIDs) is not emulated: what a
  program may read or write is decided by Linux permissions.
