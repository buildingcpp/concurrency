#pragma once

#include "./blocking_state.h"
#include "./fairness_selector.h"
#include "./node_traits.h"
#include "./signal_id.h"

#include <include/synchronization_mode.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>


namespace bcpp
{

    template <std::size_t tree_depth, synchronization_mode mode> class signal_set;


    template <std::size_t tree_depth>
    inline constexpr std::size_t tree_capacity_v = node_traits<tree_depth>::node_capacity;


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
    template <std::size_t tree_depth, synchronization_mode mode = synchronization_mode::non_blocking>
    class alignas(64) signal_tree
    {
    public:

        enum class wrap_policy
        {
            wrap,
            no_wrap
        };

        static auto constexpr capacity = tree_capacity_v<tree_depth>;
        static auto constexpr blocking = (mode == synchronization_mode::blocking);

        bool set(signal_id) noexcept;

        bool empty() const noexcept;

        template <selector_concept Selector = fairness_selector, wrap_policy policy = wrap_policy::wrap>
        [[gnu::always_inline]] inline signal_id select(signal_id &) noexcept;

        template <selector_concept Selector = fairness_selector, wrap_policy policy = wrap_policy::wrap,
                typename Rep, typename Period>
        signal_id select(signal_id &, std::chrono::duration<Rep, Period>) requires (blocking);

        template <selector_concept Selector = fairness_selector, wrap_policy policy = wrap_policy::wrap>
        signal_id try_select(signal_id &) requires (blocking);

        void stop() noexcept requires (blocking);

        std::uint64_t non_empty_tree_count() const noexcept requires (blocking);

    private:

        enum class cardinality
        {
            zero,
            non_zero
        };

        struct set_result
        {
            bool signalWasSet_;
            cardinality wasEmpty_;
        };

        struct select_result
        {
            signal_id signal_;
            cardinality isEmpty_;
        };

        using layout = detail::signal_tree_layout<tree_depth>;
        using blocking_state_type = std::conditional_t<blocking, detail::blocking_state, detail::no_blocking_state>;

        template <bool report_cardinality>
        bool set(signal_id) noexcept
            requires (not report_cardinality);

        template <bool report_cardinality>
        set_result set(signal_id) noexcept
            requires (report_cardinality);

        template <selector_concept Selector, wrap_policy policy, bool report_cardinality>
        signal_id select(signal_id &) noexcept
            requires (not report_cardinality);

        template <selector_concept Selector, wrap_policy policy, bool report_cardinality>
        select_result select(signal_id &) noexcept
            requires (report_cardinality);

        template <selector_concept Selector, wrap_policy policy, bool report_cardinality>
        auto select_impl(signal_id &) noexcept;

        template <std::size_t current_level>
        aligned_node & node(signal_id) noexcept;

        template <std::size_t current_level, bool report_cardinality>
        auto set(signal_id) noexcept;

        template <std::size_t current_level, wrap_policy policy, bool report_cardinality, typename Selector>
            requires node_traits<current_level>::is_leaf
        signal_id select(signal_id, signal_id &, bool &, Selector &) noexcept;

        template <std::size_t current_level, wrap_policy policy, bool report_cardinality, typename Selector>
            requires (not node_traits<current_level>::is_leaf)
        signal_id select(signal_id, signal_id &, bool &, Selector &) noexcept;

        std::array<aligned_node, layout::node_count> node_;
        [[no_unique_address]] blocking_state_type blockingState_;

        template <std::size_t, synchronization_mode> friend class signal_set;
    };

    signal_tree() -> signal_tree<1>;

} // namespace bcpp


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bcpp::selector_concept Selector,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy>
[[gnu::always_inline]] inline bcpp::signal_id bcpp::signal_tree<tree_depth, mode>::select
(
    signal_id & hint
) noexcept
{
    if constexpr (blocking)
    {
        auto [selected, isEmpty] = select<Selector, policy, true>(hint);

        if ((selected.valid()) && (isEmpty == cardinality::zero))
            blockingState_.decrement();

        return selected;
    }
    else
        return select<Selector, policy, false>(hint);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bcpp::selector_concept Selector,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy, bool report_cardinality>
[[gnu::always_inline]] inline bcpp::signal_id bcpp::signal_tree<tree_depth, mode>::select
(
    signal_id & hint
) noexcept requires (not report_cardinality)
{
    return select_impl<Selector, policy, report_cardinality>(hint);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bcpp::selector_concept Selector,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy, bool report_cardinality>
[[gnu::always_inline]] inline typename bcpp::signal_tree<tree_depth, mode>::select_result
bcpp::signal_tree<tree_depth, mode>::select
(
    signal_id & hint
) noexcept requires (report_cardinality)
{
    return select_impl<Selector, policy, report_cardinality>(hint);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bcpp::selector_concept Selector,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy, bool report_cardinality>
[[gnu::always_inline]] inline auto bcpp::signal_tree<tree_depth, mode>::select_impl
(
    signal_id & hint
) noexcept
{
    if ((not hint.valid()) || (hint.value_ >= capacity))
        hint = signal_id{0};

    Selector selector{std::integral_constant<std::size_t, capacity>{}};
    signal_id nextHintCandidate;
    bool isEmpty = false;
    auto selected = select<tree_depth, policy, report_cardinality>(
            hint, nextHintCandidate, isEmpty, selector);

    if (not selected.valid())
    {
        hint = {};
        if constexpr (report_cardinality)
            return select_result{{}, cardinality::non_zero};
        else
            return signal_id{};
    }
    auto b = selector.mask_ & (~selector.mask_ + 1ull);
    auto fallback = (b != 0) ? signal_id{(selected.value_ | b) & ~(b - 1ull)} : signal_id{};
    auto useCandidate = (selected.value_ < nextHintCandidate.value_) & (nextHintCandidate.value_ < capacity);
    hint = useCandidate ? nextHintCandidate : fallback;
    if constexpr (report_cardinality)
        return select_result{selected, isEmpty ? cardinality::zero : cardinality::non_zero};
    else
        return selected;
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bcpp::selector_concept Selector,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy, typename Rep, typename Period>
inline bcpp::signal_id bcpp::signal_tree<tree_depth, mode>::select
(
    signal_id & hint,
    std::chrono::duration<Rep, Period> timeout
) requires (blocking)
{
    if (timeout <= std::chrono::duration<Rep, Period>::zero())
        return select<Selector, policy>(hint);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;)
    {
        if (auto selected = select<Selector, policy>(hint); selected.valid())
            return selected;

        if (not blockingState_.wait_until(deadline))
            return {};
    }
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bcpp::selector_concept Selector,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy>
inline bcpp::signal_id bcpp::signal_tree<tree_depth, mode>::try_select
(
    signal_id & hint
) requires (blocking)
{
    return select<Selector, policy>(hint, std::chrono::nanoseconds{0});
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline void bcpp::signal_tree<tree_depth, mode>::stop
(
) noexcept requires (blocking)
{
    blockingState_.stop();
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline std::uint64_t bcpp::signal_tree<tree_depth, mode>::non_empty_tree_count
(
) const noexcept requires (blocking)
{
    return blockingState_.count();
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline bool bcpp::signal_tree<tree_depth, mode>::set
(
    signal_id index
) noexcept
{
    if constexpr (blocking)
    {
        auto [signalWasSet, wasEmpty] = set<true>(index);

        if (wasEmpty == cardinality::zero)
            blockingState_.increment();

        return signalWasSet;
    }
    else
        return set<false>(index);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bool report_cardinality>
inline bool bcpp::signal_tree<tree_depth, mode>::set
(
    signal_id index
) noexcept requires (not report_cardinality)
{
    return set<0, report_cardinality>(index);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <bool report_cardinality>
inline typename bcpp::signal_tree<tree_depth, mode>::set_result bcpp::signal_tree<tree_depth, mode>::set
(
    signal_id index
) noexcept requires (report_cardinality)
{
    return set<0, report_cardinality>(index);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline bool bcpp::signal_tree<tree_depth, mode>::empty
(
) const noexcept
{
    constexpr auto rootOffset = layout::template level_offset<tree_depth>();
    return node_[rootOffset].value_.load() == 0;
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <std::size_t current_level>
inline auto bcpp::signal_tree<tree_depth, mode>::node
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
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <std::size_t current_level, bool report_cardinality>
inline auto bcpp::signal_tree<tree_depth, mode>::set
(
    signal_id signal
) noexcept
{
    static constexpr bool is_root = (current_level == tree_depth);

    if constexpr (current_level == 0)
    {
        using traits = node_traits<0>;
        auto & leaf = node<0>(signal);
        auto const signalBit = 1ull << (signal.value_ % traits::lanes_per_node);
        auto const prior = leaf.value_.fetch_or(signalBit, std::memory_order_release);
        auto const setSuccessful = (prior & signalBit) == 0;

        if constexpr (is_root)
        {
            if constexpr (report_cardinality)
                return set_result{
                    setSuccessful,
                    (setSuccessful && (prior == 0)) ? cardinality::zero : cardinality::non_zero
                };
            else
                return setSuccessful;
        }
        else
        {
            if (setSuccessful)
                return set<1, report_cardinality>(signal);

            if constexpr (report_cardinality)
                return set_result{false, cardinality::non_zero};
            else
                return false;
        }
    }
    else
    {
        using traits = node_traits<current_level>;
        using child_traits = node_traits<current_level - 1>;
        auto & branch = node<current_level>(signal);
        auto const laneIndex = (signal.value_ / child_traits::node_capacity) % traits::lanes_per_node;
        auto const addend = 1ull << (laneIndex * traits::lane_width);

        auto const prior = branch.value_.fetch_add(addend, std::memory_order_release);

        if constexpr (is_root)
        {
            if constexpr (report_cardinality)
                return set_result{true, (prior == 0) ? cardinality::zero : cardinality::non_zero};
            else
                return true;
        }
        else
            return set<current_level + 1, report_cardinality>(signal);
    }
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <std::size_t current_level,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy,
        bool report_cardinality, typename Selector>
    requires bcpp::node_traits<current_level>::is_leaf
inline bcpp::signal_id bcpp::signal_tree<tree_depth, mode>::select
(
    signal_id hint,
    signal_id & nextHintCandidate,
    bool & isEmpty,
    Selector &
) noexcept
{
    static constexpr bool is_root = (current_level == tree_depth);

    using traits = node_traits<0>;
    static constexpr auto leafSlotMask = (1ull << traits::hint_width) - 1ull;

    auto & leaf = node<0>(hint);
    auto const nodeBase = static_cast<std::uint64_t>(hint.value_ & ~leafSlotMask);
    auto const hintedLane = hint.value_ & leafSlotMask;
    auto candidate = leaf.value_.load(std::memory_order_relaxed);
    auto const forwardMask = ~0ull << hintedLane;
    auto observed = candidate & forwardMask;

    while (observed != 0)
    {
        auto const lane = static_cast<std::size_t>(std::countr_zero(observed));
        auto const bit = 1ull << lane;
        auto const prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
        candidate = prior & ~bit;

        if ((prior & bit) != 0)
        {
            auto const selected = hint.template with_lane<0>(lane);

            if constexpr ((report_cardinality) && (is_root))
                isEmpty = (candidate == 0);

            auto const nextMask = (lane + 1ull < traits::lanes_per_node)
                    ? ~0ull << (lane + 1ull) : 0ull;
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
            auto const bit = 1ull << lane;
            auto const prior = leaf.value_.fetch_and(~bit, std::memory_order_acq_rel);
            candidate = prior & ~bit;

            if ((prior & bit) != 0)
            {
                auto const selected = hint.template with_lane<0>(lane);

                if constexpr ((report_cardinality) && (is_root))
                    isEmpty = (candidate == 0);

                auto const nextMask = (lane + 1ull < traits::lanes_per_node)
                        ? ~0ull << (lane + 1ull) : 0ull;
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
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <std::size_t current_level,
        typename bcpp::signal_tree<tree_depth, mode>::wrap_policy policy,
        bool report_cardinality, typename Selector>
    requires (not bcpp::node_traits<current_level>::is_leaf)
inline bcpp::signal_id bcpp::signal_tree<tree_depth, mode>::select
(
    signal_id hint,
    signal_id & nextHintCandidate,
    bool & isEmpty,
    Selector & selector
) noexcept
{
    using traits = node_traits<current_level>;
    auto & branch = node<current_level>(hint);
    auto candidate = branch.value_.load(std::memory_order_relaxed);

    while (candidate != 0)
    {
        auto attemptSelector = selector;
        auto const selected = attemptSelector.select(traits{}, hint.value_, candidate);
        auto const laneAddend = 1ull << (selected * traits::lane_width);

        if (branch.value_.compare_exchange_strong(candidate, candidate - laneAddend, std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            static constexpr bool is_root = (current_level == tree_depth);

            if constexpr ((report_cardinality) && (is_root))
                isEmpty = ((candidate - laneAddend) == 0);

            selector = attemptSelector;
            if (auto const localHint = hint.template lane<current_level>(); selected != localHint)
                hint = hint.template with_lane<current_level>(selected);
            return select<current_level - 1, policy, report_cardinality>(
                    hint, nextHintCandidate, isEmpty, selector);
        }
    }

    nextHintCandidate = {};
    return {};
}
