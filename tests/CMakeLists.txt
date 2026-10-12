foreach(t test_wire test_sync test_sim)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE picocomm_core)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

target_compile_definitions(test_wire PRIVATE
    PICOCOMM_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/packets.txt")

# The Conductor against the real PicoLEDs, built for its desktop simulator. Needs the PicoLEDs
# submodule (git submodule update --init).
if(EXISTS ${PROJECT_SOURCE_DIR}/PicoLEDs/sim/CMakeLists.txt)
    add_subdirectory(${PROJECT_SOURCE_DIR}/PicoLEDs/sim ${CMAKE_BINARY_DIR}/picoleds-sim)
    add_executable(test_conductor test_conductor.cpp ${PROJECT_SOURCE_DIR}/conductor/src/conductor.cpp)
    target_include_directories(test_conductor PRIVATE ${PROJECT_SOURCE_DIR}/conductor/include)
    target_link_libraries(test_conductor PRIVATE picocomm_core PicoLEDsSim)
    add_test(NAME test_conductor COMMAND test_conductor)
else()
    message(STATUS "PicoLEDs submodule not checked out: skipping test_conductor")
endif()
