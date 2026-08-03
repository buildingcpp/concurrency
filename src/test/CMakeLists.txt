if (NOT WORK_CONTRACT_BUILD_TESTS)
    return()
endif()

add_executable(work_contract_test ./work_contract_test.cpp)
target_link_libraries(work_contract_test PRIVATE work_contract)
add_test(NAME work_contract_test COMMAND work_contract_test)

add_executable(generation_test ./generation_test.cpp)
target_link_libraries(generation_test PRIVATE work_contract)
add_test(NAME generation_test COMMAND generation_test)

add_executable(api_test ./api_test.cpp)
target_link_libraries(api_test PRIVATE work_contract)
add_test(NAME api_test COMMAND api_test)

add_executable(lifecycle_test ./lifecycle_test.cpp)
target_link_libraries(lifecycle_test PRIVATE work_contract)
add_test(NAME lifecycle_test COMMAND lifecycle_test)

add_executable(concurrency_api_test ./concurrency_api_test.cpp)
target_link_libraries(concurrency_api_test PRIVATE work_contract)
add_test(NAME concurrency_api_test COMMAND concurrency_api_test)

add_executable(blocking_test ./blocking_test.cpp)
target_link_libraries(blocking_test PRIVATE work_contract)
add_test(NAME blocking_test COMMAND blocking_test)

add_executable(introduction_example_test ./introduction_example_test.cpp)
target_link_libraries(introduction_example_test PRIVATE work_contract)
add_test(NAME introduction_example_test COMMAND introduction_example_test)

# the concurrent suites churn hundreds of thousands of contracts; give them room.
set_tests_properties(
    work_contract_test
    generation_test
    api_test
    lifecycle_test
    concurrency_api_test
    blocking_test
    introduction_example_test
    PROPERTIES TIMEOUT 300
)
