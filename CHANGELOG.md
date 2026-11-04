# Changelog

All notable changes to LSIW are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions
follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.10.1] - 2026-11-04

### Fixed

- **Programs were one entry in the taskbar and missing from System
  Monitor.** Proton names every window `steam_app_<SteamAppId>`, and LSIW
  ran every program as app 0, so the taskbar grouped all of them under one
  icon; and they ran in the cgroup of LSIW Control, so System Monitor's
  Applications list counted them as LSIW Control. Now each program gets:
  - its own app ID under Proton (SteamAppId/SteamGameId, a number of 2^31
    or more, the way Steam numbers non-Steam shortcuts, so it never matches
    a real Steam game or its game-specific fixes), so its windows have
    their own class, `steam_app_N`; a game Steam starts keeps Steam's ID;
  - a desktop entry: its applications-menu shortcut (`lsiw-ID.desktop`,
    which now has `StartupWMClass`; older ones get it on the next run), or
    else a hidden one, `lsiw.ID.desktop` (`NoDisplay`), with its name and
    icon; `BAMF_DESKTOP_FILE_HINT` points at it, which Plasma's task
    manager reads for windows whose class says nothing (plain Wine);
  - a systemd user scope of its own, `app-lsiw-<desktop ID>-<random>.scope`
    in `app.slice`, the name System Monitor reads the application from.
    Wine's shared processes (wineserver, services.exe, explorer.exe ...)
    move to a `lsiw-wine-*.scope`, so a program's entry ends with the
    program. Without systemd (or libsystemd), programs run as before.
  `LSIW_NO_IDENTITY=1` turns this off.
- tests/identity.sh (in `make check-cli`): app IDs, desktop entries and,
  with a systemd user manager, the scopes.

## [1.10.0] - 2026-11-04

### Added

- **Several programs at once.** LSIW Control's *Launch* and *Run* stay
  available while programs run, so any number of programs and games run
  side by side (the same one more than once too). Their output goes to the
  same view, each part under the name of the program it comes from.
- **Stop a program of your choice.** While one program runs, *Stop* is a
  button as before; while several run, it is a menu with one entry per
  running program and *Stop all*. The menu also lists library programs
  started outside LSIW Control (from an LSIW shortcut (.lwlnk), the
  applications menu or `lsiw programs run`), which can be stopped from
  there too; with only such a program running, the plain *Stop* stops it.
- tests/instances.sh (in `make check-cli`): two programs in one prefix,
  with stand-in Protons and a stand-in wineserver.

### Fixed

- **A second program hung at "Running ..." until the first one ended**,
  for example a game started from its .lwlnk and an app launched in LSIW
  Control: its output stopped after the ProtonFixes lines. LSIW started
  Proton with `waitforexitandrun`, which waits for the prefix's wineserver
  to exit, that is for every program already running in the shared
  prefix. A program started while the prefix is in use now joins it
  (Proton `run`).
- **Two Wine versions in one prefix.** A wineserver serves only clients of
  its own Wine version, and Proton rewrote the prefix ("Upgrading prefix
  from dwproton-11.0-16 to 11.0-100") under the running program. A program
  started while another runs in its prefix now uses the Proton or Wine that
  runs the prefix (LSIW says so in the output); the next start after all
  have ended uses the configured one again.
- **A program counted as running until every program of the prefix had
  ended.** Proton's launcher (umu.exe, steam.exe) waits for all programs of
  the prefix, so play time kept counting and *Stop* stayed on for a program
  that had already closed. `lsiw` now stays behind as a small watcher of
  each program it starts with Wine or Proton (its processes carry
  `LSIW_INSTANCE`): the instance ends when its own programs have ended.
- **Stop did not reach the program itself under Wine and Proton**: Wine
  starts each Windows program in a session of its own, outside the process
  group *Stop* signalled. Stopping a program now ends every process of that
  program, and never the wineserver or Wine's services that the other
  programs of the prefix share.

## [1.9.1] - 2026-11-04

### Fixed

- **Programs did not start with Proton** (GE-Proton, DW-Proton, Proton -
  Experimental and every other build): Proton stopped with
  `FileNotFoundError: ... shared/tracked_files` in `update_builtin_libs`
  before the program started. Proton reads that list whenever the prefix
  counts as made (`pfx/creation_sync_guard`, which it also writes when it
  upgrades a prefix from before Proton 10, such as a version file saying
  `4.11-2`), and the shared prefix had the guard but not the list. LSIW now
  repairs such prefixes before starting Proton (the shared prefix, a
  program's own prefix and Steam games through LSIW Proton): it creates
  the missing list, or, for a guard without a prefix behind it, lets Proton
  set the prefix up again. Nothing in the prefix is deleted.
- **Steam detection.** Steam installed by Debian's and Ubuntu's
  steam-installer (`~/.steam/debian-installation`), in `$XDG_DATA_HOME/Steam`
  or in another Flatpak data folder is found, so its Proton versions
  (`steamapps/common/Proton*` in every library, `compatibilitytools.d`) are
  listed and run. `STEAM_COMPAT_CLIENT_INSTALL_PATH` is the Steam folder
  that holds the client itself (`steam.sh`, `steamclient.so`), not just
  the first folder with a `steamapps`.
- The warning "Proton - Experimental needs the Steam client ... and Steam
  is not installed" appeared although Steam was installed (it looked for
  `ubuntu12_64/steamclient.so` only) and was wrong anyway: Valve's Proton
  starts programs outside Steam. It is gone; with `-v` a note says when
  Steam is really missing.
- The "asks for the Steam Linux Runtime, which is not installed" message
  is only shown with `-v` for GE-Proton and similar builds, which run
  without it.

### Added

- tests/proton.sh (in `make check-cli`): prefix repair, Steam detection and
  the client folder, with a stand-in Proton.

## [1.9.0] - 2026-11-03

### Added

- **Play time tracking.** LSIW records how long each program of the
  library runs when it is started with `lsiw programs run`, an LSIW
  shortcut (.lwlnk), LSIW Control's *Launch* or an applications-menu
  shortcut: total play time, launches, first and last played, the last
  session's length and result. `lsiw` waits for the program in a small
  watcher process (Ctrl+C reaches the program; SIGTERM/SIGHUP are passed
  on; the program's exit code stays lsiw's) and saves every 30 seconds, so
  crashes and power losses lose at most that much (counted as *lost*
  sessions). Sleep time is not counted; overlapping sessions of one
  program count once. `lsiw programs playtime [ID] [--reset]`, play time in
  `lsiw programs show`; off with `[programs] track_playtime = false` or
  `LSIW_NO_PLAYTIME=1`. Records: `~/.local/share/lsiw/playtime/`
  (src/common/playtime.c). Tests: tests/playtime.sh.
- **Library layouts in LSIW Control.** The Programs page shows the
  library as a *Grid* of big icons, a *List* (as before, now with play
  time) or *Details*, a table with sortable, resizable columns (name, play
  time, last played, launches, last session, engine, program), with *Sort
  by* name, most played, recently played or most launched. The choice is
  remembered (`[control] programs_layout`, `programs_sort`). Play time
  updates live and the page shows what is running now (src/gui/libview.c).

### Changed

- Double-clicking (not a single click) a program in the List layout opens
  its settings, as the page always said.

## [1.8.0] - 2026-11-03

### Added

- **Folders as drives (D:, E:, ...).** `lsiw drives add D ~/Games` makes a
  folder the drive `D:\` ([drives] d in the configuration), for every
  engine: LSIW's own (paths keep the letter both ways; `GetLogicalDrives`,
  `GetLogicalDriveStrings`, `GetDriveType`, `GetVolumeInformation`,
  `GetVolumePathName` know the drive), Wine (`dosdevices/d:` of the
  prefix; other letters are still removed) and Proton (`dosdevices/d:` of
  the shared or the program's own prefix, and the folder is passed to
  Steam's runtime container). LSIW Control's C: drive page is now
  *Drives*, with an *Other drives* list to add, change, open and remove
  them. A letter that is no drive still means C:. Tests: tests/drives.sh
  and tests/src/t_drives.c.
- **Update check and updater.** `lsiw update [check|status|install|auto]`.
  LSIW asks GitHub for the newest release at most once a day, in the
  background, after commands typed in a terminal and when LSIW Control
  starts (off with `lsiw update auto off`, `[update] check = false` or
  `LSIW_NO_UPDATE_CHECK=1`), and shows one line when one is out.
  `lsiw update install` downloads the packages of the new release for the
  installed format, verifies them against its `SHA256SUMS` and installs
  them (sudo in a terminal, pkexec from LSIW Control; a user-owned
  AppImage is replaced in place). LSIW Control has an *Updates* page, a
  *Check for Updates* menu item and a banner when a new version is out.
- **Online and offline installers** in every release:
  `lsiw-VERSION-online-installer[-rpm|-deb|-appimage].sh` (download what
  they need) and `lsiw-VERSION-offline-installer[-rpm|-deb|-appimage].run`
  (self-extracting, everything inside, verified before installing).
  `install.sh` has `--offline` and `--version=VERSION|latest`, and finds
  the desktop user under pkexec too.

## [1.7.1] - 2026-11-02

### Added

- **Debian/Ubuntu packages and an AppImage**, next to the Fedora RPMs:
  `make deb` builds `lsiw_VERSION-1_amd64.deb` and
  `lsiw-control_VERSION-1_amd64.deb` (packaging/deb), and `make appimage`
  builds `LSIW-VERSION-x86_64.AppImage` (packaging/appimage), one file
  with `lsiw`, LSIW Control and GTK 4 that runs on any x86-64
  distribution. `install.sh` installs whichever format fits the system.

### Changed

- LSIW is now licensed under the GNU Affero General Public License,
  version 3 or later (was MIT), and lives at
  https://github.com/SoulVoid0/LSIW.

- **The C: drive is `~/.cdrive` by default** (a hidden folder), not
  `~/c_drive`. An existing `~/c_drive` moves to `~/.cdrive` the first time
  LSIW or LSIW Control builds the C: drive (`lsiw_cdrive_migrate()`), and
  `~/c_drive` becomes a link to it, so Wine and Proton prefixes, programs
  of the library and .lwlnk shortcuts that name `~/c_drive/...` keep
  working. Nothing is deleted: if `~/.cdrive` already has files, `~/c_drive`
  is left as it is and LSIW says so. A C: drive you moved elsewhere
  (`[drive] c_drive` set) is not touched. `lsiw cdrive reset` and LSIW
  Control's *Default* button go to `~/.cdrive`.

## [1.7.0] - 2026-11-02

### Added

- **LSIW shortcuts (.lwlnk).** A file that starts a program of the library
  with the settings it has there (engine, Wine or Proton version, prefix,
  DXVK, VKD3D, arguments, start folder, environment, MangoHud, debug log),
  so later changes apply to it. Double-click it, or `lsiw launch FILE`
  (`lsiw run FILE.lwlnk` and `lsiw FILE.lwlnk` work too). It keeps a copy
  of the settings for when the program is no longer in the library or the
  file is on another computer. `lsiw lwlnk create ID [PATH] [--icon IMAGE]`
  makes one (on the desktop by default, the XDG desktop folder), `lsiw
  lwlnk FILE` shows one. New MIME type `application/x-lsiw-shortcut`
  (`*.lwlnk`, and files starting with `[LSIW Shortcut]`), opened by
  LSIW's desktop entry; `lsiw register --user` makes LSIW its default.
- **Desktop shortcut** button on every program of LSIW Control's Programs
  page (and under the list), which puts an LSIW shortcut on the desktop.
- **Program icons.** LSIW reads the icon inside a Windows program (the
  first icon group of an .exe or .dll, as Explorer shows it; PNG and BMP
  images, every colour depth) or an .ico file and converts it to PNG. A
  program's icon is shown in the library list, used by its menu and desktop
  shortcuts, and embedded in its .lwlnk files. *Icon* in a program's
  settings (`icon=FILE`) chooses another: an image (LSIW Control converts
  .svg, .jpg and other formats to PNG) or an .exe/.dll to take it from.
- File managers show each .lwlnk file's own icon: a thumbnailer
  (`lsiw-lwlnk.thumbnailer`, for GNOME Files, Nemo, Caja, Thunar, PCManFM
  and Dolphin with KDE Frameworks 6.6+) reads the embedded PNG, so it works
  in GNOME's thumbnailer sandbox; GNOME Files, Nemo and Caja also get it as
  the file's custom icon at once. Without thumbnails the files show a new
  LSIW shortcut icon (`application-x-lsiw-shortcut`).
- `make check` runs `tests/lwlnk.sh`, tests of shortcuts and icons that
  need no Windows tools.

### Changed

- The Programs page's *Create shortcut* button is now *Add to
  applications menu* (the menu entry only); the desktop gets the .lwlnk
  shortcut from *Desktop shortcut*. The *Desktop and applications menu
  shortcut* setting still makes both .desktop launchers.

## [1.6.0] - 2026-11-01

### Added

- **One shared prefix.** Every Windows program and game uses one prefix by
  default, whether it runs with Wine or with Proton: `lsiw_shared_dir()`,
  `~/.local/share/lsiw/shared` (`[prefix] shared`), laid out like a Steam
  compatdata folder (`pfx/` is the Wine prefix). The Wine prefix of 1.5
  (`~/.local/share/lsiw/prefix`) moves into it once and the old path
  becomes a link. `lsiw prefix` shows it, the prefixes of programs that have
  their own, and old prefixes LSIW no longer uses (never deleted).
- **The C: drive is a folder you choose**: `[drive] c_drive`, default
  `~/c_drive`, set with `lsiw cdrive set PATH` or LSIW Control's C: drive
  page. Every engine sees it as C:\: the native engine's path converter,
  Wine's `dosdevices/c:` and the shared prefix's `drive_c` (a link to it).
  It still shows every top-level Linux folder (`C:\home`, `C:\mnt`, ...).
  What a program creates directly under C:\ is a real folder in it. Moving
  it moves its contents (copying across disks and keeping the original as
  `<old>.old`) and leaves a link at the old place; `--with-folders` moves
  the Windows folder, Program Files and ProgramData into it as well.
- **Steam games run in the shared prefix** through the LSIW Proton tools
  (`[steam] shared_prefix`, on by default). A game that already had a
  prefix of its own (`steamapps/compatdata/<appid>`) keeps it untouched as a
  backup, and the files of its user folders (saves, settings) are copied
  into LSIW's folders where missing on its first start. `lsiw steam
  default [--all]` (LSIW Control: *Use for every game*) makes LSIW Proton
  Stable Steam's default for every game, including games added later, and
  moves games set to one of Valve's Protons or another Proton with an LSIW
  twin to that twin; Steam's `config.vdf` is backed up first and Steam must
  not be running. `lsiw steam games` lists the games and their prefixes,
  `lsiw steam own|shared APPID` gives one game its own prefix or the shared
  one again.
- **Program library** (`lsiw programs`, LSIW Control's Programs page): add
  Windows programs and games, or let *Find installed* (`lsiw programs scan
  --add`) add what installers put in the Start menu. Each has its own engine
  or compatibility tool (native, any Wine, any Proton, the custom chain),
  prefix (shared, or one of its own), DXVK and VKD3D (on, off or default;
  under Wine, *on* uses the DXVK/VKD3D-Proton of an installed Proton),
  launch arguments, working folder, environment variables, MangoHud, a
  debug log, and a desktop and applications-menu shortcut that runs `lsiw
  programs run ID`.
- **Custom default engine**: `[engine] default = custom` with `[engine]
  chain` (`lsiw config chain native,wine,proton`, or the Engine page's
  *Custom configuration*): a default engine and up to three fallbacks, each
  tried when the one before cannot run the program (not installed, missing
  functions, the wrong kind of program, or it cannot be started).

### Changed

- `C:\users` is a real folder of the C: drive (`PROFILESFOLDER` defaults to
  `${ROOT}/users`), with `C:\users\<you>` and `C:\users\steamuser`
  (Proton's user) both leading to your profile, so Wine and Proton share
  one profile. `C:\users\Public` and `Default` still lead to
  `/home/Public` and `/home/Default`.
- `lsiw run --engine=proton` uses the shared prefix instead of
  `~/.local/share/lsiw/proton-prefix`, which is kept but no longer used.
- LSIW's configuration file keeps each section together when saved.

### Notes

- Switching between Wine and Proton versions on the shared prefix makes the
  next start update the prefix (wineboot), which takes a few seconds. Two
  different Wine/Proton versions cannot run in the same prefix at the same
  time; give a program its own prefix when it must run alongside another.

## [1.5.1] - 2026-10-26

### Fixed

- Discord and other Squirrel installers quit with exit code 8 (Ninite:
  "Timeout") when the Wine prefix had no .NET Framework. A prefix that was
  marked ready never got Wine Mono if its download or install failed once.
  LSIW now checks the prefix before each program runs on Wine and installs
  Wine Mono, or repairs Wine's Windows services, when they are missing. A
  failed Mono download is retried at most once a day (always by `lsiw
  wine-setup`).
- A prefix refreshed with a distribution's unpacked Wine Mono (Fedora's
  wine-mono package) never got .NET: wineboot installs it only into a new
  prefix. LSIW now installs the MSI in that case.

## [1.5.0] - 2026-10-21

### Added

- Windows shortcuts (.lnk): `lsiw run LINK.lnk` and opening one in the
  file manager start its target in the shortcut's start folder with its
  arguments. `lsiw lnk` shows a shortcut, `lsiw lnk create` and `lsiw lnk
  set` make or change one (Windows paths inside, so it also works in Wine
  and on Windows). LSIW registers itself for application/x-ms-shortcut.
- "Link to Windows Application..." in KDE's Create New menu (Dolphin and
  the desktop), and a matching template in the Templates folder on GNOME,
  Cinnamon, MATE and Xfce. Opening the new shortcut asks for the program.

### Fixed

- The Wine prefix had no Windows services (RpcSs, MSIServer, Eventlog,
  Winmgmt, Schedule, ...): wineboot starts services.exe in
  C:\windows\system32 before it creates that folder, and in LSIW's layout
  (~/Windows) it did not exist yet. LSIW creates it first, and existing
  prefixes are repaired once with `wineboot -u`. This broke installers
  such as the Visual C++ 2008 and 2010 redistributables.
- The Wine prefix had no .NET Framework: LSIW now installs Wine Mono (the
  version the Wine build asks for, SHA-256 checked; `[wine] mono = off`
  turns this off). Needed by GitHub Desktop, Discord, VSTO and other
  installers.
- Wine's PowerShell stub (which runs nothing and reports success) is
  turned off in LSIW's prefix: electron-builder installers such as
  Bitwarden thought the app was running and quit with exit code 2.

## [1.4.2] - 2026-10-21

### Fixed

- "Set up Wine prefix" failed with wineboot exit status 139 and a crash of
  wine-preloader. Wine 11.20 asks Linux for 32 GB of memory at start and
  crashes when it is refused, which Linux's default overcommit rule does
  whenever RAM plus swap is below 32 GB (Wine 11.19 does not). LSIW no
  longer downloads Wine 11.20, and when a Wine build LSIW downloaded
  crashes in wineboot, LSIW installs the release before it, makes it the
  configured Wine and sets the prefix up again.
- `lsiw wine-setup` explains a crash of Wine: the signal, RAM and swap,
  `vm.overcommit_memory`, `vm.mmap_min_addr` and SELinux.

## [1.4.1] - 2026-10-21

### Fixed

- LSIW Control's Engine page listed Valve's Protons as plain "(Steam)"
  entries. The Proton list now shows LSIW Proton Stable and LSIW's twin of
  every Proton version Steam lists (installed or not; Steam downloads one
  the first time it is used), then LSIW's twin of every other Proton. The
  new setting value `steam:NAME` (`lsiw config proton steam:proton_11`)
  names one of them.
- "Set up Wine prefix" failed with "could not load kernel32.dll" and a
  crash of wine-preloader: the Wine build LSIW downloaded needs the
  computer's 32-bit libraries. LSIW now downloads Kron4ek's WoW64 build,
  which needs none and still runs 32-bit programs, and replaces an older
  download of the same version before setting up the prefix.

## [1.4.0] - 2026-10-21

### Added

- `lsiw runners install cachyos|dwproton|rtsp|tkg|em|wineland` downloads
  Proton-CachyOS, DW-Proton, Proton-GE RTSP, Proton-Tkg, Proton-EM and
  Proton-Wineland (the newest release, or the tag given). SHA-512 checksums
  are verified where the project publishes them (CachyOS, DW-Proton,
  Wineland). Each download gets an "LSIW ..." twin in Steam.
- LSIW Control: a "Download Proton" menu on the Engine and Steam pages
  with every build.
- Twins of Proton 5.13, 5.0, 4.11, 4.2, 3.16 and 3.7, and of Proton Next
  while Steam offers or has it installed.

### Changed

- `[steam] legacy_versions` defaults to `true`: Proton 6.3 and older and
  Hotfix always get a twin.

## [1.3.0] - 2026-10-20

### Changed

- New default locations, all relative to your home folder (`$HOME`), so no
  user name is written anywhere:

  | Windows | Linux |
  |---|---|
  | `C:\windows` | `~/Windows` |
  | `C:\Program Files`, `C:\Program Files (x86)` | `~/Program Files`, `~/Program Files (x86)` |
  | `C:\ProgramData` | `~/ProgramData` |
  | `C:\users` | the folder holding your home folder (`/home`) |
  | `C:\users\<you>` | your home folder, so AppData is `~/AppData` |
  | `C:\users\Public`, `C:\users\Default` | `/home/Public`, `/home/Default` |

  The packages create `/home/Public` and `/home/Default` (tmpfiles.d),
  writable by every user like Public on Windows.
- Folders still at the old defaults under `~/.local/share/lsiw` are moved
  to the new places once, when nothing is there yet. Wine's system files
  come along, or `wineboot` recreates them.
- A folder set to exactly where its default puts it counts as the default
  (LSIW Control shows "default", not "custom"), and choosing the default
  location again removes the setting.

### Fixed

- Long paths in the Locations table no longer spill into the next column.

## [1.2.1] - 2026-10-20

### Fixed

- LSIW Control could not be made narrower than its widest page, and long
  Wine or Proton names could make it wider than the screen. Every page now
  scrolls, text wraps to the window, rows of buttons continue on a new line
  and the version lists shorten long names with "…". The window can be
  made as small as 480×320.
- Downloaded Wine builds are installed as `wine-VERSION-x86_64`.
  Kron4ek's archives are named `amd64`, which is the same processor
  family (x86-64, Intel and AMD); builds downloaded before keep working
  and are renamed, and a setting naming the old folder still finds them.

## [1.2.0] - 2026-10-20

### Steam

- Every Proton in Steam's list gets an LSIW twin: LSIW Proton Stable,
  LSIW Proton Experimental, LSIW Proton 11.0-2, 10.0-4, 9.0-4, 8.0-5,
  7.0-6, read from Steam's own app cache, whether that Proton is installed
  or not. The first time a twin of an uninstalled version is used, LSIW
  asks Steam to install it (and the Steam Linux Runtime it needs).
- LSIW Proton Stable follows the version Steam calls stable.
- Wine and Proton builds made for another processor are recognised: they
  get no Steam twin and are never picked automatically.

### Fixed

- `lsiw runners install ge` downloaded the aarch64 (ARM) build of
  GE-Proton when a release had builds for several processors; it now picks
  the build for this processor.

## [1.1.0] - 2026-10-20

### Wine, Proton and Steam

- Choice of Wine version: Wine from `$PATH`, WineHQ, Lutris and Bottles
  runners, custom paths, and Wine builds LSIW downloads
  (`lsiw runners install wine`). Every version shares LSIW's Wine prefix,
  with the folders chosen in LSIW Control.
- New `proton` engine and choice of Proton version: Steam's Proton
  versions, GE-Proton and other compatibility tools, and GE-Proton
  downloaded by LSIW (`lsiw runners install ge`, SHA-512 checked). Proton
  prefixes link Documents, Saved Games, AppData, Desktop, Downloads, Music,
  Pictures and Videos to LSIW's folders, and show the Linux filesystem on
  `C:\`.
- `lsiw run -e wine:VERSION` / `-e proton:VERSION`, `lsiw config proton`,
  `lsiw runners`.
- Steam integration (`lsiw steam add|remove`): Steam lists
  "LSIW Proton <version>" and "LSIW GE-Proton…" compatibility tools next to
  its own Proton versions; they run that Proton with LSIW's folders, inside
  the Steam Linux Runtime it asks for.
- LSIW Control: Wine and Proton version pickers and download buttons on the
  Engine page, and a new *Steam integration* page.
- `CSIDL_SAVED_GAMES` (FOLDERID_SavedGames) is a relocatable folder.
- `install.sh`: installs the release on Fedora (checksums, dnf, a rebuild
  from the source RPM on older Fedora, optional Steam integration).

### Fixed

- The RPM applies the binfmt_misc rule on installation also when built
  where `%binfmt_apply` is not defined (package release 1.1.0-2).

## [1.0.0] - 2026-10-20

First release.

### Native engine

- PE32+ (x86-64) loader: preferred-base mapping with relocation fallback,
  imports from LSIW's own DLLs or from PE DLLs next to the program,
  implicit TLS and TLS callbacks, `DllMain`, resources.
- Unimplemented imports bound to stubs that stop the program with a clear
  message; `lsiw check` lists them without running the program.
- Thread and process environment blocks reached through the GS segment.
- kernel32/kernelbase: files, directories, the console, volumes, file
  mappings, heaps and virtual memory, processes, threads, synchronisation,
  fibers, TLS/FLS, environment, modules, resources, time, locale and code
  pages (UTF-8 as the ANSI code page).
- ntdll: the native API and `Rtl` helpers programs and runtimes call
  directly.
- Windows x64 structured exception handling: table-based unwinding,
  `RaiseException`, `RtlUnwindEx`, vectored handlers,
  `__C_specific_handler`, CPU faults as Windows exceptions.
- C runtime for msvcrt, msvcr70–msvcr120, ucrtbase and the
  `api-ms-win-crt-*` sets: startup, stdio, strings (UTF-16 wide strings),
  math, time, locale.
- vcruntime140: MSVC C++ exceptions (`__CxxFrameHandler3`/`4`) and RTTI.
- ntdll NT file layer (`NtCreateFile`, `NtQueryInformationFile`,
  `NtDeviceIoControlFile`, ...) and object layer (registry, sections,
  events, timers, atoms).
- Winsock 2 with overlapped I/O and I/O completion ports, named pipes,
  thread pools, the shell32/shlwapi path and folder functions, ole32 COM
  basics, advapi32 registry and security queries.
- KUSER_SHARED_DATA mapped at 0x7FFE0000 for programs (such as Go
  programs) that read the clock from it.
- Child processes: exact command lines, exit codes, the Windows
  environment and current directory are passed to Windows children; only
  the standard handles are inherited.
- `LSIW_APITRACE=FILE` logs every call a program makes into LSIW's DLLs.

### The C: drive and Windows folders

- The whole Linux file system is the C: drive; other drive letters are
  folded onto C:, and disks appear as folders.
- Every USMT/CSIDL folder variable can be moved to any Linux folder, from
  `lsiw config`, LSIW Control or `/etc/lsiw/lsiw.conf`.

### Tools and integration

- `lsiw` command: `run`, `check`, `config`, `path`, `sync`, `info`,
  `register`, `unregister`, `wine-setup`.
- Automatic engine choice with Wine as the fallback, using an LSIW-managed
  Wine prefix that shows the same C: drive and folders.
- LSIW Control, a GTK 4 settings app.
- `.exe` files open with a double-click (desktop entry) and run directly
  from a shell (binfmt_misc rule).
- Fedora RPM packaging, GitHub Actions build on Fedora.

[Unreleased]: https://github.com/SoulVoid0/LSIW/compare/v1.7.1...HEAD
[1.7.1]: https://github.com/SoulVoid0/LSIW/releases/tag/v1.7.1
