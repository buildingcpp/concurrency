cmake_minimum_required(VERSION 3.21)

project(concurrency LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SIGNAL_TREE_BUILD_BENCHMARK "build signal tree benchmarks" OFF)
option(SIGNAL_TREE_BUILD_TESTS "build signal tree tests" ${PROJECT_IS_TOP_LEVEL})
option(WORK_CONTRACT_BUILD_BENCHMARK "build work contract throughput benchmark" OFF)
option(WORK_CONTRACT_BUILD_EXAMPLES "build work contract examples" ${PROJECT_IS_TOP_LEVEL})
option(WORK_CONTRACT_BUILD_TESTS "build work contract tests" ${PROJECT_IS_TOP_LEVEL})

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall)
else()
    add_compile_options(-O3 -g -Wall)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

message("Build Type = ${CMAKE_BUILD_TYPE}")

include(FetchContent)

if (NOT TARGET include)
    FetchContent_Declare(
        include
        GIT_REPOSITORY https://github.com/buildingcpp/include.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(include)
endif()

if (SIGNAL_TREE_BUILD_BENCHMARK)
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        system
        GIT_REPOSITORY https://github.com/buildingcpp/system.git
        GIT_TAG main
    )
    FetchContent_Declare(
        oneTBB
        GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
        GIT_TAG v2022.0.0
    )
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG main
    )
    FetchContent_Declare(
        concurrentqueue
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        GIT_TAG master
    )
    FetchContent_Declare(
        lockfree
        GIT_REPOSITORY https://github.com/boostorg/lockfree.git
        GIT_TAG master
    )

    FetchContent_MakeAvailable(system oneTBB fmt concurrentqueue lockfree)
endif()

find_package(Threads REQUIRED)

set(_concurrency_dir ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "")
set(_signal_tree_dir ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "")
set(_work_contract_dir ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "")

if (SIGNAL_TREE_BUILD_TESTS OR WORK_CONTRACT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
