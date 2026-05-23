#pragma once

#include "./fairness_selector.h"
#include "./signal_id.h"
#include "./signal_tree_level.h"

#include <type_traits>


namespace bcpp
{

    template <std::size_t N>
    inline constexpr std::size_t tree_capacity_v = node_traits<N>::node_capacity;


    //==============================================================================
    template <std::size_t N>
    class alignas(64) signal_tree : private signal_tree_level<N, tree_capacity_v<N>>
    {
    public:

        static auto constexpr capacity = tree_capacity_v<N>;

        bool set(signal_id) noexcept;
        bool empty() const noexcept;

        template <selector_concept Selector = fairness_selector>
        signal_id select(signal_id &) noexcept;
    };

    signal_tree() -> signal_tree<1>;

} // namespace bcpp


//==============================================================================
template <std::size_t N>
template <bcpp::selector_concept Selector>
auto bcpp::signal_tree<N>::select
(
    signal_id & hint
) noexcept -> signal_id
{
    if (not hint.valid() || hint.value_ >= capacity)
        hint = signal_id{0};

    Selector selector{std::integral_constant<std::size_t, capacity>{}};
    signal_id nextHintCandidate;

    auto selected = signal_tree_level<N, capacity>::template select_lane<Selector>(hint, nextHintCandidate, selector);
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
    return signal_tree_level<0, capacity>::set(index);
}


//==============================================================================
template <std::size_t N>
inline bool bcpp::signal_tree<N>::empty
(
) const noexcept
{
    return signal_tree_level<N, tree_capacity_v<N>>::empty();
}
