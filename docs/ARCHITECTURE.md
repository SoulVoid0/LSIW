# LSIW architecture

This document describes how LSIW runs a Windows program, from the moment
`lsiw run program.exe` starts (or the kernel starts it through binfmt_misc)
to the program's last `ExitProcess`. File names refer to the source tree.

```
 ./setup.exe      binfmt_misc: "MZ" -> libexec/lsiw/lsiw-binfmt (= lsiw)
 double-click     lsiw.desktop: Exec=lsiw run %f
 lsiw run x.exe
        |
        v
 src/cli: the lsiw command, which chooses the engine
        |                                    |
        | 64-bit, every import implemented   | otherwise: 32-bit, .msi, .bat,
        v                                    v  missing functions
 src/native: native engine             src/cli/wine.c: Wine fallback
 PE loader + Win32 API written in C,   LSIW's own Wine prefix, set up with
 running inside the lsiw process       the same C: drive and folders
        \                                   /
         `------------ src/common ----------'
          variables, configuration, the single C: drive

 lsiw-control (src/gui, GTK 4) edits the same configuration.
```

## Components

| Directory      | What it is                                                              |
|----------------|-------------------------------------------------------------------------|
| `include/`     | `lsiw.h` (shared API) and `pe.h` (PE/COFF structures)                   |
| `src/common/`  | Variable table, configuration, C: drive path translation, helpers       |
| `src/native/`  | The native engine: loader, TEB, SEH, Win32 and C runtime implementations |
| `src/cli/`     | The `lsiw` command, the Wine fallback, desktop and binfmt registration  |
| `src/gui/`     | `lsiw-control`, the GTK 4 settings app                                   |
| `data/`        | Desktop entries, MIME data, binfmt rule, default configuration, icon    |
| `tests/`       | Windows test programs (built with MinGW) and the test runner            |

`lsiw` is linked from `src/common`, `src/native` and `src/cli`;
`lsiw-control` from `src/common` and `src/gui` only. The GUI never loads
Windows code: it edits configuration and runs `lsiw` for anything that does.

## One Windows process = one Linux process

The native engine runs the Windows program inside the `lsiw` process
itself. `lsiw_native_run()` (`src/native/engine.c`) prepares the process
and then never returns in the normal case, because the program ends with
`ExitProcess`:

1. Ignore `SIGPIPE` (Windows reports a broken pipe as an error, not a
   signal).
2. Create the handle table and the standard handles (`handles.c`).
3. Create the main thread's TEB and PEB and point the GS segment at the
   TEB (`teb.c`).
4. Install the CPU fault handlers that turn signals into Windows
   exceptions (`seh.c`).
5. Build the Windows environment block (`k32_process.c`): `PATH` becomes a
   `;`-separated list of `C:\` paths with `C:\windows\system32` first,
   `USERPROFILE`, `APPDATA`, `TEMP`, `ProgramFiles` and the other folder
   variables come from the variable table, and `OS`, `PROCESSOR_*`,
   `NUMBER_OF_PROCESSORS` are set as on Windows.
6. Build the command line. Arguments that are existing absolute Linux paths
   are converted to `C:\` paths (`[runtime] translate_args`). A child
   started by `CreateProcess` gets its parent's exact command line through
   `LSIW_CMDLINE` instead.
7. Initialise the C runtime state (`crt_core.c`).
8. Load the executable and everything it imports (`loader.c`), then call
   its entry point with the PEB, as Windows does.

## The PE loader (`src/native/loader.c`)

* Accepts PE32+ images for x86-64 (`IMAGE_FILE_MACHINE_AMD64`). 32-bit
  images, MS-DOS programs and other CPU types are refused with a reason,
  and the CLI then hands them to Wine.
* Maps the image at its preferred base address with `MAP_FIXED_NOREPLACE`
  and falls back to any free address plus base relocations. `lsiw` is
  built as a normal position-independent executable, so the usual image
  bases (0x140000000 for MSVC, 0x400000 / 0x140000000 for MinGW) are free.
* Copies sections, applies relocations, resolves imports, sets up implicit
  TLS (`.tls`: index, template, zero fill, callbacks), applies section page
  protections, runs TLS callbacks and `DllMain` for DLLs.
* Import resolution, per DLL name:
  1. a module already loaded under that name;
  2. for the core system DLLs (kernel32, kernelbase, ntdll, msvcrt,
     ucrtbase, user32, advapi32, shell32 and every `api-ms-win-*` /
     `ext-ms-win-*` set) always LSIW's own implementation, even when a copy
     of the Windows DLL sits next to the program;
  3. a PE DLL found in the program's folder, the current folder,
     `C:\windows\system32`, then the Windows `PATH`;
  4. LSIW's own implementation of that DLL, if it has one.
* A function LSIW does not implement is bound to a small generated stub.
  Calling it prints `lsiw: the program called DLL!Function, which the LSIW
  native engine does not implement` and exits with status 127, so a
  program never runs on with a silently wrong result. `lsiw check` finds
  all such imports in advance by scanning the program and every PE DLL it
  loads, without running anything.

## TEB, PEB and the GS segment (`src/native/teb.c`)

Windows x64 code reaches its Thread Environment Block through the GS
segment: `GS:[0x30]` is the TEB itself, `GS:[0x58]` the implicit-TLS
array, `GS:[0x60]` the PEB, `GS:[0x68]` the last-error value,
`GS:[0x1480]` the `TlsAlloc` slots. glibc keeps its thread pointer in FS,
so GS is unused on Linux; every thread that runs Windows code gets its own
TEB installed with `arch_prctl(ARCH_SET_GS)`. Compiler intrinsics such as
`NtCurrentTeb()` and `GetLastError()` inlined into the program therefore
work unchanged. LSIW keeps its private per-thread data (fiber-local
storage, the C runtime's `errno`, `strtok` state) in the TEB after the
fields Windows defines.

## Win32 functions in C, called directly (Microsoft x64 ABI)

Every Windows-callable function is ordinary C compiled with
`__attribute__((ms_abi))` (`WINAPI` in `native.h`). Windows code calls it
directly: there are no thunks, no relay layer and no address-space split.
GCC takes care of the differences between the two calling conventions
(argument registers, the 32-byte shadow space, `rsi`/`rdi` and
`xmm6`–`xmm15` being callee-saved on Windows).

Conventions that follow from Windows being a different platform:

* `WCHAR` is 16 bits (UTF-16). Linux's `wchar_t` is 32 bits, so every wide
  string function (`wcslen`, `wprintf`, `CompareStringW`, ...) is
  implemented in LSIW instead of being forwarded to glibc.
* Windows is LLP64: `long` is 32 bits. The printf/scanf engines
  (`printf.c`) read an MS-ABI `va_list` themselves (`__builtin_ms_va_list`)
  and apply Windows' size rules (`%ld` is 32-bit, `%I64d`, `%zd`, `%ls` /
  `%hs` / `%S` as in msvcrt), handing each single value to the host
  formatter.
* The ANSI code page is UTF-8 (65001), so `A` and `W` functions agree and
  file names round-trip with Linux.
* `HANDLE`s are small multiples of 4 as on Windows (`handles.c`). Each
  refers to an object that wraps a file descriptor, a directory search, a
  thread, a child process or a synchronisation object. All waits go
  through one process-wide mutex and condition variable, which keeps
  `WaitForMultipleObjects` simple and correct.
* Memory a program releases with `HeapFree`, `LocalFree`, `GlobalFree`,
  `free` or `CoTaskMemFree` all comes from one process heap (`k32_mem.c`),
  so mixing them works as it does on Windows. `VirtualAlloc` and file
  mappings are built on `mmap`.

### The builtin DLL registry (`src/native/builtins.c`)

Each module exports a table:

```c
const struct lsiw_export lsiw_k32file_exports[] = {
    E(CreateFileW), E(ReadFile), EX("SetFilePointer", SetFilePointer), ...
    { NULL, NULL }
};
```

`builtins.c` maps DLL names to an ordered list of such tables. Many names
share one implementation: `kernel32`, `kernelbase` and the
`api-ms-win-core-*` sets; `msvcrt`, `msvcr70`–`msvcr120`, `ucrtbase` and
the `api-ms-win-crt-*` sets; and so on. The small tables in `crt_tables.c`
come first for the few C runtime names whose behaviour differs between
msvcrt and ucrt (`swprintf`, `vswprintf`, `wcstok`). Every builtin DLL also
gets a distinct `HMODULE` that points at a minimal, readable PE header, so
programs that inspect their modules do not crash. `GetProcAddress` and
`LoadLibrary` use the same registry.

`make export-stats` prints how many functions each table provides.

## Threads and processes (`src/native/k32_process.c`)

* `CreateThread` creates a pthread, gives it a TEB, runs TLS callbacks
  and `DllMain(DLL_THREAD_ATTACH)`, then calls the thread procedure with the
  MS ABI.
* Critical sections, SRW locks, condition variables, `InitOnce`,
  `WaitOnAddress`, events, mutexes, semaphores and waitable timers are
  implemented on the handle table's wait lock.
* `CreateProcess` of a Windows program spawns `lsiw run <program>` (the
  running `lsiw` binary, found through `/proc/self/exe`) with the exact
  Windows command line in `LSIW_CMDLINE`, inherited handles as file
  descriptors and the requested working directory. A Linux program reached
  through its `C:\` path is started directly with the split arguments.
  Exit codes and waits work through the child's process handle.
* A Windows child receives, through environment variables, the parent's
  Windows environment (`LSIW_FROM_WINDOWS`, with the Windows `HOME` and
  `PATH` in `LSIW_WIN_HOME`/`LSIW_WIN_PATH` so the Linux values stay
  usable), the `lpCurrentDirectory` name (`LSIW_CWD`) and a pipe on which
  it reports its 32-bit exit code (`LSIW_EXIT_FD`); signals become NT
  status codes (`SIGSEGV` is `0xC0000005`). These variables are hidden from
  the program.
* Every descriptor LSIW opens is close-on-exec; a child inherits only the
  standard handles named in `STARTUPINFO`, which is what programs that pass
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (Git, for one) expect.

## The NT layer (`ntdll.c`, `ntdll_file.c`, `ntdll_obj.c`, `kuser.c`)

* `ntdll_file.c` implements the `Nt*File` calls on top of the handle
  table; kernel32's `DeviceIoControl`, `LockFileEx` and `CancelIo(Ex)` call
  through it, so both layers report the same statuses.
* `ntdll_obj.c` provides the `Nt*` registry calls (`NtCreateKey` & co.),
  sections, named objects, security and the `Rtl` helpers on top of the
  kernel32 and advapi32 implementations, so there is one implementation of
  each; the registry itself lives in `advapi32.c` and is stored in
  `registry/system.reg` and `registry/user.reg` under LSIW's data folder.
* `kuser.c` maps KUSER_SHARED_DATA at `0x7FFE0000` and refreshes its clock
  fields every millisecond from a background thread; Go's runtime and some
  MSVC inlines read the time there without a system call.

## Debugging

* `LSIW_VERBOSE=1` logs the engine choice, every DLL loaded, unresolved
  imports and failed `LoadLibrary`/`GetProcAddress`/`CreateFile` calls.
* `LSIW_APITRACE=FILE` routes every import from LSIW's DLLs (and every
  `GetProcAddress` result) through a small thunk in `loader.c` that saves
  the argument registers, appends `TID DLL!Function` to `FILE` and jumps
  to the implementation. Processes started by the program append to the
  same file.

## Structured exception handling (`src/native/seh.c`)

LSIW implements the Windows x64 exception model from Microsoft's published
description of it: `.pdata`/`.xdata` unwind information,
`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`,
`RaiseException`, vectored exception and continue handlers, the unhandled
exception filter, `__C_specific_handler` (`__try`/`__except`/`__finally`),
`RtlAddFunctionTable` for JIT code, and `__fastfail` (`int 0x29`).

* **CPU faults.** `SIGSEGV`, `SIGBUS`, `SIGILL` and `SIGFPE` are caught on an
  alternate signal stack and converted to `EXCEPTION_ACCESS_VIOLATION`,
  `EXCEPTION_INT_DIVIDE_BY_ZERO`, `EXCEPTION_STACK_OVERFLOW` and so on, with
  a full `CONTEXT`. The dispatcher then walks the faulting thread's stack
  exactly as Windows would, so `__try/__except`, crash handlers and MinGW's
  C++ exceptions (libgcc's SEH personality) behave as on Windows.
* **Host regions.** LSIW's own functions are compiled for Linux and carry no
  Windows unwind tables. Every entry point that starts a dispatch or an
  unwind (`RaiseException`, `RtlUnwindEx`, the fault handler) therefore
  first captures the context of its Windows caller and records the stretch
  of host stack it occupies. When a stack walk reaches host code it
  continues from the matching record. This mirrors what Windows does when a
  walk crosses the dispatcher (back to the faulting context) or an
  in-progress unwind (a "collided unwind").
* **Context switches.** `RtlRestoreContext`, `longjmp`, fibers and the
  end of an unwind load a new `rsp`/`rip` directly. This is why `lsiw` is
  built without the CET shadow-stack marking (`-fcf-protection=branch`, see
  the Makefile): an enforced shadow stack would reject these transfers.

## The C runtime (`src/native/crt_*.c`, `printf.c`)

One implementation serves `msvcrt.dll`, the `msvcr*` versions,
`ucrtbase.dll` and the `api-ms-win-crt-*` sets, which MinGW and MSVC
programs import:

* startup and exit: `__getmainargs`, `_initterm`, `_configure_narrow_argv`,
  `atexit`/`_onexit` tables, `_cexit`, `quick_exit`;
* `errno` per thread, memory (`malloc` on the shared process heap,
  `_aligned_malloc`, `_msize`), environment, `system`, `_spawn*`, `_exec*`,
  `_beginthreadex`, signals, `setjmp`/`longjmp` with the Microsoft
  `_JUMP_BUFFER` layout;
* stdio over CRT file descriptors over Win32 handles over Linux file
  descriptors; text mode drops the `\r` of `\r\n` when reading and writes
  `\n` unchanged; `_O_U16TEXT`/`_O_U8TEXT` wide modes; `conio.h`;
* strings, wide strings (UTF-16), conversions, multibyte functions;
* `math.h` through the host `libm` (Windows x64 `long double` is a plain
  `double`), `float.h` control words, `fenv.h`, the ucrt complex functions;
* `time.h` with 64-bit `time_t` and the `_32`/`_64` variants, `locale.h`
  reporting UTF-8.

### C++ exceptions and RTTI (`src/native/vcruntime.c`)

MSVC programs throw with `_CxxThrowException` and find handlers through the
`__CxxFrameHandler3` / `__CxxFrameHandler4` language handlers (including
the compressed FH4 tables), `dynamic_cast` and `typeid` use the RTTI
complete object locators. LSIW implements these from the published layout
of MSVC's exception and RTTI data. A `catch` block runs as the
consolidation callback of `RtlUnwindEx(STATUS_UNWIND_CONSOLIDATE)`, exactly
as on Windows. MinGW-built C++ programs bring their own runtime
(`libstdc++`/`libgcc`) inside the executable or DLLs next to it; those sit
on the same SEH implementation.

## The path layer and the single C: drive (`src/common`)

* **`vars.c`** is the variable table: every folder and value variable
  Microsoft's USMT documentation recognises (`CSIDL_*`, `APPDATA`,
  `WINDIR`, `PROGRAMFILES`, `USERPROFILE`, ...) plus the standard Windows
  environment variables USMT does not list (`LOCALAPPDATA`, `PROGRAMDATA`,
  `PUBLIC`, ...). Each entry has a kind:
  *folder* (relocatable), *virtual* (a shell namespace object such as the
  Recycle Bin, mapped to the Linux trash), *value* (`USERNAME`,
  `SYSTEMDRIVE`), *alias* (`APPDATA` = `CSIDL_APPDATA`) or *derived*
  (`SYSTEM32` always lives inside `WINDIR`). Defaults are templates such as
  `${XDG:DOCUMENTS}` or `${VAR:CSIDL_PROFILE}/AppData/Roaming`.
* **`config.c`** loads `/etc/lsiw/lsiw.conf` and then the user's
  `~/.config/lsiw/lsiw.conf`, stores settings and variable overrides, and
  resolves each variable to its Linux location and its Windows path.
* **`paths.c`** translates paths. `C:\` is the C: drive folder
  (`lsiw_root_dir()`, `[drive] c_drive`, default `~/.cdrive`), which links
  every top-level Linux folder: `C:\home\me` is `/home/me` (the link is
  followed at once, so programs get the plain Linux path),
  `C:\run\media\me\USB` is a USB disk, and anything else is a real folder
  of the C: drive. Any other drive letter
  is folded onto C:, so a program asking for `D:\data` gets `C:\data`;
  disks are folders, as on Linux. A few folders that Windows programs
  expect directly under `C:\` (`windows`, `Program Files`,
  `Program Files (x86)`, `ProgramData`, `users`) are *anchored*: they appear
  under their Windows names but live wherever the user configured them.
  Lookups fall back to case-insensitive matching on each path component,
  `.` and `..` are resolved lexically like `GetFullPathName`, trailing dots
  and spaces are dropped as Windows does, `\\?\` and `\\.\` prefixes are
  accepted, and UNC paths (`\\server\share`) map to GVFS SMB mounts.
* **`lsiw_sync_root()`** fills the C: drive with symlinks (one per
  top-level Linux directory plus the anchored folders; `users` is a real
  folder of the drive with `<you>` and `steamuser` linked to the profile).
  The links it removes are only its own.
* **`prefix.c`** is where C: is (`lsiw_root_dir()`, `lsiw_cdrive_move()`,
  which moves the drive and relinks the old place), and the shared prefix
  (`lsiw_shared_dir()`; `$LSIW_PREFIX` selects a program's own prefix;
  `lsiw_prefix_link_drive()` makes `pfx/drive_c` a link to the C: drive and
  keeps a real `drive_c` as `drive_c.lsiw-backup`). It never deletes a
  folder that holds data: it renames it.
* **`programs.c`** is the program library (`[program:ID]` sections) and
  the custom engine chain (`[engine] chain`).
  `lsiw_program_icon()` gives a program's icon as a file (cached as
  `<data>/icons/ID.png`).
* **`icon.c`** reads Windows icons without any library: the resource
  directory of a PE file (the first `RT_GROUP_ICON` and its `RT_ICON`
  images) or an `.ico` file, picks the largest image up to 256x256,
  decodes BMP images (1/4/8/24/32 bits, AND mask) to RGBA and writes PNG
  with stored deflate blocks (PNG images are passed through).
* **`lwlnk.c`** reads and writes LSIW shortcuts (`.lwlnk`): Desktop-Entry
  style text naming a program of the library, a copy of its settings, and
  its icon as a base64 PNG (`IconData=data:image/png;base64,...`), so a
  sandboxed thumbnailer needs nothing but the file. `src/cli/lwlnk.c` has
  `lsiw launch` and `lsiw lwlnk create|show|thumbnail`; launching prefers
  the library's current settings and falls back to the copy.

## Configuration

Plain INI files: `[section]` headers and `key = value` lines; `#` or `;`
starts a comment line. Settings live in `[engine]` and `[runtime]`,
variable overrides in `[variables]`. `/etc/lsiw/lsiw.conf` is read first,
the user's file second (its values win); `$LSIW_CONFIG` replaces both. The
CLI (`lsiw config`) and the GUI write the user's file. `data/lsiw.conf`
documents every key.

Environment variables: `LSIW_CONFIG` (configuration file), `LSIW_DATA_DIR`
(instead of `~/.local/share/lsiw`), `LSIW_VERBOSE=1` (trace loader and
engine decisions to standard error).

## The Wine fallback (`src/cli/wine.c`)

Wine runs what the native engine cannot: 32-bit programs, windowed (GUI)
programs whose imports LSIW does not implement yet, `.msi` installers and
batch files. LSIW keeps its own Wine prefix in
`~/.local/share/lsiw/shared/pfx`, the prefix every Wine and Proton version
shares (its `drive_c` is a link to the C: drive), and arranges it to match
the native view:

* `dosdevices/c:` points at the C: drive (`lsiw_root_dir()`), and every
  other drive letter Wine creates (`z:` in particular) is removed;
* the `User Shell Folders` / `Shell Folders` registry values, the Program
  Files locations and `HKCU\Environment` `TEMP`/`TMP` point at the folders
  configured in LSIW;
* Wine is told not to take over Linux file associations.

A stamp file records a hash of every variable's location; the prefix is
refreshed automatically when a folder moves (`lsiw wine-setup` forces it).

### Engine choice

With `[engine] default = custom`, `run_chain()` tries the engines of
`[engine] chain` in order (a default and up to three fallbacks): native
when every import is implemented, Wine or Proton (optionally a given
version) when installed; an engine that cannot run the program, or that
cannot be started, hands it to the next one. A program of the library
(`lsiw programs run ID`) sets its own engine, `$LSIW_PREFIX` for a prefix
of its own, `LSIW_DXVK`/`LSIW_VKD3D` (Wine: DXVK DLLs from an installed
Proton plus `WINEDLLOVERRIDES`; Proton: `PROTON_USE_WINED3D`,
`PROTON_NO_D3D12`), its environment, `MANGOHUD` and debug variables.

`run_program()` in `src/cli/main.c` decides. With `[engine] default =
auto` (the default) it first identifies the file:

* a 64-bit PE image is checked with `lsiw_native_check()`, the same scan
  `lsiw check` prints. If every import is implemented, the native engine
  runs it. If not, Wine runs it when Wine is installed; without Wine the
  native engine runs it anyway after a warning, and the program stops with
  a message if it actually calls a missing function;
* 32-bit programs, `.msi` packages and batch files go to Wine, or fail with
  an explanation when Wine is not installed;
* if the native engine cannot load a program it accepted (a DLL that fails
  to initialise, for example), `auto` retries with Wine when it is
  installed.

`native`, `wine` and `proton` force one engine; `lsiw --engine=...` or
`lsiw run --engine=...` overrides the setting for one run. Under Wine,
`lsiw` replaces itself with `wine C:\...\PROGRAM.exe ARGS...`, so the
program's exit code is `lsiw`'s exit code in both engines.

## Wine versions, Proton and Steam (`src/common/runners.c`, `src/cli/proton.c`)

`runners.c` finds every installed Wine and Proton ("runners") and Steam's
folders (roots, `libraryfolders.vdf`, `appmanifest_*.acf`, read with a
small VDF tokenizer). It is in `src/common` because LSIW Control lists the
same versions. `lsiw_runner_pick()` resolves a setting (ID, name or path)
to one runner, with the defaults described in
[PROTON-STEAM.md](PROTON-STEAM.md).

All Wine versions share the one LSIW prefix; the Wine binary is part of
its stamp, so a switch reruns `wineboot`.

`steamtools.c` reads the list of Valve's compatibility tools from Steam's
binary app cache (`appcache/appinfo.vdf`, versions 28 and 29: a record
per app with binary KeyValues, keys by index into a trailing string table
in version 29), taking the `extended/compat_tools` entries of app 891390
and of the other apps that declare tools, so twins exist for versions that
are not installed. It also tells builds for another processor apart by the
ELF header of their Wine loader.

`proton.c` prepares Proton prefixes (`lsiw_proton_prepare()`: the user
folders of `drive_c/users/steamuser` and `users/Public` become links to
LSIW's folders, recorded in `<compatdata>/lsiw-links`), and starts Proton
the way Steam does: the runtime's and Proton's `toolmanifest.vdf`
command lines are chained (`_v2-entry-point --verb=VERB -- proton VERB
...`) with `STEAM_COMPAT_TOOL_PATHS` and `STEAM_COMPAT_MOUNTS` set. The
Steam tools `lsiw steam add` writes contain a manifest without
`require_tool_appid`, so Steam runs their `lsiw-proton` script on the host;
`lsiw steam-run` prepares the prefix there and then enters the runtime
itself. Downloads (`lsiw runners install`) use GitHub's release API, with
`git ls-remote` and the projects' fixed archive names as the fallback.

## Desktop and kernel integration

* `data/zz-lsiw.conf` is a binfmt_misc rule for the `MZ` signature. The
  interpreter, `libexec/lsiw/lsiw-binfmt`, is a symlink to `lsiw`; started
  under that name, `lsiw` behaves as `lsiw run`. binfmt_misc tries the most
  recently registered rule first and systemd-binfmt registers files in name
  order, so the `zz-` prefix puts LSIW ahead of Wine's own rule. The file
  must be executable (`chmod +x setup.exe`), as for any program.
* `data/lsiw.desktop` registers `lsiw run %f` for the Windows executable
  and installer MIME types; `data/lsiw.xml` adds `*.com`.
* `lsiw register` / `lsiw unregister` set up (or remove) the same things per
  user or system-wide on installations that did not come from a package.

## The settings app (`src/gui`)

`lsiw-control` is a GTK 4 application (application ID
`io.github.lsiw.Control`, desktop entry `lsiw-control.desktop`) for the
settings `lsiw config` manages: where each Windows folder lives and which
engine runs programs. It is linked with `src/common` only, so it uses
exactly the same configuration format, variable table and path rules as
`lsiw`, and it never loads Windows code itself.

## What LSIW cannot do, by design

* **Kernel-mode drivers** (`.sys` files, printer and device drivers,
  anti-cheat and DRM drivers) run inside the Windows kernel and use its
  internal interfaces (the I/O manager, IRPs, the Windows driver model).
  Linux is a different kernel; no user-space runtime can provide those
  interfaces. Hardware is supported on Linux by Linux drivers instead.
* **32-bit programs** need a 32-bit process; the native engine is 64-bit
  only. Wine runs them.
* **.NET programs** need the .NET runtime; use Wine with Mono, or the
  Linux build of .NET for programs that offer one.
* **DirectX and games** need graphics, audio and input stacks that the
  native engine does not provide; Wine (or Proton) is the tool for them.
