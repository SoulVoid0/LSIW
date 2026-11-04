#!/bin/bash
# Tests of the proton engine around Proton itself, with a stand-in Proton
# (a shell script that checks the prefix the way Proton's setup_prefix
# does): repair of prefixes Proton would stop on ("FileNotFoundError ...
# tracked_files"), Steam detection (~/.steam/debian-installation, Proton
# versions in steamapps/common) and STEAM_COMPAT_CLIENT_INSTALL_PATH, in a
# throw-away home folder.
#
#   LSIW=./lsiw bash tests/proton.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "proton.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere LSIW_NO_UPDATE_CHECK=1
unset STEAM_COMPAT_CLIENT_INSTALL_PATH STEAM_COMPAT_DATA_PATH SteamAppId SteamGameId UMU_ID LSIW_PREFIX
mkdir -p "$HOME"

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
same() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "got '$2', want '$3'"; fi; }

# A stand-in Proton: like setup_prefix, it reads tracked_files when the
# prefix counts as made (pfx/creation_sync_guard), and makes the prefix
# (tracked_files, guard, system.reg) when it does not.
make_proton() {   # DIR [umu]
    mkdir -p "$1"
    {
        echo '#!/bin/sh'
        [ "${2:-}" = umu ] && echo '# knows UMU_ID'
        cat <<'EOF'
c=$STEAM_COMPAT_DATA_PATH
env > "$c/../proton-env"
if [ -e "$c/pfx/creation_sync_guard" ]; then
    [ -f "$c/tracked_files" ] || { echo "FileNotFoundError: $c/tracked_files" >&2; exit 1; }
else
    mkdir -p "$c/pfx" && : > "$c/tracked_files" && : > "$c/pfx/system.reg" && : > "$c/pfx/creation_sync_guard"
fi
echo ran-proton
exit 0
EOF
    } > "$1/proton"
    chmod +x "$1/proton"
    printf '"manifest"\n{\n  "commandline" "/proton %%verb%%"\n}\n' > "$1/toolmanifest.vdf"
}

data=$HOME/.local/share/lsiw
shared=$data/shared
make_proton "$data/runners/proton/Fake-Proton" umu
"$LSIW" config proton Fake-Proton >/dev/null || fail "config proton"
exe=$tmp/prog.exe
printf 'MZ' > "$exe"
run() { "$LSIW" run --engine=proton "$exe" 2>"$tmp/err"; }

same "fresh prefix: Proton runs" "$(run)" ran-proton
same "fresh prefix: Proton made tracked_files" "$(test -f "$shared/tracked_files" && echo yes)" yes
same "no Steam: client folder is LSIW's" "$(grep '^STEAM_COMPAT_CLIENT_INSTALL_PATH=' "$data/proton-env")" \
     "STEAM_COMPAT_CLIENT_INSTALL_PATH=$data/steam-client"
same "umu build gets UMU_ID" "$(grep -c '^UMU_ID=umu-0$' "$data/proton-env")" 1

# The broken prefix of the bug report: made, but tracked_files is gone.
rm -f "$shared/tracked_files"
echo 4.11-2 > "$shared/version"
same "made prefix without tracked_files: Proton runs" "$(run)" ran-proton
same "tracked_files recreated" "$(test -f "$shared/tracked_files" && echo yes)" yes
same "repair is reported" "$(grep -c 'repaired the Proton prefix' "$tmp/err")" 1
same "registry kept" "$(test -f "$shared/pfx/system.reg" && echo yes)" yes
same "version kept" "$(cat "$shared/version")" 4.11-2

# A guard without a prefix behind it: Proton must make the prefix.
rm -f "$shared/tracked_files" "$shared/pfx/system.reg"
same "unfinished prefix: Proton runs" "$(run)" ran-proton
same "unfinished prefix is made again" "$(test -f "$shared/pfx/system.reg" && echo yes)" yes

# A healthy prefix is left alone.
run >/dev/null
same "healthy prefix: nothing repaired" "$(grep -c 'repaired' "$tmp/err")" 0

# A program's own prefix ($LSIW_PREFIX) is repaired too.
own=$tmp/own
mkdir -p "$own/pfx" && : > "$own/pfx/system.reg" && : > "$own/pfx/creation_sync_guard"
same "own prefix: Proton runs" "$(LSIW_PREFIX=$own run)" ran-proton
same "own prefix: tracked_files recreated" "$(test -f "$own/tracked_files" && echo yes)" yes

# Steam installed by Debian's/Ubuntu's steam-installer, Proton in it.
steam=$HOME/.steam/debian-installation
mkdir -p "$steam/steamapps/common" "$steam/ubuntu12_32" "$steam/linux64"
: > "$steam/steam.sh"
: > "$steam/linux64/steamclient.so"
make_proton "$steam/steamapps/common/Proton 11.0"
same "Steam's Proton is listed" "$("$LSIW" runners | grep -c 'Proton 11.0 .*Steam')" 1
"$LSIW" config proton "Proton 11.0" >/dev/null || fail "config proton 'Proton 11.0'"
same "Steam's Proton runs" "$(run)" ran-proton
same "client folder is Steam's" "$(grep '^STEAM_COMPAT_CLIENT_INSTALL_PATH=' "$data/proton-env")" \
     "STEAM_COMPAT_CLIENT_INSTALL_PATH=$(cd "$steam" && pwd -P)"
same "no 'Steam is not installed' warning" "$(grep -c 'not installed' "$tmp/err")" 0
same "Valve build runs without UMU_ID" "$(grep -c '^UMU_ID=' "$data/proton-env")" 0

if [ "$fails" -eq 0 ]; then echo "proton.sh: all tests passed"; else echo "proton.sh: $fails failed"; fi
[ "$fails" -eq 0 ]
