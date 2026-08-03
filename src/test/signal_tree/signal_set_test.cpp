#include "test_support.h"

#include <library/signal_tree.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <type_traits>
#include <vector>


namespace
{

    constexpr auto value(bcpp::concurrency::signal_id signal) noexcept
    {
        return static_cast<bcpp::concurrency::signal_id::value_type>(signal);
    }


    static_assert(std::same_as<decltype(bcpp::concurrency::signal_set{std::size_t{1}}), bcpp::concurrency::signal_set<1>>);


    template <std::size_t N>
    void capacity_rounding()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<N>::capacity;

        bcpp::concurrency::signal_set<N> zeroRequested{0};
        bcpp::concurrency::signal_set<N> oneRequested{1};
        bcpp::concurrency::signal_set<N> exactTree{treeCapacity};
        bcpp::concurrency::signal_set<N> justOverTree{treeCapacity + 1};
        bcpp::concurrency::signal_set<N> partialThirdTree{(treeCapacity * 2) + 7};

        test_support::require(zeroRequested.max() == treeCapacity - 1,
                "zero requested capacity must still allocate one physical tree");
        test_support::require(oneRequested.max() == treeCapacity - 1,
                "requested capacity must round to one physical tree");
        test_support::require(exactTree.max() == treeCapacity - 1,
                "exact physical-tree capacity must not over-allocate");
        test_support::require(justOverTree.max() == (treeCapacity * 2) - 1,
                "capacity crossing a shard boundary must allocate another tree");
        test_support::require(partialThirdTree.max() == (treeCapacity * 3) - 1,
                "capacity must round to the complete final physical tree");
    }


    template <std::size_t N>
    void one_tree_behavior()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<N>::capacity;
        bcpp::concurrency::signal_set<N> signals{treeCapacity / 2};
        auto const low = bcpp::concurrency::signal_id{0};
        auto const high = bcpp::concurrency::signal_id{treeCapacity - 1};

        test_support::require(not signals.set(bcpp::concurrency::signal_id::invalid()), "invalid signal id must be rejected");
        test_support::require(not signals.set(bcpp::concurrency::signal_id{signals.max() + 1}),
                "signal id beyond rounded capacity must be rejected");
        test_support::require(signals.set(low), "low signal must be publishable");
        test_support::require(signals.set(high), "rounded-capacity tail signal must be publishable");
        test_support::require(not signals.set(low), "duplicate signal-set publication must be idempotent");

        auto hint = bcpp::concurrency::signal_id{0};
        auto first = signals.select(hint);
        auto second = signals.select(hint);
        auto third = signals.select(hint);

        test_support::require(first == low, "selection must begin at the supplied low hint");
        test_support::require(second == high, "selection must reach the other ready signal");
        test_support::require(not third.valid(), "one-tree set must be empty after both selections");
        test_support::require(signals.set(first), "selected signal must be publishable again");
    }


    template <std::size_t N>
    void three_tree_sharding()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<N>::capacity;
        constexpr auto physicalCapacity = treeCapacity * 3;
        bcpp::concurrency::signal_set<N> signals{physicalCapacity - 7};

        for (std::uint64_t index = 0; index < physicalCapacity; ++index)
            test_support::require(signals.set(bcpp::concurrency::signal_id{index}),
                    "every signal in every physical tree must be publishable");

        std::vector<unsigned char> seen(physicalCapacity, 0);
        auto hint = bcpp::concurrency::signal_id{treeCapacity};

        for (std::size_t selectedCount = 0; selectedCount < physicalCapacity; ++selectedCount)
        {
            auto const selected = signals.select(hint);
            test_support::require(selected.valid(), "sharded set must yield every published signal");
            auto const selectedValue = value(selected);
            test_support::require(selectedValue < physicalCapacity, "sharded selection must remain in capacity");
            test_support::require(seen[selectedValue]++ == 0, "sharded signal must not be selected twice");
        }

        test_support::require(not signals.select(hint).valid(), "drained sharded set must not yield another signal");

        for (auto count : seen)
            test_support::require(count == 1, "every sharded signal must be selected exactly once");
    }


    template <std::size_t N>
    void selection_starts_in_the_hinted_tree()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<N>::capacity;
        bcpp::concurrency::signal_set<N> signals{treeCapacity * 3};
        auto const middleTreeSignal = bcpp::concurrency::signal_id{treeCapacity + 3};
        auto const finalTreeSignal = bcpp::concurrency::signal_id{(treeCapacity * 2) + 4};
        auto hint = bcpp::concurrency::signal_id{treeCapacity};

        test_support::require(signals.set(middleTreeSignal), "middle-tree signal must be publishable");
        test_support::require(signals.set(finalTreeSignal), "final-tree signal must be publishable");
        test_support::require(signals.select(hint) == middleTreeSignal,
                "selection must begin in the physical tree named by the hint");
        test_support::require(signals.select(hint) == finalTreeSignal,
                "selection must continue into the next nonempty physical tree");
    }


    template <typename MakeSignals>
    void require_leaf_selection
    (
        std::string_view caseName,
        MakeSignals makeSignals,
        std::initializer_list<std::uint64_t> ready,
        std::uint64_t initialHint,
        std::initializer_list<std::uint64_t> expected
    )
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<0>::capacity;
        auto signals = makeSignals();

        for (auto signal : ready)
            test_support::require(signals.set(bcpp::concurrency::signal_id{signal}), caseName);

        auto hint = bcpp::concurrency::signal_id{initialHint};

        for (auto selected = expected.begin(); selected != expected.end(); ++selected)
        {
            test_support::require(signals.select(hint) == bcpp::concurrency::signal_id{*selected}, caseName);

            auto const next = selected + 1;
            auto const sameTree = (next != expected.end()) && ((*selected / treeCapacity) == (*next / treeCapacity));
            if ((sameTree) && (*selected < *next))
                test_support::require(hint == bcpp::concurrency::signal_id{*next}, caseName);
        }

        test_support::require(not signals.select(hint).valid(), caseName);
        test_support::require(not hint.valid(), caseName);
    }


    template <typename MakeSignals>
    void run_local_leaf_selection_matrix
    (
        MakeSignals makeSignals,
        std::uint64_t offset
    )
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<0>::capacity;

        require_leaf_selection("one signal, hint before", makeSignals,
                {offset + 3}, offset + 2, {offset + 3});
        require_leaf_selection("one signal, hint at", makeSignals,
                {offset + 3}, offset + 3, {offset + 3});
        require_leaf_selection("one signal, hint after", makeSignals,
                {offset + 3}, offset + treeCapacity - 2, {offset + 3});
        require_leaf_selection("two signals, hint before", makeSignals,
                {offset + 3, offset + 5}, offset + 2, {offset + 3, offset + 5});
        require_leaf_selection("two signals, hint at first", makeSignals,
                {offset + 3, offset + 5}, offset + 3, {offset + 3, offset + 5});
        require_leaf_selection("two signals, hint between", makeSignals,
                {offset + 3, offset + 5}, offset + 4, {offset + 5, offset + 3});
        require_leaf_selection("two signals, hint at second", makeSignals,
                {offset + 3, offset + 5}, offset + 5, {offset + 5, offset + 3});
        require_leaf_selection("two signals, hint after", makeSignals,
                {offset + 3, offset + 5}, offset + treeCapacity - 2, {offset + 3, offset + 5});
    }


    void one_shard_leaf_selection_matrix()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<0>::capacity;
        run_local_leaf_selection_matrix([=] { return bcpp::concurrency::signal_set<0>{treeCapacity}; }, 0);
    }


    void two_shard_leaf_selection_matrix()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<0>::capacity;
        run_local_leaf_selection_matrix([=] { return bcpp::concurrency::signal_set<0>{treeCapacity * 2}; }, treeCapacity);
    }


    void many_shard_leaf_selection_matrix()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<0>::capacity;
        run_local_leaf_selection_matrix([=] { return bcpp::concurrency::signal_set<0>{treeCapacity * 9}; }, treeCapacity * 4);
    }


    void leaf_shard_traversal_matrix()
    {
        constexpr auto treeCapacity = bcpp::concurrency::signal_tree<0>::capacity;

        require_leaf_selection("two shards visit next before wrap",
                [=] { return bcpp::concurrency::signal_set<0>{treeCapacity * 2}; },
                {3, 5, treeCapacity + 4}, treeCapacity - 2, {treeCapacity + 4, 3, 5});
        require_leaf_selection("three shards skip empty middle",
                [=] { return bcpp::concurrency::signal_set<0>{treeCapacity * 3}; },
                {3, 5, (treeCapacity * 2) + 4}, treeCapacity - 2, {(treeCapacity * 2) + 4, 3, 5});
        require_leaf_selection("three shards preserve circular order",
                [=] { return bcpp::concurrency::signal_set<0>{treeCapacity * 3}; },
                {3, treeCapacity + 4, (treeCapacity * 2) + 5}, treeCapacity - 2,
                {treeCapacity + 4, (treeCapacity * 2) + 5, 3});
        require_leaf_selection("many shards skip empty trees",
                [=] { return bcpp::concurrency::signal_set<0>{treeCapacity * 9}; },
                {3, 5, (treeCapacity * 8) + 4}, treeCapacity - 2, {(treeCapacity * 8) + 4, 3, 5});
    }

} // namespace


int main()
{
    return test_support::run_suite([]
    {
        test_support::run("signal_set<0> capacity rounding", capacity_rounding<0>);
        test_support::run("signal_set<1> capacity rounding", capacity_rounding<1>);
        test_support::run("signal_set<2> capacity rounding", capacity_rounding<2>);
        test_support::run("signal_set<0> one tree", one_tree_behavior<0>);
        test_support::run("signal_set<1> one tree", one_tree_behavior<1>);
        test_support::run("signal_set<2> one tree", one_tree_behavior<2>);
        test_support::run("signal_set<0> three trees", three_tree_sharding<0>);
        test_support::run("signal_set<1> three trees", three_tree_sharding<1>);
        test_support::run("signal_set<2> three trees", three_tree_sharding<2>);
        test_support::run("signal_set<0> hinted tree", selection_starts_in_the_hinted_tree<0>);
        test_support::run("signal_set<1> hinted tree", selection_starts_in_the_hinted_tree<1>);
        test_support::run("signal_set<2> hinted tree", selection_starts_in_the_hinted_tree<2>);
        test_support::run("signal_set<0> one-shard leaf selection matrix", one_shard_leaf_selection_matrix);
        test_support::run("signal_set<0> two-shard leaf selection matrix", two_shard_leaf_selection_matrix);
        test_support::run("signal_set<0> many-shard leaf selection matrix", many_shard_leaf_selection_matrix);
        test_support::run("signal_set<0> leaf shard traversal matrix", leaf_shard_traversal_matrix);
    });
}
