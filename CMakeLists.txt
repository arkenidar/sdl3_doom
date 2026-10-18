cmake_minimum_required(VERSION 3.10)

project(MyProject)

include_directories(include)
file(GLOB_RECURSE SOURCES "src/*.c")

add_executable(app ${SOURCES})

# Locate SDL3 / SDL3_mixer via pkg-config (installed under /usr/local or distro packages)
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(SDL3_MIXER REQUIRED IMPORTED_TARGET sdl3-mixer)

# Link SDL3 and SDL3_mixer
target_link_libraries(app PRIVATE PkgConfig::SDL3 PkgConfig::SDL3_MIXER)


