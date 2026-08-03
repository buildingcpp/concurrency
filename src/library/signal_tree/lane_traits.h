#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>


namespace bcpp::concurrency
{

    template <std::size_t> struct node_traits;
    template <std::size_t> struct lane_traits;


    //==============================================================================
    template <>
    struct lane_traits<0>
    {
        static constexpr std::size_t width    = 1;
        static constexpr std::size_t capacity = (1ull << width) - 1ull;
    };


    //==============================================================================
    template <std::size_t Level>
    struct lane_traits
    {
        using child_node = node_traits<Level - 1>;

        static constexpr std::size_t width = std::bit_ceil(static_cast<unsigned>(std::bit_width(child_node::node_capacity)));
        static constexpr std::size_t capacity = (1ull << width) - 1ull;
    };

} // namespace bcpp::concurrency
