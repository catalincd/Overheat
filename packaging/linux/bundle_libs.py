#!/usr/bin/env python3
"""Copy the shared libraries a binary needs into <prefix>/lib/overheat, except the
ones that must come from the host system (glibc, the Vulkan loader, fontconfig).
Prints the highest GLIBC symbol version the bundle requires.

usage: bundle_libs.py <prefix>   (expects <prefix>/bin/overheat)
"""
import os, re, shutil, subprocess, sys

prefix = os.path.abspath(sys.argv[1])
exe = os.path.join(prefix, "bin", "overheat")
libdir = os.path.join(prefix, "lib", "overheat")
os.makedirs(libdir, exist_ok=True)

# Host-provided: the C library family, the GPU driver loader, and fontconfig (its
# config files and caches belong to the host's version).
HOST = re.compile(r"^(linux-vdso|ld-linux[^.]*|libc|libm|libdl|libpthread|librt|libresolv|libutil|"
                  r"libvulkan|libfontconfig)\.so")

out = subprocess.run(["ldd", exe], capture_output=True, text=True, check=True).stdout
bundled = []
for line in out.splitlines():
    m = re.match(r"\s*(\S+)\s+=>\s+(\S+)", line)
    if not m:
        continue
    name = os.path.basename(m.group(1))
    if HOST.match(name) or not os.path.exists(m.group(2)):
        continue
    shutil.copy2(os.path.realpath(m.group(2)), os.path.join(libdir, name))
    os.chmod(os.path.join(libdir, name), 0o755)
    bundled.append(name)

subprocess.run(["strip", "--strip-unneeded", exe, *[os.path.join(libdir, b) for b in bundled]], check=False)

need = set()
for f in [exe] + [os.path.join(libdir, b) for b in bundled]:
    syms = subprocess.run(["objdump", "-T", f], capture_output=True, text=True).stdout
    need.update(re.findall(r"GLIBC_([0-9.]+)", syms))
# DT_RPATH, not DT_RUNPATH: only RPATH also covers the bundled libraries' own dependencies.
dyn = subprocess.run(["readelf", "-d", exe], capture_output=True, text=True, check=True).stdout
if "(RUNPATH)" in dyn or "$ORIGIN/../lib/overheat" not in dyn:
    sys.exit("error: bin/overheat needs DT_RPATH $ORIGIN/../lib/overheat (build with OVERHEAT_PORTABLE=ON)")
key = lambda v: tuple(int(x) for x in v.split("."))
print("bundled %d libraries: %s" % (len(bundled), " ".join(sorted(bundled))))
print("requires glibc >= %s" % max(need, key=key))
