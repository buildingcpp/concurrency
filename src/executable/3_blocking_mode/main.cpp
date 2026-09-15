// demo 3 - blocking mode: idle workers park instead of spinning

#include <library/work_contract/work_contract_group.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>


int main()
{
    using namespace std::chrono_literals;

    // a blocking group parks idle workers rather than spinning; scheduling work
    // wakes them.  a non-blocking group would instead return false and spin.
    bcpp::concurrency::blocking_work_contract_group<> workContractGroup({.capacity_ = 256});

    std::atomic<int>  completed{0};
    std::atomic<bool> running{true};

    // each worker parks in the timed execute until work arrives or the timeout
    // lapses (its re-check cadence for 'running'); stop() breaks the park at once.
    std::vector<std::thread> workers;
    for (auto i = 0; i < 4; ++i)
        workers.emplace_back(
                [&]
                {
                    bcpp::concurrency::signal_id hint{};
                    while (running.load(std::memory_order_acquire))
                        workContractGroup.execute_next_contract(hint, 50ms);
                });

    std::this_thread::sleep_for(50ms);
    std::cout << "4 workers parked (idle, no cpu)\n";

    std::vector<bcpp::concurrency::work_contract> contracts;
    contracts.reserve(8);
    for (auto i = 0; i < 8; ++i)
        contracts.push_back(workContractGroup.create_contract(
                [&, i]
                {
                    std::cout << "  contract " << i << " ran\n";
                    completed.fetch_add(1, std::memory_order_relaxed);
                }));
    for (auto & contract : contracts)
        contract.schedule();

    while (completed.load(std::memory_order_relaxed) < 8)
        std::this_thread::sleep_for(1ms);
    std::cout << completed.load() << " contracts completed\n";

    // stop wakes every parked worker so it leaves the timed execute at once
    running.store(false, std::memory_order_release);
    workContractGroup.stop();
    for (auto & worker : workers)
        worker.join();
    std::cout << "workers released and joined\n";

    return 0;
}
