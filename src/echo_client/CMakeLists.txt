add_executable(echo_client main.cpp)
target_link_libraries(echo_client PRIVATE uvx_common)
set_target_properties(echo_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

uvx_copy_yaml(echo_client)
