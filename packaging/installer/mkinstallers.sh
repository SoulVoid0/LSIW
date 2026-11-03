#!/bin/sh
# Make the online and offline installers of a release.
#
#     sh packaging/installer/mkinstallers.sh VERSION DIR
#
# DIR holds the release files (the .rpm, .deb and .AppImage packages,
# install.sh and their SHA256SUMS); the installers are added to it:
#
#   lsiw-V-online-installer.sh           any distribution (picks the format)
#   lsiw-V-online-installer-rpm.sh       Fedora, RHEL & co., openSUSE
#   lsiw-V-online-installer-deb.sh       Debian, Ubuntu & co.
#   lsiw-V-online-installer-appimage.sh  any distribution, as an AppImage
#       small scripts that download the packages they need from the release
#   lsiw-V-offline-installer.run           every format inside
#   lsiw-V-offline-installer-rpm.run       the RPMs inside
#   lsiw-V-offline-installer-deb.run       the .debs inside
#   lsiw-V-offline-installer-appimage.run  the AppImage inside
#       self-extracting installers that need no network
#
# Run `sha256sum` over DIR again afterwards to list them in SHA256SUMS.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

[ $# -eq 2 ] || { echo "usage: sh $0 VERSION DIR" >&2; exit 2; }
V=$1
DIR=$(cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$DIR"

for f in install.sh SHA256SUMS; do [ -f "$f" ] || { echo "mkinstallers: $DIR/$f is missing" >&2; exit 1; }; done
grep -q "^LSIW_VERSION=$V\$" install.sh || { echo "mkinstallers: install.sh is not for $V" >&2; exit 1; }

rpm_files=$(ls lsiw-"$V"-*.x86_64.rpm lsiw-control-"$V"-*.x86_64.rpm lsiw-"$V"-*.src.rpm 2>/dev/null | grep -v debug || :)
deb_files=$(ls lsiw_"$V"-*_amd64.deb lsiw-control_"$V"-*_amd64.deb 2>/dev/null || :)
app_files=$(ls LSIW-"$V"-x86_64.AppImage 2>/dev/null || :)

# ---- online: install.sh, with the format chosen or not ----
cp install.sh "lsiw-$V-online-installer.sh"
for fmt in rpm deb appimage; do
    sed "s/^FORMAT=\${LSIW_INSTALL_FORMAT:-}/FORMAT=\${LSIW_INSTALL_FORMAT:-$fmt}/" install.sh > "lsiw-$V-online-installer-$fmt.sh"
    grep -q "LSIW_INSTALL_FORMAT:-$fmt}" "lsiw-$V-online-installer-$fmt.sh" ||
        { echo "mkinstallers: cannot preset the format in install.sh" >&2; exit 1; }
done
chmod 0755 lsiw-"$V"-online-installer*.sh

# ---- offline: a shell header and a tar of the files ----
make_run() {    # make_run NAME FORMAT DESCRIPTION FILES...
    name=$1 fmt=$2 what=$3
    shift 3
    [ $# -gt 0 ] || { echo "mkinstallers: no files for $name" >&2; exit 1; }
    tmp=$(mktemp -d)
    # A SHA256SUMS of just these files, so install.sh checks every one.
    grep -E "[ *]($(printf '%s|' "$@" install.sh | sed 's/|$//; s/\./\\./g'))\$" SHA256SUMS > "$tmp/SHA256SUMS"
    for f in "$@"; do grep -q "[ *]$f\$" "$tmp/SHA256SUMS" || { echo "mkinstallers: $f is not in SHA256SUMS" >&2; exit 1; }; done
    cp install.sh "$@" "$tmp/"
    (cd "$tmp" && tar --owner=0 --group=0 --numeric-owner -cf payload.tar install.sh SHA256SUMS "$@")
    sum=$(sha256sum "$tmp/payload.tar" | cut -d' ' -f1)
    sed -e "s|@VERSION@|$V|g" -e "s|@NAME@|$name|g" -e "s|@WHAT@|$what|g" \
        -e "s|@SHA256@|$sum|" -e "s|@FORMAT@|$fmt|" "$HERE/run-header.sh" > "$name"
    cat "$tmp/payload.tar" >> "$name"
    chmod 0755 "$name"
    rm -rf "$tmp"
    echo "made $name ($(du -h "$name" | cut -f1))"
}

# shellcheck disable=SC2086
make_run "lsiw-$V-offline-installer-rpm.run" rpm "RPM packages: Fedora, RHEL, Alma, Rocky, openSUSE" $rpm_files
# shellcheck disable=SC2086
make_run "lsiw-$V-offline-installer-deb.run" deb "Debian packages: Debian, Ubuntu, Mint, Pop!_OS" $deb_files
# shellcheck disable=SC2086
make_run "lsiw-$V-offline-installer-appimage.run" appimage "AppImage: any x86-64 distribution" $app_files
# shellcheck disable=SC2086
make_run "lsiw-$V-offline-installer.run" "" "every format; picks the one for this system" $rpm_files $deb_files $app_files
