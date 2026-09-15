add_library(signal_tree INTERFACE)
add_library(bcpp::signal_tree ALIAS signal_tree)

target_compile_features(signal_tree INTERFACE cxx_std_20)

target_include_directories(signal_tree
INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../..>
)

target_link_libraries(signal_tree
INTERFACE
    include
    Threads::Threads
)
