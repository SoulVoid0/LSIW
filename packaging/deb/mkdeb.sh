#!/bin/sh
# Build the Debian/Ubuntu packages of LSIW from a staged install.
#
#     sh packaging/deb/mkdeb.sh STAGE VERSION OUTDIR
#
# STAGE is a `make install DESTDIR=STAGE PREFIX=/usr` tree (the Makefile's
# `deb` target makes it). Writes OUTDIR/lsiw_VERSION-1_amd64.deb and, when
# STAGE holds lsiw-control, OUTDIR/lsiw-control_VERSION-1_amd64.deb: the
# same split as the Fedora packages. Needs dpkg-deb and dpkg-shlibdeps
# (dpkg-dev); runs as any user (file owners are set to root with
# --root-owner-group).
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
DEBREV=${DEBREV:-1}
ARCH=amd64
MAINTAINER=${DEB_MAINTAINER:-"The LSIW contributors <noreply@github.com>"}
HOMEPAGE=https://github.com/SoulVoid0/LSIW

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ -x "$STAGE/usr/bin/lsiw" ] || { echo "mkdeb: $STAGE/usr/bin/lsiw is missing" >&2; exit 1; }

# shlibs NAME BINARY...: the Depends line dpkg-shlibdeps computes.
shlibs() {
    name=$1; shift
    mkdir -p "$WORK/shl/debian"
    printf 'Source: lsiw\n\nPackage: %s\nArchitecture: any\n' "$name" > "$WORK/shl/debian/control"
    (cd "$WORK/shl" && dpkg-shlibdeps -O "$@" 2>/dev/null) | sed -n 's/^shlibs:Depends=//p'
}

# copy SRC-RELATIVE-PATH... into package root $PKG
take() {
    for f in "$@"; do
        mkdir -p "$PKG/$(dirname "$f")"
        cp -a "$STAGE/$f" "$PKG/$f"
    done
    find "$PKG/usr/bin" -type f -exec strip --strip-unneeded --remove-section=.comment --remove-section=.note {} + 2>/dev/null || true
}

docs() {    # docs PACKAGE: copyright and changelog where Debian wants them
    d=$PKG/usr/share/doc/$1
    mkdir -p "$d"
    {
        echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
        echo "Upstream-Name: LSIW"
        echo "Source: $HOMEPAGE"
        echo
        echo "Files: *"
        echo "Copyright: 2026 The LSIW contributors"
        echo "License: AGPL-3.0-or-later"
        echo " On Debian systems the full text is not shipped in common-licenses;"
        echo " it follows."
        echo " ."
        sed 's/^$/./; s/^/ /' "$TOP/LICENSE"
    } > "$d/copyright"
    gzip -9n -c "$TOP/CHANGELOG.md" > "$d/changelog.gz"
    {
        echo "lsiw ($VERSION-$DEBREV) unstable; urgency=medium"
        echo
        echo "  * Upstream release $VERSION; see changelog.gz."
        echo
        echo " -- $MAINTAINER  $(date -R -d "@${SOURCE_DATE_EPOCH:-$(stat -c %Y "$TOP/CHANGELOG.md")}")"
    } | gzip -9n > "$d/changelog.Debian.gz"
    if [ "$1" = lsiw ]; then
        for f in README.md CONTRIBUTING.md docs/ARCHITECTURE.md docs/BUILDING.md \
                 docs/COMPATIBILITY.md docs/PROTON-STEAM.md; do
            cp "$TOP/$f" "$d/"
        done
    fi
    chmod -R u=rwX,go=rX "$PKG/usr/share/doc"
}

control() {   # control NAME DEPENDS EXTRA-FIELDS DESCRIPTION-FILE
    size=$(du -sk --exclude=DEBIAN "$PKG" | cut -f1)
    {
        echo "Package: $1"
        echo "Version: $VERSION-$DEBREV"
        echo "Architecture: $ARCH"
        echo "Maintainer: $MAINTAINER"
        echo "Installed-Size: $size"
        echo "Depends: $2"
        [ -z "$3" ] || printf '%s\n' "$3"
        echo "Section: otherosfs"
        echo "Priority: optional"
        echo "Homepage: $HOMEPAGE"
        cat "$4"
    } > "$PKG/DEBIAN/control"
}

build() {     # build NAME
    find "$PKG" -path "$PKG/DEBIAN" -prune -o -type f -printf '%P\0' |
        (cd "$PKG" && xargs -0r md5sum) > "$PKG/DEBIAN/md5sums"
    dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUTDIR/$1_$VERSION-${DEBREV}_$ARCH.deb" >/dev/null
    echo "wrote $OUTDIR/$1_$VERSION-${DEBREV}_$ARCH.deb"
}

# ---------------------------------------------------------------- lsiw
PKG=$WORK/lsiw
mkdir -p "$PKG/DEBIAN"
take usr/bin/lsiw usr/libexec/lsiw usr/lib/binfmt.d/zz-lsiw.conf usr/lib/tmpfiles.d/lsiw.conf \
     etc/lsiw/lsiw.conf usr/share/applications/lsiw.desktop usr/share/mime/packages/lsiw.xml \
     usr/share/templates usr/share/icons usr/share/thumbnailers
docs lsiw
echo /etc/lsiw/lsiw.conf > "$PKG/DEBIAN/conffiles"
cat > "$WORK/desc" <<'EOF'
Description: Run Windows x86-64 programs on Linux with a native engine
 LSIW (Linux Subsystem for Improved Wine) runs Windows x86-64 programs on
 Linux. Its native engine is its own: a loader for Windows executables and
 DLLs and its own implementations of kernel32, ntdll, the Microsoft C
 runtimes and other system DLLs. Wine is used only as a fallback for
 programs the native engine cannot run.
 .
 This package contains the lsiw command, the binfmt_misc rule that lets
 Windows programs run directly (./setup.exe), and the desktop integration
 that opens Windows programs, Windows shortcuts and LSIW shortcuts
 (.lwlnk) with a double-click.
EOF
control lsiw "$(shlibs lsiw "$PKG/usr/bin/lsiw"), hicolor-icon-theme" \
    "Recommends: wine, zenity, git, unzip, desktop-file-utils, shared-mime-info" "$WORK/desc"
cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    # Register the .exe handler with the running kernel (systemd-binfmt
    # does it again at every boot).
    if [ -x /usr/lib/systemd/systemd-binfmt ]; then
        /usr/lib/systemd/systemd-binfmt /usr/lib/binfmt.d/zz-lsiw.conf >/dev/null 2>&1 || true
    elif [ -x /lib/systemd/systemd-binfmt ]; then
        /lib/systemd/systemd-binfmt /usr/lib/binfmt.d/zz-lsiw.conf >/dev/null 2>&1 || true
    fi
    # C:\users\Public and C:\users\Default (/home/Public, /home/Default).
    systemd-tmpfiles --create /usr/lib/tmpfiles.d/lsiw.conf >/dev/null 2>&1 ||
        install -d -m 1777 /home/Public /home/Default >/dev/null 2>&1 || true
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
    update-mime-database /usr/share/mime >/dev/null 2>&1 || true
    gtk-update-icon-cache -q -t /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
EOF
cat > "$PKG/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
# On removal (not upgrade) take the handler out of the running kernel too.
if [ "$1" = remove ] && [ -e /proc/sys/fs/binfmt_misc/LSIW ]; then
    echo -1 > /proc/sys/fs/binfmt_misc/LSIW 2>/dev/null || true
fi
exit 0
EOF
cat > "$PKG/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
update-mime-database /usr/share/mime >/dev/null 2>&1 || true
exit 0
EOF
chmod 0755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/prerm" "$PKG/DEBIAN/postrm"
build lsiw

# -------------------------------------------------------- lsiw-control
if [ -x "$STAGE/usr/bin/lsiw-control" ]; then
    PKG=$WORK/lsiw-control
    mkdir -p "$PKG/DEBIAN"
    take usr/bin/lsiw-control usr/share/applications/lsiw-control.desktop \
         usr/share/metainfo/io.github.lsiw.Control.metainfo.xml
    docs lsiw-control
    cat > "$WORK/desc" <<'EOF'
Description: Settings app for LSIW
 LSIW Control is the GTK 4 settings app for LSIW: choose where each Windows
 folder lives on Linux, see how the C: drive maps onto the Linux file
 system, pick the engine that runs Windows programs, and check whether a
 program can run on LSIW's native engine.
EOF
    control lsiw-control "$(shlibs lsiw-control "$PKG/usr/bin/lsiw-control"), lsiw (= $VERSION-$DEBREV)" "" "$WORK/desc"
    cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
exit 0
EOF
    cp "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/postrm"
    chmod 0755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/postrm"
    build lsiw-control
fi
