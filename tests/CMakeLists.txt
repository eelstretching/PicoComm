foreach(t test_wire test_sync test_sim)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE picocomm_core)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

target_compile_definitions(test_wire PRIVATE
    PICOCOMM_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/packets.txt")
