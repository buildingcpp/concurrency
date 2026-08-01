#include "test_support.h"

#include <include/signal_tree.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>


namespace
{

    constexpr auto value(bcpp::signal_id signal) noexcept
    {
        return static_cast<bcpp::signal_id::value_type>(signal);
    }


    static_assert(std::same_as<decltype(bcpp::signal_set{std::size_t{1}}), bcpp::signal_set<1>>);


    template <std::size_t N>
    void capacity_rounding()
    {
        constexpr auto treeCapacity = bcpp::signal_tree<N>::capacity;

        bcpp::signal_set<N> zeroRequested{0};
        bcpp::signal_set<N> oneRequested{1};
        bcpp::signal_set<N> exactTree{treeCapacity};
        bcpp::signal_set<N> justOverTree{treeCapacity + 1};
        bcpp::signal_set<N> partialThirdTree{(treeCapacity * 2) + 7};

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
        constexpr auto treeCapacity = bcpp::signal_tree<N>::capacity;
        bcpp::signal_set<N> signals{treeCapacity / 2};
        auto const low = bcpp::signal_id{0};
        auto const high = bcpp::signal_id{treeCapacity - 1};

        test_support::require(not signals.set(bcpp::signal_id::invalid()), "invalid signal id must be rejected");
        test_support::require(not signals.set(bcpp::signal_id{signals.max() + 1}),
                "signal id beyond rounded capacity must be rejected");
        test_support::require(signals.set(low), "low signal must be publishable");
        test_support::require(signals.set(high), "rounded-capacity tail signal must be publishable");
        test_support::require(not signals.set(low), "duplicate signal-set publication must be idempotent");

        auto hint = bcpp::signal_id{0};
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
        constexpr auto treeCapacity = bcpp::signal_tree<N>::capacity;
        constexpr auto physicalCapacity = treeCapacity * 3;
        bcpp::signal_set<N> signals{physicalCapacity - 7};

        for (std::uint64_t index = 0; index < physicalCapacity; ++index)
            test_support::require(signals.set(bcpp::signal_id{index}),
                    "every signal in every physical tree must be publishable");

        std::vector<unsigned char> seen(physicalCapacity, 0);
        auto hint = bcpp::signal_id{treeCapacity};

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
        constexpr auto treeCapacity = bcpp::signal_tree<N>::capacity;
        bcpp::signal_set<N> signals{treeCapacity * 3};
        auto const middleTreeSignal = bcpp::signal_id{treeCapacity + 3};
        auto const finalTreeSignal = bcpp::signal_id{(treeCapacity * 2) + 4};
        auto hint = bcpp::signal_id{treeCapacity};

        test_support::require(signals.set(middleTreeSignal), "middle-tree signal must be publishable");
        test_support::require(signals.set(finalTreeSignal), "final-tree signal must be publishable");
        test_support::require(signals.select(hint) == middleTreeSignal,
                "selection must begin in the physical tree named by the hint");
        test_support::require(signals.select(hint) == finalTreeSignal,
                "selection must continue into the next nonempty physical tree");
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
    });
}
