# Overheat build environment: portable Linux packages (tarball, .deb, AppImage).
# Ubuntu 24.04 = glibc 2.39, so the packages it produces run on 24.04 / Debian 13 / Fedora 40
# and anything newer. Used by docker/build.sh linux; the source tree is bind-mounted, not copied.
FROM docker.io/library/ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++-14 cmake ninja-build pkg-config git ca-certificates curl file \
      glslc libglm-dev libvulkan-dev \
      libfreetype-dev libharfbuzz-dev libfontconfig-dev libgit2-dev libssh2-1-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
      libxtst-dev libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev \
      libdbus-1-dev libudev-dev libpipewire-0.3-dev libpulse-dev \
      dpkg-dev python3 desktop-file-utils \
 && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-14 CXX=g++-14

# SDL3 (24.04 does not package it). Installed to /usr/local; the packager bundles it.
ARG SDL_TAG=release-3.4.20
RUN git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL /tmp/SDL \
 && cmake -S /tmp/SDL -B /tmp/SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_RPATH=OFF \
 && cmake --build /tmp/SDL/build && cmake --install /tmp/SDL/build && ldconfig \
 && rm -rf /tmp/SDL

# appimagetool, unpacked so it runs without FUSE.
ARG APPIMAGETOOL_VERSION=1.9.0
RUN curl -fsSL -o /tmp/appimagetool \
      "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage" \
 && chmod +x /tmp/appimagetool && cd /opt && /tmp/appimagetool --appimage-extract >/dev/null \
 && mv /opt/squashfs-root /opt/appimagetool \
 && printf '#!/bin/sh\nexec /opt/appimagetool/AppRun "$@"\n' > /usr/local/bin/appimagetool \
 && chmod +x /usr/local/bin/appimagetool \
 && rm /tmp/appimagetool

# Runs as the host user (docker/build.sh passes --user), who has no home inside the image.
ENV HOME=/tmp/home
RUN mkdir -m 1777 /tmp/home && git config --system --add safe.directory '*'
