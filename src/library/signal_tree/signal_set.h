#pragma once

#include "./fairness_selector.h"
#include "./blocking_state.h"
#include "./signal_id.h"
#include "./signal_tree.h"

#include <include/synchronization_mode.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>


namespace bcpp::concurrency
{

    //==============================================================================
    template <std::size_t tree_depth, synchronization_mode mode = synchronization_mode::non_blocking>
    class signal_set
    {
    public:

        static auto constexpr blocking = (mode == synchronization_mode::blocking);

        signal_set(std::size_t capacity);

        bool set(signal_id) noexcept;

        template <typename Selector = fairness_selector>
        signal_id select(signal_id & hint) noexcept;

        template <typename Selector = fairness_selector, typename Rep, typename Period>
        signal_id select(signal_id & hint, std::chrono::duration<Rep, Period> timeout) requires (blocking);

        void stop() noexcept requires (blocking);

        std::uint64_t non_empty_tree_count() const noexcept requires (blocking);
        std::uint64_t max() const noexcept { return capacity_ - 1ull; }

    private:

        using signal_tree_type = signal_tree<tree_depth>;
        using wrap_policy = typename signal_tree_type::wrap_policy;
        using blocking_state_type = std::conditional_t<blocking, detail::blocking_state, detail::no_blocking_state>;

        static constexpr std::uint64_t tree_capacity = signal_tree_type::capacity;
        static constexpr std::uint64_t tree_capacity_mask = tree_capacity - 1ull;

        std::vector<signal_tree_type>        signalTrees_;
        std::size_t                          capacity_;
        [[no_unique_address]] blocking_state_type blockingState_;
    };

    signal_set(std::size_t) -> signal_set<1>;

} // namespace bcpp::concurrency


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline bcpp::concurrency::signal_set<tree_depth, mode>::signal_set
(
    std::size_t capacity
) :
    signalTrees_(((capacity == 0 ? std::size_t{1} : capacity) + tree_capacity - 1ull) / tree_capacity),
    capacity_(signalTrees_.size() * tree_capacity)
{
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline bool bcpp::concurrency::signal_set<tree_depth, mode>::set
(
    signal_id signalId
) noexcept
{
    if ((not signalId.valid()) || (signalId.value_ >= capacity_))
        return false;

    auto treeId = signalId.value_ / tree_capacity;
    auto localSignal = signal_id{signalId.value_ & tree_capacity_mask};

    if constexpr (blocking)
    {
        auto [signalWasSet, wasEmpty] = signalTrees_[treeId].template set<true>(localSignal);

        if (wasEmpty == signal_tree_type::cardinality::zero)
            blockingState_.increment();

        return signalWasSet;
    }
    else
        return signalTrees_[treeId].set(localSignal);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <typename Selector>
inline bcpp::concurrency::signal_id bcpp::concurrency::signal_set<tree_depth, mode>::select
(
    signal_id & hint
) noexcept
{
    auto treeId = 0ull;

    if ((hint.valid()) && (hint.value_ < capacity_))
        treeId = static_cast<std::size_t>(hint.value_ / tree_capacity);
    auto localHint = ((hint.valid()) && (hint.value_ < capacity_))
            ? signal_id{hint.value_ & tree_capacity_mask} : signal_id{0};
    auto scanCount = signalTrees_.size();

    if constexpr (tree_depth == 0)
        if (localHint.value_ != 0)
            ++scanCount;

    for (auto scanned = 0ull; scanned < scanCount; ++scanned)
    {
        auto offset = treeId * tree_capacity;
        auto selected = [&]
        {
            if constexpr (blocking)
            {
                auto [selected, isEmpty] = [&]
                {
                    if constexpr (tree_depth == 0)
                        return signalTrees_[treeId].template select<
                                Selector, wrap_policy::no_wrap, true>(localHint);
                    else
                        return signalTrees_[treeId].template select<
                                Selector, wrap_policy::wrap, true>(localHint);
                }();

                if ((selected.valid()) && (isEmpty == signal_tree_type::cardinality::zero))
                    blockingState_.decrement();

                return selected;
            }
            else
            {
                if constexpr (tree_depth == 0)
                    return signalTrees_[treeId].template select<Selector, wrap_policy::no_wrap>(localHint);
                else
                    return signalTrees_[treeId].template select<Selector, wrap_policy::wrap>(localHint);
            }
        }();

        if (selected.valid())
        {
            if (localHint.valid())
            {
                hint = signal_id{offset + localHint.value_};
            }
            else
            {
                treeId = (treeId + 1ull < signalTrees_.size()) ? treeId + 1ull : 0ull;
                hint = signal_id{treeId * tree_capacity};
            }
            return signal_id{offset + selected.value_};
        }
        treeId = (treeId + 1ull < signalTrees_.size()) ? treeId + 1ull : 0ull;
        localHint = signal_id{0};
        hint = signal_id{treeId * tree_capacity};
    }
    hint = {};
    return {};
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
template <typename Selector, typename Rep, typename Period>
inline bcpp::concurrency::signal_id bcpp::concurrency::signal_set<tree_depth, mode>::select
(
    signal_id & hint,
    std::chrono::duration<Rep, Period> timeout
) requires (blocking)
{
    if (timeout <= std::chrono::duration<Rep, Period>::zero())
        return select<Selector>(hint);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;)
    {
        if (auto selected = select<Selector>(hint); selected.valid())
            return selected;

        if (not blockingState_.wait_until(deadline))
            return {};
    }
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline void bcpp::concurrency::signal_set<tree_depth, mode>::stop
(
) noexcept requires (blocking)
{
    blockingState_.stop();
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode>
inline std::uint64_t bcpp::concurrency::signal_set<tree_depth, mode>::non_empty_tree_count
(
) const noexcept requires (blocking)
{
    return blockingState_.count();
}
