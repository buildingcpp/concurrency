# work_contract_group is a template and therefore header only.  all that remains
# to compile is the thread local that backs this_contract.
add_library(work_contract
    ./work_contract_this.cpp
)


target_link_libraries(work_contract
    pthread
    rt
)


target_include_directories(work_contract
PUBLIC
    ${_work_contract_dir}/src
    ${_include_dir}/src
    ${_signal_tree_dir}/src
)
