#pragma once

#include "./node.h"
#include "./signal_index.h"

#include <type_traits>
#include <concepts>
#include <array>


namespace bcpp::implementation::signal_tree
{

    template <std::size_t N0, std::size_t N1>
    requires ((std::popcount(N0) == 1) && (std::popcount(N1) == 1))
    struct level_traits
    {
        static auto constexpr number_of_nodes = N0;
        static auto constexpr node_capacity = N1;
        static auto constexpr tree_capacity = (node_capacity * number_of_nodes);
    };


    template <typename T> concept level_traits_concept = std::is_same_v<T, level_traits<T::number_of_nodes, T::node_capacity>>;
    template <typename T> concept root_level_traits = ((level_traits_concept<T>) && (T::number_of_nodes == 1));
    template <typename T> concept leaf_level_traits = ((level_traits_concept<T>) && (T::node_capacity == 64));
    template <typename T> concept non_leaf_level_traits = ((level_traits_concept<T>) && (!leaf_level_traits<T>));


    template <level_traits_concept T>
    struct level;


    template <level_traits_concept T> 
    struct child_level{using type = struct{};};


    template <non_leaf_level_traits T>
    struct child_level<T>
    {
        static auto constexpr number_of_child_nodes = T::number_of_nodes * node<node_traits<T::node_capacity, T::tree_capacity>>::number_of_counters;
        using type = level<level_traits<number_of_child_nodes, node<node_traits<T::node_capacity, T::tree_capacity>>::child_type::capacity>>;
    };


    //=============================================================================
    template <level_traits_concept T>
    class alignas(64) level final
    {
    public:

        using node_type = node<node_traits<T::node_capacity, T::tree_capacity>>;
        using child_level_type = child_level<T>::type;
        using bias_flags = std::uint64_t;
        using node_index = std::uint64_t;
        
        bool empty() const noexcept requires (root_level_traits<T>);

        std::pair<bool, bool> set
        (
            signal_index
        ) noexcept;

        template <template <std::uint64_t, std::uint64_t> class>
        std::pair<signal_index, bool> select
        (
            bias_flags
        ) noexcept requires (root_level_traits<T>);

    protected:

        static auto constexpr node_capacity = T::node_capacity;
        static auto constexpr node_count = T::number_of_nodes;
        static auto constexpr counters_per_node = node_type::number_of_counters;
        static auto constexpr bits_per_counter = node_type::bits_per_counter;
        static auto constexpr counter_capacity = (node_capacity / counters_per_node);

        template <level_traits_concept> friend struct level;

        template <template <std::uint64_t, std::uint64_t> class>
        std::pair<signal_index, bool> select
        (
            bias_flags,
            node_index
        ) noexcept;

        using node_array = std::array<node_type, node_count>;
        using iterator = node_array::iterator;

        node_array          nodes_;

        child_level_type    childLevel_;
    };

} // namespace bcpp::implementation::signal_tree


//=============================================================================
template <bcpp::implementation::signal_tree::level_traits_concept T>
inline bool bcpp::implementation::signal_tree::level<T>::empty
(
) const noexcept
requires (root_level_traits<T>)
{
    return nodes_[0].empty();
}


//=============================================================================
template <bcpp::implementation::signal_tree::level_traits_concept T>
inline std::pair<bool, bool> bcpp::implementation::signal_tree::level<T>::set
(
    // increment the counter associated with the specified index (id of a leaf node)
    // return true if this set caused the level to move from empty to non empty
    // return false otherwise.
    signal_index signalIndex
) noexcept
{
    if constexpr (non_leaf_level_traits<T>)
    {
        if (auto [_, success] = childLevel_.set(signalIndex); !success)
            return {false, false};
    }
    return nodes_[signalIndex / node_capacity].set(signalIndex % node_capacity);
}


//=============================================================================
template <bcpp::implementation::signal_tree::level_traits_concept T>
template <template <std::uint64_t, std::uint64_t> class select_function>
inline auto bcpp::implementation::signal_tree::level<T>::select
(
    // return the index of a counter which is not zero (indicates that one of the leaf nodes
    // represented by this counter is set - a non zero value)
    bias_flags biasFlags
) noexcept -> std::pair<signal_index, bool>
requires (root_level_traits<T>)
{
    return select<select_function>(biasFlags, 0);
}


//=============================================================================
template <bcpp::implementation::signal_tree::level_traits_concept T>
template <template <std::uint64_t, std::uint64_t> class select_function>
inline auto bcpp::implementation::signal_tree::level<T>::select
(
    // return the index of a child counter which is not zero (indicates that one of the leaf nodes
    // represented by this counter is set - a non zero value)
    bias_flags biasFlags,
    node_index nodeIndex
) noexcept -> std::pair<signal_index, bool>
{
    static auto constexpr bias_bits_consumed_to_select_counter = minimum_bit_count(counters_per_node) - 1;  // was node_count

    auto [selectedCounter, nodeIsZero] = nodes_[nodeIndex]. template select<select_function>(biasFlags);
    biasFlags <<= bias_bits_consumed_to_select_counter;

    if constexpr (root_level_traits<T>)
    {
        if (selectedCounter == invalid_signal_index)
            return {invalid_signal_index, false};
    }
    
    if constexpr (leaf_level_traits<T>)
    {
        return {selectedCounter, nodeIsZero};
    }
    else
    {
        static auto constexpr bias_bits_consumed_to_select_child_counter = minimum_bit_count(child_level_type::counters_per_node) - 1;
        select_bias_hint <<= bias_bits_consumed_to_select_child_counter;
        auto [childSelectedCounter, _] = childLevel_. template select<select_function>(biasFlags, (nodeIndex * counters_per_node) + selectedCounter);
        selectedCounter *= counter_capacity;
        return {selectedCounter | childSelectedCounter, nodeIsZero};
    }
}