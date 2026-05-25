cmake_minimum_required(VERSION 3.20)

project(libuv_sandbox
    LANGUAGES CXX
    DESCRIPTION "libuv C++20 network samples (TCP echo)"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(cmake/FetchLibuv.cmake)

add_library(uvx_common INTERFACE)
target_include_directories(uvx_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(uvx_common INTERFACE uv_a)

# 각 바이너리 빌드 후 config/uvx.yaml 을 출력 디렉토리에 복사하는 헬퍼.
# add_custom_command(TARGET ...) 가 target 선언 dir 에서만 호출 가능하므로,
# 각 subdirectory 의 CMakeLists.txt 에서 이 function 을 호출한다.
function(uvx_copy_yaml tgt)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/uvx.yaml"
            "$<TARGET_FILE_DIR:${tgt}>/uvx.yaml"
        COMMENT "Copying uvx.yaml to $<TARGET_FILE_DIR:${tgt}>"
    )
endfunction()

add_subdirectory(src/echo_server)
add_subdirectory(src/echo_client)
add_subdirectory(src/bench_client)
