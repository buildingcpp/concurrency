#pragma once

#include "./fairness_selector.h"
#include "./signal_id.h"
#include "./signal_tree_layout.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>


namespace bcpp
{

    template <std::size_t tree_depth>
    inline constexpr std::size_t tree_capacity_v = node_traits<tree_depth>::node_capacity;


    //==============================================================================
    template <std::size_t tree_depth>
    class alignas(64) signal_tree
    {
    public:

        enum class wrap_policy
        {
            wrap,
            no_wrap
        };

        static auto constexpr capacity = tree_capacity_v<tree_depth>;

        bool set(signal_id) noexcept;
        bool empty() const noexcept;

        template <selector_concept Selector = fairness_selector, wrap_policy policy = wrap_policy::wrap>
        [[gnu::always_inline]] inline signal_id select(signal_id &) noexcept;

    private:

        using layout = detail::signal_tree_layout<tree_depth>;

        template <std::size_t current_level>
        aligned_node & node(signal_id) noexcept;

        template <std::size_t current_level>
        bool set(signal_id) noexcept;

        template <std::size_t current_level, wrap_policy policy, typename Selector>
            requires node_traits<current_level>::is_leaf
        signal_id select(signal_id, signal_id &, Selector &) noexcept;

        template <std::size_t current_level, wrap_policy policy, typename Selector>
            requires (not node_traits<current_level>::is_leaf)
        signal_id select(signal_id, signal_id &, Selector &) noexcept;

        std::array<aligned_node, layout::node_count> node_;
    };

    signal_tree() -> signal_tree<1>;

} // namespace bcpp


//==============================================================================
template <std::size_t tree_depth>
template <bcpp::selector_concept Selector, typename bcpp::signal_tree<tree_depth>::wrap_policy policy>
[[gnu::always_inline]] inline auto bcpp::signal_tree<tree_depth>::select
(
    signal_id & hint
) noexcept -> signal_id
{
    if ((not hint.valid()) || (hint.value_ >= capacity))
        hint = signal_id{0};

    Selector selector{std::integral_constant<std::size_t, capacity>{}};
    signal_id nextHintCandidate;
    auto selected = select<tree_depth, policy>(hint, nextHintCandidate, selector);

    if (not selected.valid())
    {
        hint = {};
        return {};
    }
    auto b = selector.mask_ & (~selector.mask_ + 1ull);
    auto fallback = (b != 0) ? signal_id{(selected.value_ | b) & ~(b - 1ull)} : signal_id{};
    auto useCandidate = (selected.value_ < nextHintCandidate.value_) & (nextHintCandidate.value_ < capacity);
    hint = useCandidate ? nextHintCandidate : fallback;
    return selected;
}


//==============================================================================
template <std::size_t tree_depth>
inline bool bcpp::signal_tree<tree_depth>::set
(
    signal_id index
) noexcept
{
    return set<0>(index);
}


//==============================================================================
template <std::size_t tree_depth>
inline bool bcpp::signal_tree<tree_depth>::empty
(
) const noexcept
{
    constexpr auto rootOffset = layout::template level_offset<tree_depth>();
    return node_[rootOffset].value_.load() == 0;
}


//==============================================================================
template <std::size_t tree_depth>
template <std::size_t current_level>
inline auto bcpp::signal_tree<tree_depth>::node
(
    signal_id signal
) noexcept -> aligned_node &
{
    using traits = node_traits<current_level>;
    constexpr auto levelOffset = layout::template level_offset<current_level>();
    auto const nodeIndex = (signal.value_ % capacity) / traits::node_capacity;
    return node_[levelOffset + nodeIndex];
}


//==============================================================================
template <std::size_t tree_depth>
template <std::size_t current_level>
inline bool bcpp::signal_tree<tree_depth>::set
(
    signal_id signal
) noexcept
{
    static constexpr bool is_root = (current_level == tree_depth);

    if constexpr (current_level == 0)
    {
        using traits = node_traits<0>;
        auto & leaf = node<0>(signal);
        auto const signalBit = std::uint64_t{1} << (signal.value_ % traits::lanes_per_node);
        auto const setSuccessful = (leaf.value_.fetch_or(signalBit, std::memory_order_release) & signalBit) == 0;
        if constexpr (not is_root)
            if (setSuccessful)
                set<1>(signal);
        return setSuccessful;
    }
    else
    {
        using traits = node_traits<current_level>;
        using child_traits = node_traits<current_level - 1>;
        auto & branch = node<current_level>(signal);
        auto const laneIndex = (signal.value_ / child_traits::node_capacity) % traits::lanes_per_node;
        auto const addend = std::uint64_t{1} << (laneIndex * traits::lane_width);

        branch.value_.fetch_add(addend, std::memory_order_release);
        if constexpr (not is_root)
            set<current_level + 1>(signal);
        return true;
    }
}


//==============================================================================
template <std::size_t tree_depth>
template <std::size_t current_level,
        typename bcpp::signal_tree<tree_depth>::wrap_policy policy, typename Selector>
    requires bcpp::node_traits<current_level>::is_leaf
inline auto bcpp::signal_tree<tree_depth>::select
(
    signal_id hint,
    signal_id & nextHintCandidate,
    Selector &
) noexcept -> signal_id
{
    static constexpr bool is_root = (current_level == tree_depth);

    using traits = node_traits<0>;
    static constexpr auto leafSlotMask = (1ull << traits::hint_width) - 1ull;

    auto & leaf = node<0>(hint);
    auto const nodeBase = static_cast<std::uint64_t>(hint.value_ & ~leafSlotMask);
    auto const hintedLane = hint.value_ & leafSlotMask;
    auto candidate = leaf.value_.load(std::memory_order_relaxed);
    auto const forwardMask = ~std::uint64_t{0} << hintedLane;
    auto observed = candidate & forwardMask;

    while (observed != 0)
    {
        auto const lane = static_cast<std::size_t>(std::countr_zero(observed));
        auto const bit = std::uint64_t{1} << lane;
        auto const prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
        candidate = prior & ~bit;

        if ((prior & bit) != 0)
        {
            auto const selected = hint.template with_lane<0>(lane);
            auto const nextMask = (lane + 1ull < traits::lanes_per_node)
                    ? ~std::uint64_t{0} << (lane + 1ull) : std::uint64_t{0};
            auto const next = candidate & nextMask;

            nextHintCandidate = (next != 0)
                    ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))} : signal_id{};
            return selected;
        }
        observed = candidate & forwardMask;
    }

    if constexpr ((is_root) && (policy == wrap_policy::no_wrap))
    {
        nextHintCandidate = {};
        return {};
    }
    else
    {
        observed = candidate;

        while ((not is_root) || (observed != 0))
        {
            auto const lane = static_cast<std::size_t>(std::countr_zero(observed));
            auto const bit = std::uint64_t{1} << lane;
            auto const prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
            candidate = prior & ~bit;

            if ((prior & bit) != 0)
            {
                auto const selected = hint.template with_lane<0>(lane);
                auto const nextMask = (lane + 1ull < traits::lanes_per_node)
                        ? ~std::uint64_t{0} << (lane + 1ull) : std::uint64_t{0};
                auto const next = candidate & nextMask;
                nextHintCandidate = (next != 0)
                        ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))} : signal_id{};
                return selected;
            }

            observed = candidate;
        }

        nextHintCandidate = {};
        return {};
    }
}


//==============================================================================
template <std::size_t tree_depth>
template <std::size_t current_level,
        typename bcpp::signal_tree<tree_depth>::wrap_policy policy, typename Selector>
    requires (not bcpp::node_traits<current_level>::is_leaf)
inline auto bcpp::signal_tree<tree_depth>::select
(
    signal_id hint,
    signal_id & nextHintCandidate,
    Selector & selector
) noexcept -> signal_id
{
    using traits = node_traits<current_level>;
    auto & branch = node<current_level>(hint);
    auto candidate = branch.value_.load(std::memory_order_relaxed);

    while (candidate != 0)
    {
        auto attemptSelector = selector;
        auto const selected = attemptSelector.select(traits{}, hint.value_, candidate);
        auto const laneAddend = std::uint64_t{1} << (selected * traits::lane_width);

        if (branch.value_.compare_exchange_strong(candidate, candidate - laneAddend, std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            selector = attemptSelector;
            if (auto const localHint = hint.template lane<current_level>(); selected != localHint)
                hint = hint.template with_lane<current_level>(selected);
            return select<current_level - 1, policy>(hint, nextHintCandidate, selector);
        }
    }

    nextHintCandidate = {};
    return {};
}
