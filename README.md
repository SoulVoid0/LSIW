# LSIW: Linux Subsystem for Improved Wine

LSIW runs Windows x86-64 programs on Linux with **its own engine**: its
own loader for Windows executables and DLLs, and its own implementations of
the Windows system libraries (kernel32, ntdll, the Microsoft C runtimes and
more), written in C and called by the Windows program directly through the
Microsoft x64 calling convention. Wine is used **only as a fallback**, for
programs LSIW's engine cannot run yet.

LSIW also changes how Windows programs see your computer:

* **One C: drive, and the drives you choose.** `C:` is one folder you
  choose (`~/.cdrive` by default), the same for LSIW's engine, Wine, Proton
  and Steam games, and it also shows the whole Linux file system:
  `C:\home\me\notes.txt` is `/home/me/notes.txt`; a USB stick mounted at
  `/run/media/me/USB` is `C:\run\media\me\USB`. Any other folder can be a
  drive of its own (`lsiw drives add D ~/Games` makes it `D:\`); there is
  no `Z:`.
* **Updates.** LSIW looks for a new release once a day (`lsiw update auto
  off` stops it) and LSIW Control's *Updates* page installs it, verified
  against the release's checksums.
* **One prefix.** Every program and game uses one shared prefix, under Wine
  and under Proton, Steam games included, unless you give it its own.
* **A library.** Add your Windows programs and games and give each its own
  engine (LSIW's, any Wine, any Proton), DXVK/VKD3D, arguments, environment,
  MangoHud and a desktop shortcut. The default engine can be a chain: a
  first choice and up to three fallbacks. LSIW keeps track of how long
  each one runs (play time, launches, last played), and LSIW Control shows
  the library as a *Grid*, a *List* or *Details*.
* **Your folders.** Every Windows folder (Documents, AppData, Program Files,
  the Windows folder, Temp and every other variable Microsoft's migration
  tool [USMT recognises](https://learn.microsoft.com/en-us/windows/deployment/usmt/usmt-recognized-environment-variables))
  can be placed wherever you want on Linux, with `lsiw config`, the
  LSIW Control app or the configuration file.
* **Windows programs behave like Linux programs.** `./setup.exe` runs from
  a shell, and `.exe` files open with a double-click in your file manager.

LSIW is free software under the GNU Affero General Public License v3 or
later. It is packaged as .rpm, .deb and AppImage, so it installs on any
x86-64 Linux distribution.

## Status: what runs, what does not

LSIW 1.0 is a young engine. Be precise about what to expect:

**Runs on LSIW's own engine**

* 64-bit **console** programs (command-line tools, text-mode programs,
  build tools, interpreters) built with **MinGW-w64 or Microsoft Visual
  C++**, that use kernel32/kernelbase, ntdll, msvcrt, ucrtbase /
  the `api-ms-win-crt-*` DLLs, vcruntime140 and the other DLLs listed in
  [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).
* C and C++ (including C++ exceptions and RTTI), threads, files and
  directories, the console, pipes and child processes, memory mapping,
  structured exception handling, and DLLs that ship with the program.
* Tested with upstream Windows builds of Git for Windows (MinGit), Python,
  Node.js, CMake + Ninja, ripgrep, fd, jq, zstd, bat, delta, xsv, uutils
  coreutils and Go programs (GitHub CLI, fzf, lazygit); the list and what
  was run is in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md#programs-tested).

`lsiw check program.exe` tells you, without running it, whether a program
can run on LSIW's engine, and lists every function it would need that LSIW
does not implement yet.

**Goes to the Wine fallback** (if Wine is installed)

* **Windowed (GUI) programs**, which need user32/gdi32 windowing, drawing
  and input that LSIW's engine does not provide yet.
* **32-bit programs**: LSIW's engine runs 64-bit code only.
* **Installers** (`.msi` packages) and **batch files** (`.bat`, `.cmd`).
* Any 64-bit program that imports a function LSIW does not implement.

**Known gaps in the native engine**

* No Windows certificate store or Schannel TLS: programs that bring their
  own TLS (Go, Python, Node.js, Git with `http.sslBackend=openssl`) make
  HTTPS connections; programs that rely on Schannel do not.
* MSYS2/Cygwin programs (`sh.exe`, `bash.exe` from Git for Windows) do not
  run, so Git features that start a shell (hooks, local-path clones) fail.

**Not supported**

* **.NET programs** need the .NET runtime. Wine with Mono runs some; many
  .NET tools also have native Linux builds.
* **DirectX games and other 3D/audio-heavy software**: LSIW's engine has no
  graphics or audio stack. Wine and Proton are the right tools.
* **Kernel-mode drivers** (`.sys` files: hardware drivers, anti-cheat and
  copy-protection drivers, virtual devices). A Windows driver is a part of
  the Windows kernel: it is loaded into kernel memory and talks to the
  Windows kernel's own internal interfaces (the I/O manager, IRPs, Plug and
  Play, the Windows Driver Model). Linux has a different kernel with none
  of those interfaces, and a program like LSIW runs in user space, where
  it cannot load code into any kernel. No compatibility layer can run
  Windows drivers on Linux; hardware works on Linux through Linux drivers.
  An installer that bundles a driver may finish under Wine, but the driver
  itself never runs.

## Installing

Every release on the [releases page](https://github.com/SoulVoid0/LSIW/releases)
has the same LSIW in three formats, so it installs on every x86-64
distribution:

| Format | For | Files |
|---|---|---|
| `.rpm` | Fedora, RHEL/Alma/Rocky, openSUSE | `lsiw-VERSION-1.x86_64.rpm`, `lsiw-control-VERSION-1.x86_64.rpm`, source RPM |
| `.deb` | Debian, Ubuntu, Linux Mint, Pop!_OS, elementary | `lsiw_VERSION-1_amd64.deb`, `lsiw-control_VERSION-1_amd64.deb` |
| `.AppImage` | any distribution (Arch, Manjaro, Gentoo, Void, NixOS, ...) | `LSIW-VERSION-x86_64.AppImage` |

`lsiw` is the command and the `.exe` integration; `lsiw-control` is the
settings app (GTK 4). `SHA256SUMS` lists the checksum of every file.

Every release also has installers that do everything for you:

| Installer | Size | For |
|---|---|---|
| `install.sh` = `lsiw-VERSION-online-installer.sh` | ~20 KB | any distribution: picks rpm, deb or AppImage and downloads only what it needs |
| `lsiw-VERSION-online-installer-rpm.sh` / `-deb.sh` / `-appimage.sh` | ~20 KB | the same, with the format chosen |
| `lsiw-VERSION-offline-installer.run` | all formats | any distribution, no network needed |
| `lsiw-VERSION-offline-installer-rpm.run` / `-deb.run` / `-appimage.run` | one format | Fedora & co. / Debian & co. / any distribution, no network needed |

```sh
sh lsiw-1.10.1-offline-installer-deb.run           # install (same options as install.sh)
sh lsiw-1.10.1-offline-installer-deb.run --verify  # only check the file
sh lsiw-1.10.1-offline-installer.run --extract dir # only unpack the packages
```

Offline installers check their payload and every package against the
release's checksums and never download anything; the libraries the
packages depend on (GTK 4, ...) must already be installed (they are on
most desktops). Wine and GE-Proton are not included.

### The installer (any distribution)

```sh
curl -fLO https://github.com/SoulVoid0/LSIW/releases/latest/download/install.sh
sh install.sh            # asks for your password through sudo
```

`install.sh` picks the format for your system (dnf or zypper: rpm; apt:
deb; anything else: the AppImage), downloads the files it needs from the
release (or uses the ones next to it), checks them against `SHA256SUMS`,
installs `lsiw` and LSIW Control with Wine, and offers to add LSIW Proton
to Steam. On a Fedora older than the prebuilt RPMs need it rebuilds them
from the source RPM. Options: `--format=rpm|deb|appimage`, `--steam`,
`--ge` (download GE-Proton), `--no-wine`, `--no-control`, `--yes`,
`--version=VERSION|latest`, `--offline`, `--uninstall`;
`sh install.sh --help` lists them. Running it again is safe.

### Updating

```sh
lsiw update               # is there a newer release?
lsiw update install       # download, verify and install it
lsiw update auto off      # no automatic checks (on by default)
```

The automatic check runs at most once a day, in the background, after a
command typed in a terminal and when LSIW Control starts; it only asks
GitHub where `releases/latest` leads. When a newer release exists you see
one line (or a banner in LSIW Control); nothing is installed unless you
ask. `lsiw update install` downloads the new packages for the way LSIW is
installed (rpm, deb or AppImage), checks each one against the release's
`SHA256SUMS`, and installs them with sudo (pkexec from LSIW Control); an
AppImage in a folder you can write to is replaced in place. The checksums
protect against damaged or altered downloads; they come from the same
GitHub release, so they are not a signature.

### Fedora, RHEL, openSUSE (.rpm)

```sh
sudo dnf install ./lsiw-1.10.1-1.x86_64.rpm ./lsiw-control-1.10.1-1.x86_64.rpm
sudo dnf install wine        # optional: the fallback engine
# openSUSE: sudo zypper install --allow-unsigned-rpm ./lsiw-*.rpm
```

The prebuilt RPMs need glibc 2.38 (Fedora 39 or newer). On older systems
rebuild them: `rpmbuild --rebuild lsiw-1.10.1-1.src.rpm` (or let
`install.sh` do it).

### Debian, Ubuntu, Mint (.deb)

```sh
sudo apt install ./lsiw_1.10.1-1_amd64.deb ./lsiw-control_1.10.1-1_amd64.deb
```

`lsiw` runs on Debian 11, Ubuntu 20.04 and newer. LSIW Control needs GTK
4.12 (Debian 13, Ubuntu 24.04, Mint 22 and newer); on older releases
install `lsiw_*.deb` alone and use the command line. Remove with
`sudo apt remove lsiw-control lsiw`.

### Any distribution (.AppImage)

```sh
chmod +x LSIW-1.10.1-x86_64.AppImage
./LSIW-1.10.1-x86_64.AppImage                      # opens LSIW Control
./LSIW-1.10.1-x86_64.AppImage run setup.exe        # any lsiw command
ln -s "$PWD/LSIW-1.10.1-x86_64.AppImage" ~/.local/bin/lsiw   # a normal `lsiw` command
```

The AppImage holds `lsiw`, LSIW Control and the GTK 4 libraries, so it
needs nothing installed (FUSE is optional: set
`APPIMAGE_EXTRACT_AND_RUN=1` without it). Started with arguments, or
through a link named `lsiw`, it is the `lsiw` command; without arguments,
or through a link named `lsiw-control`, it opens LSIW Control.
`lsiw register --user` from the AppImage sets up the `.exe`
double-click with the AppImage's path. Starting `.exe` files directly from
a shell (`./setup.exe`, binfmt_misc) needs root and a fixed path, so it
comes only with the rpm and deb packages. The `lsiw` command in the
AppImage runs on glibc 2.30 or newer; LSIW Control needs glibc 2.38.

### From source

```sh
git clone https://github.com/SoulVoid0/LSIW.git
cd LSIW
make                         # needs gcc, make, and GTK 4 headers for LSIW Control
sudo make install            # into /usr/local; PREFIX=/usr for /usr
```

Or build the packages yourself: `make rpm` (Fedora, needs rpm-build and
the spec's build dependencies), `make deb` (needs dpkg-dev) or
`make appimage` (downloads linuxdeploy). See
[docs/BUILDING.md](docs/BUILDING.md) for package names per distribution,
the test suite and all build options.

## Quick start

```sh
lsiw run ~/Downloads/tool.exe --help   # run a program
lsiw ~/Downloads/tool.exe --help       # the same, shorter
chmod +x tool.exe && ./tool.exe        # straight from the shell (binfmt_misc)
lsiw check tool.exe                    # can LSIW's own engine run it?
lsiw config                            # where the Windows folders are
lsiw programs add ~/Games/Game/game.exe engine=proton dxvk=on
lsiw programs run game                 # run it with its own settings
lsiw lwlnk create game                 # a desktop shortcut (.lwlnk) with its icon
lsiw cdrive set /mnt/big/c_drive       # move the C: drive
lsiw steam default --all               # every Steam game in the shared prefix
lsiw-control                           # the settings app
```

In a file manager, double-click an `.exe` (or right-click, *Open With*,
*LSIW*). If Wine is installed too and still opens `.exe` files, run
`lsiw register --user` to make LSIW the default. A console program started
this way has no terminal window to write to: run it from a terminal to
see its output.

## Using the `lsiw` command

`lsiw help` and `lsiw help COMMAND` show the built-in help.

### `lsiw run`: run a Windows program

```sh
lsiw run setup.exe
lsiw run ~/Programs/7z.exe a archive.7z ~/Documents/report.odt
lsiw run 'C:\home\me\Programs\tool.exe'          # Windows paths work too
lsiw run '%APPDATA%\SomeApp\app.exe'             # with variables
lsiw run tool                                    # ".exe" may be left out
lsiw run --engine=native tool.exe                # force LSIW's own engine
lsiw run -e wine installer.msi                   # force Wine
lsiw run -e wine:wine-9.21-staging-x86_64 app.exe # a specific Wine version
lsiw run -e proton:GE-Proton9-20 game.exe        # Proton (GE or Valve's)
lsiw --verbose run tool.exe                      # show what LSIW decides and loads
```

Arguments are passed to the program unchanged, except that an argument
that is an existing absolute Linux path (`/home/me/report.odt`) is passed
as the Windows path for it (`C:\home\me\report.odt`). The exit status is
the program's own exit code; `126` means LSIW could not run it, `127` that
the program (or Wine, when it was needed) was not found.

### `lsiw check`: can LSIW's engine run this?

```sh
lsiw check tool.exe
```

Prints the program's type, every function it imports (from itself and
from the DLLs it loads) that LSIW does not implement yet, and the engine
`lsiw run` would choose. Exit status `0`: LSIW's engine can run it
entirely; `1`: it cannot (missing functions, 32-bit program, ...); `2`: not
found or not a program.

### `lsiw config`: Windows folders and the engine

```sh
lsiw config                                   # same as 'lsiw config list'
lsiw config list --all                        # include variables USMT does not document
lsiw config get APPDATA                       # details of one variable
lsiw config get --windows CSIDL_PERSONAL      # only the Windows path (for scripts)
lsiw config get --linux PROGRAMFILES          # only the Linux path
lsiw config set CSIDL_PERSONAL ~/Documents/Windows
lsiw config set APPDATA /data/me/AppData/Roaming
lsiw config set PROGRAMFILES '/run/media/me/Games/Program Files'
lsiw config set TEMP 'C:\tmp\windows-temp'    # Windows paths are converted
lsiw config reset APPDATA                     # back to the default
lsiw config reset --all
lsiw config engine                            # show the default engine
lsiw config engine native                     # auto | native | wine | proton
lsiw config wine /opt/wine-staging/bin/wine   # Wine binary for the fallback
lsiw config wine GE-Proton9-20                # or a version ID from 'lsiw runners'
lsiw config wine auto                         # search $PATH for wine, wine64
lsiw config proton GE-Proton9-20              # Proton version of the proton engine
```

Variable names are the ones Microsoft documents (`CSIDL_PERSONAL`,
`APPDATA`, `PROGRAMFILES`, `WINDIR`, ...), case-insensitive, with or
without `%`. Aliases set the variable they stand for (`APPDATA` and
`CSIDL_APPDATA` are one folder). A few folders always sit inside another
(`SYSTEM32` in `WINDIR`, `HOMEPATH` in the profile) and move with it.
`set` creates the folder if needed; nothing is copied from the old place.

### `lsiw path`: convert paths

```sh
lsiw path win /etc/hosts                 # C:\etc\hosts
lsiw path linux 'D:\data\file.txt'       # the D: folder's data/file.txt, or /data/file.txt without a D:
lsiw path linux '%USERPROFILE%\Desktop'  # the Linux folder behind it
lsiw path expand '%APPDATA%\App'         # C:\users\me\AppData\Roaming\App
```

### `lsiw sync`: rebuild the C: drive

```sh
lsiw sync
lsiw sync --quiet
```

Creates every configured folder that does not exist yet and rebuilds the
links of the C: drive folder (one per top-level Linux directory, plus
`windows`, `Program Files`, `ProgramData`). `lsiw run` does this
automatically.

### `lsiw cdrive`: where C: is

```sh
lsiw cdrive                                  # show it (default ~/.cdrive)
lsiw cdrive set /mnt/games/c_drive           # move it
lsiw cdrive set /mnt/games/c_drive --with-folders   # Windows, Program Files and ProgramData too
lsiw cdrive reset                            # back to ~/.cdrive
```

Every engine sees this folder as `C:\`: LSIW's own engine, Wine
(`dosdevices/c:`) and Proton (the shared prefix's `drive_c` is a link to
it). Moving it moves what is in it; across disks the files are copied and
the original is kept as `<old>.old` until you delete it. The old place
becomes a link to the new one. The LSIW Control page *Drives* does the
same with *Change location…*.

### `lsiw drives`: folders as D:, E:, ...

```sh
lsiw drives                         # C: and the other drives
lsiw drives add D ~/Games           # ~/Games is D:\
lsiw drives add E /run/media/me/USB # a USB stick as E:
lsiw drives remove D                # D: is gone; the folder stays
```

Each drive is `[drives] d = FOLDER` in the configuration (`${HOME}` and the
other tokens work). LSIW's engine sees the folder as `D:\`
(`GetLogicalDrives`, `GetDriveType`, volume label = the folder's name),
Wine and Proton get `dosdevices/d:` in their prefix, and Steam's runtime
container is told about the folder. A Linux path inside a drive folder is
shown to programs as that drive (`~/Games/x` is `D:\x`). The *Drives* page
of LSIW Control lists, adds, changes and removes them.

### `lsiw prefix`: the shared prefix

```sh
lsiw prefix
```

Every program and game uses one prefix (`~/.local/share/lsiw/shared`, the
Wine prefix being its `pfx` folder), whether it runs with Wine or with
Proton, Steam games included. A program of the library can have its own
(`lsiw programs set ID prefix=own`), and so can a Steam game (`lsiw steam
own APPID`). `lsiw prefix` lists them, and prefixes of earlier LSIW
versions that are kept but no longer used.

### `lsiw programs`: the program library

```sh
lsiw programs                                # the library
lsiw programs add ~/Games/X/x.exe --name "Game X" engine=proton:GE-Proton10-25 dxvk=on
lsiw programs set game-x args="-windowed" env:DXVK_HUD=fps mangohud=on
lsiw programs set game-x prefix=own          # a prefix of its own
lsiw programs run game-x                     # start it with its settings
lsiw programs shortcut game-x --desktop      # menu and desktop launcher (.desktop)
lsiw lwlnk create game-x                     # LSIW shortcut on the desktop (.lwlnk)
lsiw programs scan --add                     # add what installers put in the Start menu
lsiw programs playtime                       # play time, launches, last played
lsiw programs playtime game-x --reset        # forget it
lsiw programs remove game-x
```

Each program has its own engine (`default`, `auto`, `native`, `wine`,
`wine:VERSION`, `proton`, `proton:VERSION` or `custom`), prefix, DXVK and
VKD3D (`on`, `off`, `default`), arguments, working folder, environment
variables, MangoHud and debug log. Under Wine, `dxvk=on` uses the DXVK of
an installed Proton. The Programs page of LSIW Control shows the same
library, with each program's icon, *Launch*, *Settings…*, *Find
installed*, *Desktop shortcut* (on every program) and *Add to applications
menu*. `icon=FILE` (or *Icon* in *Settings…*) gives a program another icon:
an image, or an `.exe`/`.dll` to take it from; by default it is the icon
inside the program.

**Layouts.** The *Layout* buttons above the library switch between *Grid*
(big icons with the name and play time), *List* (one row per program with
its settings and a *Desktop shortcut* button) and *Details* (a table with
name, play time, last played, launches, last session, engine and program;
click a header to sort, drag to resize or reorder). *Sort by* orders them
by name, most played, recently played or most launched. LSIW Control
remembers both (`[control] programs_layout` and `programs_sort`).

**Several programs at once.** Programs and games run side by side, also
the same one more than once: *Launch* and *Run* stay available while
others run, and a program started while another runs in the same prefix
joins it instead of waiting for it to end (with the Proton or Wine version
that runs the prefix, since one prefix runs one Wine version at a time).
While several run, *Stop* is a menu with an entry for each, including
library programs started from an LSIW shortcut or the applications menu,
and *Stop all*. Stopping one ends all of its processes but never the
wineserver and Wine services the others share.

**Each program on its own in the taskbar and System Monitor.** Every
program gets its own window class under Proton (`steam_app_N`, from an
app ID of its own), a desktop entry with its name and icon (its menu
shortcut, or a hidden `lsiw.ID.desktop`) and, with systemd, a user scope
of its own (`app-lsiw-<desktop ID>-<random>.scope`), so taskbars give each
program its own entry and icon and System Monitor lists each one under
Applications with its own memory and CPU use. `LSIW_NO_IDENTITY=1` turns
this off.

**Play time.** Every start of a library program is counted: `lsiw
programs run`, LSIW shortcuts (.lwlnk), *Launch* in LSIW Control and
applications-menu shortcuts. `lsiw` stays behind as a small watcher until
the program ends and records the total play time, the number of launches,
the first and last time it was played, how long the last session lasted
and how it ended (`exit 0`, `signal 11`, ...). The time is saved every 30
seconds while the program runs, so a crash of the program, of `lsiw` or a
power loss loses at most that much; such a session is shown as *lost*.
Time the computer sleeps does not count, and two copies of the same
program running at once count once. LSIW Control updates the numbers while
it is open and shows what is running now. The records are in
`~/.local/share/lsiw/playtime/`; `[programs] track_playtime = false` or
`LSIW_NO_PLAYTIME=1` turns tracking off. Programs started with `lsiw run`
or outside LSIW (for example by Steam itself) are not counted, and
launchers that start the game and exit at once count only their own time.

### `lsiw launch` / `lsiw lwlnk`: LSIW shortcuts (.lwlnk)

```sh
lsiw lwlnk create game-x                     # ~/Desktop/Game X.lwlnk (the XDG desktop folder)
lsiw lwlnk create game-x ~/Games --icon ~/Pictures/x.png   # elsewhere, another icon
lsiw launch ~/Desktop/"Game X.lwlnk"         # or double-click it; also 'lsiw run FILE.lwlnk'
lsiw lwlnk ~/Desktop/"Game X.lwlnk"          # what it holds
```

An LSIW shortcut starts a program of the library with **the settings it
has there**: engine and Wine or Proton version, prefix, DXVK, VKD3D,
arguments, start folder, environment, MangoHud and debug log. Change them
later and the shortcut follows. The *Desktop shortcut* button on the
Programs page of LSIW Control makes one on the desktop.

The file is text (`[LSIW Shortcut]`, MIME type
`application/x-lsiw-shortcut`). It also keeps a copy of the settings, used
when the program is no longer in the library or the shortcut is copied to
another computer, and **its icon**: the icon inside the program (`.exe`,
`.dll`; the largest image up to 256x256), or the one chosen (`.png`, `.ico`;
LSIW Control converts `.svg`, `.jpg` and other images to PNG), embedded as a
PNG. File managers show it through LSIW's thumbnailer
(`/usr/share/thumbnailers/lsiw-lwlnk.thumbnailer`): GNOME Files, Nemo,
Caja, Thunar, PCManFM, and Dolphin with KDE Frameworks 6.6 or later (turn
on the LSIW shortcut previews under *Configure Dolphin › Interface ›
Previews* if they are off). GNOME Files, Nemo and Caja also get the icon
right away as the file's custom icon. Where thumbnails are off, the file
shows the LSIW shortcut icon. Making a shortcut again brings its icon up
to date; one of the same name that is not a shortcut to this program is
never replaced (`Game X (2).lwlnk`).

### `lsiw info`: what is set up

```sh
lsiw info
```

Shows the version, the configuration file, LSIW's data folder, the default
engine, whether Wine is installed (and which version), the state of the
Wine prefix, the binfmt_misc handler and the desktop integration.

### `lsiw register` / `lsiw unregister`: `.exe` integration

```sh
lsiw register                # = --user: open .exe/.msi/.lnk/.lwlnk files with LSIW (default app)
sudo lsiw register --system  # run ./program.exe directly (binfmt_misc)
lsiw unregister --user
sudo lsiw unregister --system
```

The packages and `make install` already install both; these commands are
for setting up a copy that was not installed (or for making LSIW the
default application again after installing Wine). For a copy that was not
installed, `--user` also adds the `.lwlnk` file type and its thumbnailer
under `~/.local/share`.

### `lsiw lnk`: Windows shortcuts

```sh
lsiw run "Blender 5.2.lnk"            # or double-click it: starts what it points at
lsiw lnk "Blender 5.2.lnk"            # target, arguments, start folder, icon
lsiw lnk create Notepad.lnk ~/"Program Files/Notepad++/notepad++.exe" --workdir ~/Documents
lsiw lnk set Notepad.lnk              # choose another program in a file chooser
```

A shortcut starts its target in the shortcut's start folder with its
arguments, the way Windows does. Shortcuts LSIW makes hold Windows paths
(`C:\home\me\Program Files\...`), so they also work in Wine and on
Windows. In KDE's Create New menu (Dolphin and the desktop), **Link to
Windows Application...** makes a new shortcut; opening it the first time
asks which program it starts. On GNOME, Cinnamon, MATE and Xfce,
`lsiw register` adds the same template to the Templates folder (New
Document).

### `lsiw wine-setup`: the shared prefix

```sh
lsiw wine-setup
lsiw wine-setup --quiet
```

Creates or refreshes the shared prefix (`~/.local/share/lsiw/shared/pfx`)
so that programs under Wine see the same single `C:` drive and the same
folders as under LSIW's engine. It happens automatically the first time a
program needs Wine and whenever a folder moves. The Wine prefix of LSIW
1.5 and earlier (`~/.local/share/lsiw/prefix`) is moved into it once.

### `lsiw runners`: Wine and Proton versions

```sh
lsiw runners                     # every Wine and Proton found, with its ID
lsiw runners install ge          # download the latest GE-Proton (checksum verified)
lsiw runners install cachyos     # also: dwproton, rtsp, tkg, em, wineland
lsiw runners install wine 9.21   # download a Wine build (--staging for Staging)
lsiw runners remove GE-Proton9-20
```

### `lsiw steam`: LSIW Proton in Steam

```sh
lsiw steam add      # Steam lists "LSIW Proton Stable", "LSIW Proton 11.0-2", "LSIW GE-Proton11-11", ...
lsiw steam remove
lsiw steam          # status
```

Every Proton in Steam's list gets an LSIW twin, including Valve's versions
that are not installed yet (Steam installs one the first time its twin is
used). Each twin runs the Proton of the same version in LSIW's shared
prefix: C: is your C: drive, and the game's Documents, Saved Games,
AppData, Desktop, Downloads, Music, Pictures and Videos are the folders
chosen in LSIW Control. Restart Steam after adding or removing. The LSIW
Control tab *Steam integration* does the same.

```sh
lsiw steam default --all    # (Steam closed) LSIW Proton for every game, new and existing
lsiw steam games            # games that ran with Proton, and their prefix
lsiw steam own 1245620      # this game keeps a prefix of its own
```

A game that already had its own prefix keeps it untouched as a backup; the
first time it runs in the shared prefix, the files of that prefix's user
folders (saves, settings) are copied into LSIW's folders where missing.
Details, and what Flatpak Steam cannot do, are in
[docs/PROTON-STEAM.md](docs/PROTON-STEAM.md).

### Global options

```sh
lsiw --version
lsiw --engine=wine tool.exe     # -e, --engine works before the command too
lsiw --verbose check tool.exe   # -v
LSIW_VERBOSE=1 lsiw tool.exe    # the same as --verbose, through the environment
```

## The C: drive

`C:\` is one folder, `~/.cdrive` unless you choose another (`lsiw cdrive
set`, or the *Drives* page). It holds a link to every top-level Linux
folder and to the Windows folders below; anything a program creates right
under `C:\` (`C:\Games`, say) is a real folder in it.

Until 1.7.0 the default was `~/c_drive`. LSIW moves an existing
`~/c_drive` to `~/.cdrive` once and leaves `~/c_drive` as a link to it, so
paths that still name it keep working; nothing is deleted, and a C: drive
you moved elsewhere stays where it is.

| Windows program sees                 | Linux                                            |
|--------------------------------------|--------------------------------------------------|
| `C:\`                                | the C: drive folder (default `~/.cdrive`)        |
| `C:\home\me\notes.txt`               | `/home/me/notes.txt`                             |
| `C:\run\media\me\USB\photo.jpg`      | `/run/media/me/USB/photo.jpg` (a USB stick)      |
| `C:\mnt\data`                        | `/mnt/data` (a second disk)                      |
| `D:\setup.exe`                       | the D: folder's `setup.exe` (`lsiw drives add D FOLDER`) |
| `Q:\x` (a letter that is no drive)   | `C:\x`: such a letter means C:                    |
| `C:\windows`                         | `WINDIR` (default `~/Windows`)                    |
| `C:\Program Files`                   | `PROGRAMFILES` (default `~/Program Files`)        |
| `C:\Program Files (x86)`             | `PROGRAMFILES(X86)` (default `~/Program Files (x86)`) |
| `C:\ProgramData`                     | `PROGRAMDATA` (default `~/ProgramData`)           |
| `C:\users`                           | `PROFILESFOLDER` (default: `users` in the C: drive) |
| `C:\users\me`, `C:\users\steamuser`  | `USERPROFILE` (default: your home folder, `~`; Proton's user is you too) |
| `C:\users\me\AppData\Roaming`        | `APPDATA` (default `~/AppData/Roaming`)           |
| `C:\users\me\Documents`              | `CSIDL_PERSONAL` (default: your XDG Documents folder) |
| `C:\users\Public`, `C:\users\Default` | `/home/Public`, `/home/Default` (created by the package, writable by every user) |
| `\\server\share\file`                | the GVFS mount of that share (`/run/user/UID/gvfs/smb-share:...`) |

`C:\windows`, `C:\Program Files`, `C:\Program Files (x86)`,
`C:\ProgramData` and `C:\users` always appear under those names, wherever
you put them on Linux. File names are matched case-insensitively, as
Windows programs expect. The Recycle Bin is the Linux trash.

Until 1.3.0 these folders were under `~/.local/share/lsiw` by default.
LSIW moves them to the new places the first time it runs, when nothing is
there yet; folders you set yourself stay where you put them.

## Configuration file

LSIW reads `/etc/lsiw/lsiw.conf` (for every user), then
`~/.config/lsiw/lsiw.conf` (`$XDG_CONFIG_HOME/lsiw/lsiw.conf`), whose
values win. `lsiw config` and LSIW Control write the user file; the system
file is edited by hand. The installed `/etc/lsiw/lsiw.conf` documents every
key and has examples, all commented out.

```ini
# Comments are whole lines starting with '#' or ';'.
# A comment cannot follow a value on the same line.

[engine]
default = auto
wine = /usr/bin/wine

[runtime]
translate_args = true

[variables]
CSIDL_PERSONAL = ${HOME}/Documents/Windows
APPDATA = ${HOME}/.local/share/lsiw/AppData/Roaming
PROGRAMFILES = /run/media/${USER}/Games/Program Files
COMPUTERNAME = WORKSTATION
```

| Section / key              | Values                         | Default | Meaning |
|----------------------------|--------------------------------|---------|---------|
| `[engine] default`         | `auto`, `native`, `wine`, `proton` | `auto`  | engine of `lsiw run` (see below) |
| `[engine] wine`            | version ID, command name or full path | `wine`, then `wine64` from `$PATH` | Wine version of the wine engine |
| `[engine] proton`          | version ID or Proton folder    | newest official Proton in Steam | Proton version of the proton engine |
| `[steam] full_c_drive`     | `true`, `false`                | `true`  | `C:\` in Proton prefixes shows the whole Linux filesystem |
| `[steam] root`             | folder                         | found automatically | Steam installation |
| `[drives] d` ... `[drives] z` | folder                    | none    | that folder is the drive `D:` ... `Z:` (`lsiw drives`) |
| `[update] check`           | `true`, `false`                | `true`  | look for a new release once a day (`lsiw update auto`) |
| `[runtime] translate_args` | `true`, `false`                | `true`  | pass existing absolute Linux paths in arguments as `C:\` paths |
| `[variables] NAME`         | Linux path (or value)          | per variable | where a Windows folder lives (`lsiw config list`) |

In `[variables]`, folder values may use the tokens the defaults are built
from: `${HOME}`, `${HOMES}` (the folder holding `$HOME`), `${USER}`, `${UID}`, `${HOSTNAME}`, `${DATA}`
(`~/.local/share/lsiw`), `${XDG:DOCUMENTS}` and the other XDG user
folders, and `${VAR:NAME}` (another variable's location). That makes one
line in `/etc/lsiw/lsiw.conf` right for every user. Write Linux paths in
the file; `lsiw config set` also accepts `C:\...`, `%VAR%\...` and `~/...`
and stores the Linux path. (Older files using `[runtime] backend` and
`[runtime] wine` still work.)

Environment variables:

| Variable        | Effect |
|-----------------|--------|
| `LSIW_CONFIG`   | use this configuration file instead of both files above |
| `LSIW_DATA_DIR` | keep LSIW's data somewhere other than `~/.local/share/lsiw` |
| `LSIW_VERBOSE`  | `1`: trace engine choice, loading and missing functions to standard error |
| `LSIW_APITRACE` | a file name (or `1` for standard error): log every call the program makes into LSIW's DLLs, one line per call with the thread ID |

## How the engine is chosen

With `[engine] default = auto` (the default), `lsiw run` looks at the
program first:

1. A **64-bit** program whose imports (and those of the DLLs it loads) are
   all implemented by LSIW runs on **LSIW's engine**.
2. Otherwise, if **Wine** is installed, the program runs on **Wine** and
   LSIW prints one line saying why.
3. If Wine is not installed, a 64-bit program with missing functions still
   runs on LSIW's engine, after a warning: many programs never call the
   functions they import for rare cases. If it does call one, it stops
   with a message naming the function, never with a silently wrong result.
4. 32-bit programs, `.msi` packages and batch files go to Wine; without
   Wine, LSIW explains what is needed (`sudo dnf install wine`).

`native`, `wine` and `proton` force one engine; `lsiw run --engine=...`
overrides the setting for one run. Programs running under Wine use LSIW's
own Wine prefix (shared by every Wine version), set up to show the same
`C:` drive and folders; under Proton they use LSIW's Proton prefix, whose
user folders are LSIW's folders. Auto never picks Proton by itself.

## Where LSIW keeps things

| Path                                  | Contents |
|---------------------------------------|----------|
| `/etc/lsiw/lsiw.conf`                 | system-wide settings |
| `~/.config/lsiw/lsiw.conf`            | your settings and moved folders |
| `~/Windows/`                          | `C:\windows` (Wine's system files live here too) |
| `~/Program Files/`, `~/Program Files (x86)/` | programs you install (`C:\Program Files`) |
| `~/ProgramData/`                      | shared program data (`C:\ProgramData`) |
| `~/AppData/`, `~/Saved Games/`, ...   | your Windows profile is your home folder (`C:\users\<you>`) |
| `~/.cdrive/`                          | the C: drive (`lsiw cdrive`; a hidden folder, `~/c_drive` before 1.7.1) |
| `~/.local/share/lsiw/shared/`         | the shared prefix of every program and game, Wine and Proton (`pfx/`) |
| `~/.local/share/lsiw/prefixes/ID/`    | the prefix of a program that has its own |
| `~/.local/share/applications/lsiw-ID.desktop` | menu shortcuts of library programs |
| `~/Desktop/NAME.lwlnk`                | LSIW shortcuts made with *Desktop shortcut* |
| `~/.local/share/lsiw/icons/ID.png`    | program icons taken from the programs (made again when needed) |
| `~/.local/share/lsiw/prefix`, `root`  | links left by LSIW 1.5 and earlier (to `shared/pfx` and the C: drive) |
| `~/.local/share/lsiw/proton-prefix/`  | the Proton prefix of LSIW 1.5 and earlier, kept but no longer used |
| `~/.local/share/lsiw/runners/`        | Wine and GE-Proton versions LSIW downloaded |
| `<Steam>/compatibilitytools.d/LSIW-*` | the LSIW Proton tools (`lsiw steam add`) |

## Uninstalling

Fedora packages:

```sh
sudo dnf remove lsiw lsiw-control
```

From source (same `PREFIX` as for installing):

```sh
sudo make uninstall
```

Both remove the binfmt_misc handler from the running kernel. If you used
`lsiw register` on a copy that was not installed, undo it first with
`lsiw unregister --user` and `sudo lsiw unregister --system`.

Your data stays. To delete it as well, including every Windows program you
installed and its settings:

```sh
rm -rf ~/.config/lsiw ~/.local/share/lsiw
```

Folders you moved elsewhere (Documents, for example) are not touched.

## Project layout

```
include/            lsiw.h (shared API), pe.h (PE file format)
src/common/         variable table, configuration, C: drive path rules
src/native/         the native engine: PE loader, TEB, exceptions, Win32 and C runtime DLLs
src/cli/            the lsiw command, Wine fallback, desktop/binfmt registration
src/gui/            lsiw-control, the GTK 4 settings app
tests/              Windows test programs (MinGW) and the test runner
data/               desktop entries, MIME data, thumbnailer, binfmt rule, default config, icons, AppStream
docs/               architecture, compatibility, building
lsiw.spec           Fedora package
```

## Documentation

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the engine works
* [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md): what each DLL implements
* [docs/PROTON-STEAM.md](docs/PROTON-STEAM.md): Wine versions, Proton, GE-Proton and Steam
* [docs/BUILDING.md](docs/BUILDING.md): dependencies, building, testing, packaging
* [CONTRIBUTING.md](CONTRIBUTING.md): how to help, and the rules for original code
* [CHANGELOG.md](CHANGELOG.md)

## License

GNU Affero General Public License, version 3 or (at your option) any
later version; see [LICENSE](LICENSE). LSIW's engine is original code written from
public documentation of Windows behaviour and file formats; it contains no
code from Wine or Microsoft. Wine, when installed, is a separate program
that LSIW starts.
