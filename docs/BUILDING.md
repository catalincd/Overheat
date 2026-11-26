# Building Overheat

Overheat is a CMake project (C++20). tree-sitter and its grammars are vendored in
`third_party/`, and the shaders are compiled with `glslc` and embedded into the binary.
Everything else comes from system libraries.

| Dependency | Used for |
|---|---|
| SDL3 | window, input, clipboard |
| Vulkan loader + headers, `glslc` (shaderc) | rendering, shader compilation |
| FreeType, HarfBuzz | glyph rasterisation and shaping |
| fontconfig (Linux only) | listing installed fonts |
| libgit2 | Git integration |
| libssh2 | Remote-SSH and SFTP |
| GLM | math (header-only) |

## Linux

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake ninja shaderc glm vulkan-headers vulkan-icd-loader \
                        sdl3 freetype2 harfbuzz fontconfig libgit2 libssh2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/overheat
```

### Fedora (41+)

```bash
sudo dnf install cmake ninja-build gcc-c++ glslc glm-devel vulkan-headers vulkan-loader-devel \
                 SDL3-devel freetype-devel harfbuzz-devel fontconfig-devel libgit2-devel libssh2-devel
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

### Debian 13 / Ubuntu 25.04 and newer

```bash
sudo apt install build-essential cmake ninja-build glslc libglm-dev libvulkan-dev \
                 libsdl3-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev libgit2-dev libssh2-1-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Older releases don't package SDL3: use the container build ([Docker / Podman](#docker--podman)).

### Build options

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | (none) | `Release` for `-O3` + LTO, `Debug` for `-O0 -g -Wall` |
| `OVERHEAT_MARCH` | `native` | `-march` for optimised builds. Use `x86-64-v2` for binaries you hand to other people |
| `OVERHEAT_PORTABLE` | `OFF` | Static libstdc++ plus an rpath to `../lib/overheat`, used by the self-contained packages |

Debug builds also build `buffer_fuzz` (ASan + UBSan). Run the tests with `ctest --test-dir build`.

## Linux packages

```bash
packaging/linux/build.sh    # → dist/*.AppImage, *.tar.gz, *.deb
packaging/arch/build.sh     # → dist/*.pkg.tar.zst (from `git archive HEAD`)
```

`packaging/linux/build.sh` builds with `OVERHEAT_PORTABLE=ON`, installs into a staging prefix and
copies every shared library the binary needs into `lib/overheat/`. Only three libraries are left
to the host: glibc, the Vulkan loader (it must match the GPU driver) and fontconfig (it must match
the host's font configuration). The script prints the minimum glibc version of the result.

- The **tarball** contains `install.sh` (into `~/.local`, or `/usr/local` as root, or `--prefix DIR`)
  and an `uninstall.sh` that removes exactly what it installed.
- The **.deb** installs to `/usr` with the private libraries in `/usr/lib/overheat`.
- The **AppImage** is built only if `appimagetool` is on `PATH`.

## Docker / Podman

`docker/` holds two build environments and a wrapper that runs the packaging scripts inside them.
The host needs only Docker or Podman (Podman is preferred when both are installed; override with
`OVERHEAT_ENGINE=docker`).

| Command | Image | Output in `dist/` |
|---|---|---|
| `docker/build.sh linux` | `docker/linux` (Ubuntu 24.04, SDL3 from source, appimagetool) | `.AppImage`, `.tar.gz`, `.deb` |
| `docker/build.sh windows` | `docker/arch` (MinGW-w64, Wine) | `*-win64-setup.exe`, `*-win64-portable.zip` |
| `docker/build.sh arch` | `docker/arch` | `.pkg.tar.zst` |
| `docker/build.sh all` | both | all of the above |
| `docker/build.sh dev` | `docker/arch` | Debug build + `ctest` in `build-docker/dev` |
| `docker/build.sh shell [linux\|arch]` | either | interactive shell |

- Images are built on first use (`overheat-build-linux`, `overheat-build-arch`). Add `--rebuild` to
  pull fresh base images and rebuild them.
- The repository is bind-mounted at **its host path**, not copied, and the container runs as your
  user (`--user` for Docker, `--userns=keep-id` for Podman). Every file it writes belongs to you.
- Build trees go to `build-docker/<target>` and never touch the host's `build-*` directories. The
  packaging scripts take the location from `OVERHEAT_BUILD_DIR`.
- The Windows build still downloads the MSYS2 packages at build time (cached in
  `build-docker/win/deps`).
- To use the environment by hand: `docker/build.sh shell linux`, then run the same commands as on a
  native host.

### Older Linux distributions

The Linux packages inherit the glibc version of the machine they are built on. Anything built on
a rolling distribution needs an equally new glibc. `docker/build.sh linux` builds on
**Ubuntu 24.04 (glibc 2.39)**, so its packages run on Ubuntu 24.04+, Debian 13+, Fedora 40+ and
every rolling distro.

To go older, change the `FROM` line in `docker/linux/Dockerfile` to the oldest distribution you
want to support (for example `docker.io/library/debian:12` for glibc 2.36) and adjust the package names. Then run
`docker/build.sh linux --rebuild`. The script prints `requires glibc >= …`; check that it matches
the base image. You may need a newer GCC than the base image ships, because Overheat is C++20.

## Windows

The Windows build is **cross-compiled on Linux** with MinGW-w64 against prebuilt MSYS2 (UCRT64)
libraries:

```bash
sudo pacman -S --needed mingw-w64-gcc cmake ninja shaderc wine zip   # Arch host
packaging/windows/build.sh
# → dist/Overheat-<ver>-win64-setup.exe, dist/Overheat-<ver>-win64-portable.zip
```

What the script does:

1. `tools/win/fetch_deps.py` downloads the needed MSYS2 UCRT64 packages and their dependencies
   from repo.msys2.org, checks each one's SHA-256 against the repository index, and unpacks
   them into `build-win/deps/sysroot`. NSIS goes to a separate `build-win/tools`.
2. CMake configures with `packaging/windows/mingw-toolchain.cmake`, and the C++ runtime is linked
   statically.
3. `packaging/windows/bundle.py` stages `overheat.exe` with every DLL it imports (found by walking
   PE imports), the bundled font and the icon.
4. `makensis.exe` runs under Wine in an isolated prefix (`build-win/wineprefix`, never `~/.wine`) to
   build the installer from `packaging/windows/overheat.nsi`.

The installer is per-machine and needs admin rights. Optional components: Start menu and desktop
shortcuts, "Open with Overheat" in Explorer, and adding Overheat to `PATH`. It registers an
uninstaller in *Apps & features*. Silent install: `setup.exe /S [/D=C:\path]`.

### Native Windows (MSYS2)

You can also build on Windows itself in an MSYS2 **UCRT64** shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,shaderc,glm,vulkan-headers,vulkan-loader,sdl3,freetype,harfbuzz,libgit2,libssh2}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Smoke-testing with Wine

```bash
export WINEPREFIX=$PWD/build-win/wineprefix   # build-docker/win/wineprefix after a container build
wine build-win/stage/Overheat/overheat.exe
```

Wine's Vulkan layer forwards to your host driver. Wine's `powershell.exe` is a stub, so set
`terminal_shell = "cmd.exe"` in `%APPDATA%\Overheat\settings.toml` inside the prefix to try the
terminal.

## Project layout

| Path | Contents |
|---|---|
| `src/` | application code (see the README's *Under the hood*) |
| `shaders/` | GLSL for the quad renderer, compiled to SPIR-V and embedded |
| `cmake/` | build helpers (SPIR-V embedding) |
| `third_party/` | vendored tree-sitter core and C/C++ grammars |
| `assets/` | logo (SVG/PNG/ICO) and the bundled font |
| `packaging/` | Windows (NSIS, toolchain, bundler), Linux (packager, install scripts), Arch (PKGBUILD) |
| `docker/` | containerised build environments and `build.sh` wrapper |
| `tests/` | piece-table fuzz test |
| `tools/qa/` | scripted UI testing (`--script` mode, screenshot capture) |
