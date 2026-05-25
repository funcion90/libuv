add_executable(echo_server main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(echo_server PRIVATE uvx_common Threads::Threads)

set_target_properties(echo_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

uvx_copy_yaml(echo_server)
