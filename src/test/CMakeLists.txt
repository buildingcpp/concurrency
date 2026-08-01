if (NOT BUILD_TESTING)
    return()
endif()

find_package(Threads REQUIRED)

function(add_signal_tree_test target)
    add_executable(${target} ${target}.cpp)

    target_include_directories(${target}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    target_link_libraries(${target}
    PRIVATE
        Threads::Threads
    )

    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
endfunction()

add_signal_tree_test(signal_tree_test)
add_signal_tree_test(signal_set_test)
add_signal_tree_test(signal_tree_concurrency_test)
