include(FetchContent)

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG        v1.52.1
    GIT_SHALLOW    TRUE
    SYSTEM
)

FetchContent_MakeAvailable(libuv)

if(TARGET uv_a)
    set_target_properties(uv_a PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
