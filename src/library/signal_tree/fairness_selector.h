#pragma once

#include "./node_traits.h"

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>


namespace bcpp::concurrency
{

    //==============================================================================
    template <typename Selector>
    concept selector_concept = requires(Selector selector, std::uint64_t hint, std::uint64_t counters)
    {
        Selector{std::integral_constant<std::size_t, node_traits<1>::node_capacity>{}};
        { selector.mask_ } -> std::convertible_to<std::uint64_t>;
        { selector.select(node_traits<1>{}, hint, counters) } -> std::same_as<std::size_t>;
    };


    //==============================================================================
    class fairness_selector
    {
    public:

        template <std::size_t Capacity>
        constexpr explicit fairness_selector(std::integral_constant<std::size_t, Capacity>) noexcept
            : mask_((static_cast<std::uint64_t>(Capacity) - 1ull)
                    & ~static_cast<std::uint64_t>(node_traits<0>::level_mask))
        {
        }

        template <branch_node_traits_concept Traits>
        std::size_t select(Traits, std::uint64_t hint, std::uint64_t counters) noexcept
        {
            return tournament<Traits, Traits::lanes_per_node>(hint, counters, mask_);
        }

        std::uint64_t mask_;

    private:

        template <typename Traits, std::uint64_t L>
        static std::size_t tournament(std::uint64_t hint, std::uint64_t counters, std::uint64_t & mask) noexcept
        {
            if constexpr (L == 1)
            {
                return 0;
            }
            else
            {
                constexpr std::uint64_t hint_offset = Traits::hint_offset;
                constexpr std::uint64_t lane_width = Traits::lane_width;
                constexpr std::uint64_t hint_bit = (L / 2) << hint_offset;
                constexpr std::uint64_t half_counters = L / 2;
                constexpr std::uint64_t bits_per_half = half_counters * lane_width;
                constexpr std::uint64_t high_mask = ((std::uint64_t{1} << bits_per_half) - 1ull) << bits_per_half;

                auto const left = counters & ~high_mask;
                auto const right = counters >> bits_per_half;
                auto const leftEmpty = (left == 0);
                auto const rightEmpty = (right == 0);
                auto const preferRight = ((hint & hint_bit) != 0);
                auto const selectedRight = preferRight ? not rightEmpty : leftEmpty;

                if (selectedRight)
                {
                    mask &= ~hint_bit;
                    return half_counters + tournament<Traits, half_counters>(hint, right, mask);
                }

                if (preferRight)
                    mask &= ~((hint_bit << 1) - 1ull);
                else if (rightEmpty)
                    mask &= ~hint_bit;

                return tournament<Traits, half_counters>(hint, left, mask);
            }
        }
    };

} // namespace bcpp::concurrency
