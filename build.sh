#!/bin/sh
# Builds GrooveMusicRadio.dll with llvm-mingw (clang). Usage: sh build.sh [output.dll]
set -e
cd "$(dirname "$0")"
OUT=${1:-GrooveMusicRadio.dll}
TMP=${TMPDIR:-/tmp}/grm-build
mkdir -p "$TMP"
# dr_libs.c: David Reid's decoders, built without our warnings.
clang --target=x86_64-w64-mingw32 -c -O2 -w -I src/third_party/dr_libs src/dr_libs.c -o "$TMP/dr_libs.o"
clang --target=x86_64-w64-mingw32 -c -std=c11 -O2 -Wall -Wextra -I src/third_party/dr_libs src/music_file.c -o "$TMP/music_file.o"
clang++ --target=x86_64-w64-mingw32 -std=c++20 -shared -O2 -Wall -Wextra -Wno-missing-field-initializers \
    -DUNICODE -D_UNICODE -I sdk -I src src/GrooveMusicRadio.cpp src/library.cpp src/reader.cpp src/live.cpp src/spotify.cpp src/apps.cpp src/capture.cpp "$TMP/dr_libs.o" "$TMP/music_file.o" \
    -static -lole32 -lshell32 -lpropsys -luuid -lwinhttp -lruntimeobject -lmmdevapi -o "$OUT"
