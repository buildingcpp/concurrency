#include "./test_support.h"

#include <library/work_contract/work_contract_group.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>


using namespace std::chrono_literals;


//=============================================================================
void test_concurrent_creation(wc_test::suite & suite)
{
    bcpp::work_contract_group<64> group({.capacity_ = 512});
    std::atomic<std::uint64_t> runs{};
    std::mutex handlesMutex;
    std::vector<bcpp::work_contract> handles;
    handles.reserve(group.capacity());

    auto createUntilFull = [&]
    {
        std::vector<bcpp::work_contract> local;
        for (;;)
        {
            auto contract = group.create_contract(
                    [&] { runs.fetch_add(1, std::memory_order_relaxed); });
            if (not contract)
                break;
            local.push_back(std::move(contract));
        }

        std::lock_guard lock{handlesMutex};
        for (auto & contract : local)
            handles.push_back(std::move(contract));
    };

    std::vector<std::thread> creators;
    for (auto index = 0; index < 8; ++index)
        creators.emplace_back(createUntilFull);
    for (auto & creator : creators)
        creator.join();

    suite.check(handles.size() == group.capacity(), "concurrent creators allocate every available slot");
    for (auto & handle : handles)
        handle.schedule();
    auto const processed = wc_test::drain(group);
    suite.check(processed == handles.size(), "every concurrently created handle owns an independently schedulable contract");
    suite.check(runs.load(std::memory_order_relaxed) == handles.size(), "concurrent creation never hands out a slot twice");
}


//=============================================================================
void test_concurrent_schedule_coalescing(wc_test::suite & suite)
{
    bcpp::work_contract_group<64> group({.capacity_ = 64});
    std::atomic<std::uint64_t> runs{};
    auto contract = group.create_contract([&] { runs.fetch_add(1, std::memory_order_relaxed); });

    std::vector<std::thread> schedulers;
    for (auto index = 0; index < 8; ++index)
        schedulers.emplace_back(
                [&]
                {
                    for (auto count = 0; count < 10'000; ++count)
                        contract.schedule();
                });
    for (auto & scheduler : schedulers)
        scheduler.join();

    suite.check(wc_test::drain(group) == 1, "concurrent pending schedules coalesce into one signal");
    suite.check(runs.load(std::memory_order_relaxed) == 1, "coalesced concurrent scheduling runs once");
}


//=============================================================================
void test_schedule_while_executing(wc_test::suite & suite)
{
    bcpp::work_contract_group<64> group({.capacity_ = 64});
    std::atomic<std::uint64_t> runs{};
    std::atomic<bool> entered{};
    std::atomic<bool> proceed{};

    auto contract = group.create_contract(
            [&]
            {
                if (runs.fetch_add(1, std::memory_order_relaxed) == 0)
                {
                    entered.store(true, std::memory_order_release);
                    while (not proceed.load(std::memory_order_acquire))
                        std::this_thread::yield();
                }
            },
            bcpp::work_contract::initial_state::scheduled);

    std::thread executor([&] { group.execute_next_contract(); });
    auto const started = wc_test::eventually(
            [&] { return entered.load(std::memory_order_acquire); }, 2s);

    if (started)
    {
        std::vector<std::thread> schedulers;
        for (auto index = 0; index < 8; ++index)
            schedulers.emplace_back(
                    [&]
                    {
                        for (auto count = 0; count < 1'000; ++count)
                            contract.schedule();
                    });
        for (auto & scheduler : schedulers)
            scheduler.join();
    }

    proceed.store(true, std::memory_order_release);
    executor.join();

    suite.check(started, "the first execution entered");
    suite.check(wc_test::drain(group) == 1, "schedules during execution produce one follow-up signal");
    suite.check(runs.load(std::memory_order_relaxed) == 2, "exactly one follow-up execution ran");
}


//=============================================================================
void test_release_while_executing(wc_test::suite & suite)
{
    std::atomic<std::uint64_t> runs{};
    std::atomic<std::uint64_t> released{};
    std::atomic<bool> entered{};
    std::atomic<bool> proceed{};
    bcpp::work_contract_group<64> group(
            {.capacity_ = 64},
            {.contractReleased_ = [&](auto) { released.fetch_add(1, std::memory_order_relaxed); }});

    auto contract = group.create_contract(
            [&]
            {
                runs.fetch_add(1, std::memory_order_relaxed);
                entered.store(true, std::memory_order_release);
                while (not proceed.load(std::memory_order_acquire))
                    std::this_thread::yield();
            },
            bcpp::work_contract::initial_state::scheduled);

    std::thread executor([&] { group.execute_next_contract(); });
    auto const started = wc_test::eventually(
            [&] { return entered.load(std::memory_order_acquire); }, 2s);
    auto const accepted = started && contract.release();
    if (started)
        for (auto count = 0; count < 1'000; ++count)
            contract.schedule();

    proceed.store(true, std::memory_order_release);
    executor.join();

    suite.check(started, "the contract entered before external release");
    suite.check(accepted, "release during execution is accepted");
    suite.check(wc_test::drain(group) == 1, "release during execution produces one terminal follow-up");
    suite.check(runs.load(std::memory_order_relaxed) == 1, "terminal release suppresses every later schedule");
    suite.check(released.load(std::memory_order_relaxed) == 1, "release callback runs exactly once");
    suite.check(not contract.is_valid(), "the externally released handle becomes stale");
}


//=============================================================================
void test_concurrent_release_processing(wc_test::suite & suite)
{
    static constexpr auto contractCount = 512;
    std::atomic<int> released{};
    std::atomic<bool> running{true};
    bcpp::work_contract_group<64> group(
            {.capacity_ = contractCount},
            {.contractReleased_ = [&](auto) { released.fetch_add(1, std::memory_order_relaxed); }});
    std::vector<bcpp::work_contract> contracts;
    std::vector<std::thread> workers;
    contracts.reserve(contractCount);

    for (auto index = 0; index < contractCount; ++index)
        contracts.push_back(group.create_contract([] {}));

    for (auto index = 0; index < 8; ++index)
        workers.emplace_back(
                [&]
                {
                    while (running.load(std::memory_order_acquire))
                        group.execute_next_contract();
                });

    for (auto & contract : contracts)
        contract.release();

    auto const completed = wc_test::eventually(
            [&] { return released.load(std::memory_order_acquire) == contractCount; }, 5s);
    running.store(false, std::memory_order_release);
    for (auto & worker : workers)
        worker.join();

    auto allInvalid = true;
    for (auto const & contract : contracts)
        allInvalid &= not contract.is_valid();

    suite.check(completed, "multiple executors process every concurrent release");
    suite.check(released.load(std::memory_order_relaxed) == contractCount, "each release handler runs exactly once");
    suite.check(allInvalid, "every concurrently released handle becomes stale");
}


//=============================================================================
void test_group_isolation(wc_test::suite & suite)
{
    bcpp::work_contract_group<64> first({.capacity_ = 64});
    bcpp::work_contract_group<64> second({.capacity_ = 64});
    std::uint64_t firstRuns{};
    std::uint64_t secondRuns{};

    auto firstContract = first.create_contract([&] { ++firstRuns; });
    auto secondContract = second.create_contract([&] { ++secondRuns; });

    firstContract.schedule();
    suite.check(first.execute_next_contract(), "the owning group executes its signal");
    suite.check(not second.execute_next_contract(), "an equal id in another group is unaffected");
    suite.check(firstRuns == 1 && secondRuns == 0, "work and signals remain group-local");
}


//=============================================================================
void test_thread_local_context(wc_test::suite & suite)
{
    bcpp::work_contract_group<64> group({.capacity_ = 64});
    std::atomic<int> entered{};
    std::atomic<bool> proceed{};
    std::atomic<bool> contextsCorrect{true};
    std::array<bcpp::work_contract_id, 2> observedIds;
    std::vector<bcpp::work_contract> contracts;
    contracts.reserve(2);

    for (auto index = 0; index < 2; ++index)
    {
        contracts.push_back(group.create_contract(
                [&, index]
                {
                    if (not bcpp::this_contract::is_executing())
                        contextsCorrect.store(false, std::memory_order_relaxed);
                    observedIds[index] = bcpp::this_contract::get_id();
                    entered.fetch_add(1, std::memory_order_release);
                    while (not proceed.load(std::memory_order_acquire))
                        std::this_thread::yield();
                },
                bcpp::work_contract::initial_state::scheduled));
    }

    std::thread first([&] { group.execute_next_contract(); });
    std::thread second([&] { group.execute_next_contract(); });
    auto const bothEntered = wc_test::eventually(
            [&] { return entered.load(std::memory_order_acquire) == 2; }, 2s);
    proceed.store(true, std::memory_order_release);
    first.join();
    second.join();

    suite.check(bothEntered, "two contracts can execute concurrently");
    suite.check(contextsCorrect.load(std::memory_order_relaxed), "each worker sees its own this_contract context");
    suite.check(observedIds[0].valid() && observedIds[1].valid() && observedIds[0] != observedIds[1], "concurrent contracts have distinct internal execution ids");
    suite.check(not bcpp::this_contract::is_executing(), "the caller thread has no leaked execution context");
}


//=============================================================================
void test_explicit_hints(wc_test::suite & suite)
{
    bcpp::work_contract_group<64> group({.capacity_ = 128});
    std::uint64_t runs{};
    auto first = group.create_contract([&] { ++runs; }, bcpp::work_contract::initial_state::scheduled);
    auto second = group.create_contract([&] { ++runs; }, bcpp::work_contract::initial_state::scheduled);
    bcpp::signal_id hint{127};

    suite.check(group.execute_next_contract(hint), "the explicit-hint overload selects pending work");
    suite.check(group.execute_next_contract(hint), "the updated hint can select another contract");
    suite.check(not group.execute_next_contract(hint), "the explicit-hint overload reports exhaustion");
    suite.check(runs == 2, "hinted selection executes every pending contract once");
    suite.check(first.is_valid() && second.is_valid(), "hint selection does not alter contract lifetime");
}


//=============================================================================
int main()
{
    wc_test::suite suite{"public concurrency behavior"};
    test_concurrent_creation(suite);
    test_concurrent_schedule_coalescing(suite);
    test_schedule_while_executing(suite);
    test_release_while_executing(suite);
    test_concurrent_release_processing(suite);
    test_group_isolation(suite);
    test_thread_local_context(suite);
    test_explicit_hints(suite);
    return suite.finish();
}
