# work_contract throughput benchmark.  No external dependencies.

add_executable(benchmark
    ./main.cpp
)

target_compile_options(benchmark PRIVATE -O3 -march=native)

target_link_libraries(benchmark
PRIVATE
    bcpp::work_contract
)


# ---------------------------------------------------------------------------
# Optional: the same benchmark built against a frozen snapshot of the
# pre-rewrite work_contract (+ its vendored signal_tree), so the two can be run
# side by side.  Guarded because it compiles a copy of the old library.
# ---------------------------------------------------------------------------
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/baseline/main_baseline.cpp)

    add_executable(benchmark_baseline
        ./baseline/main_baseline.cpp
        ./baseline/vendor/library/work_contract/work_contract_group.cpp
        ./baseline/vendor/library/work_contract/work_contract_this.cpp
    )

    target_compile_options(benchmark_baseline PRIVATE -O3 -march=native)

    # NB: deliberately does *not* include the new signal_tree dependency; the
    # baseline resolves <include/signal_tree.h> from its own frozen vendor tree.
    target_include_directories(benchmark_baseline
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/baseline/vendor
        ${_include_dir}/src
    )

    target_link_libraries(benchmark_baseline
    PRIVATE
        pthread
        rt
    )

endif()
