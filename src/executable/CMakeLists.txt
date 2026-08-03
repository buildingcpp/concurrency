add_subdirectory(1_demo)
add_subdirectory(3_blocking_mode)

if (WORK_CONTRACT_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()
