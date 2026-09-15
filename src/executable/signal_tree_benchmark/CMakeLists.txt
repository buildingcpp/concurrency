add_executable(
    signal_tree_benchmark
    main.cpp
)


target_include_directories(signal_tree_benchmark
PRIVATE
    ${concurrentqueue_SOURCE_DIR}
    ${lockfree_SOURCE_DIR}/include
)

target_link_libraries(signal_tree_benchmark
PRIVATE
    bcpp::signal_tree
    fmt
    tbb
    system
)
