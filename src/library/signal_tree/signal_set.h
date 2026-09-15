#pragma once

#include "./fairness_selector.h"
#include "./blocking_state.h"
#include "./signal_id.h"
#include "./signal_tree.h"

#include <include/synchronization_mode.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>


namespace bcpp::concurrency
{

    //==============================================================================
    template
    <
        std::size_t tree_depth,
        synchronization_mode mode = synchronization_mode::non_blocking,
        process_sharing sharing = process_sharing::process_private
    >
    class signal_set
    {
    public:

        static auto constexpr blocking = (mode == synchronization_mode::blocking);

        using tree_traits = signal_tree_traits<tree_depth, (tree_depth == 0) ? signal_tree_wrap_policy::no_wrap : signal_tree_wrap_policy::wrap>;

        using tree_type = signal_tree<tree_traits>;
        using shared_state_type = detail::process_shared_blocking_state;

        signal_set(std::size_t capacity) requires (sharing == process_sharing::process_private);

        explicit signal_set(std::span<tree_type>) noexcept
            requires ((not blocking) || (sharing == process_sharing::process_private));

        signal_set(std::span<tree_type>, shared_state_type &) noexcept
            requires (blocking && (sharing == process_sharing::process_shared));

        static constexpr std::size_t required_tree_count(std::size_t capacity) noexcept
        {
            return (capacity == 0) ? 1ull : (1ull + ((capacity - 1) / tree_type::capacity));
        }

        bool set(signal_id) noexcept;

        template <typename Selector = fairness_selector>
        signal_id select(signal_id & hint) noexcept;

        template <typename Selector = fairness_selector, typename Rep, typename Period>
        signal_id select(signal_id & hint, std::chrono::duration<Rep, Period> timeout) requires (blocking);

        void stop() noexcept requires (blocking);

        std::uint64_t non_empty_tree_count() const noexcept requires (blocking);
        std::uint64_t max() const noexcept {return (capacity_ - 1ull);}

    private:

        using blocking_state_type = std::conditional_t<blocking, detail::blocking_state<sharing>, detail::no_blocking_state>;

        static constexpr std::uint64_t tree_capacity = tree_type::capacity;
        static constexpr std::uint64_t tree_capacity_mask = tree_capacity - 1ull;

        std::vector<tree_type>                  ownedSignalTrees_;
        std::span<tree_type>                    signalTrees_;
        std::size_t                             capacity_;
        [[no_unique_address]] blocking_state_type blockingState_;
    };

    signal_set(std::size_t) -> signal_set<1>;

    template
    <
        std::size_t tree_depth,
        synchronization_mode mode = synchronization_mode::non_blocking
    >
    using shared_signal_set = signal_set<tree_depth, mode, process_sharing::process_shared>;

} // namespace bcpp::concurrency


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
inline bcpp::concurrency::signal_set<tree_depth, mode, sharing>::signal_set
(
    std::size_t capacity
) requires (sharing == process_sharing::process_private) :
    ownedSignalTrees_(required_tree_count(capacity)),
    signalTrees_(ownedSignalTrees_),
    capacity_(signalTrees_.size() * tree_capacity)
{
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
inline bcpp::concurrency::signal_set<tree_depth, mode, sharing>::signal_set
(
    std::span<tree_type> signalTrees
) noexcept requires ((not blocking) || (sharing == process_sharing::process_private)) :
    signalTrees_(signalTrees),
    capacity_(signalTrees_.size() * tree_capacity)
{
    assert((not signalTrees_.empty()));
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
inline bcpp::concurrency::signal_set<tree_depth, mode, sharing>::signal_set
(
    std::span<tree_type> signalTrees,
    shared_state_type & blockingState
) noexcept requires (blocking && (sharing == process_sharing::process_shared)) :
    signalTrees_(signalTrees),
    capacity_(signalTrees_.size() * tree_capacity),
    blockingState_(blockingState)
{
    assert((not signalTrees_.empty()));
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
inline bool bcpp::concurrency::signal_set<tree_depth, mode, sharing>::set
(
    signal_id signalId
) noexcept
{
    if ((not signalId.valid()) || (signalId.value_ >= capacity_))
        return false;

    auto treeId = signalId.value_ / tree_capacity;
    auto localSignal = signal_id{signalId.value_ & tree_capacity_mask};
    auto result = signalTrees_[treeId].set(localSignal);

    if constexpr (blocking)
    {
        if ((result == tree_type::set_result::set_in_empty_tree))
            blockingState_.increment();
    }

    return (result == tree_type::set_result::set_in_empty_tree) || (result == tree_type::set_result::set_in_non_empty_tree);
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
template <typename Selector>
inline bcpp::concurrency::signal_id bcpp::concurrency::signal_set<tree_depth, mode, sharing>::select
(
    signal_id & hint
) noexcept
{
    auto treeId = 0ull;

    if ((hint.valid()) && (hint.value_ < capacity_))
        treeId = static_cast<std::size_t>(hint.value_ / tree_capacity);
    auto localHint = ((hint.valid()) && (hint.value_ < capacity_)) ? signal_id{hint.value_ & tree_capacity_mask} : signal_id{0};
    auto scanCount = signalTrees_.size();

    if constexpr (tree_depth == 0)
        if (localHint.value_ != 0)
            ++scanCount;

    for (auto scanned = 0ull; scanned < scanCount; ++scanned)
    {
        auto offset = treeId * tree_capacity;
        auto [selected, resultingOccupancy] = signalTrees_[treeId].template select<Selector>(localHint);

        if ((selected.valid()))
        {
            if constexpr (blocking)
                if ((resultingOccupancy == tree_type::occupancy::empty))
                    blockingState_.decrement();

            if ((localHint.valid()))
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
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
template <typename Selector, typename Rep, typename Period>
inline bcpp::concurrency::signal_id bcpp::concurrency::signal_set<tree_depth, mode, sharing>::select
(
    signal_id & hint,
    std::chrono::duration<Rep, Period> timeout
) requires (blocking)
{
    if (timeout <= std::chrono::duration<Rep, Period>::zero())
        return select<Selector>(hint);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true)
    {
        if (auto selected = select<Selector>(hint); selected.valid())
            return selected;
        if (not blockingState_.wait_until(deadline))
            return {};
    }
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
inline void bcpp::concurrency::signal_set<tree_depth, mode, sharing>::stop
(
) noexcept requires (blocking)
{
    blockingState_.stop();
}


//==============================================================================
template <std::size_t tree_depth, bcpp::synchronization_mode mode, bcpp::concurrency::process_sharing sharing>
inline std::uint64_t bcpp::concurrency::signal_set<tree_depth, mode, sharing>::non_empty_tree_count
(
) const noexcept requires (blocking)
{
    return blockingState_.count();
}
