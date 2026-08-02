#pragma once

#include "./signal_tree/fairness_selector.h"
#include "./signal_tree/signal_id.h"
#include "./signal_tree/signal_tree.h"

#include <cstddef>
#include <cstdint>
#include <vector>


namespace bcpp
{

    //==============================================================================
    template <std::size_t tree_depth>
    class signal_set
    {
    public:

        signal_set(std::size_t capacity);

        bool set(signal_id) noexcept;

        template <typename Selector = fairness_selector>
        signal_id select(signal_id & hint) noexcept;

        std::uint64_t max() const noexcept { return capacity_ - 1ull; }

    private:

        using signal_tree_type = signal_tree<tree_depth>;
        using wrap_policy = typename signal_tree_type::wrap_policy;

        static constexpr std::uint64_t tree_capacity = signal_tree_type::capacity;
        static constexpr std::uint64_t tree_capacity_mask = tree_capacity - 1ull;

        std::vector<signal_tree_type> signalTrees_;
        std::size_t capacity_;
    };

    signal_set(std::size_t) -> signal_set<1>;

} // namespace bcpp


//==============================================================================
template <std::size_t tree_depth>
inline bcpp::signal_set<tree_depth>::signal_set
(
    std::size_t capacity
) :
    signalTrees_(((capacity == 0 ? std::size_t{1} : capacity) + tree_capacity - 1ull) / tree_capacity),
    capacity_(signalTrees_.size() * tree_capacity)
{
}


//==============================================================================
template <std::size_t tree_depth>
inline bool bcpp::signal_set<tree_depth>::set
(
    signal_id signalId
) noexcept
{
    if ((not signalId.valid()) || (signalId.value_ >= capacity_))
        return false;

    auto treeId = signalId.value_ / tree_capacity;
    return signalTrees_[treeId].set(signal_id{signalId.value_ & tree_capacity_mask});
}


//==============================================================================
template <std::size_t tree_depth>
template <typename Selector>
inline auto bcpp::signal_set<tree_depth>::select
(
    signal_id & hint
) noexcept -> signal_id
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
        auto selected = [&]
        {
            if constexpr (tree_depth == 0)
                return signalTrees_[treeId].template select<Selector, wrap_policy::no_wrap>(localHint);
            else
                return signalTrees_[treeId].template select<Selector>(localHint);
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
