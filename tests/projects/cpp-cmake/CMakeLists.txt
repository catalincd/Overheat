cmake_minimum_required(VERSION 3.20)
project(overheat_sample CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -DBROKEN=ON adds src/broken.cpp: intentional errors (template instantiation,
# an error inside an included header) for Overheat's problem matchers.
option(BROKEN "Build the intentionally broken source" OFF)

add_executable(sample src/main.cpp src/shapes.cpp)
target_compile_options(sample PRIVATE -Wall -Wextra)
if(BROKEN)
    target_sources(sample PRIVATE src/broken.cpp)
endif()
