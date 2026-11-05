# Fedora package for LSIW. Build it from a source checkout with `make rpm`.
# SPDX-License-Identifier: AGPL-3.0-or-later

# seh.c and crt_core.c define entry points in top-level assembly that call
# back into C; LTO cannot see those references. The Makefile adds -fno-lto
# to the compile flags too; this keeps it off the link line as well.
%global _lto_cflags %{nil}

# Fallbacks for rpm setups without systemd-rpm-macros / redhat-rpm-config.
%{!?_binfmtdir:%global _binfmtdir %{_prefix}/lib/binfmt.d}
%{!?_tmpfilesdir:%global _tmpfilesdir %{_prefix}/lib/tmpfiles.d}
%{!?_metainfodir:%global _metainfodir %{_datadir}/metainfo}
%{!?build_cflags:%global build_cflags %{optflags}}
%{!?build_ldflags:%global build_ldflags %{nil}}

# The same make variables for %%build and %%install, so the install step
# finds everything already built with the distribution's flags.
%global lsiw_make_vars GUI=yes CC="%{__cc}" CFLAGS="%{build_cflags}" LDFLAGS="%{build_ldflags}" PREFIX=%{_prefix} BINDIR=%{_bindir} LIBEXECDIR=%{_libexecdir} DATADIR=%{_datadir} SYSCONFDIR=%{_sysconfdir} BINFMTDIR=%{_binfmtdir} TMPFILESDIR=%{_tmpfilesdir} METAINFODIR=%{_metainfodir}

Name:           lsiw
Version:        1.11.0
Release:        1%{?dist}
Summary:        Run Windows x86-64 programs on Linux with a native engine

License:        AGPL-3.0-or-later
URL:            https://github.com/SoulVoid0/LSIW
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

# The native engine runs Windows x86-64 machine code directly on the CPU.
ExclusiveArch:  x86_64

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  gtk4-devel
BuildRequires:  pkgconfig
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  systemd-rpm-macros

Requires:       hicolor-icon-theme
Requires(post): desktop-file-utils
Requires(postun): desktop-file-utils
# Wine is the fallback engine for programs the native engine cannot run
# (32-bit and windowed programs, installers, missing functions).
Recommends:     wine
# Error dialogs when a Windows program is started from the desktop.
Recommends:     zenity
# Finds the newest Wine and Proton releases ('lsiw runners install').
Recommends:     git-core
# Unpacks Proton-Tkg builds ('lsiw runners install tkg').
Recommends:     unzip

%description
LSIW (Linux Subsystem for Improved Wine) runs Windows x86-64 programs on
Linux. Its native engine is its own: a loader for Windows executables and
DLLs and its own implementations of kernel32, ntdll, the Microsoft C
runtimes and other system DLLs, called directly by Windows code through the
Microsoft x64 calling convention. Wine is used only as a fallback for
programs the native engine cannot run.

The whole Linux file system is the C: drive: /home/me is C:\home\me, and
other disks are folders on C: (C:\run\media\me\USB) rather than new drive
letters. Every Windows folder variable (APPDATA, Documents, Program Files,
WINDIR and the other USMT/CSIDL folders) can be moved to any Linux folder.

This package contains the lsiw command, the binfmt_misc rule that lets
.exe files run directly (./setup.exe), and the desktop integration that
opens Windows programs, Windows shortcuts and LSIW shortcuts (.lwlnk)
with a double-click and shows the icons of LSIW shortcuts.

%package control
Summary:        Settings app for LSIW
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires(post): desktop-file-utils
Requires(postun): desktop-file-utils

%description control
LSIW Control is the GTK 4 settings app for LSIW: choose where each Windows
folder lives on Linux, see how the C: drive maps onto the Linux file
system, pick the engine that runs Windows programs, and check whether a
program can run on LSIW's native engine.

%prep
%autosetup -p1

%build
%make_build %{lsiw_make_vars}

%install
%make_install %{lsiw_make_vars}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/lsiw.desktop
desktop-file-validate %{buildroot}%{_datadir}/applications/lsiw-control.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.lsiw.Control.metainfo.xml
# binfmt_misc starts lsiw through this relative symlink.
test "$(readlink -f %{buildroot}%{_libexecdir}/lsiw/lsiw-binfmt)" = "$(readlink -f %{buildroot}%{_bindir}/lsiw)"
grep -qx ':LSIW:M::MZ::%{_libexecdir}/lsiw/lsiw-binfmt:' %{buildroot}%{_binfmtdir}/zz-lsiw.conf

%post
# What %%binfmt_apply does on Fedora, spelled out so the package works the
# same when built where that macro is not defined.
if [ -x /usr/lib/systemd/systemd-binfmt ]; then
    /usr/lib/systemd/systemd-binfmt %{_binfmtdir}/zz-lsiw.conf >/dev/null 2>&1 || :
fi
update-desktop-database -q %{_datadir}/applications &>/dev/null || :
# C:\users\Public and C:\users\Default (/home/Public, /home/Default).
systemd-tmpfiles --create %{_tmpfilesdir}/lsiw.conf &>/dev/null ||
    { install -d -m 1777 /home/Public /home/Default &>/dev/null || :; }

%preun
# On removal (not upgrade) take the handler out of the running kernel too.
if [ $1 -eq 0 ] && [ -e /proc/sys/fs/binfmt_misc/LSIW ]; then
    echo -1 > /proc/sys/fs/binfmt_misc/LSIW 2>/dev/null || :
fi

%postun
update-desktop-database -q %{_datadir}/applications &>/dev/null || :

%post control
update-desktop-database -q %{_datadir}/applications &>/dev/null || :

%postun control
update-desktop-database -q %{_datadir}/applications &>/dev/null || :

%files
%license LICENSE
%doc README.md CHANGELOG.md CONTRIBUTING.md docs/ARCHITECTURE.md docs/BUILDING.md docs/COMPATIBILITY.md docs/PROTON-STEAM.md
%{_bindir}/lsiw
%dir %{_libexecdir}/lsiw
%{_libexecdir}/lsiw/lsiw-binfmt
%{_binfmtdir}/zz-lsiw.conf
%{_tmpfilesdir}/lsiw.conf
%dir %{_sysconfdir}/lsiw
%config(noreplace) %{_sysconfdir}/lsiw/lsiw.conf
%{_datadir}/applications/lsiw.desktop
%{_datadir}/mime/packages/lsiw.xml
%{_datadir}/templates/lsiw-windows-link.desktop
%{_datadir}/templates/.source/lsiw-windows-link.lnk
%{_datadir}/icons/hicolor/scalable/apps/lsiw.svg
%{_datadir}/icons/hicolor/scalable/mimetypes/application-x-lsiw-shortcut.svg
%{_datadir}/thumbnailers/lsiw-lwlnk.thumbnailer

%files control
%{_bindir}/lsiw-control
%{_datadir}/applications/lsiw-control.desktop
%{_metainfodir}/io.github.lsiw.Control.metainfo.xml

%changelog
* Thu Nov 05 2026 The LSIW contributors - 1.11.0-1
- Steam's default tool is LSIW Proton Experimental (Steam's own default is
  Proton Experimental); LSIW Proton Stable set by an older LSIW is switched
  over when Steam is closed, a default chosen by the user is kept
- Wine and Proton left running with no program are ended after 10 seconds;
  lsiw prefix running / stop; LSIW Control Engine page buttons
- Wine's shared processes show in System Monitor as "Wine services (LSIW)"
- Windows services no longer keep a program's run open

* Wed Nov 04 2026 The LSIW contributors - 1.10.1-1
- Each program shows on its own in taskbars and System Monitor: its own
  Proton app ID (window class steam_app_N), a desktop entry with
  StartupWMClass and its icon, and its own systemd user scope
  (app-lsiw-<desktop ID>-<random>.scope); Wine's shared processes go to a
  scope of their own.

* Wed Nov 04 2026 The LSIW contributors - 1.10.0-1
- Run several programs at once: a program started while another runs in
  the same prefix no longer waits for it (Proton "run", the prefix's running
  Proton or Wine version is used)
- LSIW Control: Launch and Run stay available while programs run; Stop is
  a menu of the running programs (also those started from .lwlnk
  shortcuts) when more than one runs
- Each program ends and stops on its own (LSIW_INSTANCE watcher)

* Wed Nov 04 2026 The LSIW contributors - 1.9.1-1
- Fix programs not starting with Proton (FileNotFoundError tracked_files):
  prefixes without Proton's tracked_files list are repaired
- Find Steam in ~/.steam/debian-installation and $XDG_DATA_HOME/Steam; use
  the Steam client's folder for STEAM_COMPAT_CLIENT_INSTALL_PATH; no false
  "Steam is not installed" warning

* Tue Nov 03 2026 The LSIW contributors - 1.9.0-1
- Play time tracking of library programs (total, launches, last played,
  last session); 'lsiw programs playtime'; crash- and power-loss-safe
- LSIW Control: Grid, List and Details layouts for the Programs page,
  sort by name, play time, last played or launches; remembered

* Tue Nov 03 2026 The LSIW contributors - 1.8.0-1
- Extra drives: any folder can be D:, E:, ... for the native engine, Wine
  and Proton ('lsiw drives', the Drives page of LSIW Control)
- 'lsiw update': daily background check for new releases (can be turned
  off) and an installer that verifies the release's SHA256SUMS; Updates
  page and banner in LSIW Control
- Online and offline installers for every package format

* Mon Nov 02 2026 The LSIW contributors - 1.7.1-1
- The C: drive is ~/.cdrive by default; an existing ~/c_drive moves there
  once and ~/c_drive becomes a link to it (nothing is deleted)

* Mon Nov 02 2026 The LSIW contributors - 1.7.0-1
- LSIW shortcuts (.lwlnk): start a program of the library with its
  settings ('lsiw launch', double-click); "Desktop shortcut" button on the
  Programs page; the program's icon is embedded and shown by file managers
  (thumbnailer, GNOME custom icon); new MIME type application/x-lsiw-shortcut
- Desktop and menu shortcuts of library programs use the program's icon

* Sun Nov 01 2026 The LSIW contributors - 1.6.0-1
- One shared prefix for every program and game, Wine and Proton alike,
  Steam games included (old per-game prefixes are kept as backups)
- The C: drive is a folder of its own choice (default ~/c_drive)
- Program library with per-program engine, prefix, DXVK/VKD3D, arguments,
  environment, MangoHud, debug log and desktop shortcut
- Custom default engine with up to three fallbacks

* Mon Oct 26 2026 The LSIW contributors - 1.5.1-1
- Wine prefix: install Wine Mono and repair missing services before a
  program runs, not only during setup

* Wed Oct 21 2026 The LSIW contributors - 1.5.0-1
- Windows shortcuts (.lnk): open with LSIW, 'lsiw lnk', and
  "Link to Windows Application..." in KDE's Create New menu
- Wine prefix: Windows services, Wine Mono, no PowerShell stub

* Wed Oct 21 2026 The LSIW contributors - 1.4.2-1
- Skip Wine 11.20, which crashes at start with less than 32 GB RAM+swap;
  fall back to the release before a Wine build that crashes in wineboot

* Wed Oct 21 2026 The LSIW contributors - 1.4.1-1
- Engine page: the Proton list shows LSIW's twin of every Proton
- Download Kron4ek's WoW64 Wine build; fixes "could not load kernel32.dll"

* Wed Oct 21 2026 The LSIW contributors - 1.4.0-1
- Download Proton-CachyOS, DW-Proton, Proton-GE RTSP, Proton-Tkg, Proton-EM
  and Proton-Wineland; each gets an LSIW twin in Steam
- Twins of Proton 6.3 back to 3.7, Hotfix and Proton Next

* Tue Oct 20 2026 The LSIW contributors - 1.3.0-1
- Windows folders default to $HOME (~/Windows, ~/Program Files, ...)
- Create /home/Public and /home/Default with tmpfiles.d

* Tue Oct 20 2026 The LSIW contributors - 1.2.1-1
- LSIW Control can be resized freely; pages scroll and wrap
- Downloaded Wine builds are installed as x86_64

* Tue Oct 20 2026 The LSIW contributors - 1.2.0-1
- LSIW twins of every Proton in Steam's list, installed on demand
- GE-Proton downloads pick the build for this processor

* Tue Oct 20 2026 The LSIW contributors - 1.1.0-2
- Apply the binfmt_misc rule on installation even where %%binfmt_apply is
  not defined; add install.sh

* Tue Oct 20 2026 The LSIW contributors - 1.1.0-1
- Choice of Wine version, new Proton engine with choice of Proton version,
  GE-Proton and Wine downloads, all using the folders chosen in LSIW Control
- Steam integration: "LSIW Proton <version>" compatibility tools
- LSIW Control: version pickers and a Steam integration page

* Thu Oct 15 2026 The LSIW contributors - 1.0.0-1
- First release: native engine for 64-bit console programs, the single C:
  drive, relocatable Windows folders, Wine fallback, LSIW Control
