// Validation for the packed generation/flags state word and its ABA safety.
//
// state_ = [ generation : 48 | flags : 16 ].  every schedule/release is a CAS
// whose expected value carries the generation, so a handle whose slot has been
// recycled fails the CAS instead of touching the new occupant (no ABA), and the
// worker-owned flag transitions must preserve the generation field intact.
// See ../library/work_contract/CONCURRENCY.md for the invariants these exercise.

#include <library/work_contract/work_contract_group.h>

#include <atomic>
#include <cstdint>
#include <iostream>
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
// If any flag operation (schedule/execute/clear) bled into the generation bits,
// the handle would appear stale mid-run and reschedules would be dropped -- so a
// long self-rescheduling run that stays valid and hits its exact count proves the
// flag CAS preserves the generation field.
void test_generation_preserved_across_cycles()
{
    std::cout << "generation preserved across many flag cycles\n";

    static constexpr auto target = 500'000ull;
    bcpp::concurrency::work_contract_group group({.capacity_ = 64});

    std::uint64_t runs = 0;
    bool stayedValidMidRun = true;

    auto contract = group.create_contract(
            [&]()
            {
                if (++runs < target)
                    bcpp::concurrency::this_contract::schedule();
            },
            bcpp::concurrency::work_contract::initial_state::scheduled);

    while (group.execute_next_contract())
    {
        // the handle must remain valid for the whole life of the contract
        if ((runs & 0xFFFF) == 0 && not contract.is_valid())
            stayedValidMidRun = false;
    }

    check(runs == target, "ran exactly the target number of self-reschedules");
    check(stayedValidMidRun, "handle stayed valid throughout (generation intact)");
    check(contract.is_valid(), "handle still valid after the run (never released)");
}


//=============================================================================
// The core ABA case: a contract released from within its own work leaves the
// owning handle live but stale.  Once the slot is recycled, that stale handle
// must not schedule or release the new occupant.
void test_stale_handle_cannot_touch_recycled_slot()
{
    std::cout << "stale (self-released) handle cannot touch the recycled slot\n";

    bcpp::concurrency::work_contract_group group({.capacity_ = 64});

    std::uint64_t aRuns = 0;
    // A releases itself the first time it runs
    auto a = group.create_contract(
            [&]()
            {
                ++aRuns;
                bcpp::concurrency::this_contract::release();
            },
            bcpp::concurrency::work_contract::initial_state::scheduled);

    // Fill every other slot so the next creation after A's release must reuse
    // A's slot, without exposing slot identity through the public handle.
    std::vector<bcpp::concurrency::work_contract> occupants;
    occupants.reserve(group.capacity() - 1);
    for (auto index = 1ull; index < group.capacity(); ++index)
        occupants.push_back(group.create_contract([] {}));

    while (group.execute_next_contract())    // A runs once, self-releases, recycles
        ;

    check(aRuns == 1, "self-releasing contract ran exactly once");
    check(not a.is_valid(), "self-released handle reports invalid (generation bumped)");

    // reuse the slot with a new, unscheduled contract
    std::uint64_t bRuns = 0;
    auto b = group.create_contract([&](){ ++bRuns; });
    check(b.is_valid(), "the one recycled slot accepted a new contract");
    check(b.is_valid(), "new handle is valid");

    // hammer the stale handle -- none of this may affect B
    for (auto i = 0; i < 100; ++i)
        a.schedule();
    check(not a.release(), "stale release() returns false (slot recycled)");

    while (group.execute_next_contract())
        ;
    check(bRuns == 0, "stale schedule/release never scheduled the new occupant");

    // B still works when scheduled legitimately
    b.schedule();
    while (group.execute_next_contract())
        ;
    check(bRuns == 1, "new occupant runs on a legitimate schedule");
}


//=============================================================================
// Concurrent ABA hammer.  A tiny slot pool is churned by producers that create
// exactly N one-shot self-releasing contracts (recycling the pool ~N/capacity
// times over), while ghost threads replay schedule() on the now-stale handles and
// executors drain.
//
// Every handle is HELD in the stash for the whole run -- none is ever dropped --
// so the only release of any contract is its own post-run self-release.  That
// removes the "released before it ran" race, making the run count exact:
//
//   after a full drain, total_actual == N
//     - a lost CAS update would drop a scheduled run            -> actual < N
//     - a leaked stale schedule (broken generation check) would
//       re-signal a contract during its execute window          -> actual > N
//
// The stash is append-only and publish-once (ready[i] released after ghosts[i] is
// written, never overwritten), so ghosts read fully-constructed, immutable handles
// -- concurrent schedule() on a shared handle only reads it, so that is race free.
void test_concurrent_aba_hammer()
{
    std::cout << "concurrent ABA hammer (exact-count, no dropped handles)\n";

    static constexpr auto capacity  = 64ull;        // tiny pool -> heavy collision
    static constexpr auto N         = 500'000ull;   // one-shot contracts to produce

    bcpp::concurrency::work_contract_group<64> group({.capacity_ = capacity});

    std::atomic<std::uint64_t> actual{0};
    std::atomic<std::uint64_t> nextIndex{0};        // claims a unique stash slot < N
    std::atomic<std::uint64_t> ghostHits{0};
    std::atomic<bool> stopWorkers{false};

    std::vector<bcpp::concurrency::work_contract> ghosts(N);
    std::vector<std::atomic<bool>> ready(N);
    for (auto & r : ready)
        r.store(false, std::memory_order_relaxed);

    // producers: each claims a unique index i < N and creates exactly one contract
    // for it (helping drain if the pool is momentarily full), then stashes it.  no
    // handle is ever dropped, so nothing is released before it runs.
    //
    // IMPORTANT: create UNSCHEDULED, then schedule after stashing.  a contract
    // created *scheduled* can be run, self-released and recycled before is_valid()
    // is even checked -- and is_valid() cannot tell "pool full" from "already
    // recycled", so the retry would create a duplicate contract (an extra run).
    // creating unscheduled means the contract cannot run until we schedule it, so
    // is_valid() reliably means "pool full".  (see CONCURRENCY.md, sharp edge #1)
    auto producer = [&]()
    {
        for (;;)
        {
            auto i = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (i >= N)
                return;
            bcpp::concurrency::work_contract contract;
            do
            {
                contract = group.create_contract(
                        [&]()
                        {
                            actual.fetch_add(1, std::memory_order_relaxed);
                            bcpp::concurrency::this_contract::release();   // run once, then recycle
                        });
                if (not contract.is_valid())
                    group.execute_next_contract();            // pool full: help drain
            }
            while (not contract.is_valid());

            contract.schedule();                              // schedule after we hold it
            ghosts[i] = std::move(contract);
            ready[i].store(true, std::memory_order_release);
        }
    };

    // ghosts: replay schedule() on stashed handles (valid at first, stale once
    // their contract has run and recycled).  a working generation check drops the
    // stale ones inside the CAS.
    auto ghost = [&](std::uint64_t seed)
    {
        std::uint64_t x = seed | 1ull;
        while (not stopWorkers.load(std::memory_order_relaxed))
        {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;          // xorshift, thread-local
            auto idx = x % N;
            if (ready[idx].load(std::memory_order_acquire))
            {
                ghosts[idx].schedule();
                ghostHits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    auto executor = [&]()
    {
        while (not stopWorkers.load(std::memory_order_relaxed))
            group.execute_next_contract();
    };

    std::vector<std::thread> producers, others;
    for (auto i = 0; i < 3; ++i) others.emplace_back(executor);
    for (auto i = 0; i < 2; ++i) others.emplace_back(ghost, (std::uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull);
    for (auto i = 0; i < 3; ++i) producers.emplace_back(producer);

    for (auto & t : producers)  // wait until all N contracts have been produced
        t.join();
    stopWorkers.store(true, std::memory_order_relaxed);
    for (auto & t : others)
        t.join();

    // drain every in-flight run + pending release
    while (group.execute_next_contract())
        ;

    std::cout << "  (produced=" << N << " actual=" << actual.load()
              << " ghost_schedules=" << ghostHits.load() << ")\n";
    check(ghostHits.load() > 100'000, "ghosts actually replayed stale handles");
    check(actual.load() == N, "exact-count invariant held: no leaked or lost schedules");
}


//=============================================================================
int main()
{
    test_generation_preserved_across_cycles();
    test_stale_handle_cannot_touch_recycled_slot();
    test_concurrent_aba_hammer();

    std::cout << "\n" << (failures ? "FAILURES: " : "all passed, failures = ") << failures << "\n";
    return (failures != 0);
}
