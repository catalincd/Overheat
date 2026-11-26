#!/usr/bin/env bash
# Cross-build the Windows release on Linux and package it:
#   dist/Overheat-<ver>-win64-setup.exe   (NSIS installer)
#   dist/Overheat-<ver>-win64-portable.zip
# Needs: mingw-w64-gcc, cmake, ninja, glslc, python3, wine (runs makensis.exe), zip.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
VER=$(sed -n 's/^project(overheat VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
# OVERHEAT_BUILD_DIR moves the whole build tree (docker/build.sh uses build-docker/win).
B=${OVERHEAT_BUILD_DIR:-build-win}
DEPS=$B/deps
TOOLS=$B/tools
BUILD=$B/release
STAGE=$B/stage/Overheat
mkdir -p dist

[ -d "$DEPS/sysroot/ucrt64/lib" ] || python3 -I tools/win/fetch_deps.py "$DEPS"
[ -x "$TOOLS/sysroot/ucrt64/bin/makensis.exe" ] || python3 -I tools/win/fetch_deps.py "$TOOLS" nsis

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DOVERHEAT_MARCH=x86-64-v2 \
      -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw-toolchain.cmake \
      -DOVERHEAT_WIN_SYSROOT="$ROOT/$DEPS/sysroot/ucrt64"
cmake --build "$BUILD" -j"$(nproc)"
python3 -I packaging/windows/bundle.py "$BUILD" "$DEPS/sysroot/ucrt64" "$STAGE"

# makensis.exe under an isolated Wine prefix (never the user's ~/.wine).
export WINEPREFIX="$ROOT/$B/wineprefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
[ -d "$WINEPREFIX/drive_c" ] || wineboot -i >/dev/null 2>&1
winpath() { echo "Z:$(realpath "$1" | tr / '\\')"; }
OUT="dist/Overheat-$VER-win64-setup.exe"
wine "$TOOLS/sysroot/ucrt64/bin/makensis.exe" -V2 -NOCD \
     "-DVERSION=$VER" "-DSTAGE=$(winpath "$STAGE")" "-DOUTFILE=$(winpath dist)\\$(basename "$OUT")" \
     "$(winpath packaging/windows/overheat.nsi)"

rm -f "dist/Overheat-$VER-win64-portable.zip"
(cd "$B/stage" && zip -qr9 "$ROOT/dist/Overheat-$VER-win64-portable.zip" Overheat)
ls -la "$OUT" "dist/Overheat-$VER-win64-portable.zip"
