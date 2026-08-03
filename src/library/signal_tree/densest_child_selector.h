#pragma once

#include "./node_traits.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>


namespace bcpp::concurrency
{

    //==============================================================================
    class densest_child_selector
    {
    public:

        template <std::size_t Capacity>
        constexpr explicit densest_child_selector(std::integral_constant<std::size_t, Capacity>) noexcept
        {
        }

        template <branch_node_traits_concept Traits>
        [[gnu::always_inline]] inline std::size_t select(Traits, std::uint64_t /*hint*/,
                std::uint64_t counters) const noexcept
        {
            return tournament<Traits, Traits::lanes_per_node>(counters);
        }

        std::uint64_t mask_ = 0;

    private:

        template <typename Traits, std::uint64_t L>
        [[gnu::always_inline]] static inline std::size_t tournament(std::uint64_t counters) noexcept
        {
            if constexpr (L == 1)
            {
                return 0;
            }
            else
            {
                constexpr std::uint64_t lane_width = Traits::lane_width;
                constexpr std::uint64_t half_counters = L / 2;
                constexpr std::uint64_t bits_per_half = half_counters * lane_width;
                constexpr std::uint64_t high_mask = ((std::uint64_t{1} << bits_per_half) - 1ull) << bits_per_half;
                constexpr std::uint64_t lane_mask = (std::uint64_t{1} << lane_width) - 1ull;

                auto const left = counters & ~high_mask;
                auto const right = counters >> bits_per_half;

                std::uint64_t leftSum = 0;
                std::uint64_t rightSum = 0;

                for (std::size_t i = 0; i < half_counters; ++i)
                {
                    leftSum += (left >> (i * lane_width)) & lane_mask;
                    rightSum += (right >> (i * lane_width)) & lane_mask;
                }

                return (rightSum > leftSum) ? half_counters + tournament<Traits, half_counters>(right)
                        : tournament<Traits, half_counters>(left);
            }
        }
    };

} // namespace bcpp::concurrency
