#!/bin/bash
# Tests of several programs running at once in one prefix, with stand-in
# Protons and a stand-in wineserver: a second program joins the first
# instead of waiting for it ("run", not "waitforexitandrun"), with the
# Proton that already runs the prefix; an instance ends when its own
# program ends although Proton's launcher waits for every program of the
# prefix; and stopping one instance leaves the others running.
#
#   LSIW=./lsiw bash tests/instances.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

here=$(cd "$(dirname "$0")" && pwd)
LSIW=${LSIW:-$here/../lsiw}
LSIW=$(cd "$(dirname "$LSIW")" && pwd)/$(basename "$LSIW")
[ -x "$LSIW" ] || { echo "instances.sh: no lsiw at $LSIW (build it first)" >&2; exit 1; }

tmp=$(mktemp -d)
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
export XDG_DATA_DIRS=$tmp/nowhere LSIW_NO_UPDATE_CHECK=1
unset STEAM_COMPAT_CLIENT_INSTALL_PATH STEAM_COMPAT_DATA_PATH SteamAppId SteamGameId UMU_ID LSIW_PREFIX LSIW_INSTANCE
unset WINEPREFIX LSIW_REAP_GRACE
# Ending a left-over wineserver is tested at the end, on its own.
export LSIW_NO_REAP=1
mkdir -p "$HOME"
tag=lsiw-instances-$$
# pgrep without procps (not in every container): pids whose argv[0]
# matches the extended regular expression $1.
cat > "$tmp/procs.sh" <<'END'
pmatch() {
    local d c
    for d in /proc/[0-9]*; do
        c=
        IFS= read -r -d '' c < "$d/cmdline" 2>/dev/null
        [ -n "$c" ] && [[ $c =~ $1 ]] && echo "${d#/proc/}"
    done
    return 0
}
pany() { [ -n "$(pmatch "$1")" ]; }
END
. "$tmp/procs.sh"
cleanup() {
    for p in $(pmatch "$tag"); do kill "$p" 2>/dev/null; done
    [ -n "${server_dir:-}" ] && rm -rf "$server_dir"
    rm -rf "$tmp"
}
trap cleanup EXIT

fails=0
ok()   { echo "ok - $1"; }
fail() { echo "FAIL - $1${2:+: $2}"; fails=$((fails + 1)); }
same() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "got '$2', want '$3'"; fi; }

data=$HOME/.local/share/lsiw
shared=$data/shared
mkdir -p "$shared/pfx"
: > "$shared/tracked_files"; : > "$shared/pfx/system.reg"; : > "$shared/pfx/creation_sync_guard"
# Wine's server folder of the prefix: /tmp/.wine-UID/server-DEV-INODE.
server_dir=/tmp/.wine-$(id -u)/$(printf 'server-%x-%x' $(stat -c '%d %i' "$shared/pfx"))

# A stand-in Proton: starts the prefix's wineserver (a copy of sleep named
# wineserver, working in the server folder) unless one runs, then the
# program in a session of its own as Wine does, named like a Windows
# program, and waits like umu.exe until no program of the prefix is left.
#   proton VERB EXE NAME SECONDS
make_proton() {   # DIR
    mkdir -p "$1/files/bin"
    cp "$(command -v sleep)" "$1/files/bin/wineserver"
    sed -e "s|@TMP@|$tmp|g" -e "s|@TAG@|$tag|g" -e "s|@DIR@|$1|g" -e "s|@SERVER@|$server_dir|g" \
        > "$1/proton" <<'END'
#!/bin/bash
. "@TMP@/procs.sh"
echo "$(basename "$(dirname "$0")") $1 $3" >> "@TMP@/verbs"
if ! pany "wineserver @TAG@"; then
    mkdir -p "@SERVER@"
    (cd "@SERVER@" && exec setsid bash -c 'exec -a "$0 @TAG@" "$0" 1000' "@DIR@/files/bin/wineserver" \
        </dev/null >/dev/null 2>&1 &)
fi
setsid bash -c 'exec -a "Z:\\prog\\$0.exe" sleep "$1"' "$3" "$4" </dev/null >/dev/null 2>&1 &
sleep 0.5
while pany '^Z:.prog.'; do sleep 0.2; done
exit 0
END
    chmod +x "$1/proton"
    printf '"manifest"\n{\n  "commandline" "/proton %%verb%%"\n}\n' > "$1/toolmanifest.vdf"
}
make_proton "$data/runners/proton/Fake-A"
make_proton "$data/runners/proton/Fake-B"
exe=$tmp/prog.exe
printf 'MZ' > "$exe"
# start NAME SECONDS PROTON: a program in its own process group, as LSIW
# Control and file managers start them.
start() {
    LSIW_PROTON_UMU=0 setsid "$LSIW" run --engine=proton:"$3" "$exe" "$1" "$2" \
        </dev/null >"$tmp/$1.out" 2>"$tmp/$1.err" &
    echo $! > "$tmp/$1.pid"
}
running() { pany "^Z:.prog.$1.exe" && echo yes || echo no; }
wait_for() {   # NAME: wait until its program runs
    for _ in $(seq 1 50); do [ "$(running "$1")" = yes ] && return 0; sleep 0.1; done
    return 1
}

start first 4 Fake-A
wait_for first || fail "first program starts"
same "first program waits for nothing" "$(grep '^Fake-A' "$tmp/verbs" | head -1 | cut -d' ' -f2)" waitforexitandrun
t0=$(date +%s)
start second 15 Fake-B
wait_for second || fail "second program starts while the first runs"
same "second program joins (run)" "$(grep 'second' "$tmp/verbs" | cut -d' ' -f2)" run
same "second program uses the Proton that runs the prefix" "$(grep 'second' "$tmp/verbs" | cut -d' ' -f1)" Fake-A
same "the switch is explained" "$(grep -c 'in use by Fake-A' "$tmp/second.err")" 1

# The first one's program ends after 4 s; Proton's launcher would wait
# for the second one too, but the instance ends.
for _ in $(seq 1 100); do kill -0 "$(cat "$tmp/first.pid")" 2>/dev/null || break; sleep 0.1; done
t1=$(date +%s)
same "first instance ends with its program" "$(kill -0 "$(cat "$tmp/first.pid")" 2>/dev/null && echo running || echo ended)" ended
same "... long before the second one" "$([ $((t1 - t0)) -lt 12 ] && echo yes || echo "after $((t1 - t0)) s")" yes
same "second program still runs" "$(running second)" yes

# Stopping one instance (SIGTERM to its process group, as Stop does).
start third 60 Fake-A
wait_for third || fail "third program starts"
pid=$(cat "$tmp/third.pid")
kill -TERM -- "-$pid"
for _ in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
same "stopped instance ends" "$(kill -0 "$pid" 2>/dev/null && echo running || echo ended)" ended
same "its program is gone" "$(running third)" no
same "the other program keeps running" "$(running second)" yes
same "the wineserver keeps running" "$(pany "wineserver $tag" && echo yes || echo no)" yes

kill -TERM -- "-$(cat "$tmp/second.pid")"
for _ in $(seq 1 60); do [ "$(running second)" = no ] && break; sleep 0.1; done
same "second program stops too" "$(running second)" no

# Wine left behind with no program: `lsiw prefix running` shows it idle,
# `lsiw prefix stop` ends it, Wine's own processes first.
server_up() { pany "wineserver $tag" && echo yes || echo no; }
same "the wineserver is still there" "$(server_up)" yes
WINEPREFIX=$shared/pfx setsid bash -c 'exec -a "C:\\windows\\system32\\services.exe" sleep 1000 1' \
    </dev/null >/dev/null 2>&1 &
svc=$!
sleep 0.3
out=$("$LSIW" prefix running -v)
case $out in *"idle, only Wine's own processes"*services.exe*) ok "prefix running: idle, with services.exe" ;;
             *) fail "prefix running: idle, with services.exe" "$out" ;; esac
"$LSIW" prefix stop >/dev/null
same "prefix stop ends the wineserver" "$(server_up)" no
same "... and Wine's services" "$(kill -0 "$svc" 2>/dev/null && echo running || echo ended)" ended
out=$("$LSIW" prefix running)
same "nothing runs any more" "$out" "No Wine or Proton is running."

# Left behind after a program: ended by lsiw on its own, once nothing uses
# it, but not while a program still does.
unset LSIW_NO_REAP
export LSIW_REAP_GRACE=1
start fourth 1 Fake-A
wait_for fourth || fail "fourth program starts"
for _ in $(seq 1 100); do kill -0 "$(cat "$tmp/fourth.pid")" 2>/dev/null || break; sleep 0.1; done
for _ in $(seq 1 100); do [ "$(server_up)" = no ] && break; sleep 0.1; done
same "an idle wineserver ends after the last program" "$(server_up)" no
start fifth 1 Fake-A
wait_for fifth || fail "fifth program starts"
LSIW_NO_REAP=1 start sixth 12 Fake-A
wait_for sixth || fail "sixth program starts"
for _ in $(seq 1 100); do kill -0 "$(cat "$tmp/fifth.pid")" 2>/dev/null || break; sleep 0.1; done
sleep 5
same "a wineserver in use is left alone" "$(server_up)" yes
same "... and its program runs on" "$(running sixth)" yes
kill -TERM -- "-$(cat "$tmp/sixth.pid")"
for _ in $(seq 1 60); do [ "$(running sixth)" = no ] && break; sleep 0.1; done

if [ "$fails" -eq 0 ]; then echo "instances.sh: all tests passed"; else echo "instances.sh: $fails failed"; fi
[ "$fails" -eq 0 ]
