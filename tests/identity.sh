#!/bin/bash
# Tests that each program shows on the desktop as itself: its own Proton
# app ID (so its own window class, steam_app_N), its own desktop entry with
# StartupWMClass, BAMF_DESKTOP_FILE_HINT, and, when a systemd user manager
# runs, its own app-lsiw-<desktop ID>-<random>.scope, with Wine's shared
# processes moved out of it. Uses a stand-in Proton.
#
#   LSIW=./lsiw bash tests/identity.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "identity.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

# The user manager is found through XDG_RUNTIME_DIR, kept from the caller.
tmp=$(mktemp -d)
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere LSIW_NO_UPDATE_CHECK=1 LSIW_PROTON_UMU=0
unset STEAM_COMPAT_CLIENT_INSTALL_PATH STEAM_COMPAT_DATA_PATH SteamAppId SteamGameId UMU_ID LSIW_PREFIX
unset LSIW_INSTANCE LSIW_DESKTOP_ID LSIW_STEAM_APPID BAMF_DESKTOP_FILE_HINT LSIW_NO_IDENTITY
mkdir -p "$HOME"
tag=lsiw-identity-$$
cleanup() {
    for d in /proc/[0-9]*; do
        c=
        IFS= read -r -d '' c < "$d/cmdline" 2>/dev/null
        case $c in *"$tag"*) kill "${d#/proc/}" 2>/dev/null ;; esac
    done
    rm -rf "$tmp"
}
trap cleanup EXIT

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
same() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "got '$2', want '$3'"; fi; }

data=$HOME/.local/share/lsiw
apps=$HOME/.local/share/applications
mkdir -p "$data/shared/pfx"
: > "$data/shared/tracked_files"; : > "$data/shared/pfx/system.reg"; : > "$data/shared/pfx/creation_sync_guard"

# The stand-in Proton records what the program would see, and starts a
# stand-in wineserver (a copy of sleep), as the first program does.
p=$data/runners/proton/Fake
mkdir -p "$p/files/bin"
cp "$(command -v sleep)" "$p/files/bin/wineserver"
cat > "$p/proton" <<END
#!/bin/bash
out=$tmp/seen-\$(basename "\$2" .exe)
{
    echo "SteamAppId=\${SteamAppId:-}"
    echo "SteamGameId=\${SteamGameId:-}"
    echo "hint=\${BAMF_DESKTOP_FILE_HINT:-}"
    echo "desktop=\${LSIW_DESKTOP_ID:-}"
    echo "cgroup=\$(sed -n 's/^0:://p' /proc/self/cgroup)"
} > "\$out"
if [ -n "\${WITH_SERVER:-}" ]; then
    (exec -a "wineserver $tag" "$p/files/bin/wineserver" 30 </dev/null >/dev/null 2>&1 &)
    sleep 4
    for d in /proc/[0-9]*; do
        c=
        IFS= read -r -d '' c < "\$d/cmdline" 2>/dev/null
        case \$c in "wineserver $tag"*) echo "server=\$(sed -n 's/^0:://p' \$d/cgroup)" >> "\$out" ;; esac
    done
fi
exit 0
END
chmod +x "$p/proton"
printf '"manifest"\n{\n  "commandline" "/proton %%verb%%"\n}\n' > "$p/toolmanifest.vdf"

printf 'MZ' > "$tmp/alpha.exe"
printf 'MZ' > "$tmp/beta.exe"
printf 'MZ' > "$tmp/gamma.exe"
"$LSIW" programs add "$tmp/alpha.exe" --name "Alpha Game" engine=proton:Fake >/dev/null
"$LSIW" programs add "$tmp/beta.exe" --name "Beta Game" engine=proton:Fake >/dev/null
val() { sed -n "s/^$2=//p" "$tmp/seen-$1" 2>/dev/null; }

"$LSIW" programs run alpha-game </dev/null >/dev/null 2>"$tmp/err"
"$LSIW" programs run beta-game </dev/null >/dev/null 2>>"$tmp/err"
a=$(val alpha SteamAppId); b=$(val beta SteamAppId)
same "a program gets an app ID of its own" "$([ -n "$a" ] && [ "$a" != 0 ] && echo yes)" yes
same "... above every Steam app ID" "$([ "${a:-0}" -ge 2147483648 ] && echo yes)" yes
same "... and SteamGameId the same" "$(val alpha SteamGameId)" "$a"
same "two programs get different IDs" "$([ "$a" != "$b" ] && echo yes)" yes
"$LSIW" programs run alpha-game </dev/null >/dev/null 2>>"$tmp/err"
same "the ID stays the same between runs" "$(val alpha SteamAppId)" "$a"

hidden=$apps/lsiw.alpha-game.desktop
same "a hidden desktop entry is written" "$([ -f "$hidden" ] && echo yes)" yes
same "... with the program's name" "$(sed -n 's/^Name=//p' "$hidden")" "Alpha Game"
same "... the window class of its windows" "$(sed -n 's/^StartupWMClass=//p' "$hidden")" "steam_app_$a"
same "... out of the applications menu" "$(sed -n 's/^NoDisplay=//p' "$hidden")" true
same "... that starts the program" "$(grep -c '^Exec=.* programs run alpha-game$' "$hidden")" 1
same "the program's environment points at it" "$(val alpha hint)" "$hidden"
same "... by desktop ID" "$(val alpha desktop)" lsiw.alpha-game

# With a menu shortcut, that one is the program's entry.
"$LSIW" programs shortcut beta-game >/dev/null
menu=$apps/lsiw-beta-game.desktop
same "a menu shortcut has StartupWMClass" "$(sed -n 's/^StartupWMClass=//p' "$menu")" "steam_app_$b"
# A shortcut made by an older LSIW, without it, gets it on the next run.
sed -i '/^StartupWMClass=/d' "$menu"
"$LSIW" programs run beta-game </dev/null >/dev/null 2>>"$tmp/err"
same "the shortcut is the program's entry" "$(val beta hint)" "$menu"
same "... an old shortcut gets StartupWMClass" "$(grep -c "^StartupWMClass=steam_app_$b\$" "$menu")" 1
same "... and the hidden entry goes" "$([ -e "$apps/lsiw.beta-game.desktop" ] && echo still || echo gone)" gone

"$LSIW" programs remove alpha-game >/dev/null
same "removing a program removes its hidden entry" "$([ -e "$hidden" ] && echo still || echo gone)" gone
"$LSIW" programs add "$tmp/alpha.exe" --name "Alpha Game" engine=proton:Fake >/dev/null

# A program run as a file, outside the library.
"$LSIW" run --engine=proton:Fake "$tmp/gamma.exe" </dev/null >/dev/null 2>>"$tmp/err"
g=$(val gamma SteamAppId)
same "a file run directly has an ID too" "$([ "${g:-0}" -ge 2147483648 ] && [ "$g" != "$a" ] && echo yes)" yes
gfile=$(ls "$apps"/lsiw.run-gamma-*.desktop 2>/dev/null | head -1)
same "... and its own hidden entry" "$([ -n "$gfile" ] && sed -n 's/^StartupWMClass=//p' "$gfile")" "steam_app_$g"
same "... that runs the file" "$([ -n "$gfile" ] && grep -c "^Exec=.* run -- \"$tmp/gamma.exe\"\$" "$gfile")" 1

# Steam's own app ID wins.
SteamAppId=440 SteamGameId=440 "$LSIW" programs run alpha-game </dev/null >/dev/null 2>>"$tmp/err"
same "Steam's app ID is kept" "$(val alpha SteamAppId)" 440
# Switched off.
LSIW_NO_IDENTITY=1 "$LSIW" programs run alpha-game </dev/null >/dev/null 2>>"$tmp/err"
same "LSIW_NO_IDENTITY=1: app 0 as before" "$(val alpha SteamAppId)" 0

# A scope of its own, when there is a systemd user manager.
if command -v systemctl >/dev/null && systemctl --user is-system-running >/dev/null 2>&1 ||
   [ "$(systemctl --user is-system-running 2>/dev/null)" = degraded ]; then
    WITH_SERVER=1 "$LSIW" programs run alpha-game </dev/null >/dev/null 2>>"$tmp/err"
    cg=$(val alpha cgroup)
    case $cg in
        */app.slice/app-lsiw-lsiw.alpha\\x2dgame-*.scope) ok "the program runs in app-lsiw-lsiw.alpha\\x2dgame-*.scope" ;;
        *) fail "the program runs in a scope of its own" "$cg" ;;
    esac
    sv=$(val alpha server)
    case $sv in
        */app.slice/app-lsiw-lsiw_wine-*.scope) ok "Wine's shared processes move to a scope of their own" ;;
        *) fail "Wine's shared processes move to a scope of their own" "$sv" ;;
    esac
    grep -q '^Name=Wine services (LSIW)$' "${XDG_DATA_HOME:-$HOME/.local/share}/applications/lsiw_wine.desktop" 2>/dev/null &&
        ok "System Monitor names them Wine services (LSIW)" || fail "lsiw_wine.desktop written"
else
    echo "ok - # skip: no systemd user manager"
fi

[ "$fails" -eq 0 ] || { echo "--- lsiw's messages"; cat "$tmp/err"; }
if [ "$fails" -eq 0 ]; then echo "identity.sh: all tests passed"; else echo "identity.sh: $fails failed"; fi
[ "$fails" -eq 0 ]
