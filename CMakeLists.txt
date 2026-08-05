cmake_minimum_required(VERSION 3.16)

project(agui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match nob.c: default to debug build with sanitizers.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug or Release)" FORCE)
endif()

add_compile_options(-Wall -Wextra)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O1 -fno-omit-frame-pointer)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui")

set(AGUI_SOURCES
    agui.cc
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)

add_executable(agui ${AGUI_SOURCES})

target_include_directories(agui PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ARIA2 REQUIRED IMPORTED_TARGET libaria2)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(APPINDICATOR REQUIRED IMPORTED_TARGET appindicator3-0.1 gtk+-3.0)
find_package(OpenGL REQUIRED)

target_link_libraries(agui PRIVATE
    PkgConfig::ARIA2
    PkgConfig::SDL3
    PkgConfig::APPINDICATOR
    OpenGL::GL
    ${CMAKE_DL_LIBS}
)

# Usage (equivalent to nob.c):
#   cmake -B build
#   cmake --build build
#
# Release build (equivalent to `./nob release`):
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Force full rebuild (equivalent to `./nob -B`):
#   cmake --build build --clean-first
