#!/usr/bin/env bash
# Run the LSIW Windows test programs (tests/bin/*.exe, built by tests/Makefile)
# and summarise the results.
#
#   tests/run-tests.sh                  every test, with 'lsiw run --engine=native'
#   tests/run-tests.sh t_crt t_files    only these tests (".exe" optional)
#   tests/run-tests.sh --wine           the same binaries under plain Wine
#   tests/run-tests.sh --list           list the tests
#
# LSIW binary: $LSIW, else the repository's ./lsiw or build/lsiw, else 'lsiw'
# in PATH. $LSIW_CMD replaces the whole command prefix (for example a
# development runner: LSIW_CMD=/path/to/runner). Wine: $WINE, else wine.
# Timeout per test: $TEST_TIMEOUT seconds (default 60).
#
# A test passes when its output contains "RESULT: PASS" and it exits with
# status 0 (hello.exe, which has no checks, must print its greeting).
# Logs go to tests/out/NAME.txt (NAME.wine.txt with --wine).
# Exit status: the number of failing tests.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$here")
bin="$here/bin"
out="$here/out"
timeout_s=${TEST_TIMEOUT:-60}
mode=native
names=()

usage() { sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

for a in "$@"; do
    case "$a" in
        --wine) mode=wine ;;
        --native) mode=native ;;
        --list) for f in "$bin"/*.exe; do [ -e "$f" ] && basename "$f" .exe; done; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "run-tests.sh: unknown option '$a'" >&2; usage >&2; exit 2 ;;
        *) names+=("${a%.exe}") ;;
    esac
done

if [ ! -d "$bin" ] || ! ls "$bin"/*.exe >/dev/null 2>&1; then
    echo "run-tests.sh: no test programs in $bin; build them first: make -C '$here'" >&2
    exit 2
fi

# The command that runs a Windows program.
runner=()
if [ "$mode" = wine ]; then
    wine_bin=${WINE:-$(command -v wine || command -v wine64 || true)}
    if [ -z "$wine_bin" ]; then echo "run-tests.sh: wine not found (set WINE=...)" >&2; exit 2; fi
    runner=("$wine_bin")
    export WINEDEBUG=${WINEDEBUG:--all}
    # Wine converts file names with the Unix locale's charset; in the C/POSIX
    # locale non-ASCII names cannot be created.
    case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
        *UTF-8*|*utf8*|*UTF8*|*utf-8*) ;;
        *) export LC_ALL=C.UTF-8 ;;
    esac
    engine_desc="wine ($("$wine_bin" --version 2>/dev/null | head -n1))"
elif [ -n "${LSIW_CMD:-}" ]; then
    read -r -a runner <<< "$LSIW_CMD"
    engine_desc="$LSIW_CMD"
else
    lsiw=${LSIW:-}
    if [ -z "$lsiw" ]; then
        for c in "$root/lsiw" "$root/build/lsiw"; do
            if [ -x "$c" ]; then lsiw=$c; break; fi
        done
    fi
    [ -z "$lsiw" ] && lsiw=$(command -v lsiw || true)
    if [ -z "$lsiw" ] || [ ! -x "$lsiw" ]; then
        echo "run-tests.sh: lsiw not found: build it ('make' in $root), set LSIW=/path/to/lsiw, or use --wine" >&2
        exit 2
    fi
    runner=("$lsiw" run --engine=native)
    engine_desc="$lsiw run --engine=native"
fi

# Extra arguments some tests check (see the comment at the top of each test).
test_args() {
    case "$1" in
        t_env) printf '%s\0' alpha "two words" 'with"quote' 'back\slash' 'trail\' '' ;;
    esac
}

if [ ${#names[@]} -eq 0 ]; then
    for f in "$bin"/*.exe; do names+=("$(basename "$f" .exe)"); done
fi

have_timeout=$(command -v timeout || true)
mkdir -p "$out"
suffix=$([ "$mode" = wine ] && echo .wine || echo "")
echo "Engine: $engine_desc"
echo

fails=0
rows=()
for name in "${names[@]}"; do
    exe="$bin/$name.exe"
    log="$out/$name$suffix.txt"
    if [ ! -f "$exe" ]; then
        rows+=("$(printf '%-14s %-8s %5s %5s %7s  %s' "$name" MISSING - - - "no $exe (not built?)")")
        fails=$((fails + 1))
        continue
    fi
    args=()
    while IFS= read -r -d '' x; do args+=("$x"); done < <(test_args "$name")
    start=$(date +%s.%N)
    tcmd=()
    [ -n "$have_timeout" ] && tcmd=(timeout -k 5 "$timeout_s")
    # An inner shell runs the program, so a crash ("Segmentation fault") is
    # reported in the log instead of on the terminal.
    bash -c 'cd "$0" || exit 125; "$@" </dev/null' "$here" "${tcmd[@]}" "${runner[@]}" "$exe" "${args[@]}" > "$log" 2>&1
    rc=$?
    secs=$(awk -v s="$start" -v e="$(date +%s.%N)" 'BEGIN { printf "%.1f", e - s }')
    ok=$(grep -c '^ok - ' "$log")
    bad=$(grep -c '^FAIL - ' "$log")
    note=""
    if [ "$name" = hello ]; then
        if [ $rc -eq 0 ] && grep -q 'Hello from a Windows .exe' "$log"; then status=PASS; else status=FAIL; fi
    elif [ $rc -eq 0 ] && grep -q '^RESULT: PASS' "$log"; then
        status=PASS
    else
        status=FAIL
        if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then status=TIMEOUT; note="killed after ${timeout_s}s"
        elif m=$(grep -m1 -o '[A-Za-z0-9_.-]*\.dll![A-Za-z0-9_@?$]*, which the LSIW native engine does not implement' "$log"); then
            status=UNIMPL; note="stopped at ${m%%,*}"
        elif r=$(grep -m1 '^RESULT:' "$log"); then note="${r#RESULT: }, exit status $rc"
        else note="no RESULT line, exit status $rc"
        fi
        fails=$((fails + 1))
    fi
    rows+=("$(printf '%-14s %-8s %5s %5s %6ss  %s' "$name" "$status" "$ok" "$bad" "$secs" "$note")")
    printf '%-14s %s\n' "$name" "$status"
done

echo
printf '%-14s %-8s %5s %5s %7s  %s\n' TEST STATUS OK FAIL TIME NOTE
printf '%s\n' "${rows[@]}"
echo
echo "$(( ${#names[@]} - fails )) of ${#names[@]} tests passed; logs in $out"
exit $fails
