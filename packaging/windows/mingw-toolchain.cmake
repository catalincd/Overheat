# Cross-compile for Windows x86_64 with mingw-w64-gcc against the MSYS2 UCRT64
# sysroot produced by tools/win/fetch_deps.py. Pass -DOVERHEAT_WIN_SYSROOT=<dir>/ucrt64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# CMake re-reads this file for every try_compile, inside the same process, without
# the -D cache value. The ENV{} assignments below leak back into the main project,
# so remember the sysroot in the environment as well, or a try_compile pass would
# point pkg-config back at the default path.
if(NOT OVERHEAT_WIN_SYSROOT AND DEFINED ENV{OVERHEAT_WIN_SYSROOT})
    set(OVERHEAT_WIN_SYSROOT "$ENV{OVERHEAT_WIN_SYSROOT}")
endif()
if(NOT OVERHEAT_WIN_SYSROOT)
    set(OVERHEAT_WIN_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../build-win/deps/sysroot/ucrt64")
endif()
get_filename_component(OVERHEAT_WIN_SYSROOT "${OVERHEAT_WIN_SYSROOT}" ABSOLUTE)
set(ENV{OVERHEAT_WIN_SYSROOT} "${OVERHEAT_WIN_SYSROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OVERHEAT_WIN_SYSROOT)

set(CMAKE_FIND_ROOT_PATH ${OVERHEAT_WIN_SYSROOT} /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config: read only the sysroot's .pc files. They say prefix=/ucrt64, so the
# sysroot dir is the folder that contains ucrt64/.
get_filename_component(_oh_sysroot_parent "${OVERHEAT_WIN_SYSROOT}" DIRECTORY)
set(ENV{PKG_CONFIG_LIBDIR} "${OVERHEAT_WIN_SYSROOT}/lib/pkgconfig:${OVERHEAT_WIN_SYSROOT}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_oh_sysroot_parent}")
