#!/usr/bin/env python3
"""Stage a portable Windows build: overheat.exe + every DLL it needs from the
MSYS2 sysroot (found by walking PE imports), the bundled font and the icon.

usage: bundle.py <build-dir> <sysroot/ucrt64> <out-dir>
"""
import os, shutil, subprocess, sys

build, sysroot, out = (os.path.abspath(a) for a in sys.argv[1:4])
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
bindir = os.path.join(sysroot, "bin")
available = {f.lower(): f for f in os.listdir(bindir) if f.lower().endswith(".dll")}


def imports(path):
    dump = subprocess.run(["x86_64-w64-mingw32-objdump", "-p", path], capture_output=True, text=True).stdout
    return [l.split("DLL Name:")[1].strip() for l in dump.splitlines() if "DLL Name:" in l]


shutil.rmtree(out, ignore_errors=True)
os.makedirs(os.path.join(out, "fonts"))
shutil.copy2(os.path.join(build, "overheat.exe"), out)
seen, queue = set(), [os.path.join(out, "overheat.exe")]
while queue:
    for dll in imports(queue.pop()):
        key = dll.lower()
        if key in seen or key not in available:
            continue  # system DLL (kernel32, ucrt api sets, vulkan-1 comes from the GPU driver too)
        seen.add(key)
        dst = os.path.join(out, available[key])
        shutil.copy2(os.path.join(bindir, available[key]), dst)
        queue.append(dst)

# The Vulkan loader is installed with GPU drivers, but ship ours so a missing
# system loader still works (Windows prefers the app directory).
for f in ("JetBrainsMonoNerdFontMono-Regular.ttf", "JetBrainsMono-OFL.txt"):
    shutil.copy2(os.path.join(root, "assets", "fonts", f), os.path.join(out, "fonts"))
shutil.copy2(os.path.join(root, "assets", "logo", "overheat.ico"), out)
for f in ("LICENSE", "README.md", "TODO.md"):
    if os.path.exists(os.path.join(root, f)):
        shutil.copy2(os.path.join(root, f), out)
subprocess.run(["x86_64-w64-mingw32-strip", "--strip-unneeded", os.path.join(out, "overheat.exe")])
total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(out) for f in fs)
print(f"staged {len(seen)} DLLs, {total / 1e6:.1f} MB -> {out}")
for d in sorted(seen):
    print("  ", available[d])
