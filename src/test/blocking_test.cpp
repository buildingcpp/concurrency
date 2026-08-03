#include "./test_support.h"

#include <library/work_contract/work_contract_group.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>


using namespace std::chrono_literals;


//=============================================================================
void test_indefinite_wait_for_work(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    std::atomic<bool> entered{};
    std::atomic<bool> returned{};
    std::uint64_t runs{};
    bool selected{};
    auto contract = group.create_contract([&] { ++runs; });

    std::thread worker(
            [&]
            {
                entered.store(true, std::memory_order_release);
                selected = group.execute_next_contract();
                returned.store(true, std::memory_order_release);
            });

    auto const started = wc_test::eventually(
            [&] { return entered.load(std::memory_order_acquire); }, 1s);
    std::this_thread::sleep_for(20ms);
    auto const returnedBeforeSchedule = returned.load(std::memory_order_acquire);
    contract.schedule();
    auto const wokeForWork = wc_test::eventually(
            [&] { return returned.load(std::memory_order_acquire); }, 2s);
    if (not wokeForWork)
        group.stop();
    worker.join();

    suite.check(started, "the no-timeout blocking worker started");
    suite.check(not returnedBeforeSchedule, "the no-timeout overload waits while the group is empty");
    suite.check(wokeForWork, "scheduling wakes a no-timeout blocking executor");
    suite.check(selected && runs == 1, "the awakened no-timeout executor runs the contract");
}


//=============================================================================
void test_stop_wakes_indefinite_wait(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    std::atomic<bool> entered{};
    std::atomic<bool> returned{};
    bool selected{};

    std::thread worker(
            [&]
            {
                entered.store(true, std::memory_order_release);
                selected = group.execute_next_contract();
                returned.store(true, std::memory_order_release);
            });

    auto const started = wc_test::eventually(
            [&] { return entered.load(std::memory_order_acquire); }, 1s);
    std::this_thread::sleep_for(20ms);
    auto const returnedBeforeStop = returned.load(std::memory_order_acquire);
    group.stop();
    auto const wokeForStop = wc_test::eventually(
            [&] { return returned.load(std::memory_order_acquire); }, 2s);
    worker.join();

    suite.check(started, "the indefinite stop-wakeup worker started");
    suite.check(not returnedBeforeStop, "the no-timeout overload remains blocked before stop");
    suite.check(wokeForStop, "stop wakes a no-timeout blocking executor");
    suite.check(not selected, "an indefinite stop wakeup does not masquerade as work");
}


//=============================================================================
void test_timeout(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});

    auto const before = std::chrono::steady_clock::now();
    auto const selected = group.execute_next_contract(30ms);
    auto const elapsed = std::chrono::steady_clock::now() - before;

    suite.check(not selected, "a timed blocking execute reports no work on timeout");
    suite.check(elapsed >= 15ms, "an idle timed execute actually waits");
    suite.check(elapsed < 2s, "the timeout returns within a generous upper bound");
}


//=============================================================================
void test_zero_timeout_is_try(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    std::uint64_t runs{};
    auto contract = group.create_contract([&] { ++runs; });

    suite.check(not group.try_execute_next_contract(),
            "try returns immediately when no work is available");

    contract.schedule();
    suite.check(group.execute_next_contract(0ns),
            "a zero timeout has the same semantics as try");

    bcpp::concurrency::signal_id hint{63};
    contract.schedule();
    suite.check(group.try_execute_next_contract(hint),
            "the explicit-hint try executes immediately available work");
    suite.check(runs == 2, "the try operations invoke each selected contract");
}


//=============================================================================
void test_pending_work_does_not_wait(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    std::uint64_t runs{};
    auto contract = group.create_contract(
            [&] { ++runs; },
            bcpp::concurrency::work_contract::initial_state::scheduled);

    auto const before = std::chrono::steady_clock::now();
    auto const selected = group.execute_next_contract(2s);
    auto const elapsed = std::chrono::steady_clock::now() - before;

    suite.check(selected && runs == 1, "timed blocking execute consumes already-pending work");
    suite.check(elapsed < 500ms, "pending work bypasses the wait path");
    suite.check(contract.is_valid(), "blocking execution does not release the contract");
}


//=============================================================================
void test_schedule_wakes_waiter(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    std::atomic<std::uint64_t> runs{};
    std::atomic<bool> waiting{};
    bool selected{};
    auto contract = group.create_contract([&] { runs.fetch_add(1, std::memory_order_relaxed); });

    std::thread worker(
            [&]
            {
                waiting.store(true, std::memory_order_release);
                selected = group.execute_next_contract(2s);
            });
    auto const started = wc_test::eventually(
            [&] { return waiting.load(std::memory_order_acquire); }, 1s);
    std::this_thread::sleep_for(20ms);
    contract.schedule();
    worker.join();

    suite.check(started, "the waiting worker started");
    suite.check(selected, "scheduling wakes a timed blocking executor");
    suite.check(runs.load(std::memory_order_relaxed) == 1, "the awakened worker executes the contract");
}


//=============================================================================
void test_explicit_hint_timeout_overload(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 128});
    std::uint64_t runs{};
    auto contract = group.create_contract(
            [&] { ++runs; },
            bcpp::concurrency::work_contract::initial_state::scheduled);
    bcpp::concurrency::signal_id hint{127};

    suite.check(group.execute_next_contract(hint, 1s), "the blocking hint+timeout overload selects work");
    suite.check(runs == 1, "hinted blocking execution invokes the callback");
    suite.check(not group.execute_next_contract(hint, 1ms), "the same overload times out after draining");
    suite.check(contract.is_valid(), "hinted blocking execution preserves lifetime");
}


//=============================================================================
void test_stop_wakes_all_waiters(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    static constexpr auto workerCount = 6;
    std::atomic<int> entered{};
    std::atomic<int> returned{};
    std::atomic<bool> anySelected{};
    std::vector<std::thread> workers;

    for (auto index = 0; index < workerCount; ++index)
        workers.emplace_back(
                [&]
                {
                    entered.fetch_add(1, std::memory_order_release);
                    auto const selected = group.execute_next_contract(10s);
                    if (selected)
                        anySelected.store(true, std::memory_order_relaxed);
                    returned.fetch_add(1, std::memory_order_release);
                });

    auto const allEntered = wc_test::eventually(
            [&] { return entered.load(std::memory_order_acquire) == workerCount; }, 2s);
    std::this_thread::sleep_for(20ms);
    auto const before = std::chrono::steady_clock::now();
    group.stop();
    auto const allReturned = wc_test::eventually(
            [&] { return returned.load(std::memory_order_acquire) == workerCount; }, 2s);
    auto const elapsed = std::chrono::steady_clock::now() - before;

    for (auto & worker : workers)
        worker.join();

    suite.check(allEntered, "all blocking workers entered the wait");
    suite.check(allReturned, "stop wakes every blocking worker");
    suite.check(elapsed < 2s, "stop does not wait for the original timeout");
    suite.check(not anySelected.load(std::memory_order_relaxed), "a stop wakeup does not masquerade as work");
}


//=============================================================================
void test_stop_invalidates_handles(wc_test::suite & suite)
{
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    auto first = group.create_contract([] {});
    auto second = group.create_contract([] {});

    group.stop();
    group.stop();

    suite.check(not first.is_valid() && not second.is_valid(), "stop invalidates every outstanding handle");
    suite.check(not first.release() && not second.release(), "release after stop is rejected safely");
}


//=============================================================================
void test_blocking_release(wc_test::suite & suite)
{
    std::uint64_t released{};
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = 64});
    auto contract = group.create_contract([] {}, [&] { ++released; });
    contract.release();

    suite.check(group.execute_next_contract(1s), "blocking mode processes a pending release");
    suite.check(released == 1, "blocking mode invokes the release handler");
    suite.check(not contract.is_valid(), "blocking release recycles the contract");
}


//=============================================================================
void test_multiple_blocking_workers(wc_test::suite & suite)
{
    static constexpr auto contractCount = 256;
    bcpp::concurrency::blocking_work_contract_group<64> group({.capacity_ = contractCount});
    std::atomic<int> runs{};
    std::atomic<bool> running{true};
    std::vector<bcpp::concurrency::work_contract> contracts;
    std::vector<std::thread> workers;
    contracts.reserve(contractCount);

    for (auto index = 0; index < contractCount; ++index)
        contracts.push_back(group.create_contract(
                [&] { runs.fetch_add(1, std::memory_order_relaxed); }));

    for (auto index = 0; index < 6; ++index)
        workers.emplace_back(
                [&]
                {
                    bcpp::concurrency::signal_id hint{};
                    while (running.load(std::memory_order_acquire))
                        group.execute_next_contract(hint, 100ms);
                });

    for (auto & contract : contracts)
        contract.schedule();

    auto const completed = wc_test::eventually(
            [&] { return runs.load(std::memory_order_acquire) == contractCount; }, 5s);
    running.store(false, std::memory_order_release);
    group.stop();
    for (auto & worker : workers)
        worker.join();

    suite.check(completed, "multiple blocking workers process all scheduled contracts");
    suite.check(runs.load(std::memory_order_relaxed) == contractCount, "blocking workers execute each signal exactly once");
}


//=============================================================================
int main()
{
    wc_test::suite suite{"blocking work-contract groups"};
    test_indefinite_wait_for_work(suite);
    test_stop_wakes_indefinite_wait(suite);
    test_timeout(suite);
    test_zero_timeout_is_try(suite);
    test_pending_work_does_not_wait(suite);
    test_schedule_wakes_waiter(suite);
    test_explicit_hint_timeout_overload(suite);
    test_stop_wakes_all_waiters(suite);
    test_stop_invalidates_handles(suite);
    test_blocking_release(suite);
    test_multiple_blocking_workers(suite);
    return suite.finish();
}
