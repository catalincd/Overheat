cmake_minimum_required(VERSION 3.20)
project(overheat VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release builds for distribution set OVERHEAT_MARCH to a portable baseline (x86-64-v2).
set(OVERHEAT_MARCH "native" CACHE STRING "-march for optimized builds")

find_package(PkgConfig REQUIRED)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
if(NOT WIN32)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
endif()

# tree-sitter core + grammars are vendored so every platform builds them the same way
# (and packages need no tree-sitter runtime; distro/MSYS2 builds differ in ABI and pull wasmtime).
add_library(tree_sitter STATIC third_party/tree-sitter/src/lib.c)
target_include_directories(tree_sitter
    PUBLIC third_party/tree-sitter/include
    PRIVATE third_party/tree-sitter/src)
set_target_properties(tree_sitter PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(tree_sitter PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE)
target_compile_options(tree_sitter PRIVATE -O2 -w)

# Each grammar lives in third_party/tree-sitter-<name>/ (parser.c, optional scanner.c,
# its own tree_sitter/*.h) and is registered in the GRAMMARS table in src/editor/syntax.cpp.
set(TS_GRAMMARS c cpp rust)
add_library(ts_grammars STATIC)
foreach(grammar ${TS_GRAMMARS})
    set(dir ${CMAKE_SOURCE_DIR}/third_party/tree-sitter-${grammar})
    set(srcs ${dir}/parser.c)
    if(EXISTS ${dir}/scanner.c)
        list(APPEND srcs ${dir}/scanner.c)
    endif()
    target_sources(ts_grammars PRIVATE ${srcs})
    # Per-file include dir: every grammar ships its own (possibly different) tree_sitter/parser.h.
    set_source_files_properties(${srcs} PROPERTIES INCLUDE_DIRECTORIES ${dir})
endforeach()
set_target_properties(ts_grammars PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(ts_grammars PRIVATE -O2 -w)

find_program(GLSLC glslc REQUIRED)

file(GLOB SHADER_SOURCES "${CMAKE_SOURCE_DIR}/shaders/*.glsl")
set(SPIRV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPIRV_OUTPUT_DIR})

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_FULL ${SHADER} NAME)
    string(REGEX MATCH "(vert|frag|comp)" SHADER_STAGE ${SHADER_FULL})
    string(REGEX REPLACE "\\.glsl$" ".spv" SPIRV_NAME ${SHADER_FULL})
    set(SPIRV_FILE "${SPIRV_OUTPUT_DIR}/${SPIRV_NAME}")
    add_custom_command(
        OUTPUT ${SPIRV_FILE}
        COMMAND ${GLSLC} -fshader-stage=${SHADER_STAGE} -O ${SHADER} -o ${SPIRV_FILE}
        DEPENDS ${SHADER} ${CMAKE_SOURCE_DIR}/shaders/quad_common.glsl.inc ${CMAKE_SOURCE_DIR}/shaders/quad_frag.glsl.inc
        COMMENT "Compiling ${SHADER_FULL} -> ${SPIRV_NAME}"
    )
    list(APPEND SPIRV_FILES ${SPIRV_FILE})
endforeach()

set(SPIRV_HEADER "${SPIRV_OUTPUT_DIR}/spirv.h")
string(REPLACE ";" "," SPIRV_FILES_ARG "${SPIRV_FILES}")
add_custom_command(
    OUTPUT ${SPIRV_HEADER}
    COMMAND ${CMAKE_COMMAND} "-DINPUTS=${SPIRV_FILES_ARG}" -DOUT=${SPIRV_HEADER} -P ${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake
    DEPENDS ${SPIRV_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding SPIR-V"
    VERBATIM
)
add_custom_target(shaders ALL DEPENDS ${SPIRV_HEADER})

# File-type icon font (assets/icons, built by build_icons.py and committed), embedded so
# no install path is needed to find it.
set(ICON_FONT_HEADER "${CMAKE_BINARY_DIR}/generated/overheat_icons_ttf.h")
add_custom_command(
    OUTPUT ${ICON_FONT_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/assets/icons/OverheatIcons.ttf -DOUT=${ICON_FONT_HEADER}
            -DVAR=overheat_icons_ttf -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/icons/OverheatIcons.ttf ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
    COMMENT "Embedding the icon font"
    VERBATIM
)
add_custom_target(icon_font ALL DEPENDS ${ICON_FONT_HEADER})

set(OVERHEAT_SOURCES
    src/main.cpp
    src/app.cpp
    src/renderer/vulkan_context.cpp
    src/renderer/font_atlas.cpp
    src/renderer/renderer2d.cpp
    src/editor/buffer.cpp
    src/editor/editor.cpp
    src/editor/cursor.cpp
    src/editor/syntax.cpp
    src/ui/panel.cpp
    src/ui/editor_view.cpp
    src/ui/cursor_fx.cpp
    src/ui/scope_highlight.cpp
    src/ui/file_gate_card.cpp
    src/ui/file_tree.cpp
    src/ui/terminal.cpp
    src/ui/status_bar.cpp
    src/ui/command_center.cpp
    src/ui/monitor_bar.cpp
    src/ui/tab_bar.cpp
    src/ui/file_icons.cpp
    src/ui/file_icon_map.cpp
    src/ui/command_palette.cpp
    src/ui/scm_panel.cpp
    src/ui/find_bar.cpp
    src/ui/search_panel.cpp
    src/ui/completion_popup.cpp
    src/ui/settings_page.cpp
    src/ui/chat_panel.cpp
    src/llm.cpp
    src/search.cpp
    src/git/repository.cpp
    src/ssh/session.cpp
    src/ssh/ssh_config.cpp
    src/lsp/client.cpp
    src/plugin/api.cpp
    src/keys.cpp
    src/script.cpp
    src/theme.cpp
    src/theme_builtin.cpp
    src/vfs.cpp
    src/fonts.cpp
    src/remote.cpp
    src/sysmon.cpp
    src/monitor.cpp
    src/workspace.cpp
    src/workspace_store.cpp
    src/lsp_glue.cpp
    src/status_glue.cpp
    src/platform/platform_posix.cpp
    src/platform/platform_win.cpp
)
if(WIN32)
    # Icon + version info; the .rc references the .ico by absolute path.
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/overheat.rc.in ${CMAKE_BINARY_DIR}/overheat.rc @ONLY)
    list(APPEND OVERHEAT_SOURCES ${CMAKE_BINARY_DIR}/overheat.rc)
endif()

add_executable(overheat ${OVERHEAT_SOURCES})
add_dependencies(overheat shaders icon_font)

target_include_directories(overheat PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(overheat PRIVATE
    PkgConfig::VULKAN
    PkgConfig::SDL3
    PkgConfig::FREETYPE
    PkgConfig::HARFBUZZ
    PkgConfig::LIBGIT2
    PkgConfig::LIBSSH2
    tree_sitter
    ts_grammars
)
# Self-contained Linux packages (tarball/AppImage/.deb): static C++ runtime and an
# rpath to the bundled libraries. DT_RPATH (not RUNPATH) so it also covers the
# bundled libraries' own dependencies.
option(OVERHEAT_PORTABLE "Linux: static libstdc++ + \$ORIGIN/../lib/overheat rpath" OFF)
if(OVERHEAT_PORTABLE AND NOT WIN32)
    target_link_options(overheat PRIVATE -static-libstdc++ -static-libgcc
                        "-Wl,-rpath,\$ORIGIN/../lib/overheat")
    # Last on the link line, so it beats --enable-new-dtags from pkg-config files
    # (SDL3 built from source puts it in sdl3.pc).
    target_link_libraries(overheat PRIVATE -Wl,--disable-new-dtags)
endif()

if(WIN32)
    # GUI subsystem; C++ runtime linked statically so only library DLLs ship.
    target_link_libraries(overheat PRIVATE ws2_32 ole32 shell32 uuid)
    target_link_options(overheat PRIVATE -mwindows -static-libgcc -static-libstdc++
                        -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
else()
    target_link_libraries(overheat PRIVATE PkgConfig::FONTCONFIG pthread util dl)
endif()

target_compile_definitions(overheat PRIVATE
    OVERHEAT_VERSION="${PROJECT_VERSION}"
    VULKAN_HPP_NO_EXCEPTIONS
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(overheat PRIVATE OVERHEAT_DEBUG)
    target_compile_options(overheat PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(overheat PRIVATE -O3 -march=${OVERHEAT_MARCH} -flto=auto)
    target_link_options(overheat PRIVATE -O3 -flto=auto)
endif()

# Bundled fonts next to the binary, as in the Windows install, so a build-tree
# run finds them (src/fonts.cpp looks in exe_dir/fonts).
add_custom_command(TARGET overheat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/fonts $<TARGET_FILE_DIR:overheat>/fonts)

# ---- install layout ----
include(GNUInstallDirs)
if(WIN32)
    install(TARGETS overheat RUNTIME DESTINATION .)
    install(DIRECTORY assets/fonts/ DESTINATION fonts)
    install(FILES assets/logo/overheat.ico DESTINATION .)
else()
    install(TARGETS overheat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/overheat.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES assets/logo/overheat-${size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME overheat.png)
    endforeach()
    install(FILES assets/logo/overheat.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    install(DIRECTORY assets/fonts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/overheat/fonts)
endif()

if(WIN32 OR CMAKE_CROSSCOMPILING)
    return()  # the fuzz test needs the host sanitizers
endif()

enable_testing()
add_executable(buffer_fuzz tests/buffer_fuzz.cpp src/editor/buffer.cpp)
target_include_directories(buffer_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(buffer_fuzz PRIVATE PkgConfig::FREETYPE)
target_compile_options(buffer_fuzz PRIVATE -fsanitize=address,undefined)
target_link_options(buffer_fuzz PRIVATE -fsanitize=address,undefined)
foreach(seed 1 2 3)
    add_test(NAME buffer_fuzz_${seed} COMMAND buffer_fuzz ${seed})
endforeach()

add_executable(search_test tests/search_test.cpp src/search.cpp)
target_include_directories(search_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(search_test PRIVATE -fsanitize=address,undefined)
target_link_options(search_test PRIVATE -fsanitize=address,undefined)
foreach(seed 1 2 3)
    add_test(NAME search_test_${seed} COMMAND search_test ${seed})
endforeach()

add_executable(motion_test tests/motion_test.cpp src/editor/cursor.cpp src/editor/buffer.cpp)
target_include_directories(motion_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(motion_test PRIVATE PkgConfig::FREETYPE)
target_compile_options(motion_test PRIVATE -fsanitize=address,undefined)
target_link_options(motion_test PRIVATE -fsanitize=address,undefined)
foreach(seed 1 2 3)
    add_test(NAME motion_test_${seed} COMMAND motion_test ${seed})
endforeach()

add_executable(workspace_test tests/workspace_test.cpp src/workspace_store.cpp)
target_include_directories(workspace_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(workspace_test PRIVATE PkgConfig::FREETYPE)
add_test(NAME workspace_test COMMAND workspace_test ${CMAKE_BINARY_DIR}/workspace_test_dir)

add_executable(file_icon_test tests/file_icon_test.cpp src/ui/file_icon_map.cpp)
target_include_directories(file_icon_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME file_icon_test COMMAND file_icon_test)

add_executable(sysmon_test tests/sysmon_test.cpp src/sysmon.cpp)
target_include_directories(sysmon_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(sysmon_test PRIVATE -fsanitize=address,undefined)
target_link_options(sysmon_test PRIVATE -fsanitize=address,undefined)
add_test(NAME sysmon_test COMMAND sysmon_test)

add_executable(file_check_test tests/file_check_test.cpp src/editor/buffer.cpp src/vfs.cpp)
target_include_directories(file_check_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(file_check_test PRIVATE PkgConfig::FREETYPE)
target_compile_options(file_check_test PRIVATE -fsanitize=address,undefined)
target_link_options(file_check_test PRIVATE -fsanitize=address,undefined)
# A few real binaries when present (any that are missing are simply skipped).
set(FILE_CHECK_SAMPLES "")
foreach(f /usr/bin/ls /usr/bin/cmake /bin/sh)
    if(EXISTS ${f})
        list(APPEND FILE_CHECK_SAMPLES ${f})
    endif()
endforeach()
add_test(NAME file_check_test COMMAND file_check_test ${CMAKE_BINARY_DIR}/file_check_test_dir ${FILE_CHECK_SAMPLES})

# Problem matchers, terminal links and task files against captured compiler output
# (tests/fixtures/matchers, recorded by tests/projects/capture.sh).
add_executable(problem_matcher_test tests/problem_matcher_test.cpp src/tasks/json.cpp src/tasks/matchers.cpp
               src/tasks/links.cpp src/tasks/tasks.cpp)
target_include_directories(problem_matcher_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(problem_matcher_test PRIVATE -fsanitize=address,undefined)
target_link_options(problem_matcher_test PRIVATE -fsanitize=address,undefined)
add_test(NAME problem_matcher_test COMMAND problem_matcher_test ${CMAKE_SOURCE_DIR}/tests)
