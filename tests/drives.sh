#!/bin/bash
# Tests of the extra drives (D:, E:, ...): `lsiw drives`, path conversion
# both ways, the shared prefix's dosdevices links, and (when the Windows
# test programs are built) tests/bin/t_drives.exe on a D: folder, in a
# throw-away home folder.
#
#   LSIW=./lsiw bash tests/drives.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "drives.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere LSIW_NO_UPDATE_CHECK=1
mkdir -p "$HOME"

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
same() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "got '$2', want '$3'"; fi; }

"$LSIW" drives add D "$HOME/Games" >/dev/null || fail "drives add D"
"$LSIW" drives add e: "~/Data" >/dev/null || fail "drives add e: with ~"
same "D: folder created" "$(test -d "$HOME/Games" && echo yes)" yes
same "config keeps \${HOME}" "$(grep '^e = ' "$XDG_CONFIG_HOME/lsiw/lsiw.conf")" 'e = ${HOME}/Data'
same "list shows D:" "$("$LSIW" drives | grep -c "^D:  $HOME/Games")" 1
same "D:\\x -> folder" "$("$LSIW" path linux 'D:\x\y.txt')" "$HOME/Games/x/y.txt"
same "e: is case-insensitive" "$("$LSIW" path linux 'e:\')" "$HOME/Data"
same "folder -> D:\\" "$("$LSIW" path win "$HOME/Games/a/b")" 'D:\a\b'
same "drive root -> D:\\" "$("$LSIW" path win "$HOME/Games")" 'D:\'
same "home stays C:" "$("$LSIW" path win "$HOME/Music")" "C:\\users\\$(id -un)\\Music"
same "unconfigured letter folds onto C:" "$("$LSIW" path linux 'Q:\usr')" /usr
"$LSIW" drives add C /tmp >/dev/null 2>&1 && fail "C cannot be an extra drive" || ok "C cannot be an extra drive"
"$LSIW" drives add D / >/dev/null 2>&1 && fail "/ cannot be a drive" || ok "/ cannot be a drive"
dd=$("$LSIW" prefix status >/dev/null 2>&1; echo "$XDG_DATA_HOME/lsiw/shared/pfx/dosdevices")
same "shared prefix d: link" "$(readlink "$dd/d:")" "$HOME/Games"
"$LSIW" drives remove D >/dev/null || fail "drives remove D"
same "d: link removed" "$(test -e "$dd/d:" -o -L "$dd/d:" && echo there || echo gone)" gone
same "e: link kept" "$(readlink "$dd/e:")" "$HOME/Data"
same "folder kept after remove" "$(test -d "$HOME/Games" && echo yes)" yes

if [ -f "$here/bin/t_drives.exe" ]; then
    "$LSIW" drives add D "$HOME/Games" >/dev/null
    echo lsiw-drive > "$HOME/Games/marker.txt"
    out=$("$LSIW" run --engine=native "$here/bin/t_drives.exe" 2>&1)
    if grep -q "RESULT: PASS" <<<"$out"; then ok "t_drives.exe on D:"; else fail "t_drives.exe on D:" "$(grep FAIL <<<"$out" | head -n 5)"; fi
else
    echo "skip - t_drives.exe: not built (make test-binaries)"
fi

echo "drives.sh: $fails failure(s)"
exit "$fails"
