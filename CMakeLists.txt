cmake_minimum_required(VERSION 3.24)

project(TestCoroutine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

include(FetchContent)
FetchContent_Declare(
    libcoro
    GIT_REPOSITORY https://github.com/a858438680/libcoro.git
    GIT_TAG        main
)
set(LIBCORO_BUILD_TESTS OFF)
set(LIBCORO_BUILD_EXAMPLES OFF)
set(LIBCORO_FEATURE_NETWORKING OFF)
set(LIBCORO_FEATURE_SSL OFF)
FetchContent_MakeAvailable(libcoro)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.1.1
)
FetchContent_MakeAvailable(fmt)

add_executable(test test.cpp)
target_link_libraries(test PUBLIC libcoro fmt)