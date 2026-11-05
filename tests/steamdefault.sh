#!/bin/bash
# Tests of Steam's default compatibility tool (`lsiw steam default`) in a
# throw-away home folder with a stand-in Steam: the default is LSIW Proton
# Experimental (the twin of Steam's own default), LSIW Proton Stable set by
# LSIW 1.10 or older moves to it, and a default the user chose stays.
#
#   LSIW=./lsiw bash tests/steamdefault.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "steamdefault.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere LSIW_NO_UPDATE_CHECK=1
unset STEAM_COMPAT_CLIENT_INSTALL_PATH STEAM_COMPAT_DATA_PATH SteamAppId SteamGameId UMU_ID LSIW_PREFIX
steam=$HOME/.steam/steam
mkdir -p "$steam/steamapps" "$steam/config"
conf=$XDG_CONFIG_HOME/lsiw/lsiw.conf
vdf=$steam/config/config.vdf

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
same() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "got '$2', want '$3'"; fi; }

# Steam's config.vdf with default tool $1 (none when empty).
write_vdf() {
    {
        printf '"InstallConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
        if [ -n "$1" ]; then
            printf '\t\t\t\t"CompatToolMapping"\n\t\t\t\t{\n\t\t\t\t\t"0"\n\t\t\t\t\t{\n'
            printf '\t\t\t\t\t\t"name"\t\t"%s"\n\t\t\t\t\t\t"config"\t\t""\n\t\t\t\t\t\t"priority"\t\t"75"\n' "$1"
            printf '\t\t\t\t\t}\n\t\t\t\t}\n'
        fi
        printf '\t\t\t}\n\t\t}\n\t}\n}\n'
    } > "$vdf"
}
default_tool() { sed -n 's/.*"name"[[:space:]]*"\([^"]*\)".*/\1/p' "$vdf" | head -1; }
setting() { sed -n "s/^$1 = //p" "$conf" | head -1; }

# 1. Plain `lsiw steam default`: LSIW Proton Experimental.
write_vdf ""
"$LSIW" steam default >/dev/null 2>&1 || fail "steam default"
same "default is LSIW Proton Experimental" "$(default_tool)" "LSIW-proton_experimental"
same "default_tool setting" "$(setting default_tool)" "LSIW-proton_experimental"
[ -d "$steam/compatibilitytools.d/LSIW-proton_experimental" ] && ok "Experimental twin written" ||
    fail "Experimental twin written"

# 2. What LSIW 1.10 left: default_tool = LSIW-proton-stable, Steam's
# default LSIW Proton Stable. `lsiw steam add` migrates it.
write_vdf "LSIW-proton-stable"
sed -i 's/^default_tool = .*/default_tool = LSIW-proton-stable/' "$conf"
out=$("$LSIW" steam add 2>&1)
same "old LSIW default migrated" "$(default_tool)" "LSIW-proton_experimental"
same "setting follows" "$(setting default_tool)" "LSIW-proton_experimental"
case $out in *"now LSIW Proton Experimental"*) ok "migration reported" ;; *) fail "migration reported" "$out" ;; esac
ls "$vdf".lsiw-backup-* >/dev/null 2>&1 && ok "config.vdf backed up" || fail "config.vdf backed up"

# 3. The user changed the default in Steam since: it stays.
write_vdf "GE-Proton10-25"
sed -i 's/^default_tool = .*/default_tool = LSIW-proton-stable/' "$conf"
"$LSIW" steam add >/dev/null 2>&1
same "user's default in Steam kept" "$(default_tool)" "GE-Proton10-25"

# 4. Stable named on purpose: never migrated.
write_vdf ""
"$LSIW" steam default LSIW-proton-stable >/dev/null 2>&1 || fail "steam default LSIW-proton-stable"
same "named Stable is the default" "$(default_tool)" "LSIW-proton-stable"
same "marked as chosen" "$(setting default_tool_chosen)" "true"
"$LSIW" steam add >/dev/null 2>&1
same "chosen Stable kept" "$(default_tool)" "LSIW-proton-stable"

# 5. Back to the plain default: the mark goes.
"$LSIW" steam default >/dev/null 2>&1
same "plain default again" "$(default_tool)" "LSIW-proton_experimental"
same "mark removed" "$(setting default_tool_chosen)" ""

# 6. No default in Steam at all: migration adds none.
write_vdf ""
sed -i 's/^default_tool = .*/default_tool = LSIW-proton-stable/' "$conf"
"$LSIW" steam add >/dev/null 2>&1
same "no default added by migration" "$(default_tool)" ""

[ "$fails" = 0 ] && echo "steamdefault.sh: all passed" || echo "steamdefault.sh: $fails failed"
exit $((fails > 0))
