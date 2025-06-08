cmake_minimum_required(VERSION 3.10)

project(MyProject)

include_directories(include)
file(GLOB_RECURSE SOURCES "src/*.c")

add_executable(app ${SOURCES})

# FetchContent downloads and configures dependencies
include(FetchContent)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git"
    GIT_TAG "main"
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
    SDL3_mixer
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL_mixer.git"
    GIT_TAG "main"
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(SDL3_mixer)

# Link SDL3
target_link_libraries(app SDL3::SDL3)

# Link SDL3_mixer
target_link_libraries(app SDL3_mixer::SDL3_mixer)


