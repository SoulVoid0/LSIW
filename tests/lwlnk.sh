#!/bin/bash
# Tests of LSIW shortcuts (.lwlnk) and program icons, through the lsiw
# command in a throw-away home folder. Needs no Windows tools.
#
#   LSIW=./lsiw bash tests/lwlnk.sh
#
# Prints "ok - ..." or "FAIL - ..." per check and exits with the number of
# failures.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "lwlnk.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere PATH=/usr/bin:/bin
mkdir -p "$HOME/Desktop" "$HOME/Games"
printf 'XDG_DESKTOP_DIR="$HOME/Desktop"\n' > "$tmp/user-dirs.dirs"
mkdir -p "$XDG_CONFIG_HOME" && cp "$tmp/user-dirs.dirs" "$XDG_CONFIG_HOME/user-dirs.dirs"

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
check() { local name=$1; shift; if "$@" >/dev/null 2>&1; then ok "$name"; else fail "$name"; fi; }

# A 2x2 icon: one 32-bit DIB image (BGRA rows bottom-up, then the AND
# mask). Top row: red, green; bottom row: blue, transparent.
ico() {
    printf '\x00\x00\x01\x00\x01\x00'                                  # ICONDIR, 1 image
    printf '\x02\x02\x00\x00\x01\x00\x20\x00\x38\x00\x00\x00\x16\x00\x00\x00'   # entry: 56 bytes at 22
    printf '\x28\x00\x00\x00\x02\x00\x00\x00\x04\x00\x00\x00\x01\x00\x20\x00'   # BITMAPINFOHEADER
    printf '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    printf '\xff\x00\x00\xff\x00\x00\x00\x00'                          # bottom: blue, transparent
    printf '\x00\x00\xff\xff\x00\xff\x00\xff'                          # top: red, green
    printf '\x00\x00\x00\x00\x00\x00\x00\x00'                          # AND mask (unused: alpha given)
}
ico > "$tmp/game.ico"
echo 'not a real program' > "$HOME/Games/game.exe"

"$LSIW" programs add "$HOME/Games/game.exe" --name "Test Game" engine=native args=-windowed \
    env:FOO=bar dxvk=on icon="$tmp/game.ico" >/dev/null 2>&1
check "add a program" "$LSIW" programs show test-game

out=$("$LSIW" lwlnk create test-game 2>&1)
lnk="$HOME/Desktop/Test Game.lwlnk"
[ "$out" = "$lnk" ] && [ -f "$lnk" ] && ok "create on the desktop" || fail "create on the desktop" "$out"
head -n 1 "$lnk" | grep -qx '\[LSIW Shortcut\]' && ok "magic first line" || fail "magic first line"
grep -qx 'Program=test-game' "$lnk" && grep -qx 'Args=-windowed' "$lnk" && grep -qx 'Env:FOO=bar' "$lnk" &&
    grep -qx 'DXVK=on' "$lnk" && ok "settings saved" || fail "settings saved"
grep -q '^IconData=data:image/png;base64,iVBORw0KGgo' "$lnk" && ok "icon embedded as PNG" || fail "icon embedded as PNG"

"$LSIW" lwlnk thumbnail "$lnk" "$tmp/thumb.png" 128
[ "$(head -c 8 "$tmp/thumb.png" | od -An -tx1 | tr -d ' \n')" = 89504e470d0a1a0a ] &&
    ok "thumbnail is a PNG" || fail "thumbnail is a PNG"
# IHDR: 2x2, RGBA.
[ "$(od -An -tx1 -j16 -N10 "$tmp/thumb.png" | tr -d ' \n')" = 00000002000000020806 ] &&
    ok "thumbnail size and format" || fail "thumbnail size and format"
HOME=/nonexistent "$LSIW" lwlnk thumbnail "$lnk" "$tmp/thumb2.png" 128 &&
    cmp -s "$tmp/thumb.png" "$tmp/thumb2.png" && ok "thumbnail without a home folder (sandbox)" ||
    fail "thumbnail without a home folder (sandbox)"

out=$("$LSIW" lwlnk create test-game 2>&1)
[ "$out" = "$lnk" ] && ok "re-create replaces the same shortcut" || fail "re-create replaces the same shortcut" "$out"
echo other > "$HOME/Desktop/Other.lwlnk"
"$LSIW" programs set test-game name=Other >/dev/null
out=$("$LSIW" lwlnk create test-game 2>&1)
[ "$out" = "$HOME/Desktop/Other (2).lwlnk" ] && grep -qx other "$HOME/Desktop/Other.lwlnk" &&
    ok "never replaces another file" || fail "never replaces another file" "$out"
"$LSIW" programs set test-game "name=Test Game" >/dev/null

out=$("$LSIW" lwlnk create test-game "$tmp/x" --no-icon 2>&1)
[ "$out" = "$tmp/x.lwlnk" ] && ! grep -q '^IconData=' "$tmp/x.lwlnk" && ok "create at a path, --no-icon" ||
    fail "create at a path, --no-icon" "$out"

"$LSIW" lwlnk "$lnk" | grep -q 'in the library' && ok "show" || fail "show"

# Launching: the program is not a Windows program, so the run itself fails
# (126) after LSIW took the library's settings.
out=$("$LSIW" -v launch "$lnk" 2>&1); rc=$?
echo "$out" | grep -q 'program test-game of the library' && echo "$out" | grep -q 'engine: native' &&
    [ $rc = 126 ] && ok "launch uses the library entry" || fail "launch uses the library entry" "rc=$rc $out"
"$LSIW" programs set test-game engine=auto >/dev/null
"$LSIW" -v run "$lnk" 2>&1 | grep -q 'program test-game of the library' && ok "lsiw run FILE.lwlnk" || fail "lsiw run FILE.lwlnk"
"$LSIW" -v "$lnk" 2>&1 | grep -q 'program test-game of the library' && ok "lsiw FILE.lwlnk" || fail "lsiw FILE.lwlnk"
"$LSIW" programs remove test-game >/dev/null
out=$("$LSIW" launch "$lnk" 2>&1)
echo "$out" | grep -q 'using the settings saved in' && ok "launch after removal uses the saved settings" ||
    fail "launch after removal uses the saved settings" "$out"
"$LSIW" launch "$tmp/missing.lwlnk" >/dev/null 2>&1; [ $? = 127 ] && ok "missing shortcut: 127" || fail "missing shortcut: 127"
"$LSIW" launch "$tmp/game.ico" >/dev/null 2>&1; [ $? = 126 ] && ok "not a shortcut: 126" || fail "not a shortcut: 126"

# Desktop integration for a user without a system-wide install.
"$LSIW" register --user >/dev/null 2>&1
grep -q 'application/x-lsiw-shortcut' "$XDG_DATA_HOME/applications/lsiw.desktop" && ok "desktop entry lists the type" ||
    fail "desktop entry lists the type"
[ -f "$XDG_DATA_HOME/mime/packages/lsiw-lwlnk.xml" ] && ok "user MIME type" || fail "user MIME type"
grep -q '^Exec=.* lwlnk thumbnail %i %o %s$' "$XDG_DATA_HOME/thumbnailers/lsiw-lwlnk.thumbnailer" &&
    ok "user thumbnailer" || fail "user thumbnailer"
"$LSIW" unregister --user >/dev/null 2>&1
[ ! -e "$XDG_DATA_HOME/mime/packages/lsiw-lwlnk.xml" ] && [ ! -e "$XDG_DATA_HOME/thumbnailers/lsiw-lwlnk.thumbnailer" ] &&
    ok "unregister removes them" || fail "unregister removes them"

echo
if [ $fails = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL ($fails failures)"; fi
exit $fails
