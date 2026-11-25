#!/usr/bin/env bash
# Build the Arch package from the committed tree (git archive HEAD):
#   dist/overheat-<ver>-<rel>-x86_64.pkg.tar.zst
# Install it with: sudo pacman -U dist/overheat-*.pkg.tar.zst
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
VER=$(sed -n 's/^project(overheat VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
WORK=build-arch
rm -rf "$WORK" && mkdir -p "$WORK" dist
git archive --format=tar.gz --prefix="overheat-$VER/" -o "$WORK/overheat-$VER.tar.gz" HEAD
sed "s/^pkgver=.*/pkgver=$VER/" packaging/arch/PKGBUILD > "$WORK/PKGBUILD"
SUM=$(sha256sum "$WORK/overheat-$VER.tar.gz" | cut -d' ' -f1)
sed -i "s/^sha256sums=.*/sha256sums=('$SUM')/" "$WORK/PKGBUILD"
(cd "$WORK" && PKGDEST="$ROOT/dist" makepkg -f --noconfirm >/dev/null)
ls -la dist/overheat-"$VER"-*.pkg.tar.zst
