cmake_minimum_required(VERSION 3.13)

# PicoComm bench firmware for the basement tests (docs/protocol-design.md, section 8):
#
#   sync_bench_leader.uf2    leader: beacons, answers sync, cycles a test scene every 5 s
#   sync_bench_follower.uf2  follower: joins, syncs, plays the test scenes
#   judge.uf2                a plain Pico (no WiFi needed) that timestamps the others' pulses
#
# Leader and followers pulse GP22 for 1 ms at every whole second of show time, and GP21 for 1 ms
# when a cue takes effect. See firmware/README.md.
#
#   PICO_SDK_PATH=/path/to/pico-sdk cmake -S firmware -B build -G Ninja   # pico-sdk 2.3.0
#   ninja -C build

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(picocomm_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

if(NOT PICO_CYW43_SUPPORTED)
    message(FATAL_ERROR "PicoComm needs a board with a CYW43 radio (PICO_BOARD=${PICO_BOARD})")
endif()

add_subdirectory(../pico picocomm)

# Optional: bake the show network into the images. Left empty, the bench firmware asks for them
# over USB at startup, like wifi_survey.
set(WIFI_SSID "" CACHE STRING "Show network SSID (empty: ask over USB at startup)")
set(WIFI_PASSWORD "" CACHE STRING "Show network password (empty: ask over USB at startup)")

foreach(role leader follower)
    set(target sync_bench_${role})
    add_executable(${target} sync_bench/main.cpp)
    if(role STREQUAL "leader")
        target_compile_definitions(${target} PRIVATE PICOCOMM_BENCH_LEADER=1)
    else()
        target_compile_definitions(${target} PRIVATE PICOCOMM_BENCH_LEADER=0)
    endif()
    target_compile_definitions(${target} PRIVATE
        WIFI_SSID="${WIFI_SSID}"
        WIFI_PASSWORD="${WIFI_PASSWORD}"
    )
    target_compile_options(${target} PRIVATE -Wall)
    target_link_libraries(${target} PRIVATE picocomm_pico)
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
    pico_add_extra_outputs(${target})
endforeach()

add_executable(judge judge/main.cpp)
target_compile_options(judge PRIVATE -Wall)
target_link_libraries(judge PRIVATE pico_stdlib)
pico_enable_stdio_usb(judge 1)
pico_enable_stdio_uart(judge 0)
pico_add_extra_outputs(judge)
