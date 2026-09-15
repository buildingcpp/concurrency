if (WORK_CONTRACT_BUILD_EXAMPLES)
    add_subdirectory(1_demo)
    add_subdirectory(3_blocking_mode)
endif()

if (WORK_CONTRACT_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()

if (SIGNAL_TREE_BUILD_BENCHMARK)
    add_subdirectory(signal_tree_benchmark)
endif()
