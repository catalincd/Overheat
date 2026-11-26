#!/bin/sh
# Regenerate the logo PNGs and the Windows .ico from the SVGs.
# Needs rsvg-convert (librsvg) and ImageMagick 7 (magick).
set -e
cd "$(dirname "$0")"
for size in 16 24; do
    rsvg-convert -w $size -h $size overheat-small.svg -o overheat-$size.png
done
for size in 32 48 64 128 256 512; do
    rsvg-convert -w $size -h $size overheat.svg -o overheat-$size.png
done
magick overheat-16.png overheat-24.png overheat-32.png overheat-48.png \
       overheat-64.png overheat-128.png overheat-256.png overheat.ico
