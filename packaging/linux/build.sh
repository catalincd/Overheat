#!/usr/bin/env bash
# Build the self-contained Linux release and package it:
#   dist/overheat-<ver>-linux-x86_64.tar.gz   (with install.sh / uninstall.sh)
#   dist/overheat_<ver>_amd64.deb
#   dist/Overheat-<ver>-x86_64.AppImage       (only if appimagetool is installed)
# The Arch package is built separately: packaging/arch/build.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
VER=$(sed -n 's/^project(overheat VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
# OVERHEAT_BUILD_DIR moves the whole build tree (docker/build.sh uses build-docker/linux).
B=${OVERHEAT_BUILD_DIR:-build-linux}
BUILD=$B/release
STAGE=$B/stage/overheat-$VER-linux-x86_64
mkdir -p dist

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DOVERHEAT_MARCH=x86-64-v2 \
      -DOVERHEAT_PORTABLE=ON -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD" -j"$(nproc)"

# ---- staged prefix (bin/, lib/overheat/, share/) shared by all formats ----
rm -rf "$STAGE"
DESTDIR="$ROOT/$STAGE.tmp" cmake --install "$BUILD" >/dev/null
mv "$STAGE.tmp/usr" "$STAGE" && rm -rf "$STAGE.tmp"
python3 -I packaging/linux/bundle_libs.py "$STAGE" | tee $B/bundle.txt
GLIBC=$(sed -n 's/^requires glibc >= //p' $B/bundle.txt)

# ---- tarball ----
cp packaging/linux/install.sh packaging/linux/uninstall.sh "$STAGE/"
cp LICENSE README.md "$STAGE/"
tar -C "$(dirname "$STAGE")" -czf "dist/$(basename "$STAGE").tar.gz" "$(basename "$STAGE")"

# ---- .deb (private libraries under /usr/lib/overheat) ----
DEB=$B/deb
rm -rf "$DEB" && mkdir -p "$DEB/DEBIAN" "$DEB/usr"
cp -a "$STAGE/bin" "$STAGE/lib" "$STAGE/share" "$DEB/usr/"
mkdir -p "$DEB/usr/share/doc/overheat"
cp LICENSE assets/fonts/*.txt "$DEB/usr/share/doc/overheat/"
cat > "$DEB/DEBIAN/control" <<CONTROL
Package: overheat
Version: $VER
Section: editors
Priority: optional
Architecture: amd64
Maintainer: catalincd <https://github.com/catalincd>
Depends: libc6 (>= $GLIBC), libvulkan1, libfontconfig1, mesa-vulkan-drivers | vulkan-icd
Recommends: git, openssh-client, clangd
Installed-Size: $(du -sk "$DEB/usr" | cut -f1)
Homepage: https://github.com/catalincd/Overheat
Description: Fast GPU-accelerated code editor
 Overheat is a Vulkan-rendered code editor with tree-sitter highlighting,
 LSP support, an integrated terminal, Git integration and Remote-SSH.
CONTROL
cat > "$DEB/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -qtf /usr/share/icons/hicolor || true
POST
cp "$DEB/DEBIAN/postinst" "$DEB/DEBIAN/postrm"
chmod 755 "$DEB/DEBIAN/postinst" "$DEB/DEBIAN/postrm"
find "$DEB/usr" -type d -exec chmod 755 {} +
dpkg-deb --root-owner-group -Zxz --build "$DEB" "dist/overheat_${VER}_amd64.deb" >/dev/null

# ---- AppImage ----
if command -v appimagetool >/dev/null; then
    APPDIR=$B/Overheat.AppDir
    rm -rf "$APPDIR" && mkdir -p "$APPDIR"
    cp -a "$STAGE/bin" "$STAGE/lib" "$STAGE/share" "$APPDIR/" 2>/dev/null
    mkdir -p "$APPDIR/usr" && mv "$APPDIR/bin" "$APPDIR/lib" "$APPDIR/share" "$APPDIR/usr/"
    ln -s usr/bin/overheat "$APPDIR/AppRun"
    cp packaging/linux/overheat.desktop "$APPDIR/"
    cp assets/logo/overheat-256.png "$APPDIR/overheat.png"
    ln -s overheat.png "$APPDIR/.DirIcon"
    ARCH=x86_64 appimagetool --no-appstream "$APPDIR" "dist/Overheat-$VER-x86_64.AppImage"
else
    echo "appimagetool not found: skipping the AppImage"
fi
ls -la dist/ | grep -iE "linux|deb|AppImage"
