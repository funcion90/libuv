add_executable(bench_client main.cpp)
target_link_libraries(bench_client PRIVATE uvx_common)
set_target_properties(bench_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_custom_command(TARGET bench_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/uvx.yaml"
        "$<TARGET_FILE_DIR:bench_client>/uvx.yaml"
    COMMENT "Copying uvx.yaml to output directory"
)
