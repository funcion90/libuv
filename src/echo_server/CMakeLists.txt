add_executable(echo_server main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(echo_server PRIVATE uvx_common Threads::Threads)

set_target_properties(echo_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_custom_command(TARGET echo_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/uvx.yaml"
        "$<TARGET_FILE_DIR:echo_server>/uvx.yaml"
    COMMENT "Copying uvx.yaml to output directory"
)
