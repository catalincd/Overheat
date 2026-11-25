#!/usr/bin/env python3
"""Fetch prebuilt Windows (MSYS2 UCRT64) libraries for cross-compiling Overheat.

Resolves dependencies from the repository index, downloads each package, checks
its SHA-256 against the index and unpacks it into <out>/sysroot (the ucrt64/ tree).

usage: fetch_deps.py <out-dir> [package ...]   (default: Overheat's libraries)
"""
import hashlib, io, os, re, sys, tarfile, urllib.request

REPO = "https://repo.msys2.org/mingw/ucrt64/"
PREFIX = "mingw-w64-ucrt-x86_64-"
ROOTS = ["sdl3", "freetype", "harfbuzz", "libgit2", "libssh2", "libtree-sitter",
         "vulkan-loader", "vulkan-headers", "zlib", "glm"]
# Toolchain/runtime pieces we already have from the cross compiler.
SKIP = {"crt", "headers", "winpthreads", "libwinpthread", "gcc", "binutils", "pkgconf", "python"}


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "overheat-build"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def parse_db(data):
    pkgs, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        for m in tf.getmembers():
            if not m.name.endswith("/desc"):
                continue
            fields, key = {}, None
            for line in tf.extractfile(m).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line[1:-1]
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields["NAME"][0]
            pkgs[name] = fields
            for p in fields.get("PROVIDES", []):
                provides.setdefault(re.split("[<>=]", p)[0], name)
    return pkgs, provides


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-win/deps")
    cache = os.path.join(out, "cache")
    sysroot = os.path.join(out, "sysroot")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(sysroot, exist_ok=True)
    pkgs, provides = parse_db(fetch(REPO + "ucrt64.db"))

    roots = sys.argv[2:] or ROOTS
    want, queue = [], [PREFIX + r for r in roots]
    while queue:
        name = re.split("[<>=]", queue.pop())[0]
        name = name if name in pkgs else provides.get(name, name)
        short = name[len(PREFIX):] if name.startswith(PREFIX) else name
        if name in want or short in SKIP or short.startswith("gcc"):
            continue
        if name not in pkgs:
            print("  (not in repo, skipped)", name)
            continue
        want.append(name)
        queue.extend(pkgs[name].get("DEPENDS", []))

    # gcc-libs supplies libstdc++/libgcc DLLs some packages link against.
    for extra in [] if sys.argv[2:] else (PREFIX + "gcc-libs", PREFIX + "libwinpthread"):
        if extra in pkgs and extra not in want:
            want.append(extra)

    for name in sorted(want):
        f = pkgs[name]
        fn, sha = f["FILENAME"][0], f["SHA256SUM"][0]
        path = os.path.join(cache, fn)
        if not os.path.exists(path):
            data = fetch(REPO + fn)
            if hashlib.sha256(data).hexdigest() != sha:
                sys.exit("checksum mismatch: " + fn)
            with open(path, "wb") as fh:
                fh.write(data)
        print("  %-60s %s" % (name, f["VERSION"][0]))
        with tarfile.open(path) as tf:  # Python 3.14 reads .zst
            members = [m for m in tf.getmembers()
                       if m.name.startswith("ucrt64/") and (m.isfile() or m.isdir() or m.issym())]
            tf.extractall(sysroot, members=members, filter="data")
    print("sysroot:", os.path.join(sysroot, "ucrt64"))


if __name__ == "__main__":
    main()
