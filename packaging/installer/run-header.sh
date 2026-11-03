#!/bin/sh
# LSIW @VERSION@ offline installer (@WHAT@).
#
# Everything this installs is inside this file: no download is needed.
#
#     sh @NAME@               install (same options as install.sh)
#     sh @NAME@ --help        the options
#     sh @NAME@ --extract DIR only unpack the release files into DIR
#     sh @NAME@ --verify      only check that this file is intact
#
# The files are checked against the SHA-256 recorded below and the
# release's SHA256SUMS before anything is installed. Libraries the
# packages need (GTK 4 for LSIW Control, ...) must already be on the
# system; the online installer (install.sh) fetches them when they are
# not.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
PAYLOAD_SHA256=@SHA256@
PAYLOAD_FORMAT=@FORMAT@

self=$0
die() { printf '%s: %s\n' "${self##*/}" "$*" >&2; exit 1; }
[ -r "$self" ] || die "cannot read $self (run it as: sh FILE.run)"
line=$(awk '/^__LSIW_PAYLOAD_BELOW__$/ { print NR + 1; exit }' "$self")
[ -n "$line" ] || die "this file is damaged (no payload)"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/lsiw-offline.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
tail -n +"$line" "$self" > "$tmp/payload.tar"
sum=$(sha256sum "$tmp/payload.tar" | cut -d' ' -f1)
[ "$sum" = "$PAYLOAD_SHA256" ] || die "this file is damaged or was altered (checksum mismatch); download it again"

case ${1-} in
    --verify) echo "The installer is intact (SHA-256 $sum)."; exit 0 ;;
    --extract)
        dest=${2:-lsiw-@VERSION@}
        mkdir -p "$dest"
        tar -xf "$tmp/payload.tar" -C "$dest"
        echo "Unpacked to $dest; install with: sh $dest/install.sh --offline"
        exit 0 ;;
esac
mkdir "$tmp/files"
tar -xf "$tmp/payload.tar" -C "$tmp/files"
rm -f "$tmp/payload.tar"
if [ -n "$PAYLOAD_FORMAT" ]; then
    LSIW_INSTALL_FORMAT=$PAYLOAD_FORMAT sh "$tmp/files/install.sh" --offline "$@"
else
    sh "$tmp/files/install.sh" --offline "$@"
fi
exit $?
__LSIW_PAYLOAD_BELOW__
