#!/bin/sh
# Rebuilds the broken variants of the sample projects in a scratch copy and records
# what gcc, clang, cmake and cargo print, as fixtures for tests/problem_matcher_test.cpp.
#   tests/projects/capture.sh [scratch-dir]
# Paths inside the captures are made relative to the scratch copy (`@ROOT@`), so the
# fixtures do not depend on where they were recorded.
set -u
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../fixtures/matchers"
scratch=${1:-${TMPDIR:-/tmp}/overheat-capture}
rm -rf "$scratch" && mkdir -p "$scratch" "$out"
cp -r "$here/c" "$here/cpp-cmake" "$here/rust-cargo" "$scratch/"
rel() { sed "s#$scratch#@ROOT@#g"; }

cd "$scratch/c"
make BROKEN=1 CC=gcc 2>&1 | rel > "$out/gcc_c.txt"
make BROKEN=1 CC=clang 2>&1 | rel > "$out/clang_c.txt"

cd "$scratch/cpp-cmake"
cmake -S . -B build/gcc -G Ninja -DBROKEN=ON -DCMAKE_CXX_COMPILER=g++ >/dev/null 2>&1
cmake --build build/gcc 2>&1 | rel > "$out/gcc_cpp.txt"
cmake -S . -B build/clang -G Ninja -DBROKEN=ON -DCMAKE_CXX_COMPILER=clang++ >/dev/null 2>&1
cmake --build build/clang 2>&1 | rel > "$out/clang_cpp.txt"
# A CMake error: a call to an unknown command in a temporary copy of the lists file.
mkdir -p "$scratch/cmake-error" && cp CMakeLists.txt "$scratch/cmake-error/"
printf 'no_such_command(foo)\nmessage(WARNING "sample warning")\n' >> "$scratch/cmake-error/CMakeLists.txt"
cd "$scratch/cmake-error" && cmake -S . -B build 2>&1 | rel > "$out/cmake_error.txt"

cd "$scratch/rust-cargo"
cargo build --features broken 2>&1 | rel > "$out/cargo_human.txt"
cargo build --features broken --message-format=json-diagnostic-rendered-ansi 2>/dev/null | rel > "$out/cargo_json.txt"
cargo clippy --message-format=json-diagnostic-rendered-ansi 2>/dev/null | rel > "$out/clippy_json.txt"
echo "fixtures written to $out"
