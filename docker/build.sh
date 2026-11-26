#!/usr/bin/env bash
# Build Overheat inside a container, so the host needs only Docker or Podman.
#
#   docker/build.sh linux      tarball + .deb + AppImage   (Ubuntu 24.04 image, glibc 2.39)
#   docker/build.sh windows    setup.exe + portable zip    (Arch image: MinGW-w64 + Wine/NSIS)
#   docker/build.sh arch       .pkg.tar.zst                (Arch image, makepkg)
#   docker/build.sh all        all of the above
#   docker/build.sh dev        Debug build + tests (ASan fuzz) into build-docker/dev (Arch image)
#   docker/build.sh shell [linux|arch]   interactive shell in an image
#
# Packages land in dist/. Build trees go to build-docker/<target>, apart from the host's build dirs.
# The repository is mounted at its host path and the container runs as your user, so every file
# it writes belongs to you. Images are built on first use; pass --rebuild to refresh them.
# Set OVERHEAT_ENGINE=docker|podman to choose an engine when both are installed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

REBUILD=0
ARGS=()
for a in "$@"; do [ "$a" = --rebuild ] && REBUILD=1 || ARGS+=("$a"); done
set -- "${ARGS[@]}"
TARGET=${1:-}
case $TARGET in linux|windows|arch|all|dev|shell) ;; *) sed -n '2,15s/^# \{0,1\}//p' "$0"; exit 2 ;; esac

ENGINE=${OVERHEAT_ENGINE:-$(command -v podman >/dev/null && echo podman || echo docker)}
command -v "$ENGINE" >/dev/null || { echo "error: neither podman nor docker is installed" >&2; exit 1; }

image() {  # image <linux|arch>: build it if missing (or --rebuild), print its tag
    local tag="overheat-build-$1"
    if [ "$REBUILD" = 1 ] || ! "$ENGINE" image inspect "$tag" >/dev/null 2>&1; then
        echo ">> building image $tag" >&2
        local pull=(); [ "$REBUILD" = 1 ] && pull=(--pull)
        "$ENGINE" build "${pull[@]}" -t "$tag" "$ROOT/docker/$1" >&2
    fi
    echo "$tag"
}

run() {  # run <image> <interactive 0|1> <command...>
    local img=$1 tty=$2; shift 2
    local opts=(--rm -v "$ROOT:$ROOT" -w "$ROOT" -e OVERHEAT_BUILD_DIR)
    if [ "$ENGINE" = podman ]; then
        opts+=(--userns=keep-id --security-opt label=disable)
    else
        opts+=(--user "$(id -u):$(id -g)")
    fi
    [ "$tty" = 1 ] && opts+=(-it)
    "$ENGINE" run "${opts[@]}" "$img" "$@"
}

build() {  # build <target>
    case $1 in
        linux)   OVERHEAT_BUILD_DIR=build-docker/linux run "$(image linux)" 0 packaging/linux/build.sh ;;
        windows) OVERHEAT_BUILD_DIR=build-docker/win   run "$(image arch)"  0 packaging/windows/build.sh ;;
        arch)    OVERHEAT_BUILD_DIR=build-docker/arch  run "$(image arch)"  0 packaging/arch/build.sh ;;
        dev)     run "$(image arch)" 0 bash -c '
                     cmake -S . -B build-docker/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug &&
                     cmake --build build-docker/dev && ctest --test-dir build-docker/dev --output-on-failure' ;;
        *) return 1 ;;
    esac
}

case $TARGET in
    linux|windows|arch|dev) build "$TARGET" ;;
    all) for t in linux windows arch; do echo "== $t"; build "$t"; done ;;
    shell) img=${2:-arch}; [[ $img = linux || $img = arch ]] || { echo "usage: $0 shell [linux|arch]" >&2; exit 2; }
           run "$(image "$img")" 1 bash ;;
esac
