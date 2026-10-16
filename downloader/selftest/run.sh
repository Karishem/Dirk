#!/bin/bash
# Dirk Downloader self-test (Linux, Wine + MinGW cross-compiler): the real
# download/update code against a fake megatools and a fake Mega store.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
CXX="x86_64-w64-mingw32-g++ -std=c++17 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++"
$CXX -mconsole "$HERE/fake_megatools.cpp" -o "$W/megatools.exe"
$CXX -mconsole "$HERE/downloader_selftest.cpp" -lcomctl32 -lshell32 -lole32 -ladvapi32 -o "$W/downloader_selftest.exe"
python3 "$HERE/make_store.py" "$W"
# A zip64 .zdirk (zip -fz), for the shortcut test.
(cd "$W" && mkdir z64 && printf -- '-- a' > z64/Z64.dirk && printf -- '-- b' > z64/Z64B.dirk && cd z64 && zip -q -fz ../z64.zdirk Z64.dirk Z64B.dirk)
cd "$W" && WINEDEBUG=-all wine downloader_selftest.exe
