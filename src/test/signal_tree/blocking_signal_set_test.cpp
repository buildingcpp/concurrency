#include "test_support.h"

#include <library/signal_tree.h>
#include <include/synchronization_mode.h>

#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <thread>
#include <vector>


namespace
{

    using namespace std::chrono_literals;
    template <std::size_t tree_depth>
    using blocking_signal_set = bcpp::concurrency::signal_set<tree_depth, bcpp::synchronization_mode::blocking>;

    template <std::size_t tree_depth>
    using blocking_signal_tree = bcpp::concurrency::signal_tree<tree_depth, bcpp::synchronization_mode::blocking>;


    template <typename T>
    concept has_timed_select = requires (T & signals, bcpp::concurrency::signal_id & hint)
    {
        signals.select(hint, std::chrono::milliseconds{1});
    };

    template <typename T>
    concept has_try_select = requires (T & signals, bcpp::concurrency::signal_id & hint)
    {
        signals.try_select(hint);
    };

    template <typename T>
    concept has_simple_signal_api = requires (T & signals, bcpp::concurrency::signal_id signal, bcpp::concurrency::signal_id & hint)
    {
        { signals.set(signal) } -> std::same_as<bool>;
        { signals.select(hint) } -> std::same_as<bcpp::concurrency::signal_id>;
    };

    static_assert(has_timed_select<blocking_signal_set<0>>);
    static_assert(has_timed_select<blocking_signal_tree<0>>);
    static_assert(has_try_select<blocking_signal_tree<0>>);
    static_assert(not has_timed_select<bcpp::concurrency::signal_set<0>>);
    static_assert(not has_timed_select<bcpp::concurrency::signal_tree<0>>);
    static_assert(not has_try_select<bcpp::concurrency::signal_tree<0>>);
    static_assert(has_simple_signal_api<bcpp::concurrency::signal_set<0>>);
    static_assert(has_simple_signal_api<blocking_signal_set<0>>);
    static_assert(has_simple_signal_api<bcpp::concurrency::signal_tree<0>>);
    static_assert(has_simple_signal_api<blocking_signal_tree<0>>);


    void ordinary_select_remains_immediate()
    {
        blocking_signal_set<0> signals{64};
        auto hint = bcpp::concurrency::signal_id{0};

        test_support::require(not signals.select(hint).valid(),
                "ordinary select on a blocking signal set must remain immediate");
    }


    void timed_select_returns_ready_signal()
    {
        blocking_signal_set<0> signals{64};
        auto hint = bcpp::concurrency::signal_id{0};
        auto const ready = bcpp::concurrency::signal_id{7};

        test_support::require(signals.set(ready), "ready signal must be publishable");
        test_support::require(signals.select(hint, 1s) == ready,
                "timed select must consume an already-ready signal without waiting");
    }


    void timed_select_wakes_for_set()
    {
        blocking_signal_set<0> signals{64};
        std::latch entered{1};
        std::atomic<bcpp::concurrency::signal_id::value_type> selected
        {
            static_cast<bcpp::concurrency::signal_id::value_type>(bcpp::concurrency::signal_id::invalid())
        };

        std::jthread waiter
        {
            [&]
            {
                auto hint = bcpp::concurrency::signal_id{0};
                entered.count_down();
                selected.store(static_cast<bcpp::concurrency::signal_id::value_type>(signals.select(hint, 5s)));
            }
        };

        entered.wait();
        test_support::require(signals.set(bcpp::concurrency::signal_id{9}), "cross-thread signal must be publishable");
        waiter.join();

        test_support::require(selected.load() == 9, "empty-to-nonempty transition must wake a timed selector");
    }


    void stop_wakes_timed_select()
    {
        blocking_signal_set<0> signals{64};
        std::latch entered{1};
        std::atomic<bool> returnedInvalid{false};

        std::jthread waiter
        {
            [&]
            {
                auto hint = bcpp::concurrency::signal_id{0};
                entered.count_down();
                returnedInvalid.store(not signals.select(hint, 30s).valid());
            }
        };

        entered.wait();
        signals.stop();
        waiter.join();

        test_support::require(returnedInvalid.load(), "stop must wake a timed selector with an invalid signal");
    }


    void zero_timeout_is_one_immediate_attempt()
    {
        blocking_signal_tree<0> tree;
        blocking_signal_set<0> signals{64};
        auto treeHint = bcpp::concurrency::signal_id{0};
        auto setHint = bcpp::concurrency::signal_id{0};

        test_support::require(not tree.select(treeHint, 0ns).valid(),
                "zero-timeout tree select must immediately report no signal");
        test_support::require(not signals.select(setHint, 0ns).valid(),
                "zero-timeout set select must immediately report no signal");

        test_support::require(tree.set(bcpp::concurrency::signal_id{5}), "tree try-select signal must be publishable");
        test_support::require(signals.set(bcpp::concurrency::signal_id{7}), "set try-select signal must be publishable");
        treeHint = bcpp::concurrency::signal_id{0};
        setHint = bcpp::concurrency::signal_id{0};

        test_support::require(tree.select(treeHint, 0ns) == bcpp::concurrency::signal_id{5},
                "zero-timeout tree select must consume a ready signal");
        test_support::require(signals.select(setHint, 0ns) == bcpp::concurrency::signal_id{7},
                "zero-timeout set select must consume a ready signal");
    }


    void standalone_tree_timed_select_wakes_for_set()
    {
        blocking_signal_tree<1> tree;
        std::latch entered{1};
        std::atomic<bcpp::concurrency::signal_id::value_type> selected
        {
            static_cast<bcpp::concurrency::signal_id::value_type>(bcpp::concurrency::signal_id::invalid())
        };

        std::jthread waiter
        {
            [&]
            {
                auto hint = bcpp::concurrency::signal_id{0};
                entered.count_down();
                selected.store(static_cast<bcpp::concurrency::signal_id::value_type>(tree.select(hint, 5s)));
            }
        };

        entered.wait();
        test_support::require(tree.set(bcpp::concurrency::signal_id{99}), "standalone tree signal must be publishable");
        waiter.join();

        test_support::require(selected.load() == 99,
                "standalone blocking tree must wake for a zero-to-nonzero transition");
    }


    void standalone_tree_try_select_is_immediate()
    {
        blocking_signal_tree<0> tree;
        auto hint = bcpp::concurrency::signal_id{0};

        test_support::require(not tree.try_select(hint).valid(),
                "try_select must immediately report an empty blocking tree");
        test_support::require(tree.set(bcpp::concurrency::signal_id{11}), "try_select signal must be publishable");
        hint = bcpp::concurrency::signal_id{0};
        test_support::require(tree.try_select(hint) == bcpp::concurrency::signal_id{11},
                "try_select must consume a ready blocking-tree signal");
    }


    void standalone_tree_stop_wakes_timed_select()
    {
        blocking_signal_tree<0> tree;
        std::latch entered{1};
        std::atomic<bool> returnedInvalid{false};

        std::jthread waiter
        {
            [&]
            {
                auto hint = bcpp::concurrency::signal_id{0};
                entered.count_down();
                returnedInvalid.store(not tree.select(hint, 30s).valid());
            }
        };

        entered.wait();
        tree.stop();
        waiter.join();

        test_support::require(returnedInvalid.load(),
                "stop must wake a standalone tree selector with an invalid signal");
    }


    template <std::size_t tree_depth>
    void standalone_tree_count_tracks_cardinality()
    {
        blocking_signal_tree<tree_depth> tree;
        auto first = bcpp::concurrency::signal_id{1};
        auto second = bcpp::concurrency::signal_id{2};

        test_support::require(tree.non_empty_tree_count() == 0,
                "new standalone blocking tree must have zero nonempty trees");
        test_support::require(tree.set(first), "standalone tree must accept its first signal");
        test_support::require(tree.non_empty_tree_count() == 1,
                "first signal must make the standalone tree nonempty");
        test_support::require(tree.set(second), "standalone tree must accept another signal");
        test_support::require(tree.non_empty_tree_count() == 1,
                "additional signals must preserve standalone tree cardinality");

        auto hint = bcpp::concurrency::signal_id{0};
        test_support::require(tree.select(hint).valid(), "standalone tree signal must be selectable");
        test_support::require(tree.non_empty_tree_count() == 1,
                "selecting from a still-nonempty tree must preserve cardinality");
        test_support::require(tree.select(hint).valid(), "standalone tree final signal must be selectable");
        test_support::require(tree.non_empty_tree_count() == 0,
                "selecting the final signal must restore zero cardinality");
    }


    template <std::size_t tree_depth>
    void non_empty_tree_count_tracks_physical_trees()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<tree_depth>::capacity;
        blocking_signal_set<tree_depth> signals{treeCapacity * 2};
        auto first = bcpp::concurrency::signal_id{1};
        auto second = bcpp::concurrency::signal_id{2};
        auto otherTree = bcpp::concurrency::signal_id{treeCapacity + 1};

        test_support::require(signals.non_empty_tree_count() == 0,
                "new blocking signal set must have zero nonempty trees");
        test_support::require(signals.set(first), "first physical tree must accept a signal");
        test_support::require(signals.non_empty_tree_count() == 1,
                "first signal must increment the nonempty-tree count");
        test_support::require(not signals.set(first), "duplicate signal must remain idempotent");
        test_support::require(signals.non_empty_tree_count() == 1,
                "duplicate signal must not increment the nonempty-tree count");
        test_support::require(signals.set(second), "same physical tree must accept another signal");
        test_support::require(signals.non_empty_tree_count() == 1,
                "additional signal in one tree must not increment the tree count");
        test_support::require(signals.set(otherTree), "second physical tree must accept a signal");
        test_support::require(signals.non_empty_tree_count() == 2,
                "second physical tree must increment the nonempty-tree count");

        auto hint = bcpp::concurrency::signal_id{0};
        test_support::require(signals.select(hint).valid(), "first signal must be selectable");
        test_support::require(signals.non_empty_tree_count() == 2,
                "selecting from a still-nonempty tree must preserve the tree count");
        test_support::require(signals.select(hint).valid(), "second signal must be selectable");
        test_support::require(signals.non_empty_tree_count() == 1,
                "selecting the final signal in one tree must decrement the tree count");
        test_support::require(signals.select(hint).valid(), "other tree signal must be selectable");
        test_support::require(signals.non_empty_tree_count() == 0,
                "draining every tree must restore a zero tree count");
    }


    void timed_select_honors_timeout()
    {
        blocking_signal_set<0> signals{64};
        auto hint = bcpp::concurrency::signal_id{0};
        auto timeout = 20ms;
        auto started = std::chrono::steady_clock::now();

        auto selected = signals.select(hint, timeout);
        auto elapsed = std::chrono::steady_clock::now() - started;

        test_support::require(not selected.valid(), "timed select must return invalid on timeout");
        test_support::require(elapsed >= timeout / 2, "timed select must not return immediately");
        test_support::require(elapsed < 1s, "timed select must not substantially exceed its timeout");
    }


    void one_transition_wakes_multiple_selectors()
    {
        constexpr auto waiterCount = 4;
        blocking_signal_set<0> signals{64};
        std::latch ready{waiterCount};
        std::latch start{1};
        std::atomic<int> selectedCount{0};
        std::atomic<std::uint64_t> selectedMask{0};
        std::vector<std::jthread> waiters;

        for (auto waiter = 0; waiter < waiterCount; ++waiter)
            waiters.emplace_back([&]
            {
                auto hint = bcpp::concurrency::signal_id{0};
                ready.count_down();
                start.wait();
                auto selected = signals.select(hint, 2s);

                if (selected.valid())
                {
                    selectedCount.fetch_add(1);
                    selectedMask.fetch_or(1ull << static_cast<bcpp::concurrency::signal_id::value_type>(selected));
                }
            });

        ready.wait();
        start.count_down();

        for (auto signal = 0; signal < waiterCount; ++signal)
            test_support::require(signals.set(bcpp::concurrency::signal_id{static_cast<std::uint64_t>(signal)}),
                    "each waiting selector must receive a distinct signal");

        for (auto & waiter : waiters)
            waiter.join();

        test_support::require(selectedCount.load() == waiterCount,
                "one nonempty transition must release every selector needed to drain the tree");
        test_support::require(selectedMask.load() == 0xfull,
                "concurrent selectors must consume each published signal exactly once");
        test_support::require(signals.non_empty_tree_count() == 0,
                "concurrent drain must restore a zero tree count");
    }


    template <std::size_t tree_depth>
    void concurrent_shard_transitions_do_not_lose_wakes()
    {
        constexpr std::size_t shardCount = 4;
        constexpr std::size_t consumerCount = 4;
        constexpr std::size_t repetitionCount = 5'000;
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<tree_depth>::capacity;
        constexpr auto selectionCount = shardCount * repetitionCount;

        blocking_signal_set<tree_depth> signals{treeCapacity * shardCount};
        std::array<std::atomic<std::size_t>, shardCount> selectedPerShard;
        std::atomic<std::size_t> nextSelection{0};
        std::atomic<bool> failed{false};
        std::barrier start{shardCount + consumerCount};
        std::vector<std::jthread> producers;
        std::vector<std::jthread> consumers;

        for (auto & selected : selectedPerShard)
            selected.store(0, std::memory_order_relaxed);

        for (std::size_t shard = 0; shard < shardCount; ++shard)
            producers.emplace_back([&, shard]
            {
                auto signal = bcpp::concurrency::signal_id{(shard * treeCapacity) + 7};
                start.arrive_and_wait();

                for (std::size_t repetition = 0; repetition < repetitionCount; ++repetition)
                {
                    while ((not signals.set(signal)) && (not failed.load(std::memory_order_relaxed)))
                        std::this_thread::yield();

                    if (failed.load(std::memory_order_relaxed))
                        return;
                }
            });

        for (std::size_t consumer = 0; consumer < consumerCount; ++consumer)
            consumers.emplace_back([&, consumer]
            {
                auto hint = bcpp::concurrency::signal_id{(consumer % shardCount) * treeCapacity};
                start.arrive_and_wait();

                for (;;)
                {
                    auto selection = nextSelection.fetch_add(1, std::memory_order_relaxed);
                    if (selection >= selectionCount)
                        return;

                    auto selected = signals.select(hint, 10s);
                    if (not selected.valid())
                    {
                        if (not failed.exchange(true, std::memory_order_relaxed))
                            signals.stop();
                        return;
                    }

                    auto selectedValue = static_cast<bcpp::concurrency::signal_id::value_type>(selected);
                    auto shard = selectedValue / treeCapacity;
                    if ((shard >= shardCount) || (selectedValue != ((shard * treeCapacity) + 7)))
                    {
                        if (not failed.exchange(true, std::memory_order_relaxed))
                            signals.stop();
                        return;
                    }

                    selectedPerShard[shard].fetch_add(1, std::memory_order_relaxed);
                }
            });

        for (auto & producer : producers)
            producer.join();
        for (auto & consumer : consumers)
            consumer.join();

        test_support::require(not failed.load(std::memory_order_relaxed),
                "concurrent physical-tree transitions must not lose a blocking wake");
        for (auto const & selected : selectedPerShard)
            test_support::require(selected.load(std::memory_order_relaxed) == repetitionCount,
                    "every physical tree transition must be selected exactly once");
        test_support::require(signals.non_empty_tree_count() == 0,
                "concurrent physical-tree transitions must leave the shared count balanced");
    }


    void repeated_set_select_transitions_do_not_lose_wakes()
    {
        constexpr auto repetitionCount = 10'000;
        blocking_signal_set<0> signals{64};
        std::atomic<bool> failed{false};

        std::jthread selector
        {
            [&]
            {
                auto hint = bcpp::concurrency::signal_id{0};

                for (auto repetition = 0; repetition < repetitionCount; ++repetition)
                    if (signals.select(hint, 5s) != bcpp::concurrency::signal_id{7})
                    {
                        failed.store(true);
                        return;
                    }
            }
        };

        for (auto repetition = 0; repetition < repetitionCount; ++repetition)
            while (not signals.set(bcpp::concurrency::signal_id{7}))
                std::this_thread::yield();

        selector.join();

        test_support::require(not failed.load(), "repeated empty transitions must not lose a wake");
        test_support::require(signals.non_empty_tree_count() == 0,
                "repeated transitions must leave the nonempty-tree count balanced");
    }


    void repeated_standalone_tree_transitions_do_not_lose_wakes()
    {
        constexpr auto repetitionCount = 10'000;
        blocking_signal_tree<0> tree;
        std::atomic<bool> failed{false};

        std::jthread selector
        {
            [&]
            {
                auto hint = bcpp::concurrency::signal_id{0};

                for (auto repetition = 0; repetition < repetitionCount; ++repetition)
                    if (tree.select(hint, 5s) != bcpp::concurrency::signal_id{7})
                    {
                        failed.store(true);
                        return;
                    }
            }
        };

        for (auto repetition = 0; repetition < repetitionCount; ++repetition)
            while (not tree.set(bcpp::concurrency::signal_id{7}))
                std::this_thread::yield();

        selector.join();

        test_support::require(not failed.load(),
                "standalone tree empty transitions must not lose a wake");
        test_support::require(tree.non_empty_tree_count() == 0,
                "standalone tree transitions must leave cardinality balanced");
    }

} // namespace


int main()
{
    return test_support::run_suite([]
    {
        test_support::run("ordinary blocking-set select", ordinary_select_remains_immediate);
        test_support::run("timed select of ready signal", timed_select_returns_ready_signal);
        test_support::run("timed select wakes for set", timed_select_wakes_for_set);
        test_support::run("stop wakes timed select", stop_wakes_timed_select);
        test_support::run("zero timeout is immediate selection", zero_timeout_is_one_immediate_attempt);
        test_support::run("standalone tree timed select wakes",
                standalone_tree_timed_select_wakes_for_set);
        test_support::run("standalone tree try_select", standalone_tree_try_select_is_immediate);
        test_support::run("standalone tree stop wakes", standalone_tree_stop_wakes_timed_select);
        test_support::run("signal_tree<0> blocking cardinality",
                standalone_tree_count_tracks_cardinality<0>);
        test_support::run("signal_tree<1> blocking cardinality",
                standalone_tree_count_tracks_cardinality<1>);
        test_support::run("signal_tree<2> blocking cardinality",
                standalone_tree_count_tracks_cardinality<2>);
        test_support::run("signal_set<0> physical-tree count",
                non_empty_tree_count_tracks_physical_trees<0>);
        test_support::run("signal_set<1> physical-tree count",
                non_empty_tree_count_tracks_physical_trees<1>);
        test_support::run("signal_set<2> physical-tree count",
                non_empty_tree_count_tracks_physical_trees<2>);
        test_support::run("timed select timeout", timed_select_honors_timeout);
        test_support::run("one transition wakes multiple selectors",
                one_transition_wakes_multiple_selectors);
        test_support::run("signal_set<0> concurrent blocking shard transitions",
                concurrent_shard_transitions_do_not_lose_wakes<0>);
        test_support::run("signal_set<1> concurrent blocking shard transitions",
                concurrent_shard_transitions_do_not_lose_wakes<1>);
        test_support::run("signal_set<2> concurrent blocking shard transitions",
                concurrent_shard_transitions_do_not_lose_wakes<2>);
        test_support::run("repeated set/select transitions",
                repeated_set_select_transitions_do_not_lose_wakes);
        test_support::run("repeated standalone tree transitions",
                repeated_standalone_tree_transitions_do_not_lose_wakes);
    });
}
