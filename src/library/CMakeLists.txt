add_subdirectory(signal_tree)
add_subdirectory(work_contract)

add_library(concurrency INTERFACE)
add_library(bcpp::concurrency ALIAS concurrency)

target_link_libraries(concurrency
INTERFACE
    bcpp::work_contract
)
