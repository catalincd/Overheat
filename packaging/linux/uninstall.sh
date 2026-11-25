#!/usr/bin/env bash
# Remove an Overheat installed by install.sh. Settings in ~/.config/overheat are kept.
set -euo pipefail
LIST="$(cd "$(dirname "$0")" && pwd)/installed-files"
[ -f "$LIST" ] || { echo "no installed-files list next to this script"; exit 1; }
mapfile -t files < "$LIST"
for f in "${files[@]}"; do
    case "$f" in */overheat|*/overheat.desktop|*/overheat.png|*/overheat.svg) rm -rf -- "$f" ;; esac
done
# Directories go last (this script lives in share/overheat).
for f in "${files[@]}"; do
    case "$f" in */lib/overheat|*/share/overheat) rm -rf -- "$f" ;; esac
done
echo "Overheat removed."
