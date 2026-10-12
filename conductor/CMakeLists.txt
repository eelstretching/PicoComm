# The Conductor, for pico-sdk projects that use PicoLEDs. From a prop firmware's CMakeLists.txt
# (after pico_sdk_init() and adding PicoLEDs and picocomm_pico):
#
#   add_subdirectory(path/to/PicoComm/conductor picocomm-conductor)
#   target_link_libraries(my_prop PRIVATE picocomm_conductor)
#
# The PicoLEDs library target is called PicoLEDs; set PICOCOMM_PICOLEDS_TARGET if yours differs.

if(NOT DEFINED PICOCOMM_PICOLEDS_TARGET)
    set(PICOCOMM_PICOLEDS_TARGET PicoLEDs)
endif()

add_library(picocomm_conductor INTERFACE)
target_sources(picocomm_conductor INTERFACE ${CMAKE_CURRENT_LIST_DIR}/src/conductor.cpp)
target_include_directories(picocomm_conductor INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(picocomm_conductor INTERFACE picocomm_pico ${PICOCOMM_PICOLEDS_TARGET})
