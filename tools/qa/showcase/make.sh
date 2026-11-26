#!/usr/bin/env bash
# Regenerates docs/screenshots/themes.webp: the README slideshow of C, C++ and Rust in
# twelve themes. Runs the app hidden through tools/qa/drive.py (isolated config, sh as
# the shell, a scratch copy of src/ so no personal paths show), captions each frame and
# assembles an animated WebP. Needs: a built ./build/overheat, clangd, ImageMagick.
#   tools/qa/showcase/make.sh [scratch dir]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SC=${1:-$(mktemp -d)}
D=$SC/overheat-demo
rm -rf "$D" "$SC/f" "$SC/c" && mkdir -p "$D" "$SC/f" "$SC/c"
cp -r "$ROOT/tools/qa/showcase/src" "$D/"
# Per-language flags so clangd shows no false errors (C17 / C++20).
python3 -I - "$D" <<'EOF'
import json, sys
d = sys.argv[1]
json.dump([{"directory": d, "file": d + "/src/ring.c", "arguments": ["cc", "-std=c17", "-c", "src/ring.c"]},
           {"directory": d, "file": d + "/src/scene.cpp", "arguments": ["c++", "-std=c++20", "-c", "src/scene.cpp"]}],
          open(d + "/compile_commands.json", "w"), indent=1)
EOF

FRAMES=(
  "Overheat Dark|scene.cpp|C++"   "Gruvbox Dark|main.rs|Rust"      "Cyberpunk Neon|scene.cpp|C++"
  "Tokyo Night|ring.c|C"          "Catppuccin Mocha|main.rs|Rust"  "Dracula|scene.cpp|C++"
  "One Dark Pro|ring.c|C"         "Nord|main.rs|Rust"              "GitHub Light|scene.cpp|C++"
  "Gruvbox Light|ring.c|C"        "Solarized Light|main.rs|Rust"   "Overheat Light|scene.cpp|C++"
)
FONT=$ROOT/assets/fonts/JetBrainsMonoNerdFontMono-Regular.ttf
i=0
for f in "${FRAMES[@]}"; do
    IFS='|' read -r theme file lang <<<"$f"
    i=$((i + 1)); n=$(printf %02d $i); o=$SC/run$n
    rm -rf "$o" && mkdir -p "$o/config/overheat"
    printf 'theme = "%s"\nterminal_shell = "sh"\ninlay_hints = "false"\n' "$theme" > "$o/config/overheat/settings.toml"
    # Wait for clangd, hide the terminal (first Ctrl+` focuses it), expand src/, put
    # the cursor inside a scope so the scope highlight shows.
    printf 'wait 3500\nkey ctrl+`\nwait 200\nkey ctrl+`\nwait 400\nclick 110 76\nwait 300\nclick 900 420\nwait 1500\nprobe shot:s\nquit\n' > "$o/s.txt"
    OVERHEAT_BIN=${OVERHEAT_BIN:-$ROOT/build/overheat} QA_CWD=$D \
        python3 "$ROOT/tools/qa/drive.py" 1400 820 "$o/s.txt" "$o" "$D/src/$file" >/dev/null
    magick "$o/s.png" -resize 1120x -gravity SouthEast -font "$FONT" -pointsize 15 \
        -fill '#00000099' -draw "roundrectangle %[fx:w-310],%[fx:h-62],%[fx:w-14],%[fx:h-30],8,8" \
        -fill white -annotate +26+38 "$theme · $lang" "$SC/c/$n.png"
    echo "frame $n: $theme · $lang"
done
magick -delay 250 -loop 0 "$SC"/c/*.png -quality 82 -define webp:method=6 "$ROOT/docs/screenshots/themes.webp"
ls -la "$ROOT/docs/screenshots/themes.webp"
