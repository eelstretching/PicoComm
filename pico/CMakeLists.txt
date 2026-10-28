# PicoComm for pico-sdk projects. From a prop firmware's CMakeLists.txt (after pico_sdk_init()):
#
#   add_subdirectory(path/to/PicoComm/pico picocomm)
#   target_link_libraries(my_prop PRIVATE picocomm_pico)
#
# Needs an RP2350 board with a CYW43 radio (pico2_w, or the universal-strip-controller's RM2).

add_library(picocomm_pico INTERFACE)

target_sources(picocomm_pico INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../src/wire.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/sync.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/follower.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/leader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/elector.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/node.cpp
)

target_include_directories(picocomm_pico INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../include
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/lwip
)

target_link_libraries(picocomm_pico INTERFACE
    pico_stdlib
    pico_multicore
    pico_rand
    pico_unique_id
    pico_util
    pico_cyw43_arch_lwip_poll
    hardware_adc
)
