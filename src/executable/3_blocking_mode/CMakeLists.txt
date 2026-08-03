find_package(Threads REQUIRED)

add_executable(demo_3_blocking_mode main.cpp)

target_link_libraries(demo_3_blocking_mode
    work_contract
    Threads::Threads
)
