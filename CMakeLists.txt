cmake_minimum_required(VERSION 3.10)

project(MyProject)

include_directories(include)
file(GLOB_RECURSE SOURCES "src/*.c")

add_executable(app ${SOURCES})

# Enable the sound subsystem (the Makefile defines this via -DFEATURE_SOUND;
# without it i_sound.c compiles out the SFX module and only music is wired up).
target_compile_definitions(app PRIVATE FEATURE_SOUND)

# Locate SDL3 / SDL3_mixer via pkg-config (installed under /usr/local or distro packages)
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(SDL3_MIXER REQUIRED IMPORTED_TARGET sdl3-mixer)

# Link SDL3 and SDL3_mixer
target_link_libraries(app PRIVATE PkgConfig::SDL3 PkgConfig::SDL3_MIXER)


