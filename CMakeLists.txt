cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 20)

project(work_contract)

option(WORK_CONTRACT_BUILD_BENCHMARK "build work contract benchmarks" ON)
option(WORK_CONTRACT_BUILD_EXAMPLES "build work contract examples" ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message("*** Build type not set.  defaulting to Release")
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
        -g
        -O0
        -march=native
        -fPIE
        -Wno-changes-meaning # tbb 
        -Wall
    )
else()
    add_compile_options(
        -O3
        -g
        -march=native
        -fPIE
        -Wno-changes-meaning # tbb
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

if (WORK_CONTRACT_BUILD_BENCHMARK)
    fetch_dependency("https://github.com/fmtlib/fmt.git;main")
    fetch_dependency("https://github.com/cameron314/concurrentqueue.git;master")
    #fetch_dependency("https://github.com/boostorg/lockfree.git;master")
    fetch_dependency("https://github.com/erez-strauss/lockfree_mpmc_queue.git;master")
    fetch_dependency("https://github.com/google/googletest.git;main")
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(TBB_BUILD_TESTS OFF)
    set(TBB_BUILD_SHARED OFF)
    set(TBB_BUILD_STATIC ON)
    fetch_dependency("https://github.com/wjakob/tbb.git;master")
endif(WORK_CONTRACT_BUILD_BENCHMARK)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${PROJECT_NAME})
set(_${PROJECT_NAME}_dir ${CMAKE_CURRENT_SOURCE_DIR} CACHE STRING "")

add_subdirectory(src)

