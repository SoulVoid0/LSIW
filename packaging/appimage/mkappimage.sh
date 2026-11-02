#!/bin/sh
# Build the LSIW AppImage from a staged install.
#
#     sh packaging/appimage/mkappimage.sh STAGE VERSION OUTDIR
#
# STAGE is a `make install DESTDIR=STAGE PREFIX=/usr` tree with both lsiw
# and lsiw-control (the Makefile's `appimage` target makes it). Writes
# OUTDIR/LSIW-VERSION-x86_64.AppImage: lsiw, LSIW Control and the GTK 4
# libraries LSIW Control needs, so it runs on distributions without GTK 4.
#
# Uses linuxdeploy and linuxdeploy-plugin-gtk. Set LINUXDEPLOY to an
# existing linuxdeploy (the plugin must sit next to it); otherwise both are
# downloaded into OUTDIR/../tools. Works without FUSE.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

[ $# -eq 3 ] || { echo "usage: $0 STAGE VERSION OUTDIR" >&2; exit 2; }
STAGE=$(cd "$1" && pwd)
VERSION=$2
mkdir -p "$3"
OUTDIR=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)

for f in usr/bin/lsiw usr/bin/lsiw-control; do
    [ -x "$STAGE/$f" ] || { echo "mkappimage: $STAGE/$f is missing (build with GUI=yes)" >&2; exit 1; }
done

if [ -z "${LINUXDEPLOY-}" ]; then
    TOOLS=$(dirname "$OUTDIR")/tools
    mkdir -p "$TOOLS"
    LINUXDEPLOY=$TOOLS/linuxdeploy-x86_64.AppImage
    if [ ! -x "$LINUXDEPLOY" ]; then
        curl -fL --retry 3 -o "$LINUXDEPLOY.part" \
            https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
        chmod +x "$LINUXDEPLOY.part" && mv "$LINUXDEPLOY.part" "$LINUXDEPLOY"
    fi
    if [ ! -x "$TOOLS/linuxdeploy-plugin-gtk.sh" ]; then
        curl -fL --retry 3 -o "$TOOLS/linuxdeploy-plugin-gtk.sh.part" \
            https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
        chmod +x "$TOOLS/linuxdeploy-plugin-gtk.sh.part"
        mv "$TOOLS/linuxdeploy-plugin-gtk.sh.part" "$TOOLS/linuxdeploy-plugin-gtk.sh"
    fi
fi
# linuxdeploy finds its plugins in $PATH and next to itself.
PATH=$(dirname "$LINUXDEPLOY"):$PATH
export PATH

APPDIR=$(dirname "$OUTDIR")/LSIW.AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR"
cp -a "$STAGE/usr" "$APPDIR/usr"
# The default configuration, for reference (LSIW reads /etc/lsiw/lsiw.conf
# and ~/.config/lsiw; an AppImage cannot install into /etc).
mkdir -p "$APPDIR/usr/share/doc/lsiw"
cp "$STAGE/etc/lsiw/lsiw.conf" "$APPDIR/usr/share/doc/lsiw/lsiw.conf.example"
cp "$TOP/LICENSE" "$TOP/README.md" "$TOP/CHANGELOG.md" "$APPDIR/usr/share/doc/lsiw/"
# binfmt_misc and tmpfiles.d need root and fixed paths: packages only.
rm -rf "$APPDIR/usr/lib/binfmt.d" "$APPDIR/usr/lib/tmpfiles.d" "$APPDIR/usr/libexec"
find "$APPDIR/usr/bin" -type f -exec strip --strip-unneeded {} + 2>/dev/null || true

# The oldest C library the bundled GTK accepts (LSIW Control); lsiw itself
# may need less. AppRun checks it before starting LSIW Control.
# shellcheck disable=SC2046  # one argument per library
need=$(objdump -T "$APPDIR/usr/bin/lsiw-control" $(ldd "$APPDIR/usr/bin/lsiw-control" |
        awk '/=> \// && !/libc\.so|libm\.so|ld-linux|libpthread|libdl|librt/ {print $3}') 2>/dev/null |
        grep -o 'GLIBC_2\.[0-9]*' | sort -t. -k2 -n | tail -1 | sed 's/GLIBC_//')
echo "${need:-2.17}" > "$APPDIR/usr/share/doc/lsiw/gui-glibc"

cd "$(dirname "$OUTDIR")"
export APPIMAGE_EXTRACT_AND_RUN=1 DEPLOY_GTK_VERSION=4 NO_STRIP=1
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
export LDAI_OUTPUT="$OUTDIR/LSIW-$VERSION-x86_64.AppImage"
export LDAI_UPDATE_INFORMATION="gh-releases-zsync|SoulVoid0|LSIW|latest|LSIW-*x86_64.AppImage.zsync"
"$LINUXDEPLOY" --appimage-extract-and-run --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/lsiw-control" \
    --executable "$APPDIR/usr/bin/lsiw" \
    --desktop-file "$APPDIR/usr/share/applications/lsiw-control.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/lsiw.svg" \
    --custom-apprun "$HERE/AppRun" \
    --plugin gtk --output appimage
ls -l "$OUTDIR"
