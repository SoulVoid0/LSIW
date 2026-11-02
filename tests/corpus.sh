#!/bin/sh
# Real-program corpus for the native engine. Usage: corpus.sh LSIW DIR
# DIR/realworld/x and DIR/realworld/dl2/x hold the unpacked upstream release
# archives (zstd, jq, ninja, ripgrep, fd, uutils, fzf, gh, python, cmake, bat,
# delta, lazygit, xsv, node, MinGit) at the paths used below.
L=$1; SP=$2; R=$SP/realworld; X=$R/x; D=$R/dl2/x
export HOME=$SP/fakehome
W=$(mktemp -d); cd $W
printf '{"a":[1,2,{"b":"x"}]}' > t.json; printf 'a,b\n1,2\n3,4\n' > t.csv
run() { name=$1; shift; out=$(timeout 60 $L run -e native "$@" 2>&1 | head -c 400 | tr '\n' ' '); echo "$name | $? | $out"; }
run zstd $X/zstd/zstd-v1.5.6-win64/zstd.exe -q -f t.json -o t.zst
run jq $X/jq/jq.exe -c .a t.json
run ninja $X/ninja/ninja.exe --version
run rg-msvc $X/rg-msvc/ripgrep-14.1.1-x86_64-pc-windows-msvc/rg.exe -c . t.csv
run rg-gnu $X/rg-gnu/ripgrep-14.1.1-x86_64-pc-windows-gnu/rg.exe -c . t.csv
run fd $X/fd-msvc/fd-v10.2.0-x86_64-pc-windows-msvc/fd.exe -e json
run coreutils $X/coreutils/coreutils-0.0.28-x86_64-pc-windows-msvc/coreutils.exe sha256sum t.json
run fzf $X/fzf/fzf.exe --version
run gh $X/gh/bin/gh.exe --version
run python $X/python/python/python.exe -c "import json,sqlite3,hashlib;print(hashlib.sha1(b'x').hexdigest()[:8],sqlite3.sqlite_version)"
run cmake $X/cmake/cmake-3.30.5-windows-x86_64/bin/cmake.exe -E sha256sum t.json
run bat $D/bat-v0.24.0-x86_64-pc-windows-msvc/bat-v0.24.0-x86_64-pc-windows-msvc/bat.exe -pp t.csv
run delta $D/delta-0.18.2-x86_64-pc-windows-msvc/delta-0.18.2-x86_64-pc-windows-msvc/delta.exe --version
run lazygit $D/lazygit_0.44.1_Windows_x86_64/lazygit.exe --version
run xsv $D/xsv-0.13.0-x86_64-pc-windows-msvc/xsv.exe count t.csv
run node $D/node/node-v20.18.0-win-x64/node-v20.18.0-win-x64/node.exe -e "console.log(require('fs').existsSync('t.json'), require('crypto').createHash('sha1').update('x').digest('hex').slice(0,8))"
G=$D/MinGit-2.47.0.2-64-bit/mingw64/bin/git.exe
run git-init $G init -q repo
cd repo; echo hi > a.txt
run git-add $G add a.txt
run git-commit $G -c user.name=t -c user.email=t@t commit -qm first
run git-log $G log --oneline
echo more >> a.txt
run git-stash $G stash -q
run git-stash-pop $G stash pop -q
run git-gc $G gc -q
run git-clone-https $G -c http.sslBackend=openssl clone -q --depth 1 https://github.com/octocat/Hello-World hw
ls hw 2>&1 | head -1
cd /; rm -rf $W
