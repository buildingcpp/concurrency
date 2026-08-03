add_executable(
    signal_tree_benchmark 
    main.cpp
)


target_include_directories(signal_tree_benchmark
PRIVATE
    ${_fmt_src_path}/include
    ${_include_dir}/src
    ${_system_dir}/src
    ${CMAKE_SOURCE_DIR}/src
    ${_concurrentqueue_src_path}
    ${_lockfree_src_path}/include
    ${_oddTBB_src_path}
    ${_tbb_src_path}/include
    ${_lockfree_mpmc_queue_src_path}
    ${_googletest_src_path}
)

target_link_libraries(signal_tree_benchmark 
PRIVATE
    pthread
    rt
    fmt
    tbb
    system
)