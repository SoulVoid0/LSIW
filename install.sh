#!/bin/sh
# LSIW installer for any x86-64 Linux distribution.
#
#     sh install.sh                # install LSIW and LSIW Control
#     sh install.sh --help         # options
#
# This is the online installer: run it from a folder with the release
# files, or on its own, and the files that are not next to it are
# downloaded from the GitHub release
# (https://github.com/SoulVoid0/LSIW/releases). The offline installers
# (lsiw-VERSION-offline-installer*.run) carry the files inside and run this
# script with --offline.
#
# It picks the package format that fits the system:
#   rpm       Fedora, RHEL/CentOS/Alma/Rocky, openSUSE (dnf or zypper); on a
#             Fedora too old for the prebuilt RPMs it rebuilds them from the
#             source RPM;
#   deb       Debian, Ubuntu, Mint, Pop!_OS and other apt systems;
#   appimage  everything else (Arch, Gentoo, Void, NixOS, ...): the AppImage
#             goes to /opt/lsiw, with `lsiw` and `lsiw-control` commands in
#             /usr/local/bin and a menu entry.
# Then it
#   1. checks the files against SHA256SUMS;
#   2. installs the packages (with GTK 4 for LSIW Control, and Wine);
#   3. optionally adds "LSIW Proton" to Steam and downloads GE-Proton, as
#      the user who ran it (not root).
#
# Running it again is safe: an installed version is kept, an older one is
# upgraded.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

LSIW_VERSION=1.9.0
LSIW_RELEASE=1
LSIW_REPO=SoulVoid0/LSIW

usage() {
    cat <<EOF
Usage: sh install.sh [OPTIONS]

Installs LSIW $LSIW_VERSION from the release files next to this script, or
downloads them from https://github.com/$LSIW_REPO/releases.

Options:
  -y, --yes         answer yes to every question
      --format=FMT  rpm, deb or appimage (default: the one for this system)
      --version=V   install release V instead of $LSIW_VERSION ("latest": the
                    newest release)
      --offline     never download anything: use only the files next to
                    this script and packages already on the system
      --no-control  do not install LSIW Control (the settings app)
      --no-wine     do not install Wine (the fallback engine)
      --steam       add "LSIW Proton" to Steam (default: ask)
      --no-steam    do not touch Steam
      --ge          also download the latest GE-Proton
      --rebuild     (Fedora) build the RPMs from the source RPM even if the
                    prebuilt ones would work
      --uninstall   remove LSIW (and its Steam tools)
  -h, --help        show this help
EOF
}

YES=0 CONTROL=1 WINE=1 STEAM=ask GE=0 REBUILD=0 UNINSTALL=0 OFFLINE=0 UPDATE=0
FORMAT=${LSIW_INSTALL_FORMAT:-} WANT_VERSION=
while [ $# -gt 0 ]; do
    case $1 in
        -y|--yes) YES=1 ;;
        --offline) OFFLINE=1 ;;
        --update) UPDATE=1 YES=1 ;;      # run by 'lsiw update install'
        --version=*) WANT_VERSION=${1#--version=} ;;
        --version) shift; WANT_VERSION=${1-} ;;
        --format=*) FORMAT=${1#--format=} ;;
        --format) shift; FORMAT=${1-} ;;
        --no-control) CONTROL=0 ;;
        --no-wine) WINE=0 ;;
        --steam) STEAM=yes ;;
        --no-steam) STEAM=no ;;
        --ge) GE=1 ;;
        --rebuild) REBUILD=1 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "install.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
case $FORMAT in
    ""|rpm|deb|appimage) ;;
    *) echo "install.sh: --format must be rpm, deb or appimage" >&2; exit 2 ;;
esac

if [ -t 1 ]; then
    say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
else
    say()  { printf '==> %s\n' "$*"; }
fi
warn() { printf 'install.sh: warning: %s\n' "$*" >&2; }
die()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

ask() {     # ask "Question?" -> 0 for yes
    [ "$YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    printf '%s [y/N] ' "$1"
    read -r answer || return 1
    case $answer in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# Another release than the one this script belongs to.
case $WANT_VERSION in
    "") ;;
    latest)
        [ "$OFFLINE" = 0 ] || die "--version=latest needs the network (not --offline)"
        command -v curl >/dev/null 2>&1 || die "--version=latest needs curl"
        url=$(curl -fsS --proto =https -o /dev/null -w '%{redirect_url}' \
              "https://github.com/$LSIW_REPO/releases/latest") || die "cannot reach GitHub to find the newest release"
        LSIW_VERSION=${url##*/tag/v}
        ;;
    *) LSIW_VERSION=${WANT_VERSION#v} ;;
esac
case $LSIW_VERSION in
    ""|*[!0-9.]*) die "'$LSIW_VERSION' is not a version" ;;
esac

glibc_version() { getconf GNU_LIBC_VERSION 2>/dev/null | sed 's/^glibc //'; }
version_ge() {   # version_ge A B -> 0 when A >= B
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]
}

# ------------------------------------------------------------------
# Where we are, who we are
# ------------------------------------------------------------------

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"

[ "$(uname -m)" = x86_64 ] || die "LSIW runs on x86-64 only (this machine: $(uname -m))"

os_ids=" linux "
os_name=$(uname -s)
if [ -r /etc/os-release ]; then
    # In a subshell: os-release sets VERSION, NAME, ... which must not leak.
    # shellcheck disable=SC1091
    os_ids=$(. /etc/os-release && echo " ${ID:-} ${ID_LIKE:-} ")
    # shellcheck disable=SC1091
    os_name=$(. /etc/os-release && echo "${PRETTY_NAME:-unknown}")
fi

if command -v dnf5 >/dev/null 2>&1; then DNF=dnf5; else DNF=dnf; fi
have() { command -v "$1" >/dev/null 2>&1; }

if [ -z "$FORMAT" ]; then
    case $os_ids in
        *" fedora "*|*" rhel "*|*" centos "*|*" suse "*|*" opensuse "*)
            if have dnf5 || have dnf || have zypper; then FORMAT=rpm; fi ;;
        *" debian "*|*" ubuntu "*)
            if have apt-get; then FORMAT=deb; fi ;;
    esac
    FORMAT=${FORMAT:-appimage}
fi
PKGTOOL=
case $FORMAT in
    rpm)
        if have dnf5 || have dnf; then PKGTOOL=dnf
        elif have zypper; then PKGTOOL=zypper
        else die "--format=rpm needs dnf or zypper"; fi ;;
    deb) have apt-get || die "--format=deb needs apt-get" ;;
esac

if [ "$(id -u)" = 0 ]; then
    SUDO=
    # The user to set Steam up for: the one who ran sudo or pkexec, if any.
    DESKTOP_USER=${SUDO_USER:-}
    if [ -z "$DESKTOP_USER" ] && [ -n "${PKEXEC_UID:-}" ]; then
        DESKTOP_USER=$(id -un "$PKEXEC_UID" 2>/dev/null || :)
    fi
else
    have sudo || die "run this as root, or install sudo"
    SUDO=sudo
    DESKTOP_USER=$(id -un)
fi

as_user() {   # run a command as the desktop user
    if [ -z "$DESKTOP_USER" ] || [ "$DESKTOP_USER" = root ]; then
        "$@"
    elif [ "$(id -un)" = "$DESKTOP_USER" ]; then
        "$@"
    else
        sudo -H -u "$DESKTOP_USER" -- "$@"
    fi
}

BIN_RPM=lsiw-$LSIW_VERSION-$LSIW_RELEASE.x86_64.rpm
CONTROL_RPM=lsiw-control-$LSIW_VERSION-$LSIW_RELEASE.x86_64.rpm
SRC_RPM=lsiw-$LSIW_VERSION-$LSIW_RELEASE.src.rpm
BIN_DEB=lsiw_$LSIW_VERSION-${LSIW_RELEASE}_amd64.deb
CONTROL_DEB=lsiw-control_$LSIW_VERSION-${LSIW_RELEASE}_amd64.deb
APPIMAGE=LSIW-$LSIW_VERSION-x86_64.AppImage
OPT=/opt/lsiw

# Temporary folders, removed at exit.
CLEANUP=
trap 'rm -rf $CLEANUP' EXIT
TMP=     # tempdir NAME: a new folder in $TMP (not in a subshell)
tempdir() {
    TMP=$(mktemp -d "${TMPDIR:-/tmp}/lsiw-$1.XXXXXX")
    CLEANUP="$CLEANUP $TMP"
}

# ------------------------------------------------------------------
# Uninstall
# ------------------------------------------------------------------

if [ "$UNINSTALL" = 1 ]; then
    if have lsiw; then
        if as_user lsiw steam 2>/dev/null | grep -q 'integration: on'; then
            say "Removing the LSIW Proton tools from Steam"
            as_user lsiw steam remove || :
        fi
        say "Removing the .exe integration"
        as_user lsiw unregister --user >/dev/null 2>&1 || :
    fi
    if have rpm && rpm -q lsiw >/dev/null 2>&1; then
        say "Removing the RPM packages"
        pkgs=
        for p in lsiw-control lsiw; do rpm -q "$p" >/dev/null 2>&1 && pkgs="$pkgs $p"; done
        if have dnf5 || have dnf; then
            # shellcheck disable=SC2086
            $SUDO "$DNF" remove -y $pkgs
        else
            # shellcheck disable=SC2086
            $SUDO zypper --non-interactive remove $pkgs
        fi
    fi
    if have dpkg-query && dpkg-query -W -f='${Status}' lsiw 2>/dev/null | grep -q 'ok installed'; then
        say "Removing the Debian packages"
        pkgs=lsiw
        dpkg-query -W -f='${Status}' lsiw-control 2>/dev/null | grep -q 'ok installed' && pkgs="lsiw-control lsiw"
        # shellcheck disable=SC2086
        $SUDO apt-get remove -y $pkgs
    fi
    if [ -d "$OPT" ]; then
        say "Removing the AppImage"
        $SUDO rm -f /usr/local/bin/lsiw /usr/local/bin/lsiw-control \
            /usr/local/share/applications/lsiw-control.desktop \
            /usr/local/share/icons/hicolor/scalable/apps/lsiw.svg
        $SUDO rm -rf "$OPT"
    fi
    echo "LSIW is removed. Your settings (~/.config/lsiw) and data (~/.local/share/lsiw) are kept."
    exit 0
fi

# ------------------------------------------------------------------
# 1. Release files and checksums
# ------------------------------------------------------------------

if [ ! -f SHA256SUMS ] && [ -f lsiw.spec ] && [ -f Makefile ]; then
    die "this is a source tree: build packages with 'make rpm', 'make deb' or 'make appimage' (see README.md), or run install.sh from the release files"
fi

case $FORMAT in
    rpm) files="$BIN_RPM $CONTROL_RPM $SRC_RPM" ;;
    deb) files="$BIN_DEB $CONTROL_DEB" ;;
    appimage) files=$APPIMAGE ;;
esac
say "Installing LSIW $LSIW_VERSION from the $FORMAT packages ($os_name)"

missing=
for f in SHA256SUMS $files; do [ -f "$f" ] || missing="$missing $f"; done
if [ -n "$missing" ]; then
    # Not next to this script: download them from the GitHub release.
    [ "$OFFLINE" = 0 ] || die "missing release files:$missing (this installer runs offline; use the online installer, install.sh, to download them)"
    have curl || have wget || die "missing release files:$missing (install curl or wget to download them)"
    tempdir release; DL=$TMP
    base=https://github.com/$LSIW_REPO/releases/download/v$LSIW_VERSION
    say "Downloading the release files from $base"
    for f in SHA256SUMS $files; do
        if [ -f "$f" ]; then cp "$f" "$DL/$f"; continue; fi
        if have curl; then
            curl -fL --retry 3 --progress-bar -o "$DL/$f" "$base/$f" || die "could not download $f"
        else
            wget -q --show-progress -O "$DL/$f" "$base/$f" || die "could not download $f"
        fi
    done
    HERE=$DL
    cd "$HERE"
fi

say "Checking the release files"
for f in $files; do
    grep -q "[ *]$f\$" SHA256SUMS || die "$f is not listed in SHA256SUMS"
done
sha256sum --quiet --check --ignore-missing SHA256SUMS ||
    die "checksum mismatch: the download is damaged or was altered; download the release again"
echo "All checksums match."

# ------------------------------------------------------------------
# 2. Packages
# ------------------------------------------------------------------

# A package tool failed: offline, that usually means a dependency (GTK 4,
# ...) is not installed yet.
offline_hint() {
    if [ "$OFFLINE" = 1 ]; then
        die "the packages could not be installed without the network; a library they need is probably missing. Connect to the network and run the online installer (install.sh), or install the missing packages first"
    fi
    die "the packages could not be installed"
}

install_rpm() {
    # The prebuilt RPMs need the C library they were built against.
    glibc_have=$(rpm -q --qf '%{VERSION}' glibc 2>/dev/null || echo 0)
    glibc_need=$(rpm -qp --requires "$BIN_RPM" 2>/dev/null | sed -n 's/.*GLIBC_\([0-9.]*\).*/\1/p' | sort -V | tail -n 1)
    glibc_need=${glibc_need:-0}

    if [ "$REBUILD" = 1 ] || ! version_ge "$glibc_have" "$glibc_need"; then
        if [ "$REBUILD" = 1 ]; then
            say "Building the RPMs from the source RPM"
        else
            say "This system's C library ($glibc_have) is older than the prebuilt RPMs need ($glibc_need): building them from the source RPM"
        fi
        [ "$OFFLINE" = 0 ] || warn "offline: the build tools must already be installed"
        if [ "$OFFLINE" = 1 ]; then
            :
        elif [ "$PKGTOOL" = dnf ]; then
            $SUDO "$DNF" install -y rpm-build gcc make gtk4-devel pkgconfig desktop-file-utils \
                libappstream-glib systemd-rpm-macros
        else
            $SUDO zypper --non-interactive install rpm-build gcc make gtk4-devel pkgconf-pkg-config \
                desktop-file-utils appstream-glib systemd-rpm-macros
        fi
        tempdir build; BUILD=$TMP
        # Built as the user who ran this script, never as root.
        if [ -n "$SUDO" ] || [ -z "$DESKTOP_USER" ] || [ "$DESKTOP_USER" = root ]; then
            rpmbuild --rebuild --define "_topdir $BUILD" "$HERE/$SRC_RPM"
        else
            chown "$DESKTOP_USER" "$BUILD"
            as_user rpmbuild --rebuild --define "_topdir $BUILD" "$HERE/$SRC_RPM"
        fi
        bin=$(find "$BUILD/RPMS" -name "lsiw-$LSIW_VERSION-*.x86_64.rpm" ! -name '*debug*' | head -n 1)
        control=$(find "$BUILD/RPMS" -name "lsiw-control-$LSIW_VERSION-*.x86_64.rpm" ! -name '*debug*' | head -n 1)
        [ -n "$bin" ] && [ -n "$control" ] || die "the rebuild did not produce the RPMs"
    else
        bin=$HERE/$BIN_RPM
        control=$HERE/$CONTROL_RPM
    fi

    pkgs=$bin
    [ "$CONTROL" = 1 ] && pkgs="$pkgs $control"
    installed=$(rpm -q --qf '%{VERSION}-%{RELEASE}\n' lsiw 2>/dev/null | head -n 1 || :)
    case $installed in
        "$LSIW_VERSION-$LSIW_RELEASE"*) say "LSIW $LSIW_VERSION is already installed; making sure everything is there" ;;
        *"not installed"*|"") say "Installing the packages" ;;
        *) say "Upgrading LSIW $installed to $LSIW_VERSION-$LSIW_RELEASE" ;;
    esac
    if [ "$PKGTOOL" = zypper ]; then
        norefresh=
        [ "$OFFLINE" = 1 ] && norefresh=--no-refresh
        # shellcheck disable=SC2086
        $SUDO zypper --non-interactive $norefresh install --allow-unsigned-rpm $pkgs ||
            offline_hint
        if [ "$OFFLINE" = 0 ] && [ "$WINE" = 1 ] && ! have wine; then
            say "Installing Wine (the fallback engine)"
            $SUDO zypper --non-interactive install wine || warn "Wine could not be installed; LSIW's own engine still works"
        fi
        return 0
    fi
    weak=
    [ "$WINE" = 0 ] && weak=--setopt=install_weak_deps=False
    if [ "$OFFLINE" = 1 ]; then
        # shellcheck disable=SC2086
        $SUDO "$DNF" install -y --setopt=install_weak_deps=False --disablerepo='*' $pkgs || offline_hint
    else
        # shellcheck disable=SC2086
        $SUDO "$DNF" install -y $weak $pkgs || offline_hint
    fi
    if [ "$OFFLINE" = 0 ] && [ "$WINE" = 1 ] && ! have wine; then
        say "Installing Wine (the fallback engine)"
        $SUDO "$DNF" install -y wine || warn "Wine could not be installed; LSIW's own engine still works"
    fi
}

install_deb() {
    # The lsiw command needs glibc 2.30 (Ubuntu 20.04, Debian 11); LSIW
    # Control needs GTK 4.12 and glibc 2.38 (Ubuntu 24.04, Debian 13).
    pkgs="$HERE/$BIN_DEB"
    if [ "$CONTROL" = 1 ]; then
        if version_ge "$(glibc_version)" 2.38; then
            pkgs="$pkgs $HERE/$CONTROL_DEB"
        else
            warn "LSIW Control needs Ubuntu 24.04, Debian 13 or newer (glibc 2.38, GTK 4.12); installing the lsiw command only"
            CONTROL=0
        fi
    fi
    rec=--no-install-recommends
    [ "$WINE" = 1 ] && [ "$OFFLINE" = 0 ] && rec=
    # Offline, apt needs no download when every library is installed
    # already (and fails with the hint below when one is missing).
    if [ "$OFFLINE" = 0 ]; then
        $SUDO apt-get update -qq || warn "apt-get update failed; using the package lists as they are"
    fi
    # shellcheck disable=SC2086
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $rec $pkgs || offline_hint
}

install_appimage() {
    if [ "$CONTROL" = 1 ] && ! version_ge "$(glibc_version)" 2.38; then
        warn "LSIW Control needs glibc 2.38 or newer; the lsiw command works on this system"
    fi
    $SUDO install -d "$OPT" /usr/local/bin /usr/local/share/applications \
        /usr/local/share/icons/hicolor/scalable/apps
    $SUDO install -m 0755 "$APPIMAGE" "$OPT/$APPIMAGE"
    $SUDO ln -sfn "$APPIMAGE" "$OPT/LSIW.AppImage"
    # Older versions are not needed any more.
    for old in "$OPT"/LSIW-*-x86_64.AppImage; do
        [ -f "$old" ] && [ "$old" != "$OPT/$APPIMAGE" ] && $SUDO rm -f "$old"
    done
    # The AppImage runs lsiw or LSIW Control by the name it is started as.
    $SUDO ln -sfn "$OPT/LSIW.AppImage" /usr/local/bin/lsiw
    $SUDO ln -sfn "$OPT/LSIW.AppImage" /usr/local/bin/lsiw-control
    # The icon, taken from the AppImage (extracting needs no FUSE).
    tempdir appimage; tmp=$TMP
    if (cd "$tmp" && "$OPT/$APPIMAGE" --appimage-extract usr/share/icons/hicolor/scalable/apps/lsiw.svg >/dev/null 2>&1); then
        $SUDO install -m 0644 "$tmp/squashfs-root/usr/share/icons/hicolor/scalable/apps/lsiw.svg" \
            /usr/local/share/icons/hicolor/scalable/apps/lsiw.svg
    fi
    if [ "$CONTROL" = 1 ]; then
        printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=LSIW Control' \
            'GenericName=Windows Program Settings' \
            'Comment=Choose where Windows folders live on Linux and how Windows programs run' \
            'Icon=lsiw' 'Exec=/usr/local/bin/lsiw-control' 'Terminal=false' \
            'StartupWMClass=io.github.lsiw.Control' 'Categories=GTK;Settings;' |
            $SUDO tee /usr/local/share/applications/lsiw-control.desktop >/dev/null
        $SUDO chmod 0644 /usr/local/share/applications/lsiw-control.desktop
    fi
    if [ "$WINE" = 1 ] && ! have wine; then
        warn "Wine is not installed; install it with your package manager for the programs LSIW's own engine cannot run"
    fi
    echo "Note: starting .exe files directly from a shell (./setup.exe) needs the rpm or deb package; with the AppImage use 'lsiw run' or a double-click."
}

case $FORMAT in
    rpm) install_rpm ;;
    deb) install_deb ;;
    appimage) install_appimage ;;
esac

# The .exe double-click (and, with the packages, binfmt) rules are system
# wide; this makes LSIW the default application for .exe files for the
# desktop user too.
as_user lsiw register --user >/dev/null 2>&1 || :

# ------------------------------------------------------------------
# 3. Steam
# ------------------------------------------------------------------

steam_found=0
for d in "${HOME:-/nonexistent}/.steam/steam" "${HOME:-/nonexistent}/.local/share/Steam"; do
    [ -d "$d/steamapps" ] && steam_found=1
done
if [ -n "$DESKTOP_USER" ] && [ "$DESKTOP_USER" != "$(id -un)" ]; then
    uhome=$(getent passwd "$DESKTOP_USER" | cut -d: -f6)
    for d in "$uhome/.steam/steam" "$uhome/.local/share/Steam"; do
        [ -d "$d/steamapps" ] && steam_found=1
    done
fi

if [ "$GE" = 1 ] && [ "$OFFLINE" = 1 ]; then
    warn "offline: GE-Proton is not downloaded (run 'lsiw runners install ge' later)"
    GE=0
fi
if [ "$GE" = 1 ]; then
    say "Downloading GE-Proton"
    as_user lsiw runners install ge || warn "GE-Proton could not be downloaded"
fi

case $STEAM in
    yes) do_steam=1 ;;
    no) do_steam=0 ;;
    *)
        do_steam=0
        if [ "$steam_found" = 1 ] &&
           ask "Add \"LSIW Proton\" to Steam (Proton with LSIW's folders)?"; then
            do_steam=1
        fi
        ;;
esac
if [ "$do_steam" = 1 ]; then
    if [ "$steam_found" = 0 ]; then
        warn "Steam was not found (start it once first); skipping the Steam integration"
    else
        say "Adding LSIW Proton to Steam"
        as_user lsiw steam add || warn "the Steam integration could not be added"
    fi
fi

# ------------------------------------------------------------------

say "Done"
lsiw --version
echo "Run Windows programs with 'lsiw run PROGRAM.exe' or by double-clicking them."
[ "$CONTROL" = 1 ] && echo "Settings: LSIW Control (in the applications menu, or 'lsiw-control')."
[ "$do_steam" = 1 ] && [ "$steam_found" = 1 ] && echo "Restart Steam to see the LSIW Proton tools."
exit 0
