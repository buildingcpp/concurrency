// Correctness / stress validation for the signal_tree backed work_contract.
//
// Covers concurrent execution (exact counts under contention), slot recycling,
// stale-handle safety, handle-outlives-group, and the group-level exception and
// release handlers.  See ../../CONCURRENCY.md for the model these exercise.

#include <library/work_contract/work_contract_group.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool condition, char const * what)
    {
        std::cout << (condition ? "  ok:   " : "  FAIL: ") << what << "\n";
        failures += (condition ? 0 : 1);
    }
}


//=============================================================================
// every scheduled contract must eventually run, exactly the requested number of
// times, with many threads draining concurrently.
template <std::size_t subtree>
void test_concurrent_execution()
{
    std::cout << "concurrent execution, subtree=" << subtree << "\n";

    static constexpr auto contract_count = 400ull;
    static constexpr auto target_per_contract = 500ull;

    bcpp::work_contract_group<subtree> group({.capacity_ = contract_count});

    std::vector<std::atomic<std::uint64_t>> counts(contract_count);
    std::vector<bcpp::work_contract> contracts;
    contracts.reserve(contract_count);

    for (auto i = 0ull; i < contract_count; ++i)
    {
        contracts.push_back(group.create_contract(
                [&counts, i]()
                {
                    if (++counts[i] < target_per_contract)
                        bcpp::this_contract::schedule();
                },
                bcpp::work_contract::initial_state::scheduled));
        check(contracts.back().is_valid() || i >= contract_count, "contract created");
        if (!contracts.back().is_valid())
            break;
    }

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> executed{0};
    std::vector<std::thread> workers;
    for (auto t = 0; t < 8; ++t)
        workers.emplace_back([&]()
                {
                    while (!done.load(std::memory_order_relaxed))
                        if (group.execute_next_contract())
                            executed.fetch_add(1, std::memory_order_relaxed);
                });

    // wait for completion
    for (;;)
    {
        auto complete = 0ull;
        for (auto & c : counts)
            complete += (c.load() >= target_per_contract);
        if (complete == contract_count)
            break;
        std::this_thread::yield();
    }
    done = true;
    for (auto & w : workers)
        w.join();

    auto exact = true;
    for (auto & c : counts)
        exact &= (c.load() == target_per_contract);
    check(exact, "every contract ran exactly the target number of times");
    check(executed.load() == contract_count * target_per_contract, "no duplicate or lost executions");
}


//=============================================================================
// slots must be returned to the available set on release, so create/release in a
// loop far beyond capacity must keep succeeding.
void test_slot_recycling()
{
    std::cout << "slot recycling\n";

    bcpp::work_contract_group group({.capacity_ = 64});
    auto created = 0ull;

    for (auto round = 0ull; round < 1000ull; ++round)
    {
        auto contract = group.create_contract([](){});
        if (!contract.is_valid())
            break;
        ++created;
        contract.release();
        while (group.execute_next_contract())  // release is processed by a worker
            ;
    }
    check(created == 1000ull, "slots recycled across 1000 create/release rounds");
}


//=============================================================================
// a handle referring to a slot that has since been recycled must not disturb the
// new occupant of that slot.
void test_stale_handle()
{
    std::cout << "stale handle safety\n";

    bcpp::work_contract_group group({.capacity_ = 64});

    auto first = group.create_contract([](){ bcpp::this_contract::release(); });
    std::vector<bcpp::work_contract> occupants;
    occupants.reserve(group.capacity() - 1);
    for (auto index = 1ull; index < group.capacity(); ++index)
        occupants.push_back(group.create_contract([] {}));

    first.schedule();
    while (group.execute_next_contract())
        ;

    std::atomic<std::uint64_t> secondRuns{0};
    auto second = group.create_contract([&](){ ++secondRuns; });
    check(second.is_valid(), "the one recycled slot accepted a new contract");

    // first still owns shared state, but its generation is stale after recycling.
    check(!first.is_valid(), "stale handle reports invalid");
    first.schedule();                       // generation mismatch: safe no-op
    check(!first.release(), "stale release is a no-op");
    while (group.execute_next_contract())
        ;
    check(secondRuns.load() == 0, "stale handle did not schedule the new occupant");

    second.schedule();
    while (group.execute_next_contract())
        ;
    check(secondRuns.load() == 1, "new occupant still works");
}


//=============================================================================
// a handle must survive the death of its group.
void test_handle_outlives_group()
{
    std::cout << "handle outliving group\n";

    bcpp::work_contract contract;
    {
        bcpp::work_contract_group group({.capacity_ = 64});
        contract = group.create_contract([](){});
        check(contract.is_valid(), "valid while group alive");
    }
    check(!contract.is_valid(), "group destruction invalidates outstanding handles");
    contract.schedule();                    // must not crash
    check(!contract.release(), "release after group death is a safe no-op");
    // and the destructor runs here, also after group death
}


//=============================================================================
void test_exception_handler()
{
    std::cout << "exception handling\n";

    std::atomic<std::uint64_t> caught{0};
    bcpp::work_contract_group group(
            {.capacity_ = 64},
            {.contractException_ = [&](auto, std::exception_ptr){ ++caught; }});

    auto contract = group.create_contract([](){ throw std::runtime_error("boom"); },
            bcpp::work_contract::initial_state::scheduled);
    while (group.execute_next_contract())
        ;
    check(caught.load() == 1, "group level exception handler invoked");
}


//=============================================================================
void test_release_handler()
{
    std::cout << "release handling\n";

    std::atomic<std::uint64_t> released{0};
    bcpp::work_contract_group group(
            {.capacity_ = 64},
            {.contractReleased_ = [&](auto){ ++released; }});

    {
        auto contract = group.create_contract([](){});
    }   // destructor releases
    while (group.execute_next_contract())
        ;
    check(released.load() == 1, "group level release handler invoked on handle destruction");
}


//=============================================================================
void test_release_remains_valid_until_processed()
{
    std::cout << "release remains valid until processed\n";

    bcpp::work_contract_group group({.capacity_ = 64});
    auto contract = group.create_contract([](){});

    check(contract.release(), "release request accepted");
    check(contract.is_valid(), "handle remains valid while release is pending");

    while (group.execute_next_contract())
        ;

    check(!contract.is_valid(), "handle becomes invalid when release is processed");
}


//=============================================================================
int main()
{
    test_concurrent_execution<64>();
    test_concurrent_execution<512>();
    test_slot_recycling();
    test_stale_handle();
    test_handle_outlives_group();
    test_exception_handler();
    test_release_handler();
    test_release_remains_valid_until_processed();

    std::cout << "\n" << (failures ? "FAILURES: " : "all passed, failures = ") << failures << "\n";
    return (failures != 0);
}
