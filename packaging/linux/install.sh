#!/usr/bin/env bash
# Install Overheat from this folder.
#   ./install.sh            per-user, into ~/.local
#   sudo ./install.sh       system-wide, into /usr/local
#   ./install.sh --prefix DIR
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" = "--prefix" ]; then PREFIX=$2
elif [ "$(id -u)" = 0 ]; then PREFIX=/usr/local
else PREFIX="$HOME/.local"; fi

echo "Installing Overheat into $PREFIX"
install -Dm755 "$HERE/bin/overheat" "$PREFIX/bin/overheat"
mkdir -p "$PREFIX/lib/overheat" "$PREFIX/share/overheat"
cp -a "$HERE/lib/overheat/." "$PREFIX/lib/overheat/"
cp -a "$HERE/share/overheat/." "$PREFIX/share/overheat/"
install -Dm644 "$HERE/share/applications/overheat.desktop" "$PREFIX/share/applications/overheat.desktop"
for d in "$HERE"/share/icons/hicolor/*/apps; do
    size=$(basename "$(dirname "$d")")
    for f in "$d"/*; do install -Dm644 "$f" "$PREFIX/share/icons/hicolor/$size/apps/$(basename "$f")"; done
done
# Record what we installed so uninstall.sh removes exactly that.
{
    echo "$PREFIX/bin/overheat"
    echo "$PREFIX/lib/overheat"
    echo "$PREFIX/share/overheat"
    echo "$PREFIX/share/applications/overheat.desktop"
    find "$HERE/share/icons" -type f | sed "s|^$HERE|$PREFIX|"
} > "$PREFIX/share/overheat/installed-files"
install -Dm755 "$HERE/uninstall.sh" "$PREFIX/share/overheat/uninstall.sh"

command -v update-desktop-database >/dev/null && update-desktop-database -q "$PREFIX/share/applications" || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor" 2>/dev/null || true
echo "Done. Run 'overheat' (make sure $PREFIX/bin is on your PATH)."
echo "Uninstall with: $PREFIX/share/overheat/uninstall.sh"
