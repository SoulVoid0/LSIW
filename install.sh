#!/bin/sh
# LSIW installer for Fedora.
#
# Run it from the folder holding the release files (the unpacked release
# zip):
#
#     sh install.sh                # install LSIW and LSIW Control
#     sh install.sh --help         # options
#
# What it does:
#   1. checks the files against SHA256SUMS;
#   2. installs the lsiw and lsiw-control RPMs with dnf, which also installs
#      what they need (GTK 4, and Wine as a recommended package). When this
#      Fedora is too old for the prebuilt RPMs (their C library is newer),
#      it rebuilds them from the source RPM first;
#   3. optionally adds "LSIW Proton" to Steam and downloads GE-Proton, as
#      the user who ran it (not root).
#
# Running it again is safe: an installed version is kept, an older one is
# upgraded.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

LSIW_VERSION=1.7.1
LSIW_RELEASE=1

usage() {
    cat <<EOF
Usage: sh install.sh [OPTIONS]

Installs LSIW $LSIW_VERSION on Fedora from the release files next to this script.

Options:
  -y, --yes         answer yes to every question
      --no-control  do not install LSIW Control (the settings app)
      --no-wine     do not install Wine (the fallback engine)
      --steam       add "LSIW Proton" to Steam (default: ask)
      --no-steam    do not touch Steam
      --ge          also download the latest GE-Proton
      --rebuild     build the RPMs from the source RPM even if the
                    prebuilt ones would work
      --uninstall   remove LSIW (and its Steam tools)
  -h, --help        show this help
EOF
}

YES=0 CONTROL=1 WINE=1 STEAM=ask GE=0 REBUILD=0 UNINSTALL=0
while [ $# -gt 0 ]; do
    case $1 in
        -y|--yes) YES=1 ;;
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

say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf 'install.sh: warning: %s\n' "$*" >&2; }
die()  { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

ask() {     # ask "Question?" -> 0 for yes
    [ "$YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    printf '%s [y/N] ' "$1"
    read -r answer || return 1
    case $answer in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# ------------------------------------------------------------------
# Where we are, who we are
# ------------------------------------------------------------------

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"

[ -r /etc/os-release ] || die "cannot read /etc/os-release; this installer is for Fedora"
# In a subshell: os-release sets VERSION, NAME, ... which must not leak.
# shellcheck disable=SC1091
os_ids=$(. /etc/os-release && echo " ${ID:-} ${ID_LIKE:-} ")
# shellcheck disable=SC1091
os_name=$(. /etc/os-release && echo "${PRETTY_NAME:-unknown}")
case $os_ids in
    *" fedora "*) ;;
    *) die "this installer is for Fedora (found: $os_name). Use the binary tarball or build from source (docs/BUILDING.md)." ;;
esac
[ "$(uname -m)" = x86_64 ] || die "LSIW runs on x86-64 only (this machine: $(uname -m))"

if [ "$(id -u)" = 0 ]; then
    SUDO=
    # The user to set Steam up for: the one who ran sudo, if any.
    DESKTOP_USER=${SUDO_USER:-}
else
    command -v sudo >/dev/null 2>&1 || die "run this as root, or install sudo"
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

if command -v dnf5 >/dev/null 2>&1; then DNF=dnf5; else DNF=dnf; fi

# ------------------------------------------------------------------
# Uninstall
# ------------------------------------------------------------------

if [ "$UNINSTALL" = 1 ]; then
    if command -v lsiw >/dev/null 2>&1; then
        if as_user lsiw steam 2>/dev/null | grep -q 'integration: on'; then
            say "Removing the LSIW Proton tools from Steam"
            as_user lsiw steam remove || :
        fi
        say "Removing the .exe integration"
        as_user lsiw unregister --user >/dev/null 2>&1 || :
    fi
    say "Removing the packages"
    pkgs=
    for p in lsiw-control lsiw; do rpm -q "$p" >/dev/null 2>&1 && pkgs="$pkgs $p"; done
    # shellcheck disable=SC2086
    [ -n "$pkgs" ] && $SUDO "$DNF" remove -y $pkgs
    echo "LSIW is removed. Your settings (~/.config/lsiw) and data (~/.local/share/lsiw) are kept."
    exit 0
fi

# ------------------------------------------------------------------
# 1. Checksums
# ------------------------------------------------------------------

BIN_RPM=lsiw-$LSIW_VERSION-$LSIW_RELEASE.x86_64.rpm
CONTROL_RPM=lsiw-control-$LSIW_VERSION-$LSIW_RELEASE.x86_64.rpm
SRC_RPM=lsiw-$LSIW_VERSION-$LSIW_RELEASE.src.rpm

say "Checking the release files"
if [ ! -f SHA256SUMS ] && [ -f lsiw.spec ]; then
    die "this is a source tree: build and install with 'make rpm' (see README.md), or run install.sh from the unpacked release zip"
fi
[ -f SHA256SUMS ] || die "SHA256SUMS is missing: run this script from the unpacked release zip"
missing=
for f in $BIN_RPM $CONTROL_RPM $SRC_RPM; do
    [ -f "$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || die "missing release files:$missing"
sha256sum --quiet --check --ignore-missing SHA256SUMS ||
    die "checksum mismatch: the download is damaged or was altered; download the release again"
echo "All checksums match."

# ------------------------------------------------------------------
# 2. Packages
# ------------------------------------------------------------------

# The prebuilt RPMs need the C library they were built against.
glibc_have=$(rpm -q --qf '%{VERSION}' glibc 2>/dev/null || echo 0)
glibc_need=$(rpm -qp --requires "$BIN_RPM" 2>/dev/null | sed -n 's/.*GLIBC_\([0-9.]*\).*/\1/p' | sort -V | tail -n 1)
glibc_need=${glibc_need:-0}
older=$(printf '%s\n%s\n' "$glibc_need" "$glibc_have" | sort -V | head -n 1)

if [ "$REBUILD" = 1 ] || [ "$older" != "$glibc_need" ]; then
    if [ "$REBUILD" = 1 ]; then
        say "Building the RPMs from the source RPM"
    else
        say "This Fedora's C library ($glibc_have) is older than the prebuilt RPMs need ($glibc_need): building them from the source RPM"
    fi
    $SUDO "$DNF" install -y rpm-build gcc make gtk4-devel pkgconfig desktop-file-utils \
        libappstream-glib systemd-rpm-macros
    BUILD=$(mktemp -d "${TMPDIR:-/tmp}/lsiw-build.XXXXXX")
    trap 'rm -rf "$BUILD"' EXIT
    # Built as the user who ran this script, never as root.
    if [ -n "$SUDO" ] || [ -z "$DESKTOP_USER" ] || [ "$DESKTOP_USER" = root ]; then
        rpmbuild --rebuild --define "_topdir $BUILD" "$HERE/$SRC_RPM"
    else
        chown "$DESKTOP_USER" "$BUILD"
        as_user rpmbuild --rebuild --define "_topdir $BUILD" "$HERE/$SRC_RPM"
    fi
    BIN_RPM=$(find "$BUILD/RPMS" -name "lsiw-$LSIW_VERSION-*.x86_64.rpm" ! -name '*debug*' | head -n 1)
    CONTROL_RPM=$(find "$BUILD/RPMS" -name "lsiw-control-$LSIW_VERSION-*.x86_64.rpm" ! -name '*debug*' | head -n 1)
    [ -n "$BIN_RPM" ] && [ -n "$CONTROL_RPM" ] || die "the rebuild did not produce the RPMs"
else
    BIN_RPM=$HERE/$BIN_RPM
    CONTROL_RPM=$HERE/$CONTROL_RPM
fi

pkgs=$BIN_RPM
[ "$CONTROL" = 1 ] && pkgs="$pkgs $CONTROL_RPM"
installed=$(rpm -q --qf '%{VERSION}-%{RELEASE}\n' lsiw 2>/dev/null | head -n 1 || :)
case $installed in
    "$LSIW_VERSION-$LSIW_RELEASE"*) say "LSIW $LSIW_VERSION is already installed; making sure everything is there" ;;
    *"not installed"*|"") say "Installing LSIW $LSIW_VERSION" ;;
    *) say "Upgrading LSIW $installed to $LSIW_VERSION-$LSIW_RELEASE" ;;
esac
weak=
[ "$WINE" = 0 ] && weak=--setopt=install_weak_deps=False
# shellcheck disable=SC2086
$SUDO "$DNF" install -y $weak $pkgs

if [ "$WINE" = 1 ] && ! command -v wine >/dev/null 2>&1; then
    say "Installing Wine (the fallback engine)"
    $SUDO "$DNF" install -y wine || warn "Wine could not be installed; LSIW's own engine still works"
fi

# The .exe double-click and binfmt rules come with the package; this makes
# LSIW the default application for .exe files for the desktop user too.
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
