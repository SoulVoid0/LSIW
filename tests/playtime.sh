#!/bin/bash
# Tests of play time tracking of library programs, through the lsiw
# command in a throw-away home folder. The timing tests need a Windows
# program that waits: tests/helpers/sleep.c, built with
# x86_64-w64-mingw32-gcc when it is installed (they are skipped without).
#
#   LSIW=./lsiw bash tests/playtime.sh
#
# Prints "ok - ..." or "FAIL - ..." per check and exits with the number of
# failures.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "playtime.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere LSIW_NO_UPDATE_CHECK=1
unset LSIW_NO_PLAYTIME
mkdir -p "$HOME/Desktop" "$HOME/Games"
mkdir -p "$XDG_CONFIG_HOME" && printf 'XDG_DESKTOP_DIR="$HOME/Desktop"\n' > "$XDG_CONFIG_HOME/user-dirs.dirs"
pt=$XDG_DATA_HOME/lsiw/playtime

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
skip() { echo "ok - $1 # SKIP $2"; }
val()  { sed -n "s/^$2=//p" "$pt/$1" 2>/dev/null; }

# A program the native engine cannot load still makes a session.
echo 'not a real program' > "$HOME/Games/broken.exe"
"$LSIW" programs add "$HOME/Games/broken.exe" --name "Broken" engine=native >/dev/null 2>&1
"$LSIW" programs run broken >/dev/null 2>&1
rc=$?
[ "$(val broken launches)" = 1 ] && ok "a failed start is a session" || fail "a failed start is a session"
[ "$(val broken last_result)" = "exit $rc" ] && ok "the exit code is recorded" ||
    fail "the exit code is recorded" "$(val broken last_result) vs $rc"
[ -z "$(ls "$pt" | grep '\.run\.')" ] && ok "no running session left" || fail "no running session left"
"$LSIW" programs show broken | grep -q '^Play time: .* in 1 session' && ok "programs show has play time" ||
    fail "programs show has play time"
"$LSIW" programs playtime | grep -q '^broken .* 1  today' && ok "programs playtime lists it" ||
    fail "programs playtime lists it" "$("$LSIW" programs playtime)"

# Turned off: nothing recorded.
LSIW_NO_PLAYTIME=1 "$LSIW" programs run broken >/dev/null 2>&1
[ "$(val broken launches)" = 1 ] && ok "LSIW_NO_PLAYTIME turns it off" || fail "LSIW_NO_PLAYTIME turns it off"

"$LSIW" programs playtime broken --reset >/dev/null
[ ! -e "$pt/broken" ] && ok "--reset forgets it" || fail "--reset forgets it"

# A stale session of a watcher that is gone (power loss): removed and
# counted as lost when read.
"$LSIW" programs run broken >/dev/null 2>&1
printf 'pid=999999999\npstart=1\nboot=old-boot\nstart=1\n' > "$pt/broken.run.999999999"
"$LSIW" programs playtime broken | grep -q '^Lost: *1 session' && ok "a stale session is counted as lost" ||
    fail "a stale session is counted as lost" "$("$LSIW" programs playtime broken)"
[ ! -e "$pt/broken.run.999999999" ] && ok "a stale session file is removed" || fail "a stale session file is removed"

"$LSIW" programs remove broken >/dev/null 2>&1
[ ! -e "$pt/broken" ] && ok "remove forgets the play time" || fail "remove forgets the play time"

cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
if ! command -v "$cc" >/dev/null 2>&1 || ! "$cc" -O2 -o "$HOME/Games/sleep.exe" "$here/helpers/sleep.c" 2>/dev/null; then
    skip "timing tests" "no $cc"
    echo "$fails failure(s)"
    exit $fails
fi

"$LSIW" programs add "$HOME/Games/sleep.exe" --name "Sleeper" engine=native >/dev/null 2>&1
"$LSIW" programs run sleeper 3 7 >/dev/null 2>&1
rc=$?
[ $rc = 7 ] && ok "the program's exit code is lsiw's" || fail "the program's exit code is lsiw's" "$rc"
t=$(val sleeper total)
[ "$t" -ge 2 ] && [ "$t" -le 4 ] && ok "3 seconds counted ($t)" || fail "3 seconds counted" "$t"

# Saved while it runs, so it is there when the watcher is killed.
LSIW_PLAYTIME_TICK=1 "$LSIW" programs run sleeper 4 >/dev/null 2>&1 &
w=$!
sleep 2.5
"$LSIW" programs playtime | grep -q '^sleeper .*running now' && ok "running now" || fail "running now"
kill -KILL $w; wait $w 2>/dev/null
t=$(val sleeper total)
[ "$t" -ge 4 ] && [ "$t" -le 6 ] && ok "time saved while it ran ($t)" || fail "time saved while it ran" "$t"
sleep 2
"$LSIW" programs playtime sleeper | grep -q 'Lost: *1 session' && ok "killed watcher: lost session" ||
    fail "killed watcher: lost session" "$("$LSIW" programs playtime sleeper)"

# Two at once count once.
"$LSIW" programs playtime sleeper --reset >/dev/null
LSIW_PLAYTIME_TICK=1 "$LSIW" programs run sleeper 4 >/dev/null 2>&1 &
a=$!
sleep 1
LSIW_PLAYTIME_TICK=1 "$LSIW" programs run sleeper 2 >/dev/null 2>&1 &
b=$!
wait $a $b
t=$(val sleeper total)
[ "$(val sleeper launches)" = 2 ] && ok "two sessions" || fail "two sessions"
[ "$t" -ge 3 ] && [ "$t" -le 5 ] && ok "overlapping sessions count once ($t)" || fail "overlapping sessions count once" "$t"

# SIGTERM to lsiw stops the program and still records the session.
"$LSIW" programs playtime sleeper --reset >/dev/null
"$LSIW" programs run sleeper 30 >/dev/null 2>&1 &
w=$!
sleep 1.5
kill -TERM $w
wait $w
rc=$?
r=$(val sleeper last_result)
[ "$rc" = 143 ] && [ "$r" = "signal 15" ] && ok "SIGTERM reaches the program ($r)" ||
    fail "SIGTERM reaches the program" "rc=$rc result=$r"

# An LSIW shortcut counts too.
"$LSIW" lwlnk create sleeper "$tmp/s.lwlnk" >/dev/null 2>&1
"$LSIW" launch "$tmp/s.lwlnk" 1 >/dev/null 2>&1
[ "$(val sleeper launches)" = 2 ] && ok ".lwlnk launches are counted" || fail ".lwlnk launches are counted"

echo "$fails failure(s)"
exit $fails
