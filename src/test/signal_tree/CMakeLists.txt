function(add_signal_tree_test target)
    add_executable(${target} ${target}.cpp)

    target_link_libraries(${target}
    PRIVATE
        bcpp::signal_tree
    )

    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
endfunction()

add_signal_tree_test(signal_tree_test)
add_signal_tree_test(signal_set_test)
add_signal_tree_test(blocking_signal_set_test)
add_signal_tree_test(selection_contract_test)
add_signal_tree_test(signal_tree_concurrency_test)
