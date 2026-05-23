#pragma once

#include "./lane_traits.h"
#include "./node_traits.h"
#include "./signal_id.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>


namespace bcpp
{

    struct alignas(64) aligned_node
    {
        std::atomic<std::uint64_t> value_{0};
    };

    struct unaligned_node
    {
        std::atomic<std::uint64_t> value_{0};
    };

    template <std::size_t, std::size_t>
    class signal_tree_level;


    //==============================================================================
    template <std::size_t capacity>
    class alignas(64) signal_tree_level<0, capacity>
    {
    public:

        using value_type = std::uint64_t;

        static auto constexpr node_width     = std::numeric_limits<value_type>::digits;
        static auto constexpr lane_width     = 1ull;
        static auto constexpr lanes_per_node = node_width / lane_width;
        static auto constexpr lane_capacity  = (1ull << lane_width) - 1ull;
        static auto constexpr node_capacity  = lanes_per_node * lane_capacity;
        static auto constexpr node_count     = capacity / node_capacity;
        static auto constexpr is_root        = (node_count == 1ull);
        static auto constexpr is_leaf        = true;
        static auto constexpr hint_width     = std::bit_width(lanes_per_node - 1ull);
        static auto constexpr hint_offset    = 0ull;
        static auto constexpr level_mask     = (1ull << (hint_offset + hint_width)) - 1;

        bool set(signal_id) noexcept;

        template <typename Selector>
        signal_id select_lane(signal_id, signal_id &, Selector &) noexcept;

        bool empty() const noexcept requires (is_root);
        aligned_node & get_node(signal_id) noexcept;

        std::array<aligned_node, node_count> node_;
    };

    template <std::size_t capacity>
    using leaf_level = signal_tree_level<0, capacity>;


    //==============================================================================
    template <std::size_t N, std::size_t capacity>
    class alignas(64) signal_tree_level : protected signal_tree_level<N - 1, capacity>
    {
    public:

        using child_level = signal_tree_level<N - 1, capacity>;
        using value_type = typename child_level::value_type;

        static auto constexpr node_width     = child_level::node_width;
        static auto constexpr lane_capacity  = child_level::node_capacity;
        static auto constexpr lane_width     = std::bit_ceil(static_cast<unsigned>(std::bit_width(lane_capacity)));
        static auto constexpr lanes_per_node = node_width / lane_width;
        static auto constexpr node_capacity  = lanes_per_node * lane_capacity;
        static auto constexpr node_count     = capacity / node_capacity;
        static auto constexpr is_root        = (node_count == 1ull);
        static auto constexpr is_leaf        = (N == 0);
        static auto constexpr hint_width     = std::bit_width(lanes_per_node - 1ull);
        static auto constexpr hint_offset    = child_level::hint_offset + child_level::hint_width;
        static auto constexpr level_mask     = (1ull << (hint_offset + hint_width)) - 1;

        void set(signal_id) noexcept requires (not is_leaf);

        template <typename Selector>
        signal_id select_lane(signal_id, signal_id &, Selector &) noexcept requires (not is_leaf);

        bool empty() const noexcept requires (is_root);
        aligned_node & get_node(signal_id) noexcept;

        std::array<aligned_node, node_count> node_;
    };

} // namespace bcpp


//==============================================================================
template <std::size_t capacity>
inline bool bcpp::leaf_level<capacity>::empty
(
) const noexcept requires (is_root)
{
    return node_[0].value_.load() == 0;
}


//==============================================================================
template <std::size_t N, std::size_t capacity>
bool bcpp::signal_tree_level<N, capacity>::empty
(
) const noexcept requires (is_root)
{
    return node_[0].value_.load() == 0;
}


//==============================================================================
template <std::size_t capacity>
inline bool bcpp::leaf_level<capacity>::set
(
    signal_id signalId
) noexcept
{
    auto & node = get_node(signalId);
    auto signalBit = 1ull << (signalId.value_ % lanes_per_node);
    auto setSuccessful = (node.value_.fetch_or(signalBit, std::memory_order_release) & signalBit) == 0;

    if constexpr (not is_root)
        if (setSuccessful)
            (reinterpret_cast<signal_tree_level<1, capacity> *>(this))->set(signalId);

    return setSuccessful;
}


//==============================================================================
template <std::size_t N, std::size_t capacity>
void bcpp::signal_tree_level<N, capacity>::set
(
    signal_id signalId
) noexcept requires (not is_leaf)
{
    auto & node = get_node(signalId);
    auto laneIndex = (signalId.value_ / child_level::node_capacity) % lanes_per_node;
    auto addend = 1ull << (laneIndex * lane_width);

    node.value_.fetch_add(addend, std::memory_order_release);

    if constexpr (not is_root)
        (reinterpret_cast<signal_tree_level<N + 1, capacity> *>(this))->set(signalId);
}


//==============================================================================
template <std::size_t capacity>
inline auto bcpp::leaf_level<capacity>::get_node
(
    signal_id signalId
) noexcept -> aligned_node &
{
    auto laneIndex = signalId.value_ % capacity;
    auto nodeIndex = laneIndex / node_capacity;
    return node_[nodeIndex];
}


//==============================================================================
template <std::size_t capacity>
template <typename Selector>
inline auto bcpp::leaf_level<capacity>::select_lane
(
    signal_id hint,
    signal_id & newHintOut,
    Selector &
) noexcept -> signal_id
{
    static auto constexpr leaf_slot_mask = (1ull << hint_width) - 1ull;

    auto & node = get_node(hint);
    auto nodeBase = hint.value_ & ~leaf_slot_mask;
    auto hintedLane = hint.value_ & leaf_slot_mask;
    auto candidate = node.value_.load(std::memory_order_relaxed);

    auto forwardMask = ~0ull << hintedLane;
    auto observed = candidate & forwardMask;

    while (observed != 0)
    {
        auto lane = static_cast<std::size_t>(std::countr_zero(observed));
        auto bit = 1ull << lane;
        auto prior = node.value_.fetch_and(~bit, std::memory_order_acq_rel);
        candidate = prior & ~bit;

        if (prior & bit)
        {
            auto selected = hint.template with_lane<0>(lane);
            auto nextMask = (lane + 1ull < lanes_per_node) ? ~0ull << (lane + 1ull) : 0ull;
            auto next = candidate & nextMask;

            newHintOut = (next != 0) ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))}
                    : signal_id{};
            return selected;
        }

        observed = candidate & forwardMask;
    }

    if constexpr (is_root)
    {
        newHintOut = {};
        return {};
    }
    else
    {
        observed = candidate;

        while (true)
        {
            auto lane = static_cast<std::size_t>(std::countr_zero(observed));
            auto bit = 1ull << lane;
            auto prior = node.value_.fetch_and(~bit, std::memory_order_acq_rel);
            candidate = prior & ~bit;

            if (prior & bit)
            {
                auto selected = hint.template with_lane<0>(lane);
                auto nextMask = (lane + 1ull < lanes_per_node) ? ~0ull << (lane + 1ull) : 0ull;
                auto next = candidate & nextMask;

                newHintOut = (next != 0) ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))}
                        : signal_id{};
                return selected;
            }
            observed = candidate;
        }
    }
}


//==============================================================================
template <std::size_t N, std::size_t capacity>
inline auto bcpp::signal_tree_level<N, capacity>::get_node
(
    signal_id signalId
) noexcept -> aligned_node &
{
    auto laneIndex = signalId.value_ % capacity;
    auto nodeIndex = laneIndex / node_capacity;
    return node_[nodeIndex];
}


//==============================================================================
template <std::size_t N, std::size_t capacity>
template <typename Selector>
auto bcpp::signal_tree_level<N, capacity>::select_lane
(
    signal_id hint,
    signal_id & naturalNextOut,
    Selector & selector
) noexcept -> signal_id requires (not is_leaf)
{
    auto & node = get_node(hint);
    auto candidate = node.value_.load(std::memory_order_relaxed);

    while (candidate != 0)
    {
        auto attemptSelector = selector;
        auto selected = attemptSelector.select(node_traits<N>{}, hint.value_, candidate);
        auto laneAddend = 1ull << (selected * lane_width);

        if (node.value_.compare_exchange_strong(candidate, candidate - laneAddend, std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            selector = attemptSelector;

            if (auto localHint = hint.template lane<N>(); selected != localHint)
                hint = hint.template with_lane<N>(selected);

            return signal_tree_level<N - 1, capacity>::template select_lane<Selector>(hint, naturalNextOut, selector);
        }
    }
    naturalNextOut = {};
    return {};
}
