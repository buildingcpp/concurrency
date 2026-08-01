#pragma once

#include "./node_traits.h"

#include <atomic>
#include <cstddef>
#include <cstdint>


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


    namespace detail
    {

        //==============================================================================
        template <std::size_t Depth>
        struct signal_tree_layout
        {
            static constexpr std::size_t capacity = node_traits<Depth>::node_capacity;

            template <std::size_t Level>
            static consteval std::size_t level_node_count()
            {
                static_assert(Level <= Depth);
                return capacity / node_traits<Level>::node_capacity;
            }

            template <std::size_t Level>
            static consteval std::size_t level_offset()
            {
                static_assert(Level <= Depth);

                if constexpr (Level == 0)
                    return 0;
                else
                    return level_offset<Level - 1>() + level_node_count<Level - 1>();
            }

            static constexpr std::size_t node_count = level_offset<Depth>() + level_node_count<Depth>();
        };

    } // namespace detail

} // namespace bcpp
