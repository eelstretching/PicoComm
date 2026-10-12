cmake_minimum_required(VERSION 3.13)

# Host (laptop) build of the portable protocol core and its tests. The Pico firmware is a separate
# project in firmware/; see README.md.
#
#   cmake -S . -B build-host && cmake --build build-host && ctest --test-dir build-host

project(picocomm_host CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(picocomm_core STATIC
    src/wire.cpp
    src/sync.cpp
    src/follower.cpp
    src/leader.cpp
)
target_include_directories(picocomm_core PUBLIC include)
target_compile_options(picocomm_core PRIVATE -Wall -Wextra)

enable_testing()
add_subdirectory(tests)
