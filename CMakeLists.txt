cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 20)

project(work_contract)

option(WORK_CONTRACT_BUILD_BENCHMARK "build work contract throughput benchmark" ON)
option(WORK_CONTRACT_BUILD_TESTS "build work contract tests (ctest)" ON)


if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message("*** Build type not set.  defaulting to Release")
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
        -g
        -O0
        -Wall
    )
else()
    add_compile_options(
        -O3
        -g
        -Wall
    )
endif()


set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
link_directories(${CMAKE_BINARY_DIR}/lib)

message("Build Type = ${CMAKE_BUILD_TYPE}")

###############################################################################
# Fetch the main bcpp scripts which allows us to fetch other dependencies more easily
###############################################################################
include(FetchContent)

if ("${_scripts_src_path}" STREQUAL "")
    set(_scripts_src_path ${CMAKE_BINARY_DIR}/scripts-src CACHE STRING "")
    message("Fetching Content: scripts.git")

    FetchContent_Declare(
        scripts
        GIT_REPOSITORY https://github.com/buildingcpp/scripts.git
        GIT_TAG main
        SOURCE_DIR        "${CMAKE_BINARY_DIR}/scripts-src"
        BINARY_DIR        "${CMAKE_BINARY_DIR}/scripts-build"
        INSTALL_DIR       "${CMAKE_BINARY_DIR}"
        INSTALL_COMMAND   ""
        )
    FetchContent_MakeAvailable(scripts)
    FetchContent_GetProperties(scripts)
endif()

include("${_scripts_src_path}/cmake/fetch_dependencies.cmake")
fetch_dependency("https://github.com/buildingcpp/include.git;main")

set(SIGNAL_TREE_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
fetch_dependency("https://github.com/buildingcpp/signal_tree.git;main")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${PROJECT_NAME})
set(_${PROJECT_NAME}_dir ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "")

if (WORK_CONTRACT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
