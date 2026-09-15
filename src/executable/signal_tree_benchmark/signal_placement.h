#pragma once

#include <library/signal_tree.h>
#include <library/signal_tree/densest_child_selector.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>


namespace adversarial_throughput_benchmark
{

    template <std::size_t tree_size>
    using signal_tree = bcpp::concurrency::signal_tree<bcpp::concurrency::signal_tree_traits<
            tree_size, bcpp::concurrency::signal_tree_wrap_policy::wrap>>;

    //==============================================================================
    template <std::size_t RequiredCapacity, std::size_t N, bool Fits>
    struct signal_tree_level_for_impl;


    //==============================================================================
    template <std::size_t RequiredCapacity, std::size_t N>
    struct signal_tree_level_for_impl<RequiredCapacity, N, true>
    {
        static constexpr auto value = N;
    };


    //==============================================================================
    template <std::size_t RequiredCapacity, std::size_t N>
    struct signal_tree_level_for_impl<RequiredCapacity, N, false>
    {
        static constexpr auto value = signal_tree_level_for_impl
        <
            RequiredCapacity,
            N + 1,
            (signal_tree<N + 1>::capacity >= RequiredCapacity)
        >::value;
    };


    //==============================================================================
    template <std::size_t RequiredCapacity>
    inline constexpr auto signal_tree_level_for_v = signal_tree_level_for_impl
    <
        RequiredCapacity,
        0,
        (signal_tree<0>::capacity >= RequiredCapacity)
    >::value;


    //==============================================================================
    template <std::size_t ScheduledSignalCount>
    class signal_placement
    {
    public:

        static constexpr auto placement_tree_size = signal_tree_level_for_v<ScheduledSignalCount>;
        static constexpr auto physical_capacity   = signal_tree<placement_tree_size>::capacity;

        using signal_index = std::uint64_t;

        signal_placement()
            : physicalSignalForLogical_(ScheduledSignalCount)
        {
            signal_tree<placement_tree_size> placementTree;

            for (auto physical = 0ull; physical < physical_capacity; ++physical)
                placementTree.set(bcpp::concurrency::signal_id{physical});
            for (auto logical = 0ull; logical < ScheduledSignalCount; ++logical)
            {
                auto hint = bcpp::concurrency::signal_id{0};
                auto const physical = placementTree.template select<
                        bcpp::concurrency::densest_child_selector>(hint).signal;
                physicalSignalForLogical_[logical] = static_cast<signal_index>(physical);
            }
        }

        signal_index physical(signal_index logical) const noexcept
        {
            return physicalSignalForLogical_[static_cast<std::size_t>(logical)];
        }

        auto const & physical_signals() const noexcept
        {
            return physicalSignalForLogical_;
        }

    private:

        std::vector<signal_index> physicalSignalForLogical_;
    };

} // namespace adversarial_throughput_benchmark
