cmake_minimum_required(VERSION 3.13)

# WiFi site survey for a Pico 2 W: prints RSSI and gateway ping times over USB once a second.
# Standalone project: build it from this directory (see README.md).

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(wifi_survey C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Optional: bake the show network into the image. Left empty, the firmware asks for them over USB
# when it starts.
set(WIFI_SSID "" CACHE STRING "Show network SSID (empty: ask over USB at startup)")
set(WIFI_PASSWORD "" CACHE STRING "Show network password (empty: ask over USB at startup)")

add_executable(wifi_survey main.cpp)

target_compile_definitions(wifi_survey PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
)

target_include_directories(wifi_survey PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(wifi_survey
    PRIVATE
        pico_stdlib
        pico_cyw43_arch_lwip_threadsafe_background
)

pico_enable_stdio_usb(wifi_survey 1)
pico_enable_stdio_uart(wifi_survey 0)

pico_add_extra_outputs(wifi_survey)
