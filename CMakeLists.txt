cmake_minimum_required(VERSION 3.20)
project(overheat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
pkg_check_modules(TREESITTER REQUIRED IMPORTED_TARGET tree-sitter)
pkg_check_modules(TREESITTER_C REQUIRED IMPORTED_TARGET tree-sitter-c)
# AUR tree-sitter-cpp ships only the shared library (no .pc, no header).
find_library(TREESITTER_CPP_LIB tree-sitter-cpp REQUIRED)

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
        DEPENDS ${SHADER} ${CMAKE_SOURCE_DIR}/shaders/quad_common.glsl.inc
        COMMENT "Compiling ${SHADER_FULL} -> ${SPIRV_NAME}"
    )
    list(APPEND SPIRV_FILES ${SPIRV_FILE})
endforeach()

add_custom_target(shaders ALL DEPENDS ${SPIRV_FILES})

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
    src/ui/file_tree.cpp
    src/ui/terminal.cpp
    src/ui/status_bar.cpp
    src/ui/tab_bar.cpp
    src/ui/command_palette.cpp
    src/git/repository.cpp
    src/ssh/session.cpp
    src/lsp/client.cpp
    src/plugin/api.cpp
    src/keys.cpp
    src/script.cpp
)

add_executable(overheat ${OVERHEAT_SOURCES})
add_dependencies(overheat shaders)

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
    PkgConfig::TREESITTER
    PkgConfig::TREESITTER_C
    ${TREESITTER_CPP_LIB}
    pthread
    util
)

target_compile_definitions(overheat PRIVATE
    SHADER_DIR="${SPIRV_OUTPUT_DIR}"
    VULKAN_HPP_NO_EXCEPTIONS
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(overheat PRIVATE OVERHEAT_DEBUG)
    target_compile_options(overheat PRIVATE -g -O0 -Wall -Wextra)
else()
    target_compile_options(overheat PRIVATE -O3 -march=native -flto)
    target_link_options(overheat PRIVATE -flto)
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
