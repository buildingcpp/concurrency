# work_contract_group is a template and therefore header only.  all that remains
# to compile is the thread local that backs this_contract.
add_library(work_contract
    ./work_contract_this.cpp
)

add_library(bcpp::work_contract ALIAS work_contract)

target_link_libraries(work_contract
PUBLIC
    bcpp::signal_tree
)


target_include_directories(work_contract
PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../..>
)
