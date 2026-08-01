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

    template <std::size_t N>
    inline constexpr std::size_t tree_capacity_v = node_traits<N>::node_capacity;


    //==============================================================================
    template <std::size_t N>
    class alignas(64) signal_tree
    {
    public:

        static auto constexpr capacity = tree_capacity_v<N>;

        bool set(signal_id) noexcept;
        bool empty() const noexcept;

        template <selector_concept Selector = fairness_selector>
        [[gnu::always_inline]] inline signal_id select(signal_id &) noexcept;

    private:

        using layout = detail::signal_tree_layout<N>;

        template <std::size_t Level>
        aligned_node & node(signal_id) noexcept;

        template <std::size_t Level>
        bool set(signal_id) noexcept;

        template <std::size_t Level, typename Selector>
        signal_id select(signal_id, signal_id &, Selector &) noexcept;

        std::array<aligned_node, layout::node_count> node_;
    };

    signal_tree() -> signal_tree<1>;

} // namespace bcpp


//==============================================================================
template <std::size_t N>
template <bcpp::selector_concept Selector>
[[gnu::always_inline]] inline auto bcpp::signal_tree<N>::select
(
    signal_id & hint
) noexcept -> signal_id
{
    if (not hint.valid() || hint.value_ >= capacity)
        hint = signal_id{0};

    Selector selector{std::integral_constant<std::size_t, capacity>{}};
    signal_id nextHintCandidate;
    auto selected = select<N>(hint, nextHintCandidate, selector);

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
template <std::size_t N>
inline bool bcpp::signal_tree<N>::set
(
    signal_id index
) noexcept
{
    return set<0>(index);
}


//==============================================================================
template <std::size_t N>
inline bool bcpp::signal_tree<N>::empty
(
) const noexcept
{
    constexpr auto rootOffset = layout::template level_offset<N>();
    return node_[rootOffset].value_.load() == 0;
}


//==============================================================================
template <std::size_t N>
template <std::size_t Level>
inline auto bcpp::signal_tree<N>::node
(
    signal_id signal
) noexcept -> aligned_node &
{
    using traits = node_traits<Level>;
    constexpr auto levelOffset = layout::template level_offset<Level>();
    auto const nodeIndex = (signal.value_ % capacity) / traits::node_capacity;
    return node_[levelOffset + nodeIndex];
}


//==============================================================================
template <std::size_t N>
template <std::size_t Level>
inline bool bcpp::signal_tree<N>::set
(
    signal_id signal
) noexcept
{
    static_assert(Level <= N);

    if constexpr (Level == 0)
    {
        using traits = node_traits<0>;
        auto & leaf = node<0>(signal);
        auto const signalBit = std::uint64_t{1} << (signal.value_ % traits::lanes_per_node);
        auto const setSuccessful = (leaf.value_.fetch_or(signalBit, std::memory_order_release) & signalBit) == 0;

        if constexpr (N > 0)
            if (setSuccessful)
                set<1>(signal);

        return setSuccessful;
    }
    else
    {
        using traits = node_traits<Level>;
        using child_traits = node_traits<Level - 1>;
        auto & branch = node<Level>(signal);
        auto const laneIndex = (signal.value_ / child_traits::node_capacity) % traits::lanes_per_node;
        auto const addend = std::uint64_t{1} << (laneIndex * traits::lane_width);

        branch.value_.fetch_add(addend, std::memory_order_release);

        if constexpr (Level < N)
            set<Level + 1>(signal);

        return true;
    }
}


//==============================================================================
template <std::size_t N>
template <std::size_t Level, typename Selector>
inline auto bcpp::signal_tree<N>::select
(
    signal_id hint,
    signal_id & naturalNextOut,
    Selector & selector
) noexcept -> signal_id
{
    static_assert(Level <= N);

    if constexpr (Level == 0)
    {
        using traits = node_traits<0>;
        static constexpr auto leafSlotMask = (std::uint64_t{1} << traits::hint_width) - std::uint64_t{1};

        auto & leaf = node<0>(hint);
        auto const nodeBase = hint.value_ & ~leafSlotMask;
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
                        ? ~std::uint64_t{0} << (lane + 1ull)
                        : std::uint64_t{0};
                auto const next = candidate & nextMask;

                naturalNextOut = (next != 0)
                        ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))}
                        : signal_id{};
                return selected;
            }

            observed = candidate;
        }

        if constexpr (N == 0)
        {
            naturalNextOut = {};
            return {};
        }
        else
        {
            observed = candidate;

            while (true)
            {
                auto const lane = static_cast<std::size_t>(std::countr_zero(observed));
                auto const bit = std::uint64_t{1} << lane;
                auto const prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
                candidate = prior & ~bit;

                if ((prior & bit) != 0)
                {
                    auto const selected = hint.template with_lane<0>(lane);
                    auto const nextMask = (lane + 1ull < traits::lanes_per_node)
                            ? ~std::uint64_t{0} << (lane + 1ull)
                            : std::uint64_t{0};
                    auto const next = candidate & nextMask;

                    naturalNextOut = (next != 0)
                            ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))}
                            : signal_id{};
                    return selected;
                }

                observed = candidate;
            }
        }
    }
    else
    {
        using traits = node_traits<Level>;
        auto & branch = node<Level>(hint);
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

                if (auto const localHint = hint.template lane<Level>(); selected != localHint)
                    hint = hint.template with_lane<Level>(selected);

                return select<Level - 1>(hint, naturalNextOut, selector);
            }
        }

        naturalNextOut = {};
        return {};
    }
}
