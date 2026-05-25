add_executable(bench_client main.cpp)
target_link_libraries(bench_client PRIVATE uvx_common)
set_target_properties(bench_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

uvx_copy_yaml(bench_client)
