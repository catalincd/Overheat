# Overheat build environment on Arch Linux:
#  - native development builds (cmake / ctest),
#  - the Arch package (packaging/arch/build.sh, makepkg),
#  - the Windows cross-build: MinGW-w64 + MSYS2 UCRT64 libraries + NSIS under Wine
#    (packaging/windows/build.sh).
# Used by docker/build.sh arch|windows|dev; the source tree is bind-mounted, not copied.
FROM docker.io/library/archlinux:base-devel
# Wine lives in [multilib].
RUN printf '\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n' >> /etc/pacman.conf \
 && pacman -Syu --noconfirm --needed \
      git cmake ninja pkgconf python zip shaderc glm vulkan-headers vulkan-icd-loader \
      sdl3 freetype2 harfbuzz fontconfig libgit2 libssh2 \
      mingw-w64-gcc wine \
 && pacman -Scc --noconfirm

# Runs as the host user (docker/build.sh passes --user), who has no home inside the image.
# makepkg refuses to run as root, which this also satisfies.
ENV HOME=/tmp/home WINEDEBUG=-all
RUN mkdir -m 1777 /tmp/home && git config --system --add safe.directory '*'
