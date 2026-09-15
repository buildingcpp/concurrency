#pragma once

#include "./fairness_selector.h"
#include "./node_traits.h"

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>


namespace bcpp::concurrency
{

    enum class signal_tree_wrap_policy
    {
        wrap,
        no_wrap
    };


    template <std::size_t tree_size_v, signal_tree_wrap_policy wrap_policy_v>
    struct signal_tree_traits
    {
        static constexpr auto tree_size = tree_size_v;
        static constexpr auto wrapping = wrap_policy_v;
    };


    using default_signal_tree_traits = signal_tree_traits<1, signal_tree_wrap_policy::wrap>;


    template <typename T>
    concept signal_tree_traits_concept = std::is_same_v<
            T, signal_tree_traits<T::tree_size, T::wrapping>>;

} // namespace bcpp::concurrency


#include "./signal_id.h"


namespace bcpp::concurrency
{

    template <std::size_t tree_depth, synchronization_mode mode, process_sharing sharing> class signal_set;


    template <std::size_t tree_size>
    inline constexpr std::size_t tree_capacity_v = node_traits<tree_size>::node_capacity;


    struct alignas(64) aligned_node
    {
        std::atomic<std::uint64_t> value_{0};
    };


    namespace detail
    {

        //==============================================================================
        template <std::size_t tree_depth>
        struct signal_tree_layout
        {
            static constexpr std::size_t capacity = node_traits<tree_depth>::node_capacity;

            template <std::size_t current_level>
            static consteval std::size_t level_node_count()
            {
                static_assert(current_level <= tree_depth);
                return capacity / node_traits<current_level>::node_capacity;
            }

            template <std::size_t current_level>
            static consteval std::size_t level_offset()
            {
                static_assert(current_level <= tree_depth);

                if constexpr (current_level == 0)
                    return 0;
                else
                    return level_offset<current_level - 1>() + level_node_count<current_level - 1>();
            }

            static constexpr std::size_t node_count = level_offset<tree_depth>() + level_node_count<tree_depth>();
        };

    } // namespace detail


    //==============================================================================
    template <signal_tree_traits_concept T>
    class alignas(64) signal_tree
    {
    public:

        using traits = T;

        enum class occupancy
        {
            empty,
            non_empty
        };

        enum class set_result : std::uint8_t
        {
            already_set_in_non_empty_tree = 0b00,
            set_in_non_empty_tree         = 0b01,
            already_set_in_empty_tree     = 0b10,
            set_in_empty_tree             = 0b11
        };

        struct select_result
        {
            signal_id signal;
            occupancy resulting_occupancy;
        };

        static auto constexpr tree_size = traits::tree_size;
        static auto constexpr capacity = tree_capacity_v<tree_size>;

        set_result set(signal_id) noexcept;

        bool empty() const noexcept;

        template <selector_concept Selector = fairness_selector>
        [[gnu::always_inline]] inline select_result select(signal_id &) noexcept;

    private:

        using layout = detail::signal_tree_layout<tree_size>;

        template <std::size_t current_level>
        aligned_node & node(signal_id) noexcept;

        template <std::size_t current_level>
        set_result set(signal_id) noexcept;

        template <std::size_t current_level, typename Selector>
        requires node_traits<current_level>::is_leaf
        signal_id select(signal_id, signal_id &, occupancy &, Selector &) noexcept;

        template <std::size_t current_level, typename Selector>
        requires (not node_traits<current_level>::is_leaf)
        signal_id select(signal_id, signal_id &, occupancy &, Selector &) noexcept;

        std::array<aligned_node, layout::node_count> node_;

        template <std::size_t, synchronization_mode, process_sharing> friend class signal_set;
    };

    signal_tree() -> signal_tree<default_signal_tree_traits>;

} // namespace bcpp::concurrency


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
template <bcpp::concurrency::selector_concept Selector>
[[gnu::always_inline]] inline auto bcpp::concurrency::signal_tree<T>::select
(
    signal_id & hint
) noexcept -> select_result
{
    if ((not hint.valid()) || (static_cast<signal_id::value_type>(hint) >= capacity))
        hint = signal_id{0};

    Selector selector{std::integral_constant<std::size_t, capacity>{}};
    signal_id nextHintCandidate;
    auto resultingOccupancy = occupancy::non_empty;
    auto selected = select<tree_size>(hint, nextHintCandidate, resultingOccupancy, selector);

    if ((not selected.valid()))
    {
        hint = {};
        return {{}, occupancy::non_empty};
    }
    auto b = selector.mask_ & (~selector.mask_ + 1ull);
    auto selectedValue = static_cast<signal_id::value_type>(selected);
    auto nextHintCandidateValue = static_cast<signal_id::value_type>(nextHintCandidate);
    auto fallback = (b != 0) ? signal_id{(selectedValue | b) & ~(b - 1ull)} : signal_id{};
    auto useCandidate = (selectedValue < nextHintCandidateValue) & (nextHintCandidateValue < capacity);
    hint = useCandidate ? nextHintCandidate : fallback;
    return {selected, resultingOccupancy};
}


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
inline auto bcpp::concurrency::signal_tree<T>::set
(
    signal_id index
) noexcept -> set_result
{
    return set<0>(index);
}


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
inline bool bcpp::concurrency::signal_tree<T>::empty
(
) const noexcept
{
    auto constexpr rootOffset = layout::template level_offset<tree_size>();
    return node_[rootOffset].value_.load() == 0;
}


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
template <std::size_t current_level>
inline auto bcpp::concurrency::signal_tree<T>::node
(
    signal_id signal
) noexcept -> aligned_node &
{
    using node_traits_type = node_traits<current_level>;
    auto constexpr levelOffset = layout::template level_offset<current_level>();
    auto nodeIndex = (static_cast<signal_id::value_type>(signal) % capacity) / node_traits_type::node_capacity;
    return node_[levelOffset + nodeIndex];
}


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
template <std::size_t current_level>
inline auto bcpp::concurrency::signal_tree<T>::set
(
    signal_id signal
) noexcept -> set_result
{
    static constexpr bool is_root = (current_level == tree_size);

    if constexpr (current_level == 0)
    {
        using node_traits_type = node_traits<0>;
        auto & leaf = node<0>(signal);
        auto signalBit = 1ull << (static_cast<signal_id::value_type>(signal) % node_traits_type::lanes_per_node);
        auto prior = leaf.value_.fetch_or(signalBit, std::memory_order_release);
        auto setSuccessful = (prior & signalBit) == 0;

        if constexpr (is_root)
        {
            if (setSuccessful)
                return (prior == 0) ? set_result::set_in_empty_tree : set_result::set_in_non_empty_tree;
            return (prior == 0) ? set_result::already_set_in_empty_tree : set_result::already_set_in_non_empty_tree;
        }
        else
        {
            if (setSuccessful)
                return set<1>(signal);
            return set_result::already_set_in_non_empty_tree;
        }
    }
    else
    {
        using node_traits_type = node_traits<current_level>;
        using child_traits = node_traits<current_level - 1>;
        auto & branch = node<current_level>(signal);
        auto laneIndex = (static_cast<signal_id::value_type>(signal) / child_traits::node_capacity) % node_traits_type::lanes_per_node;
        auto addend = (1ull << (laneIndex * node_traits_type::lane_width));
        auto prior = branch.value_.fetch_add(addend, std::memory_order_release);

        if constexpr (is_root)
            return (prior == 0) ? set_result::set_in_empty_tree : set_result::set_in_non_empty_tree;
        else
            return set<current_level + 1>(signal);
    }
}


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
template <std::size_t current_level, typename Selector>
    requires bcpp::concurrency::node_traits<current_level>::is_leaf
inline bcpp::concurrency::signal_id bcpp::concurrency::signal_tree<T>::select
(
    signal_id hint,
    signal_id & nextHintCandidate,
    occupancy & resultingOccupancy,
    Selector &
) noexcept
{
    static auto constexpr is_root = (current_level == tree_size);

    using node_traits_type = node_traits<0>;
    static auto constexpr leafSlotMask = (1ull << node_traits_type::hint_width) - 1ull;

    auto & leaf = node<0>(hint);
    auto hintValue = static_cast<signal_id::value_type>(hint);
    auto nodeBase = static_cast<std::uint64_t>(hintValue & ~leafSlotMask);
    auto hintedLane = hintValue & leafSlotMask;
    auto candidate = leaf.value_.load(std::memory_order_relaxed);
    auto forwardMask = ~0ull << hintedLane;
    auto observed = candidate & forwardMask;

    while (observed != 0)
    {
        auto lane = static_cast<std::size_t>(std::countr_zero(observed));
        auto bit = 1ull << lane;
        auto prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
        candidate = prior & ~bit;

        if ((prior & bit) != 0)
        {
            auto selected = signal_id{(hintValue & ~node_traits_type::level_mask) | (lane << node_traits_type::hint_offset)};

            auto nextMask = (lane + 1ull < node_traits_type::lanes_per_node) ? ~0ull << (lane + 1ull) : 0ull;
            auto next = candidate & nextMask;
            nextHintCandidate = (next != 0) ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))} : signal_id{};

            if constexpr (is_root)
                resultingOccupancy = (candidate == 0) ? occupancy::empty : occupancy::non_empty;

            return selected;
        }
        observed = candidate & forwardMask;
    }

    if constexpr ((is_root) && (traits::wrapping == signal_tree_wrap_policy::no_wrap))
    {
        nextHintCandidate = {};
        return {};
    }
    else
    {
        observed = candidate;

        while ((not is_root) || (observed != 0))
        {
            auto lane = static_cast<std::size_t>(std::countr_zero(observed));
            auto bit = 1ull << lane;
            auto prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
            candidate = prior & ~bit;

            if ((prior & bit) != 0)
            {
                auto selected = signal_id{(hintValue & ~node_traits_type::level_mask) | (lane << node_traits_type::hint_offset)};
                auto nextMask = (lane + 1ull < node_traits_type::lanes_per_node) ? ~0ull << (lane + 1ull) : 0ull;
                auto next = candidate & nextMask;
                nextHintCandidate = (next != 0) ? signal_id{nodeBase + static_cast<std::uint64_t>(std::countr_zero(next))} : signal_id{};

                if constexpr (is_root)
                    resultingOccupancy = (candidate == 0) ? occupancy::empty : occupancy::non_empty;

                return selected;
            }
            observed = candidate;
        }

        nextHintCandidate = {};
        return {};
    }
}


//==============================================================================
template <bcpp::concurrency::signal_tree_traits_concept T>
template <std::size_t current_level, typename Selector>
    requires (not bcpp::concurrency::node_traits<current_level>::is_leaf)
inline bcpp::concurrency::signal_id bcpp::concurrency::signal_tree<T>::select
(
    signal_id hint,
    signal_id & nextHintCandidate,
    occupancy & resultingOccupancy,
    Selector & selector
) noexcept
{
    using node_traits_type = node_traits<current_level>;
    auto & branch = node<current_level>(hint);
    auto candidate = branch.value_.load(std::memory_order_relaxed);

    while (candidate != 0)
    {
        auto attemptSelector = selector;
        auto hintValue = static_cast<signal_id::value_type>(hint);
        auto selected = attemptSelector.select(node_traits_type{}, hintValue, candidate);
        auto laneAddend = 1ull << (selected * node_traits_type::lane_width);

        if (branch.value_.compare_exchange_strong(candidate, candidate - laneAddend, std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            static constexpr bool is_root = (current_level == tree_size);

            selector = attemptSelector;
            static auto constexpr laneMask = (signal_id::value_type{1} << node_traits_type::hint_width) - signal_id::value_type{1};
            auto localHint = (hintValue >> node_traits_type::hint_offset) & laneMask;

            if ((selected != localHint))
                hint = signal_id{(hintValue & ~node_traits_type::level_mask) | (selected << node_traits_type::hint_offset)};

            if constexpr (is_root)
                resultingOccupancy = ((candidate - laneAddend) == 0)
                        ? occupancy::empty : occupancy::non_empty;

            return select<current_level - 1>(hint, nextHintCandidate, resultingOccupancy, selector);
        }
    }

    nextHintCandidate = {};
    return {};
}
