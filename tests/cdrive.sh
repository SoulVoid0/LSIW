#!/bin/bash
# Tests of the C: drive location: the default ~/.cdrive and the one-time
# move of the pre-1.7.1 default ~/c_drive, through the lsiw command in a
# throw-away home folder. Needs no Windows tools.
#
#   LSIW=./lsiw bash tests/cdrive.sh
#
# Prints "ok - ..." or "FAIL - ..." per check and exits with the number of
# failures.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "cdrive.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
check() { local name=$1; shift; if "$@" >/dev/null 2>&1; then ok "$name"; else fail "$name"; fi; }

# A fresh home folder for each case.
home() {
    export HOME=$tmp/$1 XDG_CONFIG_HOME=$tmp/$1/.config XDG_DATA_HOME=$tmp/$1/.local/share
    export XDG_DATA_DIRS=$tmp/nowhere PATH=/usr/bin:/bin
    mkdir -p "$HOME"
}

# 1. A new install: C: is ~/.cdrive, no ~/c_drive appears.
home fresh
"$LSIW" sync -q
check "fresh: C: drive is ~/.cdrive" test -d "$HOME/.cdrive" -a ! -L "$HOME/.cdrive"
check "fresh: no ~/c_drive" test ! -e "$HOME/c_drive" -a ! -L "$HOME/c_drive"
check "fresh: ~/.cdrive shows /usr" test -L "$HOME/.cdrive/usr"
out=$("$LSIW" cdrive 2>&1)
check "fresh: lsiw cdrive shows ~/.cdrive" grep -qF "$HOME/.cdrive (default)" <<<"$out"

# 2. An install from before 1.7.1: ~/c_drive with files moves to ~/.cdrive.
home old
mkdir -p "$HOME/c_drive/Games/Thing" "$HOME/pfx/dosdevices"
echo data > "$HOME/c_drive/Games/Thing/save.dat"
ln -s /usr "$HOME/c_drive/usr"                       # one of LSIW's own links
ln -s "$HOME/c_drive" "$HOME/pfx/dosdevices/c:"     # an old prefix's C:
"$LSIW" sync -q 2>"$tmp/old.err"
check "old: ~/.cdrive is a real folder" test -d "$HOME/.cdrive" -a ! -L "$HOME/.cdrive"
check "old: files moved along" grep -qx data "$HOME/.cdrive/Games/Thing/save.dat"
check "old: ~/c_drive is a link to ~/.cdrive" test "$(readlink "$HOME/c_drive")" = "$HOME/.cdrive"
check "old: old paths still work" grep -qx data "$HOME/c_drive/Games/Thing/save.dat"
check "old: old prefix C: still works" grep -qx data "$HOME/pfx/dosdevices/c:/Games/Thing/save.dat"
check "old: says where C: went" grep -qF "C: drive is now $HOME/.cdrive" "$tmp/old.err"
"$LSIW" sync -q 2>"$tmp/old2.err"
check "old: second run is quiet" test ! -s "$tmp/old2.err"
check "old: link left alone on second run" test -L "$HOME/c_drive"

# 3. Both folders hold files: nothing moves, nothing is deleted.
home both
mkdir -p "$HOME/c_drive/A" "$HOME/.cdrive/B"
echo a > "$HOME/c_drive/A/f"; echo b > "$HOME/.cdrive/B/f"
"$LSIW" sync -q 2>"$tmp/both.err"
check "both: ~/c_drive kept" grep -qx a "$HOME/c_drive/A/f"
check "both: ~/c_drive is still a folder" test -d "$HOME/c_drive" -a ! -L "$HOME/c_drive"
check "both: ~/.cdrive kept" grep -qx b "$HOME/.cdrive/B/f"
check "both: warns" grep -qF "both $HOME/c_drive and $HOME/.cdrive exist" "$tmp/both.err"

# 4. A C: drive the user chose is not touched, and ~/c_drive stays.
home chosen
mkdir -p "$HOME/c_drive/X" "$XDG_CONFIG_HOME/lsiw"
printf '[drive]\nc_drive = %s/c_drive\n' "$HOME" > "$XDG_CONFIG_HOME/lsiw/lsiw.conf"
"$LSIW" sync -q
check "chosen: ~/c_drive stays the C: drive" test -d "$HOME/c_drive/X" -a ! -L "$HOME/c_drive"
check "chosen: no ~/.cdrive" test ! -e "$HOME/.cdrive"

# 5. Moving away and back: reset goes to ~/.cdrive.
home reset
mkdir -p "$HOME/c_drive/Games"
"$LSIW" cdrive set "$HOME/elsewhere" >/dev/null 2>&1
check "set: moved from ~/c_drive (via ~/.cdrive)" test -d "$HOME/elsewhere/Games"
"$LSIW" cdrive reset >/dev/null 2>&1
check "reset: back in ~/.cdrive" test -d "$HOME/.cdrive/Games" -a ! -L "$HOME/.cdrive"
check "reset: setting cleared" sh -c "! grep -qs '^c_drive' '$XDG_CONFIG_HOME/lsiw/lsiw.conf'"

exit $fails
